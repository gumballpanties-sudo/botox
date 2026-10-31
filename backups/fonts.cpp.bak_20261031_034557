#include "fonts.h"
#include "../includes/includes.h"
#include "../../dependencies/fonts/spectator_fonts.h"
#include "../../dependencies/imgui/helpers/fonts.h"
#include "../../hacks/mc_hud/mc_font_ttf.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace
{
	std::string to_lower( std::string text )
	{
		std::transform( text.begin( ), text.end( ), text.begin( ),
		                []( const unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );

		return text;
	}

	void scan_directory( const std::string& directory, std::vector< font_file_t >& out )
	{
		std::error_code error_code{ };

		if ( !std::filesystem::exists( directory, error_code ) || error_code )
			return;

		for ( const std::filesystem::directory_entry& entry :
		      std::filesystem::recursive_directory_iterator( directory, std::filesystem::directory_options::skip_permission_denied, error_code ) ) {
			if ( !entry.is_regular_file( error_code ) || error_code )
				continue;

			if ( !entry.path( ).has_filename( ) )
				continue;

			const std::string extension = to_lower( entry.path( ).extension( ).string( ) );

			if ( extension != ".ttf" && extension != ".otf" && extension != ".ttc" )
				continue;

			const std::u8string stem = entry.path( ).stem( ).u8string( );
			const std::u8string path = entry.path( ).u8string( );

			out.push_back( { std::string( reinterpret_cast< const char* >( stem.c_str( ) ) ),
			                 std::string( reinterpret_cast< const char* >( path.c_str( ) ) ) } );
		}
	}

	/* file = stem in the imgui subtabs, family = registry value = name in the chud / def hud list (real ttf family, a
	   second weight gets its style appended so chud skips it). verdana / tahoma skipped (windows ships them), icon fonts
	   skipped (no text glyphs) */
	struct embedded_font_t {
		const char* m_file;
		const char* m_family;
		const unsigned char* m_data;
		std::size_t m_size;
	};

#define EMBEDDED_FONT( file, family, data ) { file, family, data, sizeof( data ) }
	constexpr embedded_font_t k_embedded_fonts[] = {
		EMBEDDED_FONT( "Minecraft", "Minecraft", n_mc_assets::k_minecraft_ttf ),
		EMBEDDED_FONT( "Montserrat Regular", "Montserrat", montserrat_regular ),
		EMBEDDED_FONT( "Montserrat Medium", "Montserrat Medium", dna_montserrat_medium ),
		EMBEDDED_FONT( "Montserrat Medium Italic", "Montserrat Medium Italic", montserrat_medium_italic ),
		EMBEDDED_FONT( "Inter Bold", "Inter", inter_bold ),
		EMBEDDED_FONT( "Inter Medium", "Inter Medium", inter_medium ),
		EMBEDDED_FONT( "Inter Semi Bold", "Inter Semi Bold", inter_semibold ),
		EMBEDDED_FONT( "Rubik Regular", "Rubik", rubik_regular ),
		EMBEDDED_FONT( "Rubik Medium", "Rubik Medium", rubik_medium ),
		EMBEDDED_FONT( "SF Pro Display Semibold", "SF Pro Display", sf_pro_display_semibold ),
		EMBEDDED_FONT( "SF Pro Display Bold", "SF Pro Display Bold", sf_pro_display_bold ),
		EMBEDDED_FONT( "SF UI Display Semibold", "SF UI Display", sf_ui_display_semibold ),
		EMBEDDED_FONT( "Droid Sans Bold", "Droid Sans", droid_sans_bold ),
		EMBEDDED_FONT( "Nunito Medium", "Nunito Medium", nunito_medium ),
		EMBEDDED_FONT( "PT Root UI Bold", "PT Root UI", pt_root_ui_bold ),
		EMBEDDED_FONT( "Sunflower Medium", "Sunflower", sunflower_medium ),
	};
#undef EMBEDDED_FONT

	/* per user windows font: imgui subtabs scan this folder, chud / def hud read HKCU + whitelist it in fonts.conf.
	   no admin, file rewritten only when changed, registry value left alone when the user already has that family */
	void install_font( const std::string& directory, const char* file, const char* family, const void* data, const std::size_t size )
	{
		const std::string path = directory + "\\" + file + ".ttf";

		std::error_code error_code{ };
		if ( std::filesystem::file_size( path, error_code ) != size || error_code ) {
			// in use by a running csgo = same file already, skipping is fine
			if ( std::ofstream out( path, std::ios::binary | std::ios::trunc ); out )
				out.write( static_cast< const char* >( data ), static_cast< std::streamsize >( size ) );
		}

		HKEY key = nullptr;
		if ( RegCreateKeyExA( HKEY_CURRENT_USER, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, nullptr, 0,
		                      KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr ) != ERROR_SUCCESS )
			return;

		const std::string value = std::string( family ) + " (TrueType)";
		if ( RegQueryValueExA( key, value.c_str( ), nullptr, nullptr, nullptr, nullptr ) != ERROR_SUCCESS )
			RegSetValueExA( key, value.c_str( ), 0, REG_SZ, reinterpret_cast< const BYTE* >( path.c_str( ) ), static_cast< DWORD >( path.size( ) + 1 ) );

		RegCloseKey( key );
	}

	void install_embedded_fonts( )
	{
		char directory[ MAX_PATH ]{ };
		if ( !LI_FN( ExpandEnvironmentStringsA )( "%LOCALAPPDATA%\\Microsoft\\Windows\\Fonts", directory, MAX_PATH ) )
			return;

		std::error_code error_code{ };
		std::filesystem::create_directories( directory, error_code );

		for ( const embedded_font_t& font : k_embedded_fonts )
			install_font( directory, font.m_file, font.m_family, font.m_data, font.m_size );

		// stb compressed: a throwaway atlas decompresses it into ConfigData, never built
		ImFontAtlas* const atlas = IM_NEW( ImFontAtlas )( );
		if ( atlas->AddFontFromMemoryCompressedTTF( smallest_pixel_compressed_data, smallest_pixel_compressed_size, 10.f ) ) {
			const ImFontConfig& config = atlas->ConfigData.back( );
			install_font( directory, "Smallest Pixel-7", "Smallest Pixel-7", config.FontData, static_cast< std::size_t >( config.FontDataSize ) );
		}
		IM_DELETE( atlas );
	}
}

void n_fonts::impl_t::on_attach( )
{
	this->m_font_files.clear( );
	this->m_font_file_names.clear( );

	install_embedded_fonts( );

	scan_directory( std::vformat( "{}\\Fonts", std::make_format_args( g_ctx.m_windows_directory ) ), this->m_font_files );

	char user_fonts_directory[ MAX_PATH ]{ };
	if ( LI_FN( ExpandEnvironmentStringsA )( "%LOCALAPPDATA%\\Microsoft\\Windows\\Fonts", user_fonts_directory, MAX_PATH ) )
		scan_directory( user_fonts_directory, this->m_font_files );

	std::sort( this->m_font_files.begin( ), this->m_font_files.end( ), []( const font_file_t& a, const font_file_t& b ) {
		return to_lower( a.m_display_name ) < to_lower( b.m_display_name );
	} );

	this->m_font_files.erase( std::unique( this->m_font_files.begin( ), this->m_font_files.end( ),
	                                       []( const font_file_t& a, const font_file_t& b ) {
											   return to_lower( a.m_display_name ) == to_lower( b.m_display_name );
										   } ),
	                          this->m_font_files.end( ) );

	this->m_font_file_names.reserve( this->m_font_files.size( ) );

	for ( const font_file_t& font_file : this->m_font_files )
		this->m_font_file_names.push_back( font_file.m_display_name );
}

std::string n_fonts::impl_t::find_path( const std::string& display_name )
{
	const std::string lower_name = to_lower( display_name );

	for ( const font_file_t& font_file : this->m_font_files )
		if ( to_lower( font_file.m_display_name ) == lower_name )
			return font_file.m_full_path;

	return std::vformat( "{}\\Fonts\\{}.ttf", std::make_format_args( g_ctx.m_windows_directory, display_name ) );
}
