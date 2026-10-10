#include "misc.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <iterator>
#include <mutex>
#include <span>
#include <vector>
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/logger/logger.h"
#include "../../globals/media_player/audio_bands.h"
#include "../../hooks/functions/flash_probe.h"
#include "../../utilities/perf/perf_watch.h"
#include "../avatar_cache/avatar_cache.h"
#include "../entity_cache/entity_cache.h"
#include "../lagcomp/lagcomp.h"
#include "../menu/menu.h"
#include "../movement/movement.h"

// never open a second logger on this file
extern void botox_dbg_log( const char* fmt, ... );

void n_misc::impl_t::on_create_move_pre( )
{
	this->disable_post_processing( );

	this->remove_panorama_blur( );

	this->practice_window_think( );

	this->force_maxunlag( );

	fake_pov_create_move( );

}

void n_misc::impl_t::on_paint_traverse( )
{
	this->update_clantag( );

	/* not in on_create_move_pre (needs a live local): must re-check while dead / on load screens */
	this->force_host_lagcomp( );

	/* scoreboard number only; must stop the instant the net channel isn't loopback */
	this->fix_offline_ping( );

	/* a stale sv_lagpushticks after death / disconnect would shift the next server's lag comp */
	this->force_lagpush( );

	this->draw_spectating_local( );

	this->force_crosshair( );

	this->performance( );

	{
		PERF_ZONE( zone_paint_bots );
		bot_names_frame( );
	}

	if ( g_ctx.m_world_restore_requested.load( std::memory_order_acquire ) )
		return;

	PERF_ZONE( zone_paint_world );

	this->old_shaders( );

	// main thread, keeps running while dead / spectating
	this->world_modulation( );

	this->world_texture( );
}

void n_misc::impl_t::on_frame_stage_notify( int stage )
{
	if ( stage != render_start )
		return;

	// per frame, main thread: same thread as the demo recorder's GetLocalViewAngles read
	fake_pov_frame( );

	this->remove_smoke( );
	this->remove_flash( );
	this->flash_debug( );
}

void n_misc::impl_t::flash_debug( )
{
	if ( !g_ctx.m_local || !g_interfaces.m_global_vars_base )
		return;

	const float duration = g_ctx.m_local->get_flash_duration( );

	static float last  = 0.f;
	static bool logging = false;

	if ( duration <= 0.f ) {
		if ( logging )
			g_console.print( "flash: over" );

		logging = false;

		g_flash_probe.m_flash_fades = 0;
		g_flash_probe.m_other_fades = 0;

		g_flash_probe.m_blind = false;
		g_flash_probe.m_fresh = false;

		for ( int point = 0; point < 5; point++ )
			for ( int i = 0; i < 3; i++ )
				g_flash_probe.m_pre[ point ][ i ] = g_flash_probe.m_post[ point ][ i ] = g_flash_probe.m_final[ point ][ i ] = -1;

		return;
	}

	g_flash_probe.m_blind = true;

	const float now = g_interfaces.m_global_vars_base->m_real_time;

	/* sampler (d3d thread, costs a frame copy) paces the print so pixels + fade counters share a window.
	   first line prints at once; a skipped sample still prints stale after 1s */
	if ( logging && !g_flash_probe.m_fresh && now - last < 1.f )
		return;

	logging               = true;
	last                  = now;
	g_flash_probe.m_fresh = false;

	const float max_alpha = g_ctx.m_local->get_flash_max_alpha( );
	const int ao          = GET_VARIABLE( g_variables.m_ambient_occlusion, bool ) ? 1 : 0;
	const int dof         = GET_VARIABLE( g_variables.m_depth_of_field, bool ) ? 1 : 0;
	const int removal     = GET_VARIABLE( g_variables.m_remove_flash, bool ) ? 1 : 0;

	const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	const int weapon_id = weapon ? static_cast< int >( weapon->get_item_definition_index( ) ) : -1;

	const auto weapon_data = weapon && g_interfaces.m_weapon_system
	                             ? g_interfaces.m_weapon_system->get_weapon_data( static_cast< short >( weapon_id ) )
	                             : nullptr;
	const int weapon_type  = weapon_data ? weapon_data->m_weapon_type : -1;

	const int menu = g_menu.m_opened ? 1 : 0;

	const int fades       = g_flash_probe.m_flash_fades;
	const int other_fades = g_flash_probe.m_other_fades;
	const int fade_rgb    = g_flash_probe.m_last_color[ 0 ];
	const int fade_alpha  = g_flash_probe.m_last_color[ 3 ];
	const char* fade_name = g_flash_probe.m_last_name[ 0 ] ? g_flash_probe.m_last_name : "(none)";

	g_flash_probe.m_flash_fades = 0;
	g_flash_probe.m_other_fades = 0;

	const int blur  = GET_VARIABLE( g_variables.m_motion_blur, bool ) ? 1 : 0;
	const int grade = GET_VARIABLE( g_variables.m_color_correction, bool ) ? 1 : 0;
	const int spoof = GET_VARIABLE( g_variables.m_resolution_spoof, bool ) ? 1 : 0;

	/* back buffer at 3 stages: centre rgb + 4 quarter points' red. white = fade landed; dark at all 5 = never
	   reached the bb. read FINAL first: pre / post are read at end_scene, where the bb is a frame stale. */
	const auto stage = [ ]( const int ( &points )[ 5 ][ 3 ] ) {
		return std::format( "{:d},{:d},{:d} q {:d}/{:d}/{:d}/{:d}", points[ 0 ][ 0 ], points[ 0 ][ 1 ], points[ 0 ][ 2 ], points[ 1 ][ 0 ],
		                    points[ 2 ][ 0 ], points[ 3 ][ 0 ], points[ 4 ][ 0 ] );
	};

	std::string pre_pixels   = stage( g_flash_probe.m_pre );
	std::string post_pixels  = stage( g_flash_probe.m_post );
	std::string final_pixels = stage( g_flash_probe.m_final );

	g_console.print( std::vformat( "flash: duration {:.2f} max_alpha {:.1f} wep {:d} type {:d} menu {:d} fades {:d}/{:d} last {:s} rgb {:d} a {:d} "
	                               "| bb pre {:s} post {:s} final {:s} "
	                               "| ao {:d} dof {:d} blur {:d} grade {:d} spoof {:d} remove_flash {:d}",
	                               std::make_format_args( duration, max_alpha, weapon_id, weapon_type, menu, fades, other_fades, fade_name, fade_rgb,
	                                                      fade_alpha, pre_pixels, post_pixels, final_pixels, ao, dof, blur, grade, spoof, removal ) )
	                     .c_str( ) );
}

static std::string watermark_clock( )
{
	std::time_t raw = std::time( nullptr );
	std::tm     lt  = { };
	localtime_s( &lt, &raw );

	char time_buf[ 6 ] = { };
	snprintf( time_buf, sizeof( time_buf ), "%02d:%02d", lt.tm_hour, lt.tm_min );

	return time_buf;
}

std::string n_misc::watermark_label( )
{
	switch ( GET_VARIABLE( g_variables.m_watermark_label, int ) ) {
	case 0:
		return watermark_clock( );
	case 1:
		return std::to_string( g_utilities.get_average_fps( ImGui::GetIO( ) ) ) + " fps";
	case 2:
		return GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string );
	default:
		return { };
	}
}

std::string n_misc::watermark_brand( )
{
	const std::string& text = GET_VARIABLE( g_variables.m_watermark_text, std::string );
	return text.empty( ) ? "botox" : text;
}

static std::vector< std::string > watermark_info_parts( )
{
	std::vector< std::string > parts;

	if ( GET_VARIABLE( g_variables.m_watermark_user, bool ) )
		parts.push_back( " | " + GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string ) );

	/* server tickrate = 1 / interval_per_tick. in game only: outside it the interval is stale */
	if ( GET_VARIABLE( g_variables.m_watermark_tickrate, bool ) && g_interfaces.m_engine_client->is_in_game( ) && g_interfaces.m_global_vars_base &&
	     g_interfaces.m_global_vars_base->m_interval_per_tick > 0.f )
		parts.push_back( " | " + std::to_string( static_cast< int >( 1.f / g_interfaces.m_global_vars_base->m_interval_per_tick + 0.5f ) ) + " tick" );

	if ( GET_VARIABLE( g_variables.m_watermark_fps, bool ) )
		parts.push_back( " | " + std::to_string( g_utilities.get_average_fps( ImGui::GetIO( ) ) ) + " fps" );

	if ( GET_VARIABLE( g_variables.m_watermark_time, bool ) )
		parts.push_back( " | " + watermark_clock( ) );

	return parts;
}

void n_misc::impl_t::draw_watermark( )
{
	n_misc::g_watermark_box = { };

	if ( g_ctx.m_width <= 0 || g_ctx.m_height <= 0 )
		return;

	const int style = GET_VARIABLE( g_variables.m_watermark_style, int );

	if ( style == 3 )
		return this->draw_watermark_clarity_v2( );

	if ( style == 19 )
		return this->draw_watermark_cucumber( );

	if ( !GET_VARIABLE( g_variables.m_watermark, bool ) )
		return;

	switch ( style ) {
	case 1:
		return this->draw_watermark_kamibebra( );
	case 2:
		return this->draw_watermark_clarity( );
	case 4:
		return this->draw_watermark_delusional( );
	case 5:
		return this->draw_watermark_interwebz( );
	case 6:
		return this->draw_watermark_havoc( );
	case 7:
		return this->draw_watermark_onetap( );
	case 8:
		return this->draw_watermark_airflow( );
	case 9:
		return this->draw_watermark_evolve( );
	case 10:
		return this->draw_watermark_legendware( );
	case 11:
		return this->draw_watermark_interium( );
	case 12:
		return this->draw_watermark_skebob( );
	case 13:
		return this->draw_watermark_cumidere( );
	case 14:
		return this->draw_watermark_illusory( );
	case 15:
		return this->draw_watermark_lumi( );
	case 16:
		return this->draw_watermark_billware( );
	case 17:
		return this->draw_watermark_dna( );
	case 18:
		return this->draw_watermark_dna_clarity( );
	case 20:
		return this->draw_watermark_howeweware( );
	default:
		break;
	}

	const auto bold_font = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
	const auto slim_font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	if ( !bold_font || !slim_font )
		return;

	const ImU32 accent = ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent );
	const ImU32 white  = IM_COL32( 255, 255, 255, 255 );

	std::vector< std::tuple< std::string, ImU32, ImFont* > > segments;
	segments.emplace_back( n_misc::watermark_brand( ), white, bold_font );

	for ( auto& part : watermark_info_parts( ) )
		segments.emplace_back( std::move( part ), accent, slim_font );

	float content_w = 0.f, content_h = 0.f;
	for ( const auto& [ text, color, seg_font ] : segments ) {
		const auto s = seg_font->CalcTextSizeA( seg_font->FontSize, FLT_MAX, 0.f, text.c_str( ) );
		content_w += s.x;
		content_h = ( std::max )( content_h, s.y );
	}

	constexpr float padding_x = 14.f, height = 25.f, min_width = 95.f;

	const float rounding = ImGui::GetStyle( ).WindowRounding;

	const float width = segments.size( ) > 1u
	                    ? ( std::max )( content_w + padding_x * 2.f, min_width )
	                    : content_w + padding_x * 2.f;

	const float pos_x = g_ctx.m_width - width - 8.f;
	const float pos_y = 8.f;

	n_misc::g_watermark_box = ImVec4( pos_x, pos_y, pos_x + width, pos_y + height );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddRectFilled( ImVec2( pos_x, pos_y ), ImVec2( pos_x + width, pos_y + height ),
	                          ImColor( 25 / 255.f, 25 / 255.f, 25 / 255.f, 1.f ), rounding );
	draw_list->AddRect( ImVec2( pos_x + 1.f, pos_y + 1.f ), ImVec2( pos_x + width - 1.f, pos_y + height - 1.f ),
	                    ImColor( 50 / 255.f, 50 / 255.f, 50 / 255.f, 1.f ), rounding );

	float       text_x = pos_x + ( width - content_w ) * 0.5f;
	const float text_y = pos_y + ( height - content_h ) * 0.5f;

	for ( const auto& [ text, color, seg_font ] : segments ) {
		draw_list->AddText( seg_font, seg_font->FontSize, ImVec2( text_x, text_y ), color, text.c_str( ) );
		text_x += seg_font->CalcTextSizeA( seg_font->FontSize, FLT_MAX, 0.f, text.c_str( ) ).x;
	}

	g_render.end_stretch_block( block );
}

static c_color kami_rgb_last{ 255, 255, 255, 255 };

static c_color kami_rgb_color( const float dt )
{
	static float rgb_phase = 0.f;

	float speed = GET_VARIABLE( g_variables.m_watermark_kami_rgb_speed, float );
	if ( speed <= 0.f )
		speed = 0.5f;

	rgb_phase += dt * speed * 0.2f;
	if ( rgb_phase > 1.f )
		rgb_phase -= std::floor( rgb_phase );

	const float h = rgb_phase * 6.f;
	const int   i = static_cast< int >( h );
	const float f = h - static_cast< float >( i ), q = 1.f - f;

	constexpr float s = 0.6f, v = 0.9f;

	float r = 0.f, g = 0.f, b = 0.f;
	switch ( i % 6 ) {
	case 0: r = v; g = v * ( 1.f - s * ( 1.f - f ) ); b = v * ( 1.f - s ); break;
	case 1: r = v * ( 1.f - s * q ); g = v; b = v * ( 1.f - s ); break;
	case 2: r = v * ( 1.f - s ); g = v; b = v * ( 1.f - s * ( 1.f - f ) ); break;
	case 3: r = v * ( 1.f - s ); g = v * ( 1.f - s * q ); b = v; break;
	case 4: r = v * ( 1.f - s * ( 1.f - f ) ); g = v * ( 1.f - s ); b = v; break;
	case 5: r = v; g = v * ( 1.f - s ); b = v * ( 1.f - s * q ); break;
	}

	return kami_rgb_last = c_color( static_cast< int >( r * 255.f ), static_cast< int >( g * 255.f ), static_cast< int >( b * 255.f ), 255 );
}

void n_misc::impl_t::draw_watermark_kamibebra( )
{
	ImFont* const font = g_render.m_fonts[ GET_VARIABLE( g_variables.m_watermark_kami_bold_font, bool ) ? e_font_names::font_name_kamibebra_bold_13
	                                                                                                      : e_font_names::font_name_kamibebra_13 ];
	if ( !font )
		return;

	const float dt = ImGui::GetIO( ).DeltaTime;

	const bool        custom_text = GET_VARIABLE( g_variables.m_watermark_kami_custom_text, bool );
	const std::string main_text   = custom_text ? GET_VARIABLE( g_variables.m_watermark_kami_custom_text_value, std::string ) : n_misc::watermark_brand( );

	static std::string last_main_text{ };
	static size_t      typing_index = 0;
	static float       typing_timer = 0.f, pause_timer = 0.f;
	static bool        typing_deleting = false, in_pause = false, pause_after_typing = false;

	if ( main_text != last_main_text ) {
		typing_index    = 0;
		typing_deleting = false;
		typing_timer    = 0.f;
		last_main_text  = main_text;
	}

	std::string display_main_text = main_text;

	if ( GET_VARIABLE( g_variables.m_watermark_kami_typing, bool ) ) {
		if ( in_pause ) {
			pause_timer += dt;

			if ( pause_timer >= ( pause_after_typing ? GET_VARIABLE( g_variables.m_watermark_kami_typing_inactive, float )
			                                         : GET_VARIABLE( g_variables.m_watermark_kami_deleting_inactive, float ) ) ) {
				in_pause        = false;
				pause_timer     = 0.f;
				typing_deleting = pause_after_typing;
			}
		} else {
			typing_timer += dt;

			const int chars_per_second = typing_deleting ? GET_VARIABLE( g_variables.m_watermark_kami_deleting_speed, int )
			                                             : GET_VARIABLE( g_variables.m_watermark_kami_typing_speed, int );

			if ( typing_timer >= 1.f / static_cast< float >( ( std::max )( chars_per_second, 1 ) ) ) {
				typing_timer = 0.f;

				if ( !typing_deleting && typing_index < main_text.length( ) )
					++typing_index;
				else if ( typing_deleting && typing_index > 0 )
					--typing_index;
				else {
					in_pause           = true;
					pause_timer        = 0.f;
					pause_after_typing = !typing_deleting;
				}
			}
		}

		display_main_text = main_text.substr( 0, typing_index );
	} else {
		typing_index    = 0;
		typing_deleting = false;
	}

	const int  show_flags = GET_VARIABLE( g_variables.m_watermark_kami_show_flags, int );
	const bool show_user = ( show_flags & 1 ) != 0, show_time = ( show_flags & 2 ) != 0, show_fps = ( show_flags & 4 ) != 0;

	static int   cached_fps       = 0;
	static float fps_update_timer = 0.f;

	if ( show_fps ) {
		fps_update_timer += dt;

		if ( fps_update_timer >= 0.5f ) {
			cached_fps       = dt > 0.f ? static_cast< int >( 1.f / dt ) : 0;
			fps_update_timer = 0.f;
		}
	}

	const std::string time_text = show_time ? watermark_clock( ) : std::string{ };
	const std::string fps_text  = std::to_string( cached_fps );
	const std::string& user     = GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string );

	std::string full_text = display_main_text;
	if ( show_user )
		full_text += " | " + user;
	if ( show_time )
		full_text += " | " + time_text;
	if ( show_fps )
		full_text += " | " + fps_text + " fps";

	const float  font_size = font->FontSize;
	const ImVec2 full_size = font->CalcTextSizeA( font_size, FLT_MAX, 0.f, full_text.c_str( ) );

	constexpr float min_width = 95.f, height = 28.f, padding_x = 14.f;

	float width = min_width;

	if ( !GET_VARIABLE( g_variables.m_watermark_kami_mikudere_type, bool ) && ( custom_text || ( show_flags & 7 ) != 0 ) ) {
		const float  target_width  = ( std::max )( full_size.x + padding_x * 2.f, min_width );
		static float current_width = target_width;

		current_width += ( target_width - current_width ) * 12.f * dt;
		width = current_width;
	}

	const float pos_x = g_ctx.m_width - width - 8.f, pos_y = 8.f;

	n_misc::g_watermark_box = ImVec4( pos_x, pos_y, pos_x + width, pos_y + height );

	const float rounding =
		GET_VARIABLE( g_variables.m_watermark_kami_rounding, bool ) ? GET_VARIABLE( g_variables.m_watermark_kami_rounding_value, float ) : 4.f;

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddRectFilled( ImVec2( pos_x, pos_y ), ImVec2( pos_x + width, pos_y + height ), IM_COL32( 16, 16, 16, 255 ), rounding );

	const c_color& stroke = GET_VARIABLE( g_variables.m_watermark_kami_outline_color, c_color );

	if ( GET_VARIABLE( g_variables.m_watermark_kami_outline, bool ) ) {
		const c_color outline = GET_VARIABLE( g_variables.m_watermark_kami_rgb_mode, bool ) ? kami_rgb_color( dt ) : stroke;

		draw_list->AddRect( ImVec2( pos_x, pos_y ), ImVec2( pos_x + width, pos_y + height ), IM_COL32( outline[ 0 ], outline[ 1 ], outline[ 2 ], outline[ 3 ] ),
		                    rounding, ImDrawFlags_RoundCornersAll, 1.f );
	}

	/* "text color like stroke" takes the picked colour, never the rgb one */
	const ImU32 main_color =
		GET_VARIABLE( g_variables.m_watermark_kami_text_color_like_stroke, bool ) ? IM_COL32( stroke[ 0 ], stroke[ 1 ], stroke[ 2 ], 255 ) : IM_COL32( 255, 255, 255, 255 );
	constexpr ImU32 info_color = IM_COL32( 146, 144, 145, 255 );

	float       text_x = pos_x + ( width - full_size.x ) * 0.5f;
	const float text_y = pos_y + ( height - full_size.y ) * 0.5f - 1.f;

	const auto put = [ & ]( const std::string& text, const ImU32 color ) {
		draw_list->AddText( font, font_size, ImVec2( text_x, text_y ), color, text.c_str( ) );
		text_x += font->CalcTextSizeA( font_size, FLT_MAX, 0.f, text.c_str( ) ).x;
	};

	put( display_main_text, main_color );

	if ( show_user ) {
		put( " | ", info_color );
		put( user, info_color );
	}

	if ( show_time ) {
		put( " | ", info_color );
		put( time_text, info_color );
	}

	if ( show_fps ) {
		put( " | ", info_color );
		put( fps_text, main_color );
		put( " fps", info_color );
	}

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_clarity( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
	if ( !font )
		return;

	std::string text = n_misc::watermark_brand( );
	for ( const auto& part : watermark_info_parts( ) )
		text += part;

	const float  font_size = font->FontSize;
	const ImVec2 text_size = font->CalcTextSizeA( font_size, FLT_MAX, 0.f, text.c_str( ) );
	const ImVec2 position( g_ctx.m_width - 6.f - text_size.x, 5.f );

	n_misc::g_watermark_box = ImVec4( position.x, position.y, position.x + text_size.x, position.y + text_size.y );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddText( font, font_size, ImVec2( position.x + 1.f, position.y + 1.f ), IM_COL32( 0, 0, 0, 255 ), text.c_str( ) );
	draw_list->AddText( font, font_size, position, IM_COL32( 255, 255, 255, 255 ), text.c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_delusional( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_tahoma_12 ];
	if ( !font )
		return;

	const std::string label = watermark_label( );
	const std::string main  = label.empty( ) ? watermark_brand( ) : watermark_brand( ) + " | " + label;

	const ImVec2 text_size = font->CalcTextSizeA( 12.f, FLT_MAX, 0.f, main.c_str( ) );
	const ImVec2 padding( 7.f, 7.f ), margin( 4.f, 3.f );
	const float  w = static_cast< float >( g_ctx.m_width );

	const ImVec4 accent              = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );
	const ImU32  outline_start_color = ImColor( accent.x, accent.y, accent.z, 1.f );
	const ImU32  outline_end_color   = ImColor( accent.x, accent.y, accent.z, 0.f );

	n_misc::g_watermark_box = ImVec4( w - text_size.x - padding.x - margin.x * 2.f - 1.f, padding.y - 1.f, w - padding.x + 1.f,
	                                  text_size.y + padding.y + margin.y * 2.f + 1.f );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddRectFilledMultiColor( ImVec2( w - text_size.x - padding.x - margin.x * 2.f - 1.f, padding.y - 1.f ),
	                                    ImVec2( w - padding.x + 1.f, text_size.y + padding.y + margin.y * 2.f + 1.f ), outline_start_color,
	                                    outline_start_color, outline_end_color, outline_end_color );

	draw_list->AddRectFilled( ImVec2( w - text_size.x - padding.x - margin.x * 2.f, padding.y ),
	                          ImVec2( w - padding.x, text_size.y + padding.y + margin.y * 2.f ), ImColor( 0.08f, 0.08f, 0.08f, 1.f ), 0.f );

	draw_list->AddText( font, 12.f, ImVec2( w - text_size.x - padding.x - margin.x + 1.f, padding.y + margin.y ), IM_COL32( 0, 0, 0, 255 ),
	                    main.c_str( ) );
	draw_list->AddText( font, 12.f, ImVec2( w - text_size.x - padding.x - margin.x, padding.y + margin.y ), IM_COL32( 255, 255, 255, 255 ),
	                    main.c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_interwebz( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_interwebz_calibri ];
	if ( !font )
		return;

	std::string brand = watermark_brand( );
	std::transform( brand.begin( ), brand.end( ), brand.begin( ), []( const unsigned char c ) { return static_cast< char >( std::toupper( c ) ); } );

	const std::string label = watermark_label( );
	const std::string line  = brand + " CS:GO" + ( label.empty( ) ? "" : " | " + label );
	const char* const text  = line.c_str( );

	const float  font_size = font->FontSize;
	const float  width     = std::floor( font->CalcTextSizeA( font_size, FLT_MAX, 0.f, text ).x / 2.f );
	const ImVec2 position( 250.f - width, 20.f + 16.f - std::round( font->Ascent ) );

	constexpr ImVec2 offsets[ 8 ] = { { -1.f, -1.f }, { 0.f, -1.f }, { 1.f, -1.f }, { -1.f, 0.f },
		                             { 1.f, 0.f },   { -1.f, 1.f }, { 0.f, 1.f },  { 1.f, 1.f } };

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	for ( const auto& offset : offsets )
		draw_list->AddText( font, font_size, ImVec2( position.x + offset.x, position.y + offset.y ), IM_COL32( 0, 0, 0, 255 ), text );

	draw_list->AddText( font, font_size, position, IM_COL32( 255, 255, 255, 255 ), text );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_onetap( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_tahoma_12 ];
	if ( !font )
		return;

	std::string text = watermark_brand( );

	if ( const std::string label = watermark_label( ); !label.empty( ) )
		text += " | " + label;

	if ( g_ctx.m_local && g_interfaces.m_engine_client->is_in_game( ) ) {
		const float interval = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.f;
		const int   rate     = interval > 0.f ? static_cast< int >( 1.f / interval + 0.5f ) : 0;
		const int   choke    = g_interfaces.m_client_state ? g_interfaces.m_client_state->m_choked_commands : 0;

		text += std::format( " | rate: {} | choke: {} | shots: {}", rate, choke, g_ctx.m_local->get_shots_fired( ) );
	}

	const float w = static_cast< float >( g_ctx.m_width );

	n_misc::g_watermark_box = ImVec4( w - 280.f, 10.f, w - 10.f, 30.f );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddRectFilled( ImVec2( w - 280.f, 10.f ), ImVec2( w - 10.f, 30.f ), IM_COL32( 185, 90, 105, 160 ) );
	draw_list->AddText( font, font->FontSize, ImVec2( w - 275.f, 14.f ), IM_COL32( 255, 255, 255, 150 ), text.c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_clarity_v2( )
{
	if ( !GET_VARIABLE( g_variables.m_watermark, bool ) )
		return;

	const auto font      = g_render.m_fonts[ e_font_names::font_name_clarity_inter_semibold_14 ];
	const auto icon_font = g_render.m_fonts[ e_font_names::font_name_clarity_icon_50 ];
	if ( !font || !icon_font || !g_interfaces.m_global_vars_base )
		return;

	const auto text_size = [ ]( const ImFont* f, const std::string& text ) {
		const ImVec2 size = f->CalcTextSizeA( f->FontSize, FLT_MAX, 0.f, text.c_str( ) );
		return std::pair{ static_cast< int >( size.x ), static_cast< int >( size.y ) };
	};

	static int   fps = 0, frames = 0;
	static float frame_sum = 0.f, last_update = 0.f, slot_width = 0.f;

	frame_sum += g_interfaces.m_global_vars_base->m_abs_frame_time;
	++frames;
	if ( const float now = g_interfaces.m_global_vars_base->m_real_time; now - 0.4f >= last_update || now < last_update ) {
		fps         = frame_sum > 0.f ? static_cast< int >( 1.f / ( frame_sum / static_cast< float >( frames ) ) ) : 0;
		frames      = 0;
		frame_sum   = 0.f;
		last_update = now;
	}

	const ImVec4 accent_v = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );
	const ImU32  accent   = ImGui::ColorConvertFloat4ToU32( ImVec4( accent_v.x, accent_v.y, accent_v.z, 1.f ) );

	const std::string fps_text = std::to_string( fps );

	/* number slot = digits * width( "9" ) + 3, eased at 15 / s so the box never jitters per digit */
	const float slot_target = static_cast< float >( static_cast< int >( fps_text.size( ) ) * text_size( font, "9" ).first + 3 );
	if ( slot_width != slot_target )
		slot_width = std::lerp( slot_width, slot_target, std::clamp( 15.f * ImGui::GetIO( ).DeltaTime, 0.f, 1.f ) );

	struct segment_t {
		std::string m_text;
		ImU32 m_color;
		int m_width;
	};

	const segment_t segments[ ] = {
		{ watermark_brand( ), accent, -1 },
		{ { }, 0, 12 },
		{ GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string ), IM_COL32( 255, 255, 255, 150 ), -1 },
		{ { }, 0, 12 },
		{ fps_text, IM_COL32( 255, 255, 255, 255 ), static_cast< int >( slot_width ) },
		{ "fps", IM_COL32( 255, 255, 255, 150 ), -1 },
	};

	const auto segment_width = [ & ]( const segment_t& segment ) {
		return segment.m_width == -1 ? text_size( font, segment.m_text ).first : segment.m_width;
	};

	int text_width = 0;
	for ( const auto& segment : segments )
		text_width += segment_width( segment );

	constexpr int font_h = 10;
	const int w = text_width + 10 * 2, h = font_h + 7 * 2;
	const int x = g_ctx.m_width - 7 - w, y = 7;

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const auto rect = [ & ]( const int grow ) {
		return std::pair{ ImVec2( static_cast< float >( x - grow ), static_cast< float >( y - grow ) ),
			              ImVec2( static_cast< float >( x + w + grow ), static_cast< float >( y + h + grow ) ) };
	};

	const auto [ fill_min, fill_max ] = rect( 3 );
	draw_list->AddRectFilled( fill_min, fill_max, IM_COL32( 25, 25, 25, 255 ), 5.f );

	n_misc::g_watermark_box = ImVec4( fill_min.x, fill_min.y, fill_max.x, fill_max.y );

	{
		const auto [ icon_w, icon_h ] = text_size( icon_font, "A" );

		draw_list->PushClipRect( fill_min, fill_max, true );
		draw_list->AddText( icon_font, icon_font->FontSize,
		                    ImVec2( static_cast< float >( x + 15 - icon_w / 2 ), static_cast< float >( y + h / 2 - 1 - icon_h / 2 ) ),
		                    ( accent & ~IM_COL32_A_MASK ) | ( 35u << IM_COL32_A_SHIFT ), "A" );
		draw_list->PopClipRect( );
	}

	const ImU32 rings[ ] = { IM_COL32( 0, 0, 0, 255 ), IM_COL32( 50, 50, 50, 255 ), IM_COL32( 0, 0, 0, 255 ) };
	for ( int grow = 0; grow < 3; ++grow ) {
		const auto [ ring_min, ring_max ] = rect( grow );
		draw_list->AddRect( ring_min, ring_max, rings[ grow ], 5.f );
	}

	int       cursor = x + 10;
	const int mid    = y + h / 2;
	for ( const auto& segment : segments ) {
		if ( !segment.m_text.empty( ) ) {
			const float ty     = static_cast< float >( mid - text_size( font, segment.m_text ).second / 2 );
			const float a      = static_cast< float >( ( segment.m_color >> IM_COL32_A_SHIFT ) & 0xFF ) / 255.f;
			const auto  pass_a = static_cast< ImU32 >( ( 1.f - std::sqrt( 1.f - a ) ) * 255.f + 0.5f );
			const ImU32 color  = ( segment.m_color & ~IM_COL32_A_MASK ) | ( pass_a << IM_COL32_A_SHIFT );
			for ( const float dx : { 0.f, 0.5f } )
				draw_list->AddText( font, font->FontSize, ImVec2( static_cast< float >( cursor ) + dx, ty ), color, segment.m_text.c_str( ) );
		} else {
			constexpr int half = font_h / 2;
			draw_list->AddLine( ImVec2( static_cast< float >( cursor + 6 ), static_cast< float >( mid - half ) ),
			                    ImVec2( static_cast< float >( cursor + 6 ), static_cast< float >( mid + half ) ), IM_COL32( 50, 50, 50, 255 ) );
		}
		cursor += segment_width( segment );
	}

	g_render.end_stretch_block( block );
}

static std::tuple< ImFont*, ImFont*, float > media_player_fonts( )
{
	ImFont* title  = nullptr;
	ImFont* artist = nullptr;
	float size     = 0.f;

	const auto pick = [ & ]( const e_font_names bold, const e_font_names regular ) {
		title  = g_render.m_fonts[ bold ];
		artist = g_render.m_fonts[ regular ];
	};

	{
		switch ( GET_VARIABLE( g_variables.m_watermark_style, int ) ) {
		case 1: pick( e_font_names::font_name_kamibebra_bold_13, e_font_names::font_name_kamibebra_13 ); break;
		case 3: pick( e_font_names::font_name_clarity_inter_semibold_14, e_font_names::font_name_clarity_inter_semibold_14 ); break;
		case 4:
		case 7: pick( e_font_names::font_name_tahoma_12, e_font_names::font_name_tahoma_12 ); break;
		case 5: pick( e_font_names::font_name_interwebz_calibri, e_font_names::font_name_interwebz_calibri ); break;
		case 6: pick( e_font_names::font_name_havoc_tahoma_13, e_font_names::font_name_havoc_tahoma_13 ); break;
		case 8: pick( e_font_names::font_name_airflow_14, e_font_names::font_name_airflow_14 ); break;
		case 9: pick( e_font_names::font_name_evolve_bold_16, e_font_names::font_name_evolve_16 ); break;
		case 10: pick( e_font_names::font_name_legendware_verdana_12, e_font_names::font_name_legendware_verdana_12 ); break;
		case 11:
			pick( e_font_names::font_name_interium_droid_24, e_font_names::font_name_interium_droid_24 );
			size = 16.f;
			break;
		case 12: pick( e_font_names::font_name_skebob_arial_12, e_font_names::font_name_skebob_arial_12 ); break;
		case 13: pick( e_font_names::font_name_cumidere_tahoma_16, e_font_names::font_name_cumidere_tahoma_16 ); break;
		case 14:
			pick( e_font_names::font_name_illusory_tahoma_13, e_font_names::font_name_illusory_tahoma_13 );
			size = 12.f;
			break;
		case 15: pick( e_font_names::font_name_lumi_rubik_16, e_font_names::font_name_lumi_rubik_16 ); break;
		case 16: pick( e_font_names::font_name_tahoma_bd_12, e_font_names::font_name_tahoma_bd_12 ); break;
		case 17: pick( e_font_names::font_name_dna_montserrat_15, e_font_names::font_name_dna_montserrat_15 ); break;
		case 18: pick( e_font_names::font_name_dna_pt_root_bold_15, e_font_names::font_name_dna_pt_root_bold_15 ); break;
		case 19: pick( e_font_names::font_name_cucumber_tahoma_14, e_font_names::font_name_cucumber_tahoma_14 ); break;
		case 20: pick( e_font_names::font_name_howeweware_minecraft_14, e_font_names::font_name_howeweware_minecraft_14 ); break;
		default: break;
		}
	}

	if ( !title || !artist ) {
		pick( e_font_names::font_name_verdana_bd_11, e_font_names::font_name_verdana_11 );
		size = 0.f;
	}

	return { title, artist, size };
}

static n_misc::media_look_t media_player_look( const int style )
{
	constexpr ImU32 white = IM_COL32( 255, 255, 255, 255 );

	const ImVec4 accent_v = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );
	const ImU32 accent    = ImGui::ColorConvertFloat4ToU32( ImVec4( accent_v.x, accent_v.y, accent_v.z, 1.f ) );

	switch ( style ) {
	case 1: {
		const c_color& stroke = GET_VARIABLE( g_variables.m_watermark_kami_outline_color, c_color );
		const ImU32 main =
			GET_VARIABLE( g_variables.m_watermark_kami_text_color_like_stroke, bool ) ? IM_COL32( stroke[ 0 ], stroke[ 1 ], stroke[ 2 ], 255 ) : white;
		return { true, main, IM_COL32( 146, 144, 145, 255 ), 0 };
	}
	case 2: return { false, white, white, 1 };
	case 3: return { true, accent, IM_COL32( 255, 255, 255, 150 ), 0 };
	case 4: return { true, white, white, 1 };
	case 5: return { false, white, white, 2 };
	case 7: return { true, IM_COL32( 255, 255, 255, 150 ), IM_COL32( 255, 255, 255, 150 ), 0 };
	default: {
		n_misc::media_look_t look{ true, white, accent, 0 };
		n_misc::media_player_donor_look( style, look );
		return look;
	}
	}
}

/* the style's watermark box grown to [ min, max ]. bare styles ( m_box false ) pass 0 */
static void media_player_frame( ImDrawList* list, const ImVec2 min, const ImVec2 max, const int style )
{
	switch ( style ) {
	case 0:
		list->AddRectFilled( min, max, IM_COL32( 25, 25, 25, 255 ), ImGui::GetStyle( ).WindowRounding );
		list->AddRect( ImVec2( min.x + 1.f, min.y + 1.f ), ImVec2( max.x - 1.f, max.y - 1.f ), IM_COL32( 50, 50, 50, 255 ), ImGui::GetStyle( ).WindowRounding );
		break;
	case 1: {
		const float rounding =
			GET_VARIABLE( g_variables.m_watermark_kami_rounding, bool ) ? GET_VARIABLE( g_variables.m_watermark_kami_rounding_value, float ) : 4.f;

		list->AddRectFilled( min, max, IM_COL32( 16, 16, 16, 255 ), rounding );

		if ( GET_VARIABLE( g_variables.m_watermark_kami_outline, bool ) ) {
			const c_color outline =
				GET_VARIABLE( g_variables.m_watermark_kami_rgb_mode, bool ) ? kami_rgb_last : GET_VARIABLE( g_variables.m_watermark_kami_outline_color, c_color );

			list->AddRect( min, max, IM_COL32( outline[ 0 ], outline[ 1 ], outline[ 2 ], outline[ 3 ] ), rounding, ImDrawFlags_RoundCornersAll, 1.f );
		}
		break;
	}
	case 3: {
		list->AddRectFilled( min, max, IM_COL32( 25, 25, 25, 255 ), 5.f );

		const ImU32 rings[ ] = { IM_COL32( 0, 0, 0, 255 ), IM_COL32( 50, 50, 50, 255 ), IM_COL32( 0, 0, 0, 255 ) };
		for ( int grow = 0; grow < 3; ++grow ) {
			const float inset = static_cast< float >( 3 - grow );
			list->AddRect( ImVec2( min.x + inset, min.y + inset ), ImVec2( max.x - inset, max.y - inset ), rings[ grow ], 5.f );
		}
		break;
	}
	case 4: {
		const ImVec4 accent = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );
		const ImU32 top = ImColor( accent.x, accent.y, accent.z, 1.f ), bottom = ImColor( accent.x, accent.y, accent.z, 0.f );

		list->AddRectFilledMultiColor( min, max, top, top, bottom, bottom );
		list->AddRectFilled( ImVec2( min.x + 1.f, min.y + 1.f ), ImVec2( max.x - 1.f, max.y - 1.f ), ImColor( 0.08f, 0.08f, 0.08f, 1.f ) );
		break;
	}
	case 7: list->AddRectFilled( min, max, IM_COL32( 185, 90, 105, 160 ) ); break;
	default: n_misc::media_player_donor_frame( list, min, max, style ); break;
	}
}

// "4:11 / 6:09", hours once a track runs past one. empty = length unknown
static std::string media_time_text( )
{
	const long long total = g_media_player.get_total_ms( );
	if ( total <= 0 )
		return { };

	const auto format = [ ]( const double ms ) {
		const long long s = static_cast< long long >( ms / 1000.0 );
		char out[ 32 ]{ };
		if ( s >= 3600 )
			snprintf( out, sizeof( out ), "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60 );
		else
			snprintf( out, sizeof( out ), "%lld:%02lld", s / 60, s % 60 );
		return std::string( out );
	};

	const double total_ms = static_cast< double >( total );
	return format( std::clamp( g_media_player.get_position_ms( ), 0.0, total_ms ) ) + " / " + format( total_ms );
}

/* fires on press. a tap shorter than one frame only shows as a release, so the release counter catches it */
static void media_player_binds( )
{
	struct bind_t {
		int m_key;
		std::uint32_t m_seq;
		bool m_fired;
	};
	static bind_t binds[ 3 ]{ };

	const key_bind_t* keys[ 3 ]{ &GET_VARIABLE( g_variables.m_media_player_previous_key, key_bind_t ),
		                         &GET_VARIABLE( g_variables.m_media_player_toggle_key, key_bind_t ),
		                         &GET_VARIABLE( g_variables.m_media_player_next_key, key_bind_t ) };
	static constexpr n_media_player::impl_t::e_command k_commands[ 3 ]{ n_media_player::impl_t::command_previous, n_media_player::impl_t::command_toggle,
		                                                                 n_media_player::impl_t::command_next };

	const bool on      = GET_VARIABLE( g_variables.m_media_player_ingame_control, bool );
	const bool blocked = g_input.keys_blocked( );

	for ( int i = 0; i < 3; i++ ) {
		auto& bind    = binds[ i ];
		const int key = keys[ i ]->m_key;

		// media keys already reach the player through windows, firing too would skip twice
		if ( !on || key <= 0 || key > 255 || ( key >= VK_MEDIA_NEXT_TRACK && key <= VK_MEDIA_PLAY_PAUSE ) ) {
			bind.m_key = 0;
			continue;
		}

		const std::uint32_t seq = g_input.m_release_seq[ key ];
		const bool down         = g_input.is_key_down( key );

		if ( bind.m_key != key ) {
			bind = { key, seq, down };
			continue;
		}

		bool fire = false;
		if ( seq != bind.m_seq ) {
			fire         = !bind.m_fired;
			bind.m_seq   = seq;
			bind.m_fired = false;
		}
		if ( down && !bind.m_fired ) {
			fire         = true;
			bind.m_fired = true;
		}

		if ( fire && !blocked )
			g_media_player.request( k_commands[ i ] );
	}
}

void n_misc::impl_t::draw_media_player( )
{
	if ( !GET_VARIABLE( g_variables.m_media_player, bool ) )
		return;

	media_player_binds( );

	if ( g_ctx.m_width <= 0 || g_ctx.m_height <= 0 )
		return;

	/* update thread never touches the device: texture built here, render thread */
	g_media_player.update_texture( g_interfaces.m_direct_device );

	if ( !g_media_player.m_has_media )
		return;

	const auto [ title_font, artist_font, size_override ] = media_player_fonts( );
	if ( !title_font || !artist_font )
		return;

	const float title_px  = size_override > 0.f ? size_override : title_font->FontSize;
	const float artist_px = size_override > 0.f ? size_override : artist_font->FontSize;

	const int position     = std::clamp( GET_VARIABLE( g_variables.m_media_player_position, int ), 0, 5 );
	const bool from_bottom = position >= 3;
	const int align        = position == 0 || position == 5 ? 2 : position == 1 || position == 4 ? 0 : 1;
	const float screen_w  = static_cast< float >( g_ctx.m_width ), mid_x = screen_w * 0.5f;
	const ImVec4 wm_box   = n_misc::g_watermark_box;
	const bool wm_column  = wm_box.z > wm_box.x && wm_box.y < g_ctx.m_height * 0.5f &&
	                        ( align == 2 ? wm_box.z > mid_x : align == 0 ? wm_box.x < mid_x : wm_box.x < mid_x && wm_box.z > mid_x );
	const bool under_wm   = !from_bottom && wm_column;
	const float anchor_x  = align == 1 ? mid_x : align == 2 ? ( under_wm ? wm_box.z : screen_w - 8.f ) : ( under_wm ? wm_box.x : 8.f );
	const float anchor_y   = from_bottom ? g_ctx.m_height - 8.f : under_wm ? wm_box.w + 4.f : 8.f;
	const auto left_of     = [ & ]( const float w ) { return align == 0 ? anchor_x : align == 1 ? anchor_x - w * 0.5f : anchor_x - w; };

	const auto title  = g_utilities.truncate_utf8( g_media_player.get_title( ), 64 );
	const auto artist = g_utilities.truncate_utf8( g_media_player.get_artist( ), 64 );

	if ( title.empty( ) )
		return;

	const int style           = GET_VARIABLE( g_variables.m_watermark_style, int );
	const media_look_t look   = media_player_look( style );
	const bool background     = GET_VARIABLE( g_variables.m_media_player_background, bool );
	const int frame_style     = look.m_box ? style : 0;
	const int bare_fx         = look.m_fx == 2 || look.m_fx == 3 ? look.m_fx : 1;
	const int fx              = background ? look.m_fx : bare_fx;

	static constexpr ImVec2 outline[ 8 ] = { { -1.f, -1.f }, { 0.f, -1.f }, { 1.f, -1.f }, { -1.f, 0.f },
		                                     { 1.f, 0.f },   { -1.f, 1.f }, { 0.f, 1.f },  { 1.f, 1.f } };

	const auto put = [ ]( ImDrawList* list, const int effect, ImFont* font, const float px, const ImVec2 pos, const ImU32 color, const std::string& text ) {
		const ImU32 black = color & IM_COL32_A_MASK;

		if ( effect == 1 )
			list->AddText( font, px, ImVec2( pos.x + 1.f, pos.y + 1.f ), black, text.c_str( ) );
		else if ( effect == 3 )
			list->AddText( font, px, ImVec2( pos.x - 1.f, pos.y + 1.f ), black, text.c_str( ) );
		else if ( effect == 2 )
			for ( const auto& offset : outline )
				list->AddText( font, px, ImVec2( pos.x + offset.x, pos.y + offset.y ), black, text.c_str( ) );

		list->AddText( font, px, pos, color, text.c_str( ) );
	};

	constexpr float art_size = 30.f, bar_height = 1.f, row_gap = 4.f;

	const auto draw_lyrics = [ & ]( ImDrawList* list, const float edge, const float player_w, const bool boxed, const int effect, ImRect& bounds ) {
		static std::vector< n_media_player::impl_t::lyric_line_t > lines{ };
		static int generation = -1;
		static float scroll = 0.f, box_w = 0.f;

		const int fresh = g_media_player.get_lyrics( lines, generation );
		const bool snap = fresh != generation;
		generation      = fresh;

		if ( !GET_VARIABLE( g_variables.m_media_player_lyrics, bool ) || lines.empty( ) )
			return;

		const double now = g_media_player.get_position_ms( ) + GET_VARIABLE( g_variables.m_media_player_lyrics_offset, float );
		const auto next  = std::upper_bound( lines.begin( ), lines.end( ), now, [ ]( const double t, const auto& line ) { return t < line.m_start_ms; } );

		const int count  = static_cast< int >( lines.size( ) );
		const int active = ( std::max )( 0, static_cast< int >( next - lines.begin( ) ) - 1 );
		const int rows   = std::clamp( GET_VARIABLE( g_variables.m_media_player_lyrics_rows, int ), 1, 9 );
		const int shown  = ( std::min )( rows, count );
		const int first  = std::clamp( active - ( rows - 1 ) / 2, 0, count - shown );

		const float pad  = boxed ? 10.f : 0.f;
		const float step = artist_px + row_gap;

		const float fade_step = 0.55f / static_cast< float >( ( std::max )( 1, rows / 2 ) );

		const auto text_at = [ & ]( const int k ) { return g_utilities.truncate_utf8( lines[ k ].m_text, 96 ); };

		float target_w = player_w;
		for ( int k = first; k < first + shown; k++ )
			target_w = ( std::max )( target_w, artist_font->CalcTextSizeA( artist_px, FLT_MAX, 0.f, text_at( k ).c_str( ) ).x + pad * 2.f );

		const float ease = 1.f - std::exp( -ImGui::GetIO( ).DeltaTime * 10.f );
		scroll           = snap ? static_cast< float >( first ) : scroll + ( static_cast< float >( first ) - scroll ) * ease;
		box_w            = snap || box_w <= 0.f ? target_w : box_w + ( target_w - box_w ) * ease;

		const float box_h = pad * 2.f + static_cast< float >( shown ) * artist_px + static_cast< float >( shown - 1 ) * row_gap;
		const float top   = from_bottom ? edge - box_h : edge;

		const ImVec2 min( left_of( box_w ), top );
		const ImVec2 max( min.x + box_w, top + box_h );
		bounds.Add( ImRect( min, max ) );

		if ( boxed )
			media_player_frame( list, min, max, frame_style );

		list->PushClipRect( min, max, true );

		for ( int k = ( std::max )( 0, first - 1 ); k <= ( std::min )( count - 1, first + shown ); k++ ) {
			const auto text = text_at( k );
			if ( text.empty( ) )
				continue;

			const float row  = static_cast< float >( k ) - scroll;
			const float edge = std::clamp( ( std::min )( row + 1.f, static_cast< float >( shown ) - row ), 0.f, 1.f );
			const float fade = ( std::max )( 0.f, 1.f - static_cast< float >( std::abs( k - active ) ) * fade_step ) * edge;

			const ImU32 alpha = static_cast< ImU32 >( static_cast< float >( ( look.m_title >> IM_COL32_A_SHIFT ) & 0xFF ) * fade );
			if ( !alpha )
				continue;

			const float text_w = artist_font->CalcTextSizeA( artist_px, FLT_MAX, 0.f, text.c_str( ) ).x;
			const float text_x = align == 0 ? min.x + pad : align == 1 ? std::floor( ( min.x + max.x - text_w ) * 0.5f ) : max.x - pad - text_w;
			put( list, effect, artist_font, artist_px, ImVec2( text_x, min.y + pad + row * step ),
			     ( look.m_title & ~IM_COL32_A_MASK ) | ( alpha << IM_COL32_A_SHIFT ), text );
		}

		list->PopClipRect( );
	};

	/* prev / play-pause / next while the menu is open. drawn last: the hit test needs the whole block's bounds,
	   dpi panel scale pivots on them ( same mapping as the spectator list drag ) */
	const bool buttons         = g_menu.m_opened;
	const std::string time_now = GET_VARIABLE( g_variables.m_media_player_time, bool ) ? media_time_text( ) : std::string{ };
	const float icon           = std::clamp( std::floor( artist_px * 0.7f ), 7.f, 14.f );
	const float cell           = std::floor( icon * 1.6f ), cell_gap = std::floor( icon * 0.4f );
	const float buttons_w      = buttons ? cell * 3.f + cell_gap * 2.f : 0.f;
	const ImU32 accent         = ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent );

	const auto draw_buttons = [ & ]( ImDrawList* list, const float x, const float mid_y, const int effect, const ImRect& bounds ) {
		const ImVec2 at = g_render.panel_mouse( bounds.Min, bounds.GetSize( ) );
		const bool live = !GImGui->HoveredWindow; // a menu window over the player eats the click
		const bool click = ImGui::IsMouseClicked( ImGuiMouseButton_::ImGuiMouseButton_Left );

		const ImU32 hover_color = look.m_title != look.m_artist ? look.m_title : accent;
		const float h           = icon * 0.5f;

		static constexpr n_media_player::impl_t::e_command k_commands[ 3 ]{ n_media_player::impl_t::command_previous,
			                                                                 n_media_player::impl_t::command_toggle, n_media_player::impl_t::command_next };

		int hit = -1;
		for ( int b = 0; b < 3; b++ ) {
			const float cx     = x + static_cast< float >( b ) * ( cell + cell_gap ) + cell * 0.5f;
			const bool hovered = live && std::abs( at.x - cx ) <= cell * 0.5f + 1.f && std::abs( at.y - mid_y ) <= cell * 0.5f + 1.f;

			if ( hovered && click ) {
				hit = b;
				g_media_player.request( k_commands[ b ] );
			}

			// clockwise on screen, the aa fringe goes outward
			const auto shape = [ & ]( const float sx, const float sy, const ImU32 color ) {
				if ( b == 0 ) {
					list->AddTriangleFilled( ImVec2( sx - h, sy ), ImVec2( sx, sy - h ), ImVec2( sx, sy + h ), color );
					list->AddTriangleFilled( ImVec2( sx, sy ), ImVec2( sx + h, sy - h ), ImVec2( sx + h, sy + h ), color );
				} else if ( b == 2 ) {
					list->AddTriangleFilled( ImVec2( sx + h, sy ), ImVec2( sx, sy + h ), ImVec2( sx, sy - h ), color );
					list->AddTriangleFilled( ImVec2( sx, sy ), ImVec2( sx - h, sy + h ), ImVec2( sx - h, sy - h ), color );
				} else if ( g_media_player.m_is_playing ) {
					list->AddRectFilled( ImVec2( sx - h * 0.8f, sy - h ), ImVec2( sx - h * 0.2f, sy + h ), color );
					list->AddRectFilled( ImVec2( sx + h * 0.2f, sy - h ), ImVec2( sx + h * 0.8f, sy + h ), color );
				} else
					list->AddTriangleFilled( ImVec2( sx - h * 0.7f, sy - h ), ImVec2( sx + h * 0.9f, sy ), ImVec2( sx - h * 0.7f, sy + h ), color );
			};

			const ImU32 color = hovered ? hover_color : look.m_artist;
			if ( effect )
				shape( cx + ( effect == 3 ? -1.f : 1.f ), mid_y + 1.f, color & IM_COL32_A_MASK );
			shape( cx, mid_y, color );
		}

		// click off the menu or near the block: which button, where the mouse mapped, what window ate it
		const bool by_block = at.x >= bounds.Min.x - 20.f && at.x <= bounds.Max.x + 20.f && at.y >= bounds.Min.y - 20.f && at.y <= bounds.Max.y + 20.f;
		if ( click && ( by_block || live ) ) {
			const ImVec2 mouse = g_render.screen_mouse( );
			botox_dbg_log( "MEDIA: click hit=%d mouse=%.0f,%.0f at=%.0f,%.0f row=%.0f..%.0f,%.0f win=%s", hit, mouse.x, mouse.y, at.x, at.y, x, x + buttons_w,
			               mid_y, live ? "-" : GImGui->HoveredWindow->Name );
		}
	};

	if ( GET_VARIABLE( g_variables.m_media_player_simple, bool ) ) {
		auto line = artist.empty( ) ? title : title + " - " + artist;
		if ( !time_now.empty( ) )
			line += "  " + time_now;
		const auto line_size = artist_font->CalcTextSizeA( artist_px, FLT_MAX, 0.f, line.c_str( ) );
		const float total_w  = line_size.x + ( buttons ? cell_gap * 2.f + buttons_w : 0.f );

		const auto draw_list = ImGui::GetForegroundDrawList( );
		const auto block     = g_render.begin_stretch_block( draw_list );
		const float line_y   = from_bottom ? anchor_y - line_size.y : anchor_y;
		const float line_x   = std::floor( left_of( total_w ) );
		put( draw_list, bare_fx, artist_font, artist_px, ImVec2( line_x, line_y ), look.m_title, line );

		ImRect bounds( ImVec2( line_x, line_y ), ImVec2( line_x + total_w, line_y + line_size.y ) );
		draw_lyrics( draw_list, from_bottom ? line_y - row_gap : line_y + line_size.y + row_gap, total_w, false, bare_fx, bounds );
		if ( buttons )
			draw_buttons( draw_list, line_x + line_size.x + cell_gap * 2.f, std::floor( line_y + line_size.y * 0.5f ), bare_fx, bounds );
		g_render.end_stretch_block( block );
		return;
	}

	const auto title_size  = title_font->CalcTextSizeA( title_px, FLT_MAX, 0.f, title.c_str( ) );
	const auto artist_size = artist_font->CalcTextSizeA( artist_px, FLT_MAX, 0.f, artist.c_str( ) );

	const float padding   = background ? 10.f : 0.f;
	const float min_width = background ? 140.f : 0.f;

	const bool progress_bar = GET_VARIABLE( g_variables.m_media_player_progress_bar, bool );

	const bool visualizer = GET_VARIABLE( g_variables.m_media_player_visualizer, bool );
	const float visual_h  = std::clamp( GET_VARIABLE( g_variables.m_media_player_visualizer_height, float ), 4.f, 64.f );

	const auto art = GET_VARIABLE( g_variables.m_media_player_cover_art, bool ) ? g_media_player.m_image : nullptr;

	const float art_gap = ( std::max )( padding, 6.f );

	const float art_offset_x = art ? art_size + art_gap : 0.f;

	const float text_col_w = ( std::max )( title_size.x, artist_size.x );
	const float text_col_h = title_size.y + row_gap + artist_size.y;

	const float content_h = art ? ( std::max )( art_size, text_col_h ) : text_col_h;

	const float time_w     = time_now.empty( ) ? 0.f : artist_font->CalcTextSizeA( artist_px, FLT_MAX, 0.f, time_now.c_str( ) ).x;
	const bool control_row = buttons || time_w > 0.f;
	const float control_h  = control_row ? ( std::max )( artist_size.y, cell ) : 0.f;
	const float control_w  = buttons_w + time_w + ( buttons && time_w > 0.f ? 8.f : 0.f );

	const float width  = ( std::max )( { text_col_w + art_offset_x + padding * 2.f, control_w + padding * 2.f, min_width } );
	const float height = padding + content_h + ( progress_bar || visualizer ? row_gap + bar_height : 0.f ) + ( control_row ? row_gap + control_h : 0.f ) + padding;

	const float pos_x = std::floor( left_of( width ) );
	const float pos_y = from_bottom ? anchor_y - height : anchor_y;

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	if ( background )
		media_player_frame( draw_list, ImVec2( pos_x, pos_y ), ImVec2( pos_x + width, pos_y + height ), frame_style );

	const auto accent_color = static_cast< ImColor >( ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent ) );

	if ( visualizer ) {
		float bands[ n_audio_bands::k_bands ]{ };
		g_media_player.get_bands( bands );

		const ImU32 rgb = GET_VARIABLE( g_variables.m_media_player_visualizer_custom_color, bool )
		                      ? GET_VARIABLE( g_variables.m_media_player_visualizer_color, c_color ).get_u32( )
		                      : static_cast< ImU32 >( accent_color );
		const ImU32 color = ( rgb & ~IM_COL32_A_MASK ) | ( 185u << IM_COL32_A_SHIFT );

		const bool smooth   = GET_VARIABLE( g_variables.m_media_player_visualizer_smooth, bool );
		const float left    = pos_x + padding;
		const float right   = pos_x + width - padding;
		const float floor_y = pos_y + padding + content_h + row_gap;
		const float gap     = 1.f;
		const float bar_w   = static_cast< float >( std::clamp( GET_VARIABLE( g_variables.m_media_player_visualizer_bar_width, int ), 1, 12 ) );
		const int bars      = smooth ? static_cast< int >( n_audio_bands::k_bands )
		                             : ( std::max )( 1, static_cast< int >( ( right - left + gap ) / ( bar_w + gap ) ) );
		const float slot    = ( right - left ) / static_cast< float >( bars );

		if ( smooth ) {
			const auto curve_top = [ & ]( const float x ) {
				const float u = ( x - left ) / slot - 0.5f;
				const int i   = static_cast< int >( std::floor( u ) );
				const float t = u - static_cast< float >( i );
				const auto at = [ & ]( const int k ) { return bands[ std::clamp( k, 0, bars - 1 ) ]; };
				const float p0 = at( i - 1 ), p1 = at( i ), p2 = at( i + 1 ), p3 = at( i + 2 );
				const float level =
					0.5f * ( 2.f * p1 + ( p2 - p0 ) * t + ( 2.f * p0 - 5.f * p1 + 4.f * p2 - p3 ) * t * t + ( 3.f * ( p1 - p2 ) + p3 - p0 ) * t * t * t );
				const float h = std::clamp( level, 0.f, 1.f ) * visual_h;
				return floor_y - ( progress_bar ? h : ( std::max )( 1.f, h ) );
			};

			const auto old_flags = draw_list->Flags;
			draw_list->Flags &= ~ImDrawListFlags_AntiAliasedFill;
			for ( float x = left; x < right; x += 1.f ) {
				const float x2 = ( std::min )( x + 1.f, right );
				draw_list->AddQuadFilled( ImVec2( x, curve_top( x ) ), ImVec2( x2, curve_top( x2 ) ), ImVec2( x2, floor_y ), ImVec2( x, floor_y ), color );
			}
			draw_list->Flags = old_flags;

			for ( float x = left; x < right; x += 1.f )
				draw_list->PathLineTo( ImVec2( x, curve_top( x ) ) );
			draw_list->PathLineTo( ImVec2( right, curve_top( right ) ) );
			draw_list->PathStroke( color, ImDrawFlags_None, 1.f );
		}

		for ( int bar = 0; bar < bars && !smooth; bar++ ) {
			const float level = n_audio_bands::bar_level( bands, static_cast< std::size_t >( bar ), static_cast< std::size_t >( bars ) );
			const float bar_h = level * visual_h;
			if ( progress_bar && bar_h < 1.f )
				continue;
			const float top = floor_y - ( std::max )( 1.f, bar_h );
			const float x  = std::floor( left + slot * static_cast< float >( bar ) );
			const float x2 = bar == bars - 1 ? right : std::floor( left + slot * static_cast< float >( bar + 1 ) ) - gap;
			draw_list->AddRectFilled( ImVec2( x, top ), ImVec2( x2, floor_y ), color );
		}
	}

	const bool left_side = align == 0;
	const float art_x    = left_side ? pos_x + padding : pos_x + width - padding - art_size;
	const float art_y    = pos_y + padding + ( content_h - art_size ) * 0.5f;

	if ( art )
		draw_list->AddImageRounded( art, ImVec2( art_x, art_y ), ImVec2( art_x + art_size, art_y + art_size ), ImVec2( 0.f, 0.f ), ImVec2( 1.f, 1.f ),
		                            IM_COL32_WHITE, 2.f );

	const float text_left  = art ? art_x + art_size + art_gap : pos_x + padding;
	const float text_right = art ? art_x - art_gap : pos_x + width - padding;
	const float text_top   = pos_y + padding + ( content_h - text_col_h ) * 0.5f;

	const ImVec2 title_pos( left_side ? text_left : text_right - title_size.x, text_top );
	const ImVec2 artist_pos( left_side ? text_left : text_right - artist_size.x, text_top + title_size.y + row_gap );

	put( draw_list, fx, title_font, title_px, title_pos, look.m_title, title );
	put( draw_list, fx, artist_font, artist_px, artist_pos, look.m_artist, artist );

	if ( progress_bar ) {
		const float bar_x     = pos_x + padding;
		const float bar_y     = pos_y + padding + content_h + row_gap;
		const float bar_width = pos_x + width - padding - bar_x;

		draw_list->AddRectFilled( ImVec2( bar_x, bar_y ), ImVec2( bar_x + bar_width, bar_y + bar_height ), IM_COL32( 60, 60, 60, 255 ), 1.f );
		draw_list->AddRectFilled( ImVec2( bar_x, bar_y ), ImVec2( bar_x + bar_width * g_media_player.get_progress( ), bar_y + bar_height ), accent_color,
		                          1.f );
	}

	ImRect bounds( ImVec2( pos_x, pos_y ), ImVec2( pos_x + width, pos_y + height ) );
	draw_lyrics( draw_list, from_bottom ? pos_y - row_gap : pos_y + height + row_gap, width, background, fx, bounds );

	if ( control_row ) {
		const float row_y = pos_y + height - padding - control_h;

		if ( time_w > 0.f ) {
			const float time_x = left_side ? pos_x + width - padding - time_w : pos_x + padding;
			put( draw_list, fx, artist_font, artist_px, ImVec2( std::floor( time_x ), std::floor( row_y + ( control_h - artist_size.y ) * 0.5f ) ), look.m_artist,
			     time_now );
		}

		if ( buttons )
			draw_buttons( draw_list, left_side ? pos_x + padding : pos_x + width - padding - buttons_w, std::floor( row_y + control_h * 0.5f ), fx, bounds );
	}

	g_render.end_stretch_block( block );
}

std::vector< n_misc::clantag_frame_t > n_misc::parse_clantag_frames( const std::string& value, float fallback_hold )
{
	std::vector< clantag_frame_t > frames;
	if ( value.empty( ) )
		return frames;

	std::string line;
	for ( const char c : value + '\n' ) {
		if ( c == '\r' )
			continue;
		if ( c != '\n' ) {
			line += c;
			continue;
		}

		clantag_frame_t frame;
		frame.m_hold = fallback_hold;

		const size_t tab = line.rfind( '\t' );
		if ( tab != std::string::npos ) {
			frame.m_hold = std::strtof( line.c_str( ) + tab + 1, nullptr );
			line.resize( tab );
		}

		/* hand-edited configs: 0 would spam the server, nan would never advance */
		if ( std::isnan( frame.m_hold ) )
			frame.m_hold = 1.f;
		frame.m_hold = std::clamp( frame.m_hold, 0.1f, 10.f );
		if ( line.length( ) > 24 )
			line.resize( 24 );

		frame.m_text = line;
		frames.push_back( frame );
		line.clear( );
	}

	return frames;
}

std::string n_misc::join_clantag_frames( const std::vector< clantag_frame_t >& frames )
{
	std::string joined;
	for ( size_t i = 0; i < frames.size( ); i++ ) {
		char hold[ 16 ];
		snprintf( hold, sizeof( hold ), "\t%.2f", frames[ i ].m_hold );

		if ( i )
			joined += '\n';
		joined += frames[ i ].m_text + hold;
	}

	return joined;
}

void n_misc::impl_t::update_clantag( )
{
	using set_clan_tag_t = void( __fastcall* )( const char*, const char* );

	static auto set_clan_tag = reinterpret_cast< set_clan_tag_t >( g_modules[ ENGINE_DLL ].find_pattern( "53 56 57 8B DA 8B F9 FF 15" ) );
	if ( !set_clan_tag )
		return;

	static std::string last_sent = "";
	static bool cleared          = true;

	if ( !GET_VARIABLE( g_variables.m_clantag, bool ) || GET_VARIABLE( g_variables.m_safe_mode, bool ) ) {
		if ( !cleared ) {
			/* same send rule as below: never emit CmdKeyValues while not on a team */
			if ( g_interfaces.m_engine_client->is_in_game( ) && g_ctx.m_local &&
			     ( g_ctx.m_local->get_team( ) == e_team_id::team_tt || g_ctx.m_local->get_team( ) == e_team_id::team_ct ) )
				set_clan_tag( "", "" );
			last_sent.clear( );
			cleared = true;
		}

		return;
	}

	if ( !g_interfaces.m_engine_client->is_connected( ) ) {
		last_sent.clear( );
		return;
	}

	if ( !g_interfaces.m_engine_client->is_in_game( ) || !g_ctx.m_local )
		return;

	const auto local_team = g_ctx.m_local->get_team( );
	if ( local_team != e_team_id::team_tt && local_team != e_team_id::team_ct ) {
		last_sent.clear( );
		return;
	}

	/* changelevel keeps is_connected true but the new server never got the tag */
	static std::string last_level = "";
	const char* level             = g_interfaces.m_engine_client->get_level_name( );
	if ( level && last_level != level ) {
		last_level = level;
		last_sent.clear( );
	}

	std::string base = GET_VARIABLE( g_variables.m_clantag_text, std::string );
	if ( base.empty( ) )
		base = "botox";
	else if ( base.length( ) > 24 )
		base.resize( 24 );

	const double time = GetTickCount64( ) / 1000.0;
	std::string tag;

	static int stage             = 0;
	static double last_step      = 0.0;
	static std::string last_base = base;

	if ( base != last_base ) {
		stage     = 0;
		last_step = 0.0;
		last_base = base;
	}

	const int anim = GET_VARIABLE( g_variables.m_clantag_animation, int );

	float speed = GET_VARIABLE( g_variables.m_clantag_speed, float );
	if ( speed < 0.1f )
		speed = 0.1f;

	const std::string song_title = anim == 4 && g_media_player.m_has_media ? g_utilities.truncate_utf8( g_media_player.get_title( ), 96 ) : std::string{ };

	if ( anim == 4 && !song_title.empty( ) ) {
		/* title, scroll left into artist, loop. each part waits 1 s once its end is shown.
		   server keeps 15 bytes ( MAX_CLAN_TAG_LENGTH 16 ), 1 goes to the trailing space below */
		constexpr int k_window_bytes = 14;

		const std::string artist = g_utilities.truncate_utf8( g_media_player.get_artist( ), 96 );

		std::vector< std::string > chars;
		std::vector< std::pair< int, int > > parts;
		const auto push = [ & ]( const std::string& text ) {
			for ( size_t i = 0; i < text.size( ); ) {
				size_t n = 1;
				while ( i + n < text.size( ) && ( static_cast< unsigned char >( text[ i + n ] ) & 0xC0 ) == 0x80 )
					n++;
				chars.push_back( static_cast< unsigned char >( text[ i ] ) < 0x20 ? " " : text.substr( i, n ) );
				i += n;
			}
		};
		const auto push_part = [ & ]( const std::string& text, const char* gap ) {
			const int start = static_cast< int >( chars.size( ) );
			push( text );
			parts.emplace_back( start, static_cast< int >( chars.size( ) ) );
			push( gap );
		};

		push_part( song_title, artist.empty( ) ? "   " : " - " );
		if ( !artist.empty( ) )
			push_part( artist, " - " );

		/* window no wider than the longest part: short title + artist never both on screen */
		const int count = static_cast< int >( chars.size( ) );
		int widest      = 0;
		for ( const auto& [ start, end ] : parts )
			widest = ( std::max )( widest, end - start );

		const auto window = [ & ]( const int at ) {
			int length = 0, bytes = 0;
			while ( length < widest && bytes + static_cast< int >( chars[ ( at + length ) % count ].size( ) ) <= k_window_bytes )
				bytes += static_cast< int >( chars[ ( at + length++ ) % count ].size( ) );
			return length;
		};

		static std::string song_key = "";
		static int song_stage       = 0;
		static double song_step     = 0.0;

		const std::string key = song_title + '\n' + artist;
		if ( key != song_key ) {
			song_key   = key;
			song_stage = 0;
			song_step  = time;
		}

		if ( artist.empty( ) && window( 0 ) >= parts[ 0 ].second )
			tag = song_title;
		else {
			song_stage %= count;

			bool pause = false;
			for ( const auto& [ start, end ] : parts ) {
				int at = start;
				while ( at + window( at ) < end )
					at++;
				pause |= at == song_stage;
			}

			if ( time - song_step >= speed + ( pause ? 1.0 : 0.0 ) ) {
				song_step  = time;
				song_stage = ( song_stage + 1 ) % count;
			}

			const int length = window( song_stage );
			for ( int i = 0; i < length; i++ )
				tag += chars[ ( song_stage + i ) % count ];
		}
	}
	else if ( anim == 4 )
		tag = base;
	else if ( anim == 3 ) {
		auto frames = n_misc::parse_clantag_frames( GET_VARIABLE( g_variables.m_clantag_frames, std::string ), speed );
		frames.erase( std::remove_if( frames.begin( ), frames.end( ), []( const n_misc::clantag_frame_t& frame ) { return frame.m_text.empty( ); } ),
		              frames.end( ) );

		if ( frames.empty( ) )
			tag = base;
		else {
			const int frame_count = static_cast< int >( frames.size( ) );

			stage %= frame_count;
			if ( time - last_step >= frames[ stage ].m_hold ) {
				last_step = time;
				stage     = ( stage + 1 ) % frame_count;
			}

			tag = frames[ stage ].m_text;
		}
	}
	else if ( anim == 2 ) {
		const std::string loop = base;
		const int loop_length  = static_cast< int >( loop.length( ) );

		stage %= loop_length;
		if ( time - last_step >= speed ) {
			last_step = time;
			stage     = ( stage + 1 ) % loop_length;
		}

		tag = loop.substr( stage ) + loop.substr( 0, stage );
	}
	else if ( anim == 1 ) {
		const int text_length      = static_cast< int >( base.length( ) );
		const int typing_end_stage = text_length - 1;
		const int pause_stage      = typing_end_stage + 1;
		const int delete_end_stage = pause_stage + text_length;
		const int empty_stage      = delete_end_stage + 1;
		const int total_stages     = empty_stage + 1;

		float hold = GET_VARIABLE( g_variables.m_clantag_hold, float );
		if ( hold < speed )
			hold = speed;

		const float delay = stage == pause_stage ? hold : speed;
		if ( time - last_step >= delay ) {
			last_step = time;

			if ( ++stage >= total_stages )
				stage = 0;
		}

		if ( stage <= typing_end_stage )
			tag = base.substr( 0, stage + 1 );
		else if ( stage == pause_stage )
			tag = base;
		else if ( stage <= delete_end_stage ) {
			const int deleted = stage - pause_stage;
			if ( deleted < text_length )
				tag = base.substr( deleted );
		}
	}
	else
		tag = base;

	if ( !tag.empty( ) )
		tag += ' ';

	if ( tag == last_sent || tag.length( ) > 32 )
		return;

	set_clan_tag( tag.c_str( ), tag.c_str( ) );
	last_sent = tag;
	cleared   = false;
}

void n_misc::impl_t::on_end_scene( )
{
	this->draw_watermark( );
	this->draw_watermark_gif( );

	points_draw( );

	this->draw_media_player( );

	if ( !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) || g_ctx.m_local->get_observer_mode( ) == 1  )
		return;

	fake_pov_draw( );

	[ & ]( const bool can_draw_practice_window ) {
		if ( !can_draw_practice_window || !GET_VARIABLE( g_variables.m_practice_window_show, bool ) )
			return;

		if ( !g_ctx.m_local->is_alive( ) )
			return;

		constexpr auto background_height = 25.f;
		constexpr auto title_text        = "practice";
		const auto title_text_size       = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ]->CalcTextSizeA(
            g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ]->FontSize, FLT_MAX, 0.f, title_text );

		ImGui::SetNextWindowSizeConstraints( ImVec2( title_text_size.x + 25.f, title_text_size.y + 5.f ), ImVec2( FLT_MAX, FLT_MAX ) );
		ImGui::Begin( ( "botox-practice-window-ui" ), 0,
		              ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar |
		                  ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_::ImGuiWindowFlags_AlwaysAutoResize );
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

				RenderFadedGradientLine( draw_list, ImVec2( position.x, position.y + background_height - 1.f ), ImVec2( size.x, 1.f ),
				                         static_cast< ImColor >( ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent ) ) );

				draw_list->AddText(
					g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ], g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ]->FontSize,
					ImVec2( position.x + ( ( size.x - title_text_size.x ) / 2.f ), position.y + ( ( background_height - title_text_size.y ) / 2.f ) ),
					ImColor( 1.f, 1.f, 1.f ), title_text );

				ImGui::PushClipRect( ImVec2( position.x + 1.f, position.y + 1.f ), ImVec2( position.x + size.x - 1.f, position.y + size.y - 1.f ),
				                     false );
				draw_list->AddRect( ImVec2( position.x + 1.f, position.y + 1.f ), ImVec2( position.x + size.x - 1.f, position.y + size.y - 1.f ),
				                    ImColor( 50 / 255.f, 50 / 255.f, 50 / 255.f ), ImGui::GetStyle( ).WindowRounding );
				ImGui::PopClipRect( );
			}( );

			ImGui::SetCursorPosY( 30.f );

			if ( !g_convars[ HASH_BT( "sv_cheats" ) ]->get_bool( ) ) {
				ImGui::Text( "sv_cheats is not enabled" );
			} else {
				const auto cp_key = GET_VARIABLE( g_variables.m_practice_cp_key, key_bind_t ).m_key;

				ImGui::Text( std::format( "{} - checkpoint", cp_key != 0 ? FILTERED_KEY_NAMES[ cp_key ] : "none" ).c_str( ) );

				const auto tp_key = GET_VARIABLE( g_variables.m_practice_tp_key, key_bind_t ).m_key;

				ImGui::Text( std::format( "{} - teleport", tp_key != 0 ? FILTERED_KEY_NAMES[ tp_key ] : "none" ).c_str( ) );
			}
		}
		ImGui::End( );
	}( GET_VARIABLE( g_variables.m_practice_window, bool ) );

	[ & ]( bool can_draw_spectator_list, int style, int type ) {
		if ( !can_draw_spectator_list )
			return;

		switch ( style ) {
		case 0:
			if ( type == 0 )
				draw_spectator_list( );
			break;
		case 1:
			draw_spectator_list_delusional( );
			break;
		case 2:
			draw_spectator_list_interwebz( );
			break;
		case 3:
			draw_spectator_list_winxp( );
			break;
		case 4:
			draw_spectator_list_clarity( );
			break;
		case 5:
			draw_spectator_list_chillware( );
			break;
		case 6:
			draw_spectator_list_lobotomy( );
			break;
		case 7:
			draw_spectator_list_kamibebra( );
			break;
		case 8:
			draw_spectator_list_eyes( );
			break;
		case 9:
			draw_spectator_list_cumhacck( );
			break;
		case 10:
			draw_spectator_list_clarity_v2( );
			break;
		case 11:
			draw_spectator_list_airplane( );
			break;
		case 12:
			draw_spectator_list_bhopcheat( );
			break;
		case 13:
			draw_spectator_list_millionware( );
			break;
		case 14:
			draw_spectator_list_aimware( );
			break;
		case 15:
			draw_spectator_list_havoc( );
			break;
		case 16:
			draw_spectator_list_onetap( );
			break;
		case 17:
			draw_spectator_list_airflow( );
			break;
		case 18:
			draw_spectator_list_evolve( );
			break;
		case 19:
			draw_spectator_list_legendware( );
			break;
		case 20:
			draw_spectator_list_interium( );
			break;
		case 21:
			draw_spectator_list_chillware_v2( );
			break;
		case 22:
			draw_spectator_list_skebob( );
			break;
		case 23:
			draw_spectator_list_cumidere( );
			break;
		case 24:
			draw_spectator_list_illusory( );
			break;
		case 25:
			draw_spectator_list_lumi( );
			break;
		case 26:
			draw_spectator_list_billware( );
			break;
		case 27:
			draw_spectator_list_inkabanium( );
			break;
		case 28:
			draw_spectator_list_dna( );
			break;
		case 29:
			draw_spectator_list_cucumber( );
			break;
		case 30:
			draw_spectator_list_howeweware( );
			break;
		default:
			break;
		}
	}( GET_VARIABLE( g_variables.m_spectators_list, bool ), GET_VARIABLE( g_variables.m_spectators_list_style, int ),
	   GET_VARIABLE( g_variables.m_spectators_list_type, int ) );
}

void n_misc::impl_t::practice_window_think( )
{
	if ( !GET_VARIABLE( g_variables.m_practice_window, bool ) )
		return;

	if ( !g_convars[ HASH_BT( "sv_cheats" ) ]->get_bool( ) || g_interfaces.m_engine_client->is_console_visible( ) )
		return;

	const auto cp_key = GET_VARIABLE( g_variables.m_practice_cp_key, key_bind_t ).m_key;

	const auto tp_key = GET_VARIABLE( g_variables.m_practice_tp_key, key_bind_t ).m_key;

	if ( g_input.is_key_released( cp_key ) ) {
		if ( !( g_ctx.m_local->get_flags( ) & fl_onground ) ) {
			g_logger.print( "you need to be on ground to set a checkpoint.", "[practice]" );
			return;
		}

		c_angle saved_angles = { };
		g_interfaces.m_engine_client->get_view_angles( saved_angles );

		g_misc.practice.saved_angles   = saved_angles;
		g_misc.practice.saved_position = g_ctx.m_local->get_abs_origin( );

		g_logger.print( "saved checkpoint.", "[practice]" );
	} else if ( g_input.is_key_released( tp_key ) ) {
		if ( g_misc.practice.saved_angles.is_zero( ) || g_misc.practice.saved_position.is_zero( ) )
			return;

		g_interfaces.m_engine_client->client_cmd_unrestricted(
			std::string( "setpos " )
				.append( std::vformat( "{} {} {}", std::make_format_args( g_misc.practice.saved_position.m_x, g_misc.practice.saved_position.m_y,
		                                                                  g_misc.practice.saved_position.m_z ) ) )
				.append( ";setang " )
				.append( std::vformat( "{} {} {}", std::make_format_args( g_misc.practice.saved_angles.m_x, g_misc.practice.saved_angles.m_y,
		                                                                  g_misc.practice.saved_angles.m_z ) ) )
				/* setpos never touches velocity server side; vscript SetAbsVelocity on the issuing player does */
				.append( ";ent_fire !activator RunScriptCode \"self.SetVelocity(Vector(0,0,0))\"" )
				.c_str( ) );
	}
}

void n_misc::impl_t::disable_post_processing( )
{
	static auto post_process = g_convars[ HASH_BT( "mat_postprocess_enable" ) ];
	if ( !post_process )
		return;

	const bool want = !GET_VARIABLE( g_variables.m_disable_post_processing, bool ) || GET_VARIABLE( g_variables.m_safe_mode, bool );

	if ( post_process->get_bool( ) != want )
		post_process->set_value( want );
}

void n_misc::impl_t::remove_panorama_blur( )
{
	static auto disable_blur = g_convars[ HASH_BT( "@panorama_disable_blur" ) ];
	if ( !disable_blur )
		return;

	const bool want = GET_VARIABLE( g_variables.m_remove_panorama_blur, bool );

	if ( disable_blur->get_bool( ) != want )
		disable_blur->set_value( want );
}

void n_misc::impl_t::remove_smoke( )
{
	static unsigned char* smoke_count_write = g_modules[ CLIENT_DLL ].find_pattern( "A3 ? ? ? ? 57 8B CB" );

	static const char* vistasmoke_wireframe[] = {
		"particle/vistasmokev1/vistasmokev1_smokegrenade",
	};

	static const char* vistasmoke_nodraw[] = {
		"particle/vistasmokev1/vistasmokev1_fire",
		"particle/vistasmokev1/vistasmokev1_emods",
		"particle/vistasmokev1/vistasmokev1_emods_impactdust",
	};

	static bool set = false;

	const auto flag_material = []( const char* name, e_material_var_flags flag, bool value ) {
		c_material* material = g_interfaces.m_material_system->find_material( name, TEXTURE_GROUP_OTHER );
		if ( !material || material->is_error_material( ) )
			return;

		material->set_material_var_flag( flag, value );
	};

	if ( !GET_VARIABLE( g_variables.m_remove_smoke, bool ) ) {
		if ( set ) {
			for ( const auto name : vistasmoke_wireframe ) {
				flag_material( name, material_var_wireframe, false );
				flag_material( name, material_var_no_draw, false );
			}

			for ( const auto name : vistasmoke_nodraw ) {
				flag_material( name, material_var_wireframe, false );
				flag_material( name, material_var_no_draw, false );
			}

			set = false;
		}

		return;
	}

	set = true;

	const bool wireframe = GET_VARIABLE( g_variables.m_remove_smoke_mode, int ) == 0;

	for ( const auto name : vistasmoke_wireframe ) {
		flag_material( name, material_var_wireframe, wireframe );
		flag_material( name, material_var_no_draw, !wireframe );
	}

	for ( const auto name : vistasmoke_nodraw ) {
		flag_material( name, material_var_wireframe, wireframe );
		flag_material( name, material_var_no_draw, !wireframe );
	}

	if ( smoke_count_write ) {
		static auto smoke_count = *reinterpret_cast< std::uintptr_t* >( smoke_count_write + 0x1 );

		if ( smoke_count )
			*reinterpret_cast< int* >( smoke_count ) = 0;
	}
}

void n_misc::impl_t::remove_flash( )
{
	if ( !GET_VARIABLE( g_variables.m_remove_flash, bool ) )
		return;

	if ( !g_ctx.m_local )
		return;

	const float max_alpha = 255.f * std::clamp( GET_VARIABLE( g_variables.m_remove_flash_opacity, float ), 0.f, 100.f ) * 0.01f;

	g_ctx.m_local->get_flash_max_alpha( ) = max_alpha;

	const auto spectated = reinterpret_cast< c_base_entity* >(
		g_interfaces.m_client_entity_list->get_client_entity_from_handle( g_ctx.m_local->get_observer_target_handle( ) ) );

	if ( spectated && spectated != g_ctx.m_local )
		spectated->get_flash_max_alpha( ) = max_alpha;
}

namespace {
	enum e_old_shader_group : int {
		old_shader_group_none    = 0,
		old_shader_group_weapons = 1,
		old_shader_group_arms    = 2,
		old_shader_group_players = 4,
		old_shader_group_world   = 8,
		old_shader_group_props   = 0x10,
	};

	enum e_material_var_type : int {
		material_var_type_float  = 0,
		material_var_type_string = 1,
		material_var_type_vector = 2,
		material_var_type_int    = 4,
	};

	constexpr const char* old_shader_vars[] = {
		"$phong",
		"$phongboost",
		"$phongexponent",
		"$phongfresnelranges",
		"$envmaptint",
		"$envmaplightscale",
		"$envmapcontrast",
		"$envmapsaturation",
		"$envmapfresnel",
		"$envmapfresnelminmaxexp",
		"$rimlightexponent",
		"$rimlightboost",
		"$rimlight",
		"$ambientreflectionboost",
		"$fakerimboost",
	};

	constexpr e_material_var_flags old_shader_flags[] = {
		material_var_envmapsphere,
		material_var_envmapcameraspace,
		material_var_basealphaenvmapmask,
		material_var_normalmapalphaenvmapmask,
	};

	constexpr int old_shader_var_count  = static_cast< int >( std::size( old_shader_vars ) );
	constexpr int old_shader_flag_count = static_cast< int >( std::size( old_shader_flags ) );

	/* IMaterialVar raw: m_intVal +0x8, m_VecVal +0xC, byte +0x1C = type (low nibble) | comps (bits 4-6),
	   getter slots are msvc overload-reversed; a raw read never precaches. */
	int var_type( c_material_var* var )
	{
		return *reinterpret_cast< unsigned char* >( reinterpret_cast< uintptr_t >( var ) + 0x1C ) & 0x0F;
	}

	int var_components( c_material_var* var )
	{
		return ( *reinterpret_cast< unsigned char* >( reinterpret_cast< uintptr_t >( var ) + 0x1C ) >> 4 ) & 0x07;
	}

	int var_int( c_material_var* var )
	{
		return *reinterpret_cast< int* >( reinterpret_cast< uintptr_t >( var ) + 0x8 );
	}

	void var_vec( c_material_var* var, float* out )
	{
		std::memcpy( out, reinterpret_cast< const void* >( reinterpret_cast< uintptr_t >( var ) + 0xC ), 4 * sizeof( float ) );
	}

	/* saved by VALUE + type, never a var pointer. m_captured per var: a param can appear a pass or two
	   after the material does (shader init runs later) */
	struct saved_var_t {
		float m_vec[ 4 ];
		int m_int;
		int m_type;
		bool m_captured;
	};

	struct saved_material_t {
		saved_var_t m_vars[ old_shader_var_count ];
		bool m_flags[ old_shader_flag_count ];
		bool m_flags_captured;
		bool m_flags_dirty;
		unsigned int m_name_hash;
		float m_next_check;
	};

	unsigned int material_name_hash( const char* name )
	{
		unsigned int hash = 0x811c9dc5u;
		for ( ; name && *name; name++ ) {
			hash ^= static_cast< unsigned char >( *name );
			hash *= 0x01000193u;
		}

		return hash;
	}

	std::unordered_map< c_material*, saved_material_t > old_shader_saved;
	int old_shader_last_mask   = old_shader_group_none;
	float old_shader_next_walk = 0.f;

	enum e_modulation_group : int {
		modulation_group_none  = -1,
		modulation_group_world = 0,
		modulation_group_props = 1,
		modulation_group_sky   = 2,
		modulation_group_max   = 3,
	};

	struct modulated_material_t {
		float m_original[ 3 ];
		float m_applied[ 3 ];
		bool m_has_applied;
		unsigned int m_name_hash;
		int m_group;
	};

	int classify_modulation( const char* group, const char* name )
	{
		if ( group ) {
			if ( std::strcmp( group, "SkyBox textures" ) == 0 )
				return modulation_group_sky;

			if ( std::strcmp( group, "World textures" ) == 0 )
				return modulation_group_world;

			if ( std::strcmp( group, "StaticProp textures" ) == 0 )
				return modulation_group_props;
		}

		if ( !name )
			return modulation_group_none;

		if ( std::strstr( name, "cs_custom_material" ) )
			return modulation_group_none;

		if ( std::strstr( name, "models/player" ) || std::strstr( name, "models/weapons" ) || std::strstr( name, "weapons/" ) ||
		     std::strstr( name, "models/inventory_items" ) || std::strstr( name, "arms" ) || std::strstr( name, "glove" ) ||
		     std::strstr( name, "sleeve" ) )
			return modulation_group_none;

		if ( std::strstr( name, "models/" ) )
			return modulation_group_props;

		return modulation_group_none;
	}

	c_material_var* modulation_color2( c_material* material )
	{
		bool found = false;
		auto* var  = material->find_var( "$color2", &found, false );

		return found ? var : nullptr;
	}

	bool write_modulation( c_material* material, int group, const float* color )
	{
		if ( group == modulation_group_sky ) {
			material->color_modulate( color[ 0 ], color[ 1 ], color[ 2 ] );
			return false;
		}

		auto* var = modulation_color2( material );
		if ( !var )
			return false;

		var->set_vector( color[ 0 ], color[ 1 ], color[ 2 ] );
		return true;
	}

	bool same_modulation( const float* a, const float* b )
	{
		return std::fabs( a[ 0 ] - b[ 0 ] ) < 0.002f && std::fabs( a[ 1 ] - b[ 1 ] ) < 0.002f && std::fabs( a[ 2 ] - b[ 2 ] ) < 0.002f;
	}

	bool apply_modulation( c_material* material, modulated_material_t& entry, const float* color, int& rebuild_budget )
	{
		if ( entry.m_has_applied && same_modulation( entry.m_applied, color ) )
			return true;

		const bool rebuilds = entry.m_group != modulation_group_sky;

		if ( rebuilds && rebuild_budget <= 0 )
			return false;

		if ( write_modulation( material, entry.m_group, color ) ) {
			material->recompute_state_snapshots( );
			rebuild_budget--;
		}

		std::memcpy( entry.m_applied, color, sizeof( entry.m_applied ) );
		entry.m_has_applied = true;

		return true;
	}

	/* false = this material can not carry a tint at all, so it is never tracked and never written */
	bool read_modulation( c_material* material, int group, float* out )
	{
		if ( group == modulation_group_sky ) {
			material->get_color_modulation( &out[ 0 ], &out[ 1 ], &out[ 2 ] );
			return true;
		}

		auto* var = modulation_color2( material );
		if ( !var )
			return false;

		if ( var_type( var ) != material_var_type_vector ) {
			out[ 0 ] = out[ 1 ] = out[ 2 ] = 1.f;
			return true;
		}

		float vec[ 4 ] = { };
		var_vec( var, vec );

		out[ 0 ] = vec[ 0 ];
		out[ 1 ] = vec[ 1 ];
		out[ 2 ] = vec[ 2 ];

		return true;
	}

	std::unordered_map< c_material*, modulated_material_t > world_modulation_saved;
	int world_modulation_last_mask   = 0;
	float world_modulation_next_walk = 0.f;

	int world_modulation_debug_walks = 0;

	void restore_modulated_materials( int keep_mask = 0, bool forget = true )
	{
		auto* material_system = g_interfaces.m_material_system;

		int restored = 0;

		if ( material_system && !world_modulation_saved.empty( ) ) {
			const unsigned short invalid = material_system->invalid_material( );
			for ( unsigned short handle = material_system->first_material( ); handle != invalid;
			      handle                = material_system->next_material( handle ) ) {
				c_material* material = material_system->get_material( handle );
				if ( !material )
					continue;

				const auto it = world_modulation_saved.find( material );
				if ( it == world_modulation_saved.end( ) )
					continue;

				if ( material->shader_param_count( ) <= 0 )
					continue;

				if ( it->second.m_name_hash != material_name_hash( material->get_name( ) ) )
					continue;

				if ( !it->second.m_has_applied || ( ( 1 << it->second.m_group ) & keep_mask ) )
					continue;

				if ( write_modulation( material, it->second.m_group, it->second.m_original ) )
					material->recompute_state_snapshots( );

				it->second.m_has_applied = false;
				restored++;
			}

			g_console.print( std::format( "[modulation] restore | keep {} | tracked {} | written back {}", keep_mask,
			                              world_modulation_saved.size( ), restored )
			                     .c_str( ) );
		}

		if ( forget )
			world_modulation_saved.clear( );
	}

	bool var_is_restorable( int type, int components )
	{
		return type == material_var_type_int || type == material_var_type_float ||
		       ( type == material_var_type_vector && components == 3 );
	}

	/* true only when it wrote - an already-zero var must not cost a snapshot rebuild */
	bool strip_var( c_material_var* var, saved_var_t& saved )
	{
		const int type = var_type( var );
		if ( !var_is_restorable( type, var_components( var ) ) )
			return false;

		if ( !saved.m_captured ) {
			saved.m_type = type;
			saved.m_int  = var_int( var );
			var_vec( var, saved.m_vec );
			saved.m_captured = true;
		}
		/* re-typed under us (game rebuilt the var): leave it, never zero one shape and restore another */
		else if ( saved.m_type != type )
			return false;

		if ( type == material_var_type_int ) {
			if ( var_int( var ) == 0 )
				return false;

			var->set_int( 0 );
			return true;
		}

		float current[ 4 ]{ };
		var_vec( var, current );

		if ( type == material_var_type_float ) {
			if ( current[ 0 ] == 0.f )
				return false;

			var->set_float( 0.f );
			return true;
		}

		if ( current[ 0 ] == 0.f && current[ 1 ] == 0.f && current[ 2 ] == 0.f )
			return false;

		var->set_vector( 0.f, 0.f, 0.f );
		return true;
	}

	void restore_var( c_material_var* var, const saved_var_t& saved )
	{
		switch ( saved.m_type ) {
		case material_var_type_int:
			var->set_int( saved.m_int );
			break;
		case material_var_type_vector:
			var->set_vector( saved.m_vec[ 0 ], saved.m_vec[ 1 ], saved.m_vec[ 2 ] );
			break;
		default:
			var->set_float( saved.m_vec[ 0 ] );
			break;
		}
	}

	/* name first, texture group as fallback. order matters: arms / glove / sleeve
	   before plain weapon, "models/" last. */
	int classify_material( const char* name, const char* group, const char* shader )
	{
		if ( !name )
			return old_shader_group_none;

		if ( std::strstr( name, "models/player" ) )
			return old_shader_group_players;

		if ( std::strstr( name, "cs_custom_material" ) ) {
			if ( shader && std::strstr( shader, "Weapon" ) )
				return old_shader_group_weapons;

			return old_shader_group_players;
		}

		const bool weapon_path = std::strstr( name, "models/weapons" ) || std::strstr( name, "weapons/" );

		if ( weapon_path && ( std::strstr( name, "arms" ) || std::strstr( name, "glove" ) || std::strstr( name, "sleeve" ) ) )
			return old_shader_group_arms;

		if ( weapon_path || std::strstr( name, "knife" ) || std::strstr( name, "bayonet" ) || std::strstr( name, "karam" ) ||
		     std::strstr( name, "butterfly" ) || std::strstr( name, "falchion" ) || std::strstr( name, "stiletto" ) ||
		     std::strstr( name, "ursus" ) || std::strstr( name, "widowmaker" ) )
			return old_shader_group_weapons;

		if ( std::strstr( name, "models/" ) )
			return old_shader_group_props;

		if ( !group )
			return old_shader_group_none;

		if ( std::strcmp( group, "World textures" ) == 0 )
			return old_shader_group_world;

		if ( std::strcmp( group, "StaticProp textures" ) == 0 )
			return old_shader_group_props;

		return old_shader_group_none;
	}

	/* put everything back and forget it. walks the dict and only touches entries still in
	   it, so destroyed materials are never followed. no refresh( ) (rule 2). */
	void restore_tracked_materials( )
	{
		auto* material_system = g_interfaces.m_material_system;

		if ( material_system && !old_shader_saved.empty( ) ) {
			const unsigned short invalid = material_system->invalid_material( );
			for ( unsigned short handle = material_system->first_material( ); handle != invalid;
			      handle                = material_system->next_material( handle ) ) {
				c_material* material = material_system->get_material( handle );
				if ( !material )
					continue;

				const auto it = old_shader_saved.find( material );
				if ( it == old_shader_saved.end( ) )
					continue;

				if ( material->shader_param_count( ) <= 0 )
					continue;

				const saved_material_t& saved = it->second;

				/* recycled address, different material - these values were never written to it */
				if ( saved.m_name_hash != material_name_hash( material->get_name( ) ) )
					continue;

				bool restored = false;

				for ( int i = 0; i < old_shader_var_count; i++ ) {
					if ( !saved.m_vars[ i ].m_captured )
						continue;

					bool found         = false;
					auto* material_var = material->find_var( old_shader_vars[ i ], &found, false );
					if ( !found || !material_var )
						continue;

					if ( var_type( material_var ) != saved.m_vars[ i ].m_type )
						continue;

					restore_var( material_var, saved.m_vars[ i ] );
					restored = true;
				}

				if ( saved.m_flags_captured && saved.m_flags_dirty ) {
					for ( int i = 0; i < old_shader_flag_count; i++ )
						material->set_material_var_flag( old_shader_flags[ i ], saved.m_flags[ i ] );

					restored = true;
				}

				if ( restored )
					material->recompute_state_snapshots( );
			}
		}

		old_shader_saved.clear( );
	}
}

void n_misc::impl_t::old_shaders( )
{
	auto* material_system = g_interfaces.m_material_system;
	if ( !material_system )
		return;

	auto* engine = g_interfaces.m_engine_client;
	if ( engine && engine->is_connected( ) && !engine->is_in_game( ) )
		return;

	int mask = old_shader_group_none;
	if ( GET_VARIABLE( g_variables.m_old_shaders, bool ) ) {
		if ( GET_VARIABLE( g_variables.m_old_shaders_weapons, bool ) )
			mask |= old_shader_group_weapons;
		if ( GET_VARIABLE( g_variables.m_old_shaders_arms, bool ) )
			mask |= old_shader_group_arms;
		if ( GET_VARIABLE( g_variables.m_old_shaders_players, bool ) )
			mask |= old_shader_group_players;
		if ( GET_VARIABLE( g_variables.m_old_shaders_world, bool ) )
			mask |= old_shader_group_world;
		if ( GET_VARIABLE( g_variables.m_old_shaders_props, bool ) )
			mask |= old_shader_group_props;
	}

	if ( mask != old_shader_last_mask ) {
		restore_tracked_materials( );
		old_shader_next_walk = 0.f;
	}

	old_shader_last_mask = mask;

	if ( mask == old_shader_group_none )
		return;

	const float now = g_interfaces.m_global_vars_base->m_real_time;
	if ( now < old_shader_next_walk && now >= old_shader_next_walk - 1.f )
		return;
	old_shader_next_walk = now + 1.f;

	const unsigned short invalid = material_system->invalid_material( );
	for ( unsigned short handle = material_system->first_material( ); handle != invalid; handle = material_system->next_material( handle ) ) {
		c_material* material = material_system->get_material( handle );
		if ( !material || material->is_error_material( ) )
			continue;

		const char* name = material->get_name( );
		if ( !name )
			continue;

		/* never touch map materials */
		if ( std::strncmp( name, "maps/", 5 ) == 0 )
			continue;

		if ( material->shader_param_count( ) <= 0 )
			continue;

		if ( !( classify_material( name, material->get_texture_group_name( ), material->get_shader_name( ) ) & mask ) )
			continue;

		const unsigned int name_hash = material_name_hash( name );

		auto it          = old_shader_saved.find( material );
		bool first_touch = it == old_shader_saved.end( );
		if ( first_touch )
			it = old_shader_saved.emplace( material, saved_material_t{ } ).first;
		else if ( it->second.m_name_hash != name_hash ) {
			it->second  = saved_material_t{ };
			first_touch = true;
		}

		saved_material_t& saved = it->second;
		saved.m_name_hash       = name_hash;

		/* stripped materials are stable (game never writes these back): re-verify on a slow beat, spend the
		   fast pass on new / unfinished ones */
		if ( !first_touch && now < saved.m_next_check )
			continue;

		bool touched = false;

		for ( int i = 0; i < old_shader_var_count; i++ ) {
			bool found         = false;
			auto* material_var = material->find_var( old_shader_vars[ i ], &found, false );

			if ( !found || !material_var )
				continue;

			if ( strip_var( material_var, saved.m_vars[ i ] ) )
				touched = true;
		}

		if ( !saved.m_flags_captured ) {
			for ( int i = 0; i < old_shader_flag_count; i++ ) {
				saved.m_flags[ i ] = material->get_material_var_flag( old_shader_flags[ i ] );
				if ( saved.m_flags[ i ] ) {
					material->set_material_var_flag( old_shader_flags[ i ], false );
					saved.m_flags_dirty = true;
				}
			}

			saved.m_flags_captured = true;
			touched                = saved.m_flags_dirty || touched;
		}

		if ( touched )
			material->recompute_state_snapshots( );

		saved.m_next_check = touched ? 0.f : now + 5.f;
	}
}

void n_misc::impl_t::world_modulation( )
{
	auto* material_system = g_interfaces.m_material_system;
	if ( !material_system )
		return;

	auto* engine = g_interfaces.m_engine_client;
	if ( engine && engine->is_connected( ) && !engine->is_in_game( ) )
		return;

	const std::uint32_t enable_variables[ modulation_group_max ]     = { g_variables.m_world_modulation, g_variables.m_prop_modulation,
		                                                                g_variables.m_skybox_modulation };
	const std::uint32_t color_variables[ modulation_group_max ]      = { g_variables.m_world_modulation_color, g_variables.m_prop_modulation_color,
		                                                                g_variables.m_skybox_modulation_color };
	const std::uint32_t brightness_variables[ modulation_group_max ] = { g_variables.m_world_modulation_brightness,
		                                                                 g_variables.m_prop_modulation_brightness,
		                                                                 g_variables.m_skybox_modulation_brightness };

	int mask = 0;
	for ( int group = 0; group < modulation_group_max; group++ ) {
		if ( GET_VARIABLE( enable_variables[ group ], bool ) )
			mask |= 1 << group;
	}

	/* a group leaving the mask gets its colour back; originals never re-read while tracked
	   ( see restore_modulated_materials ) */
	if ( mask != world_modulation_last_mask ) {
		restore_modulated_materials( mask, false );
		world_modulation_next_walk   = 0.f;
		world_modulation_debug_walks = mask != 0 ? 2 : 0;
	}

	world_modulation_last_mask = mask;

	if ( mask == 0 )
		return;

	const float now = g_interfaces.m_global_vars_base->m_real_time;

	float wanted[ modulation_group_max ][ 3 ] = { };
	for ( int group = 0; group < modulation_group_max; group++ ) {
		const c_color color    = GET_VARIABLE( color_variables[ group ], c_color );
		const float brightness = std::clamp( GET_VARIABLE( brightness_variables[ group ], float ) * 0.01f, 0.f, 4.f );

		wanted[ group ][ 0 ] = color.base< color_type_r >( ) * brightness;
		wanted[ group ][ 1 ] = color.base< color_type_g >( ) * brightness;
		wanted[ group ][ 2 ] = color.base< color_type_b >( ) * brightness;
	}

	static float last_written[ modulation_group_max ][ 3 ] = { };

	const bool colour_changed = std::memcmp( last_written, wanted, sizeof( wanted ) ) != 0;

	if ( colour_changed )
		std::memcpy( last_written, wanted, sizeof( wanted ) );

	const bool walk = colour_changed || now >= world_modulation_next_walk || now < world_modulation_next_walk - 1.f;

	if ( !walk )
		return;

	int rebuild_budget = 64;
	bool deferred      = false;

	const bool debug = world_modulation_debug_walks > 0;
	int matched[ modulation_group_max ] = { };
	int reverted                        = 0;
	int missed_models                   = 0;
	std::string missed_sample;

	const unsigned short invalid = material_system->invalid_material( );
	for ( unsigned short handle = material_system->first_material( ); handle != invalid; handle = material_system->next_material( handle ) ) {
		c_material* material = material_system->get_material( handle );
		if ( !material || material->is_error_material( ) )
			continue;

		if ( material->shader_param_count( ) <= 0 )
			continue;

		const char* name          = material->get_name( );
		const char* texture_group = material->get_texture_group_name( );
		const int group           = classify_modulation( texture_group, name );

		if ( group == modulation_group_none || !( ( 1 << group ) & mask ) ) {
			if ( debug && name && std::strstr( name, "models/" ) && group == modulation_group_none ) {
				missed_models++;
				if ( missed_models <= 4 )
					missed_sample.append( std::format( " [{} @ {}]", name, texture_group ? texture_group : "?" ) );
			}

			continue;
		}

		const unsigned int name_hash = material_name_hash( name );

		auto it = world_modulation_saved.find( material );
		if ( it == world_modulation_saved.end( ) || it->second.m_name_hash != name_hash ) {
			modulated_material_t entry{ };
			entry.m_name_hash = name_hash;
			entry.m_group     = group;

			if ( !read_modulation( material, group, entry.m_original ) )
				continue;

			it = world_modulation_saved.insert_or_assign( material, entry ).first;
		}
		else if ( debug ) {
			float live[ 3 ] = { };
			read_modulation( material, group, live );

			const float* want = wanted[ group ];
			if ( std::fabs( live[ 0 ] - want[ 0 ] ) > 0.004f || std::fabs( live[ 1 ] - want[ 1 ] ) > 0.004f ||
			     std::fabs( live[ 2 ] - want[ 2 ] ) > 0.004f )
				reverted++;
		}

		if ( debug )
			matched[ group ]++;

		if ( !apply_modulation( material, it->second, wanted[ group ], rebuild_budget ) )
			deferred = true;
	}

	world_modulation_next_walk = deferred ? 0.f : now + 1.f;

	if ( debug ) {
		world_modulation_debug_walks--;

		g_console.print( std::format( "[modulation] mask {} | world {} props {} sky {} | prop rgb {:.2f} {:.2f} {:.2f} | reverted {} | "
		                              "models missed {}{}",
		                              mask, matched[ modulation_group_world ], matched[ modulation_group_props ],
		                              matched[ modulation_group_sky ], wanted[ modulation_group_props ][ 0 ],
		                              wanted[ modulation_group_props ][ 1 ], wanted[ modulation_group_props ][ 2 ], reverted,
		                              missed_models, missed_sample )
		                     .c_str( ) );
	}
}

namespace
{
	constexpr const char* world_texture_skip[] = { "decal",  "overlay", "tools/",  "sign",    "poster", "glass",  "window",    "graffiti",
		                                           "number", "marking", "stripe",  "water/",  "/water", "_water", "liquid",    "skybox",
		                                           "effects/", "vgui/", "sprite",  "fence",   "grate",  "lights/", "chainlink", "foliage",
		                                           "vines" };

	constexpr const char* world_texture_roofs[] = { "ceiling", "roof", "shingle", "awning" };
	constexpr const char* world_texture_metal[] = { "metal", "corrugated", "ibeam", "steel", "rollupdoor", "girder", "vent", "pipe", "rust",
		                                            "tin_", "aluminum" };
	constexpr const char* world_texture_wood[]  = { "wood", "plank", "plywood", "crate", "lumber", "pallet", "bark", "siding" };
	constexpr const char* world_texture_ground_veto[] = { "wall", "wll", "sandstone", "brick", "sandbag" };
	constexpr const char* world_texture_ground[] = { "grass", "dirt", "mud",    "sand",    "gravel", "snow", "ice0", "beach",
		                                             "moss",  "ground", "nature/", "soil", "hay",    "farm" };
	constexpr const char* world_texture_floors_veto[] = { "wall", "wll" };
	constexpr const char* world_texture_floors[] = { "floor",   "flr",    "tile",   "asphalt", "road",  "sidewalk",    "blacktop", "street",
		                                             "pavement", "cobble", "cbble",  "carpet",  "parking", "stair",     "step",     "curb",
		                                             "pebble",  "herringbone", "marble", "tarmac", "highway" };
	constexpr const char* world_texture_walls[] = { "wall",  "wll",    "plaster", "brick",    "conc",       "cement",     "cinder", "stone",
		                                            "stucco", "adobe", "rock",    "cliff",    "trim",       "pillar",     "column", "building",
		                                            "facade", "hieroglyph", "heiroglyph", "door", "base" };

	struct world_texture_rule_t {
		int m_category;
		std::span< const char* const > m_veto;
		std::span< const char* const > m_match;
	};

	/* first hit wins: roofs before metal ("metalroof"), floors veto "wall" ("tilewall") */
	constexpr world_texture_rule_t world_texture_rules[] = {
		{ 5, { }, world_texture_roofs },
		{ 4, { }, world_texture_metal },
		{ 3, { }, world_texture_wood },
		{ 0, world_texture_ground_veto, world_texture_ground },
		{ 1, world_texture_floors_veto, world_texture_floors },
		{ 2, { }, world_texture_walls },
	};

	struct world_texture_slot_t {
		const char* m_texture;
		const char* m_transform;
	};

	constexpr world_texture_slot_t world_texture_slots[] = {
		{ "$basetexture", "$basetexturetransform" },
		{ "$basetexture2", "$basetexturetransform2" },
		{ "$basetexture3", nullptr },
		{ "$basetexture4", nullptr },
	};
	constexpr const char* world_texture_bumps[]  = { "$bumpmap", "$bumpmap2" };
	constexpr const char* world_texture_strips[] = { "$envmaptint", "$detailblendfactor" };

	constexpr int material_var_type_texture = 3;

	constexpr float world_texture_identity[ 16 ] = { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f };

	/* originals captured ONCE and add_ref'd: var writes are queued, a re-read returns our own value */
	struct world_texture_material_t {
		c_texture* m_original[ std::size( world_texture_slots ) ];
		c_texture* m_original_bump[ std::size( world_texture_bumps ) ];
		bool m_scalable[ std::size( world_texture_slots ) ];
		saved_var_t m_stripped[ std::size( world_texture_strips ) ];
		c_texture* m_original_decal;
		int m_decal_mode;
		c_texture* m_applied;
		unsigned int m_name_hash;
		int m_category;
	};

	std::unordered_map< c_material*, world_texture_material_t > world_texture_saved;
	unsigned int world_texture_last_signature = 0;
	float world_texture_next_walk             = 0.f;

	bool world_texture_any( const char* hay, std::span< const char* const > needles )
	{
		for ( const char* needle : needles ) {
			if ( std::strstr( hay, needle ) )
				return true;
		}

		return false;
	}

	int world_texture_match( const char* hay )
	{
		if ( !hay[ 0 ] )
			return -1;

		for ( const auto& rule : world_texture_rules ) {
			if ( world_texture_any( hay, rule.m_veto ) )
				continue;

			if ( world_texture_any( hay, rule.m_match ) )
				return rule.m_category;
		}

		return -1;
	}

	/* lowercase, '/' only, "maps/<map>/" cut: the map name ("de_cbble") must never pick a category */
	void world_texture_normalize( const char* source, char* out, std::size_t size )
	{
		std::size_t length = 0;
		for ( ; source && source[ length ] && length + 1 < size; length++ ) {
			const char c  = source[ length ] == '\\' ? '/' : source[ length ];
			out[ length ] = static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		}
		out[ length ] = '\0';

		if ( std::strncmp( out, "maps/", 5 ) == 0 ) {
			if ( const char* rest = std::strchr( out + 5, '/' ) )
				std::memmove( out, rest + 1, std::strlen( rest + 1 ) + 1 );
		}
	}

	int world_texture_classify( const char* name, const char* base_texture )
	{
		char material[ 192 ], texture[ 192 ];
		world_texture_normalize( name, material, sizeof( material ) );
		world_texture_normalize( base_texture, texture, sizeof( texture ) );

		if ( world_texture_any( material, world_texture_skip ) || world_texture_any( texture, world_texture_skip ) )
			return -1;

		int category = world_texture_match( material );
		if ( category < 0 )
			category = world_texture_match( texture );

		return category < 0 ? n_misc::world_texture_category_count - 1 : category;
	}

	c_material_var* world_texture_var( c_material* material, const char* name )
	{
		bool found = false;
		auto* var  = material->find_var( name, &found, false );

		return found ? var : nullptr;
	}

	c_texture* world_texture_read( c_material* material, const char* name )
	{
		auto* var = world_texture_var( material, name );
		if ( !var || var_type( var ) != material_var_type_texture )
			return nullptr;

		return var->get_texture( );
	}

	/* alpha-tested / translucent: a new opaque base turns fences and glass solid */
	bool world_texture_eligible( c_material* material )
	{
		if ( material->is_translucent( ) || material->is_alpha_tested( ) )
			return false;

		char shader[ 64 ];
		world_texture_normalize( material->get_shader_name( ), shader, sizeof( shader ) );

		return ( std::strstr( shader, "lightmapped" ) || std::strstr( shader, "worldvertextransition" ) ) && !std::strstr( shader, "reflective" );
	}

	bool prop_texture_eligible( c_material* material )
	{
		if ( material->is_translucent( ) || material->is_alpha_tested( ) )
			return false;

		char shader[ 64 ];
		world_texture_normalize( material->get_shader_name( ), shader, sizeof( shader ) );

		return std::strstr( shader, "vertexlit" ) != nullptr;
	}

	/* per slot, retried every apply: a slot never captured was never written, so it is still the original */
	void world_texture_capture( c_material* material, world_texture_material_t& entry )
	{
		for ( std::size_t i = 0; i < std::size( world_texture_slots ); i++ ) {
			if ( entry.m_original[ i ] )
				continue;

			c_texture* texture = world_texture_read( material, world_texture_slots[ i ].m_texture );
			if ( !texture )
				continue;

			texture->add_ref( );
			entry.m_original[ i ] = texture;

			if ( world_texture_slots[ i ].m_transform ) {
				auto* transform       = world_texture_var( material, world_texture_slots[ i ].m_transform );
				entry.m_scalable[ i ] = transform && transform->matrix_is_identity( );
			}
		}

		/* ssbump decodes 3 basis weights, a flat tangent normal there tints the lightmap */
		auto* ssbump = world_texture_var( material, "$ssbump" );
		if ( !ssbump || var_type( ssbump ) != material_var_type_int || var_int( ssbump ) == 0 ) {
			for ( std::size_t i = 0; i < std::size( world_texture_bumps ); i++ ) {
				if ( entry.m_original_bump[ i ] )
					continue;

				c_texture* texture = world_texture_read( material, world_texture_bumps[ i ] );
				if ( !texture )
					continue;

				texture->add_ref( );
				entry.m_original_bump[ i ] = texture;
			}
		}

		for ( std::size_t i = 0; i < std::size( world_texture_strips ); i++ ) {
			if ( entry.m_stripped[ i ].m_captured )
				continue;

			auto* var = world_texture_var( material, world_texture_strips[ i ] );
			if ( !var )
				continue;

			const int type = var_type( var );
			if ( !var_is_restorable( type, var_components( var ) ) )
				continue;

			saved_var_t& saved = entry.m_stripped[ i ];
			saved.m_type       = type;
			saved.m_int        = var_int( var );
			var_vec( var, saved.m_vec );
			saved.m_captured = true;
		}

		/* hr props ($decaltexture): stripes, signs, grime drawn over base on uv2 */
		if ( !entry.m_original_decal ) {
			if ( c_texture* texture = world_texture_read( material, "$decaltexture" ) ) {
				texture->add_ref( );
				entry.m_original_decal = texture;

				auto* mode         = world_texture_var( material, "$decalblendmode" );
				entry.m_decal_mode = mode && var_type( mode ) == material_var_type_int ? var_int( mode ) : 0;
			}
		}
	}

	/* uvs were baked against the ORIGINAL texture size: scale so the new one keeps its own texel density */
	void world_texture_scale( c_material_var* transform, c_texture* from, c_texture* to )
	{
		const float sx = static_cast< float >( from->get_mapping_width( ) ) / static_cast< float >( std::max( to->get_mapping_width( ), 1 ) );
		const float sy = static_cast< float >( from->get_mapping_height( ) ) / static_cast< float >( std::max( to->get_mapping_height( ), 1 ) );

		float matrix[ 16 ];
		std::memcpy( matrix, world_texture_identity, sizeof( matrix ) );
		matrix[ 0 ] = sx;
		matrix[ 5 ] = sy;

		transform->set_matrix( matrix );
	}

	bool world_texture_apply( c_material* material, world_texture_material_t& entry, c_texture* texture, c_texture* flat_normal, c_texture* white )
	{
		world_texture_capture( material, entry );

		/* textures not loaded yet (vars still strings): leave m_applied null so the next walk retries */
		bool wrote = false;
		for ( std::size_t i = 0; i < std::size( world_texture_slots ); i++ ) {
			if ( !entry.m_original[ i ] )
				continue;

			if ( auto* var = world_texture_var( material, world_texture_slots[ i ].m_texture ) ) {
				var->set_texture( texture );
				wrote = true;
			}

			if ( entry.m_scalable[ i ] ) {
				if ( auto* transform = world_texture_var( material, world_texture_slots[ i ].m_transform ) )
					world_texture_scale( transform, entry.m_original[ i ], texture );
			}
		}

		if ( !wrote )
			return false;

		if ( flat_normal ) {
			for ( std::size_t i = 0; i < std::size( world_texture_bumps ); i++ ) {
				if ( !entry.m_original_bump[ i ] )
					continue;

				if ( auto* var = world_texture_var( material, world_texture_bumps[ i ] ) )
					var->set_texture( flat_normal );
			}
		}

		/* mode 1 = base * decal: white is a no-op; mode 0 = alpha over: the new texture covers it */
		if ( entry.m_original_decal ) {
			c_texture* decal = entry.m_decal_mode == 1 ? white : texture;
			if ( auto* var = world_texture_var( material, "$decaltexture" ); var && decal )
				var->set_texture( decal );
		}

		for ( std::size_t i = 0; i < std::size( world_texture_strips ); i++ ) {
			const saved_var_t& saved = entry.m_stripped[ i ];
			if ( !saved.m_captured )
				continue;

			auto* var = world_texture_var( material, world_texture_strips[ i ] );
			if ( !var )
				continue;

			if ( saved.m_type == material_var_type_int )
				var->set_int( 0 );
			else if ( saved.m_type == material_var_type_vector )
				var->set_vector( 0.f, 0.f, 0.f );
			else
				var->set_float( 0.f );
		}

		entry.m_applied = texture;
		return true;
	}

	void world_texture_restore( c_material* material, world_texture_material_t& entry )
	{
		for ( std::size_t i = 0; i < std::size( world_texture_slots ); i++ ) {
			if ( !entry.m_original[ i ] )
				continue;

			if ( auto* var = world_texture_var( material, world_texture_slots[ i ].m_texture ) )
				var->set_texture( entry.m_original[ i ] );

			if ( entry.m_scalable[ i ] ) {
				if ( auto* transform = world_texture_var( material, world_texture_slots[ i ].m_transform ) )
					transform->set_matrix( world_texture_identity );
			}
		}

		for ( std::size_t i = 0; i < std::size( world_texture_bumps ); i++ ) {
			if ( !entry.m_original_bump[ i ] )
				continue;

			if ( auto* var = world_texture_var( material, world_texture_bumps[ i ] ) )
				var->set_texture( entry.m_original_bump[ i ] );
		}

		if ( entry.m_original_decal ) {
			if ( auto* var = world_texture_var( material, "$decaltexture" ) )
				var->set_texture( entry.m_original_decal );
		}

		for ( std::size_t i = 0; i < std::size( world_texture_strips ); i++ ) {
			if ( !entry.m_stripped[ i ].m_captured )
				continue;

			if ( auto* var = world_texture_var( material, world_texture_strips[ i ] ) )
				restore_var( var, entry.m_stripped[ i ] );
		}

		entry.m_applied = nullptr;
	}

	void world_texture_release( world_texture_material_t& entry )
	{
		for ( c_texture* texture : entry.m_original ) {
			if ( texture )
				texture->release( );
		}

		for ( c_texture* texture : entry.m_original_bump ) {
			if ( texture )
				texture->release( );
		}

		if ( entry.m_original_decal )
			entry.m_original_decal->release( );
	}

	/* walks the live list, never follows a stored pointer: destroyed materials are skipped */
	void world_texture_restore_all( bool forget )
	{
		auto* material_system = g_interfaces.m_material_system;

		int restored = 0;

		if ( material_system && !world_texture_saved.empty( ) ) {
			const unsigned short invalid = material_system->invalid_material( );
			for ( unsigned short handle = material_system->first_material( ); handle != invalid;
			      handle                = material_system->next_material( handle ) ) {
				c_material* material = material_system->get_material( handle );
				if ( !material )
					continue;

				const auto it = world_texture_saved.find( material );
				if ( it == world_texture_saved.end( ) || !it->second.m_applied )
					continue;

				if ( material->shader_param_count( ) <= 0 || it->second.m_name_hash != material_name_hash( material->get_name( ) ) )
					continue;

				world_texture_restore( material, it->second );
				restored++;
			}

			if ( restored > 0 )
				g_console.print( std::format( "[world texture] restore | tracked {} | written back {}", world_texture_saved.size( ), restored ).c_str( ) );
		}

		if ( forget ) {
			for ( auto& [ material, entry ] : world_texture_saved )
				world_texture_release( entry );

			world_texture_saved.clear( );
		}
	}

	c_texture* world_texture_find( const char* name )
	{
		if ( !name || !name[ 0 ] )
			return nullptr;

		c_texture* texture = g_interfaces.m_material_system->find_texture( name, TEXTURE_GROUP_WORLD, false );

		return texture && !texture->is_error( ) ? texture : nullptr;
	}

	/* material -> name hash; only ones WE set no_draw on */
	std::unordered_map< c_material*, unsigned int > world_decal_hidden;
	float world_decal_next_walk = 0.f;
	bool world_decal_was_on     = false;

	/* bullet holes / blood / sprays / infodecal ("Decal textures"), $decal + decalmodulate overlays, flagless overlays by name */
	bool world_decal_material( c_material* material, const char* group, const char* name )
	{
		if ( group && std::strcmp( group, TEXTURE_GROUP_DECAL ) == 0 )
			return true;

		if ( material->get_material_var_flag( material_var_decal ) )
			return true;

		char shader[ 64 ];
		world_texture_normalize( material->get_shader_name( ), shader, sizeof( shader ) );
		if ( std::strstr( shader, "decalmodulate" ) )
			return true;

		if ( !group || std::strcmp( group, TEXTURE_GROUP_WORLD ) != 0 || !( material->is_translucent( ) || material->is_alpha_tested( ) ) )
			return false;

		char lower[ 192 ];
		world_texture_normalize( name, lower, sizeof( lower ) );

		return std::strstr( lower, "decal" ) || std::strstr( lower, "overlay" );
	}

	/* walks the live list, never follows a stored pointer */
	void world_decal_unhide_all( )
	{
		auto* material_system = g_interfaces.m_material_system;
		if ( material_system && !world_decal_hidden.empty( ) ) {
			const unsigned short invalid = material_system->invalid_material( );
			for ( unsigned short handle = material_system->first_material( ); handle != invalid;
			      handle                = material_system->next_material( handle ) ) {
				c_material* material = material_system->get_material( handle );
				if ( !material )
					continue;

				const auto it = world_decal_hidden.find( material );
				if ( it != world_decal_hidden.end( ) && it->second == material_name_hash( material->get_name( ) ) )
					material->set_material_var_flag( material_var_no_draw, false );
			}

			g_console.print( std::format( "[world texture] decals shown {}", world_decal_hidden.size( ) ).c_str( ) );
		}

		world_decal_hidden.clear( );
	}

	void world_decals( bool on, float now )
	{
		if ( !on ) {
			if ( world_decal_was_on )
				world_decal_unhide_all( );

			world_decal_was_on = false;
			return;
		}

		if ( world_decal_was_on && now < world_decal_next_walk && now >= world_decal_next_walk - 1.f )
			return;

		world_decal_was_on    = true;
		world_decal_next_walk = now + 1.f;

		auto* material_system = g_interfaces.m_material_system;

		int hidden                   = 0;
		const unsigned short invalid = material_system->invalid_material( );
		for ( unsigned short handle = material_system->first_material( ); handle != invalid; handle = material_system->next_material( handle ) ) {
			c_material* material = material_system->get_material( handle );
			if ( !material || material->is_error_material( ) || material->shader_param_count( ) <= 0 )
				continue;

			const char* name             = material->get_name( );
			const unsigned int name_hash = material_name_hash( name );

			const auto it = world_decal_hidden.find( material );
			if ( it != world_decal_hidden.end( ) ) {
				if ( it->second == name_hash )
					continue;

				world_decal_hidden.erase( it );
			}

			if ( material->get_material_var_flag( material_var_no_draw ) ||
			     !world_decal_material( material, material->get_texture_group_name( ), name ) )
				continue;

			material->set_material_var_flag( material_var_no_draw, true );
			world_decal_hidden.emplace( material, name_hash );
			hidden++;
		}

		if ( hidden > 0 )
			g_console.print( std::format( "[world texture] decals hidden +{} total {}", hidden, world_decal_hidden.size( ) ).c_str( ) );
	}
}

void n_misc::impl_t::world_texture( )
{
	auto* material_system = g_interfaces.m_material_system;
	if ( !material_system )
		return;

	auto* engine = g_interfaces.m_engine_client;
	if ( engine && engine->is_connected( ) && !engine->is_in_game( ) )
		return;

	const bool enabled  = GET_VARIABLE( g_variables.m_world_texture, bool );
	const auto& presets = GET_VARIABLE( g_variables.m_world_texture_preset, std::vector< int > );

	world_decals( enabled && GET_VARIABLE( g_variables.m_world_texture_remove_decals, bool ), g_interfaces.m_global_vars_base->m_real_time );
	const auto& customs = GET_VARIABLE( g_variables.m_world_texture_custom, std::vector< std::string > );

	/* last slot = props, not a world category */
	constexpr int prop_slot  = world_texture_category_count;
	constexpr int slot_count = world_texture_category_count + 1;

	const char* wanted_names[ slot_count ] = { };
	unsigned int signature                 = enabled ? 0x9e3779b9u : 0u;
	bool any                               = false;

	for ( int category = 0; enabled && category < world_texture_category_count; category++ ) {
		const int preset = category < static_cast< int >( presets.size( ) ) ? presets[ category ] : 0;

		const char* name = nullptr;
		if ( preset == world_texture_preset_custom ) {
			if ( category < static_cast< int >( customs.size( ) ) && !customs[ category ].empty( ) )
				name = customs[ category ].c_str( );
		}
		else if ( preset > 0 && preset < world_texture_preset_count )
			name = world_texture_presets[ preset ].m_texture;

		wanted_names[ category ] = name;
		signature                = signature * 31u + material_name_hash( name ) + static_cast< unsigned int >( category );
		any                      = any || name;
	}

	if ( GET_VARIABLE( g_variables.m_prop_texture, bool ) ) {
		const int preset   = GET_VARIABLE( g_variables.m_prop_texture_preset, int );
		const auto& custom = GET_VARIABLE( g_variables.m_prop_texture_custom, std::string );

		const char* name = nullptr;
		if ( preset == world_texture_preset_custom )
			name = custom.empty( ) ? nullptr : custom.c_str( );
		else if ( preset > 0 && preset < world_texture_preset_count )
			name = world_texture_presets[ preset ].m_texture;

		wanted_names[ prop_slot ] = name;
		signature                 = signature * 31u + material_name_hash( name ) + 0x50u;
		any                       = any || name;
	}

	const bool changed = signature != world_texture_last_signature;
	const float now    = g_interfaces.m_global_vars_base->m_real_time;

	if ( !changed && now < world_texture_next_walk && now >= world_texture_next_walk - 1.f )
		return;

	world_texture_last_signature = signature;
	world_texture_next_walk      = now + 1.f;

	if ( !any ) {
		if ( changed )
			world_texture_restore_all( false );

		return;
	}

	c_texture* wanted[ slot_count ] = { };
	std::string missing;

	for ( int category = 0; category < slot_count; category++ ) {
		wanted[ category ] = world_texture_find( wanted_names[ category ] );

		if ( changed && wanted_names[ category ] && !wanted[ category ] )
			missing.append( std::format( " {}", wanted_names[ category ] ) );
	}

	c_texture* flat_normal = world_texture_find( "dev/flat_normal" );
	c_texture* white       = world_texture_find( "vgui/white" );

	int applied                = 0;
	int restored               = 0;
	int reverted               = 0;
	int matched[ slot_count ] = { };

	const unsigned short invalid = material_system->invalid_material( );
	for ( unsigned short handle = material_system->first_material( ); handle != invalid; handle = material_system->next_material( handle ) ) {
		c_material* material = material_system->get_material( handle );
		if ( !material || material->is_error_material( ) || material->shader_param_count( ) <= 0 )
			continue;

		const char* group = material->get_texture_group_name( );
		const char* name  = material->get_name( );
		const bool world  = group && std::strcmp( group, TEXTURE_GROUP_WORLD ) == 0;
		if ( !world && classify_modulation( group, name ) != modulation_group_props )
			continue;

		const unsigned int name_hash = material_name_hash( name );

		auto it = world_texture_saved.find( material );
		if ( it == world_texture_saved.end( ) || it->second.m_name_hash != name_hash ) {
			/* recycled address: those originals belong to a dead material, drop the refs, write nothing */
			if ( it != world_texture_saved.end( ) )
				world_texture_release( it->second );

			world_texture_material_t entry{ };
			entry.m_name_hash = name_hash;
			entry.m_category  = -1;

			/* props skip the world name list: it dropped opaque signs, windows, fences, skybox props, bark, "farm_tools/" */
			if ( !world )
				entry.m_category = prop_texture_eligible( material ) ? prop_slot : -1;
			else if ( world_texture_eligible( material ) ) {
				c_texture* base  = world_texture_read( material, "$basetexture" );
				entry.m_category = world_texture_classify( name, base ? base->get_name( ) : nullptr );
			}

			if ( entry.m_category < 0 && name && std::strstr( name, "models/" ) )
				g_console.print( std::format( "[prop texture] skip {} | group {} | shader {} | tr {} at {}", name, group ? group : "-",
				                              material->get_shader_name( ), material->is_translucent( ) ? 1 : 0, material->is_alpha_tested( ) ? 1 : 0 )
				                     .c_str( ) );

			it = world_texture_saved.insert_or_assign( material, entry ).first;
		}

		world_texture_material_t& entry = it->second;
		if ( entry.m_category < 0 )
			continue;

		matched[ entry.m_category ]++;

		c_texture* want = wanted[ entry.m_category ];

		/* re-precache rebuilds vars from the vmt behind our back */
		if ( entry.m_applied && entry.m_original[ 0 ] && world_texture_read( material, "$basetexture" ) == entry.m_original[ 0 ] ) {
			if ( reverted++ < 4 )
				g_console.print( std::format( "[world texture] reverted {}", name ).c_str( ) );
			entry.m_applied = nullptr;
		}

		if ( want == entry.m_applied )
			continue;

		if ( want ) {
			if ( world_texture_apply( material, entry, want, flat_normal, white ) ) {
				applied++;
				if ( changed && entry.m_category == prop_slot )
					g_console.print( std::format( "[prop texture] set {} | decal {} mode {}", name, entry.m_original_decal ? 1 : 0, entry.m_decal_mode ).c_str( ) );
			}
			else if ( changed && entry.m_category == prop_slot )
				g_console.print( std::format( "[prop texture] no base {}", name ).c_str( ) );
		}
		else {
			world_texture_restore( material, entry );
			restored++;
		}
	}

	if ( changed || applied > 0 || restored > 0 ) {
		std::string counts;
		for ( int category = 0; category < world_texture_category_count; category++ )
			counts.append( std::format( " {} {}", world_texture_categories[ category ], matched[ category ] ) );
		counts.append( std::format( " props {}", matched[ prop_slot ] ) );

		g_console.print( std::format( "[world texture] applied {} restored {} reverted {} |{} | flat normal {}{}{}", applied, restored, reverted, counts,
		                              flat_normal ? 1 : 0, missing.empty( ) ? "" : " | missing", missing )
		                     .c_str( ) );
	}
}

void n_misc::impl_t::on_level_shutdown( )
{
	restore_tracked_materials( );
	old_shader_next_walk = 0.f;

	restore_modulated_materials( );
	world_modulation_next_walk = 0.f;

	world_texture_restore_all( true );
	world_texture_last_signature = 0;
	world_texture_next_walk      = 0.f;

	world_decal_unhide_all( );
	world_decal_was_on = false;
}

void n_misc::impl_t::force_maxunlag( )
{
	static auto sv_maxunlag = g_convars[ HASH_BT( "sv_maxunlag" ) ];
	if ( !sv_maxunlag ) {
		sv_maxunlag = g_convars[ HASH_BT( "sv_maxunlag" ) ];
		if ( !sv_maxunlag )
			return;
	}

	if ( sv_maxunlag->get_float( ) != 1.f )
		sv_maxunlag->set_value( 1.f );
}

void n_misc::impl_t::force_host_lagcomp( )
{
	static auto net_fakelag = g_convars[ HASH_BT( "net_fakelag" ) ];
	if ( !net_fakelag )
		net_fakelag = g_convars[ HASH_BT( "net_fakelag" ) ];

	/* pinned at 0 (also wipes stale config values). write only on change: change callback, and the engine
	   ramps toward it at 200ms/sec */
	if ( net_fakelag && net_fakelag->get_float( ) != 0.f )
		net_fakelag->set_value( 0.f );

	const auto nci = g_interfaces.m_engine_client->get_net_channel_info( );
	if ( !g_interfaces.m_engine_client->is_connected_safe( ) || !nci || !nci->is_loopback( ) )
		return;

	if ( g_interfaces.m_global_vars_base && !g_interfaces.m_global_vars_base->m_remote_client )
		g_interfaces.m_global_vars_base->m_remote_client = true;

	if ( const auto sv_unlag = g_convars[ HASH_BT( "sv_unlag" ) ]; sv_unlag && !sv_unlag->get_bool( ) )
		sv_unlag->set_value( 1.f );

	if ( const auto cl_lagcompensation = g_convars[ HASH_BT( "cl_lagcompensation" ) ];
	     cl_lagcompensation && !cl_lagcompensation->get_bool( ) )
		cl_lagcompensation->set_value( 1.f );
}

c_base_entity* find_player_resource( )
{
	static int resource_index = -1;

	if ( !g_interfaces.m_engine_client->is_connected_safe( ) ) {
		resource_index = -1;
		return nullptr;
	}

	const auto class_id_of = []( c_base_entity* entity ) -> e_class_ids {
		if ( !entity )
			return static_cast< e_class_ids >( -1 );

		const auto networkable  = static_cast< c_client_networkable* >( entity );
		const auto client_class = networkable->get_client_class( );
		return client_class ? client_class->m_class_id : static_cast< e_class_ids >( -1 );
	};

	auto resource = ( resource_index > 0 ) ? g_interfaces.m_client_entity_list->get< c_base_entity >( resource_index ) : nullptr;

	/* re-validate every frame: entity slots get reused, and it's one vtable call */
	if ( class_id_of( resource ) != e_class_ids::ccs_player_resource ) {
		resource       = nullptr;
		resource_index = -1;

		for ( int i = 1; i <= g_interfaces.m_client_entity_list->get_highest_entity_index( ); ++i ) {
			const auto entity = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
			if ( class_id_of( entity ) != e_class_ids::ccs_player_resource )
				continue;

			resource       = entity;
			resource_index = i;
			break;
		}
	}

	return resource;
}

static std::mutex g_player_list_mutex;
static std::unordered_map< unsigned long long, player_list_entry_t > g_player_list_saved;
static bool g_player_list_loaded = false;

static std::filesystem::path player_list_path( )
{
	return g_config.m_path / "player_list.txt";
}

/* bots carry xuid 0 ( or a fake one ): never saved */
static unsigned long long player_list_xuid( const player_info_t& info )
{
	return info.m_fake_player ? 0ull : info.m_ull_xuid;
}

/* caller holds the mutex */
static void player_list_load( )
{
	if ( g_player_list_loaded )
		return;
	g_player_list_loaded = true;

	FILE* file = nullptr;
	if ( _wfopen_s( &file, player_list_path( ).c_str( ), L"r" ) || !file )
		return;

	unsigned long long xuid = 0;
	int revive = 0, ignore = 0, only = 0;
	while ( fscanf_s( file, "%llu %d %d %d", &xuid, &revive, &ignore, &only ) == 4 ) {
		if ( xuid )
			g_player_list_saved[ xuid ] = player_list_entry_t{ 0, revive != 0, ignore != 0, only != 0 };
	}
	fclose( file );
}

void player_list_stamp( const int index, const player_info_t& info )
{
	if ( index < 1 || index > 64 || g_player_list[ index ].m_user_id == info.m_user_id )
		return;

	std::lock_guard lock( g_player_list_mutex );
	player_list_load( );

	player_list_entry_t entry{ };
	if ( const auto xuid = player_list_xuid( info ) ) {
		if ( const auto it = g_player_list_saved.find( xuid ); it != g_player_list_saved.end( ) )
			entry = it->second;
	}
	entry.m_user_id        = info.m_user_id;
	g_player_list[ index ] = entry;
}

void player_list_save( const int index, const player_info_t& info )
{
	const auto xuid = player_list_xuid( info );
	if ( index < 1 || index > 64 || !xuid )
		return;

	std::lock_guard lock( g_player_list_mutex );
	player_list_load( );

	const auto& entry = g_player_list[ index ];
	if ( entry.m_revive || entry.m_ignore || entry.m_only )
		g_player_list_saved[ xuid ] = entry;
	else
		g_player_list_saved.erase( xuid );

	FILE* file = nullptr;
	if ( _wfopen_s( &file, player_list_path( ).c_str( ), L"w" ) || !file )
		return;
	for ( const auto& [ id, picks ] : g_player_list_saved )
		fprintf( file, "%llu %d %d %d\n", id, picks.m_revive ? 1 : 0, picks.m_ignore ? 1 : 0, picks.m_only ? 1 : 0 );
	fclose( file );
}

/* once per frame: stamp every slot so saved picks apply with the menu closed. empty slots wiped,
   stale "only" from someone who left never narrows aim */
static void player_list_sync( )
{
	static int synced_frame = -1;
	const int frame         = g_interfaces.m_global_vars_base->m_frame_count;
	if ( frame == synced_frame )
		return;
	synced_frame = frame;

	const auto engine = g_interfaces.m_engine_client;
	const int clients = engine->is_connected_safe( ) ? std::min( engine->get_max_clients( ), 64 ) : 0;
	for ( int i = 1; i <= 64; i++ ) {
		player_info_t info{ };
		if ( i <= clients && engine->get_player_info( i, &info ) && !info.m_is_hltv )
			player_list_stamp( i, info );
		else if ( g_player_list[ i ].m_user_id )
			g_player_list[ i ] = player_list_entry_t{ };
	}
}

const player_list_entry_t* player_list_get( const int index )
{
	if ( index < 1 || index > 64 )
		return nullptr;

	player_list_sync( );
	if ( !g_player_list[ index ].m_user_id )
		return nullptr;

	player_info_t info{ };
	if ( !g_interfaces.m_engine_client->get_player_info( index, &info ) || info.m_user_id != g_player_list[ index ].m_user_id )
		return nullptr;

	return &g_player_list[ index ];
}

bool player_list_aim_allowed( const int index )
{
	if ( const auto entry = player_list_get( index ) ) {
		if ( entry->m_ignore )
			return false;
		if ( entry->m_only )
			return true;
	}

	for ( int i = 1; i <= 64; i++ ) {
		if ( g_player_list[ i ].m_only && player_list_get( i ) )
			return false;
	}

	return true;
}

void n_misc::impl_t::fix_offline_ping( )
{
	const auto nci = g_interfaces.m_engine_client->is_connected_safe( ) ? g_interfaces.m_engine_client->get_net_channel_info( ) : nullptr;
	if ( !nci || !nci->is_loopback( ) )
		return;

	const int local_index = g_interfaces.m_engine_client->get_local_player( );
	if ( local_index <= 0 )
		return;

	const auto resource = find_player_resource( );
	if ( !resource )
		return;

	const int window_ms = static_cast< int >( ( g_lagcomp.real_latency( ) + g_lagcomp.extend( ) ) * 1000.f );

	resource->get_resource_ping( local_index ) = std::max( 5, window_ms );
}

void n_misc::impl_t::force_lagpush( )
{
	static auto sv_lagpushticks = g_convars[ HASH_BT( "sv_lagpushticks" ) ];
	if ( !sv_lagpushticks ) {
		sv_lagpushticks = g_convars[ HASH_BT( "sv_lagpushticks" ) ];
		if ( !sv_lagpushticks )
			return;
	}

	const int push   = g_lagcomp.push_ticks( );
	const int wanted = -push;

	if ( sv_lagpushticks->get_int( ) != wanted ) {
		sv_lagpushticks->set_value( wanted );

		/* LAGPUSH DIAG - one line per change (the EXTEND line never prints while we host) */
		botox_dbg_log( "LAGPUSH: ticks=%d shot=%d centre=%.0fms correct=%.0fms", push, static_cast< int >( g_lagcomp.m_shot ),
		                   g_lagcomp.window_center( ) * 1000.f, g_lagcomp.correct_time( ) * 1000.f );
	}
}

void n_misc::impl_t::force_crosshair( )
{
	static auto weapon_debug_spread_show = g_convars[ HASH_BT( "weapon_debug_spread_show" ) ];
	if ( !weapon_debug_spread_show )
		return;

	const bool should_draw = GET_VARIABLE( g_variables.m_force_crosshair, bool ) && !GET_VARIABLE( g_variables.m_safe_mode, bool ) && g_ctx.m_local && g_ctx.m_local->is_alive( ) &&
	                         !g_ctx.m_local->is_scoped( );

	const int wanted = should_draw ? 3 : 0;

	if ( weapon_debug_spread_show->get_int( ) != wanted )
		weapon_debug_spread_show->set_value( wanted );
}

void n_misc::impl_t::performance( bool restore )
{
	const bool enabled = !restore && GET_VARIABLE( g_variables.m_performance, bool );

	struct knob_t {
		const char* m_name;
		std::uint32_t m_variable;
		float m_value;
		c_cconvar* m_convar;
		bool m_captured;
		float m_stored;
	};

	static knob_t knobs[ ] = {
		{ "cl_threaded_bone_setup", g_variables.m_performance_threaded_bones, 1.f, nullptr, false, 0.f },
		{ "cl_forcepreload", g_variables.m_performance_force_preload, 1.f, nullptr, false, 0.f },
		{ "mat_queue_mode", g_variables.m_performance_multicore_render, 2.f, nullptr, false, 0.f },
		/* engine sleeps 50 ms a frame while unfocused (recording tabbed out) */
		{ "engine_no_focus_sleep", g_variables.m_performance_no_focus_sleep, 0.f, nullptr, false, 0.f },
	};

	for ( auto& knob : knobs ) {
		if ( !knob.m_convar && !( knob.m_convar = g_convars[ HASH_RT( knob.m_name ) ] ) )
			continue;

		if ( !enabled || !GET_VARIABLE( knob.m_variable, bool ) ) {
			if ( knob.m_captured ) {
				knob.m_convar->set_value( knob.m_stored );
				knob.m_captured = false;
			}

			continue;
		}

		if ( !knob.m_captured ) {
			knob.m_stored   = knob.m_convar->get_float( );
			knob.m_captured = true;
		}

		if ( knob.m_convar->get_float( ) != knob.m_value )
			knob.m_convar->set_value( knob.m_value );
	}

	const HANDLE process = GetCurrentProcess( );
	static DWORD stored_class = 0;
	if ( enabled && GET_VARIABLE( g_variables.m_performance_high_priority, bool ) ) {
		if ( !stored_class && ( stored_class = GetPriorityClass( process ) ) != 0 )
			SetPriorityClass( process, HIGH_PRIORITY_CLASS );
	} else if ( stored_class ) {
		SetPriorityClass( process, stored_class );
		stored_class = 0;
	}

	static bool fast_cores = false;
	if ( const bool want = enabled && GET_VARIABLE( g_variables.m_performance_fast_cores, bool ); want != fast_cores ) {
		PROCESS_POWER_THROTTLING_STATE state{ };
		state.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		state.ControlMask = want ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION : 0;
		if ( !SetProcessInformation( process, ProcessPowerThrottling, &state, sizeof( state ) ) && want ) {
			state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
			SetProcessInformation( process, ProcessPowerThrottling, &state, sizeof( state ) );
		}
		fast_cores = want;
		n_perf::s_eco_workers.store( want, std::memory_order_relaxed );
	}
}
