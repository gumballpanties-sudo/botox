#include "skins_internal.h"

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

	if ( !GET_VARIABLE( g_variables.m_agent_enable, bool ) ) {
		restore_model( );

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

		// gloves changer on: it draws hands, spend the string on a sleeve; off: must be hands
		const bool gloves_active = GET_VARIABLE( g_variables.m_gloves_enable, bool )
		                        && GET_VARIABLE( g_variables.m_gloves_model, int ) > 0;

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
			botox_dbg_log( "AGT: custom %s body=%d", custom.c_str( ), g_agent_custom_ok ? 1 : 0 );
		}
		return g_agent_custom_ok;
	};

	const auto apply = [ & ]( int sel, const std::string& custom, bool& prev ) {
		const char* rel = !custom.empty( ) && custom_body( custom ) ? custom.c_str( )
		                : sel > 0 && sel < AGENT_COUNT          ? AGENT_MODELS[ sel ]
		                                                         : nullptr;
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
			}
		} else {
			restore_model( );
		}
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

	const bool knife_enable = GET_VARIABLE( g_variables.m_knife_enable, bool );
	const int knife_model   = GET_VARIABLE( g_variables.m_knife_model, int );
	const int knife_paint   = GET_VARIABLE( g_variables.m_knife_paint_kit, int );
	const int knife_seed    = GET_VARIABLE( g_variables.m_knife_seed, int );

	float knife_wear = GET_VARIABLE( g_variables.m_knife_wear, float );

	if ( GET_VARIABLE( g_variables.m_knife_custom_color, bool ) ) {
		const c_color* const knife_colors[ 4 ] = {
			&GET_VARIABLE( g_variables.m_knife_color_1, c_color ), &GET_VARIABLE( g_variables.m_knife_color_2, c_color ),
			&GET_VARIABLE( g_variables.m_knife_color_3, c_color ), &GET_VARIABLE( g_variables.m_knife_color_4, c_color )
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

	const bool weapon_skins_enable = GET_VARIABLE( g_variables.m_weapon_skins_enable, bool );
	if ( weapon_skins_enable )
		weapons_touched = true;

	auto& paint_kits               = GET_VARIABLE( g_variables.m_weapon_skins_paint_kit, std::vector< int > );
	auto& wears                    = GET_VARIABLE( g_variables.m_weapon_skins_wear, std::vector< float > );
	auto& seeds                    = GET_VARIABLE( g_variables.m_weapon_skins_seed, std::vector< int > );
	auto& stattraks                = GET_VARIABLE( g_variables.m_weapon_skins_stattrak, std::vector< bool > );
	auto& stattrak_kills           = GET_VARIABLE( g_variables.m_weapon_skins_stattrak_kills, std::vector< int > );
	auto& custom_names             = GET_VARIABLE( g_variables.m_weapon_skins_custom_name, std::vector< std::string > );
	auto& custom_colors            = GET_VARIABLE( g_variables.m_weapon_skins_custom_color, std::vector< bool > );
	auto& colors_1                 = GET_VARIABLE( g_variables.m_weapon_skins_color_1, std::vector< c_color > );
	auto& colors_2                 = GET_VARIABLE( g_variables.m_weapon_skins_color_2, std::vector< c_color > );
	auto& colors_3                 = GET_VARIABLE( g_variables.m_weapon_skins_color_3, std::vector< c_color > );
	auto& colors_4                 = GET_VARIABLE( g_variables.m_weapon_skins_color_4, std::vector< c_color > );
	auto& sticker_kits             = GET_VARIABLE( g_variables.m_weapon_skins_sticker_kit, std::vector< int > );
	auto& sticker_wears            = GET_VARIABLE( g_variables.m_weapon_skins_sticker_wear, std::vector< float > );
	auto& sticker_scales           = GET_VARIABLE( g_variables.m_weapon_skins_sticker_scale, std::vector< float > );
	auto& sticker_rotations        = GET_VARIABLE( g_variables.m_weapon_skins_sticker_rotation, std::vector< float > );

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

				apply_extras( weapon, GET_VARIABLE( g_variables.m_knife_stattrak, bool ),
				              GET_VARIABLE( g_variables.m_knife_stattrak_kills, int ),
				              GET_VARIABLE( g_variables.m_knife_custom_name, std::string ), quality_unusual  );
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

namespace
{
	using equip_wearable_t = void( __thiscall* )( void* wearable, void* owner );
	using this_only_t      = void( __thiscall* )( void* self );

	// hands loadout lookup (0x3E0840) reads CS game rules unchecked: null while loading / after disconnect = crash
	bool game_rules_up( )
	{
		static const auto site =
			g_modules[ CLIENT_DLL ].find_pattern( "8B 4D 04 E8 ? ? ? ? 8B 0D ? ? ? ? 8B 01 FF 90 98 04 00 00 C6 45 FC 00" );
		return site && **reinterpret_cast< void*** >( site + 10 );
	}

	/* evolve's glove apply. unique on the final client.dll: CEconWearable::Equip (0x723170),
	   C_CSPlayer::InvalidateViewModelArmConfig (0x3EF290, nulls cfg + drops arms on all 3 viewmodels),
	   C_BaseViewModel::UpdateAllViewmodelAddons (0x215080, rebuilds arms from the equipped glove). glove null = no equip. */
	void rebuild_arms( c_base_entity* local, c_base_entity* glove, const char* why )
	{
		if ( !g_interfaces.m_engine_client->is_in_game( ) || !game_rules_up( ) ) {
			botox_dbg_log( "GLV: arms %s skipped, game rules down", why );
			return;
		}

		static const auto equip = reinterpret_cast< equip_wearable_t >(
			g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 EC 10 53 8B 5D 08 57 8B F9" ) );
		static const auto invalidate_arms = reinterpret_cast< this_only_t >(
			g_modules[ CLIENT_DLL ].find_pattern( "51 56 57 8B F9 33 F6 C7 87 ? ? ? ? 00 00 00 00 56 8B CF E8" ) );
		static const auto update_addons = reinterpret_cast< this_only_t >(
			g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 E4 F8 83 EC 2C 53 8B D9 56 57 8B 03 FF 90 F0 03 00 00 8B F8" ) );

		const auto viewmodel = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_view_model_handle( ) );

		botox_dbg_log( "GLV: arms %s equip %p inval %p addons %p vm %p glove %p", why, equip, invalidate_arms, update_addons, viewmodel,
		               glove );

		if ( glove && equip )
			equip( glove, local );
		if ( invalidate_arms )
			invalidate_arms( local );
		if ( viewmodel && update_addons )
			update_addons( viewmodel );
	}
}

void n_skins::impl_t::gloves_changer( )
{
	if ( !g_interfaces.m_engine_client->is_connected( ) && !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	const auto local = g_ctx.m_local;
	if ( !local )
		return;

	const int glove_model = GET_VARIABLE( g_variables.m_gloves_model, int );

	const bool gloves_wanted = GET_VARIABLE( g_variables.m_gloves_enable, bool ) && glove_model > 0 && glove_model < GLOVE_COUNT;

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
				rebuild_arms( local, nullptr, "off" );
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

	const float wear      = GET_VARIABLE( g_variables.m_gloves_wear, float );
	const int paint_kit   = GET_VARIABLE( g_variables.m_gloves_paint_kit, int );
	const int seed        = GET_VARIABLE( g_variables.m_gloves_seed, int );
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

		const auto glove_view = item_view_of( glove );
		const auto view_cmo   = glove_view + ITEM_VIEW_CMO;
		const auto cmo        = weapon_cmo_offset( );
		const auto glove_cmo  = cmo ? reinterpret_cast< std::uintptr_t >( glove ) + cmo : 0;

		set_view_attribute( glove_view, "set item texture prefab", static_cast< float >( paint_kit > 0 ? paint_kit : 0 ) );
		set_view_attribute( glove_view, "set item texture seed", static_cast< float >( paint_kit > 0 ? seed : 0 ) );
		set_view_attribute( glove_view, "set item texture wear", paint_kit > 0 ? wear : 0.f );

		// game composites a glove once (InitializeAttributes), arms copy the old one off the entity
		clear_owner( reinterpret_cast< void* >( view_cmo ) );
		if ( glove_cmo )
			clear_owner( reinterpret_cast< void* >( glove_cmo ) );

		// arms take the FIRST visuals processor by name (0x717850), stale ones win. ponytail: old ones leak, Release is non-virtual
		constexpr std::uintptr_t ITEM_VIEW_PROCESSOR_COUNT = 0x23C;
		*reinterpret_cast< int* >( glove_view + ITEM_VIEW_PROCESSOR_COUNT ) = 0;

		if ( paint_kit > 0 ) {
			if ( const auto generate = get_update_generated_material( ) )
				generate( reinterpret_cast< void* >( glove_view ), 0, 0xFFFF, 9 );
		}

		if ( glove_cmo && custom_material_count( view_cmo ) > 0 && custom_material_count( glove_cmo ) <= 0 ) {
			using duplicate_t = void( __thiscall* )( void* owner, void* other );
			const auto vtable = *reinterpret_cast< void*** >( view_cmo );
			if ( vtable && vtable[ 3 ] )
				reinterpret_cast< duplicate_t >( vtable[ 3 ] )( reinterpret_cast< void* >( view_cmo ), reinterpret_cast< void* >( glove_cmo ) );
		}

		botox_dbg_log( "GLV: regen paint %d view mats %d glove mats %d procs %d", paint_kit, custom_material_count( view_cmo ),
		               glove_cmo ? custom_material_count( glove_cmo ) : -1, *reinterpret_cast< int* >( glove_view + ITEM_VIEW_PROCESSOR_COUNT ) );
	}

	static_cast< c_client_networkable* >( glove )->pre_data_update( 0  );

	// rebuild now, and once more after the skin composite settles
	if ( regen ) {
		rebuild_arms( local, glove, "change" );
		re_arm_at = now + 0.5f;
	}
	else if ( re_arm_at > 0.f && ( now >= re_arm_at || now < re_arm_at - 1.f ) ) {
		rebuild_arms( local, nullptr, "settle" );
		re_arm_at = 0.f;
	}
}
