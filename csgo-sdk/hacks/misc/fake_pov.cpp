#include "misc.h"
#include "../../dependencies/imgui/imgui.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include <algorithm>
#include <cmath>

extern void botox_dbg_log( const char* fmt, ... );

namespace
{
	enum e_fake_pov_mode { mode_right, mode_left, mode_up, mode_bottom, mode_backwards, mode_spinning };

	/* main thread writes (frame + create_move), end_scene only reads the arrow pair */
	float yaw = 0.f, pitch = 0.f;
	float spin = 0.f;
	float arrow_ang = 0.f;
	float arrow_len = 0.f;
	bool active     = false;
	bool wanted     = false;
	bool prev_enabled = false;

	bool bind_on( )
	{
		auto& key = GET_VARIABLE( g_variables.m_fake_pov_key, key_bind_t );
		if ( key.m_key_style == 0 || key.m_key == 0 )
			return true;

		return g_input.check_input( &key );
	}

	float normalize_deg( float a )
	{
		a = std::fmodf( a + 180.f, 360.f );
		if ( a < 0.f )
			a += 360.f;
		return a - 180.f;
	}

	float approach_angle( const float cur, const float target, const float k )
	{
		return cur + normalize_deg( target - cur ) * k;
	}

	bool recording( )
	{
		return g_interfaces.m_engine_client->is_recording_demo( ) && !g_interfaces.m_engine_client->is_playing_demo( );
	}
}

void fake_pov_frame( )
{
	const float dt = std::min( g_interfaces.m_global_vars_base->m_abs_frame_time, 0.1f );

	wanted             = GET_VARIABLE( g_variables.m_fake_pov, bool ) && bind_on( );
	const bool enabled = wanted && !g_interfaces.m_engine_client->is_playing_demo( );
	const int mode    = GET_VARIABLE( g_variables.m_fake_pov_mode, int );

	float target_pitch = 0.f, target_yaw = 0.f, target_arrow = 0.f, target_len = 0.f;
	if ( enabled ) {
		target_len = 1.f;
		switch ( mode ) {
		case mode_right: target_yaw = -90.f; target_arrow = 90.f; break;
		case mode_left: target_yaw = 90.f; target_arrow = 270.f; break;
		case mode_up: target_pitch = -89.f; break;
		case mode_bottom: target_pitch = 89.f; target_arrow = 180.f; break;
		case mode_backwards: target_yaw = 180.f; target_arrow = 180.f; break;
		case mode_spinning:
			spin         = normalize_deg( spin + GET_VARIABLE( g_variables.m_fake_pov_spin_speed, float ) * dt );
			target_yaw   = spin;
			target_arrow = -spin;
			break;
		}
	}

	if ( !enabled || mode != mode_spinning )
		spin = yaw;

	const float k = 1.f - std::exp( -std::max( 0.1f, GET_VARIABLE( g_variables.m_fake_pov_smooth, float ) ) * dt );
	yaw           = approach_angle( yaw, target_yaw, k );
	pitch         = pitch + ( target_pitch - pitch ) * k;
	arrow_ang     = approach_angle( arrow_ang, target_arrow, k );
	arrow_len     = arrow_len + ( target_len - arrow_len ) * k;

	active = enabled || std::fabs( yaw ) > 0.05f || std::fabs( pitch ) > 0.05f;
}

void fake_pov_create_move( )
{
	const bool enabled = wanted;

	if ( prev_enabled && !enabled )
		botox_dbg_log( "FPOV: off edge snap=%d active=%d rec=%d off=%.1f/%.1f", GET_VARIABLE( g_variables.m_fake_pov_snap_view, bool ) ? 1 : 0,
		               active ? 1 : 0, recording( ) ? 1 : 0, pitch, yaw );

	if ( GET_VARIABLE( g_variables.m_fake_pov_snap_view, bool ) && prev_enabled && !enabled && active ) {
		c_angle view = { };
		g_interfaces.m_engine_client->get_view_angles( view );
		view.m_x = std::clamp( view.m_x + pitch, -89.f, 89.f );
		view.m_y = normalize_deg( view.m_y + yaw );
		view.m_z = 0.f;
		g_interfaces.m_engine_client->set_view_angles( view );

		yaw = pitch = spin = arrow_ang = arrow_len = 0.f;
		active = false;
	}

	prev_enabled = enabled;
}

/* CPrediction::GetLocalViewAngles = demo POV. original = pl.v_angle = last cmd view ( forced angles leak ), engine view = real mouse */
void fake_pov_demo_angles( c_angle& angles )
{
	if ( !recording( ) )
		return;

	const auto local = g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) );
	if ( !local || !local->is_alive( ) )
		return;

	c_angle view = { };
	g_interfaces.m_engine_client->get_view_angles( view );
	angles.m_x = std::clamp( view.m_x + ( active ? pitch : 0.f ), -89.f, 89.f );
	angles.m_y = normalize_deg( view.m_y + ( active ? yaw : 0.f ) );
	angles.m_z = view.m_z;
}

void fake_pov_draw( )
{
	if ( !GET_VARIABLE( g_variables.m_fake_pov_arrow, bool ) || g_interfaces.m_engine_client->is_playing_demo( ) )
		return;

	const float len = std::clamp( arrow_len, 0.f, 1.f );
	if ( len <= 0.01f )
		return;

	const ImU32 color = GET_VARIABLE( g_variables.m_fake_pov_arrow_color, c_color ).get_u32( len );

	const float cx   = g_ctx.m_width * 0.5f;
	const float cy   = g_ctx.m_height * 0.5f;
	const float size = std::max( 2.f, GET_VARIABLE( g_variables.m_fake_pov_arrow_size, float ) );
	const float dist = std::max( 0.f, GET_VARIABLE( g_variables.m_fake_pov_arrow_dist, float ) );

	const float rad = arrow_ang * 3.1415926535f / 180.f;
	const float dx = std::sin( rad ), dy = -std::cos( rad );
	const float px = -dy, py = dx;

	const float tip_r = dist + size * 1.6f;
	const float half  = size * 0.75f;

	ImDrawList* draw_list = ImGui::GetForegroundDrawList( );
	const auto block      = g_render.begin_stretch_block( draw_list, false );

	draw_list->AddTriangleFilled( ImVec2( cx + dx * tip_r, cy + dy * tip_r ),
	                              ImVec2( cx + dx * dist + px * half, cy + dy * dist + py * half ),
	                              ImVec2( cx + dx * dist - px * half, cy + dy * dist - py * half ), color );

	g_render.end_stretch_block( block );
}
