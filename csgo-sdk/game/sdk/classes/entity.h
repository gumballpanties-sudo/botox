#pragma once
#include "../../../globals/netvars/netvars.h"
#include "../../../utilities/memory/virtual.h"
#include "../structs/var_mapping.h"
#include "c_breakable_with_prop_data.h"
#include "c_utl_vector.h"

enum class e_class_ids;

class c_vector;
class c_angle;
class c_client_unknown;
class c_game_trace;
class c_vector;
class c_angle;
class bf_read;

struct ray_t;
struct model_t;
struct matrix3x4_t;
struct mstudiohitboxset_t;
struct data_map_t;
struct bounding_box_t;

class c_handle_entity
{
public:
	virtual ~c_handle_entity( ) { }
	virtual void set_ref_ehandle( const unsigned int& ref ) = 0;
	virtual const unsigned int& get_ref_ehandle( ) const    = 0;
};

class c_collideable
{
public:
	virtual c_handle_entity* get_entity_handle( )                                                 = 0;
	virtual c_vector& get_obb_mins( )                                                             = 0;
	virtual c_vector& get_obb_maxs( )                                                             = 0;
	virtual void world_space_trigger_bounds( c_vector* world_mins, c_vector* world_maxs ) const   = 0;
	virtual bool test_collision( const ray_t& ray, unsigned int contents_mask, c_game_trace& tr ) = 0;
	virtual bool test_hitboxes( const ray_t& ray, unsigned int contents_mask, c_game_trace& tr )  = 0;
	virtual int get_collision_model_index( )                                                      = 0;
	virtual const model_t* get_collision_model( )                                                 = 0;
	virtual c_vector& get_collision_origin( ) const                                               = 0;
	virtual c_angle& get_collision_angles( ) const                                                = 0;
	virtual const matrix3x4_t& collision_to_world_transform( ) const                              = 0;
	virtual int get_solid( ) const                                                                = 0;
	virtual int get_solid_flags( ) const                                                          = 0;
	virtual c_client_unknown* get_i_client_unknown( )                                             = 0;
	virtual int get_collision_group( ) const                                                      = 0;

	virtual void world_space_surrounding_bounds( c_vector* mins, c_vector* maxs ) = 0;
	virtual unsigned int get_required_trigger_flags( ) const                      = 0;
	virtual const matrix3x4_t* get_root_parent_to_world_transform( ) const        = 0;
	virtual void* get_v_physics_object( ) const                                   = 0;
};

class c_client_alpha_property
{
public:
	virtual c_client_unknown* get_i_client_unknown( )                                              = 0;
	virtual void set_alpha_modulation( unsigned char alpha )                                       = 0;
	virtual void set_render_fx( int render_fx, int render_mode, float start_time, float duration ) = 0;
	virtual void set_fade( float global_fade_scale, float dist_fade_start, float dist_fade_end )   = 0;
	virtual void set_desync_offset( int offset )                                                   = 0;
	virtual void enable_alpha_modulation_override( bool enable )                                   = 0;
	virtual void enable_shadow_alpha_modulation_override( bool enable )                            = 0;
	virtual void set_distance_fade_mode( int fade_mode )                                           = 0;
};

class c_client_networkable;
class c_client_renderable;
class c_client_entity;
class c_base_entity;
class c_client_thinkable;
class c_user_cmd;
class c_animation_state;

class c_client_unknown : public c_handle_entity
{
public:
	virtual c_collideable* get_collideable( )                     = 0;
	virtual c_client_networkable* get_client_networkable( )       = 0;
	virtual c_client_renderable* get_client_renderable( )         = 0;
	virtual c_client_entity* get_client_entity( )                 = 0;
	virtual c_base_entity* get_base_entity( )                     = 0;
	virtual c_client_thinkable* get_client_thinkable( )           = 0;
	virtual c_client_alpha_property* get_client_alpha_property( ) = 0;
};

struct renderable_instance_t {
	unsigned char m_alpha;
};

class c_client_renderable
{
public:
	virtual c_client_unknown* get_client_unknown( )                            = 0;
	virtual c_vector& get_render_origin( )                                     = 0;
	virtual c_angle& get_render_angles( )                                      = 0;
	virtual bool should_draw( )                                                = 0;
	virtual int get_render_flags( )                                            = 0;
	virtual bool is_transparent( )                                             = 0;
	virtual unsigned short get_shadow_handle( ) const                          = 0;
	virtual unsigned short& render_handle( )                                   = 0;
	virtual const model_t* get_model( ) const                                  = 0;
	virtual int draw_model( int flags, const renderable_instance_t& instance ) = 0;
	virtual int get_body( )                                                    = 0;
	virtual void get_color_modulation( float* color )                          = 0;
	virtual bool lod_test( )                                                   = 0;
	virtual bool setup_bones(
		matrix3x4_t* bone_to_world_out, int max_bones, int bone_mask,
		float
			current_time ) = 0;
	virtual void setup_weights( const matrix3x4_t* bone_to_world, int flex_weight_count, float* flex_weights, float* flex_delayed_weights ) = 0;
	virtual void do_animation_events( )                                                                                                     = 0;
	virtual void* get_pvs_notify_interface( )                                                                                               = 0;
	virtual void get_render_bounds( c_vector& mins, c_vector& maxs )                                                                        = 0;
	virtual void get_render_bounds_worldspace( c_vector& mins, c_vector& maxs )                                                             = 0;
	virtual void get_shadow_render_bounds( c_vector& mins, c_vector& maxs, int shadow_type )                                                = 0;
	virtual bool should_receive_projected_textures( int flags )                                                                             = 0;
	virtual bool get_shadow_cast_distance( float* distance, int shadow_type ) const                                                         = 0;
	virtual bool get_shadow_cast_direction( c_vector* direction, int shadow_type ) const                                                    = 0;
	virtual bool is_shadow_dirty( )                                                                                                         = 0;
	virtual void mark_shadow_dirty( bool dirty )                                                                                            = 0;
	virtual c_client_renderable* get_shadow_parent( )                                                                                       = 0;
	virtual c_client_renderable* first_shadow_child( )                                                                                      = 0;
	virtual c_client_renderable* next_shadow_peer( )                                                                                        = 0;
	virtual int shadow_cast_type( )                                                                                                         = 0;
	virtual void unused2( ) { }
	virtual void create_model_instance( )                                         = 0;
	virtual unsigned short get_model_instance( )                                  = 0;
	virtual const matrix3x4_t& renderable_to_world_transform( )                   = 0;
	virtual int lookup_attachment( const char* attachment_name )                  = 0;
	virtual bool get_attachment( int index, c_vector& origin, c_angle& aview )    = 0;
	virtual bool get_attachment( int index, matrix3x4_t& attachment )             = 0;
	virtual bool compute_lighting_origin( int attachment_index, c_vector model_lighting_center, const matrix3x4_t& matrix,
	                                      c_vector& transformed_lighting_center ) = 0;
	virtual float* get_render_clip_plane( )                                       = 0;
	virtual int get_skin( )                                                       = 0;
	virtual void on_threaded_draw_setup( )                                        = 0;
	virtual bool uses_flex_delayed_weights( )                                     = 0;
	virtual void record_tool_message( )                                           = 0;
	virtual bool should_draw_for_split_screen_user( int slot )                    = 0;
	virtual unsigned char override_alpha_modulation( unsigned char alpha )        = 0;
	virtual unsigned char override_shadow_alpha_modulation( unsigned char alpha ) = 0;
	virtual void* get_client_model_renderable( )                                  = 0;
};

class c_base_client;
class c_client_networkable
{
public:
	virtual c_client_unknown* get_client_unknown( )                       = 0;
	virtual void release( )                                               = 0;
	virtual c_base_client* get_client_class( )                            = 0;
	virtual void notify_should_transmit( int state )                      = 0;
	virtual void on_pre_data_changed( int update_type )                   = 0;
	virtual void on_data_changed( int update_type )                       = 0;
	virtual void pre_data_update( int update_type )                       = 0;
	virtual void post_data_update( int update_type )                      = 0;
	virtual void on_data_unchanged_in_pvs( )                              = 0;
	virtual bool is_dormant( ) const                                      = 0;
	virtual int get_index( ) const                                        = 0;
	virtual void receive_message( e_class_ids class_index, bf_read& msg ) = 0;
	virtual void* get_data_table_base_ptr( )                              = 0;
	virtual void set_destroyed_on_recreate_entities( )                    = 0;
};

class c_client_think_handle;
class c_client_thinkable
{
public:
	virtual c_client_unknown* get_client_unknown( )               = 0;
	virtual void client_think( )                                  = 0;
	virtual c_client_think_handle* get_think_handle( )            = 0;
	virtual void set_think_handle( c_client_think_handle* think ) = 0;
	virtual void release( )                                       = 0;
};

class c_client_entity : public c_client_unknown, public c_client_renderable, public c_client_networkable, public c_client_thinkable
{
public:
	virtual const c_vector& get_abs_origin( ) const                             = 0;
	virtual const c_angle& get_abs_angles( ) const                              = 0;
	virtual void* get_mouth( )                                                  = 0;
	virtual bool get_sound_spatialization( struct spatialization_info_t& info ) = 0;
	virtual bool is_blurred( )                                                  = 0;

	void set_abs_origin( const c_vector& origin );
	void set_abs_angles( const c_angle& angle );

	data_map_t* get_data_desc_map( )
	{
		return g_virtual.call< data_map_t* >( this, 15 );
	}

	data_map_t* get_prediction_desc_map( )
	{
		return g_virtual.call< data_map_t* >( this, 17 );
	}
};

class c_base_entity : public c_client_entity
{
public:
	NETVAR_RESOURCE_VARIABLE( int, get_resource_ping, "CPlayerResource->m_iPing" );

	/* DT_BasePlayer */
	NETVAR_PVARIABLE( float, get_fall_velocity, "CBasePlayer->m_flFallVelocity" );
	NETVAR_VARIABLE( c_angle, get_view_punch, "CBasePlayer->m_viewPunchAngle" );
	NETVAR_VARIABLE( c_angle, get_punch, "CBasePlayer->m_aimPunchAngle" );
	NETVAR_VARIABLE( int, get_shots_fired, "CCSPlayer->m_iShotsFired" );
	NETVAR_VARIABLE( c_vector, get_view_offset, "CBasePlayer->m_vecViewOffset[0]" );
	NETVAR_VARIABLE( float, get_friction, "CBasePlayer->m_flFriction" );
	NETVAR_VARIABLE( int, get_tick_base, "CBasePlayer->m_nTickBase" );
	NETVAR_PVARIABLE( int, get_next_think_tick, "CBasePlayer->m_nNextThinkTick" );
	NETVAR_VARIABLE( c_vector, get_velocity, "CBasePlayer->m_vecVelocity[0]" );
	NETVAR_VARIABLE( c_vector, get_ladder_normal, "CBasePlayer->m_vecLadderNormal" );
	NETVAR_PVARIABLE_OFFSET( c_angle, get_view_angles, "CBasePlayer->deadflag", 0x4 );
	NETVAR_VARIABLE( unsigned int, get_ground_entity_handle, "CBasePlayer->m_hGroundEntity" );
	NETVAR_VARIABLE( int, get_health, "CBasePlayer->m_iHealth" );
	NETVAR_VARIABLE( int, get_life_state, "CBasePlayer->m_lifeState" );
	NETVAR_VARIABLE( float, get_max_speed, "CBasePlayer->m_flMaxspeed" );
	NETVAR_VARIABLE( int, get_flags, "CBasePlayer->m_fFlags" );
	NETVAR_VARIABLE( int, get_zoom_fov, "CBasePlayer->m_iFOV" );
	NETVAR_VARIABLE( int, get_default_fov, "CBasePlayer->m_iDefaultFOV" );
	NETVAR_VARIABLE( int, get_observer_mode, "CBasePlayer->m_iObserverMode" );
	NETVAR_VARIABLE( unsigned int, get_observer_target_handle, "CBasePlayer->m_hObserverTarget" );
	NETVAR_VARIABLE( unsigned int, get_view_model_handle, "CBasePlayer->m_hViewModel[0]" );
	NETVAR_PVARIABLE( const char, get_last_place, "CBasePlayer->m_szLastPlaceName" );
	NETVAR_VARIABLE_OFFSET( int, get_button_disabled, "CBasePlayer->m_hViewEntity", -0xc );
	NETVAR_VARIABLE_OFFSET( int, get_button_forced, "CBasePlayer->m_hViewEntity", -0x8 );
	NETVAR_PVARIABLE_OFFSET( c_user_cmd*, get_current_command, "CBasePlayer->m_hViewEntity", -0x4 );
	NETVAR_VARIABLE( c_vector, get_base_velocity, "CBasePlayer->m_vecBaseVelocity" );
	NETVAR_VARIABLE( float, get_duck_amount, "CBasePlayer->m_flDuckAmount" );
	NETVAR_VARIABLE( float, get_duck_speed, "CBasePlayer->m_flDuckSpeed" );
	NETVAR_VARIABLE( bool, is_ducked, "CBasePlayer->m_bDucked" );
	NETVAR_VARIABLE( bool, is_ducking, "CBasePlayer->m_bDucking" );
	NETVAR_VARIABLE( bool, is_in_duck_jump, "CBasePlayer->m_bInDuckJump" );
	NETVAR_VARIABLE( float, get_last_duck_time, "CBasePlayer->m_flLastDuckTime" );
	NETVAR_VARIABLE( int, get_duck_time_msecs, "CBasePlayer->m_nDuckTimeMsecs" );
	NETVAR_VARIABLE( int, get_duck_jump_time_msecs, "CBasePlayer->m_nDuckJumpTimeMsecs" );
	NETVAR_VARIABLE( int, get_jump_time_msecs, "CBasePlayer->m_nJumpTimeMsecs" );
	NETVAR_VARIABLE( float, get_step_size, "CBasePlayer->m_flStepSize" );

	/* DT_BaseEntity */
	NETVAR_VARIABLE( int, get_team, "CBaseEntity->m_iTeamNum" );
	NETVAR_VARIABLE( c_vector, get_origin, "CBaseEntity->m_vecOrigin" );
	NETVAR_VARIABLE( float, get_simulation_time, "CBaseEntity->m_flSimulationTime" );
	NETVAR_VARIABLE_OFFSET( float, get_old_simulation_time, "CBaseEntity->m_flSimulationTime", 0x4 );
	NETVAR_VARIABLE( bool, is_predictable, "CBaseEntity->m_bPredictable" );
	NETVAR_VARIABLE( int, get_model_index, "CBaseEntity->m_nModelIndex" );
	NETVAR_VARIABLE( unsigned int, get_owner_entity_handle, "CBaseEntity->m_hOwnerEntity" );
	NETVAR_VARIABLE( int, get_collision_group, "CBaseEntity->m_CollisionGroup" );
	NETVAR_VARIABLE( int, get_effects, "CBaseEntity->m_fEffects" );
	NETVAR_VARIABLE( int, is_spotted, "CBaseEntity->m_bSpotted" );

	/* DT_CSRagdoll: a dead player's body. only valid on a CCSRagdoll */
	NETVAR_VARIABLE( unsigned int, get_ragdoll_player_handle, "CCSRagdoll->m_hPlayer" );
	NETVAR_VARIABLE( unsigned int, get_ragdoll_handle, "CCSPlayer->m_hRagdoll" );

	/* DT_BaseAnimating */
	NETVAR_VARIABLE( int, get_sequence, "CBaseAnimating->m_nSequence" );
	NETVAR_VARIABLE( int, get_hitbox_set, "CBaseAnimating->m_nHitboxSet" );
	NETVAR_VARIABLE( float, get_cycle, "CBaseAnimating->m_flCycle" );
	NETVAR_VARIABLE_OFFSET( int, get_bone_mask, "CBaseAnimating->m_nForceBone", 0x14 );
	NETVAR_VARIABLE_OFFSET( int, get_previous_bone_mask, "CBaseAnimating->m_nForceBone", 0xF );

	/* DT_CSPlayer */
	NETVAR_VARIABLE_OFFSET( int, is_used_new_animation_state, "CCSPlayer->m_flLastExoJumpTime", 0x8 );
	NETVAR_VARIABLE_OFFSET( c_animation_state*, get_animation_state, "CCSPlayer->m_bIsScoped", -0x14 );
	NETVAR_VARIABLE( int, get_survival_team, "CCSPlayer->m_nSurvivalTeam" );
	NETVAR_VARIABLE( int, get_armor, "CCSPlayer->m_ArmorValue" );
	NETVAR_VARIABLE( bool, is_scoped, "CCSPlayer->m_bIsScoped" );
	NETVAR_VARIABLE_OFFSET( float, get_collision_change_time, "CCSPlayer->m_bIsScoped", -0x50 );
	NETVAR_VARIABLE_OFFSET( float, get_collision_change_origin, "CCSPlayer->m_bIsScoped", -0x54 );
	NETVAR_VARIABLE( bool, has_heavy_armor, "CCSPlayer->m_bHasHeavyArmor" );
	NETVAR_VARIABLE( float, get_flash_max_alpha, "CCSPlayer->m_flFlashMaxAlpha" );
	NETVAR_VARIABLE( bool, has_helmet, "CCSPlayer->m_bHasHelmet" );
	NETVAR_VARIABLE( bool, in_buy_zone, "CCSPlayer->m_bInBuyZone" );
	NETVAR_VARIABLE( int, get_money, "CCSPlayer->m_iAccount" );
	NETVAR_VARIABLE( bool, is_defusing, "CCSPlayer->m_bIsDefusing" );
	NETVAR_VARIABLE( bool, is_grabbing_hostage, "CCSPlayer->m_bIsGrabbingHostage" );
	NETVAR_VARIABLE( bool, has_defuser, "CCSPlayer->m_bHasDefuser" );
	NETVAR_VARIABLE( bool, is_walking, "CCSPlayer->m_bIsWalking" );
	/* blind seconds LEFT (server countdown). > 0 = overlay up on their screen. not m_flFlashMaxAlpha */
	NETVAR_VARIABLE( float, get_flash_duration, "CCSPlayer->m_flFlashDuration" );
	/* server immunity: OnTakeDamage_Alive returns 0 when set. FL_GODMODE (1<<15) is unreadable,
	   m_fFlags is cropped to PLAYER_FLAG_BITS (11). */
	NETVAR_VARIABLE( bool, has_immunity, "CCSPlayer->m_bGunGameImmunity" );
	NETVAR_PVARIABLE( char, get_arms_model, "CCSPlayer->m_szArmsModel" );
	NETVAR_VARIABLE( bool, is_looking_at_weapon, "CCSPlayer->m_bIsLookingAtWeapon" );
	NETVAR_VARIABLE( bool, is_holding_look_at_weapon, "CCSPlayer->m_bIsHoldingLookAtWeapon" );
	NETVAR_VARIABLE( float, get_velocity_modifier, "CCSPlayer->m_flVelocityModifier" );
	NETVAR_VARIABLE( float, get_health_shot_expire, "CCSPlayer->m_flHealthShotBoostExpirationTime" );

	/* DT_WeaponCSBase */
	NETVAR_VARIABLE( bool, is_burst_mode, "CWeaponCSBase->m_bBurstMode" );
	NETVAR_VARIABLE( float, get_accuracy_penalty, "CWeaponCSBase->m_fAccuracyPenalty" );
	NETVAR_VARIABLE( int, get_weapon_mode, "CWeaponCSBase->m_weaponMode" );
	NETVAR_VARIABLE( float, get_fire_ready_time, "CWeaponCSBase->m_flPostponeFireReadyTime" );
	/* who dropped this gun, NULL (0xFFFFFFFF) until the first Drop. skinchanger: spawned-with vs picked-up */
	NETVAR_VARIABLE( unsigned int, get_prev_owner_handle, "CWeaponCSBase->m_hPrevOwner" );

	/* DT_BaseCombatCharacter */
	NETVAR_VARIABLE( float, get_next_attack, "CBaseCombatCharacter->m_flNextAttack" );
	NETVAR_VARIABLE( unsigned int, get_active_weapon_handle, "CBaseCombatCharacter->m_hActiveWeapon" );
	NETVAR_PVARIABLE( unsigned int, get_weapons_handle, "CBaseCombatCharacter->m_hMyWeapons" );
	NETVAR_PVARIABLE( unsigned int, get_wearables_handle, "CBaseCombatCharacter->m_hMyWearables" );

	/* DT_BaseCombatWeapon */
	NETVAR_VARIABLE( float, get_next_primary_attack, "CBaseCombatWeapon->m_flNextPrimaryAttack" );
	NETVAR_VARIABLE( float, get_next_secondary_attack, "CBaseCombatWeapon->m_flNextSecondaryAttack" );
	NETVAR_VARIABLE( int, get_ammo, "CBaseCombatWeapon->m_iClip1" );

	NETVAR_VARIABLE( float, get_c4_blow_time, "CPlantedC4->m_flC4Blow" );
	NETVAR_VARIABLE( bool, is_bomb_ticking, "CPlantedC4->m_bBombTicking" );
	NETVAR_VARIABLE( bool, is_bomb_defused, "CPlantedC4->m_bBombDefused" );
	NETVAR_VARIABLE( float, get_defuse_end_time, "CPlantedC4->m_flDefuseCountDown" );
	NETVAR_VARIABLE( int, get_ammo_reserve, "CBaseCombatWeapon->m_iPrimaryReserveAmmoCount" );
	NETVAR_VARIABLE( int, get_view_model_index, "CBaseCombatWeapon->m_iViewModelIndex" );
	NETVAR_VARIABLE( int, get_world_model_index, "CBaseCombatWeapon->m_iWorldModelIndex" );
	NETVAR_VARIABLE( unsigned int, get_world_model_handle, "CBaseCombatWeapon->m_hWeaponWorldModel" ); /* skinchanger knife world model */

	/* DT_BaseAttributableItem */
	NETVAR_VARIABLE( short, get_item_definition_index, "CBaseAttributableItem->m_iItemDefinitionIndex" );
	NETVAR_VARIABLE( int, get_item_id_high, "CBaseAttributableItem->m_iItemIDHigh" );
	NETVAR_VARIABLE( int, get_item_id_low, "CBaseAttributableItem->m_iItemIDLow" );
	NETVAR_VARIABLE( int, get_account_id, "CBaseAttributableItem->m_iAccountID" );
	NETVAR_VARIABLE( int, get_entity_quality, "CBaseAttributableItem->m_iEntityQuality" );
	NETVAR_PVARIABLE( char, get_custom_name, "CBaseAttributableItem->m_szCustomName" );
	NETVAR_VARIABLE( int, get_owner_xuid_low, "CBaseAttributableItem->m_OriginalOwnerXuidLow" );
	NETVAR_VARIABLE( int, get_owner_xuid_high, "CBaseAttributableItem->m_OriginalOwnerXuidHigh" );
	NETVAR_VARIABLE( int, get_fall_back_paint_kit, "CBaseAttributableItem->m_nFallbackPaintKit" );
	NETVAR_VARIABLE( int, get_fall_back_seed, "CBaseAttributableItem->m_nFallbackSeed" );
	NETVAR_VARIABLE( float, get_fall_back_wear, "CBaseAttributableItem->m_flFallbackWear" );
	NETVAR_VARIABLE( int, get_fall_back_stat_trak, "CBaseAttributableItem->m_nFallbackStatTrak" );

	/* DT_WeaponCSBaseGun */
	NETVAR_VARIABLE( int, get_burst_shots_remaining, "CWeaponCSBaseGun->m_iBurstShotsRemaining" );

	/* DT_BaseViewModel */
	NETVAR_VARIABLE( unsigned int, get_owner_handle, "CBaseViewModel->m_hOwner" );
	NETVAR_VARIABLE( unsigned int, get_weapon_handle, "CBaseViewModel->m_hWeapon" );
	NETVAR_VARIABLE( int, get_view_model_body, "CBaseViewModel->m_nBody" ); /* skinchanger knife_gg team body */
	NETVAR_VARIABLE( float, get_anim_time, "CBaseEntity->m_flAnimTime" ); /* view model cycle = ( time - this ) * rate */
	NETVAR_VARIABLE( int, get_reset_events_parity, "CBaseViewModel->m_nResetEventsParity" ); /* bump = anim events restart */
	NETVAR_OFFSET( c_angle, get_cam_driver_angles, 0x29B0 );
	NETVAR_OFFSET( float, get_cam_driver_applied_time, 0x29BC );

	/* DT_FogController */
	NETVAR_VARIABLE( int, get_fog_enable, "CFogController->m_fog.enable" );
	NETVAR_VARIABLE( float, get_fog_start, "CFogController->m_fog.start" );
	NETVAR_VARIABLE( float, get_fog_end, "CFogController->m_fog.end" );
	NETVAR_VARIABLE( float, get_fog_density, "CFogController->m_fog.maxdensity" );
	NETVAR_VARIABLE( int, get_fog_color, "CFogController->m_fog.colorPrimary" );
	NETVAR_VARIABLE( int, get_fog_color_secondary, "CFogController->m_fog.colorSecondary" );

	/* DT_Precipitation */
	NETVAR_VARIABLE( int, precipitation_type, "CPrecipitation->m_nPrecipType" );

	NETVAR_VARIABLE( bool, did_smoke_effect, "CSmokeGrenadeProjectile->m_bDidSmokeEffect" );
	NETVAR_VARIABLE( int, get_smoke_effect_tick_begin, "CSmokeGrenadeProjectile->m_nSmokeEffectTickBegin" );

	/* datamap variables */
	ADD_DATAFIELD( int, get_eflags, this->get_prediction_desc_map( ), "m_iEFlags" );
	ADD_PDATAFIELD( int, get_buttons, this->get_prediction_desc_map( ), "m_nButtons" );
	ADD_DATAFIELD( int, get_button_last, this->get_prediction_desc_map( ), "m_afButtonLast" );
	ADD_DATAFIELD( int, get_button_pressed, this->get_prediction_desc_map( ), "m_afButtonPressed" );
	ADD_DATAFIELD( int, get_button_released, this->get_prediction_desc_map( ), "m_afButtonReleased" );
	ADD_PDATAFIELD( int, get_impulse, this->get_prediction_desc_map( ), "m_nImpulse" );
	ADD_DATAFIELD( float, get_surface_friction, this->get_prediction_desc_map( ), "m_surfaceFriction" );
	ADD_DATAFIELD( const matrix3x4_t, get_coordinate_frame, this->get_data_desc_map( ), "m_rgflCoordinateFrame" );
	ADD_DATAFIELD( int, get_move_type, this->get_prediction_desc_map( ), "m_MoveType" );
	ADD_DATAFIELD( float, get_stamina, this->get_prediction_desc_map( ), "m_flStamina" );
	ADD_DATAFIELD( c_vector, get_network_origin, this->get_prediction_desc_map( ), "m_vecNetworkOrigin" );
	ADD_DATAFIELD( bool, is_reloading, this->get_prediction_desc_map( ), "m_bInReload" );

	/* offsets */
	NETVAR_OFFSET( unsigned int, get_index, 0x64 );
	NETVAR_OFFSET( bool, is_dormant, 0xed );
	NETVAR_OFFSET( c_utl_vector< matrix3x4_t >, get_cached_bone_data, 0x2914 );
	NETVAR_OFFSET( bool, can_use_fast_path, 0x2700 );

	const bool is_alive( )
	{
		return ( this->get_life_state( ) == 0 /* LIFE_ALIVE */ );
	}

	const int is_max_health( )
	{
		return g_virtual.call< int >( this, 123 );
	}

	unsigned int physics_solid_mask_for_entity( )
	{
		return g_virtual.call< unsigned int >( this, 152 );
	}

	void pre_think( )
	{
		g_virtual.call< void >( this, 318 );
	}

	float spawn_time( ) noexcept
	{
		return *reinterpret_cast< float* >( reinterpret_cast< uintptr_t >( this ) + 0x103C0 );
	}

	const char* get_class_name( )
	{
		return g_virtual.call< const char* >( this, 143 );
	}

	void update_collision_bounds( )
	{
		g_virtual.call< void >( this, 340 );
	}

	void think( )
	{
		g_virtual.call< void >( this, 139 );
	}

	void studio_frame_advance( )
	{
		g_virtual.call< void >( this, 220 );
	}

	const bool is_player( )
	{
		return g_virtual.call< bool >( this, 158 );
	}

	[[nodiscard]] float get_spread( )
	{
		return g_virtual.call< float >( this, 452 );
	}

	[[nodiscard]] float get_inaccuracy( )
	{
		return g_virtual.call< float >( this, 482 );
	}

	void set_sequence( int sequence )
	{
		g_virtual.call< void >( this, 219, sequence );
	}

	void set_model_index( int index )
	{
		g_virtual.call< void >( this, 75, index );
	}

	var_mapping* get_var_map( )
	{
		return reinterpret_cast< var_mapping* >( reinterpret_cast< std::uintptr_t >( this ) + 0x24 );
	};

	void invalidate_bone_cache( );
	/* invalidate_bone_cache is a no-op outside OnRenderStart..OnRenderEnd (s_bEnableInvalidateBoneCache). this writes the fields */
	bool force_bone_rebuild( );

	void modify_eye_position( const c_animation_state* animation_state, c_vector* position ) const;
	void set_next_think( int think );
	void post_think( );
	void restore_data( const char* context, int slot, int type );
	bool save_data( const char* context, int slot, int type );
	void on_post_restore_data( );
	void set_collision_bounds( const c_vector& mins, const c_vector& maxs );

	bool physics_run_think( int think_method );
	bool can_shoot( c_base_entity* weapon );
	bool is_enemy( c_base_entity* entity );
	bool get_bounding_box( bounding_box_t* box );
	bool is_valid_enemy( );
	bool is_valid_aim_target( ); /* is_valid_enemy, plus teammates when aimbot "target team" is on */
	bool is_valid_player( );
	bool is_armored( const int hit_group );
	bool is_breakable( );

	int get_bone_by_hash( const unsigned int hash ) const;
	int get_max_health( );
	int& get_take_damage( );

	c_vector get_bone_position( int bone );
	c_vector get_hitbox_position( int hitbox, float point_scale = 0.5f );
	c_vector get_hitbox_position( int hitbox, matrix3x4_t* matrix, float point_scale = 0.5f );
	/* correct = anim state's head-bone z pull ( air / duck only ). off by default: local first-person bone cache is
	   stale ( model never drawn ), so it dropped the eye to an old head height = rays started elsewhere after a jump.
	   the camera ( CalcView ) skips it too, see hooks/functions/modify_eye_position.cpp */
	c_vector get_eye_position( bool should_correct = false );

	bool can_see_player( c_base_entity* player );
	bool can_see_matrix( c_base_entity* player, matrix3x4_t* matrix );
	// LOS to an EXACT world point (per-hitbox), unlike can_see_matrix which always traces to the head.
	bool can_see_position( c_base_entity* player, const c_vector& point );
	/* same, with the caller's eye position: get_eye_position is a VIRTUAL call (slot 169) */
	bool can_see_position( c_base_entity* player, const c_vector& point, const c_vector& eye_position );
	c_user_cmd& get_last_command( );
};

struct hitbox_resolver_t {
	mstudiohitboxset_t* m_set = nullptr;

	bool setup( c_base_entity* entity );

	// nullptr matrix / unknown hitbox gives zero vector, same as the get_hitbox_position it replaces
	c_vector position( int hitbox, matrix3x4_t* matrix, float point_scale = 0.5f ) const;
};

class c_breakable_surface : public c_base_entity, public c_breakable_with_prop_data
{
public:
	NETVAR_VARIABLE( bool, is_broken, "CBreakableSurface->m_bIsBroken" );
};
