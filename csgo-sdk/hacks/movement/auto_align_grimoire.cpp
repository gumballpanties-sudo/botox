/* auto align type 2 grimoire = USELESSSRCCODE/grimv5/features/movement/movement.cpp AlignmentAssist::handle_alignment ( 2293-2456 ) 1:1.
   botox owner gates as lumia ( tb key, fireman ladder, air stuck key ); no pj creep, no wedge guard ( donor has neither ) */
#include "movement_internal.h"

void n_movement::impl_t::auto_align_grimoire( c_user_cmd* cmd )
{
	if ( !GET_VARIABLE( g_variables.m_auto_align, bool ) )
		return;
	if ( tb_key( ) || m_fireman_data.is_ladder || as_key( ) )
		return;

	const auto local = g_ctx.m_local;
	if ( !local || !local->is_alive( ) || local->get_flags( ) & fl_onground )
		return;
	if ( const auto mt = local->get_move_type( ); mt == e_move_types::move_type_ladder || mt == e_move_types::move_type_noclip )
		return;
	auto* collideable = local->get_collideable( );
	if ( !collideable )
		return;

	constexpr float k_max_radians       = 2.f * 3.141592654f;
	constexpr float k_step              = k_max_radians / 16.f;
	constexpr float k_wall_offset_scale = -0.005f;
	constexpr float k_speed_factor      = 9.f;
	constexpr int k_max_test_steps      = 5;
	constexpr float k_stability_start   = 45.f;
	constexpr float k_stability_step    = 5.f;
	constexpr float k_epsilon           = 0.001f;
	/* donor's pixelsurf vz, 64 tick only ( 128 = -3.125 ): kept */
	constexpr float k_surf_vz           = -6.25f;

	const c_vector start_pos = local->get_abs_origin( );
	const c_vector mins      = collideable->get_obb_mins( );
	const c_vector maxs      = collideable->get_obb_maxs( );
	c_trace_filter_world filter;
	const int frame = g_interfaces.m_prediction->m_commands_predicted - 1;

	/* float accumulator like the donor ( 16 or 17 steps ) */
	for ( float angle = 0.f; angle < k_max_radians; angle += k_step ) {
		const c_vector end_pos( std::cos( angle ) + start_pos.m_x, std::sin( angle ) + start_pos.m_y, start_pos.m_z );
		trace_t trace;
		ray_t ray( start_pos, end_pos, mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &trace );
		if ( trace.m_fraction >= 1.f || trace.m_plane.m_normal.m_z != 0.f )
			continue;
		/* pixel_surf's wall gate, never on a start solid hit ( botox / lumia rule ) */
		if ( !trace.m_start_solid )
			wall_detected = true;

		const c_vector wall_dir( trace.m_plane.m_normal.m_x * k_wall_offset_scale, trace.m_plane.m_normal.m_y * k_wall_offset_scale, 0.f );
		trace_t touch_trace;
		ray_t touch_ray( start_pos, start_pos + wall_dir, mins, maxs );
		g_interfaces.m_engine_trace->trace_ray( touch_ray, mask_playersolid, &filter, &touch_trace );

		c_angle to_wall_ang = wall_dir.to_angle2( );
		to_wall_ang.normalize( );
		const float delta_yaw = deg2rad( to_wall_ang.m_y - cmd->m_view_point.m_y );
		const float cos_rot   = std::cos( delta_yaw );
		const float sin_rot   = std::sin( delta_yaw );

		{
			c_vector velo = local->get_velocity( );
			velo.m_z      = 0.f;
			if ( velo.length_2d( ) > 0.f ) {
				c_angle delta_ang = velo.to_angle2( ) - to_wall_ang;
				delta_ang.normalize( );
				if ( std::fabs( delta_ang.m_y ) > 92.5f ) {
					aa_outs[ aa_out_angle ]++;
					break;
				}
			}
		}

		if ( touch_trace.m_fraction < 1.f ) {
			/* already aligned: hold the surf vz */
			const float want_x  = ( cmd->m_buttons & in_forward ) ? 1.f : ( ( cmd->m_buttons & in_back ) ? -1.f : 0.f );
			const float want_y  = ( cmd->m_buttons & in_moveright ) ? 1.f : ( ( cmd->m_buttons & in_moveleft ) ? -1.f : 0.f );
			const bool no_input = want_x == 0.f && want_y == 0.f;
			if ( no_input && !GET_VARIABLE( g_variables.m_auto_align_grim_freelook, bool ) )
				break;
			for ( float strength = k_stability_start; strength > 0.f; strength -= k_stability_step ) {
				const float fmove = no_input ? cos_rot * strength : want_x * strength * k_speed_factor;
				const float smove = no_input ? -sin_rot * strength : want_y * strength * k_speed_factor;
				c_user_cmd fake   = *cmd;
				fake.m_forward_move = fmove;
				fake.m_side_move    = smove;
				g_prediction.begin( local, &fake );
				g_prediction.end( local );
				const float zvel = local->get_velocity( ).m_z;
				g_prediction.restore_entity_to_predicted_frame( frame );
				if ( std::abs( zvel - k_surf_vz ) < k_epsilon ) {
					cmd->m_forward_move = fmove;
					cmd->m_side_move    = smove;
					aa_outs[ aa_out_pin ]++;
					return;
				}
			}
		} else {
			/* not aligned: push into the wall till the hull touches */
			const auto test_move = [ & ]( const float forward, const float side, const int ticks ) {
				c_user_cmd fake     = *cmd;
				fake.m_forward_move = forward;
				fake.m_side_move    = side;
				for ( int t = 0; t < ticks; ++t ) {
					/* donor end( ) steps the tickbase per sim */
					if ( t )
						++local->get_tick_base( );
					g_prediction.begin( local, &fake );
					g_prediction.end( local );
				}
				const c_vector new_origin = local->get_abs_origin( );
				trace_t test_trace;
				ray_t test_ray( new_origin, new_origin + wall_dir, mins, maxs );
				g_interfaces.m_engine_trace->trace_ray( test_ray, mask_playersolid, &filter, &test_trace );
				const bool aligned = test_trace.m_fraction < 1.f;
				g_prediction.restore_entity_to_predicted_frame( frame );
				return aligned;
			};
			for ( int ticks = 1; ticks <= 2; ++ticks ) {
				for ( int i = 1; i <= k_max_test_steps; ++i ) {
					const float fmove = cos_rot * static_cast< float >( i ) * k_speed_factor;
					const float smove = -sin_rot * static_cast< float >( i ) * k_speed_factor;
					if ( test_move( fmove, smove, ticks ) ) {
						cmd->m_forward_move = fmove;
						cmd->m_side_move    = smove;
						aa_outs[ aa_out_touch ]++;
						return;
					}
				}
			}
		}
		break;
	}
}
