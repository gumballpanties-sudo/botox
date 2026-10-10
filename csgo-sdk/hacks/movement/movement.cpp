#include "movement.h"
#include "movement_internal.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <cwchar>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include "../../dependencies/imgui/imgui.h"
#include "../../dependencies/json/json.hpp"
#include "../../game/sdk/classes/c_physics_surface_props.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/interfaces/interfaces.h"
#include "../chat_hud.h"
#include "../prediction/prediction.h"
#include "texturebug.h"
#include "wall_climb.h"
#include "edgebug.h"
#include "edge_skip.h"
#include "tick_scale.h"
#include "../misc/misc.h"
#include "../../utilities/perf/perf_watch.h"

void CorrectMovement( c_user_cmd* cmd, c_angle wish_angle, c_angle old_angles );
void start_movement_fix( c_user_cmd* cmd );
void end_movement_fix( c_user_cmd* cmd );
void gnd_wish_check( const char* stage, bool final_view = false );

bool wall_detected                = false;
bool should_align                 = false;

int aa_gate_skips                 = 0;
int aa_reach_skips                = 0;
int aa_fail_skips                 = 0;
int aa_outs[ aa_out_count ]{ };

int b_buttons                     = 0;

bool g_air_stuck_noclip_active = false;

/* true any tick air_stuck wrote the cmd (approach/grind/hold). everything after air_stuck
   must yield on this or it stomps the pin (edgebug too) */
bool g_air_stuck_owns_cmd = false;

bool g_air_stuck_holding = false;

bool g_air_stuck_owned_prev_tick = false;

c_angle g_air_stuck_authored_view{ };
bool g_air_stuck_authored_view_valid = false;

bool g_air_stuck_stamp_valid    = false;
float g_air_stuck_stamp_yaw     = 0.0f;
float g_air_stuck_stamp_forward = 0.0f;
float g_air_stuck_stamp_side    = 0.0f;
int g_air_stuck_stamp_buttons   = 0;
bool g_air_stuck_stamp_force    = false;
float g_air_stuck_stamp_pitch   = 0.0f;

class WindowDetect
{
public:
	enum WindowState {
		FADE_IN,
		VISIBLE,
		FADE_OUT
	};
	struct AnimatedWindow {
		int type;
		std::string info;
		float elapsed;
		WindowState state;
		float fadeInDuration;
		float visibleDuration;
		float fadeOutDuration;
		float totalDuration;
		float alpha;
		ImVec2 targetPosition;
		ImVec2 currentPosition;
		float accentProgress;
		float accentWidth;
		bool startFadeOut;
		/* stack slot from the bottom; index + pixel gap so rows never overlap at any res */
		int stackIndex;
	};
	static constexpr float NORMAL_FADEIN_DURATION  = 0.25f;
	static constexpr float VISIBLE_DURATION        = 1.8f;
	static constexpr float NORMAL_FADEOUT_DURATION = 0.2f;
	static constexpr float QUICK_FADEOUT_DURATION  = 0.12f;
	static constexpr float WINDOW_SPACING          = 5.0f / 1080.0f;
	static constexpr float BASE_Y_FRACTION = 0.8f;
	static constexpr float BOX_PADDING_X   = 14.0f;
	static constexpr float BOX_HEIGHT      = 25.0f;
	static constexpr float BOX_MIN_WIDTH   = 95.0f;
	static constexpr float STACK_GAP       = 4.0f;
	void AddWindow( int type, const std::string& info )
	{
		if ( info.empty( ) || info.length( ) > 200 ) {
			return;
		}
		std::lock_guard< std::mutex > lock( m_windows_mutex );
		static std::deque< std::pair< std::string, float > > recent_messages;
		float current_time = GetTickCount64( ) / 1000.f;
		while ( !recent_messages.empty( ) && current_time - recent_messages.front( ).second > 2.0f ) {
			recent_messages.pop_front( );
		}
		for ( const auto& recent : recent_messages ) {
			if ( recent.first == info ) {
				return;
			}
		}
		recent_messages.push_back( { info, current_time } );
		const float baseYPosition         = 0.8f;
		constexpr int MAX_VISIBLE_WINDOWS = 5;
		/* Draw measures the text on the d3d thread */
		float windowHeight = 30.0f / 1080.0f;
		AnimatedWindow newWin;
		newWin.type            = type;
		newWin.info            = info;
		newWin.elapsed         = 0.f;
		newWin.state           = FADE_IN;
		newWin.fadeInDuration  = NORMAL_FADEIN_DURATION;
		newWin.visibleDuration = VISIBLE_DURATION;
		newWin.fadeOutDuration = NORMAL_FADEOUT_DURATION;
		newWin.totalDuration   = NORMAL_FADEIN_DURATION + VISIBLE_DURATION + NORMAL_FADEOUT_DURATION;
		newWin.alpha           = 0.f;
		newWin.accentProgress  = 0.f;
		newWin.accentWidth     = 0.f;
		newWin.startFadeOut    = false;
		newWin.stackIndex      = 0;
		int visibleCount       = 0;
		for ( auto& win : windows ) {
			if ( win.state != FADE_OUT ) {
				visibleCount++;
			}
		}
		for ( auto& win : windows ) {
			if ( win.state != FADE_OUT ) {
				win.stackIndex++;
				win.targetPosition.y -= ( windowHeight + WINDOW_SPACING );
				win.currentPosition.y -= ( windowHeight + WINDOW_SPACING );
				if ( visibleCount >= MAX_VISIBLE_WINDOWS &&
				     win.targetPosition.y < baseYPosition - ( MAX_VISIBLE_WINDOWS - 1 ) * ( windowHeight + WINDOW_SPACING ) ) {
					win.state           = FADE_OUT;
					win.elapsed         = 0.f;
					win.fadeOutDuration = NORMAL_FADEOUT_DURATION;
				}
			}
		}
		newWin.targetPosition  = ImVec2( 0.5f, baseYPosition );
		newWin.currentPosition = ImVec2( 0.5f, baseYPosition );
		if ( newWin.targetPosition.x < 0.0f || newWin.targetPosition.x > 1.0f || newWin.targetPosition.y < 0.0f || newWin.targetPosition.y > 1.0f ) {
			newWin.targetPosition.x = std::clamp( newWin.targetPosition.x, 0.01f, 0.99f );
			newWin.targetPosition.y = std::clamp( newWin.targetPosition.y, 0.01f, 0.99f );
			newWin.currentPosition  = newWin.targetPosition;
		}
		windows.push_back( newWin );
	}
	void Update( float deltaTime )
	{
		std::lock_guard< std::mutex > lock( m_windows_mutex );
		CleanupOldWindows( );
		std::sort( windows.begin( ), windows.end( ),
		           []( const AnimatedWindow& a, const AnimatedWindow& b ) { return a.targetPosition.y > b.targetPosition.y; } );
		for ( auto it = windows.begin( ); it != windows.end( ); ) {
			it->elapsed += deltaTime;
			float totalElapsed = 0.0f;
			if ( it->state == FADE_IN ) {
				totalElapsed = it->elapsed + 0.2f;
			} else if ( it->state == VISIBLE ) {
				totalElapsed = it->fadeInDuration + it->elapsed + 0.2f;
			} else if ( it->state == FADE_OUT ) {
				totalElapsed = it->fadeInDuration + it->visibleDuration + it->elapsed + 0.2f;
			}
			it->accentProgress = totalElapsed / ( it->totalDuration + 0.4f );
			if ( it->accentProgress > 1.0f )
				it->accentProgress = 1.0f;
			switch ( it->state ) {
			case FADE_IN:
				it->alpha = it->elapsed / it->fadeInDuration;
				if ( it->alpha > 1.0f )
					it->alpha = 1.0f;
				if ( it->elapsed >= it->fadeInDuration ) {
					it->state   = VISIBLE;
					it->elapsed = 0.f;
					it->alpha   = 1.f;
				}
				break;
			case VISIBLE:
				it->alpha = 1.f;
				if ( it->elapsed >= it->visibleDuration && !it->startFadeOut ) {
					it->startFadeOut = true;
					it->state        = FADE_OUT;
					it->elapsed      = 0.f;
				}
				break;
			case FADE_OUT:
				it->alpha = 1.0f - ( it->elapsed / it->fadeOutDuration );
				if ( it->alpha < 0.0f )
					it->alpha = 0.0f;
				if ( it->elapsed >= it->fadeOutDuration ) {
					it = windows.erase( it );
					continue;
				}
				break;
			}
			++it;
		}
	}
	void Draw( ImDrawList* draw, const ImVec2& displaySize )
	{
		std::lock_guard< std::mutex > lock( m_windows_mutex );
		for ( const auto& win : windows ) {
			if ( win.state == FADE_OUT && win.alpha <= 0.0f ) {
				continue;
			}
			auto* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
			if ( !font )
				continue;
			ImVec2 infoTextSize = font->CalcTextSizeA( font->FontSize, FLT_MAX, NULL, win.info.c_str( ) );
			float fixedHeight   = BOX_HEIGHT;
			ImVec2 size         = ImVec2( ( std::max )( infoTextSize.x + BOX_PADDING_X * 2.f, BOX_MIN_WIDTH ), fixedHeight );
			const float stack_y = displaySize.y * BASE_Y_FRACTION - win.stackIndex * ( fixedHeight + STACK_GAP );
			ImVec2 pos      = ImVec2( displaySize.x * win.currentPosition.x - size.x * 0.5f, stack_y - size.y * 0.5f );
			int a           = static_cast< int >( win.alpha * 255 );
			ImU32 bgColor     = IM_COL32( 15, 15, 15, a );
			ImU32 borderColor = IM_COL32( 50, 50, 50, a );
			ImU32 textColor = IM_COL32( 240, 240, 240, a );
			const float cornerRadius = ImGui::GetStyle( ).WindowRounding;
			draw->AddRectFilled( pos, ImVec2( pos.x + size.x, pos.y + size.y ), bgColor, cornerRadius );
			draw->AddRect( ImVec2( pos.x + 1.f, pos.y + 1.f ), ImVec2( pos.x + size.x - 1.f, pos.y + size.y - 1.f ), borderColor, cornerRadius );
			c_color accent_from_menu = GET_VARIABLE( g_variables.m_accent, c_color );
			ImVec4 accentColor = ImVec4( accent_from_menu[ 0 ] / 255.0f, accent_from_menu[ 1 ] / 255.0f, accent_from_menu[ 2 ] / 255.0f, win.alpha );
			const float accentHeight = 1.0f;
			float maxWidth           = size.x - 2.0f;
			float margin             = 1.0f;
			float currentWidth       = maxWidth * ( 1.0f - win.accentProgress );
			if ( currentWidth < 0 )
				currentWidth = 0;
			float startX = pos.x + margin;
			if ( currentWidth > 0 ) {
				float centerX   = pos.x + size.x * 0.5f;
				float halfWidth = currentWidth * 0.5f;
				float lineY     = pos.y;
				draw->AddRectFilled( ImVec2( centerX - halfWidth, lineY ), ImVec2( centerX + halfWidth, lineY + accentHeight ),
				                     ImGui::GetColorU32( accentColor ) );
			}
			ImVec2 textPos    = ImVec2( pos.x + ( size.x - infoTextSize.x ) * 0.5f,
			                            pos.y + ( size.y - infoTextSize.y ) * 0.5f );
			if ( win.info.find( "(" ) != std::string::npos && win.info.find( ")" ) != std::string::npos ) {
				ImFont* doctrine_font = font;
				std::string text = win.info;
				float current_x  = textPos.x;
				float shadow_x   = textPos.x + 1;
				size_t pos       = 0;
				while ( pos < text.length( ) ) {
					if ( text[ pos ] == '(' || text[ pos ] == ')' ) {
						std::string bracket( 1, text[ pos ] );
						draw->AddText( doctrine_font, doctrine_font->FontSize, ImVec2( shadow_x, textPos.y + 1 ), IM_COL32( 0, 0, 0, a / 4 ),
						               bracket.c_str( ) );
						draw->AddText( doctrine_font, doctrine_font->FontSize, ImVec2( current_x, textPos.y ), textColor, bracket.c_str( ) );
						ImVec2 bracket_size = doctrine_font->CalcTextSizeA( doctrine_font->FontSize, FLT_MAX, NULL, bracket.c_str( ) );
						current_x += bracket_size.x + 0.3f;
						shadow_x += bracket_size.x + 0.3f;
					} else {
						std::string char_str( 1, text[ pos ] );
						draw->AddText( font, font->FontSize, ImVec2( shadow_x, textPos.y + 1 ), IM_COL32( 0, 0, 0, a / 4 ), char_str.c_str( ) );
						draw->AddText( font, font->FontSize, ImVec2( current_x, textPos.y ), textColor, char_str.c_str( ) );
						ImVec2 char_size = font->CalcTextSizeA( font->FontSize, FLT_MAX, NULL, char_str.c_str( ) );
						current_x += char_size.x;
						shadow_x += char_size.x;
					}
					pos++;
				}
			} else {
				draw->AddText( font, font->FontSize, ImVec2( textPos.x + 1, textPos.y + 1 ), IM_COL32( 0, 0, 0, a / 4 ), win.info.c_str( ) );
				draw->AddText( font, font->FontSize, textPos, textColor, win.info.c_str( ) );
			}
		}
	}

private:
	std::vector< AnimatedWindow > windows;
	std::mutex m_windows_mutex;

	/* callers already hold m_windows_mutex - do not lock here */
	void CleanupOldWindows( )
	{
		constexpr int MAX_WINDOWS = 10;
		if ( windows.size( ) > MAX_WINDOWS ) {
			int windowsToRemove = windows.size( ) - MAX_WINDOWS;
			std::sort( windows.begin( ), windows.end( ),
			           []( const AnimatedWindow& a, const AnimatedWindow& b ) { return a.targetPosition.y < b.targetPosition.y; } );
			for ( int i = 0; i < windowsToRemove && i < windows.size( ); i++ ) {
				if ( windows[ i ].state != FADE_OUT ) {
					windows[ i ].state           = FADE_OUT;
					windows[ i ].elapsed         = 0.f;
					windows[ i ].fadeOutDuration = QUICK_FADEOUT_DURATION;
				}
			}
		}
	}
};
WindowDetect myWindowDetect;
void movement_add_window( int type, const std::string& info )
{
	myWindowDetect.AddWindow( type, info );
}
int ignoretime = 0;
void n_movement::impl_t::on_create_move_pre( )
{
	{
		static unsigned int sim_sec = 0u, res_sec = 0u, sim_peak = 0u, ticks_sec = 0u;
		static unsigned long long sim_next_report = 0ull;

		sim_sec += g_prediction.m_sim_count;
		res_sec += g_prediction.m_restore_count;
		if ( g_prediction.m_sim_count > sim_peak )
			sim_peak = g_prediction.m_sim_count;
		g_prediction.m_sim_count = g_prediction.m_restore_count = 0u;
		ticks_sec++;

		if ( const unsigned long long now = GetTickCount64( ); now >= sim_next_report ) {
			sim_next_report = now + 1000ull;
			if ( ticks_sec )
				botox_dbg_log( "SIM: sims=%u/s restores=%u/s ticks=%u avg=%.1f peak=%u fair=%u hungry=0x%x", sim_sec, res_sec, ticks_sec,
				               static_cast< float >( sim_sec ) / static_cast< float >( ticks_sec ), sim_peak, n_tick::s_fair_cuts,
				               n_tick::s_hungry_prev );
			sim_sec = res_sec = sim_peak = ticks_sec = 0u;
			n_tick::s_fair_cuts = 0u;
		}
	}

	g_air_stuck_owned_prev_tick     = g_air_stuck_owns_cmd;
	g_air_stuck_owns_cmd            = false;
	g_air_stuck_authored_view_valid = false;
	/* this-tick fact: stale would re-press a dead weld cmd */
	g_air_stuck_stamp_valid = false;
	g_air_stuck_stamp_force = false;

	if ( g_ctx.m_cmd ) {
		m_user_forward_move = g_ctx.m_cmd->m_forward_move;
		m_user_side_move    = g_ctx.m_cmd->m_side_move;
		m_user_buttons      = g_ctx.m_cmd->m_buttons;

		m_user_forward_move_raw = g_ctx.m_cmd->m_forward_move;
		m_user_side_move_raw    = g_ctx.m_cmd->m_side_move;
		m_user_buttons_raw      = g_ctx.m_cmd->m_buttons;
	}

	if ( GET_VARIABLE( g_variables.m_no_crouch_cooldown, bool ) && !GET_VARIABLE( g_variables.m_safe_mode, bool ) )
		g_ctx.m_cmd->m_buttons |= e_command_buttons::in_bullrush;

	this->pixel_finder( g_ctx.m_cmd );
	gnd_wish_check( "pixel_finder" );
	this->ladder_freelook_climb( g_ctx.m_cmd );
	if ( g_ctx.m_cmd && m_ladder_freelook_climb_data.m_phase != ladder_freelook_climb_data_t::phase_idle ) {
		g_ctx.m_cmd->m_view_point = c_angle( 0.f, m_ladder_freelook_climb_data.m_server_yaw, 0.f );
		return;
	}
	this->auto_one_hop( g_ctx.m_cmd );
	gnd_wish_check( "auto_one_hop" );
	if ( m_auto_one_hop_data.m_phase != auto_one_hop_data_t::phase_idle )
		return;

	const auto move_type = g_ctx.m_local->get_move_type( );
	if ( move_type == e_move_types::move_type_ladder || move_type == e_move_types::move_type_noclip || move_type == e_move_types::move_type_fly ||
	     move_type == e_move_types::move_type_observer ) {
		if ( move_type != e_move_types::move_type_observer && g_ctx.m_local->is_alive( ) ) {
			g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
			this->pixel_calc( g_ctx.m_cmd );
			this->route_calc( g_ctx.m_cmd );
			this->dist_calc( g_ctx.m_cmd );
		}
		if ( move_type == e_move_types::move_type_ladder )
			this->ladder_glide( g_ctx.m_cmd );
		return;
	}

	if ( GET_VARIABLE( g_variables.m_bunny_hop, bool ) &&
	     !( GET_VARIABLE( g_variables.m_jump_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_jump_bug_key, key_bind_t ) ) ) )
		this->bunny_hop( );

	this->delay_hop( g_ctx.m_cmd );

	if ( g_ctx.m_local ) {
		if ( g_ctx.m_local->get_health( ) != 0 && g_ctx.m_local->is_alive( ) ) {
			b_buttons                 = g_ctx.m_cmd->m_buttons;
			const int predicted_frame = g_interfaces.m_prediction->m_commands_predicted - 1;
			g_prediction.restore_entity_to_predicted_frame( predicted_frame );
			if ( GET_VARIABLE( g_variables.m_jump_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_jump_bug_key, key_bind_t ) ) )
				this->jump_bug_crouch( g_ctx.m_cmd );
			gnd_wish_check( "jb_crouch" );
			this->pixelsurf_assist( g_ctx.m_cmd );
			gnd_wish_check( "ps_assist" );
			this->pixelsurf_assist_ground_help( g_ctx.m_cmd );
			gnd_wish_check( "ps_ground_help" );
			this->autobounce_assist( g_ctx.m_cmd );
			gnd_wish_check( "bounce_assist" );

			if ( m_pixelsurf_data.should_pixel_surf )
				g_ctx.m_cmd->m_buttons |= in_duck;
			else if ( m_pixelsurf_data.should_unduck )
				g_ctx.m_cmd->m_buttons &= ~in_duck;
			if ( GET_VARIABLE( g_variables.m_auto_align, bool ) && !( g_prediction.backup_data.m_flags & 1 ) &&
			     !( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) ) {
				static long long aa_total = 0ll, aa_calls = 0ll, aa_worst = 0ll, aa_sim_total = 0ll, aa_sim_worst = 0ll;
				static unsigned long long aa_next_report = 0ull;
				LARGE_INTEGER aa_a{ }, aa_b{ }, aa_freq{ };
				QueryPerformanceFrequency( &aa_freq );
				QueryPerformanceCounter( &aa_a );
				const unsigned int aa_sim_before = g_prediction.m_sim_count;

				this->auto_align( g_ctx.m_cmd );

				const long long aa_sims_here = static_cast< long long >( g_prediction.m_sim_count - aa_sim_before );
				QueryPerformanceCounter( &aa_b );
				const long long aa_us = ( aa_b.QuadPart - aa_a.QuadPart ) * 1000000ll / ( aa_freq.QuadPart ? aa_freq.QuadPart : 1ll );
				aa_total += aa_us;
				aa_calls++;
				aa_sim_total += aa_sims_here;
				if ( aa_us > aa_worst )
					aa_worst = aa_us;
				if ( aa_sims_here > aa_sim_worst )
					aa_sim_worst = aa_sims_here;
				if ( const unsigned long long now = GetTickCount64( ); now >= aa_next_report ) {
					aa_next_report = now + 1000ull;
					if ( aa_calls )
						botox_dbg_log(
						    "AA: calls=%lld avg=%lldus worst=%lldus total=%lldus/s sims=%lld simavg=%.1f simworst=%lld gate=%d reach=%d fail=%d ipt=%.5f "
						    "nowall=%d noface=%d away=%d angle=%d pin=%d touch=%d miss=%d fine=%d park=%d unstick=%d keep=%d tb=%d stomp=%d poff=%d slant=%d lip=%d",
						    aa_calls, aa_total / aa_calls, aa_worst, aa_total, aa_sim_total,
						    static_cast< float >( aa_sim_total ) / static_cast< float >( aa_calls ), aa_sim_worst, aa_gate_skips, aa_reach_skips,
						    aa_fail_skips, n_tick::interval( ), aa_outs[ aa_out_near ], aa_outs[ aa_out_face ], aa_outs[ aa_out_away ],
						    aa_outs[ aa_out_angle ], aa_outs[ aa_out_pin ], aa_outs[ aa_out_touch ], aa_outs[ aa_out_miss ], aa_outs[ aa_out_fine ],
						    aa_outs[ aa_out_park ], aa_outs[ aa_out_unstick ], aa_outs[ aa_out_keep ], aa_outs[ aa_out_tb ], aa_outs[ aa_out_stomp ], aa_outs[ aa_out_park_off ],
							    aa_outs[ aa_out_slant ], aa_outs[ aa_out_lip ] );
					aa_total = aa_calls = aa_worst = aa_sim_total = aa_sim_worst = 0ll;
					aa_gate_skips = aa_reach_skips = aa_fail_skips = 0;
					std::fill( std::begin( aa_outs ), std::end( aa_outs ), 0 );
				}
			} else {
				/* skipped call must clear it, or a landing leaves a wall latched for pixel_surf */
				wall_detected = false;
				const auto col = g_ctx.m_local->get_collideable( );
				if ( col && !GET_VARIABLE( g_variables.m_auto_align, bool ) && GET_VARIABLE( g_variables.m_pixel_surf, bool ) &&
				     !( g_prediction.backup_data.m_flags & 1 ) && !( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) ) {
					const c_vector start = g_ctx.m_local->get_abs_origin( );
					const c_vector mins = col->get_obb_mins( ), maxs = col->get_obb_maxs( );
					c_trace_filter_world fil;
					trace_t tr;
					ray_t near_ray( start, start, mins - c_vector( 1.f, 1.f, 0.f ), maxs + c_vector( 1.f, 1.f, 0.f ) );
					g_interfaces.m_engine_trace->trace_ray( near_ray, mask_playersolid, &fil, &tr );
					for ( int k = 0; k < 16 && tr.did_hit( ) && !wall_detected; k++ ) {
						const float a = static_cast< float >( k ) * ( 3.14159265358979323846f * 2.f / 16.f );
						ray_t ray( start, c_vector( start.m_x + std::cosf( a ), start.m_y + std::sinf( a ), start.m_z ), mins, maxs );
						trace_t hit;
						g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &hit );
						wall_detected = hit.m_fraction != 1.f && hit.m_plane.m_normal.m_z == 0.f;
					}
				}
			}
			g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
			gnd_wish_check( "auto_align" );

			{
				const float pre_fwd     = g_ctx.m_cmd->m_forward_move;
				const float pre_side    = g_ctx.m_cmd->m_side_move;
				const int pre_buttons   = g_ctx.m_cmd->m_buttons;
				const c_angle pre_view  = g_ctx.m_cmd->m_view_point;

				this->air_stuck( g_ctx.m_cmd );

				g_air_stuck_holding  = m_air_stuck_data.m_air_stuck;
				g_air_stuck_owns_cmd = m_air_stuck_data.m_air_stuck || g_air_stuck_authored_view_valid ||
				                       g_ctx.m_cmd->m_forward_move != pre_fwd || g_ctx.m_cmd->m_side_move != pre_side ||
				                       g_ctx.m_cmd->m_buttons != pre_buttons || g_ctx.m_cmd->m_view_point.m_x != pre_view.m_x ||
				                       g_ctx.m_cmd->m_view_point.m_y != pre_view.m_y;
			}
			gnd_wish_check( "air_stuck" );
			this->fast_stop( g_ctx.m_cmd );
			gnd_wish_check( "fast_stop" );
			this->blockbot( g_ctx.m_cmd );
			gnd_wish_check( "blockbot" );

			this->pixel_calc( g_ctx.m_cmd );
			this->route_calc( g_ctx.m_cmd );
			this->dist_calc( g_ctx.m_cmd );
			gnd_wish_check( "calc" );
			this->fast_ladder( g_ctx.m_cmd );
			this->ladder_glide( g_ctx.m_cmd );
			this->ladder_bug( g_ctx.m_cmd );
			gnd_wish_check( "ladder" );
			this->AutoStrafe( g_ctx.m_cmd );
			gnd_wish_check( "auto_strafe" );
			this->strafe_optimizer( g_ctx.m_cmd );
			gnd_wish_check( "strafe_optimizer" );
			g_wall_climb.on_create_move( g_ctx.m_cmd );
			gnd_wish_check( "wall_climb" );
			this->route_pj_creep( g_ctx.m_cmd );
			gnd_wish_check( "pj_creep" );
			this->auto_crouch( g_ctx.m_cmd );
			gnd_wish_check( "auto_crouch" );
			m_pixelsurf_assist_t.set_point = false;

			if ( g_ctx.m_local->get_flags( ) & 1 )
				m_pixelsurf_data.should_pixel_surf = m_pixelsurf_data.should_unduck = false;
		}
	}
}
void n_movement::impl_t::apply_nulls( )
{
	if ( !g_ctx.m_cmd )
		return;
	static int prev_buttons = 0, side_win = 0, fwd_win = 0;
	const int buttons = g_ctx.m_cmd->m_buttons;
	const int pressed = buttons & ~prev_buttons;
	prev_buttons      = buttons;
	if ( pressed & in_moveleft )
		side_win = -1;
	if ( pressed & in_moveright )
		side_win = 1;
	if ( pressed & in_back )
		fwd_win = -1;
	if ( pressed & in_forward )
		fwd_win = 1;

	if ( !GET_VARIABLE( g_variables.m_nulls, bool ) )
		return;
	if ( side_win && ( buttons & ( in_moveleft | in_moveright ) ) == ( in_moveleft | in_moveright ) ) {
		g_ctx.m_cmd->m_buttons &= ~( side_win > 0 ? in_moveleft : in_moveright );
		g_ctx.m_cmd->m_side_move = static_cast< float >( side_win ) * g_convars.float_or( HASH_BT( "cl_sidespeed" ), 450.f );
	}
	if ( fwd_win && ( buttons & ( in_forward | in_back ) ) == ( in_forward | in_back ) ) {
		g_ctx.m_cmd->m_buttons &= ~( fwd_win > 0 ? in_back : in_forward );
		g_ctx.m_cmd->m_forward_move = fwd_win > 0 ? g_convars.float_or( HASH_BT( "cl_forwardspeed" ), 450.f )
		                                          : -g_convars.float_or( HASH_BT( "cl_backspeed" ), 450.f );
	}
}
void n_movement::impl_t::half_sideways( )
{
	static int prev_buttons = 0;
	const auto cmd = g_ctx.m_cmd;
	if ( !cmd || !g_ctx.m_local )
		return;
	const int last = prev_buttons;
	prev_buttons   = cmd->m_buttons;
	if ( !GET_VARIABLE( g_variables.m_hsw, bool ) || !g_input.check_input( &GET_VARIABLE( g_variables.m_hsw_key, key_bind_t ) ) ||
	     g_ctx.m_local->get_move_type( ) != e_move_types::move_type_walk )
		return;

	const bool on_ground = ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) != 0;
	const bool autohop   = GET_VARIABLE( g_variables.m_bunny_hop, bool ) || g_convars.float_or( HASH_BT( "sv_autobunnyhopping" ), 0.f ) != 0.f;
	const bool jumping   = on_ground && ( cmd->m_buttons & in_jump ) && ( autohop || !( last & in_jump ) );
	const bool grounded  = on_ground && !jumping;

	const int s   = ( GET_VARIABLE( g_variables.m_hsw_side, int ) == 0 ? 1 : -1 ) * ( grounded ? 1 : -1 );
	const float f = static_cast< float >( s ) * 450.f;

	if ( ( !grounded && GET_VARIABLE( g_variables.m_hsw_allow_w_s, bool ) ) || ( grounded && GET_VARIABLE( g_variables.m_hsw_pre_onground, bool ) ) ) {
		if ( cmd->m_buttons & in_forward ) {
			cmd->m_forward_move = 450.f;
			cmd->m_side_move    = -f;
			cmd->m_buttons |= s == 1 ? in_moveleft : in_moveright;
		}
		if ( cmd->m_buttons & in_back ) {
			cmd->m_forward_move = -450.f;
			cmd->m_side_move    = f;
			cmd->m_buttons |= s == 1 ? in_moveright : in_moveleft;
		}
	}
	if ( !grounded || ( GET_VARIABLE( g_variables.m_hsw_pre_onground, bool ) && GET_VARIABLE( g_variables.m_hsw_allow_a_d, bool ) ) ) {
		if ( cmd->m_buttons & in_moveleft ) {
			cmd->m_forward_move = f;
			cmd->m_side_move    = -450.f;
			cmd->m_buttons |= s == 1 ? in_forward : in_back;
		}
		if ( cmd->m_buttons & in_moveright ) {
			cmd->m_forward_move = -f;
			cmd->m_side_move    = 450.f;
			cmd->m_buttons |= s == 1 ? in_back : in_forward;
		}
	}
	if ( ( cmd->m_buttons & in_forward ) && ( cmd->m_buttons & in_back ) )
		cmd->m_forward_move = 0.f;
	if ( ( cmd->m_buttons & in_moveleft ) && ( cmd->m_buttons & in_moveright ) )
		cmd->m_side_move = 0.f;
}
void n_movement::impl_t::bunny_hop( )
{
	static bool was_air = false;
	if ( !( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) ) {
		g_ctx.m_cmd->m_buttons &= ~e_command_buttons::in_jump;
		was_air = true;
		return;
	}
	if ( was_air ) {
		was_air = false;
		if ( rand( ) % 100 >= GET_VARIABLE( g_variables.m_bunny_hop_chance, int ) )
			g_ctx.m_cmd->m_buttons &= ~e_command_buttons::in_jump;
	}
}

void n_movement::impl_t::delay_hop( c_user_cmd* cmd )
{
	if ( !cmd || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_delay_hop, bool ) )
		return;
	if ( !g_input.check_input( &GET_VARIABLE( g_variables.m_delay_hop_key, key_bind_t ) ) )
		return;

	const auto move_type = g_ctx.m_local->get_move_type( );
	if ( move_type == e_move_types::move_type_ladder || move_type == e_move_types::move_type_noclip ||
	     move_type == e_move_types::move_type_observer )
		return;

	static int last_air_tick = 0;
	const int wait           = GET_VARIABLE( g_variables.m_delay_hop_ticks, int );

	if ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) {
		if ( cmd->m_tick_count > last_air_tick + wait ) {
			cmd->m_buttons |= e_command_buttons::in_jump;
			last_air_tick = cmd->m_tick_count;
		} else
			cmd->m_buttons &= ~e_command_buttons::in_jump; /* still waiting: bhop's held space must not jump early */
	} else {
		last_air_tick = cmd->m_tick_count;
		cmd->m_buttons &= ~e_command_buttons::in_jump;
	}
}
static bool ps_line_covered( const c_vector& p, const char* map, const float z_tol )
{
	for ( const auto& segment : g_movement.m_pixelsurf_data.m_trajectory_segments ) {
		if ( segment.map_name != map )
			continue;

		const auto& pts = segment.points;
		for ( size_t i = 0; i < pts.size( ); ++i ) {
			const c_vector& a = pts[ i ];
			const c_vector& b = pts[ i + 1 < pts.size( ) ? i + 1 : i ];
			if ( std::fabs( a.m_z - p.m_z ) >= z_tol )
				continue;
			if ( g_movement.point_to_line_distance( c_vector_2d( p.m_x, p.m_y ), c_vector_2d( a.m_x, a.m_y ), c_vector_2d( b.m_x, b.m_y ) ) < 4.f )
				return true;
		}
	}
	return false;
}

static std::vector< std::vector< c_vector > > ps_line_uncovered_runs( const std::vector< c_vector >& pts, const char* map, const float z_tol )
{
	std::vector< std::vector< c_vector > > runs;
	std::vector< char > covered( pts.size( ) );
	for ( size_t i = 0; i < pts.size( ); ++i )
		covered[ i ] = ps_line_covered( pts[ i ], map, z_tol );

	std::vector< c_vector > run;
	for ( size_t i = 1; i < pts.size( ); ++i ) {
		if ( covered[ i - 1 ] && covered[ i ] ) {
			if ( run.size( ) >= 2 )
				runs.push_back( std::move( run ) );
			run.clear( );
			continue;
		}
		if ( run.empty( ) )
			run.push_back( pts[ i - 1 ] );
		run.push_back( pts[ i ] );
	}
	if ( run.size( ) >= 2 )
		runs.push_back( std::move( run ) );
	return runs;
}

static void ps_line_project( const std::vector< c_vector >& pts, std::vector< std::vector< ImVec2 > >& out )
{
	std::vector< ImVec2 > run;
	run.reserve( pts.size( ) );
	for ( const auto& p : pts ) {
		c_vector_2d s;
		if ( g_render.world_to_screen( p, s ) ) {
			run.emplace_back( s.m_x, s.m_y );
			continue;
		}
		if ( !run.empty( ) )
			out.push_back( std::move( run ) );
		run.clear( );
	}
	if ( !run.empty( ) )
		out.push_back( std::move( run ) );
}

bool can = false;
void n_movement::impl_t::on_paint_traverse( )
{
	can = true;
	this->pixel_finder( nullptr );
	this->pixelsurf_assist_render( );
	this->pixel_calc_render( );
	this->route_calc_render( );
	this->dist_calc_render( );
	g_edgebug.render( );
	g_edge_skip.render( );
	this->auto_bounce_render( );

	if ( GET_VARIABLE( g_variables.m_pixel_surf_line_render, bool ) && !m_pixelsurf_data.m_trajectory_segments.empty( ) ) {
		const bool is_pixelsurf_active = m_pixelsurf_data.m_predicted_succesful || m_pixelsurf_data.m_pin_detected;

		{
			const c_color line_color      = GET_VARIABLE( g_variables.m_pixel_surf_line_color, c_color );
			const float thickness         = GET_VARIABLE( g_variables.m_pixel_surf_line_thickness, float );
			const std::string current_map = g_interfaces.m_engine_client->get_level_name_short( );

			if ( m_pixelsurf_data.m_delete_mode && ImGui::IsMouseClicked( 0 ) && !ImGui::GetIO( ).WantCaptureMouse ) {
				ImVec2 mouse_pos    = g_render.screen_mouse( );
				mouse_pos.x         = g_render.to_overlay_x( mouse_pos.x );
				int closest_segment = -1;
				float min_dist      = 15.0f;

				for ( size_t seg_idx = 0; seg_idx < m_pixelsurf_data.m_trajectory_segments.size( ); ++seg_idx ) {
					const auto& segment = m_pixelsurf_data.m_trajectory_segments[ seg_idx ];
					if ( segment.map_name != current_map || segment.points.size( ) < 2 )
						continue;

					for ( size_t i = 1; i < segment.points.size( ); ++i ) {
						c_vector_2d screen_start, screen_end;
						if ( g_render.world_to_screen( segment.points[ i - 1 ], screen_start ) &&
						     g_render.world_to_screen( segment.points[ i ], screen_end ) ) {
							float dist = point_to_line_distance( c_vector_2d( mouse_pos.x, mouse_pos.y ), screen_start, screen_end );
							if ( dist < min_dist ) {
								min_dist        = dist;
								closest_segment = seg_idx;
							}
						}
					}
				}

				if ( closest_segment >= 0 )
					m_pixelsurf_data.m_trajectory_segments.erase( m_pixelsurf_data.m_trajectory_segments.begin( ) + closest_segment );
			}

			std::vector< std::vector< ImVec2 > > screen_runs;
			for ( const auto& segment : m_pixelsurf_data.m_trajectory_segments ) {
				if ( segment.map_name != current_map || segment.points.size( ) < 2 )
					continue;
				ps_line_project( segment.points, screen_runs );
			}

			static int live_tick = -1;
			static std::vector< std::vector< c_vector > > live_runs;
			const bool live_on = GET_VARIABLE( g_variables.m_pixel_surf_line_full_trajectory, bool ) && is_pixelsurf_active && g_ctx.m_local &&
			                     g_ctx.m_local->is_alive( );
			if ( !live_on ) {
				live_tick = -1;
				live_runs.clear( );
			}

			if ( live_on && live_tick != g_interfaces.m_global_vars_base->m_tick_count ) {
				live_tick = g_interfaces.m_global_vars_base->m_tick_count;
				live_runs.clear( );
				c_vector feet_pos = g_ctx.m_local->get_origin( );
				feet_pos.m_z += 4.0f; // don't start in the floor

				c_vector horiz_dir = g_ctx.m_local->get_velocity( );
				horiz_dir.m_z      = 0.f;

				if ( horiz_dir.length_2d( ) > 0.1f ) {
					horiz_dir.normalize_in_place( );

					trace_t     initial{ };
					float       closest_dist = FLT_MAX;
					bool        found_wall   = false;
					constexpr int   k_scan_dirs   = 16;
					constexpr float k_scan_radius = 32.0f;
					constexpr float k_two_pi      = 6.28318530717958647692f;

					for ( int i = 0; i < k_scan_dirs; ++i ) {
						const float ang = static_cast< float >( i ) * ( k_two_pi / k_scan_dirs );
						c_vector dir( cosf( ang ), sinf( ang ), 0.f );

						trace_t        t{ };
						ray_t          ray( feet_pos, feet_pos + dir * k_scan_radius );
						c_trace_filter flt( g_ctx.m_local );
						g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &t );

						if ( t.m_fraction < 1.0f && !t.m_start_solid ) {
							const float d = t.m_fraction * k_scan_radius;
							if ( d < closest_dist ) {
								closest_dist = d;
								initial      = t;
								found_wall   = true;
							}
						}
					}

					if ( found_wall ) {
						const c_vector wall_normal = initial.m_plane.m_normal;

						c_vector tangent = horiz_dir - wall_normal * horiz_dir.dot_product( wall_normal );

						if ( tangent.length( ) < 0.01f ) {
							tangent = c_vector( -wall_normal.m_y, wall_normal.m_x, 0.f );
							if ( tangent.dot_product( horiz_dir ) < 0.f )
								tangent = tangent * -1.0f;
						}

						if ( tangent.length( ) > 0.01f ) {
							tangent.normalize_in_place( );

							std::vector< c_vector > scan_points;
							scan_points.reserve( 48 );
							scan_points.push_back( initial.m_end );

							constexpr float k_step          = 16.0f;
							constexpr int   k_max_steps     = 48;
							constexpr float k_probe_depth   = 48.0f;
							constexpr float k_normal_cohere = 0.8f;

							c_vector probe_ref = initial.m_end;
							for ( int i = 0; i < k_max_steps; ++i ) {
								probe_ref = probe_ref + tangent * k_step;

								c_vector probe_start = probe_ref + wall_normal * k_probe_depth;
								c_vector probe_end   = probe_ref - wall_normal * k_probe_depth;

								trace_t t{ };
								ray_t ray( probe_start, probe_end );
								c_trace_filter flt2( g_ctx.m_local );
								g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt2, &t );

								if ( t.m_fraction >= 1.0f || t.m_start_solid )
									break;

								if ( t.m_plane.m_normal.dot_product( wall_normal ) < k_normal_cohere )
									break;

								scan_points.push_back( t.m_end );

								probe_ref = t.m_end + wall_normal * 0.5f;
							}

							live_runs = ps_line_uncovered_runs( scan_points, current_map.c_str( ), 6.f );
						}
					}
				}
			}
			for ( const auto& run : live_runs )
				ps_line_project( run, screen_runs );

			/* one queue object for every line: style picked on the render thread */
			const int style         = GET_VARIABLE( g_variables.m_pixel_surf_line_style, int );
			const unsigned int col  = line_color.get_u32( );
			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_callback,
				std::make_any< callback_draw_object_t >( callback_draw_object_t{ [ runs = std::move( screen_runs ), style, col, thickness ]( ImDrawList* dl ) {
					for ( const auto& run : runs ) {
						if ( style == 2 ) {
							for ( const auto& p : run )
								dl->AddCircleFilled( p, std::max( thickness, 1.5f ), col, 12 );
							continue;
						}
						if ( run.size( ) < 2 )
							continue;
						if ( style == 0 ) {
							dl->AddPolyline( run.data( ), static_cast< int >( run.size( ) ), col, ImDrawFlags_None, thickness );
							continue;
						}

						const float dash = std::max( thickness * 3.f, 4.f );
						float phase      = 0.f;
						for ( size_t i = 1; i < run.size( ); ++i ) {
							const ImVec2 a = run[ i - 1 ];
							const float dx = run[ i ].x - a.x, dy = run[ i ].y - a.y;
							const float len = std::sqrt( dx * dx + dy * dy );
							if ( len < 0.01f || len > 16384.f )
								continue;
							const float ux = dx / len, uy = dy / len;
							for ( float t = 0.f; t < len; ) {
								const bool on    = phase < dash;
								const float step = std::min( ( on ? dash : dash * 2.f ) - phase, len - t );
								if ( on )
									dl->AddLine( ImVec2( a.x + ux * t, a.y + uy * t ), ImVec2( a.x + ux * ( t + step ), a.y + uy * ( t + step ) ), col,
									             thickness );
								t += step;
								phase += step;
								if ( phase >= dash * 2.f )
									phase -= dash * 2.f;
							}
						}
					}
				} } ) );
		}
	}

	if ( g_ctx.m_local )
		if ( g_interfaces.m_engine_client->is_in_game( ) )
			if ( g_ctx.m_local->is_alive( ) ) {
				RenderPoints( m_points_check, g_ctx.m_local->get_origin( ), g_interfaces.m_engine_client->get_level_name_short( ) );
			}
	can = false;
}

n_movement::impl_t::trick_states_t n_movement::impl_t::get_trick_states( )
{
	trick_states_t states{ };

	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return states;

	const bool on_ground = ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) != 0;

	states.m_edge_bug_queued = g_edgebug.m_found;
	states.m_edge_bug_tick   = g_edgebug.m_found ? g_edgebug.m_found_tick : 0;

	states.m_texture_bug_now = g_texturebug.m_hit || g_texturebug.m_hs_hit || g_texturebug.m_hb_hit;

	states.m_air_stuck_now = m_air_stuck_data.m_air_stuck || g_air_stuck_noclip_active;

	/* wall climb: own flag, like tb. WC yields to a pinned surf, never overlaps the test below */
	states.m_wall_climb_now = g_wall_climb.m_hit;

	/* pixel surf last: air stuck also pins vz at -halfgrav and would read as a pixel surf. a texture bug face is never one */
	const bool pinned_air = m_pixelsurf_data.m_pin_detected && !on_ground && !states.m_air_stuck_now;
	const int pin_kind    = m_pixelsurf_data.m_pin_kind;
	const int start_kind  = m_pixelsurf_data.m_start_pin_kind;
	states.m_pixel_surf_now = pinned_air && !states.m_texture_bug_now && pin_kind != 2 && start_kind != 2;

	const bool start_air      = m_pixelsurf_data.m_start_pinned && !states.m_air_stuck_now;
	states.m_texture_bug_surf = start_air && ( start_kind == 2 || ( ( g_texturebug.m_hit || g_texturebug.m_hs_hit ) && start_kind == 0 ) );

	return states;
}

void n_movement::impl_t::on_create_move_post( )
{
	if ( !g_ctx.m_local )
		return;
	if ( g_ctx.m_local->get_observer_mode( ) > 0 )
		return;
	if ( g_ctx.m_local ) {
		if ( g_ctx.m_local->get_health( ) != 0 && g_ctx.m_local->is_alive( ) ) {
			const float target_ps_velocity =
				-g_convars.float_or( HASH_BT( "sv_gravity" ), 800.f ) * 0.5f * n_tick::interval( );

			static bool s_prev_start_pin = false;
			const bool start_pin = is_pixelsurf_velocity( g_prediction.backup_data.m_velocity.m_z ) &&
			                       !( g_prediction.backup_data.m_flags & e_flags::fl_onground );
			g_movement.m_pixelsurf_data.m_pin_detected =
				( is_pixelsurf_velocity( g_prediction.backup_data.m_velocity.m_z ) && is_pixelsurf_velocity( g_ctx.m_local->get_velocity( ).m_z ) ) ||
				( start_pin && s_prev_start_pin );
			s_prev_start_pin = start_pin;
			{
				constexpr float no_face = n_prediction::impl_t::k_no_face;
				auto& ps            = g_movement.m_pixelsurf_data;
				const auto kind_of  = [ ]( float dz ) { return dz == no_face ? 0 : dz > 1.f ? 2 : 1; };
				ps.m_pin_kind       = ps.m_pin_detected ? kind_of( g_prediction.m_pin_face_dz ) : 0;
				ps.m_start_pinned    = start_pin;
				ps.m_start_pin_kind  = ps.m_start_pinned ? kind_of( ps.m_sent_face_dz ) : 0;
				if ( ( ps.m_start_pinned || ps.m_pin_detected ) && GET_VARIABLE( g_variables.m_debug_log, bool ) )
					botox_dbg_log( "PINK: kind=%d dz=%.4f start=%d skind=%d sdz=%.4f tb=%d hs=%d z=%.3f", ps.m_pin_kind,
					               g_prediction.m_pin_face_dz == no_face ? -1.f : g_prediction.m_pin_face_dz, ( int )ps.m_start_pinned,
					               ps.m_start_pin_kind, ps.m_sent_face_dz == no_face ? -1.f : ps.m_sent_face_dz, ( int )g_texturebug.m_hit,
					               ( int )g_texturebug.m_hs_hit, g_ctx.m_local->get_origin( ).m_z );
				ps.m_sent_face_dz = g_prediction.m_pin_face_dz;
			}

			/* m_in_pixel_surf = indicator ( vz pinned now ), never a latch; get_trick_states reads m_pin_detected */
			g_movement.m_pixelsurf_data.m_in_pixel_surf = g_movement.m_pixelsurf_data.m_pin_detected;

			{
				PERF_ZONE( zone_cmd_detections );
				this->run_detections( );
				this->jump_stats( );
			}
			g_wall_climb.m_hit = false;

			const bool fireman_grab = m_fireman_data.fell_ready && g_ctx.m_cmd &&
			                          static_cast< unsigned >( g_ctx.m_cmd->m_tick_count - m_fireman_data.grab_tick ) <= 2u;
			const bool fireman_busy = m_fireman_data.is_ladder || m_fireman_data.launch_ticks > 0 || fireman_grab;

			m_edge_jump_data.m_ladder_detected = false;
			const bool wc_catch = g_wall_climb.caught( g_ctx.m_cmd );
			{
				PERF_ZONE( zone_cmd_detections );
				if ( !fireman_busy && !wc_catch && GET_VARIABLE( g_variables.m_edge_jump, bool ) &&
				     g_input.check_input( &GET_VARIABLE( g_variables.m_edge_jump_key, key_bind_t ) ) )
					this->edge_jump( );
				else
					m_edge_jump_data.m_detected = false;
				if ( !wc_catch && GET_VARIABLE( g_variables.m_jump_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_jump_bug_key, key_bind_t ) ) ) {
					if ( GET_VARIABLE( g_variables.m_jump_bug_kangaroo, bool ) )
						this->kangaroo( );
					else
						this->jump_bug( );
				}
			}

			if ( GET_VARIABLE( g_variables.m_pixel_surf_line_render, bool ) && g_ctx.m_local ) {
				PERF_ZONE( zone_cmd_detections );
				const bool now_in_surf = get_trick_states( ).m_pixel_surf_now;
				c_vector current_pos   = g_ctx.m_local->get_origin( );

				if ( now_in_surf && !g_movement.m_pixelsurf_data.m_was_in_surf ) {
					c_vector mid_pos = current_pos;

					c_vector wall_pos = mid_pos;
					c_vector velocity = g_ctx.m_local->get_velocity( );
					velocity.m_z      = 0.f;

					bool     surface_found = false;
					c_vector wall_normal{ };
					c_vector tangent{ };

					constexpr int   k_angle_steps     = 32;
					constexpr float k_probe_dist      = 128.0f;
					constexpr float k_normal_z_eps    = 0.35f;
					constexpr float k_facing_thresh   = -0.1f;
					constexpr float k_two_pi          = 6.28318530717958647692f;

					float          best_fraction = 1.0f;
					c_trace_filter flt_scan( g_ctx.m_local );

					for ( int i = 0; i < k_angle_steps; ++i ) {
						const float    ang = static_cast< float >( i ) / static_cast< float >( k_angle_steps ) * k_two_pi;
						const c_vector dir( std::cos( ang ), std::sin( ang ), 0.f );
						const c_vector end_pos = mid_pos + dir * k_probe_dist;

						trace_t t{ };
						ray_t   ray( mid_pos, end_pos );
						g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt_scan, &t );

						if ( t.m_fraction >= 1.0f )
							continue;
						if ( std::fabs( t.m_plane.m_normal.m_z ) > k_normal_z_eps )
							continue;
						// normal must face against the ray, else it's the back face of the brush
						if ( t.m_plane.m_normal.m_x * dir.m_x + t.m_plane.m_normal.m_y * dir.m_y > k_facing_thresh )
							continue;
						if ( t.m_fraction >= best_fraction )
							continue;

						best_fraction = t.m_fraction;
						wall_pos      = t.m_end;
						wall_normal   = t.m_plane.m_normal;
						surface_found = true;
					}

					if ( surface_found ) {
						tangent = velocity - wall_normal * velocity.dot_product( wall_normal );

						if ( tangent.length( ) < 0.01f )
							tangent = c_vector( -wall_normal.m_y, wall_normal.m_x, 0.f );

						if ( tangent.length( ) > 0.01f ) {
							tangent.normalize_in_place( );
							if ( velocity.length_2d( ) > 1.0f && tangent.dot_product( velocity ) < 0.f )
								tangent = tangent * -1.0f;
						} else {
							surface_found = false;
						}
					}

					constexpr float k_test_al     = 15.97803f;
					constexpr float k_step        = 16.0f;
					constexpr int   k_max_steps   = 128;
					constexpr float k_probe_depth = 48.0f;
					const float     k_half_grav   = get_half_gravity_per_tick( );
					const int       k_pred_frame  = g_interfaces.m_prediction->m_commands_predicted - 1;
					const float     k_surf_z      = current_pos.m_z;

					const c_vector saved_origin   = g_ctx.m_local->get_origin( );
					const c_vector saved_abs      = g_ctx.m_local->get_abs_origin( );
					const int      saved_buttons  = g_ctx.m_cmd ? g_ctx.m_cmd->m_buttons : 0;
					const float    saved_fwd_move = g_ctx.m_cmd ? g_ctx.m_cmd->m_forward_move : 0.f;
					const float    saved_side_mv  = g_ctx.m_cmd ? g_ctx.m_cmd->m_side_move : 0.f;

					auto is_solution_at = [ & ]( const c_vector& candidate_origin ) -> bool {
						if ( !g_ctx.m_cmd )
							return false;
						g_prediction.restore_entity_to_predicted_frame( k_pred_frame );
						*( int* )( ( uintptr_t )( c_base_entity* )g_ctx.m_local + 0x25C ) = 2;
						g_ctx.m_cmd->m_buttons |= in_jump;
						g_ctx.m_cmd->m_buttons |= in_duck;
						const c_vector ang{ wall_normal.m_x * -1.f, wall_normal.m_y * -1.f, 0.f };
						const c_angle  to_wall  = ang.to_angle( );
						const float    rotation = deg2rad( to_wall.m_y - g_ctx.m_cmd->m_view_point.m_y );
						g_ctx.m_cmd->m_forward_move = cosf( rotation ) * 10.f;
						g_ctx.m_cmd->m_side_move    = -sinf( rotation ) * 10.f;

						g_ctx.m_local->get_origin( ) = candidate_origin;
						g_ctx.m_local->set_abs_origin( candidate_origin );

						g_prediction.begin( g_ctx.m_local, g_ctx.m_cmd );
						g_prediction.end( g_ctx.m_local );

						return std::abs( g_ctx.m_local->get_velocity( ).m_z - k_half_grav ) < 3.0f;
					};

					auto scan_along_wall = [ & ]( const c_vector& dir ) -> std::vector< c_vector > {
						std::vector< c_vector > out;
						c_vector probe_ref = wall_pos;
						c_vector last_pt   = wall_pos;
						for ( int i = 0; i < k_max_steps; ++i ) {
							probe_ref = probe_ref + dir * k_step;

							c_vector probe_start = probe_ref + wall_normal * k_probe_depth;
							c_vector probe_end   = probe_ref - wall_normal * k_probe_depth;
							trace_t  t{ };
							ray_t    ray2( probe_start, probe_end );
							c_trace_filter flt2( g_ctx.m_local );
							g_interfaces.m_engine_trace->trace_ray( ray2, mask_playersolid, &flt2, &t );
							if ( t.m_fraction >= 1.0f || t.m_start_solid )
								break;
							if ( t.m_plane.m_normal.dot_product( wall_normal ) < 0.95f )
								break;

							const c_vector delta = t.m_end - last_pt;
							if ( delta.length_2d( ) > k_step * 1.5f )
								break;
							const float normal_jump = std::fabs( delta.dot_product( wall_normal ) );
							if ( normal_jump > 1.0f )
								break;

							auto sign_or_zero = []( float v ) -> float {
								if ( v < -0.001f ) return -1.f;
								if ( v > 0.001f ) return 1.f;
								return 0.f;
							};
							c_vector candidate( t.m_end.m_x + sign_or_zero( wall_normal.m_x ) * k_test_al,
							                    t.m_end.m_y + sign_or_zero( wall_normal.m_y ) * k_test_al, k_surf_z );

							const bool predict_ok = is_solution_at( candidate );
							if ( !predict_ok && i > 0 )
								break;

							out.push_back( t.m_end );
							last_pt   = t.m_end;
							probe_ref = t.m_end + wall_normal * 0.5f;

							if ( !predict_ok )
								break;
						}
						return out;
					};

					const char* entry_map = g_interfaces.m_engine_client->get_level_name_short( );

					if ( !ps_line_covered( wall_pos, entry_map, 2.f ) ) {
						trajectory_segment_t new_segment;
						new_segment.map_name = entry_map;

						if ( surface_found ) {
							const auto forward  = scan_along_wall( tangent );
							const auto backward = scan_along_wall( tangent * -1.0f );

							g_prediction.restore_entity_to_predicted_frame( k_pred_frame );
							g_ctx.m_local->get_origin( ) = saved_origin;
							g_ctx.m_local->set_abs_origin( saved_abs );
							if ( g_ctx.m_cmd ) {
								g_ctx.m_cmd->m_buttons      = saved_buttons;
								g_ctx.m_cmd->m_forward_move = saved_fwd_move;
								g_ctx.m_cmd->m_side_move    = saved_side_mv;
							}

							new_segment.points.reserve( backward.size( ) + 1 + forward.size( ) );
							for ( auto it = backward.rbegin( ); it != backward.rend( ); ++it )
								new_segment.points.push_back( *it );
							new_segment.points.push_back( wall_pos );
							for ( const auto& p : forward )
								new_segment.points.push_back( p );
						} else {
							new_segment.points.push_back( wall_pos );
						}

						auto& segments = g_movement.m_pixelsurf_data.m_trajectory_segments;
						if ( new_segment.points.size( ) < 2 ) {
							segments.push_back( std::move( new_segment ) );
						} else {
							// never draw over a saved line: keep only the stretches it doesn't cover
							for ( auto& run : ps_line_uncovered_runs( new_segment.points, entry_map, 2.f ) )
								segments.push_back( { std::move( run ), new_segment.map_name } );
						}
					}
					g_movement.m_pixelsurf_data.m_last_position = wall_pos;
				}

				g_movement.m_pixelsurf_data.m_was_in_surf = now_in_surf;
			}

			{
				static bool prev_in_pixel_surf = false;
				static bool prev_lirili_key    = false;
				const bool now_ps              = g_movement.m_pixelsurf_data.m_pin_detected;
				const bool lirili_key          = GET_VARIABLE( g_variables.m_lirili_larila, bool ) &&
				                        g_input.check_input( &GET_VARIABLE( g_variables.m_lirili_larila_key, key_bind_t ) );
				if ( lirili_key && !prev_lirili_key )
					g_interfaces.m_engine_client->execute_client_cmd( "y6_ast 1" );
				else if ( !lirili_key && prev_lirili_key )
					g_interfaces.m_engine_client->execute_client_cmd( "y6_ast 0" );
				if ( GET_VARIABLE( g_variables.m_lirili_larila, bool ) && now_ps && !prev_in_pixel_surf && lirili_key && g_ctx.m_local ) {
					m_lirili_larila_pending_origin = g_ctx.m_local->get_origin( );
					m_lirili_larila_pending_render = true;
				}
				prev_in_pixel_surf = now_ps;
				prev_lirili_key    = lirili_key;
			}
			const bool aoh_active             = m_auto_one_hop_data.m_phase != auto_one_hop_data_t::phase_idle;
			const bool ladder_freelook_active = m_ladder_freelook_climb_data.m_phase != ladder_freelook_climb_data_t::phase_idle;
			const auto move_type              = g_prediction.backup_data.m_move_type;
			if ( move_type != e_move_types::move_type_noclip && move_type != e_move_types::move_type_fly &&
			     move_type != e_move_types::move_type_observer ) {
				if ( !aoh_active && !ladder_freelook_active ) {
					gnd_wish_check( "post_start" );
					{
						PERF_ZONE( zone_cmd_texturebug );
						g_texturebug.on_create_move( g_ctx.m_cmd );
					}
					if ( g_texturebug.m_resimmed )
						m_pixelsurf_data.m_sent_face_dz = g_texturebug.m_sent_face_dz;
					gnd_wish_check( "texture_bug" );
					m_air_stuck_data.m_texture_bug_detect   = g_texturebug.m_hit || g_texturebug.m_hs_hit || g_texturebug.m_hb_hit;
					m_air_stuck_data.m_texture_bug_owns_cmd = g_texturebug.m_owns_cmd;
					/* a WC catch / proven hop / head bounce is simmed as sent: lj / mj's duck would change it unsimmed
					   (air duck = head -9, the proven bounce never lands) */
					const bool wc_caught = g_wall_climb.caught( g_ctx.m_cmd ) || g_texturebug.m_hb_hit;
					if ( !wc_caught && !fireman_busy && GET_VARIABLE( g_variables.m_long_jump, bool ) &&
					     g_input.check_input( &GET_VARIABLE( g_variables.m_long_jump_key, key_bind_t ) ) )
						this->long_jump( );
					else
						m_long_jump_data.m_detected = m_long_jump_data.m_ducking = false;
					this->adaptive_key_cancel( );
					if ( !wc_caught && GET_VARIABLE( g_variables.m_mini_jump, bool ) &&
					     g_input.check_input( &GET_VARIABLE( g_variables.m_mini_jump_key, key_bind_t ) ) )
						this->mini_jump( );
					if ( GET_VARIABLE( g_variables.m_pixel_surf_fix, bool ) )
						this->pixel_surf_fix( );
					gnd_wish_check( "pixel_surf_fix" );
					{
						PERF_ZONE( zone_cmd_fireman );
						this->fire_man( g_ctx.m_cmd );
					}
					gnd_wish_check( "fire_man" );
					m_auto_bounce_data.m_owns_cmd = false; /* stale true on a skipped tick eats movement_fix */
					{
						PERF_ZONE( zone_cmd_auto_bounce );
						this->auto_bounce( g_ctx.m_cmd, m_fireman_data.owns_cmd || m_fireman_data.is_ladder || g_air_stuck_owns_cmd ||
						                                    g_texturebug.m_owns_cmd || g_texturebug.m_hit || g_texturebug.m_hb_hit ||
						                                    g_wall_climb.caught( g_ctx.m_cmd ) );
					}
					gnd_wish_check( "auto_bounce" );
					{
						PERF_ZONE( zone_cmd_pixel_surf );
						this->pixel_surf( target_ps_velocity );
					}
					gnd_wish_check( "pixel_surf" );
				}

				if ( !g_texturebug.m_hit && !g_texturebug.m_acted && !m_fireman_data.is_ladder && !m_fireman_data.owns_cmd &&
				     !m_auto_bounce_data.m_owns_cmd && !g_wall_climb.caught( g_ctx.m_cmd ) ) {
					if ( !g_air_stuck_owns_cmd ) {
						this->movement_fix( g_ctx.old_view_point );
					} else if ( g_air_stuck_authored_view_valid && g_ctx.m_cmd ) {
						const float view_drift =
							std::fabsf( std::remainder( g_ctx.m_cmd->m_view_point.m_y - g_air_stuck_authored_view.m_y, 360.0f ) );
						if ( view_drift > 0.001f )
							this->movement_fix( g_air_stuck_authored_view );
					}
				}
			}
			gnd_wish_check( "movement_fix", true );
			if ( !aoh_active && !ladder_freelook_active ) {
				this->anti_ladder( g_ctx.m_cmd );
			}
			gnd_wish_check( "anti_ladder", true );
		}
	}
}
void n_movement::impl_t::detect_edgebug( c_user_cmd* cmd )
{
	bool edgebug_active = GET_VARIABLE( g_variables.edge_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) );
	if ( !edgebug_active ) {
		m_edgebug_data.m_will_edgebug = false;
		m_edgebug_data.m_will_fail    = true;
		return;
	}
	bool jumpbug_active =
		GET_VARIABLE( g_variables.m_jump_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_jump_bug_key, key_bind_t ) );
	bool on_ladder = g_ctx.m_local->get_move_type( ) == move_type_ladder;
	bool fireman_active =
		GET_VARIABLE( g_variables.m_fire_man, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_fire_man_key, key_bind_t ) );
	bool ladderbug_active =
		GET_VARIABLE( g_variables.m_ladder_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_ladder_bug_key, key_bind_t ) );
	if ( jumpbug_active ) {
		m_edgebug_data.m_will_edgebug = false;
		m_edgebug_data.m_will_fail    = true;
		return;
	}
	if ( on_ladder && ( fireman_active || ladderbug_active ) ) {
		m_edgebug_data.m_will_edgebug = false;
		m_edgebug_data.m_will_fail    = true;
		return;
	}
	if ( g_prediction.backup_data.m_velocity.m_z > 0 || g_utilities.is_in< int >( g_ctx.m_local->get_move_type( ), invalid_move_types ) ) {
		m_edgebug_data.m_will_edgebug = false;
		m_edgebug_data.m_will_fail    = true;
		return;
	}
	if ( round( g_ctx.m_local->get_velocity( ).m_z ) == 0 || g_prediction.backup_data.m_flags & fl_onground ) {
		m_edgebug_data.m_will_edgebug = false;
		m_edgebug_data.m_will_fail    = true;
		return;
	}
	const float vz_s = eb_indicator_tick_scale( );
	if ( g_prediction.backup_data.m_velocity.m_z < -100.f * vz_s && g_ctx.m_local->get_velocity( ).m_z > g_prediction.backup_data.m_velocity.m_z &&
	     g_ctx.m_local->get_velocity( ).m_z < -5.f * vz_s && !( g_ctx.m_local->get_flags( ) & fl_onground ) &&
	     g_prediction.backup_data.m_origin.m_z > g_ctx.m_local->get_abs_origin( ).m_z ) {
		float velocity_change   = g_ctx.m_local->get_velocity( ).m_z - g_prediction.backup_data.m_velocity.m_z;
		bool sharp_change       = velocity_change > 15.0f * vz_s;
		bool small_horizontal   = g_ctx.m_local->get_velocity( ).length_2d( ) < 300.0f;
		bool slow_fall          = g_ctx.m_local->get_velocity( ).m_z > -10.0f * vz_s && g_ctx.m_local->get_velocity( ).m_z < -3.0f * vz_s;
		bool not_jump_bug_style = !( g_ctx.m_local->get_velocity( ).m_z < 400.0f && g_ctx.m_local->get_velocity( ).length_2d( ) > 200.0f );
		bool not_fireman_style =
			!( g_prediction.backup_data.m_velocity.length_2d( ) < 200.0f && g_ctx.m_local->get_velocity( ).length_2d( ) > 250.0f );
		bool not_surf_style = !( fabsf( g_ctx.m_local->get_velocity( ).m_z / vz_s - ( -6.25f ) ) < 1.0f );
		if ( std::floor( g_prediction.backup_data.m_velocity.m_z / vz_s ) < -101 && std::floor( g_ctx.m_local->get_velocity( ).m_z / vz_s ) == -7 &&
		     g_ctx.m_local->get_velocity( ).length_2d( ) >= g_prediction.backup_data.m_velocity.length_2d( ) && sharp_change && small_horizontal &&
		     slow_fall && not_jump_bug_style && not_fireman_style && not_surf_style ) {
			m_edgebug_data.m_will_edgebug = true;
			m_edgebug_data.m_will_fail    = false;
		} else {
			float previous_velocity = g_ctx.m_local->get_velocity( ).m_z;
			for ( int i = 0; i < 4; i++ ) {
				g_prediction.begin( g_ctx.m_local, cmd );
				g_prediction.end( g_ctx.m_local );
			}
			static auto sv_gravity_cvar = g_interfaces.m_convar->find_var( "sv_gravity" );
			float gravity               = 800.f;
			if ( sv_gravity_cvar ) {
				gravity = sv_gravity_cvar->get_float( );
			}
			float expected_vertical_velocity = std::roundf( ( -gravity ) * g_interfaces.m_global_vars_base->m_interval_per_tick + previous_velocity );
			const bool sim_on_ladder         = g_ctx.m_local->get_move_type( ) == move_type_ladder;
			bool velocity_match              = !sim_on_ladder && expected_vertical_velocity == std::roundf( g_ctx.m_local->get_velocity( ).m_z );
			bool still_stable_conditions = sharp_change && small_horizontal && slow_fall && not_jump_bug_style && not_fireman_style && not_surf_style;
			m_edgebug_data.m_will_edgebug = velocity_match && still_stable_conditions;
			m_edgebug_data.m_will_fail    = !( velocity_match && still_stable_conditions );
		}
	} else {
		m_edgebug_data.m_will_edgebug = false;
		m_edgebug_data.m_will_fail    = true;
	}
}
void n_movement::impl_t::edgebug_data_t::reset( )
{
	g_movement.m_edgebug_data.m_edgebug_method = edgebug_type_t::eb_standing;
	g_movement.m_edgebug_data.m_last_tick      = 0;
	g_movement.m_edgebug_data.m_ticks_to_stop  = 0;
	g_movement.m_edgebug_data.m_will_edgebug   = false;
	g_movement.m_edgebug_data.m_will_fail      = false;
}
void n_movement::impl_t::pixelsurf_data_t::reset( )
{
	g_movement.m_pixelsurf_data.m_predicted_succesful = false;
	g_movement.m_pixelsurf_data.m_in_pixel_surf       = false;
	g_movement.m_pixelsurf_data.m_should_duck         = false;
	g_movement.m_pixelsurf_data.m_prediction_ticks    = 0;
	g_movement.m_pixelsurf_data.m_was_in_surf         = false;
	g_movement.m_pixelsurf_data.should_pixel_surf     = false;
	g_movement.m_pixelsurf_data.should_unduck         = false;
}

float n_movement::impl_t::point_to_line_distance( const c_vector_2d& point, const c_vector_2d& line_start, const c_vector_2d& line_end )
{
	float dx        = line_end.m_x - line_start.m_x;
	float dy        = line_end.m_y - line_start.m_y;
	float length_sq = dx * dx + dy * dy;

	if ( length_sq == 0.0f )
		return point.distance( line_start );

	float t = ( ( point.m_x - line_start.m_x ) * dx + ( point.m_y - line_start.m_y ) * dy ) / length_sq;
	t       = std::max( 0.0f, std::min( 1.0f, t ) );

	c_vector_2d projection( line_start.m_x + t * dx, line_start.m_y + t * dy );
	return point.distance( projection );
}
void n_movement::impl_t::movement_fix( const c_angle& old_view_point )
{
	c_vector forward, right, up;
	g_math.angle_vectors( old_view_point, &forward, &right, &up );
	c_vector old_forward, old_right, old_up;
	g_math.angle_vectors( g_ctx.m_cmd->m_view_point, &old_forward, &old_right, &old_up );
	forward.m_z = right.m_z = 0.0f;
	old_forward.m_z = old_right.m_z = 0.0f;
	up.m_x = up.m_y = old_up.m_x = old_up.m_y = 0.0f;
	const float forward_len                   = sqrtf( forward.m_x * forward.m_x + forward.m_y * forward.m_y );
	const float right_len                     = sqrtf( right.m_x * right.m_x + right.m_y * right.m_y );
	const float old_forward_len               = sqrtf( old_forward.m_x * old_forward.m_x + old_forward.m_y * old_forward.m_y );
	const float old_right_len                 = sqrtf( old_right.m_x * old_right.m_x + old_right.m_y * old_right.m_y );
	if ( forward_len > 0.001f ) {
		forward.m_x /= forward_len;
		forward.m_y /= forward_len;
	}
	if ( right_len > 0.001f ) {
		right.m_x /= right_len;
		right.m_y /= right_len;
	}
	if ( old_forward_len > 0.001f ) {
		old_forward.m_x /= old_forward_len;
		old_forward.m_y /= old_forward_len;
	}
	if ( old_right_len > 0.001f ) {
		old_right.m_x /= old_right_len;
		old_right.m_y /= old_right_len;
	}
	if ( fabsf( up.m_z ) > 0.001f )
		up.m_z = up.m_z > 0.0f ? 1.0f : -1.0f;
	if ( fabsf( old_up.m_z ) > 0.001f )
		old_up.m_z = old_up.m_z > 0.0f ? 1.0f : -1.0f;
	const float original_forward = g_ctx.m_cmd->m_forward_move;
	const float original_side    = g_ctx.m_cmd->m_side_move;
	const float original_up      = g_ctx.m_cmd->m_up_move;
	const float input_magnitude  = sqrtf( original_forward * original_forward + original_side * original_side );
	const float world_forward_x  = forward.m_x * original_forward + right.m_x * original_side;
	const float world_forward_y  = forward.m_y * original_forward + right.m_y * original_side;
	const float world_up_z       = up.m_z * original_up;
	const float cos_delta        = old_forward.m_x * forward.m_x + old_forward.m_y * forward.m_y;
	const float sin_delta        = old_forward.m_x * forward.m_y - old_forward.m_y * forward.m_x;
	const float rotated_x        = world_forward_x * cos_delta + world_forward_y * sin_delta;
	const float rotated_y        = -world_forward_x * sin_delta + world_forward_y * cos_delta;
	const float new_forward      = old_forward.m_x * rotated_x + old_forward.m_y * rotated_y + old_up.m_z * world_up_z;
	const float new_side         = old_right.m_x * rotated_x + old_right.m_y * rotated_y;
	const float output_magnitude = sqrtf( new_forward * new_forward + new_side * new_side );
	const float scale_factor = ( output_magnitude > 0.001f && input_magnitude > 0.001f ) ? fminf( 1.0f, input_magnitude / output_magnitude ) : 1.0f;
	const float cached_forward_speed = g_convars.float_or( HASH_BT( "cl_forwardspeed" ), 450.f );
	const float cached_side_speed    = g_convars.float_or( HASH_BT( "cl_sidespeed" ), 450.f );
	const float final_forward        = new_forward * scale_factor;
	const float final_side           = new_side * scale_factor;
	g_ctx.m_cmd->m_forward_move      = std::clamp( final_forward, -cached_forward_speed, cached_forward_speed );
	g_ctx.m_cmd->m_side_move         = std::clamp( final_side, -cached_side_speed, cached_side_speed );
}
void n_movement::impl_t::on_frame_stage_notify( int stage )
{
	if ( stage != e_client_frame_stage::start )
		return;
}
void n_movement::impl_t::on_end_scene( )
{
	RenderHitmarker( );
	render_point_settings_window( );
	route_calc_ui( );
	pixel_calc_ui( );
	float deltaTime = ImGui::GetIO( ).DeltaTime;
	myWindowDetect.Update( deltaTime );
	ImDrawList* draw = ImGui::GetBackgroundDrawList( );
	const auto block = g_render.begin_stretch_block( draw );
	myWindowDetect.Draw( draw, ImVec2( g_ctx.m_width, g_ctx.m_height ) );
	g_render.end_stretch_block( block );
}

void n_movement::impl_t::edge_jump( )
{
	const auto move_type = g_prediction.backup_data.m_move_type;
	if ( move_type == e_move_types::move_type_noclip || move_type == e_move_types::move_type_fly || move_type == e_move_types::move_type_observer )
		return;
	static int saved_tick{ };
	const int ej_window = n_tick::ticks( 15 );
	if ( g_interfaces.m_global_vars_base->m_tick_count - saved_tick > 1 && g_interfaces.m_global_vars_base->m_tick_count - saved_tick < ej_window ) {
		g_ctx.m_cmd->m_forward_move = 0.f;
		g_ctx.m_cmd->m_side_move    = 0.f;
		g_ctx.m_cmd->m_buttons      = g_ctx.m_cmd->m_buttons & ~( in_forward | in_back | in_moveleft | in_moveleft );
		g_ctx.m_cmd->m_buttons |= in_duck;
	}
	if ( move_type == e_move_types::move_type_ladder ) {
		if ( !GET_VARIABLE( g_variables.m_edge_jump_ladder, bool ) )
			return;
		const int pre = g_ctx.m_local->get_move_type( );
		g_prediction.begin( g_ctx.m_local, g_ctx.m_cmd );
		g_prediction.end( g_ctx.m_local );
		const int post = g_ctx.m_local->get_move_type( );
		if ( pre == e_move_types::move_type_ladder ) {
			if ( post != e_move_types::move_type_ladder ) {
				saved_tick = g_interfaces.m_global_vars_base->m_tick_count;
				m_edge_jump_data.m_ladder_detected = true;
				g_ctx.m_cmd->m_buttons |= in_jump;
				g_ctx.m_cmd->m_forward_move = 0.f;
				g_ctx.m_cmd->m_side_move    = 0.f;
				g_ctx.m_cmd->m_buttons      = g_ctx.m_cmd->m_buttons & ~( in_forward | in_back | in_moveleft | in_moveleft );
			}
		}
		if ( g_interfaces.m_global_vars_base->m_tick_count - saved_tick > 1 && g_interfaces.m_global_vars_base->m_tick_count - saved_tick < ej_window ) {
			g_ctx.m_cmd->m_forward_move = 0.f;
			g_ctx.m_cmd->m_side_move    = 0.f;
			g_ctx.m_cmd->m_buttons      = g_ctx.m_cmd->m_buttons & ~( in_forward | in_back | in_moveleft | in_moveleft );
			g_ctx.m_cmd->m_buttons |= in_duck;
		}
		return;
	}
	if ( ( g_prediction.backup_data.m_flags & e_flags::fl_onground ) && !( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) ) {
		g_ctx.m_cmd->m_buttons |= e_command_buttons::in_jump;
		m_edge_jump_data.m_detected = true;
	} else {
		m_edge_jump_data.m_detected = false;
	}
}
bool n_movement::impl_t::get_closest_wall_normal( c_vector& out_normal )
{
	if ( !g_ctx.m_local )
		return false;

	const auto start_pos = g_ctx.m_local->get_abs_origin( );

	const auto mins = g_ctx.m_local->get_collideable( )->get_obb_mins( );
	const auto maxs = g_ctx.m_local->get_collideable( )->get_obb_maxs( );

	constexpr float step = ( std::numbers::pi_v< float > * 2.f ) / 16.f;

	for ( float angle = 0.f; angle < ( std::numbers::pi_v< float > * 2.f ); angle += step ) {
		c_vector direction( cosf( angle ), sinf( angle ), 0.f );
		c_vector end_pos = start_pos + ( direction * 32.f );

		trace_t trace;
		c_trace_filter filter( g_ctx.m_local );
		ray_t ray( start_pos, end_pos, mins, maxs );

		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &trace );

		if ( trace.m_fraction < 1.0f ) {
			out_normal = trace.m_plane.m_normal;
			return true;
		}
	}
	return false;
}

void n_movement::impl_t::air_stuck_serverside( c_user_cmd* cmd )
{
	static bool prev_key_pressed = false;
	static bool air_stuck_active = false;

	if ( !GET_VARIABLE( g_variables.m_air_stuck, bool ) ) {
		if ( air_stuck_active ) {
			g_interfaces.m_engine_client->execute_client_cmd( "sm_noclip @me" );
			air_stuck_active          = false;
			g_air_stuck_noclip_active = false;
		}
		m_air_stuck_data.reset( );
		prev_key_pressed = false;
		return;
	}

	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || !cmd ) {
		if ( air_stuck_active ) {
			g_interfaces.m_engine_client->execute_client_cmd( "sm_noclip @me" );
			air_stuck_active          = false;
			g_air_stuck_noclip_active = false;
		}
		prev_key_pressed = false;
		return;
	}

	auto& key_bind = GET_VARIABLE( g_variables.m_air_stuck_key, key_bind_t );

	bool key_pressed = false;
	if ( key_bind.m_key != 0 ) {
		key_pressed = g_input.check_input( &key_bind );
	}

	bool on_ground = g_ctx.m_local->get_flags( ) & e_flags::fl_onground;

	if ( key_pressed && !prev_key_pressed && !on_ground ) {
		g_interfaces.m_engine_client->execute_client_cmd( "sm_noclip @me" );
		air_stuck_active = true;
		g_air_stuck_noclip_active = true;
	} else if ( !key_pressed && prev_key_pressed && air_stuck_active ) {
		g_interfaces.m_engine_client->execute_client_cmd( "sm_noclip @me" );
		air_stuck_active          = false;
		g_air_stuck_noclip_active = false;
		m_air_stuck_data.reset( );
	}

	if ( air_stuck_active ) {
		m_air_stuck_data.reset( );
		cmd->m_forward_move = 0.f;
		cmd->m_side_move    = 0.f;
		cmd->m_up_move      = 0.f;
		cmd->m_buttons &= ~( in_forward | in_back | in_moveleft | in_moveright | in_jump | in_duck );
	}

	prev_key_pressed = key_pressed;
}

void n_movement::impl_t::pixel_surf_fix( )
{
	if ( !g_ctx.m_local )
		return;
	if ( !g_ctx.m_local->is_alive( ) )
		return;
	if ( g_prediction.backup_data.m_velocity.m_z > 0.f )
		return;
	if ( g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_surf_assist_key, key_bind_t ) ) )
		return;
	if ( g_input.check_input( &GET_VARIABLE( g_variables.m_bounce_assist_key, key_bind_t ) ) )
		return;
	if ( g_prediction.backup_data.m_velocity.length_2d( ) >= 285.91f ) {
		if ( g_ctx.m_local->get_flags( ) & 1 ) {
			static auto sv_airAcelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
			if ( !sv_airAcelerate ) {
				return;
			}
			float Razn                  = ( ( g_prediction.backup_data.m_velocity.length_2d( ) + 2.f - 285.91f ) / 12.f * n_tick::rate( ) );
			c_vector velocity           = g_prediction.backup_data.m_velocity * -1.f;
			velocity.m_z                = 0.f;
			float rotation              = deg2rad( velocity.to_angle2( ).m_y - g_ctx.m_cmd->m_view_point.m_y );
			float cos_rot               = cos( rotation );
			float sin_rot               = sin( rotation );
			float forwardmove           = cos_rot * Razn;
			float sidemove              = -sin_rot * Razn;
			g_ctx.m_cmd->m_forward_move = forwardmove;
			g_ctx.m_cmd->m_side_move    = sidemove;
		}
	}
}
void n_movement::impl_t::long_jump( )
{
	static int saved_tick  = 0;
	const bool left_ground = ( g_prediction.backup_data.m_flags & e_flags::fl_onground ) && !( g_ctx.m_local->get_flags( ) & e_flags::fl_onground );
	const bool jumped_this_tick = ( g_ctx.m_cmd->m_buttons & e_command_buttons::in_jump ) != 0;
	const bool edge_jump        = GET_VARIABLE( g_variables.m_long_jump_edge_jump, bool );
	const bool on_ladder        = edge_jump && GET_VARIABLE( g_variables.m_long_jump_edge_jump_ladder, bool ) &&
	                       g_prediction.backup_data.m_move_type == e_move_types::move_type_ladder;
	const bool now_ladder       = g_ctx.m_local->get_move_type( ) == e_move_types::move_type_ladder;
	/* ladder jump lives in LadderMove: on the exit tick the trace misses first, so jump the tick before (edge_jump idiom) */
	static int ladder_jump_tick = 0;
	bool left_ladder            = false;
	if ( on_ladder && now_ladder ) {
		if ( m_edge_jump_data.m_ladder_detected )
			left_ladder = true;
		else {
			g_prediction.begin( g_ctx.m_local, g_ctx.m_cmd );
			g_prediction.end( g_ctx.m_local );
			left_ladder = g_ctx.m_local->get_move_type( ) != e_move_types::move_type_ladder;
		}
		if ( left_ladder ) {
			ladder_jump_tick            = g_interfaces.m_global_vars_base->m_tick_count;
			g_ctx.m_cmd->m_forward_move = 0.f;
			g_ctx.m_cmd->m_side_move    = 0.f;
			g_ctx.m_cmd->m_buttons &= ~( in_forward | in_back | in_moveleft | in_moveright );
		}
	}
	if ( ( edge_jump && left_ground ) || left_ladder ) {
		g_ctx.m_cmd->m_buttons |= e_command_buttons::in_jump;
		botox_dbg_log( "LJ: edge jump %s\n", left_ladder ? "ladder" : "ground" );
	}
	if ( ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) || on_ladder )
		saved_tick = g_interfaces.m_global_vars_base->m_tick_count;
	if ( !( g_interfaces.m_global_vars_base->m_tick_count - saved_tick > n_tick::ticks( 2 ) ) &&
	     !( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) && !now_ladder ) {
		g_ctx.m_cmd->m_buttons |= e_command_buttons::in_duck;
		m_long_jump_data.m_detected = left_ground || jumped_this_tick || g_interfaces.m_global_vars_base->m_tick_count - ladder_jump_tick <= 1;
		m_long_jump_data.m_ducking  = true;
	} else {
		m_long_jump_data.m_detected = false;
		m_long_jump_data.m_ducking  = false;
	}
}

void n_movement::impl_t::adaptive_key_cancel( )
{
	auto& d        = m_long_jump_data;
	const auto cmd = g_ctx.m_cmd;
	if ( !cmd || !GET_VARIABLE( g_variables.m_long_jump, bool ) || !GET_VARIABLE( g_variables.m_long_jump_adaptive, bool ) ) {
		d.m_adaptive_armed = false;
		return;
	}
	const int now = g_interfaces.m_global_vars_base->m_tick_count;
	if ( !d.m_adaptive_armed && ( g_prediction.backup_data.m_flags & e_flags::fl_onground ) ) {
		const c_vector vel = g_prediction.backup_data.m_velocity;
		d.m_adaptive_key   = 0;
		if ( vel.length_2d( ) > 1.f ) {
			const float rel = std::remainderf( cmd->m_view_point.m_y - rad2deg( std::atan2f( vel.m_y, vel.m_x ) ), 360.f );
			if ( std::fabsf( rel ) <= 45.f )
				d.m_adaptive_key = e_command_buttons::in_forward;
			else if ( std::fabsf( rel ) >= 135.f )
				d.m_adaptive_key = e_command_buttons::in_back;
			else
				d.m_adaptive_key = rel < 0.f ? e_command_buttons::in_moveleft : e_command_buttons::in_moveright;
		}
	}
	if ( !d.m_adaptive_armed && m_long_jump_data.m_detected && d.m_adaptive_key ) {
		d.m_adaptive_armed = true;
		d.m_adaptive_tick  = now;
		botox_dbg_log( "LJ: adaptive arm key=0x%x\n", d.m_adaptive_key );
	}
	if ( !d.m_adaptive_armed )
		return;
	/* none ticked = landed, or the cancel would never end */
	const auto& stop       = GET_VARIABLE( g_variables.m_long_jump_adaptive_stop, std::vector< bool > );
	const bool stop_let_go = stop.size( ) > 0 && stop[ 0 ];
	const bool stop_timed  = stop.size( ) > 2 && stop[ 2 ];
	const bool stop_landed = ( stop.size( ) > 1 && stop[ 1 ] ) || ( !stop_let_go && !stop_timed );
	const char* why        = nullptr;
	if ( GET_VARIABLE( g_variables.m_hsw, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_hsw_key, key_bind_t ) ) )
		why = "hsw";
	else if ( GET_VARIABLE( g_variables.m_auto_strafe, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_auto_strafe_key, key_bind_t ) ) )
		why = "autostrafe";
	else if ( stop_let_go && !( g_ctx.m_input_buttons & d.m_adaptive_key ) )
		why = "let go";
	else if ( stop_landed && ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) )
		why = "landed";
	else if ( stop_timed &&
	          now - d.m_adaptive_tick >= static_cast< int >( std::round( GET_VARIABLE( g_variables.m_long_jump_adaptive_time, float ) / n_tick::interval( ) ) ) )
		why = "time";
	if ( why ) {
		d.m_adaptive_armed = false;
		botox_dbg_log( "LJ: adaptive stop %s after %d\n", why, now - d.m_adaptive_tick );
		return;
	}
	cmd->m_buttons &= ~d.m_adaptive_key;
	if ( d.m_adaptive_key == e_command_buttons::in_forward || d.m_adaptive_key == e_command_buttons::in_back )
		cmd->m_forward_move = 0.f;
	else
		cmd->m_side_move = 0.f;
}

void n_movement::impl_t::mini_jump( )
{
	static bool should_duck = false;
	const bool left_ground  = ( g_prediction.backup_data.m_flags & e_flags::fl_onground ) && !( g_ctx.m_local->get_flags( ) & e_flags::fl_onground );
	const bool jumped_this_tick = ( g_ctx.m_cmd->m_buttons & e_command_buttons::in_jump ) != 0;
	if ( g_prediction.backup_data.m_flags && g_ctx.m_local->get_flags( ) & e_flags::fl_onground )
		should_duck = false;
	if ( g_prediction.backup_data.m_flags & e_flags::fl_onground && !( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) ) {
		if ( GET_VARIABLE( g_variables.m_mini_jump_edge_jump, bool ) )
			g_ctx.m_cmd->m_buttons |= e_command_buttons::in_jump;
		g_ctx.m_cmd->m_buttons |= e_command_buttons::in_duck;
		m_mini_jump_data.m_detected = left_ground || jumped_this_tick;
		if ( GET_VARIABLE( g_variables.m_mini_jump_hold_duck, bool ) )
			should_duck = true;
	} else {
		m_mini_jump_data.m_detected = false;
	}
	if ( should_duck )
		g_ctx.m_cmd->m_buttons |= e_command_buttons::in_duck;
}
void n_movement::impl_t::fast_stop( c_user_cmd* cmd )
{
	if ( !cmd || !GET_VARIABLE( g_variables.m_fast_stop, bool ) )
		return;
	if ( !( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) )
		return;
	if ( m_user_forward_move_raw != 0.f || m_user_side_move_raw != 0.f ||
	     ( m_user_buttons_raw & ( in_forward | in_back | in_moveleft | in_moveright | in_jump ) ) != 0 )
		return;
	if ( cmd->m_forward_move != 0.f || cmd->m_side_move != 0.f || ( cmd->m_buttons & in_jump ) != 0 )
		return;
	if ( m_fireman_data.is_ladder || m_fireman_data.owns_cmd || m_fireman_data.launch_ticks > 0 )
		return;

	const c_vector velocity = g_ctx.m_local->get_velocity( );
	if ( velocity.length_2d( ) < 1.f )
		return;

	constexpr float wish = 0.1f;
	const float rotation = deg2rad( ( velocity * -1.f ).to_angle2( ).m_y - cmd->m_view_point.m_y );
	cmd->m_forward_move  = cosf( rotation ) * wish;
	cmd->m_side_move     = -sinf( rotation ) * wish;
}

void n_movement::impl_t::jump_bug( )
{
	if ( g_prediction.backup_data.m_flags & 1 )
		g_movement.m_jumpbug_data.m_can_jb = false;
	static float btime;
	if ( btime < g_interfaces.m_global_vars_base->m_current_time )
		g_movement.m_jumpbug_data.m_can_jb = false;
	if ( ( GET_VARIABLE( g_variables.m_jump_bug, bool ) && g_ctx.m_local->get_velocity( ).m_z > g_prediction.backup_data.m_velocity.m_z &&
	       !( g_prediction.backup_data.m_flags & 1 ) && !( g_ctx.m_local->get_flags( ) & 1 ) && g_ctx.m_local->get_move_type( ) != move_type_ladder &&
	       g_ctx.m_local->get_move_type( ) != move_type_noclip && g_ctx.m_local->get_move_type( ) != move_type_observer ) ) {
		btime                              = g_interfaces.m_global_vars_base->m_current_time + .2f;
		g_movement.m_jumpbug_data.m_can_jb = true;
	}
	[[unlikely]] if ( !( g_ctx.m_cmd->m_buttons & e_command_buttons::in_jump ) ) {
		static bool ducked = false;
		if ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground && !( g_prediction.backup_data.m_flags & e_flags::fl_onground ) && !ducked ) {
			g_ctx.m_cmd->m_buttons |= e_command_buttons::in_duck;
			ducked = true;
		} else
			ducked = false;
		if ( g_prediction.backup_data.m_flags & e_flags::fl_onground && ducked )
			ducked = false;
	} else {
		/* jump + jb key held = every landing edge, ungated ( can_jb only arms on rising vz, a ledge drop never rises ) */
		if ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground && !( g_prediction.backup_data.m_flags & e_flags::fl_onground ) )
			g_ctx.m_cmd->m_buttons |= e_command_buttons::in_duck;
		if ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground )
			g_ctx.m_cmd->m_buttons &= ~e_command_buttons::in_jump;
		if ( !( g_ctx.m_local->get_flags( ) & fl_onground ) && g_prediction.backup_data.m_flags & fl_onground )
			g_ctx.m_cmd->m_buttons &= ~e_command_buttons::in_duck;
	}
}

void n_movement::impl_t::kangaroo( )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;

	if ( g_prediction.backup_data.m_flags & 1 )
		g_movement.m_jumpbug_data.m_can_jb = false;

	static float btime_kangaroo;
	if ( btime_kangaroo < g_interfaces.m_global_vars_base->m_current_time )
		g_movement.m_jumpbug_data.m_can_jb = false;

	if ( ( !m_edgebug_data.m_will_edgebug &&
	       g_ctx.m_local->get_velocity( ).m_z > g_prediction.backup_data.m_velocity.m_z && !( g_prediction.backup_data.m_flags & 1 ) &&
	       !( g_ctx.m_local->get_flags( ) & 1 ) && g_ctx.m_local->get_move_type( ) != move_type_ladder &&
	       g_ctx.m_local->get_move_type( ) != move_type_noclip && g_ctx.m_local->get_move_type( ) != move_type_observer ) ) {
		btime_kangaroo                     = g_interfaces.m_global_vars_base->m_current_time + .2f;
		g_movement.m_jumpbug_data.m_can_jb = true;
	}

	[[unlikely]] if ( !( g_ctx.m_cmd->m_buttons & e_command_buttons::in_jump ) ) {
		static bool ducked_kangaroo = false;

		if ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground && !( g_prediction.backup_data.m_flags & e_flags::fl_onground ) && !ducked_kangaroo ) {
			g_ctx.m_cmd->m_buttons |= e_command_buttons::in_duck;
			ducked_kangaroo              = true;
			m_kangaroo_data.m_successful = true;
		} else {
			ducked_kangaroo = false;
			if ( g_prediction.backup_data.m_velocity.m_z > 0.f )
				m_kangaroo_data.m_successful = false;
		}

		if ( g_prediction.backup_data.m_flags & e_flags::fl_onground && ducked_kangaroo )
			ducked_kangaroo = false;
	} else {
		if ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground && !( g_prediction.backup_data.m_flags & e_flags::fl_onground ) ) {
			g_ctx.m_cmd->m_buttons |= e_command_buttons::in_duck;
			m_kangaroo_data.m_successful = true;
		}

		if ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground )
			g_ctx.m_cmd->m_buttons &= ~e_command_buttons::in_jump;

		if ( !( g_ctx.m_local->get_flags( ) & fl_onground ) && g_prediction.backup_data.m_flags & fl_onground )
			g_ctx.m_cmd->m_buttons &= ~e_command_buttons::in_duck;

		if ( g_prediction.backup_data.m_velocity.m_z > 0.f )
			m_kangaroo_data.m_successful = false;
	}
}

void n_movement::impl_t::jump_bug_crouch( c_user_cmd* cmd )
{
	if ( !GET_VARIABLE( g_variables.m_jump_bug_crouch, bool ) || !( cmd->m_buttons & in_duck ) )
		return;
	const int flags = g_ctx.m_local->get_flags( );
	if ( ( flags & fl_onground ) || !( flags & fl_ducking ) || g_ctx.m_local->get_velocity( ).m_z > 0.f )
		return;
	if ( g_texturebug.m_hit || g_texturebug.m_hs_hit || g_texturebug.m_owns_cmd || g_texturebug.m_acted ||
	     is_pixelsurf_velocity( g_ctx.m_local->get_velocity( ).m_z ) )
		return;
	cmd->m_buttons &= ~in_jump;

	const auto col = g_ctx.m_local->get_collideable( );
	if ( !col )
		return;
	const c_vector org = g_ctx.m_local->get_abs_origin( );
	c_trace_filter flt( g_ctx.m_local );
	trace_t tr;
	ray_t ray( org, org - c_vector( 0.f, 0.f, 12.f ), col->get_obb_mins( ), col->get_obb_maxs( ) );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &tr );
	if ( tr.m_fraction == 1.f )
		return;

	c_user_cmd probe = *cmd;
	probe.m_buttons  = ( cmd->m_buttons & ~in_duck ) | in_jump;
	predict_cmd( &probe );
	bool fire = g_ctx.m_local->get_velocity( ).m_z > 0.f;
	restore_prediction_frame( );
	if ( fire ) {
		probe.m_buttons = cmd->m_buttons & ~in_duck;
		predict_cmd( &probe );
		fire = ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;
		restore_prediction_frame( );
	}
	if ( fire ) {
		cmd->m_buttons = ( cmd->m_buttons & ~in_duck ) | in_jump;
		botox_dbg_log( "JBC: fire z=%.3f floor_gap=%.3f vz=%.1f", org.m_z, tr.m_fraction * 12.f, g_ctx.m_local->get_velocity( ).m_z );
	}
}

void n_movement::impl_t::rotate_movement( c_user_cmd* cmd, c_angle& angle )
{
	if ( angle.m_x == 0 && angle.m_y == 0 && angle.m_z == 0 )
		g_interfaces.m_engine_client->get_view_angles( angle );
	c_vector vec_move    = c_vector( cmd->m_forward_move, cmd->m_side_move, 0.f );
	const float speed    = vec_move.length_2d( );
	const float rotation = deg2rad( cmd->m_view_point.m_y - angle.m_y );
	cmd->m_forward_move  = std::cosf( rotation ) * speed;
	cmd->m_side_move     = std::sinf( rotation ) * speed;
}

void n_movement::impl_t::auto_one_hop( c_user_cmd* cmd )
{
	const bool enabled     = GET_VARIABLE( g_variables.m_auto_one_hop, bool );
	const bool key_pressed = g_input.check_input( &GET_VARIABLE( g_variables.m_auto_one_hop_key, key_bind_t ) );

	if ( !enabled || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || !cmd ) {
		m_auto_one_hop_data.reset( );
		return;
	}

	if ( !key_pressed && m_auto_one_hop_data.m_phase == auto_one_hop_data_t::phase_idle ) {
		m_auto_one_hop_data.reset( );
		return;
	}

	auto colidable = g_ctx.m_local->get_collideable( );
	if ( !colidable )
		return;

	const int move_type  = g_ctx.m_local->get_move_type( );
	const bool on_ladder = move_type == e_move_types::move_type_ladder;
	const bool on_ground = g_ctx.m_local->get_flags( ) & e_flags::fl_onground;
	auto& data           = m_auto_one_hop_data;

	if ( data.m_phase == auto_one_hop_data_t::phase_strafing ) {
		if ( on_ground || on_ladder ) {
			data.reset( );
			return;
		}

		if ( data.m_ladder_normal.length( ) < 0.1f )
			return;

		const c_vector into_ladder( -data.m_ladder_normal.m_x, -data.m_ladder_normal.m_y, 0.0f );
		float target_yaw = rad2deg( std::atan2( into_ladder.m_y, into_ladder.m_x ) );
		if ( data.m_air_ticks == 0 ) {
			float rotation      = deg2rad( target_yaw - cmd->m_view_point.m_y );
			cmd->m_forward_move = std::cos( rotation ) * 380.f;
			cmd->m_side_move    = -std::sin( rotation ) * 380.f + 225.f;
			data.m_air_ticks += 1;
			return;
		}
		if ( data.m_air_ticks == 1 ) {
			float rotation      = deg2rad( target_yaw - cmd->m_view_point.m_y );
			cmd->m_forward_move = std::cos( rotation ) * 420.f;
			cmd->m_side_move    = -std::sin( rotation ) * 420.f + 110.f;
			data.m_air_ticks += 1;
			return;
		}

		const float speed = g_ctx.m_local->get_velocity( ).length_2d( );

		if ( speed > 15.0f ) {
			static auto sv_airaccelerate  = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
			static auto cl_sidespeed_cvar = g_interfaces.m_convar->find_var( "cl_sidespeed" );
			if ( sv_airaccelerate && cl_sidespeed_cvar ) {
				const float cl_sidespeed = cl_sidespeed_cvar->get_float( );
				const float max_speed    = g_ctx.m_local->get_max_speed( );
				const float term         = 30.0f / sv_airaccelerate->get_float( ) / max_speed * 100.0f / speed;
				float delta_air          = 0.0f;
				if ( term > -1.0f && term < 1.0f )
					delta_air = std::acos( term );

				if ( delta_air != 0.0f ) {
					const float yaw      = deg2rad( cmd->m_view_point.m_y );
					const auto vel       = g_ctx.m_local->get_velocity( );
					const float vel_dir  = std::atan2( vel.m_y, vel.m_x ) - yaw;
					const float wish_dir = deg2rad( target_yaw ) - yaw;

					float angle_delta = vel_dir - wish_dir;
					if ( angle_delta > M_PIN )
						angle_delta -= M_PIN * 2.0f;
					if ( angle_delta < -M_PIN )
						angle_delta += M_PIN * 2.0f;

					const float final_move = angle_delta < 0.0f ? vel_dir + delta_air : vel_dir - delta_air;
					cmd->m_forward_move    = std::cos( final_move ) * cl_sidespeed;
					cmd->m_side_move       = -std::sin( final_move ) * cl_sidespeed;
				}
			}
		} else {
			float rotation      = deg2rad( target_yaw - cmd->m_view_point.m_y );
			cmd->m_forward_move = std::cos( rotation ) * 450.f;
			cmd->m_side_move    = -std::sin( rotation ) * 450.f;
		}
		data.m_air_ticks += 1;
		return;
	}

	if ( data.m_phase == auto_one_hop_data_t::phase_jumped ) {
		if ( !on_ladder ) {
			data.m_phase     = auto_one_hop_data_t::phase_strafing;
			data.m_air_ticks = 0;
			return;
		}
		cmd->m_buttons |= e_command_buttons::in_jump;
		cmd->m_forward_move = 0.f;
		cmd->m_side_move    = 0.f;
		return;
	}

	if ( !key_pressed ) {
		if ( data.m_phase == auto_one_hop_data_t::phase_climbing )
			data.reset( );
		return;
	}

	const int pred_frame = g_interfaces.m_prediction->m_commands_predicted - 1;

	bool found_ladder = false;
	c_vector ladder_normal;

	if ( on_ladder ) {
		found_ladder = true;
		if ( data.m_ladder_normal.length( ) < 0.1f ) {
			trace_t trace;
			constexpr float step = 3.14159265f * 2.0f / 16.0f;
			for ( float a = 0; a < 3.14159265f * 2.0f; a += step ) {
				c_vector wishdir( cosf( a ), sinf( a ), 0.f );
				auto start_pos = g_ctx.m_local->get_abs_origin( );
				auto end_pos   = start_pos + wishdir;
				c_trace_filter flt( g_ctx.m_local );
				ray_t ray( start_pos, end_pos, colidable->get_obb_mins( ), colidable->get_obb_maxs( ) );
				g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &trace );
				if ( trace.m_fraction < 1.0f && trace.m_plane.m_normal.m_z < 0.4f ) {
					ladder_normal = trace.m_plane.m_normal;
					break;
				}
			}
		}
	} else {
		trace_t trace;
		constexpr float step = 3.14159265f * 2.0f / 16.0f;
		for ( float a = 0; a < 3.14159265f * 2.0f; a += step ) {
			c_vector wishdir( cosf( a ), sinf( a ), 0.f );
			auto start_pos = g_ctx.m_local->get_abs_origin( );
			auto end_pos   = start_pos + wishdir;
			c_trace_filter flt( g_ctx.m_local );
			ray_t ray( start_pos, end_pos, colidable->get_obb_mins( ), colidable->get_obb_maxs( ) );
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &trace );
			if ( trace.m_fraction < 1.0f && trace.m_plane.m_normal.m_z < 0.4f ) {
				c_vector wall_n = trace.m_plane.m_normal;
				c_vector into_wall( -wall_n.m_x, -wall_n.m_y, 0.f );
				c_angle wall_ang = into_wall.to_angle( );
				float rot        = deg2rad( wall_ang.m_y - cmd->m_view_point.m_y );

				g_prediction.restore_entity_to_predicted_frame( pred_frame );
				float save_fwd      = cmd->m_forward_move;
				float save_side     = cmd->m_side_move;
				int save_buttons    = cmd->m_buttons;
				cmd->m_forward_move = std::cos( rot ) * 450.f;
				cmd->m_side_move    = -std::sin( rot ) * 450.f;
				cmd->m_buttons &= ~e_command_buttons::in_jump;

				g_prediction.begin( g_ctx.m_local, cmd );
				g_prediction.end( g_ctx.m_local );
				bool became_ladder = g_ctx.m_local->get_move_type( ) == e_move_types::move_type_ladder;

				cmd->m_forward_move = save_fwd;
				cmd->m_side_move    = save_side;
				cmd->m_buttons      = save_buttons;
				g_prediction.restore_entity_to_predicted_frame( pred_frame );

				if ( became_ladder ) {
					found_ladder  = true;
					ladder_normal = wall_n;
					break;
				}
			}
		}
	}

	if ( !found_ladder ) {
		if ( data.m_phase == auto_one_hop_data_t::phase_climbing )
			data.reset( );
		return;
	}

	if ( ladder_normal.length( ) > 0.1f )
		data.m_ladder_normal = ladder_normal;

	if ( data.m_phase == auto_one_hop_data_t::phase_idle )
		data.m_phase = auto_one_hop_data_t::phase_climbing;

	cmd->m_buttons |= in_forward;
	cmd->m_buttons &= ~in_back;
	cmd->m_forward_move = 450.f;
	cmd->m_side_move    = 0.f;
	cmd->m_up_move      = 0.f;

	if ( !on_ladder )
		return;

	g_prediction.restore_entity_to_predicted_frame( pred_frame );

	float backup_forward = cmd->m_forward_move;
	float backup_side    = cmd->m_side_move;
	float backup_up      = cmd->m_up_move;
	int backup_buttons   = cmd->m_buttons;

	bool will_leave = false;
	for ( int i = 0; i < 1; i++ ) {
		g_prediction.begin( g_ctx.m_local, cmd );
		if ( g_ctx.m_local->get_move_type( ) != e_move_types::move_type_ladder ) {
			will_leave = true;
			g_prediction.end( g_ctx.m_local );
			break;
		}
		g_prediction.end( g_ctx.m_local );
	}
	g_prediction.restore_entity_to_predicted_frame( pred_frame );

	cmd->m_forward_move = backup_forward;
	cmd->m_side_move    = backup_side;
	cmd->m_up_move      = backup_up;
	cmd->m_buttons      = backup_buttons;

	if ( will_leave ) {
		cmd->m_buttons |= e_command_buttons::in_jump;
		cmd->m_forward_move = 0.f;
		cmd->m_side_move    = 0.f;
		data.m_phase        = auto_one_hop_data_t::phase_jumped;
	}
}

void n_movement::impl_t::auto_crouch( c_user_cmd* cmd )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) ) {
		m_auto_crouch_data.m_ducking_velo  = 0.f;
		m_auto_crouch_data.m_standing_velo = 0.f;
		m_auto_crouch_data.m_founded       = false;
		return;
	}
	if ( const auto mt = g_ctx.m_local->get_move_type( );
	     mt == move_type_ladder || mt == move_type_noclip || mt == move_type_fly || mt == move_type_observer ) {
		m_auto_crouch_data.m_ducking_velo  = 0.f;
		m_auto_crouch_data.m_standing_velo = 0.f;
		m_auto_crouch_data.m_founded       = false;
		return;
	}
	const bool enabled     = GET_VARIABLE( g_variables.m_auto_crouch, bool );
	const bool key_pressed = g_input.check_input( &GET_VARIABLE( g_variables.m_auto_crouch_key, key_bind_t ) );
	const bool jump_bug_active =
		GET_VARIABLE( g_variables.m_jump_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_jump_bug_key, key_bind_t ) );

	if ( !enabled || !key_pressed || jump_bug_active ) {
		m_auto_crouch_data.m_founded = false;
		return;
	}

	if ( g_air_stuck_owns_cmd ) {
		m_auto_crouch_data.m_founded = false;
		return;
	}

	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || !cmd )
		return;

	if ( ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) || g_ctx.m_local->get_move_type( ) == e_move_types::move_type_ladder ) {
		m_auto_crouch_data.m_founded = false;
		return;
	}

	const int ticks = GET_VARIABLE( g_variables.m_auto_crouch_ticks, int );

	bool can_land_standing = false;
	bool can_land_ducking  = false;
	c_vector standing_land_origin( 0, 0, 0 );
	c_vector ducking_land_origin( 0, 0, 0 );
	c_vector current_origin = g_ctx.m_local->get_abs_origin( );

	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
	for ( int i = 0; i < ticks; i++ ) {
		g_prediction.begin( g_ctx.m_local, cmd );
		if ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) {
			can_land_standing    = true;
			standing_land_origin = g_ctx.m_local->get_abs_origin( );
			g_prediction.end( g_ctx.m_local );
			break;
		}
		g_prediction.end( g_ctx.m_local );
	}

	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
	c_user_cmd duck_cmd = *cmd;
	duck_cmd.m_buttons |= e_command_buttons::in_duck | e_command_buttons::in_bullrush;

	for ( int i = 0; i < ticks; i++ ) {
		g_prediction.begin( g_ctx.m_local, &duck_cmd );
		if ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) {
			can_land_ducking    = true;
			ducking_land_origin = g_ctx.m_local->get_abs_origin( );
			g_prediction.end( g_ctx.m_local );
			break;
		}
		g_prediction.end( g_ctx.m_local );
	}

	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );

	if ( !can_land_standing && can_land_ducking ) {
		if ( ducking_land_origin.m_z > current_origin.m_z - 1.0f ) {
			cmd->m_buttons |= e_command_buttons::in_duck | e_command_buttons::in_bullrush;
			m_auto_crouch_data.m_founded = true;
		} else {
			m_auto_crouch_data.m_founded = false;
		}
	} else {
		m_auto_crouch_data.m_founded = false;
	}
}

void n_movement::impl_t::blockbot( c_user_cmd* cmd )
{
	if ( !GET_VARIABLE( g_variables.m_blockbot, bool ) )
		return;

	if ( !g_input.check_input( &GET_VARIABLE( g_variables.m_blockbot_key, key_bind_t ) ) )
		return;

	if ( !g_interfaces.m_engine_client->is_connected( ) || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;

	if ( g_ctx.m_local->get_move_type( ) == e_move_types::move_type_ladder ||
	     g_ctx.m_local->get_move_type( ) == e_move_types::move_type_noclip )
		return;

	float best_dist = std::numeric_limits< float >::max( );
	int best_target = -1;

	for ( int i = 1; i < g_interfaces.m_global_vars_base->m_max_clients; i++ ) {
		auto entity = reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity( i ) );
		if ( !entity || entity->is_dormant( ) || !entity->is_alive( ) || entity == g_ctx.m_local )
			continue;

		float dist = g_ctx.m_local->get_abs_origin( ).dist_to( entity->get_abs_origin( ) );
		if ( dist < best_dist ) {
			best_dist   = dist;
			best_target = i;
		}
	}

	if ( best_target == -1 )
		return;

	auto entity = reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity( best_target ) );
	if ( !entity )
		return;

	const float speed      = entity->get_velocity( ).length( );
	const c_vector forward = entity->get_abs_origin( ) - g_ctx.m_local->get_abs_origin( );

	if ( entity->get_bone_position( 6 ).m_z < g_ctx.m_local->get_abs_origin( ).m_z &&
	     g_ctx.m_local->get_abs_origin( ).dist_to( entity->get_abs_origin( ) ) < 100.f ) {
		cmd->m_forward_move = ( ( sin( deg2rad( cmd->m_view_point.m_y ) ) * forward.m_y ) +
		                        ( cos( deg2rad( cmd->m_view_point.m_y ) ) * forward.m_x ) ) * speed;
		cmd->m_side_move    = ( ( cos( deg2rad( cmd->m_view_point.m_y ) ) * -forward.m_y ) +
		                        ( sin( deg2rad( cmd->m_view_point.m_y ) ) * forward.m_x ) ) * speed;
	} else {
		float yaw_delta = ( atan2( forward.m_y, forward.m_x ) * 180.0f / 3.14159265359f ) - cmd->m_view_point.m_y;

		if ( yaw_delta > 180.f )
			yaw_delta -= 360.f;
		else if ( yaw_delta < -180.f )
			yaw_delta += 360.f;

		if ( yaw_delta > 0.25f )
			cmd->m_side_move = -speed;
		else if ( yaw_delta < -0.25f )
			cmd->m_side_move = speed;
	}
}

struct fake_user_move {
	float sidemove     = 0.f;
	float forwardmove  = 0.f;
	int buttons        = 0;
	c_angle viewangles = c_angle( 0.f, 0.f, 0.f );
};
fake_user_move g_moveuser[ 1000 ];
void n_movement::impl_t::ladder_bug( c_user_cmd* cmd )
{
	m_ladder_bug_data.founded = false;
	static int ticks_to_stop  = 0;

	/* sv_autobunnyhopping is forced on below and must go back: captured on key down, restored on key up.
	   ABOVE the local / alive guards so dying mid hold still restores */
	static bool held          = false;
	static int stored_autohop = -1;

	auto cvar = g_interfaces.m_convar->find_var( "sv_autobunnyhopping" );

	const bool wants = GET_VARIABLE( g_variables.m_ladder_bug, bool ) &&
	                   g_input.check_input( &GET_VARIABLE( g_variables.m_ladder_bug_key, key_bind_t ) ) && g_ctx.m_local &&
	                   g_interfaces.m_engine_client->is_in_game( ) && g_ctx.m_local->is_alive( );

	if ( !wants ) {
		ticks_to_stop = 0;

		if ( held && cvar && stored_autohop >= 0 )
			cvar->set_value( stored_autohop );

		held           = false;
		stored_autohop = -1;
		return;
	}

	if ( !cvar ) {
		return;
	}

	if ( !held ) {
		stored_autohop = cvar->get_int( );
		held           = true;
	}

	if ( cvar->get_int( ) == 0 )
		cvar->set_value( 1 );
	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
	if ( !m_ladder_bug_data.founded && g_ctx.m_local->get_move_type( ) != move_type_ladder && !( g_ctx.m_local->get_flags( ) & 1 ) ) {
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		if ( g_ctx.m_local->get_move_type( ) == move_type_ladder ) {
			cmd->m_up_move = 450.f;
			cmd->m_buttons |= in_jump;
			m_ladder_bug_data.founded = true;
		} else {
			m_ladder_bug_data.founded = false;
		}
	}
}
void n_movement::impl_t::anti_ladder( c_user_cmd* cmd )
{
	if ( !g_ctx.m_local )
		return;
	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return;
	if ( !g_ctx.m_local->is_alive( ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_ladder_bug, bool ) || !g_input.check_input( &GET_VARIABLE( g_variables.m_ladder_bug_key, key_bind_t ) ) ) {
		return;
	}
	static int ticks_on_ladder = 0;
	if ( g_prediction.backup_data.m_move_type == move_type_ladder ) {
		if ( ticks_on_ladder > 10 ) {
			c_vector_2d direction_move = c_vector_2d( cmd->m_forward_move, cmd->m_side_move );
			for ( int i = 0; i < 8; i++ ) {
				g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
				int old_move_type = g_ctx.m_local->get_move_type( );
				if ( i == 0 ) {
					cmd->m_forward_move = 450.f;
					cmd->m_side_move    = 0.f;
				} else if ( i == 1 ) {
					cmd->m_forward_move = -450.f;
					cmd->m_side_move    = 0.f;
				} else if ( i == 2 ) {
					cmd->m_forward_move = 0.f;
					cmd->m_side_move    = 450.f;
				} else if ( i == 3 ) {
					cmd->m_forward_move = 0.f;
					cmd->m_side_move    = -450.f;
				} else if ( i == 4 ) {
					cmd->m_forward_move = 0.f;
					cmd->m_side_move    = 0.f;
				} else if ( i == 5 ) {
					cmd->m_forward_move = 450.f;
					cmd->m_side_move    = -450.f;
				} else if ( i == 6 ) {
					cmd->m_side_move    = 450.f;
					cmd->m_forward_move = -450.f;
				} else if ( i == 7 ) {
					cmd->m_forward_move = 450.f;
					cmd->m_side_move    = 450.f;
				}
				g_prediction.begin( g_ctx.m_local, cmd );
				g_prediction.end( g_ctx.m_local );
				int new_move_type = g_ctx.m_local->get_move_type( );
				if ( old_move_type == move_type_ladder && new_move_type != move_type_ladder ) {
					direction_move = c_vector_2d( cmd->m_forward_move, cmd->m_side_move );
				}
			}
			cmd->m_forward_move = direction_move.m_x;
			cmd->m_side_move    = direction_move.m_y;
		}
		ticks_on_ladder++;
	} else {
		ticks_on_ladder = 0;
	}
}
void n_movement::impl_t::strafe_to_yaw( c_user_cmd* cmd, c_angle& angle, const float yaw )
{
	const static auto max_side_speed = g_convars.float_or( HASH_BT( "cl_sidespeed" ), 450.f );
	angle.m_y += yaw;
	cmd->m_side_move    = 0.f;
	cmd->m_forward_move = 0.f;
	const auto degrees  = rad2deg( std::atan2f( g_ctx.m_local->get_velocity( ).m_y, g_ctx.m_local->get_velocity( ).m_x ) );
	const auto delta    = g_math.normalize_angle( angle.m_y - degrees );
	cmd->m_side_move    = delta > 0.f ? -max_side_speed : max_side_speed;
	angle.m_y           = g_math.normalize_angle( angle.m_y - delta );
}

void n_movement::impl_t::fast_ladder( c_user_cmd* cmd )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_fast_ladder, bool ) )
		return;
	if ( !g_input.check_input( &GET_VARIABLE( g_variables.m_fast_ladder_key, key_bind_t ) ) )
		return;
	if ( g_ctx.m_local->get_move_type( ) == move_type_ladder ) {
		if ( cmd->m_side_move == 0 ) {
			cmd->m_view_point.m_y += 45.0f;
		}
		if ( cmd->m_buttons & in_forward ) {
			if ( cmd->m_side_move > 0 )
				cmd->m_view_point.m_y -= 1.0f;
			if ( cmd->m_side_move < 0 )
				cmd->m_view_point.m_y += 1.0f;
		}
		if ( cmd->m_buttons & in_back ) {
			if ( cmd->m_side_move > 0 )
				cmd->m_view_point.m_y += 1.0f;
			if ( cmd->m_side_move < 0 )
				cmd->m_view_point.m_y -= 1.0f;
		}
		if ( cmd->m_buttons & in_forward ) {
			cmd->m_forward_move = 450.f;
		}
		if ( cmd->m_buttons & in_back ) {
			cmd->m_forward_move = -450.f;
		}
		if ( cmd->m_side_move > 0 ) {
			cmd->m_side_move = 450.f;
		}
		if ( cmd->m_side_move < 0 ) {
			cmd->m_side_move = -450.f;
		}
	}
}

extern void start_movement_fix( c_user_cmd* cmd );
extern void end_movement_fix( c_user_cmd* cmd );
void n_movement::impl_t::ladder_freelook_climb( c_user_cmd* cmd )
{
	auto& data = m_ladder_freelook_climb_data;

	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || !cmd ) {
		data.reset( );
		return;
	}

	const bool enabled     = GET_VARIABLE( g_variables.m_ladder_freelook_climb, bool );
	const bool key_pressed = g_input.check_input( &GET_VARIABLE( g_variables.m_ladder_freelook_climb_key, key_bind_t ) );
	if ( !enabled ) {
		data.reset( );
		return;
	}

	const int move_type  = g_ctx.m_local->get_move_type( );
	const bool on_ladder = move_type == e_move_types::move_type_ladder;
	const bool on_ground = ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) != 0;

	auto clear_walk_input = [ & ]( ) { cmd->m_buttons &= ~( in_forward | in_back | in_moveleft | in_moveright ); };

	auto apply_walk_buttons = [ & ]( float forward_move, float side_move ) {
		clear_walk_input( );
		if ( forward_move > 0.0f )
			cmd->m_buttons |= in_forward;
		else if ( forward_move < 0.0f )
			cmd->m_buttons |= in_back;
		if ( side_move > 0.0f )
			cmd->m_buttons |= in_moveright;
		else if ( side_move < 0.0f )
			cmd->m_buttons |= in_moveleft;
	};

	auto normalize_radians = []( float angle ) {
		while ( angle > M_PIN )
			angle -= M_PIN * 2.0f;
		while ( angle < -M_PIN )
			angle += M_PIN * 2.0f;
		return angle;
	};

	auto restore_freelook_camera = []( const c_angle& visual ) {
		c_angle v = visual;
		g_interfaces.m_engine_client->set_view_angles( v );
	};

	auto apply_server_locked_climb = [ & ]( ) {
		cmd->m_buttons &= ~in_jump;
		clear_walk_input( );
		cmd->m_forward_move = 450.0f;
		cmd->m_side_move    = 0.0f;
		cmd->m_up_move      = 0.0f;
		apply_walk_buttons( cmd->m_forward_move, cmd->m_side_move );
	};

	auto apply_server_jump_into_ladder = [ & ]( ) {
		cmd->m_buttons |= in_jump;
		clear_walk_input( );
		cmd->m_forward_move = 450.0f;
		cmd->m_side_move    = 0.0f;
		cmd->m_up_move      = 0.0f;
		apply_walk_buttons( cmd->m_forward_move, cmd->m_side_move );
	};

	static constexpr int k_ladder_freelook_simple_air_ticks = 8;

	auto apply_forward_air_strafe = [ & ]( float target_yaw, bool simple_wish_only ) {
		cmd->m_up_move = 0.0f;
		cmd->m_buttons &= ~in_jump;
		clear_walk_input( );

		const c_vector velocity       = g_ctx.m_local->get_velocity( );
		const float speed             = velocity.length_2d( );
		static auto sv_airaccelerate  = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
		static auto cl_sidespeed_cvar = g_interfaces.m_convar->find_var( "cl_sidespeed" );

		if ( !simple_wish_only && speed > 15.0f && sv_airaccelerate && cl_sidespeed_cvar ) {
			const float max_speed = g_ctx.m_local->get_max_speed( );
			const float term      = 30.0f / sv_airaccelerate->get_float( ) / max_speed * 100.0f / speed;
			float delta_air       = 0.0f;
			if ( term > -1.0f && term < 1.0f )
				delta_air = std::acos( term );

			if ( delta_air != 0.0f ) {
				const float yaw                = deg2rad( cmd->m_view_point.m_y );
				const float velocity_direction = std::atan2( velocity.m_y, velocity.m_x ) - yaw;
				const float wish_direction     = deg2rad( target_yaw ) - yaw;
				const float delta              = normalize_radians( velocity_direction - wish_direction );
				const float final_move         = delta < 0.0f ? velocity_direction + delta_air : velocity_direction - delta_air;
				cmd->m_forward_move            = std::cos( final_move ) * cl_sidespeed_cvar->get_float( );
				cmd->m_side_move               = -std::sin( final_move ) * cl_sidespeed_cvar->get_float( );
			} else {
				const float rotation = deg2rad( g_math.normalize_angle( target_yaw - cmd->m_view_point.m_y ) );
				cmd->m_forward_move  = std::cos( rotation ) * 450.0f;
				cmd->m_side_move     = -std::sin( rotation ) * 450.0f;
			}
		} else {
			const float rotation = deg2rad( g_math.normalize_angle( target_yaw - cmd->m_view_point.m_y ) );
			cmd->m_forward_move  = std::cos( rotation ) * 450.0f;
			cmd->m_side_move     = -std::sin( rotation ) * 450.0f;
		}
		apply_walk_buttons( cmd->m_forward_move, cmd->m_side_move );
	};

	if ( data.m_phase == ladder_freelook_climb_data_t::phase_strafing ) {
		if ( !key_pressed ) {
			data.reset( );
			return;
		}

		const float speed_2d = g_ctx.m_local->get_velocity( ).length_2d( );
		data.m_keep_strafing = speed_2d < 245.0f;

		if ( on_ground || on_ladder ) {
			data.reset( );
			return;
		}

		const c_angle visual_angles = cmd->m_view_point;
		cmd->m_view_point           = c_angle( 0.0f, data.m_server_yaw, 0.0f );
		const bool simple_air       = data.m_air_ticks < k_ladder_freelook_simple_air_ticks || data.m_keep_strafing;
		apply_forward_air_strafe( data.m_server_yaw, simple_air );
		data.m_air_ticks += 1;
		restore_freelook_camera( visual_angles );
		return;
	}

	if ( data.m_phase == ladder_freelook_climb_data_t::phase_jumping ) {
		if ( !on_ladder ) {
			data.m_phase                = ladder_freelook_climb_data_t::phase_strafing;
			data.m_air_ticks            = 0;
			data.m_keep_strafing        = true;
			const c_angle visual_angles = cmd->m_view_point;
			cmd->m_view_point           = c_angle( 0.0f, data.m_server_yaw, 0.0f );
			apply_forward_air_strafe( data.m_server_yaw, true );
			data.m_air_ticks += 1;
			restore_freelook_camera( visual_angles );
			return;
		}

		const c_angle visual_angles = cmd->m_view_point;
		cmd->m_view_point           = c_angle( 0.0f, data.m_server_yaw, 0.0f );
		apply_server_jump_into_ladder( );
		restore_freelook_camera( visual_angles );
		return;
	}

	if ( !key_pressed ) {
		data.reset( );
		return;
	}

	auto colidable = g_ctx.m_local->get_collideable( );
	if ( !colidable ) {
		data.reset( );
		return;
	}

	const int pred_frame   = g_interfaces.m_prediction->m_commands_predicted - 1;
	bool found_ladder      = false;
	c_vector ladder_normal = { };

	const bool reuse_climb_geometry =
		data.m_phase == ladder_freelook_climb_data_t::phase_climbing && on_ladder && data.m_ladder_normal.length_2d( ) > 0.01f;

	if ( reuse_climb_geometry ) {
		found_ladder  = true;
		ladder_normal = data.m_ladder_normal;
	} else {
		trace_t trace;
		const float step    = 3.14159265f * 2.0f / 32.0f;
		float best_fraction = 1.0f;

		for ( float a = 0.0f; a < 3.14159265f * 2.0f; a += step ) {
			c_vector wishdir( cosf( a ), sinf( a ), 0.0f );
			auto start_pos = g_ctx.m_local->get_abs_origin( );
			auto end_pos   = start_pos + wishdir * 2.0f;
			c_trace_filter flt( g_ctx.m_local );
			ray_t ray( start_pos, end_pos, colidable->get_obb_mins( ), colidable->get_obb_maxs( ) );
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &flt, &trace );

			if ( trace.m_fraction >= best_fraction || trace.m_plane.m_normal.m_z >= 0.4f || trace.m_plane.m_normal.m_z <= -0.4f )
				continue;

			const c_vector wall_normal = trace.m_plane.m_normal;

			if ( on_ladder ) {
				if ( trace.m_fraction < best_fraction ) {
					best_fraction = trace.m_fraction;
					ladder_normal = wall_normal;
					found_ladder  = true;
				}
			} else {
				const c_vector into_dir( -wall_normal.m_x, -wall_normal.m_y, 0.0f );
				const c_angle into_angle = into_dir.to_angle( );
				const float rotation     = deg2rad( into_angle.m_y - cmd->m_view_point.m_y );

				const float backup_forward = cmd->m_forward_move;
				const float backup_side    = cmd->m_side_move;
				const float backup_up      = cmd->m_up_move;
				const int backup_buttons   = cmd->m_buttons;

				g_prediction.restore_entity_to_predicted_frame( pred_frame );
				cmd->m_forward_move = std::cos( rotation ) * 450.0f;
				cmd->m_side_move    = -std::sin( rotation ) * 450.0f;
				cmd->m_up_move      = 0.0f;
				cmd->m_buttons &= ~in_jump;

				g_prediction.begin( g_ctx.m_local, cmd );
				g_prediction.end( g_ctx.m_local );
				const bool became_ladder = g_ctx.m_local->get_move_type( ) == e_move_types::move_type_ladder;

				g_prediction.restore_entity_to_predicted_frame( pred_frame );
				cmd->m_forward_move = backup_forward;
				cmd->m_side_move    = backup_side;
				cmd->m_up_move      = backup_up;
				cmd->m_buttons      = backup_buttons;

				if ( became_ladder ) {
					found_ladder  = true;
					ladder_normal = wall_normal;
					break;
				}
			}
		}
	}

	if ( !found_ladder ) {
		if ( data.m_phase == ladder_freelook_climb_data_t::phase_idle )
			return;

		data.reset( );
		return;
	}

	c_vector ladder_n = ladder_normal;
	const c_vector into_ladder( -ladder_n.m_x, -ladder_n.m_y, 0.0f );
	if ( !reuse_climb_geometry ) {
		data.m_ladder_normal = ladder_n;
		data.m_server_yaw    = into_ladder.to_angle( ).m_y;
	}
	data.m_exit_yaw = data.m_server_yaw;
	data.m_phase    = ladder_freelook_climb_data_t::phase_climbing;

	const c_angle visual_angles = cmd->m_view_point;
	cmd->m_view_point           = c_angle( 0.0f, data.m_server_yaw, 0.0f );
	apply_server_locked_climb( );

	if ( !on_ladder ) {
		restore_freelook_camera( visual_angles );
		return;
	}

	const float backup_forward = cmd->m_forward_move;
	const float backup_side    = cmd->m_side_move;
	const float backup_up      = cmd->m_up_move;
	const int backup_buttons   = cmd->m_buttons;
	const c_angle backup_view  = cmd->m_view_point;

	bool will_leave_ladder = false;
	g_prediction.restore_entity_to_predicted_frame( pred_frame );

	cmd->m_view_point   = c_angle( 0.0f, data.m_server_yaw, 0.0f );
	cmd->m_forward_move = 450.0f;
	cmd->m_side_move    = 0.0f;
	cmd->m_up_move      = 0.0f;
	cmd->m_buttons &= ~in_jump;

	for ( int i = 0; i < 2; ++i ) {
		g_prediction.begin( g_ctx.m_local, cmd );
		if ( g_ctx.m_local->get_move_type( ) != e_move_types::move_type_ladder ) {
			will_leave_ladder = true;
			g_prediction.end( g_ctx.m_local );
			break;
		}
		g_prediction.end( g_ctx.m_local );
	}
	g_prediction.restore_entity_to_predicted_frame( pred_frame );

	cmd->m_forward_move = backup_forward;
	cmd->m_side_move    = backup_side;
	cmd->m_up_move      = backup_up;
	cmd->m_buttons      = backup_buttons;
	cmd->m_view_point   = backup_view;

	if ( will_leave_ladder ) {
		movement_add_window( 5, std::string( "ladder dbg sy=" )
		                            .append( std::to_string( static_cast< int >( std::round( data.m_server_yaw ) ) ) )
		                            .append( " fw=" )
		                            .append( std::to_string( static_cast< int >( std::round( cmd->m_forward_move ) ) ) )
		                            .append( " sd=" )
		                            .append( std::to_string( static_cast< int >( std::round( cmd->m_side_move ) ) ) ) );
		data.m_phase = ladder_freelook_climb_data_t::phase_jumping;
	}

	restore_freelook_camera( visual_angles );
}

void n_movement::impl_t::ladder_glide( c_user_cmd* cmd )
{
	if ( !GET_VARIABLE( g_variables.m_ladder_glide, bool ) || !g_input.check_input( &GET_VARIABLE( g_variables.m_ladder_glide_key, key_bind_t ) ) )
		return;
	if ( g_ctx.m_local->get_move_type( ) == move_type_ladder ) {
		if ( cmd->m_side_move == 0 ) {
			cmd->m_view_point.m_y += 45.0f;
		}
		if ( cmd->m_buttons & in_forward ) {
			if ( cmd->m_side_move > 0 )
				cmd->m_view_point.m_y -= 1.0f;
			if ( cmd->m_side_move < 0 )
				cmd->m_view_point.m_y += 90.0f;
			cmd->m_buttons &= ~in_moveleft;
			cmd->m_buttons |= in_moveright;
		}
		if ( cmd->m_buttons & in_back ) {
			if ( cmd->m_side_move < 0 )
				cmd->m_view_point.m_y -= 1.0f;
			if ( cmd->m_side_move > 0 )
				cmd->m_view_point.m_y += 90.0f;
			cmd->m_buttons &= ~in_moveright;
			cmd->m_buttons |= in_moveleft;
		}
	}
}

static struct {
	c_vector origin{ }, vel{ };
	bool ground = true;
	c_vector net_origin{ }, view{ };
	int flags   = 0;
	float duck  = 0.f;
	bool pre    = false;
} s_cmd_start;

static bool s_ps_bail = false;

void ps_cmd_start( )
{
	s_cmd_start.pre = false;
	if ( !g_ctx.m_local )
		return;
	if ( g_ctx.m_local->is_alive( ) )
		restore_prediction_frame( );
	s_cmd_start.origin     = g_ctx.m_local->get_abs_origin( );
	s_cmd_start.vel        = g_ctx.m_local->get_velocity( );
	s_cmd_start.ground     = ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;
	s_cmd_start.net_origin = g_ctx.m_local->get_origin( );
	s_cmd_start.view       = g_ctx.m_local->get_view_offset( );
	s_cmd_start.flags      = g_ctx.m_local->get_flags( );
	s_cmd_start.duck       = g_ctx.m_local->get_duck_amount( );
	s_cmd_start.pre        = true;

	static bool prev_key = false;
	const bool key       = ( g_ctx.m_input_buttons & in_duck ) != 0;
	const bool ducked    = ( g_ctx.m_local->get_flags( ) & fl_ducking ) != 0;
	const bool riding    = !s_cmd_start.ground &&
	                    std::fabsf( s_cmd_start.vel.m_z - g_prediction.get_engine_target_predict_z_velocity( ) ) < 0.01f;
	if ( s_cmd_start.ground || g_ctx.m_local->get_move_type( ) != move_type_walk )
		s_ps_bail = false;
	else if ( key != prev_key && riding && key != ducked && !s_ps_bail ) {
		s_ps_bail = true;
		g_movement.m_pixelsurf_data.should_pixel_surf = g_movement.m_pixelsurf_data.should_unduck = false;
		botox_dbg_log( "PSU: bail key=%s on %s surf z=%.3f", key ? "duck" : "stand", ducked ? "duck" : "stand", s_cmd_start.origin.m_z );
	}
	prev_key = key;
}

bool ps_bailed( )
{
	return s_ps_bail;
}

/* every PRE sim must end on START: the real cmd predicts from what is left, and the aimbot eye ( slot 169 ) and POST readers
   read its end. a stage that leaves a probe spot behind is put back here and named. close = last check before the real cmd */
void cmd_start_guard( const char* stage, const bool close )
{
	c_base_entity* const local = g_ctx.m_local;
	if ( !s_cmd_start.pre || !local || !local->is_alive( ) )
		return;
	if ( close )
		s_cmd_start.pre = false;
	const auto same = [ ]( const c_vector& a, const c_vector& b ) { return a.m_x == b.m_x && a.m_y == b.m_y && a.m_z == b.m_z; };
	const auto at_start = [ & ]( ) {
		return same( local->get_abs_origin( ), s_cmd_start.origin ) && same( local->get_origin( ), s_cmd_start.net_origin ) &&
		       same( local->get_velocity( ), s_cmd_start.vel ) && same( local->get_view_offset( ), s_cmd_start.view ) &&
		       local->get_flags( ) == s_cmd_start.flags && local->get_duck_amount( ) == s_cmd_start.duck;
	};
	if ( at_start( ) )
		return;
	const c_vector at = local->get_abs_origin( );
	const float vz = local->get_velocity( ).m_z, view_z = local->get_view_offset( ).m_z, duck = local->get_duck_amount( );
	const int flags = local->get_flags( );
	restore_prediction_frame( );
	const bool fixed = at_start( );
	static const char* logged     = nullptr;
	static unsigned long long next = 0ull;
	if ( const unsigned long long now = GetTickCount64( ); stage != logged || now >= next || !fixed ) {
		botox_dbg_log( "START: %s left local off START d=%.3f at %.2f %.2f %.2f start %.2f %.2f %.2f vz %.2f/%.2f view %.2f/%.2f duck %.3f/%.3f "
		               "fl %x/%x -> %s",
		               stage, ( at - s_cmd_start.origin ).length( ), at.m_x, at.m_y, at.m_z, s_cmd_start.origin.m_x, s_cmd_start.origin.m_y,
		               s_cmd_start.origin.m_z, vz, s_cmd_start.vel.m_z, view_z, s_cmd_start.view.m_z, duck, s_cmd_start.duck, flags,
		               s_cmd_start.flags, fixed ? "restored" : "RESTORE MISSED" );
		logged = stage;
		next   = now + 250ull;
	}
}

static struct {
	float yaw = 0.f, len = 0.f;
	const char* logged = nullptr;
	unsigned long long next = 0ull;
} s_gnd;

static void gnd_wish( const bool final_view, float& yaw, float& len )
{
	const c_user_cmd* cmd = g_ctx.m_cmd;
	const float y         = deg2rad( final_view ? cmd->m_view_point.m_y : g_ctx.old_view_point.m_y );
	const float wx = cmd->m_forward_move * std::cosf( y ) + cmd->m_side_move * std::sinf( y );
	const float wy = cmd->m_forward_move * std::sinf( y ) - cmd->m_side_move * std::cosf( y );
	len            = std::sqrt( wx * wx + wy * wy );
	yaw            = len > 0.01f ? rad2deg( std::atan2f( wy, wx ) ) : 0.f;
}

void gnd_wish_start( )
{
	if ( g_ctx.m_cmd )
		gnd_wish( false, s_gnd.yaw, s_gnd.len );
}

void gnd_wish_check( const char* stage, const bool final_view )
{
	if ( !g_ctx.m_cmd || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	g_wall_climb.trace_stomp( g_ctx.m_cmd, stage );
	cmd_start_guard( stage, false );
	float yaw, len;
	gnd_wish( final_view, yaw, len );
	const float turn = len > 0.01f && s_gnd.len > 0.01f ? std::remainderf( yaw - s_gnd.yaw, 360.f ) : 0.f;
	if ( s_cmd_start.ground && ( std::fabsf( turn ) > 0.5f || std::fabsf( len - s_gnd.len ) > 1.f ) ) {
		const unsigned long long now = GetTickCount64( );
		if ( stage != s_gnd.logged || now >= s_gnd.next ) {
			const int b = g_ctx.m_input_buttons;
			botox_dbg_log( "GND: %s turn=%.1f len=%.0f->%.0f fwd=%.1f side=%.1f view=%.1f old=%.1f keys=%c%c%c%c spd=%.1f", stage, turn, s_gnd.len,
			               len, g_ctx.m_cmd->m_forward_move, g_ctx.m_cmd->m_side_move, g_ctx.m_cmd->m_view_point.m_y, g_ctx.old_view_point.m_y,
			               ( b & in_forward ) ? 'W' : '-', ( b & in_moveleft ) ? 'A' : '-', ( b & in_back ) ? 'S' : '-',
			               ( b & in_moveright ) ? 'D' : '-', s_cmd_start.vel.length_2d( ) );
			s_gnd.logged = stage;
			s_gnd.next   = now + 250ull;
		}
	}
	s_gnd.yaw = yaw;
	s_gnd.len = len;
}

static bool starts_on_surf_pin( )
{
	return !s_cmd_start.ground && std::fabsf( s_cmd_start.vel.m_z - g_prediction.get_engine_target_predict_z_velocity( ) ) < 0.01f;
}

/* mv->m_flMaxSpeed at AirAccelerate: GetPlayerMaxSpeed ( cs_player_shared.cpp:335, m_flMaxspeed is always 260, the weapon caps it )
   -> CheckParameters crops ( cs_gamemovement.cpp:178 ) -> Duck's crop on the amount AFTER this tick's duck ( :1533 ) */
static float air_wish_speed( const c_user_cmd* cmd )
{
	c_base_entity* const local = g_ctx.m_local;
	const int flags            = local->get_flags( );
	const bool ground          = flags & fl_onground;

	float ws = g_convars.float_or( HASH_BT( "sv_maxspeed" ), 320.f );
	if ( local->get_max_speed( ) > 0.f && local->get_max_speed( ) < ws )
		ws = local->get_max_speed( );
	ws = local->has_heavy_armor( ) ? 130.f : std::min( ws, 260.f );
	if ( const auto weapon = g_interfaces.m_weapon_system ? g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_active_weapon_handle( ) ) : nullptr )
		if ( const auto data = g_interfaces.m_weapon_system->get_weapon_data( weapon->get_item_definition_index( ) ) )
			if ( const float w = data->m_max_speed[ local->is_scoped( ) ? 1 : 0 ]; w > 0.f )
				ws = std::min( ws, w );

	const float ipt  = n_tick::engine_interval( );
	const float amt0 = local->get_duck_amount( );
	const float ds   = local->get_duck_speed( );
	const bool held  = ( cmd->m_buttons & in_duck ) && ds >= 1.5f &&
	                  ( ( flags & fl_ducking ) || static_cast< float >( local->get_tick_base( ) ) * ipt >=
	                                                   local->get_last_duck_time( ) + g_convars.float_or( HASH_BT( "sv_timebetweenducks" ), 0.4f ) );

	if ( ( cmd->m_buttons & in_speed ) && !held && amt0 <= 0.f && !( flags & fl_ducking ) && local->get_velocity( ).length( ) < ws * 0.52f + 25.f )
		ws *= 0.52f;
	if ( ground )
		ws *= std::clamp( local->get_velocity_modifier( ), 0.f, 1.f );
	if ( const float st = local->get_stamina( ); st > 0.f ) {
		const float k = std::clamp( 1.f - st / 100.f, 0.f, 1.f );
		ws *= k * k;
	}

	float amt = amt0;
	if ( held )
		amt = ground ? std::min( amt0 + ipt * ds * 0.8f, 1.f ) : 1.f;
	else if ( amt0 > 0.f )
		amt = ground ? std::max( amt0 - ipt * std::max( 1.5f, ds ), 0.f ) : 0.f; /* ponytail: assumes CanUnduck, a ceiling keeps 0.34 */
	return ws * ( 0.34f * amt + 1.f - amt );
}

static float s_as_mouse_x = 0.f, s_as_mouse_y = 0.f;

static bool auto_strafe_live( )
{
	return GET_VARIABLE( g_variables.m_auto_strafe, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_auto_strafe_key, key_bind_t ) );
}

void n_movement::impl_t::auto_strafe_mouse( float* x, float* y )
{
	if ( !x || !y || !auto_strafe_live( ) || GET_VARIABLE( g_variables.m_auto_strafe_type, int ) != 2 )
		return;
	s_as_mouse_x += *x;
	s_as_mouse_y += *y;
	*x = *y = 0.f;
}

void n_movement::impl_t::AutoStrafe( c_user_cmd* cmd )
{
	if ( !auto_strafe_live( ) ) {
		s_as_mouse_x = s_as_mouse_y = 0.f;
		return;
	}
	if ( !g_ctx.m_local || !cmd )
		return;

	static float key_off = 0.f, mouse_off = 0.f;
	float target   = cmd->m_view_point.m_y;
	const int type = GET_VARIABLE( g_variables.m_auto_strafe_type, int );
	if ( type == 1 ) {
		if ( m_user_forward_move_raw != 0.f || m_user_side_move_raw != 0.f )
			key_off = rad2deg( std::atan2f( m_user_side_move_raw, m_user_forward_move_raw ) );
		else if ( !GET_VARIABLE( g_variables.m_auto_strafe_last_keys, bool ) )
			key_off = 0.f;
		target -= key_off;
	} else if ( type == 2 ) {
		const float mx = s_as_mouse_x, my = s_as_mouse_y;
		s_as_mouse_x = s_as_mouse_y = 0.f;
		if ( mx != 0.f || my != 0.f )
			mouse_off = rad2deg( std::atan2f( mx, -my ) );
		else if ( !GET_VARIABLE( g_variables.m_auto_strafe_last_mouse, bool ) )
			return;
		target -= mouse_off;
	}

	const int flags = g_ctx.m_local->get_flags( );
	if ( flags & fl_onground ) {
		const bool fresh = GET_VARIABLE( g_variables.m_bunny_hop, bool ) || g_convars.float_or( HASH_BT( "sv_autobunnyhopping" ), 0.f ) != 0.f;
		if ( !fresh || !( cmd->m_buttons & e_command_buttons::in_jump ) )
			return;
	}

	if ( g_air_stuck_owns_cmd || starts_on_surf_pin( ) )
		return;

	const c_vector velocity = g_ctx.m_local->get_velocity( );
	const float speed       = velocity.length_2d( );
	if ( speed < 1.f )
		return;

	const auto col = g_ctx.m_local->get_collideable( );
	if ( GET_VARIABLE( g_variables.m_auto_strafe_avoid_walls, bool ) && col ) {
		const c_vector from = g_ctx.m_local->get_origin( );
		const float t       = deg2rad( target );
		const float dx = std::cosf( t ), dy = std::sinf( t );
		c_trace_filter fil( g_ctx.m_local );
		trace_t far_tr;
		ray_t far_ray( from, c_vector( from.m_x + dx * 10000.f, from.m_y + dy * 10000.f, from.m_z ), col->get_obb_mins( ), col->get_obb_maxs( ) );
		g_interfaces.m_engine_trace->trace_ray( far_ray, mask_playersolid, &fil, &far_tr );
		if ( far_tr.did_hit( ) && far_tr.m_plane.m_normal.m_z < 0.7f ) {
			const c_vector in( -far_tr.m_plane.m_normal.m_x, -far_tr.m_plane.m_normal.m_y, -far_tr.m_plane.m_normal.m_z );
			const float dist = GET_VARIABLE( g_variables.m_auto_strafe_avoid_dist, float );
			trace_t near_tr;
			ray_t near_ray( from, c_vector( from.m_x + in.m_x * dist, from.m_y + in.m_y * dist, from.m_z + in.m_z * dist ), col->get_obb_mins( ),
			                col->get_obb_maxs( ) );
			g_interfaces.m_engine_trace->trace_ray( near_ray, mask_playersolid, &fil, &near_tr );
			if ( near_tr.did_hit( ) && near_tr.m_plane.m_normal.m_z < 0.7f ) {
				const float in_yaw = rad2deg( std::atan2f( in.m_y, in.m_x ) );
				target = ( in.m_y * dx - in.m_x * dy > 0.f ) ? in_yaw - 90.f : in_yaw + 90.f;
			}
		}
	}

	const float ws    = air_wish_speed( cmd );
	const float cap   = g_convars.float_or( HASH_BT( "sv_air_max_wishspeed" ), 30.f );
	const float accel = g_convars.float_or( HASH_BT( "sv_airaccelerate" ), 12.f ) * ws * n_tick::engine_interval( ) * g_ctx.m_local->get_surface_friction( );
	const float theta = rad2deg( std::acos( std::clamp( ( cap - accel ) / speed, 0.f, 1.f ) ) );

	const auto turn_at = [ & ]( const float phi_deg ) {
		const float phi = deg2rad( phi_deg );
		const float add = cap - speed * std::cosf( phi );
		if ( add <= 0.f )
			return 0.f;
		const float s = std::min( accel, add );
		return rad2deg( std::atan2f( s * std::sinf( phi ), speed + s * std::cosf( phi ) ) );
	};
	const float max_turn = turn_at( theta );

	const float vel_yaw = rad2deg( std::atan2f( velocity.m_y, velocity.m_x ) );
	const float error   = std::remainderf( target - vel_yaw, 360.f );

	static float last_side = 1.f;
	float side = error > 0.f ? 1.f : -1.f, phi = theta;
	if ( std::fabsf( error ) < max_turn ) {
		const float want = error - last_side * max_turn * 0.5f;
		side             = want > 0.f ? 1.f : -1.f;

		const float need = std::min( std::fabsf( want ), max_turn );
		float lo = rad2deg( std::acos( std::clamp( cap / speed, 0.f, 1.f ) ) ), hi = theta;
		for ( int i = 0; i < 16; i++ ) {
			const float mid = 0.5f * ( lo + hi );
			( turn_at( mid ) < need ? lo : hi ) = mid;
		}
		phi = hi;
	}
	last_side = side;

	const float rel  = deg2rad( vel_yaw + side * phi - cmd->m_view_point.m_y );
	const float move = g_convars.float_or( HASH_BT( "cl_sidespeed" ), 450.f );

	cmd->m_forward_move = std::cosf( rel ) * move;
	cmd->m_side_move    = -std::sinf( rel ) * move;

	botox_dbg_log( "AS: v=%.1f th=%.2f phi=%.2f max_turn=%.2f err=%.2f ws=%.1f acc=%.2f fric=%.2f tgt=%.1f vel=%.1f turn=%d gnd=%d duck=%d", speed, theta, phi,
	               max_turn, error, ws, accel, g_ctx.m_local->get_surface_friction( ), target, vel_yaw, side > 0.f ? 1 : -1, ( flags & fl_onground ) ? 1 : 0,
	               ( cmd->m_buttons & in_duck ) ? 1 : 0 );
}
namespace {
	struct {
		float total     = 0.f;
		float applied   = 0.f;
		float start     = 0.f;
		float drained   = 0.f;
		float last_view = 0.f;
		bool have_last  = false;
	} g_so;
}

void n_movement::impl_t::strafe_optimizer( c_user_cmd* cmd )
{
	if ( !cmd )
		return;

	const float view_yaw  = cmd->m_view_point.m_y;
	const float user_turn = g_so.have_last ? std::remainderf( view_yaw - g_so.last_view - g_so.drained, 360.f ) : 0.f;
	g_so.last_view        = view_yaw;
	g_so.drained          = 0.f;
	g_so.have_last        = true;

	if ( !GET_VARIABLE( g_variables.m_strafe_optimizer, bool ) || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) ) {
		g_so.total = g_so.applied = 0.f;
		return;
	}
	if ( GET_VARIABLE( g_variables.m_auto_strafe, bool ) &&
	     g_input.check_input( &GET_VARIABLE( g_variables.m_auto_strafe_key, key_bind_t ) ) )
		return;
	if ( g_air_stuck_owns_cmd || starts_on_surf_pin( ) )
		return;

	const int move_type = g_ctx.m_local->get_move_type( );
	if ( move_type != e_move_types::move_type_walk || ( g_prediction.backup_data.m_flags & e_flags::fl_onground ) )
		return;

	const float fwd  = cmd->m_forward_move;
	const float side = cmd->m_side_move;
	if ( side == 0.f || fwd != m_user_forward_move_raw || side != m_user_side_move_raw )
		return;

	const c_vector vel = g_prediction.backup_data.m_velocity;
	const float speed  = vel.length_2d( );
	if ( speed < 1.f || speed <= GET_VARIABLE( g_variables.m_strafe_optimizer_min_speed, float ) )
		return;

	const float cap   = g_convars.float_or( HASH_BT( "sv_air_max_wishspeed" ), 30.f );
	const float ws    = std::min( air_wish_speed( cmd ), std::sqrt( fwd * fwd + side * side ) );
	const float accel = g_convars.float_or( HASH_BT( "sv_airaccelerate" ), 12.f ) * ws * n_tick::engine_interval( ) * g_ctx.m_local->get_surface_friction( );
	const float theta = rad2deg( std::acos( std::clamp( ( cap - accel ) / speed, 0.f, 1.f ) ) );

	const float offset  = rad2deg( std::atan2f( -side, fwd ) );
	const float vel_yaw = rad2deg( std::atan2f( vel.m_y, vel.m_x ) );

	const float err_l     = std::remainderf( vel_yaw + theta - offset - view_yaw, 360.f );
	const float err_r     = std::remainderf( vel_yaw - theta - offset - view_yaw, 360.f );
	const bool turn_left  = std::fabsf( err_l ) <= std::fabsf( err_r );
	const float err       = turn_left ? err_l : err_r;
	const float dir       = turn_left ? 1.f : -1.f;

	/* only a turn the player makes: mouse going the strafe's way and view short of best (never pull back) */
	if ( user_turn * dir <= 0.f || err * dir <= 0.f )
		return;

	const float step = err * std::clamp( GET_VARIABLE( g_variables.m_strafe_optimizer_gain, float ) / 100.f, 0.f, 1.f );

	cmd->m_view_point.m_y = std::remainderf( view_yaw + step, 360.f );
	g_ctx.old_view_point.m_y = cmd->m_view_point.m_y;

	/* camera: absolute target (unfinished drain is inside `err`), replace, never add */
	g_so.total   = step;
	g_so.applied = 0.f;
	g_so.start   = g_interfaces.m_global_vars_base->m_real_time;

	botox_dbg_log( "SO: v=%.1f th=%.2f err=%.2f step=%.2f turn=%.2f dir=%d back=%d off=%.0f", speed, theta, err, step, user_turn,
	               turn_left ? 1 : -1, std::fabsf( std::remainderf( view_yaw - vel_yaw, 360.f ) ) > 90.f ? 1 : 0, offset );
}

void n_movement::impl_t::strafe_optimizer_mouse( float* x )
{
	if ( !x || g_so.total == 0.f )
		return;

	const float m_yaw = g_convars.float_or( HASH_BT( "m_yaw" ), 0.022f );
	const float dt    = n_tick::interval( );
	const float age   = g_interfaces.m_global_vars_base->m_real_time - g_so.start;
	/* stale ( menu open skips the hook ) or unusable: drop, next pull re-targets */
	if ( m_yaw == 0.f || dt <= 0.f || age < 0.f || age > 4.f * dt ) {
		g_so.total = g_so.applied = 0.f;
		return;
	}

	const float t   = std::min( age / dt, 1.f );
	const float add = g_so.total * t - g_so.applied;
	g_so.applied += add;
	g_so.drained += add;
	if ( t >= 1.f )
		g_so.total = g_so.applied = 0.f;

	*x -= add / m_yaw;
}

float tb_wallstrafe_yaw( const c_vector& n, const c_vector& vel, float cap );

static bool ps_wall_ahead( const int n, c_vector* normal = nullptr )
{
	const auto col = g_ctx.m_local ? g_ctx.m_local->get_collideable( ) : nullptr;
	if ( !col )
		return false;
	const float dt      = n_tick::engine_interval( ) * static_cast< float >( n );
	const c_vector vel  = g_prediction.backup_data.m_velocity;
	const c_vector from = g_prediction.backup_data.m_origin;
	const c_vector to( from.m_x + vel.m_x * dt, from.m_y + vel.m_y * dt, from.m_z );
	const c_vector grow( 1.f, 1.f, 0.f );
	c_trace_filter fil( g_ctx.m_local );
	trace_t tr;
	ray_t ray( from, to, col->get_obb_mins( ) - grow, col->get_obb_maxs( ) + grow );
	g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
	const bool hit = tr.did_hit( ) && std::fabsf( tr.m_plane.m_normal.m_z ) < 0.7f;
	if ( hit && normal )
		*normal = tr.m_plane.m_normal;
	return hit;
}

static bool ps_wall_touch( c_vector& normal )
{
	const auto col = g_ctx.m_local ? g_ctx.m_local->get_collideable( ) : nullptr;
	if ( !col )
		return false;
	const c_vector from = g_ctx.m_local->get_origin( );
	const float dx[ 4 ] = { 0.05f, -0.05f, 0.f, 0.f }, dy[ 4 ] = { 0.f, 0.f, 0.05f, -0.05f };
	c_trace_filter fil( g_ctx.m_local );
	float best = 1.f;
	for ( int k = 0; k < 4; k++ ) {
		trace_t tr;
		ray_t ray( from, c_vector( from.m_x + dx[ k ], from.m_y + dy[ k ], from.m_z ), col->get_obb_mins( ), col->get_obb_maxs( ) );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
		if ( tr.did_hit( ) && !tr.m_start_solid && tr.m_plane.m_normal.m_z == 0.f && tr.m_fraction < best ) {
			best   = tr.m_fraction;
			normal = tr.m_plane.m_normal;
		}
	}
	return best < 1.f;
}

void n_movement::impl_t::pixel_surf( float  )
{
	auto& data = m_pixelsurf_data;
	if ( !( GET_VARIABLE( g_variables.m_pixel_surf, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_surf_key, key_bind_t ) ) ) ) {
		data.should_pixel_surf = data.should_unduck = false;
		return;
	}

	static int ticks    = 0;
	static int unpinned = 0;
	if ( !g_ctx.m_local )
		return;
	if ( !g_ctx.m_local->is_alive( ) ) {
		ticks = 0;
		return;
	}
	if ( g_ctx.m_local->get_move_type( ) == move_type_noclip || g_ctx.m_local->get_move_type( ) == move_type_ladder ) {
		data.should_pixel_surf = data.should_unduck = false;
		return;
	}
	if ( ( g_ctx.m_local->get_flags( ) & 1 ) || s_ps_bail )
		return;

	const bool latched     = data.should_pixel_surf || data.should_unduck;
	const bool as_owns     = g_air_stuck_holding || g_air_stuck_owns_cmd;
	const bool tb_owns     = g_texturebug.m_hit || ( g_texturebug.m_acted && !g_texturebug.m_assist );
	if ( as_owns || tb_owns || g_wall_climb.active( g_ctx.m_cmd ) || g_wall_climb.caught( g_ctx.m_cmd ) ) {
		if ( latched )
			botox_dbg_log( "PSU: yield by=%s", as_owns ? "as" : tb_owns ? "tb" : "wc" );
		data.should_pixel_surf = data.should_unduck = false;
		return;
	}

	if ( !latched && !wall_detected && !ps_wall_ahead( n_tick::ticks( 8 ) ) )
		return;
	if ( !latched && n_tick::cmd_clock_left( ) <= 0ll )
		return;

	c_user_cmd* cmd   = g_ctx.m_cmd;
	const int frame   = g_interfaces.m_prediction->m_commands_predicted - 1;
	const float pin   = g_prediction.get_engine_target_predict_z_velocity( );
	/* xy < 1 = wall hang ( stuck ), not a surf. 10-07 21:28 log: all 3 rides spd 0, one hung 53 ticks */
	const auto pinned = [ pin ]( const c_vector& v ) { return std::fabsf( v.m_z - pin ) < 0.01f && v.length_2d( ) >= 1.f; };

	const auto hunt = [ & ]( const int buttons, const int n, const int delay = 0 ) {
		const int live = cmd->m_buttons;
		g_prediction.restore_entity_to_predicted_frame( frame );
		const int base = g_ctx.m_local->get_tick_base( );
		int found = -1, run = 0;
		for ( int z = 0; z <= n && found < 0; z++ ) {
			cmd->m_buttons                  = z < delay ? live : buttons;
			g_ctx.m_local->get_tick_base( ) = base + z;
			g_prediction.begin( g_ctx.m_local, cmd );
			g_prediction.end( g_ctx.m_local );
			if ( g_ctx.m_local->get_flags( ) & 1 )
				break;
			run = pinned( g_ctx.m_local->get_velocity( ) ) ? run + 1 : 0;
			if ( run == 2 )
				found = z - 1;
		}
		cmd->m_buttons = live;
		g_prediction.restore_entity_to_predicted_frame( frame );
		return found;
	};

	const auto keeps_pin = [ & ]( ) {
		g_prediction.restore_entity_to_predicted_frame( frame );
		const int base = g_ctx.m_local->get_tick_base( );
		bool ok        = true;
		for ( int z = 0; z < 2 && ok; z++ ) {
			g_ctx.m_local->get_tick_base( ) = base + z;
			g_prediction.begin( g_ctx.m_local, cmd );
			g_prediction.end( g_ctx.m_local );
			ok = !( g_ctx.m_local->get_flags( ) & 1 ) && pinned( g_ctx.m_local->get_velocity( ) );
		}
		g_prediction.restore_entity_to_predicted_frame( frame );
		return ok;
	};

	const bool keys_free = !m_auto_bounce_data.m_owns_cmd && !m_fireman_data.owns_cmd;
	const auto hold      = [ & ]( ) -> int {
		g_prediction.restore_entity_to_predicted_frame( frame );
		c_vector n{ };
		if ( !keys_free || !ps_wall_touch( n ) )
			return 0;
		const float fwd = cmd->m_forward_move, side = cmd->m_side_move;
		const float y   = deg2rad( cmd->m_view_point.m_y );
		const c_vector wish( fwd * std::cosf( y ) + side * std::sinf( y ), fwd * std::sinf( y ) - side * std::cosf( y ), 0.f );
		const float len = wish.length_2d( );
		constexpr float k_leave_dot = 0.5f;
		if ( len > 0.01f && ( wish.m_x * n.m_x + wish.m_y * n.m_y ) / len > k_leave_dot )
			return 0;
		/* strafing into the face = 450 at the best along-wall strafe yaw ( wall eats the into part ), only if it keeps the pin */
		constexpr float k_gain_dot = 0.25f;
		if ( len > 0.01f && -( wish.m_x * n.m_x + wish.m_y * n.m_y ) / len > k_gain_dot ) {
			const float gy = tb_wallstrafe_yaw( n, g_prediction.backup_data.m_velocity, g_convars.float_or( HASH_BT( "sv_air_max_wishspeed" ), 30.f ) );
			if ( gy != FLT_MAX ) {
				set_move_toward_yaw( cmd, gy, 450.f );
				if ( keeps_pin( ) ) {
					const float r       = deg2rad( gy - g_ctx.old_view_point.m_y );
					cmd->m_forward_move = std::cosf( r ) * 450.f;
					cmd->m_side_move    = -std::sinf( r ) * 450.f;
					return 4;
				}
				cmd->m_forward_move = fwd;
				cmd->m_side_move    = side;
			}
		}
		if ( keeps_pin( ) )
			return 1;
		const float yaws[ 2 ] = { rad2deg( std::atan2f( wish.m_y, wish.m_x ) ), rad2deg( std::atan2f( -n.m_y, -n.m_x ) ) };
		const float presses[ 2 ] = { 45.f, 450.f };
		for ( int i = 0; i < 2; i++ ) {
			if ( i == 0 && len <= 0.01f )
				continue;
			for ( const float f : presses ) {
				set_move_toward_yaw( cmd, yaws[ i ], f );
				if ( !keeps_pin( ) )
					continue;
				const float r       = deg2rad( yaws[ i ] - g_ctx.old_view_point.m_y );
				cmd->m_forward_move = std::cosf( r ) * f;
				cmd->m_side_move    = -std::sinf( r ) * f;
				return 2 + i;
			}
		}
		cmd->m_forward_move = fwd;
		cmd->m_side_move    = side;
		return 0;
	};
	static int last_by = 0;
	const auto log_hold = [ & ]( const int by ) {
		data.m_ps_hold |= by != 0 || latched;
		if ( by != last_by )
			botox_dbg_log( "PSU: hold by=%s fwd=%.0f side=%.0f vz=%.2f", by == 0 ? "none" : by == 1 ? "keys" : by == 2 ? "wish" : by == 3 ? "wall" : "gain",
			               cmd->m_forward_move, cmd->m_side_move, g_prediction.backup_data.m_velocity.m_z );
		last_by = by;
	};

	if ( !latched ) {
		const bool holds_duck = ( cmd->m_buttons & in_duck ) != 0;
		const int held        = hold( );
		log_hold( held );
		if ( held )
			return;
		if ( hunt( cmd->m_buttons, n_tick::ticks( 8 ) ) >= 0 )
			return;
		const int flip_buttons = holds_duck ? cmd->m_buttons & ~in_duck : cmd->m_buttons | in_duck;
		const int live_buttons = cmd->m_buttons;
		int flip               = hunt( flip_buttons, n_tick::ticks( 8 ) );
		/* your crouch is dropped only on the last tick that still catches: one more crouched tick still pins = wait,
		   so a guess 8 ticks out never uncrouches you (path / keys change before the lip = nothing lost) */
		if ( holds_duck && flip >= 0 && hunt( flip_buttons, n_tick::ticks( 8 ), 1 ) >= 0 )
			return;
		if ( flip < 0 ) {
			cmd->m_buttons = flip_buttons;
			const int by   = hold( );
			if ( by < 2 ) {
				cmd->m_buttons = live_buttons;
				return;
			}
			log_hold( by );
			flip = 0;
		}
		( holds_duck ? data.should_unduck : data.should_pixel_surf ) = true;
		data.m_ps_hold = true;
		ticks          = cmd->m_tick_count + flip + n_tick::ticks( 16 );
		unpinned       = 0;
		cmd->m_buttons = flip_buttons;
		botox_dbg_log( "PSU: latch %s in=%d z=%.3f vz=%.2f spd=%.1f", holds_duck ? "stand" : "duck", flip, g_prediction.backup_data.m_origin.m_z,
		               g_prediction.backup_data.m_velocity.m_z, g_prediction.backup_data.m_velocity.length_2d( ) );
		return;
	}

	const int own_buttons = cmd->m_buttons;
	cmd->m_buttons        = data.should_pixel_surf ? cmd->m_buttons | in_duck : cmd->m_buttons & ~in_duck;
	const int held        = hold( );
	log_hold( held );
	const c_vector& v0 = g_prediction.backup_data.m_velocity;
	const bool stuck   = !held && std::fabsf( v0.m_z - pin ) < 0.01f && v0.length_2d( ) < 1.f;
	if ( stuck )
		cmd->m_buttons = own_buttons;
	else {
		if ( cmd->m_tick_count <= ticks )
			return;
		unpinned = pinned( v0 ) ? 0 : unpinned + 1;
		if ( unpinned < 2 )
			return;
		if ( hunt( cmd->m_buttons, n_tick::ticks( 4 ) ) >= 0 ) {
			unpinned = 0;
			return;
		}
	}
	botox_dbg_log( "PSU: release %s vz=%.2f stuck=%d", data.should_pixel_surf ? "duck" : "stand", v0.m_z, stuck ? 1 : 0 );
	data.should_pixel_surf = data.should_unduck = false;
}

bool ps_latched( )
{
	return g_movement.m_pixelsurf_data.should_pixel_surf || g_movement.m_pixelsurf_data.should_unduck;
}

bool eb_cmd_taken( );

namespace {
	struct tung_snap_t {
		int cmd       = -1;
		int tick_base = -1;
		c_vector from{ };
		c_vector to{ };
		bool sv_done = false;
		float yaw = 0.f, len = 0.f;
	};
	tung_snap_t s_tung[ 128 ];

	struct tung_plane_t {
		float p = 0.f, d = 0.f;
		c_vector n{ }, at{ };
		float a0 = 0.f;
	};
	tung_plane_t s_learn[ 16 ];
	int s_learn_n = 0, s_learn_next = 0;

	/* map brushes sit on the 16u grid: plane -48 / -160 rides, odd planes never did */
	int tung_grid_rank( const float p )
	{
		const int i = static_cast< int >( p );
		return i % 16 == 0 ? 0 : i % 8 == 0 ? 1 : i % 4 == 0 ? 2 : i % 2 == 0 ? 3 : 4;
	}

	int s_tung_stance = -1, s_tung_grace = 0;

	/* largest hull move along dir that the trace still calls clear, bisected
	   over the float's bit pattern (exact to the ulp). -1 = nothing within 1u */
	float tung_wall_gap( const c_vector& from, const c_vector& dir )
	{
		const auto col = g_ctx.m_local ? g_ctx.m_local->get_collideable( ) : nullptr;
		if ( !col )
			return -1.f;
		c_trace_filter fil( g_ctx.m_local );
		const auto clear = [ & ]( const float d ) {
			trace_t tr;
			ray_t ray( from, c_vector( from.m_x + dir.m_x * d, from.m_y + dir.m_y * d, from.m_z ), col->get_obb_mins( ), col->get_obb_maxs( ) );
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
			return !tr.m_start_solid && !tr.did_hit( );
		};
		if ( clear( 1.f ) )
			return -1.f;
		const auto to_f = []( const std::uint32_t b ) {
			float f;
			std::memcpy( &f, &b, 4 );
			return f;
		};
		const float one = 1.f;
		std::uint32_t lo = 0u, hi;
		std::memcpy( &hi, &one, 4 );
		while ( lo < hi ) {
			const std::uint32_t mid = lo + ( hi - lo + 1u ) / 2u;
			if ( clear( to_f( mid ) ) )
				lo = mid;
			else
				hi = mid - 1u;
		}
		return to_f( lo );
	}

	bool tung_slant_touch( const c_vector& at )
	{
		const auto col = g_ctx.m_local ? g_ctx.m_local->get_collideable( ) : nullptr;
		if ( !col )
			return false;
		const float dx[ 5 ] = { 0.05f, -0.05f, 0.f, 0.f, 0.f }, dy[ 5 ] = { 0.f, 0.f, 0.05f, -0.05f, 0.f }, dz[ 5 ] = { 0.f, 0.f, 0.f, 0.f, -0.05f };
		c_trace_filter fil( g_ctx.m_local );
		for ( int k = 0; k < 5; k++ ) {
			trace_t tr;
			ray_t ray( at, c_vector( at.m_x + dx[ k ], at.m_y + dy[ k ], at.m_z + dz[ k ] ), col->get_obb_mins( ), col->get_obb_maxs( ) );
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
			if ( const float nz = std::fabsf( tr.m_plane.m_normal.m_z ); tr.did_hit( ) && !tr.m_start_solid && nz > 0.01f && nz < 0.99f )
				return true;
		}
		return false;
	}

	/* diagonal wall / seam noise zeroes the whole vector, gravity then lands vz exactly on the pin: xy 0 = hang, not a surf */
	bool tung_start_stuck( )
	{
		return starts_on_surf_pin( ) && s_cmd_start.vel.length_2d( ) < 1.f;
	}
}

void tung_surf_apply_sv( c_move_data* mv )
{
	if ( !mv || !g_interfaces.m_engine_client )
		return;
	const int local = g_interfaces.m_engine_client->get_local_player( );
	if ( local <= 0 || static_cast< int >( mv->m_player_handle & 0xFFFF ) != local )
		return;
	for ( auto& s : s_tung ) {
		if ( s.cmd < 0 || s.sv_done || ( mv->m_abs_origin - s.from ).length( ) > 0.05f )
			continue;
		botox_dbg_log( "TUNG: server run cmd=%d z %.3f -> %.3f xy %.5f", s.cmd, mv->m_abs_origin.m_z, s.to.m_z, ( s.to - s.from ).length_2d( ) );
		mv->m_abs_origin             = s.to;
		mv->m_game_code_moved_player = true;
		s.sv_done                    = true;
		return;
	}
}

bool tung_surf_apply( c_base_entity* player, c_move_data* mv )
{
	if ( !player || player != g_ctx.m_local || !mv )
		return false;
	const c_user_cmd* cur = *player->get_current_command( );
	if ( !cur )
		return false;
	const auto& s = s_tung[ cur->m_command_number & 127 ];
	if ( s.cmd != cur->m_command_number )
		return false;
	const float d = ( mv->m_abs_origin - s.from ).length( );
	if ( g_prediction.m_in_begin ? player->get_tick_base( ) != s.tick_base : d > 8.f )
		return false;
	if ( !g_prediction.m_in_begin ) {
		static int logged = -1;
		if ( logged != s.cmd )
			botox_dbg_log( "TUNG: engine run cmd=%d tb=%d/%d d=%.3f z %.3f -> %.3f", s.cmd, player->get_tick_base( ), s.tick_base, d,
			               mv->m_abs_origin.m_z, s.to.m_z );
		logged = s.cmd;
	}
	/* same start = exact floats ( the wall pull is ulp exact ), a corrected start keeps the pull as an offset */
	if ( d < 0.05f )
		mv->m_abs_origin = s.to;
	else
		mv->m_abs_origin = c_vector( mv->m_abs_origin.m_x + s.to.m_x - s.from.m_x, mv->m_abs_origin.m_y + s.to.m_y - s.from.m_y, s.to.m_z );
	mv->m_game_code_moved_player = true;
	return true;
}

void n_movement::impl_t::tung_surf( c_user_cmd* cmd )
{
	const bool on = GET_VARIABLE( g_variables.m_tung_surf, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_tung_surf_key, key_bind_t ) );

	static bool y6_sent = false;
	if ( const bool want = on && GET_VARIABLE( g_variables.m_tung_surf_y6, bool ); want != y6_sent ) {
		g_interfaces.m_engine_client->execute_client_cmd( want ? "y6_ast 1" : "y6_ast 0" );
		y6_sent = want;
	}
	static bool was_on = false;
	if ( on != was_on )
		botox_dbg_log( "TUNG: bind %s", on ? "on" : "off" );
	was_on = on;

	if ( const auto nci = g_interfaces.m_engine_client->get_net_channel_info( ); nci && !nci->is_loopback( ) ) {
		for ( auto& s : s_tung )
			s.cmd = -1;
		s_tung_stance = -1;
		return;
	}

	int &s_stance = s_tung_stance, &s_stance_grace = s_tung_grace;
	if ( s_ps_bail && s_stance >= 0 ) {
		botox_dbg_log( "TUNG: stance bail %s", s_stance ? "duck" : "stand" );
		s_stance = -1;
	}
	if ( s_stance >= 0 ) {
		const bool stuck  = tung_start_stuck( );
		const bool riding = cmd && g_ctx.m_local && !stuck && starts_on_surf_pin( ) && !tung_slant_touch( g_prediction.backup_data.m_origin );
		if ( riding || ( cmd && !stuck && s_stance_grace > 0 ) ) {
			cmd->m_buttons = ( cmd->m_buttons & ~in_duck ) | s_stance;
			s_stance_grace = riding ? 0 : s_stance_grace - 1;
		} else {
			botox_dbg_log( "TUNG: stance release %s", s_stance ? "duck" : "stand" );
			s_stance = -1;
		}
	}

	if ( !on || !cmd || !g_ctx.m_local || g_ctx.m_local->get_move_type( ) != e_move_types::move_type_walk )
		return;
	static float s_ban = FLT_MAX;
	if ( ( g_prediction.backup_data.m_flags & fl_onground ) || g_prediction.backup_data.m_velocity.m_z > 0.f )
		s_ban = FLT_MAX;
	else if ( starts_on_surf_pin( ) && std::floor( g_prediction.backup_data.m_origin.m_z ) < s_ban ) {
		s_ban = std::floor( g_prediction.backup_data.m_origin.m_z );
		botox_dbg_log( "TUNG: ban plane>=%.0f", s_ban );
	}
	if ( eb_cmd_taken( ) || ( g_texturebug.m_acted && !g_texturebug.m_assist ) || g_air_stuck_holding || g_air_stuck_owns_cmd || ps_latched( ) )
		return;
	if ( s_ps_bail )
		return;
	const c_vector o0 = g_prediction.backup_data.m_origin;
	const c_vector v0 = g_prediction.backup_data.m_velocity;
	static bool s_chain = false;
	if ( ( g_prediction.backup_data.m_flags & fl_onground ) || v0.m_z > 0.f ) {
		s_chain = false;
		return;
	}
	c_vector n{ };
	if ( !ps_wall_touch( n ) && !ps_wall_ahead( 1, &n ) )
		return;
	if ( n_tick::cmd_clock_left( ) <= 0ll )
		return;
	n_tick::c_sim_budget budget;
	budget.start( 0.25f, 0.f, n_tick::search_tung );
	const int cn = cmd->m_command_number;
	if ( !s_chain && s_tung[ ( cn - 1 ) & 127 ].cmd == cn - 1 && s_tung[ ( cn - 2 ) & 127 ].cmd == cn - 2 ) {
		s_chain = true;
		botox_dbg_log( "TUNG: chain stop z=%.3f vz=%.2f", o0.m_z, v0.m_z );
	}

	constexpr float k_eps = 0.03125f;
	const float pin       = g_prediction.get_engine_target_predict_z_velocity( );
	const auto pin_vz = [ pin ]( ) {
		return !( g_ctx.m_local->get_flags( ) & fl_onground ) && std::fabsf( g_ctx.m_local->get_velocity( ).m_z - pin ) < 0.01f;
	};
	const auto pinned = [ & ]( ) { return pin_vz( ) && g_ctx.m_local->get_velocity( ).length_2d( ) >= 1.f; };
	const auto stuck  = [ & ]( ) { return pin_vz( ) && g_ctx.m_local->get_velocity( ).length_2d( ) < 1.f; };
	const int frame = g_interfaces.m_prediction->m_commands_predicted - 1;
	g_prediction.restore_entity_to_predicted_frame( frame );
	const int base = g_ctx.m_local->get_tick_base( );
	auto& slot     = s_tung[ cmd->m_command_number & 127 ];
	slot.cmd       = -1;
	const float a0 = g_ctx.m_local->get_duck_amount( );
	if ( a0 > 0.f && a0 < 1.f )
		return;

	const float live_fwd = cmd->m_forward_move, live_side = cmd->m_side_move;
	const float vy       = deg2rad( cmd->m_view_point.m_y );
	const float wx = live_fwd * std::cosf( vy ) + live_side * std::sinf( vy ), wy = live_fwd * std::sinf( vy ) - live_side * std::cosf( vy );
	const float wlen = std::sqrt( wx * wx + wy * wy );
	const float nlen = std::sqrt( n.m_x * n.m_x + n.m_y * n.m_y );
	const float away = wlen > 0.01f && nlen > 0.01f ? ( wx * n.m_x + wy * n.m_y ) / ( wlen * nlen ) : 0.f;
	const bool press = GET_VARIABLE( g_variables.m_tung_surf_press, bool ) && nlen > 0.01f && away <= 0.5f;
	const float press_yaw = rad2deg( std::atan2f( -n.m_y, -n.m_x ) );
	const auto use_press  = [ & ]( const bool on_ ) {
		if ( on_ )
			set_move_toward_yaw( cmd, press_yaw, 450.f );
		else {
			cmd->m_forward_move = live_fwd;
			cmd->m_side_move    = live_side;
		}
	};
	const auto commit_move = [ & ]( const float yaw, const float len ) {
		const float r       = deg2rad( yaw - g_ctx.old_view_point.m_y );
		cmd->m_forward_move = std::cosf( r ) * len;
		cmd->m_side_move    = -std::sinf( r ) * len;
	};
	const auto commit_press = [ & ]( ) { commit_move( press_yaw, 450.f ); };
	const c_vector nu = nlen > 0.01f ? c_vector( n.m_x / nlen, n.m_y / nlen, 0.f ) : c_vector( 0.f, 0.f, 0.f );

	{
		static std::string s_map;
		if ( const std::string map = g_interfaces.m_engine_client->get_level_name_short( ); map != s_map ) {
			s_map        = map;
			s_learn_n    = 0;
			s_learn_next = 0;
		}
	}
	/* any stance: hull mins z = 0 both ways, feet = origin, so a seam ridden ducked sits on the same plane standing
	   ( was stance keyed: rides are mostly ducked, standing never got a learned plane ) */
	const auto learned_at = [ & ]( const float p ) -> tung_plane_t* {
		if ( nlen <= 0.01f )
			return nullptr;
		for ( int i = 0; i < s_learn_n; i++ ) {
			auto& l = s_learn[ i ];
			if ( l.p == p && l.n.m_x * nu.m_x + l.n.m_y * nu.m_y > 0.99f &&
			     std::fabsf( nu.m_x * o0.m_x + nu.m_y * o0.m_y - l.d ) < 4.f && ( o0 - l.at ).length_2d( ) < 1024.f )
				return &l;
		}
		return nullptr;
	};

	if ( starts_on_surf_pin( ) ) {
		if ( tung_slant_touch( o0 ) || tung_start_stuck( ) ) {
			static int s_slant_cn = -1;
			if ( s_slant_cn != cn - 1 )
				botox_dbg_log( "TUNG: %s ride let go z=%.3f vz=%.2f spd=%.1f", tung_start_stuck( ) ? "stuck" : "slant", o0.m_z, v0.m_z,
				               s_cmd_start.vel.length_2d( ) );
			s_slant_cn = cn;
			return;
		}
		s_chain = false;
		if ( const float p = std::round( o0.m_z - 0.5f * k_eps ); nlen > 0.01f && std::fabsf( o0.m_z - ( p + 0.5f * k_eps ) ) < 0.01f ) {
			if ( auto l = learned_at( p ) ) {
				l->at = o0;
				l->a0 = a0;
			}
			else {
				s_learn[ s_learn_next ] = { p, nu.m_x * o0.m_x + nu.m_y * o0.m_y, nu, o0, a0 };
				s_learn_next            = ( s_learn_next + 1 ) % 16;
				s_learn_n               = std::min( s_learn_n + 1, 16 );
				botox_dbg_log( "TUNG: learn plane=%.0f n=%.2f,%.2f duck=%.0f count=%d", p, nu.m_x, nu.m_y, a0, s_learn_n );
			}
		}
		static bool held = false;
		if ( !press ) {
			held = false;
			return;
		}
		const auto keeps = [ & ]( ) {
			bool ok = true;
			for ( int k = 0; k < 2 && ok; k++ ) {
				g_ctx.m_local->get_tick_base( ) = base + k;
				g_prediction.begin( g_ctx.m_local, cmd );
				g_prediction.end( g_ctx.m_local );
				ok = pinned( );
			}
			g_prediction.restore_entity_to_predicted_frame( frame );
			return ok;
		};
		bool now = false;
		if ( !keeps( ) ) {
			use_press( true );
			now = keeps( );
			use_press( false );
			if ( now )
				commit_press( );
		}
		if ( now != held )
			botox_dbg_log( "TUNG: hold %s fwd=%.0f side=%.0f away=%.2f", now ? "press" : "keys", cmd->m_forward_move, cmd->m_side_move, away );
		held = now;
		return;
	}

	/* live keys pin 2 ticks running before landing = the real surf is coming: never snap over it
	   ( same test as auto ps hunt ). one log per yield run */
	{
		int run = 0, found = -1;
		for ( int z = 0; z <= n_tick::ticks( 8 ) && found < 0; z++ ) {
			g_ctx.m_local->get_tick_base( ) = base + z;
			g_prediction.begin( g_ctx.m_local, cmd );
			g_prediction.end( g_ctx.m_local );
			if ( g_ctx.m_local->get_flags( ) & fl_onground )
				break;
			run = pinned( ) ? run + 1 : 0;
			if ( run == 2 )
				found = z - 1;
		}
		g_prediction.restore_entity_to_predicted_frame( frame );
		static int s_yield_cn = -1;
		if ( found >= 0 ) {
			if ( s_yield_cn != cn - 1 )
				botox_dbg_log( "TUNG: yield real surf in=%d z=%.3f vz=%.2f", found, o0.m_z, v0.m_z );
			s_yield_cn = cn;
			return;
		}
	}

	if ( const auto& prev = s_tung[ ( cn - 1 ) & 127 ]; prev.cmd == cn - 1 ) {
		bool ok = false;
		const bool can = GET_VARIABLE( g_variables.m_tung_surf_press, bool ) && prev.len > 0.01f && away <= 0.5f;
		if ( can ) {
			set_move_toward_yaw( cmd, prev.yaw, prev.len );
			ok = true;
			for ( int k = 0; k < 2 && ok; k++ ) {
				g_ctx.m_local->get_tick_base( ) = base + k;
				g_prediction.begin( g_ctx.m_local, cmd );
				g_prediction.end( g_ctx.m_local );
				ok = pinned( );
			}
			g_prediction.restore_entity_to_predicted_frame( frame );
			cmd->m_forward_move = live_fwd;
			cmd->m_side_move    = live_side;
		}
		const float live_yaw = wlen > 0.01f ? rad2deg( std::atan2f( wy, wx ) ) : 0.f;
		botox_dbg_log( "TUNG: snap keys %s live yaw=%.1f len=%.0f snap yaw=%.1f len=%.0f z=%.3f vz=%.2f", !can ? "off" : ok ? "hold" : "fail",
		               live_yaw, wlen, prev.yaw, prev.len, o0.m_z, v0.m_z );
		if ( ok ) {
			commit_move( prev.yaw, prev.len );
			return;
		}
	}

	const bool pressing = press && away >= -0.3f;
	use_press( pressing );

	const float fall     = std::max( 0.f, -( v0.m_z + pin ) * n_tick::engine_interval( ) );
	const float snap     = std::clamp( GET_VARIABLE( g_variables.m_tung_surf_snap, float ), 0.f, 32.f );
	const int live_btn   = cmd->m_buttons;
	const auto col       = g_ctx.m_local->get_collideable( );
	c_trace_filter fil( g_ctx.m_local );
	const c_vector dir = nlen > 0.01f ? c_vector( -n.m_x / nlen, -n.m_y / nlen, 0.f ) : c_vector( 0.f, 0.f, 0.f );
	const float gap    = nlen > 0.01f ? tung_wall_gap( o0, dir ) : -1.f;
	int tried[ 2 ] = { }, count[ 2 ] = { }, f_path = 0, f_t0 = 0, f_t1 = 0, f_t2 = 0, f_slant = 0, f_stuck = 0, pulls = 0, flip_took = -1, f_stance = 0;
	int lips[ 2 ] = { }, lip_fail[ 3 ] = { };
	int banned = 0, learned = 0;
	int regaps = 0, f_pull_blk = 0;
	float z_nat0 = o0.m_z;
	for ( int st = 0; st < 2 && !( st && budget.expired( ) ); st++ ) {
		cmd->m_buttons = st ? live_btn ^ in_duck : live_btn;
		const int stance = cmd->m_buttons & in_duck;
		const auto take  = [ & ]( ) {
			use_press( false );
			if ( pressing )
				commit_press( );
			if ( st ) {
				s_stance       = stance;
				s_stance_grace = 1;
			}
		};
		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		const c_vector e    = g_ctx.m_local->get_abs_origin( );
		const float z_nat   = e.m_z;
		const bool nat_pin  = pinned( );
		const bool nat_land = ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;
		const bool e_duck   = ( g_ctx.m_local->get_flags( ) & e_flags::fl_ducking ) != 0;
		/* stance changed inside the tick = mid crouch catch: never snap / flip into it */
		const bool settled = g_ctx.m_local->get_duck_amount( ) == a0;
		const bool nat_slant = nat_pin && tung_slant_touch( e );
		if ( st )
			flip_took = e_duck == ( stance != 0 ) ? 1 : 0;
		g_prediction.restore_entity_to_predicted_frame( frame );
		if ( !st )
			z_nat0 = z_nat;
		if ( nat_land ) {
			if ( !st ) {
				use_press( false );
				return;
			}
			continue;
		}
		if ( nat_pin && ( !st || settled ) ) {
			if ( nat_slant && ( pressing || st ) ) {
				if ( st )
					continue;
				use_press( false );
				return;
			}
			take( );
			if ( pressing || st )
				botox_dbg_log( "TUNG: catch by %s z=%.3f vz=%.2f", st ? ( stance ? "duck" : "stand" ) : "press", o0.m_z, v0.m_z );
			return;
		}
		if ( !settled ) {
			f_stance++;
			continue;
		}

		float planes[ 128 ];
		int& n_pl = count[ st ];
		for ( float p = std::ceil( z_nat - snap - 0.5f * k_eps ); p + 0.5f * k_eps - z_nat <= fall + snap && n_pl < 128; p += 1.f ) {
			if ( p >= s_ban ) {
				banned++;
				continue;
			}
			if ( const float shift = p + 0.5f * k_eps - z_nat; snap > 0.f || shift > 0.f )
				planes[ n_pl++ ] = p;
		}
		std::sort( planes, planes + n_pl, [ z_nat ]( const float a, const float b ) { return std::fabsf( a - z_nat ) < std::fabsf( b - z_nat ); } );

		const auto outside = [ & ]( const float p ) {
			const float shift = p + 0.5f * k_eps - z_nat;
			return shift <= 0.f ? -shift : std::max( 0.f, shift - fall );
		};
		struct cand_t {
			float p;
			int pull;
			bool lip;
			double rank;
		};
		const auto rank_of = [ & ]( const float p ) {
			const int tier = learned_at( p ) ? 0 : outside( p ) == 0.f ? 1 : 2;
			return tier * 1e6 + tung_grid_rank( p ) * 1e4 + static_cast< double >( outside( p ) );
		};
		cand_t cands[ 256 ];
		int n_c = 0;
		const c_vector hmin = col ? col->get_obb_mins( ) : c_vector( -16.f, -16.f, 0.f );
		const c_vector hmax( col ? col->get_obb_maxs( ).m_x : 16.f, col ? col->get_obb_maxs( ).m_y : 16.f, e_duck ? 54.f : 72.f );
		for ( int i = 0; i < n_pl && !s_chain; i++ ) {
			const double r = rank_of( planes[ i ] );
			learned += r < 1e6;
			for ( int pull = 0; pull < ( gap > 0.f && col ? 2 : 1 ); pull++ )
				cands[ n_c++ ] = { planes[ i ], pull, false, r };
		}
		const auto lip_at = [ & ]( const cand_t& c ) {
			const c_vector s( e.m_x + dir.m_x * gap * c.pull, e.m_y + dir.m_y * gap * c.pull, c.p + 0.5f * k_eps );
			trace_t tr;
			ray_t ray( s, c_vector( s.m_x, s.m_y, s.m_z - 1.f ), hmin, hmax );
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
			return !tr.m_start_solid && tr.did_hit( ) && s.m_z - tr.m_end.m_z < k_eps;
		};
		std::stable_sort( cands, cands + n_c, []( const cand_t& a, const cand_t& b ) { return a.rank < b.rank; } );

		/* predicted ride length ranks proven snaps: a short one waits up to 8 more tries for a longer ride */
		const int ride_max = 2 + n_tick::ticks( 6 );
		int cap            = 32;
		struct {
			tung_snap_t slot;
			float p, shift, pull_gap;
			bool regap, lip;
			tung_plane_t* l;
			int ride, at_try;
		} best{ };
		for ( int i = 0, grp_end = 0; i < n_c && !s_chain && tried[ st ] < cap && !budget.expired( ); i++ ) {
			if ( i == grp_end ) {
				const double o = cands[ i ].rank;
				for ( grp_end = i; grp_end < n_c && cands[ grp_end ].rank == o; grp_end++ ) {
					cands[ grp_end ].lip = lip_at( cands[ grp_end ] );
					lips[ st ] += cands[ grp_end ].lip;
				}
				std::stable_partition( cands + i, cands + grp_end, []( const cand_t& c ) { return c.lip; } );
			}
			const float p     = cands[ i ].p;
			const int pull    = cands[ i ].pull;
			const float shift = p + 0.5f * k_eps - z_nat;
			if ( col ) {
				trace_t tr;
				ray_t ray( o0, c_vector( o0.m_x, o0.m_y, o0.m_z + shift ), col->get_obb_mins( ), col->get_obb_maxs( ) );
				g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
				if ( tr.m_start_solid || tr.m_fraction < 1.f ) {
					f_path++;
					continue;
				}
			}
			c_vector to( o0.m_x, o0.m_y, o0.m_z + shift );
			float pull_gap = gap;
			if ( pull ) {
				const c_vector at = to;
				to                = c_vector( at.m_x + dir.m_x * gap, at.m_y + dir.m_y * gap, at.m_z );
				/* the wall at the snapped height may sit elsewhere: pull must stay clear there too */
				trace_t tr;
				ray_t ray( at, to, col->get_obb_mins( ), col->get_obb_maxs( ) );
				g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
				if ( tr.m_start_solid || tr.did_hit( ) ) {
					pull_gap = tr.m_start_solid ? -1.f : tung_wall_gap( at, dir );
					if ( pull_gap <= 0.f ) {
						f_pull_blk++;
						continue;
					}
					regaps++;
					to = c_vector( at.m_x + dir.m_x * pull_gap, at.m_y + dir.m_y * pull_gap, at.m_z );
				}
			}
			tried[ st ]++;
			pulls += pull;
			slot    = { cmd->m_command_number, base, o0, to, false, pressing ? press_yaw : rad2deg( std::atan2f( wy, wx ) ), pressing ? 450.f : wlen };
			int bad = -1, ride = 2;
			for ( int k = 0; k < 3 && bad < 0; k++ ) {
				g_ctx.m_local->get_tick_base( ) = base + k;
				g_prediction.begin( g_ctx.m_local, cmd );
				g_prediction.end( g_ctx.m_local );
				if ( k > 0 && stuck( ) )
					bad = 4;
				else if ( ( k == 0 ? ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0 : !pinned( ) ) || g_ctx.m_local->get_duck_amount( ) != a0 )
					bad = k;
				else if ( k == 2 && tung_slant_touch( g_ctx.m_local->get_abs_origin( ) ) )
					bad = 3;
			}
			/* ride on with the same keys: a slide that hits a seam / diagonal face hangs a few ticks later */
			for ( int k = 3; bad < 0 && k < 3 + n_tick::ticks( 6 ); k++, ride++ ) {
				g_ctx.m_local->get_tick_base( ) = base + k;
				g_prediction.begin( g_ctx.m_local, cmd );
				g_prediction.end( g_ctx.m_local );
				if ( stuck( ) )
					bad = 4;
				else if ( !pinned( ) || g_ctx.m_local->get_duck_amount( ) != a0 )
					break;
			}
			g_prediction.restore_entity_to_predicted_frame( frame );
			if ( bad < 0 ) {
				if ( ride > best.ride ) {
					if ( !best.ride )
						cap = std::min( cap, tried[ st ] + 8 );
					best = { slot, p, shift, pull ? pull_gap : 0.f, pull && pull_gap != gap, cands[ i ].lip, learned_at( p ), ride, tried[ st ] };
				}
				slot.cmd = -1;
				if ( ride >= ride_max )
					break;
				continue;
			}
			( bad == 0 ? f_t0 : bad == 1 ? f_t1 : bad == 2 ? f_t2 : bad == 3 ? f_slant : f_stuck )++;
			if ( cands[ i ].lip && bad < 3 )
				lip_fail[ bad ]++;
			slot.cmd = -1;
		}
		if ( best.ride ) {
			slot = best.slot;
			take( );
			const auto l = best.l;
			botox_dbg_log( "TUNG: snap plane=%.0f shift=%.3f pull=%.6f regap=%d lip=%d learned=%d grid=%d z=%.3f nat=%.3f fall=%.2f vz=%.2f spd=%.1f duck=%d/%d flip=%d "
			               "try=%d/%d press=%d away=%.2f ride=%d/%d",
			               best.p, best.shift, best.pull_gap, best.regap ? 1 : 0, best.lip ? 1 : 0, !l ? 0 : l->a0 == a0 ? 1 : 2, tung_grid_rank( best.p ), o0.m_z, z_nat,
			               fall, v0.m_z, v0.length_2d( ), stance ? 1 : 0, ( g_prediction.backup_data.m_flags & e_flags::fl_ducking ) ? 1 : 0, st, best.at_try,
			               tried[ st ], pressing ? 1 : 0, away, best.ride, ride_max );
			return;
		}
		/* held stance settled: the flip always changes stance in its tick = mid crouch, never snaps ( log: every flip
		   mid_crouch=1, tried +0 ). only a held transition leaves the flip as "keep stance" */
		if ( settled )
			break;
	}
	cmd->m_buttons = live_btn;
	use_press( false );

	static unsigned long long next = 0ull;
	if ( const unsigned long long now = GetTickCount64( ); now >= next ) {
		botox_dbg_log( "TUNG: miss us=%lld/%lld ban=%d learned=%d tried=%d+%d/%d+%d lips=%d+%d lip_fail=%d/%d/%d chain=%d pulls=%d gap=%.6f regap=%d "
		               "pull_blk=%d flip_took=%d mid_crouch=%d path=%d t0=%d t1=%d t2=%d slant=%d stuck=%d z=%.3f nat=%.3f fall=%.2f vz=%.2f spd=%.1f duck=%d/%d press=%d away=%.2f",
		               budget.used_us( ), budget.used_us( ) + budget.left_us( ), banned, learned,
		               tried[ 0 ], tried[ 1 ], count[ 0 ] * ( gap > 0.f && col ? 2 : 1 ), count[ 1 ] * ( gap > 0.f && col ? 2 : 1 ), lips[ 0 ],
		               lips[ 1 ], lip_fail[ 0 ], lip_fail[ 1 ], lip_fail[ 2 ], s_chain ? 1 : 0, pulls, gap, regaps, f_pull_blk, flip_took, f_stance, f_path, f_t0, f_t1,
		               f_t2, f_slant, f_stuck,
		               o0.m_z, z_nat0, fall, v0.m_z, v0.length_2d( ), ( live_btn & in_duck ) ? 1 : 0,
		               ( g_prediction.backup_data.m_flags & e_flags::fl_ducking ) ? 1 : 0, pressing ? 1 : 0, away );
		next = now + 250ull;
	}
}

bool bind_cmd_taken( )
{
	return g_texturebug.m_hit || g_texturebug.m_hs_hit || g_texturebug.m_hb_hit || g_texturebug.m_owns_cmd ||
	       g_movement.m_fireman_data.owns_cmd || g_movement.m_auto_bounce_data.m_owns_cmd;
}
bool eb_cmd_taken( )
{
	return bind_cmd_taken( ) || g_edge_skip.active( );
}

void ps_ride_diag( const c_user_cmd* cmd )
{
	static struct {
		int ticks = 0, buttons = 0, prev_buttons = 0;
		float fwd = 0.f, side = 0.f, yaw = 0.f;
		c_vector origin{ }, vel{ };
		bool duck_latch = false, stand_latch = false, as = false, tb = false, strafe = false;
	} ride;

	if ( !cmd || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) ) {
		ride.ticks = 0;
		return;
	}

	static bool stomped = false;
	const bool latch_duck = g_movement.m_pixelsurf_data.should_pixel_surf;
	const bool stomp      = ps_latched( ) && ( ( cmd->m_buttons & in_duck ) != 0 ) != latch_duck;
	if ( stomp && !stomped )
		botox_dbg_log( "PSU: stomp latch=%s sent=%s eb=%d/%d vz=%.2f", latch_duck ? "duck" : "stand", latch_duck ? "stand" : "duck",
		               g_edgebug.m_found ? 1 : 0, g_edgebug.m_ducked ? 1 : 0, g_prediction.backup_data.m_velocity.m_z );
	stomped = stomp;

	static bool tung_stomped = false;
	const bool tung_stomp    = s_tung_stance >= 0 && ( cmd->m_buttons & in_duck ) != s_tung_stance;
	if ( tung_stomp && !tung_stomped )
		botox_dbg_log( "TUNG: stomp stance=%s sent=%s eb=%d/%d", s_tung_stance ? "duck" : "stand", s_tung_stance ? "stand" : "duck",
		               g_edgebug.m_found ? 1 : 0, g_edgebug.m_ducked ? 1 : 0 );
	tung_stomped = tung_stomp;

	const auto& start = s_cmd_start;
	if ( starts_on_surf_pin( ) ) {
		ride.ticks++;
		ride.prev_buttons = ride.ticks > 1 ? ride.buttons : cmd->m_buttons;
		ride.buttons      = cmd->m_buttons;
		ride.fwd          = cmd->m_forward_move;
		ride.side         = cmd->m_side_move;
		ride.yaw          = cmd->m_view_point.m_y;
		ride.origin       = start.origin;
		ride.vel          = start.vel;
		ride.duck_latch   = g_movement.m_pixelsurf_data.should_pixel_surf;
		ride.stand_latch  = g_movement.m_pixelsurf_data.should_unduck;
		ride.as           = g_air_stuck_owns_cmd;
		ride.tb           = g_texturebug.m_owns_cmd || g_texturebug.m_hit;
		ride.strafe = GET_VARIABLE( g_variables.m_auto_strafe, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_auto_strafe_key, key_bind_t ) );
		return;
	}
	if ( ride.ticks < 2 || !GET_VARIABLE( g_variables.m_debug_log, bool ) ) {
		ride.ticks = 0;
		return;
	}

	const float yaw_r = deg2rad( ride.yaw );
	c_vector dir( std::cosf( yaw_r ), std::sinf( yaw_r ), 0.f );
	if ( const float v = ride.vel.length_2d( ); v > 1.f )
		dir = c_vector( ride.vel.m_x / v, ride.vel.m_y / v, 0.f );
	c_vector wall_n{ };
	const char* wall = "none";
	if ( const auto col = g_ctx.m_local->get_collideable( ) ) {
		const c_vector mins( col->get_obb_mins( ).m_x, col->get_obb_mins( ).m_y, 1.f ), maxs( col->get_obb_maxs( ).m_x, col->get_obb_maxs( ).m_y, 36.f );
		c_trace_filter fil( g_ctx.m_local );
		const float sides[ ] = { 1.f, -1.f };
		for ( const float s : sides ) {
			const c_vector to( ride.origin.m_x - dir.m_y * 2.f * s, ride.origin.m_y + dir.m_x * 2.f * s, ride.origin.m_z );
			trace_t tr;
			ray_t ray( ride.origin, to, mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
			if ( tr.did_hit( ) && !tr.m_start_solid && std::fabsf( tr.m_plane.m_normal.m_z ) < 0.7f ) {
				wall_n = tr.m_plane.m_normal;
				wall   = s > 0.f ? "left" : "right";
				break;
			}
		}
	}

	const c_vector wish( ride.fwd * std::cosf( yaw_r ) + ride.side * std::sinf( yaw_r ), ride.fwd * std::sinf( yaw_r ) - ride.side * std::cosf( yaw_r ), 0.f );
	const float wish_len = wish.length_2d( );
	const float out      = wish_len > 0.01f && wall_n.length_2d( ) > 0.f ? ( wish.m_x * wall_n.m_x + wish.m_y * wall_n.m_y ) / wish_len : 0.f;

	botox_dbg_log( "PSU: end ride=%d latch=%d/%d duck=%d%d out=%.2f fwd=%.0f side=%.0f as=%d tb=%d strafe=%d wall=%s | next vz=%.2f gnd=%d "
	               "spd=%.1f->%.1f dz=%.3f",
	               ride.ticks, ride.duck_latch ? 1 : 0, ride.stand_latch ? 1 : 0, ( ride.prev_buttons & in_duck ) ? 1 : 0,
	               ( ride.buttons & in_duck ) ? 1 : 0, out, ride.fwd, ride.side, ride.as ? 1 : 0, ride.tb ? 1 : 0, ride.strafe ? 1 : 0, wall,
	               start.vel.m_z, start.ground ? 1 : 0, ride.vel.length_2d( ), start.vel.length_2d( ), start.origin.m_z - ride.origin.m_z );
	ride.ticks = 0;
}

bool botox_is_ladder_trace( const trace_t& tr )
{
	if ( tr.m_contents & e_contents::contents_ladder )
		return true;

	if ( g_interfaces.m_physics_surface_props ) {
		if ( auto* data = g_interfaces.m_physics_surface_props->get_surface_data( tr.surface.m_surface_props ) ) {
			if ( data->m_game.m_climbable != 0 )
				return true;
		}
	}

	if ( !g_interfaces.m_engine_trace )
		return false;

	const c_vector& n     = tr.m_plane.m_normal;
	const float depths[ ] = { 2.f, 8.f };
	for ( const float out : depths ) {
		const c_vector probe( tr.m_end.m_x + n.m_x * out, tr.m_end.m_y + n.m_y * out, tr.m_end.m_z + n.m_z * out );
		if ( g_interfaces.m_engine_trace->get_point_contents( probe, e_contents::contents_ladder ) & e_contents::contents_ladder )
			return true;
	}
	return false;
}

void play_trick_sound( const int sound_index, const float volume, const char* custom_path )
{
	if ( sound_index <= 0 )
		return;

	const char* path = nullptr;
	switch ( sound_index ) {
	case 1: path = "buttons\\arena_switch_press_02.wav"; break;
	case 2: path = "buttons\\button22.wav";              break;
	case 3: path = "survival\\money_collect_01.wav";     break;
	case 4: path = "Ui\\beep07.wav";                     break;
	case 5: path = ( custom_path && custom_path[ 0 ] ) ? custom_path : nullptr; break;
	}

	if ( !path )
		return;

	/* custom found in C:\botox\sounds wins over csgo/sound. engine always opens "sound\" + name from game dir,
	   so climb out: extra ".." clamp at drive root ( csgo must be on C: ) */
	std::string botox_path;
	if ( sound_index == 5 ) {
		std::error_code ec;
		if ( std::filesystem::is_regular_file( std::filesystem::path( "C:\\botox\\sounds" ) / path, ec ) ) {
			botox_path = "..\\..\\..\\..\\..\\..\\..\\..\\..\\..\\..\\..\\..\\..\\..\\..\\botox\\sounds\\";
			botox_path += path;
			path = botox_path.c_str( );
		}
	}

	if ( g_interfaces.m_engine_sound )
		for ( float left = volume; left > 0.f; left -= 1.f )
			g_interfaces.m_engine_sound->emit_ambient_sound( path, std::min( left, 1.f ) );
	else if ( g_interfaces.m_surface )
		g_interfaces.m_surface->play_sound( path );
}

static void print_detection( const e_detection_types type, const char* token )
{
	if ( !GET_VARIABLE( g_variables.m_detections, bool ) )
		return;

	const auto& types = GET_VARIABLE( g_variables.m_detection_types, std::vector< bool > );
	if ( types.size( ) <= static_cast< std::size_t >( type ) || !types[ type ] )
		return;

	if ( n_interfaces::chat_element )
		n_interfaces::chat_element->chatprintf( 0, 0, token );
}

static bool wall_held_landing( const c_vector& origin )
{
	const auto col = g_ctx.m_local ? g_ctx.m_local->get_collideable( ) : nullptr;
	if ( !col || !g_interfaces.m_engine_trace )
		return false;
	const c_vector mins = col->get_obb_mins( ), maxs = col->get_obb_maxs( );
	c_trace_filter fil( g_ctx.m_local );
	for ( int k = 0; k < 4; k++ ) {
		const float a = deg2rad( static_cast< float >( k * 90 ) );
		trace_t tr;
		ray_t ray( origin, origin + c_vector( std::cosf( a ), std::sinf( a ), 0.f ) * 0.05f, mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
		if ( !tr.did_hit( ) || tr.m_start_solid || std::fabsf( tr.m_plane.m_normal.m_z ) >= 0.7f )
			continue;
		const c_vector off = origin + c_vector( tr.m_plane.m_normal.m_x, tr.m_plane.m_normal.m_y, 0.f );
		trace_t down;
		ray_t dray( off, off - c_vector( 0.f, 0.f, 2.f ), mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( dray, mask_playersolid, &fil, &down );
		if ( !down.m_start_solid && ( !down.did_hit( ) || down.m_plane.m_normal.m_z < 0.7f ) )
			return true;
	}
	return false;
}

/* detections fire when a trick was PERFORMED: off cmd STARTS ( what the engine ran on the sent cmd ), never a feature's sim / plan */
void n_movement::impl_t::run_detections( )
{
	const trick_states_t states = get_trick_states( );

	static int pending_edge_bug_tick = 0;
	const int current_tick           = g_interfaces.m_global_vars_base->m_tick_count;

	if ( !( GET_VARIABLE( g_variables.edge_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) ) ) )
		pending_edge_bug_tick = 0;

	if ( !states.m_edge_bug_queued )
		pending_edge_bug_tick = 0;

	if ( states.m_edge_bug_queued && states.m_edge_bug_tick >= current_tick )
		pending_edge_bug_tick = states.m_edge_bug_tick;

	static int edge_bug_chain = 0, ground_ticks = 0;
	bool jumped = false;
	{
		const bool grounded = ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) != 0;
		jumped              = ( g_prediction.backup_data.m_flags & e_flags::fl_onground ) && !grounded && g_ctx.m_cmd &&
		                      ( g_ctx.m_cmd->m_buttons & e_command_buttons::in_jump );
		ground_ticks        = grounded ? ground_ticks + 1 : 0;
		if ( edge_bug_chain && ( ground_ticks >= 2 || jumped ) ) {
			botox_dbg_log( "EBC: reset chain=%d ground_ticks=%d jumped=%d", edge_bug_chain, ground_ticks, jumped ? 1 : 0 );
			edge_bug_chain = 0;
		}
	}

	const auto& start  = g_prediction.backup_data;
	const float g      = g_convars.float_or( HASH_BT( "sv_gravity" ), 800.f ) * 0.5f * n_tick::interval( );
	const bool air     = !( start.m_flags & e_flags::fl_onground ) && start.m_move_type == move_type_walk;
	const float vz     = start.m_velocity.m_z;
	const float speed  = start.m_velocity.length_2d( );
	static bool prev_air = false;
	static float prev_vz = 0.f, prev_speed = 0.f;
	static int prev_tick = 0;
	const bool prev_ok = prev_air && prev_tick == current_tick - 1;

	/* physical bug: lip reflect zeroes vz, FinishGravity leaves -g, xy kept. START before it airborne + falling ( a grounded start = landing,
	   never an eb; rising = seam / ceiling clip ). candidate = bug in cmd current-1, confirmed next start if airborne and falling again */
	int phys_bug_tick = 0;
	bool phys_cand    = false;
	{
		static float cand_vz = 0.f;
		static int cand_tick = 0;

		const bool tb = states.m_texture_bug_now || m_pixelsurf_data.m_start_pin_kind == 2;
		const bool ps = m_pixelsurf_data.m_ps_hold;
		m_pixelsurf_data.m_ps_hold = false;

		const bool landed_walk = ( start.m_flags & e_flags::fl_onground ) && start.m_move_type == move_type_walk;
		if ( cand_tick && current_tick == cand_tick + 1 ) {
			if ( !tb && ( landed_walk || ( air && vz < cand_vz - g * 1.5f ) ) )
				phys_bug_tick = cand_tick - 1;
			else
				botox_dbg_log( "EBC: cand lost air=%d gnd=%d tb=%d vz=%.2f cand_vz=%.2f tick=%d", air ? 1 : 0, landed_walk ? 1 : 0, tb ? 1 : 0, vz,
				               cand_vz, current_tick );
		}
		cand_tick = 0;

		if ( air && prev_ok && prev_vz < -15.f && vz < 1.f - g && vz > -g - std::max( 1.f, prev_vz * -0.25f ) && speed > 1.f &&
		     speed >= prev_speed * 0.5f ) {
			if ( tb || ps )
				botox_dbg_log( "EBC: %s veto hit=%d kind=%d tick=%d", tb ? "tb" : "ps", states.m_texture_bug_now ? 1 : 0,
				               m_pixelsurf_data.m_start_pin_kind, current_tick );
			else {
				cand_tick = current_tick;
				cand_vz   = vz;
				phys_cand = true;
			}
		}
	}

	static int last_bug_tick = 0;
	const auto fire_edge_bug = [ & ]( const int bug_tick, const char* src ) {
		const int gap = bug_tick - last_bug_tick;
		if ( last_bug_tick && std::abs( gap ) <= n_tick::ticks( 2 ) ) {
			botox_dbg_log( "EBC: dup dropped src=%s gap=%d tick=%d", src, gap, current_tick );
			return;
		}
		on_healthshot( 1 );
		g_edge_bug_chain = ++edge_bug_chain;
		botox_dbg_log( "EBC: eb src=%s chain=%d ground_ticks=%d tick=%d bug=%d", src, edge_bug_chain, ground_ticks, current_tick, bug_tick );
		points_on_trick( e_points_trick::edge_bug );
		print_detection( e_detection_types::detect_eb, CHAT_TOKEN_EDGEBUG );
		if ( g_es_sound_tick && std::abs( current_tick - g_es_sound_tick ) <= n_tick::ticks( 2 ) )
			botox_dbg_log( "EBC: sound skipped, es rang tick=%d es=%d", current_tick, g_es_sound_tick );
		else if ( GET_VARIABLE( g_variables.m_edge_bug_sound, int ) > 0 ) {
			g_eb_sound_tick = current_tick;
			play_trick_sound( GET_VARIABLE( g_variables.m_edge_bug_sound, int ),
			                  GET_VARIABLE( g_variables.m_edge_bug_sound_volume, float ),
			                  GET_VARIABLE( g_variables.m_edge_bug_sound_custom, std::string ).c_str( ) );
		}
		last_bug_tick = bug_tick;
	};
	if ( pending_edge_bug_tick && current_tick >= pending_edge_bug_tick ) {
		if ( phys_cand )
			fire_edge_bug( pending_edge_bug_tick - 1, "plan" );
		else
			botox_dbg_log( "EBC: plan no bug due=%d tick=%d vz=%.2f air=%d prev=%d/%.2f", pending_edge_bug_tick, current_tick, vz, air ? 1 : 0,
			               prev_ok ? 1 : 0, prev_vz );
		pending_edge_bug_tick = 0;
	}
	if ( phys_bug_tick )
		fire_edge_bug( phys_bug_tick, "phys" );

	const auto fresh = [ ]( const bool now, int& off ) {
		const bool f = now && off >= n_tick::ticks( 8 );
		off          = now ? 0 : off < INT_MAX ? off + 1 : off;
		return f;
	};
	const auto detect = [ & ]( const bool now, int& off, const e_detection_types type, const char* token, const e_points_trick trick,
	                           const e_points_trick ride ) {
		if ( fresh( now, off ) ) {
			points_on_trick( trick );
			print_detection( type, token );
			return true;
		}
		if ( now )
			points_on_trick( ride );
		return false;
	};
	const bool pinned = air && is_pixelsurf_velocity( vz ), prev_pinned = prev_ok && is_pixelsurf_velocity( prev_vz );

	const bool tb_phys = states.m_texture_bug_surf && prev_ok && prev_vz < 0.f;
	static int tb_off = INT_MAX;
	if ( detect( tb_phys, tb_off, e_detection_types::detect_tb, CHAT_TOKEN_TEXTUREBUG, e_points_trick::texture_bug, e_points_trick::texture_bug_ride ) )
		botox_dbg_log( "TBC: tb kind=%d hit=%d prev_vz=%.2f xy=%.1f tick=%d", m_pixelsurf_data.m_start_pin_kind, g_texturebug.m_hit ? 1 : 0, prev_vz,
		               speed, current_tick );

	const bool as_phys = g_air_stuck_noclip_active ? start.m_move_type == move_type_noclip
	                                               : pinned && start.m_velocity.m_x == 0.f && start.m_velocity.m_y == 0.f;
	static int as_off = INT_MAX;
	if ( detect( as_phys, as_off, e_detection_types::detect_as, CHAT_TOKEN_AIRSTUCK, e_points_trick::air_stuck, e_points_trick::air_stuck_ride ) )
		botox_dbg_log( "TBC: as noclip=%d vz=%.3f tick=%d", g_air_stuck_noclip_active ? 1 : 0, vz, current_tick );

	const bool ps_phys = pinned && prev_pinned && speed > 0.f && !as_phys && !states.m_texture_bug_surf && m_pixelsurf_data.m_start_pin_kind != 2;
	static int ps_off = INT_MAX;
	if ( detect( ps_phys, ps_off, e_detection_types::detect_ps, CHAT_TOKEN_PIXELSURF, e_points_trick::pixel_surf, e_points_trick::pixel_surf_ride ) ) {
		on_healthshot( 2 );
		botox_dbg_log( "TBC: ps kind=%d xy=%.1f tick=%d", m_pixelsurf_data.m_start_pin_kind, speed, current_tick );
	}

	const bool landed = prev_ok && ( start.m_flags & e_flags::fl_onground ) && start.m_move_type == move_type_walk;
	if ( landed && wall_held_landing( start.m_origin ) ) {
		points_on_trick( e_points_trick::wall_climb );
		print_detection( e_detection_types::detect_wc, CHAT_TOKEN_WALLCLIMB );
		botox_dbg_log( "TBC: wc prev_vz=%.2f z=%.3f tick=%d", prev_vz, start.m_origin.m_z, current_tick );
	}

	if ( prev_ok && prev_vz < 0.f && air && vz > 250.f )
		points_on_trick( e_points_trick::jump_bug );
	if ( prev_ok && prev_vz < -15.f && start.m_move_type == move_type_ladder )
		points_on_trick( e_points_trick::fireman );
	if ( prev_ok && prev_vz > g * 2.f + 1.f && air && fabsf( vz + g ) < 1.f )
		points_on_trick( e_points_trick::head_ceiling );

	prev_air   = air;
	prev_vz    = vz;
	prev_speed = speed;
	prev_tick  = current_tick;

	struct surf_sound_t {
		bool was_surfing = false;
		c_vector last_pos{ };
		float accum = 0.f;
		int off_ticks = INT_MAX;
		unsigned long long next_time = 0ull;
	};
	const auto surf_sound = [ ]( surf_sound_t& s, const bool surfing, const int sound, const float vol, const std::string& custom ) {
		if ( surfing && g_ctx.m_local ) {
			const c_vector pos = g_ctx.m_local->get_origin( );
			constexpr unsigned long long min_gap = 120ull;
			const unsigned long long now         = GetTickCount64( );
			const bool new_surf                  = !s.was_surfing && s.off_ticks >= n_tick::ticks( 8 );
			s.off_ticks                          = 0;
			if ( new_surf ) {
				if ( now >= s.next_time ) {
					play_trick_sound( sound, vol, custom.c_str( ) );
					s.next_time = now + min_gap;
				}
				s.accum = 0.f;
			} else {
				const c_vector d = pos - s.last_pos;
				const float travelled = sqrtf( d.m_x * d.m_x + d.m_y * d.m_y );
				if ( travelled >= 0.05f )
					s.accum += travelled;
				while ( s.accum >= 10.f ) {
					s.accum -= 10.f;
					if ( now >= s.next_time ) {
						play_trick_sound( sound, vol, custom.c_str( ) );
						s.next_time = now + min_gap;
					}
				}
			}
			s.last_pos = pos;
		} else {
			if ( s.off_ticks < INT_MAX )
				++s.off_ticks;
			if ( s.off_ticks >= n_tick::ticks( 8 ) )
				s.accum = 0.f;
		}
		s.was_surfing = surfing;
	};

	static surf_sound_t ps_sound{ }, tb_sound{ };
	surf_sound( ps_sound, ps_phys, GET_VARIABLE( g_variables.m_pixel_surf_sound, int ),
	            GET_VARIABLE( g_variables.m_pixel_surf_sound_volume, float ), GET_VARIABLE( g_variables.m_pixel_surf_sound_custom, std::string ) );
	surf_sound( tb_sound, tb_phys, GET_VARIABLE( g_variables.m_texture_bug_sound, int ),
	            GET_VARIABLE( g_variables.m_texture_bug_sound_volume, float ), GET_VARIABLE( g_variables.m_texture_bug_sound_custom, std::string ) );
}

void n_movement::impl_t::jump_stats( )
{
	struct state_t {
		bool m_active = false, m_jb = false, m_void = false, m_ducked = false, m_duck_open = false;
		int m_chain = 0, m_ground_ticks = 0, m_duck_ticks = 0, m_last_tick = 0;
		int m_strafes = 0, m_last_dir = 0, m_sync_ticks = 0, m_gain_ticks = 0;
		float m_pre = 0.f, m_max = 0.f, m_peak = 0.f;
		c_vector m_start{ };
	};
	static state_t s{ };

	if ( !GET_VARIABLE( g_variables.m_jump_stats, bool ) || !g_ctx.m_cmd ) {
		s = { };
		return;
	}

	const int tick = g_ctx.m_cmd->m_tick_count;
	if ( tick != s.m_last_tick + 1 )
		s.m_active = false;
	s.m_last_tick = tick;

	if ( g_ctx.m_local->get_move_type( ) != move_type_walk ) {
		s.m_active = false;
		return;
	}

	const auto& start      = g_prediction.backup_data;
	const bool was_ground  = ( start.m_flags & fl_onground ) != 0;
	const bool now_ground  = ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;
	const float speed_in   = start.m_velocity.length_2d( );
	const float speed_out  = g_ctx.m_local->get_velocity( ).length_2d( );
	const c_vector origin  = g_ctx.m_local->get_origin( );

	s.m_ground_ticks = was_ground ? s.m_ground_ticks + 1 : 0;
	if ( s.m_ground_ticks > 2 )
		s.m_chain = 0;

	const auto take_off = [ & ]( const bool jb ) {
		s.m_active = true;
		s.m_jb     = jb;
		s.m_void   = false;
		s.m_start  = start.m_origin;
		s.m_peak   = start.m_origin.m_z;
		s.m_pre    = speed_in;
		s.m_max    = speed_in;
		s.m_strafes = s.m_last_dir = s.m_sync_ticks = s.m_gain_ticks = 0;
		s.m_ducked     = ( g_ctx.m_cmd->m_buttons & in_duck ) != 0;
		s.m_duck_open  = s.m_ducked;
		s.m_duck_ticks = 0;
	};

	if ( was_ground && !now_ground ) {
		s.m_chain = s.m_ground_ticks == 1 ? s.m_chain + 1 : 0;
		take_off( false );
	} else if ( !was_ground && !now_ground && start.m_velocity.m_z <= 0.f && g_ctx.m_local->get_velocity( ).m_z > 0.f ) {
		take_off( true );
	}

	if ( !s.m_active )
		return;

	if ( !now_ground ) {
		s.m_max  = std::max( s.m_max, speed_out );
		s.m_peak = std::max( s.m_peak, origin.m_z );

		if ( speed_out > 50.f ) {
			++s.m_sync_ticks;
			if ( speed_out > speed_in + 0.000001f )
				++s.m_gain_ticks;
		}

		const int dir = g_ctx.m_cmd->m_mouse_delta_x > 0 ? 1 : g_ctx.m_cmd->m_mouse_delta_x < 0 ? -1 : 0;
		if ( dir && dir != s.m_last_dir ) {
			++s.m_strafes;
			s.m_last_dir = dir;
		}

		if ( s.m_duck_open && ( g_ctx.m_cmd->m_buttons & in_duck ) )
			++s.m_duck_ticks;
		else
			s.m_duck_open = false;

		const trick_states_t tricks = get_trick_states( );
		if ( tricks.m_edge_bug_queued || tricks.m_texture_bug_now || tricks.m_air_stuck_now || tricks.m_wall_climb_now ||
		     tricks.m_pixel_surf_now )
			s.m_void = true;
		return;
	}

	s.m_active = false;
	if ( s.m_void || std::fabs( origin.m_z - s.m_start.m_z ) >= ( s.m_jb ? 16.f : 2.f ) )
		return;

	const float distance = s.m_start.dist_to_2d( origin ) + 32.f;
	const float min_distance = GET_VARIABLE( g_variables.m_jump_stats_show_fails, bool ) ? 150.f : 220.f;
	if ( distance <= min_distance || distance >= 280.f )
		return;

	struct type_t {
		const char* m_name;
		float m_tiers[ 5 ];
	};
	static constexpr type_t k_jump      = { "J", { 227.f, 232.f, 237.f, 241.f, 243.f } };
	static constexpr type_t k_long      = { "LJ", { 230.f, 235.f, 240.f, 244.f, 246.f } };
	static constexpr type_t k_mini      = { "MJ", { 220.f, 225.f, 230.f, 234.f, 236.f } };
	static constexpr type_t k_bhop      = { "BH", { 230.f, 234.f, 235.f, 238.f, 240.f } };
	static constexpr type_t k_multi_bhop = { "MBH", { 235.f, 237.f, 242.f, 245.f, 247.f } };
	static constexpr type_t k_jump_bug  = { "JB", { 250.f, 255.f, 260.f, 265.f, 268.f } };
	static constexpr const char* k_colors[ 6 ] = { "#bebebe", "#4b6af9", "#41fe3f", "#fc0301", "#e0ad37", "#d22ce4" };

	const type_t& type = s.m_jb            ? k_jump_bug
	                     : s.m_chain > 1   ? k_multi_bhop
	                     : s.m_chain == 1  ? k_bhop
	                     : !s.m_ducked     ? k_jump
	                     : s.m_duck_ticks <= 1 ? k_mini
	                                           : k_long;

	int tier = 0;
	while ( tier < 5 && distance >= type.m_tiers[ tier ] )
		++tier;

	const int sync = s.m_sync_ticks ? static_cast< int >( s.m_gain_ticks * 100.f / s.m_sync_ticks ) : 0;
	const char* value = k_colors[ tier ];
	const char* grey  = k_colors[ 0 ];

	g_jump_stats_line = std::format( R"(<font color="{0}"> {2}: {3:.2f}</font><font color="{1}"> units | </font>)"
	                                 R"(<font color="{0}">{4}</font><font color="{1}"> strafes | </font>)"
	                                 R"(<font color="{0}">{5:.0f}</font><font color="{1}"> pre | </font>)"
	                                 R"(<font color="{0}">{6:.0f}</font><font color="{1}"> max | </font>)"
	                                 R"(<font color="{0}">{7:.2f}</font><font color="{1}"> height | </font>)"
	                                 R"(<font color="{0}">{8}%</font><font color="{1}"> sync</font>)",
	                                 value, grey, type.m_name, distance, s.m_strafes, s.m_pre, s.m_max, s.m_peak - s.m_start.m_z, sync );

	if ( n_interfaces::chat_element )
		n_interfaces::chat_element->chatprintf( 0, 0, CHAT_TOKEN_JUMPSTATS );
}
