#include "skins_internal.h"
#include "../visuals/old_mods.h"
#include "../visuals/screen/reflections.h"
#include "../../globals/logger/logger.h"

void n_skins::impl_t::agent_changer( )
{
	const auto local = g_ctx.m_local;
	if ( !local || !local->is_alive( ) || class_id_of( local ) != e_class_ids::ccs_player )
		return;

	bool& prev_t  = g_agent_prev_t;
	bool& prev_ct = g_agent_prev_ct;

	/* put the game's model back ourselves; the server never resends m_nModelIndex */
	const auto restore_model = [ & ]( ) {
		if ( g_agent_orig_idx > 0 && local->get_model_index( ) != g_agent_orig_idx ) {
			local->set_model_index( g_agent_orig_idx );
			local->invalidate_bone_cache( );
		}

		g_agent_orig_idx    = 0;
		g_agent_applied_idx = 0;
	};

	/* custom body's own first person arms: our arm config in; off = null it, the game looks its own up again */
	const auto custom_arms = [ & ]( const char* arms ) {
		const auto cfg  = custom_arm_cfg( );
		const auto slot = arm_cfg_slot( local );
		if ( !cfg || !slot )
			return;

		if ( !arms ) {
			if ( *slot == cfg )
				rebuild_player_arms( local, nullptr, "custom arms off" );
			return;
		}
		if ( *slot == cfg && std::strcmp( cfg->m_path, arms ) == 0 )
			return;

		std::snprintf( cfg->m_path, sizeof( cfg->m_path ), "%s", arms );
		botox_dbg_log( "AGT: arms %s", arms );
		rebuild_player_arms( local, nullptr, "custom arms", cfg );
	};

	if ( !GET_VARIABLE( g_variables.m_agent_enable, bool ) || g_ctx.m_eject_restoring ) {
		restore_model( );
		custom_arms( nullptr );

		if ( prev_t && local->get_team( ) == team_tt ) {
			prev_t = prev_ct = false;
			m_forcing_update = true;
		}
		if ( prev_ct && local->get_team( ) == team_ct ) {
			prev_t = prev_ct = false;
			m_forcing_update = true;
		}
		return;
	}

	const auto model_idx_valid = []( int idx ) { return idx > 0; };

	const auto precache = []( const char* path ) -> bool {
		if ( !path || !*path || !g_interfaces.m_string_tables )
			return false;

		if ( g_interfaces.m_model_info->get_model_index( path ) > 0 )
			return true;

		const auto table = g_interfaces.m_string_tables->find_table( "modelprecache" );
		if ( !table )
			return false;

		const int count = table->get_num_strings( );
		if ( count < g_agent_last_table_count )
			g_agent_tried.clear( );
		g_agent_last_table_count = count;

		if ( !g_agent_tried.insert( path ).second )
			return false;

		// load FIRST, add_string second (precache thread order): a dead entry in the networked
		// table poisons every later model lookup.
		if ( !g_interfaces.m_model_info->find_or_load_model( path ) ) {
			botox_dbg_log( "AGT: load failed %s", path );
			return false;
		}

		// never fill the table to the brim
		if ( const int max = table->get_max_strings( ); max > 0 && count >= max - 16 ) {
			botox_dbg_log( "AGT: table full %d/%d %s", count, max, path );
			return false;
		}

		const int added = table->add_string( false, path );
		botox_dbg_log( "AGT: precache %s slot=%d count=%d", path, added, count );
		return added != -1;
	};

	const auto resolve_idx = [ & ]( const char* rel ) -> int {
		if ( !rel || !*rel )
			return 0;

		precache( rel );

		return g_interfaces.m_model_info->get_model_index( rel );
	};

	constexpr bool arms_fallback_enabled = true;

	const auto precache_faction_arms = [ & ]( const char* player_model ) {
		if ( !player_model || !*player_model )
			return;

		for ( int i = 0; i < ARM_CONFIG_COUNT; ++i ) {
			if ( !std::strstr( player_model, ARM_CONFIGS[ i ].m_model_substr ) )
				continue;

			if ( ARM_CONFIGS[ i ].m_glove )
				precache( ARM_CONFIGS[ i ].m_glove );
			if ( ARM_CONFIGS[ i ].m_sleeve )
				precache( ARM_CONFIGS[ i ].m_sleeve );
			break;
		}
	};

	const auto arms_fallback = [ & ]( const char* player_model ) {
		if ( !arms_fallback_enabled )
			return;

		if ( !player_model || !*player_model )
			return;

		const arm_fallback_t* fb = nullptr;
		for ( int i = 0; i < ARM_FALLBACK_COUNT; ++i ) {
			if ( std::strstr( player_model, ARM_FALLBACKS[ i ].m_model_substr ) ) {
				fb = &ARM_FALLBACKS[ i ];
				break;
			}
		}
		if ( !fb )
			return;

		if ( g_interfaces.m_model_info->get_model_index( fb->m_own ) > 0 )
			return;

		// gloves changer on: it draws hands, spend the string on a sleeve; off: must be hands.
		// old csgo players: pack ct gloves are stubs, the sleeves are the old hands
		const bool gloves_active = ( GET_VARIABLE( PLAYER_VAR( m_gloves_enable ), bool )
		                             && GET_VARIABLE( PLAYER_VAR( m_gloves_model ), int ) > 0 )
		                        || g_old_mods.players_mounted( );

		const char* const* list = gloves_active ? fb->m_sleeves : fb->m_hands;

		const char* pick = nullptr;
		for ( int i = 0; i < 3 && list[ i ]; ++i ) {
			if ( g_interfaces.m_model_info->get_model_index( list[ i ] ) > 0 ) {
				pick = list[ i ];
				break;
			}
		}

		for ( int i = 0; !pick && gloves_active && i < 3 && fb->m_hands[ i ]; ++i ) {
			if ( g_interfaces.m_model_info->get_model_index( fb->m_hands[ i ] ) > 0 )
				pick = fb->m_hands[ i ];
		}

		for ( int i = 0; !pick && i < ARM_GENERIC_COUNT; ++i ) {
			if ( g_interfaces.m_model_info->get_model_index( ARM_GENERIC[ i ] ) > 0 )
				pick = ARM_GENERIC[ i ];
		}
		if ( !pick )
			return;

		char* arms = local->get_arms_model( );
		if ( !arms || std::strcmp( arms, pick ) == 0 )
			return;

		constexpr std::size_t max_model_string_size = 128;
		const std::size_t len                       = std::strlen( pick );
		if ( len >= max_model_string_size )
			return; // never write a truncated path

		std::memcpy( arms, pick, len + 1 );
	};

	// custom = loose csgo\models\player .mdl picked in the menu, wins over the list index
	// saved configs can hold a pick from before the menu filtered arms / hats / masks: never wear those
	const auto custom_body = [ & ]( const std::string& custom ) {
		if ( custom != g_agent_custom_checked ) {
			g_agent_custom_checked = custom;
			g_agent_custom_ok      = g_utilities.is_player_mdl( custom );
			g_agent_custom_arms    = g_agent_custom_ok ? g_utilities.player_arms_mdl( custom ) : std::string( );
			botox_dbg_log( "AGT: custom %s body=%d arms=%s", custom.c_str( ), g_agent_custom_ok ? 1 : 0,
			               g_agent_custom_arms.empty( ) ? "-" : g_agent_custom_arms.c_str( ) );
		}
		return g_agent_custom_ok;
	};

	const auto apply = [ & ]( int sel, const std::string& custom, bool& prev ) {
		const bool own   = !custom.empty( ) && custom_body( custom );
		const char* rel  = own                            ? custom.c_str( )
		                 : sel > 0 && sel < AGENT_COUNT ? AGENT_MODELS[ sel ]
		                                                : nullptr;
		const char* arms = nullptr;
		if ( rel ) {
			const int idx   = resolve_idx( rel );
			const int cur   = local->get_model_index( );

			if ( model_idx_valid( idx ) && cur != idx ) {
				if ( g_agent_log_left > 0 ) {
					--g_agent_log_left;
					botox_dbg_log( "AGT: set %s idx=%d cur=%d orig=%d applied=%d", rel, idx, cur, g_agent_orig_idx, g_agent_applied_idx );
				}

				if ( cur > 0 && cur != g_agent_applied_idx )
					g_agent_orig_idx = cur;

				local->set_model_index( idx );
				local->invalidate_bone_cache( );

				g_agent_applied_idx = idx;
			}

			if ( model_idx_valid( idx ) ) {
				precache_faction_arms( rel );
				arms_fallback( rel );

				const std::string& own_arms = g_agent_custom_arms;
				if ( own && !own_arms.empty( ) && own_arms.size( ) < sizeof( custom_arm_cfg_t::m_path ) && model_idx_valid( resolve_idx( own_arms.c_str( ) ) ) )
					arms = own_arms.c_str( );
			}
		} else {
			restore_model( );
		}
		custom_arms( arms );
		if ( const bool comp = rel != nullptr; prev != comp ) {
			prev             = comp;
			m_forcing_update = true;
		}
	};

	if ( local->get_team( ) == team_tt )
		apply( GET_VARIABLE( g_variables.m_agent_t, int ), GET_VARIABLE( g_variables.m_agent_t_custom, std::string ), prev_t );
	else if ( local->get_team( ) == team_ct )
		apply( GET_VARIABLE( g_variables.m_agent_ct, int ), GET_VARIABLE( g_variables.m_agent_ct_custom, std::string ), prev_ct );
}

void n_skins::impl_t::knife_changer( )
{
	const auto local = g_ctx.m_local;
	if ( !local )
		return;

	const auto active_weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_active_weapon_handle( ) );
	if ( !active_weapon )
		return;

	const auto weapons = local->get_weapons_handle( );
	if ( !weapons )
		return;

	const bool knife_enable = GET_VARIABLE( WEAPON_VAR( m_knife_enable ), bool );
	const int knife_model   = GET_VARIABLE( WEAPON_VAR( m_knife_model ), int );
	const int knife_paint   = GET_VARIABLE( WEAPON_VAR( m_knife_paint_kit ), int );
	const int knife_seed    = GET_VARIABLE( WEAPON_VAR( m_knife_seed ), int );

	float knife_wear = GET_VARIABLE( WEAPON_VAR( m_knife_wear ), float );

	if ( GET_VARIABLE( WEAPON_VAR( m_knife_custom_color ), bool ) ) {
		const c_color* const knife_colors[ 4 ] = {
			&GET_VARIABLE( WEAPON_VAR( m_knife_color_1 ), c_color ), &GET_VARIABLE( WEAPON_VAR( m_knife_color_2 ), c_color ),
			&GET_VARIABLE( WEAPON_VAR( m_knife_color_3 ), c_color ), &GET_VARIABLE( WEAPON_VAR( m_knife_color_4 ), c_color )
		};

		knife_wear = wear_with_color_key( knife_wear, knife_colors );
	}

	int knife_indexes[ KNIFE_COUNT ];
	fill_knife_indexes( knife_indexes );

	static int prev_knife_model   = -1;
	static int prev_knife_paint   = -1;
	static float prev_knife_wear  = -1.f;
	static int prev_knife_seed    = -1;
	static bool prev_knife_enable = false;
	if ( prev_knife_model != knife_model || prev_knife_paint != knife_paint || prev_knife_wear != knife_wear
	     || prev_knife_seed != knife_seed || prev_knife_enable != knife_enable ) {
		prev_knife_model  = knife_model;
		prev_knife_paint  = knife_paint;
		prev_knife_wear   = knife_wear;
		prev_knife_seed   = knife_seed;
		prev_knife_enable = knife_enable;
		m_forcing_update  = true;
	}

	const bool holding_knife = is_knife_class( class_id_of( active_weapon ) );

	/* knife off / "default": server never resends our netvars, we revert them ourselves */
	const bool knife_active = knife_enable && knife_model > 0 && knife_model < KNIFE_COUNT;

	static bool knife_touched   = false;
	static bool weapons_touched = false;

	if ( knife_active )
		knife_touched = true;

	static const std::string no_name{ };

	const bool weapon_skins_enable = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_enable ), bool );
	if ( weapon_skins_enable )
		weapons_touched = true;

	auto& paint_kits               = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_paint_kit ), std::vector< int > );
	auto& wears                    = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_wear ), std::vector< float > );
	auto& seeds                    = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_seed ), std::vector< int > );
	auto& stattraks                = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_stattrak ), std::vector< bool > );
	auto& stattrak_kills           = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_stattrak_kills ), std::vector< int > );
	auto& custom_names             = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_custom_name ), std::vector< std::string > );
	auto& custom_colors            = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_custom_color ), std::vector< bool > );
	auto& colors_1                 = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_color_1 ), std::vector< c_color > );
	auto& colors_2                 = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_color_2 ), std::vector< c_color > );
	auto& colors_3                 = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_color_3 ), std::vector< c_color > );
	auto& colors_4                 = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_color_4 ), std::vector< c_color > );
	auto& sticker_kits             = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_sticker_kit ), std::vector< int > );
	auto& sticker_wears            = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_sticker_wear ), std::vector< float > );
	auto& sticker_scales           = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_sticker_scale ), std::vector< float > );
	auto& sticker_rotations        = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_sticker_rotation ), std::vector< float > );

	unsigned int weapon_state_hash = weapon_skins_enable ? 2166136261u : 16777619u;

	const auto hash_step = [ & ]( unsigned int value ) {
		weapon_state_hash ^= value;
		weapon_state_hash *= 16777619u;
	};

	for ( int i = 0; i < WEAPON_COUNT; i++ ) {
		if ( i < static_cast< int >( paint_kits.size( ) ) )
			hash_step( static_cast< unsigned int >( paint_kits[ i ] ) );
		if ( i < static_cast< int >( seeds.size( ) ) )
			hash_step( static_cast< unsigned int >( seeds[ i ] ) );
		if ( i < static_cast< int >( wears.size( ) ) )
			hash_step( *reinterpret_cast< const unsigned int* >( &wears[ i ] ) );
		if ( i < static_cast< int >( stattraks.size( ) ) )
			hash_step( stattraks[ i ] ? 1u : 0u );
		if ( i < static_cast< int >( stattrak_kills.size( ) ) )
			hash_step( static_cast< unsigned int >( stattrak_kills[ i ] ) );
		if ( i < static_cast< int >( custom_colors.size( ) ) )
			hash_step( custom_colors[ i ] ? 1u : 0u );

		for ( int slot = 0; slot < STICKER_SLOTS; slot++ ) {
			const int index = i * STICKER_SLOTS + slot;

			if ( index < static_cast< int >( sticker_kits.size( ) ) )
				hash_step( static_cast< unsigned int >( sticker_kits[ index ] ) );
			if ( index < static_cast< int >( sticker_wears.size( ) ) )
				hash_step( *reinterpret_cast< const unsigned int* >( &sticker_wears[ index ] ) );
			if ( index < static_cast< int >( sticker_scales.size( ) ) )
				hash_step( *reinterpret_cast< const unsigned int* >( &sticker_scales[ index ] ) );
			if ( index < static_cast< int >( sticker_rotations.size( ) ) )
				hash_step( *reinterpret_cast< const unsigned int* >( &sticker_rotations[ index ] ) );
			if ( imported_sticker( i, slot ) )
				hash_step( static_cast< unsigned int >( config_sticker( i, slot ).m_kit ) );
		}
	}

	static unsigned int prev_weapon_state_hash = 0;
	if ( prev_weapon_state_hash != weapon_state_hash ) {
		prev_weapon_state_hash = weapon_state_hash;
		m_forcing_update       = true;
	}

	const int account_id = local_account_id( );

	for ( int w = 0; w < 64 && weapons[ w ] != 0xFFFFFFFF; w++ ) {
		const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( weapons[ w ] );
		if ( !weapon )
			continue;

		if ( !weapon_is_ours( weapon, weapons[ w ] ) )
			continue;

		// must clear these or the real inventory item wins over our fallback netvars
		weapon->get_owner_xuid_low( )  = 0;
		weapon->get_owner_xuid_high( ) = 0;
		weapon->get_item_id_high( )    = -1;

		// account id must MATCH us (0 = stattrak error screen)
		weapon->get_account_id( ) = account_id;

		const e_class_ids weapon_class = class_id_of( weapon );

		if ( knife_active ) {
			if ( holding_knife )
				apply_knife_model( weapon, KNIFE_MODELS[ knife_model ] );

			if ( is_knife_class( weapon_class ) ) {
				apply_knife_skin( weapon, KNIFE_IDS[ knife_model ], knife_paint, knife_indexes[ knife_model ], knife_wear );
				weapon->get_fall_back_seed( ) = knife_seed;

				apply_extras( weapon, GET_VARIABLE( WEAPON_VAR( m_knife_stattrak ), bool ),
				              GET_VARIABLE( WEAPON_VAR( m_knife_stattrak_kills ), int ),
				              GET_VARIABLE( WEAPON_VAR( m_knife_custom_name ), std::string ), quality_unusual  );
				continue;
			}
		} else if ( knife_touched && is_knife_class( weapon_class ) ) {
			const bool gold_knife = weapon_class == e_class_ids::c_knife_gg;
			const bool terrorist  = local->get_team( ) == team_tt;

			weapon->get_item_definition_index( ) =
				static_cast< short >( gold_knife ? weapon_knife_gg : ( terrorist ? weapon_knife_t : weapon_knife ) );

			if ( const int vanilla_idx = knife_indexes[ gold_knife ? 17 : ( terrorist ? 16 : 15 ) ]; vanilla_idx > 0 )
				weapon->get_model_index( ) = vanilla_idx;

			weapon->get_fall_back_paint_kit( ) = 0;
			weapon->get_fall_back_seed( )      = 0;
			weapon->get_fall_back_wear( )      = 0.f;

			apply_extras( weapon, false, 0, no_name, quality_default );

			knife_touched = false;
			continue;
		}

		/* feature off, never reach the apply loop */
		if ( !weapon_skins_enable ) {
			if ( weapons_touched ) {
				for ( int i = 0; i < WEAPON_COUNT; i++ ) {
					if ( weapon->get_item_definition_index( ) != WEAPON_IDS[ i ] )
						continue;

					weapon->get_fall_back_paint_kit( ) = 0;
					weapon->get_fall_back_seed( )      = 0;
					weapon->get_fall_back_wear( )      = 0.f;

					apply_extras( weapon, false, 0, no_name, quality_default );
					break;
				}
			}
			continue;
		}

		for ( int i = 0; i < WEAPON_COUNT; i++ ) {
			if ( weapon->get_item_definition_index( ) != WEAPON_IDS[ i ] )
				continue;

			if ( i < static_cast< int >( paint_kits.size( ) ) )
				weapon->get_fall_back_paint_kit( ) = paint_kits[ i ];

			if ( i < static_cast< int >( wears.size( ) ) ) {
				float wear = wears[ i ];

				if ( i < static_cast< int >( custom_colors.size( ) ) && custom_colors[ i ] && i < static_cast< int >( colors_1.size( ) ) &&
				     i < static_cast< int >( colors_2.size( ) ) && i < static_cast< int >( colors_3.size( ) ) &&
				     i < static_cast< int >( colors_4.size( ) ) ) {
					const c_color* const weapon_colors[ 4 ] = { &colors_1[ i ], &colors_2[ i ], &colors_3[ i ], &colors_4[ i ] };
					wear                                    = wear_with_color_key( wear, weapon_colors );
				}

				weapon->get_fall_back_wear( ) = wear;
			}
			if ( i < static_cast< int >( seeds.size( ) ) )
				weapon->get_fall_back_seed( ) = seeds[ i ];

			const bool stattrak     = i < static_cast< int >( stattraks.size( ) ) && stattraks[ i ];
			const int kills         = i < static_cast< int >( stattrak_kills.size( ) ) ? stattrak_kills[ i ] : 0;
			const std::string& name = i < static_cast< int >( custom_names.size( ) ) ? custom_names[ i ] : no_name;

			apply_extras( weapon, stattrak, kills, name, quality_default  );
			break;
		}
	}

	if ( weapon_skins_enable || knife_active ) {
		bool rebuilt_new = false;

		for ( int w = 0; w < 64 && weapons[ w ] != 0xFFFFFFFF; w++ ) {
			const unsigned int handle = weapons[ w ];

			if ( std::find( m_seen_weapons.begin( ), m_seen_weapons.end( ), handle ) != m_seen_weapons.end( ) )
				continue;

			if ( std::find_if( m_pending_weapons.begin( ), m_pending_weapons.end( ),
			                   [ handle ]( const pending_weapon_t& pending ) { return pending.m_handle == handle; } )
			     != m_pending_weapons.end( ) )
				continue;

			// picked up, never composite over their skin
			const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( handle );
			if ( !weapon || !weapon_is_ours( weapon, handle ) )
				continue;

			m_pending_weapons.push_back( { handle, 0 } );
		}

		for ( std::size_t p = 0; p < m_pending_weapons.size( ); ) {
			auto& pending = m_pending_weapons[ p ];

			const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( pending.m_handle );
			if ( !weapon ) {
				m_pending_weapons.erase( m_pending_weapons.begin( ) + static_cast< std::ptrdiff_t >( p ) );
				continue;
			}

			rebuild_custom_materials( weapon );
			pending.m_tries++;
			rebuilt_new = true;

			bool done = pending.m_tries >= NEW_WEAPON_MIN_TRIES;

			if ( done && weapon->get_fall_back_paint_kit( ) > 0 ) {
				const auto cmo = weapon_cmo_offset( );
				done           = cmo && custom_material_count( reinterpret_cast< std::uintptr_t >( weapon ) + cmo ) > 0;
			}

			if ( done || pending.m_tries >= NEW_WEAPON_MAX_TRIES ) {
				m_seen_weapons.push_back( pending.m_handle );
				m_pending_weapons.erase( m_pending_weapons.begin( ) + static_cast< std::ptrdiff_t >( p ) );
				continue;
			}

			p++;
		}

		if ( rebuilt_new ) {
			if ( const auto viewmodel = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_view_model_handle( ) ) )
				static_cast< c_client_networkable* >( viewmodel )->on_data_changed( 0  );
		}
	}

	if ( !weapon_skins_enable )
		weapons_touched = false;
}

void n_skins::impl_t::gloves_changer( )
{
	if ( !g_interfaces.m_engine_client->is_connected( ) && !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	const auto local = g_ctx.m_local;
	if ( !local )
		return;

	const int glove_model = GET_VARIABLE( PLAYER_VAR( m_gloves_model ), int );

	// old csgo players: the old arms carry their own hands, a glove on top = two pairs
	const bool gloves_wanted = GET_VARIABLE( PLAYER_VAR( m_gloves_enable ), bool ) && glove_model > 0 && glove_model < GLOVE_COUNT
	                        && !g_old_mods.players_mounted( );

	const auto wearables = local->get_wearables_handle( );
	if ( !wearables )
		return;

	static unsigned int stored_glove_handle = 0;

	// model / skin swaps need a full update. function scope so teardown can reset them
	// (else re-enabling the same glove never forces a rebuild).
	static int prev_model  = 0;
	static int prev_paint  = -1;
	static int prev_seed   = -1;
	static float prev_wear = -1.f;
	static float regen_at  = 0.f;
	static float re_arm_at = 0.f;

	auto glove = g_interfaces.m_client_entity_list->get< c_base_entity >( wearables[ 0 ] );

	if ( !glove ) {
		if ( const auto our_glove = g_interfaces.m_client_entity_list->get< c_base_entity >( stored_glove_handle ) ) {
			wearables[ 0 ] = stored_glove_handle;
			glove          = our_glove;
		}
	}

	if ( !gloves_wanted || !local->is_alive( ) || !g_interfaces.m_engine_client->is_connected( )
	     || !g_interfaces.m_engine_client->is_in_game( ) ) {
		if ( glove && stored_glove_handle && wearables[ 0 ] == stored_glove_handle ) {
			const auto networkable = static_cast< c_client_networkable* >( glove );
			networkable->set_destroyed_on_recreate_entities( );
			networkable->release( );
			wearables[ 0 ] = 0xFFFFFFFF;
		}

		if ( stored_glove_handle ) {
			stored_glove_handle = 0;
			prev_model          = 0;
			prev_paint          = -1;
			prev_seed           = -1;
			prev_wear           = -1.f;
			m_forcing_update    = true;

			if ( local->is_alive( ) )
				rebuild_player_arms( local, nullptr, "off" );
		}
		regen_at  = 0.f;
		re_arm_at = 0.f;
		return;
	}

	if ( !glove ) {
		const auto entry  = g_interfaces.m_client_entity_list->get_highest_entity_index( ) + 1;
		const auto serial = rand( ) % 0x1000;
		glove             = make_glove( entry, serial );
		if ( !glove )
			return;
		wearables[ 0 ]      = entry | ( serial << 16 );
		stored_glove_handle = wearables[ 0 ];

		m_forcing_update = true;
	}

	// palette rides in the wear: new colours = new composite
	const float wear      = n_skins::glove_wear( );
	const int paint_kit   = GET_VARIABLE( PLAYER_VAR( m_gloves_paint_kit ), int );
	const int seed        = GET_VARIABLE( PLAYER_VAR( m_gloves_seed ), int );
	const int model_index = g_interfaces.m_model_info->get_model_index( GLOVE_MODELS[ glove_model ] );

	const float now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_current_time : 0.f;

	const bool glove_changed = prev_model != glove_model || prev_paint != paint_kit || prev_seed != seed || prev_wear != wear;
	if ( glove_changed ) {
		prev_model       = glove_model;
		prev_paint       = paint_kit;
		prev_seed        = seed;
		prev_wear        = wear;
		m_forcing_update = true;
		regen_at         = now + 0.15f; // sliders move every frame, one composite once they stop
		botox_dbg_log( "GLV: change model %d idx %d paint %d seed %d wear %.4f glove 0x%X", glove_model, model_index, paint_kit, seed,
		               wear, stored_glove_handle );
	}

	apply_glove_model( glove );

	glove->get_item_definition_index( ) = GLOVE_IDS[ glove_model ];
	glove->get_fall_back_paint_kit( )   = paint_kit;
	glove->set_model_index( model_index );
	glove->get_entity_quality( ) = quality_unusual;
	glove->get_fall_back_wear( ) = wear;

	glove->get_item_id_high( )        = -1;
	glove->get_fall_back_seed( )      = seed;
	glove->get_fall_back_stat_trak( ) = STATTRAK_OFF;

	const bool regen = regen_at > 0.f && ( now >= regen_at || now < regen_at - 1.f );
	if ( regen ) {
		regen_at = 0.f;
		regen_glove_material( glove, paint_kit, seed, wear );
	}

	static_cast< c_client_networkable* >( glove )->pre_data_update( 0  );

	// rebuild now, and once more after the skin composite settles
	if ( regen ) {
		rebuild_player_arms( local, glove, "change" );
		re_arm_at = now + 0.5f;
	}
	else if ( re_arm_at > 0.f && ( now >= re_arm_at || now < re_arm_at - 1.f ) ) {
		rebuild_player_arms( local, nullptr, "settle" );
		re_arm_at = 0.f;
	}
}

namespace
{
	/* listen server = one model cache: its view model + knife hold the same graph. what the `flush` command does
	   (CServerGameDLL::InvalidateMdlCache, live slot 33): CBaseAnimating::InvalidateMdlCache = [vt+0x320], GetModelPtr
	   relocks lazily. live server.dll: IServerTools FirstEntity 7 / NextEntity 8, IServerEntity GetModelIndex 6 */
	int drop_server_hdrs( const model_t* const mesh_models[ KNIFE_COUNT ], const int indexes[ KNIFE_COUNT ], unsigned int& held_meshes )
	{
		static void* tools = nullptr;
		if ( !tools ) {
			if ( void* sv = GetModuleHandleA( "server.dll" ) )
				tools = module_t( sv, "server.dll" ).find_interface( "VSERVERTOOLS001" );
			if ( !tools )
				return -1;
		}

		int dropped = 0;
		for ( void* entity = g_virtual.call< void* >( tools, 7 ); entity; entity = g_virtual.call< void*, void* >( tools, 8, entity ) ) {
			const int model_index = g_virtual.call< int >( entity, 6 );
			for ( int i = 1; i < KNIFE_COUNT; i++ ) {
				if ( !mesh_models[ i ] || model_index != indexes[ i ] )
					continue;

				g_virtual.call< void >( entity, 200 );
				held_meshes |= 1u << i;
				dropped++;
				break;
			}
		}
		return dropped;
	}

	/* freed graphs go straight back to the next 0xa8 alloc = the rebuild, and a same address graph keeps every activity
	   map (client + server, studio.cpp ValidateAgainst) on the old donor's table: idle -> draw seqs, inspect gone.
	   live datacache.dll GetVirtualModelFast: g_pMemAlloc Alloc( 0xa8 ) = slot 1, Free = slot 5 */
	constexpr std::size_t VIRTUAL_MODEL_SIZE = 0xa8;
	constexpr int GRAPH_DECOYS               = 8;

	void* mem_alloc( )
	{
		static void** exported = [ ]( ) -> void** {
			const auto tier0 = GetModuleHandleA( "tier0.dll" );
			return tier0 ? reinterpret_cast< void** >( GetProcAddress( tier0, "g_pMemAlloc" ) ) : nullptr;
		}( );
		return exported ? *exported : nullptr;
	}
}

/* a mid map flush crashed because CStudioHdr keeps a raw graph pointer (studio.cpp ResetVModel) on the view model AND
   the knife weapon (SendWeaponAnim picks off the weapon's hdr): draw played, next anim read freed memory. so do what a
   model change does: model 0 (OnNewModel deletes the hdr), flush, model back (new hdr, include resolves the new donor) */
void n_skins::impl_t::knife_anim_live( )
{
	const char* pick           = g_anim_pick.load( std::memory_order_relaxed );
	const unsigned int targets = g_anim_targets_pick.load( std::memory_order_relaxed );
	const char* donor          = g_anim_donor.load( std::memory_order_relaxed );
	const unsigned int live    = g_anim_targets.load( std::memory_order_relaxed );

	if ( pick == donor && targets == live )
		return;

	unsigned int meshes = 0;
	for ( int i = 1; i < KNIFE_COUNT; i++ ) {
		const unsigned int bit = 1u << i;
		if ( ( ( live & bit ) ? donor : nullptr ) != ( ( targets & bit ) ? pick : nullptr ) )
			meshes |= bit;
	}

	g_anim_donor.store( pick, std::memory_order_relaxed );
	g_anim_targets.store( targets, std::memory_order_relaxed );

	if ( !meshes || !g_interfaces.m_model_cache )
		return;

	int indexes[ KNIFE_COUNT ];
	fill_knife_indexes( indexes );

	const model_t* mesh_models[ KNIFE_COUNT ] = { };
	for ( int i = 1; i < KNIFE_COUNT; i++ ) {
		if ( ( meshes & ( 1u << i ) ) && indexes[ i ] > 0 )
			mesh_models[ i ] = g_interfaces.m_model_info->get_model( indexes[ i ] );
	}

	struct held_t {
		c_base_entity* m_entity;
		int m_index;
	};
	std::vector< held_t > held;
	unsigned int held_meshes = 0;

	for ( int e = 1; e <= g_interfaces.m_client_entity_list->get_highest_entity_index( ); e++ ) {
		const auto entity = g_interfaces.m_client_entity_list->get< c_base_entity >( e );
		const model_t* model = entity ? entity->get_model( ) : nullptr;
		if ( !model )
			continue;

		for ( int i = 1; i < KNIFE_COUNT; i++ ) {
			if ( model != mesh_models[ i ] )
				continue;

			held.push_back( { entity, indexes[ i ] } );
			held_meshes |= 1u << i;
			entity->set_model_index( 0 );
			break;
		}
	}

	const auto nci       = g_interfaces.m_engine_client->get_net_channel_info( );
	const int server_held = nci && nci->is_loopback( ) ? drop_server_hdrs( mesh_models, indexes, held_meshes ) : 0;

	unsigned short handles[ KNIFE_COUNT ];
	const void* old_graphs[ KNIFE_COUNT ] = { };
	for ( int i = 1; i < KNIFE_COUNT; i++ ) {
		handles[ i ] = 0xffff;
		if ( !( meshes & ( 1u << i ) ) )
			continue;

		// fresh handle + our own ref: a cached one can be recycled into another model
		handles[ i ] = g_interfaces.m_model_cache->find_mdl( KNIFE_MODELS[ i ] );
		if ( handles[ i ] == 0xffff )
			continue;

		if ( held_meshes & ( 1u << i ) )
			old_graphs[ i ] = g_interfaces.m_model_cache->get_virtual_model( handles[ i ] );

		g_interfaces.m_model_cache->flush( handles[ i ], c_model_cache::flush_virtual_model );
	}

	void* const allocator = mem_alloc( );
	void* decoys[ GRAPH_DECOYS ] = { };
	for ( auto& decoy : decoys )
		decoy = allocator ? g_virtual.call< void*, std::size_t >( allocator, 1, VIRTUAL_MODEL_SIZE ) : nullptr;

	for ( const auto& entry : held )
		entry.m_entity->set_model_index( entry.m_index );

	int same = 0;
	for ( int i = 1; i < KNIFE_COUNT; i++ ) {
		if ( handles[ i ] == 0xffff )
			continue;

		// server-only holders rebuild lazily, after the decoys are gone: build theirs now too
		if ( held_meshes & ( 1u << i ) ) {
			const void* fresh = g_interfaces.m_model_cache->get_virtual_model( handles[ i ] );
			same += fresh == old_graphs[ i ];
			botox_dbg_log( "KANIM: mesh %d graph %p -> %p same=%d donor %s", i, old_graphs[ i ], fresh, fresh == old_graphs[ i ] ? 1 : 0,
			               pick ? pick : "stock" );
		}

		g_interfaces.m_model_cache->release( handles[ i ] );
	}

	int decoyed = 0;
	for ( void* decoy : decoys ) {
		if ( !decoy )
			continue;
		for ( int i = 1; i < KNIFE_COUNT; i++ )
			decoyed += decoy == old_graphs[ i ];
		g_virtual.call< void, void* >( allocator, 5, decoy );
	}

	botox_dbg_log( "KANIM: live meshes 0x%X held %d server %d same %d decoyed %d alloc %p", meshes, static_cast< int >( held.size( ) ),
	               server_held, same, decoyed, allocator );

	g_knife_view.m_view_model = nullptr;

	// arms bone merge onto the view model and cached its old hdr's merge flags: rebuild them like the glove changer does
	const auto local = g_ctx.m_local;
	if ( !local || !local->is_alive( ) )
		return;

	const auto viewmodel = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_view_model_handle( ) );
	for ( const auto& entry : held ) {
		if ( entry.m_entity == viewmodel ) {
			rebuild_player_arms( local, nullptr, "knife anims" );
			break;
		}
	}
}

namespace
{
	/* stattrak digits sample an orange vtf, $color2 alone can't make them blue. white copy ( every dxt5 endpoint = its
	   brightest channel as gray, peak scaled to white ) saved to csgo\materials\stattrak_color\ ( same files as
	   StattrakColor.lua ), digit materials draw it tinted by $color2. one viewmodel at a time = color of the held weapon */
	struct stattrak_group_t {
		const char* m_source;
		const char* m_white;
		const char* m_original;
		std::vector< std::string > m_materials;
		c_texture* m_texture = nullptr;
		bool m_failed        = false;
		bool m_applied       = false;
	};

	std::vector< std::string > stattrak_digit_materials( )
	{
		std::vector< std::string > out{ };
		for ( const char* dir : { "models/weapons/stattrack/stat_digit", "models/weapons/stattrack_weaponpreview/stat_digit" } ) {
			for ( int i = 0; i < 6; i++ ) {
				char name[ 96 ];
				std::snprintf( name, sizeof( name ), "%s%03d", dir, i );
				out.emplace_back( name );
			}
		}
		return out;
	}

	stattrak_group_t g_stattrak_groups[ ] = {
		{ "materials/models/weapons/stattrack/stat_digit.vtf", "stattrak_color/digit_white", "models/weapons/stattrack/stat_digit",
		  stattrak_digit_materials( ) },
		{ "materials/models/weapons/stattrack/stattrak_error.vtf", "stattrak_color/error_white", "models/weapons/stattrack/stattrak_error",
		  { "models/weapons/stattrack/stattrak_error001" } },
	};

	struct stattrak_material_t {
		c_material* m_material;
		float m_color2[ 3 ] = { -1.f, -1.f, -1.f }; // last written, -1 = never
	};
	std::unordered_map< std::string, stattrak_material_t > g_stattrak_materials{ }; // referenced: material + our vars stay loaded
	bool g_stattrak_prepared      = false;
	bool g_stattrak_warned        = false;
	unsigned int g_stattrak_key   = 0;
	float g_stattrak_next_check   = 0.f;

	bool whiten_vtf( std::vector< unsigned char >& vtf )
	{
		const auto u16 = [ & ]( std::size_t at ) { return static_cast< unsigned int >( vtf[ at ] | vtf[ at + 1 ] << 8 ); };
		const auto u32 = [ & ]( std::size_t at ) { return u16( at ) | u16( at + 2 ) << 16; };

		// 7.3+ header, IMAGE_FORMAT_DXT5 = 15
		if ( vtf.size( ) < 104 || std::memcmp( vtf.data( ), "VTF\0", 4 ) != 0 || u32( 4 ) != 7 || u32( 8 ) < 3 || u32( 52 ) != 15 )
			return false;

		const unsigned int width = u16( 16 ), height = u16( 18 ), frames = u16( 24 ), mips = vtf[ 56 ];

		std::size_t start = 0;
		for ( unsigned int i = 0, count = u32( 68 ); i < count; i++ ) {
			const std::size_t entry = 80 + i * 8;
			if ( entry + 8 > vtf.size( ) )
				return false;
			if ( vtf[ entry ] == 0x30 && vtf[ entry + 1 ] == 0 && vtf[ entry + 2 ] == 0 )
				start = u32( entry + 4 );
		}
		if ( !start )
			return false;

		std::size_t size = 0;
		for ( unsigned int m = 0; m < mips; m++ )
			size += static_cast< std::size_t >( ( ( std::max )( 1u, width >> m ) + 3 ) / 4 ) * ( ( ( std::max )( 1u, height >> m ) + 3 ) / 4 ) * 16;
		size *= frames;
		if ( !size || start + size > vtf.size( ) )
			return false;

		const auto brightest = []( unsigned int c ) {
			return ( std::max )( { ( ( c >> 11 ) & 31 ) * 255 / 31, ( ( c >> 5 ) & 63 ) * 255 / 63, ( c & 31 ) * 255 / 31 } );
		};

		unsigned int peak = 1;
		for ( std::size_t o = start; o < start + size; o += 16 )
			peak = ( std::max )( { peak, brightest( u16( o + 8 ) ), brightest( u16( o + 10 ) ) } );

		// dxt5 color blocks always interpolate 4 colors, grays blend like the old endpoints did: digits, glow, dim cells stay put
		for ( std::size_t o = start; o < start + size; o += 16 ) {
			for ( const std::size_t at : { o + 8, o + 10 } ) {
				const unsigned int v    = ( std::min )( 255u, ( brightest( u16( at ) ) * 255 + peak / 2 ) / peak );
				const unsigned int r5   = ( v * 31 + 127 ) / 255, g6 = ( v * 63 + 127 ) / 255;
				const unsigned int gray = r5 << 11 | g6 << 5 | r5;
				vtf[ at ]               = static_cast< unsigned char >( gray & 0xff );
				vtf[ at + 1 ]           = static_cast< unsigned char >( gray >> 8 );
			}
		}
		return true;
	}

	std::string stattrak_dir( )
	{
		char exe[ MAX_PATH ]{ };
		if ( !GetModuleFileNameA( nullptr, exe, MAX_PATH ) )
			return { };

		std::string root = exe;
		const auto slash = root.find_last_of( "\\/" );
		if ( slash == std::string::npos )
			return { };
		root.erase( slash );

		CreateDirectoryA( ( root + "\\csgo\\materials" ).c_str( ), nullptr );
		root += "\\csgo\\materials\\stattrak_color";
		CreateDirectoryA( root.c_str( ), nullptr );
		return root + "\\";
	}

	bool is_vtf_file( const std::string& path )
	{
		std::ifstream file( path, std::ios::binary );
		char head[ 4 ]{ };
		return file.read( head, 4 ) && std::memcmp( head, "VTF\0", 4 ) == 0;
	}

	void stattrak_prepare( )
	{
		const std::string dir = stattrak_dir( );

		for ( auto& group : g_stattrak_groups ) {
			const std::string path = dir + ( std::strrchr( group.m_white, '/' ) + 1 ) + ".vtf";
			if ( dir.empty( ) ) {
				group.m_failed = true;
				continue;
			}
			if ( is_vtf_file( path ) )
				continue;

			std::vector< unsigned char > vtf{ };
			if ( !read_game_file( group.m_source, vtf ) || !whiten_vtf( vtf ) ) {
				group.m_failed = true;
				g_logger.print( std::string( "can't make a white copy of " ) + group.m_source + "\n", "[stattrak]" );
				continue;
			}

			std::ofstream out( path, std::ios::binary | std::ios::trunc );
			if ( !out.write( reinterpret_cast< const char* >( vtf.data( ) ), static_cast< std::streamsize >( vtf.size( ) ) ) ) {
				out.close( );
				DeleteFileA( path.c_str( ) );
				group.m_failed = true;
				g_logger.print( "can't write " + path + "\n", "[stattrak]" );
			}
		}
	}

	c_texture* stattrak_white( stattrak_group_t& group )
	{
		if ( group.m_failed )
			return nullptr;

		if ( !group.m_texture ) {
			group.m_texture = g_interfaces.m_material_system->find_texture( group.m_white, TEXTURE_GROUP_MODEL, false );
			if ( !group.m_texture ) {
				group.m_failed = true;
				return nullptr;
			}
			group.m_texture->increment_reference_count( );
		}

		// every check: sv_pure without the bypass turns loose files into the error texture
		if ( group.m_texture->is_error( ) ) {
			if ( !g_stattrak_warned ) {
				g_stattrak_warned = true;
				g_logger.print( "csgo won't load materials\\stattrak_color ( sv_pure server: turn on sv_pure bypass )\n", "[stattrak]" );
			}
			return nullptr;
		}
		return group.m_texture;
	}

	stattrak_material_t* stattrak_material( const std::string& name )
	{
		if ( const auto it = g_stattrak_materials.find( name ); it != g_stattrak_materials.end( ) )
			return &it->second;

		c_material* const material = g_interfaces.m_material_system->find_material( name.c_str( ), TEXTURE_GROUP_MODEL, false );
		if ( !material || material->is_error_material( ) )
			return nullptr;

		material->increment_reference_count( );
		return &( g_stattrak_materials[ name ] = stattrak_material_t{ material } );
	}

	void stattrak_set( stattrak_material_t& entry, c_texture* texture, float r, float g, float b )
	{
		c_material* const material = entry.m_material;

		bool found = false;
		if ( c_material_var* const var = material->find_var( "$basetexture", &found, false ); found && var && texture && var->get_texture( ) != texture ) {
			var->set_texture( texture );
			entry.m_color2[ 0 ] = -1.f; // stock texture back = material reloaded, its $color2 too
		}

		/* csgo bakes $color2 into the material's per instance command buffer ( BaseShader.cpp PI_SetModulation* ->
		   ApplyColor2Factor ), built only while snapshotting: a write alone never shows ( white digits ). rebuild on change,
		   like misc.cpp world modulation. queued to the render thread with the write, no-op before precache */
		if ( entry.m_color2[ 0 ] == r && entry.m_color2[ 1 ] == g && entry.m_color2[ 2 ] == b )
			return;

		found = false;
		if ( c_material_var* const var = material->find_var( "$color2", &found, false ); found && var ) {
			var->set_vector( r, g, b );
			material->recompute_state_snapshots( );
			entry.m_color2[ 0 ] = r;
			entry.m_color2[ 1 ] = g;
			entry.m_color2[ 2 ] = b;
		}
	}

	void stattrak_restore( stattrak_group_t& group )
	{
		c_texture* original = g_interfaces.m_material_system->find_texture( group.m_original, TEXTURE_GROUP_MODEL, false );
		if ( original && original->is_error( ) )
			original = nullptr;

		for ( const std::string& name : group.m_materials ) {
			if ( const auto it = g_stattrak_materials.find( name ); it != g_stattrak_materials.end( ) )
				stattrak_set( it->second, original, 1.f, 1.f, 1.f );
		}
		group.m_applied = false;
	}

	void stattrak_apply( const c_color& color )
	{
		if ( !g_stattrak_prepared ) {
			g_stattrak_prepared = true;
			stattrak_prepare( );
		}

		const float r = color.get< 0 >( ) / 255.f, g = color.get< 1 >( ) / 255.f, b = color.get< 2 >( ) / 255.f;

		for ( auto& group : g_stattrak_groups ) {
			if ( c_texture* const texture = stattrak_white( group ) ) {
				for ( const std::string& name : group.m_materials ) {
					if ( stattrak_material_t* const entry = stattrak_material( name ) )
						stattrak_set( *entry, texture, r, g, b );
				}
				group.m_applied = true;
			} else if ( group.m_applied )
				stattrak_restore( group );
		}
	}

	void stattrak_restore_all( )
	{
		for ( auto& group : g_stattrak_groups ) {
			if ( group.m_applied )
				stattrak_restore( group );
		}
	}
}

void n_skins::impl_t::stattrak_color( )
{
	if ( g_ctx.m_eject_restoring ) {
		stattrak_restore_all( );

		for ( auto& group : g_stattrak_groups ) {
			if ( group.m_texture ) {
				group.m_texture->decrement_reference_count( );
				group.m_texture = nullptr;
			}
		}
		for ( const auto& [ name, entry ] : g_stattrak_materials )
			entry.m_material->decrement_reference_count( );
		g_stattrak_materials.clear( );
		return;
	}

	c_color color{ };
	bool custom = false;

	c_base_entity* const local  = g_interfaces.m_engine_client->is_connected_safe( )
	                                  ? g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) )
	                                  : nullptr;
	c_base_entity* const weapon = local && local->is_alive( ) ? g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_active_weapon_handle( ) )
	                                                          : nullptr;

	if ( weapon && !is_knife_class( class_id_of( weapon ) ) && GET_VARIABLE( WEAPON_VAR( m_weapon_skins_enable ), bool ) ) {
		const auto& stattraks = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_stattrak ), std::vector< bool > );
		const auto& customs   = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_stattrak_custom_color ), std::vector< bool > );
		const auto& colors    = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_stattrak_color ), std::vector< c_color > );

		for ( int i = 0; i < WEAPON_COUNT; i++ ) {
			if ( weapon->get_item_definition_index( ) != WEAPON_IDS[ i ] )
				continue;

			custom = i < static_cast< int >( stattraks.size( ) ) && stattraks[ i ] && i < static_cast< int >( customs.size( ) ) && customs[ i ] &&
			         i < static_cast< int >( colors.size( ) );
			if ( custom )
				color = colors[ i ];
			break;
		}
	}

	/* swapping $basetexture on digits the map already loaded doesn't take on the 1st map. like StattrakColor.lua: white mask
	   goes on from the menu as soon as ANY custom color is set up ( either team ), weapon switches only move $color2 */
	const auto weapons_custom = []( bool tside ) {
		if ( !GET_VARIABLE( tside ? g_variables.m_weapon_skins_enable_tside : g_variables.m_weapon_skins_enable, bool ) )
			return false;
		const auto& stattraks = GET_VARIABLE( tside ? g_variables.m_weapon_skins_stattrak_tside : g_variables.m_weapon_skins_stattrak, std::vector< bool > );
		const auto& customs   = GET_VARIABLE( tside ? g_variables.m_weapon_skins_stattrak_custom_color_tside : g_variables.m_weapon_skins_stattrak_custom_color,
		                                      std::vector< bool > );
		for ( std::size_t i = 0; i < stattraks.size( ) && i < customs.size( ); i++ ) {
			if ( stattraks[ i ] && customs[ i ] )
				return true;
		}
		return false;
	};
	const bool any_custom = weapons_custom( false ) || weapons_custom( true );

	// white mask * this = the stock look ( StattrakColor.lua default )
	if ( !custom )
		color = c_color( 230, 110, 30, 255 );

	const unsigned int key = any_custom ? 0x01000000u | color.get< 0 >( ) | color.get< 1 >( ) << 8 | color.get< 2 >( ) << 16 : 0u;

	// material reloads ( map change, sv_pure, mat_reloadallmaterials ) put the stock texture back: recheck twice a second
	const float now = g_interfaces.m_global_vars_base->m_real_time;
	if ( key == g_stattrak_key && now < g_stattrak_next_check )
		return;
	g_stattrak_key        = key;
	g_stattrak_next_check = now + 0.5f;

	if ( any_custom )
		stattrak_apply( color );
	else
		stattrak_restore_all( );
}
