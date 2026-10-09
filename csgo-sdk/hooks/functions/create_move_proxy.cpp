#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

#include "../../hacks/aimbot/aimbot.h"
#include "../../hacks/lagcomp/lagcomp.h"
#include "../../hacks/misc/misc.h"
#include "../../hacks/misc/chat_extras.h"
#include "../../hacks/misc/scaleform/scaleform.h"
#include "../../hacks/chud_hud/chud_hud.h"
#include "../../hacks/movement/edgebug.h"
#include "../../hacks/movement/edge_skip.h"
#include "../../hacks/movement/movement.h"
#include "../../hacks/movement/movement_recorder.h"
#include "../../hacks/movement/tick_scale.h"
#include "../../hacks/movement/wall_climb.h"
#include "../../hacks/prediction/prediction.h"
#include "../../hacks/skins/skins.h"
#include "../../hacks/visuals/screen/flip_world.h"
#include "../../hacks/web/websurface.h"
#include "../../utilities/perf/perf_watch.h"

/* air_stuck's last word: re-expresses its proven move in the cmd's final view so no later writer
   leaves the weld pushing a stale direction. no-op on ticks air_stuck did not author */
extern void air_stuck_final_stamp( c_user_cmd* cmd );
extern void air_stuck_shot_proof( c_user_cmd* cmd );

extern void start_movement_fix( c_user_cmd* cmd );
extern void end_movement_fix( c_user_cmd* cmd );

extern void botox_dbg_log( const char* fmt, ... );

extern bool g_air_stuck_owns_cmd;
extern bool g_air_stuck_stamp_valid;
extern bool g_air_stuck_stamp_force;
extern bool HITGODA;
extern bool HITGODA2;

extern void ps_cmd_start( );
extern void ps_ride_diag( const c_user_cmd* cmd );
extern void gnd_wish_start( );
extern void gnd_wish_check( const char* stage, bool final_view = false );
extern void cmd_start_guard( const char* stage, bool close );
extern void psa_ride_end( const c_user_cmd* cmd );

namespace
{
	c_vector ladder_velocity( const c_angle& view, const int buttons, const c_vector& n, const bool on_floor )
	{
		constexpr float k_climb = 200.f;
		const float climb       = ( buttons & ( in_duck | in_speed ) ) ? k_climb * 0.34f : k_climb;

		float fwd = 0.f, right = 0.f;
		if ( buttons & in_back )
			fwd -= climb;
		if ( buttons & in_forward )
			fwd += climb;
		if ( buttons & in_moveleft )
			right -= climb;
		if ( buttons & in_moveright )
			right += climb;

		if ( fwd == 0.f && right == 0.f )
			return { };

		c_vector forward{ }, side{ };
		g_math.angle_vectors( view, &forward, &side );
		const c_vector velocity = forward * fwd + side * right;

		c_vector perp( -n.m_y, n.m_x, 0.f );
		if ( perp.length_squared( ) > 1e-8f )
			perp = perp.normalized( );
		const c_vector tmp( -n.m_z * perp.m_y, n.m_z * perp.m_x, n.m_x * perp.m_y - n.m_y * perp.m_x );

		const float normal   = velocity.dot_product( n );
		const c_vector cross = n * normal;
		c_vector lateral     = velocity - cross;

		const float tmp_dist  = tmp.dot_product( lateral );
		const float perp_dist = perp.dot_product( lateral );

		c_vector angle_vec = perp * perp_dist + cross;
		if ( angle_vec.length_squared( ) > 1e-8f )
			angle_vec = angle_vec.normalized( );

		if ( angle_vec.dot_product( n ) < g_convars.float_or( HASH_BT( "sv_ladder_angle" ), -0.707f ) )
			lateral = tmp * tmp_dist + perp * ( g_convars.float_or( HASH_BT( "sv_ladder_dampen" ), 0.2f ) * perp_dist );

		c_vector out = lateral - tmp * normal;

		if ( const float scale = g_convars.float_or( HASH_BT( "sv_ladder_scale_speed" ), 0.78f ); scale > 0.f )
			out = out * scale;

		if ( on_floor && normal > 0.f )
			out = out + n * k_climb;

		return out;
	}

	constexpr int k_ladder_keys = in_forward | in_back | in_moveleft | in_moveright;

	int ladder_keys_for( const c_user_cmd* cmd, c_base_entity* local, const c_angle& to, float* out_error = nullptr, c_vector* out_want = nullptr )
	{
		const c_vector n    = local->get_ladder_normal( );
		const bool on_floor = ( local->get_flags( ) & fl_onground ) != 0;
		const c_vector want = ladder_velocity( cmd->m_view_point, cmd->m_buttons, n, on_floor );
		const int base      = cmd->m_buttons & ~k_ladder_keys;

		int best_buttons = cmd->m_buttons;
		float best_error = 1e30f;

		for ( int f = -1; f <= 1; f++ ) {
			for ( int s = -1; s <= 1; s++ ) {
				const int buttons = base | ( f > 0 ? in_forward : 0 ) | ( f < 0 ? in_back : 0 ) | ( s < 0 ? in_moveleft : 0 ) |
				                    ( s > 0 ? in_moveright : 0 );

				if ( const float error = ( ladder_velocity( to, buttons, n, on_floor ) - want ).length_squared( ); error < best_error ) {
					best_error   = error;
					best_buttons = buttons;
				}
			}
		}

		if ( out_error )
			*out_error = std::sqrtf( best_error );
		if ( out_want )
			*out_want = want;
		return best_buttons;
	}

	void ladder_shot_keys( c_user_cmd* cmd, c_base_entity* local, const c_angle& shot_view )
	{
		float error   = 0.f;
		c_vector want = { };
		const int best_buttons = ladder_keys_for( cmd, local, shot_view, &error, &want );
		const c_vector n       = local->get_ladder_normal( );

		botox_dbg_log( "AIMLADDER: keys %d -> %d want=%.0f/%.0f/%.0f err=%.1f n=%.2f/%.2f/%.2f", cmd->m_buttons & k_ladder_keys,
		               best_buttons & k_ladder_keys, want.m_x, want.m_y, want.m_z, error, n.m_x, n.m_y, n.m_z );

		cmd->m_buttons = best_buttons;
	}

	/* reach = latch trace ( :3598, full wishdir incl pitch ) along the forced heading per unit */
	bool ladder_aim_move( const c_user_cmd& forced, const c_angle& live, const float off, c_user_cmd& out, float& reach )
	{
		c_vector f0{ }, r0{ }, f1{ }, r1{ };
		g_math.angle_vectors( forced.m_view_point, &f0, &r0 );
		g_math.angle_vectors( live, &f1, &r1 );

		const float ox = f0.m_x * forced.m_forward_move + r0.m_x * forced.m_side_move;
		const float oy = f0.m_y * forced.m_forward_move + r0.m_y * forced.m_side_move;
		const float wl = std::sqrtf( ox * ox + oy * oy );
		const float fh = f1.m_x * f1.m_x + f1.m_y * f1.m_y;
		const float rh = r1.m_x * r1.m_x + r1.m_y * r1.m_y;
		if ( wl < 1e-3f || fh < 1e-6f || rh < 1e-6f )
			return false;

		const float rad = off * ( std::numbers::pi_v< float > / 180.f );
		const float wx  = ox * std::cosf( rad ) - oy * std::sinf( rad );
		const float wy  = ox * std::sinf( rad ) + oy * std::cosf( rad );

		const float k = std::sqrtf( forced.m_forward_move * forced.m_forward_move + forced.m_side_move * forced.m_side_move );
		float fwd     = k * ( wx * f1.m_x + wy * f1.m_y ) / ( wl * fh );
		float side    = k * ( wx * r1.m_x + wy * r1.m_y ) / ( wl * rh );

		const c_vector dir = f1 * fwd + r1 * side;
		const float len    = dir.length( );
		if ( len < 1e-6f )
			return false;
		reach = ( dir.m_x * ox + dir.m_y * oy ) / ( len * wl );

		if ( const float big = std::max( std::fabsf( fwd ), std::fabsf( side ) ); big > 450.f ) {
			fwd *= 450.f / big;
			side *= 450.f / big;
		}

		out                = forced;
		out.m_view_point   = live;
		out.m_forward_move = fwd;
		out.m_side_move    = side;
		return true;
	}

	struct sim_end_t {
		c_vector origin{ }, velocity{ }, ladder{ };
		int flags = 0, move_type = 0;
	};

	sim_end_t sim_end( c_base_entity* local, const c_user_cmd& cmd, const int frame )
	{
		c_user_cmd copy = cmd;
		g_prediction.restore_entity_to_predicted_frame( frame );
		g_prediction.begin( local, &copy );
		g_prediction.end( local );
		const int move_type = local->get_move_type( );
		return { local->get_abs_origin( ), local->get_velocity( ), move_type == move_type_ladder ? local->get_ladder_normal( ) : c_vector{ },
			     local->get_flags( ) & ( fl_onground | fl_ducking ), move_type };
	}

	/* ladder normal = later fling dir ( :3713 ), another face is another exit */
	bool same_end( const sim_end_t& a, const sim_end_t& b )
	{
		constexpr float k_pos = 0.01f, k_vel = 0.05f, k_normal = 0.001f;
		return a.move_type == b.move_type && a.flags == b.flags && std::fabsf( a.origin.m_x - b.origin.m_x ) <= k_pos &&
		       std::fabsf( a.origin.m_y - b.origin.m_y ) <= k_pos && std::fabsf( a.origin.m_z - b.origin.m_z ) <= k_pos &&
		       std::fabsf( a.velocity.m_x - b.velocity.m_x ) <= k_vel && std::fabsf( a.velocity.m_y - b.velocity.m_y ) <= k_vel &&
		       std::fabsf( a.velocity.m_z - b.velocity.m_z ) <= k_vel && std::fabsf( a.ladder.m_x - b.ladder.m_x ) <= k_normal &&
		       std::fabsf( a.ladder.m_y - b.ladder.m_y ) <= k_normal && std::fabsf( a.ladder.m_z - b.ladder.m_z ) <= k_normal;
	}

	/* live pitch dips the latch trace: try the 3 headings that reach the face furthest */
	bool latch_live( c_base_entity* local, c_user_cmd* cmd, const c_user_cmd& forced, const c_angle& live, const int frame, const sim_end_t& want )
	{
		struct cand_t {
			float reach = 0.f;
			c_user_cmd cmd{ };
		};
		cand_t cands[ 17 ]{ };
		int n = 0;
		for ( int i = -8; i <= 8; i++ )
			if ( ladder_aim_move( forced, live, i * 10.f, cands[ n ].cmd, cands[ n ].reach ) && cands[ n ].reach > 0.f )
				n++;

		std::sort( cands, cands + n, []( const cand_t& a, const cand_t& b ) { return a.reach > b.reach; } );
		for ( int i = 0; i < std::min( n, 3 ); i++ )
			if ( same_end( sim_end( local, cands[ i ].cmd, frame ), want ) ) {
				*cmd = cands[ i ].cmd;
				return true;
			}
		return false;
	}

	/* ladder climb ( :3726 ) + walk latch ( :3598 ) read pitch, so live view is sent only if it sims to the forced end */
	void silent_view( c_user_cmd* cmd, c_base_entity* local )
	{
		if ( !GET_VARIABLE( g_variables.m_silent_view, bool ) || !local || !local->is_alive( ) || g_edgebug.m_found )
			return;

		c_angle live{ };
		g_interfaces.m_engine_client->get_view_angles( live );
		live.normalize( );
		live.clamp( );
		live.m_z = cmd->m_view_point.m_z;

		c_angle drift = cmd->m_view_point - live;
		drift.normalize( );
		if ( std::fabsf( drift.m_x ) < 0.001f && std::fabsf( drift.m_y ) < 0.001f )
			return;

		const c_user_cmd forced = *cmd;
		start_movement_fix( cmd );
		cmd->m_view_point = live;
		end_movement_fix( cmd );

		/* pin needs exact floats: air_stuck_shot_proof re-proves it, stamp view on a miss */
		if ( g_air_stuck_stamp_valid && g_air_stuck_stamp_force )
			return;

		const int frame = g_interfaces.m_prediction->m_commands_predicted - 1;
		g_prediction.restore_entity_to_predicted_frame( frame );
		const int start_move_type = local->get_move_type( );

		c_user_cmd keys  = *cmd;
		const bool climb = start_move_type == move_type_ladder && ( forced.m_buttons & k_ladder_keys ) && !( forced.m_buttons & in_jump );
		if ( climb )
			keys.m_buttons = ladder_keys_for( &forced, local, live );

		const sim_end_t want = sim_end( local, forced, frame );

		const char* how = nullptr;
		if ( same_end( sim_end( local, *cmd, frame ), want ) )
			how = "rot";
		else if ( climb && keys.m_buttons != cmd->m_buttons && same_end( sim_end( local, keys, frame ), want ) ) {
			*cmd = keys;
			how  = "keys";
		} else if ( start_move_type != move_type_ladder && want.move_type == move_type_ladder && latch_live( local, cmd, forced, live, frame, want ) )
			how = "latch";

		if ( !how )
			*cmd = forced;

		sim_end( local, *cmd, frame );

		static int s_last = -1;
		static float s_next_log = 0.f;
		const int state       = how ? ( how[ 0 ] == 'r' ? 1 : how[ 0 ] == 'k' ? 2 : 3 ) : 0;
		const float now       = g_interfaces.m_global_vars_base->m_real_time;
		if ( state != s_last || ( !how && now >= s_next_log ) ) {
			s_last     = state;
			s_next_log = now + 1.f;
			botox_dbg_log( "VIEW: %s d=%.1f/%.1f mt=%d->%d keys=%d->%d fwd=%.1f/%.1f -> %.1f/%.1f fr=%d/%d", how ? how : "KEPT forced", drift.m_x,
			               drift.m_y, start_move_type, want.move_type, forced.m_buttons & k_ladder_keys, cmd->m_buttons & k_ladder_keys,
			               forced.m_forward_move, forced.m_side_move, cmd->m_forward_move, cmd->m_side_move,
			               ( int )g_movement.m_fireman_data.owns_cmd, ( int )g_movement.m_fireman_data.is_ladder );
		}
	}
}

void __stdcall create_move( int sequence_number, float input_sample_frametime, bool is_active, bool& send_packet )
{
	static auto original = g_hooks.m_create_move_proxy.get_original< decltype( &n_detoured_functions::create_move_proxy ) >( );
	original( g_interfaces.m_base_client, 0, sequence_number, input_sample_frametime, is_active );

	/* bail leaves the game's cmd untouched. must precede the backtrack pump (eject frees records) */
	const n_ctx::hook_scope_t hook_scope_guard;
	if ( g_ctx.m_unloading.load( std::memory_order_relaxed ) )
		return;

	c_user_cmd* cmd                   = g_interfaces.m_input->get_user_cmd( sequence_number );
	c_verified_user_cmd* verified_cmd = g_interfaces.m_input->get_verified_cmd( sequence_number );

	if ( !cmd || !verified_cmd || !is_active )
		return;

	n_tick::begin_cmd_clock( );

	n_perf::cmd_begin( );
	struct perf_cmd_guard_t {
		~perf_cmd_guard_t( )
		{
			n_tick::s_cmd_clock_live = false;
			n_perf::cmd_end( );
		}
	} perf_cmd_guard;

	g_ctx.m_cmd = cmd;

	g_aimbot.m_shot_view_valid = false;
	g_aimbot.m_shot_is_aim     = false;

	const auto local       = g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) );
	const auto net_channel = g_interfaces.m_client_state->m_net_channel;

	if ( g_interfaces.m_engine_client->is_hltv( ) ) {
		g_ctx.m_local = nullptr;
		return;
	}

	g_ctx.m_local = local;

	// before anything reads input: A/D follow the mirrored screen
	g_flip_world.on_create_move( cmd );

	g_lagcomp.begin_command( cmd );

	g_ctx.old_view_point      = cmd->m_view_point;
	g_ctx.m_first_view_angles = cmd->m_view_point;
	g_ctx.m_input_buttons     = cmd->m_buttons;
	g_ctx.m_input_mouse_dx    = cmd->m_mouse_delta_x;
	g_edgebug.donor_mouse_fix( cmd );
	g_ctx.m_previous_tick    = g_interfaces.m_global_vars_base->m_tick_count;
	g_ctx.m_target_velocity_z =
		( ( g_convars.float_or( HASH_BT( "sv_gravity" ), 800.f ) * 0.5f ) * g_interfaces.m_global_vars_base->m_interval_per_tick ) * -1.f;

	{
		PERF_ZONE( zone_cmd_prediction );
		g_prediction.update( );
	}

	ps_cmd_start( );

	{
		PERF_ZONE( zone_cmd_scaleform );
		g_scaleform.on_createmove( );
	}

	{
		PERF_ZONE( zone_cmd_chud );
		g_chud.on_createmove( );
	}

	n_chat_extras::on_create_move( );

	g_aimbot.run_nospread( );

	[ & ]( ) {
		/* live, non-spectating local only; else clear state (stale flags survive respawn) */
		if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || !g_ctx.m_cmd ||
		     g_ctx.m_local->get_observer_mode( ) != e_obs_mode::obs_mode_none ) {
			g_movement.m_edgebug_data.reset( );
			g_movement.m_pixelsurf_data.reset( );
			/* EdgeBugPostPredict never runs here: a plan latched at death would hold mouse lock */
			g_edgebug.m_found  = false;
			g_edgebug.m_ducked = false;
			g_edgebug.drop_mouse( );
			g_edge_skip.reset( true );
			return;
		}

		g_movement_recorder.capture_user_input( cmd );
		g_movement_recorder.apply_playback( cmd );

		gnd_wish_start( );
		g_aimbot.nospread_pre_move( );
		gnd_wish_check( "nospread_pre" );

		g_movement.apply_nulls( );
		gnd_wish_check( "nulls" );
		g_movement.half_sideways( );
		gnd_wish_check( "hsw" );

		/* before every sim: the snap ( ProcessMovement ) must be in all of them */
		{
			PERF_ZONE( zone_cmd_tung );
			g_movement.tung_surf( cmd );
		}
		cmd_start_guard( "tung", false );

		{
			PERF_ZONE( zone_cmd_edgebug );
			g_edgebug.PrePredictionEdgeBug( cmd );
			cmd_start_guard( "edgebug_pre", false );
			g_edge_skip.pre( cmd );
			cmd_start_guard( "edge_skip_pre", false );
		}

		{
			PERF_ZONE( zone_cmd_movement_pre );
			g_movement.on_create_move_pre( );
		}
		gnd_wish_check( "movement_pre" );

		{
			PERF_ZONE( zone_cmd_misc );
			g_misc.on_create_move_pre( );
		}
		gnd_wish_check( "misc" );

		g_skins.deagle_spinner( );
		gnd_wish_check( "deagle_spinner" );

		/* pre features may have rewritten it: predict + detect on the exact recorded inputs */
		g_movement_recorder.apply_playback( cmd );

		cmd_start_guard( "pre_end", true );
		g_prediction.begin( g_ctx.m_local, cmd, true );

		g_aimbot.run_zeusbug( );

		g_aimbot.rcs_track( );

		{
			PERF_ZONE( zone_cmd_aimbot );
			g_aimbot.on_create_move_post( );
		}

		{
			PERF_ZONE( zone_cmd_lagcomp );
			g_aimbot.run_backtrack( );
		}

		/* can_shoot is for THIS cmd: tick_base already = its tick (our begin never advances it) */
		g_aimbot.nospread_mark_shot( );

		g_misc.force_lagpush( );

		g_prediction.end( g_ctx.m_local );
		gnd_wish_check( "aimbot" );

		{
			PERF_ZONE( zone_cmd_movement_post );
			g_movement.on_create_move_post( );
		}

		/* recorder owns cmd: eb post over a desynced replay overwrites recorded duck timing */
		if ( !g_movement_recorder.owns_cmd( ) ) {
			PERF_ZONE( zone_cmd_edgebug );
			g_edgebug.EdgeBugPostPredict( cmd );
			{
				PERF_ZONE( zone_cmd_edge_skip );
				g_edge_skip.post( cmd );
			}
		} else {
			/* post never runs: a plan latched by pre would hold mouse lock */
			g_edgebug.m_found  = false;
			g_edgebug.m_ducked = false;
			g_edgebug.drop_mouse( );
			g_edge_skip.reset( true );
		}
		gnd_wish_check( "edgebug_post", true );

		air_stuck_final_stamp( cmd );
		gnd_wish_check( "as_stamp", true );

		/* debug: a wall climb catch rewritten after it committed never lands */
		g_wall_climb.check_stomp( cmd );
		g_movement.auto_align_sent( cmd );

		g_movement_recorder.on_create_move( cmd );

		silent_view( cmd, g_ctx.m_local );
		gnd_wish_check( "silent_view", true );

	}( );

	if ( Web_BlockFire( ) )
		cmd->m_buttons &= ~( in_attack | in_second_attack );

	if ( g_aimbot.m_shot_view_valid && local && local->is_alive( ) && g_aimbot.shot_leaves( ) ) {
		const c_angle shot_view( g_aimbot.m_shot_pitch, g_aimbot.m_shot_yaw, cmd->m_view_point.m_z );

		c_angle drift = shot_view - cmd->m_view_point;
		drift.normalize( );

		if ( std::fabsf( drift.m_x ) > 0.01f || std::fabsf( drift.m_y ) > 0.01f ) {
			botox_dbg_log( "AIMFIX: aim=%d drift=%.2f/%.2f cmd=%.1f/%.1f -> shot=%.1f/%.1f mt=%d\n", g_aimbot.m_shot_is_aim ? 1 : 0,
			               drift.m_x, drift.m_y, cmd->m_view_point.m_x, cmd->m_view_point.m_y, shot_view.m_x, shot_view.m_y,
			               local->get_move_type( ) );

			if ( local->get_move_type( ) == move_type_ladder )
				ladder_shot_keys( cmd, local, shot_view );

			start_movement_fix( cmd );
			cmd->m_view_point = shot_view;
			end_movement_fix( cmd );
		}
	}
	gnd_wish_check( "shot_guard", true );

	if ( local && local->is_alive( ) && ( cmd->m_buttons & ( in_attack | in_second_attack ) ) && !( local->get_flags( ) & fl_onground ) ) {
		c_angle engine_view{ };
		g_interfaces.m_engine_client->get_view_angles( engine_view );
		const c_angle punch = local->get_punch( );
		botox_dbg_log( "SHOT: cmd=%.2f/%.2f raw=%.2f/%.2f eng=%.2f/%.2f shot=%d:%.2f/%.2f punch=%.2f/%.2f eye=%.2f vz=%.2f duck=%d "
		               "ps=%d/%d pin=%d as=%d psa=%d bsa=%d",
		               cmd->m_view_point.m_x, cmd->m_view_point.m_y, g_ctx.old_view_point.m_x, g_ctx.old_view_point.m_y, engine_view.m_x,
		               engine_view.m_y, g_aimbot.m_shot_view_valid ? 1 : 0, g_aimbot.m_shot_pitch, g_aimbot.m_shot_yaw, punch.m_x, punch.m_y,
		               local->get_eye_position( false ).m_z - local->get_abs_origin( ).m_z, local->get_velocity( ).m_z,
		               ( cmd->m_buttons & in_duck ) ? 1 : 0, g_movement.m_pixelsurf_data.should_pixel_surf ? 1 : 0,
		               g_movement.m_pixelsurf_data.should_unduck ? 1 : 0, g_movement.m_pixelsurf_data.m_pin_detected ? 1 : 0,
		               g_air_stuck_owns_cmd ? 1 : 0, HITGODA ? 1 : 0, HITGODA2 ? 1 : 0 );
	}

	if ( net_channel ) {
		g_lagcomp.on_create_move_update( net_channel );

		if ( !g_hooks.m_send_net_msg.is_hooked( ) )
			g_hooks.m_send_net_msg.create( g_virtual.get( net_channel, 40 ), &n_detoured_functions::send_net_msg );

		if ( !g_hooks.m_send_datagram.is_hooked( ) )
			g_hooks.m_send_datagram.create( g_virtual.get( net_channel, 46 ), &n_detoured_functions::send_datagram );
	}

	if ( GET_VARIABLE( g_variables.m_tung_surf, bool ) && !g_hooks.m_process_movement_sv.is_hooked( ) ) {
		static unsigned long long next_try = 0ull;
		if ( const unsigned long long now = GetTickCount64( ); now >= next_try ) {
			next_try = now + 5000ull;
			if ( void* sv = GetModuleHandleA( "server.dll" ) )
				if ( void* gm = module_t( sv, "server.dll" ).find_interface( "GameMovement001" ) ) {
					const bool ok = g_hooks.m_process_movement_sv.create( g_virtual.get( gm, 1 ), &n_detoured_functions::process_movement_sv );
					botox_dbg_log( "TUNG: server ProcessMovement hook %s gm=%p", ok ? "ok" : "FAILED", gm );
				}
		}
	}

	cmd->m_view_point.normalize( );
	cmd->m_view_point.clamp( );

	air_stuck_shot_proof( cmd );

	g_ctx.m_last_tick_yaw = cmd->m_view_point.m_y;
	g_ctx.last_view_point = cmd->m_view_point;

	ps_ride_diag( cmd );
	psa_ride_end( cmd );

	g_prediction.stamp_sent_stamina( cmd );

	/* interium air stuck: server drops INT_MAX cmds, player hangs. community servers only */
	if ( GET_VARIABLE( g_variables.m_air_freeze, bool ) && g_input.check_input( &GET_VARIABLE( g_variables.m_air_freeze_key, key_bind_t ) ) ) {
		cmd->m_command_number = 0x7fffffff;
		cmd->m_tick_count     = 0x7fffffff;
	}

	verified_cmd->m_user_cmd = *cmd;
	verified_cmd->m_hash_crc = cmd->get_check_sum( );
}

__declspec( naked ) void __fastcall n_detoured_functions::create_move_proxy( [[maybe_unused]] void* ecx, [[maybe_unused]] void* edx,
                                                                             [[maybe_unused]] int sequence_number,
                                                                             [[maybe_unused]] float input_sample_frametime,
                                                                             [[maybe_unused]] bool is_active )
{
	__asm
	{
		push	ebp
		mov		ebp, esp;
		push	ebx;
		push	esp;
		push	dword ptr[is_active];
		push	dword ptr[input_sample_frametime];
		push	dword ptr[sequence_number];
		call	create_move
		pop		ebx
		pop		ebp
		retn	0Ch
	}
}
