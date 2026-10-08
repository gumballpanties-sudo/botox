#include "edge_skip.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "assist_predict.h"
#include "edgebug.h"
#include "wall_climb.h"

extern bool g_air_stuck_owns_cmd;
bool ps_latched( );
bool bind_cmd_taken( );
void play_trick_sound( const int sound_index, const float volume, const char* custom_path );
extern void botox_dbg_log( const char* fmt, ... );

float n_edge_skip::perfect_angle( const c_vector& vel, const float friction, const float max_speed )
{
	static auto sv_airaccelerate     = g_interfaces.m_convar->find_var( "sv_airaccelerate" );
	static auto sv_air_max_wishspeed = g_interfaces.m_convar->find_var( "sv_air_max_wishspeed" );
	const float aa    = sv_airaccelerate ? sv_airaccelerate->get_float( ) : 12.f;
	const float wish  = sv_air_max_wishspeed ? sv_air_max_wishspeed->get_float( ) : 30.f;
	const float diff  = wish - aa * friction * max_speed * n_tick::engine_interval( );
	const float speed = vel.length_2d( );
	if ( diff <= 0.f || speed == 0.f )
		return 0.f;
	return fabsf( 90.f - rad2deg( acosf( std::clamp( diff / speed, -1.f, 1.f ) ) ) );
}

namespace {
	constexpr int k_max_sims = 2048;

	bool slope_under( const c_vector& at, const bool ducked )
	{
		c_trace_filter filter( g_ctx.m_local );
		ray_t ray( at, c_vector( at.m_x, at.m_y, at.m_z - 2.f ), c_vector( -16.f, -16.f, 0.f ), c_vector( 16.f, 16.f, ducked ? 54.f : 72.f ) );
		trace_t tr;
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		return tr.m_fraction < 1.f && tr.m_plane.m_normal.m_z > 0.f && tr.m_plane.m_normal.m_z < 0.7f;
	}

	void restore_start( )
	{
		g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
	}

	bool use_eb( )
	{
		return GET_VARIABLE( g_variables.m_edge_skip_use_eb, bool );
	}
	template< typename T >
	const T& knob( const std::uint32_t eb, const std::uint32_t own )
	{
		return GET_VARIABLE( use_eb( ) ? eb : own, T );
	}
}

float n_edge_skip::impl_t::lock_amount( ) const
{
	if ( !m_active || !knob< bool >( g_variables.m_edgebug_mouse_lock, g_variables.m_edge_skip_mouse_lock ) )
		return 0.f;
	const float strength = std::clamp( knob< float >( g_variables.m_edgebug_mouse_lock_strength, g_variables.m_edge_skip_mouse_lock_strength ), 0.f, 100.f ) / 100.f;
	const float ramp = knob< int >( g_variables.m_edgebug_mouse_lock_type, g_variables.m_edge_skip_mouse_lock_type ) == 0
	                       ? std::clamp( static_cast< float >( m_hit + 1 ) / static_cast< float >( m_last + 1 ), 0.f, 1.f )
	                       : 1.f;
	return strength * ramp;
}

void n_edge_skip::impl_t::render( ) const
{
	if ( !m_active || !knob< bool >( g_variables.m_edgebug_visualize, g_variables.m_edge_skip_visualize ) )
		return;
	n_edgebug::draw_plan( m_viz, m_last + 2, m_hit, m_land_pos );
}

bool n_edge_skip::impl_t::bent( )
{
	if ( m_max_bend >= 180.f )
		return false;
	const c_vector vel = g_ctx.m_local->get_velocity( );
	if ( vel.length_2d( ) <= 1.f || fabsf( std::remainderf( rad2deg( atan2f( vel.m_y, vel.m_x ) ) - m_course_yaw, 360.f ) ) <= m_max_bend )
		return false;
	m_bends++;
	return true;
}

void n_edge_skip::impl_t::reset( const bool forget )
{
	m_active = false;
	m_hit    = 0;
	if ( forget ) {
		m_had_ground = false;
		m_yaw_valid  = false;
	}
}

void n_edge_skip::impl_t::pre( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	if ( !cmd || !local || !local->is_alive( ) ) {
		m_had_ground = false;
		m_yaw_valid  = false;
		return;
	}
	m_start_flags     = local->get_flags( );
	m_start_move_type = local->get_move_type( );
	m_start_vel       = local->get_velocity( );
	m_user_buttons    = cmd->m_buttons;
	m_eb_latched = g_edgebug.m_found;
	if ( ( m_start_flags & fl_onground ) && m_start_move_type == move_type_walk ) {
		m_ground_z   = local->get_origin( ).m_z;
		m_had_ground = true;
		m_air        = 0;
	} else if ( m_air < 100000 )
		m_air++;
	const float yaw = rad2deg( atan2f( m_start_vel.m_y, m_start_vel.m_x ) );
	m_turn_left     = m_yaw_valid && std::remainderf( yaw - m_prev_yaw, 360.f ) > 0.f;
	m_prev_yaw      = yaw;
	m_yaw_valid     = true;
}

void n_edge_skip::impl_t::apply( c_user_cmd* cmd, const tick_t& t, const c_angle& view ) const
{
	cmd->m_forward_move   = 0.f;
	cmd->m_side_move      = t.side;
	cmd->m_view_point     = view;
	cmd->m_view_point.m_y = t.yaw;
	start_movement_fix( cmd );
	cmd->m_view_point = view;
	end_movement_fix( cmd );
	n_assist::set_move( cmd, cmd->m_forward_move, cmd->m_side_move );
	cmd->m_buttons &= ~( in_duck | in_jump );
	if ( t.duck )
		cmd->m_buttons |= in_duck;
}

void n_edge_skip::impl_t::sim( c_user_cmd* cmd, const int index, const tick_t& t )
{
	const auto local = g_ctx.m_local;
	cmd->m_buttons   = m_base_buttons;
	apply( cmd, t, m_view );
	local->get_tick_base( ) = m_tick_base0 + index;
	g_prediction.begin( local, cmd, false, true );
	g_prediction.end( local );
	m_sims++;
}

bool n_edge_skip::impl_t::out( ) const
{
	return m_sims >= k_max_sims || m_budget.expired( );
}

bool n_edge_skip::impl_t::replay_to( c_user_cmd* cmd, const int end )
{
	restore_start( );
	for ( int i = 0; i < end; i++ ) {
		if ( out( ) )
			return false;
		sim( cmd, i, m_plan[ i ] );
	}
	return true;
}

bool n_edge_skip::impl_t::grab( c_user_cmd* cmd, const int prefix, const float ground_z, const bool first_neg, const int window )
{
	const auto local = g_ctx.m_local;
	const int last   = std::min( prefix + window, k_max_ticks ) - 1;
	if ( prefix < 0 || last < prefix )
		return false;
	const float dt = n_tick::engine_interval( );
	c_trace_filter filter( local );
	/* necessary, never sufficient: duck hull grown by the tick's travel, swept from the +9 start down past the catch band.
	   empty = no floor. start solid ( wall in the box ) = can't tell, sim it */
	const auto may_land = [ & ]( const c_vector& start, const c_vector& vel ) {
		const float top = start.m_z + 9.f + std::max( vel.m_z, 0.f ) * dt;
		if ( top < ground_z - 2.f )
			return false;
		const float r = ( vel.length_2d( ) + 30.f ) * dt + 1.f;
		ray_t ray( c_vector( start.m_x, start.m_y, top + 1.f ), c_vector( start.m_x, start.m_y, ground_z - 4.5f ),
		           c_vector( -16.f - r, -16.f - r, 0.f ), c_vector( 16.f + r, 16.f + r, 54.f ) );
		trace_t tr;
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
		return tr.m_start_solid || tr.m_fraction < 1.f;
	};
	const bool base_saved = prefix > 0 && g_prediction.snapshot_save( 0 );
	for ( int pass = 0; pass < 2; pass++ ) {
		const bool neg = ( pass == 0 ) == first_neg;
		if ( pass > 0 && base_saved ) {
			restore_start( );
			g_prediction.snapshot_load( 0 );
			m_snaps++;
		} else if ( pass > 0 || prefix == 0 ) {
			if ( !replay_to( cmd, prefix ) )
				return false;
		}
		m_chains++;
		constexpr int k_max_cand = 64;
		int cand[ k_max_cand ], slot[ k_max_cand ];
		int n_cand = 0;
		for ( int k = prefix; k <= last && !out( ); k++ ) {
			const c_vector start = local->get_origin( );
			const c_vector vel   = local->get_velocity( );
			const float ang      = perfect_angle( vel, local->get_surface_friction( ), local->get_max_speed( ) );
			const float vel_yaw  = rad2deg( atan2f( vel.m_y, vel.m_x ) );
			tick_t& t            = m_plan[ k ];
			t.yaw                = neg ? vel_yaw - ang : vel_yaw + ang;
			t.side               = neg ? -450.f : 450.f;
			t.duck               = false;
			t.origin             = start;
			const bool cand_k    = may_land( start, vel );
			const int s = cand_k && n_cand < k_max_cand && g_prediction.snapshot_save( 1 + n_cand ) ? 1 + n_cand : -1;
			sim( cmd, k, t );
			/* no-duck landed: test the twin, but never take it on this tick */
			if ( ( local->get_flags( ) & fl_onground ) || local->get_move_type( ) != move_type_walk )
				break;
			if ( bent( ) )
				break;
			if ( cand_k && n_cand < k_max_cand ) {
				cand[ n_cand ] = k;
				slot[ n_cand++ ] = s;
			}
			if ( local->get_origin( ).m_z + 9.f < ground_z - 2.f && local->get_velocity( ).m_z <= 0.f )
				break;
		}
		m_cands += n_cand;
		for ( int c = 0; c < n_cand; c++ ) {
			const int k = cand[ c ];
			if ( slot[ c ] >= 0 ) {
				if ( out( ) )
					return false;
				restore_start( );
				g_prediction.snapshot_load( slot[ c ] );
				m_snaps++;
			} else if ( !replay_to( cmd, k ) || out( ) )
				return false;
			tick_t twin = m_plan[ k ];
			twin.duck   = true;
			sim( cmd, k, twin );
			m_twins++;
			if ( ( local->get_flags( ) & fl_onground ) && local->get_origin( ).m_z >= ground_z - 2.f ) {
				m_plan[ k ].duck = true;
				m_last           = k;
				m_plan_ground_z  = ground_z;
				m_land_pos       = local->get_origin( );
				return true;
			}
		}
	}
	return false;
}

/* bug family: per side ( turn side first ) strafe n = 0..4 ticks, then no input, never duck. first tick vz rises airborne = bug
   ( unless a steep slope sits under both ends ), then ledge grab onto its end z. a rise always ends the run */
bool n_edge_skip::impl_t::family( c_user_cmd* cmd, const bool first_neg )
{
	const auto local = g_ctx.m_local;
	for ( int s = 0; s < 2; s++ ) {
		const bool neg = ( s == 0 ) == first_neg;
		/* n = 0 never strafes: same run on both sides, once */
		for ( int n = s == 0 ? 0 : 1; n <= 4; n++ ) {
			if ( out( ) )
				return false;
			const int strafe = n == 0 ? 0 : n_tick::ticks( n );
			restore_start( );
			float prev_vz     = local->get_velocity( ).m_z;
			c_vector prev_end = local->get_origin( );
			bool prev_ducked  = ( local->get_flags( ) & fl_ducking ) != 0;
			for ( int t = 0; t < m_max_ticks && !out( ); t++ ) {
				tick_t& p = m_plan[ t ];
				p.origin  = local->get_origin( );
				p.duck    = false;
				p.yaw     = m_view.m_y;
				p.side    = 0.f;
				if ( t < strafe ) {
					const c_vector vel  = local->get_velocity( );
					const float ang     = perfect_angle( vel, local->get_surface_friction( ), local->get_max_speed( ) );
					const float vel_yaw = rad2deg( atan2f( vel.m_y, vel.m_x ) );
					p.yaw               = neg ? vel_yaw - ang : vel_yaw + ang;
					p.side              = neg ? -450.f : 450.f;
				}
				sim( cmd, t, p );
				if ( ( local->get_flags( ) & fl_onground ) || local->get_move_type( ) != move_type_walk || bent( ) )
					break;
				const c_vector end = local->get_origin( );
				const float vz     = local->get_velocity( ).m_z;
				const bool ducked  = ( local->get_flags( ) & fl_ducking ) != 0;
				if ( vz > prev_vz ) {
					if ( !slope_under( end, ducked ) || !slope_under( prev_end, prev_ducked ) ) {
						m_bugs++;
						if ( t + 1 < m_max_ticks && grab( cmd, t + 1, end.m_z, true, m_max_ticks - ( t + 1 ) ) ) {
							m_found_bug = t;
							return true;
						}
					}
					break;
				}
				prev_vz     = vz;
				prev_end    = end;
				prev_ducked = ducked;
			}
		}
	}
	return false;
}

void n_edge_skip::impl_t::search( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	const float predict_time = std::clamp( knob< float >( g_variables.m_edgebug_predict_time, g_variables.m_edge_skip_predict_time ), 0.02f, 2.f );
	m_max_ticks              = std::clamp( static_cast< int >( std::round( predict_time / n_tick::interval( ) ) ), 2, k_max_ticks );
	const bool walk_off = m_had_ground && m_air >= 1 && m_air <= n_tick::ticks( 6 );

	const int depth = std::clamp( use_eb( ) ? ( GET_VARIABLE( g_variables.m_edgebug_detection_mode, int ) == 1
	                                                ? GET_VARIABLE( g_variables.m_edgebug_search_amount, int )
	                                                : 2 )
	                                        : GET_VARIABLE( g_variables.m_edge_skip_search_amount, int ),
	                              1, 4 );
	m_budget.start( 0.15f * static_cast< float >( depth ), 1.f / 128.f, n_tick::search_es );
	m_sims = m_bugs = m_chains = m_cands = m_twins = m_snaps = m_bends = 0;
	m_found_bug = -1;
	const c_angle view0 = cmd->m_view_point;
	const float fwd0    = cmd->m_forward_move;
	const float side0   = cmd->m_side_move;
	const int buttons0  = cmd->m_buttons;
	m_view              = view0;
	m_base_buttons      = m_user_buttons & ~( in_duck | in_jump | in_forward | in_back | in_moveleft | in_moveright );
	restore_start( );
	m_tick_base0         = local->get_tick_base( );
	const c_vector org0  = local->get_origin( );
	const c_vector vel0  = local->get_velocity( );
	const bool ducked0   = ( local->get_flags( ) & fl_ducking ) != 0;
	m_course_yaw = rad2deg( atan2f( vel0.m_y, vel0.m_x ) );
	m_max_bend   = vel0.length_2d( ) > 1.f ? std::clamp( knob< float >( g_variables.m_edgebug_autostrafe_max_angle, g_variables.m_edge_skip_max_angle ), 0.f, 180.f ) : 180.f;

	int src = 0;
	if ( walk_off && grab( cmd, 0, m_ground_z, m_turn_left, m_max_ticks ) )
		src = 1;

	bool reach = false;
	if ( !src ) {
		static auto sv_gravity = g_interfaces.m_convar->find_var( "sv_gravity" );
		const float grav       = sv_gravity ? sv_gravity->get_float( ) : 800.f;
		const float window     = static_cast< float >( m_max_ticks ) * n_tick::engine_interval( );
		const float grow       = 2.f + 30.f * static_cast< float >( n_tick::ticks( 4 ) ) * window;
		const float seg_t      = window / 3.f;
		const c_vector mins( -16.f - grow, -16.f - grow, ducked0 ? -10.f : -1.f );
		const c_vector maxs( 16.f + grow, 16.f + grow, 72.f + grav * seg_t * seg_t * 0.125f );
		c_trace_filter filter( local );
		c_vector prev = org0;
		for ( int seg = 1; seg <= 3 && !reach; seg++ ) {
			const float ts = seg_t * static_cast< float >( seg );
			c_vector p     = org0 + vel0 * ts;
			p.m_z          = org0.m_z + vel0.m_z * ts - 0.5f * grav * ts * ts;
			ray_t ray( prev, p, mins, maxs );
			trace_t tr;
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &tr );
			reach = tr.m_start_solid || tr.m_fraction < 1.f;
			prev  = p;
		}
		if ( reach && family( cmd, m_turn_left ) )
			src = 2;
	}

	cmd->m_view_point       = view0;
	cmd->m_forward_move     = fwd0;
	cmd->m_side_move        = side0;
	cmd->m_buttons          = buttons0;
	local->get_tick_base( ) = m_tick_base0;
	/* never leave a sim tick live */
	if ( m_sims > 0 )
		restore_start( );

	if ( walk_off || reach )
		botox_dbg_log( "ES: src=%d wo=%d air=%d gz=%.2f z=%.2f vz=%.2f turn=%d win=%d reach=%d chains=%d bugs=%d cand=%d twins=%d snp=%d sims=%d us=%lld found=%d bug=%d at=%d bend=%d/%.0f depth=%d eb=%d\n",
		               src, walk_off ? 1 : 0, m_air, m_ground_z, org0.m_z, vel0.m_z, m_turn_left ? 1 : 0, m_max_ticks, reach ? 1 : 0, m_chains,
		               m_bugs, m_cands, m_twins, m_snaps, m_sims, m_budget.used_us( ), src ? 1 : 0, m_found_bug, src ? m_last : -1, m_bends,
		               m_max_bend, depth, use_eb( ) ? 1 : 0 );
	if ( !src )
		return;
	m_active      = true;
	m_active_tick = g_interfaces.m_global_vars_base->m_tick_count;
	m_hit         = 0;
	for ( int i = 0; i <= m_last; i++ )
		m_viz[ i ] = m_plan[ i ].origin;
	m_viz[ m_last + 1 ] = m_land_pos;
	g_edgebug.m_found  = false;
	g_edgebug.m_ducked = false;
}

void n_edge_skip::impl_t::post( c_user_cmd* cmd )
{
	const auto local = g_ctx.m_local;
	if ( !cmd || !local || !local->is_alive( ) || !GET_VARIABLE( g_variables.m_edgebug_edge_skip, bool ) ||
	     !g_input.check_input( &GET_VARIABLE( g_variables.m_edge_skip_key, key_bind_t ) ) ) {
		reset( );
		return;
	}
	if ( m_start_move_type != move_type_walk || g_air_stuck_owns_cmd || g_wall_climb.caught( cmd ) || ps_latched( ) || bind_cmd_taken( ) ) {
		reset( );
		return;
	}
	if ( m_start_flags & fl_onground ) {
		if ( m_active ) {
			const bool on_lip = m_ground_z >= m_plan_ground_z - 2.f;
			const bool eb_rang = g_eb_sound_tick && g_eb_sound_tick >= m_active_tick;
			if ( on_lip )
				++g_es_fired;
			botox_dbg_log( "ES: landed hit=%d last=%d gz=%.2f z=%.2f lip=%d eb_rang=%d\n", m_hit - 1, m_last, m_plan_ground_z, m_ground_z,
			               on_lip ? 1 : 0, eb_rang ? 1 : 0 );
			if ( const int sound = knob< int >( g_variables.m_edge_bug_sound, g_variables.m_edge_skip_sound ); on_lip && !eb_rang && sound > 0 ) {
				g_es_sound_tick = g_interfaces.m_global_vars_base->m_tick_count;
				play_trick_sound( sound,
				                  knob< float >( g_variables.m_edge_bug_sound_volume, g_variables.m_edge_skip_sound_volume ),
				                  knob< std::string >( g_variables.m_edge_bug_sound_custom, g_variables.m_edge_skip_sound_custom ).c_str( ) );
			}
		}
		reset( );
		return;
	}
	if ( m_active ) {
		restore_start( );
		if ( m_hit > m_last ) {
			botox_dbg_log( "ES: missed spent=%d z=%.2f gz=%.2f\n", m_last, local->get_origin( ).m_z, m_plan_ground_z );
			reset( );
		} else if ( const float dist = local->get_origin( ).dist_to( m_plan[ m_hit ].origin ); dist > 1.f ) {
			botox_dbg_log( "ES: drop at %d/%d dist=%.3f\n", m_hit, m_last, dist );
			reset( );
		}
	}
	if ( !m_active ) {
		static int n_eb = 0, n_vz = 0, n_duck = 0, n_run = 0;
		if ( static unsigned long long next = 0ull; GetTickCount64( ) >= next ) {
			next = GetTickCount64( ) + 1000ull;
			if ( n_eb || n_vz || n_duck || n_run )
				botox_dbg_log( "ES: gates eb=%d vz=%d duck=%d run=%d\n", n_eb, n_vz, n_duck, n_run );
			n_eb = n_vz = n_duck = n_run = 0;
		}
		if ( m_eb_latched )
			n_eb++;
		else if ( m_start_vel.m_z > 0.f )
			n_vz++;
		else if ( m_user_buttons & in_duck )
			n_duck++;
		else {
			n_run++;
			search( cmd );
		}
	}
	if ( !m_active )
		return;
	const c_angle view = cmd->m_view_point;
	apply( cmd, m_plan[ m_hit ], view );
	m_hit++;
}
