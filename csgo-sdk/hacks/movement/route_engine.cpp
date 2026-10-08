#include "movement.h"
#include "edgebug.h"
#include "tick_scale.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

extern void botox_dbg_log( const char* fmt, ... );

const char* route_point_type_name( int type )
{
	switch ( type ) {
	case route_pt_pixelsurf: return "pixelsurf";
	case route_pt_edgebug: return "edgebug";
	case route_pt_headbang: return "headbang";
	case route_pt_texturebug: return "texturebug";
	case route_pt_pixeljump: return "pixeljump";
	case route_pt_headbounce: return "headbounce";
	default: return "ground";
	}
}

namespace n_route
{
	const char* const k_style_names[ style_count ] = { "jump", "minijump", "longjump", "jumpbug", "walk off" };

	const char* const k_move_names[ move_count ] = { "jump", "minijump", "longjump", "hop",     "mj hop",
	                                                 "lj hop", "crouch hop", "delay hop", "jumpbug", "walk off" };

	int move_of( int style, int timing, bool ducked_in )
	{
		if ( style == style_walk_off )
			return move_walk_off;
		if ( style == style_jumpbug )
			return move_jumpbug;
		if ( timing == timing_delay )
			return move_delay_hop;
		if ( timing == timing_bhop ) {
			if ( ducked_in )
				return move_crouch_hop;
			return style == style_minijump ? move_mj_hop : ( style == style_longjump ? move_lj_hop : move_hop );
		}
		return style == style_minijump ? move_minijump : ( style == style_longjump ? move_longjump : move_jump );
	}
}

namespace
{
	float convar_float( const char* name, float fallback )
	{
		auto* cv = g_interfaces.m_convar ? g_interfaces.m_convar->find_var( name ) : nullptr;
		return cv ? cv->get_float( ) : fallback;
	}

	struct engine_numbers_t {
		float dt = 0.015625f, grav_tick = 12.5f, half_g = 6.25f;
		float bleed = 0.9375f, stam_max = 80.f, jump_cost = 0.080f, land_cost = 0.050f;
		float duck_gap = 0.4f;
		float max_vel  = 3500.f;
		/* loop caps are DURATIONS, derived per solve from dt ( defaults = 64 tick ). the rest wait
		   must outlast a full stamina bleed ( 80 at 60/s = 1.33 s ) at any tickrate. */
		int air_ticks  = 256;
		int rest_ticks = 128;
		float travel   = n_route::k_route_travel;
	};
	engine_numbers_t g_num{ };

	void refresh_engine_numbers( )
	{
		const float d   = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.015625f;
		g_num.dt        = d > 0.0001f ? d : 0.015625f;
		if ( n_tick::forced_128( ) && g_num.dt > 1.f / 128.f )
			g_num.dt = 1.f / 128.f;
		g_num.grav_tick = convar_float( "sv_gravity", 800.f ) * g_num.dt;
		g_num.half_g    = 0.5f * g_num.grav_tick;
		g_num.bleed     = convar_float( "sv_staminarecoveryrate", 60.f ) * g_num.dt;
		g_num.stam_max  = convar_float( "sv_staminamax", 80.f );
		g_num.jump_cost = convar_float( "sv_staminajumpcost", 0.080f );
		g_num.land_cost = convar_float( "sv_staminalandcost", 0.050f );
		g_num.duck_gap  = convar_float( "sv_timebetweenducks", 0.4f );
		g_num.max_vel   = convar_float( "sv_maxvelocity", 3500.f );

		const auto secs = [ ]( float s ) {
			const int n = static_cast< int >( s / g_num.dt );
			return n < 1 ? 1 : n;
		};
		g_num.air_ticks  = secs( 4.f );
		g_num.rest_ticks = secs( 2.f );
		g_num.travel     = n_route::k_route_travel;
	}

	float tick_dt( ) { return g_num.dt; }
	int air_tick_cap( ) { return g_num.air_ticks; }
	int rest_tick_cap( ) { return g_num.rest_ticks; }
	float gravity_per_tick( ) { return g_num.grav_tick; }
	float half_gravity_per_tick( ) { return g_num.half_g; }
	float stamina_max( ) { return g_num.stam_max; }
	float stamina_jump_cost( ) { return g_num.jump_cost; }
	float stamina_land_cost( ) { return g_num.land_cost; }
	float stamina_per_tick( ) { return g_num.bleed; }

	float g_impulse = 301.993377f;

	constexpr float k_duck_delta = 9.f;
	constexpr float k_hull_stand = 72.f;
	constexpr float k_hull_duck  = 54.f;
	float hull_top( const n_route::sim_t& s ) { return s.ducked ? k_hull_duck : k_hull_stand; }
	constexpr float k_dist_eps = 0.03125f;
	constexpr int k_tb_max_catches = 6;
	constexpr float k_duck_speed = 8.f;
	constexpr float k_duck_penalty = 2.f;
	constexpr float k_duck_min     = 1.5f;
	constexpr float k_duck_recover      = 3.f;
	constexpr float k_duck_recover_move = 6.f;
	constexpr float k_duck_recover_dist = 64.f;
	constexpr float k_ground_catch = 2.f;
	// NON_JUMP_VELOCITY: going up faster than this is never grounded
	constexpr float k_non_jump_velocity = 140.f;
	constexpr float k_step_size = 18.f;
	constexpr int k_pj_creep_ticks = 3;

	float approach( float target, float value, float speed )
	{
		const float d = target - value;
		if ( d > speed )
			return value + speed;
		if ( d < -speed )
			return value - speed;
		return target;
	}

	float clampf( float v, float lo, float hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }

	void duck_recover( n_route::sim_t& s )
	{
		s.duck_speed = approach( k_duck_speed, s.duck_speed, tick_dt( ) * k_duck_recover );
		if ( s.duck_speed >= k_duck_speed )
			s.full_duck_time = s.time;
		else if ( ( s.duck_amount <= 0.f || s.duck_amount >= 1.f ) &&
		          g_num.travel * ( s.time - s.full_duck_time ) > k_duck_recover_dist )
			s.duck_speed = approach( k_duck_speed, s.duck_speed, tick_dt( ) * k_duck_recover_move );
	}
	float duck_recover_since( const n_route::sim_t& s )
	{
		if ( s.duck_speed >= k_duck_speed || g_num.travel <= 0.f )
			return 0.f;
		return ( std::min )( s.time - s.full_duck_time, k_duck_recover_dist / g_num.travel + tick_dt( ) );
	}

	/* middle hundredth: a LOG label for [rc edge], never the arrival test. a pixel's depth under its
	   marker isn't derivable ( check( ) accepts; [rc surf] measures once ridden ). */
	int pixel_bucket( float v )
	{
		const int unit = static_cast< int >( v );
		return static_cast< int >( ( v - unit ) * 100.f );
	}
	bool check_centre( float a, float target )
	{
		if ( !check( a, target ) )
			return false;
		if ( static_cast< int >( a ) != static_cast< int >( target ) )
			return false;
		return pixel_bucket( a ) == pixel_bucket( target ) - 1;
	}
	bool pixel_plane( float marker, float& plane )
	{
		const float f    = std::floor( marker );
		const float frac = marker - f;
		if ( frac <= 0.f || frac > k_dist_eps + 0.0005f )
			return false;
		plane = f;
		return true;
	}
	bool in_pixel_window( float z, float plane ) { return z > plane && z <= plane + k_dist_eps; }
	constexpr float k_w_lo_stand = 0.0203f;
	constexpr float k_w_lo_duck  = 0.0105f;
	float pixel_w_lo( float plane, bool ducked ) { return plane + ( ducked ? k_w_lo_duck : k_w_lo_stand ); }
	float pixel_centre_gap( float z, float plane, bool ducked )
	{
		return std::fabs( z - 0.5f * ( pixel_w_lo( plane, ducked ) + plane + k_dist_eps ) );
	}
	float pixel_window_gap( float z, float plane )
	{
		if ( z <= plane )
			return plane - z;
		return z > plane + k_dist_eps ? z - plane - k_dist_eps : 0.f;
	}
	int pixel_tier( float z, float plane, bool ducked )
	{
		const float lo = pixel_w_lo( plane, ducked );
		if ( z < lo )
			return 2;
		return ( std::min )( z - lo, plane + k_dist_eps - z ) >= 0.001f ? 0 : 1;
	}
	void sim_categorize( n_route::sim_t& s, float floor_z, bool solid, bool move_to_end, bool skin = false )
	{
		const float over = s.z - floor_z;
		if ( solid && skin && move_to_end ) {
			s.on_ground = false;
			return;
		}
		if ( !solid || s.vz > k_non_jump_velocity ||
		     ( skin ? over <= n_route::k_skin_lo || over >= n_route::k_skin_hi : over > k_ground_catch - k_dist_eps ) )
			return;
		s.on_ground = true;
		s.vz = 0.f;
		if ( move_to_end && s.z - floor_z <= k_step_size )
			s.z = floor_z;
	}

	bool duck_allowed( const n_route::sim_t& s )
	{
		if ( s.duck_speed < k_duck_min )
			return false;
		if ( !s.fl_ducking && s.time < s.last_duck_time + g_num.duck_gap )
			return false;
		return true;
	}

	/* would a duck pressed now go in? press pays its -2 first, then DuckingEnabled ( CheckParameters
	   order ). a refused-duck minijump is a plain jump; a +9 catch that never ducks falls past. */
	bool duck_press_lands( const n_route::sim_t& s )
	{
		n_route::sim_t probe = s;
		if ( !probe.raw_duck )
			probe.duck_speed = ( std::max )( 0.f, probe.duck_speed - k_duck_penalty );
		return duck_allowed( probe );
	}

	void sim_duck( n_route::sim_t& s, bool held, float floor_z, bool solid, bool skin = false )
	{
		if ( !held && s.duck_amount > 0.f )
			s.ducking = true;
		else if ( held && s.duck_amount < 1.f )
			s.ducking = true;

		duck_recover( s );

		if ( held && s.ducking ) {
			s.duck_amount = approach( 1.f, s.duck_amount, tick_dt( ) * s.duck_speed * 0.8f );
			if ( s.duck_amount >= 1.f || !s.on_ground ) {
				if ( !s.on_ground )
					s.z += k_duck_delta;
				s.ducked         = true;
				s.ducking        = false;
				s.duck_amount    = 1.f;
				s.fl_ducking     = true;
				s.last_duck_time = s.time;
				sim_categorize( s, floor_z, solid, false, skin );
			}
		}
		const float over9         = s.z - k_duck_delta - floor_z;
		const bool floor_blocks   = solid && ( skin ? over9 > -k_dist_eps && over9 < n_route::k_skin_hi - 2.f : over9 < 0.f );
		const bool ceiling_blocks = s.z + k_hull_stand > s.ceiling + k_dist_eps;
		if ( !held && s.ducking && !s.on_ground && ( floor_blocks || ceiling_blocks ) ) {
			s.duck_amount = 1.f;
			s.ducked      = true;
			s.ducking     = false;
			s.fl_ducking  = true;
		} else if ( !held && s.ducking ) {
			s.duck_amount = approach( 0.f, s.duck_amount, tick_dt( ) * ( s.duck_speed > 1.5f ? s.duck_speed : 1.5f ) );
			s.ducked      = false;
			if ( s.duck_amount <= 0.f || !s.on_ground ) {
				if ( !s.on_ground )
					s.z -= k_duck_delta;
				s.ducked      = false;
				s.ducking     = false;
				s.duck_amount = 0.f;
				s.fl_ducking  = false;
				sim_categorize( s, floor_z, solid, false, skin );
			} else if ( s.duck_amount <= 0.75f ) {
				s.fl_ducking = false;
			}
		}
	}

	void sim_check_jump( n_route::sim_t& s )
	{
		if ( !s.on_ground )
			return;
		s.on_ground        = false;
		const float startz = s.vz;
		if ( s.ducking || s.ducked || s.on_player )
			s.vz = g_impulse;
		else
			s.vz += g_impulse;
		s.on_player = false;
		if ( s.stamina > 0.f )
			s.vz *= clampf( 1.f - s.stamina / 100.f, 0.f, 1.f );
		s.vz -= half_gravity_per_tick( );
		s.stamina = ( std::min )( s.stamina + stamina_jump_cost( ) * ( s.vz - startz ), stamina_max( ) );
	}

	void sim_tick( n_route::sim_t& s, bool jump, bool duck_held, float floor_z, bool solid, bool skin = false )
	{
		const float half_g = half_gravity_per_tick( );

		if ( duck_held != s.raw_duck )
			s.duck_speed = ( std::max )( 0.f, s.duck_speed - k_duck_penalty );
		s.raw_duck = duck_held;
		if ( !duck_allowed( s ) )
			duck_held = false;

		bool ground_entity = s.on_ground;

		s.stamina = ( std::max )( 0.f, s.stamina - stamina_per_tick( ) );
		if ( !s.on_ground )
			s.fall = -s.vz;

		sim_duck( s, duck_held, floor_z, solid, skin );

		s.vz -= half_g;
		if ( jump ) {
			sim_check_jump( s );
			if ( !s.on_ground )
				ground_entity = false; // SetGroundEntity(NULL), a launch tick never snaps
		}

		if ( s.on_ground ) {
			s.vz   = 0.f;
			s.fall = 0.f;
		} else {
			const float head_pre = s.z + hull_top( s );
			s.z += s.vz * tick_dt( );
			if ( s.vz > 0.f && head_pre <= s.roof + k_dist_eps && s.z + hull_top( s ) > s.roof ) {
				s.z        = s.roof - hull_top( s );
				s.vz       = 0.f;
				s.roof_hit = true;
				s.ceiling  = ( std::min )( s.ceiling, s.roof );
			}
			if ( solid && !skin && s.vz < 0.f && s.z < floor_z )
				s.z = floor_z;
		}

		sim_categorize( s, floor_z, solid, ground_entity, skin );

		s.vz -= half_g;
		if ( s.vz < -g_num.max_vel )
			s.vz = -g_num.max_vel;
		s.time += tick_dt( );
		if ( !s.on_ground )
			s.on_player = false;
		else
			s.ceiling = 3.4e38f;
		if ( s.on_ground ) {
			s.vz = 0.f;
			if ( s.fall > 0.f ) {
				const float before = s.stamina;
				s.stamina          = ( std::min )( s.stamina + stamina_land_cost( ) * s.fall, stamina_max( ) );
				s.land_cost        = s.stamina - before;
				s.fall             = 0.f;
			}
		}
	}

	bool jumpbug_band( const n_route::sim_t& s_in, float floor_z, n_route::sim_t& out, bool skin = false )
	{
		if ( s_in.ducked || s_in.ducking || s_in.on_ground )
			return false;
		n_route::sim_t band = s_in;
		sim_tick( band, false, true, floor_z, true, skin );
		if ( !band.ducked || band.on_ground )
			return false;
		n_route::sim_t probe = band;
		sim_tick( probe, true, false, floor_z, true, skin );
		if ( probe.on_ground || probe.vz <= 0.f )
			return false;
		out = band;
		return true;
	}

	struct leg_t {
		std::string name{ };
		int type       = route_pt_ground;
		float dep_z    = 0.f;
		float dep_stam = 0.f;
		float apex     = 0.f;
		float arrive_z = 0.f;
		float fall     = 0.f;
		float hover    = 0.f;
		float gap      = 0.f;
		int tier       = 0;
		float stam_out = 0.f;
	};

	struct alt_t {
		std::vector< std::string > prefix{ };
		std::size_t len = 0;
		std::string text{ };
	};
	constexpr std::size_t k_alt_cap = 512;

	struct node_t {
		n_route::sim_t st{ };
		std::vector< std::string > elements{ };
		std::shared_ptr< const std::vector< alt_t > > alts{ };
		std::vector< leg_t > legs{ };
		int cost = 0;
		float floor       = 0.f;
		unsigned int mask = n_route::k_all_styles;
		long long paths = 1;
		bool jb_ok = false;
		n_route::sim_t jb{ };
		bool hop_only = false;
		bool skin = false;
		bool crouch_only = false;
		int crouches = 0;
		/* how hard to play: easiest = fewest binds, then fewest bind switches ( bind_of ). whole-sequence
		   properties; the dedup key holds bind_mask + last_bind so a merge never keeps the harder twin. */
		unsigned int bind_mask = 0u;
		int last_bind          = -1;
		int switches           = 0;
		int presses            = 0;
	};

	int bind_of( int style, bool from_ground )
	{
		if ( !from_ground || style == n_route::style_walk_off )
			return -1;
		return style;
	}
	int bind_count( const node_t& n )
	{
		int c = 0;
		for ( unsigned int m = n.bind_mask; m; m &= m - 1 )
			++c;
		return c;
	}
	bool easier( const node_t& a, const node_t& b )
	{
		if ( a.crouches != b.crouches )
			return a.crouches < b.crouches;
		const int ba = bind_count( a ), bb = bind_count( b );
		if ( ba != bb )
			return ba < bb;
		if ( a.switches != b.switches )
			return a.switches < b.switches;
		if ( a.presses != b.presses )
			return a.presses < b.presses;
		return a.cost < b.cost;
	}

	void merge_alts( node_t& keep, const node_t& drop, long long& over_cap )
	{
		const std::size_t len       = keep.elements.size( );
		const std::string keep_text = n_route::route_text( keep.elements );
		std::vector< alt_t > add;
		const auto offer = [ & ]( std::vector< std::string > prefix ) {
			std::string text = n_route::route_text( prefix );
			if ( text == keep_text )
				return;
			const auto same = [ & ]( const alt_t& a ) { return a.len == len && a.text == text; };
			if ( std::any_of( add.begin( ), add.end( ), same ) || ( keep.alts && std::any_of( keep.alts->begin( ), keep.alts->end( ), same ) ) )
				return;
			if ( add.size( ) + ( keep.alts ? keep.alts->size( ) : 0 ) >= k_alt_cap ) {
				++over_cap;
				return;
			}
			add.push_back( { std::move( prefix ), len, std::move( text ) } );
		};
		offer( drop.elements );
		if ( drop.alts )
			for ( const alt_t& a : *drop.alts ) {
				std::vector< std::string > prefix = a.prefix;
				prefix.insert( prefix.end( ), drop.elements.begin( ) + static_cast< std::ptrdiff_t >( a.len ), drop.elements.end( ) );
				offer( std::move( prefix ) );
			}
		if ( add.empty( ) )
			return;
		auto merged = std::make_shared< std::vector< alt_t > >( );
		if ( keep.alts )
			*merged = *keep.alts;
		merged->insert( merged->end( ), std::make_move_iterator( add.begin( ) ), std::make_move_iterator( add.end( ) ) );
		keep.alts = std::move( merged );
	}

	constexpr std::size_t k_beam       = 1u << 18;
	constexpr std::size_t k_beam_plain = 1u << 17;
	int timing_cost( int style, int timing, int delay_ticks )
	{
		if ( style == n_route::style_jumpbug )
			return 3;
		if ( style == n_route::style_walk_off )
			return 0;
		switch ( timing ) {
		case n_route::timing_bhop: return 1;
		case n_route::timing_delay: return 1 + delay_ticks;
		default: return 0;
		}
	}

	std::string departure_name( int style, int timing, int delay_ticks, bool ducked_in, bool hold_crouch )
	{
		std::string name;
		if ( timing == n_route::timing_delay ) {
			char buf[ 24 ]{ };
			sprintf_s( buf, "delay hop %dt", delay_ticks );
			name = buf;
		} else if ( hold_crouch ) {
			name = style == n_route::style_walk_off ? "walk off (ducked)" : "jump (ducked)";
		} else if ( style == n_route::style_jump && ducked_in ) {
			name = "jump (release duck)";
		} else if ( style == n_route::style_longjump && ducked_in ) {
			name = "lj hop (release duck)";
		} else {
			switch ( const int move = n_route::move_of( style, timing, ducked_in ) ) {
			case n_route::move_jump:
			case n_route::move_hop: name = "jump (stand)"; break;
			default: name = n_route::k_move_names[ move ]; break;
			}
		}
		return name;
	}

	std::string arrival_name( int type, bool ducked )
	{
		const std::string stance = ducked ? " (ducked)" : " (stand)";
		if ( type == route_pt_pixelsurf )
			return "pixelsurf" + stance;
		if ( type == route_pt_edgebug )
			return "edgebug" + stance;
		if ( type == route_pt_headbang )
			return "headbang" + stance;
		if ( type == route_pt_texturebug )
			return "texturebug" + stance;
		if ( type == route_pt_pixeljump )
			return "pixeljump" + stance;
		if ( type == route_pt_headbounce )
			return "headbounce" + stance;
		return std::string( );
	}

	/* dedup key: nodes in the same state fly the same arcs. quantum must be far finer than a pixel
	   window ( 1/4 stamina = ~0.28u of apex, too coarse ). node_key adds jumpbug origin + bind history. */
	/* phys_key = PHYSICS half only, so two key histories of one state don't eat two beam slots */
	std::string phys_key( const node_t& n, float floor_z )
	{
		const n_route::sim_t& st = n.st;
		char buf[ 160 ]{ };
		sprintf_s( buf, "%d|%d|%d|%d|%d|%d|%d|%d|%d", static_cast< int >( st.stamina * 256.f ),
		           static_cast< int >( st.vz * 64.f ), static_cast< int >( ( st.z - floor_z ) * 1024.f ),
		           static_cast< int >( st.duck_amount * 8.f ), st.ducked ? 1 : 0, st.on_ground ? 1 : 0, n.jb_ok ? 1 : 0,
		           n.jb_ok ? static_cast< int >( ( n.jb.z - floor_z ) * 1024.f ) : 0,
		           n.jb_ok ? static_cast< int >( n.jb.stamina * 256.f ) : 0 );
		return buf;
	}
	// float bits: dedup merges exact states only. a merged twin is listed as an alt of the lead, so
	// its future must be the lead's bit for bit. near-only merges (duck gate, low stamina) fly their own arcs
	unsigned int bits( float f )
	{
		unsigned int u = 0u;
		memcpy( &u, &f, sizeof( u ) );
		return u;
	}
	std::string node_key( const node_t& n, float floor_z )
	{
		const n_route::sim_t& st = n.st;
		const float since = ( std::min )( st.time - st.last_duck_time, 1.f );
		char buf[ 208 ]{ };
		/* crouch tier never merges into a crouch-free twin: the crouch-free search stays the pre-crouch one to the node */
		sprintf_s( buf, "|%x|%d|%d|%d|%x|%x|%x|%x|%x|%x|%x|%x|%x|%d%d%d%d%d%d|%x|%x|%x", n.bind_mask, n.last_bind, n.crouch_only ? 1 : 0,
		           n.crouches > 0 ? 1 : 0, bits( st.z ), bits( st.vz ), bits( st.stamina ), bits( st.duck_amount ), bits( st.duck_speed ),
		           bits( since ), bits( duck_recover_since( st ) ), bits( st.fall ), bits( st.ceiling ), st.fl_ducking ? 1 : 0,
		           st.raw_duck ? 1 : 0, st.ducking ? 1 : 0, n.hop_only ? 1 : 0, n.skin ? 1 : 0, st.on_player ? 1 : 0,
		           n.jb_ok ? bits( n.jb.z ) : 0u, n.jb_ok ? bits( n.jb.vz ) : 0u, n.jb_ok ? bits( n.jb.stamina ) : 0u );
		return phys_key( n, floor_z ) + buf;
	}

	std::string beam_key( const node_t& n, float floor_z )
	{
		const n_route::sim_t& st = n.st;
		char buf[ 96 ]{ };
		sprintf_s( buf, "%d|%d|%d|%d|%d", static_cast< int >( st.stamina * 16.f ),
		           static_cast< int >( ( st.z - floor_z ) * 8.f ), st.ducked ? 1 : 0, st.on_ground ? 1 : 0, n.jb_ok ? 1 : 0 );
		return buf;
	}

	bool duck_at_launch( int style ) { return style == n_route::style_minijump || style == n_route::style_longjump; }
	bool duck_held_after( int style ) { return style == n_route::style_longjump; }
	int lj_bind_air_ticks( ) { return n_tick::ticks( 2 ) - 1; }

	struct press_spec_t {
		const char* name = "";
		int style        = n_route::style_jump;
		int timing       = n_route::timing_rest;
		int delay        = 1;
		bool hold_crouch = false;
	};

	struct leg_result_t {
		bool ok         = false;
		const char* why = "ok";
		float dep_z = 0.f, dep_stam = 0.f, apex = 0.f, land_z = 0.f, hover = 0.f, fall = 0.f, stam_out = 0.f;
		bool absolute = false, press_ducked = false, ducked_out = false;
	};

	leg_result_t run_leg( n_route::sim_t& s, const press_spec_t& m, float floor_z )
	{
		using namespace n_route;
		leg_result_t r;
		const float half_g = half_gravity_per_tick( );

		int wait = 0;
		if ( s.on_ground ) {
			if ( m.timing == timing_rest )
				wait = rest_tick_cap( );
			else if ( m.timing == timing_delay )
				wait = m.delay;
		}
		for ( int i = 0; i < wait; ++i ) {
			if ( m.timing == timing_rest && i > 0 && s.stamina <= 0.f && !s.ducking && !s.ducked )
				break;
			sim_tick( s, false, false, floor_z, true );
			if ( !s.on_ground ) {
				r.why = "fell";
				return r;
			}
		}

		if ( m.hold_crouch ) {
			if ( !s.ducked ) {
				if ( !duck_press_lands( s ) ) {
					r.why = "STRIP";
					return r;
				}
				s.duck_speed     = ( std::max )( 0.f, s.duck_speed - k_duck_penalty );
				s.last_duck_time = s.time;
			}
			s.raw_duck    = true;
			s.ducked      = true;
			s.ducking     = false;
			s.duck_amount = 1.f;
			s.fl_ducking  = true;
		}

		r.dep_z        = s.z;
		r.dep_stam     = s.stamina;
		r.press_ducked = s.ducked || s.ducking;

		const bool press_duck = duck_at_launch( m.style ) || m.hold_crouch;
		const bool hold_duck  = duck_held_after( m.style ) || m.hold_crouch;
		if ( press_duck && !r.press_ducked && !duck_press_lands( s ) ) {
			r.why = "STRIP";
			return r;
		}
		r.absolute = r.press_ducked || press_duck;

		if ( m.style == style_walk_off ) {
			if ( s.on_ground ) {
				s.vz        = -half_g;
				s.on_ground = false;
			}
		} else {
			sim_tick( s, true, press_duck, floor_z, true );
			if ( s.on_ground ) {
				r.why = "noair";
				return r;
			}
		}

		float apex = s.z;
		for ( int i = 0, cap = air_tick_cap( ); i < cap; ++i ) {
			bool held = hold_duck;
			if ( held && m.style == style_longjump && !m.hold_crouch && i >= lj_bind_air_ticks( ) )
				held = false;
			const float vz_in = s.vz;
			sim_tick( s, false, held, floor_z, true );
			if ( s.z > apex )
				apex = s.z;
			if ( s.on_ground ) {
				r.ok     = true;
				r.land_z = s.z;
				r.hover  = s.z - floor_z;
				r.fall   = -vz_in;
				break;
			}
			if ( s.z < floor_z - 4096.f )
				break;
		}
		r.apex       = apex;
		r.stam_out   = s.stamina;
		r.ducked_out = s.ducked || s.ducking;
		if ( !r.ok )
			r.why = "nolnd";
		return r;
	}

	const press_spec_t k_first_moves[] = {
		{ "jump", n_route::style_jump, n_route::timing_rest },
		{ "crouch jump", n_route::style_jump, n_route::timing_rest, 1, true },
		{ "minijump", n_route::style_minijump, n_route::timing_rest },
		{ "longjump", n_route::style_longjump, n_route::timing_rest },
		{ "walk off", n_route::style_walk_off, n_route::timing_rest },
	};

	const press_spec_t k_second_moves[] = {
		{ "hop", n_route::style_jump, n_route::timing_bhop },
		{ "crouch hop (hold)", n_route::style_jump, n_route::timing_bhop, 1, true },
		{ "mj hop", n_route::style_minijump, n_route::timing_bhop },
		{ "lj hop", n_route::style_longjump, n_route::timing_bhop },
		{ "hop +1t", n_route::style_jump, n_route::timing_delay, 1 },
		{ "hop +2t", n_route::style_jump, n_route::timing_delay, 2 },
		{ "jump (rest)", n_route::style_jump, n_route::timing_rest },
		{ "minijump (rest)", n_route::style_minijump, n_route::timing_rest },
		{ "longjump (rest)", n_route::style_longjump, n_route::timing_rest },
		{ "walk off", n_route::style_walk_off, n_route::timing_rest },
	};

	const char* branch_name( const leg_result_t& r ) { return r.absolute ? "abs" : "add"; }
	const char* stance_name( bool ducked ) { return ducked ? "duck " : "stand"; }

	void jm_row( const char* first, const char* second, const leg_result_t& r, float solo_rise )
	{
		if ( !r.ok ) {
			botox_dbg_log( "[rc jm] %-20s %-18s %-5s  press %8.3f stam %5.2f  -- the engine refuses this press\n",
			                   first, second, r.why, r.dep_z, r.dep_stam );
			return;
		}
		const float rise = r.apex - r.dep_z;
		char delta[ 24 ]{ };
		if ( solo_rise > -1e29f )
			sprintf_s( delta, "%+7.3f", rise - solo_rise );
		else
			sprintf_s( delta, "%7s", "-" );
		botox_dbg_log( "[rc jm] %-20s %-18s ok     press %8.3f stam %5.2f %s %s  rise %7.3f  vs solo %s  apex %8.3f  "
		                   "land %8.3f hover %5.3f fall %6.1f  stam out %5.2f -> %s\n",
		                   first, second, r.dep_z, r.dep_stam, branch_name( r ), stance_name( r.press_ducked ), rise,
		                   delta, r.apex, r.land_z, r.hover, r.fall, r.stam_out, stance_name( r.ducked_out ) );
	}
}

namespace n_route
{
	int ground_player( )
	{
		auto* local = g_ctx.m_local;
		if ( !local || !( local->get_flags( ) & fl_onground ) || !g_interfaces.m_client_entity_list )
			return 0;
		auto* e = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_ground_entity_handle( ) );
		if ( !e || e == local || !e->is_player( ) || !e->is_alive( ) )
			return 0;
		return e->get_index( );
	}

	bool measure_launch( c_user_cmd* cmd )
	{
		auto* local = g_ctx.m_local;
		if ( !local || !local->is_alive( ) || !cmd || !g_interfaces.m_prediction )
			return false;

		refresh_engine_numbers( );

		const c_vector origin   = local->get_origin( );
		const c_vector abs_orig = local->get_abs_origin( );
		const c_vector velocity = local->get_velocity( );
		const int flags         = local->get_flags( );
		const int buttons       = cmd->m_buttons;
		const float forward     = cmd->m_forward_move;
		const float side        = cmd->m_side_move;
		const float up          = cmd->m_up_move;
		const float stamina     = local->get_stamina( );
		const int frame         = g_interfaces.m_prediction->m_commands_predicted - 1;

		g_prediction.restore_entity_to_predicted_frame( frame );
		local->get_velocity( ) = c_vector( 0.f, 0.f, 0.f );
		const bool absolute = ground_player( ) > 0 || local->get_duck_amount( ) > 0.f;
		/* stamina = prediction-DATAMAP field; ADD_DATAFIELD returns offset 0 on a miss, so a blind write
		   hits the vtable pointer. range-check before writing. */
		if ( const float s = local->get_stamina( ); s >= 0.f && s <= 200.f )
			local->get_stamina( ) = 0.f;

		bool ok = false;
		for ( int i = 0; i < 8; ++i ) {
			cmd->m_buttons      = ( local->get_flags( ) & fl_onground ) ? in_jump : 0;
			cmd->m_forward_move = 0.f;
			cmd->m_side_move    = 0.f;
			cmd->m_up_move      = 0.f;

			g_prediction.begin( local, cmd );
			g_prediction.end( local );

			const float vz_after = local->get_velocity( ).m_z;
			if ( vz_after > 1.f ) {
				const float measured = vz_after + ( absolute ? 2.f : 3.f ) * half_gravity_per_tick( );
				if ( measured > 100.f && measured < 1000.f ) {
					g_impulse = measured;
					ok        = true;
				}
				break;
			}
		}

		g_prediction.restore_entity_to_predicted_frame( frame );
		local->get_origin( ) = origin;
		local->set_abs_origin( abs_orig );
		local->get_velocity( ) = velocity;
		local->get_flags( )    = flags;
		if ( stamina >= 0.f && stamina <= 200.f )
			local->get_stamina( ) = stamina;
		cmd->m_buttons      = buttons;
		cmd->m_forward_move = forward;
		cmd->m_side_move    = side;
		cmd->m_up_move      = up;

		const float read_back = g_impulse;
		const float cv_impulse = convar_float( "sv_jump_impulse", 301.993377f );
		if ( !ok || std::fabs( g_impulse - cv_impulse ) < 0.01f )
			g_impulse = cv_impulse;

		sim_t probe{ };
		float apex_jump = 0.f, apex_lj = 0.f;
		for ( int variant = 0; variant < 2; ++variant ) {
			probe        = sim_t{ };
			float& apex  = variant == 0 ? apex_jump : apex_lj;
			const bool d = variant == 1;
			sim_tick( probe, true, d, 0.f, true );
			for ( int i = 0, cap = air_tick_cap( ); i < cap && !probe.on_ground; ++i ) {
				sim_tick( probe, false, d, 0.f, true );
				if ( probe.z > apex )
					apex = probe.z;
			}
		}
		const bool at_64 = tick_dt( ) > 0.0150f && tick_dt( ) < 0.0163f;
		botox_dbg_log( "[rc sim] impulse %.4f ( measured %d, read %.6f ) | %.0f tick %s, dt %.6f, caps air %d rest %d - jump apex %.4f%s, "
		                   "lj apex %.4f%s\n",
		                   g_impulse, ok ? 1 : 0, ok ? read_back : 0.f, 1.f / tick_dt( ), n_tick::forced_128( ) ? "( 128 fix FORCED )" : "( engine )",
		                   tick_dt( ), air_tick_cap( ), rest_tick_cap( ), apex_jump, at_64 ? " ( want 54.6537 )" : "", apex_lj,
		                   at_64 ? " ( want 65.9975 )" : "" );
		return true;
	}

	void solve( const solve_input_t& in, solve_output_t& out )
	{
		out = solve_output_t{ };
		if ( in.points.empty( ) )
			return;

		refresh_engine_numbers( );

		{
			std::string moves;
			for ( int m = 0; m < move_count; ++m ) {
				if ( !( in.global_moves & ( 1u << m ) ) )
					continue;
				moves += moves.empty( ) ? "" : ", ";
				moves += k_move_names[ m ];
			}
			botox_dbg_log( "[rc opts] delay hop %d ticks | one move per point | start %s | mid-route: bhop / counted hop / "
			                   "jumpbug only, no rest | moves ( %#x ): %s\n",
			                   in.delay_ticks, in.start_on_ground ? "standing" : "in air", in.global_moves,
			                   moves.empty( ) ? "NONE" : moves.c_str( ) );
		}

		{
			static float dumped_impulse = -1.f;
			if ( std::fabs( dumped_impulse - g_impulse ) > 0.01f ) {
				dumped_impulse = g_impulse;
				dump_jump_matrix( );
			}
		}
		g_num.travel = in.travel;

		const float half_g  = half_gravity_per_tick( );
		const float eb_null = -half_g;

		std::vector< long double > branch_max;
		std::vector< long double > unsearched;
		const auto commit_counts = [ & ]( ) {
			long double space = 1.L, missed = 0.L, suffix = 1.L;
			for ( const long double b : branch_max )
				space *= b;
			for ( std::size_t i = branch_max.size( ); i-- > 0; ) {
				missed += unsearched[ i ] * suffix;
				suffix *= branch_max[ i ];
			}
			out.space    = space;
			out.searched = space > missed ? space - missed : 0.L;
		};

		std::vector< node_t > frontier;
		long long alts_over_cap = 0;
		node_t seed;
		seed.st.z         = in.start_z;
		seed.st.on_ground = in.start_on_ground;
		seed.st.on_player = in.start_on_ground && in.start_on_player;
		seed.st.vz         = in.start_on_ground ? 0.f : in.start_vz;
		seed.st.stamina    = in.start_stamina;
		seed.st.duck_speed = in.start_duck_speed;
		if ( in.start_ducked ) {
			seed.st.ducked      = true;
			seed.st.fl_ducking  = true;
			seed.st.duck_amount = 1.f;
			seed.st.raw_duck    = true;
		} else if ( in.start_duck_amount > 0.f ) {
			seed.st.ducking     = true;
			seed.st.duck_amount = in.start_duck_amount;
		}
		seed.floor        = in.start_z;
		seed.mask         = in.start_styles == 0u ? k_all_styles : in.start_styles;
		frontier.emplace_back( seed );

		// one marker = one move, never a hidden hop. a point one move can't reach is reported unreachable
		const std::vector< route_point_t >& points = in.points;
		const auto point_label = [ ]( std::size_t p ) {
			char buf[ 24 ]{ };
			sprintf_s( buf, "point %d", static_cast< int >( p ) + 1 );
			return std::string( buf );
		};
		int deepest = -1;
		long double nodes_done = 0.L;
		std::size_t prev_width = 1;

		for ( std::size_t p = 0; p < points.size( ); ++p ) {
			route_point_t point = points[ p ];
			const int shown_type = route_point_shown_type( point );
			if ( point.type == route_pt_headbounce && point.hb_ceiling )
				point.type = route_pt_headbang;
			const bool last            = p + 1 == points.size( );
			const float target_z       = point.quantized_z;
			/* solid = STOPS an arc via the 2u trace: floor, ledge, pixel jump lip. pixel / seam / ceiling never
			   ground you mid-flight. a lip grounds only a hull inside it ( pj_creep's job ), then it's a floor. */
			const bool solid           = point.type == route_pt_ground || point.type == route_pt_edgebug ||
			                             point.type == route_pt_pixeljump;
			const bool skin            = point.type == route_pt_pixeljump && point.pj_skin;
			float px_plane      = 0.f;
			const bool px_grid  = point.type == route_pt_pixelsurf && pixel_plane( target_z, px_plane );

			if ( static_cast< int >( p ) > deepest ) {
				deepest       = static_cast< int >( p );
				out.failed_at = static_cast< int >( p );
				out.best_gap  = FLT_MAX;
				out.want_z    = target_z;
				out.closest_z = target_z;
				out.blocked_moves = 0u;
			}

			std::vector< node_t > next;
			std::unordered_map< std::string, std::size_t > seen;
			std::vector< long long > fan;
			fan.reserve( frontier.size( ) );
			std::vector< std::pair< float, std::string > > misses;
			std::vector< std::pair< float, std::string > > edge;
			float best_by_presses[ 16 ];
			for ( float& b : best_by_presses )
				b = FLT_MAX;

			/* progress = nodes expanded / ( done + this point + later points at this point's growth, beam capped ) */
			const long double width = static_cast< long double >( frontier.size( ) );
			long double later       = 0.L;
			{
				const long double grow = ( std::max )( 1.L, width / static_cast< long double >( prev_width ) );
				long double w          = width;
				for ( std::size_t j = p + 1; j < points.size( ); ++j ) {
					w = ( std::min )( w * grow, static_cast< long double >( k_beam ) );
					later += w;
				}
			}
			std::size_t expanded = 0;

			for ( const node_t& node : frontier ) {
				if ( ( ++expanded & 63u ) == 0u ) {
					if ( in.cancel && in.cancel->load( std::memory_order_relaxed ) )
						return;
					/* one writer: a re-estimate at a new point never shows the bar stepping back */
					if ( const int permille = static_cast< int >( 999.L * ( nodes_done + expanded ) / ( nodes_done + width + later ) );
					     in.progress && permille > in.progress->load( std::memory_order_relaxed ) )
						in.progress->store( permille, std::memory_order_relaxed );
				}
				const bool node_ducked = node.st.ducked || node.st.ducking;
				long long opts         = 0;
				/* roof pass whose head never hit the roof = the open pass's arc again: dropped, not counted twice */
				bool roof_pass = false;
				const auto admit = [ & ]( node_t& made ) {
					if ( roof_pass && !made.st.roof_hit )
						return;
					++opts;
					made.paths = node.paths;
					made.alts  = node.alts;
					if ( last ) {
						next.emplace_back( std::move( made ) );
						return;
					}
					const std::string key = node_key( made, target_z );
					if ( const auto it = seen.find( key ); it != seen.end( ) ) {
						node_t& twin     = next[ it->second ];
						const long long sum =
							( twin.paths > LLONG_MAX - made.paths ) ? LLONG_MAX : twin.paths + made.paths;
						if ( easier( made, twin ) ) {
							merge_alts( made, twin, alts_over_cap );
							twin = std::move( made );
						} else
							merge_alts( twin, made, alts_over_cap );
						twin.paths = sum;
						return;
					}
					seen.emplace( key, next.size( ) );
					next.emplace_back( std::move( made ) );
				};
				for ( int style = 0; style < style_count; ++style ) {
					if ( !node.st.on_ground && style != style_walk_off )
						continue;
					if ( node.st.on_ground && !( node.mask & ( 1u << style ) ) )
						continue;
					/* a jumpbug off a landing whose arc stepped over the 2 unit band never fires */
					if ( style == style_jumpbug && !node.jb_ok )
						continue;

					if ( style == style_walk_off && node.st.on_ground ) {
						if ( !node.legs.empty( ) )
							continue;
						if ( target_z > node.st.z - k_ground_catch )
							continue;
					}

					for ( int timing = 0; timing < timing_count; ++timing ) {
						if ( style == style_walk_off && timing != timing_rest )
							continue;
						if ( style == style_jumpbug && timing != timing_bhop )
							continue;
						if ( timing == timing_delay && in.delay_ticks <= 0 )
							continue;
						if ( timing == timing_delay && ( style != style_jump || node_ducked ) )
							continue;
						if ( timing != timing_rest && node.legs.empty( ) && in.start_on_ground )
							continue;
						if ( timing == timing_rest && node.st.on_ground && !node.legs.empty( ) &&
						     style != style_walk_off )
							continue;
						if ( style == style_jumpbug && node.legs.empty( ) )
							continue;
						if ( node.hop_only && timing != timing_bhop )
							continue;
						if ( node_ducked && timing != timing_rest && style == style_minijump )
							continue;

						if ( const unsigned int bit = 1u << move_of( style, timing, node_ducked );
						     node.st.on_ground && !( in.global_moves & bit ) ) {
							out.blocked_moves |= bit;
							out.refused_moves |= bit;
							continue;
						}
						const int only_delay = timing == timing_delay ? in.delay_ticks : 1;
						for ( int delay_n = only_delay; delay_n <= only_delay; ++delay_n ) {

						const bool can_hold_crouch =
							( style == style_jump || style == style_walk_off ) && timing != timing_delay &&
							( node.st.on_ground ? !node.hop_only && ( timing == timing_rest || node_ducked ) : node_ducked );
						const bool lj_here = ( node.mask & ( 1u << style_longjump ) ) &&
						                     ( in.global_moves & ( 1u << move_of( style_longjump, timing, node_ducked ) ) );
						const bool duck_air_here = ( node.st.on_ground ? style != style_walk_off : !node_ducked ) &&
						                           ( point.type == route_pt_pixeljump || point.type == route_pt_headbang ||
						                             point.type == route_pt_headbounce || point.type == route_pt_ground );
						const bool can_duck_air  = duck_air_here && ( style != style_minijump || !lj_here );
						const bool can_duck_late = duck_air_here && style == style_minijump;
						const bool roof_leg   = point.roof < 3.0e38f && ( point.type == route_pt_ground || point.type == route_pt_pixelsurf ||
						                                                   point.type == route_pt_pixeljump || point.type == route_pt_edgebug );
						const int ceil_passes = point.type == route_pt_headbang || roof_leg ? 2 : 1;
						bool plain_arrived    = false;
						for ( int pass = 0; pass < 4 * ceil_passes; ++pass ) {
							const int variant     = pass % 4;
							const bool ceil_over  = pass >= 4 && point.type == route_pt_headbang;
							const bool under_roof = pass >= 4 && roof_leg;
							roof_pass             = under_roof;
							const int hold_crouch = variant == 1 ? 1 : 0;
							const bool duck_late  = variant == 3;
							const bool duck_air   = variant == 2 || duck_late; /* both must ARRIVE ducked */
							if ( ( hold_crouch == 1 && !can_hold_crouch ) || ( variant == 2 && !can_duck_air ) ||
							     ( duck_late && !can_duck_late ) )
								continue;
							if ( node.crouch_only && node.st.on_ground && hold_crouch != 1 )
								continue;
							if ( duck_air && plain_arrived && point.type == route_pt_pixeljump && !last && !node.st.on_ground )
								continue;
							if ( duck_air && plain_arrived && point.type == route_pt_ground && last )
								continue;
							if ( duck_air && point.type == route_pt_ground && plain_arrived ) {
								if ( !( in.global_moves & ( 1u << move_crouch_hop ) ) ) {
									out.refused_moves |= 1u << move_crouch_hop;
									continue;
								}
								if ( point.styles != 0u && !( point.styles & ( 1u << style_jump ) ) )
									continue;
							}
							/* a jumpbug never touched down: presses from the band tick ( ducked, air, no OnLand ) */
							sim_t s = style == style_jumpbug ? node.jb : node.st;

							int wait = 0;
							if ( !s.on_ground )
								wait = 0;
							else if ( timing == timing_rest )
								wait = rest_tick_cap( );
							else if ( timing == timing_delay )
								wait = delay_n;
							bool bailed = false;
							for ( int i = 0; i < wait; ++i ) {
								/* i > 0: a rest is never the landing tick; stand one tick so CategorizePosition
								   snaps onto the plane ( else the departure hovers ) */
								if ( timing == timing_rest && i > 0 && s.stamina <= 0.f && !s.ducking && !s.ducked )
									break;
								sim_tick( s, false, false, node.floor, true );
								if ( !s.on_ground ) {
									bailed = true;
									break;
								}
							}
							if ( bailed )
								continue;

							if ( hold_crouch == 1 && s.on_ground ) {
								if ( !s.ducked ) {
									if ( !duck_press_lands( s ) )
										continue;
									s.duck_speed     = ( std::max )( 0.f, s.duck_speed - k_duck_penalty );
									s.last_duck_time = s.time;
								}
								s.raw_duck    = true;
								s.ducked      = true;
								s.ducking     = false;
								s.duck_amount = 1.f;
								s.fl_ducking  = true;
							}
							if ( hold_crouch == 1 && !s.on_ground && !s.raw_duck && !duck_press_lands( s ) )
								continue;

							const float dep_z    = style == style_jumpbug ? s.z - k_duck_delta : s.z;
							const float dep_stam = s.stamina;
							const bool press_ducked = s.ducked || s.ducking;

							const bool press_duck = duck_at_launch( style ) || hold_crouch == 1;
							const bool hold_duck  = duck_held_after( style ) || hold_crouch == 1;
							if ( hold_crouch == 1 && node.st.on_ground &&
							     !( in.global_moves & ( 1u << move_of( style, timing, true ) ) ) ) {
								out.refused_moves |= 1u << move_of( style, timing, true );
								continue;
							}
							/* the style's +duck must pass the gate, else it's a standing jump mislabeled */
							if ( press_duck && !press_ducked && !duck_press_lands( s ) )
								continue;

							if ( ceil_over )
								s.ceiling = ( std::min )( s.ceiling, target_z );
							s.roof     = under_roof ? point.roof : 3.4e38f;
							s.roof_hit = false;

							const sim_t s_launch_in = s;
							if ( style == style_walk_off ) {
								if ( s.on_ground ) {
									s.vz        = -half_g;
									s.on_ground = false;
								}
							} else {
								sim_tick( s, true, press_duck, node.floor, true, node.skin );
								if ( s.on_ground )
									continue; /* the press never left the floor */
								/* a jumpbug whose unduck was refused ( ceiling over the band ) never jumped: its band just falls */
								if ( style == style_jumpbug && s.vz <= 0.f )
									continue;
							}

							const std::string dep =
								node.st.on_ground
									? ( node.hop_only ? "(pixel) " : "" ) + departure_name( style, timing, delay_n, press_ducked, hold_crouch == 1 )
									: std::string( hold_crouch == 1 ? "fall (ducked)" : "fall" );
							const int leg_cost = timing_cost( style, timing, delay_n );

							float attempt_gap = FLT_MAX;
							float attempt_z   = 0.f;
							float apex        = s.z;
							bool made_any     = false;

							const auto push_leg = [ & ]( node_t& made, float arrive_z, float fall, float hover, float gap,
							                            int tier = 0 ) {
								leg_t leg;
								leg.name     = made.st.roof_hit ? dep + " [roof]" : dep;
								leg.type     = shown_type;
								leg.dep_z    = dep_z;
								leg.dep_stam = dep_stam;
								leg.apex     = apex;
								leg.arrive_z = arrive_z;
								leg.fall     = fall;
								leg.hover    = hover;
								leg.gap      = gap;
								leg.tier     = tier;
								leg.stam_out = made.st.stamina;
								made.legs    = node.legs;
								made.legs.emplace_back( std::move( leg ) );
								made.floor = target_z;
								made.mask  = point.styles == 0u ? k_all_styles : point.styles;
								made.bind_mask = node.bind_mask;
								made.last_bind = node.last_bind;
								made.switches  = node.switches;
								made.presses   = node.presses;
								made.crouches  = node.crouches +
								                ( duck_air && ( !node.st.on_ground || ( plain_arrived && point.type == route_pt_ground ) ) ? 1 : 0 );
								if ( const int bind = bind_of( style, node.st.on_ground ); bind >= 0 ) {
									if ( made.last_bind >= 0 && made.last_bind != bind )
										++made.switches;
									made.last_bind = bind;
									made.bind_mask |= 1u << bind;
									++made.presses;
								}
							};
							const auto try_bang = [ & ]( float z_in, float hull_in, bool ducked_in ) {
								if ( z_in + hull_in > target_z + 0.001f || s.z + hull_in <= target_z )
									return false;
								if ( ( ducked_in && !point.allow_duck ) || ( !ducked_in && !point.allow_stand ) )
									return true;
								if ( duck_air && !ducked_in )
									return true;

								node_t made;
								made.st           = s;
								made.st.z         = target_z - hull_in;
								made.st.vz        = eb_null;
								made.st.on_ground = false;
								made.st.fall      = -eb_null;
								made.st.ceiling   = target_z;
								made.elements     = node.elements;
								made.cost         = node.cost + leg_cost;
								made.elements.emplace_back( dep );
								made.elements.emplace_back( arrival_name( shown_type, ducked_in ) );
								push_leg( made, made.st.z, 0.f, 0.f, std::fabs( ( z_in + hull_in ) - target_z ) );
								admit( made );
								made_any = true;
								return true;
							};
							const float launch_hull = hull_top( s_launch_in );
							const bool launch_cross = point.type == route_pt_headbang && style != style_walk_off &&
							                          s.ducked == s_launch_in.ducked && s_launch_in.z + launch_hull <= target_z + 0.001f &&
							                          s.z + launch_hull > target_z;
							if ( duck_air && launch_cross )
								continue;
							const bool launch_bang = launch_cross && try_bang( s_launch_in.z, launch_hull, s_launch_in.ducked );

							const auto armed = [ & ]( ) { return s.z >= target_z && s.vz - half_g < 0.f; };
							bool above = armed( );
							bool rise_landed = false;
							int creep_ticks = 0;
							bool lip_missed = false;
							int tb_catches = 0, tb_first = -1;

							const auto land_floor = [ & ]( const sim_t& st, const sim_t& st_in, float vz_in, bool held_now ) {
								node_t made;
								made.st           = st;
								made.st.on_player = point.ent > 0;
								made.elements     = node.elements;
								made.cost         = node.cost + leg_cost;
								made.elements.emplace_back( dep );
								if ( point.type == route_pt_pixeljump ) {
									if ( last )
										made.elements.emplace_back( arrival_name( point.type, st.ducked || st.ducking ) );
									made.hop_only = true;
									made.skin     = skin;
								} else if ( point.type == route_pt_ground && last && duck_air )
									made.elements.emplace_back( "ground (ducked)" );
								made.crouch_only = duck_air && plain_arrived && point.type == route_pt_ground;
								if ( !held_now && vz_in < 0.f ) {
									made.jb_ok        = jumpbug_band( st_in, target_z, made.jb, skin );
									made.jb.on_player = made.st.on_player;
								}
								constexpr float k_skin_last_push = 0.98f * ( 0.03125f / 16.f - n_route::k_creep_gap );
								const int tier = skin && st.z - target_z < n_route::k_skin_lo + k_skin_last_push ? 1 : 0;
								push_leg( made, st.z, -vz_in, st.z - target_z, st.z - target_z, tier );
								admit( made );
								made_any = true;
							};

							for ( int i = 0, cap = launch_bang ? 0 : air_tick_cap( ); i < cap; ++i ) {
								bool held = hold_duck;
								if ( held && style == style_longjump && hold_crouch == 0 && i >= lj_bind_air_ticks( ) )
									held = false;
								if ( duck_air && ( !duck_late || i > 0 ) )
									held = true;
								if ( armed( ) )
									above = true;
								const float vz_in = s.vz;
								/* tick-start hull: a seam catch CLIPS the move ( origin stays ), and the straddle
								   is the swept hull's. a duck-finishing tick moved 9 on its own: never a catch. */
								const float z_in       = s.z;
								const bool ducked_in   = s.ducked;
								const float hull_in    = hull_top( s );
								const sim_t s_tick_in  = point.type == route_pt_ground || point.type == route_pt_pixeljump ||
								                         point.type == route_pt_pixelsurf
								                             ? s
								                             : sim_t{ };
								const bool above_used = above;
								bool lip_in = true;
								if ( point.type == route_pt_pixeljump ) {
									const bool pushed = z_in >= target_z - k_dist_eps;
									creep_ticks += pushed ? 1 : 0;
									lip_in = !lip_missed && creep_ticks >= k_pj_creep_ticks;
									if ( lip_in && pushed && creep_ticks == k_pj_creep_ticks ) {
										sim_t raw = s;
										sim_tick( raw, false, held, target_z, false, skin );
										lip_in = raw.z >= target_z;
									}
								}
								sim_tick( s, false, held, target_z, solid && above && lip_in, skin );
								if ( !lip_in && s.vz < 0.f && s.z < target_z - k_dist_eps )
									lip_missed = true;
								if ( s.z > apex )
									apex = s.z;

								if ( !above_used && !rise_landed && !s.on_ground && s_tick_in.vz - half_g > 0.f && lip_in &&
								     ( point.type == route_pt_ground || point.type == route_pt_pixeljump ) ) {
									sim_t probe = s_tick_in;
									sim_tick( probe, false, held, target_z, true, skin );
									if ( probe.on_ground && probe.z >= target_z && probe.ducked == s_tick_in.ducked &&
									     ( !duck_air || probe.ducked || probe.ducking ) ) {
										rise_landed = true;
										land_floor( probe, s_tick_in, vz_in, held );
										if ( probe.z - target_z < attempt_gap ) {
											attempt_gap = probe.z - target_z;
											attempt_z   = probe.z;
										}
									}
								}

								const bool falling = s.vz < 0.f || s.on_ground;
								/* pixel catch happens on the NEXT tick ( clip at frac 0 ): its move must go down. the apex
								   tick ends with vz up to +half_g and still counts */
								const bool px_next_down = s.vz - half_g < 0.f;
								const bool px_duck_ok = point.type == route_pt_pixelsurf && !( s.ducked || s.ducking ) &&
								                        point.allow_duck && duck_press_lands( s_tick_in );

								const bool by_head = point.type == route_pt_headbang || point.type == route_pt_texturebug;
								const bool hb_rise   = point.type == route_pt_headbounce && !s.on_ground && vz_in - half_g > 0.f;
								const float hb_head  = z_in + hull_in;
								const bool countable = point.type == route_pt_headbounce ? hb_rise
								                       : by_head ? !s.on_ground : ( point.type == route_pt_pixelsurf ? px_next_down : falling );
								if ( countable ) {
									float gap;
									float px_z = point.type == route_pt_headbounce ? hb_head : s.z;
									if ( point.type == route_pt_headbounce ) {
										gap = hb_head < target_z - k_dist_eps ? target_z - k_dist_eps - hb_head
										      : hb_head >= target_z          ? hb_head - target_z
										                                     : 0.f;
									} else if ( point.type == route_pt_pixelsurf ) {
										const auto px_gap = [ & ]( float z ) {
											return px_grid ? pixel_window_gap( z, px_plane ) : std::fabs( z - target_z );
										};
										gap = FLT_MAX;
										if ( ( s.ducked || s.ducking ) ? point.allow_duck : point.allow_stand )
											gap = px_gap( s.z );
										if ( px_duck_ok && px_gap( s.z + k_duck_delta ) < gap ) {
											gap  = px_gap( s.z + k_duck_delta );
											px_z = s.z + k_duck_delta;
										}
									} else if ( point.type == route_pt_headbang ) {
										gap = std::fabs( s.z + hull_top( s ) - target_z );
									} else if ( point.type == route_pt_texturebug ) {
										const float h = hull_top( s );
										gap = s.z >= target_z          ? s.z - target_z
										      : s.z + h <= target_z    ? target_z - ( s.z + h )
										                               : 0.f;
									} else {
										gap = std::fabs( s.z - target_z );
										if ( skin ) {
											const float over = s.z - target_z;
											gap = over < n_route::k_skin_lo ? n_route::k_skin_lo - over
											      : over > n_route::k_skin_hi ? over - n_route::k_skin_hi : 0.f;
										}
									}
									const float report_z =
										point.type == route_pt_headbang ? s.z + hull_top( s ) : px_z;
									if ( gap < out.best_gap ) {
										out.best_gap  = gap;
										out.closest_z = report_z;
									}
									if ( gap < attempt_gap ) {
										attempt_gap = gap;
										attempt_z   = report_z;
									}
								}

								if ( s.z < target_z - 4096.f )
									break;
								if ( falling && point.type == route_pt_headbang && s.z + k_hull_stand < target_z )
									break;
								if ( falling && point.type == route_pt_texturebug && s.z + k_hull_stand < target_z - 1.f )
									break;
								if ( falling && skin && !s.on_ground && s.z + k_duck_delta < target_z + n_route::k_skin_lo )
									break;
								/* pj passed with the hull still outside the lip: surfs the seam or falls past, never lands */
								if ( lip_missed )
									break;

								if ( point.type == route_pt_pixelsurf ) {
									if ( !px_next_down )
										continue;
									for ( int variant = 0; variant < 2; ++variant ) {
										const bool catch_duck = variant == 1;
										if ( catch_duck && !px_duck_ok )
											continue;
										const bool ducked = catch_duck || s.ducked || s.ducking;
										if ( !catch_duck && ( ducked ? !point.allow_duck : !point.allow_stand ) )
											continue;
										const float catch_z = catch_duck ? s.z + k_duck_delta : s.z;
										int tier       = 0;
										float rank_gap = std::fabs( catch_z - target_z );
										if ( px_grid ) {
											if ( !in_pixel_window( catch_z, px_plane ) )
												continue;
											tier     = pixel_tier( catch_z, px_plane, ducked );
											rank_gap = pixel_centre_gap( catch_z, px_plane, ducked );
										} else if ( point.has_measured ) {
											if ( catch_z > target_z + 0.0005f || catch_z < target_z - 0.004f )
												continue;
										} else {
											if ( !check( catch_z, target_z ) )
												continue;
											tier = check_centre( catch_z, target_z )
											           ? 0
											           : ( pixel_bucket( catch_z ) == pixel_bucket( target_z ) ? 1 : 2 );
										}
										if ( tier > 0 ) {
											char line[ 160 ]{ };
											sprintf_s( line, "%-18s%s catch %9.4f  gap %6.4f", dep.c_str( ),
											           catch_duck ? " +9" : "   ", catch_z, target_z - catch_z );
											edge.emplace_back( rank_gap, line );
										}

										node_t made;
										made.st = s;
										made.st.z = catch_z;
										made.st.vz        = eb_null;
										made.st.on_ground = false;
										made.st.fall      = -eb_null;
										if ( catch_duck ) {
											made.st.duck_speed     = ( std::max )( 0.f, s_tick_in.duck_speed - k_duck_penalty );
											made.st.last_duck_time = s_tick_in.time;
											made.st.raw_duck       = true;
											made.st.ducked         = true;
											made.st.ducking        = false;
											made.st.duck_amount    = 1.f;
											made.st.fl_ducking     = true;
										}
										for ( int h = 0; h < rest_tick_cap( ) &&
										                 ( made.st.stamina > 0.f || made.st.time < made.st.last_duck_time + g_num.duck_gap ||
										                   made.st.duck_speed < k_duck_speed );
										      ++h ) {
											made.st.stamina = ( std::max )( 0.f, made.st.stamina - stamina_per_tick( ) );
											duck_recover( made.st );
											made.st.time += tick_dt( );
										}
										made.elements = node.elements;
										made.cost     = node.cost + leg_cost;
										made.elements.emplace_back( dep );
										made.elements.emplace_back( arrival_name( shown_type, ducked ) );
										push_leg( made, catch_z, s.fall, 0.f, rank_gap, tier );
										admit( made );
										made_any = true;
									}
									// the plain arc hangs on the first tick in the window (allowed stance or not): later ticks
									// never happen. near the apex two tick ends can sit in one 1/32, one chance only
									const bool plain_caught = px_grid ? in_pixel_window( s.z, px_plane )
									                                  : ( point.has_measured ? s.z <= target_z + 0.0005f && s.z >= target_z - 0.004f
									                                                         : check( s.z, target_z ) );
									if ( plain_caught )
										break;
									continue;
								}

								if ( point.type == route_pt_headbang ) {
									if ( s.on_ground )
										break;
									if ( vz_in <= 0.f )
										continue;
									if ( s.ducked != ducked_in )
										continue;
									if ( try_bang( z_in, hull_in, ducked_in ) )
										break;
									continue;
								}

								if ( point.type == route_pt_headbounce ) {
									if ( !hb_rise || hb_head >= target_z )
										break;
									if ( s.ducked != ducked_in || hb_head < target_z - k_dist_eps )
										continue;
									if ( ( ducked_in && !point.allow_duck ) || ( !ducked_in && !point.allow_stand ) || ( duck_air && !ducked_in ) )
										break;

									node_t made;
									made.st           = s;
									made.st.z         = z_in;
									made.st.vz        = eb_null;
									made.st.on_ground = false;
									made.st.fall      = -eb_null;
									made.elements     = node.elements;
									made.cost         = node.cost + leg_cost;
									made.elements.emplace_back( dep );
									made.elements.emplace_back( arrival_name( point.type, ducked_in ) );
									push_leg( made, z_in, -vz_in, 0.f, target_z - hb_head );
									admit( made );
									made_any = true;
									break;
								}

								if ( point.type == route_pt_texturebug ) {
									if ( s.on_ground )
										break;
									if ( s.ducked != ducked_in )
										continue;
									if ( z_in + k_dist_eps >= target_z || z_in + hull_in - k_dist_eps <= target_z )
										continue;
									if ( ( ducked_in && !point.allow_duck ) || ( !ducked_in && !point.allow_stand ) )
										continue;
									if ( tb_first < 0 )
										tb_first = i;
									if ( ++tb_catches > k_tb_max_catches )
										break;

									node_t made;
									made.st           = s;
									made.st.z         = z_in;   /* clipped: the move never moved it */
									made.st.vz        = eb_null;
									made.st.on_ground = false;
									made.st.fall      = -eb_null;
									made.elements     = node.elements;
									made.cost         = node.cost + leg_cost + ( i - tb_first );
									made.elements.emplace_back( dep );
									{
										std::string label = arrival_name( point.type, ducked_in );
										/* later push-in tick: "Nt late", never "+Nt" ( read as a delayed press ) */
										if ( i > tb_first ) {
											char suffix[ 16 ]{ };
											sprintf_s( suffix, " %dt late", i - tb_first );
											label += suffix;
										}
										made.elements.emplace_back( label );
									}
									push_leg( made, z_in, -vz_in, 0.f, 0.f );
									admit( made );
									made_any = true;
									continue;
								}

								if ( point.type == route_pt_edgebug ) {
									if ( !s.on_ground )
										continue;
									if ( !g_edgebug.IsEdgeBugTick( vz_in, eb_null, false, false ) )
										break;
									const bool ducked = s.ducked;
									if ( ( ducked && !point.allow_duck ) || ( !ducked && !point.allow_stand ) )
										break;

									node_t made;
									made.st           = s;
									made.st.z         = target_z;
									made.st.vz        = eb_null;
									made.st.on_ground = false; /* an edgebug never lands: the fall goes on */
									made.st.fall      = 0.f;
									made.elements     = node.elements;
									made.cost         = node.cost + leg_cost;
									made.elements.emplace_back( dep );
									made.elements.emplace_back( arrival_name( point.type, ducked ) );
									push_leg( made, s.z, -vz_in, s.z - target_z, std::fabs( s.z - target_z ) );
									admit( made );
									made_any = true;
									break;
								}

								if ( s.on_ground ) {
									if ( !duck_air || s.ducked || s.ducking )
										land_floor( s, s_tick_in, vz_in, held );
									break;
								}
							}
							if ( variant == 0 )
								plain_arrived = made_any;
							/* roof pass never touched it: the open pass already reported this arc */
							if ( under_roof && !s.roof_hit )
								continue;

							if ( !made_any )
								++opts;
							if ( const std::size_t presses = node.legs.size( ) + 1;
							     presses < 16 && attempt_gap < best_by_presses[ presses ] )
								best_by_presses[ presses ] = attempt_gap;
							if ( attempt_gap < FLT_MAX ) {
								char line[ 224 ]{ };
								sprintf_s( line, "%-24s%s from %9.3f stam %5.2f -> apex %9.3f closest %9.3f (off %6.3f)", dep.c_str( ),
								           duck_late ? " [duck late]" : duck_air ? " [duck air]" : ( ceil_over ? " [ceil over]" : under_roof ? " [roof]" : "" ), dep_z, dep_stam, apex,
								           attempt_z, attempt_gap );
								misses.emplace_back( attempt_gap, line );
							}
							}
						}
					}
				}
				fan.emplace_back( opts );
			}
			nodes_done += width;
			prev_width = frontier.size( );

			{
				long long fan_max = 1;
				for ( const long long f : fan )
					fan_max = ( std::max )( fan_max, f );
				branch_max.emplace_back( static_cast< long double >( fan_max ) );
				unsearched.emplace_back( 0.L );
			}

			if ( last && !next.empty( ) ) {
				out.jumps = static_cast< int >( next.front( ).legs.size( ) );
				for ( int n = 0; n < 16; ++n )
					out.reach_gap[ n ] = best_by_presses[ n ];
			}

			/* [rc reach]: best gap per route length at this marker ( absent = never tried; window ~0.03 ) */
			{
				std::string reach;
				for ( int n = 1; n < 16; ++n ) {
					if ( best_by_presses[ n ] == FLT_MAX )
						continue;
					char cell[ 48 ]{ };
					sprintf_s( cell, "%s%d jump%s closest %.3f", reach.empty( ) ? "" : " | ", n, n == 1 ? " " : "s", best_by_presses[ n ] );
					reach += cell;
				}
				if ( !reach.empty( ) )
					botox_dbg_log( "[rc reach] %s ( %s ): %s\n", point_label( p ).c_str( ),
					                   route_point_type_name( shown_type ), reach.c_str( ) );
			}

			if ( !edge.empty( ) ) {
				std::stable_sort( edge.begin( ), edge.end( ),
				                  []( const auto& a, const auto& b ) { return a.first < b.first; } );
				botox_dbg_log( "[rc edge] %s: %d catches on the W-held band edge or under it ( gentle press ) - KEPT, ranked under:\n",
				                   point_label( p ).c_str( ), static_cast< int >( edge.size( ) ) );
				int shown = 0;
				for ( const auto& e : edge ) {
					if ( shown++ >= 8 )
						break;
					botox_dbg_log( "[rc edge]   %s\n", e.second.c_str( ) );
				}
			}

			if ( next.empty( ) ) {
				commit_counts( );
				out.failed_at = static_cast< int >( p );
				out.want_z    = target_z;

				std::stable_sort( misses.begin( ), misses.end( ),
				                  []( const auto& a, const auto& b ) { return a.first < b.first; } );
				botox_dbg_log( "[rc miss] %s ( %s ) wants z %.4f - %d departures tried, closest first:\n",
				                   point_label( p ).c_str( ), route_point_type_name( shown_type ), target_z,
				                   static_cast< int >( misses.size( ) ) );
				int shown = 0;
				for ( const auto& m : misses ) {
					if ( shown++ >= 12 )
						break;
					botox_dbg_log( "[rc miss]   %s\n", m.second.c_str( ) );
					out.miss_lines.emplace_back( m.second );
				}
				return;
			}

			out.arcs_after.emplace_back( static_cast< int >( next.size( ) ) );

			const std::size_t n_plain =
				static_cast< std::size_t >( std::count_if( next.begin( ), next.end( ), []( const node_t& n ) { return n.crouches == 0; } ) );
			const bool trim = !last && ( n_plain > k_beam_plain || next.size( ) > k_beam );
			if ( !last )
				botox_dbg_log( "[rc beam] %s: %d arcs ( %d with an optional crouch )%s\n", point_label( p ).c_str( ),
				                   static_cast< int >( next.size( ) ), static_cast< int >( next.size( ) - n_plain ),
				                   trim ? " -> TRIMMED, routes may be lost" : " ( all kept )" );

			if ( trim ) {
				out.trimmed = true;
				long double before = 0.L;
				for ( const node_t& n : next )
					before += static_cast< long double >( n.paths );
				std::stable_sort( next.begin( ), next.end( ), easier );
				std::vector< node_t > keep;
				keep.reserve( k_beam );
				std::vector< bool > taken( next.size( ), false );
				const auto phys_of = [ ]( const node_t& n ) {
					return phys_key( n, n.floor ) + "|" + std::to_string( static_cast< int >( n.floor * 32.f ) );
				};
				/* one tier [ lo, hi ) up to cap: easiest per distinct launch, then each PHYSICAL state once ( key histories
				   duplicate states and must not crowd out a fine variant ), then extra key histories */
				const auto fill = [ & ]( std::size_t lo, std::size_t hi, std::size_t cap ) {
					const std::size_t first = keep.size( );
					std::unordered_set< std::string > spread;
					for ( std::size_t i = lo; i < hi && keep.size( ) < cap; ++i ) {
						if ( !spread.insert( beam_key( next[ i ], target_z ) ).second )
							continue;
						taken[ i ] = true;
						keep.emplace_back( std::move( next[ i ] ) );
					}
					std::unordered_set< std::string > phys;
					for ( std::size_t k = first; k < keep.size( ); ++k )
						phys.insert( phys_of( keep[ k ] ) );
					for ( std::size_t i = lo; i < hi && keep.size( ) < cap; ++i ) {
						if ( taken[ i ] || !phys.insert( phys_of( next[ i ] ) ).second )
							continue;
						taken[ i ] = true;
						keep.emplace_back( std::move( next[ i ] ) );
					}
					for ( std::size_t i = lo; i < hi && keep.size( ) < cap; ++i ) {
						if ( !taken[ i ] ) {
							taken[ i ] = true;
							keep.emplace_back( std::move( next[ i ] ) );
						}
					}
				};
				fill( 0, n_plain, k_beam_plain );
				fill( n_plain, next.size( ), k_beam );
				next = std::move( keep );

				long double after = 0.L;
				for ( const node_t& n : next )
					after += static_cast< long double >( n.paths );
				if ( before > after )
					unsearched.back( ) += before - after;
			}
			frontier = std::move( next );
		}

		const auto final_gap = []( const node_t& n ) { return n.legs.empty( ) ? 0.f : n.legs.back( ).gap; };
		const auto tier_of = []( const node_t& n ) { return n.legs.empty( ) ? 0 : n.legs.back( ).tier; };
		std::stable_sort( frontier.begin( ), frontier.end( ), [ & ]( const node_t& a, const node_t& b ) {
			if ( tier_of( a ) != tier_of( b ) )
				return tier_of( a ) < tier_of( b );
			if ( easier( a, b ) )
				return true;
			if ( easier( b, a ) )
				return false;
			if ( a.legs.size( ) != b.legs.size( ) )
				return a.legs.size( ) < b.legs.size( );
			const int ga = static_cast< int >( final_gap( a ) * 2000.f ), gb = static_cast< int >( final_gap( b ) * 2000.f );
			if ( ga != gb )
				return ga < gb;
			std::size_t la = 0, lb = 0;
			for ( const auto& e : a.elements )
				la += e.size( );
			for ( const auto& e : b.elements )
				lb += e.size( );
			return la < lb;
		} );

		const auto last_press_plain = []( std::string e ) {
			if ( e == "jump (ducked)" || e == "jump (release duck)" )
				e = "jump (duck*)";
			if ( e == "fall (ducked)" )
				e = "fall";
			return e;
		};
		const auto is_arrival = []( const std::string& e ) {
			for ( const char* a : { "pixelsurf", "edgebug", "headbang", "texturebug", "pixeljump", "headbounce", "ground" } )
				if ( e.rfind( a, 0 ) == 0 )
					return true;
			return false;
		};
		const int cap = in.max_results > 0 ? in.max_results : 1;
		std::unordered_set< std::string > printed;
		std::unordered_set< std::string > outcomes;
		bool chained = false;
		int alt_rows = 0;
		const auto list_row = [ & ]( const node_t& n, const std::vector< std::string >& els, bool alt ) {
			std::string text;
			for ( const auto& e : els ) {
				if ( e.empty( ) )
					continue;
				text += e == "fall (ducked)" ? std::string( "fall" ) : e;
				text += '>';
			}
			if ( !printed.insert( route_text( els ) ).second )
				return;
			{
				std::string key;
				int last_press = static_cast< int >( els.size( ) ) - 1;
				while ( last_press >= 0 && ( els[ last_press ].empty( ) || is_arrival( els[ last_press ] ) ) )
					--last_press;
				for ( int i = 0; i < static_cast< int >( els.size( ) ); ++i )
					key += ( i == last_press ? last_press_plain( els[ i ] ) : els[ i ] ) + ">";
				char tail[ 48 ]{ };
				sprintf_s( tail, "|%d|%d", n.legs.empty( ) ? 0 : static_cast< int >( std::lround( n.legs.back( ).arrive_z * 10000.f ) ),
				           n.st.ducked ? 1 : 0 );
				if ( !outcomes.insert( key + tail ).second )
					return;
			}
			alt_rows += alt;
			if ( n.legs.size( ) < 16 )
				out.reach_hits[ n.legs.size( ) ]++;
			if ( !chained ) {
				chained = true;
				for ( std::size_t i = 0; i < n.legs.size( ); ++i ) {
					const leg_t& l = n.legs[ i ];
					botox_dbg_log( "[rc chain] %d %-9s %-11s from %9.3f stam %5.2f -> apex %9.3f %s %9.3f fall %6.1f "
					                   "hover %5.3f gap %6.3f stam %5.2f\n",
					                   static_cast< int >( i ) + 1, route_point_type_name( l.type ), l.name.c_str( ), l.dep_z,
					                   l.dep_stam, l.apex, ( l.type == route_pt_ground || l.type == route_pt_pixeljump ) ? "land " : "catch", l.arrive_z, l.fall,
					                   l.hover, l.gap, l.stam_out );
				}
			}
			if ( static_cast< int >( out.routes.size( ) ) < cap ) {
				if ( !n.legs.empty( ) ) {
					const leg_t& l = n.legs.back( );
					botox_dbg_log( "[rc list] %2d  gap %6.4f  band %s  catch %9.4f  %s  keys %d  switches %d  presses %d  "
					                   "cost %d  %s%s\n",
					                   static_cast< int >( out.routes.size( ) ) + 1, l.gap,
					                   l.tier == 0 ? "GOOD" : ( l.tier == 1 ? "iffy" : "poor" ), l.arrive_z,
					                   n.st.ducked ? "ducked" : "stand ", bind_count( n ), n.switches, n.presses, n.cost,
					                   alt ? "[alt] " : "", text.c_str( ) );
				}
				solution_t s;
				s.elements = els;
				s.cost     = n.cost;
				s.gap      = final_gap( n );
				out.routes.emplace_back( std::move( s ) );
			}
		};
		/* per band tier: every lead first ( the easiest ways in, popup order ), then every alt in the same node order.
		   an alt shares its lead's catch, so a W-held alt never lands under a gentle-press-only lead */
		std::size_t alts_total = 0;
		const auto list_tier = [ & ]( const node_t& n ) { return ( std::min )( tier_of( n ), 2 ); };
		for ( int tier = 0; tier <= 2; ++tier ) {
			for ( const node_t& n : frontier )
				if ( list_tier( n ) == tier )
					list_row( n, n.elements, false );
			for ( const node_t& n : frontier ) {
				if ( !n.alts || list_tier( n ) != tier )
					continue;
				alts_total += n.alts->size( );
				for ( const alt_t& a : *n.alts ) {
					std::vector< std::string > els = a.prefix;
					els.insert( els.end( ), n.elements.begin( ) + static_cast< std::ptrdiff_t >( a.len ), n.elements.end( ) );
					list_row( n, els, true );
				}
			}
		}
		botox_dbg_log( "[rc alts] %d unique rows from %d merged sequences, %lld over the %d cap NOT listed\n", alt_rows,
		               static_cast< int >( alts_total ), alts_over_cap, static_cast< int >( k_alt_cap ) );
		out.total     = static_cast< long long >( outcomes.size( ) );
		out.failed_at = -1;
		commit_counts( );
	}

	std::string route_text( const std::vector< std::string >& elements )
	{
		std::string out;
		for ( std::string e : elements ) {
			if ( e.empty( ) || e.rfind( "fall", 0 ) == 0 )
				continue;
			/* pixel jump tag = stance you LAND it in ( 10-06: a ducked landing read "(pixeljump) jump (stand)", user jumped it
			   standing and fell past 7x ). ducked = the jump off releases duck; the jump off itself prints plain */
			if ( e.rfind( "(pixel) ", 0 ) == 0 ) {
				const auto rel   = e.find( " (release duck)" );
				std::string move = e.substr( 8, rel == std::string::npos ? std::string::npos : rel - 8 );
				if ( move == "jump (stand)" )
					move = "jump";
				e = ( rel == std::string::npos ? "(pixeljump) " : "(pixeljump ducked) " ) + move;
			}
			if ( const auto p = e.find( " (release duck)" ); p != std::string::npos )
				e.replace( p, 15, e.compare( p - 4, 4, "jump" ) == 0 ? " (stand)" : "" );
			if ( const auto p = e.find( "t late" ); p != std::string::npos )
				e.erase( e.rfind( ' ', p ) );
			if ( !out.empty( ) )
				out += " -> ";
			out += e;
		}
		return out;
	}

	std::string move_list( unsigned int mask, const char* join )
	{
		std::string names;
		for ( int m = 0; m < move_count; ++m )
			if ( mask & ( 1u << m ) )
				names += ( names.empty( ) ? "" : join ) + std::string( k_move_names[ m ] );
		return names;
	}

	std::string solve_hint( const solve_input_t& in, unsigned int refused, std::string& example, int budget_ms )
	{
		refused &= ~in.global_moves;
		example.clear( );
		std::string line;
		const auto t0 = std::chrono::steady_clock::now( );
		const auto works = [ & ]( unsigned int bits ) {
			if ( std::chrono::steady_clock::now( ) - t0 > std::chrono::milliseconds( budget_ms ) )
				return false;
			botox_dbg_log( "[rc hint] re-solve with %s switched on:\n", move_list( bits, " + " ).c_str( ) );
			solve_input_t retry = in;
			retry.global_moves |= bits;
			retry.max_results = 1;
			retry.progress    = nullptr;
			solve_output_t out;
			solve( retry, out );
			if ( out.routes.empty( ) )
				return false;
			line = route_line( out.routes.front( ) );
			return true;
		};
		unsigned int alone = 0u;
		int count          = 0;
		for ( int m = 0; m < move_count; ++m ) {
			if ( !( refused & ( 1u << m ) ) )
				continue;
			++count;
			if ( works( 1u << m ) ) {
				alone |= 1u << m;
				if ( example.empty( ) )
					example = line;
			}
		}
		if ( alone != 0u )
			return move_list( alone, " or " );
		if ( count < 2 || !works( refused ) )
			return std::string( );
		unsigned int need = refused;
		example           = line;
		for ( int m = 0; m < move_count; ++m ) {
			const unsigned int bit = 1u << m;
			if ( ( need & bit ) && works( need & ~bit ) ) {
				need &= ~bit;
				example = line;
			}
		}
		return move_list( need, " and " );
	}

	float max_block( const bool jumpbug, const float dz, const float max_speed )
	{
		if ( max_speed <= 0.f )
			return -1.f;
		/* worker reads g_num mid-solve */
		if ( !solve_busy( ) )
			refresh_engine_numbers( );
		const float dt  = tick_dt( );
		const float cap = convar_float( "sv_air_max_wishspeed", 30.f );
		const float a   = convar_float( "sv_airaccelerate", 12.f ) * max_speed * dt;
		const float dot = a >= cap ? 0.f : cap - a;

		float best = -1.f;
		for ( int variant = 0; variant < 3; ++variant ) {
			n_route::sim_t s{ };
			s.z         = jumpbug ? k_ground_catch - k_dist_eps : 0.f;
			s.on_ground = true;
			float vx = 1.1f * max_speed, vy = 0.f, x = 0.f;
			for ( int t = 0; t < air_tick_cap( ); ++t ) {
				const float z0 = s.z, x0 = x;
				sim_tick( s, t == 0, variant == 2 || ( variant == 1 && t >= 1 ), -1e9f, false );

				const float v = std::sqrt( vx * vx + vy * vy );
				const float c = clampf( dot / v, -1.f, 1.f ), sn = std::sqrt( 1.f - c * c );
				const float side = vy > 0.f ? -1.f : 1.f;
				const float wx = c * vx / v - side * sn * vy / v, wy = c * vy / v + side * sn * vx / v;
				const float add = ( std::min )( a, cap - ( vx * wx + vy * wy ) );
				if ( add > 0.f ) {
					vx += add * wx;
					vy += add * wy;
				}
				x += vx * dt;

				if ( z0 >= dz && s.z < dz && s.vz < 0.f ) {
					best = ( std::max )( best, x0 + ( x - x0 ) * ( z0 - dz ) / ( z0 - s.z ) + 32.f );
					break;
				}
			}
		}
		return best;
	}

	void dump_jump_matrix( )
	{
		refresh_engine_numbers( );
		constexpr float k_plane = 0.f;

		botox_dbg_log( "[rc jm] == jump matrix == impulse %.4f | dt %.6f | gravity/tick %.4f | bleed %.4f/tick | "
		                   "jump cost %.3f/u | land cost %.3f/u | stamina max %.1f\n",
		                   g_impulse, tick_dt( ), gravity_per_tick( ), stamina_per_tick( ), stamina_jump_cost( ),
		                   stamina_land_cost( ), stamina_max( ) );

		botox_dbg_log( "[rc jm] -- A: from a stop ( stamina 0, on the plane, standing ) --\n" );
		for ( const press_spec_t& m : k_first_moves ) {
			sim_t s{ };
			s.z = k_plane;
			jm_row( "-", m.name, run_leg( s, m, k_plane ), -1e30f );
		}

		float solo[ std::size( k_second_moves ) ]{ };
		botox_dbg_log( "[rc jm] -- B: the same presses with fresh legs ( the `vs solo` baseline ) --\n" );
		for ( std::size_t j = 0; j < std::size( k_second_moves ); ++j ) {
			sim_t s{ };
			s.z                = k_plane;
			const leg_result_t r = run_leg( s, k_second_moves[ j ], k_plane );
			solo[ j ]            = r.ok ? r.apex - r.dep_z : -1e30f;
			jm_row( "-", k_second_moves[ j ].name, r, -1e30f );
		}

		botox_dbg_log( "[rc jm] -- C: second press off each landing ( `vs solo` = what the first jump cost it ) --\n" );
		for ( const press_spec_t& first : k_first_moves ) {
			sim_t base{ };
			base.z                     = k_plane;
			const leg_result_t first_r = run_leg( base, first, k_plane );
			if ( !first_r.ok ) {
				botox_dbg_log( "[rc jm] %-20s -- never landed ( %s ), nothing can follow it\n", first.name, first_r.why );
				continue;
			}
			botox_dbg_log( "[rc jm] %-20s hands over: hover %5.3f  stamina %5.2f  stance %s\n", first.name, first_r.hover,
			                   first_r.stam_out, stance_name( first_r.ducked_out ) );
			for ( std::size_t j = 0; j < std::size( k_second_moves ); ++j ) {
				sim_t s = base;
				jm_row( first.name, k_second_moves[ j ].name, run_leg( s, k_second_moves[ j ], k_plane ), solo[ j ] );
			}
		}

		botox_dbg_log( "[rc jm] -- D: the levers on their own ( pressed with no wait, state set by hand ) --\n" );
		const press_spec_t levers[] = {
			{ "hop", style_jump, timing_bhop },
			{ "mj hop", style_minijump, timing_bhop },
			{ "lj hop", style_longjump, timing_bhop },
			{ "crouch hop (hold)", style_jump, timing_bhop, 1, true },
		};
		for ( const press_spec_t& m : levers ) {
			for ( int stance = 0; stance < 2; ++stance ) {
				for ( const float stam : { 0.f, 6.f, 12.f, 18.f, 24.f } ) {
					for ( const float hover : { 0.f, 1.f, 2.f } ) {
						if ( stance == 1 && hover > 0.f )
							continue;
						sim_t s{ };
						s.z         = k_plane + hover;
						s.on_ground = true;
						s.stamina   = stam;
						if ( stance == 1 ) {
							s.ducked      = true;
							s.duck_amount = 1.f;
							s.fl_ducking  = true;
							s.raw_duck    = true;
						}
						const leg_result_t r = run_leg( s, m, k_plane );
						char tag[ 64 ]{ };
						sprintf_s( tag, "stam %4.1f hover %3.1f %s", stam, hover, stance_name( stance == 1 ) );
						jm_row( tag, m.name, r, -1e30f );
					}
				}
			}
		}
		botox_dbg_log( "[rc jm] == end ==\n" );
	}
}
