#include "skins.h"
#include "skins_internal.h"

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/logger/logger.h"
#include "../lagcomp/lagcomp.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

bool n_skins::knife_anims_fit( int anim_index, int knife_index )
{
	if ( anim_index <= 0 || anim_index >= KNIFE_COUNT )
		return true;

	const bool model_changer = GET_VARIABLE( g_variables.m_knife_enable, bool );
	const int mesh           = ( model_changer && knife_index > 0 && knife_index < KNIFE_COUNT ) ? knife_index : 15 ;

	return ( KNIFE_ANIM_COMPAT[ anim_index ] & ( 1u << mesh ) ) != 0;
}

const char* n_skins::knife_anim_redirect( const char* requested_model )
{
	if ( !requested_model || !*requested_model )
		return nullptr;

	for ( int i = 1; i < KNIFE_COUNT; i++ ) {
		if ( same_model_file( KNIFE_MODELS[ i ], requested_model ) )
			return nullptr;
	}

	unsigned int meshes = 0;

	for ( int i = 1; i < KNIFE_COUNT; i++ ) {
		if ( same_model_file( KNIFE_ANIM_MODELS[ i ], requested_model ) )
			meshes |= ( 1u << i );
	}

	if ( !meshes )
		return nullptr;

	const char* donor = g_anim_donor.load( std::memory_order_relaxed );
	const bool swap   = donor && *donor && !same_model_file( donor, requested_model ) && ( meshes & g_anim_targets.load( std::memory_order_relaxed ) );

	for ( int i = 1; i < KNIFE_COUNT; i++ ) {
		if ( meshes & ( 1u << i ) )
			g_mesh_anim[ i ].store( swap ? donor : KNIFE_ANIM_MODELS[ i ], std::memory_order_relaxed );
	}

	g_graph_donor.store( donor, std::memory_order_relaxed );

	return swap ? donor : nullptr;
}

const char* n_skins::knife_killfeed_name( )
{
	if ( !GET_VARIABLE( g_variables.m_knife_enable, bool ) )
		return nullptr;

	const int chosen = GET_VARIABLE( g_variables.m_knife_model, int );
	if ( chosen <= 0 || chosen >= KNIFE_COUNT )
		return nullptr;

	return KNIFE_KILLFEED_NAMES[ chosen ];
}

bool n_skins::custom_colors_for_material( const char* vmt_path, int paint_kit, paint_colors_t& out )
{
	if ( !vmt_path || !*vmt_path || paint_kit <= 0 )
		return false;

	std::string folder = vmt_path;

	const auto file_slash = folder.find_last_of( "/\\" );
	if ( file_slash == std::string::npos )
		return false;

	folder.erase( file_slash );

	if ( const auto slash = folder.find_last_of( "/\\" ); slash != std::string::npos )
		folder.erase( 0, slash + 1 );

	if ( folder.empty( ) )
		return false;

	const auto fill = []( paint_colors_t& colors, const c_color& first, const c_color& second, const c_color& third, const c_color& fourth ) {
		const c_color* const source[ 4 ] = { &first, &second, &third, &fourth };

		for ( int i = 0; i < 4; i++ ) {
			colors.m_rgb[ i ][ 0 ] = static_cast< float >( ( *source[ i ] )[ e_color_type::color_type_r ] );
			colors.m_rgb[ i ][ 1 ] = static_cast< float >( ( *source[ i ] )[ e_color_type::color_type_g ] );
			colors.m_rgb[ i ][ 2 ] = static_cast< float >( ( *source[ i ] )[ e_color_type::color_type_b ] );
		}
	};

	/* knives paint from knife_<model> (gold knife's is "c4", never matches) */
	if ( folder.rfind( "knife_", 0 ) == 0 ) {
		if ( !GET_VARIABLE( g_variables.m_knife_enable, bool ) || !GET_VARIABLE( g_variables.m_knife_custom_color, bool ) )
			return false;

		if ( GET_VARIABLE( g_variables.m_knife_paint_kit, int ) != paint_kit )
			return false;

		fill( out, GET_VARIABLE( g_variables.m_knife_color_1, c_color ), GET_VARIABLE( g_variables.m_knife_color_2, c_color ),
		      GET_VARIABLE( g_variables.m_knife_color_3, c_color ), GET_VARIABLE( g_variables.m_knife_color_4, c_color ) );

		return true;
	}

	if ( !GET_VARIABLE( g_variables.m_weapon_skins_enable, bool ) )
		return false;

	auto& paint_kits = GET_VARIABLE( g_variables.m_weapon_skins_paint_kit, std::vector< int > );
	auto& enabled    = GET_VARIABLE( g_variables.m_weapon_skins_custom_color, std::vector< bool > );
	auto& colors_1   = GET_VARIABLE( g_variables.m_weapon_skins_color_1, std::vector< c_color > );
	auto& colors_2   = GET_VARIABLE( g_variables.m_weapon_skins_color_2, std::vector< c_color > );
	auto& colors_3   = GET_VARIABLE( g_variables.m_weapon_skins_color_3, std::vector< c_color > );
	auto& colors_4   = GET_VARIABLE( g_variables.m_weapon_skins_color_4, std::vector< c_color > );

	for ( int i = 0; i < WEAPON_COUNT; i++ ) {
		if ( folder != WEAPON_MATERIAL_NAMES[ i ] )
			continue;

		if ( i >= static_cast< int >( enabled.size( ) ) || !enabled[ i ] )
			return false;

		if ( i >= static_cast< int >( paint_kits.size( ) ) || paint_kits[ i ] != paint_kit )
			return false;

		if ( i >= static_cast< int >( colors_1.size( ) ) || i >= static_cast< int >( colors_2.size( ) ) ||
		     i >= static_cast< int >( colors_3.size( ) ) || i >= static_cast< int >( colors_4.size( ) ) )
			return false;

		fill( out, colors_1[ i ], colors_2[ i ], colors_3[ i ], colors_4[ i ] );
		return true;
	}

	return false;
}

namespace
{
	std::unordered_map< int, n_skins::paint_colors_t > g_actual_colors{ };
}

void n_skins::cache_actual_colors( int paint_kit, const paint_colors_t& colors )
{
	g_actual_colors[ paint_kit ] = colors;
}

bool n_skins::get_actual_colors( int paint_kit, paint_colors_t& out )
{
	const auto found = g_actual_colors.find( paint_kit );
	if ( found == g_actual_colors.end( ) )
		return false;

	out = found->second;
	return true;
}

void n_skins::impl_t::dump_model_list( )
{
	static bool done = false;
	if ( done )
		return;

	if ( !g_interfaces.m_string_tables )
		return;

	const auto table = g_interfaces.m_string_tables->find_table( "modelprecache" );
	if ( !table )
		return;

	const int count = table->get_num_strings( );
	if ( count <= 0 )
		return;

	done = true;

	char temp[ MAX_PATH ]{ };
	const DWORD n = GetTempPathA( MAX_PATH, temp );
	const std::string path = ( n > 0 && n < MAX_PATH ? std::string( temp, n ) : std::string( ".\\" ) ) + "botox_models.log";

	std::ofstream f( path, std::ios::trunc );
	if ( !f )
		return;

	f << "=== models/player (" << count << " precached models total) ===\n";
	for ( int i = 0; i < count; ++i ) {
		const char* s = table->get_string( i );
		if ( s && std::strstr( s, "models/player" ) )
			f << "  [" << i << "] " << s << "\n";
	}

	f << "\n=== ALL precached models ===\n";
	for ( int i = 0; i < count; ++i ) {
		if ( const char* s = table->get_string( i ) )
			f << "  [" << i << "] " << s << "\n";
	}
}

bool n_skins::impl_t::local_refresh( )
{
	const auto local = g_ctx.m_local;
	if ( !local )
		return false;

	const auto weapons = local->get_weapons_handle( );
	if ( !weapons )
		return false;

	int rebuilt = 0;

	for ( int w = 0; w < 64 && weapons[ w ] != 0xFFFFFFFF; w++ ) {
		const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( weapons[ w ] );
		if ( !weapon )
			continue;

		// never rebuild someone else's gun
		if ( !weapon_is_ours( weapon, weapons[ w ] ) )
			continue;

		rebuild_custom_materials( weapon );
		rebuilt++;
	}

	if ( const auto viewmodel = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_view_model_handle( ) ) )
		static_cast< c_client_networkable* >( viewmodel )->on_data_changed( 0  );

	return rebuilt > 0;
}

void n_skins::impl_t::full_update( )
{
	/* spawn grace: 2s after spawn, don't refresh during the online join burst (crashed on team
	   select). request stays pending. tracked before the bails so every spawn gets its own window. */
	static float alive_since = 0.f;

	static bool spawn_refresh_asked = false;

	const float time_now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_current_time : 0.f;

	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) ) {
		alive_since         = 0.f;
		spawn_refresh_asked = false;

		m_seen_weapons.clear( );
		m_pending_weapons.clear( );
		g_weapon_owned.clear( );
		return;
	}

	if ( alive_since <= 0.f || time_now < alive_since )
		alive_since = time_now;

	if ( !spawn_refresh_asked && time_now - alive_since >= 2.f ) {
		spawn_refresh_asked = true;
		m_forcing_update    = true;
	}

	if ( !m_forcing_update )
		return;

	if ( time_now - alive_since < 2.f )
		return;

	// throttle 1s (per-frame updates crashed). keep m_forcing_update pending through the cooldown.
	static float last_update_time = 0.f;

	const float now = time_now;

	if ( !g_interfaces.m_client_state || !g_interfaces.m_client_state->m_net_channel )
		return;

	if ( std::abs( now - last_update_time ) < 1.f )
		return;

	last_update_time = now;

	if ( local_refresh( ) )
		m_forcing_update = false;
}

void n_skins::impl_t::on_level_init( )
{
	g_agent_tried.clear( );
	g_agent_last_table_count = 0;

	// stale "original" index would mean another model now
	g_agent_orig_idx    = 0;
	g_agent_applied_idx = 0;
	g_agent_log_left    = 64;

	// stale request from the old map
	m_forcing_update = false;
	m_seen_weapons.clear( );
	m_pending_weapons.clear( );
	g_weapon_owned.clear( );

}

void n_skins::impl_t::on_level_pre_load( )
{
	const char* donor          = g_anim_pick.load( std::memory_order_relaxed );
	const unsigned int targets = g_anim_targets_pick.load( std::memory_order_relaxed );
	g_anim_donor.store( donor, std::memory_order_relaxed );
	g_anim_targets.store( targets, std::memory_order_relaxed );

	static unsigned int graph_targets = ~0u;
	if ( donor == g_graph_donor.load( std::memory_order_relaxed ) && targets == graph_targets )
		return;
	graph_targets = targets;

	if ( !g_interfaces.m_model_cache )
		return;

	/* resolve handles fresh (stale ones are reused by other models). find_mdl adds a ref, released
	   right after; an unloaded knife's flush is a no-op. */
	for ( int i = 1; i < KNIFE_COUNT; i++ ) {
		const unsigned short handle = g_interfaces.m_model_cache->find_mdl( KNIFE_MODELS[ i ] );
		if ( handle == 0xffff )
			continue;

		g_interfaces.m_model_cache->flush( handle, c_model_cache::flush_virtual_model );
		g_interfaces.m_model_cache->release( handle );
	}

	for ( auto& mesh : g_mesh_anim )
		mesh.store( nullptr, std::memory_order_relaxed );
}

void n_skins::impl_t::on_frame_stage_notify( int stage )
{

	if ( stage == render_start )
		fix_dagger_view( );

	knife_hold( stage );

	if ( stage != net_update_postdataupdate_start )
		return;

	init_parser( );

	agent_changer( );
	knife_anim_live( );
	knife_changer( );
	gloves_changer( );
	full_update( );
}

void n_skins::impl_t::publish_anim_donor( )
{
	const bool enable = GET_VARIABLE( g_variables.m_knife_anims_enable, bool );
	const int model   = GET_VARIABLE( g_variables.m_knife_anims_model, int );

	const char* pick = enable && model > 0 && model < KNIFE_COUNT ? KNIFE_ANIM_MODELS[ model ] : nullptr;
	g_anim_pick.store( pick, std::memory_order_relaxed );

	const int mesh             = GET_VARIABLE( g_variables.m_knife_model, int );
	const unsigned int targets = GET_VARIABLE( g_variables.m_knife_enable, bool ) && mesh > 0 && mesh < KNIFE_COUNT
	                                 ? 1u << mesh
	                                 : ( 1u << 15 ) | ( 1u << 16 ) ;
	g_anim_targets_pick.store( targets, std::memory_order_relaxed );

	if ( !g_interfaces.m_engine_client->is_in_game( ) ) {
		g_anim_donor.store( pick, std::memory_order_relaxed );
		g_anim_targets.store( targets, std::memory_order_relaxed );
	}
}

void n_skins::impl_t::fix_dagger_view( )
{
	if ( !GET_VARIABLE( g_variables.m_knife_fix_view, bool ) )
		return;

	const auto vm = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_view_model_handle( ) );
	if ( !vm )
		return;

	static float last_daggers = -1.f;
	const float now           = g_interfaces.m_global_vars_base->m_real_time;

	const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( vm->get_weapon_handle( ) );
	if ( weapon && weapon->get_item_definition_index( ) == weapon_knife_push )
		last_daggers = now;
	else if ( last_daggers < 0.f || now - last_daggers > 0.5f )
		return;

	vm->get_cam_driver_applied_time( ) = 0.f;
	vm->get_cam_driver_angles( )       = c_angle( );
}

void n_skins::impl_t::knife_hold( int stage )
{
	if ( stage != render_start && stage != render_end )
		return;

	auto& view         = g_knife_view;
	const auto vm      = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_view_model_handle( ) );
	const auto globals = g_interfaces.m_global_vars_base;
	const float now    = ( static_cast< float >( g_ctx.m_local->get_tick_base( ) ) + globals->m_interpolation_amount ) * globals->m_interval_per_tick;

	if ( stage == render_end ) {
		if ( !view.m_view_model || vm != view.m_view_model )
			return;

		view.m_cycle = vm->get_cycle( );

		if ( view.m_applied ) {
			view.m_applied                 = false;
			vm->get_sequence( )            = view.m_saved_seq;
			vm->get_anim_time( )           = view.m_saved_time;
			vm->get_cycle( )               = view.m_saved_cycle;
			vm->get_reset_events_parity( ) = view.m_saved_parity;
		}
		return;
	}

	const auto weapon     = vm ? g_interfaces.m_client_entity_list->get< c_base_entity >( vm->get_weapon_handle( ) ) : nullptr;
	const auto model      = vm ? g_interfaces.m_model_info->get_model( vm->get_model_index( ) ) : nullptr;
	const char* name      = model ? g_interfaces.m_model_info->get_model_name( model ) : nullptr;
	const char* anim_model = name ? effective_anim_model( intended_knife_mesh( name ) ) : nullptr;

	if ( !weapon || !is_knife_class( class_id_of( weapon ) ) || !anim_model ) {
		view.m_view_model = nullptr;
		return;
	}

	const int net_seq    = vm->get_sequence( );
	const float net_time = vm->get_anim_time( );

	if ( view.m_view_model != vm ) {
		view          = { vm, net_seq, net_time, net_seq, net_time, vm->get_cycle( ) };
		view.m_parity = vm->get_reset_events_parity( );
		return;
	}

	const bool event = net_seq != view.m_net_seq || net_time > view.m_net_time + 0.1f;
	view.m_net_seq   = net_seq;
	view.m_net_time  = net_time;

	if ( event ) {
		if ( knife_sequence_is_idle( anim_model, net_seq ) && view.m_cycle < 0.98f )
			view.m_pending = true;
		else {
			view.m_seq       = net_seq;
			view.m_time      = net_time;
			view.m_pending   = false;
			view.m_own_clock = false;
			view.m_parity    = vm->get_reset_events_parity( );
		}
	} else if ( !view.m_pending && !view.m_own_clock )
		view.m_time = net_time;

	if ( view.m_pending && view.m_cycle >= 0.999f ) {
		view.m_seq       = net_seq;
		view.m_time      = now;
		view.m_pending   = false;
		view.m_own_clock = true;
	}

	if ( view.m_seq == net_seq && view.m_time == net_time )
		return;

	view.m_saved_seq    = net_seq;
	view.m_saved_time   = net_time;
	view.m_saved_cycle  = vm->get_cycle( );
	view.m_saved_parity = vm->get_reset_events_parity( );
	view.m_applied      = true;

	vm->get_sequence( )            = view.m_seq;
	vm->get_anim_time( )           = view.m_time;
	vm->get_reset_events_parity( ) = view.m_parity;
}

void n_skins::impl_t::deagle_spinner( )
{
	static bool held         = false;
	static bool rolling      = false;
	static float last_send   = -1.f;
	static float last_cancel = -9.f;
	static int rolls         = 0;

	const float now = g_interfaces.m_global_vars_base->m_real_time;

	const auto send = [ & ]( bool hold, float cycle ) {
		g_interfaces.m_engine_client->client_cmd_unrestricted( hold ? "+lookatweapon" : "-lookatweapon" );
		botox_dbg_log( "[spin] %s cycle %.3f", hold ? "+" : "-", cycle );
		held      = hold;
		last_send = now;
	};

	const auto local  = g_ctx.m_local;
	const auto weapon = local ? g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_active_weapon_handle( ) ) : nullptr;

	if ( !GET_VARIABLE( g_variables.m_deagle_spinner, bool ) || !weapon || weapon->get_item_definition_index( ) != weapon_deagle ) {
		rolling = false;
		rolls   = 0;
		if ( held )
			send( false, -1.f );
		return;
	}

	const auto vm = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_view_model_handle( ) );
	if ( !vm )
		return;

	const bool key  = g_input.check_input( &GET_VARIABLE( g_variables.m_deagle_spinner_key, key_bind_t ) );
	const float rtt = g_lagcomp.real_latency( );

	const bool settled = now - last_send > rtt + 0.3f;
	// a + or cancel is back in a snapshot
	const float echo = rtt + 2.f * g_interfaces.m_global_vars_base->m_interval_per_tick + 0.05f;

	/* server rolls lookat01:lookat02 10:1 off RandomInt (string cmd, outside prediction: no seed to steer).
	   miss: IN_ATTACK2 = StopLookingAtWeapon (CCSPlayer::PlayerRunCommand), no-op on a deagle. roll again */
	if ( !local->is_looking_at_weapon( ) || vm->get_sequence( ) != k_deagle_lookat02 ) {
		if ( local->is_looking_at_weapon( ) ) {
			rolling = true;
			if ( now - last_cancel > echo && g_ctx.m_cmd ) {
				g_ctx.m_cmd->m_buttons |= in_second_attack;
				last_cancel = now;
				botox_dbg_log( "[spin] miss %d, cancel", rolls );
			}
			return;
		}

		rolling = rolling || key;
		if ( rolling ) {
			if ( now - last_send > std::max( echo, k_roll_gap ) && now - last_cancel > echo ) {
				// own server: rig the roll (hooks/functions/random_int.cpp), real lookat02 = demos + spectators see it
				if ( const auto nci = g_interfaces.m_engine_client->get_net_channel_info( ); nci && nci->is_loopback( ) )
					g_rig_lookat_until.store( GetTickCount64( ) + 1000ull, std::memory_order_relaxed );
				send( true, -1.f );
				rolls++;
			}
		} else if ( settled && held )
			send( false, -1.f );
		return;
	}

	if ( rolling )
		botox_dbg_log( "[spin] hit after %d rolls", rolls );
	g_rig_lookat_until.store( 0, std::memory_order_relaxed );
	rolling = false;
	rolls   = 0;

	const float cycle = vm->get_cycle( );
	// the "-" must land before the loop event: round trip + frame / packet jitter
	const float lead = rtt + 3.f * g_interfaces.m_global_vars_base->m_interval_per_tick + 0.08f;
	const bool want  = want_spin_hold( key, held, cycle, lead, echo );

	if ( want != held || ( settled && local->is_holding_look_at_weapon( ) != want ) )
		send( want, cycle );
}

void n_skins::impl_t::animation_hook( )
{
	for ( auto client_class = g_interfaces.m_base_client->get_all_classes( ); client_class; client_class = client_class->m_next ) {
		if ( !client_class->m_network_name || std::strcmp( client_class->m_network_name, "CBaseViewModel" ) )
			continue;

		const auto recv_table = client_class->m_recv_table;
		if ( !recv_table )
			break;

		for ( int i = 0; i < recv_table->m_props_count; i++ ) {
			const auto prop = &recv_table->m_props[ i ];
			if ( !prop || !prop->m_var_name )
				continue;

			if ( !std::strcmp( prop->m_var_name, "m_nSequence" ) ) {
				g_original_sequence_proxy = prop->m_proxy_fn;
				prop->m_proxy_fn          = &sequence_proxy;
			} else if ( !std::strcmp( prop->m_var_name, "m_nModelIndex" ) ) {
				g_original_model_index_proxy = prop->m_proxy_fn;
				prop->m_proxy_fn             = &model_index_proxy;
			}
		}
		break;
	}
}

void n_skins::impl_t::animation_unhook( )
{
	for ( auto client_class = g_interfaces.m_base_client->get_all_classes( ); client_class; client_class = client_class->m_next ) {
		if ( !client_class->m_network_name || std::strcmp( client_class->m_network_name, "CBaseViewModel" ) )
			continue;

		const auto recv_table = client_class->m_recv_table;
		if ( !recv_table )
			break;

		for ( int i = 0; i < recv_table->m_props_count; i++ ) {
			const auto prop = &recv_table->m_props[ i ];
			if ( !prop || !prop->m_var_name )
				continue;

			if ( !std::strcmp( prop->m_var_name, "m_nSequence" ) && g_original_sequence_proxy )
				prop->m_proxy_fn = g_original_sequence_proxy;
			else if ( !std::strcmp( prop->m_var_name, "m_nModelIndex" ) && g_original_model_index_proxy )
				prop->m_proxy_fn = g_original_model_index_proxy;
		}
		break;
	}
}
