#pragma once
#include "movement.h"
#include "tick_scale.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

extern void start_movement_fix( c_user_cmd* cmd );
extern void end_movement_fix( c_user_cmd* cmd );

namespace n_assist
{
	enum e_launch : int {
		launch_jump = 0,
		launch_minijump,
		launch_longjump,
		launch_crouch_hop,
		launch_mini_crouch_hop,
		launch_jumpbug,
		launch_count
	};
	inline const char* const k_launch_names[ launch_count ] = { "jump", "mj", "lj", "crouch jump", "crouch jb", "jb" };

	struct program_t {
		e_launch launch = launch_jump;
		bool crouch     = false;
	};

	inline int buttons( const program_t& p, const int i, const bool ground, const int air, const int jb_tick, const int lj_hold )
	{
		if ( air < 0 ) {
			if ( p.launch == launch_jumpbug )
				return i == jb_tick ? in_duck : in_jump;
			if ( !ground )
				return p.launch == launch_crouch_hop || p.launch == launch_mini_crouch_hop ? in_duck : 0;
			return p.launch == launch_jump || p.launch == launch_mini_crouch_hop ? in_jump : in_jump | in_duck;
		}
		const bool own        = ( p.launch == launch_longjump && air < lj_hold ) || p.launch == launch_mini_crouch_hop;
		const int crouch_from = p.launch == launch_minijump ? 2 : 1;
		return own || ( p.crouch && air >= crouch_from ) ? in_duck : 0;
	}

	inline constexpr program_t k_programs[ ] = {
		{ launch_jump, false },       { launch_jump, true },       { launch_minijump, false },   { launch_minijump, true },
		{ launch_longjump, false },   { launch_longjump, true },   { launch_crouch_hop, false }, { launch_crouch_hop, true },
		{ launch_mini_crouch_hop, true }, { launch_jumpbug, false }, { launch_jumpbug, true },
	};

	struct flag_t {
		bool points_check_t::*flag;
		program_t program;
		const char* bind;
	};
	inline constexpr flag_t k_flags[ ] = {
		{ &points_check_t::jump, { launch_jump, false }, "j" },
		{ &points_check_t::c_jump, { launch_jump, true }, "j" },
		{ &points_check_t::minijump, { launch_minijump, false }, "mj" },
		{ &points_check_t::c_minijump, { launch_minijump, true }, "mj" },
		{ &points_check_t::longjump, { launch_longjump, false }, "lj" },
		{ &points_check_t::c_longjump, { launch_longjump, true }, "lj" },
		{ &points_check_t::crouch_hop, { launch_crouch_hop, false }, "ch" },
		{ &points_check_t::high_crouch_jump, { launch_crouch_hop, false }, "hcj" },
		{ &points_check_t::c_crouch_hop, { launch_crouch_hop, true }, "ch" },
		{ &points_check_t::c_high_crouch_jump, { launch_crouch_hop, true }, "hcj" },
		{ &points_check_t::c_mini_crouch_hop, { launch_mini_crouch_hop, true }, "mch" },
		{ &points_check_t::jumpbug, { launch_jumpbug, false }, "jb" },
		{ &points_check_t::c_jumpbug, { launch_jumpbug, true }, "jb" },
	};

	inline bool same( const program_t& a, const program_t& b ) { return a.launch == b.launch && a.crouch == b.crouch; }

	inline int settle_air( const program_t& p, const int lj_hold ) { return p.launch == launch_longjump ? lj_hold : 2; }

	enum e_arrival : int { arrival_surf = 0, arrival_bang };

	struct target_t {
		float z     = 0.f;
		float plane = 0.f;
		bool grid   = false;
		int point   = -1;
	};

	inline target_t make_target( const float z, const int point )
	{
		target_t t;
		t.z          = z;
		t.point      = point;
		t.plane      = std::floor( z );
		const float frac = z - t.plane;
		t.grid       = frac > 0.f && frac <= 0.03125f + 0.0005f;
		return t;
	}

	struct sample_t {
		double z_in = 0.0, vz_in = 0.0, z = 0.0, vz = 0.0;
		float hull_in = 72.f, hull = 72.f;
		bool ground  = false;
		bool clipped = false;
	};

	inline bool in_window( const target_t& t, const float z )
	{
		return t.grid ? z > t.plane && z <= t.plane + 0.03125f : check( z, t.z );
	}

	inline bool arrives( const e_arrival kind, const target_t& t, const sample_t& s, const double half_g )
	{
		if ( s.ground )
			return false;
		if ( kind == arrival_surf ) {
			if ( s.vz - half_g >= 0.0 )
				return false;
			return in_window( t, static_cast< float >( s.z ) );
		}
		if ( s.vz_in <= 0.0 || s.hull_in != s.hull )
			return false;
		if ( s.z_in + s.hull_in <= t.z + 0.001 && s.z + s.hull > t.z )
			return true;
		return s.clipped && std::fabs( s.z + s.hull - t.z ) < 0.05;
	}

	inline float near_gap( const e_arrival kind, const target_t& t, const sample_t& s, const double half_g )
	{
		if ( s.ground )
			return 1e9f;
		if ( kind == arrival_surf )
			return s.vz - half_g < 0.0 ? static_cast< float >( std::fabs( s.z - t.z ) ) : 1e9f;
		return s.vz_in > 0.0 ? static_cast< float >( std::fabs( s.z + s.hull - t.z ) ) : 1e9f;
	}

	inline bool past( const e_arrival kind, const double lowest, const sample_t& s )
	{
		return kind == arrival_surf ? s.vz < 0.0 && s.z < lowest - 1.0 : s.vz < 0.0;
	}

	struct ctx_t {
		e_arrival kind = arrival_surf;
		int frame      = 0;
		int buttons    = 0;
		float fwd = 0.f, side = 0.f;
		float fwd0 = 0.f, side0 = 0.f;
		c_angle view{ };
		c_angle delta{ };
		int pre_ticks = 64;  /* the launch must happen inside this */
		int air_ticks = 256;
		int lj_hold   = 3;
		int jb_tick   = -1;
		double lowest      = 0.0;
		double prune_below = 96.0;
		float gravity      = 800.f;
		n_tick::c_sim_budget* budget = nullptr;
		int sims     = 0;
		int max_sims = 512;
		bool out_of_budget = false;
	};

	struct tick_t {
		int buttons = 0;
		float z     = 0.f;
		bool ground = false;
	};

	struct run_t {
		bool hit = false, exact = false, complete = true;
		int target  = -1;
		int arrival = -1, launch = -1, landing = -1;
		float arrive_z  = 0.f;
		float gap       = 1e9f;
		float end_z     = 0.f;
		bool end_ground = false;
		int lip          = -1; /* hit: 1 = the arrival tick pinned in a sim, -1 = no face in reach ( not probed ) */
		int nolip        = -1; /* first window hit nothing pinned: its arrival tick, target, z */
		int nolip_target = -1;
		float nolip_z    = 0.f;
		std::vector< tick_t > ticks;
	};

	inline bool big_turn( const float a, const float b )
	{
		float d = std::fabs( a - b );
		if ( d > 180.f )
			d = 360.f - d;
		return d >= 90.f;
	}

	inline run_t run( ctx_t& c, const program_t& p, const std::vector< target_t >& targets, const bool full )
	{
		run_t r;
		auto* local = g_ctx.m_local;
		auto* cmd   = g_ctx.m_cmd;
		auto* col   = local ? local->get_collideable( ) : nullptr;
		if ( !local || !cmd || !col )
			return r;

		g_prediction.restore_entity_to_predicted_frame( c.frame );
		const int tick_base  = local->get_tick_base( );
		const float frametime = n_tick::engine_interval( );
		const double dt      = frametime;
		const double h     = static_cast< double >( 1.f * c.gravity ) * 0.5 * static_cast< double >( frametime );
		const double h_end = static_cast< double >( 1.f * c.gravity * frametime ) * 0.5;
		const double pin   = -h_end;
		c_angle view        = c.view;
		int air             = -1;
		bool free_prev      = false;
		unsigned dead       = 0u;
		r.ticks.reserve( 128 );

		const auto aim = [ & ]( const int i, const bool ground_in, c_angle& v ) {
			cmd->m_forward_move = i == 0 ? c.fwd0 : c.fwd;
			cmd->m_side_move    = i == 0 ? c.side0 : c.side;
			cmd->m_view_point   = c.view;
			if ( !ground_in && ( c.delta.m_x != 0.f || c.delta.m_y != 0.f ) ) {
				if ( !big_turn( v.m_y, c.view.m_y ) )
					v = ( v + c.delta ).normalize( ).clamp( );
				cmd->m_view_point = v;
				start_movement_fix( cmd );
				cmd->m_view_point = c.view;
				end_movement_fix( cmd );
			}
		};

		/* 10-09 log: 13 of 33 plans hit the window on a face with no lip under that xy ( pin 0, wall 0.000 ). sim the arrival tick in
		   its stance: your keys, auto align's pushes into the face ( 10..90 ), the ride hold's 45 / 450. none pins = no lip there,
		   state goes back to the arrival and the arc falls on to lower points */
		const auto lip = [ & ]( const int i, const int press ) -> int {
			const c_vector o    = local->get_origin( );
			const c_vector mins = col->get_obb_mins( ), maxs = col->get_obb_maxs( );
			c_trace_filter fil( local );
			float best = 1.f;
			c_vector n{ };
			for ( int d = 0; d < 8; ++d ) {
				const float a = static_cast< float >( d ) * 0.78539816f;
				trace_t tr;
				ray_t ray( o, c_vector( o.m_x + 0.1f * std::cos( a ), o.m_y + 0.1f * std::sin( a ), o.m_z ), mins, maxs );
				g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &tr );
				if ( !tr.m_start_solid && tr.m_fraction < best && std::fabs( tr.m_plane.m_normal.m_z ) < 0.7f ) {
					best = tr.m_fraction;
					n    = tr.m_plane.m_normal;
				}
			}
			if ( best >= 1.f || !g_prediction.snapshot_save( 0 ) )
				return -1;
			const float rot             = deg2rad( c_vector( -n.m_x, -n.m_y, 0.f ).to_angle2( ).m_y - c.view.m_y );
			constexpr float k_presses[ ] = { 0.f, 10.f, 20.f, 30.f, 40.f, 45.f, 50.f, 60.f, 70.f, 80.f, 90.f, 450.f };
			for ( const float m : k_presses ) {
				if ( m > 0.f ) {
					g_prediction.restore_entity_to_predicted_frame( c.frame );
					g_prediction.snapshot_load( 0 );
					cmd->m_forward_move = std::cos( rot ) * m;
					cmd->m_side_move    = -std::sin( rot ) * m;
					cmd->m_view_point   = c.view;
				} else {
					c_angle next_view = view;
					aim( i + 1, false, next_view );
				}
				cmd->m_buttons          = ( c.buttons & ~in_duck ) | ( press & in_duck );
				local->get_tick_base( ) = tick_base + i + 1;
				g_prediction.begin( local, cmd );
				g_prediction.end( local );
				++c.sims;
				const c_vector v = local->get_velocity( );
				if ( !( local->get_flags( ) & fl_onground ) && std::fabs( v.m_z - pin ) < 0.01 && v.length_2d( ) >= 1.f )
					return 1;
			}
			g_prediction.restore_entity_to_predicted_frame( c.frame );
			g_prediction.snapshot_load( 0 );
			return 0;
		};

		for ( int i = 0; i < c.pre_ticks + c.air_ticks; ++i ) {
			if ( air < 0 && i >= c.pre_ticks )
				break;
			if ( ( c.budget && c.budget->expired( ) ) || c.sims >= c.max_sims ) {
				r.complete = false;
				c.out_of_budget = true;
				break;
			}

			const bool ground_in = ( local->get_flags( ) & fl_onground ) != 0;
			aim( i, ground_in, view );

			const int press = buttons( p, i, ground_in, air, c.jb_tick, c.lj_hold );
			cmd->m_buttons  = ( c.buttons & ~( in_jump | in_duck ) ) | press;

			tick_t rec;
			rec.buttons = press;
			rec.z       = local->get_origin( ).m_z;
			rec.ground  = ground_in;
			r.ticks.push_back( rec );

			sample_t s;
			s.z_in               = local->get_origin( ).m_z;
			s.vz_in              = local->get_velocity( ).m_z;
			s.hull_in            = col->get_obb_maxs( ).m_z;
			const bool ducked_in = ( local->get_flags( ) & fl_ducking ) != 0;

			// curtime must move: the 0.4s re-duck gate is a curtime test, a probe begin( ) never advances the
			// tickbase. restore_entity_to_predicted_frame puts it back
			local->get_tick_base( ) = tick_base + i;
			g_prediction.begin( local, cmd );
			g_prediction.end( local );
			++c.sims;

			s.z      = local->get_origin( ).m_z;
			s.vz     = local->get_velocity( ).m_z;
			s.hull   = col->get_obb_maxs( ).m_z;
			s.ground = ( local->get_flags( ) & fl_onground ) != 0;

			if ( air < 0 ) {
				if ( !s.ground && s.vz > 0.0 && s.vz > s.vz_in + 100.0 ) {
					air      = 0;
					r.launch = i;
				} else {
					if ( s.ground && !ground_in && r.landing < 0 )
						r.landing = i;
					if ( s.ground && ground_in )
						break;
					if ( c.kind == arrival_surf && !s.ground && s.vz < 0.0 && s.z < c.lowest - c.prune_below )
						break;
					continue;
				}
			} else
				++air;

			if ( targets.empty( ) )
				break;

			s.clipped = s.vz_in > 0.0 && std::fabs( s.vz - pin ) < 0.01;
			for ( int t = 0; t < static_cast< int >( targets.size( ) ); ++t ) {
				if ( dead >> t & 1u )
					continue;
				r.gap = ( std::min )( r.gap, near_gap( c.kind, targets[ t ], s, h ) );
				if ( arrives( c.kind, targets[ t ], s, h ) ) {
					r.lip = c.kind == arrival_surf ? lip( i, press ) : -1;
					if ( r.lip == 0 ) {
						dead |= 1u << t;
						if ( r.nolip < 0 ) {
							r.nolip        = i;
							r.nolip_target = t;
							r.nolip_z      = static_cast< float >( s.z );
						}
						continue;
					}
					r.hit      = true;
					r.exact    = true;
					r.target   = t;
					r.arrival  = i;
					r.arrive_z = static_cast< float >( s.z );
					return r;
				}
			}
			if ( s.ground || past( c.kind, c.lowest, s ) )
				break;
			if ( full )
				continue;

			const double dv      = s.vz_in - s.vz;
			const bool ducked    = ( local->get_flags( ) & fl_ducking ) != 0;
			const bool wants     = ( buttons( p, i + 1, false, air + 1, c.jb_tick, c.lj_hold ) & in_duck ) != 0;
			const bool free_step = s.hull == s.hull_in && ducked == ducked_in && std::fabs( dv - ( h + h_end ) ) < 1e-3 &&
			                       std::fabs( s.z - ( s.z_in + ( s.vz_in - h ) * dt ) ) < 1e-3;
			const bool settled   = free_step && free_prev && air >= settle_air( p, c.lj_hold ) && ducked == wants;
			free_prev            = free_step;
			if ( !settled )
				continue;

			float zf        = static_cast< float >( s.z ), vf = static_cast< float >( s.vz );
			const float off = s.hull * 0.5f;
			for ( int j = i + 1; j < c.pre_ticks + c.air_ticks; ++j ) {
				sample_t f;
				f.z_in    = zf;
				f.vz_in   = vf;
				f.hull_in = f.hull = s.hull;
				vf                 = static_cast< float >( static_cast< double >( vf ) - h );
				const float end    = zf + frametime * vf;
				const float delta  = end - zf;
				const float start  = ( zf + off ) - off;
				zf                 = start + delta;
				vf                 = static_cast< float >( static_cast< double >( vf ) - h_end );
				f.z                = zf;
				f.vz               = vf;
				for ( int t = 0; t < static_cast< int >( targets.size( ) ); ++t ) {
					if ( dead >> t & 1u )
						continue;
					r.gap = ( std::min )( r.gap, near_gap( c.kind, targets[ t ], f, h ) );
					if ( arrives( c.kind, targets[ t ], f, h ) ) {
						r.hit      = true;
						r.lip      = -1;
						r.target   = t;
						r.arrival  = j;
						r.arrive_z = zf;
						return r;
					}
				}
				if ( past( c.kind, c.lowest, f ) )
					break;
			}
			break;
		}
		r.end_z      = local->get_origin( ).m_z;
		r.end_ground = ( local->get_flags( ) & fl_onground ) != 0;
		return r;
	}

	struct plan_t {
		bool active = false;
		program_t program{ };
		int start_tick = 0;
		int launch = -1, arrival = -1;
		int point  = -1;
		float target_z = 0.f, arrive_z = 0.f;
		int unpinned   = 0;
		std::vector< tick_t > ticks;

		void clear( )
		{
			active   = false;
			unpinned = 0;
			ticks.clear( );
		}

		void take( const program_t& p, run_t& r, const int tick_count, const int cand, const float target )
		{
			active     = true;
			program    = p;
			start_tick = tick_count;
			launch     = r.launch;
			arrival    = r.arrival;
			point      = cand;
			target_z   = target;
			arrive_z   = r.arrive_z;
			unpinned   = 0;
			ticks      = std::move( r.ticks );
		}
	};

	inline float start_dz( const plan_t& plan, const int k )
	{
		return g_ctx.m_local->get_origin( ).m_z - plan.ticks[ k ].z;
	}

	inline bool on_track( const plan_t& plan, const int k )
	{
		if ( !g_ctx.m_local || k < 0 || k >= static_cast< int >( plan.ticks.size( ) ) )
			return false;
		const bool ground = ( g_ctx.m_local->get_flags( ) & fl_onground ) != 0;
		return ground == plan.ticks[ k ].ground && std::fabs( start_dz( plan, k ) ) < 0.25f;
	}

	inline void set_move( c_user_cmd* cmd, const float fwd, const float side )
	{
		cmd->m_forward_move = fwd;
		cmd->m_side_move    = side;
		cmd->m_buttons &= ~( in_forward | in_back | in_moveleft | in_moveright );
		if ( fwd > 0.f )
			cmd->m_buttons |= in_forward;
		else if ( fwd < 0.f )
			cmd->m_buttons |= in_back;
		if ( side > 0.f )
			cmd->m_buttons |= in_moveright;
		else if ( side < 0.f )
			cmd->m_buttons |= in_moveleft;
	}
}
