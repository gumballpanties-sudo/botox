#include "screen.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "resolution_spoof.h"

struct motion_blur_history_t {
	motion_blur_history_t( )
	{
		this->m_last_time_update                = 0.0f;
		this->m_previous_pitch                  = 0.0f;
		this->m_previous_yaw                    = 0.0f;
		this->m_previous_position               = c_vector( 0.0f, 0.0f, 0.0f );
		this->m_previous_frame_basis_vectors    = { };
		this->m_no_rotational_motion_blur_until = 0.0f;
	}

	float m_last_time_update;
	float m_previous_pitch;
	float m_previous_yaw;
	c_vector m_previous_position;
	matrix3x4_t m_previous_frame_basis_vectors;
	float m_no_rotational_motion_blur_until;
};

struct motion_blur_data_t {
	float m_values[ 4 ]          = { 0.f, 0.f, 0.f, 0.f };
	float m_viewport_values[ 4 ] = { 0.f, 0.f, 1.f, 1.f };
} motion_blur_data{ };

static float scale_fov_by_width_ratio( float fov_degrees, float ratio )
{
	float half_angle_radians = fov_degrees * ( 0.5f * 3.14159265358979323846f / 180.0f );
	float t = tanf( half_angle_radians ) * ratio;
	return atanf( t ) * ( 2.0f * 180.0f / 3.14159265358979323846f );
}

static float unscale_fov_by_width_ratio( float fov_degrees, float ratio )
{
	if ( ratio <= 0.0f )
		return fov_degrees;

	float half_angle_radians = fov_degrees * ( 0.5f * 3.14159265358979323846f / 180.0f );
	float t = tanf( half_angle_radians ) / ratio;
	return atanf( t ) * ( 2.0f * 180.0f / 3.14159265358979323846f );
}

bool n_screen::impl_t::on_draw_view_models( c_view_setup& setup )
{
	if ( GET_VARIABLE( g_variables.m_viewmodel_fov_enable, bool ) ) {
		float aspect = setup.m_aspect_ratio;
		if ( aspect <= 0.0f )
			aspect = ( setup.m_height != 0 )
				? static_cast< float >( setup.m_width ) / static_cast< float >( setup.m_height )
				: 1.0f;

		const float ratio = aspect * 0.75f;
		float wanted      = GET_VARIABLE( g_variables.m_viewmodel_fov, float );

		const auto local = g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) );
		if ( local && local->is_scoped( ) ) {
			static c_cconvar* base_fov = nullptr;
			if ( !base_fov )
				base_fov = g_convars[ HASH_BT( "viewmodel_fov" ) ];

			const float base = base_fov ? base_fov->get_float( ) : 54.f;
			wanted -= base - unscale_fov_by_width_ratio( setup.m_fov_viewmodel, ratio );
		}

		setup.m_fov_viewmodel = std::clamp( scale_fov_by_width_ratio( wanted, ratio ), 1.0f, 179.0f );
	}

	if ( !GET_VARIABLE( g_variables.m_motion_blur, bool ) )
		return false;

	float x = static_cast< float >( setup.m_x );
	float y = static_cast< float >( setup.m_y );
	float w = static_cast< float >( setup.m_width );
	float h = static_cast< float >( setup.m_height );

	// never write mat_resolveFullFrameDepth here: motion blur needs no depth, depth_source needs it
	// at 1, and each change triggers ReloadMaterials("particle").

	bool blur_forward        = GET_VARIABLE( g_variables.m_motion_blur_forward, bool );
	float rotation_intensity = GET_VARIABLE( g_variables.m_motion_blur_rotation_intensity, float );
	float blur_strength      = GET_VARIABLE( g_variables.m_motion_blur_strength, float );
	float falling_min        = GET_VARIABLE( g_variables.m_motion_blur_falling_min, float );
	float falling_max        = GET_VARIABLE( g_variables.m_motion_blur_falling_max, float );
	float falling_intensity  = GET_VARIABLE( g_variables.m_motion_blur_falling_intensity, float );
	float roll_intensity     = GET_VARIABLE( g_variables.m_motion_blur_roll_intensity, float );

	/* falling max must stay above min, else div by zero below */
	if ( falling_max <= falling_min )
		falling_max = falling_min + 0.01f;

	static motion_blur_history_t history = { };

	matrix3x4_t current_basis_vectors{ };
	g_math.angle_matrix( setup.m_angles, current_basis_vectors );

	c_vector current_side_vec( current_basis_vectors[ 0 ][ 1 ], current_basis_vectors[ 1 ][ 1 ], current_basis_vectors[ 2 ][ 1 ] );
	c_vector current_forward_vec( current_basis_vectors[ 0 ][ 0 ], current_basis_vectors[ 1 ][ 0 ], current_basis_vectors[ 2 ][ 0 ] );
	c_angle normalized_angles = setup.m_angles.normalize( );
	c_vector position_change  = history.m_previous_position - setup.m_origin;

	float time_elapsed  = g_interfaces.m_global_vars_base->m_real_time - history.m_last_time_update;
	float travelled     = position_change.length( );

	if ( ( travelled > 30.0f && time_elapsed >= 0.5f ) || time_elapsed > ( 1.0f / 15.0f ) ) {
		motion_blur_data.m_values[ 0 ] = 0.0f;
		motion_blur_data.m_values[ 1 ] = 0.0f;
		motion_blur_data.m_values[ 2 ] = 0.0f;
		motion_blur_data.m_values[ 3 ] = 0.0f;
	} else if ( travelled > 50.0f ) {
		history.m_no_rotational_motion_blur_until = g_interfaces.m_global_vars_base->m_real_time + 1.0f;
	} else {
		float horizontal_fov = setup.m_fov;
		float vertical_fov   = ( setup.m_aspect_ratio <= 0.0f ) ? ( setup.m_fov ) : ( setup.m_fov / setup.m_aspect_ratio );

		float view_dot_motion = current_forward_vec.dot_product( position_change );
		if ( blur_forward )
			motion_blur_data.m_values[ 2 ] = view_dot_motion;
		else
			motion_blur_data.m_values[ 2 ] = view_dot_motion * fabs( current_forward_vec[ 2 ] );

		float yaw_diff_original = history.m_previous_yaw - normalized_angles.m_y;
		if ( ( ( history.m_previous_yaw - normalized_angles.m_y > 180.0f ) || ( history.m_previous_yaw - normalized_angles.m_y < -180.0f ) ) &&
		     ( ( history.m_previous_yaw + normalized_angles.m_y > -180.0f ) && ( history.m_previous_yaw + normalized_angles.m_y < 180.0f ) ) )
			yaw_diff_original = history.m_previous_yaw + normalized_angles.m_y;

		float side_dot_motion   = current_side_vec.dot_product( position_change );
		float yaw_diff_adjusted = yaw_diff_original + ( side_dot_motion / 3.0f );

		if ( yaw_diff_original < 0.0f )
			yaw_diff_adjusted = std::clamp( yaw_diff_adjusted, yaw_diff_original, 0.0f );
		else
			yaw_diff_adjusted = std::clamp( yaw_diff_adjusted, 0.0f, yaw_diff_original );

		float undampened_yaw           = yaw_diff_adjusted / horizontal_fov;
		motion_blur_data.m_values[ 0 ] = undampened_yaw * ( 1.0f - ( fabs( normalized_angles.m_x ) / 90.0f ) );

		float pitch_compensate_mask = 1.0f - ( ( 1.0f - fabs( current_forward_vec[ 2 ] ) ) * ( 1.0f - fabs( current_forward_vec[ 2 ] ) ) );
		float pitch_diff_original   = history.m_previous_pitch - normalized_angles.m_x;
		float pitch_diff_adjusted   = pitch_diff_original;

		if ( normalized_angles.m_x > 0.0f )
			pitch_diff_adjusted = pitch_diff_original - ( ( view_dot_motion / 2.0f ) * pitch_compensate_mask );
		else
			pitch_diff_adjusted = pitch_diff_original + ( ( view_dot_motion / 2.0f ) * pitch_compensate_mask );

		if ( pitch_diff_original < 0.0f )
			pitch_diff_adjusted = std::clamp( pitch_diff_adjusted, pitch_diff_original, 0.0f );
		else
			pitch_diff_adjusted = std::clamp( pitch_diff_adjusted, 0.0f, pitch_diff_original );

		motion_blur_data.m_values[ 1 ] = pitch_diff_adjusted / vertical_fov;

		motion_blur_data.m_values[ 3 ] = undampened_yaw;
		motion_blur_data.m_values[ 3 ] *= ( std::fabs( normalized_angles.m_x ) / 90.0f ) * ( std::fabs( normalized_angles.m_x ) / 90.0f ) *
		                                  ( std::fabs( normalized_angles.m_x ) / 90.0f );

		if ( ( g_interfaces.m_global_vars_base->m_real_time - history.m_last_time_update ) > 0.0f )
			motion_blur_data.m_values[ 2 ] /= (g_interfaces.m_global_vars_base->m_real_time - history.m_last_time_update ) * 30.0f;
		else
			motion_blur_data.m_values[ 2 ] = 0.0f;

		motion_blur_data.m_values[ 2 ] =
			std::clamp( ( std::fabs( motion_blur_data.m_values[ 2 ] ) - falling_min ) / ( falling_max - falling_min ), 0.0f, 1.0f ) *
			( motion_blur_data.m_values[ 2 ] >= 0.0f ? 1.0f : -1.0f );
		motion_blur_data.m_values[ 2 ] /= 30.0f;

		motion_blur_data.m_values[ 0 ] *= rotation_intensity * blur_strength;
		motion_blur_data.m_values[ 1 ] *= rotation_intensity * blur_strength;
		motion_blur_data.m_values[ 2 ] *= falling_intensity * blur_strength;
		motion_blur_data.m_values[ 3 ] *= roll_intensity * blur_strength;
	}

	if ( g_interfaces.m_global_vars_base->m_real_time < history.m_no_rotational_motion_blur_until ) {
		motion_blur_data.m_values[ 0 ] = 0.0f;
		motion_blur_data.m_values[ 1 ] = 0.0f;
		motion_blur_data.m_values[ 3 ] = 0.0f;
	} else
		history.m_no_rotational_motion_blur_until = 0.0f;

	history.m_previous_position            = setup.m_origin;
	history.m_previous_frame_basis_vectors = current_basis_vectors;
	history.m_previous_pitch               = normalized_angles.m_x;
	history.m_previous_yaw                 = normalized_angles.m_y;
	history.m_last_time_update             = g_interfaces.m_global_vars_base->m_real_time;

	if ( true ) {
		c_texture* full_frame = g_interfaces.m_material_system->find_texture( "_rt_FullFrameFB", TEXTURE_GROUP_RENDER_TARGET );
		float src_width       = static_cast< float >( full_frame->get_actual_width( ) );
		float src_height      = static_cast< float >( full_frame->get_actual_height( ) );
		int offset{ };

		offset                                  = ( x > 0 ) ? 1 : 0;
		motion_blur_data.m_viewport_values[ 0 ] = float( x + offset ) / ( src_width - 1 );

		offset                                  = ( x < ( src_width - 1 ) ) ? -1 : 0;
		motion_blur_data.m_viewport_values[ 3 ] = float( x + w + offset ) / ( src_width - 1 );

		offset                                  = ( y > 0 ) ? 1 : 0;
		motion_blur_data.m_viewport_values[ 1 ] = float( y + offset ) / ( src_height - 1 );

		offset                                  = ( y < ( src_height - 1 ) ) ? -1 : 0;
		motion_blur_data.m_viewport_values[ 2 ] = float( y + h + offset ) / ( src_height - 1 );
	}

	bool performed_motion_blur = false;
	if ( true ) {
		c_material_render_context* render_context = g_interfaces.m_material_system->get_render_context( );
		c_texture* full_frame                     = g_interfaces.m_material_system->find_texture( "_rt_FullFrameFB", TEXTURE_GROUP_RENDER_TARGET );

		int src_width  = full_frame->get_actual_width( );
		int src_height = full_frame->get_actual_height( );
		int vport_width, vport_height, dummy;
		render_context->get_view_port( &dummy, &dummy, &vport_width, &vport_height );

		update_screen_effect_texture( 0, int( x ), int( y ), int( w ), int( h ), false );

		c_material* mat_motion_blur = g_interfaces.m_material_system->find_material( "dev/motion_blur", TEXTURE_GROUP_OTHER, true );

		bool found                 = false;
		c_material_var* blur_param = mat_motion_blur->find_var( "$MotionBlurInternal", &found, false );
		if ( !found )
			return false;

		blur_param->set_vector_component( motion_blur_data.m_values[ 0 ], 0 );
		blur_param->set_vector_component( motion_blur_data.m_values[ 1 ], 1 );
		blur_param->set_vector_component( motion_blur_data.m_values[ 2 ], 2 );
		blur_param->set_vector_component( motion_blur_data.m_values[ 3 ], 3 );

		c_material_var* viewport_blur_param = mat_motion_blur->find_var( "$MotionBlurViewportInternal", &found, false );
		if ( !found )
			return false;

		viewport_blur_param->set_vector_component( motion_blur_data.m_viewport_values[ 0 ], 0 );
		viewport_blur_param->set_vector_component( motion_blur_data.m_viewport_values[ 1 ], 1 );
		viewport_blur_param->set_vector_component( motion_blur_data.m_viewport_values[ 2 ], 2 );
		viewport_blur_param->set_vector_component( motion_blur_data.m_viewport_values[ 3 ], 3 );

		if ( mat_motion_blur && src_width > 0 && src_height > 0 ) {
			render_context->draw_screen_space_rectangle( mat_motion_blur, 0, 0, vport_width, vport_height, x, y, x + w - 1, y + h - 1, src_width,
			                                             src_height );
			performed_motion_blur = true;
		}
	}

	return performed_motion_blur;
}

void n_screen::impl_t::update_screen_effect_texture( int texture_index, int x, int y, int w, int h, bool dest_fullscreen, rect_t* actual_rect )
{
	rect_t src_rect{ x, y, w, h };

	static auto get_full_frame_frame_buffer_texture = []( int texture_index ) {
		static auto get_full_frame_frame_buffer_texture_fn =
			reinterpret_cast< c_texture*( __thiscall* )( int ) >( g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 81 EC ? ? ? ? 56 8B F1 83 3C" ) );

		return get_full_frame_frame_buffer_texture_fn ? get_full_frame_frame_buffer_texture_fn( texture_index ) : nullptr;
	};

	c_material_render_context* render_context = g_interfaces.m_material_system->get_render_context( );
	c_texture* texture                        = get_full_frame_frame_buffer_texture( texture_index );
	if ( !render_context || !texture )
		return;

	int src_width, src_height;
	render_context->get_render_target_dimensions( &src_width, &src_height );
	int dest_width  = texture->get_actual_width( );
	int dest_height = texture->get_actual_height( );

	rect_t dest_rect = src_rect;
	if ( !dest_fullscreen && ( src_width > dest_width || src_height > dest_height ) ) {
		int scale_x        = dest_width / src_width;
		int scale_y        = dest_height / src_height;
		dest_rect.m_x      = src_rect.m_x * scale_x;
		dest_rect.m_y      = src_rect.m_y * scale_y;
		dest_rect.m_width  = src_rect.m_width * scale_x;
		dest_rect.m_height = src_rect.m_height * scale_y;
		dest_rect.m_x      = std::clamp( dest_rect.m_x, 0, dest_width );
		dest_rect.m_y      = std::clamp( dest_rect.m_y, 0, dest_height );
		dest_rect.m_width  = std::clamp( dest_rect.m_width, 0, dest_width - dest_rect.m_x );
		dest_rect.m_height = std::clamp( dest_rect.m_height, 0, dest_height - dest_rect.m_y );
	}

	render_context->copy_render_target_to_texture_ex( texture, 0, &src_rect, dest_fullscreen ? NULL : &dest_rect );
	render_context->set_frame_buffer_copy_texture( texture, texture_index );

	if ( actual_rect ) {
		actual_rect->m_x      = dest_rect.m_x;
		actual_rect->m_y      = dest_rect.m_y;
		actual_rect->m_width  = dest_rect.m_width;
		actual_rect->m_height = dest_rect.m_height;
	}
}

void n_screen::impl_t::on_override_view( c_view_setup* setup )
{
	this->viewmodel_convars( );

	if ( !setup )
		return;

	if ( GET_VARIABLE( g_variables.m_world_fov_enable, bool ) ) {
		const auto local = g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) );

		if ( local && local->is_alive( ) && !local->is_scoped( ) )
			setup->m_fov += GET_VARIABLE( g_variables.m_world_fov, float );
	}

	if ( GET_VARIABLE( g_variables.m_remove_visual_recoil, bool ) ) {
		const auto local = g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) );

		if ( local && local->is_alive( ) ) {
			static c_cconvar* recoil_scale = g_convars[ HASH_BT( "weapon_recoil_scale" ) ];
			static c_cconvar* tracking     = g_convars[ HASH_BT( "view_recoil_tracking" ) ];

			const float scale = ( recoil_scale ? recoil_scale->get_float( ) : 2.f ) * ( tracking ? tracking->get_float( ) : 0.45f );

			setup->m_angles -= local->get_view_punch( ) + local->get_punch( ) * scale;
		}
	}

	this->viewmodel_offset( );
}

void n_screen::impl_t::viewmodel_offset( )
{
	if ( !GET_VARIABLE( g_variables.m_viewmodel_offset_enable, bool ) )
		return;

	static c_cconvar *game_x = nullptr, *game_y = nullptr, *game_z = nullptr;
	if ( !game_x ) {
		game_x = g_convars[ HASH_BT( "viewmodel_offset_x" ) ];
		game_y = g_convars[ HASH_BT( "viewmodel_offset_y" ) ];
		game_z = g_convars[ HASH_BT( "viewmodel_offset_z" ) ];
	}

	const float x    = GET_VARIABLE( g_variables.m_viewmodel_x, float ) - ( game_x ? game_x->get_float( ) : 0.f );
	const float y    = GET_VARIABLE( g_variables.m_viewmodel_y, float ) - ( game_y ? game_y->get_float( ) : 0.f );
	const float z    = GET_VARIABLE( g_variables.m_viewmodel_z, float ) - ( game_z ? game_z->get_float( ) : 0.f );
	const float roll = GET_VARIABLE( g_variables.m_viewmodel_roll, float );

	if ( x == 0.f && y == 0.f && z == 0.f && roll == 0.f )
		return;

	if ( !g_interfaces.m_engine_client->is_connected_safe( ) )
		return;

	const auto local = g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) );
	if ( !local || !local->is_alive( ) )
		return;

	if ( local->is_scoped( ) )
		return;

	const auto viewmodel = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_view_model_handle( ) );
	if ( !viewmodel )
		return;

	c_vector forward{ }, right{ }, up{ };
	g_math.angle_vectors( viewmodel->get_abs_angles( ), &forward, &right, &up );

	viewmodel->set_abs_origin( viewmodel->get_abs_origin( ) + forward * y + right * x + up * z );

	if ( roll != 0.f ) {
		c_angle angles = viewmodel->get_abs_angles( );
		angles.m_z += roll;
		viewmodel->set_abs_angles( angles );
	}
}

void n_screen::impl_t::on_frame_stage_notify( const int stage )
{
	if ( stage != e_client_frame_stage::render_start )
		return;

	this->third_person( );
	this->aspect_ratio( );

	// convar side only, the crunch is on the d3d thread
	g_resolution_spoof.on_frame_stage_notify( );
}

void n_screen::impl_t::third_person( const bool restore )
{
	if ( !g_interfaces.m_input )
		return;

	static bool forced = false;

	const bool wanted = !restore && GET_VARIABLE( g_variables.m_third_person, bool ) &&
	                    g_input.check_input( &GET_VARIABLE( g_variables.m_third_person_key, key_bind_t ) ) && g_ctx.m_local &&
	                    g_ctx.m_local->is_alive( );

	if ( !wanted ) {
		if ( forced ) {
			g_interfaces.m_input->m_camera_in_third_person = false;
			forced                                         = false;
		}

		return;
	}

	c_angle view_angles{ };
	g_interfaces.m_engine_client->get_view_angles( view_angles );

	float distance = GET_VARIABLE( g_variables.m_third_person_distance, float );

	if ( GET_VARIABLE( g_variables.m_third_person_collision, bool ) ) {
		constexpr float hull = 9.f;

		c_vector forward{ };
		g_math.angle_vectors( c_angle( view_angles.m_x, view_angles.m_y, 0.f ), &forward );

		const c_vector eye = g_ctx.m_local->get_abs_origin( ) + g_ctx.m_local->get_view_offset( );

		trace_t trace{ };
		c_trace_filter filter( g_ctx.m_local );

		ray_t ray( eye, eye - forward * distance, c_vector( -hull, -hull, -hull ), c_vector( hull, hull, hull ) );
		g_interfaces.m_engine_trace->trace_ray( ray, mask_solid, &filter, &trace );

		distance *= trace.m_fraction;
	}

	g_interfaces.m_input->m_camera_in_third_person = true;
	g_interfaces.m_input->m_camera_offset          = c_vector( view_angles.m_x, view_angles.m_y, distance );

	forced = true;
}

void n_screen::impl_t::aspect_ratio( const bool restore )
{
	static c_cconvar* aspect = nullptr;
	if ( !aspect && !( aspect = g_convars[ HASH_BT( "r_aspectratio" ) ] ) )
		return;

	const bool enabled = !restore && GET_VARIABLE( g_variables.m_aspect_ratio_enable, bool );

	const float wanted = enabled ? GET_VARIABLE( g_variables.m_aspect_ratio, float ) : 0.f;

	if ( aspect->get_float( ) != wanted )
		aspect->set_value( wanted );
}

void n_screen::impl_t::on_release( )
{
	this->viewmodel_convars( true );
	this->third_person( true );
	this->aspect_ratio( true );

	g_resolution_spoof.on_release( );
}

void n_screen::impl_t::viewmodel_convars( const bool restore )
{
	struct managed_convar_t {
		c_cconvar* m_convar = nullptr;
		float m_default     = 0.f;
	};

	static const auto touch = []( managed_convar_t& managed, const unsigned int hash, const bool enabled, const float value ) {
		if ( !managed.m_convar ) {
			managed.m_convar = g_convars[ hash ];
			if ( !managed.m_convar )
				return;

			managed.m_default = managed.m_convar->get_float( );

			managed.m_convar->unlock_bounds( );
		}

		const float wanted = enabled ? value : managed.m_default;

		if ( managed.m_convar->get_float( ) == wanted )
			return;

		managed.m_convar->set_value( wanted );

		if ( managed.m_convar->get_float( ) != wanted )
			managed.m_convar->force_value( wanted );
	};

	static managed_convar_t sway{ }, bob_cycle{ }, bob_vertical{ }, bob_lateral{ }, bob_lower{ }, shift_left{ }, shift_right{ };

	const bool sway_on = !restore && GET_VARIABLE( g_variables.m_weapon_sway, bool );
	touch( sway, HASH_BT( "cl_wpn_sway_scale" ), sway_on, GET_VARIABLE( g_variables.m_weapon_sway_scale, float ) );

	const bool bob_on = !restore && GET_VARIABLE( g_variables.m_viewmodel_bob, bool );

	// cycle 0 = divide by zero in CalcViewModelBobHelper (no guard), gives a NaN viewmodel origin.
	// floored; zero amounts already give no bob.
	const float cycle = GET_VARIABLE( g_variables.m_viewmodel_bob_cycle, float );
	touch( bob_cycle, HASH_BT( "cl_bobcycle" ), bob_on, cycle < 0.01f ? 0.01f : cycle );
	touch( bob_vertical, HASH_BT( "cl_bobamt_vert" ), bob_on, GET_VARIABLE( g_variables.m_viewmodel_bob_vertical, float ) );
	touch( bob_lateral, HASH_BT( "cl_bobamt_lat" ), bob_on, GET_VARIABLE( g_variables.m_viewmodel_bob_lateral, float ) );
	touch( bob_lower, HASH_BT( "cl_bob_lower_amt" ), bob_on, GET_VARIABLE( g_variables.m_viewmodel_bob_lower, float ) );
	touch( shift_left, HASH_BT( "cl_viewmodel_shift_left_amt" ), bob_on, GET_VARIABLE( g_variables.m_viewmodel_shift_left, float ) );
	touch( shift_right, HASH_BT( "cl_viewmodel_shift_right_amt" ), bob_on, GET_VARIABLE( g_variables.m_viewmodel_shift_right, float ) );
}
