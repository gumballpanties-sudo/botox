#include "menu_internal.h"
#include "../misc/misc.h"
#include "../misc/chat_extras.h"
#include "../network/botox_net.h"
#include "../../game/sdk/includes/includes.h"

#include <shellapi.h>
#include <commdlg.h>
#include <winhttp.h>

#include <atomic>
#include <cctype>
#include <fstream>
#include <mutex>
#include <unordered_map>

#pragma comment( lib, "comdlg32.lib" )
#pragma comment( lib, "winhttp.lib" )

namespace
{
	/* one pick at a time: box that asked + its variable, result lands in image_pick_apply */
	std::mutex s_pick_mutex;
	ImGuiID s_pick_id          = 0;
	std::string* s_pick_target = nullptr;
	std::string s_pick_result, s_pick_status;
	bool s_pick_done = false;

	std::atomic< DWORD > s_pick_thread{ 0 };
	std::atomic< HINTERNET > s_pick_session{ nullptr };

	void pick_finish( const std::string& status, const std::string& result )
	{
		std::lock_guard< std::mutex > lock( s_pick_mutex );
		s_pick_status = status;
		s_pick_result = result;
		s_pick_done   = true;
	}

	/* discord fetches the image itself, so a local file needs a public url. freeimage.host public key (their api page).
	   not catbox: discord proxy gets 502 from files.catbox.moe = "?" image */
	std::string image_upload( const std::string& path )
	{
		std::ifstream file( path, std::ios::binary );
		const std::string data( ( std::istreambuf_iterator< char >( file ) ), std::istreambuf_iterator< char >( ) );
		if ( data.empty( ) || data.size( ) > 20u * 1024u * 1024u )
			return { };

		/* only the extension leaves the pc, not the file name */
		std::string extension = path.substr( path.find_last_of( "\\/" ) + 1 );
		const auto dot        = extension.find_last_of( '.' );
		extension             = dot == std::string::npos ? "" : extension.substr( dot );
		std::erase_if( extension, []( const char c ) { return !isalnum( static_cast< unsigned char >( c ) ) && c != '.'; } );

		const std::string boundary = std::format( "----botox{:08x}{:08x}", GetTickCount( ), GetCurrentThreadId( ) );
		std::string body = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"key\"\r\n\r\n6d207e02198a847aa98d0a2a901485a5\r\n--" + boundary +
		                   "\r\nContent-Disposition: form-data; name=\"format\"\r\n\r\ntxt\r\n--" + boundary +
		                   "\r\nContent-Disposition: form-data; name=\"source\"; filename=\"image" + extension +
		                   "\"\r\nContent-Type: application/octet-stream\r\n\r\n";
		body += data;
		body += "\r\n--" + boundary + "--\r\n";

		HINTERNET session = WinHttpOpen( L"botox", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0 );
		if ( !session )
			return { };

		s_pick_session = session;
		WinHttpSetTimeouts( session, 5000, 5000, 30000, 30000 );

		std::string out;
		HINTERNET connection = WinHttpConnect( session, L"freeimage.host", INTERNET_DEFAULT_HTTPS_PORT, 0 );
		HINTERNET request    = connection ? WinHttpOpenRequest( connection, L"POST", L"/api/1/upload", nullptr, WINHTTP_NO_REFERER,
		                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE )
		                                  : nullptr;

		const std::wstring header = L"Content-Type: multipart/form-data; boundary=" + std::wstring( boundary.begin( ), boundary.end( ) );

		if ( request &&
		     WinHttpSendRequest( request, header.c_str( ), static_cast< DWORD >( -1 ), body.data( ), static_cast< DWORD >( body.size( ) ),
		                         static_cast< DWORD >( body.size( ) ), 0 ) &&
		     WinHttpReceiveResponse( request, nullptr ) ) {
			char buffer[ 1024 ]{ };
			DWORD read = 0;
			while ( out.size( ) < 4096 && WinHttpReadData( request, buffer, sizeof( buffer ), &read ) && read )
				out.append( buffer, read );
		}

		if ( request )
			WinHttpCloseHandle( request );
		if ( connection )
			WinHttpCloseHandle( connection );

		HINTERNET mine = session;
		if ( s_pick_session.compare_exchange_strong( mine, nullptr ) )
			WinHttpCloseHandle( session );

		while ( !out.empty( ) && isspace( static_cast< unsigned char >( out.back( ) ) ) )
			out.pop_back( );

		return out.starts_with( "https://" ) && out.size( ) < 256 ? out : std::string{ };
	}

	unsigned long __stdcall pick_thread( void* upload )
	{
		const HRESULT com = CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE );

		char path[ MAX_PATH ]{ };
		OPENFILENAMEA dialog{ };
		dialog.lStructSize = sizeof( dialog );
		dialog.lpstrFilter = "images\0*.png;*.jpg;*.jpeg;*.gif;*.bmp\0all files\0*.*\0";
		dialog.lpstrFile   = path;
		dialog.nMaxFile    = MAX_PATH;
		dialog.lpstrTitle  = upload ? "pick image (uploads to freeimage.host)" : "pick image";
		dialog.Flags       = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

		s_pick_thread = GetCurrentThreadId( );
		const bool picked = GetOpenFileNameA( &dialog ) && !g_ctx.m_unloading;
		s_pick_thread = 0;

		if ( !picked )
			pick_finish( "", "" );
		else if ( !upload )
			pick_finish( "", path );
		else {
			{
				std::lock_guard< std::mutex > lock( s_pick_mutex );
				s_pick_status = "uploading...";
			}

			const std::string url = image_upload( path );
			pick_finish( url.empty( ) ? "upload failed" : "uploaded", url );
		}

		if ( SUCCEEDED( com ) )
			CoUninitialize( );

		g_image_pick_alive = false;
		return 0;
	}

	/* text box + browse button. upload = local file goes to freeimage.host, box gets the url (discord) */
	void image_path_input( const char* label, const char* hint, std::string& value, const bool upload )
	{
		static std::unordered_map< ImGuiID, std::array< char, 260 > > buffers;

		const ImGuiID id = ImGui::GetID( label );
		auto& buffer     = buffers[ id ];

		std::string status;
		{
			std::lock_guard< std::mutex > lock( s_pick_mutex );
			if ( s_pick_id == id )
				status = s_pick_status;
		}

		if ( ImGui::InputTextWithHint( label, hint, buffer.data( ), buffer.size( ) ) )
			value = buffer.data( );
		else if ( !ImGui::IsItemActive( ) && value != buffer.data( ) )
			strncpy_s( buffer.data( ), buffer.size( ), value.c_str( ), _TRUNCATE );

		const bool busy = g_image_pick_alive;

		ImGui::BeginDisabled( busy );
		if ( ImGui::Button( std::format( "{}##pick {}", upload ? "upload from pc" : "browse", label ).c_str( ), ImVec2( -1.f, 15.f ) ) && !busy ) {
			{
				std::lock_guard< std::mutex > lock( s_pick_mutex );
				s_pick_id     = id;
				s_pick_target = &value;
				s_pick_status = "picking...";
				s_pick_result.clear( );
				s_pick_done = false;
			}

			g_image_pick_alive = true; // before the thread runs: an eject right now must still wait for it
			g_utilities.create_thread( pick_thread, upload ? reinterpret_cast< void* >( 1 ) : nullptr );
		}
		ImGui::EndDisabled( );

		if ( !status.empty( ) )
			ImGui::Label( "%s", status.c_str( ) );
	}
}

void image_pick_apply( )
{
	std::lock_guard< std::mutex > lock( s_pick_mutex );
	if ( !s_pick_done )
		return;

	if ( s_pick_target && !s_pick_result.empty( ) )
		*s_pick_target = s_pick_result;

	s_pick_result.clear( );
	s_pick_done = false;
}

void image_pick_cancel( )
{
	if ( const DWORD thread = s_pick_thread )
		EnumThreadWindows(
			thread,
			[]( HWND window, LPARAM ) -> BOOL {
				char name[ 16 ]{ };
				if ( GetClassNameA( window, name, sizeof( name ) ) && !strcmp( name, "#32770" ) )
					PostMessageA( window, WM_COMMAND, IDCANCEL, 0 );
				return TRUE;
			},
			0 );

	if ( HINTERNET session = s_pick_session.exchange( nullptr ) )
		WinHttpCloseHandle( session );
}

void n_menu::impl_t::tab_inventory( )
{
	g_skins.init_parser( );

	const auto copy_to_buffer = []( char* buffer, std::size_t size, const std::string& value ) {
		const std::size_t len = value.size( ) < size - 1 ? value.size( ) : size - 1;
		std::memcpy( buffer, value.c_str( ), len );
		buffer[ len ] = '\0';
	};

	const auto skin_selector = []( const char* label, const std::vector< n_skins::paint_kit_entry_t >& kits, int* paint_kit,
	                               ImGuiTextFilter& filter ) {
		if ( kits.empty( ) ) {
			ImGui::SliderInt( label, paint_kit, 0, 1200, "%d" );
			return;
		}

		const char* current = "default";
		for ( const auto& kit : kits ) {
			if ( kit.m_id == *paint_kit ) {
				current = kit.m_name.c_str( );
				break;
			}
		}

		ImGui::Label( "%s: %s", label, current );

		ImGui::PushID( label );

		filter.Draw( "search##skin filter" );

		if ( ImGui::BeginListBox( "##skin list", ImVec2( 0.f, 90.f ) ) ) {
			if ( filter.PassFilter( "default" ) ) {
				if ( ImGui::Selectable( "default", *paint_kit == 0 ) )
					*paint_kit = 0;
			}

			static std::vector< int > matches{ };
			matches.clear( );

			for ( int i = 0; i < static_cast< int >( kits.size( ) ); i++ ) {
				if ( filter.PassFilter( kits[ i ].m_name.c_str( ) ) )
					matches.push_back( i );
			}

			ImGuiListClipper clipper{ };
			clipper.Begin( static_cast< int >( matches.size( ) ) );

			while ( clipper.Step( ) ) {
				for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++ ) {
					const auto& kit = kits[ matches[ row ] ];

					ImGui::PushID( kit.m_id );

					if ( ImGui::Selectable( kit.m_name.c_str( ), kit.m_id == *paint_kit ) )
						*paint_kit = kit.m_id;

					ImGui::PopID( );
				}
			}

			ImGui::EndListBox( );
		}

		ImGui::PopID( );
	};

	// custom / custom_sel: extra entries listed under the built ins ( agents: loose csgo\models\player .mdl files )
	const auto name_selector = []( const char* label, const char* const* names, int count, int* index, ImGuiTextFilter& filter,
	                               const std::vector< std::string >* custom = nullptr, std::string* custom_sel = nullptr ) {
		const bool custom_on = custom_sel && !custom_sel->empty( );
		ImGui::Label( "%s: %s", label, custom_on ? custom_sel->c_str( ) : ( *index >= 0 && *index < count ) ? names[ *index ] : "?" );

		ImGui::PushID( label );

		filter.Draw( "search##name filter" );

		if ( ImGui::BeginListBox( "##name list", ImVec2( 0.f, 90.f ) ) ) {
			for ( int i = 0; i < count; i++ ) {
				if ( !filter.PassFilter( names[ i ] ) )
					continue;

				ImGui::PushID( i );

				if ( ImGui::Selectable( names[ i ], !custom_on && *index == i ) ) {
					*index = i;
					if ( custom_sel )
						custom_sel->clear( );
				}

				ImGui::PopID( );
			}

			if ( custom && custom_sel && !custom->empty( ) ) {
				ImGui::TextDisabled( "custom models" );

				for ( const std::string& path : *custom ) {
					if ( !filter.PassFilter( path.c_str( ) ) )
						continue;

					if ( ImGui::Selectable( path.c_str( ), custom_on && *custom_sel == path ) )
						*custom_sel = path;
				}
			}

			ImGui::EndListBox( );
		}

		ImGui::PopID( );
	};

	const auto color_row = []( const char* id, c_color* colors[ 4 ] ) {
		const ImVec2 next_position = ImGui::GetCursorPos( );

		for ( int i = 0; i < 4; i++ ) {
			ImGui::SameLine( );

			const std::string label = std::string( "##" ) + id + " color " + std::to_string( i );

			if ( ImGui::ColorEdit4( label.c_str( ), colors[ i ], color_picker_no_alpha_flags, 3 - i ) )
				g_skins.m_forcing_update = true;
		}

		ImGui::SetCursorPos( next_position );
	};

	const auto seed_actual_colors = []( int paint_kit, std::array< c_color*, 4 > colors ) {
		n_skins::paint_colors_t actual{ };
		if ( !n_skins::get_actual_colors( paint_kit, actual ) )
			return;

		for ( int i = 0; i < 4; i++ )
			*colors[ i ] = c_color( static_cast< int >( actual.m_rgb[ i ][ 0 ] ), static_cast< int >( actual.m_rgb[ i ][ 1 ] ),
			                        static_cast< int >( actual.m_rgb[ i ][ 2 ] ) );
	};

	const auto update_button = []( ) {
		if ( ImGui::Button( "update", ImVec2( -1.f, 15.f ) ) )
			g_skins.m_forcing_update = true;
	};

	static bool edit_t = false;

	const auto team_bar = []( std::uint32_t same_var, const auto& copy_from_other ) {
		bool& same = GET_VARIABLE( same_var, bool );

		if ( !same ) {
			for ( const bool t : { false, true } ) {
				const bool on = edit_t == t;
				if ( on )
					ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_Accent ) );
				if ( ImGui::Button( t ? "terrorist" : "counter terrorist", ImVec2( -1.f, 15.f ) ) )
					edit_t = t;
				if ( on )
					ImGui::PopStyleColor( );
			}
		}

		ImGui::Checkbox( "same skins on both teams", &same );

		if ( !same && ImGui::Button( edit_t ? "copy from counter terrorist" : "copy from terrorist", ImVec2( -1.f, 15.f ) ) )
			copy_from_other( edit_t );
	};

	// GET_VARIABLE( dst ) = GET_VARIABLE( src ): same size vectors keep their buffer, frame stage holds refs
	const auto copy_team = []( auto* type, std::uint32_t ct, std::uint32_t t, bool to_t ) {
		using T = std::remove_pointer_t< decltype( type ) >;
		GET_VARIABLE( to_t ? t : ct, T ) = GET_VARIABLE( to_t ? ct : t, T );
	};

#define TV( name ) ( tside ? g_variables.name##_tside : g_variables.name )
#define COPY_TEAM( name, type ) copy_team( static_cast< type* >( nullptr ), g_variables.name, g_variables.name##_tside, to_t )

	switch ( this->m_subtab ) {
	case 0: {
		const bool tside = !GET_VARIABLE( g_variables.m_skins_same_weapons, bool ) && edit_t;

		menu_columns_begin( );

		if ( menu_group_begin( "weapons" ) ) {
			team_bar( g_variables.m_skins_same_weapons, [ & ]( bool to_t ) {
				COPY_TEAM( m_weapon_skins_enable, bool );
				COPY_TEAM( m_weapon_skins_paint_kit, std::vector< int > );
				COPY_TEAM( m_weapon_skins_wear, std::vector< float > );
				COPY_TEAM( m_weapon_skins_seed, std::vector< int > );
				COPY_TEAM( m_weapon_skins_stattrak, std::vector< bool > );
				COPY_TEAM( m_weapon_skins_stattrak_kills, std::vector< int > );
				COPY_TEAM( m_weapon_skins_custom_name, std::vector< std::string > );
				COPY_TEAM( m_weapon_skins_custom_color, std::vector< bool > );
				COPY_TEAM( m_weapon_skins_color_1, std::vector< c_color > );
				COPY_TEAM( m_weapon_skins_color_2, std::vector< c_color > );
				COPY_TEAM( m_weapon_skins_color_3, std::vector< c_color > );
				COPY_TEAM( m_weapon_skins_color_4, std::vector< c_color > );
				COPY_TEAM( m_weapon_skins_sticker_kit, std::vector< int > );
				COPY_TEAM( m_weapon_skins_sticker_wear, std::vector< float > );
				COPY_TEAM( m_weapon_skins_sticker_scale, std::vector< float > );
				COPY_TEAM( m_weapon_skins_sticker_rotation, std::vector< float > );
				COPY_TEAM( m_knife_enable, bool );
				COPY_TEAM( m_knife_model, int );
				COPY_TEAM( m_knife_fix_view, bool );
				COPY_TEAM( m_knife_anims_enable, bool );
				COPY_TEAM( m_knife_anims_model, int );
				COPY_TEAM( m_knife_paint_kit, int );
				COPY_TEAM( m_knife_wear, float );
				COPY_TEAM( m_knife_seed, int );
				COPY_TEAM( m_knife_stattrak, bool );
				COPY_TEAM( m_knife_stattrak_kills, int );
				COPY_TEAM( m_knife_custom_name, std::string );
				COPY_TEAM( m_knife_custom_color, bool );
				COPY_TEAM( m_knife_color_1, c_color );
				COPY_TEAM( m_knife_color_2, c_color );
				COPY_TEAM( m_knife_color_3, c_color );
				COPY_TEAM( m_knife_color_4, c_color );
				g_skins.m_forcing_update = true;
			} );

			ImGui::Checkbox( "enable weapon skins", &GET_VARIABLE( TV( m_weapon_skins_enable ), bool ) );
			ImGui::Combo( "weapon", &GET_VARIABLE( g_variables.m_weapon_skins_selected, int ),
			              "usp-s\0p2000\0glock-18\0p250\0five-seven\0tec-9\0cz75-auto\0dual berettas\0deagle\0r8 revolver\0famas\0galil "
			              "ar\0m4a4\0m4a1-s\0ak-47\0sg 553\0aug\0ssg 08\0awp\0scar-20\0g3sg1\0sawed-off\0m249\0negev\0mag-7\0xm1014\0nova\0pp-"
			              "bizon\0mp5-sd\0mp7\0mp9\0mac-10\0p90\0ump-45\0" );

			const int sel        = GET_VARIABLE( g_variables.m_weapon_skins_selected, int );
			auto& paint_kits     = GET_VARIABLE( TV( m_weapon_skins_paint_kit ), std::vector< int > );
			auto& wears          = GET_VARIABLE( TV( m_weapon_skins_wear ), std::vector< float > );
			auto& seeds          = GET_VARIABLE( TV( m_weapon_skins_seed ), std::vector< int > );
			auto& stattraks      = GET_VARIABLE( TV( m_weapon_skins_stattrak ), std::vector< bool > );
			auto& stattrak_kills = GET_VARIABLE( TV( m_weapon_skins_stattrak_kills ), std::vector< int > );
			auto& custom_names   = GET_VARIABLE( TV( m_weapon_skins_custom_name ), std::vector< std::string > );

			if ( sel >= 0 && sel < static_cast< int >( paint_kits.size( ) ) && sel < static_cast< int >( wears.size( ) ) &&
			     sel < static_cast< int >( seeds.size( ) ) && sel < static_cast< int >( stattraks.size( ) ) &&
			     sel < static_cast< int >( stattrak_kills.size( ) ) && sel < static_cast< int >( custom_names.size( ) ) ) {
				static ImGuiTextFilter weapon_skin_filter{ };
				skin_selector( "paint kit", g_skins.m_parser_skins, &paint_kits[ sel ], weapon_skin_filter );

				ImGui::SliderFloat( "wear", &wears[ sel ], 0.0001f, 1.f, "%.4f" );
				ImGui::SliderInt( "pattern", &seeds[ sel ], 0, 1023, "%d" );

				bool stattrak = stattraks[ sel ];
				if ( ImGui::Checkbox( "stattrak", &stattrak ) )
					stattraks[ sel ] = stattrak;

				if ( stattrak )
					ImGui::SliderInt( "kills", &stattrak_kills[ sel ], 0, 99999, "%d" );

				static char weapon_name[ 41 ] = { };
				static int last_selected      = -1;

				if ( last_selected != sel ) {
					last_selected = sel;
					copy_to_buffer( weapon_name, sizeof( weapon_name ), custom_names[ sel ] );
				}

				if ( ImGui::InputText( "name tag", weapon_name, sizeof( weapon_name ) ) )
					custom_names[ sel ] = weapon_name;
				else if ( !ImGui::IsItemActive( ) && custom_names[ sel ] != weapon_name )
					copy_to_buffer( weapon_name, sizeof( weapon_name ), custom_names[ sel ] );

				auto& custom_colors = GET_VARIABLE( TV( m_weapon_skins_custom_color ), std::vector< bool > );
				auto& colors_1      = GET_VARIABLE( TV( m_weapon_skins_color_1 ), std::vector< c_color > );
				auto& colors_2      = GET_VARIABLE( TV( m_weapon_skins_color_2 ), std::vector< c_color > );
				auto& colors_3      = GET_VARIABLE( TV( m_weapon_skins_color_3 ), std::vector< c_color > );
				auto& colors_4      = GET_VARIABLE( TV( m_weapon_skins_color_4 ), std::vector< c_color > );

				if ( sel < static_cast< int >( custom_colors.size( ) ) && sel < static_cast< int >( colors_1.size( ) ) &&
				     sel < static_cast< int >( colors_2.size( ) ) && sel < static_cast< int >( colors_3.size( ) ) &&
				     sel < static_cast< int >( colors_4.size( ) ) ) {
					bool custom_color = custom_colors[ sel ];
					if ( ImGui::Checkbox( "custom color", &custom_color ) ) {
						custom_colors[ sel ]     = custom_color;
						g_skins.m_forcing_update = true;

						if ( custom_color )
							seed_actual_colors( paint_kits[ sel ], { &colors_1[ sel ], &colors_2[ sel ], &colors_3[ sel ], &colors_4[ sel ] } );
					}

					if ( custom_color ) {
						c_color* weapon_colors[ 4 ] = { &colors_1[ sel ], &colors_2[ sel ], &colors_3[ sel ], &colors_4[ sel ] };
						color_row( "weapon", weapon_colors );
					}
				}

				auto& sticker_kits      = GET_VARIABLE( TV( m_weapon_skins_sticker_kit ), std::vector< int > );
				auto& sticker_wears     = GET_VARIABLE( TV( m_weapon_skins_sticker_wear ), std::vector< float > );
				auto& sticker_scales    = GET_VARIABLE( TV( m_weapon_skins_sticker_scale ), std::vector< float > );
				auto& sticker_rotations = GET_VARIABLE( TV( m_weapon_skins_sticker_rotation ), std::vector< float > );

				ImGui::Combo( "sticker slot", &GET_VARIABLE( g_variables.m_weapon_skins_sticker_slot, int ),
				              "slot 1\0slot 2\0slot 3\0slot 4\0slot 5\0" );

				const int slot    = GET_VARIABLE( g_variables.m_weapon_skins_sticker_slot, int );
				const int sticker = sel * 5 + slot;

				if ( slot >= 0 && slot < 5 && sticker < static_cast< int >( sticker_kits.size( ) ) &&
				     sticker < static_cast< int >( sticker_wears.size( ) ) && sticker < static_cast< int >( sticker_scales.size( ) ) &&
				     sticker < static_cast< int >( sticker_rotations.size( ) ) ) {
					static ImGuiTextFilter sticker_filter{ };
					skin_selector( "sticker", g_skins.m_parser_stickers, &sticker_kits[ sticker ], sticker_filter );

					if ( sticker_kits[ sticker ] > 0 ) {
						ImGui::SliderFloat( "sticker wear", &sticker_wears[ sticker ], 0.f, 1.f, "%.4f" );
						ImGui::SliderFloat( "sticker scale", &sticker_scales[ sticker ], 0.1f, 5.f, "%.2f" );
						ImGui::SliderFloat( "sticker rotation", &sticker_rotations[ sticker ], 0.f, 360.f, "%.0f" );
					}
				}
			}

			update_button( );

			static bool open_reset_popup = false;
			if ( ImGui::Button( "reset skins", ImVec2( -1.f, 15.f ) ) )
				open_reset_popup = true;

			if ( open_reset_popup ) {
				save_popup( "reset all weapon skins", open_reset_popup, ImVec2( 220.f, -1.f ), []( ) {
					if ( ImGui::Button( "yes", ImVec2( ( ImGui::GetRowFrameWidth( ) - 14.f ) * 0.5f, 15.f ) ) ) {
						for ( const std::uint32_t variable : { g_variables.m_weapon_skins_paint_kit, g_variables.m_weapon_skins_paint_kit_tside,
						                                       g_variables.m_weapon_skins_seed, g_variables.m_weapon_skins_seed_tside,
						                                       g_variables.m_weapon_skins_stattrak_kills, g_variables.m_weapon_skins_stattrak_kills_tside,
						                                       g_variables.m_weapon_skins_sticker_kit, g_variables.m_weapon_skins_sticker_kit_tside } )
							g_config.reset< std::vector< int > >( variable );

						for ( const std::uint32_t variable : { g_variables.m_weapon_skins_wear, g_variables.m_weapon_skins_wear_tside,
						                                       g_variables.m_weapon_skins_sticker_wear, g_variables.m_weapon_skins_sticker_wear_tside,
						                                       g_variables.m_weapon_skins_sticker_scale, g_variables.m_weapon_skins_sticker_scale_tside,
						                                       g_variables.m_weapon_skins_sticker_rotation, g_variables.m_weapon_skins_sticker_rotation_tside } )
							g_config.reset< std::vector< float > >( variable );

						for ( const std::uint32_t variable : { g_variables.m_weapon_skins_stattrak, g_variables.m_weapon_skins_stattrak_tside,
						                                       g_variables.m_weapon_skins_custom_color, g_variables.m_weapon_skins_custom_color_tside } )
							g_config.reset< std::vector< bool > >( variable );

						for ( const std::uint32_t variable : { g_variables.m_weapon_skins_color_1, g_variables.m_weapon_skins_color_1_tside,
						                                       g_variables.m_weapon_skins_color_2, g_variables.m_weapon_skins_color_2_tside,
						                                       g_variables.m_weapon_skins_color_3, g_variables.m_weapon_skins_color_3_tside,
						                                       g_variables.m_weapon_skins_color_4, g_variables.m_weapon_skins_color_4_tside } )
							g_config.reset< std::vector< c_color > >( variable );

						g_config.reset< std::vector< std::string > >( g_variables.m_weapon_skins_custom_name );
						g_config.reset< std::vector< std::string > >( g_variables.m_weapon_skins_custom_name_tside );

						g_logger.print( "reset all weapon skins" );
						open_reset_popup = false;
					}

					ImGui::SameLine( );

					if ( ImGui::Button( "no", ImVec2( -1.f, 15.f ) ) )
						open_reset_popup = false;
				} );
			}
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "knife" ) ) {
			ImGui::Checkbox( "enable knife anims", &GET_VARIABLE( TV( m_knife_anims_enable ), bool ) );
			if ( GET_VARIABLE( TV( m_knife_anims_enable ), bool ) ) {
				// applies live, knife_anim_live
				if ( !n_skins::knife_anims_fit( GET_VARIABLE( TV( m_knife_anims_model ), int ),
				                                GET_VARIABLE( TV( m_knife_model ), int ), GET_VARIABLE( TV( m_knife_enable ), bool ) ) )
					ImGui::TextColored( ImVec4( 1.f, 0.4f, 0.4f, 1.f ), "wrong bones for this knife — draw will not play" );

				ImGui::Combo( "knife anims", &GET_VARIABLE( TV( m_knife_anims_model ), int ),
				              "default\0bayonet\0m9 bayonet\0karambit\0bowie\0butterfly\0falchion\0flip\0gut\0huntsman\0shadow "
				              "daggers\0navaja\0stiletto\0talon\0ursus\0default ct\0default t\0gold knife\0css "
				              "knife\0outdoor\0canis\0cord\0skeleton\0" );
			}

			ImGui::Checkbox( "enable knife", &GET_VARIABLE( TV( m_knife_enable ), bool ) );
			if ( GET_VARIABLE( TV( m_knife_enable ), bool ) ) {
				ImGui::Combo( "knife model", &GET_VARIABLE( TV( m_knife_model ), int ),
				              "default\0bayonet\0m9 bayonet\0karambit\0bowie\0butterfly\0falchion\0flip\0gut\0huntsman\0shadow "
				              "daggers\0navaja\0stiletto\0talon\0ursus\0default ct\0default t\0gold knife\0css "
				              "knife\0outdoor\0canis\0cord\0skeleton\0" );

				if ( GET_VARIABLE( TV( m_knife_model ), int ) == 10 )
					ImGui::Checkbox( "fix view", &GET_VARIABLE( TV( m_knife_fix_view ), bool ) );

				static ImGuiTextFilter knife_skin_filter{ };
				skin_selector( "paint kit", g_skins.m_parser_skins, &GET_VARIABLE( TV( m_knife_paint_kit ), int ),
				               knife_skin_filter );

				ImGui::SliderFloat( "knife wear", &GET_VARIABLE( TV( m_knife_wear ), float ), 0.0001f, 1.f, "%.4f" );
				ImGui::SliderInt( "knife pattern", &GET_VARIABLE( TV( m_knife_seed ), int ), 0, 1023, "%d" );

				ImGui::Checkbox( "knife stattrak", &GET_VARIABLE( TV( m_knife_stattrak ), bool ) );
				if ( GET_VARIABLE( TV( m_knife_stattrak ), bool ) )
					ImGui::SliderInt( "knife kills", &GET_VARIABLE( TV( m_knife_stattrak_kills ), int ), 0, 99999, "%d" );

				static char knife_name[ 41 ] = { };
				auto& knife_name_var         = GET_VARIABLE( TV( m_knife_custom_name ), std::string );

				if ( ImGui::InputText( "knife name tag", knife_name, sizeof( knife_name ) ) )
					knife_name_var = knife_name;
				else if ( !ImGui::IsItemActive( ) && knife_name_var != knife_name )
					copy_to_buffer( knife_name, sizeof( knife_name ), knife_name_var );

				if ( ImGui::Checkbox( "custom color##knife", &GET_VARIABLE( TV( m_knife_custom_color ), bool ) ) ) {
					g_skins.m_forcing_update = true;

					if ( GET_VARIABLE( TV( m_knife_custom_color ), bool ) )
						seed_actual_colors( GET_VARIABLE( TV( m_knife_paint_kit ), int ),
						                     { &GET_VARIABLE( TV( m_knife_color_1 ), c_color ),
						                       &GET_VARIABLE( TV( m_knife_color_2 ), c_color ),
						                       &GET_VARIABLE( TV( m_knife_color_3 ), c_color ),
						                       &GET_VARIABLE( TV( m_knife_color_4 ), c_color ) } );
				}
				if ( GET_VARIABLE( TV( m_knife_custom_color ), bool ) ) {
					c_color* knife_colors[ 4 ] = {
						&GET_VARIABLE( TV( m_knife_color_1 ), c_color ), &GET_VARIABLE( TV( m_knife_color_2 ), c_color ),
						&GET_VARIABLE( TV( m_knife_color_3 ), c_color ), &GET_VARIABLE( TV( m_knife_color_4 ), c_color )
					};

					color_row( "knife", knife_colors );
				}
			}

			update_button( );
		}
		menu_group_end( );

		menu_columns_end( );
		break;
	}
	case 1: {
		const bool same  = GET_VARIABLE( g_variables.m_skins_same_player, bool );
		const bool tside = !same && edit_t;

		menu_columns_begin( );

		if ( menu_group_begin( "agents" ) ) {
			team_bar( g_variables.m_skins_same_player, [ & ]( bool to_t ) {
				COPY_TEAM( m_gloves_enable, bool );
				COPY_TEAM( m_gloves_model, int );
				COPY_TEAM( m_gloves_paint_kit, int );
				COPY_TEAM( m_gloves_wear, float );
				COPY_TEAM( m_gloves_seed, int );
				// agents were always per team: m_agent_t / m_agent_ct
				copy_team( static_cast< int* >( nullptr ), g_variables.m_agent_ct, g_variables.m_agent_t, to_t );
				copy_team( static_cast< std::string* >( nullptr ), g_variables.m_agent_ct_custom, g_variables.m_agent_t_custom, to_t );
				g_skins.m_forcing_update = true;
			} );

			ImGui::Checkbox( "enable agents", &GET_VARIABLE( g_variables.m_agent_enable, bool ) );
			if ( GET_VARIABLE( g_variables.m_agent_enable, bool ) ) {
				ImGui::Checkbox( "custom models", &GET_VARIABLE( g_variables.m_agent_custom_models, bool ) );
				const bool show_custom = GET_VARIABLE( g_variables.m_agent_custom_models, bool );

				// is_player_mdl reads whole mdl + vvd: one uncached file per frame, rescan when list reappears
				static std::vector< std::string > pending, custom_models;
				static std::unordered_map< std::string, bool > is_body;
				static int last_frame = -2;
				if ( show_custom ) {
					if ( ImGui::GetFrameCount( ) - last_frame > 1 ) {
						pending = g_utilities.game_files( "models/player", ".mdl" );
						std::reverse( pending.begin( ), pending.end( ) );
						custom_models.clear( );
					}
					last_frame = ImGui::GetFrameCount( );

					while ( !pending.empty( ) ) {
						const std::string path = std::move( pending.back( ) );
						pending.pop_back( );
						auto it          = is_body.find( path );
						const bool fresh = it == is_body.end( );
						if ( fresh )
							it = is_body.emplace( path, g_utilities.is_player_mdl( path ) ).first;
						if ( it->second )
							custom_models.push_back( path );
						if ( fresh )
							break;
					}
					if ( !pending.empty( ) )
						ImGui::TextDisabled( "loading custom models... %d left", static_cast< int >( pending.size( ) ) );
				}

				static ImGuiTextFilter t_agent_filter{ }, ct_agent_filter{ };
				if ( same || edit_t )
					name_selector( "t agent", n_skins::AGENT_NAMES, n_skins::AGENT_NAME_COUNT, &GET_VARIABLE( g_variables.m_agent_t, int ),
					               t_agent_filter, show_custom ? &custom_models : nullptr, &GET_VARIABLE( g_variables.m_agent_t_custom, std::string ) );
				if ( same || !edit_t )
					name_selector( "ct agent", n_skins::AGENT_NAMES, n_skins::AGENT_NAME_COUNT, &GET_VARIABLE( g_variables.m_agent_ct, int ),
					               ct_agent_filter, show_custom ? &custom_models : nullptr, &GET_VARIABLE( g_variables.m_agent_ct_custom, std::string ) );
			}

			update_button( );
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "gloves" ) ) {
			ImGui::Checkbox( "enable gloves", &GET_VARIABLE( TV( m_gloves_enable ), bool ) );
			if ( GET_VARIABLE( TV( m_gloves_enable ), bool ) ) {
				static const char* const glove_models[] = { "default", "broken fang", "bloodhound", "sport", "slick",
					                                        "leather wrap", "moto", "specialist", "hydra" };
				static ImGuiTextFilter glove_model_filter{ };
				name_selector( "glove model", glove_models, IM_ARRAYSIZE( glove_models ), &GET_VARIABLE( TV( m_gloves_model ), int ),
				               glove_model_filter );

				static ImGuiTextFilter glove_skin_filter{ };
				skin_selector( "paint kit", g_skins.m_parser_gloves, &GET_VARIABLE( TV( m_gloves_paint_kit ), int ),
				               glove_skin_filter );

				ImGui::SliderFloat( "glove wear", &GET_VARIABLE( TV( m_gloves_wear ), float ), 0.0001f, 1.f, "%.4f" );
				ImGui::SliderInt( "glove pattern", &GET_VARIABLE( TV( m_gloves_seed ), int ), 0, 1023, "%d" );
			}

			update_button( );
		}
		menu_group_end( );

		menu_columns_end( );
		break;
	}
	}

#undef TV
#undef COPY_TEAM
}

void n_menu::impl_t::tab_fonts( )
{
	/* indicators + esp share one layout: font settings | file list */
	const auto render_font_page = [ ]( const char* name, const std::uint32_t settings_var, const std::uint32_t flags_var,
	                                   const e_custom_font_names custom_font, const e_font_names default_font, ImGuiTextFilter& filter ) {
		menu_columns_begin( );

		if ( menu_group_begin( name ) ) {
			ImGui::SliderInt( "size", &GET_VARIABLE( settings_var, font_setting_t ).m_size, 0, 50, "%d" );

			ImGui::MultiCombo( "font flags", GET_VARIABLE( flags_var, std::vector< bool > ),
			                   { "no hinting", "no autohint", "force autohint", "light hinting", "monohinting", "bold", "oblique", "monochrome" },
			                   GET_VARIABLE( flags_var, std::vector< bool > ).size( ) );

			if ( ImGui::Button( "reset to default", ImVec2( -1.f, 15.f ) ) )
				g_render.m_custom_fonts[ custom_font ] = g_render.m_fonts[ default_font ];

			if ( ImGui::Button( "reload fonts", ImVec2( -1.f, 15.f ) ) )
				g_render.m_reload_fonts = true;
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "font list" ) ) {
			filter.Draw( "search for a font" );

			if ( ImGui::BeginListBox( "##font list", ImVec2( 0.f, 300.f ) ) ) {
				for ( const auto& iterator : g_fonts.m_font_file_names ) {
					if ( filter.PassFilter( iterator.c_str( ) ) )
						if ( ImGui::Selectable( iterator.c_str( ),
						                        HASH_RT( GET_VARIABLE( settings_var, font_setting_t ).m_name.c_str( ) ) == HASH_RT( iterator.c_str( ) ) ) )
							GET_VARIABLE( settings_var, font_setting_t ).m_name = iterator;
				}
				ImGui::EndListBox( );
			}
		}
		menu_group_end( );

		menu_columns_end( );
	};

	/* chud + default hud: game font family | system font list */
	const auto render_system_font_list = [ ]( std::string& font_var, char* buffer, const std::size_t buffer_size, ImGuiTextFilter& filter ) {
		filter.Draw( "search for a font" );

		if ( ImGui::BeginListBox( "##font list", ImVec2( 0.f, 300.f ) ) ) {
			for ( const auto& iterator : g_chud.system_fonts( ) ) {
				if ( filter.PassFilter( iterator.c_str( ) ) )
					if ( ImGui::Selectable( iterator.c_str( ), HASH_RT( font_var.c_str( ) ) == HASH_RT( iterator.c_str( ) ) ) ) {
						font_var = iterator;
						const std::size_t len = ImMin( font_var.size( ), buffer_size - 1 );
						std::memcpy( buffer, font_var.c_str( ), len );
						buffer[ len ] = '\0';
					}
			}
			ImGui::EndListBox( );
		}
	};

	const auto font_family_input = [ ]( std::string& font_var, char* buffer, const std::size_t buffer_size ) {
		if ( ImGui::InputText( "font family", buffer, buffer_size ) )
			font_var = buffer;
		else if ( !ImGui::IsItemActive( ) && font_var != buffer ) {
			const std::size_t len = ImMin( font_var.size( ), buffer_size - 1 );
			std::memcpy( buffer, font_var.c_str( ), len );
			buffer[ len ] = '\0';
		}
	};

	switch ( this->m_subtab ) {
	case 0: {
		static ImGuiTextFilter indicator_font_search_filter{ };
		render_font_page( "indicator font", g_variables.m_indicator_font_settings, g_variables.m_indicator_font_flags,
		                  e_custom_font_names::custom_font_name_indicator, e_font_names::font_name_indicator_29, indicator_font_search_filter );
		break;
	}
	case 1: {
		static ImGuiTextFilter esp_font_search_filter{ };
		render_font_page( "esp font", g_variables.m_esp_font_settings, g_variables.m_esp_font_flags, e_custom_font_names::custom_font_name_esp,
		                  e_font_names::font_name_verdana_bd_11, esp_font_search_filter );
		break;
	}
	case 2: {
		auto& font_var = GET_VARIABLE( g_variables.m_chud_hud_font_custom, std::string );

		static char chud_font_buffer[ 64 ] = { };
		static ImGuiTextFilter chud_font_search_filter{ };

		menu_columns_begin( );

		if ( menu_group_begin( "chud hud font" ) ) {
			if ( ImGui::Button( "register selected font (restart game after)", ImVec2( -1.f, 15.f ) ) )
				g_chud.register_system_fonts( );

			if ( ImGui::Button( "restore stock fonts.conf", ImVec2( -1.f, 15.f ) ) )
				g_chud.restore_fonts_conf( );

			font_family_input( font_var, chud_font_buffer, sizeof( chud_font_buffer ) );

			ImGui::SliderFloat( "font scale", &GET_VARIABLE( g_variables.m_chud_hud_font_scale, float ), 50.f, 150.f, "%.0f%%" );

			ImGui::SliderFloat( "text nudge x", &GET_VARIABLE( g_variables.m_chud_hud_font_nudge_x, float ), -20.f, 20.f, "%.0f" );
			ImGui::SliderFloat( "text nudge y", &GET_VARIABLE( g_variables.m_chud_hud_font_nudge_y, float ), -20.f, 20.f, "%.0f" );
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "font list" ) )
			render_system_font_list( font_var, chud_font_buffer, sizeof( chud_font_buffer ), chud_font_search_filter );
		menu_group_end( );

		menu_columns_end( );
		break;
	}
	case 3: {
		auto& font_var = GET_VARIABLE( g_variables.m_def_hud_font, std::string );

		static char def_font_buffer[ 64 ] = { };
		static ImGuiTextFilter def_font_search_filter{ };

		menu_columns_begin( );

		if ( menu_group_begin( "default hud font" ) ) {
			if ( ImGui::Button( "apply to default hud (restart game after)", ImVec2( -1.f, 15.f ) ) )
				g_chud.register_def_hud_font( );

			if ( ImGui::Button( "back to stratum2", ImVec2( -1.f, 15.f ) ) )
				g_chud.clear_def_hud_font( );

			if ( ImGui::Button( "restore stock fonts.conf", ImVec2( -1.f, 15.f ) ) )
				g_chud.restore_fonts_conf( );

			font_family_input( font_var, def_font_buffer, sizeof( def_font_buffer ) );
		}
		menu_group_end( );

		menu_columns_next( );

		if ( menu_group_begin( "font list" ) )
			render_system_font_list( font_var, def_font_buffer, sizeof( def_font_buffer ), def_font_search_filter );
		menu_group_end( );

		menu_columns_end( );
		break;
	}
	}
}

void n_menu::impl_t::player_list_page( )
{
	const auto engine = g_interfaces.m_engine_client;
	const int local   = engine->get_local_player( );
	const int clients = engine->is_connected_safe( ) ? ImMin( engine->get_max_clients( ), 64 ) : 0;
	int shown         = 0;

	/* negative y = sized after show_text's 20 px title shift */
	if ( ImGui::BeginChild( "players", ImVec2( 0.f, -menu_bottom_band_height ), true, 0, true ) ) {
		/* name gets ~36%, the 4 cells split the rest evenly, header centred in its slot */
		const float row_width = ImGui::GetContentRegionAvail( ).x;
		const float name_width = ImFloor( row_width * 0.36f ), slot = ( row_width - name_width ) / 4.f;
		const auto slot_x = [ & ]( const int index, const char* header ) {
			return ImFloor( name_width + slot * index + ( slot - ImGui::CalcTextSize( header ).x ) / 2.f );
		};
		const float k_team = slot_x( 0, "team" ), k_revive = slot_x( 1, "auto revive" ), k_ignore = slot_x( 2, "aimbot ignore" ),
		            k_only = slot_x( 3, "only aimbot" );
		const float base_x = ImGui::GetCursorScreenPos( ).x;
		const auto column  = [ base_x ]( const float offset, const char* header = nullptr, const float width = 0.f ) {
			const float centre = header ? ImMax( 0.f, ( ImGui::CalcTextSize( header ).x - width ) / 2.f ) : 0.f;
			ImGui::SameLine( base_x - ImGui::GetWindowPos( ).x + offset + centre );
		};

		ImGui::Label( "name" );
		column( k_team );
		ImGui::Label( "team" );
		column( k_revive );
		ImGui::Label( "auto revive" );
		column( k_ignore );
		ImGui::Label( "aimbot ignore" );
		column( k_only );
		ImGui::Label( "only aimbot" );

		if ( ImGui::BeginChild( "##player list rows", ImVec2( 0.f, 0.f ), false, 0, false ) ) {
			const float square = ImGui::GetFrameHeight( );

			for ( int i = 1; i <= clients; i++ ) {
				player_info_t info{ };
				if ( !engine->get_player_info( i, &info ) || info.m_is_hltv )
					continue;

				player_list_stamp( i, info );
				auto& entry = g_player_list[ i ];

				const auto entity = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
				const int team    = entity ? entity->get_team( ) : 0;

				std::string name = std::string( info.m_name ).substr( 0, 18 );
				if ( info.m_fake_player )
					name = "BOT " + name;
				if ( i == local )
					name += " (you)";

				ImGui::PushID( i );

				ImGui::SetCursorScreenPos( ImVec2( base_x, ImGui::GetCursorScreenPos( ).y ) );
				ImGui::AlignTextToFramePadding( );
				ImGui::Text( "%s", name.c_str( ) );

				const char* team_text = team == 2 ? "T" : team == 3 ? "CT" : "-";
				column( k_team, "team", ImGui::CalcTextSize( team_text ).x );
				if ( team == 2 )
					ImGui::TextColored( ImVec4( 0.92f, 0.75f, 0.33f, 1.f ), "%s", team_text );
				else if ( team == 3 )
					ImGui::TextColored( ImVec4( 0.36f, 0.55f, 0.85f, 1.f ), "%s", team_text );
				else
					ImGui::TextDisabled( "%s", team_text );

				column( k_revive, "auto revive", square );
				if ( ImGui::Checkbox( "##revive", &entry.m_revive ) )
					player_list_save( i, info );

				/* aimbot never targets you: "-" like a teamless team cell keeps the grid whole */
				if ( i == local ) {
					const float dash = ImGui::CalcTextSize( "-" ).x;
					column( k_ignore, "aimbot ignore", dash );
					ImGui::TextDisabled( "-" );
					column( k_only, "only aimbot", dash );
					ImGui::TextDisabled( "-" );
				}
				else {
					column( k_ignore, "aimbot ignore", square );
					if ( ImGui::Checkbox( "##ignore", &entry.m_ignore ) )
						player_list_save( i, info );
					column( k_only, "only aimbot", square );
					if ( ImGui::Checkbox( "##only", &entry.m_only ) )
						player_list_save( i, info );
				}

				ImGui::PopID( );
				shown++;
			}

			if ( !shown )
				ImGui::TextDisabled( "no players" );
		}
		ImGui::EndChild( );
	}
	ImGui::EndChild( );
}

void n_menu::impl_t::tab_misc( )
{
	if ( this->m_subtab == 1 ) {
		this->player_list_page( );
		return;
	}

	menu_columns_begin( );

	if ( menu_group_begin( "overlays" ) ) {
		ImGui::Checkbox( "watermark", &GET_VARIABLE( g_variables.m_watermark, bool ) );
		if ( GET_VARIABLE( g_variables.m_watermark, bool ) ) {
			ImGui::OptionPopup(
				"watermark configuration",
				[]( ) {
					auto& style = GET_VARIABLE( g_variables.m_watermark_style, int );
					if ( style < 0 || style > 20 )
						style = 0;

					ImGui::Combo( "style##watermark style", &style,
					              "default\0kamibebra\0clarity\0clarity v2\0delusional\0interwebz\0havoc\0onetap\0airflow\0ev0lve\0legendware\0"
					              "interium\0skebob\0cumidere\0illusory\0lumi\0billware\0dna\0dna clarity\0cucumber\0howeweware\0" );

					{
						static char name_buffer[ 25 ] = { };
						auto& name_var                = GET_VARIABLE( g_variables.m_watermark_text, std::string );

						if ( ImGui::InputTextWithHint( "name##watermark text", "botox", name_buffer, sizeof( name_buffer ) ) )
							name_var = name_buffer;
						else if ( !ImGui::IsItemActive( ) && name_var != name_buffer ) {
							strncpy_s( name_buffer, name_var.c_str( ), sizeof( name_buffer ) - 1 );
							name_buffer[ sizeof( name_buffer ) - 1 ] = '\0';
						}
					}

					if ( style == 0 || style == 2 || style == 15 ) {
						ImGui::Checkbox( "user##watermark user", &GET_VARIABLE( g_variables.m_watermark_user, bool ) );
						ImGui::Checkbox( "tickrate", &GET_VARIABLE( g_variables.m_watermark_tickrate, bool ) );
						ImGui::Checkbox( "fps", &GET_VARIABLE( g_variables.m_watermark_fps, bool ) );
						ImGui::Checkbox( "time", &GET_VARIABLE( g_variables.m_watermark_time, bool ) );
					}

					if ( style == 20 ) {
						ImGui::Checkbox( "time##watermark howeweware", &GET_VARIABLE( g_variables.m_watermark_time, bool ) );
						ImGui::Checkbox( "ping##watermark howeweware", &GET_VARIABLE( g_variables.m_watermark_ping, bool ) );
						ImGui::Checkbox( "tickrate##watermark howeweware", &GET_VARIABLE( g_variables.m_watermark_tickrate, bool ) );
						ImGui::Checkbox( "fps##watermark howeweware", &GET_VARIABLE( g_variables.m_watermark_fps, bool ) );
						ImGui::Checkbox( "velocity##watermark howeweware", &GET_VARIABLE( g_variables.m_watermark_velocity, bool ) );
					}

					if ( style == 12 )
						ImGui::Checkbox( "typing effect##watermark skebob", &GET_VARIABLE( g_variables.m_watermark_skebob_typing, bool ) );

					const bool label_style =
						style == 4 || style == 5 || style == 6 || style == 7 || style == 9 || style == 11 || style == 13 || style == 14 || style == 16 ||
						style == 17 || style == 18;

					auto& label = GET_VARIABLE( g_variables.m_watermark_label, int );
					if ( label < 0 || label > 3 )
						label = 0;

					if ( label_style )
						ImGui::Combo( "label##watermark label", &label, "time\0fps\0user\0none\0" );

					const bool shows_user = ( ( style == 0 || style == 2 || style == 15 ) && GET_VARIABLE( g_variables.m_watermark_user, bool ) ) ||
					                        style == 12 ||
					                        ( style == 1 && ( GET_VARIABLE( g_variables.m_watermark_kami_show_flags, int ) & 1 ) ) || style == 3 ||
					                        style == 8 || style == 10 || style == 17 || style == 18 || style == 19 || style == 20 || ( label_style && label == 2 );

					if ( shows_user ) {
						static char user_buffer[ 33 ] = { };
						auto& user_var                = GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string );

						if ( ImGui::InputTextWithHint( "user name##watermark clarity user", "user", user_buffer, sizeof( user_buffer ) ) )
							user_var = user_buffer;
						else if ( !ImGui::IsItemActive( ) && user_var != user_buffer ) {
							strncpy_s( user_buffer, user_var.c_str( ), sizeof( user_buffer ) - 1 );
							user_buffer[ sizeof( user_buffer ) - 1 ] = '\0';
						}
					}

					if ( style == 9 )
						ImGui::SliderFloat( "alpha override##watermark evolve", &GET_VARIABLE( g_variables.m_watermark_evolve_alpha, float ), 0.f, 100.f,
						                    "%.0f%%" );

					if ( style == 6 )
						ImGui::ColorEdit4( "blue color##watermark havoc", &GET_VARIABLE( g_variables.m_watermark_havoc_color, c_color ), color_picker_alpha_flags );

					if ( style == 17 ) {
						ImGui::Checkbox( "detection glow##watermark dna", &GET_VARIABLE( g_variables.m_watermark_dna_glow, bool ) );
						if ( GET_VARIABLE( g_variables.m_watermark_dna_glow, bool ) ) {
							ImGui::SliderInt( "glow ticks##watermark dna", &GET_VARIABLE( g_variables.m_watermark_dna_glow_ticks, int ), 1, 64 );
							ImGui::Checkbox( "glow on spotted enemy##watermark dna", &GET_VARIABLE( g_variables.m_watermark_dna_glow_spotted, bool ) );
						}
					}

					if ( style == 11 )
						ImGui::ColorEdit4( "background color##watermark interium", &GET_VARIABLE( g_variables.m_watermark_interium_bg, c_color ),
						                   color_picker_alpha_flags );

					if ( style == 15 ) {
						auto& design = GET_VARIABLE( g_variables.m_watermark_lumi_design, int );
						if ( design < 0 || design > 1 )
							design = 0;
						ImGui::Combo( "design##watermark lumi", &design, "original\0new\0" );
						ImGui::Checkbox( "animated title##watermark lumi", &GET_VARIABLE( g_variables.m_watermark_lumi_animated_title, bool ) );
						ImGui::Checkbox( "no border##watermark lumi", &GET_VARIABLE( g_variables.m_watermark_lumi_no_border, bool ) );
						ImGui::ColorEdit4( "title color##watermark lumi", &GET_VARIABLE( g_variables.m_watermark_lumi_title_color, c_color ),
						                   color_picker_alpha_flags );
						ImGui::Checkbox( "glow##watermark lumi", &GET_VARIABLE( g_variables.m_watermark_lumi_glow, bool ) );
						if ( GET_VARIABLE( g_variables.m_watermark_lumi_glow, bool ) ) {
							ImGui::SliderFloat( "glow radius##watermark lumi", &GET_VARIABLE( g_variables.m_watermark_lumi_glow_radius, float ), 1.f, 15.f,
							                    "%.1f px" );
							ImGui::SliderFloat( "glow opacity##watermark lumi", &GET_VARIABLE( g_variables.m_watermark_lumi_glow_opacity, float ), 0.f, 1.f,
							                    "%.2f" );
							ImGui::Checkbox( "shadow color##watermark lumi", &GET_VARIABLE( g_variables.m_watermark_lumi_shadow_color, bool ) );
						}
						ImGui::SliderFloat( "offset x##watermark lumi", &GET_VARIABLE( g_variables.m_watermark_lumi_offset_x, float ), -500.f, 500.f, "%.0f px" );
						ImGui::SliderFloat( "offset y##watermark lumi", &GET_VARIABLE( g_variables.m_watermark_lumi_offset_y, float ), -500.f, 500.f, "%.0f px" );
					}

					ImGui::Checkbox( "gif next to watermark##watermark gif", &GET_VARIABLE( g_variables.m_watermark_gif, bool ) );
					if ( GET_VARIABLE( g_variables.m_watermark_gif, bool ) ) {
						auto& gif_type = GET_VARIABLE( g_variables.m_watermark_gif_type, int );
						if ( gif_type < 0 || gif_type > 2 )
							gif_type = 0;
						ImGui::Combo( "gif type##watermark gif type", &gif_type, "original\0shit\0custom\0" );

						if ( gif_type == 2 )
							image_path_input( "gif file path##watermark gif path", "C:\\mascot.gif or imgur link", GET_VARIABLE( g_variables.m_watermark_gif_path, std::string ),
							                  false );

						auto& gif_side = GET_VARIABLE( g_variables.m_watermark_gif_side, int );
						if ( gif_side < 0 || gif_side > 2 )
							gif_side = 0;
						ImGui::Combo( "gif side##watermark gif side", &gif_side, "auto\0left\0right\0" );

						ImGui::SliderFloat( "gif scale##watermark gif", &GET_VARIABLE( g_variables.m_watermark_gif_scale, float ), 25.f, 500.f, "%.0f%%" );
						ImGui::SliderFloat( "gif rotation##watermark gif", &GET_VARIABLE( g_variables.m_watermark_gif_rotation, float ), -180.f, 180.f, "%.0f deg" );
						ImGui::SliderFloat( "gif gap##watermark gif", &GET_VARIABLE( g_variables.m_watermark_gif_gap, float ), -50.f, 50.f, "%.0f px" );
						ImGui::SliderFloat( "gif offset x##watermark gif", &GET_VARIABLE( g_variables.m_watermark_gif_offset_x, float ), -500.f, 500.f, "%.0f px" );
						ImGui::SliderFloat( "gif offset y##watermark gif", &GET_VARIABLE( g_variables.m_watermark_gif_offset_y, float ), -500.f, 500.f, "%.0f px" );
						ImGui::SliderFloat( "gif alpha##watermark gif", &GET_VARIABLE( g_variables.m_watermark_gif_alpha, float ), 0.f, 100.f, "%.0f%%" );
						ImGui::Checkbox( "gif mirror##watermark gif", &GET_VARIABLE( g_variables.m_watermark_gif_flip, bool ) );
					}

					if ( style != 1 )
						return;

					auto& show_flags = GET_VARIABLE( g_variables.m_watermark_kami_show_flags, int );
					ImGui::CheckboxFlags( "user##watermark kami", &show_flags, 1 );
					ImGui::CheckboxFlags( "time##watermark kami", &show_flags, 2 );
					ImGui::CheckboxFlags( "fps##watermark kami", &show_flags, 4 );

					ImGui::Checkbox( "bold font##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_bold_font, bool ) );

					ImGui::Checkbox( "custom text##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_custom_text, bool ) );
					if ( GET_VARIABLE( g_variables.m_watermark_kami_custom_text, bool ) ) {
						static char text_buffer[ 21 ] = { };
						auto& text_var                = GET_VARIABLE( g_variables.m_watermark_kami_custom_text_value, std::string );

						if ( ImGui::InputTextWithHint( "##watermark kami text", "type shit here...", text_buffer, sizeof( text_buffer ) ) )
							text_var = text_buffer;
						else if ( !ImGui::IsItemActive( ) && text_var != text_buffer ) {
							strncpy_s( text_buffer, text_var.c_str( ), sizeof( text_buffer ) - 1 );
							text_buffer[ sizeof( text_buffer ) - 1 ] = '\0';
						}
					}

					ImGui::Checkbox( "typing animation##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_typing, bool ) );
					if ( GET_VARIABLE( g_variables.m_watermark_kami_typing, bool ) ) {
						ImGui::SliderInt( "typing speed##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_typing_speed, int ), 1, 30, "%d chars/s" );
						ImGui::SliderInt( "deleting speed##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_deleting_speed, int ), 1, 30,
						                  "%d chars/s" );
						ImGui::SliderFloat( "hold typed##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_typing_inactive, float ), 0.f, 10.f,
						                    "%.1fs" );
						ImGui::SliderFloat( "hold deleted##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_deleting_inactive, float ), 0.f,
						                    10.f, "%.1fs" );
					}

					ImGui::Checkbox( "mikudere type##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_mikudere_type, bool ) );

					ImGui::Checkbox( "custom rounding##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_rounding, bool ) );
					if ( GET_VARIABLE( g_variables.m_watermark_kami_rounding, bool ) )
						ImGui::SliderFloat( "rounding##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_rounding_value, float ), 0.f, 15.f,
						                    "%.1f" );

					ImGui::Checkbox( "outline##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_outline, bool ) );
					if ( GET_VARIABLE( g_variables.m_watermark_kami_outline, bool ) ) {
						ImGui::ColorEdit4( "outline color##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_outline_color, c_color ),
						                   color_picker_alpha_flags );

						ImGui::Checkbox( "rgb outline##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_rgb_mode, bool ) );
						if ( GET_VARIABLE( g_variables.m_watermark_kami_rgb_mode, bool ) )
							ImGui::SliderFloat( "rgb speed##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_rgb_speed, float ), 0.1f, 5.f,
							                    "%.1f" );
					}

					ImGui::Checkbox( "text color like outline##watermark kami", &GET_VARIABLE( g_variables.m_watermark_kami_text_color_like_stroke, bool ) );
				},
				ImVec2( 220.f, -1.f ) );
		}

		ImGui::Checkbox( "spectator list", &GET_VARIABLE( g_variables.m_spectators_list, bool ) );
		if ( GET_VARIABLE( g_variables.m_spectators_list, bool ) ) {
			ImGui::OptionPopup(
				"spectators configuration",
				[]( ) {
					const int color_style = GET_VARIABLE( g_variables.m_spectators_list_style, int );
					const int lumi_type = color_style == 25 ? GET_VARIABLE( g_variables.m_spectators_lumi_type, int ) : -1;

					if ( color_style == 0 || color_style == 5 || color_style == 6 || color_style == 8 || color_style == 10 || color_style == 23 ||
					     color_style == 24 || color_style == 25 || color_style == 26 || color_style == 29 )
						ImGui::ColorEdit4( "spectating local color##spectator list text color one",
						                   &GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color ),
						                   color_picker_alpha_flags );

					if ( color_style == 9 )
						ImGui::ColorEdit4( "list color##spectator list text color one",
						                   &GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color ),
						                   color_picker_alpha_flags );

					if ( color_style == 27 ) {
						ImGui::Checkbox( "rainbow##spectatorlist inba", &GET_VARIABLE( g_variables.m_spectators_inba_rainbow, bool ) );
						if ( GET_VARIABLE( g_variables.m_spectators_inba_rainbow, bool ) )
							ImGui::SliderFloat( "rainbow speed##spectatorlist inba", &GET_VARIABLE( g_variables.m_spectators_inba_rainbow_speed, float ), 0.f,
							                    5.f, "%.2f" );
						else
							ImGui::ColorEdit4( "text color##spectator list text color one",
							                   &GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color ), color_picker_alpha_flags );
					}

					if ( color_style == 0 || color_style == 5 || color_style == 8 || color_style == 23 || color_style == 24 || color_style == 26 ||
					     color_style == 29 || color_style == 30 || lumi_type == 2 )
						ImGui::ColorEdit4( "spectating other color##spectator list text color two",
						                   &GET_VARIABLE( g_variables.m_spectators_list_text_color_two, c_color ),
						                   color_picker_alpha_flags );

					if ( color_style != 9 && color_style != 12 && color_style != 13 && color_style != 14 && color_style != 16 && color_style != 18 &&
					     color_style != 19 && color_style != 20 && color_style != 21 && color_style != 24 && color_style != 26 && color_style != 28 &&
					     color_style != 29 && color_style != 30 && ( color_style != 25 || lumi_type == 1 ) )
						ImGui::Combo( "listed players##spectatorlist player", &GET_VARIABLE( g_variables.m_spectators_list_type, int ),
					                  "all spectators\0local spectators\0" );

					if ( GET_VARIABLE( g_variables.m_spectators_list_style, int ) > 30 )
						GET_VARIABLE( g_variables.m_spectators_list_style, int ) = 0;

					ImGui::Combo( "style##spectatorlist style", &GET_VARIABLE( g_variables.m_spectators_list_style, int ),
				                  "default\0delusional\0interwebz\0windows xp\0clarity\0chillware\0lobotomy v2\0kamibebra\0eyes\0cumhacck\0clarity v2\0"
				                  "airplane\0bhop cheat\0millionware\0aimware\0havoc\0onetap\0airflow\0ev0lve\0legendware\0interium\0chillware v2\0"
				                  "skebob\0cumidere\0illusory\0lumi\0billware\0inkabanium\0dna\0cucumber\0howeweware\0" );

					const int style = GET_VARIABLE( g_variables.m_spectators_list_style, int );

					if ( style == 25 ) {
						auto& type = GET_VARIABLE( g_variables.m_spectators_lumi_type, int );
						if ( type < 0 || type > 2 )
							type = 0;
						ImGui::Combo( "type##spectatorlist lumi type", &type, "default\0new\0interwebz\0" );
					}

					if ( style == 1 || style == 2 || style == 3 || style == 15 || style == 22 || style == 23 || style == 24 || style == 26 || style == 28 ||
					     style == 29 || style == 30 || ( style == 25 && GET_VARIABLE( g_variables.m_spectators_lumi_type, int ) != 1 ) )
						ImGui::Checkbox( "show targets##spectatorlist targets",
						                 &GET_VARIABLE( g_variables.m_spectators_list_show_target, bool ) );

					if ( style == 22 ) {
						ImGui::Checkbox( "draw background##spectatorlist skebob", &GET_VARIABLE( g_variables.m_spectators_skebob_background, bool ) );
						ImGui::Checkbox( "draw title##spectatorlist skebob", &GET_VARIABLE( g_variables.m_spectators_skebob_title, bool ) );
					}

					if ( ( style == 0 && GET_VARIABLE( g_variables.m_spectators_list_type, int ) == 0 ) || style == 4 || style == 5 || style == 6 ||
					     style == 11 || style == 15 || style == 17 || style == 18 || style == 21 || style == 22 || style == 23 || style == 25 ||
					     style == 26 || style == 27 || style == 28 || style == 29 || style == 30 )
						ImGui::Checkbox( "always show##spectatorlist always show", &GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) );

					if ( style == 18 )
						ImGui::SliderFloat( "alpha override##spectatorlist evolve", &GET_VARIABLE( g_variables.m_watermark_evolve_alpha, float ), 0.f,
						                    100.f, "%.0f%%" );

					if ( style == 15 )
						ImGui::ColorEdit4( "blue color##spectatorlist havoc", &GET_VARIABLE( g_variables.m_spectators_havoc_color, c_color ),
						                   color_picker_alpha_flags );

					if ( style == 20 )
						ImGui::ColorEdit4( "background color##spectatorlist interium", &GET_VARIABLE( g_variables.m_spectators_interium_bg, c_color ),
						                   color_picker_alpha_flags );

					if ( style == 25 ) {
						ImGui::Checkbox( "hide GOTV##spectatorlist lumi", &GET_VARIABLE( g_variables.m_spectators_lumi_hide_gotv, bool ) );
						if ( lumi_type == 0 )
							ImGui::Checkbox( "glow##spectatorlist lumi", &GET_VARIABLE( g_variables.m_spectators_lumi_glow, bool ) );
						if ( lumi_type != 2 )
							ImGui::Checkbox( "blur background##spectatorlist lumi", &GET_VARIABLE( g_variables.m_spectators_lumi_blur, bool ) );
						if ( lumi_type == 1 ) {
							ImGui::Checkbox( "mascot##spectatorlist lumi", &GET_VARIABLE( g_variables.m_spectators_lumi_mascot, bool ) );
							if ( GET_VARIABLE( g_variables.m_spectators_lumi_mascot, bool ) ) {
								image_path_input( "mascot file path##spectatorlist lumi", "C:\\mascot.gif or imgur link",
								                  GET_VARIABLE( g_variables.m_spectators_lumi_mascot_path, std::string ), false );
								ImGui::SliderFloat( "mascot size##spectatorlist lumi", &GET_VARIABLE( g_variables.m_spectators_lumi_mascot_size, float ), 20.f,
								                    150.f, "%.0f px" );
							}
						}
					}

					if ( style == 1 )
						ImGui::Checkbox( "left side##spectatorlist delusional left",
						                 &GET_VARIABLE( g_variables.m_spectators_list_delusional_left, bool ) );

					if ( style == 5 )
						ImGui::SliderFloat( "max width##spectatorlist chillware width",
						                    &GET_VARIABLE( g_variables.m_spectators_chillware_max_width, float ), 190.f, 600.f, "%.0f" );

					if ( style == 2 ) {
						ImGui::SliderInt( "width##spectatorlist width", &GET_VARIABLE( g_variables.m_spectators_list_width, int ), 195,
						                  500 );

						ImGui::Checkbox( "background##spectatorlist interwebz background",
						                 &GET_VARIABLE( g_variables.m_spectators_list_interwebz_background, bool ) );

						ImGui::SliderInt( "label offset##spectatorlist interwebz label x",
						                  &GET_VARIABLE( g_variables.m_spectators_list_interwebz_label_x, int ), -150, 150 );
					}

					if ( style == 7 ) {
						ImGui::Checkbox( "animate new entries##spectatorlist kamibebra animate",
						                 &GET_VARIABLE( g_variables.m_spectators_kamibebra_animate, bool ) );

						ImGui::Checkbox( "dynamic list##spectatorlist kamibebra dynamic",
						                 &GET_VARIABLE( g_variables.m_spectators_kamibebra_dynamic, bool ) );

						ImGui::Checkbox( "always show##spectatorlist kamibebra always show",
						                 &GET_VARIABLE( g_variables.m_spectators_kamibebra_always_show, bool ) );
						ImGui::Checkbox( "bold font##spectatorlist kamibebra bold", &GET_VARIABLE( g_variables.m_spectators_kamibebra_bold, bool ) );
						ImGui::Checkbox( "window shadow##spectatorlist kamibebra window shadow",
						                 &GET_VARIABLE( g_variables.m_spectators_kamibebra_window_shadow, bool ) );

						ImGui::ColorEdit4( "local color##spectatorlist kamibebra local color",
						                   &GET_VARIABLE( g_variables.m_spectators_kamibebra_local_color, c_color ), color_picker_alpha_flags );
						ImGui::ColorEdit4( "other color##spectatorlist kamibebra other color",
						                   &GET_VARIABLE( g_variables.m_spectators_kamibebra_other_color, c_color ), color_picker_alpha_flags );

						ImGui::Checkbox( "local shadow##spectatorlist kamibebra local shadow",
						                 &GET_VARIABLE( g_variables.m_spectators_kamibebra_local_shadow, bool ) );
						if ( GET_VARIABLE( g_variables.m_spectators_kamibebra_local_shadow, bool ) ) {
							ImGui::SameLine( );
							ImGui::ColorEdit4( "##spectatorlist kamibebra local shadow color",
							                   &GET_VARIABLE( g_variables.m_spectators_kamibebra_local_shadow_color, c_color ),
							                   color_picker_alpha_flags );
						}

						ImGui::Checkbox( "other shadow##spectatorlist kamibebra other shadow",
						                 &GET_VARIABLE( g_variables.m_spectators_kamibebra_other_shadow, bool ) );
						if ( GET_VARIABLE( g_variables.m_spectators_kamibebra_other_shadow, bool ) ) {
							ImGui::SameLine( );
							ImGui::ColorEdit4( "##spectatorlist kamibebra other shadow color",
							                   &GET_VARIABLE( g_variables.m_spectators_kamibebra_other_shadow_color, c_color ),
							                   color_picker_alpha_flags );
						}

						ImGui::Checkbox( "custom title##spectatorlist kamibebra custom title",
						                 &GET_VARIABLE( g_variables.m_spectators_kamibebra_custom_title, bool ) );
						if ( GET_VARIABLE( g_variables.m_spectators_kamibebra_custom_title, bool ) ) {
							static char title_buffer[ 64 ] = { };
							auto& title_var                = GET_VARIABLE( g_variables.m_spectators_kamibebra_title_text, std::string );

							if ( ImGui::InputTextWithHint( "title text##spectatorlist kamibebra title", "Spectators", title_buffer, sizeof( title_buffer ) ) )
								title_var = title_buffer;
							else if ( !ImGui::IsItemActive( ) && title_var != title_buffer ) {
								strncpy_s( title_buffer, title_var.c_str( ), sizeof( title_buffer ) - 1 );
								title_buffer[ sizeof( title_buffer ) - 1 ] = '\0';
							}
						}

						ImGui::Checkbox( "bebra gif##spectatorlist kamibebra gif", &GET_VARIABLE( g_variables.m_spectators_kamibebra_gif, bool ) );
						if ( GET_VARIABLE( g_variables.m_spectators_kamibebra_gif, bool ) ) {
							ImGui::Combo( "gif type##spectatorlist kamibebra gif type", &GET_VARIABLE( g_variables.m_spectators_kamibebra_gif_type, int ),
							              "original\0shit\0custom\0" );
							if ( GET_VARIABLE( g_variables.m_spectators_kamibebra_gif_type, int ) == 2 )
								image_path_input( "gif file path##spectatorlist kamibebra gif path", "C:\\mascot.gif or imgur link",
								                  GET_VARIABLE( g_variables.m_spectators_kamibebra_gif_path, std::string ), false );
							ImGui::SliderFloat( "gif size##spectatorlist kamibebra gif size",
							                    &GET_VARIABLE( g_variables.m_spectators_kamibebra_gif_size, float ), 32.f, 128.f, "%.0f px" );
						}
					}

					if ( style == 8 )
						ImGui::SliderFloat( "size##spectatorlist eyes size", &GET_VARIABLE( g_variables.m_spectators_eyes_size, float ), 16.f,
						                    64.f, "%.0f px" );

					if ( style == 11 ) {
						ImGui::ColorEdit4( "title color##spectatorlist airplane title", &GET_VARIABLE( g_variables.m_spectators_airplane_title_color, c_color ),
						                   color_picker_alpha_flags );
						ImGui::ColorEdit4( "gradient color##spectatorlist airplane gradient",
						                   &GET_VARIABLE( g_variables.m_spectators_airplane_gradient_color, c_color ), color_picker_alpha_flags );
						ImGui::ColorEdit4( "local color##spectatorlist airplane local", &GET_VARIABLE( g_variables.m_spectators_airplane_local_color, c_color ),
						                   color_picker_alpha_flags );
						ImGui::ColorEdit4( "other color##spectatorlist airplane other", &GET_VARIABLE( g_variables.m_spectators_airplane_other_color, c_color ),
						                   color_picker_alpha_flags );

						ImGui::Checkbox( "outline text##spectatorlist airplane outline", &GET_VARIABLE( g_variables.m_spectators_airplane_outline, bool ) );
						ImGui::Combo( "title case##spectatorlist airplane title case", &GET_VARIABLE( g_variables.m_spectators_airplane_title_case, int ),
						              "Spectators\0spectators\0SPECTATORS\0" );
					}

					if ( style == 12 )
						ImGui::Checkbox( "observer mode##spectatorlist bhopcheat mode", &GET_VARIABLE( g_variables.m_spectators_bhopcheat_mode, bool ) );

					if ( ( style < 13 && style != 8 && style != 9 && style != 11 ) || style == 22 ||
					     ( style == 25 && GET_VARIABLE( g_variables.m_spectators_lumi_type, int ) != 2 ) )
						ImGui::Checkbox( "spectator avatars", &GET_VARIABLE( g_variables.m_spectators_avatar, bool ) );

					if ( style == 10 )
						ImGui::Checkbox( "always show##spectatorlist clarity v2", &GET_VARIABLE( g_variables.m_spectators_always_show, bool ) );
				},
				ImVec2( 200.f, -1.f ) );
		}

		ImGui::Checkbox( "media player", &GET_VARIABLE( g_variables.m_media_player, bool ) );
		if ( GET_VARIABLE( g_variables.m_media_player, bool ) ) {
			ImGui::OptionPopup(
				"media player configuration",
				[]( ) {
					auto& position = GET_VARIABLE( g_variables.m_media_player_position, int );
					if ( position < 0 || position > 5 )
						position = 0;
					ImGui::Combo( "position##media player position", &position, "top right\0top left\0top center\0bottom center\0bottom left\0bottom right\0" );

					ImGui::Checkbox( "simple", &GET_VARIABLE( g_variables.m_media_player_simple, bool ) );
					if ( !GET_VARIABLE( g_variables.m_media_player_simple, bool ) ) {
						ImGui::Checkbox( "cover art", &GET_VARIABLE( g_variables.m_media_player_cover_art, bool ) );
						ImGui::Checkbox( "background", &GET_VARIABLE( g_variables.m_media_player_background, bool ) );
						ImGui::Checkbox( "progress bar", &GET_VARIABLE( g_variables.m_media_player_progress_bar, bool ) );

						ImGui::Checkbox( "waveform",&GET_VARIABLE( g_variables.m_media_player_visualizer, bool ) );
						if ( GET_VARIABLE( g_variables.m_media_player_visualizer, bool ) ) {
							ImGui::SliderFloat( "height##visualizer", &GET_VARIABLE( g_variables.m_media_player_visualizer_height, float ), 4.f, 64.f,
							                    "%.0f px", ImGuiSliderFlags_AlwaysClamp );
							ImGui::Checkbox( "smooth##visualizer", &GET_VARIABLE( g_variables.m_media_player_visualizer_smooth, bool ) );
							if ( !GET_VARIABLE( g_variables.m_media_player_visualizer_smooth, bool ) )
								ImGui::SliderInt( "bar width##visualizer", &GET_VARIABLE( g_variables.m_media_player_visualizer_bar_width, int ), 1, 12, "%d px",
								                  ImGuiSliderFlags_AlwaysClamp );
							ImGui::Checkbox( "custom color##visualizer", &GET_VARIABLE( g_variables.m_media_player_visualizer_custom_color, bool ) );
							if ( GET_VARIABLE( g_variables.m_media_player_visualizer_custom_color, bool ) ) {
								ImGui::SameLine( );
								ImGui::ColorEdit4( "##visualizer color", &GET_VARIABLE( g_variables.m_media_player_visualizer_color, c_color ),
								                   color_picker_alpha_flags );
							}
						}
					}

					ImGui::Checkbox( "lyrics", &GET_VARIABLE( g_variables.m_media_player_lyrics, bool ) );
					if ( GET_VARIABLE( g_variables.m_media_player_lyrics, bool ) ) {
						ImGui::SliderInt( "rows##lyrics", &GET_VARIABLE( g_variables.m_media_player_lyrics_rows, int ), 1, 9, "%d", ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "offset##lyrics", &GET_VARIABLE( g_variables.m_media_player_lyrics_offset, float ), -3000.f, 3000.f, "%.0f ms",
						                    ImGuiSliderFlags_AlwaysClamp );
					}

					ImGui::Checkbox( "precise time", &GET_VARIABLE( g_variables.m_media_player_time, bool ) );

					ImGui::Checkbox( "in game control", &GET_VARIABLE( g_variables.m_media_player_ingame_control, bool ) );
					if ( GET_VARIABLE( g_variables.m_media_player_ingame_control, bool ) ) {
						ImGui::Label( "previous song" );
						ImGui::Keybind( "previous##media key", &GET_VARIABLE( g_variables.m_media_player_previous_key, key_bind_t ), false );
						ImGui::Label( "pause / play" );
						ImGui::Keybind( "pause##media key", &GET_VARIABLE( g_variables.m_media_player_toggle_key, key_bind_t ), false );
						ImGui::Label( "next song" );
						ImGui::Keybind( "next##media key", &GET_VARIABLE( g_variables.m_media_player_next_key, key_bind_t ), false );
					}
				},
				ImVec2( 200.f, -1.f ) );
		}

		ImGui::Checkbox( "points", &GET_VARIABLE( g_variables.m_points, bool ) );
		if ( GET_VARIABLE( g_variables.m_points, bool ) ) {
			ImGui::OptionPopup(
				"points configuration",
				[]( ) {
					ImGui::Checkbox( "big font##points", &GET_VARIABLE( g_variables.m_points_big_font, bool ) );
					ImGui::SliderFloat( "appear speed##points", &GET_VARIABLE( g_variables.m_points_appear_speed, float ), 0.5f, 20.f, "x%.2f" );
					ImGui::SliderFloat( "bob speed##points", &GET_VARIABLE( g_variables.m_points_speed, float ), 0.1f, 10.f, "x%.2f" );
					ImGui::SliderFloat( "x offset##points", &GET_VARIABLE( g_variables.m_points_x_offset, float ), -300.f, 300.f, "%.0f px" );
					ImGui::SliderFloat( "y offset##points", &GET_VARIABLE( g_variables.m_points_y_offset, float ), -50.f, 50.f, "%.0f px" );
				},
				ImVec2( 200.f, -1.f ) );
		}

		ImGui::Checkbox( "low fps warning", &GET_VARIABLE( g_variables.m_fps_warning, bool ) );

		ImGui::Checkbox( "hit marker", &GET_VARIABLE( g_variables.m_hit_marker, bool ) );
		if ( GET_VARIABLE( g_variables.m_hit_marker, bool ) ) {
			ImGui::OptionPopup(
				"hit marker configuration",
				[]( ) {
					ImGui::SliderFloat( "size##hitmarkersize", &GET_VARIABLE( g_variables.m_hit_marker_size, float ), 4.f, 30.f, "%.0f" );
					ImGui::SliderFloat( "speed##hitmarkerspeed", &GET_VARIABLE( g_variables.m_hit_marker_speed, float ), 0.25f, 3.f,
					                    "%.2f" );
					ImGui::SliderFloat( "thickness##hitmarkerthickness", &GET_VARIABLE( g_variables.m_hit_marker_thickness, float ), 0.5f,
					                    4.f, "%.1f" );
					ImGui::ColorEdit4( "color##hitmarkercolor", &GET_VARIABLE( g_variables.m_hit_marker_color, c_color ),
					                   color_picker_alpha_flags );

					ImGui::Checkbox( "outline##hitmarkeroutline", &GET_VARIABLE( g_variables.m_hit_marker_outline, bool ) );
					if ( GET_VARIABLE( g_variables.m_hit_marker_outline, bool ) ) {
						ImGui::ColorEdit4( "outline color##hitmarkeroutlinecolor",
						                   &GET_VARIABLE( g_variables.m_hit_marker_outline_color, c_color ), color_picker_alpha_flags );
						ImGui::SliderFloat( "outline thickness##hitmarkeroutlinethickness",
						                    &GET_VARIABLE( g_variables.m_hit_marker_outline_thickness, float ), 0.5f, 4.f, "%.1f" );
					}
				},
				ImVec2( 220.f, -1.f ) );
		}

		ImGui::Checkbox( "hit sound", &GET_VARIABLE( g_variables.m_hit_sound, bool ) );
		if ( GET_VARIABLE( g_variables.m_hit_sound, bool ) ) {
			ImGui::OptionPopup(
				"hit sound configuration",
				[]( ) {
					ImGui::Combo( "sound##hitsound", &GET_VARIABLE( g_variables.m_hit_sound_type, int ),
				                  "arena switch\0button\0money\0beep\0custom\0" );

					if ( GET_VARIABLE( g_variables.m_hit_sound_type, int ) == 4 ) {
						auto&       hs_snd_var           = GET_VARIABLE( g_variables.m_hit_sound_custom, std::string );
						static char hs_snd_buffer[ 128 ] = { };
						if ( ImGui::InputText( "sound path##hitsndpath", hs_snd_buffer, sizeof( hs_snd_buffer ) ) )
							hs_snd_var = hs_snd_buffer;
						else if ( !ImGui::IsItemActive( ) && hs_snd_var != hs_snd_buffer ) {
							const std::size_t len =
								hs_snd_var.size( ) < sizeof( hs_snd_buffer ) - 1 ? hs_snd_var.size( ) : sizeof( hs_snd_buffer ) - 1;
							std::memcpy( hs_snd_buffer, hs_snd_var.c_str( ), len );
							hs_snd_buffer[ len ] = '\0';
						}
					}

					ImGui::SliderFloat( "volume##hitsndvol", &GET_VARIABLE( g_variables.m_hit_sound_volume, float ), 0.f, 2.f, "%.2f" );
				},
				ImVec2( 220.f, -1.f ) );
		}

		ImGui::Checkbox( "healthshot effect", &GET_VARIABLE( g_variables.m_healthshot_effect, bool ) );
		if ( GET_VARIABLE( g_variables.m_healthshot_effect, bool ) ) {
			ImGui::OptionPopup(
				"healthshot effect settings",
				[]( ) {
					ImGui::MultiCombo( "trigger on##healthshot", GET_VARIABLE( g_variables.m_healthshot_triggers, std::vector< bool > ),
					                   { "kill", "edgebug", "pixelsurf" }, GET_VARIABLE( g_variables.m_healthshot_triggers, std::vector< bool > ).size( ) );
					ImGui::SliderFloat( "duration##healthshot", &GET_VARIABLE( g_variables.m_healthshot_duration, float ), 0.1f, 5.f, "%.1f s" );
				},
				ImVec2( 220.f, -1.f ) );
		}

		/* where a player died; a pick the map never precached falls back to an energy splash.
		   smoke / soul / bouncy balls / melt / minecraft are our own particles, balls only on your own kills */
		ImGui::Checkbox( "death particles", &GET_VARIABLE( g_variables.m_death_particles, bool ) );
		if ( GET_VARIABLE( g_variables.m_death_particles, bool ) ) {
			ImGui::OptionPopup(
				"death particles settings",
				[]( ) {
					ImGui::Combo( "effect##deathparticles", &GET_VARIABLE( g_variables.m_death_particles_type, int ),
					              "blood burst\0sparks\0confetti\0smoke puff\0explosion\0soul\0bouncy balls\0melt\0minecraft\0" );

					const int type = GET_VARIABLE( g_variables.m_death_particles_type, int );
					if ( type < 5 && type != 3 )
						return;

					if ( type == 8 )
						ImGui::Combo( "drops##deathparticles", &GET_VARIABLE( g_variables.m_death_particles_mc_mode, int ),
						              "only x\0both x and xp\0only xp\0" );

					if ( type == 5 )
						ImGui::Combo( "soul##deathparticles", &GET_VARIABLE( g_variables.m_death_particles_soul_style, int ),
						              "energy\0heavy electrical\0light electrical\0core\0" );

					if ( type != 3 && type != 7 && type != 8 ) {
						ImGui::Checkbox( "multicolor##deathparticles", &GET_VARIABLE( g_variables.m_death_particles_multicolor, bool ) );
						c_color& dp_color = GET_VARIABLE( g_variables.m_death_particles_color, c_color );
						if ( !GET_VARIABLE( g_variables.m_death_particles_multicolor, bool ) )
							ImGui::ColorEdit4( "color##deathparticles", &dp_color, color_picker_alpha_flags );
						else {
							int dp_alpha = dp_color[ 3 ];
							if ( ImGui::SliderInt( "alpha##deathparticles", &dp_alpha, 0, 255, "%d", ImGuiSliderFlags_AlwaysClamp ) )
								dp_color[ 3 ] = static_cast< std::uint8_t >( dp_alpha );
						}
					}
					if ( type == 5 )
						ImGui::Checkbox( "glow##deathparticles", &GET_VARIABLE( g_variables.m_death_particles_glow, bool ) );

					if ( type == 7 ) {
						ImGui::SliderFloat( "goo##deathparticlesmelt", &GET_VARIABLE( g_variables.m_death_particles_melt_goo, float ), 0.5f, 3.f, "%.2fx body",
						                    ImGuiSliderFlags_AlwaysClamp );
						ImGui::SliderFloat( "speed##deathparticlesmelt", &GET_VARIABLE( g_variables.m_death_particles_melt_speed, float ), 0.25f, 3.f, "%.2fx",
						                    ImGuiSliderFlags_AlwaysClamp );
					} else if ( type == 6 )
						ImGui::SliderInt( "balls##deathparticles", &GET_VARIABLE( g_variables.m_death_particles_amount, int ), 5, 100, "%d",
						                  ImGuiSliderFlags_AlwaysClamp );
					else if ( type != 8 )
						ImGui::SliderInt( "amount##deathparticles", &GET_VARIABLE( g_variables.m_death_particles_amount, int ), 1, 40,
						                  "%d per bone", ImGuiSliderFlags_AlwaysClamp );

					if ( type != 7 )
						ImGui::SliderFloat( "size##deathparticles", &GET_VARIABLE( g_variables.m_death_particles_size, float ), 0.5f, type == 6 ? 4.f : 3.f,
						                    "%.1fx", ImGuiSliderFlags_AlwaysClamp );
					if ( type != 8 )
						ImGui::SliderFloat( "lifetime##deathparticles", &GET_VARIABLE( g_variables.m_death_particles_lifetime, float ), 1.f, 8.f,
						                    "%.1f s", ImGuiSliderFlags_AlwaysClamp );
					else if ( GET_VARIABLE( g_variables.m_death_particles_mc_mode, int ) != 0 )
						ImGui::SliderFloat( "xp stays##deathparticles", &GET_VARIABLE( g_variables.m_death_particles_mc_xp_time, float ), 1.f, 60.f,
						                    "%.0f s", ImGuiSliderFlags_AlwaysClamp );
				},
				ImVec2( 220.f, -1.f ) );
		}

		ImGui::Checkbox( "console color", &GET_VARIABLE( g_variables.m_console_color_enable, bool ) );
		if ( GET_VARIABLE( g_variables.m_console_color_enable, bool ) ) {
			ImGui::SameLine( );
			ImGui::ColorEdit4( "##console color", &GET_VARIABLE( g_variables.m_console_color, c_color ), color_picker_alpha_flags );
		}

		ImGui::MultiCombo( "displayed logs", GET_VARIABLE( g_variables.m_log_types, std::vector< bool > ),
		                   { "damage", "team damage", "purchase", "votes" },
		                   GET_VARIABLE( g_variables.m_log_types, std::vector< bool > ).size( ) );
	}
	menu_group_end( );

	menu_columns_next( );

	if ( menu_group_begin( "game" ) ) {
		safe_checkbox( "clantag", &GET_VARIABLE( g_variables.m_clantag, bool ) );
		if ( GET_VARIABLE( g_variables.m_clantag, bool ) && !GET_VARIABLE( g_variables.m_safe_mode, bool ) ) {
			ImGui::OptionPopup(
				"clantag configuration",
				[]( ) {
					auto& anim = GET_VARIABLE( g_variables.m_clantag_animation, int );
					if ( anim > 4 )
						anim = 4;

					ImGui::Combo( "animation##clantag", &anim, "static\0typewriter\0scroll\0frame by frame\0now playing\0" );

					static char tag_buffer[ 25 ] = { };
					auto& tag_var                = GET_VARIABLE( g_variables.m_clantag_text, std::string );

					if ( anim != 3 ) {
						if ( ImGui::InputText( "tag text##clantag", tag_buffer, sizeof( tag_buffer ) ) )
							tag_var = tag_buffer;
						else if ( !ImGui::IsItemActive( ) && tag_var != tag_buffer ) {
							strncpy_s( tag_buffer, tag_var.c_str( ), sizeof( tag_buffer ) - 1 );
							tag_buffer[ sizeof( tag_buffer ) - 1 ] = '\0';
						}
					}

					if ( anim == 3 ) {
						static std::vector< n_misc::clantag_frame_t > rows;
						static std::string synced = "\x01"; /* never a real value, first draw always splits */
						static int selected       = 0;
						auto& frames_var          = GET_VARIABLE( g_variables.m_clantag_frames, std::string );

						if ( frames_var != synced ) {
							rows   = n_misc::parse_clantag_frames( frames_var, GET_VARIABLE( g_variables.m_clantag_speed, float ) );
							synced = frames_var;
						}

						bool changed     = false;
						const int count  = static_cast< int >( rows.size( ) );
						const float half = ( ImGui::GetRowFrameWidth( ) - 14.f ) * 0.5f;

						ImGui::Label( "frames" );
						if ( ImGui::BeginListBox( "##clantag frames", ImVec2( 0.f, 92.f ) ) ) {
							for ( int i = 0; i < count; i++ ) {
								char row[ 64 ];
								snprintf( row, sizeof( row ), "%d. %s   %.1fs", i + 1, rows[ i ].m_text.empty( ) ? "(empty)" : rows[ i ].m_text.c_str( ),
								          rows[ i ].m_hold );

								ImGui::PushID( i );
								if ( ImGui::Selectable( row, selected == i, ImGuiSelectableFlags_DontClosePopups ) )
									selected = i;
								ImGui::PopID( );
							}
							ImGui::EndListBox( );
						}

						selected = std::clamp( selected, 0, ( std::max )( count - 1, 0 ) );

						if ( count > 0 ) {
							auto& frame = rows[ selected ];

							char buffer[ 25 ];
							strncpy_s( buffer, frame.m_text.c_str( ), _TRUNCATE );
							if ( ImGui::InputText( "frame text##clantag", buffer, sizeof( buffer ) ) ) {
								frame.m_text = buffer;
								changed      = true;
							}

							if ( ImGui::SliderFloat( "hold time##clantag frame", &frame.m_hold, 0.1f, 10.f, "%.1fs", ImGuiSliderFlags_AlwaysClamp ) )
								changed = true;

							if ( ImGui::Button( "move up##clantag", ImVec2( half, 15.f ) ) && selected > 0 ) {
								std::swap( rows[ selected ], rows[ selected - 1 ] );
								selected -= 1;
								changed = true;
							}
							ImGui::SameLine( );
							if ( ImGui::Button( "move down##clantag", ImVec2( -1.f, 15.f ) ) && selected + 1 < count ) {
								std::swap( rows[ selected ], rows[ selected + 1 ] );
								selected += 1;
								changed = true;
							}
						}

						if ( ImGui::Button( "add frame##clantag", ImVec2( half, 15.f ) ) && count < 32 ) {
							n_misc::clantag_frame_t frame;
							if ( count > 0 )
								frame.m_hold = rows[ selected ].m_hold;

							selected = count > 0 ? selected + 1 : 0;
							rows.insert( rows.begin( ) + selected, frame );
							changed = true;
						}
						ImGui::SameLine( );
						if ( ImGui::Button( "remove frame##clantag", ImVec2( -1.f, 15.f ) ) && count > 0 ) {
							rows.erase( rows.begin( ) + selected );
							changed = true;
						}

						if ( changed ) {
							frames_var = n_misc::join_clantag_frames( rows );
							synced     = frames_var;
						}
					}

					if ( anim == 4 )
						ImGui::TextDisabled( "tag text shows when nothing plays" );
					if ( anim == 1 || anim == 2 || anim == 4 )
						ImGui::SliderFloat( "speed##clantag", &GET_VARIABLE( g_variables.m_clantag_speed, float ), 0.1f, 2.f, "%.1fs" );
					if ( anim == 1 )
						ImGui::SliderFloat( "hold time##clantag", &GET_VARIABLE( g_variables.m_clantag_hold, float ), 0.5f, 10.f, "%.1fs" );
				},
				ImVec2( 200.f, -1.f ) );
		}

		ImGui::Checkbox( "chat translator", &GET_VARIABLE( g_variables.m_chat_translator, bool ) );
		if ( GET_VARIABLE( g_variables.m_chat_translator, bool ) ) {
			ImGui::OptionPopup(
				"chat translator settings",
				[]( ) {
					ImGui::Combo( "translate to##chat translator", &GET_VARIABLE( g_variables.m_chat_translator_language, int ),
					              n_chat_extras::k_language_names );
				},
				ImVec2( 220.f, -1.f ) );
		}

		ImGui::Checkbox( "chat images", &GET_VARIABLE( g_variables.m_chat_images, bool ) );

		ImGui::Checkbox( "bot names", &GET_VARIABLE( g_variables.m_bot_names, bool ) );
		if ( GET_VARIABLE( g_variables.m_bot_names, bool ) ) {
			ImGui::OptionPopup(
				"bot names configuration",
				[]( ) {
					const int mode = GET_VARIABLE( g_variables.m_bot_names_mode, int );
					ImGui::Combo( "names##botnames", &GET_VARIABLE( g_variables.m_bot_names_mode, int ),
					              "original\0same name\0random string\0steam names\0" );

					if ( mode == 1 ) {
						auto& name_var               = GET_VARIABLE( g_variables.m_bot_names_same, std::string );
						static char name_buffer[ 128 ] = { };
						if ( ImGui::InputText( "name##botnames", name_buffer, sizeof( name_buffer ) ) )
							name_var = name_buffer;
						else if ( !ImGui::IsItemActive( ) && name_var != name_buffer ) {
							const std::size_t len = name_var.size( ) < sizeof( name_buffer ) - 1 ? name_var.size( ) : sizeof( name_buffer ) - 1;
							std::memcpy( name_buffer, name_var.c_str( ), len );
							name_buffer[ len ] = '\0';
						}
					} else if ( mode == 2 ) {
						ImGui::SliderInt( "length##botnames", &GET_VARIABLE( g_variables.m_bot_names_length, int ), 1, 32 );
					} else if ( mode == 3 ) {
						ImGui::Label( "%s", bot_names_status( ).c_str( ) );
					}

					ImGui::Checkbox( "remove bot prefix##botnames", &GET_VARIABLE( g_variables.m_bot_names_no_prefix, bool ) );
					ImGui::Checkbox( "random ping##botnames", &GET_VARIABLE( g_variables.m_bot_names_ping, bool ) );
					ImGui::Checkbox( "random rank + medal##botnames", &GET_VARIABLE( g_variables.m_bot_names_profile, bool ) );

					if ( ImGui::Button( "randomize##botnames", ImVec2( -1.f, 15.f ) ) )
						bot_names_randomize( );
				},
				ImVec2( 220.f, -1.f ) );
		}

		ImGui::Checkbox( "auto revive", &GET_VARIABLE( g_variables.m_auto_revive, bool ) );
		if ( GET_VARIABLE( g_variables.m_auto_revive, bool ) ) {
			ImGui::OptionPopup(
				"auto revive settings",
				[]( ) {
					ImGui::Combo( "revive##autorevive", &GET_VARIABLE( g_variables.m_auto_revive_mode, int ),
					              "revive everyone\0revive only yourself\0only targets\0yourself and targets\0" );
					if ( GET_VARIABLE( g_variables.m_auto_revive_mode, int ) >= 2 )
						ImGui::Label( "targets: misc > player list" );
				},
				ImVec2( 220.f, -1.f ) );
		}

		ImGui::Checkbox( "deagle spinner", &GET_VARIABLE( g_variables.m_deagle_spinner, bool ) );
		ImGui::Keybind( "deagle spinner key", &GET_VARIABLE( g_variables.m_deagle_spinner_key, key_bind_t ) );

		safe_checkbox( "sv_pure bypass", &GET_VARIABLE( g_variables.m_pure_bypass, bool ) );

		ImGui::Checkbox( "performance", &GET_VARIABLE( g_variables.m_performance, bool ) );
		if ( GET_VARIABLE( g_variables.m_performance, bool ) ) {
			ImGui::OptionPopup(
				"performance configuration",
				[]( ) {
					ImGui::Checkbox( "threaded bone setup", &GET_VARIABLE( g_variables.m_performance_threaded_bones, bool ) );

					ImGui::Checkbox( "preload assets", &GET_VARIABLE( g_variables.m_performance_force_preload, bool ) );

					ImGui::Checkbox( "skip unused bone records",
					                 &GET_VARIABLE( g_variables.m_performance_skip_unused_records, bool ) );

					ImGui::Checkbox( "multicore rendering", &GET_VARIABLE( g_variables.m_performance_multicore_render, bool ) );

					ImGui::Checkbox( "high priority", &GET_VARIABLE( g_variables.m_performance_high_priority, bool ) );

					ImGui::Checkbox( "game on fast cores", &GET_VARIABLE( g_variables.m_performance_fast_cores, bool ) );

					ImGui::Checkbox( "full fps when tabbed out", &GET_VARIABLE( g_variables.m_performance_no_focus_sleep, bool ) );				},
				ImVec2( 220.f, -1.f ) );
		}

		ImGui::Checkbox( "discord rpc", &GET_VARIABLE( g_variables.m_discord_rpc, bool ) );
		if ( GET_VARIABLE( g_variables.m_discord_rpc, bool ) ) {
			ImGui::OptionPopup(
				"discord rpc configuration",
				[]( ) {
					image_path_input( "image url##discord rpc", "https://i.imgur.com/KDUzwZf.png", GET_VARIABLE( g_variables.m_discord_rpc_image, std::string ),
					                  true );
				},
				ImVec2( 200.f, -1.f ) );
		}

#ifdef _DEBUG
		ImGui::Checkbox( "debugger menu", &GET_VARIABLE( g_variables.m_debugger_visual, bool ) );
#endif
	}
	menu_group_end( );

	if ( menu_group_begin( "convenience" ) ) {
		struct command_entry_t {
			const char* m_label;
			const char* m_command;
		};

		static constexpr command_entry_t commands[ ] = {
			{ "kill all bots", "sv_cheats 1; bot_kill all"       },
			{ "kick all bots", "bot_kick all"                    },
			{ "give ak47",     "sv_cheats 1; give weapon_ak47"   },
			{ "give awp",      "sv_cheats 1; give weapon_awp"    },
			{ "give deagle",   "sv_cheats 1; give weapon_deagle" },
			{ "give usp",      "sv_cheats 1; give weapon_usp_silencer"     },
		};

		if ( ImGui::Button( "connect y6o EU", ImVec2( -1.f, 15.f ) ) )
			g_interfaces.m_engine_client->client_cmd_unrestricted( "connect 193.23.209.155:27015" );

		for ( const auto& command : commands ) {
			if ( ImGui::Button( command.m_label, ImVec2( -1.f, 15.f ) ) )
				g_interfaces.m_engine_client->client_cmd_unrestricted( command.m_command );
		}
	}
	menu_group_end( );

	menu_columns_end( );
}

/* list_rows 0 = fill what the child has left above the 6 buttons */
static void config_panel( const bool visuals, int& selected, char* file_buffer, const std::size_t buffer_size, int list_rows )
{
	struct popup_state_t {
		bool m_save = false, m_load = false, m_remove = false;
	};
	static popup_state_t popup_states[ 2 ] = { };
	popup_state_t& popups                  = popup_states[ visuals ];

	const char* const kind = visuals ? "visuals config" : "config";
	auto& names            = g_config.file_names( visuals );

	ImGui::InputText( visuals ? "visuals file name" : "config file name", file_buffer, buffer_size, 0, 0, 0, -7.f );

	std::string converted_file_name = file_buffer;

	if ( list_rows <= 0 ) {
		const ImGuiStyle& style   = ImGui::GetStyle( );
		const float buttons       = 6.f * ( 15.f + style.ItemSpacing.y );
		const float row           = ImGui::GetTextLineHeightWithSpacing( );
		const float list_space    = ImGui::GetContentRegionAvail( ).y - buttons - style.ItemSpacing.y - 6.f;
		list_rows                 = std::clamp( static_cast< int >( list_space / row - 0.25f ), 3, 12 );
	}

	ImGui::SetNextItemWidth( ImGui::GetContentRegionAvail( ).x - 2.f );

	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_FramePadding, ImVec2( 0.f, 3.f ) );

	ImGui::ListBox( visuals ? "##visuals config list" : "##config list", &selected,
	                [ &names ]( int index ) { return names[ index ].c_str( ); }, static_cast< int >( names.size( ) ), list_rows );

	ImGui::PopStyleVar( );

	if ( selected < 0 || selected >= static_cast< int >( names.size( ) ) )
		selected = 0;

	std::string selected_name = !names.empty( ) ? names[ selected ] : "";

	if ( ImGui::Button( ( "create" ), ImVec2( -1.f, 15.f ) ) && !converted_file_name.empty( ) ) {
		if ( !g_config.save( converted_file_name, visuals ) )
			g_console.print( std::vformat( "failed to create {:s}", std::make_format_args( converted_file_name ) ).c_str( ) );
		else
			g_logger.print( std::vformat( "created {:s} {:s}", std::make_format_args( kind, converted_file_name ) ).c_str( ) );

		g_config.refresh( );
	}

	if ( ImGui::Button( ( "save" ), ImVec2( -1.f, 15.f ) ) )
		popups.m_save = true;

	if ( ImGui::Button( ( "load" ), ImVec2( -1.f, 15.f ) ) )
		popups.m_load = true;

	if ( ImGui::Button( ( "remove" ), ImVec2( -1.f, 15.f ) ) )
		popups.m_remove = true;

	if ( ImGui::Button( ( "refresh" ), ImVec2( -1.f, 15.f ) ) )
		g_config.refresh( );

	if ( ImGui::Button( ( "open folder" ), ImVec2( -1.f, 15.f ) ) )
		ShellExecuteA( nullptr, "open", g_config.folder( visuals ).string( ).c_str( ), nullptr, nullptr, SW_SHOWNORMAL );

	/* nothing selected: a save/load/remove on "" would write ".bx" or hit an empty list */
	if ( names.empty( ) )
		popups = { };

	if ( popups.m_save ) {
		save_popup( "save confirmation", popups.m_save, ImVec2( 220.f, -1.f ), [ & ]( ) {
			if ( ImGui::Button( "yes", ImVec2( ( ImGui::GetRowFrameWidth( ) - 14.f ) * 0.5f, 15.f ) ) ) {
				if ( !g_config.save( selected_name, visuals ) )
					g_console.print( std::vformat( "failed to save {:s}", std::make_format_args( selected_name ) ).c_str( ) );
				else
					g_logger.print( std::vformat( "saved {:s} {:s}", std::make_format_args( kind, selected_name ) ).c_str( ) );

				popups.m_save = false;
			}

			ImGui::SameLine( );

			if ( ImGui::Button( "no", ImVec2( -1.f, 15.f ) ) ) {
				g_logger.print( std::vformat( "canceled saving {:s} {:s}", std::make_format_args( kind, selected_name ) ).c_str( ) );

				popups.m_save = false;
			}
		} );
	}

	if ( popups.m_load ) {
		save_popup( "load confirmation", popups.m_load, ImVec2( 220.f, -1.f ), [ & ]( ) {
			if ( ImGui::Button( "yes", ImVec2( ( ImGui::GetRowFrameWidth( ) - 14.f ) * 0.5f, 15.f ) ) ) {
				if ( !g_config.load( selected_name, visuals ) )
					g_console.print( std::vformat( "failed to load {:s}", std::make_format_args( selected_name ) ).c_str( ) );
				else {
					g_logger.print( std::vformat( "loaded {:s} {:s}", std::make_format_args( kind, selected_name ) ).c_str( ) );
					if ( !visuals )
						g_render.m_reload_fonts = true;
				}

				popups.m_load = false;
			}

			ImGui::SameLine( );

			if ( ImGui::Button( "no", ImVec2( -1.f, 15.f ) ) )
				popups.m_load = false;
		} );
	}

	if ( popups.m_remove ) {
		save_popup( "remove confirmation", popups.m_remove, ImVec2( 220.f, -1.f ), [ & ]( ) {
			if ( ImGui::Button( "yes", ImVec2( ( ImGui::GetRowFrameWidth( ) - 14.f ) * 0.5f, 15.f ) ) ) {
				g_logger.print( std::vformat( "removed {:s} {:s}", std::make_format_args( kind, selected_name ) ).c_str( ) );

				g_config.remove( static_cast< std::size_t >( selected ), visuals );
				selected = 0;

				popups.m_remove = false;
			}

			ImGui::SameLine( );

			if ( ImGui::Button( "no", ImVec2( -1.f, 15.f ) ) )
				popups.m_remove = false;
		} );
	}
}

void n_menu::impl_t::tab_settings( )
{
	menu_columns_begin( );

	if ( menu_group_begin( "config" ) ) {
		config_panel( false, this->m_selected_config, this->m_config_file, sizeof( this->m_config_file ), 12 );
	}
	menu_group_end( );

	if ( menu_group_begin( "system" ) ) {
		ImGui::Checkbox( "debug log", &GET_VARIABLE( g_variables.m_debug_log, bool ) );
		ImGui::Checkbox( "safe mode", &GET_VARIABLE( g_variables.m_safe_mode, bool ) );

		safe_checkbox( "botox network", &GET_VARIABLE( g_variables.m_botox_network, bool ) );
		if ( GET_VARIABLE( g_variables.m_botox_network, bool ) && !GET_VARIABLE( g_variables.m_safe_mode, bool ) ) {
			ImGui::Checkbox( "share esp", &GET_VARIABLE( g_variables.m_botox_network_esp, bool ) );
			ImGui::Checkbox( "share skins", &GET_VARIABLE( g_variables.m_botox_network_skins, bool ) );
			ImGui::Text( "botox users here: %d", g_botox_net.m_users.load( std::memory_order_relaxed ) );
		}
	}
	menu_group_end( );

	menu_columns_next( );

	if ( menu_group_begin( "visuals config" ) ) {
		config_panel( true, this->m_selected_visual_config, this->m_visual_config_file, sizeof( this->m_visual_config_file ), 12 );
	}
	menu_group_end( );

	if ( menu_group_begin( "menu" ) ) {
		ImGui::ColorEdit4( "accent color##accent color", &GET_VARIABLE( g_variables.m_accent, c_color ), color_picker_no_alpha_flags );

		{
			float& dpi_scale       = GET_VARIABLE( g_variables.m_dpi_scale, float );
			static float dpi_edit  = 100.f;
			static bool dpi_active = false;

			if ( !dpi_active )
				dpi_edit = dpi_scale;

			ImGui::SliderFloat( "dpi scale", &dpi_edit, 25.f, 200.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp );

			dpi_active = ImGui::IsItemActive( );
			if ( !dpi_active )
				dpi_scale = dpi_edit;

			ImGui::Checkbox( "dpi scale panels", &GET_VARIABLE( g_variables.m_dpi_scale_panels, bool ) );
		}
	}
	menu_group_end( );

	menu_columns_end( );
}
