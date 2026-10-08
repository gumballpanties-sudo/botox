#include "movement.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "tick_scale.h"
#include "air_rollout.h"
#include <algorithm>
#include <cmath>

extern void botox_dbg_log( const char* fmt, ... );

namespace
{
	constexpr float k_air_veer_cap = 30.f;
	constexpr float k_face_group_z = 8.f;
	constexpr float k_arrive_frac = 0.5f;
	constexpr float k_walkable_nz = 0.7f;
	constexpr int k_arm_window    = 16;
	constexpr float k_clip_vz_eps = 1.f;
	constexpr int k_settle_cmds   = 4;
	constexpr float k_chord_time = 0.0625f;
	constexpr float k_sweep_time = 1.f;
	constexpr float k_plan_hyst  = 10.f;
	constexpr float k_same_face  = 0.99f; /* normal dot: a re-swept KEEP must meet the face it was solved on */
	constexpr float k_budget_share = 0.25f;
	constexpr float k_fly_slack = 0.25f;
	constexpr float k_fly_max   = 2.f;
	constexpr float k_coast_chord = 0.03125f;
	/* scan: ring of down-rays under the hull ( a pixel the hull can't stand on still answers one ). were menu sliders,
	   a stale saved .bx could zero them */
	constexpr float k_scan_radius       = 24.f;
	constexpr int k_scan_rays           = 8;
	constexpr float k_scan_inner_radius = 10.f;
	constexpr int k_scan_inner_rays     = 6;
	constexpr float k_scan_depth        = 1024.f;
	constexpr float k_support_offsets[ ] = { 3.f, 1.5f, 0.f, -1.5f, -3.f, -6.f, -9.f };
	constexpr int k_support_coarse[ ]    = { 0, 2, 4, 5, 6 }; /* the 3u-spaced slots the robust rule judges */
	constexpr int k_refine_steps         = 6;
	constexpr float k_refine_dz          = 1.f;
	constexpr int k_refine_under         = 6;
	constexpr int k_refine_wide          = 8;
	constexpr float k_duck_lift          = 9.f;  /* cs air duck: origin +9 ( cs_gamemovement.cpp:1244 ) */
	constexpr float k_duck_hull_z        = 54.f;
	constexpr float k_duck_speed         = 0.34f; /* CS_PLAYER_SPEED_DUCK_MODIFIER, crops air wish too */
	constexpr int k_align_slides         = 4;
	constexpr float k_wall_nz            = 0.1f;

	/* bounce align ( clarity cfg_misc_bounce_auto_align: its bounce sim aligns into walls within 0.1u ). per cmd */
	bool g_ab_align = false;
	constexpr float k_lookahead_time = 1.f; /* hard cap, menu reach ends it first */
	constexpr float k_lookahead_step = 0.03125f;
	constexpr float k_wide_rings[ ]   = { 36.f, 54.f, 72.f };
	constexpr int k_wide_ring_rays[ ] = { 16, 20, 24 };
	constexpr int k_wide_max          = 16 + 20 + 24;
	constexpr float k_glide_near      = 8.f;
	constexpr float k_aim_span        = 8.f;
	constexpr int k_aim_steps         = 6;
	constexpr float k_side_max        = 20.f; /* deg off the fall line the bounce may throw you */
	constexpr int k_refuse_release    = 3;
	constexpr float k_exit_time       = 0.0625f;
	constexpr float k_exit_min        = 4.f;
	constexpr float k_snap_reach      = 2.25f; /* engine 2u + margin for steer error */
	constexpr float k_phase_shifts[ ] = { 1.5f, -1.5f, 3.f, -3.f, 4.5f, -4.5f, 6.f, -6.f };
	constexpr float k_min_face_nz     = 0.99f; /* ~8 deg: flatter = floor, not a bounce */

	/* physics dt, never n_tick::interval ( the 128 fix would size for a server that isn't there ) */
	float ab_phys_dt( )
	{
		return g_interfaces.m_global_vars_base && g_interfaces.m_global_vars_base->m_interval_per_tick > 0.f
		         ? g_interfaces.m_global_vars_base->m_interval_per_tick
		         : 0.015625f;
	}

	float ab_gravity( )
	{
		static auto sv_gravity = g_interfaces.m_convar->find_var( "sv_gravity" );
		return sv_gravity ? sv_gravity->get_float( ) : 800.f;
	}

	float ab_max_speed( )
	{
		const float speed = g_ctx.m_local ? g_ctx.m_local->get_max_speed( ) : 0.f;
		return speed > 1.f ? std::min( speed, 450.f ) : 250.f;
	}

	bool ab_bhop_uncapped( )
	{
		static auto sv_enablebunnyhopping = g_interfaces.m_convar->find_var( "sv_enablebunnyhopping" );
		return sv_enablebunnyhopping && sv_enablebunnyhopping->get_int( ) != 0;
	}

	float ab_keep_speed( const float result, const float nz )
	{
		return nz >= k_walkable_nz && !ab_bhop_uncapped( ) ? std::min( result, ab_max_speed( ) * 1.1f ) : result;
	}

	float ab_stop_speed( const float dist, const float vz_start, const n_air::world_t& world )
	{
		const float dt = world.dt;
		const float g  = world.g;

		const auto travel = [ & ]( const float w ) {
			float v = w, vz = vz_start, d = w * dt;
			for ( int k = 0; k < 256 && v > 0.f; ++k ) {
				vz -= g * dt;
				v = std::max( v - 0.9f * n_air::accel_speed( world, k + 1 ) * n_air::friction( vz, world ), 0.f );
				d += v * dt;
			}
			return d;
		};

		float lo = 0.f, hi = 450.f;
		if ( travel( hi ) <= dist )
			return hi;
		for ( int i = 0; i < 12; ++i ) {
			const float mid = ( lo + hi ) * 0.5f;
			( travel( mid ) <= dist ? lo : hi ) = mid;
		}
		return lo;
	}

	// drop in perp gain: best run-then-brake distance, upper bound (the flight decides). straight closing caps at 30
	float ab_gain_reach( const float speed, const float ttc, const n_air::world_t& world )
	{
		const float dt    = world.dt;
		const float accel = n_air::accel_speed( world );
		const float a     = std::min( accel, k_air_veer_cap );
		const float brake = std::max( 0.9f * accel / dt, 1.f );
		const int ticks   = std::min( static_cast< int >( ttc / dt ), 512 );

		float v = speed, d = 0.f, best = 0.f;
		for ( int k = 0; k <= ticks; ++k ) {
			if ( static_cast< float >( k ) * dt + v / brake <= ttc )
				best = std::max( best, d + v * v / ( 2.f * brake ) );
			d += v * dt;
			v = v < k_air_veer_cap ? v + k_air_veer_cap : std::sqrt( v * v + a * a );
		}
		return best * 1.2f + 4.f;
	}

	float ab_time_to_contact( const float vz, const float drop )
	{
		const float g = ab_gravity( );
		return g > 1.f ? std::max( ( vz + std::sqrt( vz * vz + 2.f * g * drop ) ) / g, 0.f ) : 0.f;
	}

	float ab_clip_xy( const c_vector& v, const c_vector& n )
	{
		const float into = v.dot_product( n );
		if ( into >= 0.f )
			return -1.f;

		return c_vector( v.m_x - n.m_x * into, v.m_y - n.m_y * into, 0.f ).length_2d( );
	}

	float ab_bounce( const c_vector& xy, const float vz_hit, const c_vector& n )
	{
		return ab_keep_speed( ab_clip_xy( c_vector( xy.m_x, xy.m_y, -vz_hit ), n ), n.m_z );
	}

	// drop in glides in on time unless the approach runs uphill (clip eats it, dash + stop keeps more)
	bool ab_glides( const c_vector& to, const float dist, const c_vector& n )
	{
		return dist < k_glide_near || to.m_x * n.m_x + to.m_y * n.m_y >= 0.f;
	}

	c_vector ab_glide_xy( const c_vector& origin, const c_vector& at, const float ttc, const c_vector& n )
	{
		const c_vector to( at.m_x - origin.m_x, at.m_y - origin.m_y, 0.f );
		if ( !ab_glides( to, to.length_2d( ), n ) )
			return c_vector( 0.f, 0.f, 0.f );
		const float t = std::max( ttc, ab_phys_dt( ) );
		return c_vector( to.m_x / t, to.m_y / t, 0.f );
	}

	// angle between the bounce's exit xy and straight downhill
	float ab_side_deg( const c_vector& out, const c_vector& n )
	{
		const float flat = std::sqrt( n.m_x * n.m_x + n.m_y * n.m_y );
		const float spd  = out.length_2d( );
		if ( flat < 0.01f || spd < 1.f )
			return 0.f;
		return rad2deg( std::acos( std::clamp( ( out.m_x * n.m_x + out.m_y * n.m_y ) / ( flat * spd ), -1.f, 1.f ) ) );
	}

	// fly ends at contact: a wall / crease right past it eats the slide. fraction of the exit run the hull gets ( 1 = clear )
	float ab_exit_room( const trace_t& hit, const c_vector& out, const c_vector& mins, const c_vector& maxs )
	{
		const float spd = out.length_2d( );
		if ( spd < 1.f )
			return 1.f;

		const float len = std::max( spd * k_exit_time, k_exit_min );
		const c_vector from( hit.m_end.m_x, hit.m_end.m_y, hit.m_end.m_z + 0.125f );
		c_trace_filter filter( g_ctx.m_local );
		trace_t tr{ };
		ray_t ray( from, c_vector( from.m_x + out.m_x / spd * len, from.m_y + out.m_y / spd * len, from.m_z ), mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		if ( tr.m_start_solid )
			return 0.f;
		return tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z < k_walkable_nz ? tr.m_fraction : 1.f;
	}

	c_vector ab_keep_velocity( const c_vector& velocity, const c_vector& n )
	{
		c_vector keep( velocity.m_x, velocity.m_y, 0.f );

		const float flat = std::sqrt( n.m_x * n.m_x + n.m_y * n.m_y );
		if ( flat < 0.001f )
			return keep;

		const float out = ( velocity.m_x * n.m_x + velocity.m_y * n.m_y ) / flat;
		if ( out < 0.f ) {
			keep.m_x -= n.m_x / flat * out;
			keep.m_y -= n.m_y / flat * out;
		}
		return keep;
	}

	struct ab_hit_t {
		c_vector m_pos{ };
		c_vector m_normal{ };
		float m_angle = 0.f;
		float m_shape = 0.f;
	};

	struct ab_contact_t {
		c_vector m_pos{ };
		c_vector m_normal{ };
		float m_time = 0.f;
	};

	struct ab_plan_t {
		bool m_valid = false;
		bool m_keep  = false;
		c_vector m_target{ };
		c_vector m_normal{ };
		float m_drop   = 0.f;
		float m_vz_hit = 0.f;
		float m_ttc    = 0.f;
		float m_result = 0.f;
		float m_floor  = 0.f;
		int m_lookahead = 0;
		float m_place   = 99.f;
		bool m_flown     = false;
		float m_pred     = -1.f;
		int m_pred_ticks = -1;
		bool m_first_ok  = false;
		n_air::state_t m_first{ };
		int m_slides     = 0;
		float m_aim      = 0.f;
		float m_side     = -1.f;
		float m_room     = -1.f;
	};

	ab_hit_t ab_make_hit( const trace_t& tr )
	{
		const float nz = std::clamp( tr.m_plane.m_normal.m_z, -1.f, 1.f );
		ab_hit_t h{ };
		h.m_pos    = tr.m_end;
		h.m_normal = tr.m_plane.m_normal;
		h.m_angle  = rad2deg( std::acos( nz ) );
		h.m_shape  = nz * std::sqrt( std::max( 1.f - nz * nz, 0.f ) );
		return h;
	}

	/* down ray: z of what it lands on ( miss = bottom ), flat = may straddle a lip with a neighbour */
	struct ab_ray_t {
		c_vector start{ };
		float z   = 0.f;
		bool flat = false;
	};

	ab_ray_t ab_ray_down( const c_vector& start, const float depth, c_trace_filter& filter, trace_t& tr )
	{
		ray_t ray( start, c_vector( start.m_x, start.m_y, start.m_z - depth ) );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );

		ab_ray_t r{ start, start.m_z - depth, true };
		if ( tr.m_fraction <= 0.f ) {
			r.z    = start.m_z;
			r.flat = false;
		} else if ( tr.m_fraction < 1.f ) {
			r.z    = tr.m_end.m_z;
			r.flat = tr.m_plane.m_normal.m_z >= 0.99f;
		}
		return r;
	}

	// ledge lip: two flat neighbours at different heights straddle an edge. halve toward it, a bevel answers sloped
	bool ab_refine( c_trace_filter& filter, ab_ray_t a, ab_ray_t b, const float depth, ab_hit_t& out )
	{
		for ( int i = 0; i < k_refine_steps; ++i ) {
			trace_t tr{ };
			const ab_ray_t m = ab_ray_down( ( a.start + b.start ) * 0.5f, depth, filter, tr );
			if ( tr.m_fraction > 0.f && tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z > 0.f && tr.m_plane.m_normal.m_z < 0.99f ) {
				out = ab_make_hit( tr );
				return true;
			}
			( std::fabs( m.z - a.z ) < std::fabs( m.z - b.z ) ? a : b ) = m;
		}
		return false;
	}

	bool ab_lip( const ab_ray_t& a, const ab_ray_t& b )
	{
		return a.flat && b.flat && std::fabs( a.z - b.z ) > k_refine_dz;
	}

	bool ab_scan_below( const c_vector& at, const float radius, const int rays, const float depth, ab_hit_t& out,
	                    c_vector* patch_avg = nullptr, const n_tick::c_sim_budget* budget = nullptr )
	{
		constexpr float two_pi   = 6.28318530717958647692f;
		constexpr int k_max_rays = 64;

		c_trace_filter filter( g_ctx.m_local );
		ab_hit_t hits[ k_max_rays + k_refine_under ];
		ab_ray_t starts[ k_max_rays ];
		int count   = 0;
		float top_z = -1e9f;

		const int ring  = std::clamp( rays, 1, k_max_rays - 1 - k_scan_inner_rays );
		const int total = 1 + k_scan_inner_rays + ring;
		for ( int i = 0; i < total; ++i ) {
			c_vector start = at;
			start.m_z += 2.f;
			if ( i > 0 ) {
				const bool inner = i <= k_scan_inner_rays;
				const int n      = inner ? k_scan_inner_rays : ring;
				const int j      = inner ? i - 1 : i - 1 - k_scan_inner_rays;
				const float r    = inner ? k_scan_inner_radius : radius;
				const float ang  = static_cast< float >( j ) / static_cast< float >( n ) * two_pi;
				start.m_x += std::cos( ang ) * r;
				start.m_y += std::sin( ang ) * r;
			}

			trace_t tr{ };
			starts[ i ] = ab_ray_down( start, depth, filter, tr );

			if ( tr.m_fraction >= 1.f || tr.m_fraction <= 0.f || tr.m_plane.m_normal.m_z <= 0.f )
				continue;

			hits[ count++ ] = ab_make_hit( tr );
			top_z           = std::max( top_z, tr.m_end.m_z );
		}

		/* centre-inner pairs first ( a lip right under the hull ), then each ring's neighbours */
		int n_ref       = 0;
		const auto pair = [ & ]( const int i, const int j ) {
			if ( n_ref >= k_refine_under || !ab_lip( starts[ i ], starts[ j ] ) || ( budget && budget->expired( ) ) )
				return;
			++n_ref;
			if ( ab_refine( filter, starts[ i ], starts[ j ], depth, hits[ count ] ) ) {
				top_z = std::max( top_z, hits[ count ].m_pos.m_z );
				++count;
			}
		};
		for ( int j = 0; j < k_scan_inner_rays; ++j )
			pair( 0, 1 + j );
		for ( int j = 0; j < k_scan_inner_rays; ++j )
			pair( 1 + j, 1 + ( j + 1 ) % k_scan_inner_rays );
		for ( int j = 0; j < ring; ++j )
			pair( 1 + k_scan_inner_rays + j, 1 + k_scan_inner_rays + ( j + 1 ) % ring );

		if ( count <= 0 )
			return false;

		int best = -1;
		for ( int i = 0; i < count; ++i ) {
			if ( hits[ i ].m_pos.m_z >= top_z - k_face_group_z && ( best < 0 || hits[ i ].m_shape > hits[ best ].m_shape ) )
				best = i;
		}

		if ( best < 0 )
			return false;

		out = hits[ best ];

		if ( patch_avg ) {
			c_vector sum( 0.f, 0.f, 0.f );
			int sum_count = 0;
			for ( int i = 0; i < count; ++i ) {
				if ( hits[ i ].m_pos.m_z >= top_z - k_face_group_z && hits[ i ].m_normal.dot_product( out.m_normal ) >= k_same_face ) {
					sum = sum + hits[ i ].m_pos;
					++sum_count;
				}
			}
			*patch_avg = sum / static_cast< float >( sum_count );
		}

		return true;
	}

	bool ab_hull_down( const c_vector& from, const float depth, const c_vector& mins, const c_vector& maxs, ab_contact_t& out )
	{
		c_trace_filter filter( g_ctx.m_local );
		trace_t tr{ };
		ray_t ray( from, c_vector( from.m_x, from.m_y, from.m_z - depth ), mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );

		if ( tr.m_start_solid || tr.m_fraction <= 0.f || tr.m_fraction >= 1.f || tr.m_plane.m_normal.m_z <= 0.f )
			return false;

		out.m_pos    = tr.m_end;
		out.m_normal = tr.m_plane.m_normal;
		return true;
	}

	bool ab_sweep_fall( const c_vector& origin, const c_vector& xy, const float vz, const float depth, const c_vector& mins,
	                    const c_vector& maxs, const n_tick::c_sim_budget& budget, ab_contact_t& out )
	{
		const float g = ab_gravity( );
		c_trace_filter filter( g_ctx.m_local );

		c_vector prev = origin;
		for ( float t = k_chord_time; t < k_sweep_time + 0.001f; t += k_chord_time ) {
			if ( budget.expired( ) )
				return false;

			const c_vector p( origin.m_x + xy.m_x * t, origin.m_y + xy.m_y * t, origin.m_z + vz * t - 0.5f * g * t * t );

			trace_t tr{ };
			ray_t ray( prev, p, mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );

			if ( tr.m_start_solid )
				return false;
			if ( tr.m_fraction < 1.f ) {
				if ( tr.m_fraction <= 0.f || tr.m_plane.m_normal.m_z <= 0.f )
					return false;

				out.m_pos    = tr.m_end;
				out.m_normal = tr.m_plane.m_normal;
				out.m_time   = t - k_chord_time * ( 1.f - tr.m_fraction );
				return true;
			}
			if ( origin.m_z - p.m_z > depth )
				return false;

			prev = p;
		}
		return false;
	}

	// align: a landing spot the hull can't fit at ( wall in reach ) slides flush along the wall it hits flying there level
	bool ab_flush_to_wall( const c_vector& origin, const c_vector& patch, const c_vector& mins, const c_vector& maxs, c_vector& out )
	{
		c_trace_filter filter( g_ctx.m_local );
		const c_vector to( patch.m_x, patch.m_y, origin.m_z );
		trace_t tr{ };
		ray_t ray( origin, to, mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		if ( tr.m_start_solid || tr.m_fraction >= 1.f || std::fabs( tr.m_plane.m_normal.m_z ) >= k_wall_nz )
			return false;

		const c_vector& n = tr.m_plane.m_normal;
		const c_vector left( to.m_x - tr.m_end.m_x, to.m_y - tr.m_end.m_y, 0.f );
		const float into = left.m_x * n.m_x + left.m_y * n.m_y;
		const c_vector start( tr.m_end.m_x + n.m_x * 0.03125f, tr.m_end.m_y + n.m_y * 0.03125f, tr.m_end.m_z );
		trace_t slide{ };
		ray_t slide_ray( start, c_vector( start.m_x + left.m_x - n.m_x * into, start.m_y + left.m_y - n.m_y * into, start.m_z ), mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( slide_ray, mask_playersolid, &filter, &slide );
		if ( slide.m_start_solid )
			return false;

		out = slide.m_end;
		return true;
	}

	bool ab_plan_stop_at( const c_vector& origin, const c_vector& velocity, const c_vector& patch, const float probe, const float need,
	                      const float max_ttc, const c_vector& mins, const c_vector& maxs, ab_plan_t& out, const bool gain = false )
	{
		ab_contact_t c{ };
		c_vector flush{ };
		if ( !ab_hull_down( c_vector( patch.m_x, patch.m_y, origin.m_z ), probe, mins, maxs, c ) &&
		     !( g_ab_align && ab_flush_to_wall( origin, patch, mins, maxs, flush ) &&
		        ab_hull_down( flush, probe + ( origin.m_z - flush.m_z ), mins, maxs, c ) ) )
			return false;

		out.m_keep   = false;
		out.m_target = c.m_pos;
		out.m_normal = c.m_normal;
		out.m_drop   = std::max( origin.m_z - c.m_pos.m_z, 0.f );
		out.m_vz_hit = std::sqrt( velocity.m_z * velocity.m_z + 2.f * ab_gravity( ) * out.m_drop );
		out.m_ttc    = ab_time_to_contact( velocity.m_z, out.m_drop );
		out.m_floor  = ab_bounce( c_vector( 0.f, 0.f, 0.f ), out.m_vz_hit, c.m_normal );
		out.m_result = gain ? ab_bounce( ab_glide_xy( origin, c.m_pos, out.m_ttc, c.m_normal ), out.m_vz_hit, c.m_normal ) : out.m_floor;

		const c_vector to( c.m_pos.m_x - origin.m_x, c.m_pos.m_y - origin.m_y, 0.f );
		const float dist   = to.length_2d( );
		const float toward = dist > 1.f ? std::max( ( velocity.m_x * to.m_x + velocity.m_y * to.m_y ) / dist, 0.f ) : 0.f;

		const bool reachable = dist <= ( toward + k_air_veer_cap ) * out.m_ttc + 1.f ||
		                       ( gain && dist <= ab_gain_reach( velocity.length_2d( ), out.m_ttc, n_air::read_world( mins, maxs ) ) );

		out.m_valid = out.m_result >= need && out.m_ttc <= max_ttc && reachable;
		return out.m_valid;
	}

	float ab_ray_score( const c_vector& origin, const c_vector& velocity, const ab_hit_t& hit, const bool glide )
	{
		const float ray_drop = std::max( origin.m_z - hit.m_pos.m_z, 0.f );
		const float vz_hit   = std::sqrt( velocity.m_z * velocity.m_z + 2.f * ab_gravity( ) * ray_drop );
		const c_vector xy    = glide ? ab_glide_xy( origin, hit.m_pos, ab_time_to_contact( velocity.m_z, ray_drop ), hit.m_normal ) : c_vector( 0.f, 0.f, 0.f );
		return ab_bounce( xy, vz_hit, hit.m_normal );
	}

	bool ab_plan_stop( const c_vector& origin, const c_vector& velocity, const ab_hit_t& hit, const c_vector& patch, const float need,
	                   const float max_ttc, const c_vector& mins, const c_vector& maxs, ab_plan_t& out, const bool gain = false )
	{
		out.m_result = ab_ray_score( origin, velocity, hit, gain );
		if ( out.m_result < need )
			return false;

		const float ray_drop = std::max( origin.m_z - hit.m_pos.m_z, 0.f );
		const float flat     = std::sqrt( hit.m_normal.m_x * hit.m_normal.m_x + hit.m_normal.m_y * hit.m_normal.m_y );

		if ( flat >= 0.01f ) {
			constexpr int n = static_cast< int >( sizeof( k_support_offsets ) / sizeof( k_support_offsets[ 0 ] ) );

			const c_vector up( -hit.m_normal.m_x / flat, -hit.m_normal.m_y / flat, 0.f );
			const float support = maxs.m_x * std::fabs( up.m_x ) + maxs.m_y * std::fabs( up.m_y );
			const float probe = ray_drop + 64.f + ( support - k_support_offsets[ n - 1 ] ) * flat / std::max( hit.m_normal.m_z, 0.1f );

			const auto plan_at = [ & ]( const float k, ab_plan_t& p ) {
				const float back = support - k;
				const c_vector at( hit.m_pos.m_x - up.m_x * back, hit.m_pos.m_y - up.m_y * back, hit.m_pos.m_z );
				ab_plan_stop_at( origin, velocity, at, probe, need, max_ttc, mins, maxs, p, gain );
				p.m_place = k;
				return p.m_normal.dot_product( hit.m_normal ) >= k_same_face;
			};

			ab_plan_t plans[ n ];
			bool on_face[ n ];
			for ( int i = 0; i < n; ++i )
				on_face[ i ] = plan_at( k_support_offsets[ i ], plans[ i ] );

			/* lowest 3u slot whose neighbours land on the face too ( room for a stop error ) */
			constexpr int nc = static_cast< int >( sizeof( k_support_coarse ) / sizeof( k_support_coarse[ 0 ] ) );
			for ( int j = nc - 2; j >= 1; --j ) {
				const int i = k_support_coarse[ j ];
				if ( plans[ i ].m_valid && on_face[ k_support_coarse[ j - 1 ] ] && on_face[ k_support_coarse[ j + 1 ] ] ) {
					out = plans[ i ];
					return true;
				}
			}

			/* thin face ( ledge lip ): middle of the widest valid run, its lowest slot if the middle misses */
			int lo = -1, hi = -1;
			for ( int i = 0; i < n; ) {
				if ( !plans[ i ].m_valid ) {
					++i;
					continue;
				}
				int j = i;
				while ( j + 1 < n && plans[ j + 1 ].m_valid )
					++j;
				if ( lo < 0 || j - i > hi - lo ) {
					lo = i;
					hi = j;
				}
				i = j + 1;
			}
			if ( lo >= 0 ) {
				ab_plan_t mid{ };
				out = hi > lo && plan_at( ( k_support_offsets[ lo ] + k_support_offsets[ hi ] ) * 0.5f, mid ) && mid.m_valid ? mid : plans[ hi ];
				return true;
			}
		}

		return ab_plan_stop_at( origin, velocity, patch, ray_drop + 64.f, need, max_ttc, mins, maxs, out, gain );
	}

	/* drop in: rings beside us, each ray hit its own patch ( the "highest face" rule only holds under the hull ).
	   hull must fly there level ( no patch through a wall ). best analytic first, first whose flight pays ( accept ) wins */
	template < class A >
	bool ab_plan_wide( const c_vector& origin, const c_vector& velocity, const float need, const float reach, const c_vector& mins,
	                   const c_vector& maxs, const n_tick::c_sim_budget& budget, A&& accept, ab_plan_t& out )
	{
		constexpr float two_pi = 6.28318530717958647692f;
		constexpr int k_rings  = static_cast< int >( sizeof( k_wide_rings ) / sizeof( k_wide_rings[ 0 ] ) );
		c_trace_filter filter( g_ctx.m_local );

		struct ray_cand_t {
			ab_hit_t hit;
			float score;
		};
		ray_cand_t cands[ k_wide_max + k_refine_wide ];
		int count = 0, n_ref = 0;

		const auto offer = [ & ]( const ab_hit_t& hit ) {
			const float score = ab_ray_score( origin, velocity, hit, true );
			if ( score >= need )
				cands[ count++ ] = { hit, score };
		};

		for ( int ri = 0; ri < k_rings && k_wide_rings[ ri ] <= reach; ++ri ) {
			const float r  = k_wide_rings[ ri ];
			const int rays = k_wide_ring_rays[ ri ];
			ab_ray_t ring[ 24 ];
			int cast = 0;
			for ( ; cast < rays && !budget.expired( ); ++cast ) {
				const float ang = ( static_cast< float >( cast ) + ( ri & 1 ? 0.5f : 0.f ) ) / static_cast< float >( rays ) * two_pi;
				const c_vector start( origin.m_x + std::cos( ang ) * r, origin.m_y + std::sin( ang ) * r, origin.m_z + 2.f );

				trace_t tr{ };
				ring[ cast ] = ab_ray_down( start, k_scan_depth, filter, tr );

				const float nz = tr.m_plane.m_normal.m_z;
				if ( tr.m_fraction >= 1.f || tr.m_fraction <= 0.f || nz <= 0.f || nz >= 0.99f )
					continue;

				offer( ab_make_hit( tr ) );
			}

			for ( int j = 0; j < rays && n_ref < k_refine_wide && cast == rays && !budget.expired( ); ++j ) {
				const int k = ( j + 1 ) % rays;
				if ( !ab_lip( ring[ j ], ring[ k ] ) )
					continue;
				++n_ref;
				ab_hit_t hit{ };
				if ( ab_refine( filter, ring[ j ], ring[ k ], k_scan_depth, hit ) )
					offer( hit );
			}
		}

		std::sort( cands, cands + count, [ ]( const ray_cand_t& a, const ray_cand_t& b ) { return a.score > b.score; } );

		for ( int i = 0; i < count && !budget.expired( ); ++i ) {
			ab_plan_t cand{ };
			if ( !ab_plan_stop( origin, velocity, cands[ i ].hit, cands[ i ].hit.m_pos, need, 1e9f, mins, maxs, cand, true ) )
				continue;

			trace_t wall{ };
			ray_t level( origin, c_vector( cand.m_target.m_x, cand.m_target.m_y, origin.m_z ), mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( level, mask_playersolid, &filter, &wall );
			const bool slide_ok = g_ab_align && !wall.m_start_solid && std::fabs( wall.m_plane.m_normal.m_z ) < k_wall_nz;
			if ( wall.m_start_solid || ( wall.m_fraction < 1.f && !slide_ok ) || !accept( cand ) )
				continue;

			out             = cand;
			out.m_lookahead = -1;
			return true;
		}
		return false;
	}

	// keep: sweep current xy, strip the into-face part, re-sweep (must meet the same face).
	// contact under a tick out = no plan (would arm on a slide already riding)
	bool ab_plan_keep( const c_vector& origin, const c_vector& velocity, const float need, const float depth, const c_vector& mins,
	                   const c_vector& maxs, const n_tick::c_sim_budget& budget, ab_plan_t& out )
	{
		c_vector xy( velocity.m_x, velocity.m_y, 0.f );

		ab_contact_t c{ };
		if ( !ab_sweep_fall( origin, xy, velocity.m_z, depth, mins, maxs, budget, c ) || c.m_time < ab_phys_dt( ) )
			return false;

		const c_vector kept = ab_keep_velocity( xy, c.m_normal );
		if ( ( kept - xy ).length_2d( ) > 1.f ) {
			const c_vector first = c.m_normal;
			xy                   = kept;
			if ( !ab_sweep_fall( origin, xy, velocity.m_z, depth, mins, maxs, budget, c ) || c.m_time < ab_phys_dt( ) ||
			     c.m_normal.dot_product( first ) < k_same_face )
				return false;
		}

		const float g      = ab_gravity( );
		const float drop   = origin.m_z - c.m_pos.m_z;
		const float vz_mag = std::sqrt( std::max( velocity.m_z * velocity.m_z + 2.f * g * drop, 0.f ) );
		const float vz_hit = velocity.m_z - g * c.m_time < 0.f ? -vz_mag : vz_mag;

		out.m_keep   = true;
		out.m_target = c.m_pos;
		out.m_normal = c.m_normal;
		out.m_drop   = drop;
		out.m_vz_hit = vz_mag;
		out.m_ttc    = c.m_time;
		out.m_result = ab_keep_speed( ab_clip_xy( c_vector( xy.m_x, xy.m_y, vz_hit ), c.m_normal ), c.m_normal.m_z );
		out.m_floor  = out.m_result;
		out.m_valid  = out.m_result >= need;
		return out.m_valid;
	}

	void ab_steer( c_user_cmd* cmd, const c_vector& velocity, const c_vector& want, const n_air::world_t& world )
	{
		const c_vector delta( want.m_x - velocity.m_x, want.m_y - velocity.m_y, 0.f );
		const float need = delta.length_2d( );

		cmd->m_buttons &= ~( in_forward | in_back | in_moveleft | in_moveright );

		if ( need > 1.f ) {
			const float move_yaw = delta.to_angle( ).m_y;
			const float rotation = deg2rad( move_yaw - cmd->m_view_point.m_y );
			const float wish     = n_air::exact_wish( need, velocity.m_z, world );

			cmd->m_forward_move = std::cos( rotation ) * wish;
			cmd->m_side_move    = -std::sin( rotation ) * wish;
		} else {
			cmd->m_forward_move = 0.f;
			cmd->m_side_move    = 0.f;
		}
	}

	c_vector ab_stop_want( const c_vector& origin, const c_vector& velocity, const c_vector& target, const c_vector& normal, const bool drop_in,
	                       const n_air::world_t& world, float* out_dist = nullptr, float* out_want = nullptr )
	{
		const c_vector to_target( target.m_x - origin.m_x, target.m_y - origin.m_y, 0.f );
		const float dist = to_target.length_2d( );
		const float ttc  = ab_time_to_contact( velocity.m_z, std::max( origin.m_z - target.m_z, 0.f ) );

		c_vector want( 0.f, 0.f, 0.f );
		float want_speed = 0.f;

		if ( dist > 0.5f ) {
			const bool glide    = drop_in && ab_glides( to_target, dist, normal );
			const float arrive  = glide ? std::max( ttc, ab_phys_dt( ) ) : drop_in ? ab_phys_dt( ) : std::max( ttc * k_arrive_frac, ab_phys_dt( ) );
			const float profile = std::min( dist / arrive, ab_stop_speed( dist, velocity.m_z, world ) );
			const float speed   = velocity.length_2d( );
			want_speed = std::min( profile, speed + k_air_veer_cap );
			want       = c_vector( to_target.m_x / dist * want_speed, to_target.m_y / dist * want_speed, 0.f );

			// aimed + under the profile: straight adds nothing past 30, a perp push grows |v|^2 by a^2 a tick
			const float perp_gain = std::min( n_air::accel_speed( world ) * n_air::friction( velocity.m_z, world ), k_air_veer_cap );
			if ( drop_in && speed >= 28.f && profile >= std::sqrt( speed * speed + perp_gain * perp_gain ) &&
			     velocity.m_x * to_target.m_x + velocity.m_y * to_target.m_y >= 0.94f * speed * dist ) {
				const float cross   = velocity.m_x * to_target.m_y - velocity.m_y * to_target.m_x;
				const c_vector perp = cross >= 0.f ? c_vector( -velocity.m_y / speed, velocity.m_x / speed, 0.f )
				                                   : c_vector( velocity.m_y / speed, -velocity.m_x / speed, 0.f );
				want       = c_vector( velocity.m_x + perp.m_x * 450.f, velocity.m_y + perp.m_y * 450.f, 0.f );
				want_speed = want.length_2d( );
			}
		}

		if ( out_dist )
			*out_dist = dist;
		if ( out_want )
			*out_want = want_speed;

		return want;
	}

	constexpr int k_viz_max = n_movement::impl_t::auto_bounce_data_t::k_viz_max;

	n_air::result_t ab_fly( const n_air::state_t& from, const n_air::world_t& w, const ab_plan_t& plan, const bool drop_in,
	                        const n_tick::c_sim_budget* budget, c_vector* path = nullptr, int* path_n = nullptr )
	{
		const float horizon = std::min( plan.m_ttc + k_fly_slack, k_fly_max );
		return n_air::fly( from, w, horizon, k_coast_chord, 1.f, budget, [ & ]( const n_air::state_t& s, c_vector& want ) {
			if ( path && *path_n < k_viz_max )
				path[ ( *path_n )++ ] = s.origin;
			want = plan.m_keep ? ab_keep_velocity( s.velocity, plan.m_normal )
			                   : ab_stop_want( s.origin, s.velocity, plan.m_target, plan.m_normal, drop_in, n_air::advance( w, s.k ) );
			return true;
		} );
	}

	float ab_fly_speed( const n_air::result_t& r )
	{
		if ( !r.valid || !r.hit )
			return -1.f;

		const float nz = r.trace.m_plane.m_normal.m_z;
		return nz > 0.f ? ab_keep_speed( r.out.length_2d( ), nz ) : -1.f;
	}

	struct ab_gates_t {
		int yield = 0, off = 0, ground = 0, settle = 0, noscan = 0, noplan = 0, refused = 0, armed = 0, hold = 0, side = 0, blk = 0, snap = 0;
		float best = -1.f, need = 0.f, spd = 0.f, ang = -1.f, rang = -1.f;
		unsigned long long next = 0ull;
	} g_gates;

	void ab_gates_report( )
	{
		const unsigned long long now = GetTickCount64( );
		if ( now < g_gates.next )
			return;

		auto& g = g_gates;
		if ( GET_VARIABLE( g_variables.m_debug_log, bool ) &&
		     ( g.yield || g.off || g.ground || g.settle || g.noscan || g.noplan || g.refused || g.armed || g.hold ) )
			botox_dbg_log( "[ab] gates yield=%d off=%d ground=%d settle=%d noscan=%d noplan=%d refused=%d side=%d blk=%d snap=%d armed=%d hold=%d best=%.0f need=%.0f spd=%.0f ang=%.1f rang=%.1f\n",
			               g.yield, g.off, g.ground, g.settle, g.noscan, g.noplan, g.refused, g.side, g.blk, g.snap, g.armed, g.hold, g.best, g.need, g.spd, g.ang, g.rang );
		g      = ab_gates_t{ };
		g.next = now + 1000ull;
	}

	struct ab_verify_t {
		bool engine = false;
		float pred  = -1.f;
		int ticks   = -1;
		float err   = -1.f;
		bool land   = false;
		c_vector land_pos{ }, land_normal{ };
	};

	ab_verify_t ab_verify( const c_user_cmd* cmd, const n_air::world_t& w, const ab_plan_t& plan, const bool drop_in,
	                       const n_tick::c_sim_budget& budget, c_vector* path = nullptr, int* path_n = nullptr )
	{
		ab_verify_t v;
		const n_air::engine_t e = n_air::engine_tick( cmd, w );
		if ( !e.valid )
			return v;

		v.engine = true;
		if ( plan.m_first_ok )
			v.err = ( e.end.origin - plan.m_first.origin ).length( );
		if ( path )
			path[ ( *path_n )++ ] = e.start.origin;

		if ( e.contact ) {
			v.pred        = ab_keep_speed( e.end.velocity.length_2d( ), plan.m_normal.m_z );
			v.ticks       = 0;
			v.land        = true;
			v.land_pos    = e.end.origin;
			v.land_normal = plan.m_normal;
			if ( path )
				path[ ( *path_n )++ ] = e.end.origin;
			return v;
		}

		const n_air::result_t r = ab_fly( e.end, n_air::advance( w, 1 ), plan, drop_in, &budget, path, path_n );
		if ( r.hit ) {
			v.land        = true;
			v.land_pos    = r.trace.m_end;
			v.land_normal = r.trace.m_plane.m_normal;
			if ( path && *path_n < k_viz_max )
				path[ ( *path_n )++ ] = r.trace.m_end;
		}
		if ( r.valid ) {
			v.pred  = ab_fly_speed( r );
			v.ticks = r.ticks + 1;
		}
		return v;
	}
}

void n_movement::impl_t::auto_bounce( c_user_cmd* cmd, const bool yield )
{
	auto& data = m_auto_bounce_data;

	ab_gates_report( );
	if ( yield ) {
		if ( GET_VARIABLE( g_variables.m_auto_bounce, bool ) )
			++g_gates.yield;
		return;
	}

	data.m_owns_cmd = false;
	data.m_found    = false;

	if ( !cmd || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || !g_interfaces.m_engine_client->is_in_game( ) )
		return;
	if ( !GET_VARIABLE( g_variables.m_auto_bounce, bool ) ||
	     !g_input.check_input( &GET_VARIABLE( g_variables.m_auto_bounce_key, key_bind_t ) ) ) {
		if ( GET_VARIABLE( g_variables.m_auto_bounce, bool ) )
			++g_gates.off;
		data.reset( );
		return;
	}

	const auto move_type = g_ctx.m_local->get_move_type( );
	if ( move_type == e_move_types::move_type_ladder || move_type == e_move_types::move_type_noclip ||
	     move_type == e_move_types::move_type_fly || move_type == e_move_types::move_type_observer ) {
		data.reset( );
		return;
	}

	const bool ground_start = ( g_prediction.backup_data.m_flags & e_flags::fl_onground ) != 0;
	const bool ground_live  = ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) != 0;

	const bool jump_on = GET_VARIABLE( g_variables.m_auto_bounce_jump, bool );
	const bool crouch  = GET_VARIABLE( g_variables.m_auto_bounce_crouch, bool );

	if ( ground_start ) {
		++g_gates.ground;
		if ( data.m_jump_owed && jump_on ) {
			cmd->m_buttons |= in_jump;
			if ( crouch )
				cmd->m_buttons |= in_duck;
			if ( GET_VARIABLE( g_variables.m_debug_log, bool ) )
				botox_dbg_log( "[ab] JUMP spd=%.0f pred=%.0f entry=%.0f sd=%.1f%s\n", g_prediction.backup_data.m_velocity.length_2d( ), data.m_result,
				               data.m_entry_speed, ab_side_deg( g_prediction.backup_data.m_velocity, data.m_normal ), crouch ? " CROUCH" : "" );
		}

		data.reset( );
		return;
	}

	if ( data.m_jump_owed && jump_on )
		cmd->m_buttons &= ~in_jump;
	/* air duck = feet up 9 at once: 9u more fall onto the face, more vz to turn into speed. arm to ground only
	   ( a held key with no bounce must not crop air strafe to 0.34 ) */
	if ( crouch && data.m_jump_owed )
		cmd->m_buttons |= in_duck;

	if ( ground_live )
		return;

	auto* const collideable = g_ctx.m_local->get_collideable( );
	if ( !collideable )
		return;
	const c_vector mins = collideable->get_obb_mins( );
	c_vector maxs       = collideable->get_obb_maxs( );

	c_vector origin         = g_prediction.backup_data.m_origin;
	const c_vector velocity = g_prediction.backup_data.m_velocity;
	/* the duck lands inside the arming cmd's tick, before the move: plan from the ducked hull already */
	if ( crouch && !( g_prediction.backup_data.m_flags & e_flags::fl_ducking ) ) {
		origin.m_z += k_duck_lift;
		maxs.m_z = k_duck_hull_z;
	}
	const float speed_2d    = velocity.length_2d( );
	const bool debug        = GET_VARIABLE( g_variables.m_debug_log, bool );

	/* contact watch, cmd start to cmd start: a steep-face clip never grounds, so this is what ends the
	   arm. gaps owned by tb/air_stuck/fireman are bridged with that many ticks of gravity */
	const float prev_vz    = data.m_last_vz;
	const float prev_speed = data.m_last_speed;
	const int gap          = cmd->m_command_number - data.m_last_cmd;
	const bool chained     = data.m_last_cmd != 0 && gap >= 1 && gap <= k_settle_cmds;
	data.m_last_cmd        = cmd->m_command_number;
	data.m_last_vz         = velocity.m_z;
	data.m_last_speed      = speed_2d;

	if ( chained && velocity.m_z > prev_vz - ab_gravity( ) * ab_phys_dt( ) * static_cast< float >( gap ) + k_clip_vz_eps ) {
		if ( debug && data.m_armed_tick > 0 )
			botox_dbg_log( "[ab] CONTACT plan=%s spd=%.0f->%.0f predicted=%.0f entry=%.0f vz=%.0f->%.0f\n", data.m_keep ? "keep" : "stop",
			               prev_speed, speed_2d, data.m_result, data.m_entry_speed, prev_vz, velocity.m_z );
		data.disarm( );
		data.m_clip_cmd = cmd->m_command_number;
		++g_gates.settle;
		return;
	}
	if ( data.m_clip_cmd != 0 && cmd->m_command_number - data.m_clip_cmd <= k_settle_cmds ) {
		++g_gates.settle;
		return;
	}

	if ( data.m_armed_tick > 0 && cmd->m_tick_count - data.m_armed_tick > k_arm_window ) {
		data.disarm( );
		data.m_jump_owed = false;
	}
	const bool armed = data.m_armed_tick > 0;

	/* gain over your own xy ( clarity ). old max( entry, 250 ) floor + 1.1 jump crop left only 275 exact: slow drops never armed */
	const float entry    = armed ? data.m_entry_speed : speed_2d;
	const float base     = entry;
	const float gain_min = std::max( GET_VARIABLE( g_variables.m_auto_bounce_min_gain, float ), 1.f );
	const float need     = base + gain_min;

	const float radius = k_scan_radius;
	const int rays     = k_scan_rays;
	const float depth  = k_scan_depth;
	const float reach  = std::max( GET_VARIABLE( g_variables.m_auto_bounce_reach, float ), 16.f );

	const int mode    = GET_VARIABLE( g_variables.m_auto_bounce_mode, int );
	const bool drop_in = mode == 0;
	const bool keep_on = mode == 1;
	const auto lock_input = [ & ]( ) {
		cmd->m_up_move = 0.f;
		cmd->m_buttons &= ~in_speed;
		if ( !crouch )
			cmd->m_buttons &= ~in_duck;
	};
	const float stop_max_ttc = keep_on ? k_sweep_time : 1e9f;

	n_tick::c_sim_budget budget{ };
	budget.start( k_budget_share, 0.f, n_tick::search_ab );

	n_air::world_t world = n_air::read_world( mins, maxs );
	if ( crouch )
		world.duck = k_duck_speed;
	g_ab_align        = GET_VARIABLE( g_variables.m_auto_bounce_align, bool );
	world.wall_slides = g_ab_align ? k_align_slides : 0;
	world.snap        = k_snap_reach;
	n_air::state_t start{ };
	start.origin   = origin;
	start.velocity = velocity;

	const auto verify = [ & ]( const ab_plan_t& p ) {
		const bool viz = GET_VARIABLE( g_variables.m_auto_bounce_visualize, bool );
		int n          = 0;
		const ab_verify_t v = ab_verify( cmd, world, p, drop_in, budget, viz ? data.m_viz : nullptr, &n );
		data.m_viz_n      = n;
		data.m_viz_land   = v.land ? v.land_pos : p.m_target;
		data.m_viz_normal = v.land ? v.land_normal : p.m_normal;
		return v;
	};

	int flown = 0, refused = 0;
	const auto on_face = [ ]( const n_air::result_t& r, const ab_plan_t& c ) {
		return r.hit && r.trace.m_plane.m_normal.dot_product( c.m_normal ) >= k_same_face;
	};
	const auto accept = [ & ]( ab_plan_t& c ) {
		/* the face must pay from vz alone: a glide onto flat floor is air strafe, not a bounce */
		if ( !c.m_valid || c.m_floor < gain_min || c.m_normal.m_z > k_min_face_nz || ( c.m_target - origin ).length_2d( ) > reach )
			return false;

		n_air::result_t r = ab_fly( start, world, c, drop_in, &budget );
		if ( !r.valid )
			return c.m_floor >= need;

		/* pixel strip: flight lands off the face -> bisect the target along the slope ( landed higher = too far uphill ) */
		const float flat = std::sqrt( c.m_normal.m_x * c.m_normal.m_x + c.m_normal.m_y * c.m_normal.m_y );
		if ( !c.m_keep && r.hit && !on_face( r, c ) && flat >= 0.01f ) {
			const c_vector up( -c.m_normal.m_x / flat, -c.m_normal.m_y / flat, 0.f );
			const c_vector base = c.m_target;
			float lo = -k_aim_span, hi = k_aim_span;
			( r.trace.m_end.m_z > base.m_z + 0.1f ? hi : lo ) = 0.f;
			for ( int i = 0; i < k_aim_steps && !budget.expired( ); ++i ) {
				ab_plan_t p = c;
				p.m_aim     = ( lo + hi ) * 0.5f;
				p.m_target  = c_vector( base.m_x + up.m_x * p.m_aim, base.m_y + up.m_y * p.m_aim, base.m_z );
				const n_air::result_t t = ab_fly( start, world, p, drop_in, &budget );
				if ( !t.valid )
					break;
				if ( on_face( t, p ) ) {
					c = p;
					r = t;
					break;
				}
				( t.hit && t.trace.m_end.m_z > base.m_z + 0.1f ? hi : lo ) = p.m_aim;
			}
		}

		/* walkable face: a tick ending inside the 2u ground trace grounds you with no clip. slide the spot along the slope so
		   the last tick end sits above that band and the move itself hits the face */
		if ( r.snapped ) {
			++g_gates.snap;
			if ( !c.m_keep && flat >= 0.01f ) {
				const c_vector up( -c.m_normal.m_x / flat, -c.m_normal.m_y / flat, 0.f );
				const float rise    = flat / std::max( c.m_normal.m_z, 0.1f );
				const c_vector base = c.m_target;
				for ( const float s : k_phase_shifts ) {
					if ( budget.expired( ) )
						break;
					ab_plan_t p = c;
					p.m_aim     = c.m_aim + s;
					p.m_target  = c_vector( base.m_x + up.m_x * s, base.m_y + up.m_y * s, base.m_z + s * rise );
					const n_air::result_t t = ab_fly( start, world, p, drop_in, &budget );
					if ( t.valid && t.hit && !t.snapped && on_face( t, p ) ) {
						c = p;
						r = t;
						break;
					}
				}
			}
		}

		++flown;
		c.m_flown      = true;
		c.m_pred       = ab_fly_speed( r );
		c.m_pred_ticks = r.ticks;
		c.m_first_ok   = !r.hit || r.ticks > 0;
		c.m_first      = r.first;
		c.m_slides     = r.slides;
		c.m_side       = r.hit ? ab_side_deg( r.out, r.trace.m_plane.m_normal ) : -1.f;
		const bool straight = c.m_side <= k_side_max;
		c.m_room            = r.hit ? ab_exit_room( r.trace, r.out, world.mins, world.maxs ) : 1.f;
		const bool clear    = c.m_room >= 1.f;
		if ( c.m_pred >= need && straight && clear && !r.snapped && ( c.m_keep || on_face( r, c ) ) )
			return true;

		if ( !straight )
			++g_gates.side;
		if ( !clear )
			++g_gates.blk;
		if ( r.hit )
			g_gates.rang = rad2deg( std::acos( std::clamp( r.trace.m_plane.m_normal.m_z, -1.f, 1.f ) ) );
		++refused;
		c.m_valid = false;
		return false;
	};

	const auto score = [ ]( const ab_plan_t& p ) { return p.m_flown ? p.m_pred : p.m_result; };

	/* unarmed: every source runs, best flown speed wins. analytic under the best ( + hyst ) skips its flight */
	ab_plan_t stop{ };
	const auto beats = [ & ]( const ab_plan_t& c ) { return !stop.m_valid || c.m_result + k_plan_hyst > score( stop ); };
	const auto take  = [ & ]( const ab_plan_t& c ) {
		if ( c.m_valid && ( !stop.m_valid || score( c ) > score( stop ) ) )
			stop = c;
	};

	ab_hit_t hit{ };
	c_vector patch = origin;
	bool seen_below = false;
	if ( !armed ) {
		seen_below = ab_scan_below( origin, radius, rays, depth, hit, &patch, &budget );
		if ( seen_below && ab_plan_stop( origin, velocity, hit, patch, need, stop_max_ttc, mins, maxs, stop, drop_in ) )
			accept( stop );
	}
	const float best_below = stop.m_result;
	const float best_ang   = seen_below ? hit.m_angle : -1.f;

	/* armed: committed spot only, never re-picked. release after k_refuse_release flights in a row say it can't pay */
	bool held_refused = false;
	if ( armed && !data.m_keep ) {
		if ( ab_plan_stop_at( origin, velocity, data.m_target, std::max( origin.m_z - data.m_target.m_z, 0.f ) + 64.f, need, stop_max_ttc, mins,
		                      maxs, stop, drop_in ) )
			accept( stop );
		held_refused = stop.m_flown && !stop.m_valid;
	}

	if ( !armed ) {
		const float dt      = ab_phys_dt( );
		const float grav    = ab_gravity( );
		const int max_ticks = static_cast< int >( k_lookahead_time / dt + 0.5f );
		const int step      = std::max( static_cast< int >( k_lookahead_step / dt + 0.5f ), 1 );

		for ( int t = step; t <= max_ticks && !budget.expired( ); t += step ) {
			const float time = static_cast< float >( t ) * dt;
			if ( speed_2d * time > reach )
				break;
			const c_vector p( origin.m_x + velocity.m_x * time, origin.m_y + velocity.m_y * time,
			                  origin.m_z + velocity.m_z * time - 0.5f * grav * time * time );

			ab_plan_t cand{ };
			if ( ab_scan_below( p, radius, rays, depth, hit, &patch, &budget ) &&
			     ab_plan_stop( origin, velocity, hit, patch, need, stop_max_ttc, mins, maxs, cand, drop_in ) && beats( cand ) && accept( cand ) ) {
				cand.m_lookahead = t;
				take( cand );
			}
		}

		if ( drop_in ) {
			ab_plan_t wide{ };
			if ( ab_plan_wide( origin, velocity, need, reach, mins, maxs, budget, [ & ]( ab_plan_t& c ) { return beats( c ) && accept( c ); }, wide ) )
				take( wide );
		}
	}

	// armed keep stays keep, armed stop stays stop ( no flip between the two mid-air )
	ab_plan_t keep{ };
	if ( keep_on && ( !armed || data.m_keep ) && ab_plan_keep( origin, velocity, need, depth, mins, maxs, budget, keep ) )
		accept( keep );

	const ab_plan_t* plan = keep.m_valid && ( !stop.m_valid || score( keep ) >= score( stop ) ) ? &keep : stop.m_valid ? &stop : nullptr;

	g_gates.refused += refused;
	if ( !plan ) {
		if ( !armed ) {
			++( seen_below ? g_gates.noplan : g_gates.noscan );
			if ( best_below > g_gates.best ) {
				g_gates.spd = speed_2d;
				g_gates.ang = best_ang;
			}
			g_gates.best = std::max( { g_gates.best, best_below, keep.m_result } );
			g_gates.need = need;
			data.disarm( );
			return;
		}
		/* flight keeps saying the held drop misses / throws sideways: braking on only eats your speed */
		/* held spot pays under your entry speed: holding the brake only loses speed ( log 3409: 148 braked to 51, bounced 138 ) */
		const bool losing = !data.m_keep && !held_refused && stop.m_result < data.m_entry_speed;
		if ( losing || ( held_refused && ++data.m_refused >= k_refuse_release ) ) {
			if ( debug )
				botox_dbg_log( "[ab] RELEASE spd=%.0f vz=%.0f rang=%.1f sd=%.1f%s\n", speed_2d, velocity.m_z, g_gates.rang, stop.m_side,
				               losing ? " LOSING" : "" );
			data.disarm( );
			data.m_jump_owed = false;
			return;
		}
		++g_gates.hold;

		const float ttc_hold = ab_time_to_contact( velocity.m_z, std::max( origin.m_z - data.m_target.m_z, 0.f ) );
		if ( data.m_keep )
			ab_steer( cmd, velocity, ab_keep_velocity( velocity, data.m_normal ), world );
		else
			ab_steer( cmd, velocity, ab_stop_want( origin, velocity, data.m_target, data.m_normal, drop_in, world ), world );

		if ( drop_in )
			lock_input( );

		data.m_found    = true;
		data.m_owns_cmd = true;

		ab_plan_t held{ };
		held.m_keep   = data.m_keep;
		held.m_target = data.m_target;
		held.m_normal = data.m_normal;
		held.m_ttc    = ttc_hold;

		const ab_verify_t v = verify( held );
		if ( v.ticks >= 0 )
			data.m_result = v.pred;

		if ( debug )
			botox_dbg_log( "[ab] HOLD %s ttc=%.3f spd=%.0f vz=%.0f fly=%d/%d eng=%.0f(%d) us=%lld%s\n", data.m_keep ? "KEEP" : drop_in ? "DROP" : "STOP",
			               ttc_hold, speed_2d, velocity.m_z, flown, refused, v.pred, v.ticks, budget.used_us( ), held_refused ? " REFUSED" : "" );
		return;
	}

	if ( !armed )
		data.m_entry_speed = speed_2d;
	++g_gates.armed;

	data.m_armed_tick = cmd->m_tick_count;
	data.m_refused    = 0;
	data.m_jump_owed  = true;
	data.m_found      = true;
	data.m_keep       = plan->m_keep;
	data.m_target     = plan->m_target;
	data.m_normal     = plan->m_normal;
	data.m_angle      = rad2deg( std::acos( std::clamp( plan->m_normal.m_z, -1.f, 1.f ) ) );
	data.m_result     = score( *plan );
	data.m_gain       = data.m_result - base;

	float dist = ( plan->m_target - origin ).length_2d( );
	float want = 0.f;

	if ( plan->m_keep ) {
		const c_vector kept = ab_keep_velocity( velocity, plan->m_normal );
		want                = kept.length_2d( );
		ab_steer( cmd, velocity, kept, world );
	} else
		ab_steer( cmd, velocity, ab_stop_want( origin, velocity, plan->m_target, plan->m_normal, drop_in, world, &dist, &want ), world );

	if ( jump_on )
		cmd->m_buttons &= ~in_jump;
	if ( crouch )
		cmd->m_buttons |= in_duck;

	if ( drop_in )
		lock_input( );

	data.m_owns_cmd = true;

	const ab_verify_t v = verify( *plan );
	if ( v.ticks >= 0 )
		data.m_result = v.pred;

	if ( debug )
		botox_dbg_log( "[ab] %s ang=%.1f drop=%.1f vz=%.0f->%.0f res=%.0f flr=%.0f (keep=%.0f%s stop=%.0f%s) gain=%.0f base=%.0f entry=%.0f "
		               "spd=%.0f dist=%.1f want=%.0f ttc=%.3f la=%d pl=%.2f aim=%.2f al=%d sd=%.1f rm=%.2f fwd=%.0f side=%.0f fly=%d/%d pred=%.0f(%d) eng=%.0f(%d) err=%.2f auth=%.1f us=%lld%s%s\n",
		               plan->m_keep ? "KEEP" : drop_in ? "DROP" : "STOP", data.m_angle, plan->m_drop, velocity.m_z, plan->m_vz_hit, plan->m_result,
		               plan->m_floor, keep.m_result, keep.m_valid ? "" : "x", stop.m_result, stop.m_valid ? "" : "x", data.m_gain, base,
		               data.m_entry_speed, speed_2d, dist, want, plan->m_ttc, plan->m_lookahead, plan->m_place, plan->m_aim, g_ab_align ? plan->m_slides : -1,
		               plan->m_side, plan->m_room, cmd->m_forward_move, cmd->m_side_move,
		               flown, refused, plan->m_pred, plan->m_pred_ticks, v.pred, v.ticks, v.err, n_air::accel_speed( world ), budget.used_us( ), budget.expired( ) ? " BUDGET" : "",
		               crouch ? " CROUCH" : "" );
}

// visualize: the armed plan's flight from your body to the contact, ring on the face it drops onto. paint thread, one trace
void n_movement::impl_t::auto_bounce_render( )
{
	const auto& data = m_auto_bounce_data;
	if ( !GET_VARIABLE( g_variables.m_auto_bounce_visualize, bool ) || !data.m_found || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) ||
	     !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	const c_color& col       = GET_VARIABLE( g_variables.m_auto_bounce_visualize_color, c_color );
	const unsigned int color = col.get_u32( );
	auto line = [ & ]( const c_vector& a, const c_vector& b ) {
		c_vector_2d sa, sb;
		if ( g_render.world_to_screen( a, sa ) && g_render.world_to_screen( b, sb ) )
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line,
			                                   std::make_any< line_draw_object_t >( line_draw_object_t{ sa, sb, color, 1.5f } ) );
	};

	if ( data.m_viz_n >= 2 ) {
		line( g_ctx.m_local->get_abs_origin( ), data.m_viz[ 1 ] );
		for ( int i = 1; i + 1 < data.m_viz_n; i++ )
			line( data.m_viz[ i ], data.m_viz[ i + 1 ] );
	}

	c_vector centre = data.m_viz_land;
	c_vector n      = data.m_viz_normal.length( ) > 0.5f ? data.m_viz_normal.normalized( ) : c_vector( 0.f, 0.f, 1.f );
	{
		c_trace_filter filter( g_ctx.m_local );
		ray_t ray( c_vector( centre.m_x, centre.m_y, centre.m_z + 2.f ), c_vector( centre.m_x, centre.m_y, centre.m_z - 64.f ) );
		trace_t tr;
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		if ( tr.did_hit( ) && !tr.m_start_solid ) {
			n      = tr.m_plane.m_normal;
			centre = tr.m_end + n * 0.5f;
		}
	}

	constexpr int k_pts   = 24;
	constexpr float k_rad = 16.f;
	const c_vector up     = fabsf( n.m_z ) < 0.99f ? c_vector( 0.f, 0.f, 1.f ) : c_vector( 1.f, 0.f, 0.f );
	const c_vector u      = n.cross_product( up ).normalized( );
	const c_vector v      = n.cross_product( u );
	c_vector ring[ k_pts ];
	for ( int i = 0; i < k_pts; i++ ) {
		const float a = 6.28318530718f * static_cast< float >( i ) / static_cast< float >( k_pts );
		ring[ i ]     = centre + u * ( cosf( a ) * k_rad ) + v * ( sinf( a ) * k_rad );
	}

	// one convex poly, never a centre fan ( imgui AA spokes ). skipped when a vertex is off screen
	poly_draw_object_t poly{ };
	poly.m_color   = col.get_u32( 0.25f );
	bool on_screen = true;
	for ( int i = 0; i < k_pts && on_screen; i++ )
		on_screen = g_render.world_to_screen( ring[ i ], poly.m_points[ i ] );
	if ( on_screen ) {
		poly.m_count = k_pts;
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_poly, std::make_any< poly_draw_object_t >( poly ) );
	}
	for ( int i = 0; i < k_pts; i++ )
		line( ring[ i ], ring[ ( i + 1 ) % k_pts ] );

	// arrow: bounce direction = last flight segment clipped onto the face ( ClipVelocity ), ring colour
	if ( data.m_viz_n >= 2 ) {
		const c_vector in = data.m_viz[ data.m_viz_n - 1 ] - data.m_viz[ data.m_viz_n - 2 ];
		c_vector dir      = in - n * in.dot_product( n );
		if ( dir.length( ) > 0.01f ) {
			dir                 = dir.normalized( );
			const c_vector side = n.cross_product( dir );
			const c_vector tip  = centre + dir * ( k_rad + 12.f );
			line( centre + dir * k_rad, tip );
			line( tip, tip - dir * 6.f + side * 4.f );
			line( tip, tip - dir * 6.f - side * 4.f );
		}
	}
}
