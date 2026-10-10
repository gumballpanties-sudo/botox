#pragma once
#include "movement.h"
#include "tick_scale.h"
#include "../prediction/prediction.h"
#include "../../game/sdk/includes/includes.h"
#include "../../game/sdk/classes/c_physics_surface_props.h"
#include "../../globals/includes/includes.h"
#include "../../globals/interfaces/interfaces.h"
#include <algorithm>
#include <cmath>

void CorrectMovement( c_user_cmd* cmd, c_angle wish_angle, c_angle old_angles );
void start_movement_fix( c_user_cmd* cmd );
void end_movement_fix( c_user_cmd* cmd );

extern bool wall_detected;
extern bool should_align;

extern int aa_gate_skips;
extern int aa_reach_skips;
extern int aa_fail_skips;
enum e_aa_out { aa_out_near, aa_out_face, aa_out_away, aa_out_angle, aa_out_pin, aa_out_touch, aa_out_miss, aa_out_fine, aa_out_park, aa_out_unstick, aa_out_keep, aa_out_tb, aa_out_stomp, aa_out_park_off, aa_out_slant, aa_out_lip, aa_out_count };
extern int aa_outs[ aa_out_count ];

extern bool g_air_stuck_noclip_active;
extern bool g_air_stuck_owns_cmd;
extern bool g_air_stuck_holding;
extern bool g_air_stuck_owned_prev_tick;
extern c_angle g_air_stuck_authored_view;
extern bool g_air_stuck_authored_view_valid;
extern bool g_air_stuck_stamp_valid;
extern float g_air_stuck_stamp_yaw;
extern float g_air_stuck_stamp_forward;
extern float g_air_stuck_stamp_side;
extern int g_air_stuck_stamp_buttons;
extern bool g_air_stuck_stamp_force;
extern float g_air_stuck_stamp_pitch;

void botox_dbg_log( const char* fmt, ... );
void play_trick_sound( const int sound_index, const float volume = 1.f, const char* custom_path = nullptr );
bool botox_is_ladder_trace( const trace_t& tr );
void on_healthshot( int trigger );

void RenderPoints( std::vector< points_check_t >& points, const c_vector& playerPos, const std::string& currentMap );
void render_point_settings_window( );
void pf_render_dots( const std::vector< c_vector >& surf, const std::vector< c_vector >& bounce, const std::vector< c_vector >& tb, const std::vector< c_vector >& pj );

constexpr float ALIGN_OFFSET      = 15.97803f;

inline float GetWallSupportDistance( c_base_entity* local, const c_vector& wallNormal )
{
	if ( !local )
		return ALIGN_OFFSET;

	auto* collideable = local->get_collideable( );
	if ( !collideable )
		return ALIGN_OFFSET;

	const c_vector mins = collideable->get_obb_mins( );
	const c_vector maxs = collideable->get_obb_maxs( );
	const c_vector half_extents( std::max( std::fabsf( mins.m_x ), std::fabsf( maxs.m_x ) ),
	                             std::max( std::fabsf( mins.m_y ), std::fabsf( maxs.m_y ) ),
	                             std::max( std::fabsf( mins.m_z ), std::fabsf( maxs.m_z ) ) );

	return std::fabsf( wallNormal.m_x ) * half_extents.m_x + std::fabsf( wallNormal.m_y ) * half_extents.m_y +
	       std::fabsf( wallNormal.m_z ) * half_extents.m_z;
}

[[nodiscard]] __forceinline bool is_feature_active( const bool enabled, key_bind_t& key )
{
	return enabled && g_input.check_input( &key );
}

[[nodiscard]] __forceinline bool tb_key( )
{
	return is_feature_active( GET_VARIABLE( g_variables.m_texture_bug, bool ), GET_VARIABLE( g_variables.m_texture_bug_key, key_bind_t ) );
}

[[nodiscard]] __forceinline bool as_key( )
{
	return is_feature_active( GET_VARIABLE( g_variables.m_air_stuck, bool ), GET_VARIABLE( g_variables.m_air_stuck_key, key_bind_t ) );
}

__forceinline void predict_cmd( c_user_cmd* cmd )
{
	g_prediction.begin( g_ctx.m_local, cmd );
	g_prediction.end( g_ctx.m_local );
}

__forceinline void restore_prediction_frame( )
{
	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
}

[[nodiscard]] __forceinline float target_bugged_z_velocity( )
{
	static auto sv_gravity = g_convars[ HASH_BT( "sv_gravity" ) ];
	return -( sv_gravity->get_float( ) * n_tick::interval( ) * 0.5f );
}

[[nodiscard]] __forceinline int merge_movement_buttons( const int live_buttons, const int stuck_buttons )
{
	constexpr int movement_mask = in_moveright | in_moveleft | in_back | in_forward | in_duck | in_jump;
	return ( stuck_buttons & movement_mask ) | ( live_buttons & ~movement_mask );
}

__forceinline void set_move_toward_yaw( c_user_cmd* cmd, const float target_yaw, const float forward )
{
	const float rotation = deg2rad( target_yaw - cmd->m_view_point.m_y );
	cmd->m_forward_move  = std::cosf( rotation ) * forward;
	cmd->m_side_move     = -std::sinf( rotation ) * forward;
}

[[nodiscard]] inline float wall_support_extent( const c_vector& wall_normal )
{
	const c_vector mins = g_ctx.m_local->get_collideable( )->get_obb_mins( );
	const c_vector maxs = g_ctx.m_local->get_collideable( )->get_obb_maxs( );

	const c_vector half_extents( std::max( std::fabsf( mins.m_x ), std::fabsf( maxs.m_x ) ),
	                             std::max( std::fabsf( mins.m_y ), std::fabsf( maxs.m_y ) ),
	                             std::max( std::fabsf( mins.m_z ), std::fabsf( maxs.m_z ) ) );

	return std::fabsf( wall_normal.m_x ) * half_extents.m_x + std::fabsf( wall_normal.m_y ) * half_extents.m_y +
	       std::fabsf( wall_normal.m_z ) * half_extents.m_z;
}

constexpr float M_PIN = 3.14159265358979323846f;

inline float get_half_gravity_per_tick( )
{
	const float sv_gravity = g_convars[ HASH_BT( "sv_gravity" ) ] ? g_convars[ HASH_BT( "sv_gravity" ) ]->get_float( ) : 800.f;
	return -( sv_gravity * n_tick::interval( ) * 0.5f );
}

inline float eb_indicator_tick_scale( ) { return n_tick::scale( ); }

inline bool is_pixelsurf_velocity( float z, float tolerance = 0.01f )
{
	return std::abs( z - get_half_gravity_per_tick( ) ) < tolerance;
}

inline bool steering_away_from_wall( const c_user_cmd* cmd, const c_vector& wall_normal, float forward_move, float side_move, float min_dot )
{
	if ( std::fabsf( forward_move ) < 0.01f && std::fabsf( side_move ) < 0.01f )
		return false;

	c_vector forward{ }, right{ };
	g_math.angle_vectors( c_angle( 0.f, cmd->m_view_point.m_y, 0.f ), &forward, &right, nullptr );

	c_vector wish = forward * forward_move + right * side_move;
	wish.m_z      = 0.f;
	const float wish_length = wish.length_2d( );
	if ( wish_length < 1.f )
		return false;
	wish = wish * ( 1.f / wish_length );

	c_vector normal( wall_normal.m_x, wall_normal.m_y, 0.f );
	const float normal_length = normal.length_2d( );
	if ( normal_length < 0.001f )
		return false;
	normal = normal * ( 1.f / normal_length );

	return ( wish.m_x * normal.m_x + wish.m_y * normal.m_y ) > min_dot;
}

