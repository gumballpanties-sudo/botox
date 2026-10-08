#include "prediction.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

static void fix_original_stamina( );

void n_prediction::impl_t::update( )
{
	auto nci = g_interfaces.m_engine_client->get_net_channel_info( );
	if ( nci ) {
		const float window = std::max( g_lagcomp.time_limit( ), std::max( g_lagcomp.window_center( ), g_lagcomp.extend( ) ) + 0.2f );

		g_ctx.m_max_allocations =
			std::clamp( g_math.time_to_ticks( std::min( window, g_lagcomp.max_unlag( ) + 0.2f ) ), 3, n_lagcomp::impl_t::max_records );
	} else
		g_ctx.m_max_allocations = 0;
	if ( const bool valid = g_interfaces.m_client_state->m_delta_tick > 0; valid )
		g_interfaces.m_prediction->update( g_interfaces.m_client_state->m_delta_tick, valid, g_interfaces.m_client_state->m_last_command_ack,
		                                   g_interfaces.m_client_state->m_last_outgoing_command + g_interfaces.m_client_state->m_choked_commands );
	if ( g_interfaces.m_prediction->m_commands_predicted == 0 )
		fix_original_stamina( );
	g_prediction.backup_data.m_flags         = g_ctx.m_local->get_flags( );
	g_prediction.backup_data.m_move_type     = g_ctx.m_local->get_move_type( );
	g_prediction.backup_data.m_velocity      = g_ctx.m_local->get_velocity( );
	g_prediction.backup_data.m_fall_velocity = g_ctx.m_local->get_fall_velocity( );
	g_prediction.backup_data.m_stamina       = g_ctx.m_local->get_stamina( );
	g_prediction.backup_data.m_duck_amount   = g_ctx.m_local->get_duck_amount( );
	g_prediction.backup_data.m_origin       = g_ctx.m_local->get_abs_origin( );
	g_prediction.backup_data.m_view_angles   = g_ctx.m_cmd->m_view_point;
}
static float first_world_floor_dz( const float start_feet_z )
{
	static_assert( sizeof( trace_t ) == 84, "engine CGameTrace is 84 bytes" );
	struct touch_list_t {
		void* m_vtable;
		const unsigned char* m_memory;
		int m_allocated, m_grow, m_size;
		const unsigned char* m_elements;
	};
	constexpr int k_stride = 12 + static_cast< int >( sizeof( trace_t ) );
	const auto* list = reinterpret_cast< const touch_list_t* >( g_interfaces.m_move_helper );
	if ( !list || list->m_size <= 0 || list->m_size > 64 || !list->m_memory || list->m_memory != list->m_elements ||
	     list->m_allocated < list->m_size )
		return n_prediction::impl_t::k_no_face;
	for ( int i = 0; i < list->m_size; ++i ) {
		const auto* tr = reinterpret_cast< const trace_t* >( list->m_memory + i * k_stride + 12 );
		if ( tr->m_plane.m_normal.m_z > 0.999f )
			return tr->m_plane.m_distance - start_feet_z;
	}
	return n_prediction::impl_t::k_no_face;
}

void n_prediction::impl_t::begin( c_base_entity* local, c_user_cmd* cmd, const bool real_command, const bool lean )
{
	if ( !g_interfaces.m_move_helper )
		return;

	m_sim_count++;

	/* probes strip attack bits (belt and braces: our post_think has no ItemPostFrame, so begin( )
	   never fires anyway). restored before return. */
	const int stripped_fire = real_command ? 0 : ( cmd->m_buttons & ( in_attack | in_second_attack ) );
	cmd->m_buttons &= ~stripped_fire;

	/* and must not aim: set_local_view_angles writes pl.v_angle (= EyeAngles), restore after probes */
	const c_angle saved_view = local->get_view_angles( ) ? *local->get_view_angles( ) : c_angle{ };

	*local->get_current_command( ) = cmd;
	local->get_last_command( )     = *cmd;
	if ( static bool once = false; !once ) {
		/* operand of a mov: a miss on another build leaves the pointer null, never a read at address 2 */
		const auto operand = []( const unsigned char* site ) -> void* { return site ? *reinterpret_cast< void* const* >( site + 0x2 ) : nullptr; };

		m_prediction_random_seed =
			static_cast< unsigned int* >( operand( g_modules[ CLIENT_DLL ].find_pattern( "8B 0D ? ? ? ? BA ? ? ? ? E8 ? ? ? ? 83 C4 04" ) ) );
		m_prediction_player = static_cast< c_base_entity** >( operand( g_modules[ CLIENT_DLL ].find_pattern( "89 35 ? ? ? ? F3 0F 10 48 20" ) ) );
		once                     = true;
	}
	if ( m_prediction_random_seed )
		*m_prediction_random_seed = cmd->m_random_seed;
	if ( m_prediction_player )
		*m_prediction_player = local;
	m_old_current_time = g_interfaces.m_global_vars_base->m_current_time;
	m_old_frame_time   = g_interfaces.m_global_vars_base->m_frame_time;
	m_old_tick_count   = g_interfaces.m_global_vars_base->m_tick_count;
	const bool old_is_first_prediction = g_interfaces.m_prediction->m_is_first_time_predicted;
	const bool old_in_prediction       = g_interfaces.m_prediction->m_in_prediction;
	g_interfaces.m_global_vars_base->m_current_time =
		static_cast< float >( local->get_tick_base( ) ) * g_interfaces.m_global_vars_base->m_interval_per_tick;
	g_interfaces.m_global_vars_base->m_frame_time =
		g_interfaces.m_prediction->m_engine_paused ? 0.f : g_interfaces.m_global_vars_base->m_interval_per_tick;
	g_interfaces.m_global_vars_base->m_tick_count = local->get_tick_base( );
	g_interfaces.m_prediction->m_is_first_time_predicted = false;
	g_interfaces.m_prediction->m_in_prediction           = true;
	cmd->m_buttons |= local->get_button_forced( ); /* engine never strips m_afButtonDisabled ( kisak prediction.cpp:1059 ) */
	g_interfaces.m_game_movement->start_track_prediction_errors( local );
	g_prediction.update_button_state( local, cmd );
	g_interfaces.m_prediction->check_moving_ground( local, g_interfaces.m_global_vars_base->m_frame_time );
	g_interfaces.m_prediction->set_local_view_angles( cmd->m_view_point );
	if ( !lean && local->physics_run_think( 0 ) )
		local->pre_think( );
	/* RunThink gates on m_nTickBase ( kisak prediction.cpp:969 ) */
	if ( int* next_think_tick = local->get_next_think_tick( );
	     !lean && *next_think_tick > 0 && *next_think_tick <= local->get_tick_base( ) ) {
		*next_think_tick = -1;
		local->set_next_think( 0 );
		local->think( );
	}
	g_interfaces.m_move_helper->set_host( local );
	c_move_data m_move_data{ };
	m_move_data.m_max_speed = real_command ? m_real_max_speed : m_bounds_max_speed;
	const float start_feet_z = local->get_origin( ).m_z;
	g_interfaces.m_prediction->setup_move( local, cmd, g_interfaces.m_move_helper, &m_move_data );
	m_in_begin = true;
	g_interfaces.m_game_movement->process_movement( local, &m_move_data );
	m_in_begin = false;
	m_last_max_speed = m_move_data.m_max_speed;
	m_last_face_dz   = first_world_floor_dz( start_feet_z );
	if ( real_command ) {
		m_real_max_speed = m_last_max_speed;
		m_pin_face_dz    = m_last_face_dz;
	}
	g_interfaces.m_prediction->finish_move( local, cmd, &m_move_data );
	g_interfaces.m_move_helper->process_impacts( );
	if ( !lean )
		local->post_think( );
	g_interfaces.m_prediction->m_in_prediction           = old_in_prediction;
	g_interfaces.m_prediction->m_is_first_time_predicted = old_is_first_prediction;
	cmd->m_buttons |= stripped_fire;

	if ( !real_command && local->get_view_angles( ) )
		*local->get_view_angles( ) = saved_view;
}
void n_prediction::impl_t::end( c_base_entity* local ) const
{
	if ( !g_interfaces.m_move_helper )
		return;
	g_interfaces.m_game_movement->finish_track_prediction_errors( local );
	g_interfaces.m_move_helper->set_host( nullptr );
	g_interfaces.m_global_vars_base->m_current_time = m_old_current_time;
	g_interfaces.m_global_vars_base->m_frame_time   = m_old_frame_time;
	g_interfaces.m_global_vars_base->m_tick_count   = m_old_tick_count;
	*local->get_current_command( ) = nullptr;
	if ( m_prediction_random_seed )
		*m_prediction_random_seed = -1;
	if ( m_prediction_player )
		*m_prediction_player = nullptr;
	g_interfaces.m_game_movement->reset( );
}
constexpr float k_forced_prediction_interval_128 = 1.0f / 128.0f;
constexpr float k_default_prediction_interval    = 1.0f / 64.0f;
void n_prediction::impl_t::update_button_state( c_base_entity* local, c_user_cmd* cmd )
{
	const int buttons         = cmd->m_buttons;
	const int local_buttons   = *local->get_buttons( );
	const int buttons_changed = buttons ^ local_buttons;
	local->get_button_last( )     = local_buttons;
	*local->get_buttons( )        = buttons;
	local->get_button_pressed( )  = buttons_changed & buttons;
	local->get_button_released( ) = buttons_changed & ( ~buttons );
};
extern void botox_dbg_log( const char* fmt, ... );

/* SLOT_ORIGINALDATA never holds a server m_surfaceFriction ( not networked ), so offline it latches a stale value. airborne it
   is exact from START vz: 0.25 while 0 < vz <= 140 else 1.0 ( CategorizePosition between gravity halves ). ground keeps the restored value */
static void fix_original_friction( )
{
	c_base_entity* const local = g_ctx.m_local;
	if ( ( local->get_flags( ) & fl_onground ) || local->get_move_type( ) != e_move_types::move_type_walk )
		return;
	float& friction = local->get_surface_friction( );
	if ( !( friction >= 0.1f && friction <= 1.f ) ) /* datamap miss reads the vtable pointer: never write through it */
		return;
	const float g      = g_convars[ HASH_BT( "sv_gravity" ) ] ? g_convars[ HASH_BT( "sv_gravity" ) ]->get_float( ) : 800.f;
	const float ipt    = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.f;
	const float vz_cat = local->get_velocity( ).m_z + g * ipt * 0.5f;
	const float fixed  = vz_cat > 0.f && vz_cat <= 140.f ? 0.25f : 1.f;
	if ( friction != fixed && GET_VARIABLE( g_variables.m_debug_log, bool ) ) {
		static int s_logged = 0;
		if ( s_logged++ < 64 )
			botox_dbg_log( "FRIC: stale=%.3f fixed=%.2f vz=%.2f", friction, fixed, local->get_velocity( ).m_z );
	}
	friction = fixed;
}

/* m_flStamina ships 14 bits over 0..100 ( +-0.003 ), so offline every START holds it rounded: ~1 coord ulp a tick of air speed,
   the whole tb park window. stamp_sent_stamina sims the sent cmd and the next START takes its end while the net value agrees */
static int s_stam_cmd   = -1;
static float s_stam_end = 0.f;

static void fix_original_stamina( )
{
	c_base_entity* const local = g_ctx.m_local;
	if ( s_stam_cmd < 0 || !local )
		return;
	float& stamina  = local->get_stamina( );
	const bool log  = GET_VARIABLE( g_variables.m_debug_log, bool );
	const int ack   = g_interfaces.m_client_state->m_last_command_ack;
	static int s_logged_cmd = -1, s_logged = 0;
	if ( ack != s_stam_cmd ) {
		if ( log && stamina > 0.f && s_logged_cmd != s_stam_cmd && s_logged < 128 ) {
			s_logged_cmd = s_stam_cmd, ++s_logged;
			botox_dbg_log( "STAM: miss ack=%d want=%d net=%.5f", ack, s_stam_cmd, stamina );
		}
		return;
	}
	if ( stamina == s_stam_end )
		return;
	constexpr float k_step = 100.f / 16383.f;
	const bool agree       = std::fabs( stamina - s_stam_end ) <= k_step;
	if ( log && s_logged_cmd != s_stam_cmd && s_logged < 128 ) {
		s_logged_cmd = s_stam_cmd, ++s_logged;
		botox_dbg_log( "STAM: %s net=%.5f ours=%.5f d=%.2e", agree ? "fixed" : "reject", stamina, s_stam_end, s_stam_end - stamina );
	}
	if ( agree )
		stamina = s_stam_end;
}

void n_prediction::impl_t::stamp_sent_stamina( c_user_cmd* cmd )
{
	c_base_entity* const local = g_ctx.m_local;
	if ( !local || !cmd || !local->is_alive( ) || g_interfaces.m_prediction->m_commands_predicted != 0 ) {
		s_stam_cmd = -1;
		return;
	}
	restore_entity_to_predicted_frame( -1 );
	begin( local, cmd );
	end( local );
	s_stam_cmd = cmd->m_command_number;
	s_stam_end = local->get_stamina( );
}

/* engine RestoreEntityToPredictedFrame by pattern. frame -1 = SLOT_ORIGINALDATA
   (clean pre-prediction state): never guard frame < 0, that no-ops every restore. */
void n_prediction::impl_t::restore_entity_to_predicted_frame( int frame )
{
	if ( frame < -1 || !g_ctx.m_local )
		return;

	m_restore_count++;

	using restore_fn_t = void( __stdcall* )( int slot, int frame );
	static auto engine_restore =
		reinterpret_cast< restore_fn_t >( g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 8B 4D ? 56 E8 ? ? ? ? 8B 75" ) );

	if ( static bool logged = false; !logged ) {
		logged = true;
		botox_dbg_log( "restore: engine fn @ %p (null = pattern MISS, using fallback)", engine_restore );
	}

	if ( engine_restore )
		engine_restore( 0, frame );
	else {
		g_ctx.m_local->restore_data( "RestoreEntityToPredictedFrame", frame, 2  );
		g_ctx.m_local->on_post_restore_data( );
	}
	if ( frame == -1 ) {
		fix_original_friction( );
		fix_original_stamina( );
	}
}

static constexpr int k_backup_slots = 150, k_snapshot_margin = 16;

static int intermediate_count_offset( )
{
	static const int offset = [ ] {
		const unsigned char* fn = g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 E4 F8 83 EC 40 56 8B 75 0C" );
		if ( !fn || std::memcmp( fn + 0x36, "\x69\xC2\x96\x00\x00\x00", 6 ) != 0 || fn[ 0x45 ] != 0x89 || fn[ 0x46 ] != 0xB7 )
			return -1;
		const int off = *reinterpret_cast< const int* >( fn + 0x47 );
		return off > 0x100 && off < 0x4000 ? off : -1;
	}( );
	return offset;
}

int n_prediction::impl_t::snapshot_slots( ) const
{
	if ( !g_ctx.m_local || intermediate_count_offset( ) < 0 )
		return 0;
	return std::clamp( k_backup_slots - ( g_interfaces.m_prediction->m_commands_predicted + k_snapshot_margin ), 0, 64 );
}

bool n_prediction::impl_t::snapshot_save( const int index )
{
	if ( index < 0 || index >= snapshot_slots( ) )
		return false;
	int& count      = *reinterpret_cast< int* >( reinterpret_cast< std::uintptr_t >( g_ctx.m_local ) + intermediate_count_offset( ) );
	const int keep  = count;
	const bool done = g_ctx.m_local->save_data( "botox snapshot", k_backup_slots - 1 - index, 2  );
	count           = keep;
	return done;
}

void n_prediction::impl_t::snapshot_load( const int index )
{
	m_restore_count++;
	g_ctx.m_local->restore_data( "botox snapshot", k_backup_slots - 1 - index, 2  );
	g_ctx.m_local->on_post_restore_data( );
}

/* pin target ( -g*dt/2 ) on the engine's ipt. never the 128 checkbox: prediction sims step engine ipt. */
float n_prediction::impl_t::get_engine_target_predict_z_velocity( ) const
{
	const float sv_gravity = g_convars[ HASH_BT( "sv_gravity" ) ] ? g_convars[ HASH_BT( "sv_gravity" ) ]->get_float( ) : 800.f;
	const float interval   = g_interfaces.m_global_vars_base && g_interfaces.m_global_vars_base->m_interval_per_tick > 0.f
	                           ? g_interfaces.m_global_vars_base->m_interval_per_tick
	                           : k_default_prediction_interval;
	return -( sv_gravity * interval * 0.5f );
}

bool n_prediction::impl_t::should_force_128_tick( ) const
{
	return GET_VARIABLE( g_variables.m_tick_fix_128, bool );
}

float n_prediction::impl_t::get_interval_per_tick( ) const
{
	if ( should_force_128_tick( ) )
		return k_forced_prediction_interval_128;

	if ( g_interfaces.m_global_vars_base && g_interfaces.m_global_vars_base->m_interval_per_tick > 0.0f )
		return g_interfaces.m_global_vars_base->m_interval_per_tick;

	return k_default_prediction_interval;
}

/* engine ipt, never the 128 checkbox: callers compare sim/engine velocities, and sims step engine ipt. */
bool n_prediction::impl_t::is_target_predict_z_velocity( float velocity, float epsilon ) const
{
	return std::fabs( velocity - get_engine_target_predict_z_velocity( ) ) <= epsilon;
}
