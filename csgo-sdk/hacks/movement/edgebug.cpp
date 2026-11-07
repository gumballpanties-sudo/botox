#include "edgebug.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "tick_scale.h"
#include "wall_climb.h"
#include <cstring>

extern bool g_air_stuck_owns_cmd;
bool ps_latched( );
bool eb_cmd_taken( );
extern void botox_dbg_log( const char* fmt, ... );

namespace {
	inline float get_edgebug_half_gravity_per_tick( )
	{
		static auto sv_gravity = g_interfaces.m_convar->find_var( "sv_gravity" );
		const float gravity = sv_gravity ? sv_gravity->get_float( ) : 800.f;
		/* detector's gravity step; via n_tick so the 128 fix moves it. */
		return gravity * 0.5f * n_tick::interval( );
	}

	/* tickrate rules live in tick_scale.h: per-tick * scale, tick count / scale, fall speeds untouched. */
	inline float eb_tick_scale( ) { return n_tick::scale( ); }

	/* tick count in 64-tick units -> ticks on this server. */
	inline int eb_ticks( int ticks_at_64 ) { return n_tick::ticks( ticks_at_64 ); }

	/* DETECTION BANDS: pure ORs of five shapes, one table each ( order irrelevant ).
	   vz0 = vel.z BEFORE the sim tick, vz = after, g = half gravity per tick, s = tick scale. */

	/* vz0 < -( g * mul + off * s + fall ) && |vz + g| < tol * s. off/tol per-tick ( scaled ), fall never scaled. */
	struct eb_grav_band_t {
		float mul, off, fall, tol;
	};
	/* vz0 < -fall && |vz| < tol * s: fell this fast, landed this slow. */
	struct eb_abs_band_t {
		float fall, tol;
	};
	/* abs band + the tick killed at least `change` * s u/s of fall. */
	struct eb_chg_band_t {
		float change, fall, tol;
	};
	/* |vz / vz0| < ratio && vz0 < -fall: fall cut to a fraction. both tickrate-invariant. */
	struct eb_ratio_band_t {
		float ratio, fall;
	};

	template < std::size_t N >
	inline bool eb_any( const eb_grav_band_t ( &bands )[ N ], float vz0, float vz, float g, float s )
	{
		for ( const auto& b : bands )
			if ( vz0 < -( g * b.mul + b.off * s + b.fall ) && fabsf( vz + g ) < b.tol * s )
				return true;
		return false;
	}
	template < std::size_t N >
	inline bool eb_any( const eb_abs_band_t ( &bands )[ N ], float vz0, float vz, float s )
	{
		for ( const auto& b : bands )
			if ( vz0 < -b.fall && fabsf( vz ) < b.tol * s )
				return true;
		return false;
	}
	template < std::size_t N >
	inline bool eb_any( const eb_chg_band_t ( &bands )[ N ], float vz0, float vz, float vel_change, float s )
	{
		for ( const auto& b : bands )
			if ( vel_change > b.change * s && vz0 < -b.fall && fabsf( vz ) < b.tol * s )
				return true;
		return false;
	}
	template < std::size_t N >
	inline bool eb_any( const eb_ratio_band_t ( &bands )[ N ], float vz0, float vel_ratio )
	{
		for ( const auto& b : bands )
			if ( vel_ratio < b.ratio && vz0 < -b.fall )
				return true;
		return false;
	}

	/* NORMAL mode ( m_edgebug_detection_mode 0 ) — 23 bands. */
	constexpr eb_grav_band_t k_normal_grav[] = {
		{ 0.65f, 0.f, 0.f, 2.8f },  { 1.00f, 0.25f, 0.f, 4.8f }, { 1.40f, 0.f, 0.f, 5.8f },
		{ 1.44f, 0.f, 0.f, 1.4f },  { 0.50f, 0.f, 0.f, 2.5f },   { 2.00f, 0.f, 0.f, 6.5f },
		{ 0.40f, 0.f, 0.f, 2.0f },  { 2.50f, 0.f, 0.f, 7.0f },   { 0.30f, 0.f, 0.f, 1.7f },
		{ 3.00f, 0.f, 0.f, 7.5f },  { 0.25f, 0.f, 0.f, 1.5f },   { 3.50f, 0.f, 0.f, 8.0f },
	};
	constexpr eb_abs_band_t k_normal_abs[] = {
		{ 75.f, 14.5f }, { 18.f, 7.5f }, { 5.f, 2.2f },   { 95.f, 16.f },  { 3.f, 1.8f },
		{ 105.f, 17.5f },{ 2.f, 1.5f },  { 115.f, 19.f }, { 1.5f, 1.2f },  { 125.f, 20.5f },
	};

	/* ENHANCED mode ( 1 ) — 62 bands, five shapes plus the four one-offs kept in the function. */
	constexpr eb_grav_band_t k_enhanced_grav[] = {
		{ 0.60f, 0.f, 0.f, 3.0f },  { 1.12f, 0.f, 0.f, 1.8f },  { 1.10f, 0.f, 0.f, 3.8f },
		{ 0.40f, 0.f, 0.f, 2.2f },  { 0.30f, 0.f, 0.f, 2.0f },  { 1.50f, 0.f, 0.f, 4.5f },
		{ 2.00f, 0.f, 0.f, 5.5f },  { 0.25f, 0.f, 0.f, 1.8f },  { 2.50f, 0.f, 0.f, 6.0f },
		{ 0.20f, 0.f, 0.f, 1.5f },  { 3.00f, 0.f, 0.f, 7.0f },  { 4.00f, 0.f, 0.f, 9.0f },
		{ 5.00f, 0.f, 0.f, 10.5f },
	};
	constexpr eb_abs_band_t k_enhanced_abs[] = {
		{ 20.f, 10.f }, { 4.f, 2.5f },  { 35.f, 13.f },  { 55.f, 18.f }, { 75.f, 23.f },
		{ 3.f, 2.0f },  { 90.f, 28.f }, { 100.f, 32.f }, { 2.5f, 1.5f }, { 110.f, 35.f },
		{ 120.f, 38.f },{ 2.0f, 1.2f }, { 1.0f, 0.8f },  { 130.f, 22.f },{ 140.f, 24.f },
	};
	/* near an edge, so a looser verdict is allowed. */
	constexpr eb_grav_band_t k_edge_grav[] = {
		{ 0.f, 0.f, 12.f, 4.5f }, { 0.f, 0.f, 18.f, 6.5f },
	};
	constexpr eb_abs_band_t k_edge_abs[] = {
		{ 25.f, 9.f },  { 8.f, 4.5f },  { 65.f, 28.f }, { 5.f, 3.5f },   { 40.f, 14.f }, { 80.f, 32.f },
		{ 3.f, 2.5f },  { 50.f, 17.f }, { 90.f, 35.f }, { 100.f, 38.f }, { 2.f, 1.8f },
	};
	constexpr eb_chg_band_t k_enhanced_chg[] = {
		{ 15.f, 20.f, 11.f }, { 25.f, 35.f, 15.f }, { 40.f, 50.f, 20.f }, { 10.f, 15.f, 8.f },
		{ 35.f, 45.f, 18.f }, { 50.f, 60.f, 23.f }, { 60.f, 70.f, 25.f }, { 80.f, 85.f, 30.f },
	};
	constexpr eb_ratio_band_t k_enhanced_ratio[] = {
		{ 0.30f, 15.f }, { 0.20f, 10.f }, { 0.15f, 8.f }, { 0.25f, 12.f },
		{ 0.18f, 9.f },  { 0.12f, 7.f },  { 0.08f, 5.f }, { 0.05f, 3.f },
	};
	/* vz0 < -fall && vz > vz0 + rise — the fall visibly bounced back up. */
	constexpr eb_abs_band_t k_enhanced_rise[] = { { 30.f, 18.f }, { 45.f, 28.f } };

	/* NOTHING IN REACH = NO SEARCH: one conservative hull sweep along the window's ballistic arc
	   ( 3 chords, grown 32u in xy for strafe variants ) can never hide an edgebug. returns the first
	   contact TICK ( aims the duck sweep ), -1 = open air. live hull: a press only raises the feet, but a
	   ducked hull grows 9u down for the release family ( air unduck drops origin 9, cs_gamemovement.cpp:1206 ). */
	struct eb_hull_t {
		c_vector mins{ }, maxs{ };
		float grav = 800.f;
		bool valid = false;
	};

	eb_hull_t eb_make_hull( const float grow = 32.f )
	{
		eb_hull_t h;
		const auto local = g_ctx.m_local;
		if ( !local )
			return h;
		const auto col = local->get_collideable( );
		if ( !col )
			return h;
		h.mins = col->get_obb_mins( );
		h.maxs = col->get_obb_maxs( );
		h.mins.m_x -= grow; h.mins.m_y -= grow;
		h.maxs.m_x += grow; h.maxs.m_y += grow;
		h.mins.m_z -= 1.f;
		if ( local->get_flags( ) & fl_ducking )
			h.mins.m_z -= 9.f;
		static auto sv_gravity = g_interfaces.m_convar->find_var( "sv_gravity" );
		h.grav  = sv_gravity ? sv_gravity->get_float( ) : 800.f;
		h.valid = true;
		return h;
	}

	int eb_first_reach_tick( const eb_hull_t& hull, const c_vector& origin, const c_vector& vel, const int window_ticks, const int segs = 3 )
	{
		if ( !hull.valid || window_ticks <= 0 )
			return 0;
		const float ipt    = n_tick::interval( );
		const float window = static_cast< float >( window_ticks ) * ipt;
		const float seg_t = window / static_cast< float >( segs );
		c_vector maxs     = hull.maxs;
		maxs.m_z += hull.grav * seg_t * seg_t * 0.125f;

		c_trace_filter filter( g_ctx.m_local );
		c_vector prev = origin;
		for ( int seg = 1; seg <= segs; seg++ ) {
			const float t0 = seg_t * static_cast< float >( seg - 1 );
			const float t  = seg_t * static_cast< float >( seg );
			c_vector p     = origin + vel * t;
			p.m_z          = origin.m_z + vel.m_z * t - 0.5f * hull.grav * t * t;
			ray_t ray( prev, p, hull.mins, maxs );
			trace_t tr;
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
			if ( tr.did_hit( ) ) {
				const float hit_time = t0 + ( t - t0 ) * tr.m_fraction;
				return std::clamp( static_cast< int >( hit_time / ipt ), 0, window_ticks - 1 );
			}
			prev = p;
		}
		return -1;
	}

	/* heading error of start -> end vs aim yaw; 90 = path too short to have a heading */
	float eb_aim_err( const c_vector& start, const c_vector& end, const float aim_yaw )
	{
		const float dx = end.m_x - start.m_x, dy = end.m_y - start.m_y;
		if ( dx * dx + dy * dy < 4.f )
			return 90.f;
		return fabsf( std::remainderf( rad2deg( atan2f( dy, dx ) ) - aim_yaw, 360.f ) );
	}

	/* base run state at loop top of tick i ( no candidate pending ): duck sweeps resume here instead of re-simming 0..i-1.
	   tools/wc_engine/eb_lab.cpp EB_CHECK = resumed run == full run. slots 0..55 ( wall climb owns 62 / 63 ) */
	struct eb_snap_t {
		int slot = -1;
		float strafe_side = 1.f, last_yaw = 0.f;
		c_angle cur{ }, view{ };
		float fwd = 0.f, side = 0.f;
		int buttons = 0;
		c_vector vel_backup{ };
	};
	constexpr int k_eb_snap_slots = 56;
	eb_snap_t s_eb_snap[ 8 ][ n_edgebug::k_predicted_cmd_max ];
	n_edgebug::predicted_cmd_t s_eb_snap_cmds[ 8 ][ n_edgebug::k_predicted_cmd_max ];

	/* ebanat FUN_1011ac10 ring: 16 vertical rays 32u out, +200 -> -100; a face under 0.6 nz = a ramp clipped the fall, not a lip */
	bool eb_steep_ring( )
	{
		const c_vector o = g_ctx.m_local->get_origin( );
		c_trace_filter filter( g_ctx.m_local );
		for ( float a = 0.f; a < 6.2831855f; a += 0.3926991f ) {
			const float x = o.m_x + cosf( a ) * 32.f, y = o.m_y + sinf( a ) * 32.f;
			ray_t ray( c_vector( x, y, o.m_z + 200.f ), c_vector( x, y, o.m_z - 100.f ) );
			trace_t tr;
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
			if ( tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z < 0.6f )
				return true;
		}
		return false;
	}

	/* hull 2u down from the sim'd origin: a steep face ( 0 < nz < 0.7 ) under the feet = the fall slid on a ramp, not a lip */
	bool eb_on_slant( )
	{
		const auto col = g_ctx.m_local->get_collideable( );
		if ( !col )
			return false;
		const c_vector o = g_ctx.m_local->get_origin( );
		c_trace_filter filter( g_ctx.m_local );
		ray_t ray( o, c_vector( o.m_x, o.m_y, o.m_z - 2.f ), col->get_obb_mins( ), col->get_obb_maxs( ) );
		trace_t tr;
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		return !tr.m_start_solid && tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z > 0.f && tr.m_plane.m_normal.m_z < 0.7f;
	}
}

void n_edgebug::impl_t::log_lock( const bool on )
{
	static bool was_on = false;
	if ( on == was_on )
		return;
	was_on = on;
	botox_dbg_log( "EB: lock %s type=%d str=%.0f at=%d/%d\n", on ? "on" : "off", GET_VARIABLE( g_variables.m_edgebug_mouse_lock_type, int ),
	               GET_VARIABLE( g_variables.m_edgebug_mouse_lock_strength, float ), m_replay_hits, m_prediction_ticks );
}

void CorrectMovement( c_user_cmd* cmd, c_angle wish_angle, c_angle old_angles )
{
	if ( old_angles.m_x == wish_angle.m_x && old_angles.m_y == wish_angle.m_y )
		return;
	c_vector wish_fwd, wish_right, wish_up, cmd_fwd, cmd_right, cmd_up;
	auto viewangles = old_angles;
	auto movedata = c_vector( cmd->m_forward_move, cmd->m_side_move, cmd->m_up_move );
	viewangles.normalize( );
	if ( !( g_ctx.m_local->get_flags( ) & 1 ) && viewangles.m_z != 0.f )
		movedata.m_y = 0.f;
	g_math.angle_vectors( wish_angle, &wish_fwd, &wish_right, &wish_up );
	g_math.angle_vectors( viewangles, &cmd_fwd, &cmd_right, &cmd_up );
	auto norm = []( float x, float y ) { float l = sqrt( x * x + y * y ); return l > 0.f ? 1.f / l : 0.f; };
	float wf = norm( wish_fwd.m_x, wish_fwd.m_y ), wr = norm( wish_right.m_x, wish_right.m_y );
	float cf = norm( cmd_fwd.m_x, cmd_fwd.m_y ), cr = norm( cmd_right.m_x, cmd_right.m_y );
	c_vector wfn( wf * wish_fwd.m_x, wf * wish_fwd.m_y, 0.f );
	c_vector wrn( wr * wish_right.m_x, wr * wish_right.m_y, 0.f );
	c_vector cfn( cf * cmd_fwd.m_x, cf * cmd_fwd.m_y, 0.f );
	c_vector crn( cr * cmd_right.m_x, cr * cmd_right.m_y, 0.f );
	float vx = wfn.m_x * movedata.m_x + wrn.m_x * movedata.m_y;
	float vy = wfn.m_y * movedata.m_x + wrn.m_y * movedata.m_y;
	float det = cfn.m_x * crn.m_y - cfn.m_y * crn.m_x;
	if ( fabsf( det ) > 0.001f ) {
		cmd->m_forward_move = std::clamp( ( vx * crn.m_y - vy * crn.m_x ) / det, -450.f, 450.f );
		cmd->m_side_move = std::clamp( ( cfn.m_x * vy - cfn.m_y * vx ) / det, -450.f, 450.f );
	}
	cmd->m_buttons &= ~( in_moveright | in_moveleft | in_back | in_forward );
	if ( cmd->m_side_move > 0.f ) cmd->m_buttons |= in_moveright;
	else if ( cmd->m_side_move < 0.f ) cmd->m_buttons |= in_moveleft;
	if ( cmd->m_forward_move > 0.f ) cmd->m_buttons |= in_forward;
	else if ( cmd->m_forward_move < 0.f ) cmd->m_buttons |= in_back;
}
/* BEND CONE: max strafe angle caps how far the FLIGHT PATH bends off m_course_yaw, never the push dir.
   push is measured as the move vector in the wish frame; only a push ACROSS the path accelerates
   ( 30 u/s wishspd cap ). at the cone edge an outward push is MIRRORED across the velocity
   ( overshoot <= one tick of lateral accel ). 180 = no cone. */
void n_edgebug::impl_t::CommitStrafe( c_user_cmd* cmd, c_angle wish )
{
	const float cap = std::clamp( GET_VARIABLE( g_variables.m_edgebug_autostrafe_max_angle, float ), 0.f, 180.f );
	if ( cap < 180.f && m_course_valid ) {
		const c_vector vel = g_ctx.m_local->get_velocity( );
		if ( vel.length_2d( ) > 1.f ) {
			const float vel_yaw = rad2deg( atan2f( vel.m_y, vel.m_x ) );
			/* dev = path bend off course so far, rel = side the push bends it. */
			const float dev      = std::remainderf( vel_yaw - m_course_yaw, 360.f );
			const float move_off = rad2deg( atan2f( -cmd->m_side_move, cmd->m_forward_move ) );
			const float rel      = std::remainderf( wish.m_y + move_off - vel_yaw, 360.f );
			if ( fabsf( dev ) >= cap && ( ( dev > 0.f ) == ( rel > 0.f ) ) )
				wish.m_y = vel_yaw - rel - move_off; /* mirror across the path */
		}
	}
	CorrectMovement( cmd, wish, cmd->m_view_point );
}
static c_angle s_original_angle;
static float s_original_fwd, s_original_side;
void start_movement_fix( c_user_cmd* cmd )
{
	s_original_angle = cmd->m_view_point;
	s_original_fwd = cmd->m_forward_move;
	s_original_side = cmd->m_side_move;
}
void end_movement_fix( c_user_cmd* cmd )
{
	float f1 = s_original_angle.m_y < 0.f ? 360.f + s_original_angle.m_y : s_original_angle.m_y;
	float f2 = cmd->m_view_point.m_y < 0.f ? 360.f + cmd->m_view_point.m_y : cmd->m_view_point.m_y;
	float delta = f2 < f1 ? fabsf( f2 - f1 ) : 360.f - fabsf( f1 - f2 );
	delta = 360.f - delta;
	cmd->m_forward_move = cosf( deg2rad( delta ) ) * s_original_fwd + cosf( deg2rad( delta + 90.f ) ) * s_original_side;
	cmd->m_side_move = sinf( deg2rad( delta ) ) * s_original_fwd + sinf( deg2rad( delta + 90.f ) ) * s_original_side;
}
void n_edgebug::impl_t::PrePredictionEdgeBug( c_user_cmd* cmd )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	m_backup_velocity = g_ctx.m_local->get_velocity( );
	m_backup_move_type = g_ctx.m_local->get_move_type( );
	m_backup_flags = g_ctx.m_local->get_flags( );
	m_user_forward_move = cmd->m_forward_move;
	m_user_side_move = cmd->m_side_move;
	m_user_buttons = cmd->m_buttons;
	if ( GET_VARIABLE( g_variables.m_edgebug_style, int ) == 2 )
		donor_pre( cmd );
	else if ( GET_VARIABLE( g_variables.m_edgebug_style, int ) == 4 )
		lob_pre( cmd );
}
bool n_edgebug::impl_t::IsNearEdge( )
{
	if ( !g_ctx.m_local )
		return false;
	/* get_origin( ) not abs: runs per sim tick, abs may rebuild the transform; local player is never parented. */
	c_vector origin = g_ctx.m_local->get_origin( );
	c_vector velocity = g_ctx.m_local->get_velocity( );
	float speed_2d = velocity.length_2d( );
	if ( speed_2d < 1.f )
		return false;
	c_vector move_dir( velocity.m_x / speed_2d, velocity.m_y / speed_2d, 0.f );
	/* lookahead is a DISTANCE ( 2-3 ticks of travel at 64 ): step in 64-tick units at any tickrate. */
	constexpr float k_step_time = 1.f / 64.f;
	const int max_steps = speed_2d < 120.f ? 2 : 3;
	c_trace_filter filter( g_ctx.m_local );
	for ( int step = 1; step <= max_steps; step++ ) {
		c_vector check_pos = origin + move_dir * ( speed_2d * k_step_time * step );
		c_vector trace_start( check_pos.m_x, check_pos.m_y, origin.m_z + 2.f );
		c_vector trace_end( check_pos.m_x, check_pos.m_y, origin.m_z - 64.f );
		ray_t ray( trace_start, trace_end );
		trace_t trace;
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &trace );
		if ( trace.m_fraction > 0.8f )
			return true;
	}
	return false;
}
bool n_edgebug::impl_t::EdgeBugCheck( const edge_context_t& raw_ctx )
{
	if ( !raw_ctx.was_falling || raw_ctx.on_ground )
		return false;
	/* per-tick quantity ( tol around -g, per-tick delta, offset on g ) -> literal * s; fall speed / ratio ->
	   as-is ( NEVER scale fall speeds: fires at half the real speed on 128 ). s == 1 at 64. */
	const float s   = eb_tick_scale( );
	const float g   = raw_ctx.gravity_tick;
	const float vz0 = raw_ctx.backup_vel_z;
	const float vz  = raw_ctx.current_vel_z;
	/* EBANAT mode ( 2 ) = FUN_1011ac10: snapped to -g, or fell a step, slowed, still falls a step ( ramp ring excluded ).
	   the confirm tick then has to fall one free step ( search loop ). */
	if ( raw_ctx.mode == 2 ) {
		if ( vz0 < -g && roundf( vz ) == -roundf( g ) )
			return true;
		return vz0 < -g + 0.25f && vz0 < vz && vz < -g + 0.25f && ( !raw_ctx.trace_edge || !eb_steep_ring( ) );
	}
	if ( raw_ctx.mode == 0 ) {
		if ( eb_any( k_normal_grav, vz0, vz, g, s ) || eb_any( k_normal_abs, vz0, vz, s ) )
			return true;
		/* exact landing tick: vz snapped to one gravity step. */
		return vz0 < -g * 0.7f && roundf( vz ) == -roundf( g );
	}
	const float vel_change = fabsf( vz - vz0 );
	const float vel_ratio  = vz0 != 0.f ? fabsf( vz / vz0 ) : 0.f;
	const bool bands = [ & ] {
		if ( eb_any( k_enhanced_grav, vz0, vz, g, s ) ||
		     eb_any( k_enhanced_abs, vz0, vz, s ) ||
		     eb_any( k_enhanced_chg, vz0, vz, vel_change, s ) ||
		     eb_any( k_enhanced_ratio, vz0, vel_ratio ) )
			return true;
		/* bands before IsNearEdge( ): its traces run per SIM tick. */
		if ( ( eb_any( k_edge_grav, vz0, vz, g, s ) || eb_any( k_edge_abs, vz0, vz, s ) ) && raw_ctx.trace_edge && IsNearEdge( ) )
			return true;
		for ( const auto& b : k_enhanced_rise )
			if ( vz0 < -b.fall && vz > vz0 + b.tol * s )
				return true;
		if ( vz0 < -g * 0.9f && roundf( vz ) == -roundf( g ) )
			return true;
		/* fall taken away: unchanged vz, or vz parked around zero. */
		if ( vz0 < -12.f && fabsf( vz - vz0 ) < 1.8f * s )
			return true;
		return vz0 < -15.f && vz > -2.f * s && vz < 2.f * s;
	}( );
	/* loose bands also fire when a ramp clip slows the fall: reject a steep face under the feet ( after the bands, trace per candidate only ).
	   trace_edge false = route_engine workers, no traces there. */
	return bands && ( !raw_ctx.trace_edge || !eb_on_slant( ) );
}
/* same context the live search builds, from just the two velocities. */
bool n_edgebug::impl_t::IsEdgeBugTick( float backup_vel_z, float current_vel_z, bool on_ground, bool trace_edge )
{
	edge_context_t ctx;
	ctx.trace_edge    = trace_edge;
	ctx.backup_vel_z  = backup_vel_z;
	ctx.current_vel_z = current_vel_z;
	ctx.gravity_tick  = get_edgebug_half_gravity_per_tick( );
	ctx.was_falling   = backup_vel_z < -6.f * eb_tick_scale( );
	ctx.on_ground     = on_ground;
	ctx.mode          = GET_VARIABLE( g_variables.m_edgebug_detection_mode, int );
	return EdgeBugCheck( ctx );
}

void n_edgebug::impl_t::ApplyAutoStrafe( c_user_cmd* cmd, bool duck )
{
	if ( duck )
		cmd->m_buttons |= in_duck;
	else
		cmd->m_buttons &= ~in_duck;
	/* member, not a static ( edgebug.h ); flips per tick of a run. */
	m_strafe_side = -m_strafe_side;
	const float side_flip = m_strafe_side;
	auto velocity = g_ctx.m_local->get_velocity( );
	c_angle wish_angle = cmd->m_view_point;
	float speed = velocity.length_2d( );
	float ideal_strafe = std::clamp( rad2deg( atanf( 15.f / speed ) ), 0.f, 90.f );
	cmd->m_forward_move = 0.f;
	static auto cl_sidespeed = g_interfaces.m_convar->find_var( "cl_sidespeed" );
	float cl_sidespeed_val = cl_sidespeed ? cl_sidespeed->get_float( ) : 450.f;
	/* member, not a static: must not leak between variants. */
	float yaw_delta = std::remainderf( wish_angle.m_y - m_strafe_last_yaw, 360.f );
	float abs_yaw_delta = fabsf( yaw_delta );
	m_strafe_last_yaw = wish_angle.m_y;
	if ( abs_yaw_delta <= ideal_strafe || abs_yaw_delta >= 30.f ) {
		c_angle vel_dir;
		g_math.vector_angles( velocity, vel_dir );
		float vel_delta = std::remainderf( wish_angle.m_y - vel_dir.m_y, 360.f );
		float retrack = std::clamp( rad2deg( atanf( 30.f / speed ) ), 0.f, 90.f ) * 2.f;
		if ( vel_delta <= retrack || speed <= 15.f ) {
			if ( -retrack <= vel_delta || speed <= 15.f ) {
				wish_angle.m_y += side_flip * ideal_strafe;
				cmd->m_side_move = cl_sidespeed_val * side_flip;
			} else {
				wish_angle.m_y = vel_dir.m_y - retrack;
				cmd->m_side_move = cl_sidespeed_val;
			}
		} else {
			wish_angle.m_y = vel_dir.m_y + retrack;
			cmd->m_side_move = -cl_sidespeed_val;
		}
		CommitStrafe( cmd, wish_angle );
	} else if ( yaw_delta > 0.f ) {
		cmd->m_side_move = -cl_sidespeed_val;
		/* frame = view: no-op until the cap bites */
		CommitStrafe( cmd, cmd->m_view_point );
	} else {
		cmd->m_side_move = cl_sidespeed_val;
		CommitStrafe( cmd, cmd->m_view_point );
	}
}
/* lip target, once per real tick, world space ( geometry is static during the sim ). aim at the EDGE,
   not open air: at the ballistic point near first contact lay 9 lanes across the path ( +-64u ), floor-trace
   each, take the boundary nearest the path ( solid/void or a drop > 18u step height ). */
bool n_edgebug::impl_t::FindEdgeTarget( const int reach_tick, const int window_ticks )
{
	m_edge_target_valid = false;
	const auto local = g_ctx.m_local;
	if ( !local )
		return false;
	const c_vector vel = local->get_velocity( );
	const float speed  = vel.length_2d( );
	/* no horizontal path = no lane frame. */
	if ( speed < 30.f )
		return false;
	static auto sv_gravity = g_interfaces.m_convar->find_var( "sv_gravity" );
	const float grav      = sv_gravity ? sv_gravity->get_float( ) : 800.f;
	const float ipt       = n_tick::interval( );
	const c_vector origin = local->get_abs_origin( );
	/* one tick past first contact; no contact -> a third of the window. */
	const int probe_tick = std::clamp( reach_tick >= 0 ? reach_tick + 1 : window_ticks / 3, 1, std::max( 1, window_ticks ) );
	const float t = static_cast< float >( probe_tick ) * ipt;
	c_vector p = origin + vel * t;
	p.m_z      = origin.m_z + vel.m_z * t - 0.5f * grav * t * t;
	/* lane frame across the path; middle lane = the path. */
	const c_vector right( vel.m_y / speed, -vel.m_x / speed, 0.f );
	constexpr int k_lanes     = 9;
	constexpr float k_lane    = 16.f;
	constexpr float k_step_up = 18.f; /* engine step height: anything smaller is walkable, not an edge */
	float lane_z[ k_lanes ];
	bool lane_solid[ k_lanes ];
	c_trace_filter filter( local );
	for ( int i = 0; i < k_lanes; i++ ) {
		const float off = static_cast< float >( i - k_lanes / 2 ) * k_lane;
		const c_vector c = p + right * off;
		ray_t ray( c_vector( c.m_x, c.m_y, c.m_z + 16.f ), c_vector( c.m_x, c.m_y, c.m_z - 128.f ) );
		trace_t tr;
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		lane_solid[ i ] = tr.did_hit( );
		lane_z[ i ]     = tr.did_hit( ) ? tr.m_end.m_z : -99999.f;
	}
	float best_off  = 0.f;
	float best_dist = 99999.f;
	/* never aim at an unreachable lip:
	     bearing: must sit inside CommitStrafe's cone.
	     reach:   to-edge wishes across the CURRENT velocity, so AirAccelerate adds up to 30 u/s EVERY tick:
	              n ticks buy <= 30 * ipt * n( n + 1 ) / 2 lateral, + half a hull ( tools/eb_fix_check.py ). */
	const float cap     = std::clamp( GET_VARIABLE( g_variables.m_edgebug_autostrafe_max_angle, float ), 0.f, 180.f );
	/* +1: p is off the live entity ( END of cmd ), sims start a tick earlier ( START ). */
	const float n       = static_cast< float >( probe_tick + 1 );
	const float max_lat = 30.f * ipt * n * ( n + 1.f ) * 0.5f + 16.f;
	for ( int i = 0; i < k_lanes - 1; i++ ) {
		const bool a = lane_solid[ i ], b = lane_solid[ i + 1 ];
		/* lip = solid next to void, or a drop taller than a step. */
		if ( a == b && ( !a || fabsf( lane_z[ i ] - lane_z[ i + 1 ] ) < k_step_up ) )
			continue;
		const float off_a = static_cast< float >( i - k_lanes / 2 ) * k_lane;
		const float mid   = off_a + k_lane * 0.5f;
		/* aim half a hull onto the drop side: on the lip = normal landing, full hull past = miss. */
		const float air_sign = ( a == b ? ( lane_z[ i ] < lane_z[ i + 1 ] ? -1.f : 1.f ) : ( a ? 1.f : -1.f ) );
		const float off = mid + air_sign * 8.f;
		if ( fabsf( off ) > max_lat )
			continue;
		if ( cap < 180.f && m_course_valid ) {
			const c_vector q = p + right * off;
			const float bearing = std::remainderf(
			    rad2deg( atan2f( q.m_y - origin.m_y, q.m_x - origin.m_x ) ) - m_course_yaw, 360.f );
			if ( fabsf( bearing ) > cap )
				continue;
		}
		if ( const float d = fabsf( off ); d < best_dist ) {
			best_dist = d;
			best_off  = off;
		}
	}
	if ( best_dist > 90.f )
		return false;
	m_edge_target       = p + right * best_off;
	m_edge_target_valid = true;
	return true;
}
void n_edgebug::impl_t::ApplyAutoStrafeToEdge( c_user_cmd* cmd, bool duck )
{
	/* no lip -> plain strafe. */
	if ( !m_edge_target_valid ) {
		ApplyAutoStrafe( cmd, duck );
		return;
	}
	if ( duck )
		cmd->m_buttons |= in_duck;
	else
		cmd->m_buttons &= ~in_duck;
	const c_vector velocity = g_ctx.m_local->get_velocity( );
	const float speed = velocity.length_2d( );
	if ( speed < 1.f ) {
		cmd->m_forward_move = 0.f;
		cmd->m_side_move = 0.f;
		return;
	}
	const c_vector origin = g_ctx.m_local->get_abs_origin( );
	const c_vector vel_dir( velocity.m_x / speed, velocity.m_y / speed, 0.f );
	const c_vector to_target( m_edge_target.m_x - origin.m_x, m_edge_target.m_y - origin.m_y, 0.f );
	/* only the part ACROSS the path can be bought: addspeed = 30 - dot( vel, wishdir ), so a wish
	   perpendicular to velocity gets the full 30 each tick. CorrectMovement does the view rotation. */
	const float along = to_target.m_x * vel_dir.m_x + to_target.m_y * vel_dir.m_y;
	c_vector lat( to_target.m_x - vel_dir.m_x * along, to_target.m_y - vel_dir.m_y * along, 0.f );
	const float lat_len = lat.length_2d( );
	/* on the line through the lip: plain strafe for speed. */
	if ( lat_len < 1.f ) {
		ApplyAutoStrafe( cmd, duck );
		return;
	}
	c_angle target_angle;
	target_angle.m_x = 0.f;
	target_angle.m_y = rad2deg( atan2f( lat.m_y / lat_len, lat.m_x / lat_len ) );
	/* full forward in the target's frame. */
	cmd->m_forward_move = 450.f;
	cmd->m_side_move = 0.f;
	CommitStrafe( cmd, target_angle );
}
void n_edgebug::impl_t::ReStorePrediction( )
{
	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
}
void n_edgebug::impl_t::EdgeBugPostPredict( c_user_cmd* cmd )
{
	/* raw mouse, not view delta: mouse lock holds the view still while the hand still turns */
	static auto m_yaw_var = g_interfaces.m_convar->find_var( "m_yaw" );
	const float mouse_yaw = -( m_yaw_var ? m_yaw_var->get_float( ) : 0.022f ) * m_mouse_x;
	m_mouse_x             = 0.f;
	const bool steer      = GET_VARIABLE( g_variables.m_edgebug_mouse_steer, bool );
	const auto drop_plan = [ this ]( const char* why ) {
		if ( m_found )
			botox_dbg_log( "EB: drop why=%s at %d/%d\n", why, m_replay_hits, m_prediction_ticks );
		m_found            = false;
		m_replay_hits      = 0;
		m_ducked           = false;
		m_steer_since_plan = 0.f;
	};
	if ( const int style = GET_VARIABLE( g_variables.m_edgebug_style, int ); style != 4 && m_lob.found ) {
		m_lob.found  = false;
		m_lob.ticks  = 0;
		m_lob.ducked = false;
	}
	if ( const int style = GET_VARIABLE( g_variables.m_edgebug_style, int ); style != 0 ) {
		if ( style == 4 ) {
			if ( m_donor.eblength )
				donor_reset( );
			if ( m_hw.will_edgebug )
				hw_reset( );
			lob_post( cmd );
			return;
		}
		if ( style == 3 ) {
			if ( m_donor.eblength )
				donor_reset( );
			hw_post( cmd );
			return;
		}
		if ( m_hw.will_edgebug )
			hw_reset( );
		donor_post( cmd, style );
		return;
	}
	if ( m_hw.will_edgebug )
		hw_reset( );
	if ( m_donor.eblength )
		donor_reset( );
	if ( !GET_VARIABLE( g_variables.edge_bug, bool ) ||
	     !g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) ) ) {
		drop_plan( "key" );
		return;
	}
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	const auto mt = g_ctx.m_local->get_move_type( );
	/* also m_backup_move_type ( PRE ): post-prediction a ladder tick can read back as walk.
	   air stuck / wall climb / pixel surf / binds own the cmd: we run LAST and a replay would stomp them. */
	if ( const char* why = ( mt == move_type_ladder || mt == move_type_noclip || mt == move_type_observer || m_backup_move_type == move_type_ladder ) ? "move"
	                       : g_air_stuck_owns_cmd       ? "as"
	                       : g_wall_climb.caught( cmd ) ? "wc"
	                       : ps_latched( )              ? "ps"
	                       : eb_cmd_taken( )            ? "taken"
	                                                    : nullptr ) {
		drop_plan( why );
		return;
	}
	if ( m_ducked ) {
		if ( !( m_backup_flags & 1 ) )
			cmd->m_buttons &= ~in_jump;
		else
			cmd->m_buttons |= in_jump;
		cmd->m_buttons |= in_duck;
	}
	if ( m_backup_flags & 1 ) {
		/* landed = plan over ( a real bug never grounds ): else mouse lock + indicator stay latched. */
		drop_plan( "landed" );
		/* grounded: next fall starts a fresh cadence. */
		m_last_search_tick = -1;
		m_last_contact_tick = -1;
		return;
	}
	const float vz_scale = eb_tick_scale( );
	/* -40 is a real fall speed: NOT scaled. */
	if ( !m_found && m_ducked && m_backup_velocity.m_z < -40.f )
		m_ducked = false;
	const int detection_mode = GET_VARIABLE( g_variables.m_edgebug_detection_mode, int );
	static c_angle prev_angle;
	/* view delta the search carries forward ( unscaled ). */
	c_angle delta_angle = cmd->m_view_point - prev_angle;
	prev_angle = cmd->m_view_point;
	delta_angle.m_y = std::remainderf( delta_angle.m_y, 360.f );
	if ( steer && mouse_yaw != 0.f )
		delta_angle.m_y = std::clamp( mouse_yaw, -30.f, 30.f );
	/* only a near-180 flip counts as large. */
	auto is_large_delta = []( float y1, float y2 ) {
		float d = fabsf( y1 - y2 );
		if ( d > 180.f ) d = 360.f - d;
		return d >= 179.f;
	};
	/* to-edge IS the autostrafe switch, in both modes. */
	const bool to_edge    = GET_VARIABLE( g_variables.m_edgebug_autostrafe_to_edge, bool );
	const bool autostrafe = to_edge;
	/* 09-28: 6 normal, enhanced = old search amount slider default 4 */
	int search_count = detection_mode == 1 ? 4 : 6;
	/* 6 / 7 are the to-edge pair: needs eight variants. */
	if ( to_edge && search_count < 8 )
		search_count = 8;
	/* 4..7 are autostrafe passes: pointless with the strafe off. */
	if ( !autostrafe && search_count > 4 )
		search_count = 4;
	/* search window in SECONDS ( one slider, both modes ) -> ticks via n_tick. 2 s @128 = 256 = replay buffer. */
	const float predict_time = std::clamp( GET_VARIABLE( g_variables.m_edgebug_predict_time, float ), 0.02f, 2.f );
	int max_ticks = static_cast< int >( std::round( predict_time / n_tick::interval( ) ) );
	max_ticks = std::clamp( max_ticks, 1, n_edgebug::k_predicted_cmd_max );
	/* orig_* = cmd as it is now ( maybe a wall feature's move ): restored on a no-find tick. */
	int orig_buttons = cmd->m_buttons;
	float orig_fwd = cmd->m_forward_move;
	float orig_side = cmd->m_side_move;
	c_angle orig_viewangle = cmd->m_view_point;
	/* the SEARCH replays the user's real move ( variants 2/3 ), captured pre-prediction. view is safe
	   off cmd: wall features only write fwd/side. */
	const float user_fwd = m_user_forward_move;
	const float user_side = m_user_side_move;
	const int user_buttons = m_user_buttons;
	static int last_variant = 0, last_fam = 0, last_st = 3, last_bug_duck = -1;
	static float last_bend = 0.f;
	/* mouse steer re-plan: turned >= 1 deg since the plan latched, ticks(2)+ before its bug tick ( never swap a bug
	   that close ). runs write m_run_cmds, so no find keeps the old plan in m_predicted_cmds */
	if ( m_found && steer )
		m_steer_since_plan += delta_angle.m_y;
	const bool replan = m_found && steer && fabsf( m_steer_since_plan ) >= 1.f && m_prediction_ticks - m_replay_hits >= eb_ticks( 2 );
	if ( !m_found || replan ) do {
		/* gate counters, logged once a second: vz = too slow to open, gap = cadence skip,
		   air = nothing in reach, run = full searches, hit = finds. */
		static int eb_n_vz = 0, eb_n_gap = 0, eb_n_air = 0, eb_n_run = 0, eb_n_hit = 0;
		if ( static unsigned long long eb_next_report = 0ull; GetTickCount64( ) >= eb_next_report ) {
			eb_next_report = GetTickCount64( ) + 1000ull;
			if ( eb_n_vz || eb_n_gap || eb_n_air || eb_n_run )
				botox_dbg_log( "EB: gates vz=%d gap=%d air=%d run=%d hit=%d", eb_n_vz, eb_n_gap, eb_n_air, eb_n_run, eb_n_hit );
			eb_n_vz = eb_n_gap = eb_n_air = eb_n_run = eb_n_hit = 0;
		}
		/* opening fall speed: real speed, no scale. */
		if ( m_backup_velocity.m_z >= ( detection_mode >= 1 ? -4.f : -10.f ) ) {
			eb_n_vz++;
			break;
		}
		/* SEARCH CADENCE: at 128 search every other tick ( 4x the sims of 64 otherwise ). back to every
		   tick once a pass saw ground within eb_ticks( 8 ) ( duck press decided there ). no-op at 64. */
		const int now_tick = g_interfaces.m_global_vars_base->m_tick_count;
		const bool every_tick = ( m_last_contact_tick >= 0 && m_last_contact_tick <= eb_ticks( 8 ) );
		if ( const int search_gap = eb_ticks( 1 );
		     search_gap > 1 && m_last_search_tick >= 0 && now_tick - m_last_search_tick < search_gap && !every_tick ) {
			eb_n_gap++;
			break;
		}
		m_last_search_tick = now_tick;
		/* 3 traces: skip the search in open air + aim the duck sweep. hull reused by the tail probe. */
		const eb_hull_t hull = eb_make_hull( );
		const int reach_tick = eb_first_reach_tick( hull, g_ctx.m_local->get_abs_origin( ), g_ctx.m_local->get_velocity( ), max_ticks );
		if ( reach_tick < 0 ) {
			m_last_contact_tick = -1; /* nothing to hit -> spaced searches */
			eb_n_air++;
			break;
		}
		/* runs collect plans; the held plan ( m_found ) stays live until one is picked below */
		m_steer_since_plan = 0.f;
		m_plan_count       = 0;
		const int want_paths = steer ? std::clamp( GET_VARIABLE( g_variables.m_edgebug_paths, int ), 1, k_plan_max ) : 1;
		const auto enough    = [ & ]( ) { return m_plan_count >= want_paths; };
		/* course yaw off the live entity before any sim: bend cone + FindEdgeTarget reference, re-taken every search tick. */
		if ( const c_vector live_vel = g_ctx.m_local->get_velocity( ); live_vel.length_2d( ) > 30.f ) {
			m_course_yaw   = rad2deg( atan2f( live_vel.m_y, live_vel.m_x ) );
			m_course_valid = true;
		} else {
			m_course_valid = false; /* near-vertical drop: no path to bend */
		}
		/* lip target before any sim. invalid = 6/7 would re-sim 4/5's stream, skipped below. */
		if ( to_edge )
			FindEdgeTarget( reach_tick, max_ticks );
		else
			m_edge_target_valid = false;
		eb_n_run++;
		/* window trim: sim only up to reach_tick + eb_ticks( 10 ) margin ( chord approx + 32u growth ).
		   a verdict needs geometry; later ticks are re-found next search. */
		const int run_ticks = reach_tick >= 0 ? std::min( max_ticks, reach_tick + 1 + eb_ticks( 10 ) ) : max_ticks;
		int total_predictions = 0;
		/* per-tick budget is a CLOCK ( tick_scale.h ); MAX_PREDICTIONS is only a lying-QPC ceiling.
		   a cadence-skipped tick's share goes to the tick that searches ( gap_owned x2 ). */
		const int gap_owned = every_tick ? 1 : std::clamp( eb_ticks( 1 ), 1, 2 );
		/* the budget is always spent in full, so the share IS the fps bill. half share while geometry is
		   far ( > eb_ticks( 12 ) and not every_tick ): far plans are re-made many times before they matter. */
		const bool near_geometry = every_tick || ( reach_tick >= 0 && reach_tick <= eb_ticks( 12 ) );
		/* share is WALL CLOCK: ask for the 64-tick microseconds on any tickrate, then cap against this
		   tick ( 0.60 normal / 0.75 enhanced ). 64 unchanged. */
		/* search effort slider scales the ask and lifts the cap with it ( c_sim_budget clamps 0.9 ) */
		const float effort = std::clamp( GET_VARIABLE( g_variables.m_edgebug_search_effort, float ), 0.25f, 3.f );
		const float want_s = ( detection_mode >= 1 ? 0.45f : 0.30f ) * ( 1.f / 64.f ) *
		                     static_cast< float >( gap_owned ) * ( near_geometry ? 1.f : 0.5f ) * effort;
		/* enhanced / ebanat = deeper search, may own more of a tick. */
		const float budget_share = std::min( std::min( 0.9f, ( detection_mode >= 1 ? 0.75f : 0.60f ) * std::max( 1.f, effort ) ), want_s / n_tick::interval( ) );
		/* runaway ceiling derived from the clock at 8 us/sim ( real sims ~20-25 ), so it only fires on a
		   broken timer; never a tick-scaled sim count. floor = one pass of this window. */
		const int MAX_PREDICTIONS = std::clamp(
		    std::max( static_cast< int >( n_tick::interval( ) * 1000000.f * budget_share / 8.f ),
		              run_ticks * ( detection_mode >= 1 ? 6 : 4 ) ), 16, 2048 );
		n_tick::c_sim_budget budget;
		budget.start( budget_share, 0.f, n_tick::search_eb );
		/* measured cost of a full pass, for "don't start what you can't finish". */
		long long worst_run_us = 0ll;
		/* duck press window: 9u duck vs 12.5u fall step @64, 6.25 @128 -> finer timing, twice the ticks. */
		const int duck_retry    = std::clamp( eb_ticks( 3 ), 2, 8 );
		const bool duck_sweep   = GET_VARIABLE( g_variables.m_edgebug_duck_timing, bool );
		/* stance dropdown: bug tick ducked / standing. none picked = both */
		const auto& eb_types = GET_VARIABLE( g_variables.m_edgebug_types, std::vector< bool > );
		bool allow_duck      = eb_types.size( ) > 0 && eb_types[ 0 ];
		bool allow_stand     = eb_types.size( ) > 1 && eb_types[ 1 ];
		if ( !allow_duck && !allow_stand )
			allow_duck = allow_stand = true;
		/* bug ticks the stance refused ( `srej=` ) */
		int stance_rej = 0;
		/* last pass's first ground tick, -1 = never; contact_z = origin z there ( refine tells lip vs lower floor ). */
		int contact_tick = -1;
		float contact_z  = 0.f;
		/* SNAPSHOTS: base runs save their loop-top state from snap_lo on ( slot quota per variant ), press / release runs
		   resume at their tick instead of re-simming the shared prefix. lab ( eb_lab.cpp ): same finds, sweeps ~4x cheaper */
		const int snap_cap = std::min( k_eb_snap_slots, g_prediction.snapshot_slots( ) );
		const int snap_lo  = std::max( 1, reach_tick + 1 - duck_retry - std::max( 2, eb_ticks( 2 ) ) - 4 );
		int snap_used = 0, snap_limit = 0, snap_runs = 0;
		for ( auto& row : s_eb_snap )
			for ( auto& sn : row )
				sn.slot = -1;
		/* plan family for the log: 0 base 1 press 2 release 3 bend 4 ebanat 5 refine 6 wide press 7 wide release */
		int fam_now = 0;
		/* constant for the whole search. */
		const float grav_tick = get_edgebug_half_gravity_per_tick( );
		/* TAIL PROBE: past reach_tick, re-run the conservative hull sweep for the ticks left and end the
		   run when clear. only after the opening geometry, so it can't cut a run that could still graze. */
		const int probe_every = std::clamp( eb_ticks( 2 ), 1, 8 );
		/* sim ticks the probe saved ( `cut=` in the log ). */
		int tail_cut_ticks = 0;
		/* EBANAT SHAPES ( FUN_1011b8c0 ), variants 8..15: fixed turn step a tick, hard side wish at the cone, sinus wish,
		   duck toggled every tick. wish offsets off its max angle ( 15..360 there ). */
		constexpr float k_eb_turn = 4.f;
		const float eb_cap        = std::clamp( GET_VARIABLE( g_variables.m_edgebug_autostrafe_max_angle, float ), 15.f, 180.f );
		static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
		const float air_accel        = std::max( 0.1f, sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f );
		/* user-move pair ( 2/3 strafe off, 4/5 on ): with no move + no view delta it equals 0/1 exactly, skip. */
		const bool user_move_dead = fabsf( user_fwd ) < 0.1f && fabsf( user_side ) < 0.1f &&
		                           fabsf( delta_angle.m_y ) < 0.01f && fabsf( delta_angle.m_x ) < 0.01f;

		/* one pass. duck_at = press held from this tick ( -1 = variant's own rule ); undo_at = released
		   and left released ( -1 = never ). bend = first |bend| ticks pushed across the velocity ( + right ), a fraction
		   = the last tick pushed that much. from > 0 = resume at tick `from` off base variant from_v's snapshot; save = this
		   base run stores its own. fills m_run_cmds, latches m_found, sets contact_tick. */
		auto run_variant = [ & ]( const int v, const int duck_at, const int undo_at = -1, const float bend = 0.f, const int from = -1,
		                          const int from_v = -1, const bool save = false ) {
			const long long run_started_us = budget.used_us( );
			ReStorePrediction( );
			m_strafe_side = 1.f; /* same opening side + yaw every run, so runs are comparable */
			m_strafe_last_yaw = orig_viewangle.m_y;
			c_vector search_vel_backup = g_ctx.m_local->get_velocity( );
			cmd->m_view_point = orig_viewangle;
			cmd->m_forward_move = user_fwd;
			cmd->m_side_move = user_side;
			cmd->m_buttons = user_buttons;
			c_angle current_angle = cmd->m_view_point;
			contact_tick = -1;
			int i0 = 0, written = 0;
			if ( from > 0 ) {
				const eb_snap_t& sn = s_eb_snap[ from_v ][ from ];
				g_prediction.snapshot_load( sn.slot );
				m_strafe_side       = sn.strafe_side;
				m_strafe_last_yaw   = sn.last_yaw;
				current_angle       = sn.cur;
				cmd->m_view_point   = sn.view;
				cmd->m_forward_move = sn.fwd;
				cmd->m_side_move    = sn.side;
				cmd->m_buttons      = sn.buttons;
				search_vel_backup   = sn.vel_backup;
				std::memcpy( m_run_cmds, s_eb_snap_cmds[ from_v ], sizeof( predicted_cmd_t ) * from );
				i0 = written = from;
				snap_runs++;
			}
			/* user's own air move + view delta carried forward; shared by cases 2/3 and 4/5. */
			auto replay_user_move = [ & ]( const bool duck ) {
				if ( duck )
					cmd->m_buttons |= in_duck;
				else
					cmd->m_buttons &= ~in_duck;
				cmd->m_forward_move = user_fwd;
				cmd->m_side_move = user_side;
				if ( !is_large_delta( current_angle.m_y, orig_viewangle.m_y ) )
					current_angle = ( current_angle + delta_angle ).normalize( ).clamp( );
				cmd->m_view_point = current_angle;
				start_movement_fix( cmd );
				cmd->m_view_point = orig_viewangle;
				end_movement_fix( cmd );
			};
			/* mouse steer: strafe turns with the mouse ( same per-tick delta as the replay ), folded silent at replay */
			auto strafe = [ & ]( const bool duck ) {
				if ( steer && !is_large_delta( current_angle.m_y, orig_viewangle.m_y ) ) {
					current_angle     = ( current_angle + delta_angle ).normalize( ).clamp( );
					cmd->m_view_point = current_angle;
				}
				ApplyAutoStrafe( cmd, duck );
			};
			/* ebanat 2/3 with the mouse still: your move, view turned `step` a tick, folded silent */
			auto turn_move = [ & ]( const float step, const bool duck ) {
				current_angle.m_y   = std::remainderf( current_angle.m_y + step, 360.f );
				cmd->m_buttons      = duck ? cmd->m_buttons | in_duck : cmd->m_buttons & ~in_duck;
				cmd->m_forward_move = user_move_dead ? 450.f : user_fwd;
				cmd->m_side_move    = user_side;
				cmd->m_view_point   = current_angle;
				start_movement_fix( cmd );
				cmd->m_view_point = orig_viewangle;
				end_movement_fix( cmd );
			};
			/* ebanat 4..7: side press in a wish frame `off` deg off the view ( + = left, side -450 ), through the cone */
			auto side_wish = [ & ]( const float off, const bool duck ) {
				cmd->m_buttons      = duck ? cmd->m_buttons | in_duck : cmd->m_buttons & ~in_duck;
				c_angle wish        = orig_viewangle;
				wish.m_x            = 0.f;
				wish.m_y            = std::remainderf( orig_viewangle.m_y + off, 360.f );
				cmd->m_forward_move = 0.f;
				cmd->m_side_move    = off > 0.f ? -450.f : 450.f;
				CommitStrafe( cmd, wish );
			};
			/* an edgebug KEEPS FALLING: ramp clips / stucks / bounces also pass the bands. hold a candidate
			   one tick and sim the NEXT one: still airborne + falling = real, grounded / rising = not. */
			int pending = -1;
			c_vector pending_pos{ };
			float pending_vz   = 0.f;
			bool pending_duck  = false;
			/* end_pos taken at candidate time: a held candidate may latch ticks later. */
			/* same variant landing within 4u of a kept plan = same path, not an alternative */
			bool run_found = false;
			auto latch = [ & ]( const int tick_index, const c_vector& end_pos, const bool duck ) {
				run_found = true;
				for ( int p = 0; p < m_plan_count; p++ )
					if ( m_plans[ p ].variant == v && m_plans[ p ].end_pos.dist_to( end_pos ) < 4.f )
						return;
				if ( m_plan_count >= k_plan_max )
					return;
				plan_t& pl = m_plans[ m_plan_count++ ];
				std::memcpy( pl.cmds, m_run_cmds, sizeof( predicted_cmd_t ) * ( tick_index + 1 ) );
				pl.ticks   = tick_index;
				pl.variant = v;
				pl.bend    = bend;
				pl.fam     = fam_now;
				pl.duck    = duck;
				pl.end_pos = end_pos;
			};
			/* a candidate on the trimmed window's last tick still gets its confirm tick ( buffer allows max_ticks ). */
			for ( int i = i0; i < run_ticks || ( pending >= 0 && i < max_ticks ); i++ ) {
				if ( const bool grounded = ( g_ctx.m_local->get_flags( ) & 1 ) != 0;
				     grounded || g_ctx.m_local->get_velocity( ).m_z > 0.f ) {
					if ( grounded ) {
						contact_tick = i;
						contact_z    = g_ctx.m_local->get_origin( ).m_z;
					}
					pending = -1; /* landed / bounced: not an edgebug */
					break;
				}
				/* loop top = START of cmd i: the state a press / release at i resumes from */
				if ( save && pending < 0 && i >= snap_lo && snap_used < snap_limit && i < k_predicted_cmd_max &&
				     g_prediction.snapshot_save( snap_used ) ) {
					eb_snap_t& sn  = s_eb_snap[ v ][ i ];
					sn.slot        = snap_used++;
					sn.strafe_side = m_strafe_side;
					sn.last_yaw    = m_strafe_last_yaw;
					sn.cur         = current_angle;
					sn.view        = cmd->m_view_point;
					sn.fwd         = cmd->m_forward_move;
					sn.side        = cmd->m_side_move;
					sn.buttons     = cmd->m_buttons;
					sn.vel_backup  = search_vel_backup;
				}
				switch ( v ) {
				case 0:
					cmd->m_buttons &= ~in_duck;
					cmd->m_forward_move = 0.f;
					cmd->m_side_move = 0.f;
					break;
				case 1:
					cmd->m_buttons |= in_duck;
					cmd->m_forward_move = 0.f;
					cmd->m_side_move = 0.f;
					break;
				/* 2 / 3: silent search, steered angle folded into fwd/side, real view never moves. */
				case 2:
				case 3:
					if ( autostrafe )
						strafe( v == 3 );
					else
						replay_user_move( v == 3 );
					break;
				/* 4 / 5 carry whichever shape 2 / 3 gave up, so the two pairs never duplicate. */
				case 4:
				case 5:
					if ( autostrafe )
						replay_user_move( v == 5 );
					else
						strafe( v == 5 );
					break;
				case 6:
					if ( to_edge )
						ApplyAutoStrafeToEdge( cmd, false );
					else
						strafe( false );
					break;
				case 7:
					if ( to_edge )
						ApplyAutoStrafeToEdge( cmd, true );
					else
						strafe( true );
					break;
				/* 8..15 = ebanat shapes ( PASS 3 only ) */
				case 8:
					turn_move( k_eb_turn, false );
					break;
				case 9:
					turn_move( -k_eb_turn, true );
					break;
				case 10:
					turn_move( -k_eb_turn, false );
					break;
				case 11:
					turn_move( k_eb_turn, true );
					break;
				case 12:
					side_wish( eb_cap, false );
					break;
				case 13:
					side_wish( -eb_cap, true );
					break;
				case 14:
					side_wish( sinf( static_cast< float >( i + 1 ) * 0.85f ) * eb_cap * 0.65f, true );
					break;
				case 15:
					strafe( ( i & 1 ) != 0 );
					break;
				}
				/* square to velocity: +30 u/s lateral a tick, speed kept, cone capped in CommitStrafe. fractional last tick:
				   accel = airaccelerate * wishspeed * ipt, so wishspeed 30 * frac / ( airaccel * ipt ) buys 30 * frac */
				if ( i < static_cast< int >( ceilf( fabsf( bend ) ) ) ) {
					const c_vector vel = g_ctx.m_local->get_velocity( );
					if ( vel.length_2d( ) > 1.f ) {
						c_angle wish;
						wish.m_x = 0.f;
						wish.m_y = rad2deg( atan2f( vel.m_y, vel.m_x ) ) + ( bend > 0.f ? -90.f : 90.f );
						const float frac = std::min( 1.f, fabsf( bend ) - static_cast< float >( i ) );
						cmd->m_forward_move = frac >= 1.f ? 450.f : std::min( 450.f, 30.f * frac / ( air_accel * n_tick::interval( ) ) );
						cmd->m_side_move    = 0.f;
						CommitStrafe( cmd, wish );
					}
				}
				/* duck timing overlay: last word on the duck bit, held from duck_at on. */
				if ( duck_at >= 0 ) {
					if ( i < duck_at )
						cmd->m_buttons &= ~in_duck;
					else
						cmd->m_buttons |= in_duck;
				}
				/* release = the -9u knob: airborne duck/unduck finish instantly ( cs_gamemovement.cpp:1427 / :1462 ),
				   origin +-9 = 0.5 * ( 72 - 54 ) ( FinishDuck :1244, FinishUnDuck :1206 ). left released: re-press
				   inside 0.4 s refused ( sv_timebetweenducks, :168 ). */
				if ( undo_at >= 0 && i >= undo_at )
					cmd->m_buttons &= ~in_duck;
				/* confirm tick is past the replay: real duck = plan's last bit ( m_ducked hold ) or the user's own,
				   never this variant's overlay ( a press here would fake the "still airborne" ). */
				if ( pending >= 0 ) {
					if ( ( m_run_cmds[pending].buttons | user_buttons ) & in_duck )
						cmd->m_buttons |= in_duck;
					else
						cmd->m_buttons &= ~in_duck;
				}
				m_run_cmds[i].forwardmove = cmd->m_forward_move;
				m_run_cmds[i].sidemove = cmd->m_side_move;
				m_run_cmds[i].buttons = cmd->m_buttons;
				m_run_cmds[i].viewangle = cmd->m_view_point;
				m_run_cmds[i].origin = g_ctx.m_local->get_origin( );
				written = i + 1;
				if ( total_predictions >= MAX_PREDICTIONS || budget.expired( ) ) break;
				g_prediction.begin( g_ctx.m_local, cmd );
				g_prediction.end( g_ctx.m_local );
				total_predictions++;
				/* ladder grab nulls vz like an edgebug: drop the variant before the verdict can latch. */
				if ( const auto sim_move_type = g_ctx.m_local->get_move_type( );
				     sim_move_type == move_type_ladder || sim_move_type == move_type_noclip || sim_move_type == move_type_observer ) {
					pending = -1;
					break;
				}
				/* confirm tick: the candidate's verdict comes off THIS sim, then the run is over. */
				if ( pending >= 0 ) {
					const bool grounded = ( g_ctx.m_local->get_flags( ) & 1 ) != 0;
					if ( grounded ) {
						contact_tick = i + 1; /* same index the loop-top check would give */
						contact_z    = g_ctx.m_local->get_origin( ).m_z;
					}
					else if ( const float vz_now = g_ctx.m_local->get_velocity( ).m_z;
					          vz_now <= 0.f && ( detection_mode != 2 || roundf( vz_now ) == roundf( pending_vz - 2.f * grav_tick ) ) )
						latch( pending, pending_pos, pending_duck ); /* mode 2: ebanat's exact free step after the slowdown */
					pending = -1;
					break;
				}
				edge_context_t ctx;
				ctx.backup_vel_z = search_vel_backup.m_z;
				ctx.current_vel_z = g_ctx.m_local->get_velocity( ).m_z;
				ctx.gravity_tick = grav_tick;
				/* "falling at all" ~ one gravity step: per-tick, so this one DOES scale. */
				ctx.was_falling = search_vel_backup.m_z < -6.f * vz_scale;
				ctx.on_ground = ( g_ctx.m_local->get_flags( ) & 1 ) != 0;
				ctx.mode = detection_mode;
				const bool bug_tick = EdgeBugCheck( ctx );
				const bool bug_duck = ( g_ctx.m_local->get_flags( ) & fl_ducking ) != 0;
				if ( bug_tick && !( bug_duck ? allow_duck : allow_stand ) )
					stance_rej++;
				if ( bug_tick && ( bug_duck ? allow_duck : allow_stand ) ) {
					/* no buffer left to confirm: take it. */
					if ( i + 1 >= max_ticks ) {
						latch( i, g_ctx.m_local->get_origin( ), bug_duck );
						break;
					}
					pending      = i;
					pending_pos  = g_ctx.m_local->get_origin( );
					pending_vz   = g_ctx.m_local->get_velocity( ).m_z;
					pending_duck = bug_duck;
				}
				search_vel_backup = g_ctx.m_local->get_velocity( );
				/* tail probe: past reach tick, no pending candidate, ticks left worth saving. */
				if ( pending < 0 && i > reach_tick && i + probe_every < run_ticks &&
				     ( ( i - reach_tick ) % probe_every ) == 0 &&
				     eb_first_reach_tick( hull, g_ctx.m_local->get_origin( ), search_vel_backup, run_ticks - i - 1 ) < 0 ) {
					tail_cut_ticks += run_ticks - i - 1;
					break;
				}
			}
			/* window / budget ran out before the confirm tick: take it unconfirmed. */
			if ( !run_found && pending >= 0 )
				latch( pending, pending_pos, pending_duck );
			if ( save )
				std::memcpy( s_eb_snap_cmds[ v ], m_run_cmds, sizeof( predicted_cmd_t ) * written );
			cmd->m_view_point = orig_viewangle;
			cmd->m_forward_move = orig_fwd;
			cmd->m_side_move = orig_side;
			cmd->m_buttons = orig_buttons;
			if ( const long long spent = budget.used_us( ) - run_started_us; spent > worst_run_us )
				worst_run_us = spent;
		};

		/* closest floor any pass saw: feeds the duck sweep + next tick's cadence. */
		int best_contact = -1;
		const int replay_variant = autostrafe ? 4 : 2;
		/* BREADTH FIRST: every variant once, THEN the duck sweeps. never sweep inside the variant loop
		   ( variant 0's sweep eats the budget and 1..7 never run ). */
		int sweep_center[ 8 ];
		for ( int& c : sweep_center )
			c = -1;
		/* centre from the reach CHORD ( can be a tick or two out ): press window widened past it. */
		bool sweep_approx[ 8 ]{ };
		/* base variants that ACTUALLY ran ( budget may end the pass between v and v + 1 ). */
		bool base_ran[ 8 ]{ };
		int base_runs = 0;
		/* duck press runs ( `sw=` in the log ). */
		int sweep_runs = 0;
		/* duck release runs ( `rel=` ), the -9u family. */
		int release_runs = 0;
		/* last finder goes first by SWAP, so no variant is skipped. */
		const int first_variant = ( last_variant > 0 && last_variant < search_count ) ? last_variant : 0;
		auto variant_at = [ & ]( const int idx ) { return idx == 0 ? first_variant : ( idx == first_variant ? 0 : idx ); };
		auto out_of_budget = [ & ]( ) {
			/* don't start a run that can't finish; never gates the FIRST run. */
			return total_predictions > 0 && ( total_predictions >= MAX_PREDICTIONS || budget.cannot_fit( worst_run_us ) );
		};
		/* base outcome per variant: the refine pass brackets on these */
		int base_contact[ 8 ];
		float base_contact_z[ 8 ]{ };
		for ( int& c : base_contact )
			c = -1;
		/* pass 0: every variant once, each saving up to its share of the snapshot slots */
		const int snap_quota = snap_cap / std::max( 1, search_count );
		for ( int idx = 0; idx < search_count && !enough( ); idx++ ) {
			const int v = variant_at( idx );
			if ( user_move_dead && ( v == replay_variant || v == replay_variant + 1 ) )
				continue;
			/* 6 / 7 only as to-edge; without a lip they'd repeat 2 / 3. */
			if ( ( !to_edge || !m_edge_target_valid ) && ( v == 6 || v == 7 ) )
				continue;
			/* odd = duck held all run, always a ducked bug */
			if ( !allow_duck && ( v & 1 ) )
				continue;
			if ( out_of_budget( ) )
				break;
			snap_limit = std::min( snap_cap, snap_used + snap_quota );
			run_variant( v, -1, -1, 0.f, -1, -1, duck_sweep && snap_quota > 0 );
			base_runs++;
			base_ran[ v ]       = true;
			base_contact[ v ]   = contact_tick;
			base_contact_z[ v ] = contact_z;
			if ( contact_tick > best_contact )
				best_contact = contact_tick;
			/* no contact != no edge: a tiny missed lip grounds nothing, so centre the sweep on reach_tick.
			   >= 0, not > 0: reach 0 = geometry already in the first chord, the lip that matters most. */
			sweep_center[ v ] = contact_tick > 0 ? contact_tick : ( reach_tick >= 0 ? reach_tick + 1 : -1 );
			sweep_approx[ v ] = contact_tick <= 0;
		}
		/* press / release at k off base variant w's snapshot at k when it has one, else the full run */
		auto run_at = [ & ]( const int v, const int duck_at, const int undo_at, const int k, const int w ) {
			if ( k > 0 && k < k_predicted_cmd_max && s_eb_snap[ w ][ k ].slot >= 0 )
				run_variant( v, duck_at, undo_at, 0.f, k, w );
			else
				run_variant( v, duck_at, undo_at );
		};
		/* PASS 0b: LIP REFINE ( eb_lab 10-31: strafe on 20.5% -> 52% of falls, strips 27 -> 64, pillars 27 -> 93 / 150 ).
		   zero move ( 0 stand / 1 duck ) pushed square to the path for b ticks: grid +-2 +-6 +-12, then between two b that
		   land on the lip / miss it the bug sits on the boundary, bisect b ( fractional last tick ). steers = to edge only */
		int refine_runs = 0;
		if ( GET_VARIABLE( g_variables.m_edgebug_lip_refine, bool ) && autostrafe && near_geometry && m_plan_count == 0 ) {
			const int steps = std::clamp( GET_VARIABLE( g_variables.m_edgebug_refine_steps, int ), 1, 12 );
			constexpr float k_grid[] = { 2.f, -2.f, 6.f, -6.f, 12.f, -12.f };
			fam_now = 5;
			for ( int v = 0; v < 2 && m_plan_count == 0 && !out_of_budget( ); v++ ) {
				if ( !base_ran[ v ] || ( v == 0 && !allow_stand ) )
					continue;
				float val[ 7 ], cz[ 7 ];
				int con[ 7 ], n = 0;
				val[ n ] = 0.f;
				con[ n ] = base_contact[ v ];
				cz[ n++ ] = base_contact_z[ v ];
				for ( const float g : k_grid ) {
					if ( m_plan_count || out_of_budget( ) || fabsf( g ) >= static_cast< float >( run_ticks ) )
						break;
					run_variant( v, -1, -1, g );
					refine_runs++;
					val[ n ] = g;
					con[ n ] = contact_tick;
					cz[ n++ ] = contact_z;
				}
				/* brackets = neighbours on the signed b line, the ones nearest b = 0 first */
				int idx[ 7 ], order[ 6 ], no = 0;
				for ( int j = 0; j < n; j++ )
					idx[ j ] = j;
				std::sort( idx, idx + n, [ & ]( const int a, const int b ) { return val[ a ] < val[ b ]; } );
				for ( int j = 0; j + 1 < n; j++ )
					order[ no++ ] = j;
				std::sort( order, order + no, [ & ]( const int a, const int b ) {
					return fabsf( val[ idx[ a ] ] + val[ idx[ a + 1 ] ] ) < fabsf( val[ idx[ b ] ] + val[ idx[ b + 1 ] ] );
				} );
				/* ONE lip per variant: the landing nearest b = 0 ( your path ). a run on any other floor = miss, so no
				   bracket can lock onto a step the big pushes carried it to */
				int near_land = -1;
				for ( int j = 0; j < n; j++ )
					if ( con[ j ] >= 0 && ( near_land < 0 || fabsf( val[ j ] ) < fabsf( val[ near_land ] ) ) )
						near_land = j;
				const float lip = near_land >= 0 ? cz[ near_land ] : 0.f;
				const auto on_lip = [ & ]( const int j ) { return con[ j ] >= 0 && fabsf( cz[ j ] - lip ) < 1.f; };
				for ( int q = 0; q < no && near_land >= 0 && m_plan_count == 0 && !out_of_budget( ); q++ ) {
					const int ja = idx[ order[ q ] ], jb = idx[ order[ q ] + 1 ];
					const bool la = on_lip( ja ), lb = on_lip( jb );
					if ( la == lb )
						continue;
					float land = la ? val[ ja ] : val[ jb ], miss = la ? val[ jb ] : val[ ja ];
					for ( int st = 0; st < steps && m_plan_count == 0 && !out_of_budget( ); st++ ) {
						const float mid = 0.5f * ( land + miss );
						run_variant( v, -1, -1, mid );
						refine_runs++;
						/* a lower floor counts as a miss: only the lip level is the edge being bracketed */
						if ( contact_tick >= 0 && fabsf( contact_z - lip ) < 1.f )
							land = mid;
						else
							miss = mid;
					}
				}
			}
		}
		/* PASS 1: DUCK TIMING. the press tick decides the bug ( 9u press vs 6.25u fall step @128 ).
		   only ticks around contact. EVEN variants only: the overlay makes an odd twin re-sim its even
		   twin exactly ( needs m_strafe_side fixed per run ). ROUND ROBIN on the offset ( latest = nearest
		   the lip first ), so every variant gets its best press before any gets a second. */
		/* press window per even variant; press-at-k and release-at-k sweep the same ticks. */
		int sweep_first[ 8 ]{ }, sweep_last[ 8 ]{ };
		int max_off = -1;
		if ( duck_sweep && !enough( ) ) {
			for ( int v = 0; v < 8; v += 2 ) {
				sweep_last[ v ] = -1;
				if ( sweep_center[ v ] <= 0 )
					continue;
				/* press >= run_ticks = base run again. approx centre slack >= 2 ticks at any rate
				   ( a chord tick of error is worse at 64 ). */
				sweep_last[ v ] = std::min( sweep_center[ v ] - 1 + ( sweep_approx[ v ] ? std::max( 2, eb_ticks( 2 ) ) : 0 ), run_ticks - 1 );
				/* press at 0 = the odd twin's base run: start at 1 if that twin ran. */
				sweep_first[ v ] = std::max( base_ran[ v + 1 ] ? 1 : 0,
				                             sweep_center[ v ] - duck_retry - ( sweep_approx[ v ] ? std::max( 2, eb_ticks( 2 ) ) : 0 ) );
				if ( const int span = sweep_last[ v ] - sweep_first[ v ]; span > max_off )
					max_off = span;
			}
			/* cannot_fit, not expired: a truncated run gives no verdict. */
			for ( int off = 0; off <= max_off && !enough( ); off++ ) {
				for ( int idx = 0; idx < search_count && !enough( ); idx++ ) {
					const int v = variant_at( idx );
					if ( v & 1 )
						continue;
					const int k = sweep_last[ v ] - off;
					if ( sweep_last[ v ] < 0 || k < sweep_first[ v ] )
						continue;
					if ( out_of_budget( ) ) {
						off = max_off; /* budget gone for every variant */
						break;
					}
					if ( allow_duck ) {
						fam_now = 1;
						run_at( v, k, -1, k, v );
						sweep_runs++;
					}
					/* release at k ( -9u family ): ducked from 0, released at k, never re-pressed ( 0.4 s
					   sv_timebetweenducks ). INTERLEAVED with presses or it never gets budget. release at 0
					   = even base run, skipped. */
					if ( allow_stand && !enough( ) && k >= 1 && !out_of_budget( ) ) {
						fam_now = 2;
						run_at( v, 0, k, k, v + 1 );
						release_runs++;
					}
				}
			}
		}
		/* PASS 1b: FULL WINDOW. near ticks done, the rest latest first while budget lasts ( 10-30 log: strafe-off
		   searches stopped at ~60% of budget, out of variants ). eb_lab: strafe off 6.3% -> 9.0%. duck only, no steer */
		int wide_runs = 0;
		if ( duck_sweep && GET_VARIABLE( g_variables.m_edgebug_full_window, bool ) && !enough( ) ) {
			for ( int k = run_ticks - 1; k >= 1 && !enough( ) && !out_of_budget( ); k-- )
				for ( int idx = 0; idx < search_count && !enough( ) && !out_of_budget( ); idx++ ) {
					const int v = variant_at( idx );
					if ( ( v & 1 ) || !base_ran[ v ] || ( sweep_last[ v ] >= 0 && k >= sweep_first[ v ] && k <= sweep_last[ v ] ) )
						continue;
					if ( allow_duck ) {
						fam_now = 6;
						run_at( v, k, -1, k, v );
						wide_runs++;
					}
					if ( allow_stand && !enough( ) && ( base_ran[ v + 1 ] || !allow_duck ) && !out_of_budget( ) ) {
						fam_now = 7;
						run_at( v, 0, k, k, v + 1 );
						wide_runs++;
					}
				}
		}
		/* PASS 2: BEND. no-finds all land ( 10-11 log: 327/327 con>=0 ) = lip crossed at the wrong phase, duck only
		   moves height. bend turns heading ~7 deg a tick @250 = lateral + along-path phase. smallest first, near
		   geometry + nothing found so any base find stays as before. */
		int bend_runs = 0;
		/* bend + ebanat shapes steer the path: only with the autostrafe switch ( to edge ) on. off = your move / duck only */
		if ( GET_VARIABLE( g_variables.m_edgebug_path_bend, bool ) && autostrafe && near_geometry && m_plan_count == 0 ) {
			fam_now = 3;
			constexpr int k_bend[] = { 1, 2, 3, 4, 6, 8, 12 };
			for ( int b = 0; b < 2 * static_cast< int >( std::size( k_bend ) ) && !enough( ) && !out_of_budget( ); b++ ) {
				const int bend = ( b & 1 ) ? -k_bend[ b / 2 ] : k_bend[ b / 2 ];
				if ( abs( bend ) >= run_ticks )
					break;
				for ( int idx = 0; idx < search_count && !enough( ) && !out_of_budget( ); idx++ ) {
					const int v = variant_at( idx );
					if ( !base_ran[ v ] )
						continue;
					run_variant( v, -1, -1, static_cast< float >( bend ) );
					bend_runs++;
				}
			}
		}
		/* PASS 3: EBANAT SHAPES. 10-11 log: no-find searches ran out of variants, not budget ( 290/327 under 90% ) -> the
		   rest goes to FUN_1011b8c0's shapes. zero plans only, so every base / sweep / bend find stays as before. */
		int eb_runs = 0;
		if ( GET_VARIABLE( g_variables.m_edgebug_ebanat_shapes, bool ) && autostrafe && near_geometry && m_plan_count == 0 ) {
			fam_now = 4;
			for ( int v = 8; v < 16 && !enough( ) && !out_of_budget( ); v++ ) {
				run_variant( v, -1 );
				eb_runs++;
			}
		}
		/* no sim contact != nothing near: fall back to reach_tick so the cadence stays every-tick near lips. */
		m_last_contact_tick = best_contact >= 0 ? best_contact : reach_tick;
		/* pick the plan whose path heads closest to where the mouse turns ( aim = view + half the turn over its flight ).
		   one plan ( steer off / paths 1 ) = first find, as 09-28 */
		const auto aim_at = [ & ]( const int ticks ) {
			return orig_viewangle.m_y + std::clamp( delta_angle.m_y * static_cast< float >( ticks + 1 ) * 0.5f, -90.f, 90.f );
		};
		int sel       = -1;
		float sel_err = 0.f;
		for ( int p = 0; p < m_plan_count; p++ ) {
			const plan_t& pl = m_plans[ p ];
			const float err  = eb_aim_err( pl.cmds[ 0 ].origin, pl.end_pos, aim_at( pl.ticks ) );
			if ( sel < 0 || err < sel_err - 0.5f ) {
				sel     = p;
				sel_err = err;
			}
		}
		/* re-plan swaps only for a clearly better heading: 4 deg margin, else the path flickers between near twins */
		if ( replan && sel >= 0 ) {
			const float cur_err = eb_aim_err( m_predicted_cmds[ m_replay_hits ].origin, m_follow_end, aim_at( m_prediction_ticks - m_replay_hits ) );
			const bool keep     = sel_err > cur_err - 4.f;
			botox_dbg_log( "EB: steer %s at %d/%d best=%.1f cur=%.1f of %d steer=%.2f\n", keep ? "keep" : "swap", m_replay_hits, m_prediction_ticks,
			               sel_err, cur_err, m_plan_count, delta_angle.m_y );
			if ( keep )
				sel = -1;
		}
		if ( sel >= 0 ) {
			const plan_t& pl   = m_plans[ sel ];
			const int now_tick = g_interfaces.m_global_vars_base->m_tick_count;
			std::memcpy( m_predicted_cmds, pl.cmds, sizeof( predicted_cmd_t ) * ( pl.ticks + 1 ) );
			m_found            = true;
			m_prediction_ticks = pl.ticks;
			last_variant       = pl.variant;
			m_found_tick       = now_tick + ( pl.ticks + 1 );
			m_replay_hits      = 0;
			m_follow_end       = pl.end_pos;
			last_bend          = pl.bend;
			last_fam           = pl.fam;
			last_bug_duck      = pl.duck ? 1 : 0;
			last_st            = ( allow_duck ? 1 : 0 ) | ( allow_stand ? 2 : 0 );
			m_viz_count        = 0;
			m_viz_alts         = 0;
			m_viz_base         = now_tick;
			if ( GET_VARIABLE( g_variables.m_edgebug_visualize, bool ) ) {
				for ( int p = 0; p <= pl.ticks && p < k_predicted_cmd_max; p++ )
					m_viz_path[m_viz_count++] = pl.cmds[p].origin;
				m_viz_path[m_viz_count++] = pl.end_pos;
				m_viz_pos = pl.end_pos;
				/* the other plans drawn faded */
				for ( int a = 0; a < m_plan_count; a++ ) {
					int n = 0;
					for ( int p = 0; p <= m_plans[ a ].ticks && p < k_predicted_cmd_max; p++ )
						m_viz_alt[ a ][ n++ ] = m_plans[ a ].cmds[ p ].origin;
					m_viz_alt[ a ][ n++ ] = m_plans[ a ].end_pos;
					m_viz_alt_count[ a ]  = n;
					m_viz_alt_pos[ a ]    = m_plans[ a ].end_pos;
				}
				m_viz_alts     = m_plan_count;
				m_viz_sel      = sel;
				m_viz_alt_base = now_tick;
			}
			eb_n_hit++;
		}
		/* every search logs. win = slider window, run = trimmed window, reach = first hull contact, edge = lip
		   found, base = distinct variants run, sw / rel = press / release runs, sims = spent / ceiling,
		   us = spent / share, tick = tick the bug is due. rf / wd = refine / full-window runs, snp = runs resumed off a
		   snapshot, fam = picked plan's family ( 0 base 1 press 2 release 3 bend 4 ebanat 5 refine 6 / 7 wide press / release ). */
		botox_dbg_log( "EB: mode=%d win=%d run=%d reach=%d edge=%d vars=%d base=%d sw=%d rel=%d sims=%d/%d cut=%d us=%lld/%lld con=%d ipt=%.5f vz=%.2f found=%d var=%d at=%d tick=%d pth=%d/%d bd=%d bnd=%.2f ebv=%d rf=%d wd=%d snp=%d fam=%d st=%d srej=%d bst=%d\n",
		               detection_mode, max_ticks, run_ticks, reach_tick, m_edge_target_valid ? 1 : 0,
		               search_count, base_runs, sweep_runs, release_runs, total_predictions, MAX_PREDICTIONS, tail_cut_ticks,
		               budget.used_us( ), static_cast< long long >( n_tick::interval( ) * 1000000.f * budget_share ),
		               best_contact, n_tick::interval( ), m_backup_velocity.m_z, m_found ? 1 : 0, last_variant,
		               m_found ? m_prediction_ticks : -1, m_found ? m_found_tick : 0, m_plan_count, want_paths, bend_runs, last_bend, eb_runs, refine_runs, wide_runs, snap_runs, last_fam,
		               ( allow_duck ? 1 : 0 ) | ( allow_stand ? 2 : 0 ), stance_rej, m_found ? last_bug_duck : -1 );
	} while ( false );
	if ( m_found ) {
		ReStorePrediction( );
		const bool spent = m_replay_hits > m_prediction_ticks;
		const float dist = spent ? 0.f : g_ctx.m_local->get_origin( ).dist_to( m_predicted_cmds[m_replay_hits].origin );
		if ( spent || dist > 1.f ) {
			/* dist = plan vs world disagree: the REPLAY diverged ( another cmd writer or a server correction ). */
			/* m_backup_flags = start of this cmd = end of the bug cmd: real stance vs the sim's */
			if ( spent )
				botox_dbg_log( "EB: drop why=spent at %d/%d duck=%d sim=%d st=%d vz=%.2f\n", m_replay_hits, m_prediction_ticks,
				               ( m_backup_flags & fl_ducking ) ? 1 : 0, last_bug_duck, last_st, m_backup_velocity.m_z );
			else
				botox_dbg_log( "EB: replay drop at %d/%d dist=%.3f\n", m_replay_hits, m_prediction_ticks, dist );
			m_replay_hits = 0;
			m_found = false;
			return;
		}
		if ( m_predicted_cmds[m_replay_hits].buttons & in_duck ) {
			cmd->m_buttons |= in_duck;
			m_ducked = true;
		} else {
			cmd->m_buttons &= ~in_duck;
			/* drop the hold too, or m_ducked re-presses against a release plan. */
			m_ducked = false;
		}
		if ( m_backup_flags & 1 )
			cmd->m_buttons |= in_jump;
		else
			cmd->m_buttons &= ~in_jump;
		cmd->m_forward_move = m_predicted_cmds[m_replay_hits].forwardmove;
		cmd->m_side_move = m_predicted_cmds[m_replay_hits].sidemove;
		cmd->m_view_point = m_predicted_cmds[m_replay_hits].viewangle;
		/* always silent: fold the replay's angle into fwd/side, real view handed back. */
		start_movement_fix( cmd );
		cmd->m_view_point = orig_viewangle;
		end_movement_fix( cmd );
		m_replay_hits++;
	}
}

void n_edgebug::impl_t::render( )
{
	const bool on = GET_VARIABLE( g_variables.m_edgebug_visualize, bool ) && m_found && m_viz_count >= 2;
	const float dt = std::clamp( g_interfaces.m_global_vars_base->m_abs_frame_time, 0.f, 0.1f );
	const float k  = 1.f - expf( -18.f * dt );
	const int cur  = m_viz_base + m_replay_hits;
	const auto ease = [ & ]( viz_disp_t& d, const c_vector* path, const int count, const int base, const c_vector& pad ) {
		const int from  = std::clamp( cur - base, 0, count - 1 );
		const int n     = count - from;
		const int tb    = base + from;
		const int shift = tb - d.base;
		if ( d.n < 2 || shift < 0 || shift >= d.n ) {
			std::memcpy( d.pts, path + from, sizeof( c_vector ) * n );
			d.n    = n;
			d.base = tb;
			d.pad  = pad;
			return;
		}
		if ( shift > 0 ) {
			std::memmove( d.pts, d.pts + shift, sizeof( c_vector ) * ( d.n - shift ) );
			d.n -= shift;
			d.base = tb;
		}
		const c_vector old_end = d.pts[ d.n - 1 ];
		const int m            = std::max( n, d.n );
		for ( int i = 0; i < m; i++ ) {
			const c_vector& to = path[ from + std::min( i, n - 1 ) ];
			const c_vector& at = i < d.n ? d.pts[ i ] : old_end;
			d.pts[ i ]         = at + ( to - at ) * k;
		}
		d.n = m;
		while ( d.n > n && d.pts[ d.n - 1 ].dist_to( d.pts[ d.n - 2 ] ) < 0.5f )
			d.n--;
		d.pad = d.pad + ( pad - d.pad ) * k;
	};
	for ( int a = 0; a < k_plan_max; a++ ) {
		viz_disp_t& d   = m_disp[ a ];
		const bool live = on && a < m_viz_alts && a != m_viz_sel;
		if ( live )
			ease( d, m_viz_alt[ a ], m_viz_alt_count[ a ], m_viz_alt_base, m_viz_alt_pos[ a ] );
		d.alpha += ( ( live ? 0.45f : 0.f ) - d.alpha ) * k;
		if ( d.alpha > 0.02f && d.n >= 2 )
			draw_plan( d.pts, d.n, 0, d.pad, d.alpha );
		else if ( !live )
			d.n = 0;
	}
	viz_disp_t& f = m_disp[ k_plan_max ];
	if ( !on || !g_ctx.m_local ) {
		f.n = 0;
		return;
	}
	ease( f, m_viz_path, m_viz_count, m_viz_base, m_viz_pos );
	if ( f.n < 2 )
		return;
	const c_vector first = f.pts[ 0 ];
	f.pts[ 0 ]           = g_ctx.m_local->get_abs_origin( );
	draw_plan( f.pts, f.n, 0, f.pad );
	f.pts[ 0 ] = first;
}

void n_edgebug::draw_plan( const c_vector* path, const int count, const int from, const c_vector& pad_pos, const float alpha )
{
	if ( count < 2 || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || !g_interfaces.m_engine_client->is_in_game( ) )
		return;
	const unsigned int color = GET_VARIABLE( g_variables.m_edgebug_visualize_color, c_color ).get_u32( alpha );
	const float thickness    = GET_VARIABLE( g_variables.m_edgebug_visualize_thickness, float );
	auto line = [ & ]( const c_vector& a, const c_vector& b ) {
		c_vector_2d sa, sb;
		if ( g_render.world_to_screen( a, sa ) && g_render.world_to_screen( b, sb ) )
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line,
			                                   std::make_any< line_draw_object_t >( line_draw_object_t{ sa, sb, color, thickness } ) );
	};
	for ( int i = std::clamp( from, 0, count - 1 ); i + 1 < count; i++ )
		line( path[i], path[i + 1] );
	const c_vector p       = pad_pos;
	const bool circle      = GET_VARIABLE( g_variables.m_edgebug_visualize_shape, int ) == 1;
	const int fill_mode    = GET_VARIABLE( g_variables.m_edgebug_visualize_fill, int );
	const bool projected   = GET_VARIABLE( g_variables.m_edgebug_visualize_projected, bool );
	const float reach      = GET_VARIABLE( g_variables.m_edgebug_visualize_size, float );
	constexpr int k_max_pts = 32; /* = poly_draw_object_t cap, fill shares the ring */
	/* projected square walks its edges ( 8 a side ) so the outline bends over steps instead of cutting through */
	const int pts = circle || projected ? k_max_pts : 4;
	const int per_side = pts / 4;
	c_vector pad[k_max_pts];
	for ( int i = 0; i < pts; i++ ) {
		if ( circle ) {
			const float a = 6.28318530718f * static_cast< float >( i ) / static_cast< float >( pts );
			pad[i] = { p.m_x + cosf( a ) * reach, p.m_y + sinf( a ) * reach, p.m_z };
		}
		else {
			const int s0 = i / per_side, s1 = ( s0 + 1 ) % 4;
			const float t = static_cast< float >( i % per_side ) / static_cast< float >( per_side );
			const auto cx = [ & ]( const int s ) { return s < 2 ? -reach : reach; };
			const auto cy = [ & ]( const int s ) { return s == 1 || s == 2 ? reach : -reach; };
			pad[i] = { p.m_x + cx( s0 ) + ( cx( s1 ) - cx( s0 ) ) * t, p.m_y + cy( s0 ) + ( cy( s1 ) - cy( s0 ) ) * t, p.m_z };
		}
	}
	if ( projected ) {
		/* each sample drops onto floor within step height ( 18u ) of the pad. void / in-wall samples take the
		   neighbour's floor height, never fall to the floor below the lip. paint thread, not the search clock. */
		c_trace_filter filter( g_ctx.m_local );
		bool hit[k_max_pts];
		int first = -1;
		for ( int i = 0; i < pts; i++ ) {
			ray_t ray( c_vector( pad[i].m_x, pad[i].m_y, p.m_z + 18.f ), c_vector( pad[i].m_x, pad[i].m_y, p.m_z - 18.f ) );
			trace_t tr;
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
			hit[i] = tr.did_hit( ) && !tr.m_start_solid;
			if ( hit[i] ) {
				pad[i].m_z = tr.m_end.m_z + 0.5f;
				if ( first < 0 )
					first = i;
			}
		}
		for ( int j = 1; first >= 0 && j < pts; j++ ) {
			const int i = ( first + j ) % pts;
			if ( !hit[i] )
				pad[i].m_z = pad[( i + pts - 1 ) % pts].m_z;
		}
	}
	if ( fill_mode != 0 ) {
		// one convex poly, never a centre fan: imgui AA-fringes each triangle = bright spokes
		const unsigned int fill_color = GET_VARIABLE( g_variables.m_edgebug_visualize_pad_color, c_color )
			.get_u32( GET_VARIABLE( g_variables.m_edgebug_visualize_fill_alpha, float ) * alpha );
		poly_draw_object_t poly{ };
		poly.m_color = fill_color;
		bool all_on_screen = true;
		for ( int i = 0; i < pts; i++ ) {
			if ( !g_render.world_to_screen( pad[i], poly.m_points[i] ) ) {
				all_on_screen = false;
				break;
			}
		}
		if ( all_on_screen ) {
			poly.m_count = pts;
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_poly, std::make_any< poly_draw_object_t >( poly ) );
		}
	}
	if ( fill_mode != 1 )
		for ( int i = 0; i < pts; i++ )
			line( pad[i], pad[( i + 1 ) % pts] );
}

namespace {
	float donor_yaw( const float y ) { return std::remainderf( y, 360.f ); }

	float donor_mouse_yaw( const c_user_cmd* cmd )
	{
		static auto m_yaw = g_interfaces.m_convar->find_var( "m_yaw" );
		static auto sens  = g_interfaces.m_convar->find_var( "sensitivity" );
		return std::clamp( static_cast< float >( cmd->m_mouse_delta_x ) * ( m_yaw ? m_yaw->get_float( ) : 0.022f ) *
		                       ( sens ? sens->get_float( ) : 1.f ),
		                   -30.f, 30.f );
	}

	void donor_fix_move( c_user_cmd* cmd, const c_angle& angle )
	{
		if ( !( g_ctx.m_local->get_flags( ) & fl_onground ) && cmd->m_view_point.m_z != 0.f && ( cmd->m_buttons & in_attack ) )
			cmd->m_side_move = 0.f;
		const float len = sqrtf( cmd->m_forward_move * cmd->m_forward_move + cmd->m_side_move * cmd->m_side_move );
		if ( len == 0.f )
			return;
		const float yaw = deg2rad( rad2deg( atan2f( cmd->m_side_move, cmd->m_forward_move ) ) + cmd->m_view_point.m_y - angle.m_y );
		float fwd       = cosf( yaw ) * len;
		if ( cmd->m_view_point.m_x < -90.f || cmd->m_view_point.m_x > 90.f )
			fwd = -fwd;
		cmd->m_forward_move = std::clamp( fwd, -450.f, 450.f );
		cmd->m_side_move    = std::clamp( sinf( yaw ) * len, -450.f, 450.f );
		cmd->m_up_move      = std::clamp( cmd->m_up_move, -320.f, 320.f );
		cmd->m_buttons &= ~( in_forward | in_back | in_moveright | in_moveleft );
	}
}

void n_edgebug::impl_t::donor_reset( )
{
	m_donor       = { };
	m_found       = false;
	m_replay_hits = 0;
}

bool n_edgebug::impl_t::donor_check( c_user_cmd* cmd, bool& brk )
{
	const auto local = g_ctx.m_local;
	if ( const int mt = local->get_move_type( ); mt == move_type_ladder || mt == move_type_noclip ) {
		brk = true;
		return false;
	}
	const auto& start      = g_prediction.backup_data;
	const c_vector vel     = local->get_velocity( );
	static auto sv_gravity = g_interfaces.m_convar->find_var( "sv_gravity" );
	const float gravity    = sv_gravity ? sv_gravity->get_float( ) : 800.f;
	const float interval   = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float tick_rate  = interval > 0.f ? 1.f / interval : 0.f;
	const float grav_velo  = ( ( gravity / 2.f ) / tick_rate ) * -1.f;
	if ( std::roundf( vel.m_z ) >= 0.f || ( local->get_flags( ) & fl_onground ) || std::roundf( vel.length_2d( ) ) == 0.f ) {
		brk = true;
		return false;
	}
	if ( start.m_velocity.m_z < 0.f && vel.m_z > start.m_velocity.m_z && vel.m_z < 0.f ) {
		if ( start.m_origin.m_z < local->get_origin( ).m_z )
			return false;
		const int z_vel = static_cast< int >( vel.m_z );
		++local->get_tick_base( );
		g_prediction.begin( local, cmd, false, true );
		g_prediction.end( local );
		const float rounded = std::roundf( -gravity * interval ) + static_cast< float >( z_vel );
		const float next_vz = std::roundf( local->get_velocity( ).m_z );
		if ( rounded == next_vz || ( next_vz == 0.f && ( local->get_flags( ) & fl_onground ) ) )
			return true;
		brk = true;
		return false;
	}
	return start.m_velocity.m_z < grav_velo && std::roundf( vel.m_z ) == std::roundf( grav_velo ) && local->get_move_type( ) != move_type_ladder;
}

void n_edgebug::impl_t::donor_pre( c_user_cmd* cmd )
{
	if ( m_found && m_donor.crouched && m_donor.ticks_left )
		cmd->m_buttons |= in_duck;
	if ( m_found && m_donor.ticks_left && m_donor.strafing ) {
		cmd->m_forward_move = m_donor.forwardmove;
		cmd->m_side_move    = m_donor.sidemove;
	}
}

void n_edgebug::impl_t::donor_post( c_user_cmd* cmd, const int style )
{
	const auto local = g_ctx.m_local;
	if ( !local || !local->is_alive( ) || !g_interfaces.m_engine_client->is_connected( ) ) {
		donor_reset( );
		return;
	}
	const bool del      = style == 1;
	const auto& start   = g_prediction.backup_data;
	const int move_type = local->get_move_type( );
	const bool key      = GET_VARIABLE( g_variables.edge_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) );
	if ( del ) {
		if ( ( start.m_flags & fl_onground ) || move_type == move_type_noclip || move_type == move_type_observer || move_type == move_type_ladder ) {
			donor_reset( );
			return;
		}
		if ( !key && ( !m_donor.ticks_left || m_donor.ticks_left > 2 ) ) {
			donor_reset( );
			return;
		}
	} else if ( !key || ( start.m_flags & fl_onground ) || move_type != move_type_walk ) {
		donor_reset( );
		return;
	}
	if ( m_backup_move_type == move_type_ladder || g_air_stuck_owns_cmd || g_wall_climb.caught( cmd ) || ps_latched( ) || eb_cmd_taken( ) ) {
		donor_reset( );
		return;
	}

	const c_angle view_backup = cmd->m_view_point;
	if ( !m_donor.ticks_left ) {
		float yawdelta        = donor_mouse_yaw( cmd );
		const float interval  = g_interfaces.m_global_vars_base->m_interval_per_tick;
		const int ticklimit   = std::clamp( del ? GET_VARIABLE( g_variables.m_edgebug_del_ticks, int )
		                                        : static_cast< int >( 0.5f + GET_VARIABLE( g_variables.m_edgebug_dna_scan, float ) / interval ),
		                                    0, k_predicted_cmd_max );
		const bool advanced   = del ? GET_VARIABLE( g_variables.m_edgebug_del_advanced, bool ) : GET_VARIABLE( g_variables.m_edgebug_dna_advanced, bool );
		const int pred_rounds = ( advanced && yawdelta != 0.f ) ? 4 : 2;
		const int search      = del ? GET_VARIABLE( g_variables.m_edgebug_del_search, int ) : std::max( GET_VARIABLE( g_variables.m_edgebug_dna_range, int ), 1 );
		const int divisor     = del || advanced ? search : 2;
		const float del_limit = GET_VARIABLE( g_variables.m_edgebug_del_angle_limit, float );
		const bool dna_custom = GET_VARIABLE( g_variables.m_edgebug_dna_custom_angle, bool );
		const float dna_limit = GET_VARIABLE( g_variables.m_edgebug_dna_angle_limit, float );
		const c_user_cmd base = *cmd;
		const c_angle originalangles = base.m_view_point;
		const c_vector end_vel = local->get_velocity( );
		m_donor.startingyaw    = originalangles.m_y;
		const int tick_count   = g_interfaces.m_global_vars_base->m_tick_count;
		c_vector path[ k_predicted_cmd_max + 1 ];
		int sims = 0;
		/* dna rising: every run breaks after its 1st sim. strafe rounds break before any sim at del limit 0 / dna limit != 0 */
		const bool dna_rising  = !del && ( end_vel.m_z > 0.f || start.m_velocity.m_z > 0.f );
		const bool strafe_runs = !dna_rising && divisor != 0 && ( del ? del_limit != 0.f : dna_limit == 0.f );
		/* rounds 0/1 have no wish = ballistic: nothing solid in the swept start hull = no contact = no find */
		bool still_runs = !dna_rising && ticklimit > 0;
		int reach       = -1;
		if ( still_runs ) {
			ReStorePrediction( );
			const int window = ticklimit + 1;
			reach            = eb_first_reach_tick( eb_make_hull( ), local->get_origin( ), local->get_velocity( ), window,
			                                        std::clamp( static_cast< int >( std::ceil( window * interval / 0.1f ) ), 3, 16 ) );
			still_runs       = reach >= 0;
		}
		/* contact far ( or none ): search every 2nd tick, a far bug found 1 tick later is the same bug */
		static int s_last_search = -1;
		const bool far_skip = !dna_rising && ticklimit > 0 && ( reach < 0 || reach > n_tick::ticks( 16 ) ) && s_last_search >= 0 && tick_count > s_last_search &&
		                      tick_count - s_last_search < n_tick::ticks( 2 );
		if ( far_skip )
			still_runs = false;
		else
			s_last_search = tick_count;
		/* exact donor fan lists ( round 3 fans from round 2's overshoot ), walked under a clock below */
		float fan[ 2 ][ 64 ];
		int fan_n[ 2 ] = { };
		if ( strafe_runs && pred_rounds > 2 && !far_skip ) {
			float yd = yawdelta;
			for ( int r = 0; r < 2; ++r ) {
				const float max_delta = yd;
				for ( yd = max_delta / divisor; fabsf( yd ) <= fabsf( max_delta ) && fan_n[ r ] < 64; yd += max_delta / divisor )
					fan[ r ][ fan_n[ r ]++ ] = yd;
			}
		}
		const long long t0 = n_tick::qpc_now( );
		const auto used_us = [ t0 ]( ) { return ( n_tick::qpc_now( ) - t0 ) * 1000000ll / n_tick::qpc_freq( ); };

		const auto run = [ & ]( const int round, const float yd ) {
			const bool crouched = round == 0 || round == 2, strafing = round > 1;
			*cmd = base;
			if ( crouched )
				cmd->m_buttons |= in_duck;
			else
				cmd->m_buttons &= ~in_duck;
			if ( !strafing ) {
				cmd->m_forward_move = 0.f;
				cmd->m_side_move    = 0.f;
				if ( !del )
					cmd->m_up_move = 0.f;
			} else if ( !del ) {
				cmd->m_side_move = round == 2 && originalangles.m_y <= 0.f ? base.m_side_move * -450.f : base.m_side_move * 450.f;
			}
			ReStorePrediction( );
			const int tick_base0 = local->get_tick_base( );
			int path_n      = 0;
			path[ path_n++ ] = local->get_origin( );
			for ( int t = 1; t <= ticklimit; ++t ) {
				if ( strafing ) {
					cmd->m_view_point.m_y = donor_yaw( originalangles.m_y + yd * static_cast< float >( t ) );
					const float turned    = fabsf( cmd->m_view_point.m_y - m_donor.startingyaw );
					if ( del ) {
						if ( del_limit == 0.f || turned > del_limit )
							break;
					} else {
						if ( ( ( turned > ( dna_custom ? 1.f : 0.f ) ) ? dna_limit : 1.f ) != 0.f )
							break;
					}
				}
				/* donors' end( ) steps the tickbase per sim: curtime must move ( duck gate ), restore puts it back */
				local->get_tick_base( ) = tick_base0 + t - 1;
				g_prediction.begin( local, cmd, false, true );
				g_prediction.end( local );
				sims++;
				const c_vector vel = local->get_velocity( );
				if ( local->get_move_type( ) == move_type_ladder ||
				     ( del ? vel.m_z > 0.f || vel.length_2d( ) == 0.f : end_vel.m_z > 0.f || start.m_velocity.m_z > 0.f ) )
					break;
				if ( path_n <= k_predicted_cmd_max )
					path[ path_n++ ] = local->get_origin( );
				bool br = false;
				if ( donor_check( cmd, br ) ) {
					m_donor.ticks_left  = t;
					m_donor.eblength    = t;
					m_donor.edgebugtick = tick_count + t;
					m_donor.detecttick  = tick_count;
					m_donor.forwardmove = cmd->m_forward_move;
					m_donor.sidemove    = cmd->m_side_move;
					m_donor.yawdelta    = yd;
					m_donor.crouched    = crouched;
					m_donor.strafing    = strafing;
					m_donor.mousedx     = base.m_mouse_delta_x;
					std::memcpy( m_viz_path, path, sizeof( c_vector ) * path_n );
					m_viz_count = path_n;
					m_viz_pos   = local->get_origin( );
					m_viz_base  = tick_count;
					m_viz_alts  = 0;
					m_viz_sel   = -1;
					break;
				}
				if ( br )
					break;
			}
		};

		int found_round = -1;
		for ( int round = 0; round < 2 && still_runs && found_round < 0; ++round ) {
			run( round, yawdelta );
			if ( m_donor.ticks_left )
				found_round = round;
		}
		/* strafe fan over 0.2 tick of clock ( rounds 0/1 never cut ): a cut fan resumes where it stopped next search */
		static int s_fan_resume = 0;
		const int fan_total     = fan_n[ 0 ] + fan_n[ 1 ];
		bool fan_cut            = false;
		if ( found_round < 0 && fan_total > 0 ) {
			const long long cap_us = static_cast< long long >( n_tick::engine_interval( ) * 200000.f );
			const int first        = s_fan_resume % fan_total;
			int i                  = 0;
			for ( ; i < fan_total; ++i ) {
				if ( used_us( ) >= cap_us )
					break;
				const int k = ( first + i ) % fan_total, r = k < fan_n[ 0 ] ? 0 : 1;
				run( 2 + r, fan[ r ][ r ? k - fan_n[ 0 ] : k ] );
				if ( m_donor.ticks_left ) {
					found_round = 2 + r;
					break;
				}
			}
			fan_cut      = found_round < 0 && i < fan_total;
			s_fan_resume = fan_cut ? ( first + i ) % fan_total : 0;
		}
		static int n_search = 0, n_skip = 0, n_far = 0, n_cut = 0, n_sims = 0, n_peak = 0;
		static long long n_us = 0, n_peak_us = 0;
		const long long us = used_us( );
		n_search++;
		n_skip += !still_runs && !far_skip;
		n_far += far_skip;
		n_cut += fan_cut;
		n_sims += sims;
		n_peak    = std::max( n_peak, sims );
		n_us += us;
		n_peak_us = std::max( n_peak_us, us );
		if ( static unsigned long long next_report = 0ull; GetTickCount64( ) >= next_report ) {
			next_report = GetTickCount64( ) + 1000ull;
			botox_dbg_log( "EB: style=%d gate searches=%d skip=%d far=%d cut=%d sims=%d peak=%d us=%lld peak_us=%lld\n", style, n_search, n_skip,
			               n_far, n_cut, n_sims, n_peak, n_us, n_peak_us );
			n_search = n_skip = n_far = n_cut = n_sims = n_peak = 0;
			n_us = n_peak_us = 0;
		}
		*cmd = base;
		ReStorePrediction( );
		if ( m_donor.ticks_left )
			botox_dbg_log( "EB: style=%d find round=%d t=%d duck=%d strafe=%d yd=%.2f fwd=%.0f side=%.0f sims=%d lim=%d vz=%.2f\n", style,
			               found_round, m_donor.eblength, m_donor.crouched ? 1 : 0, m_donor.strafing ? 1 : 0, m_donor.yawdelta,
			               m_donor.forwardmove, m_donor.sidemove, sims, ticklimit, start.m_velocity.m_z );
	}

	if ( !m_donor.ticks_left ) {
		m_found = false;
		return;
	}
	m_found            = true;
	m_ducked           = false;
	m_found_tick       = m_donor.edgebugtick;
	m_prediction_ticks = m_donor.eblength;
	const float yaw    = donor_yaw( m_donor.startingyaw + m_donor.yawdelta * static_cast< float >( m_donor.eblength - ( m_donor.ticks_left - 1 ) ) );
	if ( m_donor.crouched )
		cmd->m_buttons |= del ? in_duck : ( in_duck | in_bullrush );
	else
		cmd->m_buttons &= ~in_duck;
	if ( m_donor.strafing ) {
		cmd->m_forward_move = m_donor.forwardmove;
		cmd->m_side_move    = m_donor.sidemove;
		if ( del ) {
			const c_angle new_view( cmd->m_view_point.m_x, yaw, cmd->m_view_point.m_z );
			cmd->m_view_point.m_y = yaw;
			if ( GET_VARIABLE( g_variables.m_edgebug_del_silent, bool ) ) {
				cmd->m_view_point = view_backup;
				donor_fix_move( cmd, new_view );
			}
		} else {
			cmd->m_mouse_delta_x = m_donor.mousedx;
			const c_angle ang( view_backup.m_x + 18.f, yaw, 0.f );
			cmd->m_view_point.m_y = yaw;
			donor_fix_move( cmd, ang );
		}
	} else {
		cmd->m_forward_move = 0.f;
		cmd->m_side_move    = 0.f;
	}
	m_donor.ticks_left--;
	m_replay_hits = m_donor.eblength - m_donor.ticks_left;
}

void n_edgebug::impl_t::donor_frame_start( )
{
	const int style = GET_VARIABLE( g_variables.m_edgebug_style, int );
	if ( style == 3 ) {
		hw_frame_start( );
		return;
	}
	if ( style == 4 ) {
		lob_frame_start( );
		return;
	}
	if ( style == 0 || !m_donor.ticks_left || !m_donor.strafing || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;
	if ( style == 1 && GET_VARIABLE( g_variables.m_edgebug_del_silent, bool ) )
		return;
	const float ipt       = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float to_eb     = static_cast< float >( m_donor.edgebugtick - m_donor.detecttick ) * ipt;
	const float from_find = g_interfaces.m_global_vars_base->m_current_time - static_cast< float >( m_donor.detecttick ) * ipt;
	c_angle view( g_ctx.m_first_view_angles.m_x, m_donor.startingyaw, g_ctx.m_first_view_angles.m_z );
	view.m_y += donor_yaw( m_donor.yawdelta * ( static_cast< float >( m_donor.eblength ) * ( from_find / to_eb ) ) );
	g_interfaces.m_engine_client->set_view_angles( view );
}

void n_edgebug::impl_t::donor_mouse_fix( c_user_cmd* cmd )
{
	if ( GET_VARIABLE( g_variables.m_edgebug_style, int ) != 1 || !GET_VARIABLE( g_variables.m_edgebug_del_mouse_fix, bool ) )
		return;
	static c_angle last{ };
	c_angle delta = cmd->m_view_point - last;
	delta.m_y     = std::remainderf( delta.m_y, 360.f );
	delta.clamp( );
	static auto sens = g_interfaces.m_convar->find_var( "sensitivity" );
	if ( !sens )
		return;
	const auto counts = []( const float d ) {
		const int v = static_cast< int >( std::clamp( d, -32768.f, 32767.f ) );
		return static_cast< short >( v == 0 ? 1 : v );
	};
	if ( delta.m_x != 0.f ) {
		static auto m_pitch = g_interfaces.m_convar->find_var( "m_pitch" );
		if ( !m_pitch )
			return;
		cmd->m_mouse_delta_y = counts( ( delta.m_x / m_pitch->get_float( ) ) / sens->get_float( ) );
	}
	if ( delta.m_y != 0.f ) {
		static auto m_yaw = g_interfaces.m_convar->find_var( "m_yaw" );
		if ( !m_yaw )
			return;
		cmd->m_mouse_delta_x = counts( ( delta.m_y / m_yaw->get_float( ) ) / sens->get_float( ) );
	}
	last = cmd->m_view_point;
}

void n_edgebug::impl_t::hw_reset( )
{
	m_hw.ducking       = false;
	m_hw.last_tick     = 0;
	m_hw.ticks_to_stop = 0;
	m_hw.will_edgebug  = false;
	m_hw.will_fail     = false;
}

void n_edgebug::impl_t::hw_detect( c_user_cmd* cmd )
{
	const auto local  = g_ctx.m_local;
	const auto& start = g_prediction.backup_data;
	const int mt      = local->get_move_type( );
	if ( start.m_velocity.m_z > 0 || mt == move_type_ladder || mt == move_type_observer || mt == move_type_noclip ) {
		m_hw.will_edgebug = false;
		m_hw.will_fail    = true;
		return;
	}
	const c_vector vel = local->get_velocity( );
	if ( std::roundf( vel.m_z ) == 0 || start.m_flags & fl_onground ) {
		m_hw.will_edgebug = false;
		m_hw.will_fail    = true;
	} else if ( start.m_velocity.m_z < -6.f && vel.m_z > start.m_velocity.m_z && vel.m_z < -6.f && !( local->get_flags( ) & fl_onground ) &&
	            start.m_origin.m_z > local->get_origin( ).m_z ) {
		const float gravity = g_convars.float_or( HASH_BT( "sv_gravity" ), 800.f );
		if ( std::floor( start.m_velocity.m_z ) < -7 && std::floor( vel.m_z ) == -7 && vel.length_2d( ) >= start.m_velocity.length_2d( ) ) {
			m_hw.will_edgebug = true;
			m_hw.will_fail    = false;
		} else {
			const float previous_velocity = vel.m_z;
			g_prediction.begin( local, cmd );
			g_prediction.end( local );
			const float expected = std::roundf( ( -gravity ) * g_interfaces.m_global_vars_base->m_interval_per_tick + previous_velocity );
			m_hw.will_edgebug    = expected == std::roundf( local->get_velocity( ).m_z );
			m_hw.will_fail       = !m_hw.will_edgebug;
		}
	}
}

void n_edgebug::impl_t::hw_post( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	if ( !local || !local->is_alive( ) ) {
		hw_reset( );
		m_found = false;
		return;
	}
	const auto& start = g_prediction.backup_data;
	/* donor's on_create_move_post returns before edge_bug here: latch kept, nothing applied */
	if ( const int mt = start.m_move_type; mt == move_type_ladder || mt == move_type_noclip || mt == move_type_fly || mt == move_type_observer )
		return;
	const bool key = GET_VARIABLE( g_variables.edge_bug, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) );
	if ( !key || ps_latched( ) || m_backup_move_type == move_type_ladder || g_air_stuck_owns_cmd || g_wall_climb.caught( cmd ) || eb_cmd_taken( ) ) {
		hw_reset( );
		m_found = false;
		return;
	}

	const int ticks       = std::clamp( GET_VARIABLE( g_variables.m_edgebug_hw_ticks, int ), 0, k_predicted_cmd_max - 2 );
	const float yaw_delta = std::clamp( start.m_view_angles.m_y - g_ctx.m_last_tick_yaw, -( 180.f / ticks ), 180.f / ticks );
	const float original_forward_move = cmd->m_forward_move;
	const float original_side_move    = cmd->m_side_move;
	const c_angle original_view_point = cmd->m_view_point;
	const int tick_count              = g_interfaces.m_global_vars_base->m_tick_count;
	const bool was_found              = m_hw.will_edgebug;
	c_vector path[ k_predicted_cmd_max + 1 ];
	int sims = 0;

	const auto loop_through_ticks = [ & ]( const bool ducked, const bool strafe = false ) {
		if ( m_hw.will_edgebug )
			return;
		ReStorePrediction( );
		m_hw.starting_yaw = original_view_point.m_y;
		int path_n        = 0;
		path[ path_n++ ]  = local->get_origin( );
		for ( int i = 0; i <= ticks; i++ ) {
			c_user_cmd sim = *cmd;
			sim.m_buttons |= in_bullrush;
			if ( ducked ) {
				sim.m_buttons |= in_duck;
				local->get_flags( ) |= fl_ducking;
			} else {
				sim.m_buttons &= ~in_duck;
				local->get_flags( ) &= ~fl_ducking;
			}
			if ( !strafe ) {
				m_hw.strafing      = false;
				sim.m_forward_move = 0;
				sim.m_side_move    = 0;
				sim.m_buttons &= ~( in_jump | in_forward | in_back | in_moveleft | in_moveright );
			} else {
				m_hw.strafing          = true;
				sim.m_forward_move     = original_forward_move;
				sim.m_side_move        = original_side_move;
				sim.m_view_point.m_y   = g_math.normalize_angle( original_view_point.m_y + ( yaw_delta * i ) );
			}
			g_prediction.begin( local, &sim );
			g_prediction.end( local );
			sims++;
			if ( path_n <= k_predicted_cmd_max )
				path[ path_n++ ] = local->get_origin( );
			if ( start.m_flags & fl_onground || std::round( start.m_velocity.m_z ) >= 0 || local->get_flags( ) & fl_onground ||
			     local->get_move_type( ) == move_type_ladder ) {
				m_hw.will_edgebug = false;
				break;
			}
			if ( !m_hw.will_edgebug )
				hw_detect( &sim );
			if ( m_hw.will_edgebug ) {
				m_hw.saved_mousedx = static_cast< float >( std::abs( sim.m_mouse_delta_x ) );
				m_hw.ticks_to_stop = i + 1;
				m_hw.last_tick     = tick_count;
				m_hw.ducking       = ducked;
				if ( strafe ) {
					m_hw.yaw_delta    = yaw_delta;
					m_hw.forward_move = original_forward_move;
					m_hw.side_move    = original_side_move;
				} else
					m_hw.forward_move = m_hw.side_move = 0;
				std::memcpy( m_viz_path, path, sizeof( c_vector ) * path_n );
				m_viz_count = path_n;
				m_viz_pos   = local->get_origin( );
				m_viz_base  = tick_count;
				m_viz_alts  = 0;
				m_viz_sel   = -1;
				break;
			}
			if ( m_hw.will_fail ) {
				m_hw.will_fail = false;
				break;
			}
		}
		ReStorePrediction( );
	};

	loop_through_ticks( false );
	loop_through_ticks( true );
	if ( GET_VARIABLE( g_variables.m_edgebug_hw_advanced, bool ) &&
	     yaw_delta < ( GET_VARIABLE( g_variables.m_edgebug_hw_strafe_delta_max, float ) / 10000.f ) && !g_ctx.m_low_fps ) {
		loop_through_ticks( false, true );
		loop_through_ticks( true, true );
	}
	if ( !was_found && m_hw.will_edgebug )
		botox_dbg_log( "EB: style=3 find t=%d duck=%d strafe=%d yd=%.3f fwd=%.0f side=%.0f sims=%d lim=%d vz=%.2f\n", m_hw.ticks_to_stop,
		               m_hw.ducking ? 1 : 0, m_hw.strafing ? 1 : 0, m_hw.yaw_delta, m_hw.forward_move, m_hw.side_move, sims, ticks,
		               start.m_velocity.m_z );

	if ( m_hw.will_edgebug ) {
		ReStorePrediction( );
		if ( tick_count < m_hw.ticks_to_stop + m_hw.last_tick + 1 ) {
			cmd->m_buttons &= ~( in_jump | in_forward | in_back | in_moveleft | in_moveright );
			if ( m_hw.strafing ) {
				cmd->m_side_move      = m_hw.side_move;
				cmd->m_forward_move   = m_hw.forward_move;
				cmd->m_view_point.m_y = g_math.normalize_angle( m_hw.starting_yaw + ( m_hw.yaw_delta * ( tick_count - m_hw.last_tick ) ) );
			} else {
				cmd->m_side_move    = 0.f;
				cmd->m_forward_move = 0.f;
			}
			if ( m_hw.ducking )
				cmd->m_buttons |= in_duck;
			else
				cmd->m_buttons &= ~in_duck;
		} else
			hw_reset( );
	}

	m_found = m_hw.will_edgebug;
	if ( m_found ) {
		m_ducked           = false;
		m_found_tick       = m_hw.last_tick + m_hw.ticks_to_stop;
		m_prediction_ticks = m_hw.ticks_to_stop;
		m_replay_hits      = tick_count - m_hw.last_tick + 1;
	} else
		m_replay_hits = 0;
}

void n_edgebug::impl_t::hw_frame_start( )
{
	if ( !m_hw.will_edgebug || !m_hw.strafing )
		return;
	if ( const int mt = g_prediction.backup_data.m_move_type; mt == move_type_ladder || mt == move_type_noclip || mt == move_type_fly || mt == move_type_observer )
		return;
	c_angle wish_angles( g_prediction.backup_data.m_view_angles.m_x, m_hw.starting_yaw, 0.f );
	const float hit_time_delta = g_math.ticks_to_time( m_hw.ticks_to_stop );
	const float cur_time_delta = g_interfaces.m_global_vars_base->m_current_time - g_math.ticks_to_time( m_hw.last_tick );
	wish_angles.m_y += g_math.normalize_angle( m_hw.yaw_delta * ( m_hw.ticks_to_stop * ( cur_time_delta / hit_time_delta ) ) );
	g_interfaces.m_engine_client->set_view_angles( wish_angles );
}

void n_edgebug::impl_t::hw_mouse( float* x, float* y )
{
	if ( !m_hw.will_edgebug || !x || *x == 0 )
		return;
	const float amt    = GET_VARIABLE( g_variables.m_edgebug_hw_lock_amt, float );
	const int left     = m_hw.last_tick + m_hw.ticks_to_stop - g_interfaces.m_global_vars_base->m_tick_count;
	const float factor = 100.f / ( static_cast< float >( std::pow( m_hw.ticks_to_stop, left / *x ) ) + 100.f + ( amt * m_hw.saved_mousedx ) );
	if ( !std::isnan( factor ) )
		*x *= factor;
	if ( y && *y != 0 )
		*y *= 1.f - ( 0.1f * amt * 10.f );
}

/* style 4 lobotomy v2 = USELESSSRCCODE/lobotomyv2-main/hacks/movement/edgebug.cpp 1:1 */
namespace {
	void lob_correct_movement( c_user_cmd* cmd, c_angle wish_angle, c_angle old_angles )
	{
		if ( old_angles.m_x == wish_angle.m_x && old_angles.m_y == wish_angle.m_y && old_angles.m_z == wish_angle.m_z )
			return;
		c_vector wish_forward, wish_right, wish_up, cmd_forward, cmd_right, cmd_up;
		auto viewangles = old_angles;
		auto movedata   = c_vector( cmd->m_forward_move, cmd->m_side_move, cmd->m_up_move );
		viewangles.normalize( );
		if ( !( g_ctx.m_local->get_flags( ) & 1 ) && viewangles.m_z != 0.f )
			movedata.m_y = 0.f;
		g_math.angle_vectors( wish_angle, &wish_forward, &wish_right, &wish_up );
		g_math.angle_vectors( viewangles, &cmd_forward, &cmd_right, &cmd_up );
		auto v8  = sqrt( wish_forward.m_x * wish_forward.m_x + wish_forward.m_y * wish_forward.m_y ),
		     v10 = sqrt( wish_right.m_x * wish_right.m_x + wish_right.m_y * wish_right.m_y ), v12 = sqrt( wish_up.m_z * wish_up.m_z );
		c_vector wish_forward_norm( 1.0f / v8 * wish_forward.m_x, 1.0f / v8 * wish_forward.m_y, 0.f ),
			wish_right_norm( 1.0f / v10 * wish_right.m_x, 1.0f / v10 * wish_right.m_y, 0.f ), wish_up_norm( 0.f, 0.f, 1.0f / v12 * wish_up.m_z );
		auto v14 = sqrt( cmd_forward.m_x * cmd_forward.m_x + cmd_forward.m_y * cmd_forward.m_y ),
		     v16 = sqrt( cmd_right.m_x * cmd_right.m_x + cmd_right.m_y * cmd_right.m_y ), v18 = sqrt( cmd_up.m_z * cmd_up.m_z );
		c_vector cmd_forward_norm( 1.0f / v14 * cmd_forward.m_x, 1.0f / v14 * cmd_forward.m_y, 1.0f / v14 * 0.0f ),
			cmd_right_norm( 1.0f / v16 * cmd_right.m_x, 1.0f / v16 * cmd_right.m_y, 1.0f / v16 * 0.0f ), cmd_up_norm( 0.f, 0.f, 1.0f / v18 * cmd_up.m_z );
		auto v22 = wish_forward_norm.m_x * movedata.m_x, v26 = wish_forward_norm.m_y * movedata.m_x, v28 = wish_forward_norm.m_z * movedata.m_x,
		     v24 = wish_right_norm.m_x * movedata.m_y, v23 = wish_right_norm.m_y * movedata.m_y, v25 = wish_right_norm.m_z * movedata.m_y,
		     v30 = wish_up_norm.m_x * movedata.m_z, v27 = wish_up_norm.m_z * movedata.m_z, v29 = wish_up_norm.m_y * movedata.m_z;
		c_vector correct_movement;
		correct_movement.m_x = cmd_forward_norm.m_x * v24 + cmd_forward_norm.m_y * v23 + cmd_forward_norm.m_z * v25 +
		                       ( cmd_forward_norm.m_x * v22 + cmd_forward_norm.m_y * v26 + cmd_forward_norm.m_z * v28 ) +
		                       ( cmd_forward_norm.m_y * v30 + cmd_forward_norm.m_x * v29 + cmd_forward_norm.m_z * v27 );
		correct_movement.m_y = cmd_right_norm.m_x * v24 + cmd_right_norm.m_y * v23 + cmd_right_norm.m_z * v25 +
		                       ( cmd_right_norm.m_x * v22 + cmd_right_norm.m_y * v26 + cmd_right_norm.m_z * v28 ) +
		                       ( cmd_right_norm.m_x * v29 + cmd_right_norm.m_y * v30 + cmd_right_norm.m_z * v27 );
		correct_movement.m_z = cmd_up_norm.m_x * v23 + cmd_up_norm.m_y * v24 + cmd_up_norm.m_z * v25 +
		                       ( cmd_up_norm.m_x * v26 + cmd_up_norm.m_y * v22 + cmd_up_norm.m_z * v28 ) +
		                       ( cmd_up_norm.m_x * v30 + cmd_up_norm.m_y * v29 + cmd_up_norm.m_z * v27 );
		cmd->m_forward_move = std::clamp( correct_movement.m_x, -450.f, 450.f );
		cmd->m_side_move    = std::clamp( correct_movement.m_y, -450.f, 450.f );
		cmd->m_up_move      = std::clamp( correct_movement.m_z, -320.f, 320.f );
		cmd->m_buttons &= ~( in_moveright | in_moveleft | in_back | in_forward );
		if ( cmd->m_side_move != 0.0 ) {
			if ( cmd->m_side_move <= 0.0 )
				cmd->m_buttons |= in_moveleft;
			else
				cmd->m_buttons |= in_moveright;
		}
		if ( cmd->m_forward_move != 0.0 ) {
			if ( cmd->m_forward_move <= 0.0 )
				cmd->m_buttons |= in_back;
			else
				cmd->m_buttons |= in_forward;
		}
	}

	/* donor's 4 copy-pasted strafe blocks, each with its own static side / old_yaw */
	void lob_strafe( c_user_cmd* cmd, float& side, float& old_yaw )
	{
		side = -side;
		const c_vector velocity  = g_ctx.m_local->get_velocity( );
		c_angle wish_angle       = cmd->m_view_point;
		const float speed        = velocity.length_2d( );
		const float ideal_strafe = std::clamp( rad2deg( atan( 15.f / speed ) ), 0.f, 90.f );
		cmd->m_forward_move      = 0.f;
		static auto cl_sidespeed = g_interfaces.m_convar->find_var( "cl_sidespeed" );
		const float yaw_delta     = std::remainderf( wish_angle.m_y - old_yaw, 360.f );
		const float abs_yaw_delta = fabsf( yaw_delta );
		old_yaw                   = wish_angle.m_y;
		const float sidespeed     = cl_sidespeed ? cl_sidespeed->get_float( ) : 450.f;
		if ( abs_yaw_delta <= ideal_strafe || abs_yaw_delta >= 30.f ) {
			c_angle velocity_direction;
			g_math.vector_angles( velocity, velocity_direction );
			const float velocity_delta = std::remainderf( wish_angle.m_y - velocity_direction.m_y, 360.0f );
			const float retrack        = std::clamp( rad2deg( atan( 30.f / speed ) ), 0.f, 90.f ) * 2.f;
			if ( velocity_delta <= retrack || speed <= 15.f ) {
				if ( -retrack <= velocity_delta || speed <= 15.0f ) {
					wish_angle.m_y += side * ideal_strafe;
					cmd->m_side_move = sidespeed * side;
				} else {
					wish_angle.m_y   = velocity_direction.m_y - retrack;
					cmd->m_side_move = sidespeed;
				}
			} else {
				wish_angle.m_y   = velocity_direction.m_y + retrack;
				cmd->m_side_move = -sidespeed;
			}
			lob_correct_movement( cmd, wish_angle, cmd->m_view_point );
		} else if ( yaw_delta > 0.f )
			cmd->m_side_move = -sidespeed;
		else
			cmd->m_side_move = sidespeed;
	}
} // namespace

void n_edgebug::impl_t::lob_pre( c_user_cmd* cmd )
{
	m_lob.vel_backup  = g_ctx.m_local->get_velocity( );
	m_lob.view_backup = cmd->m_view_point;
}

bool n_edgebug::impl_t::lob_check( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	if ( !local || !local->is_alive( ) || !g_interfaces.m_engine_client->is_in_game( ) || !g_interfaces.m_engine_client->is_connected( ) )
		return false;
	if ( const auto mt = local->get_move_type( ); mt == move_type_ladder || mt == move_type_noclip )
		return false;
	/* donor caches the VALUE on first call */
	static const float sv_gravity = [ ] {
		const auto var = g_interfaces.m_convar->find_var( "sv_gravity" );
		return var ? var->get_float( ) : 800.f;
	}( );
	const float interval    = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float gravity_vel = sv_gravity * 0.5f * interval;
	if ( m_lob.vel_backup.m_z < -gravity_vel && std::round( local->get_velocity( ).m_z ) == -std::round( gravity_vel ) )
		return true;
	if ( m_lob.vel_backup.m_z < -6.0f && local->get_velocity( ).m_z > m_lob.vel_backup.m_z && local->get_velocity( ).m_z < -6.0f ) {
		const float vz_before = local->get_velocity( ).m_z;
		c_vector check_origin = local->get_abs_origin( );
		check_origin.m_z += 200.f;
		g_prediction.begin( local, cmd );
		g_prediction.end( local );
		static auto sv_gravity_live = g_interfaces.m_convar->find_var( "sv_gravity" );
		const float expected        = roundf( -( sv_gravity_live ? sv_gravity_live->get_float( ) : 800.f ) * interval + vz_before );
		if ( expected == std::round( local->get_velocity( ).m_z ) ) {
			constexpr double k_pi = 3.14159265358979323846;
			const float step      = static_cast< float >( k_pi ) * 2.0f / 16;
			for ( float a = 0; a < ( k_pi * 2.0f ); a += step ) {
				const c_vector start( 32.f * cosf( a ) + check_origin.m_x, 32.f * sinf( a ) + check_origin.m_y, check_origin.m_z );
				const c_vector end = start - c_vector( 0, 0, 300 );
				trace_t tr;
				ray_t ray( start, end );
				c_trace_filter filter( local );
				g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
				if ( tr.m_fraction != 1.f && tr.m_plane.m_normal.m_z < 0.6f ) {
					m_lob.brk = true;
					return false;
				}
			}
			return true;
		}
	}
	return false;
}

void n_edgebug::impl_t::lob_post( c_user_cmd* cmd )
{
	auto& s          = m_lob;
	const auto local = g_ctx.m_local;
	const bool was   = s.found;
	int sims = 0, found_v = -1;
	[ & ] {
		if ( !GET_VARIABLE( g_variables.edge_bug, bool ) || !g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) ) ) {
			s.found  = false;
			s.ticks  = 0;
			s.ducked = false;
			return;
		}
		if ( !local ) {
			s.found = false;
			s.ticks = 0;
			return;
		}
		if ( !local->is_alive( ) )
			return;
		/* botox owners ( air stuck / wall climb / ps / binds / ladder ) = key up */
		if ( m_backup_move_type == move_type_ladder || ps_latched( ) || g_air_stuck_owns_cmd || g_wall_climb.caught( cmd ) || eb_cmd_taken( ) ) {
			s.found  = false;
			s.ticks  = 0;
			s.ducked = false;
			return;
		}

		if ( s.ducked ) {
			if ( !( m_backup_flags & 1 ) )
				cmd->m_buttons &= ~in_jump;
			else
				cmd->m_buttons |= in_jump;
			cmd->m_buttons |= in_duck;
		}
		if ( m_backup_flags & 1 ) {
			s.ducked = false;
			return;
		}
		if ( !s.found && s.ducked && m_backup_velocity.m_z < -40.f )
			s.ducked = false;
		const bool mega = GET_VARIABLE( g_variables.m_edgebug_lob_mega, bool );
		/* donor writes the config var */
		if ( mega )
			GET_VARIABLE( g_variables.m_edgebug_lob_strafe_angle, float ) = 179.f;
		const auto is_large_angle_difference = []( const float yaw1, const float yaw2 ) {
			float diff = std::abs( yaw1 - yaw2 );
			if ( diff > 180.0f )
				diff = 360.0f - diff;
			return diff >= GET_VARIABLE( g_variables.m_edgebug_lob_strafe_angle, float );
		};
		const int buttons_backup     = cmd->m_buttons;
		const float forward_backup   = cmd->m_forward_move;
		const float side_backup      = cmd->m_side_move;
		const c_angle view_backup    = cmd->m_view_point;
		const bool silent            = GET_VARIABLE( g_variables.m_edgebug_lob_silent, bool );
		const bool autostrafe        = GET_VARIABLE( g_variables.m_edgebug_lob_autostrafe, bool );
		const int delta_type         = GET_VARIABLE( g_variables.m_edgebug_lob_delta_type, int );
		float delta_scale            = 0.f;
		if ( delta_type == 0 )
			delta_scale = 1.f;
		if ( delta_type == 1 )
			delta_scale = 0.8f;
		if ( delta_type == 2 )
			delta_scale = 0.5f;
		if ( delta_type == 3 )
			delta_scale = 0.25f;
		if ( mega )
			delta_scale = 1.f;
		const c_angle delta_angle = ( view_backup - s.new_angle ) * delta_scale;
		s.new_angle               = cmd->m_view_point;
		int search                = GET_VARIABLE( g_variables.m_edgebug_lob_advanced, bool ) ? 4 : 2;
		if ( mega )
			search = 6;
		/* donor slider 1..64 into NewCmd[ 128 ]; replay reads cmds[ ticks + 1 ] */
		const int ticks = std::clamp( GET_VARIABLE( g_variables.m_edgebug_lob_ticks, int ), 0, 126 );

		if ( !s.found ) {
			for ( int v = 0; v < search; v++ ) {
				if ( s.last_type ) {
					v           = s.last_type;
					s.last_type = 0;
				}
				ReStorePrediction( );
				s.vel_backup        = local->get_velocity( );
				cmd->m_view_point   = view_backup;
				cmd->m_forward_move = forward_backup;
				cmd->m_side_move    = side_backup;
				cmd->m_buttons      = buttons_backup;
				c_angle current_angle = cmd->m_view_point;
				const auto view_turn  = [ & ] {
					cmd->m_forward_move = forward_backup;
					cmd->m_side_move    = side_backup;
					if ( !is_large_angle_difference( current_angle.m_y, view_backup.m_y ) )
						current_angle = ( current_angle + delta_angle ).normalize( ).clamp( );
					cmd->m_view_point = current_angle;
					if ( silent ) {
						start_movement_fix( cmd );
						cmd->m_view_point = view_backup;
						end_movement_fix( cmd );
					}
				};
				for ( int i = 0; i < ticks; i++ ) {
					if ( s.found || local->get_flags( ) & 1 || local->get_velocity( ).m_z > 0.f )
						break;
					if ( v == 0 || v == 1 ) {
						if ( v == 1 )
							cmd->m_buttons |= in_duck;
						else
							cmd->m_buttons &= ~in_duck;
						cmd->m_forward_move = 0;
						cmd->m_side_move    = 0;
					} else if ( mega ) {
						if ( v == 3 || v == 5 )
							cmd->m_buttons |= in_duck;
						else
							cmd->m_buttons &= ~in_duck;
						if ( v == 2 || v == 3 )
							view_turn( );
						else if ( v == 4 || v == 5 ) {
							auto& st = s.strafe[ v - 4 ];
							lob_strafe( cmd, st.side, st.old_yaw );
						}
					} else if ( v == 2 || v == 3 ) {
						if ( v == 3 )
							cmd->m_buttons |= in_duck;
						else
							cmd->m_buttons &= ~in_duck;
						if ( autostrafe ) {
							auto& st = s.strafe[ v ];
							lob_strafe( cmd, st.side, st.old_yaw );
						} else
							view_turn( );
					}
					s.cmds[ i ] = { cmd->m_view_point, cmd->m_forward_move, cmd->m_side_move, cmd->m_buttons, local->get_origin( ) };
					if ( s.found || local->get_flags( ) & 1 || local->get_velocity( ).m_z > 0.f )
						break;
					g_prediction.begin( local, cmd );
					g_prediction.end( local );
					sims++;
					s.found = lob_check( cmd );
					if ( s.brk ) {
						s.brk = false;
						break;
					}
					s.vel_backup = local->get_velocity( );
					if ( s.found ) {
						const int tick_count = g_interfaces.m_global_vars_base->m_tick_count;
						s.ticks              = i;
						s.last_type          = v;
						s.sec_last_type      = v;
						s.eb_tick            = tick_count + ( i + 1 );
						s.pred_ticks         = i;
						s.pred_stamp         = tick_count;
						s.mouse_offset       = std::abs( cmd->m_mouse_delta_x );
						found_v              = v;
						for ( int k = 0; k <= i; k++ )
							m_viz_path[ k ] = s.cmds[ k ].origin;
						m_viz_path[ i + 1 ] = local->get_origin( );
						m_viz_count         = i + 2;
						m_viz_pos           = local->get_origin( );
						m_viz_base          = tick_count;
						m_viz_alts          = 0;
						m_viz_sel           = -1;
					}
				}
				cmd->m_view_point   = view_backup;
				cmd->m_forward_move = forward_backup;
				cmd->m_side_move    = side_backup;
				cmd->m_buttons      = buttons_backup;
			}
		}
		if ( s.found ) {
			ReStorePrediction( );
			const float dist = local->get_origin( ).dist_to( s.cmds[ s.hits ].origin );
			if ( s.hits > s.ticks || dist > 1.f ) {
				s.hits          = 0;
				s.found         = false;
				s.sec_last_type = 0;
				return;
			}
			if ( s.cmds[ s.hits ].buttons & in_duck ) {
				cmd->m_buttons |= in_duck;
				s.ducked = true;
			} else
				cmd->m_buttons &= ~in_duck;
			if ( m_backup_flags & 1 )
				cmd->m_buttons |= in_jump;
			else
				cmd->m_buttons &= ~in_jump;
			cmd->m_forward_move = s.cmds[ s.hits ].forwardmove;
			cmd->m_side_move    = s.cmds[ s.hits ].sidemove;
			cmd->m_view_point   = s.cmds[ s.hits ].viewangle;
			if ( silent ) {
				start_movement_fix( cmd );
				cmd->m_view_point = view_backup;
				end_movement_fix( cmd );
			}
			s.aim = cmd->m_view_point;
			s.hits += 1;
		}
	}( );

	if ( !was && found_v >= 0 )
		botox_dbg_log( "EB: style=4 find v=%d t=%d duck=%d sims=%d lim=%d search=%d vz=%.2f\n", found_v, s.ticks,
		               ( s.cmds[ s.ticks ].buttons & in_duck ) ? 1 : 0, sims, GET_VARIABLE( g_variables.m_edgebug_lob_ticks, int ),
		               GET_VARIABLE( g_variables.m_edgebug_lob_mega, bool ) ? 6 : GET_VARIABLE( g_variables.m_edgebug_lob_advanced, bool ) ? 4 : 2,
		               m_backup_velocity.m_z );
	m_found = s.found;
	if ( m_found ) {
		m_ducked           = false;
		m_found_tick       = s.eb_tick;
		m_prediction_ticks = s.ticks + 1;
		m_replay_hits      = s.hits;
	} else
		m_replay_hits = 0;
}

void n_edgebug::impl_t::lob_frame_start( )
{
	if ( !g_ctx.m_local )
		return;
	auto& s = m_lob;
	if ( !s.found ) {
		s.aim_to = s.view_backup;
		return;
	}
	if ( GET_VARIABLE( g_variables.m_edgebug_lob_silent, bool ) || s.sec_last_type <= 1 || GET_VARIABLE( g_variables.m_edgebug_lob_autostrafe, bool ) )
		return;
	constexpr float smoothing = 0.1f, max_delta = 10.f;
	/* donor while-loop wrap; non-finite would spin forever */
	const auto wrap = []( float d ) {
		if ( !std::isfinite( d ) )
			return 0.f;
		while ( d > 180.f )
			d -= 360.f;
		while ( d < -180.f )
			d += 360.f;
		return d;
	};
	const auto step = [ & ]( const float aim, float& to ) {
		float d = wrap( aim - to );
		if ( fabs( d ) > max_delta )
			d = d > 0 ? max_delta : -max_delta;
		to += d * smoothing;
	};
	step( s.aim.m_x, s.aim_to.m_x );
	step( s.aim.m_y, s.aim_to.m_y );
	step( s.aim.m_z, s.aim_to.m_z );
	s.aim_to.normalize( );
	g_interfaces.m_engine_client->set_view_angles( s.aim_to );
}

void n_edgebug::impl_t::lob_mouse( float* x )
{
	const auto local = g_ctx.m_local;
	if ( !local || !local->is_alive( ) || !g_interfaces.m_engine_client->is_in_game( ) || !g_interfaces.m_engine_client->is_connected( ) )
		return;
	/* donor compares the whole flags word, so this almost never returns */
	if ( const auto fg = local->get_flags( ); fg == fl_inwater || fg == fl_waterjump )
		return;
	if ( const auto mt = local->get_move_type( ); mt == move_type_ladder || mt == move_type_noclip )
		return;
	if ( !m_lob.found || *x == 0.0 )
		return;
	const float v3 = static_cast< float >( m_lob.pred_stamp + m_lob.pred_ticks - g_interfaces.m_global_vars_base->m_tick_count ) / *x;
	const float v4 = static_cast< float >( 100.0 / ( atan2f( static_cast< float >( m_lob.pred_ticks ), v3 ) + 100.0 +
	                           static_cast< float >( GET_VARIABLE( g_variables.m_edgebug_lob_lock, float ) * m_lob.mouse_offset ) ) );
	if ( !std::isnan( v4 ) )
		*x = *x * v4;
}
