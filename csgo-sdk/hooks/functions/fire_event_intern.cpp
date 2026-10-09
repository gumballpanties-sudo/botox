#include "../../game/sdk/includes/includes.h"
#include "../../globals/logger/logger.h"
#include "../../hacks/misc/scaleform/scaleform.h"
#include "../../hacks/chud_hud/chud_hud.h"
#include "../../hacks/mc_hud/mc_hud.h"
#include "../hooks.h"
#include "../../hacks/visuals/players/dormancy/dormancy.h"
#include "../../hacks/visuals/players/players.h"
#include "../../hacks/visuals/bullets/bullets.h"
#include "../../hacks/movement/movement.h"
#include "../../hacks/movement/movement_recorder.h"
#include "../../hacks/misc/misc.h"
#include "../../hacks/skins/skins.h"
#include "../../utilities/console/console.h"

#include <cstring>

extern void botox_dbg_log( const char* fmt, ... );

bool __fastcall n_detoured_functions::fire_event_intern( void* ecx, void* edx, game_event_t* game_event )
{
	static auto original = g_hooks.m_fire_event_intern.get_original< decltype( &n_detoured_functions::fire_event_intern ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, game_event ) );

	const auto hashed_event = HASH_RT( game_event->get_name( ) );

	if ( !g_render.m_initialised )
		return original( ecx, edx, game_event );

	const ImColor im_accent_color = ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent );
	const c_unsigned_char_color accent_color =
		c_unsigned_char_color( im_accent_color.Value.x * 255, im_accent_color.Value.y * 255, im_accent_color.Value.z * 255, 255 );

	[ & ]( ) {
		int m_attacker{ }, m_victim{ }, m_group{ }, m_health{ }, m_damage{ };
		std::string m_name{ };
		c_base_entity* m_attacker_ent{ };

		switch ( hashed_event ) {
		case HASH_BT( "game_newmawp" ):
			g_scaleform.m_should_force_update = true;
			for ( int i = 0; i < 64; i++ ) {
				g_players.m_stored_cur_time[ i ] = 0.f;
				g_players.m_fading_alpha[ i ] = 0.f;
				g_dormancy.m_sound_players[ i ].reset( );
				g_players.god_on_hurt( i + 1 );
			}
			break;
		case HASH_BT( "bot_takeover" ):
		case HASH_BT( "switch_team" ):
		case HASH_BT( "round_start" ):
			if ( hashed_event == HASH_BT( "round_start" ) ) {
				g_movement_recorder.set_round_frozen( true );
				g_scaleform.m_killfeed_round_reset = true;
				for ( auto& row : g_chud.m_kill_rows )
					row.m_keep = false;
			}

			g_scaleform.m_should_update_teamcount_avatar = true;
			for ( int i = 0; i < 64; i++ ) {
				g_players.m_stored_cur_time[ i ] = 0.f;
				g_players.m_fading_alpha[ i ]    = 0.f;
				g_dormancy.m_sound_players[ i ].reset( );
				g_players.god_on_hurt( i + 1 );
			}
			break;
		case HASH_BT( "player_team" ):
			g_scaleform.m_should_update_teamcount_avatar = true;
			break;
		case HASH_BT( "bullet_impact" ):
			g_bullets.on_bullet_impact( game_event );
			g_players.god_on_impact( g_interfaces.m_engine_client->get_player_for_user_id( game_event->get_int( "userid" ) ),
			                         c_vector( game_event->get_float( "x" ), game_event->get_float( "y" ), game_event->get_float( "z" ) ) );
			break;
		case HASH_BT( "round_mvp" ):
			g_scaleform.m_pending_mvp = true;
			break;
		case HASH_BT( "round_prestart" ):
			g_movement_recorder.set_round_frozen( true );
			break;
		case HASH_BT( "round_freeze_end" ):
			g_movement_recorder.set_round_frozen( false );
			break;
		case HASH_BT( "round_end" ):
			g_movement_recorder.set_round_frozen( true );
			g_scaleform.m_winpanel_team          = game_event->get_int( "winner" );
			g_scaleform.m_should_update_winpanel = true;
			break;
		case HASH_BT( "player_death" ): {
			g_scaleform.m_should_update_deathnotices = true;

			const int attacker_id = g_interfaces.m_engine_client->get_player_for_user_id( game_event->get_int( "attacker" ) );
			const int victim_id   = g_interfaces.m_engine_client->get_player_for_user_id( game_event->get_int( "userid" ) );
			const int local_id    = g_interfaces.m_engine_client->get_local_player( );

			g_players.god_on_hurt( victim_id );

			if ( victim_id == local_id )
				g_movement_recorder.force_stop( );

			const int revive_mode = GET_VARIABLE( g_variables.m_auto_revive_mode, int );
			const auto picked     = player_list_get( victim_id );
			const bool is_target  = picked && picked->m_revive;
			const bool is_self    = victim_id == local_id;

			if ( GET_VARIABLE( g_variables.m_auto_revive, bool ) && victim_id > 0 &&
			     ( revive_mode == 0 || ( is_self && ( revive_mode == 1 || revive_mode == 3 ) ) ||
			       ( is_target && ( revive_mode == 2 || revive_mode == 3 ) ) ) ) {
				const std::string revive = "sm_respawn #" + std::to_string( game_event->get_int( "userid" ) );
				g_interfaces.m_engine_client->execute_client_cmd( revive.c_str( ) );
				botox_dbg_log( "REVIVE: %s", revive.c_str( ) );
			}

			if ( attacker_id == local_id && attacker_id != victim_id ) {
				const std::string killed_with = game_event->get_string( "weapon" );

				/* never trust the setter slot (a wrong one corrupts live events): write, read back,
				   stop for the session if it didn't stick */
				static bool set_string_usable = true;

				if ( set_string_usable && ( killed_with == "bayonet" || killed_with.rfind( "knife", 0 ) == 0 ) ) {
					if ( const char* knife_name = n_skins::knife_killfeed_name( ) ) {
						game_event->set_string( "weapon", knife_name );

						const char* read_back = game_event->get_string( "weapon" );
						const bool stuck      = read_back && std::strcmp( read_back, knife_name ) == 0;

						static bool logged = false;
						if ( !logged ) {
							logged = true;

							const char* shown   = read_back ? read_back : "(null)";
							const char* verdict = stuck ? "ok" : "SLOT IS WRONG";

							g_console.print(
								std::vformat( "killfeed weapon: wrote '{:s}', reads back '{:s}' — {:s}",
							                  std::make_format_args( knife_name, shown, verdict ) )
									.c_str( ) );
						}

						set_string_usable = stuck;
					}
				}
			}

			if ( attacker_id == local_id && attacker_id != victim_id )
				on_healthshot( 0 );
			on_death_particles( victim_id, attacker_id );

			player_info_t attacker_info{ }, victim_info{ };
			const bool got_attacker = g_interfaces.m_engine_client->get_player_info( attacker_id, &attacker_info );
			const bool got_victim   = g_interfaces.m_engine_client->get_player_info( victim_id, &victim_info );

			const bool has_attacker = got_attacker && attacker_id > 0 && attacker_id != victim_id;

			if ( got_victim ) {
				g_chud.on_player_death( has_attacker ? attacker_info.m_name : "", victim_info.m_name, game_event->get_string( "weapon" ),
				                        game_event->get_bool( "headshot" ), attacker_id == local_id || victim_id == local_id,
				                        has_attacker && attacker_id == local_id );
			}

			g_mc_hud.on_player_death( attacker_id, victim_id, game_event->get_string( "weapon" ) );
			break;
		}
		case HASH_BT( "player_hurt" ):
			m_attacker     = g_interfaces.m_engine_client->get_player_for_user_id( game_event->get_int( "attacker" ) );
			m_victim       = g_interfaces.m_engine_client->get_player_for_user_id( game_event->get_int( "userid" ) );
			m_attacker_ent = g_interfaces.m_client_entity_list->get< c_base_entity >( m_attacker );

			g_players.god_on_hurt( m_victim );

			if ( m_attacker < 1 || m_attacker > 64 || m_victim < 1 || m_victim > 64 )
				break;

			if ( m_victim == g_interfaces.m_engine_client->get_local_player( ) && m_attacker != m_victim &&
			     g_config.get< std::vector< bool > >( g_variables.m_log_types )[ e_log_types::log_type_hit_enemy ] ) {
				player_info_t shooter{ };
				if ( !g_interfaces.m_engine_client->get_player_info( m_attacker, &shooter ) )
					break;

				m_group = game_event->get_int( "hitgroup" );
				m_name  = std::string( shooter.m_name ).substr( 0, 24 );
				m_damage = game_event->get_int( "dmg_health" );
				m_health = game_event->get_int( "health" );

				const char* gun_raw = game_event->get_string( "weapon" );
				const std::string gun = gun_raw && *gun_raw ? gun_raw : "unknown";
				std::string part = m_group == hitgroup_neck ? "neck" : g_utilities.m_hit_groups[ ( m_group >= 0 && m_group < 8 ) ? m_group : 8 ];
				if ( m_group == hitgroup_stomach && std::rand( ) % 10 == 0 )
					part = "dih";

				g_logger.print( std::vformat( "{} hit you with {} for {} in the {}\n", std::make_format_args( m_name, gun, m_damage, part ) ),
				                "[hurt]" );

				const std::string out = std::format( "{} hit you | weapon: {} | dmg: {} hp | hitgroup: {} | {} health remaining\n", m_name, gun,
				                                     m_damage, part, m_health );

				g_interfaces.m_convar->console_color_printf( accent_color, "[hurt] " );
				g_interfaces.m_convar->console_color_printf( c_unsigned_char_color::console_text_color( ), "%s", out.c_str( ) );
				botox_dbg_log( "HURT %.*s", static_cast< int >( out.size( ) - 1 ), out.c_str( ) );
				break;
			}

			if ( m_attacker != g_interfaces.m_engine_client->get_local_player( ) ||
			          m_victim == g_interfaces.m_engine_client->get_local_player( ) )
				break;

			m_group = game_event->get_int( "hitgroup" );

			botox_dbg_log( "HIT: victim=%d group=%d dmg=%d", m_victim, m_group, game_event->get_int( "dmg_health" ) );

			on_hit_marker( );
			on_hit_sound( );
			g_bullets.on_player_hurt( m_victim );
			g_players.hitboxes_on_hurt( m_victim );

			if ( const auto victim_ent = g_interfaces.m_client_entity_list->get< c_base_entity >( m_victim ) ) {
				int hitbox = e_hitboxes::hitbox_chest;
				switch ( m_group ) {
				case e_hitgroup::hitgroup_head:     hitbox = e_hitboxes::hitbox_head;            break;
				case e_hitgroup::hitgroup_neck:     hitbox = e_hitboxes::hitbox_neck;            break;
				case e_hitgroup::hitgroup_stomach:  hitbox = e_hitboxes::hitbox_stomach;         break;
				case e_hitgroup::hitgroup_leftarm:  hitbox = e_hitboxes::hitbox_left_upper_arm;  break;
				case e_hitgroup::hitgroup_rightarm: hitbox = e_hitboxes::hitbox_right_upper_arm; break;
				case e_hitgroup::hitgroup_leftleg:  hitbox = e_hitboxes::hitbox_left_thigh;      break;
				case e_hitgroup::hitgroup_rightleg: hitbox = e_hitboxes::hitbox_right_thigh;     break;
				default: break;
				}

				c_vector damage_origin{ };

				if ( auto& bones = victim_ent->get_cached_bone_data( ); bones.count( ) > 0 && bones.base( ) )
					damage_origin = victim_ent->get_hitbox_position( hitbox, bones.base( ) );

				if ( damage_origin.is_zero( ) ) {
					damage_origin = victim_ent->get_abs_origin( );
					damage_origin.m_z += 50.f;
				}

				g_players.on_player_hurt( damage_origin, m_victim, game_event->get_int( "dmg_health" ) );
			}

			if ( m_group == hitgroup_gear )
				break;

			auto m_target = g_interfaces.m_client_entity_list->get< c_base_entity* >( m_victim );
			if ( !m_target )
				break;

			player_info_t info;
			if ( !g_interfaces.m_engine_client->get_player_info( m_victim, &info ) )
				break;
			m_name   = std::string( info.m_name ).substr( 0, 24 );
			m_damage = game_event->get_int( "dmg_health" );
			m_health = game_event->get_int( "health" );

			if ( g_config.get< std::vector< bool > >( g_variables.m_log_types )[ e_log_types::log_type_hit_enemy ] ) {
				g_logger.print(
					std::vformat( "hit {} for {} in the {}\n", std::make_format_args( m_name, m_damage, g_utilities.m_hit_groups[ m_group ] ) ),
					"[damage]" );

				const std::string out =
					std::format( "hit {} | dealt: {} hp | hitgroup: {} | {} health remaining | backtrack: {} ticks\n",
				                   m_name,  std::to_string(m_damage), g_utilities.m_hit_groups[ m_group ], std::to_string(m_health),
				                                        std::to_string( g_ctx.m_record ? g_math.time_to_ticks( std::fabsf( m_attacker_ent->get_simulation_time( ) -  g_ctx.m_record->m_sim_time ) )
				                                                        : 0 )  );

				g_interfaces.m_convar->console_color_printf( accent_color, "[damage] " );
				/* "%s", never out as format: enemy names can hold %s / %n */
				g_interfaces.m_convar->console_color_printf( c_unsigned_char_color::console_text_color( ), "%s", out.c_str( ) );
				botox_dbg_log( "DMG %.*s", static_cast< int >( out.size( ) - 1 ), out.c_str( ) );
			}
			break;
		}
	}( );

	return original( ecx, edx, game_event );
}
