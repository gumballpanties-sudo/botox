#pragma once

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "tick_scale.h"
#include <algorithm>
#include <cmath>

extern void CorrectMovement( c_user_cmd* cmd, c_angle wish_angle, c_angle old_angles );
extern bool botox_is_ladder_trace( const trace_t& tr );

namespace n_tb
{
	inline int s_sims_this_cmd            = 0;
	inline int s_sim_reserve              = 0;
	constexpr int k_cmd_sim_cap    = 192;
	constexpr int k_as_sim_reserve = 16;

	// share of one server tick the whole tb family may spend: 0.40 = 6.2ms @64 / 3.1ms @128
	// started per cmd in on_create_move, engine interval, never the 128 fix
	constexpr float k_cmd_time_share = 0.40f;
	inline n_tick::c_sim_budget s_budget{ };
	inline bool s_budget_live = false;
	inline long long s_budget_us_last = 0ll;

	inline int sims_left( )
	{
		if ( s_budget_live && s_budget.expired( ) )
			return 0;
		return k_cmd_sim_cap - s_sim_reserve - s_sims_this_cmd;
	}

	inline float s_target_predict_z_vel = -6.f;

	inline int s_traces_this_cmd = 0;

	inline long long s_trace_qpc_this_cmd = 0ll;

	inline long long tb_qpc_freq( )
	{
		static const long long freq = [ ] {
			LARGE_INTEGER f{ };
			QueryPerformanceFrequency( &f );
			return f.QuadPart > 0ll ? f.QuadPart : 1ll;
		}( );
		return freq;
	}

	inline long long tb_trace_us( ) { return s_trace_qpc_this_cmd * 1000000ll / tb_qpc_freq( ); }

	inline bool s_trace_timing = false;

	inline void tb_trace( const ray_t& ray, unsigned int mask, i_trace_filter* filter, trace_t* out )
	{
		++s_traces_this_cmd;

		if ( !s_trace_timing ) {
			g_interfaces.m_engine_trace->trace_ray( ray, mask, filter, out );
			return;
		}

		LARGE_INTEGER a{ }, b{ };
		QueryPerformanceCounter( &a );
		g_interfaces.m_engine_trace->trace_ray( ray, mask, filter, out );
		QueryPerformanceCounter( &b );
		s_trace_qpc_this_cmd += b.QuadPart - a.QuadPart;
	}

	// skip redundant restores (full engine state copy): only a sim or fresh cmd dirties the frame,
	// analytic probes don't. every tb-family restore targets m_commands_predicted - 1, so one flag
	inline bool s_pred_dirty       = true;
	inline int s_restores_this_cmd = 0;
	inline int s_restores_skipped  = 0;

	inline int s_restore_frame = -1000;

	inline float s_bounds_next = 0.f;

	inline void tb_restore( const int frame )
	{
		s_bounds_next = g_prediction.m_real_max_speed;
		if ( !s_pred_dirty && frame == s_restore_frame ) {
			++s_restores_skipped;
			return;
		}
		++s_restores_this_cmd;
		s_pred_dirty    = false;
		s_restore_frame = frame;
		g_prediction.restore_entity_to_predicted_frame( frame );
	}

	inline void pred_simulate( c_user_cmd* cmd )
	{
		++s_sims_this_cmd;
		s_pred_dirty                   = true;
		g_prediction.m_bounds_max_speed = s_bounds_next;
		g_prediction.begin( g_ctx.m_local, cmd );
		g_prediction.end( g_ctx.m_local );
		s_bounds_next                   = g_prediction.m_last_max_speed;
		g_prediction.m_bounds_max_speed = 0.f;
	}

	// move_fix_to_yaw: fwd/side meant at world yaw, rotated onto the cmd's view yaw. never strips buttons, callers do
	inline void move_fix_to_yaw( const float yaw, c_user_cmd& c )
	{
		const float y = yaw >= 0.f ? yaw : yaw + 360.f;
		const float v = c.m_view_point.m_y >= 0.f ? c.m_view_point.m_y : c.m_view_point.m_y + 360.f;
		float d       = y <= v ? 360.f - std::fabsf( y - v ) : std::fabsf( v - y );
		d             = 360.f - d;
		const float f = c.m_forward_move, s = c.m_side_move;
		c.m_forward_move = std::cosf( deg2rad( d ) ) * f + std::cosf( deg2rad( d + 90.f ) ) * s;
		c.m_side_move    = std::sinf( deg2rad( d ) ) * f + std::sinf( deg2rad( d + 90.f ) ) * s;
	}

	inline c_vector air_accel_delta( const c_vector& base_vel, float world_yaw, float forward )
	{
		float wishspeed = forward;
		float yaw       = world_yaw;
		if ( wishspeed < 0.f ) {
			wishspeed = -wishspeed;
			yaw += 180.f;
		}
		if ( wishspeed <= 0.f )
			return c_vector( 0.f, 0.f, 0.f );

		const float rad = deg2rad( yaw );
		const c_vector wishdir( std::cos( rad ), std::sin( rad ), 0.f );

		const float wishspd     = std::fmin( wishspeed, 30.f );
		const float currentspeed = base_vel.m_x * wishdir.m_x + base_vel.m_y * wishdir.m_y;
		const float addspeed     = wishspd - currentspeed;
		if ( addspeed <= 0.f )
			return c_vector( 0.f, 0.f, 0.f );

		static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
		const float accel   = sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f;
		float accelspeed = accel * wishspeed * g_interfaces.m_global_vars_base->m_interval_per_tick;
		if ( accelspeed > addspeed )
			accelspeed = addspeed;
		return wishdir * accelspeed;
	}

	constexpr float k_dup_vel_eps = 0.01f;

	struct tb_dedup_t {
		static constexpr int k_max = 288;
		c_vector m_seen[ k_max ];
		int m_count = 0;

		bool seen_or_add( const c_vector& delta )
		{
			for ( int i = 0; i < m_count; ++i )
				if ( std::fabs( delta.m_x - m_seen[ i ].m_x ) < k_dup_vel_eps &&
				     std::fabs( delta.m_y - m_seen[ i ].m_y ) < k_dup_vel_eps )
					return true;
			if ( m_count < k_max )
				m_seen[ m_count++ ] = delta;
			return false;
		}
	};

	constexpr int k_fsolid_not_solid = 0x4;
	constexpr int k_fsolid_trigger   = 0x8;

	class c_trace_filter_tb_world_props : public i_trace_filter
	{
	public:
		explicit c_trace_filter_tb_world_props( c_base_entity* skip ) : m_skip( skip ) { }

		bool should_hit_entity( c_base_entity* entity, int  ) override
		{
			if ( !entity || entity == m_skip )
				return false;
			const unsigned int handle = entity->get_ref_ehandle( );
			if ( ( handle >> 16 ) == ( 1u << 15 ) )
				return true;
			const int index = static_cast< int >( handle & 0xFFFF );
			if ( index >= 1 && index <= 64 && g_interfaces.m_client_entity_list->get< c_base_entity >( index ) == entity )
				return false;
			if ( auto* collideable = entity->get_collideable( ) )
				if ( collideable->get_solid_flags( ) & ( k_fsolid_trigger | k_fsolid_not_solid ) )
					return false;
			return true;
		}

		e_trace_type get_trace_type( ) const override
		{
			return e_trace_type::trace_type_everything_filter_props;
		}

	private:
		c_base_entity* m_skip = nullptr;
	};

	constexpr int k_dispsurf_flag_surface = 0x1;

	inline bool tb_steering_away_from_wall( const c_user_cmd* cmd, const c_vector& wall_normal, float forward_move, float side_move,
	                                        float min_dot = 0.3f )
	{
		if ( forward_move == 0.f && side_move == 0.f )
			return false;

		c_vector forward, right;
		g_math.angle_vectors( c_angle( 0.f, cmd->m_view_point.m_y, 0.f ), &forward, &right, nullptr );

		c_vector wish = forward * forward_move + right * side_move;
		wish.m_z      = 0.f;
		const float wish_len = wish.length_2d( );
		if ( wish_len < 1.f )
			return false;
		wish *= ( 1.f / wish_len );

		c_vector normal = wall_normal;
		normal.m_z      = 0.f;
		const float norm_len = normal.length_2d( );
		if ( norm_len < 0.001f )
			return false;
		normal *= ( 1.f / norm_len );

		return ( wish.m_x * normal.m_x + wish.m_y * normal.m_y ) > min_dot;
	}

	inline bool tb_move_latches_ladder( const c_user_cmd* cmd, const bool check_cliff )
	{
		if ( !cmd || !g_ctx.m_local || ( cmd->m_forward_move == 0.f && cmd->m_side_move == 0.f ) )
			return false;
		auto* col = g_ctx.m_local->get_collideable( );
		if ( !col )
			return false;
		const c_vector mins                = col->get_obb_mins( );
		const c_vector maxs                = col->get_obb_maxs( );
		const c_vector org                 = g_prediction.backup_data.m_origin;
		constexpr unsigned int ladder_mask = mask_playersolid & ~e_contents::contents_playerclip;
		c_trace_filter fil( g_ctx.m_local );

		c_vector fwd, right;
		g_math.angle_vectors( cmd->m_view_point, &fwd, &right, nullptr );
		c_vector wish   = fwd * cmd->m_forward_move + right * cmd->m_side_move;
		const float len = wish.length( );
		if ( len > 1e-6f ) {
			wish *= 1.f / len;
			trace_t t{ };
			ray_t r( org, org + wish * 2.f, mins, maxs );
			tb_trace( r, ladder_mask, &fil, &t );
			if ( ( t.m_fraction < 1.f || t.m_start_solid ) && t.m_plane.m_normal.m_z != 1.f && botox_is_ladder_trace( t ) )
				return true;
		}
		if ( !check_cliff )
			return false;

		const c_vector v = g_prediction.backup_data.m_velocity;
		if ( ( g_prediction.backup_data.m_flags & fl_onground ) || v.m_z > 0.f || v.m_z <= -50.f || v.m_x == 0.f || v.m_y == 0.f )
			return false;
		const float speed = v.length( );
		if ( speed < 0.01f )
			return false;
		const c_vector from( org.m_x, org.m_y, org.m_z - 6.f );
		const c_vector to = org - v * ( 24.f / speed );
		trace_t t{ };
		ray_t r( from, to, mins, maxs );
		tb_trace( r, ladder_mask, &fil, &t );
		return t.m_fraction < 1.f && !t.m_start_solid && t.m_plane.m_normal.m_z != 1.f && botox_is_ladder_trace( t );
	}

	inline bool tb_detect_overhead( const c_vector& pos, const c_vector& mins, const c_vector& maxs, float reach, trace_t& trace_out )
	{
		c_vector end = pos;
		end.m_z += reach;
		c_trace_filter_tb_world_props filter( g_ctx.m_local );
		ray_t ray( pos, end, mins, maxs );
		tb_trace( ray, mask_playersolid, &filter, &trace_out );
		return trace_out.m_fraction < 1.f && trace_out.m_plane.m_normal.m_z < -0.1f;
	}

}
