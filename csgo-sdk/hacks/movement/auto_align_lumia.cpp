#include "movement_internal.h"
#include "lumia_prediction_state.h"
#include "../prediction/custom/lumia_native_fields.h"

// Lumia's native Auto Align search, with a Botox prediction adapter. The
// portable Lumia snapshots keep trials private without borrowing engine slots.
bool auto_align_lumia_pixel_jump(c_user_cmd*,const trace_t&,const c_vector&,const c_vector&,const c_vector&);
namespace {
class lumia_align_trial_t {
    struct maps_t {
        data_map_t* root=nullptr;
        n_lumia_state::layout_t layout;
        n_lumia_native::native_fields_t prediction,ordinary;
    };
    // never thread_local: manual mapped image has no tls index. createmove only
    static maps_t& maps(){static maps_t value;return value;}
    c_base_entity* local;
    n_lumia_state::snapshot_t caller;
    n_lumia_native::native_fields_t::snapshot_t predicted,ordinary;
    c_angle view{};
    float bounds=g_prediction.m_bounds_max_speed,last=g_prediction.m_last_max_speed;
    float real=g_prediction.m_real_max_speed,face=g_prediction.m_last_face_dz,pin=g_prediction.m_pin_face_dz;
    bool available=false;
public:
    explicit lumia_align_trial_t(c_base_entity* player):local(player){
        if(!local||!local->get_collideable()||!g_interfaces.m_prediction||!g_interfaces.m_move_helper)return;
        auto& cache=maps();auto* root=local->get_prediction_desc_map();
        if(cache.root!=root){cache.layout={};cache.layout.add(root);cache.root=root;}
        if(cache.layout.bytes<=0||!cache.prediction.bind(root)||!cache.ordinary.bind(local->get_data_desc_map()))return;
        predicted=cache.prediction.save_values(local);ordinary=cache.ordinary.save_values(local);
        caller.save(local,cache.layout);g_interfaces.m_prediction->get_local_view_angles(view);available=true;
    }
    ~lumia_align_trial_t(){
        if(!available)return;auto& cache=maps();caller.load(local,cache.layout);
        cache.ordinary.restore_values(local,ordinary);cache.prediction.restore_values(local,predicted);
        g_interfaces.m_prediction->set_local_view_angles(view);
        g_prediction.m_bounds_max_speed=bounds;g_prediction.m_last_max_speed=last;
        g_prediction.m_real_max_speed=real;g_prediction.m_last_face_dz=face;g_prediction.m_pin_face_dz=pin;
    }
    bool valid()const{return available;}
    bool begin_search(c_user_cmd* cmd){if(!available||!cmd)return false;g_prediction.begin(local,cmd,false,false);return true;}
    void end(){g_prediction.end(local);}
    bool restore_frame(int frame){if(!available)return false;g_prediction.restore_entity_to_predicted_frame(frame);return true;}
    c_vector absolute_origin()const{return local->get_abs_origin();}
    c_vector velocity()const{return local->get_velocity();}
};
}

void n_movement::impl_t::auto_align_lumia( c_user_cmd* cmd )
{
	if ( !GET_VARIABLE( g_variables.m_auto_align, bool ) )
		return;

	if ( tb_key( ) )
		return;

	if ( GET_VARIABLE( g_variables.m_wall_climb, bool ) &&
	     g_input.check_input( &GET_VARIABLE( g_variables.m_wall_climb_key, key_bind_t ) ) )
		return;

	if ( m_fireman_data.is_ladder )
		return;

	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || g_ctx.m_local->get_flags( ) & fl_onground )
		return;

	if ( const auto mt = g_ctx.m_local->get_move_type( ); mt == e_move_types::move_type_ladder || mt == e_move_types::move_type_noclip )
		return;

	auto& airstuck_key = GET_VARIABLE( g_variables.m_air_stuck_key, key_bind_t );
	if ( GET_VARIABLE( g_variables.m_air_stuck, bool ) && g_input.check_input( &airstuck_key ) )
		return;

	auto* collideable = g_ctx.m_local->get_collideable( );
	if ( !collideable )
		return;

	lumia_align_trial_t trial(g_ctx.m_local);
	if(!trial.valid())return;
	auto debug_search=[&](c_user_cmd* test) { return trial.begin_search(test); };
	const float max_radias = 3.14159265358979323846f * 2.f;
	const float step = max_radias / 16.f;
	c_vector start_pos = trial.absolute_origin();
	const auto mins = collideable->get_obb_mins( );
	const auto maxs = collideable->get_obb_maxs( );
	c_trace_filter_world fil;

	const float tick_interval = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const int cmds_pred = g_interfaces.m_prediction->m_commands_predicted - 1;

	constexpr bool freelook_enabled = true; // Chillware's wall-relative hold is always available.
	auto& ps_key = GET_VARIABLE( g_variables.m_pixel_surf_key, key_bind_t );
	const bool ps_key_active = g_input.check_input( &ps_key );

	for ( float a = 0.f; a < max_radias; a += step ) {
		c_vector end_pos;
		end_pos.m_x = cos( a ) + start_pos.m_x;
		end_pos.m_y = sin( a ) + start_pos.m_y;
		end_pos.m_z = start_pos.m_z;
		trace_t trace;
		ray_t ray( start_pos, end_pos, mins, maxs );

		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &fil, &trace );
		if ( trace.m_fraction < 1.f && trace.m_plane.m_normal.m_z == 0.f ) {
            wall_detected=true;
            if(auto_align_lumia_pixel_jump(cmd,trace,start_pos,mins,maxs))return;
			c_vector angles{ trace.m_plane.m_normal.m_x * -0.005f, trace.m_plane.m_normal.m_y * -0.005f, 0.f };
			const c_vector end_pos2 = start_pos + angles;
			trace_t trace2;
			ray_t ray2( start_pos, end_pos2, mins, maxs );
			g_interfaces.m_engine_trace->trace_ray( ray2, mask_playersolid, &fil, &trace2 );

			if ( trace2.m_fraction == 1.f ) {
				c_angle to_wall = angles.to_angle2( );
				to_wall.normalize( );
				c_vector velo = trial.velocity();
				velo.m_z = 0.f;

				if ( velo.length_2d( ) > 0.f ) {
					c_angle velo_ang = velo.to_angle2( );
					c_angle delta = velo_ang - to_wall;
					delta.normalize( );
				if ( fabsf( delta.m_z ) > 90.f )
					break;
				}

				float rotation = deg2rad( to_wall.m_y - cmd->m_view_point.m_y );
				float cos_rot = cos( rotation );
				float sin_rot = sin( rotation );
				bool should_align = false;

				for ( int i = 1; i <= 5; i++ ) {
					float forwardmove = cos_rot * i * 9.f;
					float sidemove = -sin_rot * i * 9.f;
					c_user_cmd fakecmd = *cmd;
					fakecmd.m_forward_move = forwardmove;
					fakecmd.m_side_move = sidemove;
					if(!debug_search(&fakecmd ))return;
					trial.end();

					c_vector start_pos2 = trial.absolute_origin();
					const c_vector end_pos3 = start_pos2 + angles;
					trace_t trace3;
					ray_t ray3( start_pos2, end_pos3, mins, maxs );
					g_interfaces.m_engine_trace->trace_ray( ray3, mask_playersolid, &fil, &trace3 );
					if ( trace3.m_fraction < 1.f ) {
						cmd->m_forward_move = forwardmove;
						cmd->m_side_move = sidemove;
						should_align = true;
						if(!trial.restore_frame(cmds_pred))return;
						break;
					}
					if(!trial.restore_frame(cmds_pred))return;
				}

				if ( !should_align ) {
					for ( int i = 1; i <= 5; i++ ) {
						float forwardmove = cos_rot * i * 9.f;
						float sidemove = -sin_rot * i * 9.f;
						c_vector absorigin = trial.absolute_origin();
						c_user_cmd fakecmd2 = *cmd;
						fakecmd2.m_forward_move = forwardmove;
						fakecmd2.m_side_move = sidemove;
						if(!debug_search(&fakecmd2 ))return;
						trial.end();
						if(!debug_search(&fakecmd2 ))return;
						trial.end();

						c_vector start_pos2 = trial.absolute_origin();
						start_pos2.m_z = absorigin.m_z;
						const c_vector end_pos3 = start_pos2 + angles;
						trace_t trace3;
						ray_t ray3( start_pos2, end_pos3, mins, maxs );
						g_interfaces.m_engine_trace->trace_ray( ray3, mask_playersolid, &fil, &trace3 );
						if ( trace3.m_fraction < 1.f ) {
							cmd->m_forward_move = forwardmove;
							cmd->m_side_move = sidemove;
							should_align = true;
							if(!trial.restore_frame(cmds_pred))return;
							break;
						}
						if(!trial.restore_frame(cmds_pred))return;
					}
				}
			}
			else {
				for ( int i = 45; i > 0; i -= 5 ) {
					float forwardmove = 0.f;
					float sidemove = 0.f;

					if ( cmd->m_buttons & in_forward )
						forwardmove = static_cast< float >( i );
					if ( cmd->m_buttons & in_back )
						forwardmove = static_cast< float >( -i );
					if ( cmd->m_buttons & in_moveleft )
						sidemove = static_cast< float >( -i );
					if ( cmd->m_buttons & in_moveright )
						sidemove = static_cast< float >( i );

					if ( !forwardmove && !sidemove && freelook_enabled && ps_key_active ) {
						c_angle to_wall = angles.to_angle2( );
						to_wall.normalize( );
						float rotation = deg2rad( to_wall.m_y - cmd->m_view_point.m_y );
						float cr = cos( rotation );
						float sr = sin( rotation );
						forwardmove = cr * static_cast< float >( i );
						sidemove = -sr * static_cast< float >( i );
					}

					c_user_cmd fakecmd5 = *cmd;
					fakecmd5.m_forward_move = forwardmove;
					fakecmd5.m_side_move = sidemove;
					if(!debug_search(&fakecmd5 ))return;
					trial.end();

					float zvelo = trial.velocity().m_z;
					if ( zvelo == g_prediction.get_engine_target_predict_z_velocity() ) {
						if(!trial.restore_frame(cmds_pred))return;
						cmd->m_forward_move = forwardmove;
						cmd->m_side_move = sidemove;
						break;
					}
					if(!trial.restore_frame(cmds_pred))return;
				}

				if ( !m_pixelsurf_data.should_pixel_surf ) {
					if(!trial.restore_frame(cmds_pred))return;

					auto sv_gravity_cvar = g_interfaces.m_convar->find_var( "sv_gravity" );
					const float sv_gravity = sv_gravity_cvar ? sv_gravity_cvar->get_float( ) : 800.f;
					const float tick_rate = ( tick_interval > 0.f ) ? ( 1.0f / tick_interval ) : 64.0f;
					const float target_z_velo = ( ( sv_gravity / 2.f ) / tick_rate ) * -1.f;

					c_vector scan_wall_normal = trace.m_plane.m_normal;
					c_vector wall_angles{ scan_wall_normal.m_x * -1.f, scan_wall_normal.m_y * -1.f, 0.f };
					c_angle to_wall_angle = wall_angles.to_angle2( );
					to_wall_angle.normalize( );
					bool has_duck_candidate = false;
					float duck_candidate_forward = 0.0f;
					float duck_candidate_side = 0.0f;

					for ( int scan_attempt = 0; scan_attempt < 3; scan_attempt++ ) {
						if(!trial.restore_frame(cmds_pred))return;
						for ( int move_mult = 1; move_mult <= 5; move_mult++ ) {
							float forwardmove = 0.f;
							float sidemove = 0.f;

							if ( cmd->m_buttons & in_forward )
								forwardmove = static_cast< float >( move_mult );
							if ( cmd->m_buttons & in_back )
								forwardmove = static_cast< float >( -move_mult );
							if ( cmd->m_buttons & in_moveleft )
								sidemove = static_cast< float >( -move_mult );
							if ( cmd->m_buttons & in_moveright )
								sidemove = static_cast< float >( move_mult );

							if ( !forwardmove && !sidemove && freelook_enabled && ps_key_active ) {
								float rotation = deg2rad( to_wall_angle.m_y - cmd->m_view_point.m_y );
								float cr = cos( rotation );
								float sr = sin( rotation );
								forwardmove = cr * static_cast< float >( move_mult );
								sidemove = -sr * static_cast< float >( move_mult );
							}

							if ( forwardmove != 0.f || sidemove != 0.f ) {
								auto test_bounce_with_duck = [&]( bool use_duck ) -> bool {
									c_user_cmd test_cmd = *cmd;
									test_cmd.m_forward_move = forwardmove;
									test_cmd.m_side_move = sidemove;
									test_cmd.m_buttons |= in_jump;
									if ( use_duck )
										test_cmd.m_buttons |= in_duck;
									else
										test_cmd.m_buttons &= ~in_duck;

									if(!debug_search(&test_cmd ))return false;
									trial.end();

									c_vector pred_velo = trial.velocity();
									c_vector pred_origin = trial.absolute_origin();
									bool matched = false;
									if ( pred_velo.m_z == target_z_velo ) {
										trace_t verify_trace;
										ray_t verify_ray( pred_origin, pred_origin + ( scan_wall_normal * -5.f ), mins, maxs );
										g_interfaces.m_engine_trace->trace_ray( verify_ray, mask_playersolid, &fil, &verify_trace );
										matched = verify_trace.m_fraction < 1.f && verify_trace.m_plane.m_normal.m_z == 0.f;
									}
									if(!trial.restore_frame(cmds_pred))return false;
									return matched;
								};

								if ( test_bounce_with_duck( false ) ) {
									cmd->m_forward_move = forwardmove;
									cmd->m_side_move = sidemove;
									cmd->m_buttons |= in_jump;
									goto found_bounce;
								}
								if ( !has_duck_candidate && test_bounce_with_duck( true ) ) {
									has_duck_candidate = true;
									duck_candidate_forward = forwardmove;
									duck_candidate_side = sidemove;
								}
							}
							if(!trial.restore_frame(cmds_pred))return;
						}
						if(!trial.restore_frame(cmds_pred))return;
					found_bounce:;
					}
					if ( has_duck_candidate ) {
						cmd->m_forward_move = duck_candidate_forward;
						cmd->m_side_move = duck_candidate_side;
						cmd->m_buttons |= in_jump;
						cmd->m_buttons |= in_duck;
					}
				}
			}
			break;
		}
	}
}
