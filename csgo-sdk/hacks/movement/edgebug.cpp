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
		return gravity * 0.5f * n_tick::interval( );
	}

	/* vz0 < -( g * mul + off * s + fall ) && |vz + g| < tol * s. off/tol per-tick ( scaled ), fall never scaled. */
	struct eb_grav_band_t {
		float mul, off, fall, tol;
	};
	struct eb_abs_band_t {
		float fall, tol;
	};
	struct eb_chg_band_t {
		float change, fall, tol;
	};
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
	constexpr eb_abs_band_t k_enhanced_rise[] = { { 30.f, 18.f }, { 45.f, 28.f } };

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

	float eb_aim_err( const c_vector& start, const c_vector& end, const float aim_yaw )
	{
		const float dx = end.m_x - start.m_x, dy = end.m_y - start.m_y;
		if ( dx * dx + dy * dy < 4.f )
			return 90.f;
		return fabsf( std::remainderf( rad2deg( atan2f( dy, dx ) ) - aim_yaw, 360.f ) );
	}

	// engine hull by stance ( cs_gamerules g_CSViewVectors ): lean sims never refresh the collideable's bounds
	bool eb_on_slope( const c_vector& origin, const bool ducked )
	{
		c_trace_filter filter( g_ctx.m_local );
		ray_t ray( origin, c_vector( origin.m_x, origin.m_y, origin.m_z - 2.f ), c_vector( -16.f, -16.f, 0.f ),
		           c_vector( 16.f, 16.f, ducked ? 54.f : 72.f ) );
		trace_t tr;
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		return tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z > 0.f && tr.m_plane.m_normal.m_z < 0.7f;
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
void n_edgebug::impl_t::CommitStrafe( c_user_cmd* cmd, c_angle wish )
{
	const float cap = std::clamp( GET_VARIABLE( g_variables.m_edgebug_autostrafe_max_angle, float ), 0.f, 180.f );
	if ( cap < 180.f && m_course_valid ) {
		const c_vector vel = g_ctx.m_local->get_velocity( );
		if ( vel.length_2d( ) > 1.f ) {
			const float vel_yaw = rad2deg( atan2f( vel.m_y, vel.m_x ) );
			const float dev      = std::remainderf( vel_yaw - m_course_yaw, 360.f );
			const float move_off = rad2deg( atan2f( -cmd->m_side_move, cmd->m_forward_move ) );
			const float rel      = std::remainderf( wish.m_y + move_off - vel_yaw, 360.f );
			if ( fabsf( dev ) >= cap && ( ( dev > 0.f ) == ( rel > 0.f ) ) )
				wish.m_y = vel_yaw - rel - move_off;
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
		// floor ahead > 6u under the feet = past a lip. was fraction > 0.8 ( ~51u drop ): curbs and fence rails never counted
		if ( trace.m_fraction >= 1.f || origin.m_z - trace.m_end.m_z > 6.f )
			return true;
	}
	return false;
}
bool n_edgebug::impl_t::EdgeBugCheck( const edge_context_t& raw_ctx )
{
	if ( !raw_ctx.was_falling || raw_ctx.on_ground )
		return false;
	// per-tick quantity (tol around -g, per-tick delta, offset on g) scales by literal * s; fall speed / ratio
	// stay as-is (never scale fall speeds: fires at half the real speed on 128). s == 1 at 64
	const float s   = n_tick::scale( );
	const float g   = raw_ctx.gravity_tick;
	const float vz0 = raw_ctx.backup_vel_z;
	const float vz  = raw_ctx.current_vel_z;
	if ( raw_ctx.mode == 0 ) {
		if ( eb_any( k_normal_grav, vz0, vz, g, s ) || eb_any( k_normal_abs, vz0, vz, s ) )
			return true;
		return vz0 < -g * 0.7f && roundf( vz ) == -roundf( g );
	}
	const float vel_change = fabsf( vz - vz0 );
	const float vel_ratio  = vz0 != 0.f ? fabsf( vz / vz0 ) : 0.f;
	if ( eb_any( k_enhanced_grav, vz0, vz, g, s ) ||
	     eb_any( k_enhanced_abs, vz0, vz, s ) ||
	     eb_any( k_enhanced_chg, vz0, vz, vel_change, s ) ||
	     eb_any( k_enhanced_ratio, vz0, vel_ratio ) )
		return true;
	if ( ( eb_any( k_edge_grav, vz0, vz, g, s ) || eb_any( k_edge_abs, vz0, vz, s ) ) && raw_ctx.trace_edge && IsNearEdge( ) )
		return true;
	for ( const auto& b : k_enhanced_rise )
		if ( vz0 < -b.fall && vz > vz0 + b.tol * s )
			return true;
	if ( vz0 < -g * 0.9f && roundf( vz ) == -roundf( g ) )
		return true;
	if ( vz0 < -12.f && fabsf( vz - vz0 ) < 1.8f * s )
		return true;
	return vz0 < -15.f && vz > -2.f * s && vz < 2.f * s;
}
bool n_edgebug::impl_t::IsEdgeBugTick( float backup_vel_z, float current_vel_z, bool on_ground, bool trace_edge )
{
	edge_context_t ctx;
	ctx.trace_edge    = trace_edge;
	ctx.backup_vel_z  = backup_vel_z;
	ctx.current_vel_z = current_vel_z;
	ctx.gravity_tick  = get_edgebug_half_gravity_per_tick( );
	ctx.was_falling   = backup_vel_z < -6.f * n_tick::scale( );
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
		CommitStrafe( cmd, cmd->m_view_point );
	} else {
		cmd->m_side_move = cl_sidespeed_val;
		CommitStrafe( cmd, cmd->m_view_point );
	}
}
bool n_edgebug::impl_t::FindEdgeTarget( const int reach_tick, const int window_ticks, const float steer )
{
	m_edge_target_valid = false;
	const auto local = g_ctx.m_local;
	if ( !local )
		return false;
	const auto col = local->get_collideable( );
	if ( !col )
		return false;
	const c_vector vel = local->get_velocity( );
	const float speed  = vel.length_2d( );
	if ( speed < 30.f )
		return false;
	static auto sv_gravity = g_interfaces.m_convar->find_var( "sv_gravity" );
	const float grav      = sv_gravity ? std::max( sv_gravity->get_float( ), 1.f ) : 800.f;
	const float ipt       = n_tick::interval( );
	const c_vector origin = local->get_abs_origin( );
	const int probe_tick = std::clamp( reach_tick >= 0 ? reach_tick + 1 : window_ticks / 3, 1, std::max( 1, window_ticks ) );
	const float t = static_cast< float >( probe_tick ) * ipt;
	c_vector p = origin + vel * t;
	p.m_z      = origin.m_z + vel.m_z * t - 0.5f * grav * t * t;
	const c_vector right( vel.m_y / speed, -vel.m_x / speed, 0.f );
	constexpr int k_lanes  = 9;
	constexpr float k_lane = 16.f;
	constexpr float k_lip = 2.5f;
	const c_vector fmins( col->get_obb_mins( ).m_x, col->get_obb_mins( ).m_y, 0.f );
	const c_vector fmaxs( col->get_obb_maxs( ).m_x, col->get_obb_maxs( ).m_y, 0.f );
	// from the highest feet this cmd can reach ( an air duck press lifts them 9u ) down. solid up there = a
	// wall at this height ( blocked ), never a floor to land on
	const float top = origin.m_z + ( ( local->get_flags( ) & fl_ducking ) ? 0.f : 9.f ) + 1.f;
	const float bot = std::min( p.m_z, origin.m_z ) - 128.f;
	struct lane_t {
		bool solid, blocked;
		float z;
	};
	c_trace_filter filter( local );
	const auto lane_at = [ & ]( const float off ) -> lane_t {
		const c_vector c = p + right * off;
		ray_t ray( c_vector( c.m_x, c.m_y, top ), c_vector( c.m_x, c.m_y, bot ), fmins, fmaxs );
		trace_t tr;
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		if ( tr.m_start_solid || tr.m_all_solid )
			return { true, true, top };
		return { tr.did_hit( ), false, tr.did_hit( ) ? tr.m_end.m_z : -99999.f };
	};
	const auto lip_tick = [ & ]( const float lip_z ) {
		const float drop = origin.m_z - lip_z;
		if ( drop <= 0.f )
			return 1;
		const float tt = ( vel.m_z + sqrtf( vel.m_z * vel.m_z + 2.f * grav * drop ) ) / grav;
		return std::max( 1, static_cast< int >( ceilf( tt / ipt - 0.001f ) ) );
	};
	const auto max_lat = [ & ]( const int tick ) {
		const float n = static_cast< float >( tick + 1 );
		return 30.f * ipt * n * ( n + 1.f ) * 0.5f + 16.f;
	};
	const float cap = std::clamp( GET_VARIABLE( g_variables.m_edgebug_autostrafe_max_angle, float ), 0.f, 180.f );
	const auto in_cone = [ & ]( const float off ) {
		if ( cap >= 180.f || !m_course_valid )
			return true;
		const c_vector q = p + right * off;
		return fabsf( std::remainderf( rad2deg( atan2f( q.m_y - origin.m_y, q.m_x - origin.m_x ) ) - m_course_yaw, 360.f ) ) <= cap;
	};
	lane_t lanes[ k_lanes ];
	for ( int i = 0; i < k_lanes; i++ )
		lanes[ i ] = lane_at( static_cast< float >( i - k_lanes / 2 ) * k_lane );
	const auto aim = [ ]( const float touch, const lane_t& lo, const lane_t& hi ) {
		const bool lo_solid = lo.solid != hi.solid ? lo.solid : lo.z > hi.z;
		return touch + ( lo_solid ? -8.f : 8.f );
	};
	const auto lip_z = [ ]( const lane_t& lo, const lane_t& hi ) {
		return lo.solid != hi.solid ? ( lo.solid ? lo.z : hi.z ) : std::max( lo.z, hi.z );
	};
	struct cand_t {
		int lane;
		float dist;
	};
	cand_t cands[ k_lanes - 1 ];
	int n_cands = 0;
	for ( int i = 0; i < k_lanes - 1; i++ ) {
		const lane_t& a = lanes[ i ];
		const lane_t& b = lanes[ i + 1 ];
		if ( a.blocked || b.blocked )
			continue;
		if ( a.solid == b.solid && ( !a.solid || fabsf( a.z - b.z ) < k_lip ) )
			continue;
		const float off = aim( static_cast< float >( i - k_lanes / 2 ) * k_lane + k_lane * 0.5f, a, b );
		const int tick  = lip_tick( lip_z( a, b ) );
		if ( tick > window_ticks || fabsf( off ) > max_lat( tick ) + k_lane * 0.5f || !in_cone( off ) )
			continue;
		const float bias = fabsf( steer ) >= 0.1f ? ( steer > 0.f ? 0.75f : -0.75f ) : 0.f;
		cands[ n_cands++ ] = { i, fabsf( off ) + bias * off };
	}
	std::sort( cands, cands + n_cands, [ ]( const cand_t& x, const cand_t& y ) { return x.dist < y.dist; } );
	for ( int c = 0; c < std::min( n_cands, 3 ); c++ ) {
		const int i = cands[ c ].lane;
		lane_t lo_l = lanes[ i ], hi_l = lanes[ i + 1 ];
		const bool split = lo_l.solid != hi_l.solid;
		float lo         = static_cast< float >( i - k_lanes / 2 ) * k_lane;
		float hi         = lo + k_lane;
		for ( int it = 0; it < 4; it++ ) {
			const float mid    = ( lo + hi ) * 0.5f;
			const lane_t m     = lane_at( mid );
			const bool lo_side = split ? m.solid == lo_l.solid
			                           : m.solid ? fabsf( m.z - lo_l.z ) < fabsf( m.z - hi_l.z ) : lo_l.z < hi_l.z;
			if ( lo_side ) {
				lo   = mid;
				lo_l = m;
			} else {
				hi   = mid;
				hi_l = m;
			}
		}
		if ( lo_l.blocked || hi_l.blocked || ( lo_l.solid == hi_l.solid && fabsf( lo_l.z - hi_l.z ) < k_lip ) )
			continue;
		const float off = aim( ( lo + hi ) * 0.5f, lo_l, hi_l );
		const int tick  = lip_tick( lip_z( lo_l, hi_l ) );
		if ( fabsf( off ) > 90.f || tick > window_ticks || fabsf( off ) > max_lat( tick ) || !in_cone( off ) )
			continue;
		m_edge_target       = p + right * off;
		m_edge_target_valid = true;
		m_edge_target_tick  = tick;
		return true;
	}
	return false;
}
void n_edgebug::impl_t::ApplyAutoStrafeToEdge( c_user_cmd* cmd, bool duck, const int tick )
{
	if ( duck )
		cmd->m_buttons |= in_duck;
	else
		cmd->m_buttons &= ~in_duck;
	cmd->m_forward_move = 0.f;
	cmd->m_side_move    = 0.f;
	const c_vector velocity = g_ctx.m_local->get_velocity( );
	if ( velocity.length_2d( ) < 1.f )
		return;
	const float course = deg2rad( m_course_valid ? m_course_yaw : rad2deg( atan2f( velocity.m_y, velocity.m_x ) ) );
	const c_vector fwd( cosf( course ), sinf( course ), 0.f );
	const c_vector right( sinf( course ), -cosf( course ), 0.f );
	const c_vector origin = g_ctx.m_local->get_origin( );
	const float err       = ( m_edge_target.m_x - origin.m_x ) * right.m_x + ( m_edge_target.m_y - origin.m_y ) * right.m_y;
	const float v_lat     = velocity.m_x * right.m_x + velocity.m_y * right.m_y;
	const float v_along   = velocity.m_x * fwd.m_x + velocity.m_y * fwd.m_y;
	const float ipt       = n_tick::interval( );
	static auto sv_airaccelerate = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
	const float aa = sv_airaccelerate ? std::max( sv_airaccelerate->get_float( ), 0.1f ) : 12.f;
	const int left = m_edge_target_tick - tick + 1;
	float want     = 0.f;
	if ( left >= 1 )
		want = err / ( static_cast< float >( left ) * ipt );
	else {
		const float brake = 0.5f * std::min( aa * 250.f * ipt, 30.f ) / ipt;
		want              = std::copysign( std::min( fabsf( err ) / ipt, sqrtf( 2.f * brake * fabsf( err ) ) ), err );
	}
	/* never ask for a bend past the cone: CommitStrafe's mirror would flip the push back = jitter again */
	if ( const float cap = std::clamp( GET_VARIABLE( g_variables.m_edgebug_autostrafe_max_angle, float ), 0.f, 180.f ); cap < 90.f ) {
		const float lim = fabsf( v_along ) * tanf( deg2rad( cap ) );
		want            = std::clamp( want, -lim, lim );
	}
	const float dv = want - v_lat;
	if ( fabsf( dv ) < 1.f )
		return;
	const float speed = velocity.length_2d( );
	float px = velocity.m_y / speed, py = -velocity.m_x / speed;
	float share = px * right.m_x + py * right.m_y;
	if ( ( share > 0.f ) != ( dv > 0.f ) ) {
		px    = -px;
		py    = -py;
		share = -share;
	}
	c_angle wish        = cmd->m_view_point;
	wish.m_x            = 0.f;
	wish.m_z            = 0.f;
	wish.m_y            = rad2deg( atan2f( py, px ) );
	cmd->m_forward_move = std::clamp( fabsf( dv ) / ( std::max( fabsf( share ), 0.2f ) * aa * ipt ), 0.f, 450.f );
	CommitStrafe( cmd, wish );
}
void n_edgebug::impl_t::ReStorePrediction( )
{
	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
}
void n_edgebug::impl_t::EdgeBugPostPredict( c_user_cmd* cmd )
{
	c_angle delta_angle = cmd->m_view_point - m_prev_view;
	delta_angle.m_y     = std::remainderf( delta_angle.m_y, 360.f );
	m_prev_view         = cmd->m_view_point;
	static auto m_yaw_var  = g_interfaces.m_convar->find_var( "m_yaw" );
	const float mouse_yaw  = -( m_yaw_var ? m_yaw_var->get_float( ) : 0.022f ) * m_mouse_x;
	m_mouse_x              = 0.f;
	const bool steer       = GET_VARIABLE( g_variables.m_edgebug_mouse_steer, bool );
	if ( steer && mouse_yaw != 0.f )
		delta_angle.m_y = std::clamp( mouse_yaw, -30.f, 30.f );
	const int want_paths = steer ? std::clamp( GET_VARIABLE( g_variables.m_edgebug_paths, int ), 1, k_plan_max ) : 1;
	const auto drop_plan = [ this ]( const char* why ) {
		if ( m_found )
			botox_dbg_log( "EB: drop why=%s at %d/%d\n", why, m_replay_hits, m_prediction_ticks );
		m_found            = false;
		m_replay_hits      = 0;
		m_ducked           = false;
		m_steer_since_plan = 0.f;
	};
	if ( const int style = GET_VARIABLE( g_variables.m_edgebug_style, int ); style != 0 ) {
		donor_post( cmd, style );
		return;
	}
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
		m_last_search_tick = -1;
		m_last_contact_tick = -1;
		return;
	}
	const float vz_scale = n_tick::scale( );
	if ( !m_found && m_ducked && m_backup_velocity.m_z < -40.f )
		m_ducked = false;
	const int detection_mode = GET_VARIABLE( g_variables.m_edgebug_detection_mode, int );
	auto is_large_delta = []( float y1, float y2 ) {
		float d = fabsf( y1 - y2 );
		if ( d > 180.f ) d = 360.f - d;
		return d >= 179.f;
	};
	const bool autostrafe  = GET_VARIABLE( g_variables.m_edgebug_autostrafe_to_edge, bool );
	const int search_count = 8;
	const auto variant_on  = [ & ]( const int v ) { return autostrafe || v < 2 || v == 4 || v == 5; };
	const auto& eb_types = GET_VARIABLE( g_variables.m_edgebug_types, std::vector< bool > );
	bool allow_duck      = eb_types.size( ) > 0 && eb_types[ 0 ];
	bool allow_stand     = eb_types.size( ) > 1 && eb_types[ 1 ];
	if ( !allow_duck && !allow_stand )
		allow_duck = allow_stand = true;
	const int depth = detection_mode == 1 ? std::clamp( GET_VARIABLE( g_variables.m_edgebug_search_amount, int ), 1, 4 ) : 2;
	const float predict_time = std::clamp( GET_VARIABLE( g_variables.m_edgebug_predict_time, float ), 0.02f, 2.f );
	int max_ticks = static_cast< int >( std::round( predict_time / n_tick::interval( ) ) );
	max_ticks = std::clamp( max_ticks, 1, n_edgebug::k_predicted_cmd_max );
	int orig_buttons = cmd->m_buttons;
	float orig_fwd = cmd->m_forward_move;
	float orig_side = cmd->m_side_move;
	c_angle orig_viewangle = cmd->m_view_point;
	const float user_fwd = m_user_forward_move;
	const float user_side = m_user_side_move;
	const int user_buttons = m_user_buttons;
	if ( m_found ) {
		ReStorePrediction( );
		if ( m_replay_hits > m_prediction_ticks ) {
			botox_dbg_log( "EB: drop why=spent at %d/%d\n", m_replay_hits, m_prediction_ticks );
			m_replay_hits = 0;
			m_found       = false;
		} else if ( const float dist = g_ctx.m_local->get_origin( ).dist_to( m_predicted_cmds[m_replay_hits].origin ); dist > 1.f ) {
			botox_dbg_log( "EB: replay drop at %d/%d dist=%.3f\n", m_replay_hits, m_prediction_ticks, dist );
			m_replay_hits = 0;
			m_found       = false;
		}
	}
	/* mouse steer re-plan: turned >= 1 deg since the plan latched, ticks(2)+ before its bug tick ( never swap a bug that
	   close ). the search runs off the live frame; no find keeps the old plan, it is in m_predicted_cmds, runs write m_run_cmds */
	if ( !m_found )
		m_steer_since_plan = 0.f;
	else if ( steer )
		m_steer_since_plan += delta_angle.m_y;
	const bool replan = m_found && steer && fabsf( m_steer_since_plan ) >= 1.f && m_prediction_ticks - m_replay_hits >= n_tick::ticks( 2 );
	int sel = -1;
	if ( !m_found || replan ) do {
		static int eb_n_vz = 0, eb_n_gap = 0, eb_n_air = 0, eb_n_run = 0, eb_n_hit = 0;
		if ( static unsigned long long eb_next_report = 0ull; GetTickCount64( ) >= eb_next_report ) {
			eb_next_report = GetTickCount64( ) + 1000ull;
			if ( eb_n_vz || eb_n_gap || eb_n_air || eb_n_run )
				botox_dbg_log( "EB: gates vz=%d gap=%d air=%d run=%d hit=%d", eb_n_vz, eb_n_gap, eb_n_air, eb_n_run, eb_n_hit );
			eb_n_vz = eb_n_gap = eb_n_air = eb_n_run = eb_n_hit = 0;
		}
		if ( m_backup_velocity.m_z >= ( detection_mode == 1 ? -4.f : -10.f ) ) {
			eb_n_vz++;
			break;
		}
		const int now_tick = g_interfaces.m_global_vars_base->m_tick_count;
		// every tick within 12 of contact: 10-10 log short falls got 5 searches at 2/3 gaps
		const bool every_tick = ( m_last_contact_tick >= 0 && m_last_contact_tick <= n_tick::ticks( 12 ) );
		if ( m_last_search_tick >= 0 && now_tick - m_last_search_tick < n_tick::ticks( 2 ) && !every_tick ) {
			eb_n_gap++;
			break;
		}
		m_last_search_tick = now_tick;
		const eb_hull_t hull = eb_make_hull( );
		const int reach_tick = eb_first_reach_tick( hull, g_ctx.m_local->get_abs_origin( ), g_ctx.m_local->get_velocity( ), max_ticks );
		if ( reach_tick < 0 ) {
			m_last_contact_tick = -1;
			eb_n_air++;
			break;
		}
		const eb_hull_t still_hull = eb_make_hull( 1.f );
		const int still_segs       = std::clamp( static_cast< int >( std::ceil( ( max_ticks + 1 ) * n_tick::interval( ) / 0.1f ) ), 3, 16 );
		int still_reach            = 0;
		{
			const float ipt = n_tick::interval( );
			c_vector v      = g_ctx.m_local->get_velocity( );
			c_vector o      = g_ctx.m_local->get_origin( ) - v * ipt;
			o.m_z -= 0.5f * still_hull.grav * ipt * ipt;
			v.m_z += still_hull.grav * ipt;
			still_reach = eb_first_reach_tick( still_hull, o, v, max_ticks + 1, still_segs );
		}
		if ( const c_vector live_vel = g_ctx.m_local->get_velocity( ); live_vel.length_2d( ) > 30.f ) {
			m_course_yaw   = rad2deg( atan2f( live_vel.m_y, live_vel.m_x ) );
			m_course_valid = true;
		} else {
			m_course_valid = false;
		}
		if ( autostrafe )
			FindEdgeTarget( reach_tick, max_ticks, steer ? delta_angle.m_y : 0.f );
		else
			m_edge_target_valid = false;
		eb_n_run++;
		m_steer_since_plan = 0.f;
		m_plan_count       = 0;
		const auto enough  = [ & ]( ) { return m_plan_count >= want_paths; };
		const int run_ticks = std::min( max_ticks, std::max( reach_tick + 1, still_reach ) + n_tick::ticks( 10 ) );
		int total_predictions = 0;
		const int gap_owned = every_tick ? 1 : std::clamp( n_tick::ticks( 1 ), 1, 2 );
		const bool near_geometry = every_tick || reach_tick <= n_tick::ticks( 12 );
		const float want_s = ( detection_mode == 1 ? 0.35f : 0.24f ) * ( 1.f / 64.f ) *
		                     static_cast< float >( gap_owned ) * ( near_geometry ? 1.f : 0.5f ) * ( static_cast< float >( depth ) * 0.5f );
		const float budget_share = std::min( detection_mode == 1 ? 0.60f : 0.48f, want_s / n_tick::interval( ) );
		const int MAX_PREDICTIONS = std::clamp(
		    std::max( static_cast< int >( n_tick::interval( ) * 1000000.f * budget_share / 8.f ),
		              run_ticks * ( detection_mode == 1 ? 3 * depth : 4 ) ), 16, 2048 );
		n_tick::c_sim_budget budget;
		budget.start( budget_share, 0.f, n_tick::search_eb );
		/* measured cost of a full pass, for "don't start what you can't finish". */
		long long worst_run_us = 0ll;
		const int duck_retry    = std::clamp( n_tick::ticks( 1 + depth ), 2, 12 );
		/* last pass's first ground tick, -1 = never. */
		int contact_tick = -1;
		const float grav_tick = get_edgebug_half_gravity_per_tick( );
		int rej_stance = 0, rej_slope = 0;

		int rej_rise     = 0;
		int confirm_grounded = 0, clip_lands = 0;
		float found_turn = 1.f;
		int found_src = 0;
		bool start_ducked = false;
		for ( auto& r : m_base_ref ) {
			r.last     = -1;
			r.complete = false;
		}
		constexpr float k_land_air = 1e9f, k_land_bad = -1e9f;
		float yaw_off = 0.f, last_land = k_land_bad;
		bool in_bracket = false;
		long long rst_us = 0ll;
		int sweep_first[ 8 ]{ }, sweep_last[ 8 ]{ };
		const int snap_slots = std::min( g_prediction.snapshot_slots( ), static_cast< int >( std::size( m_snap_aux ) ) );
		int snap_used = 0, snap_runs = 0, snap_ticks = 0;
		bool snap_off = false, last_truncated = false, last_found = false;
		std::memset( m_snap_at, -1, sizeof( m_snap_at ) );
		for ( int& n : m_snap_cmds_n )
			n = 0;
		int runs_started = 0;
		auto run_variant = [ & ]( const int v, const int duck_at, const int undo_at = -1, const float turn = 1.f ) {
			runs_started++;
			const long long run_started_us = budget.used_us( );
			ReStorePrediction( );
			rst_us += budget.used_us( ) - run_started_us;
			/* curtime must move per sim tick ( the 0.4 s re-duck gate, cs_gamemovement DuckingEnabled ):
			   begin( ) never steps the tickbase, restore puts it back */
			const int tick_base0 = g_ctx.m_local->get_tick_base( );
			start_ducked         = ( g_ctx.m_local->get_flags( ) & fl_ducking ) != 0;
			base_ref_t* ref = ( duck_at < 0 && undo_at < 0 && turn == 1.f && yaw_off == 0.f ) ? &m_base_ref[ v ] : nullptr;
			float land      = k_land_air;
			if ( ref ) {
				ref->last     = -1;
				ref->complete = false;
			}
			bool truncated = false;
			m_strafe_side = 1.f;
			m_strafe_last_yaw = orig_viewangle.m_y + yaw_off;
			c_vector search_vel_backup = g_ctx.m_local->get_velocity( );
			cmd->m_view_point = orig_viewangle;
			cmd->m_forward_move = user_fwd;
			cmd->m_side_move = user_side;
			cmd->m_buttons = user_buttons;
			c_angle current_angle = cmd->m_view_point;
			contact_tick = -1;
			const int snap_fam  = ( snap_off || ( v & 1 ) ) ? -1
			                      : ( duck_at > 0 && undo_at < 0 ) ? v
			                      : ( duck_at == 0 && undo_at > 0 ) ? v + 1 : -1;
			const int switch_at = undo_at > 0 ? undo_at : duck_at;
			int resume = 0;
			if ( snap_fam >= 0 && m_snap_at[ snap_fam ][ switch_at ] >= 0 ) {
				const int s = m_snap_at[ snap_fam ][ switch_at ];
				g_prediction.snapshot_load( s );
				const snap_aux_t& aux = m_snap_aux[ s ];
				cmd->m_view_point     = aux.view;
				cmd->m_forward_move   = aux.fwd;
				cmd->m_side_move      = aux.side;
				cmd->m_buttons        = aux.buttons;
				current_angle         = aux.current_angle;
				m_strafe_side         = aux.strafe_side;
				m_strafe_last_yaw     = aux.strafe_last_yaw;
				std::memcpy( m_run_cmds, m_snap_cmds[ snap_fam ], sizeof( predicted_cmd_t ) * switch_at );
				search_vel_backup = g_ctx.m_local->get_velocity( );
				resume            = switch_at;
				snap_runs++;
				snap_ticks += switch_at;
			}
			auto advance_view = [ & ]( ) {
				if ( !is_large_delta( current_angle.m_y, orig_viewangle.m_y ) )
					current_angle = ( current_angle + delta_angle * turn ).normalize( ).clamp( );
			};
			auto replay_user_move = [ & ]( const bool duck ) {
				if ( duck )
					cmd->m_buttons |= in_duck;
				else
					cmd->m_buttons &= ~in_duck;
				cmd->m_forward_move = user_fwd;
				cmd->m_side_move = user_side;
				advance_view( );
				cmd->m_view_point = current_angle;
				cmd->m_view_point.m_y += yaw_off;
				start_movement_fix( cmd );
				cmd->m_view_point = orig_viewangle;
				end_movement_fix( cmd );
			};
			int pending = -1;
			c_vector pending_pos{ };
			bool pending_clip = false;
			bool run_found = false;
			auto latch = [ & ]( const int tick_index, const c_vector& end_pos ) {
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
				pl.turn    = turn;
				pl.src     = in_bracket ? 7 : undo_at >= 0 ? 4 : duck_at >= 0 ? 3 : turn < 1.f ? 2 : 1;
				pl.end_pos = end_pos;
			};
			for ( int i = resume; i < run_ticks || ( pending >= 0 && i < max_ticks ); i++ ) {
				if ( const bool grounded = ( g_ctx.m_local->get_flags( ) & 1 ) != 0;
				     grounded || g_ctx.m_local->get_velocity( ).m_z > 0.f ) {
					if ( grounded )
						contact_tick = i;
					land    = grounded ? g_ctx.m_local->get_origin( ).m_z : k_land_bad;
					pending = -1;
					break;
				}
				if ( snap_fam >= 0 && pending < 0 && i >= 1 && i >= sweep_first[ v ] && i < switch_at && m_snap_at[ snap_fam ][ i ] < 0 &&
				     snap_used < snap_slots && g_prediction.snapshot_save( snap_used ) ) {
					m_snap_aux[ snap_used ] = { cmd->m_view_point, current_angle,  cmd->m_forward_move, cmd->m_side_move,
					                            m_strafe_side,      m_strafe_last_yaw, cmd->m_buttons };
					for ( int& n = m_snap_cmds_n[ snap_fam ]; n < i; n++ )
						m_snap_cmds[ snap_fam ][ n ] = m_run_cmds[ n ];
					m_snap_at[ snap_fam ][ i ] = static_cast< short >( snap_used++ );
				}
				/* odd variant = duck held. silent: a steered angle is folded into fwd/side, real view never moves */
				switch ( v ) {
				case 0:
				case 1:
					if ( v == 1 )
						cmd->m_buttons |= in_duck;
					else
						cmd->m_buttons &= ~in_duck;
					cmd->m_forward_move = 0.f;
					cmd->m_side_move = 0.f;
					break;
				case 2:
				case 3:
					if ( steer ) {
						advance_view( );
						cmd->m_view_point = current_angle;
					}
					if ( yaw_off != 0.f )
						cmd->m_view_point.m_y = ( steer ? current_angle.m_y : orig_viewangle.m_y ) + yaw_off;
					ApplyAutoStrafe( cmd, v == 3 );
					break;
				case 4:
				case 5:
					replay_user_move( v == 5 );
					break;
				case 6:
				case 7:
					ApplyAutoStrafeToEdge( cmd, v == 7, i );
					break;
				}
				if ( duck_at >= 0 ) {
					if ( i < duck_at )
						cmd->m_buttons &= ~in_duck;
					else
						cmd->m_buttons |= in_duck;
				}
				if ( undo_at >= 0 && i >= undo_at )
					cmd->m_buttons &= ~in_duck;
				// confirm tick is past the replay: real duck = plan's last bit (m_ducked hold) or the player's own,
				// never this variant's overlay (a press here would fake the "still airborne")
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
				const bool pre_ducked      = ( g_ctx.m_local->get_flags( ) & fl_ducking ) != 0;
				if ( total_predictions >= MAX_PREDICTIONS || budget.expired( ) ) {
					truncated = true;
					land      = k_land_bad;
					break;
				}
				g_ctx.m_local->get_tick_base( ) = tick_base0 + i;
				g_prediction.begin( g_ctx.m_local, cmd, false, true );
				g_prediction.end( g_ctx.m_local );
				total_predictions++;
				if ( ref ) {
					ref->last             = i;
					ref->ok[ i ]          = false;
					ref->org[ i ]         = g_ctx.m_local->get_origin( );
					ref->vel[ i ]         = g_ctx.m_local->get_velocity( );
					ref->flags[ i ]       = g_ctx.m_local->get_flags( );
					ref->duck_speed[ i ]  = g_ctx.m_local->get_duck_speed( );
					ref->duck_amount[ i ] = g_ctx.m_local->get_duck_amount( );
				}
				if ( const auto sim_move_type = g_ctx.m_local->get_move_type( );
				     sim_move_type == move_type_ladder || sim_move_type == move_type_noclip || sim_move_type == move_type_observer ) {
					land    = k_land_bad;
					pending = -1;
					break;
				}
				if ( pending >= 0 ) {
					const bool grounded = ( g_ctx.m_local->get_flags( ) & 1 ) != 0;
					if ( grounded ) {
						contact_tick = i + 1;
						land         = g_ctx.m_local->get_origin( ).m_z;
						if ( pending_clip ) {
							latch( pending, pending_pos );
							clip_lands++;
						} else
							confirm_grounded++;
					} else if ( g_ctx.m_local->get_velocity( ).m_z <= 0.f )
						latch( pending, pending_pos );
					pending = -1;
					break;
				}
				edge_context_t ctx;
				ctx.backup_vel_z = search_vel_backup.m_z;
				ctx.current_vel_z = g_ctx.m_local->get_velocity( ).m_z;
				ctx.gravity_tick = grav_tick;
				ctx.was_falling = search_vel_backup.m_z < -6.f * vz_scale;
				ctx.on_ground = ( g_ctx.m_local->get_flags( ) & 1 ) != 0;
				ctx.mode = detection_mode;
				const auto eb_candidate = [ & ]( ) {
					if ( !EdgeBugCheck( ctx ) )
						return false;
					const bool post_ducked = ( g_ctx.m_local->get_flags( ) & fl_ducking ) != 0;
					if ( !( post_ducked ? allow_duck : allow_stand ) ) {
						rej_stance++;
						return false;
					}
					if ( pre_ducked == post_ducked && g_ctx.m_local->get_origin( ).m_z > m_run_cmds[ i ].origin.m_z + 0.01f ) {
						rej_rise++;
						return false;
					}
					if ( eb_on_slope( g_ctx.m_local->get_origin( ), post_ducked ) && eb_on_slope( m_run_cmds[ i ].origin, pre_ducked ) ) {
						c_vector next = g_ctx.m_local->get_origin( ) + g_ctx.m_local->get_velocity( ) * n_tick::interval( );
						next.m_z += 1.f;
						if ( eb_on_slope( next, post_ducked ) ) {
							rej_slope++;
							return false;
						}
					}
					return true;
				};
				if ( eb_candidate( ) ) {
					if ( i + 1 >= max_ticks ) {
						latch( i, g_ctx.m_local->get_origin( ) );
						break;
					}
					pending     = i;
					pending_pos = g_ctx.m_local->get_origin( );
					pending_clip = ctx.backup_vel_z < -2.f * grav_tick && fabsf( ctx.current_vel_z + grav_tick ) < 0.01f;
				}
				search_vel_backup = g_ctx.m_local->get_velocity( );
				// no in-run tail probe: 10-10 log it cut 295 of 46856 sims while its hull traces ran every 2nd tick
				if ( ref && pending < 0 )
					ref->ok[ i ] = true;
			}
			if ( ref )
				ref->complete = !truncated;
			g_ctx.m_local->get_tick_base( ) = tick_base0;
			if ( !run_found && pending >= 0 )
				latch( pending, pending_pos );
			cmd->m_view_point = orig_viewangle;
			cmd->m_forward_move = orig_fwd;
			cmd->m_side_move = orig_side;
			cmd->m_buttons = orig_buttons;
			if ( const long long spent = budget.used_us( ) - run_started_us; spent > worst_run_us )
				worst_run_us = spent;
			last_truncated = truncated;
			last_found     = run_found;
			last_land      = run_found ? k_land_bad : land;
			return resume;
		};

		int best_contact = -1;
		const bool user_move_dead = fabsf( user_fwd ) < 0.1f && fabsf( user_side ) < 0.1f;
		// breadth first: every variant once, then the duck sweeps. never sweep inside the variant loop
		// (variant 0's sweep eats the budget and 1..7 never run)
		int sweep_center[ 8 ];
		for ( int& c : sweep_center )
			c = -1;
		bool sweep_approx[ 8 ]{ };
		bool base_ran[ 8 ]{ };
		struct br_pt_t {
			float p, land;
		};
		br_pt_t br_turn[ 8 ][ 6 ];
		int br_turn_n[ 8 ]{ };
		int base_runs = 0;
		int sweep_runs = 0;
		int release_runs = 0;
		const int own_first     = ( ( user_buttons & in_duck ) && allow_duck ) ? 1 : 0;
		const int first_variant = ( m_last_variant > 0 && m_last_variant < search_count && variant_on( m_last_variant ) ) ? m_last_variant : own_first;
		if ( !replan )
			m_last_variant = 0;
		auto variant_at = [ & ]( const int idx ) { return idx == 0 ? first_variant : ( idx == first_variant ? 0 : idx ); };
		auto out_of_budget = [ & ]( ) {
			// don't start a run that can't finish; never gates the first run. runs, not sims: a starved clock cuts at tick 0 = 0 sims, ~90 empty restores
			return runs_started > 0 && ( total_predictions >= MAX_PREDICTIONS || budget.cannot_fit( worst_run_us ) );
		};
		int still_skips = 0;
		for ( int idx = 0; idx < search_count && !enough( ); idx++ ) {
			const int v = variant_at( idx );
			if ( !variant_on( v ) || ( user_move_dead && ( v == 4 || v == 5 ) ) )
				continue;
			if ( v < 2 && still_reach < 0 ) {
				still_skips++;
				continue;
			}
			if ( !allow_duck && ( v & 1 ) )
				continue;
			if ( !m_edge_target_valid && ( v == 6 || v == 7 ) )
				continue;
			if ( out_of_budget( ) )
				break;
			run_variant( v, -1 );
			base_runs++;
			base_ran[ v ] = true;
			br_turn[ v ][ br_turn_n[ v ]++ ] = { 1.f, last_land };
			if ( contact_tick > best_contact )
				best_contact = contact_tick;
			sweep_center[ v ] = contact_tick > 0 ? contact_tick : std::max( reach_tick + 1, still_reach );
			sweep_approx[ v ] = contact_tick <= 0;
		}
		float sweep_turn[ 8 ] = { 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f };
		int fan_runs          = 0;
		int press_skips = 0, release_skips = 0;
		const auto near3 = []( const c_vector& a, const c_vector& b ) {
			return fabsf( a.m_x - b.m_x ) < 0.001f && fabsf( a.m_y - b.m_y ) < 0.001f && fabsf( a.m_z - b.m_z ) < 0.001f;
		};
		const auto twin_above = [ & ]( const int v, const int j ) {
			const base_ref_t& a = m_base_ref[ v ];
			const base_ref_t& b = m_base_ref[ v + 1 ];
			if ( j < 0 || j > a.last || j > b.last || !a.ok[ j ] || !b.ok[ j ] )
				return false;
			if ( ( a.flags[ j ] & ( fl_onground | fl_ducking ) ) != 0 || ( b.flags[ j ] & ( fl_onground | fl_ducking ) ) != fl_ducking )
				return false;
			if ( a.duck_amount[ j ] != 0.f || b.duck_amount[ j ] != 1.f )
				return false;
			return near3( b.org[ j ], a.org[ j ] + c_vector( 0.f, 0.f, 9.f ) ) && near3( b.vel[ j ], a.vel[ j ] );
		};
		const auto press_is_base = [ & ]( const int v, const int k ) {
			const base_ref_t& a = m_base_ref[ v ];
			if ( sweep_turn[ v ] != 1.f || !a.complete )
				return false;
			if ( k > a.last )
				return true;
			const base_ref_t& b = m_base_ref[ v + 1 ];
			// press accepted at k: the twin's press went through from the same unducked start (0.4 s gate reads the same
			// last duck time, curtime only grows) and the 2.0 spam cost leaves >= 1.5. twin >= 1.5 never strips again
			return k >= 1 && !start_ducked && b.complete && twin_above( v, k - 1 ) && a.duck_speed[ k - 1 ] - 2.f >= 1.5f &&
			       b.duck_speed[ k - 1 ] >= 1.5f;
		};
		const auto unduck_clear = [ & ]( const c_vector& org ) {
			c_trace_filter filter( g_ctx.m_local );
			ray_t ray( org, c_vector( org.m_x, org.m_y, org.m_z - 9.f ), c_vector( -16.f, -16.f, 0.f ), c_vector( 16.f, 16.f, 72.f ) );
			trace_t tr;
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid | contents_team1 | contents_team2, &filter, &tr );
			return !tr.m_start_solid && !tr.m_all_solid && tr.m_fraction >= 1.f;
		};
		const auto release_is_base = [ & ]( const int v, const int k ) {
			const base_ref_t& b = m_base_ref[ v + 1 ];
			if ( sweep_turn[ v ] != 1.f || !b.complete )
				return false;
			if ( k > b.last )
				return true;
			const base_ref_t& a = m_base_ref[ v ];
			return k >= 1 && !( user_buttons & in_duck ) && a.complete && twin_above( v, k - 1 ) && unduck_clear( b.org[ k - 1 ] );
		};
		// snapchk ( debug log on, 1/s ): a resumed run run again from tick 0 must end on the same bits
		static unsigned long long snap_check_next = 0ull;
		bool snap_check        = GET_VARIABLE( g_variables.m_debug_log, bool ) && GetTickCount64( ) >= snap_check_next;
		const auto snap_verify = [ & ]( const int resumed, const int v, const int duck_at, const int undo_at ) {
			if ( resumed <= 0 || last_found || last_truncated || !snap_check )
				return;
			snap_check      = false;
			snap_check_next = GetTickCount64( ) + 1000ull;
			const c_vector org = g_ctx.m_local->get_origin( ), vel = g_ctx.m_local->get_velocity( );
			const int flags = g_ctx.m_local->get_flags( ), contact = contact_tick;
			const int plans = m_plan_count;
			snap_off = true;
			run_variant( v, duck_at, undo_at, sweep_turn[ v ] );
			snap_off      = false;
			/* a rerun find is the mismatch talking, never a plan */
			m_plan_count  = plans;
			const float d = org.dist_to( g_ctx.m_local->get_origin( ) ) + vel.dist_to( g_ctx.m_local->get_velocity( ) );
			const bool ok = d == 0.f && flags == g_ctx.m_local->get_flags( ) && contact == contact_tick && !last_found;
			botox_dbg_log( "EB: snapchk %s v=%d duck=%d undo=%d from=%d d=%.6f flags=%d/%d con=%d/%d found=%d", last_truncated ? "cut" : ok ? "ok" : "MISMATCH", v,
			               duck_at, undo_at, resumed, d, flags, g_ctx.m_local->get_flags( ), contact, contact_tick, last_found ? 1 : 0 );
		};
		int max_off = -1;
		const auto run_sweeps = [ & ]( ) {
			if ( enough( ) || replan )
				return;
			for ( int v = 0; v < 8; v += 2 ) {
				sweep_last[ v ] = -1;
				if ( sweep_center[ v ] <= 0 )
					continue;
				sweep_last[ v ] = std::min( sweep_center[ v ] - 1 + ( sweep_approx[ v ] ? std::max( 2, n_tick::ticks( 2 ) ) : 0 ), run_ticks - 1 );
				sweep_first[ v ] = std::max( ( base_ran[ v + 1 ] && sweep_turn[ v ] == 1.f ) ? 1 : 0,
				                             sweep_center[ v ] - duck_retry - ( sweep_approx[ v ] ? std::max( 2, n_tick::ticks( 2 ) ) : 0 ) );
				if ( const int span = sweep_last[ v ] - sweep_first[ v ]; span > max_off )
					max_off = span;
			}
			const int off_end = max_off;
			for ( int off = 0; off <= off_end && !enough( ); off++ ) {
				for ( int idx = 0; idx < search_count && !enough( ); idx++ ) {
					const int v = variant_at( idx );
					if ( v & 1 )
						continue;
					const int k = sweep_last[ v ] - off;
					if ( sweep_last[ v ] < 0 || k < sweep_first[ v ] )
						continue;
					if ( out_of_budget( ) ) {
						off = off_end;
						break;
					}
					if ( allow_duck ) {
						if ( press_is_base( v, k ) )
							press_skips++;
						else {
							snap_verify( run_variant( v, k, -1, sweep_turn[ v ] ), v, k, -1 );
							sweep_runs++;
						}
					}
					// release runs interleaved with presses or they never get budget; release at 0 is even base run, skipped
					if ( allow_stand && !enough( ) && k >= 1 ) {
						if ( release_is_base( v, k ) )
							release_skips++;
						else if ( !out_of_budget( ) ) {
							snap_verify( run_variant( v, 0, k, sweep_turn[ v ] ), v, 0, k );
							release_runs++;
						}
					}
				}
			}
		};
		if ( !enough( ) && fabsf( delta_angle.m_y ) * static_cast< float >( run_ticks ) >= 0.5f ) {
			const int fan_n = std::max( depth, 2 );
			for ( int k = fan_n - 1; k >= 0 && !enough( ); k-- ) {
				const float turn = static_cast< float >( k ) / static_cast< float >( fan_n );
				for ( int v = steer ? 2 : 4; v <= 5 && !enough( ); v++ ) {
					if ( !base_ran[ v ] )
						continue;
					if ( out_of_budget( ) ) {
						k = -1;
						break;
					}
					run_variant( v, -1, -1, turn );
					fan_runs++;
					if ( br_turn_n[ v ] < 6 )
						br_turn[ v ][ br_turn_n[ v ]++ ] = { turn, last_land };
					if ( contact_tick > best_contact )
						best_contact = contact_tick;
					if ( v == 4 && sweep_approx[ v ] && contact_tick > 0 ) {
						sweep_center[ v ] = contact_tick;
						sweep_approx[ v ] = false;
						sweep_turn[ v ]   = turn;
					}
				}
			}
		}
		int br_runs = 0, br_pairs = 0;
		if ( !enough( ) && !replan ) {
			// wide: +-4 rarely straddled a lip ( 10-10 log 6 pairs / 180 searches ). model 103 -> 147 caught at same sims
			constexpr float k_br_off[ ] = { 10.f, -10.f, 25.f, -25.f, 45.f, -45.f };
			constexpr int k_br_iters    = 5;
			br_pt_t br_off[ 8 ][ 1 + std::size( k_br_off ) ];
			int br_off_n[ 8 ]{ };
			const auto differ = [ & ]( const float a, const float b ) {
				return ( a >= k_land_air ) != ( b >= k_land_air ) || ( a < k_land_air && fabsf( a - b ) > 0.25f );
			};
			bool stop  = false;
			in_bracket = true;
			for ( int v = 2; v <= 5; v++ )
				if ( base_ran[ v ] && br_turn_n[ v ] > 0 && br_turn[ v ][ 0 ].land != k_land_bad )
					br_off[ v ][ br_off_n[ v ]++ ] = { 0.f, br_turn[ v ][ 0 ].land };
			for ( const float off : k_br_off ) {
				for ( int v = 2; v <= 5 && !stop; v++ ) {
					if ( br_off_n[ v ] == 0 )
						continue;
					if ( enough( ) || out_of_budget( ) ) {
						stop = true;
						break;
					}
					yaw_off = off;
					run_variant( v, -1 );
					yaw_off = 0.f;
					br_runs++;
					if ( last_land != k_land_bad )
						br_off[ v ][ br_off_n[ v ]++ ] = { off, last_land };
				}
			}
			// air vs ground first, then the tallest step: a slope differs on every pair and must not eat the clock
			constexpr int k_br_pairs = 4;
			struct br_pair_t {
				int v;
				bool by_turn;
				br_pt_t lo, hi;
				float score;
			};
			br_pair_t pairs[ 4 * ( 5 + std::size( k_br_off ) ) ];
			int n_pairs     = 0;
			const auto scan = [ & ]( const int v, br_pt_t* pts, const int n, const bool by_turn ) {
				std::sort( pts, pts + n, [ ]( const br_pt_t& a, const br_pt_t& b ) { return a.p < b.p; } );
				for ( int j = 0; j + 1 < n; j++ ) {
					const br_pt_t &a = pts[ j ], &b = pts[ j + 1 ];
					if ( a.land == k_land_bad || b.land == k_land_bad || !differ( a.land, b.land ) )
						continue;
					const bool air = ( a.land >= k_land_air ) != ( b.land >= k_land_air );
					pairs[ n_pairs++ ] = { v, by_turn, a, b, air ? k_land_air : fabsf( a.land - b.land ) };
				}
			};
			for ( int v = 2; v <= 5; v++ ) {
				scan( v, br_turn[ v ], br_turn_n[ v ], true );
				scan( v, br_off[ v ], br_off_n[ v ], false );
			}
			std::sort( pairs, pairs + n_pairs, [ ]( const br_pair_t& a, const br_pair_t& b ) { return a.score > b.score; } );
			for ( int j = 0; j < std::min( n_pairs, k_br_pairs ) && !stop; j++ ) {
				br_pair_t& pr = pairs[ j ];
				br_pairs++;
				for ( int it = 0; it < k_br_iters; it++ ) {
					if ( enough( ) || out_of_budget( ) ) {
						stop = true;
						break;
					}
					const float mid = ( pr.lo.p + pr.hi.p ) * 0.5f;
					yaw_off         = pr.by_turn ? 0.f : mid;
					run_variant( pr.v, -1, -1, pr.by_turn ? mid : 1.f );
					yaw_off = 0.f;
					br_runs++;
					if ( last_found || last_land == k_land_bad )
						break;
					if ( differ( pr.lo.land, last_land ) )
						pr.hi = { mid, last_land };
					else
						pr.lo = { mid, last_land };
				}
			}
			in_bracket = false;
		}
		run_sweeps( );
		m_last_contact_tick = best_contact >= 0 ? best_contact : reach_tick;
		float sel_err = 0.f;
		for ( int p = 0; p < m_plan_count; p++ ) {
			const plan_t& pl = m_plans[ p ];
			const float aim  = orig_viewangle.m_y + std::clamp( delta_angle.m_y * static_cast< float >( pl.ticks + 1 ) * 0.5f, -90.f, 90.f );
			const float err  = eb_aim_err( pl.cmds[ 0 ].origin, pl.end_pos, aim );
			if ( sel < 0 || err < sel_err - 0.5f ) {
				sel     = p;
				sel_err = err;
			}
		}
		constexpr float k_swap_margin = 4.f;
		bool keep     = false;
		float cur_err = 0.f;
		if ( replan && sel >= 0 ) {
			const int left = m_prediction_ticks - m_replay_hits;
			const float aim = orig_viewangle.m_y + std::clamp( delta_angle.m_y * static_cast< float >( left + 1 ) * 0.5f, -90.f, 90.f );
			cur_err         = eb_aim_err( m_predicted_cmds[ m_replay_hits ].origin, m_follow_end, aim );
			keep            = sel_err > cur_err - k_swap_margin;
		}
		const auto viz_alts = [ & ]( const int follow ) {
			m_viz_alts = 0;
			if ( !GET_VARIABLE( g_variables.m_edgebug_visualize, bool ) )
				return;
			for ( int a = 0; a < m_plan_count; a++ ) {
				const plan_t& alt = m_plans[ a ];
				int n             = 0;
				for ( int p = 0; p <= alt.ticks && p < k_predicted_cmd_max; p++ )
					m_viz_alt[ a ][ n++ ] = alt.cmds[ p ].origin;
				m_viz_alt[ a ][ n++ ] = alt.end_pos;
				m_viz_alt_count[ a ]  = n;
				m_viz_alt_pos[ a ]    = alt.end_pos;
			}
			m_viz_alts     = m_plan_count;
			m_viz_sel      = follow;
			m_viz_alt_base = now_tick;
		};
		if ( keep ) {
			botox_dbg_log( "EB: steer keep at %d/%d best=%.1f cur=%.1f of %d\n", m_replay_hits, m_prediction_ticks, sel_err, cur_err, m_plan_count );
			viz_alts( -1 );
			sel = -1;
		} else if ( sel >= 0 ) {
			const plan_t& pl = m_plans[ sel ];
			if ( replan )
				botox_dbg_log( "EB: steer swap at %d/%d -> var=%d src=%d at=%d of %d steer=%.2f best=%.1f cur=%.1f\n", m_replay_hits, m_prediction_ticks,
				               pl.variant, pl.src, pl.ticks, m_plan_count, delta_angle.m_y, sel_err, cur_err );
			std::memcpy( m_predicted_cmds, pl.cmds, sizeof( predicted_cmd_t ) * ( pl.ticks + 1 ) );
			m_found            = true;
			m_prediction_ticks = pl.ticks;
			m_last_variant     = pl.variant;
			found_turn         = pl.turn;
			found_src          = pl.src;
			m_found_tick       = now_tick + ( pl.ticks + 1 );
			m_replay_hits      = 0;
			m_follow_end       = pl.end_pos;
			m_viz_count        = 0;
			m_viz_base         = now_tick;
			if ( GET_VARIABLE( g_variables.m_edgebug_visualize, bool ) ) {
				for ( int p = 0; p <= pl.ticks && p < k_predicted_cmd_max; p++ )
					m_viz_path[m_viz_count++] = pl.cmds[p].origin;
				m_viz_path[m_viz_count++] = pl.end_pos;
				m_viz_pos = pl.end_pos;
			}
			viz_alts( sel );
			eb_n_hit++;
		}
		botox_dbg_log( "EB: mode=%d dep=%d ty=%d%d as=%d win=%d run=%d reach=%d sreach=%d edge=%d et=%d base=%d st=%d fan=%d br=%d/%d sw=%d psk=%d rel=%d rsk=%d snp=%d/%d/%d rs=%d rr=%d ru=%d cg=%d cl=%d sims=%d/%d us=%lld/%lld rst=%lld con=%d ipt=%.5f vz=%.2f found=%d src=%d var=%d turn=%.2f at=%d tick=%d pth=%d/%d sel=%d err=%.1f rp=%d kp=%d str=%.2f\n",
		               detection_mode, depth, allow_duck ? 1 : 0, allow_stand ? 1 : 0, autostrafe ? 1 : 0, max_ticks, run_ticks, reach_tick, still_reach,
		               m_edge_target_valid ? 1 : 0, m_edge_target_valid ? m_edge_target_tick : -1, base_runs, still_skips, fan_runs, br_runs, br_pairs, sweep_runs, press_skips, release_runs, release_skips, snap_runs, snap_ticks, snap_used, rej_stance, rej_slope, rej_rise,
		               confirm_grounded, clip_lands, total_predictions, MAX_PREDICTIONS, budget.used_us( ),
		               static_cast< long long >( n_tick::interval( ) * 1000000.f * budget_share ), rst_us, best_contact, n_tick::interval( ),
		               m_backup_velocity.m_z, sel >= 0 ? 1 : 0, sel >= 0 ? found_src : 0, m_last_variant, sel >= 0 ? found_turn : 0.f, sel >= 0 ? m_prediction_ticks : -1,
		               sel >= 0 ? m_found_tick : 0, m_plan_count, want_paths, sel, sel_err, replan ? 1 : 0, keep ? 1 : 0, delta_angle.m_y );
	} while ( false );
	if ( m_found ) {
		if ( m_predicted_cmds[m_replay_hits].buttons & in_duck ) {
			cmd->m_buttons |= in_duck;
			m_ducked = true;
		} else {
			cmd->m_buttons &= ~in_duck;
			m_ducked = false;
		}
		if ( m_backup_flags & 1 )
			cmd->m_buttons |= in_jump;
		else
			cmd->m_buttons &= ~in_jump;
		cmd->m_forward_move = m_predicted_cmds[m_replay_hits].forwardmove;
		cmd->m_side_move = m_predicted_cmds[m_replay_hits].sidemove;
		cmd->m_view_point = m_predicted_cmds[m_replay_hits].viewangle;
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
	constexpr int k_max_pts = 24;
	const int pts = circle ? k_max_pts : 4;
	c_vector pad[k_max_pts];
	for ( int i = 0; i < pts; i++ ) {
		if ( circle ) {
			const float a = 6.28318530718f * static_cast< float >( i ) / static_cast< float >( pts );
			pad[i] = { p.m_x + cosf( a ) * reach, p.m_y + sinf( a ) * reach, p.m_z };
		}
		else {
			const float sx = ( i == 0 || i == 1 ) ? -reach : reach;
			const float sy = ( i == 1 || i == 2 ) ? reach : -reach;
			pad[i] = { p.m_x + sx, p.m_y + sy, p.m_z };
		}
	}
	if ( projected ) {
		/* one down ray per vertex ( 18u up = step height, 64 down ). paint thread, not the search clock. */
		c_trace_filter filter( g_ctx.m_local );
		auto drop = [ & ]( c_vector& v ) {
			ray_t ray( c_vector( v.m_x, v.m_y, v.m_z + 18.f ), c_vector( v.m_x, v.m_y, v.m_z - 64.f ) );
			trace_t tr;
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
			if ( tr.did_hit( ) )
				v.m_z = tr.m_end.m_z + 0.5f;
		};
		for ( int i = 0; i < pts; i++ )
			drop( pad[i] );
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
	if ( GET_VARIABLE( g_variables.m_edgebug_style, int ) == 2 || !GET_VARIABLE( g_variables.m_edgebug_del_mouse_fix, bool ) )
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
