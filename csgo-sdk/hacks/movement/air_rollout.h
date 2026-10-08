#pragma once
#include "movement.h"
#include "tick_scale.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include <algorithm>
#include <cmath>

namespace n_air
{
	struct world_t {
		float dt        = 0.015625f; /* engine ipt, never the 128 checkbox ( sims step the engine's ) */
		float g         = 800.f;
		float accel     = 12.f;
		float max_speed = 250.f;
		float stamina      = 0.f;
		float stamina_rate = 60.f;
		float duck         = 1.f;
		int wall_slides    = 0; /* fly: vertical walls clip xy and the fall goes on, this many times ( 0 = a wall ends it ) */
		float snap         = 0.f; /* fly: CategorizePosition 2u ground trace per tick end ( vz = 0, no clip ). 0 = off */
		c_vector mins{ }, maxs{ };
	};

	inline world_t read_world( const c_vector& mins = { }, const c_vector& maxs = { } )
	{
		world_t w;
		w.mins = mins;
		w.maxs = maxs;
		if ( g_interfaces.m_global_vars_base && g_interfaces.m_global_vars_base->m_interval_per_tick > 0.f )
			w.dt = g_interfaces.m_global_vars_base->m_interval_per_tick;
		if ( g_convars[ HASH_BT( "sv_gravity" ) ] )
			w.g = std::max( g_convars[ HASH_BT( "sv_gravity" ) ]->get_float( ), 1.f );
		if ( g_convars[ HASH_BT( "sv_airaccelerate" ) ] )
			w.accel = g_convars[ HASH_BT( "sv_airaccelerate" ) ]->get_float( );
		if ( g_convars[ HASH_BT( "sv_staminarecoveryrate" ) ] )
			w.stamina_rate = std::max( g_convars[ HASH_BT( "sv_staminarecoveryrate" ) ]->get_float( ), 0.f );
		if ( g_ctx.m_local && g_ctx.m_local->get_max_speed( ) > 1.f )
			w.max_speed = std::min( g_ctx.m_local->get_max_speed( ), 450.f );

		const float stam = g_prediction.backup_data.m_stamina;
		const float amt  = g_prediction.backup_data.m_duck_amount;
		w.stamina        = stam > 0.f && stam <= 100.f ? stam : 0.f;
		w.duck           = amt > 0.f && amt <= 1.f ? 0.34f * amt + 1.f - amt : 1.f;
		return w;
	}

	inline world_t advance( world_t w, const int ticks )
	{
		w.stamina = std::max( w.stamina - w.stamina_rate * w.dt * static_cast< float >( ticks ), 0.f );
		return w;
	}

	inline float max_speed_at( const world_t& w, const int k = 0 )
	{
		const float stam = std::max( w.stamina - w.stamina_rate * w.dt * static_cast< float >( k ), 0.f );
		const float s    = std::clamp( 1.f - stam / 100.f, 0.f, 1.f );
		return w.max_speed * s * s;
	}

	inline float accel_speed( const world_t& w, const int k = 0 )
	{
		return w.accel * max_speed_at( w, k ) * w.duck * w.dt;
	}

	inline float friction( const float vz_start, const world_t& w )
	{
		const float vz_cat = vz_start + w.g * w.dt * 0.5f;
		return vz_cat > 0.f && vz_cat <= 140.f ? 0.25f : 1.f;
	}

	inline float exact_wish( const float need, const float vz_start, const world_t& w, const int k = 0 )
	{
		const float per_wish = w.accel * w.duck * w.dt * friction( vz_start, w );
		const float cap      = max_speed_at( w, k );
		return per_wish > 0.0001f ? std::min( need / per_wish, cap ) : cap;
	}

	struct state_t {
		c_vector origin{ }, velocity{ };
		int k = 0;
	};

	struct result_t {
		bool valid = false;
		bool hit   = false;
		bool snapped = false;
		int ticks  = 0;
		int wrote  = 0;
		int slides = 0;
		c_vector in{ };
		c_vector out{ };
		trace_t trace{ };
		state_t first{ };
	};

	inline c_vector clip( const c_vector& v, const c_vector& n )
	{
		const float back = v.dot_product( n );
		return c_vector( v.m_x - n.m_x * back, v.m_y - n.m_y * back, v.m_z - n.m_z * back );
	}

	template < class P >
	result_t fly( state_t s, const world_t& w, const float max_time, const float coast_chord, const float deadband,
	              const n_tick::c_sim_budget* budget, P&& policy )
	{
		result_t r;
		c_trace_filter filter( g_ctx.m_local );

		const float h         = w.g * w.dt * 0.5f;
		const int max_ticks   = std::max( static_cast< int >( max_time / w.dt + 0.5f ), 1 );
		const int chord_ticks = std::max( static_cast< int >( coast_chord / w.dt + 0.5f ), 1 );

		for ( int k = 0; k < max_ticks; ) {
			if ( budget && budget->expired( ) )
				return r;

			c_vector v = s.velocity;
			c_vector want{ };
			bool wrote = false;

			s.k = k;
			if ( policy( s, want ) ) {
				const c_vector delta( want.m_x - v.m_x, want.m_y - v.m_y, 0.f );
				const float need = delta.length_2d( );

				if ( need > deadband ) {
					const c_vector dir( delta.m_x / need, delta.m_y / need, 0.f );
					const float wish = exact_wish( need, v.m_z, w, k ) * w.duck;
					const float add  = std::min( wish, 30.f ) - ( v.m_x * dir.m_x + v.m_y * dir.m_y );

					if ( add > 0.f ) {
						const float acc = std::min( w.accel * wish * w.dt * friction( v.m_z, w ), add );
						v.m_x += dir.m_x * acc;
						v.m_y += dir.m_y * acc;
					}

					wrote = true;
					++r.wrote;
				}
			}

			const int n     = k == 0 || wrote || w.snap > 0.f ? 1 : std::min( chord_ticks, max_ticks - k );
			const float T   = static_cast< float >( n ) * w.dt;
			const float vz0 = v.m_z;
			c_vector end( s.origin.m_x + v.m_x * T, s.origin.m_y + v.m_y * T, s.origin.m_z + vz0 * T - h * w.dt * static_cast< float >( n * n ) );

			/* wall_slides: TryPlayerMove's bump. the chord's rest runs on from the wall along the clipped xy, z keeps its fall */
			c_vector from = s.origin;
			float done    = 0.f;
			for ( ;; ) {
				trace_t tr;
				ray_t ray( from, end, w.mins, w.maxs );
				g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );

				if ( tr.m_start_solid || tr.m_all_solid )
					return r;
				if ( tr.m_fraction >= 1.f )
					break;

				const c_vector& wn = tr.m_plane.m_normal;
				const float at     = done + ( 1.f - done ) * tr.m_fraction;
				if ( r.slides < w.wall_slides && std::fabs( wn.m_z ) < 0.1f ) {
					const float into = std::min( v.m_x * wn.m_x + v.m_y * wn.m_y, 0.f );
					v.m_x -= wn.m_x * into;
					v.m_y -= wn.m_y * into;
					++r.slides;
					done = at;
					from = c_vector( tr.m_end.m_x + wn.m_x * 0.03125f, tr.m_end.m_y + wn.m_y * 0.03125f, tr.m_end.m_z );
					end  = c_vector( from.m_x + v.m_x * T * ( 1.f - done ), from.m_y + v.m_y * T * ( 1.f - done ), end.m_z );
					continue;
				}

				const int i = std::min( static_cast< int >( at * static_cast< float >( n ) ), n - 1 );
				r.valid     = true;
				r.hit       = true;
				r.ticks     = k + i;
				r.in        = c_vector( v.m_x, v.m_y, vz0 - 2.f * h * static_cast< float >( i ) - h );
				r.out       = clip( r.in, tr.m_plane.m_normal );
				r.trace     = tr;
				return r;
			}

			const float vz1 = vz0 - 2.f * h * static_cast< float >( n );
			if ( w.snap > 0.f && vz1 <= 140.f ) {
				trace_t gt;
				ray_t gr( end, c_vector( end.m_x, end.m_y, end.m_z - w.snap ), w.mins, w.maxs );
				g_interfaces.m_engine_trace->trace_ray( gr, mask_playersolid, &filter, &gt );
				if ( !gt.m_start_solid && gt.m_fraction < 1.f && gt.m_plane.m_normal.m_z >= 0.7f ) {
					r.valid   = true;
					r.hit     = true;
					r.snapped = true;
					r.ticks   = k + n - 1;
					r.in      = c_vector( v.m_x, v.m_y, vz1 );
					r.out     = c_vector( v.m_x, v.m_y, 0.f );
					r.trace   = gt;
					return r;
				}
			}

			s.origin   = end;
			s.velocity = c_vector( v.m_x, v.m_y, vz1 );
			if ( k == 0 )
				r.first = s;
			k += n;
		}

		r.valid = true;
		return r;
	}

	struct engine_t {
		bool valid   = false;
		bool contact = false;
		bool ground  = false;
		state_t start{ }, end{ };
	};

	inline engine_t engine_tick( const c_user_cmd* cmd, const world_t& w )
	{
		engine_t e;
		auto* local = g_ctx.m_local;
		if ( !cmd || !local || !g_interfaces.m_move_helper || !g_interfaces.m_prediction )
			return e;

		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
		e.start.origin   = local->get_origin( );
		e.start.velocity = local->get_velocity( );

		c_user_cmd sim = *cmd;
		g_prediction.begin( local, &sim );
		g_prediction.end( local );

		e.end.origin   = local->get_origin( );
		e.end.velocity = local->get_velocity( );
		e.ground       = ( local->get_flags( ) & e_flags::fl_onground ) != 0;
		e.contact      = e.ground || e.end.velocity.m_z > e.start.velocity.m_z - w.g * w.dt + 1.f;
		e.valid        = true;
		return e;
	}
}
