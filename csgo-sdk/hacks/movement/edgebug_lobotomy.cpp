#include "edgebug.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "wall_climb.h"

extern bool g_air_stuck_owns_cmd;
bool ps_latched( );
bool eb_cmd_taken( );
extern void botox_dbg_log( const char* fmt, ... );
void start_movement_fix( c_user_cmd* cmd );
void end_movement_fix( c_user_cmd* cmd );

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
				/* odd variant = duck held all run */
				/* dont unduck: duck held = odd only, standing = both */
				const bool user_duck = ( m_user_buttons & in_duck ) != 0;
				if ( GET_VARIABLE( g_variables.m_edgebug_dont_unduck, bool ) ? ( user_duck && !( v & 1 ) )
				                                                             : ( GET_VARIABLE( g_variables.m_edgebug_cur_stance, bool ) && ( ( v & 1 ) != 0 ) != user_duck ) )
					continue;
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
