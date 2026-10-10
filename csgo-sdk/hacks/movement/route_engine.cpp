#include "movement.h"
#include "edgebug.h"
#include "tick_scale.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "../../utilities/perf/perf_watch.h"
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
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
	constexpr float k_min_move_frac = 0.0001f; // MINIMUM_MOVE_FRACTION

	/* TracePlayerBBox vs one box brush face, z only ( the route is 1-D ), float for float: Ray_t::Init centres the hull
	   ( cmodel.h:88 ), IntersectRayWithBoxBrush ( cmodel.cpp:936 ) hits only a move that crosses the TRUE face and enters
	   DIST_EPSILON early, CM_ComputeTraceEndpoints ( cmodel.cpp:2254 ) un-centres and adds frac * delta. an end inside
	   the 1/32 skin is no hit; the next move into the face is then frac 0. up = face over the head ( box mins ) */
	struct ztrace_t {
		bool hit   = false;
		float frac = 1.f;
		float end  = 0.f;
	};
	ztrace_t trace_z( float z, float to, float hull, float face, bool up )
	{
		const float half  = hull * 0.5f;
		const float start = z + half;
		const float delta = to - z;
		const float off   = up ? ( face - start ) - half : ( face - start ) + half;
		ztrace_t t;
		if ( up ? 0.f < off && !( delta < off ) : 0.f > off && !( delta > off ) ) {
			const float f = ( up ? off - k_dist_eps : off + k_dist_eps ) * ( 1.f / delta );
			t.hit  = true;
			t.frac = f > 0.f ? f : 0.f;
		}
		t.end = t.hit ? start + -half + t.frac * delta : start + -half + delta;
		return t;
	}

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
		if ( !solid || s.vz > k_non_jump_velocity || ( skin && ( over <= n_route::k_skin_lo || over >= n_route::k_skin_hi ) ) )
			return;
		if ( skin ) {
			s.on_ground = true;
			s.vz        = 0.f;
			return;
		}
		/* CategorizePosition ( gamemovement.cpp:4609 ): hull traced 2 down ( + step size while grounded ), grounded = it
		   hits the floor face ( floor_z = standing origin, face 1/32 under it ). snaps only for 0 < frac < 1 */
		const float face = floor_z - k_dist_eps;
		const float to   = move_to_end ? ( s.z - k_ground_catch ) - k_step_size : s.z - k_ground_catch;
		const ztrace_t t = trace_z( s.z, to, hull_top( s ), face, false );
		const bool in_face = !( s.z > face ); /* never in a 1-D arc: grounded + put on top, as before */
		if ( !t.hit && !in_face )
			return;
		s.on_ground = true;
		s.vz        = 0.f;
		if ( move_to_end && in_face )
			s.z = floor_z;
		else if ( move_to_end && t.frac > 0.f )
			s.z = t.end;
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

	/* walk = a ground tick while moving ( delay hop wait ): WalkMove runs StayOnGround */
	void sim_tick( n_route::sim_t& s, bool jump, bool duck_held, float floor_z, bool solid, bool skin = false, bool walk = false )
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
			/* StayOnGround ( gamemovement.cpp:2494 ): up 2, down to step size under; a hull more than 1/64 off the floor face's
			   stop goes onto it. a landing inside the 1/32 skin stays there otherwise ( CategorizePosition's frac is 0 ) */
			if ( walk && solid && !skin ) {
				const float up    = trace_z( s.z, s.z + 2.f, hull_top( s ), 3.4e38f, true ).end;
				const ztrace_t dn = trace_z( up, s.z - k_step_size, hull_top( s ), floor_z - k_dist_eps, false );
				if ( dn.hit && dn.frac > 0.f && std::fabs( s.z - dn.end ) > 0.5f / 32.f )
					s.z = dn.end;
			}
		} else {
			/* TryPlayerMove ( gamemovement.cpp:3266 ), z: one trace into the roof face ( roof = its head trace's end, face 1/32
			   over ) or the floor face. a tiny fraction moves nothing, a hit clips vz to 0 ( ClipVelocity, axial face,
			   overbounce 1 ); the rest of the tick and bumps 2-4 go sideways */
			const bool up    = s.vz > 0.f;
			const float face = up ? s.roof + k_dist_eps : ( solid && !skin ? floor_z - k_dist_eps : -3.4e38f );
			const ztrace_t t = trace_z( s.z, s.z + tick_dt( ) * s.vz, hull_top( s ), face, up );
			if ( !t.hit || t.frac >= k_min_move_frac )
				s.z = t.end;
			if ( t.hit ) {
				s.vz = 0.f;
				if ( up ) {
					s.roof_hit = true;
					s.ceiling  = ( std::min )( s.ceiling, s.roof );
				}
			}
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

	std::string departure_name( int style, int timing, int delay_ticks, bool ducked_in, bool hold_crouch );
	std::string arrival_name( int type, bool ducked );

	/* route elements as the few values that name them: a string per arc was 3 heap blocks and most of the memory */
	struct dep_t {
		unsigned char style = 0, timing = 0, delay = 0;
		bool from_ground = false, hop_only = false, press_ducked = false, hold_crouch = false;
		std::string text( ) const
		{
			if ( !from_ground )
				return hold_crouch ? "fall (ducked)" : "fall";
			return ( hop_only ? "(pixel) " : "" ) + departure_name( style, timing, delay, press_ducked, hold_crouch );
		}
	};
	struct arr_t {
		enum : unsigned char { none, named, ground_ducked } kind = none;
		unsigned char type = 0, late = 0;
		bool ducked        = false;
		std::string text( ) const
		{
			if ( kind == ground_ducked )
				return "ground (ducked)";
			std::string label = arrival_name( type, ducked );
			/* later push-in tick: "Nt late", never "+Nt" ( read as a delayed press ) */
			if ( late > 0 ) {
				char suffix[ 16 ]{ };
				sprintf_s( suffix, " %dt late", static_cast< int >( late ) );
				label += suffix;
			}
			return label;
		}
	};
	arr_t arr_named( int type, bool ducked, int late = 0 )
	{
		arr_t a;
		a.kind   = arr_t::named;
		a.type   = static_cast< unsigned char >( type );
		a.ducked = ducked;
		a.late   = static_cast< unsigned char >( late );
		return a;
	}

	struct leg_t {
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

	/* live counts for the split budget: free_virtual never comes back after a free ( the heap keeps the pages ) */
	std::atomic< long > g_live_steps{ 0 }, g_live_alts{ 0 };
	template < std::atomic< long >& N >
	struct live_t {
		live_t( ) { N.fetch_add( 1, std::memory_order_relaxed ); }
		live_t( const live_t& ) { N.fetch_add( 1, std::memory_order_relaxed ); }
		live_t& operator=( const live_t& ) { return *this; }
		~live_t( ) { N.fetch_sub( 1, std::memory_order_relaxed ); }
	};

	struct step_t;
	/* a merged twin's way in, as pointers: prefix = head's prefix + path's elements from `from` on ( head null =
	   all of path's ). listed as prefix + the lead's elements from len on. strings are only built to compare
	   or list ( materialized prefixes were most of the memory on big routes ) */
	struct alt_t {
		std::shared_ptr< const alt_t > head{ };
		std::shared_ptr< const step_t > path{ };
		std::size_t from        = 0;
		std::size_t len         = 0;
		unsigned long long hash = 0; /* of route_text( prefix ), continued per merge without building it */
		bool empty              = false; /* route_text( prefix ) == "" */
		live_t< g_live_alts > live{ };
	};
	/* persistent list: a merge into a twin that shares its list stacks a link on top, never copies the list
	   ( a copy per twin was most of the memory on big routes ) */
	struct alt_list_t {
		std::shared_ptr< const alt_list_t > base{ };
		std::vector< std::shared_ptr< const alt_t > > items{ };
		std::size_t size = 0; /* base's + items */
		/* text hashes of items while its point folds, once there are enough that a scan of items would be quadratic */
		std::unordered_set< unsigned long long > hashes{ };
		static constexpr std::size_t k_scan = 32;
		bool has( unsigned long long h ) const
		{
			if ( items.size( ) <= k_scan )
				return std::any_of( items.begin( ), items.end( ), [ & ]( const auto& a ) { return a->hash == h; } );
			return hashes.count( h ) != 0;
		}
		void push( std::shared_ptr< const alt_t > a )
		{
			items.push_back( std::move( a ) );
			++size;
			if ( items.size( ) == k_scan + 1 )
				for ( const auto& i : items )
					hashes.insert( i->hash );
			else if ( items.size( ) > k_scan + 1 )
				hashes.insert( items.back( )->hash );
		}
		template < class F >
		void each( F&& f ) const
		{
			if ( base )
				base->each( f );
			for ( const auto& a : items )
				f( a );
		}
	};

	/* one leg + its 1..2 route elements ( dep, then arrival unless none ). children share the parent's chain:
	   a node never copies its path */
	struct step_t {
		std::shared_ptr< const step_t > up{ };
		leg_t leg{ };
		dep_t dep{ };
		arr_t arr{ };
		bool roof = false;
		/* whole path's route_text hash ( empty = no text yet ) + summed element lengths, without building it */
		unsigned long long hash = 14695981039346656037ull;
		bool empty              = true;
		std::size_t text_len    = 0;
		live_t< g_live_steps > live{ };
		int n_el( ) const { return arr.kind == arr_t::none ? 1 : 2; }
		std::string name( ) const { return roof ? dep.text( ) + " [roof]" : dep.text( ); }
	};

	/* another state the same presses can leave you in: a box top near the apex lands on whichever tick the hull
	   crosses onto it ( 10-10 log: one lj hop landed -112.179 and -111.507 on two tries ) */
	struct shadow_t {
		n_route::sim_t st{ };
		bool jb_ok = false;
		n_route::sim_t jb{ };
	};

	struct node_t {
		n_route::sim_t st{ };
		/* st + others = every state this route can be in; odds = share of landings it still works from */
		std::vector< shadow_t > others{ };
		float odds = 1.f;
		std::shared_ptr< const step_t > path{ };
		/* only the serial admit writes it, and only while it is the sole owner */
		std::shared_ptr< alt_list_t > alts{ };
		int n_legs = 0;
		int n_els  = 0;
		int cost = 0;
		float floor       = 0.f;
		unsigned int mask = n_route::k_all_styles;
		/* rows this node lists: the lead + every merged twin's, same text once ( alts holds only the shown few ) */
		long long combos = 1;
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

	std::vector< std::string > path_elements( const step_t* path )
	{
		std::size_t n = 0;
		for ( const step_t* s = path; s; s = s->up.get( ) )
			n += static_cast< std::size_t >( s->n_el( ) );
		std::vector< std::string > out( n );
		for ( const step_t* s = path; s; s = s->up.get( ) ) {
			if ( s->arr.kind != arr_t::none )
				out[ --n ] = s->arr.text( );
			out[ --n ] = s->dep.text( );
		}
		return out;
	}
	std::vector< std::string > elements_of( const node_t& n ) { return path_elements( n.path.get( ) ); }

	std::vector< std::string > alt_prefix( const alt_t& a )
	{
		std::vector< std::string > out = a.head ? alt_prefix( *a.head ) : std::vector< std::string >{ };
		const std::vector< std::string > els = path_elements( a.path.get( ) );
		out.insert( out.end( ), els.begin( ) + static_cast< std::ptrdiff_t >( a.from ), els.end( ) );
		return out;
	}
	/* fnv-1a 64, resumable: text_hash( b, text_hash( a ) ) == text_hash( a + b ) */
	unsigned long long text_hash( const std::string& text, unsigned long long h = 14695981039346656037ull )
	{
		for ( const unsigned char c : text )
			h = ( h ^ c ) * 1099511628211ull;
		return h;
	}
	/* route_text maps each element on its own, so the path's text hash continues per step */
	void stamp_text( step_t& s )
	{
		if ( const step_t* up = s.up.get( ) ) {
			s.hash     = up->hash;
			s.empty    = up->empty;
			s.text_len = up->text_len;
		}
		const auto add = [ & ]( const std::string& e ) {
			s.text_len += e.size( );
			const std::string t = n_route::route_text( { e } );
			if ( t.empty( ) )
				return;
			if ( !s.empty )
				s.hash = text_hash( " -> ", s.hash );
			s.hash  = text_hash( t, s.hash );
			s.empty = false;
		};
		add( s.dep.text( ) );
		if ( s.arr.kind != arr_t::none )
			add( s.arr.text( ) );
	}
	/* alts past keep_max are never listed ( the list stops at max_results ): counted in combos only */
	void merge_alts( node_t& keep, const node_t& drop, std::size_t keep_max )
	{
		const auto full = [ & ]( ) { return keep.alts && keep.alts->size >= keep_max; };
		if ( full( ) )
			return;
		std::vector< std::string > drop_els; /* built on the first alt that needs a tail */
		const std::size_t len              = static_cast< std::size_t >( keep.n_els );
		const unsigned long long keep_hash = keep.path ? keep.path->hash : text_hash( "" );
		/* route_text( drop_els from i on ), built once per i: route_text( a + b ) = text a + " -> " + text b ( either
		   empty = no arrow; no element's text is empty ), so an alt's hash continues over this tail alone */
		std::vector< std::string > tails( static_cast< std::size_t >( drop.n_els ) + 1 );
		std::vector< char > tail_done( tails.size( ), 0 );
		const auto tail = [ & ]( std::size_t from ) -> const std::string& {
			if ( !tail_done[ from ] ) {
				if ( drop_els.empty( ) )
					drop_els = elements_of( drop );
				tails[ from ]     = n_route::route_text( std::vector< std::string >( drop_els.begin( ) + static_cast< std::ptrdiff_t >( from ), drop_els.end( ) ) );
				tail_done[ from ] = 1;
			}
			return tails[ from ];
		};
		/* only this point's links can hold a duplicate: a link from an earlier point holds shorter routes ( smaller len ) */
		const auto here = [ & ]( const alt_list_t* l ) { return l && !l->items.empty( ) && l->items.front( )->len == len; };
		/* this point's link, the sole owner's to append to; else stacked on the shared list */
		const auto top = [ & ]( ) -> alt_list_t& {
			if ( keep.alts.use_count( ) != 1 || !here( keep.alts.get( ) ) ) {
				auto link  = std::make_shared< alt_list_t >( );
				link->size = keep.alts ? keep.alts->size : 0;
				link->base = std::move( keep.alts );
				keep.alts  = std::move( link );
			}
			return *keep.alts;
		};
		/* same 64 bit text hash = same text ( the lead's, or an alt's of this len ): a collision needs ~2^32 alts on one state */
		const auto offer = [ & ]( std::shared_ptr< const alt_t > head, std::size_t from, unsigned long long hash, bool empty ) {
			if ( hash == keep_hash || full( ) )
				return;
			for ( const alt_list_t* l = keep.alts.get( ); here( l ); l = l->base.get( ) )
				if ( l->has( hash ) )
					return;
			top( ).push( std::make_shared< const alt_t >( alt_t{ std::move( head ), drop.path, from, len, hash, empty } ) );
		};
		offer( nullptr, 0, drop.path ? drop.path->hash : text_hash( "" ), !drop.path || drop.path->empty );
		if ( drop.alts )
			drop.alts->each( [ & ]( const std::shared_ptr< const alt_t >& a ) {
				if ( full( ) )
					return;
				const std::string& t = tail( a->len );
				unsigned long long h = a->hash;
				if ( !a->empty && !t.empty( ) )
					h = text_hash( " -> ", h );
				offer( a, a->len, text_hash( t, h ), a->empty && t.empty( ) );
			} );
	}

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

	// float bits: dedup merges exact states only. a merged twin is listed as an alt of the lead, so
	// its future must be the lead's bit for bit. near-only merges (duck gate, low stamina) fly their own arcs
	unsigned int bits( float f )
	{
		unsigned int u = 0u;
		memcpy( &u, &f, sizeof( u ) );
		return u;
	}
	/* dedup key = exact sim state + jumpbug origin + bind history. crouch tier never merges into a crouch-free
	   twin: the crouch-free search stays the pre-crouch one to the node */
	/* extra = other states + odds ( 0 for a one-state route, so those keys are what they always were ) */
	struct node_key_t {
		unsigned int w[ 15 ]{ };
		unsigned long long extra = 0;
		bool operator==( const node_key_t& o ) const { return extra == o.extra && memcmp( w, o.w, sizeof( w ) ) == 0; }
	};
	struct node_key_hash_t {
		std::size_t operator( )( const node_key_t& k ) const
		{
			std::size_t h = 2166136261u;
			for ( const unsigned int v : k.w )
				h = ( h ^ v ) * 16777619u;
			return h ^ static_cast< std::size_t >( k.extra ^ ( k.extra >> 32 ) );
		}
	};
	node_key_t state_key( const node_t& n, const n_route::sim_t& st, bool jb_ok, const n_route::sim_t& jb )
	{
		const float since = ( std::min )( st.time - st.last_duck_time, 1.f );
		const unsigned int flags = ( n.crouch_only ? 1u : 0u ) | ( n.crouches > 0 ? 2u : 0u ) | ( st.fl_ducking ? 4u : 0u ) |
		                           ( st.raw_duck ? 8u : 0u ) | ( st.ducking ? 16u : 0u ) | ( n.hop_only ? 32u : 0u ) |
		                           ( n.skin ? 64u : 0u ) | ( st.on_player ? 128u : 0u ) | ( st.ducked ? 256u : 0u ) |
		                           ( st.on_ground ? 512u : 0u ) | ( jb_ok ? 1024u : 0u );
		return { { n.bind_mask, static_cast< unsigned int >( n.last_bind ), flags, bits( st.z ), bits( st.vz ), bits( st.stamina ),
		           bits( st.duck_amount ), bits( st.duck_speed ), bits( since ), bits( duck_recover_since( st ) ), bits( st.fall ),
		           bits( st.ceiling ), jb_ok ? bits( jb.z ) : 0u, jb_ok ? bits( jb.vz ) : 0u, jb_ok ? bits( jb.stamina ) : 0u } };
	}
	unsigned long long key_hash( const node_key_t& k )
	{
		unsigned long long h = 14695981039346656037ull;
		for ( const unsigned int v : k.w )
			h = ( h ^ v ) * 1099511628211ull;
		return h;
	}
	node_key_t node_key( const node_t& n )
	{
		node_key_t k = state_key( n, n.st, n.jb_ok, n.jb );
		if ( n.others.empty( ) && n.odds >= 1.f )
			return k;
		/* ponytail: 64 bit set hash, a false merge needs a collision between two state sets of one point */
		std::vector< unsigned long long > hs;
		hs.reserve( n.others.size( ) );
		for ( const shadow_t& o : n.others )
			hs.push_back( key_hash( state_key( n, o.st, o.jb_ok, o.jb ) ) );
		std::sort( hs.begin( ), hs.end( ) );
		unsigned long long h = 14695981039346656037ull ^ bits( n.odds );
		for ( const unsigned long long v : hs )
			h = ( h ^ v ) * 1099511628211ull;
		k.extra = h | 1ull;
		return k;
	}

	/* no beam: a big enough route outgrows the 32 bit address space. stop the solve while the game still has
	   room to allocate, never crash it. floor is relative to what was free at solve start: csgo often sits under
	   512 MB free ( 10-10 log: a fixed 512 MB floor tripped on 81 arcs in 5 ms ) */
	unsigned long long free_virtual( )
	{
		MEMORYSTATUSEX ms{ };
		ms.dwLength = sizeof( ms );
		return GlobalMemoryStatusEx( &ms ) ? ms.ullAvailVirtual : ~0ull;
	}
	unsigned long long memory_floor( unsigned long long start_free )
	{
		return ( std::min )( 512ull << 20, ( std::max )( 64ull << 20, start_free / 2 ) );
	}
	unsigned int physical_cores( )
	{
		DWORD len = 0;
		GetLogicalProcessorInformationEx( RelationProcessorCore, nullptr, &len );
		std::vector< unsigned char > buf( len );
		auto* info = reinterpret_cast< SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* >( buf.data( ) );
		if ( len == 0 || !GetLogicalProcessorInformationEx( RelationProcessorCore, info, &len ) )
			return ( std::max )( 1u, std::thread::hardware_concurrency( ) / 2u );
		unsigned int n = 0;
		for ( DWORD at = 0; at < len; at += reinterpret_cast< SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* >( buf.data( ) + at )->Size )
			++n;
		return ( std::max )( 1u, n );
	}

	bool low_memory_now( unsigned long long floor )
	{
		const unsigned long long now = free_virtual( );
		if ( now >= floor )
			return false;
		botox_dbg_log( "[rc mem] %llu MB free < floor %llu MB - stopping\n", now >> 20, floor >> 20 );
		return true;
	}

	/* closest-first log rows, only the shown ones kept: every arc used to format and hold its line */
	struct top_t {
		std::size_t cap = 0;
		long long count = 0;
		std::vector< std::pair< float, std::string > > rows{ };
		/* counts the row; false = it can't make the list ( a tie keeps the earlier row, as a stable sort does ) */
		bool counts( float gap )
		{
			++count;
			return rows.size( ) < cap || gap < rows.back( ).first;
		}
		bool full( ) const { return rows.size( ) >= cap; }
		void add( float gap, std::string line )
		{
			const auto at = std::upper_bound( rows.begin( ), rows.end( ), gap, []( float g, const auto& r ) { return g < r.first; } );
			rows.insert( at, { gap, std::move( line ) } );
			if ( rows.size( ) > cap )
				rows.pop_back( );
		}
		/* o's rows all come after ours in frontier order */
		void merge( top_t& o )
		{
			count += o.count;
			for ( auto& r : o.rows )
				add( r.first, std::move( r.second ) );
		}
	};

	/* one frontier node's expansion, made on any thread, folded in frontier order */
	struct expand_t {
		std::vector< node_t > made{ };
		std::vector< node_key_t > keys{ };
		std::vector< unsigned long long > sigs{ }; /* per made: the press + arrival the player sees */
		long long opts = 0;
		float best_gap = FLT_MAX, closest_z = 0.f;
		unsigned int blocked = 0u, refused = 0u;
		float best_by_presses[ 16 ]{ FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX,
		                             FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
		top_t misses{ 12 };
		top_t edge{ 8 };
	};

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
			sim_tick( s, false, false, floor_z, true, false, m.timing == timing_delay );
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

	void solve_search( const solve_input_t& in, solve_output_t& out )
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

		/* no beam: every arc is kept, so searched == space */
		std::vector< long double > branch_max;
		const auto commit_counts = [ & ]( ) {
			long double space = 1.L;
			for ( const long double b : branch_max )
				space *= b;
			out.space    = space;
			out.searched = space;
		};

		/* warm the static convar lookup in IsEdgeBugTick here: /Zc:threadSafeInit- statics race on the workers */
		(void)g_edgebug.IsEdgeBugTick( 0.f, 0.f, false, false );
		const int lj_air_ticks = lj_bind_air_ticks( );
		/* helpers = PHYSICAL cores - 2, this thread the other one: 10-10 log, logical - 2 ( 10 of 12 ) took every SMT
		   sibling and the game's own threads shared cores ( frames 16 -> 20..45 ms while solving ) */
		const unsigned int cores   = physical_cores( );
		const unsigned int helpers = ( std::min )( cores > 2u ? cores - 2u : 0u, 64u );
		const unsigned long long start_free = free_virtual( );
		const unsigned long long mem_floor  = memory_floor( start_free );
		/* logged only on a trip: free MB differs run to run, byte-exact gates replay this log */
		const auto low_memory = [ & ]( unsigned long long floor ) {
			if ( !low_memory_now( floor ) )
				return false;
			botox_dbg_log( "[rc mem] %llu MB free at start, floor %llu MB\n", start_free >> 20, floor >> 20 );
			return true;
		};

		/* deque: a million arcs never need one contiguous block of the 32 bit address space */
		std::deque< node_t > root;
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
		root.emplace_back( seed );

		// one marker = one move, never a hidden hop. a point one move can't reach is reported unreachable
		const std::vector< route_point_t >& points = in.points;
		const auto point_label = [ ]( std::size_t p ) {
			char buf[ 24 ]{ };
			sprintf_s( buf, "point %d", static_cast< int >( p ) + 1 );
			return std::string( buf );
		};
		int deepest = -1;

		std::size_t pending = 0; /* arcs in halves waiting on the split stack */
		int splits          = 0;
		int trims           = 0; /* harder halves dropped ( out.trimmed ) */
		bool cancelled      = false;
		/* time limit: past it the solve stops and lists the combos finished ( timed_out ) */
		const auto t_start = std::chrono::steady_clock::now( );
		const auto past    = [ & ]( int ms ) {
			return in.time_limit_ms > 0 && std::chrono::steady_clock::now( ) - t_start >= std::chrono::milliseconds( ms );
		};
		bool timed_out = false;
		/* progress share of one run ( halves split it ), and its own growth estimate */
		struct span_t {
			int lo = 0, hi = 999;
			long double done       = 0.L;
			std::size_t prev_width = 1;
		};
		/* split solves: dead halves keep the deepest point's misses, the rest is summed / maxed per point */
		int dead_p = -1;
		top_t dead_misses{ 12 };
		bool reach_set = false;

		/* the list, across halves: first-come dedup by text, the cap best rows kept in list order */
		const int cap = in.max_results > 0 ? in.max_results : 1;
		/* every merged row is held and listed while that's cheap ( exact count ). past exact_alts held ( 10-10: 11 ground
		   markers = 8.7M rows, 45 s listing them to show 6 ) rows are counted per arc ( combos ), a node keeps twice the
		   shown rows ( an alt the list skips as a repeat leaves room for the next ) and the list stops once full */
		bool approx          = false;
		std::size_t alt_keep = SIZE_MAX;
		using row_key_t = std::tuple< int, int, float, int, int, int, int, int, int, int, int, std::size_t, long long >;
		struct row_t {
			row_key_t key{ };
			solution_t sol{ };
			std::string line{ }; /* [rc list] after the row number, empty = no path */
			std::shared_ptr< const step_t > lead{ };
			int lead_legs = 0;
		};
		std::vector< row_t > rows;
		std::unordered_set< unsigned long long > printed, outcomes;
		long long row_seq      = 0;
		int alt_rows           = 0;
		std::size_t rows_seen  = 0;
		/* exact = per listed text ( outcomes ), counted = per arc ( combos ); approx picks which one is reported */
		long long alts_held = 0, combos_total = 0;
		long long reach_rows[ 16 ]{ }, reach_combos[ 16 ]{ };
		bool finals            = false;

		/* too many arcs for the room: past 3/4 of it a point's expansion is dropped and its frontier solved in two halves,
		   one after the other, down to single arcs. slower ( a half redoes the merges the whole shared ), out of memory
		   only past the floor. freed memory stays with the heap ( free_virtual never comes back ), so the budget is the
		   counted arcs, scaled by heap bytes per counted byte as measured before the first split */
		const double room = start_free > mem_floor ? static_cast< double >( start_free - mem_floor ) : 0.0;
		double heap_ratio = 1.0;
		unsigned long long low_seen = start_free;
		const char* split_why = "memory";
		const auto over = [ & ]( std::size_t n_front, std::size_t nodes ) {
			/* past half the time limit: chunks of <= 1024 arcs run to the last point one after the other, so whole combos
			   finish before the stop instead of one wide point nobody gets to the end of */
			constexpr std::size_t k_late_chunk = 1024;
			if ( n_front > k_late_chunk && past( in.time_limit_ms / 2 ) ) {
				split_why = "time";
				return true;
			}
			split_why = "memory";
			const double counted = static_cast< double >( nodes ) * sizeof( node_t ) +
			                       static_cast< double >( g_live_steps.load( std::memory_order_relaxed ) ) * ( sizeof( step_t ) + 24.0 ) +
			                       static_cast< double >( g_live_alts.load( std::memory_order_relaxed ) ) * ( sizeof( alt_t ) + 32.0 ) +
			                       static_cast< double >( printed.size( ) + outcomes.size( ) ) * 40.0;
			const unsigned long long now = free_virtual( );
			const double used            = static_cast< double >( start_free > now ? start_free - now : 0ull );
			if ( splits + trims == 0 ) {
				if ( counted > room / 8.0 )
					heap_ratio = ( std::min )( 4.0, ( std::max )( heap_ratio, used / counted ) );
				low_seen = now;
				return used > room * 0.75;
			}
			/* the heap grew past its low water since: the ratio was short */
			if ( now + static_cast< unsigned long long >( room / 16.0 ) < low_seen ) {
				low_seen = now;
				heap_ratio *= 1.25;
			}
			return counted * heap_ratio > room * 0.7;
		};

		enum : int { k_next, k_dead, k_split };
		const auto point_step = [ & ]( std::deque< node_t >& frontier, const std::size_t p, span_t& span ) -> int {
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

			std::deque< node_t > next;
			/* dedup by 64 bit key hash, the full key checked against the arc it points at ( a 68 byte key per entry was
			   ~100 B of map per arc, a third of the room ). a real collision goes to seen_rare */
			std::unordered_map< unsigned long long, unsigned int > seen;
			std::unordered_map< node_key_t, std::size_t, node_key_hash_t > seen_rare;
			const auto key_id = []( const node_key_t& key ) { return key_hash( key ) ^ ( key.extra * 0x9E3779B97F4A7C15ull ); };
			const auto twin_of = [ & ]( const node_key_t& key ) -> std::size_t {
				const auto it = seen.find( key_id( key ) );
				if ( it == seen.end( ) )
					return SIZE_MAX;
				if ( node_key( next[ it->second ] ) == key )
					return it->second;
				const auto rare = seen_rare.find( key );
				return rare == seen_rare.end( ) ? SIZE_MAX : rare->second;
			};
			const auto remember = [ & ]( const node_key_t& key, std::size_t at ) {
				if ( !seen.emplace( key_id( key ), static_cast< unsigned int >( at ) ).second )
					seen_rare.emplace( key, at );
			};
			std::vector< long long > fan;
			fan.reserve( frontier.size( ) );
			top_t misses{ 12 };
			top_t edge{ 8 };
			/* misses' 12th best so far: a worker never formats a line that can't make the list */
			std::atomic< float > miss_cut{ FLT_MAX };
			float best_by_presses[ 16 ];
			for ( float& b : best_by_presses )
				b = FLT_MAX;

			/* progress = nodes expanded / ( done + this point + later points at this point's growth ) */
			const long double width = static_cast< long double >( frontier.size( ) );
			long double later       = 0.L;
			{
				const long double grow = ( std::max )( 1.L, width / static_cast< long double >( span.prev_width ) );
				long double w          = width;
				for ( std::size_t j = p + 1; j < points.size( ); ++j ) {
					w *= grow;
					later += w;
				}
			}

			/* any thread: one node's arcs into its own slot. nothing shared is written here */
			/* one state's arcs ( keys come after expand( ) groups the states ) */
			const auto expand_one = [ & ]( const node_t& node, expand_t& r ) {
				const bool node_ducked = node.st.ducked || node.st.ducking;
				/* roof pass whose head never hit the roof = the open pass's arc again: dropped, not counted twice */
				bool roof_pass = false;
				int cur_pass   = 0;
				const auto admit = [ & ]( node_t& made ) {
					if ( roof_pass && !made.st.roof_hit )
						return;
					++r.opts;
					made.combos = node.combos;
					made.alts  = node.alts;
					const dep_t& d = made.path->dep;
					const arr_t& a = made.path->arr;
					r.sigs.emplace_back( static_cast< unsigned long long >( d.style ) | static_cast< unsigned long long >( d.timing ) << 4 |
					                     static_cast< unsigned long long >( d.delay ) << 8 | static_cast< unsigned long long >( d.from_ground ) << 16 |
					                     static_cast< unsigned long long >( d.hop_only ) << 17 | static_cast< unsigned long long >( d.press_ducked ) << 18 |
					                     static_cast< unsigned long long >( d.hold_crouch ) << 19 | static_cast< unsigned long long >( cur_pass ) << 20 |
					                     static_cast< unsigned long long >( a.kind ) << 28 | static_cast< unsigned long long >( a.type ) << 32 |
					                     static_cast< unsigned long long >( a.ducked ) << 40 | static_cast< unsigned long long >( a.late ) << 48 |
					                     static_cast< unsigned long long >( made.path->roof ) << 56 );
					r.made.emplace_back( std::move( made ) );
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
						if ( node.n_legs != 0 )
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
						if ( timing != timing_rest && node.n_legs == 0 && in.start_on_ground )
							continue;
						if ( timing == timing_rest && node.st.on_ground && node.n_legs != 0 &&
						     style != style_walk_off )
							continue;
						if ( style == style_jumpbug && node.n_legs == 0 )
							continue;
						if ( node.hop_only && timing != timing_bhop )
							continue;
						if ( node_ducked && timing != timing_rest && style == style_minijump )
							continue;

						if ( const unsigned int bit = 1u << move_of( style, timing, node_ducked );
						     node.st.on_ground && !( in.global_moves & bit ) ) {
							r.blocked |= bit;
							r.refused |= bit;
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
							cur_pass              = pass;
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
									r.refused |= 1u << move_crouch_hop;
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
								sim_tick( s, false, false, node.floor, true, false, timing == timing_delay );
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
								r.refused |= 1u << move_of( style, timing, true );
								continue;
							}
							/* the style's +duck must pass the gate, else it's a standing jump mislabeled */
							if ( press_duck && !press_ducked && !duck_press_lands( s ) )
								continue;

							if ( ceil_over )
								s.ceiling = ( std::min )( s.ceiling, target_z );
							/* a headbang is TryPlayerMove into the ceiling: the marker is the head's stop, like a leg roof */
							s.roof     = point.type == route_pt_headbang ? target_z : ( under_roof ? point.roof : 3.4e38f );
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

							dep_t dep;
							dep.style        = static_cast< unsigned char >( style );
							dep.timing       = static_cast< unsigned char >( timing );
							dep.delay        = static_cast< unsigned char >( delay_n );
							dep.from_ground  = node.st.on_ground;
							dep.hop_only     = node.hop_only;
							dep.press_ducked = press_ducked;
							dep.hold_crouch  = hold_crouch == 1;
							const int leg_cost = timing_cost( style, timing, delay_n );

							float attempt_gap = FLT_MAX;
							float attempt_z   = 0.f;
							float apex        = s.z;
							bool made_any     = false;

							/* arrival = the route element after dep ( kind none = no element; an empty name still counts as one ) */
							const auto push_leg = [ & ]( node_t& made, const arr_t& arrival, float arrive_z, float fall, float hover,
							                            float gap, int tier = 0 ) {
								auto step  = std::make_shared< step_t >( );
								step->up   = node.path;
								step->dep  = dep;
								step->arr  = arrival;
								step->roof = made.st.roof_hit;
								stamp_text( *step );
								leg_t& leg   = step->leg;
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
								made.n_legs  = node.n_legs + 1;
								made.n_els   = node.n_els + step->n_el( );
								made.path    = std::move( step );
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
							/* s just hit the ceiling ( roof_hit ): the move stopped there and vz clipped. gap = head at tick start */
							const auto try_bang = [ & ]( float head_in ) {
								const bool ducked = s.ducked;
								if ( ( ducked && !point.allow_duck ) || ( !ducked && !point.allow_stand ) || ( duck_air && !ducked ) )
									return;

								node_t made;
								made.st           = s;
								made.st.vz        = eb_null;
								made.st.on_ground = false;
								made.st.fall      = -eb_null;
								made.st.ceiling   = target_z;
								made.st.roof_hit  = false;
								made.cost         = node.cost + leg_cost;
								push_leg( made, arr_named( shown_type, ducked ), made.st.z, 0.f, 0.f, std::fabs( head_in - target_z ) );
								admit( made );
								made_any = true;
							};
							const bool launch_bang = point.type == route_pt_headbang && s.roof_hit;
							if ( duck_air && launch_bang )
								continue;
							if ( launch_bang )
								try_bang( s_launch_in.z + hull_top( s_launch_in ) );

							/* the floor catches a move that starts over its face ( 1/32 under the marker ), not over the marker */
							const auto armed = [ & ]( ) { return ( skin ? s.z >= target_z : s.z > target_z - k_dist_eps ) && s.vz - half_g < 0.f; };
							bool above = armed( );
							int creep_ticks = 0;
							bool lip_missed = false;
							int tb_catches = 0, tb_first = -1;

							const auto land_floor = [ & ]( const sim_t& st, const sim_t& st_in, float vz_in, bool held_now ) {
								node_t made;
								made.st           = st;
								made.st.on_player = point.ent > 0;
								made.cost         = node.cost + leg_cost;
								arr_t arrival;
								if ( point.type == route_pt_pixeljump ) {
									if ( last )
										arrival = arr_named( point.type, st.ducked || st.ducking );
									made.hop_only = true;
									made.skin     = skin;
								} else if ( point.type == route_pt_ground && last && duck_air )
									arrival.kind = arr_t::ground_ducked;
								made.crouch_only = duck_air && plain_arrived && point.type == route_pt_ground;
								if ( !held_now && vz_in < 0.f ) {
									made.jb_ok        = jumpbug_band( st_in, target_z, made.jb, skin );
									made.jb.on_player = made.st.on_player;
								}
								constexpr float k_skin_last_push = 0.98f * ( 0.03125f / 16.f - n_route::k_creep_gap );
								const int tier = skin && st.z - target_z < n_route::k_skin_lo + k_skin_last_push ? 1 : 0;
								push_leg( made, arrival, st.z, -vz_in, st.z - target_z, st.z - target_z, tier );
								admit( made );
								made_any = true;
							};

							for ( int i = 0, cap = launch_bang ? 0 : air_tick_cap( ); i < cap; ++i ) {
								bool held = hold_duck;
								if ( held && style == style_longjump && hold_crouch == 0 && i >= lj_air_ticks )
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

								/* every rising tick in reach can land: the one you get is the tick you cross onto the box */
								if ( !above_used && !s.on_ground && s_tick_in.vz - half_g > 0.f && lip_in &&
								     ( point.type == route_pt_ground || point.type == route_pt_pixeljump ) ) {
									sim_t probe = s_tick_in;
									sim_tick( probe, false, held, target_z, true, skin );
									if ( probe.on_ground && probe.z >= target_z && probe.ducked == s_tick_in.ducked &&
									     ( !duck_air || probe.ducked || probe.ducking ) ) {
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
									if ( gap < r.best_gap ) {
										r.best_gap  = gap;
										r.closest_z = report_z;
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
										if ( tier > 0 && r.edge.counts( rank_gap ) ) {
											char line[ 160 ]{ };
											sprintf_s( line, "%-18s%s catch %9.4f  gap %6.4f", dep.text( ).c_str( ),
											           catch_duck ? " +9" : "   ", catch_z, target_z - catch_z );
											r.edge.add( rank_gap, line );
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
										made.cost     = node.cost + leg_cost;
										push_leg( made, arr_named( shown_type, ducked ), catch_z, s.fall, 0.f, rank_gap, tier );
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
									if ( !s.roof_hit )
										continue;
									try_bang( z_in + hull_in );
									break;
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
									made.cost         = node.cost + leg_cost;
									push_leg( made, arr_named( point.type, ducked_in ), z_in, -vz_in, 0.f, target_z - hb_head );
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
									made.cost         = node.cost + leg_cost + ( i - tb_first );
									push_leg( made, arr_named( point.type, ducked_in, i - tb_first ), z_in, -vz_in, 0.f, 0.f );
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
									made.cost         = node.cost + leg_cost;
									push_leg( made, arr_named( point.type, ducked ), s.z, -vz_in, s.z - target_z, std::fabs( s.z - target_z ) );
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
								++r.opts;
							if ( const std::size_t presses = static_cast< std::size_t >( node.n_legs ) + 1;
							     presses < 16 && attempt_gap < r.best_by_presses[ presses ] )
								r.best_by_presses[ presses ] = attempt_gap;
							if ( attempt_gap < FLT_MAX && r.misses.counts( attempt_gap ) &&
							     attempt_gap < miss_cut.load( std::memory_order_relaxed ) ) {
								char line[ 224 ]{ };
								sprintf_s( line, "%-24s%s from %9.3f stam %5.2f -> apex %9.3f closest %9.3f (off %6.3f)", dep.text( ).c_str( ),
								           duck_late ? " [duck late]" : duck_air ? " [duck air]" : ( ceil_over ? " [ceil over]" : under_roof ? " [roof]" : "" ), dep_z, dep_stam, apex,
								           attempt_z, attempt_gap );
								r.misses.add( attempt_gap, line );
							}
							}
						}
					}
				}
			};

			/* every state the route can be in takes the same press. outputs with one press + arrival ( sig ) are one
			   route and their states one set; a press that misses from some states keeps the ones it hits, odds shrink */
			const auto expand = [ & ]( const node_t& node, expand_t& r ) {
				expand_one( node, r );
				const std::size_t n_states = node.others.size( ) + 1;
				if ( n_states == 1 && std::adjacent_find( r.sigs.begin( ), r.sigs.end( ) ) == r.sigs.end( ) ) {
					for ( node_t& made : r.made ) {
						made.odds = node.odds;
						if ( !last )
							r.keys.emplace_back( node_key( made ) );
					}
					return;
				}
				std::vector< std::size_t > from( r.made.size( ), 0 );
				for ( std::size_t j = 1; j < n_states; ++j ) {
					node_t other = node;
					other.st     = node.others[ j - 1 ].st;
					other.jb_ok  = node.others[ j - 1 ].jb_ok;
					other.jb     = node.others[ j - 1 ].jb;
					other.others.clear( );
					expand_t tmp;
					expand_one( other, tmp );
					for ( std::size_t k = 0; k < tmp.made.size( ); ++k ) {
						r.made.emplace_back( std::move( tmp.made[ k ] ) );
						r.sigs.emplace_back( tmp.sigs[ k ] );
						from.emplace_back( j );
					}
				}

				std::vector< node_t > leads;
				std::vector< unsigned long long > lead_sig;
				std::vector< std::vector< unsigned long long > > lead_seen; /* state hashes in the set */
				std::vector< std::vector< char > > lead_hit;                /* which of node's states got there */
				std::unordered_map< unsigned long long, std::size_t > at;
				for ( std::size_t k = 0; k < r.made.size( ); ++k ) {
					/* one state's same-sig outputs are always adjacent ( one press loop ): no map needed then */
					std::size_t g = leads.size( );
					if ( n_states == 1 ) {
						if ( g > 0 && lead_sig.back( ) == r.sigs[ k ] )
							g = leads.size( ) - 1;
					} else if ( const auto it = at.find( r.sigs[ k ] ); it != at.end( ) )
						g = it->second;
					node_t& made = r.made[ k ];
					if ( g == leads.size( ) ) {
						if ( n_states > 1 )
							at.emplace( r.sigs[ k ], g );
						lead_sig.emplace_back( r.sigs[ k ] );
						lead_seen.emplace_back( );
						lead_hit.emplace_back( n_states, 0 );
						lead_hit.back( )[ from[ k ] ] = 1;
						leads.emplace_back( std::move( made ) );
						continue;
					}
					lead_hit[ g ][ from[ k ] ] = 1;
					if ( lead_seen[ g ].empty( ) )
						lead_seen[ g ].emplace_back( key_hash( state_key( leads[ g ], leads[ g ].st, leads[ g ].jb_ok, leads[ g ].jb ) ) );
					const unsigned long long h = key_hash( state_key( made, made.st, made.jb_ok, made.jb ) );
					if ( std::find( lead_seen[ g ].begin( ), lead_seen[ g ].end( ), h ) != lead_seen[ g ].end( ) )
						continue;
					lead_seen[ g ].emplace_back( h );
					/* shown leg stays the earliest tick: in game a marginal box is crossed onto rising ( logs 4344, 10-10 ) */
					leads[ g ].others.push_back( { made.st, made.jb_ok, made.jb } );
				}
				/* ponytail: every landing tick counts the same, real odds lean on how you approach the box */
				for ( std::size_t g = 0; g < leads.size( ); ++g ) {
					const auto hits = std::count( lead_hit[ g ].begin( ), lead_hit[ g ].end( ), 1 );
					leads[ g ].odds = hits == static_cast< std::ptrdiff_t >( n_states )
					                      ? node.odds
					                      : node.odds * static_cast< float >( hits ) / static_cast< float >( n_states );
				}
				r.made = std::move( leads );
				r.sigs = std::move( lead_sig );
				if ( !last )
					for ( const node_t& made : r.made )
						r.keys.emplace_back( node_key( made ) );
			};

			/* fold one slot, in frontier order: the dedup ( first key wins its seat ), merges, and every min / list
			   come out exactly as a single thread walking the frontier would make them */
			const auto fold = [ & ]( expand_t& r ) {
				/* one node's two presses that print the same end in the same rows: counted once */
				std::vector< unsigned long long > last_texts;
				for ( std::size_t k = 0; k < r.made.size( ); ++k ) {
					node_t& made = r.made[ k ];
					if ( last ) {
						if ( made.path ) {
							if ( std::find( last_texts.begin( ), last_texts.end( ), made.path->hash ) != last_texts.end( ) )
								made.combos = 0;
							else
								last_texts.push_back( made.path->hash );
						}
						next.emplace_back( std::move( made ) );
						continue;
					}
					if ( const std::size_t at = twin_of( r.keys[ k ] ); at != SIZE_MAX ) {
						if ( !approx && g_live_alts.load( std::memory_order_relaxed ) > in.exact_alts ) {
							approx   = true;
							alt_keep = static_cast< std::size_t >( cap ) * 2;
						}
						node_t& twin     = next[ at ];
						/* same text twice = one row ( two passes that fly the same press ) */
						const long long sum = made.path && twin.path && made.path->hash == twin.path->hash
						                          ? ( std::max )( twin.combos, made.combos )
						                          : ( twin.combos > LLONG_MAX - made.combos ? LLONG_MAX : twin.combos + made.combos );
						if ( easier( made, twin ) ) {
							merge_alts( made, twin, alt_keep );
							twin = std::move( made );
						} else
							merge_alts( twin, made, alt_keep );
						twin.combos = sum;
						continue;
					}
					remember( r.keys[ k ], next.size( ) );
					next.emplace_back( std::move( made ) );
				}
				fan.emplace_back( r.opts );
				if ( r.best_gap < out.best_gap && static_cast< int >( p ) == deepest ) {
					out.best_gap  = r.best_gap;
					out.closest_z = r.closest_z;
				}
				if ( static_cast< int >( p ) == deepest )
					out.blocked_moves |= r.blocked;
				out.refused_moves |= r.refused;
				for ( int n = 0; n < 16; ++n )
					best_by_presses[ n ] = ( std::min )( best_by_presses[ n ], r.best_by_presses[ n ] );
				misses.merge( r.misses );
				edge.merge( r.edge );
				if ( misses.full( ) )
					miss_cut.store( misses.rows.back( ).first, std::memory_order_relaxed );
				r = expand_t{ };
			};

			/* nothing runs more than k_ahead past the fold, so a ring of 2x holds every slot in flight
			   ( claimed - folded < k_ahead + helpers ) and a slot is folded before its index comes round */
			constexpr std::size_t k_ahead = 2048, k_ring = 2 * k_ahead;
			const std::size_t n_front = frontier.size( );
			const std::size_t ring    = ( std::min )( n_front, k_ring );
			std::vector< expand_t > slots( ring );
			std::unique_ptr< std::atomic< bool >[ ] > ready( new std::atomic< bool >[ ring ]( ) );
			std::atomic< std::size_t > claim{ 0 }, folded{ 0 };
			std::atomic< bool > stop{ false }, helper_oom{ false };
			const auto run = [ & ]( std::size_t i ) {
				expand( frontier[ i ], slots[ i % ring ] );
				ready[ i % ring ].store( true, std::memory_order_release );
			};
			std::vector< std::thread > pool;
			/* declared last: joins before anything the helpers touch goes out of scope ( cancel / oom leave early ) */
			struct join_t {
				std::vector< std::thread >& pool;
				std::atomic< bool >& stop;
				~join_t( )
				{
					stop.store( true );
					for ( std::thread& t : pool )
						t.join( );
				}
			} join{ pool, stop };
			if ( n_front >= 64 )
				for ( unsigned int t = 0; t < helpers; ++t ) {
					try {
						pool.emplace_back( [ & ]( ) {
							n_perf::background_thread( );
							SetThreadPriority( GetCurrentThread( ), THREAD_PRIORITY_LOWEST );
							for ( ;; ) {
								const std::size_t i = claim.fetch_add( 1, std::memory_order_relaxed );
								if ( i >= n_front )
									return;
								while ( i >= folded.load( std::memory_order_acquire ) + k_ahead )
									if ( stop.load( std::memory_order_relaxed ) )
										return;
									else
										std::this_thread::yield( );
								if ( stop.load( std::memory_order_relaxed ) )
									return;
								try {
									run( i );
								} catch ( const std::bad_alloc& ) {
									helper_oom.store( true );
									return;
								}
							}
						} );
					} catch ( const std::system_error& ) {
						break; /* no thread to spare: the ones running ( or this one alone ) do it */
					}
				}

			for ( std::size_t i = 0; i < n_front; ++i ) {
				/* not done yet: this thread expands the next unclaimed node in the window instead of waiting */
				while ( !ready[ i % ring ].load( std::memory_order_acquire ) ) {
					if ( helper_oom.load( ) )
						throw std::bad_alloc( );
					std::size_t c = claim.load( std::memory_order_relaxed );
					if ( c < n_front && c < i + k_ahead && claim.compare_exchange_weak( c, c + 1, std::memory_order_relaxed ) )
						run( c );
					else
						std::this_thread::yield( );
				}
				fold( slots[ i % ring ] );
				ready[ i % ring ].store( false, std::memory_order_relaxed );
				folded.store( i + 1, std::memory_order_release );
				if ( ( ( i + 1 ) & 63u ) == 0u ) {
					if ( in.cancel && in.cancel->load( std::memory_order_relaxed ) ) {
						cancelled = true;
						return k_dead;
					}
					if ( past( in.time_limit_ms ) ) {
						timed_out = true;
						return k_dead;
					}
					if ( low_memory( mem_floor ) )
						throw std::bad_alloc( );
					if ( n_front >= 2 && over( n_front, pending + n_front + next.size( ) ) )
						return k_split;
					/* one writer: a re-estimate at a new point never shows the bar stepping back. a time limit fills the bar by
					   the clock when that is further */
					int permille = span.lo + static_cast< int >( ( span.hi - span.lo ) * ( span.done + ( i + 1 ) ) / ( span.done + width + later ) );
					if ( in.time_limit_ms > 0 )
						permille = ( std::max )( permille, static_cast< int >( 999 * std::chrono::duration_cast< std::chrono::milliseconds >(
						                                                                   std::chrono::steady_clock::now( ) - t_start ).count( ) /
						                                                       in.time_limit_ms ) );
					if ( in.progress && permille > in.progress->load( std::memory_order_relaxed ) )
						in.progress->store( ( std::min )( permille, 999 ), std::memory_order_relaxed );
				}
			}
			/* this point's dedup sets are done: free them before the next point inherits the links */
			for ( node_t& n : next )
				if ( n.alts && !n.alts->hashes.empty( ) )
					std::unordered_set< unsigned long long >( ).swap( n.alts->hashes );
			span.done += width;
			span.prev_width = frontier.size( );

			{
				long long fan_max = 1;
				for ( const long long f : fan )
					fan_max = ( std::max )( fan_max, f );
				if ( branch_max.size( ) <= p )
					branch_max.resize( p + 1, 1.L );
				branch_max[ p ] = ( std::max )( branch_max[ p ], static_cast< long double >( fan_max ) );
			}

			if ( last && !next.empty( ) ) {
				out.jumps = next.front( ).n_legs;
				for ( int n = 0; n < 16; ++n )
					out.reach_gap[ n ] = reach_set ? ( std::min )( out.reach_gap[ n ], best_by_presses[ n ] ) : best_by_presses[ n ];
				reach_set = true;
			}

			/* [rc reach]: best gap per route length at this marker ( absent = never tried; window ~0.03 ) */
			if ( splits == 0 ) {
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

			if ( edge.count > 0 && splits == 0 ) {
				botox_dbg_log( "[rc edge] %s: %d catches on the W-held band edge or under it ( gentle press ) - KEPT, ranked under:\n",
				                   point_label( p ).c_str( ), static_cast< int >( edge.count ) );
				for ( const auto& e : edge.rows )
					botox_dbg_log( "[rc edge]   %s\n", e.second.c_str( ) );
			}

			if ( next.empty( ) && splits > 0 ) {
				/* one half dead: the solve fails only if every half does, reported at the deepest point */
				if ( static_cast< int >( p ) > dead_p ) {
					dead_p      = static_cast< int >( p );
					dead_misses = top_t{ 12 };
				}
				if ( static_cast< int >( p ) == dead_p )
					dead_misses.merge( misses );
				return k_dead;
			}
			if ( next.empty( ) ) {
				commit_counts( );
				out.failed_at = static_cast< int >( p );
				out.want_z    = target_z;

				botox_dbg_log( "[rc miss] %s ( %s ) wants z %.4f - %d departures tried, closest first:\n",
				                   point_label( p ).c_str( ), route_point_type_name( shown_type ), target_z,
				                   static_cast< int >( misses.count ) );
				for ( const auto& m : misses.rows ) {
					botox_dbg_log( "[rc miss]   %s\n", m.second.c_str( ) );
					out.miss_lines.emplace_back( m.second );
				}
				return k_dead;
			}

			if ( out.arcs_after.size( ) <= p )
				out.arcs_after.resize( p + 1, 0 );
			out.arcs_after[ p ] += static_cast< int >( next.size( ) );

			if ( !last && splits == 0 ) {
				const std::size_t n_plain =
					static_cast< std::size_t >( std::count_if( next.begin( ), next.end( ), []( const node_t& n ) { return n.crouches == 0; } ) );
				const auto n_multi  = std::count_if( next.begin( ), next.end( ), []( const node_t& n ) { return !n.others.empty( ); } );
				const auto n_chancy = std::count_if( next.begin( ), next.end( ), []( const node_t& n ) { return n.odds < 1.f; } );
				char sets[ 96 ]{ };
				if ( n_multi > 0 || n_chancy > 0 )
					sprintf_s( sets, " | %d land on more than one tick, %d chancy", static_cast< int >( n_multi ), static_cast< int >( n_chancy ) );
				botox_dbg_log( "[rc beam] %s: %d arcs ( %d with an optional crouch )%s\n", point_label( p ).c_str( ),
				                   static_cast< int >( next.size( ) ), static_cast< int >( next.size( ) - n_plain ), sets );
			}
			frontier = std::move( next );
			return k_next;
		};

		const auto final_gap = []( const node_t& n ) { return n.path ? n.path->leg.gap : 0.f; };
		const auto tier_of = []( const node_t& n ) { return n.path ? n.path->leg.tier : 0; };
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
		/* chancy routes ( odds < 1 ) list after every safe one */
		const auto list_tier = [ & ]( const node_t& n ) { return ( std::min )( tier_of( n ), 2 ) + ( n.odds < 1.f ? 3 : 0 ); };
		/* text_len = the lead's: an alt sorts with its lead. false = the list is full and this row can't make it: one
		   walk's rows come in key order, so no later row of it can either */
		const auto list_row = [ & ]( const node_t& n, const alt_t* alt, std::size_t text_len ) -> bool {
			if ( timed_out )
				return false;
			if ( ( ++rows_seen & 4095u ) == 0u ) {
				if ( low_memory( mem_floor ) )
					throw std::bad_alloc( );
				if ( past( in.time_limit_ms ) ) {
					timed_out = true;
					return false;
				}
			}
			/* list order = one frontier's walk: tier, leads before alts, the frontier sort, walk order */
			const row_key_t row_key{ list_tier( n ), alt ? 1 : 0, -n.odds, tier_of( n ), n.crouches, bind_count( n ), n.switches, n.presses,
			                         n.cost, n.n_legs, static_cast< int >( final_gap( n ) * 2000.f ), text_len, row_seq };
			/* exact count: every row still goes through the dedup below, listed or not */
			const bool fits = rows.size( ) < static_cast< std::size_t >( cap ) || row_key < rows.back( ).key;
			if ( approx && !fits )
				return false;
			std::vector< std::string > els = elements_of( n );
			if ( alt ) {
				std::vector< std::string > pre = alt_prefix( *alt );
				pre.insert( pre.end( ), els.begin( ) + static_cast< std::ptrdiff_t >( alt->len ), els.end( ) );
				els = std::move( pre );
			}
			/* 64 bit text hashes: a false dedup needs a collision among one solve's rows */
			if ( !printed.insert( text_hash( route_text( els ) ) ).second )
				return true;
			{
				std::string key;
				int last_press = static_cast< int >( els.size( ) ) - 1;
				while ( last_press >= 0 && ( els[ last_press ].empty( ) || is_arrival( els[ last_press ] ) ) )
					--last_press;
				for ( int i = 0; i < static_cast< int >( els.size( ) ); ++i )
					key += ( i == last_press ? last_press_plain( els[ i ] ) : els[ i ] ) + ">";
				char tail[ 48 ]{ };
				sprintf_s( tail, "|%d|%d", n.path ? static_cast< int >( std::lround( n.path->leg.arrive_z * 10000.f ) ) : 0,
				           n.st.ducked ? 1 : 0 );
				if ( !outcomes.insert( text_hash( key + tail ) ).second )
					return true;
			}
			alt_rows += alt ? 1 : 0;
			if ( n.n_legs < 16 )
				reach_rows[ n.n_legs ]++;
			++row_seq;
			if ( !fits )
				return true;
			row_t row;
			row.key = row_key;
			if ( n.path ) {
				std::string text;
				for ( const auto& e : els ) {
					if ( e.empty( ) )
						continue;
					text += e == "fall (ducked)" ? std::string( "fall" ) : e;
					text += '>';
				}
				const leg_t& l = n.path->leg;
				char chancy[ 24 ]{ };
				if ( n.odds < 1.f )
					sprintf_s( chancy, "[chancy %d%%] ", static_cast< int >( n.odds * 100.f ) );
				char line[ 160 ]{ };
				sprintf_s( line, "gap %6.4f  band %s  catch %9.4f  %s  keys %d  switches %d  presses %d  cost %d  %s%s",
				           l.gap, l.tier == 0 ? "GOOD" : ( l.tier == 1 ? "iffy" : "poor" ), l.arrive_z, n.st.ducked ? "ducked" : "stand ",
				           bind_count( n ), n.switches, n.presses, n.cost, alt ? "[alt] " : "", chancy );
				row.line = line + text;
			}
			row.sol.elements = els;
			row.sol.cost     = n.cost;
			row.sol.gap      = final_gap( n );
			row.sol.odds     = n.odds;
			row.lead         = n.path;
			row.lead_legs    = n.n_legs;
			rows.insert( std::upper_bound( rows.begin( ), rows.end( ), row.key, []( const row_key_t& k, const row_t& r ) { return k < r.key; } ),
			             std::move( row ) );
			if ( rows.size( ) > static_cast< std::size_t >( cap ) )
				rows.pop_back( );
			return true;
		};
		/* one solved frontier into the list. per band tier: every lead first ( the easiest ways in, popup order ), then
		   every alt in the same node order. an alt shares its lead's catch, so a W-held alt never lands under a
		   gentle-press-only lead */
		const auto list_final = [ & ]( const std::deque< node_t >& frontier ) {
			if ( timed_out )
				return;
			finals = true;
			/* both counts every half: a later half can switch to approx, the total is then the per-arc one throughout */
			std::vector< std::size_t > text_len( frontier.size( ), 0 );
			for ( std::size_t i = 0; i < frontier.size( ); ++i ) {
				const node_t& n = frontier[ i ];
				combos_total    = combos_total > LLONG_MAX - n.combos ? LLONG_MAX : combos_total + n.combos;
				if ( n.n_legs < 16 )
					reach_combos[ n.n_legs ] += n.combos;
				text_len[ i ] = n.path ? n.path->text_len : 0;
			}
			if ( !approx && static_cast< long long >( frontier.size( ) ) > in.exact_alts )
				approx = true;
			std::vector< std::size_t > order( frontier.size( ) );
			for ( std::size_t i = 0; i < order.size( ); ++i )
				order[ i ] = i;
			std::stable_sort( order.begin( ), order.end( ), [ & ]( std::size_t ia, std::size_t ib ) {
				const node_t &a = frontier[ ia ], &b = frontier[ ib ];
				/* works whatever tick you cross onto a box first */
				if ( a.odds != b.odds )
					return a.odds > b.odds;
				if ( tier_of( a ) != tier_of( b ) )
					return tier_of( a ) < tier_of( b );
				if ( easier( a, b ) )
					return true;
				if ( easier( b, a ) )
					return false;
				if ( a.n_legs != b.n_legs )
					return a.n_legs < b.n_legs;
				const int ga = static_cast< int >( final_gap( a ) * 2000.f ), gb = static_cast< int >( final_gap( b ) * 2000.f );
				if ( ga != gb )
					return ga < gb;
				return text_len[ ia ] < text_len[ ib ];
			} );
			for ( int tier = 0; tier <= 5; ++tier ) {
				for ( const std::size_t i : order )
					if ( list_tier( frontier[ i ] ) == tier && !list_row( frontier[ i ], nullptr, text_len[ i ] ) )
						return;
				for ( const std::size_t i : order ) {
					const node_t& n = frontier[ i ];
					if ( !n.alts || list_tier( n ) != tier )
						continue;
					alts_held += static_cast< long long >( n.alts->size );
					bool more = true;
					n.alts->each( [ & ]( const std::shared_ptr< const alt_t >& a ) {
						if ( more )
							more = list_row( n, a.get( ), text_len[ i ] );
					} );
					if ( !more )
						return;
				}
			}
		};

		std::function< void( std::deque< node_t >&, std::size_t, span_t ) > solve_from;
		solve_from = [ & ]( std::deque< node_t >& frontier, std::size_t p0, span_t span ) {
			for ( std::size_t p = p0; p < points.size( ); ++p ) {
				const int r = point_step( frontier, p, span );
				if ( r == k_dead )
					return;
				/* more than 2 points to go: halves re-grow apart ( no merges between them ) and a deep split never ends
				   ( 20 ground markers: 31896 splits, 45 s, best = whatever half finished first ). drop the harder half
				   instead, list order ( works on every tick, fewest crouches / binds / switches / presses / cost ) */
				if ( r == k_split && points.size( ) - p > 2 ) {
					std::vector< std::size_t > order( frontier.size( ) );
					for ( std::size_t i = 0; i < order.size( ); ++i )
						order[ i ] = i;
					std::stable_sort( order.begin( ), order.end( ), [ & ]( std::size_t ia, std::size_t ib ) {
						const node_t &a = frontier[ ia ], &b = frontier[ ib ];
						if ( a.odds != b.odds )
							return a.odds > b.odds;
						return easier( a, b );
					} );
					const std::size_t keep = frontier.size( ) / 2;
					{
						std::deque< node_t > best;
						for ( std::size_t i = 0; i < keep; ++i )
							best.emplace_back( std::move( frontier[ order[ i ] ] ) );
						frontier.swap( best );
					}
					if ( ++trims <= 8 )
						botox_dbg_log( "[rc trim] %s ( %s ): kept the easiest %d of %d arcs, %llu MB free\n", point_label( p ).c_str( ), split_why,
						               static_cast< int >( keep ), static_cast< int >( keep + ( order.size( ) - keep ) ), free_virtual( ) >> 20 );
					--p; /* this point again, from the kept half ( unsigned wrap at 0 is undone by ++p ) */
					continue;
				}
				if ( r == k_split ) {
					const std::size_t half = frontier.size( ) / 2;
					std::deque< node_t > rest( std::make_move_iterator( frontier.begin( ) + static_cast< std::ptrdiff_t >( half ) ),
					                           std::make_move_iterator( frontier.end( ) ) );
					frontier.resize( half );
					frontier.shrink_to_fit( );
					if ( ++splits <= 8 )
						botox_dbg_log( "[rc split] %s ( %s ): %d arcs held, %llu MB free - its %d arcs solved as two halves, one after the other\n",
						               point_label( p ).c_str( ), split_why, static_cast< int >( pending + half + rest.size( ) ), free_virtual( ) >> 20,
						               static_cast< int >( half + rest.size( ) ) );
					const int mid = span.lo + ( span.hi - span.lo ) / 2;
					pending += rest.size( );
					solve_from( frontier, p, span_t{ span.lo, mid, 0.L, span.prev_width } );
					pending -= rest.size( );
					std::deque< node_t >( ).swap( frontier );
					if ( !cancelled && !timed_out )
						solve_from( rest, p, span_t{ mid, span.hi, 0.L, span.prev_width } );
					return;
				}
			}
			list_final( frontier );
		};
		solve_from( root, 0, span_t{ } );
		if ( cancelled )
			return;
		const long long total = approx ? combos_total : static_cast< long long >( outcomes.size( ) );
		if ( timed_out ) {
			out.timed_out = true;
			finals        = total > 0;
			botox_dbg_log( "[rc time] stopped at %d s: %lld combos finished\n", in.time_limit_ms / 1000, total );
		}
		if ( splits > 0 )
			botox_dbg_log( "[rc split] %d splits, %s ( %llu MB free at start, %llu now, heap %.2fx the counted arcs )\n", splits,
			               timed_out ? "stopped at the time limit" : ( finals ? "solved" : "no half got through" ), start_free >> 20,
			               free_virtual( ) >> 20, heap_ratio );
		if ( trims > 0 )
			botox_dbg_log( "[rc trim] %d times: the harder half of the arcs dropped, combos counted are a floor\n", trims );
		out.trimmed = trims > 0;
		if ( !finals ) {
			if ( splits > 0 && dead_p >= 0 ) {
				const route_point_t& point = points[ static_cast< std::size_t >( dead_p ) ];
				commit_counts( );
				out.failed_at = dead_p;
				out.want_z    = point.quantized_z;
				botox_dbg_log( "[rc miss] %s ( %s ) wants z %.4f - %d departures tried, closest first:\n",
				               point_label( static_cast< std::size_t >( dead_p ) ).c_str( ),
				               route_point_type_name( route_point_shown_type( point ) ), point.quantized_z,
				               static_cast< int >( dead_misses.count ) );
				for ( const auto& m : dead_misses.rows ) {
					botox_dbg_log( "[rc miss]   %s\n", m.second.c_str( ) );
					out.miss_lines.emplace_back( m.second );
				}
			}
			return;
		}

		if ( !rows.empty( ) ) {
			std::vector< const step_t* > steps( static_cast< std::size_t >( rows.front( ).lead_legs ) );
			std::size_t at = steps.size( );
			for ( const step_t* s = rows.front( ).lead.get( ); s && at > 0; s = s->up.get( ) )
				steps[ --at ] = s;
			for ( std::size_t i = 0; i < steps.size( ); ++i ) {
				const leg_t& l = steps[ i ]->leg;
				botox_dbg_log( "[rc chain] %d %-9s %-11s from %9.3f stam %5.2f -> apex %9.3f %s %9.3f fall %6.1f "
				                   "hover %5.3f gap %6.3f stam %5.2f\n",
				                   static_cast< int >( i ) + 1, route_point_type_name( l.type ), steps[ i ]->name( ).c_str( ), l.dep_z,
				                   l.dep_stam, l.apex, ( l.type == route_pt_ground || l.type == route_pt_pixeljump ) ? "land " : "catch", l.arrive_z, l.fall,
				                   l.hover, l.gap, l.stam_out );
			}
		}
		for ( std::size_t i = 0; i < rows.size( ); ++i ) {
			if ( !rows[ i ].line.empty( ) )
				botox_dbg_log( "[rc list] %2d  %s\n", static_cast< int >( i ) + 1, rows[ i ].line.c_str( ) );
			out.routes.emplace_back( std::move( rows[ i ].sol ) );
		}
		if ( approx )
			botox_dbg_log( "[rc alts] %lld rows counted per arc ( too many to list: %d alts listed )\n", combos_total, alt_rows );
		else
			botox_dbg_log( "[rc alts] %d unique rows from %d merged sequences\n", alt_rows, static_cast< int >( alts_held ) );
		for ( int n = 0; n < 16; ++n )
			out.reach_hits[ n ] = static_cast< int >( ( std::min )( static_cast< long long >( INT_MAX ), approx ? reach_combos[ n ] : reach_rows[ n ] ) );
		out.total        = total;
		out.total_approx = approx;
		out.failed_at = -1;
		commit_counts( );
	}

	void solve( const solve_input_t& in, solve_output_t& out )
	{
		try {
			solve_search( in, out );
		} catch ( const std::bad_alloc& ) {
			/* unwound: every arc is freed. failed_at = the point it was expanding */
			out.routes.clear( );
			out.total         = 0;
			out.out_of_memory = true;
			botox_dbg_log( "[rc] out of memory at point %d: more arcs than the game's address space holds ( %llu MB free now )\n",
			               out.failed_at + 1, free_virtual( ) >> 20 );
		}
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
			if ( budget_ms >= 0 && std::chrono::steady_clock::now( ) - t0 > std::chrono::milliseconds( budget_ms ) )
				return false;
			if ( in.cancel && in.cancel->load( std::memory_order_relaxed ) )
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
