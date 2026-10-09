#include "menu_internal.h"
#include "../../globals/fonts/fonts.h"
#include "../../globals/includes/includes.h"
#include "../../globals/logger/logger.h"
#include "../misc/scaleform/scaleform.h"
#include "../chams/chams.h"
#include "../chud_hud/chud_hud.h"
#include "../movement/movement.h"
#include "../skins/skins.h"
#include "../visuals/screen/color_curve.h"
#include "../visuals/screen/resolution_spoof.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <format>
#include <string>
#include <vector>

static const std::string& build_date_text( )
{
	static const std::string text = [ ]( ) -> std::string {
		static constexpr const char* months[ ] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };

		HMODULE module = nullptr;
		if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                         reinterpret_cast< LPCSTR >( &build_date_text ), &module ) ) {
			const auto dos = reinterpret_cast< const IMAGE_DOS_HEADER* >( module );
			if ( dos->e_magic == IMAGE_DOS_SIGNATURE ) {
				const auto nt = reinterpret_cast< const IMAGE_NT_HEADERS* >( reinterpret_cast< const std::uint8_t* >( module ) + dos->e_lfanew );
				std::tm local           = { };
				const std::time_t stamp = nt->FileHeader.TimeDateStamp;
				if ( nt->Signature == IMAGE_NT_SIGNATURE && localtime_s( &local, &stamp ) == 0 )
					return std::format( "{} {} {}", local.tm_mday, months[ local.tm_mon ], local.tm_year + 1900 );
			}
		}

		const std::string date = __DATE__;
		const int month        = static_cast< int >( std::string( "JanFebMarAprMayJunJulAugSepOctNovDec" ).find( date.substr( 0, 3 ) ) / 3 );
		return std::format( "{} {} {}", std::stoi( date.substr( 4, 2 ) ), months[ month ], date.substr( 7, 4 ) );
	}( );
	return text;
}

void modulation_row( const char* label, const std::uint32_t enable_variable, const std::uint32_t color_variable,
                            const std::uint32_t brightness_variable )
{
	ImGui::Checkbox( label, &GET_VARIABLE( enable_variable, bool ) );

	if ( !GET_VARIABLE( enable_variable, bool ) )
		return;

	char picker_id[ 64 ] = { }, slider_id[ 64 ] = { };
	std::snprintf( picker_id, sizeof( picker_id ), "##%s color picker", label );
	std::snprintf( slider_id, sizeof( slider_id ), "brightness##%s", label );

	ImGui::SameLine( );
	ImGui::ColorEdit4( picker_id, &GET_VARIABLE( color_variable, c_color ), color_picker_alpha_flags );

	ImGui::SliderFloat( slider_id, &GET_VARIABLE( brightness_variable, float ), 0.f, 400.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp );
}

void draw_color_correction_curves( )
{
	std::vector< float >* channels[ 4 ] = {
		&GET_VARIABLE( g_variables.m_color_correction_curve_points, std::vector< float > ),
		&GET_VARIABLE( g_variables.m_color_correction_curve_red_points, std::vector< float > ),
		&GET_VARIABLE( g_variables.m_color_correction_curve_green_points, std::vector< float > ),
		&GET_VARIABLE( g_variables.m_color_correction_curve_blue_points, std::vector< float > ),
	};

	n_color_curve::curve_t curves[ 4 ];
	for ( int channel = 0; channel < 4; channel++ )
		curves[ channel ] = n_color_curve::load( *channels[ channel ] );

	static constexpr ImU32 line_colors[ 4 ] = { IM_COL32( 220, 185, 85, 230 ), IM_COL32( 255, 80, 80, 230 ), IM_COL32( 80, 210, 80, 230 ),
		                                        IM_COL32( 80, 130, 255, 230 ) };
	static constexpr ImU32 dim_colors[ 4 ]  = { IM_COL32( 180, 150, 60, 80 ), IM_COL32( 200, 60, 60, 80 ), IM_COL32( 60, 170, 60, 80 ),
		                                        IM_COL32( 60, 100, 200, 80 ) };

	static const char* const labels[ 4 ] = { "master", "red", "green", "blue" };

	static int active_channel = 0;
	static int dragged_point  = -1;

	ImGui::Label( "curves" );

	const float row_width = ImGui::GetRowFrameWidth( );
	const float spacing   = ImGui::GetStyle( ).ItemSpacing.x;

	{
		const float button_width = ( row_width - spacing * 3.f ) / 4.f;

		for ( int channel = 0; channel < 4; channel++ ) {
			if ( channel > 0 )
				ImGui::SameLine( 0.f, spacing );

			const bool selected = active_channel == channel;
			if ( selected ) {
				ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( ImColor( line_colors[ channel ] ) ) );
				ImGui::PushStyleColor( ImGuiCol_Text, ImVec4( 0.f, 0.f, 0.f, 1.f ) );
			}

			ImGui::PushID( channel );
			if ( ImGui::Button( labels[ channel ], ImVec2( button_width, 15.f ) ) ) {
				active_channel = channel;
				dragged_point  = -1;
			}
			ImGui::PopID( );

			if ( selected )
				ImGui::PopStyleColor( 2 );
		}
	}

	const float graph_height = row_width * 0.65f;
	const ImVec2 origin      = ImVec2( ImGui::GetRowFrameLeft( ), ImGui::GetCursorScreenPos( ).y );

	n_color_curve::curve_t& active = curves[ active_channel ];

	const auto to_screen = [ & ]( const float x, const float y ) {
		return ImVec2( origin.x + x * row_width, origin.y + ( 1.f - y ) * graph_height );
	};

	ImGui::InvisibleButton( "##color correction curve", ImVec2( row_width, graph_height ) );

	const bool hovered = ImGui::IsItemHovered( );
	const ImVec2 mouse = ImGui::GetMousePos( );
	const float mouse_x = ( mouse.x - origin.x ) / row_width;
	const float mouse_y = 1.f - ( mouse.y - origin.y ) / graph_height;

	// one hit test drives grab, add and remove, so they never disagree on the point under the mouse
	int hit            = -1;
	float hit_distance = FLT_MAX;
	for ( int i = 0; i < active.m_count; i++ ) {
		const ImVec2 point   = to_screen( active.m_x[ i ], active.m_y[ i ] );
		const float distance = ImSqrt( ImLengthSqr( ImVec2( mouse.x - point.x, mouse.y - point.y ) ) );
		if ( distance < hit_distance ) {
			hit_distance = distance;
			hit          = i;
		}
	}

	const bool on_point = hovered && hit_distance <= 11.f;
	bool changed        = false;

	const bool active_item = ImGui::IsItemActive( );

	if ( active_item && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
		dragged_point = on_point ? hit : n_color_curve::add_point( active, mouse_x, mouse_y );

	if ( active_item && dragged_point >= 0 && dragged_point < active.m_count && ImGui::IsMouseDown( ImGuiMouseButton_Left ) ) {
		n_color_curve::move_point( active, dragged_point, mouse_x, mouse_y );
		changed = true;
	}

	if ( !active_item || !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
		dragged_point = -1;

	if ( on_point && ImGui::IsMouseClicked( ImGuiMouseButton_Right ) ) {
		n_color_curve::remove_point( active, hit );
		dragged_point = -1;
		hit           = -1;
		changed       = true;
	}

	if ( changed )
		n_color_curve::store( active, *channels[ active_channel ] );

	ImDrawList* draw_list = ImGui::GetWindowDrawList( );

	const ImVec2 bottom_right = ImVec2( origin.x + row_width, origin.y + graph_height );

	draw_list->AddRectFilled( origin, bottom_right, IM_COL32( 14, 14, 18, 220 ), 4.f );
	draw_list->AddRect( origin, bottom_right, IM_COL32( 65, 65, 75, 200 ), 4.f );

	for ( int i = 1; i < 4; i++ ) {
		const float x = origin.x + row_width * i * 0.25f;
		const float y = origin.y + graph_height * i * 0.25f;

		draw_list->AddLine( ImVec2( x, origin.y ), ImVec2( x, bottom_right.y ), IM_COL32( 45, 45, 55, 130 ) );
		draw_list->AddLine( ImVec2( origin.x, y ), ImVec2( bottom_right.x, y ), IM_COL32( 45, 45, 55, 130 ) );
	}

	draw_list->AddLine( ImVec2( origin.x, bottom_right.y ), ImVec2( bottom_right.x, origin.y ), IM_COL32( 55, 55, 70, 160 ) );

	static constexpr int segments = 96;
	ImVec2 line[ segments + 1 ];

	for ( int pass = 0; pass < 2; pass++ ) {
		for ( int channel = 0; channel < 4; channel++ ) {
			const bool selected = channel == active_channel;
			if ( selected != ( pass == 1 ) || ( !selected && n_color_curve::is_identity( curves[ channel ] ) ) )
				continue;

			for ( int i = 0; i <= segments; i++ ) {
				const float x = static_cast< float >( i ) / segments;
				line[ i ]     = to_screen( x, n_color_curve::evaluate( curves[ channel ], x ) );
			}

			draw_list->AddPolyline( line, segments + 1, selected ? line_colors[ channel ] : dim_colors[ channel ], ImDrawFlags_None,
			                        selected ? 1.5f : 1.f );
		}
	}

	for ( int i = 0; i < active.m_count; i++ ) {
		const ImVec2 point = to_screen( active.m_x[ i ], active.m_y[ i ] );
		const bool lit     = dragged_point == i || ( on_point && hit == i );

		draw_list->AddCircleFilled( point, 4.5f, lit ? IM_COL32( 255, 255, 255, 255 ) : line_colors[ active_channel ] );
		draw_list->AddCircle( point, 4.5f, IM_COL32( 255, 255, 255, 140 ) );
	}

	ImGui::Dummy( ImVec2( row_width, 2.f ) );

	if ( dragged_point >= 0 && dragged_point < active.m_count )
		ImGui::TextDisabled( "%s   in %3d   out %3d", labels[ active_channel ], static_cast< int >( active.m_x[ dragged_point ] * 255.f + 0.5f ),
		                     static_cast< int >( active.m_y[ dragged_point ] * 255.f + 0.5f ) );
	else if ( hovered )
		ImGui::TextDisabled( "left add / drag, right remove" );
	else
		ImGui::TextDisabled( "%s   %d points", labels[ active_channel ], active.m_count );

	if ( ImGui::Button( "reset channel##color correction curve", ImVec2( row_width, 15.f ) ) )
		channels[ active_channel ]->assign( n_color_curve::config_size, -1.f );

	if ( ImGui::Button( "reset curves##color correction curve", ImVec2( row_width, 15.f ) ) ) {
		for ( auto* channel : channels )
			channel->assign( n_color_curve::config_size, -1.f );
	}
}

void reset_color_correction( )
{
	GET_VARIABLE( g_variables.m_color_correction_exposure, float )         = 0.f;
	GET_VARIABLE( g_variables.m_color_correction_red, float )              = 1.f;
	GET_VARIABLE( g_variables.m_color_correction_green, float )            = 1.f;
	GET_VARIABLE( g_variables.m_color_correction_blue, float )             = 1.f;
	GET_VARIABLE( g_variables.m_color_correction_hue, float )              = 0.f;
	GET_VARIABLE( g_variables.m_color_correction_saturation, float )       = 1.f;
	GET_VARIABLE( g_variables.m_color_correction_contrast, float )         = 1.f;
	GET_VARIABLE( g_variables.m_color_correction_temperature, float )      = 0.f;
	GET_VARIABLE( g_variables.m_color_correction_levels_in_black, float )  = 0.f;
	GET_VARIABLE( g_variables.m_color_correction_levels_in_white, float )  = 1.f;
	GET_VARIABLE( g_variables.m_color_correction_levels_gamma, float )     = 1.f;
	GET_VARIABLE( g_variables.m_color_correction_levels_out_black, float ) = 0.f;
	GET_VARIABLE( g_variables.m_color_correction_levels_out_white, float ) = 1.f;
	GET_VARIABLE( g_variables.m_color_correction_highlights, float )       = 0.f;
	GET_VARIABLE( g_variables.m_color_correction_shadows, float )          = 0.f;

	GET_VARIABLE( g_variables.m_color_correction_curve_points, std::vector< float > ).assign( n_color_curve::config_size, -1.f );
	GET_VARIABLE( g_variables.m_color_correction_curve_red_points, std::vector< float > ).assign( n_color_curve::config_size, -1.f );
	GET_VARIABLE( g_variables.m_color_correction_curve_green_points, std::vector< float > ).assign( n_color_curve::config_size, -1.f );
	GET_VARIABLE( g_variables.m_color_correction_curve_blue_points, std::vector< float > ).assign( n_color_curve::config_size, -1.f );
}

void chams_dropdown( const char* label, const std::uint32_t layers_variable, const std::uint32_t colors_variable )
{
	auto& slots  = GET_VARIABLE( layers_variable, std::vector< int > );
	auto& colors = GET_VARIABLE( colors_variable, std::vector< c_color > );

	const int max_slots = static_cast< int >( slots.size( ) < colors.size( ) ? slots.size( ) : colors.size( ) );

	int used = 0;
	for ( int i = 0; i < max_slots; i++ ) {
		if ( slots[ i ] >= 0 && slots[ i ] < n_chams::chams_material_max )
			used = i + 1;
	}

	/* stack buffer: rebuilt every frame, never longer than all names once */
	char preview[ 192 ] = "none";
	int written         = 0;

	for ( int i = 0; i < used; i++ ) {
		const int printed =
			std::snprintf( preview + written, sizeof( preview ) - written, written ? ", %s" : "%s", n_chams::material_names[ slots[ i ] ] );

		if ( printed <= 0 )
			break;

		written += printed;

		if ( written >= static_cast< int >( sizeof( preview ) ) - 1 )
			break;
	}

	if ( !ImGui::BeginCombo( label, preview ) )
		return;

	for ( int material = 0; material < n_chams::chams_material_max; material++ ) {
		int slot = -1;
		for ( int i = 0; i < used; i++ ) {
			if ( slots[ i ] == material ) {
				slot = i;
				break;
			}
		}

		// same ##id either way: toggling must not change the item id
		char text[ 64 ] = { };
		if ( slot >= 0 )
			std::snprintf( text, sizeof( text ), "%i. %s##chams%i", slot + 1, n_chams::material_names[ material ], material );
		else
			std::snprintf( text, sizeof( text ), "%s##chams%i", n_chams::material_names[ material ], material );

		const bool toggled =
		    ImGui::Selectable( text, slot >= 0, ImGuiSelectableFlags_DontClosePopups | ImGuiSelectableFlags_AllowItemOverlap );

		ImGui::SetItemAllowOverlap( );

		if ( slot >= 0 ) {
			const ImVec2 next_position = ImGui::GetCursorPos( );

			ImGui::SameLine( );

			char color_id[ 96 ] = { };
			std::snprintf( color_id, sizeof( color_id ), "##chams color%s%s", label, n_chams::material_names[ material ] );

			ImGui::ColorEdit4( color_id, &colors[ slot ], color_picker_alpha_flags );

			ImGui::SetCursorPos( next_position );
		}

		if ( toggled ) {
			if ( slot >= 0 ) {
				for ( int i = slot; i < max_slots - 1; i++ ) {
					slots[ i ]  = slots[ i + 1 ];
					colors[ i ] = colors[ i + 1 ];
				}

				slots[ max_slots - 1 ] = -1;
			} else if ( used < max_slots ) {
				slots[ used ]  = material;
				colors[ used ] = c_color( 255, 255, 255, 255 );
			}

			ImGui::EndCombo( );
			return;
		}
	}

	ImGui::EndCombo( );
}

void save_popup( const char* str_id, bool& open, const ImVec2& window_size, const std::function< void( ) >& fn )
{
	if ( !open )
		return;

	ImGuiContext& g         = *GImGui;
	const ImGuiStyle& style = g.Style;

	const auto window = g.CurrentWindow;

	const auto hashed_str_id = ImHashStr( str_id );
	const auto text_size     = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ]->CalcTextSizeA(
        g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ]->FontSize, FLT_MAX, 0.f, str_id );

	const ImColor accent_color = ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent );

	// open once per request; click away closes it -> drop request, no reopen at mouse
	const ImGuiID shown_id = ImGui::GetID( str_id );
	ImGuiStorage* storage  = ImGui::GetStateStorage( );
	if ( !storage->GetBool( shown_id ) ) {
		ImGui::OpenPopup( str_id );
		storage->SetBool( shown_id, true );
	}

	ImGui::SetNextWindowSize( window_size );

	if ( !ImGui::BeginPopup( str_id ) ) {
		open = false;
		storage->SetBool( shown_id, false );
		return;
	}

	{
		const auto draw_list = ImGui::GetWindowDrawList( );

		const ImVec2 position = ImGui::GetWindowPos( ), size = ImGui::GetWindowSize( );

		auto text_animation = ImAnimationHelper( hashed_str_id, ImGui::GetIO( ).DeltaTime );

		ImGui::PushClipRect( ImVec2( position ), ImVec2( position.x + size.x, position.y + 20.f ), false );

		draw_list->AddRectFilled( ImVec2( position ), ImVec2( position.x + size.x, position.y + 20.f ), ImColor( 25 / 255.f, 25 / 255.f, 25 / 255.f ),
		                          g.Style.WindowRounding - 2.f, ImDrawFlags_RoundCornersTop );

		ImGui::PopClipRect( );

		ImGui::PushClipRect( position, ImVec2( position.x + size.x, position.y + size.y ), false );
		draw_list->AddRect( position, ImVec2( position.x + size.x, position.y + size.y ), ImColor( 50, 50, 50, 100 ), g.Style.WindowRounding - 2.f );

		text_animation.Update( ImGui::IsMouseHoveringRect( position, ImVec2( position.x + size.x, position.y + size.y ) ) ? 3.f : -2.f, 1.f, 0.5f,
		                       1.f );

		ImGui::PopClipRect( );

		const ImColor text_color = ImGui::GetColorU32( ImGuiCol_Text );

		draw_list->AddText(
			g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ], g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ]->FontSize,
			ImVec2( position.x + ( size.x - text_size.x ) / 2.f, position.y + ( 20.f - text_size.y ) / 2.f ),
			ImColor( text_color.Value.x, text_color.Value.y, text_color.Value.z, text_color.Value.w * text_animation.AnimationData->second ),
			str_id );

		RenderFadedGradientLine( draw_list, ImVec2( position.x, position.y + 20.f ), ImVec2( size.x, 1.f ),
		                         ImColor( accent_color.Value.x, accent_color.Value.y, accent_color.Value.z ) );

		ImGui::SetCursorPosY( ImGui::GetCursorPosY( ) + 20.f );

		fn( );

		if ( !open ) {
			ImGui::CloseCurrentPopup( );
			storage->SetBool( shown_id, false );
		}

		ImGui::EndPopup( );
	}
}

void menu_columns_begin( )
{
	const float width = ImFloor( ( ImGui::GetContentRegionAvail( ).x - ImGui::GetStyle( ).ItemSpacing.x ) / 2.f );
	ImGui::BeginChild( "left column", ImVec2( width, ImGui::GetContentRegionAvail( ).y - menu_bottom_band_height ), false, 0, false );
}

void menu_columns_next( )
{
	ImGui::EndChild( );
	ImGui::SameLine( );
	ImGui::BeginChild( "right column", ImVec2( 0.f, ImGui::GetContentRegionAvail( ).y - menu_bottom_band_height ), false, 0, false );
}

void menu_columns_end( )
{
	ImGui::EndChild( );
}

bool menu_group_begin( const char* name )
{
	const float height = ImGui::GetStateStorage( )->GetFloat( ImGui::GetID( name ), 40.f );
	return ImGui::BeginChild( name, ImVec2( 0.f, height ), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse, true );
}

void menu_group_end( )
{
	ImGuiWindow* const group = ImGui::GetCurrentWindow( );

	/* clipped group skips items, its cursor never moves: keep the old height */
	if ( !group->SkipItems )
		group->ParentWindow->StateStorage.SetFloat(
			group->ChildId, ImMax( group->DC.CursorMaxPos.y - group->DC.CursorStartPos.y + group->WindowPadding.y * 2.f, 1.f ) );

	ImGui::EndChild( );
}

constexpr float menu_anim_duration = 0.12f;

static bool menu_owns_window( ImGuiWindow* window, ImGuiWindow* root )
{
	for ( int depth = 0; window && depth < 32; ++depth, window = window->ParentWindow ) {
		if ( window == root )
			return true;
	}

	return false;
}

float n_menu::impl_t::fade_draw_lists( ImVector< ImDrawList* >& out )
{
	out.resize( 0 );

	if ( this->m_anim_progress <= 0.f || this->m_anim_progress >= 0.999f )
		return 1.f;

	ImGuiWindow* root = ImGui::FindWindowByName( ( "botox-ui" ) );

	if ( !root || !root->Active || root->Hidden )
		return 1.f;

	ImGuiWindow* web = ImGui::FindWindowByName( "Web##websurf" );
	ImGuiWindow* route_calc = ImGui::FindWindowByName( "route calculator##pinned" );
	ImGuiWindow* player_list = ImGui::FindWindowByName( "player list##window" );

	ImGuiContext& g = *GImGui;

	for ( ImGuiWindow* window : g.Windows ) {
		if ( !window->Active || window->Hidden ||
		     !( menu_owns_window( window, root ) || ( web && menu_owns_window( window, web ) ) ||
		        ( route_calc && menu_owns_window( window, route_calc ) ) || ( player_list && menu_owns_window( window, player_list ) ) ) )
			continue;

		ImDrawList* list = window->DrawList;

		if ( list && !out.contains( list ) )
			out.push_back( list );
	}

	return ImSaturate( this->m_anim_progress );
}

void n_menu::impl_t::on_end_scene( )
{
	ImGui::GetStyle( ).Colors[ ImGuiCol_::ImGuiCol_Accent ] = GET_VARIABLE( g_variables.m_accent, c_color ).get_vec4( );

	image_pick_apply( );

	/* eject snaps shut: init thread releases the module at once, never under a menu frame */
	if ( g_ctx.m_eject_requested )
		this->m_anim_progress = 0.f;
	else {
		/* linear in real time; hitch cap keeps one long frame from skipping the fade */
		const float step = ImMin( ImGui::GetIO( ).DeltaTime, 1.f / 30.f ) / menu_anim_duration;

		this->m_anim_progress = ImClamp( this->m_anim_progress + ( this->m_opened ? step : -step ), 0.f, 1.f );
	}

	if ( !this->m_opened && this->m_anim_progress <= 0.f )
		return;

	constexpr auto background_height = menu_band_height;
	constexpr float bottom_band_height = menu_bottom_band_height;

	ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
	                                ImGuiWindowFlags_::ImGuiWindowFlags_NoResize;

	if ( this->m_anim_progress < 0.999f )
		window_flags |= ImGuiWindowFlags_::ImGuiWindowFlags_NoInputs;

	ImGui::SetNextWindowSize( ImVec2( 750.f, 640.f - ( background_height - bottom_band_height ) ), ImGuiCond_::ImGuiCond_Always );

	ImGui::Begin( ( "botox-ui" ), 0, window_flags );
	{
		const auto window = ImGui::GetCurrentWindow( );

		const auto draw_list = window->DrawList;

		const auto size     = window->Size;
		const auto position = window->Pos;

		[ & ]( ) {
			ImGui::PushClipRect( ImVec2( position.x, position.y ), ImVec2( position.x + size.x, position.y + background_height ), false );
			draw_list->AddRectFilled( ImVec2( position.x, position.y ), ImVec2( position.x + size.x, position.y + background_height ),
			                          ImColor( 25 / 255.f, 25 / 255.f, 25 / 255.f, 1.f ), ImGui::GetStyle( ).WindowRounding,
			                          ImDrawFlags_RoundCornersTop );
			ImGui::PopClipRect( );

			ImGui::PushClipRect( ImVec2( position.x, position.y ), ImVec2( position.x + size.x, position.y + size.y ), false );
			draw_list->AddRect( ImVec2( position.x, position.y ), ImVec2( position.x + size.x, position.y + size.y ),
			                    ImColor( 50, 50, 50, 255 ), ImGui::GetStyle( ).WindowRounding );
			ImGui::PopClipRect( );

			RenderFadedGradientLine( draw_list, ImVec2( position.x, position.y + background_height - 1.f ), ImVec2( size.x, 1.f ),
			                         ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent ) );

			ImFont* const title_font = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
			ImFont* const date_font  = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];

			const auto title_text      = ( "botox" );
			const std::string date_text = " | " + build_date_text( );
			const auto title_text_size = title_font->CalcTextSizeA( title_font->FontSize, FLT_MAX, 0.f, title_text );
			const auto date_text_size  = date_font->CalcTextSizeA( date_font->FontSize, FLT_MAX, 0.f, date_text.c_str( ) );

			const float title_x = position.x + ( ( size.x - title_text_size.x - date_text_size.x ) / 2.f );
			const float title_y = position.y + ( ( background_height - title_text_size.y ) / 2.f );

			const float date_y = title_y;

			draw_list->AddText( title_font, title_font->FontSize, ImVec2( title_x, title_y ), ImColor( 1.f, 1.f, 1.f ), title_text );
			draw_list->AddText( date_font, date_font->FontSize, ImVec2( title_x + title_text_size.x, date_y ),
			                    ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent ), date_text.c_str( ) );
		}( );

		ImGui::PushClipRect( ImVec2( position.x, position.y + size.y - bottom_band_height ), ImVec2( position.x + size.x, position.y + size.y ),
		                     false );
		draw_list->AddRectFilled( ImVec2( position.x, position.y + size.y - bottom_band_height ), ImVec2( position.x + size.x, position.y + size.y ),
		                          ImColor( 25 / 255.f, 25 / 255.f, 25 / 255.f ), ImGui::GetStyle( ).WindowRounding, ImDrawFlags_RoundCornersBottom );
		ImGui::PopClipRect( );

		RenderFadedGradientLine( draw_list, ImVec2( position.x, position.y + size.y - bottom_band_height ), ImVec2( size.x, 1.f ),
		                         ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent ) );

		constexpr float tab_width   = 100.f;
		constexpr float tab_height  = 24.f;
		constexpr float tab_gap     = 6.f;
		constexpr float page_height = 17.f;

		/* page order = the case order of the tab_* switch; one page = no subtabs */
		struct menu_tab_t {
			const char* m_name;
			std::vector< const char* > m_pages;
		};

		static const menu_tab_t tabs[ ] = {
			{ "aimbot", { } }, { "visuals", { "esp", "world", "screen" } }, { "movement", { "main", "indicators", "calculators", "recorder" } },
			{ "misc", { } },   { "inventory", { "weapons", "player" } }, { "fonts", { "indicators", "esp", "chud hud", "default hud" } },
			{ "settings", { } },
		};

		constexpr int tab_count = static_cast< int >( IM_ARRAYSIZE( tabs ) );

		static int last_page[ tab_count ] = { };

		this->m_tab    = ImClamp( this->m_tab, 0, tab_count - 1 );
		this->m_subtab = ImClamp( this->m_subtab, 0, ImMax( static_cast< int >( tabs[ this->m_tab ].m_pages.size( ) ) - 1, 0 ) );

		const float tab_x = window->DC.CursorPos.x;
		float tab_y       = window->DC.CursorPos.y + background_height;

		ImFont* const tab_font  = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
		ImFont* const page_font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
		const ImU32 accent      = ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent );
		const ImColor accent_color( accent );
		const bool can_click    = ImGui::IsWindowHovered( );
		const float rounding    = ImGui::GetStyle( ).WindowRounding - 2.f;

		const auto tab_button = [ & ]( const char* name, const bool selected ) {
			const ImVec2 tab_min( tab_x, tab_y );
			const ImVec2 tab_max( tab_x + tab_width, tab_y + tab_height );

			const bool hovered = ImGui::IsMouseHoveringRect( tab_min, tab_max );

			const auto hashed_tab_name = ImHashStr( name );

			auto hovered_text_animation = ImAnimationHelper( hashed_tab_name + ImHashStr( "hovered-text-animation" ), ImGui::GetIO( ).DeltaTime );
			hovered_text_animation.Update( 2.f, hovered ? 2.f : -2.f, 0.5f );

			auto selected_animation = ImAnimationHelper( hashed_tab_name + ImHashStr( "selected-animation" ), ImGui::GetIO( ).DeltaTime );
			selected_animation.Update( 2.f, selected ? 2.f : -2.f );

			const float lit = selected_animation.AnimationData->second;

			draw_list->AddRectFilled( tab_min, tab_max, ImColor( 25 / 255.f, 25 / 255.f, 25 / 255.f ), rounding, ImDrawFlags_RoundCornersTop );
			draw_list->AddRect( tab_min, tab_max, ImColor( 50, 50, 50, 100 ), rounding, ImDrawFlags_RoundCornersTop );

			RenderFadedGradientLine( draw_list, ImVec2( tab_min.x, tab_max.y - 1.f ), ImVec2( tab_width, 1.f ),
			                         ImColor( accent_color.Value.x, accent_color.Value.y, accent_color.Value.z, lit ) );

			const auto text_size = tab_font->CalcTextSizeA( tab_font->FontSize, FLT_MAX, 0.f, name );

			draw_list->AddText( tab_font, tab_font->FontSize,
			                    ImVec2( ImFloor( tab_min.x + ( tab_width - text_size.x ) / 2.f ), ImFloor( tab_min.y + ( tab_height - text_size.y ) / 2.f ) ),
			                    ImColor::Blend( ImColor( 1.f, 1.f, 1.f, hovered_text_animation.AnimationData->second ), accent, lit ), name );

			tab_y = tab_max.y + tab_gap;

			return can_click && hovered && ImGui::IsMouseClicked( ImGuiMouseButton_Left );
		};

		for ( int tab = 0; tab < tab_count; tab++ ) {
			const menu_tab_t& entry = tabs[ tab ];

			if ( tab_button( entry.m_name, tab == this->m_tab ) ) {
				this->m_tab    = tab;
				this->m_subtab = last_page[ tab ];
			}

			if ( entry.m_pages.empty( ) )
				continue;

			/* keybind default style: slot + alpha = open amount, tabs below slide */
			auto open_animation = ImAnimationHelper( ImHashStr( entry.m_name ) + ImHashStr( "pages-open-animation" ), ImGui::GetIO( ).DeltaTime );
			const float open    = open_animation.Update( 3.f, tab == this->m_tab ? 3.f : -3.f );
			if ( open <= 0.f )
				continue;

			const int page_count = static_cast< int >( entry.m_pages.size( ) );
			const float clip_top = tab_y - tab_gap;
			const float clip_bot = tab_y + ( page_count * ( page_height + 1.f ) + tab_gap ) * open;
			draw_list->PushClipRect( ImVec2( tab_x, clip_top ), ImVec2( tab_x + tab_width, clip_bot ), true );

			for ( int page = 0; page < page_count; page++ ) {
				const char* const page_name = entry.m_pages[ page ];

				const ImVec2 row_min( tab_x, tab_y - tab_gap + 1.f );
				const ImVec2 row_max( tab_x + tab_width, row_min.y + page_height );

				const bool hovered  = open >= 1.f && tab == this->m_tab && ImGui::IsMouseHoveringRect( row_min, row_max );
				const bool selected = page == ( tab == this->m_tab ? this->m_subtab : last_page[ tab ] );

				const auto hashed_page = ImHashStr( entry.m_name ) + ImHashStr( page_name );

				auto page_hover_animation = ImAnimationHelper( hashed_page + ImHashStr( "hovered-text-animation" ), ImGui::GetIO( ).DeltaTime );
				page_hover_animation.Update( 2.f, hovered ? 2.f : -2.f, 0.5f );

				auto page_selected_animation = ImAnimationHelper( hashed_page + ImHashStr( "selected-animation" ), ImGui::GetIO( ).DeltaTime );
				page_selected_animation.Update( 2.f, selected ? 2.f : -2.f );

				const auto page_text_size = page_font->CalcTextSizeA( page_font->FontSize, FLT_MAX, 0.f, page_name );

				ImColor page_color = ImColor::Blend( ImColor( 1.f, 1.f, 1.f, page_hover_animation.AnimationData->second ), accent,
				                                     page_selected_animation.AnimationData->second );
				page_color.Value.w *= open;

				draw_list->AddText( page_font, page_font->FontSize,
				                    ImVec2( ImFloor( row_min.x + ( tab_width - page_text_size.x ) / 2.f ),
				                            ImFloor( row_min.y + ( page_height - page_text_size.y ) / 2.f ) ),
				                    page_color, page_name );

				if ( can_click && hovered && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) ) {
					this->m_subtab   = page;
					last_page[ tab ] = page;
				}

				tab_y += ( page_height + 1.f ) * open;
			}

			draw_list->PopClipRect( );

			tab_y += tab_gap * open;
		}

		/* eject sits on the sidebar floor, level with the bottom of the columns */
		tab_y = position.y + size.y - ImGui::GetStyle( ).WindowPadding.y - bottom_band_height - tab_height;

		/* init thread waits on m_eject_requested and does the release; never tear down here (d3d thread) */
		static bool open_eject_popup = false;
		if ( tab_button( "eject", false ) )
			open_eject_popup = true;

		if ( open_eject_popup ) {
			save_popup( "eject confirmation", open_eject_popup, ImVec2( 220.f, -1.f ), []( ) {
				if ( ImGui::Button( "yes", ImVec2( ( ImGui::GetRowFrameWidth( ) - 14.f ) * 0.5f, 15.f ) ) ) {
					open_eject_popup = false;

					// close menu first: release must not run under an open frame
					g_menu.m_opened         = false;
					g_ctx.m_eject_requested = true;
				}

				ImGui::SameLine( );

				if ( ImGui::Button( "no", ImVec2( -1.f, 15.f ) ) )
					open_eject_popup = false;
			} );
		}

		const float content_indent = tab_width + ImGui::GetStyle( ).ItemSpacing.x;
		ImGui::Indent( content_indent );

		ImGui::SetCursorPosY( ImGui::GetCursorPosY( ) + 25.f );

		/* own id per page: each page keeps its own column scroll + group heights */
		ImGui::PushID( this->m_tab * 16 + this->m_subtab );

		switch ( this->m_tab ) {
		case 0: this->tab_aimbot( ); break;
		case 1: this->tab_visuals( ); break;
		case 2: this->tab_movement( ); break;
		case 3: this->tab_misc( ); break;
		case 4: this->tab_inventory( ); break;
		case 5: this->tab_fonts( ); break;
		case 6: this->tab_settings( ); break;
		}

		ImGui::PopID( );

		ImGui::Unindent( content_indent );
	}
	ImGui::End( );

	this->route_calc_pinned( ( window_flags & ImGuiWindowFlags_::ImGuiWindowFlags_NoInputs ) != 0 );
	this->player_list_window( ( window_flags & ImGuiWindowFlags_::ImGuiWindowFlags_NoInputs ) != 0 );
}

void n_menu::impl_t::tab_aimbot( )
{
	struct weapon_config_t {
		const char* m_name;
		std::uint32_t m_override, m_fov, m_smooth, m_rcs, m_hitboxes;
	};

	static const weapon_config_t weapon_configs[ aim_config_max ] = {
		{ "general", 0, g_variables.m_aimbot_general_fov, g_variables.m_aimbot_general_smooth, g_variables.m_aimbot_general_rcs,
		  g_variables.m_aimbot_hitboxes },
		{ "pistol", g_variables.m_aimbot_pistol_override, g_variables.m_aimbot_pistol_fov, g_variables.m_aimbot_pistol_smooth,
		  g_variables.m_aimbot_pistol_rcs, g_variables.m_aimbot_pistol_hitboxes },
		{ "heavy pistol", g_variables.m_aimbot_heavy_pistol_override, g_variables.m_aimbot_heavy_pistol_fov,
		  g_variables.m_aimbot_heavy_pistol_smooth, g_variables.m_aimbot_heavy_pistol_rcs, g_variables.m_aimbot_heavy_pistol_hitboxes },
		{ "rifle", g_variables.m_aimbot_rifle_override, g_variables.m_aimbot_rifle_fov, g_variables.m_aimbot_rifle_smooth,
		  g_variables.m_aimbot_rifle_rcs, g_variables.m_aimbot_rifle_hitboxes },
		{ "smg", g_variables.m_aimbot_smg_override, g_variables.m_aimbot_smg_fov, g_variables.m_aimbot_smg_smooth, g_variables.m_aimbot_smg_rcs,
		  g_variables.m_aimbot_smg_hitboxes },
		{ "sniper", g_variables.m_aimbot_sniper_override, g_variables.m_aimbot_sniper_fov, g_variables.m_aimbot_sniper_smooth,
		  g_variables.m_aimbot_sniper_rcs, g_variables.m_aimbot_sniper_hitboxes },
		{ "scout", g_variables.m_aimbot_scout_override, g_variables.m_aimbot_scout_fov, g_variables.m_aimbot_scout_smooth,
		  g_variables.m_aimbot_scout_rcs, g_variables.m_aimbot_scout_hitboxes },
		{ "heavy", g_variables.m_aimbot_heavy_override, g_variables.m_aimbot_heavy_fov, g_variables.m_aimbot_heavy_smooth,
		  g_variables.m_aimbot_heavy_rcs, g_variables.m_aimbot_heavy_hitboxes },
	};

	menu_columns_begin( );

	if ( menu_group_begin( "main" ) ) {
		ImGui::Checkbox( "aimbot master switch", &GET_VARIABLE( g_variables.m_aimbot_enable, bool ) );

		ImGui::Checkbox( "aim on key", &GET_VARIABLE( g_variables.m_aimbot_on_key, bool ) );
		ImGui::Keybind( "aim key", &GET_VARIABLE( g_variables.m_aimbot_key, key_bind_t ) );

		ImGui::Checkbox( "silent aim", &GET_VARIABLE( g_variables.m_aimbot_silent, bool ) );
		ImGui::SliderFloat( "silent aim step", &GET_VARIABLE( g_variables.m_aimbot_silent_step, float ), 0.f, 90.f, "%.0f deg" );

		ImGui::Checkbox( "target team", &GET_VARIABLE( g_variables.m_aimbot_target_team, bool ) );
		ImGui::Checkbox( "scale fov with scope", &GET_VARIABLE( g_variables.m_aimbot_scope_fov_scale, bool ) );

		auto& selected = GET_VARIABLE( g_variables.m_aimbot_weapon_settings, int );
		ImGui::Combo( "weapon settings", &selected, "general\0pistols\0heavy pistols\0rifles\0smg\0sniper\0scout\0heavy\0" );

		if ( selected >= 0 && selected < aim_config_max ) {
			const weapon_config_t& config = weapon_configs[ selected ];

			if ( selected != aim_config_general )
				ImGui::Checkbox( std::format( "override general##{}", config.m_name ).c_str( ), &GET_VARIABLE( config.m_override, bool ) );

			ImGui::SliderFloat( std::format( "{} fov", config.m_name ).c_str( ), &GET_VARIABLE( config.m_fov, float ), 0.f, 180.f, "%.0f" );
			ImGui::SliderFloat( std::format( "{} smoothness", config.m_name ).c_str( ), &GET_VARIABLE( config.m_smooth, float ), 1.f, 30.f, "%.0f" );
			ImGui::SliderFloat( std::format( "{} rcs", config.m_name ).c_str( ), &GET_VARIABLE( config.m_rcs, float ), 0.f, 100.f, "%.0f" );

			const std::vector< const char* > hitbox_labels = { "head", "neck", "chest", "stomach", "pelvis", "arms", "legs" };
			auto& hitboxes                                 = GET_VARIABLE( config.m_hitboxes, std::vector< bool > );
			ImGui::MultiCombo( std::format( "hitboxes##{}", config.m_name ).c_str( ), hitboxes, hitbox_labels, hitboxes.size( ) );
		}
	}
	menu_group_end( );

	menu_columns_next( );

	if ( menu_group_begin( "configuration" ) ) {
		ImGui::SliderInt( "rcs start bullet", &GET_VARIABLE( g_variables.m_aimbot_rcs_start, int ), 1, 10 );
		ImGui::SliderFloat( "rcs pitch", &GET_VARIABLE( g_variables.m_aimbot_rcs_pitch, float ), 0.f, 100.f, "%.0f%%" );
		ImGui::SliderFloat( "rcs yaw", &GET_VARIABLE( g_variables.m_aimbot_rcs_yaw, float ), 0.f, 100.f, "%.0f%%" );
		ImGui::Checkbox( "rcs only with aimbot", &GET_VARIABLE( g_variables.m_aimbot_rcs_only_with_aimbot, bool ) );

		ImGui::Checkbox( "autowall", &GET_VARIABLE( g_variables.m_aimbot_autowall, bool ) );
		if ( GET_VARIABLE( g_variables.m_aimbot_autowall, bool ) ) {
			ImGui::OptionPopup(
				"autowall settings", [ & ]( ) { ImGui::SliderInt( "min damage", &GET_VARIABLE( g_variables.m_aimbot_min_damage, int ), 1, 100 ); },
				ImVec2( 200.f, -1 ) );
		}

		ImGui::Checkbox( "backtrack", &GET_VARIABLE( g_variables.m_backtrack_enable, bool ) );
		if ( GET_VARIABLE( g_variables.m_backtrack_enable, bool ) ) {
			ImGui::OptionPopup(
				"backtrack settings",
				[ & ]( ) {
					ImGui::Checkbox( "aim at backtrack", &GET_VARIABLE( g_variables.m_aimbot_aim_at_backtrack, bool ) );
					ImGui::SliderInt( "backtrack time", &GET_VARIABLE( g_variables.m_backtrack_time_limit, int ), 0, 200, "%d ms",
				                      ImGuiSliderFlags_AlwaysClamp );
					ImGui::Checkbox( "extended", &GET_VARIABLE( g_variables.m_backtrack_extend, bool ) );
					if ( GET_VARIABLE( g_variables.m_backtrack_extend, bool ) ) {
						ImGui::SliderInt( "extend amount", &GET_VARIABLE( g_variables.m_backtrack_extend_amount, int ), 0, 200, "%d ms",
					                      ImGuiSliderFlags_AlwaysClamp );
					}
				},
				ImVec2( 220.f, -1 ) );
		}

		/* weapon_accuracy_nospread, held only across its shot (render never sees it). hosted
		   servers only; sm_nospread = remote path, needs the sourcemod plugin */
		ImGui::Checkbox( "nospread", &GET_VARIABLE( g_variables.m_nospread_enable, bool ) );
		if ( GET_VARIABLE( g_variables.m_nospread_enable, bool ) ) {
			ImGui::OptionPopup(
				"nospread settings", [ & ]( ) { ImGui::Checkbox( "sm_nospread", &GET_VARIABLE( g_variables.m_nospread_sourcemod, bool ) ); },
				ImVec2( 200.f, -1 ) );
		}

		ImGui::Checkbox( "zeus bug", &GET_VARIABLE( g_variables.m_zeusbug, bool ) );
		ImGui::Keybind( "zeus bug key", &GET_VARIABLE( g_variables.m_zeusbug_key, key_bind_t ) );
		ImGui::Checkbox( "zeus bug swap back", &GET_VARIABLE( g_variables.m_zeusbug_swapback, bool ) );
	}
	menu_group_end( );

	menu_columns_end( );
}
