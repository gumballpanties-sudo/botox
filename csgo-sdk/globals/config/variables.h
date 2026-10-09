#pragma once
#include "../../game/sdk/classes/c_color.h"
#include "config.h"
#include "structs/font_setting_t.h"

enum e_player_flags {
	player_flag_money = 0,
	player_flag_armor,
	player_flag_helmet,
	player_flag_kit,
	player_flag_defusing,
	/* no reload row: m_bInReload is a datafield, never networked. m_bIsWalking is networked. */
	player_flag_walking,
	player_flag_scoped,
	player_flag_flashed,
	player_flag_bomb,
	player_flag_hostage,
	player_flag_immunity,
	player_flag_wallbangable,
	player_flags_max
};

/* which edge of the box the health bar rides. order must match the menu combo. */
enum e_bar_side {
	bar_side_left = 0,
	bar_side_right,
	bar_side_top,
	bar_side_bottom,
	bar_side_corner
};

enum e_distance_unit {
	distance_unit_meters = 0,
	distance_unit_units,
	distance_unit_feet
};

enum e_skybox_type {
	skybox_off = 0,
	skybox_cloudy01,
	skybox_night02,
	skybox_night02b,
	skybox_baggage,
	skybox_tibet,
	skybox_vietnam,
	skybox_lunacy,
	skybox_embassy,
	skybox_italy,
	skybox_jungle,
	skybox_office,
	skybox_daylight01,
	skybox_daylight02,
	skybox_daylight03,
	skybox_daylight04,
	skybox_day02_05,
	skybox_nukeblank,
	skybox_dustblank,
	skybox_venice,
	skybox_vertigo,
	skybox_vertigoblue,
	skybox_dust,
	skybox_aztec,
	skybox_minecraft,
	skybox_custom, // m_skybox_custom = name scanned from csgo\materials\skybox
	skybox_max
};

enum e_free_type_font_flags : int {
	font_flag_nohinting = 0,
	font_flag_noautohint,
	font_flag_forceautohint,
	font_flag_lighthinting,
	font_flag_monohinting,
	font_flag_bold,
	font_flag_oblique,
	font_flag_monochrome,
	font_flag_max
};

enum e_log_types {
	log_type_hit_enemy,
	log_type_hit_teammate,
	log_type_purchase,
	log_type_votes,
	log_type_max,
};

constexpr int chams_max_layers = 8;

/* detections multi-combo order, the label list must match it */
enum e_detection_types {
	detect_eb = 0,
	detect_tb,
	detect_as,
	detect_ps,
	detect_wc,
	detect_max
};

enum e_keybind_indicators {
	key_eb = 0,
	key_ps,
	key_ej,
	key_lj,
	key_dh,
	key_mj,
	key_jb,
	key_tb,
	key_as,
	key_wc,
	key_st,
	key_psa,
	key_fr,
	key_lb,
	key_lg,
	key_lfc,
	key_fl,
	key_oh,
	key_ac,
	key_ab,
	key_tung,
	key_es,
	key_hsw,
	key_zb,
	key_az,
	key_max
};

enum e_keybind_labels {
	label_eb = 0,
	label_ps,
	label_ej,
	label_lj,
	label_dh,
	label_mj,
	label_jb,
	label_tb,
	label_air,
	label_wc,
	label_fr,
	label_as,
	label_ast,
	label_bast,
	label_lb,
	label_lg,
	label_lfc,
	label_fl,
	label_oh,
	label_ac,
	label_ab,
	label_tung,
	label_es,
	label_hsw,
	label_zb,
	label_az,
	label_max
};

/* { menu name, default label }. default also keys the show/fade anims, so a rename never restarts them */
inline constexpr const char* k_keybind_labels[ label_max ][ 2 ] = {
	{ "edgebug", "eb" },       { "pixelsurf", "ps" },         { "edgejump", "ej" },       { "longjump", "lj" },
	{ "delayhop", "dh" },      { "minijump", "mj" },          { "jumpbug", "jb" },        { "texturebug", "tb" },
	{ "airstuck", "air" },     { "wallclimb", "wc" },         { "fireman", "fr" },        { "autostrafe", "as" },
	{ "ps assist", "ast" },    { "bounce assist", "bast" },   { "ladder bug", "lb" },     { "ladder glide", "lg" },
	{ "freelook climb", "lfc" }, { "fast ladder", "fl" },     { "one hop", "oh" },        { "auto crouch", "ac" },
	{ "auto bounce", "ab" },   { "tung surf", "tung" },       { "edge skip", "es" },      { "half sideways", "hsw" },
	{ "zeus bug", "zb" },      { "air freeze", "az" },
};

enum e_indicator_style {
	indicator_style_default = 0,
	indicator_style_static,
	indicator_style_smooth,
	indicator_style_mitosis,
	indicator_style_kamidere,
	indicator_style_squash,
	indicator_style_max
};

inline constexpr const char* k_indicator_style_names = "default\0static\0smooth\0mitosis\0kamidere\0squash & stretch\0";

enum e_smooth_origin {
	smooth_origin_auto = 0,
	smooth_origin_right,
	smooth_origin_bottom
};

inline constexpr const char* k_smooth_origin_names = "auto\0right\0bottom\0";

enum e_pre_layout {
	pre_layout_right = 0,
	pre_layout_above,
	pre_layout_below
};

inline constexpr const char* k_pre_layout_names = "right\0above\0below\0";

enum e_pre_align {
	pre_align_bottom = 0,
	pre_align_top
};

inline constexpr const char* k_pre_align_names = "bottom\0top\0";

enum e_aim_hitboxes {
	aim_hitbox_head = 0,
	aim_hitbox_neck,
	aim_hitbox_chest,
	aim_hitbox_stomach,
	aim_hitbox_pelvis,
	aim_hitbox_arms,
	aim_hitbox_legs,
	aim_hitbox_max
};

enum e_aim_config {
	aim_config_general = 0,
	aim_config_pistol,
	aim_config_heavy_pistol,
	aim_config_rifle,
	aim_config_smg,
	aim_config_sniper,
	aim_config_scout,
	aim_config_heavy,
	aim_config_max
};

/* player avatar placement: menu combo order must match */
enum e_avatar_position {
	avatar_position_name = 0,
	avatar_position_top,
	avatar_position_bottom,
	avatar_position_head
};

enum e_name_position {
	name_position_above = 0,
	name_position_below,
	name_position_inside_top,
	name_position_inside_bottom
};

enum e_hud_elements {
	hud_element_health = 0,
	hud_element_ammo,
	hud_element_weapon_select,
	hud_element_killfeed,
	hud_element_radar,
	hud_element_money,
	hud_element_timer,
	hud_element_chat,
	hud_element_alerts,
	hud_element_hint,
	hud_element_radio,
	hud_element_vote,
	hud_element_spectator,
	hud_element_freezepanel,
	hud_element_winpanel,
	hud_element_c4_icon,
	hud_element_max
};

enum e_box_outline_side {
	box_outline_side_inside = 0,
	box_outline_side_outside,
	box_outline_side_both
};

namespace n_variables
{
	struct impl_t {
		ADD_VARIABLE( c_color, m_accent, c_color( 129, 99, 251, 255 ) );
		ADD_VARIABLE( float, m_dpi_scale, 100.f );
		ADD_VARIABLE( bool, m_dpi_scale_panels, false );
		ADD_VARIABLE( bool, m_console_color_enable, false );
		ADD_VARIABLE( c_color, m_console_color, c_color( 102, 111, 102, 255 ) );

		ADD_VARIABLE( bool, m_aimbot_enable, false );
		ADD_VARIABLE( bool, m_backtrack_enable, false );
		ADD_VARIABLE( bool, m_aimbot_aim_at_backtrack, false );
		ADD_VARIABLE( bool, m_backtrack_extend, false );
		ADD_VARIABLE( int, m_backtrack_extend_amount, 200 );
		ADD_VARIABLE( int, m_backtrack_time_limit, 200 );

		/* offline lagcomp needs no variable: n_misc::force_host_lagcomp is unconditional. never re-add fake ping */

		ADD_VARIABLE( key_bind_t, m_aimbot_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_aimbot_on_key, false );
		ADD_VARIABLE( bool, m_aimbot_silent, false );
		ADD_VARIABLE( float, m_aimbot_silent_step, 15.f );
		ADD_VARIABLE( bool, m_aimbot_target_team, false );
		ADD_VARIABLE( bool, m_zeusbug, false );
		ADD_VARIABLE( bool, m_zeusbug_swapback, true );
		ADD_VARIABLE( key_bind_t, m_zeusbug_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_grenade_aim, false );
		ADD_VARIABLE( key_bind_t, m_grenade_aim_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( float, m_grenade_aim_fov, 30.f );
		ADD_VARIABLE( int, m_grenade_aim_arc, 0 );
		ADD_VARIABLE( bool, m_grenade_aim_silent, true );
		ADD_VARIABLE( bool, m_grenade_aim_auto_throw, false );
		ADD_VARIABLE( bool, m_grenade_aim_draw, true );
		ADD_VARIABLE( c_color, m_grenade_aim_color, c_color( 255, 200, 60, 255 ) );
		ADD_VARIABLE( int, m_aimbot_weapon_settings, 0 );

		ADD_VARIABLE( float, m_aimbot_pistol_fov, 5.f );
		ADD_VARIABLE( float, m_aimbot_pistol_smooth, 5.f );
		ADD_VARIABLE( float, m_aimbot_pistol_rcs, 100.f );
		ADD_VARIABLE( float, m_aimbot_heavy_pistol_fov, 5.f );
		ADD_VARIABLE( float, m_aimbot_heavy_pistol_smooth, 5.f );
		ADD_VARIABLE( float, m_aimbot_heavy_pistol_rcs, 100.f );
		ADD_VARIABLE( float, m_aimbot_smg_fov, 5.f );
		ADD_VARIABLE( float, m_aimbot_smg_smooth, 5.f );
		ADD_VARIABLE( float, m_aimbot_smg_rcs, 100.f );
		ADD_VARIABLE( float, m_aimbot_rifle_fov, 5.f );
		ADD_VARIABLE( float, m_aimbot_rifle_smooth, 5.f );
		ADD_VARIABLE( float, m_aimbot_rifle_rcs, 100.f );
		ADD_VARIABLE( float, m_aimbot_sniper_fov, 5.f );
		ADD_VARIABLE( float, m_aimbot_sniper_smooth, 5.f );
		ADD_VARIABLE( float, m_aimbot_sniper_rcs, 100.f );
		ADD_VARIABLE( float, m_aimbot_scout_fov, 5.f );
		ADD_VARIABLE( float, m_aimbot_scout_smooth, 5.f );
		ADD_VARIABLE( float, m_aimbot_scout_rcs, 100.f );
		ADD_VARIABLE( float, m_aimbot_heavy_fov, 5.f );
		ADD_VARIABLE( float, m_aimbot_heavy_smooth, 5.f );
		ADD_VARIABLE( float, m_aimbot_heavy_rcs, 100.f );

		ADD_VARIABLE( bool, m_aimbot_scope_fov_scale, false );
		ADD_VARIABLE( float, m_aimbot_scope_fov_scale_amount, 100.f );

		ADD_VARIABLE( float, m_aimbot_general_fov, 5.f );
		ADD_VARIABLE( float, m_aimbot_general_smooth, 5.f );
		ADD_VARIABLE( float, m_aimbot_general_rcs, 100.f );

		ADD_VARIABLE( int, m_aimbot_rcs_start, 1 );
		ADD_VARIABLE( float, m_aimbot_rcs_pitch, 100.f );
		ADD_VARIABLE( float, m_aimbot_rcs_yaw, 100.f );
		ADD_VARIABLE( bool, m_aimbot_rcs_only_with_aimbot, true );

		ADD_VARIABLE( bool, m_aimbot_pistol_override, false );
		ADD_VARIABLE( bool, m_aimbot_heavy_pistol_override, false );
		ADD_VARIABLE( bool, m_aimbot_rifle_override, false );
		ADD_VARIABLE( bool, m_aimbot_smg_override, false );
		ADD_VARIABLE( bool, m_aimbot_sniper_override, false );
		ADD_VARIABLE( bool, m_aimbot_scout_override, false );
		ADD_VARIABLE( bool, m_aimbot_heavy_override, false );

		ADD_VARIABLE_VECTOR( bool, e_aim_hitboxes::aim_hitbox_max, m_aimbot_hitboxes, true );
		ADD_VARIABLE_VECTOR( bool, e_aim_hitboxes::aim_hitbox_max, m_aimbot_pistol_hitboxes, true );
		ADD_VARIABLE_VECTOR( bool, e_aim_hitboxes::aim_hitbox_max, m_aimbot_heavy_pistol_hitboxes, true );
		ADD_VARIABLE_VECTOR( bool, e_aim_hitboxes::aim_hitbox_max, m_aimbot_smg_hitboxes, true );
		ADD_VARIABLE_VECTOR( bool, e_aim_hitboxes::aim_hitbox_max, m_aimbot_rifle_hitboxes, true );
		ADD_VARIABLE_VECTOR( bool, e_aim_hitboxes::aim_hitbox_max, m_aimbot_sniper_hitboxes, true );
		ADD_VARIABLE_VECTOR( bool, e_aim_hitboxes::aim_hitbox_max, m_aimbot_scout_hitboxes, true );
		ADD_VARIABLE_VECTOR( bool, e_aim_hitboxes::aim_hitbox_max, m_aimbot_heavy_hitboxes, true );
		ADD_VARIABLE( bool, m_aimbot_autowall, false );
		ADD_VARIABLE( int, m_aimbot_min_damage, 1 );

		ADD_VARIABLE( bool, m_nospread_enable, false );
		ADD_VARIABLE( bool, m_nospread_sourcemod, true );

		ADD_VARIABLE( bool, m_knife_enable, false );
		ADD_VARIABLE( int, m_knife_model, 0 );
		ADD_VARIABLE( bool, m_knife_fix_view, false );
		ADD_VARIABLE( bool, m_knife_anims_enable, false );
		ADD_VARIABLE( int, m_knife_anims_model, 0 );
		ADD_VARIABLE( bool, m_deagle_spinner, false );
		ADD_VARIABLE( key_bind_t, m_deagle_spinner_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_pure_bypass, false );
		ADD_VARIABLE( int, m_knife_paint_kit, 0 );
		ADD_VARIABLE( float, m_knife_wear, 0.0001f );
		ADD_VARIABLE( int, m_knife_seed, 0 );
		ADD_VARIABLE( bool, m_knife_stattrak, false );
		ADD_VARIABLE( int, m_knife_stattrak_kills, 0 );
		ADD_VARIABLE( std::string, m_knife_custom_name, "" );
		/* overwrites the kit's 4 palette colours in CCSWeaponVisualsDataProcessor::SetVisualsData.
		   only palette styles react (solid / ano / spray / hydro), "original" style ignores them */
		ADD_VARIABLE( bool, m_knife_custom_color, false );
		ADD_VARIABLE( c_color, m_knife_color_1, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_knife_color_2, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_knife_color_3, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_knife_color_4, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_gloves_enable, false );
		ADD_VARIABLE( int, m_gloves_model, 0 );
		ADD_VARIABLE( int, m_gloves_paint_kit, 0 );
		ADD_VARIABLE( float, m_gloves_wear, 0.0001f );
		ADD_VARIABLE( int, m_gloves_seed, 0 );
		ADD_VARIABLE( bool, m_weapon_skins_enable, false );
		ADD_VARIABLE( int, m_weapon_skins_selected, 0 );
		ADD_VARIABLE_VECTOR( int, 34, m_weapon_skins_paint_kit, 0 );
		ADD_VARIABLE_VECTOR( float, 34, m_weapon_skins_wear, 0.0001f );
		ADD_VARIABLE_VECTOR( int, 34, m_weapon_skins_seed, 0 );
		ADD_VARIABLE_VECTOR( bool, 34, m_weapon_skins_stattrak, false );
		ADD_VARIABLE_VECTOR( int, 34, m_weapon_skins_stattrak_kills, 0 );
		ADD_VARIABLE_VECTOR( std::string, 34, m_weapon_skins_custom_name, "" );
		ADD_VARIABLE_VECTOR( bool, 34, m_weapon_skins_custom_color, false );
		ADD_VARIABLE_VECTOR( c_color, 34, m_weapon_skins_color_1, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE_VECTOR( c_color, 34, m_weapon_skins_color_2, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE_VECTOR( c_color, 34, m_weapon_skins_color_3, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE_VECTOR( c_color, 34, m_weapon_skins_color_4, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( int, m_weapon_skins_sticker_slot, 0 );
		ADD_VARIABLE_VECTOR( int, 170, m_weapon_skins_sticker_kit, 0 );
		ADD_VARIABLE_VECTOR( float, 170, m_weapon_skins_sticker_wear, 0.f );
		ADD_VARIABLE_VECTOR( float, 170, m_weapon_skins_sticker_scale, 1.f );
		ADD_VARIABLE_VECTOR( float, 170, m_weapon_skins_sticker_rotation, 0.f );
		ADD_VARIABLE( bool, m_agent_enable, false );
		ADD_VARIABLE( int, m_agent_t, 0 );
		ADD_VARIABLE( int, m_agent_ct, 0 );
		ADD_VARIABLE( std::string, m_agent_t_custom, "" );
		ADD_VARIABLE( std::string, m_agent_ct_custom, "" );
		ADD_VARIABLE( bool, m_agent_custom_models, false );

		ADD_VARIABLE( bool, m_players, false );
		ADD_VARIABLE( bool, m_players_teammates, false );
		ADD_VARIABLE( float, m_players_max_distance, 0.f );
		ADD_VARIABLE( bool, m_players_distance_fade, false );
		ADD_VARIABLE( bool, m_players_sound_only, false );
		ADD_VARIABLE( float, m_players_sound_duration, 1.5f );
		ADD_VARIABLE( bool, m_players_sound_fade, true );
		ADD_VARIABLE( bool, m_players_box, false );
		ADD_VARIABLE( bool, m_players_box_corner, false );
		ADD_VARIABLE( bool, m_players_box_3d, false );
		ADD_VARIABLE( bool, m_players_box_outline, false );
		ADD_VARIABLE( int, m_players_box_outline_side, e_box_outline_side::box_outline_side_both );
		ADD_VARIABLE( float, m_players_box_rounding, 0.f );
		ADD_VARIABLE( float, m_players_box_thickness, 1.f );
		ADD_VARIABLE( float, m_players_box_outline_thickness, 1.f );
		ADD_VARIABLE( c_color, m_players_box_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_players_box_outline_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( bool, m_players_box_visibility_colors, false );
		ADD_VARIABLE( c_color, m_players_box_visible_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_players_box_invisible_color, c_color( 255, 90, 90, 255 ) );
		ADD_VARIABLE( bool, m_players_box_fill, false );
		ADD_VARIABLE( c_color, m_players_box_fill_color, c_color( 0, 0, 0, 80 ) );
		ADD_VARIABLE( bool, m_players_box_fill_gradient, false );
		ADD_VARIABLE( c_color, m_players_box_fill_bottom_color, c_color( 0, 0, 0, 0 ) );
		ADD_VARIABLE( bool, m_players_health_bar, false );
		ADD_VARIABLE( int, m_players_health_bar_side, e_bar_side::bar_side_left );
		ADD_VARIABLE( float, m_players_health_bar_thickness, 2.f );
		ADD_VARIABLE( float, m_players_health_bar_corner_rounding, 6.f );
		ADD_VARIABLE( float, m_players_health_bar_corner_size, 0.33f );
		ADD_VARIABLE( int, m_players_health_bar_corner_pick, 0 );
		ADD_VARIABLE( bool, m_players_health_bar_outline, true );
		ADD_VARIABLE( bool, m_players_health_bar_custom_color, false );
		ADD_VARIABLE( bool, m_players_health_text, false );
		ADD_VARIABLE( bool, m_players_health_suffix, false );
		ADD_VARIABLE( int, m_players_health_text_style, 0 );
		ADD_VARIABLE( bool, m_players_health_text_custom_color, false );
		ADD_VARIABLE( c_color, m_players_health_text_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_players_health_bar_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_players_health_bar_bg_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( bool, m_players_health_bar_gradient, false );
		ADD_VARIABLE( c_color, m_players_health_bar_bottom_color, c_color( 255, 0, 0, 255 ) );
		ADD_VARIABLE( bool, m_players_name, false );
		ADD_VARIABLE( c_color, m_players_name_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( int, m_players_name_position, e_name_position::name_position_above );
		ADD_VARIABLE( bool, m_players_name_visibility_colors, false );
		ADD_VARIABLE( c_color, m_players_name_visible_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_players_name_invisible_color, c_color( 255, 90, 90, 255 ) );
		ADD_VARIABLE( bool, m_weapon_name, false );
		ADD_VARIABLE( c_color, m_weapon_name_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_weapon_icon, false );
		ADD_VARIABLE( c_color, m_weapon_icon_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_player_ammo_bar, false );
		ADD_VARIABLE( c_color, m_player_ammo_bar_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_players_skeleton, false );
		ADD_VARIABLE( c_color, m_players_skeleton_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( int, m_players_skeleton_type, 0 );
		ADD_VARIABLE( float, m_players_skeleton_thickness, 1.f );
		ADD_VARIABLE( bool, m_players_skeleton_visibility_colors, false );
		ADD_VARIABLE( c_color, m_players_skeleton_visible_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_players_skeleton_invisible_color, c_color( 255, 90, 90, 255 ) );
		ADD_VARIABLE( bool, m_players_hitboxes, false );
		ADD_VARIABLE( int, m_players_hitboxes_mode, 0 );
		ADD_VARIABLE( c_color, m_players_hitboxes_color, c_color( 255, 255, 255, 150 ) );
		ADD_VARIABLE( float, m_players_hitboxes_thickness, 1.f );
		ADD_VARIABLE( float, m_players_hitboxes_duration, 2.f );
		ADD_VARIABLE( bool, m_glow_enable, false );
		ADD_VARIABLE( c_color, m_glow_vis_color, c_color( 224, 175, 86, 153 ) );
		ADD_VARIABLE( c_color, m_glow_invis_color, c_color( 114, 155, 221, 153 ) );
		ADD_VARIABLE( bool, m_glow_legacy, false );
		ADD_VARIABLE( int, m_glow_legacy_style, 0 );
		ADD_VARIABLE( bool, m_glow_normal_blend, false );
		ADD_VARIABLE( float, m_glow_thickness, 15.f );
		ADD_VARIABLE( bool, m_glow_gradient, false );
		ADD_VARIABLE( c_color, m_glow_vis_outer_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( c_color, m_glow_invis_outer_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( float, m_glow_gradient_inner, 60.f );
		ADD_VARIABLE( bool, m_glow_wave, false );
		ADD_VARIABLE( c_color, m_glow_wave_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( float, m_glow_wave_speed, 0.5f );
		ADD_VARIABLE( float, m_glow_wave_size, 0.35f );
		ADD_VARIABLE( float, m_glow_wave_strength, 0.8f );
		ADD_VARIABLE( bool, m_glow_pulse, false );
		ADD_VARIABLE( float, m_glow_pulse_speed, 1.f );
		ADD_VARIABLE( float, m_glow_pulse_min, 35.f );
		ADD_VARIABLE( bool, m_players_stencil, false );
		ADD_VARIABLE( c_color, m_players_stencil_vis_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_players_stencil_invis_color, c_color( 114, 155, 221, 255 ) );
		ADD_VARIABLE( float, m_players_stencil_thickness, 1.5f );
		ADD_VARIABLE( bool, m_inner_glow, false );
		ADD_VARIABLE( c_color, m_inner_glow_vis_color, c_color( 255, 255, 255, 200 ) );
		ADD_VARIABLE( c_color, m_inner_glow_invis_color, c_color( 114, 155, 221, 200 ) );
		ADD_VARIABLE( float, m_inner_glow_size, 12.f );
		ADD_VARIABLE( float, m_inner_glow_intensity, 1.5f );
		ADD_VARIABLE( bool, m_inner_glow_bones, true );
		ADD_VARIABLE( float, m_inner_glow_bone_width, 50.f );
		ADD_VARIABLE( bool, m_players_backtrack_trail, false );
		ADD_VARIABLE( bool, m_players_avatar, false );
		ADD_VARIABLE( float, m_players_avatar_size, 14.f );
		ADD_VARIABLE( int, m_players_avatar_position, 1 );
		ADD_VARIABLE( float, m_players_avatar_head_scale, 1.f );
		ADD_VARIABLE( bool, m_out_of_fov_arrows, false );
		ADD_VARIABLE( c_color, m_out_of_fov_arrows_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_out_of_fov_arrows_wall_color, c_color( 255, 90, 90, 255 ) );
		ADD_VARIABLE( float, m_out_of_fov_arrows_width, 18.f );
		ADD_VARIABLE( float, m_out_of_fov_arrows_height, 22.f );
		ADD_VARIABLE( int, m_out_of_fov_arrows_distance, 200 );
		ADD_VARIABLE( bool, m_out_of_fov_arrows_avatar, false );
		ADD_VARIABLE( bool, m_out_of_fov_arrows_aspect, false );
		ADD_VARIABLE( int, m_out_of_fov_arrows_style, 0 );
		ADD_VARIABLE( int, m_out_of_fov_arrows_type, 0 );
		ADD_VARIABLE( c_color, m_out_of_fov_arrows_sound_color, c_color( 255, 160, 0, 255 ) );
		ADD_VARIABLE( float, m_out_of_fov_arrows_sound_duration, 1.5f );
		ADD_VARIABLE( bool, m_out_of_fov_arrows_sound_fade, true );
		ADD_VARIABLE( bool, m_players_distance, false );
		ADD_VARIABLE( int, m_players_distance_unit, e_distance_unit::distance_unit_meters );
		ADD_VARIABLE( c_color, m_players_distance_color, c_color( 255, 255, 255, 255 ) );

		ADD_VARIABLE( bool, m_players_flags, false );
		ADD_VARIABLE_VECTOR( bool, e_player_flags::player_flags_max, m_player_flags, false );
		ADD_VARIABLE( c_color, m_players_flags_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_players_flags_alert_color, c_color( 255, 90, 90, 255 ) );

		ADD_VARIABLE( bool, m_damage_numbers, false );
		ADD_VARIABLE( c_color, m_damage_numbers_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( float, m_damage_numbers_duration, 1.5f );
		ADD_VARIABLE( float, m_damage_numbers_speed, 20.f );
		ADD_VARIABLE( bool, m_damage_numbers_stack, true );

		ADD_VARIABLE( bool, m_chams_enable, true );
		ADD_VARIABLE( bool, m_chams_local_enable, true );

		ADD_VARIABLE_VECTOR( int, chams_max_layers, m_chams_visible_layers, -1 );
		ADD_VARIABLE_VECTOR( c_color, chams_max_layers, m_chams_visible_colors, c_color( 114, 0, 221, 153 ) );

		ADD_VARIABLE_VECTOR( int, chams_max_layers, m_chams_occluded_layers, -1 );
		ADD_VARIABLE_VECTOR( c_color, chams_max_layers, m_chams_occluded_colors, c_color( 0, 200, 221, 153 ) );

		ADD_VARIABLE( bool, m_chams_ragdolls, false );

		ADD_VARIABLE_VECTOR( int, chams_max_layers, m_chams_backtrack_layers, -1 );
		ADD_VARIABLE_VECTOR( c_color, chams_max_layers, m_chams_backtrack_colors, c_color( 114, 155, 221, 153 ) );
		ADD_VARIABLE( bool, m_chams_backtrack_xqz, false );
		ADD_VARIABLE( int, m_chams_backtrack_type, 0 );
		ADD_VARIABLE( bool, m_chams_backtrack_gradient, false );
		ADD_VARIABLE( int, m_chams_backtrack_gradient_range, 64 );
		ADD_VARIABLE( int, m_chams_backtrack_max_ghosts, 8 );

		ADD_VARIABLE( float, m_sound_esp_min_volume, 0.f );
		ADD_VARIABLE( float, m_sound_esp_max_distance, 0.f );

		ADD_VARIABLE_VECTOR( int, chams_max_layers, m_chams_sound_layers, -1 );
		ADD_VARIABLE_VECTOR( c_color, chams_max_layers, m_chams_sound_colors, c_color( 255, 160, 0, 153 ) );
		ADD_VARIABLE( float, m_chams_sound_duration, 1.f );
		ADD_VARIABLE( bool, m_chams_sound_fade, true );

		ADD_VARIABLE_VECTOR( int, chams_max_layers, m_chams_arms_layers, -1 );
		ADD_VARIABLE_VECTOR( c_color, chams_max_layers, m_chams_arms_colors, c_color( 114, 0, 221, 153 ) );

		ADD_VARIABLE_VECTOR( int, chams_max_layers, m_chams_sleeve_layers, -1 );
		ADD_VARIABLE_VECTOR( c_color, chams_max_layers, m_chams_sleeve_colors, c_color( 0, 200, 221, 153 ) );

		ADD_VARIABLE_VECTOR( int, chams_max_layers, m_chams_weapon_layers, -1 );
		ADD_VARIABLE_VECTOR( c_color, chams_max_layers, m_chams_weapon_colors, c_color( 114, 155, 221, 153 ) );

		ADD_VARIABLE( bool, m_dropped_weapons, false );
		ADD_VARIABLE( bool, m_dropped_weapons_box, false );
		ADD_VARIABLE( bool, m_dropped_weapons_box_corner, false );
		ADD_VARIABLE( bool, m_dropped_weapons_box_outline, false );
		ADD_VARIABLE( c_color, m_dropped_weapons_box_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_dropped_weapons_box_outline_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( bool, m_dropped_weapons_name, false );
		ADD_VARIABLE( c_color, m_dropped_weapons_name_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_dropped_weapons_icon, false );
		ADD_VARIABLE( c_color, m_dropped_weapons_icon_color, c_color( 255, 255, 255, 255 ) );

		ADD_VARIABLE( bool, m_bomb_esp, false );
		ADD_VARIABLE( bool, m_bomb_esp_icon, true );
		ADD_VARIABLE( bool, m_bomb_esp_timer, true );
		ADD_VARIABLE( c_color, m_bomb_esp_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_bomb_esp_defuse_color, c_color( 90, 255, 130, 255 ) );
		ADD_VARIABLE( c_color, m_bomb_esp_fail_color, c_color( 255, 90, 90, 255 ) );
		ADD_VARIABLE( bool, m_bomb_timer_bar, false );
		ADD_VARIABLE( int, m_bomb_timer_bar_position, 60 );

		ADD_VARIABLE( bool, m_thrown_objects, false );
		ADD_VARIABLE( bool, m_thrown_objects_name, false );
		ADD_VARIABLE( c_color, m_thrown_objects_name_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_thrown_objects_icon, false );
		ADD_VARIABLE( c_color, m_thrown_objects_icon_color, c_color( 255, 255, 255, 255 ) );

		ADD_VARIABLE( bool, m_grenade_path, false );
		ADD_VARIABLE( c_color, m_grenade_path_color, c_color( 90, 160, 255, 255 ) );
		ADD_VARIABLE( c_color, m_grenade_path_bounce_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_grenade_path_detonate_color, c_color( 255, 90, 90, 255 ) );
		ADD_VARIABLE( float, m_grenade_path_thickness, 1.5f );
		ADD_VARIABLE( bool, m_grenade_path_radius, true );

		ADD_VARIABLE( bool, m_precipitation, false );
		ADD_VARIABLE( int, m_precipitation_type, 0 );
		ADD_VARIABLE( bool, m_fog, false );
		ADD_VARIABLE( float, m_fog_start, 0.f );
		ADD_VARIABLE( float, m_fog_end, 0.f );
		ADD_VARIABLE( c_color, m_fog_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( float, m_fog_density, 100.f );

		ADD_VARIABLE( int, m_skybox, e_skybox_type::skybox_off );
		ADD_VARIABLE( std::string, m_skybox_custom, "" );

		ADD_VARIABLE( bool, m_sun_angle, false );
		ADD_VARIABLE( float, m_sun_angle_pitch, 50.f );
		ADD_VARIABLE( float, m_sun_angle_yaw, 43.f );
		ADD_VARIABLE( float, m_sun_angle_roll, 0.f );
		ADD_VARIABLE( float, m_sun_angle_speed, 0.f );
		ADD_VARIABLE( float, m_sun_angle_shadow_distance, 0.f );

		ADD_VARIABLE( bool, m_world_modulation, false );
		ADD_VARIABLE( c_color, m_world_modulation_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( float, m_world_modulation_brightness, 100.f );

		ADD_VARIABLE( bool, m_prop_modulation, false );
		ADD_VARIABLE( c_color, m_prop_modulation_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( float, m_prop_modulation_brightness, 100.f );

		ADD_VARIABLE( bool, m_skybox_modulation, false );
		ADD_VARIABLE( c_color, m_skybox_modulation_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( float, m_skybox_modulation_brightness, 100.f );

		ADD_VARIABLE( bool, m_custom_smoke, false );
		ADD_VARIABLE( c_color, m_custom_smoke_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_custom_molotov, false );
		ADD_VARIABLE( c_color, m_custom_molotov_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_custom_blood, false );
		ADD_VARIABLE( c_color, m_custom_blood_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_custom_precipitation, false );
		ADD_VARIABLE( c_color, m_custom_precipitation_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_fullbright, false );
		ADD_VARIABLE( bool, m_flip_world, false );
		ADD_VARIABLE( bool, m_flip_world_mirror_hand, false );
		ADD_VARIABLE( bool, m_dlight, false );
		ADD_VARIABLE( c_color, m_dlight_color, c_color( 255, 120, 60, 255 ) );
		ADD_VARIABLE( bool, m_dlight_local, false );
		ADD_VARIABLE( c_color, m_dlight_local_color, c_color( 120, 160, 255, 255 ) );
		ADD_VARIABLE( float, m_dlight_radius, 150.f );
		ADD_VARIABLE( int, m_dlight_brightness, 5 );
		ADD_VARIABLE( int, m_ragdoll_gravity, 0 );
		ADD_VARIABLE( float, m_ragdoll_gravity_strength, 1.f );
		ADD_VARIABLE( bool, m_disable_post_processing, false );
		ADD_VARIABLE( bool, m_remove_panorama_blur, false );
		ADD_VARIABLE( bool, m_remove_3d_skybox, false );
		ADD_VARIABLE( bool, m_remove_hud_elements, false );
		ADD_VARIABLE_VECTOR( bool, e_hud_elements::hud_element_max, m_removed_hud_elements, false );
		ADD_VARIABLE( bool, m_keep_killfeed, false );
		ADD_VARIABLE( bool, m_old_shaders, false );
		ADD_VARIABLE( bool, m_old_shaders_weapons, true );
		ADD_VARIABLE( bool, m_old_shaders_arms, true );
		ADD_VARIABLE( bool, m_old_shaders_players, true );
		ADD_VARIABLE( bool, m_old_shaders_world, true );
		ADD_VARIABLE( bool, m_old_shaders_props, true );
		ADD_VARIABLE( bool, m_world_texture, false );
		ADD_VARIABLE_VECTOR( int, 7, m_world_texture_preset, 0 );
		ADD_VARIABLE_VECTOR( std::string, 7, m_world_texture_custom, "" );
		ADD_VARIABLE( bool, m_world_texture_remove_decals, false );
		ADD_VARIABLE( bool, m_prop_texture, false );
		ADD_VARIABLE( int, m_prop_texture_preset, 0 );
		ADD_VARIABLE( std::string, m_prop_texture_custom, "" );
		ADD_VARIABLE( bool, m_remove_ragdolls, false );
		ADD_VARIABLE( bool, m_remove_smoke, false );
		ADD_VARIABLE( int, m_remove_smoke_mode, 0 );
		ADD_VARIABLE( bool, m_remove_flash, false );
		ADD_VARIABLE( float, m_remove_flash_opacity, 0.f );
		ADD_VARIABLE( bool, m_remove_visual_recoil, false );

		ADD_VARIABLE( bool, m_bullet_tracers, false );
		ADD_VARIABLE( bool, m_bullet_tracers_local, true );
		ADD_VARIABLE( bool, m_bullet_tracers_enemy, false );
		ADD_VARIABLE( bool, m_bullet_tracers_team, false );
		ADD_VARIABLE( c_color, m_bullet_tracers_local_color, c_color( 150, 130, 255, 255 ) );
		ADD_VARIABLE( c_color, m_bullet_tracers_enemy_color, c_color( 255, 80, 80, 255 ) );
		ADD_VARIABLE( c_color, m_bullet_tracers_team_color, c_color( 80, 160, 255, 255 ) );
		ADD_VARIABLE( int, m_bullet_tracers_sprite, 0 );
		ADD_VARIABLE( float, m_bullet_tracers_life, 3.f );
		ADD_VARIABLE( float, m_bullet_tracers_width, 2.f );

		ADD_VARIABLE( bool, m_movement_trail, false );
		ADD_VARIABLE( c_color, m_movement_trail_color, c_color( 150, 130, 255, 255 ) );
		ADD_VARIABLE( c_color, m_movement_trail_crouch_color, c_color( 255, 130, 200, 255 ) );
		ADD_VARIABLE( bool, m_movement_trail_rainbow, false );
		ADD_VARIABLE( bool, m_movement_trail_air_only, true );
		ADD_VARIABLE( int, m_movement_trail_sprite, 1 );
		ADD_VARIABLE( float, m_movement_trail_life, 2.5f );
		ADD_VARIABLE( float, m_movement_trail_width, 3.f );

		ADD_VARIABLE( bool, m_bullet_impacts, false );
		ADD_VARIABLE( c_color, m_bullet_impacts_client_color, c_color( 255, 0, 0, 127 ) );
		ADD_VARIABLE( c_color, m_bullet_impacts_server_color, c_color( 0, 0, 255, 127 ) );
		ADD_VARIABLE( float, m_bullet_impacts_size, 2.f );
		ADD_VARIABLE( float, m_bullet_impacts_duration, 4.f );

		ADD_VARIABLE( bool, m_world_hit_marker, false );
		ADD_VARIABLE( c_color, m_world_hit_marker_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( float, m_world_hit_marker_size, 6.f );
		ADD_VARIABLE( float, m_world_hit_marker_duration, 1.f );

		ADD_VARIABLE( bool, m_third_person, false );
		ADD_VARIABLE( key_bind_t, m_third_person_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( float, m_third_person_distance, 100.f );
		ADD_VARIABLE( bool, m_third_person_collision, true );
		ADD_VARIABLE( bool, m_aimbot_fov_circle, false );
		ADD_VARIABLE( c_color, m_aimbot_fov_circle_color, c_color( 255, 255, 255, 140 ) );
		ADD_VARIABLE( float, m_aimbot_fov_circle_thickness, 1.f );
		ADD_VARIABLE( bool, m_aimbot_fov_circle_key_only, false );

		ADD_VARIABLE( bool, m_aspect_ratio_enable, false );
		ADD_VARIABLE( float, m_aspect_ratio, 1.777778f );
		ADD_VARIABLE( bool, m_aspect_ratio_hud, false );
		ADD_VARIABLE( bool, m_aspect_ratio_overlay, false );
		ADD_VARIABLE( bool, m_aspect_ratio_cheat_overlay, false );
		ADD_VARIABLE( bool, m_resolution_spoof, false );
		ADD_VARIABLE( int, m_resolution_spoof_width, 1024 );
		ADD_VARIABLE( int, m_resolution_spoof_height, 768 );
		ADD_VARIABLE( bool, m_resolution_spoof_dump, false );
		ADD_VARIABLE( int, m_resolution_spoof_debug, 0 );
		ADD_VARIABLE( bool, m_resolution_spoof_render_scale, true );
		/* dead end: mat_setvideomode (real mode change) CRASHES the game while injected */
		ADD_VARIABLE( bool, m_resolution_spoof_overlay, true );
		ADD_VARIABLE( bool, m_resolution_spoof_net_graph, true );

		ADD_VARIABLE( bool, m_color_correction, false );
		ADD_VARIABLE( float, m_color_correction_exposure, 0.f );
		ADD_VARIABLE( float, m_color_correction_red, 1.f );
		ADD_VARIABLE( float, m_color_correction_green, 1.f );
		ADD_VARIABLE( float, m_color_correction_blue, 1.f );
		ADD_VARIABLE( float, m_color_correction_hue, 0.f );
		ADD_VARIABLE( float, m_color_correction_saturation, 1.f );
		ADD_VARIABLE( float, m_color_correction_contrast, 1.f );
		ADD_VARIABLE( float, m_color_correction_temperature, 0.f );
		ADD_VARIABLE( float, m_color_correction_levels_in_black, 0.f );
		ADD_VARIABLE( float, m_color_correction_levels_in_white, 1.f );
		ADD_VARIABLE( float, m_color_correction_levels_gamma, 1.f );
		ADD_VARIABLE( float, m_color_correction_levels_out_black, 0.f );
		ADD_VARIABLE( float, m_color_correction_levels_out_white, 1.f );
		ADD_VARIABLE( float, m_color_correction_highlights, 0.f );
		ADD_VARIABLE( float, m_color_correction_shadows, 0.f );
		ADD_VARIABLE_VECTOR( float, 24, m_color_correction_curve_points, -1.f );
		ADD_VARIABLE_VECTOR( float, 24, m_color_correction_curve_red_points, -1.f );
		ADD_VARIABLE_VECTOR( float, 24, m_color_correction_curve_green_points, -1.f );
		ADD_VARIABLE_VECTOR( float, 24, m_color_correction_curve_blue_points, -1.f );
		ADD_VARIABLE( bool, m_sharpen, false );
		ADD_VARIABLE( float, m_sharpen_strength, 0.65f );
		ADD_VARIABLE( float, m_sharpen_threshold, 0.f );
		ADD_VARIABLE( bool, m_film_grain, false );
		ADD_VARIABLE( float, m_film_grain_intensity, 0.05f );
		ADD_VARIABLE( float, m_film_grain_size, 1.6f );
		ADD_VARIABLE( float, m_film_grain_response_position, 0.5f );
		ADD_VARIABLE( float, m_film_grain_response_range, 0.5f );
		ADD_VARIABLE( float, m_film_grain_response_minimum, 0.f );
		ADD_VARIABLE( bool, m_vignette, false );
		ADD_VARIABLE( c_color, m_vignette_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( float, m_vignette_radius, 0.45f );
		ADD_VARIABLE( float, m_vignette_feather, 0.5f );
		ADD_VARIABLE( float, m_vignette_aspect, 1.f );
		ADD_VARIABLE( float, m_vignette_blur, 0.f );
		ADD_VARIABLE( bool, m_vignette_invert, false );
		ADD_VARIABLE( bool, m_invert_filter, false );
		ADD_VARIABLE( int, m_invert_filter_mode, 0 );
		ADD_VARIABLE( float, m_invert_filter_strength, 1.f );
		ADD_VARIABLE( bool, m_posterize, false );
		ADD_VARIABLE( int, m_posterize_levels, 6 );
		ADD_VARIABLE( float, m_posterize_dither, 0.f );
		ADD_VARIABLE( float, m_posterize_strength, 1.f );
		ADD_VARIABLE( bool, m_deband, false );
		ADD_VARIABLE( int, m_deband_iterations, 2 );
		ADD_VARIABLE( float, m_deband_strength, 10.f );
		ADD_VARIABLE( float, m_deband_radius, 14.f );
		ADD_VARIABLE( float, m_deband_grain, 16.f );
		ADD_VARIABLE( bool, m_channel_shift, false );
		ADD_VARIABLE( float, m_channel_shift_red_x, 3.f );
		ADD_VARIABLE( float, m_channel_shift_red_y, 0.f );
		ADD_VARIABLE( float, m_channel_shift_green_x, 0.f );
		ADD_VARIABLE( float, m_channel_shift_green_y, 0.f );
		ADD_VARIABLE( float, m_channel_shift_blue_x, -3.f );
		ADD_VARIABLE( float, m_channel_shift_blue_y, 0.f );
		ADD_VARIABLE( bool, m_depth_of_field, false );
		ADD_VARIABLE( bool, m_depth_of_field_auto_focus, true );
		ADD_VARIABLE( float, m_depth_of_field_focus_x, 0.5f );
		ADD_VARIABLE( float, m_depth_of_field_focus_y, 0.5f );
		ADD_VARIABLE( float, m_depth_of_field_focus_speed, 8.f );
		ADD_VARIABLE( float, m_depth_of_field_focus_limit, 4000.f );
		ADD_VARIABLE( bool, m_depth_of_field_focus_see_through, false );
		ADD_VARIABLE( float, m_depth_of_field_distance, 400.f );
		ADD_VARIABLE( float, m_depth_of_field_range, 300.f );
		ADD_VARIABLE( float, m_depth_of_field_ramp, 600.f );
		ADD_VARIABLE( float, m_depth_of_field_max_blur, 20.f );
		ADD_VARIABLE( int, m_depth_of_field_style, 0 );
		ADD_VARIABLE( int, m_depth_of_field_quality, 2 );
		ADD_VARIABLE( bool, m_depth_of_field_smooth, true );
		ADD_VARIABLE( float, m_depth_of_field_edge_softness, 0.75f );
		ADD_VARIABLE( float, m_depth_of_field_bokeh_boost, 4.f );
		ADD_VARIABLE( float, m_depth_of_field_bokeh_threshold, 0.6f );
		ADD_VARIABLE( bool, m_depth_of_field_near_blur, true );
		ADD_VARIABLE( bool, m_depth_of_field_viewmodel_sharp, true );
		/* leaves mat_queue_mode alone. the pass normally forces 0: a mid frame raw d3d draw can't run against
		   a render thread a frame behind. may glitch per machine. SHARED with AO (one convar). */
		ADD_VARIABLE( bool, m_depth_of_field_keep_multicore, false );
		ADD_VARIABLE( bool, m_depth_of_field_debug_depth, false );
		ADD_VARIABLE( float, m_depth_of_field_debug_white, 2000.f );
		ADD_VARIABLE( bool, m_depth_of_field_debug_near, false );
		/* depth_source route 3 ( own INTZ = the z buffer ) even where RESZ / NvAPI work. it's what gpus
		   without either get, so test it here. msaa must be off. SHARED by every depth pass */
		ADD_VARIABLE( bool, m_depth_force_live, false );

		ADD_VARIABLE( bool, m_reflections, false );
		ADD_VARIABLE( float, m_reflections_intensity, 1.f );
		ADD_VARIABLE( float, m_reflections_distance, 0.8f );
		ADD_VARIABLE( float, m_reflections_blur, 0.5f );
		ADD_VARIABLE( float, m_reflections_falloff, 0.f );
		ADD_VARIABLE( float, m_reflections_scale, 1.f );
		ADD_VARIABLE( int, m_reflections_quality, 2 );
		ADD_VARIABLE( bool, m_reflections_smooth_normals, true );

		ADD_VARIABLE( bool, m_emphasize, false );
		ADD_VARIABLE( float, m_emphasize_focus_depth, 0.f );
		ADD_VARIABLE( float, m_emphasize_focus_range, 0.f );
		ADD_VARIABLE( float, m_emphasize_focus_edge, 0.001f );
		ADD_VARIABLE( bool, m_emphasize_spherical, true );
		ADD_VARIABLE( int, m_emphasize_sphere_fov, 180 );
		ADD_VARIABLE( float, m_emphasize_sphere_x, 0.537f );
		ADD_VARIABLE( float, m_emphasize_sphere_y, 0.5f );
		ADD_VARIABLE( c_color, m_emphasize_blend_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( float, m_emphasize_blend_factor, 0.2f );
		ADD_VARIABLE( float, m_emphasize_effect_factor, 1.f );

		ADD_VARIABLE( bool, m_show_depth, false );
		ADD_VARIABLE( int, m_show_depth_present_type, 0 );
		ADD_VARIABLE( bool, m_show_depth_blend, false );
		ADD_VARIABLE( bool, m_show_depth_live_preview, false );
		ADD_VARIABLE( bool, m_show_depth_upside_down, false );
		ADD_VARIABLE( bool, m_show_depth_reversed, false );
		ADD_VARIABLE( bool, m_show_depth_logarithmic, false );
		ADD_VARIABLE( float, m_show_depth_scale_x, 1.f );
		ADD_VARIABLE( float, m_show_depth_scale_y, 1.f );
		ADD_VARIABLE( int, m_show_depth_offset_x, 0 );
		ADD_VARIABLE( int, m_show_depth_offset_y, 0 );
		ADD_VARIABLE( float, m_show_depth_far_plane, 1000.f );
		ADD_VARIABLE( float, m_show_depth_multiplier, 1.f );

		ADD_VARIABLE( bool, m_ambient_occlusion, false );
		ADD_VARIABLE( float, m_ambient_occlusion_radius, 40.f );
		ADD_VARIABLE( float, m_ambient_occlusion_intensity, 1.6f );
		ADD_VARIABLE( int, m_ambient_occlusion_samples, 12 );
		ADD_VARIABLE( float, m_ambient_occlusion_scale, 0.75f );
		ADD_VARIABLE( float, m_ambient_occlusion_bias, 0.2f );
		ADD_VARIABLE( float, m_ambient_occlusion_blur, 1.f );
		ADD_VARIABLE( float, m_ambient_occlusion_fade_start, 1200.f );
		ADD_VARIABLE( float, m_ambient_occlusion_fade_end, 3000.f );
		ADD_VARIABLE( bool, m_ambient_occlusion_fog_fade, true );
		ADD_VARIABLE( bool, m_ambient_occlusion_smoke_fade, true );
		ADD_VARIABLE( bool, m_ambient_occlusion_viewmodel, true );
		ADD_VARIABLE( bool, m_ambient_occlusion_debug, false );

		ADD_VARIABLE( bool, m_motion_blur, false );
		ADD_VARIABLE( bool, m_motion_blur_forward, false );
		ADD_VARIABLE( float, m_motion_blur_strength, 2.f );
		ADD_VARIABLE( float, m_motion_blur_rotation_intensity, 1.f );
		ADD_VARIABLE( float, m_motion_blur_falling_intensity, 0.f );
		ADD_VARIABLE( float, m_motion_blur_roll_intensity, 0.3f );
		ADD_VARIABLE( float, m_motion_blur_falling_min, 5.f );
		ADD_VARIABLE( float, m_motion_blur_falling_max, 10.f );

		ADD_VARIABLE( bool, m_true_motion_blur, false );
		ADD_VARIABLE( float, m_true_motion_blur_shutter, 8.f );
		ADD_VARIABLE( float, m_true_motion_blur_camera, 1.f );
		ADD_VARIABLE( bool, m_true_motion_blur_players, true );
		ADD_VARIABLE( float, m_true_motion_blur_object, 0.f );
		ADD_VARIABLE( int, m_true_motion_blur_samples, 32 );
		ADD_VARIABLE( float, m_true_motion_blur_max, 0.06f );
		ADD_VARIABLE( bool, m_true_motion_blur_viewmodel, true );
		ADD_VARIABLE( float, m_true_motion_blur_viewmodel_strength, 1.f );
		ADD_VARIABLE( float, m_true_motion_blur_viewmodel_sway, 0.f );
		ADD_VARIABLE( bool, m_true_motion_blur_animation, false );
		ADD_VARIABLE( float, m_true_motion_blur_animation_trail, 2.f );

		ADD_VARIABLE( bool, m_bloom, false );
		ADD_VARIABLE( float, m_bloom_intensity, 1.2f );
		ADD_VARIABLE( float, m_bloom_curve, 1.5f );
		ADD_VARIABLE( float, m_bloom_radius, 0.67f );
		ADD_VARIABLE( float, m_bloom_saturation, 2.f );
		ADD_VARIABLE( float, m_bloom_aspect, 0.f );

		/* visuals - viewmodel. offset + roll go in override_view, which runs after CalcViewModelView. fov must not:
		   fovViewmodel is written after OverrideView, so draw_view_models writes it. no master toggle, each row gates itself */
		ADD_VARIABLE( bool, m_viewmodel_offset_enable, false );
		ADD_VARIABLE( float, m_viewmodel_x, 0.f );
		ADD_VARIABLE( float, m_viewmodel_y, 0.f );
		ADD_VARIABLE( float, m_viewmodel_z, 0.f );
		ADD_VARIABLE( float, m_viewmodel_roll, 0.f );
		ADD_VARIABLE( bool, m_viewmodel_fov_enable, false );
		ADD_VARIABLE( float, m_viewmodel_fov, 68.f );
		ADD_VARIABLE( bool, m_world_fov_enable, false );
		ADD_VARIABLE( float, m_world_fov, 0.f );
		ADD_VARIABLE( bool, m_weapon_sway, false );
		ADD_VARIABLE( float, m_weapon_sway_scale, 1.6f );
		ADD_VARIABLE( bool, m_weapon_sheen, false );
		ADD_VARIABLE( c_color, m_weapon_sheen_color, c_color( 200, 20, 15, 255 ) );
		ADD_VARIABLE( float, m_weapon_sheen_delay, 5.f );
		ADD_VARIABLE( float, m_weapon_sheen_width, 0.4f );
		ADD_VARIABLE( bool, m_viewmodel_bob, false );
		ADD_VARIABLE( float, m_viewmodel_bob_cycle, 0.98f );
		ADD_VARIABLE( float, m_viewmodel_bob_vertical, 0.25f );
		ADD_VARIABLE( float, m_viewmodel_bob_lateral, 0.4f );
		ADD_VARIABLE( float, m_viewmodel_bob_lower, 21.f );
		ADD_VARIABLE( float, m_viewmodel_shift_left, 1.5f );
		ADD_VARIABLE( float, m_viewmodel_shift_right, 0.75f );

		ADD_VARIABLE( bool, m_bunny_hop, false );
		ADD_VARIABLE( int, m_bunny_hop_chance, 100 );
		ADD_VARIABLE( bool, m_delay_hop, false );
		ADD_VARIABLE( key_bind_t, m_delay_hop_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( int, m_delay_hop_ticks, 2 );
		ADD_VARIABLE( bool, m_no_crouch_cooldown, false );
		ADD_VARIABLE( bool, m_fast_stop, false );
		ADD_VARIABLE( bool, m_edge_jump, false );
		ADD_VARIABLE( key_bind_t, m_edge_jump_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_edge_jump_ladder, false );
		ADD_VARIABLE( bool, m_long_jump, false );
		ADD_VARIABLE( key_bind_t, m_long_jump_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_long_jump_adaptive, false );
		ADD_VARIABLE_VECTOR( bool, 3, m_long_jump_adaptive_stop, true );
		ADD_VARIABLE( float, m_long_jump_adaptive_time, 0.3f );
		ADD_VARIABLE( bool, m_long_jump_edge_jump, false );
		ADD_VARIABLE( bool, m_long_jump_edge_jump_ladder, false );
		ADD_VARIABLE( bool, m_mini_jump, false );
		ADD_VARIABLE( key_bind_t, m_mini_jump_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_mini_jump_hold_duck, false );
		ADD_VARIABLE( bool, m_mini_jump_edge_jump, true );
		ADD_VARIABLE( bool, m_jump_bug, false );
		ADD_VARIABLE( key_bind_t, m_jump_bug_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_jump_bug_crouch, false );
		ADD_VARIABLE( bool, m_jump_bug_kangaroo, false );
		ADD_VARIABLE( bool, m_pixel_surf, false );
		ADD_VARIABLE( key_bind_t, m_pixel_surf_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_pixel_surf_fix, false );
		ADD_VARIABLE( int, m_pixel_surf_predict_ticks, 12 );
		ADD_VARIABLE( bool, m_pixel_surf_stay, false );
		ADD_VARIABLE( bool, m_tung_surf, false );
		ADD_VARIABLE( key_bind_t, m_tung_surf_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_tung_surf_y6, false );
		ADD_VARIABLE( float, m_tung_surf_snap, 0.f );
		/* move keys don't catch or hold the pixel, press into the wall instead. keys clearly away = never */
		ADD_VARIABLE( bool, m_tung_surf_press, false );
		ADD_VARIABLE( int, m_pixel_surf_sound, 0 );
		ADD_VARIABLE( std::string, m_pixel_surf_sound_custom, "" );
		ADD_VARIABLE( float, m_pixel_surf_sound_volume, 1.f );
		ADD_VARIABLE( bool, m_air_stuck, false );
		ADD_VARIABLE( key_bind_t, m_air_stuck_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( float, m_air_stuck_extra_reach, 4.f );
		ADD_VARIABLE( float, m_air_stuck_catch_range, 10.f );
		ADD_VARIABLE( bool, m_air_stuck_display_screen, true );
		ADD_VARIABLE( float, m_air_stuck_budget, 50.f );
		ADD_VARIABLE( bool, m_air_freeze, false );
		ADD_VARIABLE( key_bind_t, m_air_freeze_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_wall_climb, false );
		ADD_VARIABLE( key_bind_t, m_wall_climb_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_wall_climb_prevent_slow, true );
		ADD_VARIABLE( bool, m_auto_strafe, false );
		ADD_VARIABLE( key_bind_t, m_auto_strafe_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( int, m_auto_strafe_type, 0 );
		ADD_VARIABLE( bool, m_auto_strafe_last_keys, false );
		ADD_VARIABLE( bool, m_auto_strafe_last_mouse, false );
		ADD_VARIABLE( bool, m_auto_strafe_avoid_walls, false );
		ADD_VARIABLE( float, m_auto_strafe_avoid_dist, 50.f );
		ADD_VARIABLE( bool, m_strafe_optimizer, false );
		ADD_VARIABLE( float, m_strafe_optimizer_gain, 100.f );
		ADD_VARIABLE( float, m_strafe_optimizer_min_speed, 30.f );
		ADD_VARIABLE( bool, m_pixel_surf_assist, false );
		ADD_VARIABLE( key_bind_t, m_pixel_surf_assist_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( int, m_pixel_surf_assist_ticks, 12 );
		ADD_VARIABLE( int, m_pixel_surf_assist_type, 0 );
		ADD_VARIABLE( bool, m_pixel_surf_assist_render, false );
		ADD_VARIABLE( bool, m_pixel_surf_assist_show_points, true );
		ADD_VARIABLE( float, m_pixel_surf_assist_point_dist, 1500.f );
		ADD_VARIABLE( bool, m_auto_align, false );

		ADD_VARIABLE( bool, edge_bug, false );
		ADD_VARIABLE( key_bind_t, edge_bug_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_edgebug_edge_skip, false );
		ADD_VARIABLE( key_bind_t, m_edge_skip_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_edge_skip_use_eb, true );
		ADD_VARIABLE( float, m_edge_skip_predict_time, 0.35f );
		ADD_VARIABLE( float, m_edge_skip_max_angle, 180.f );
		ADD_VARIABLE( int, m_edge_skip_search_amount, 2 );
		ADD_VARIABLE( bool, m_edge_skip_mouse_lock, false );
		ADD_VARIABLE( int, m_edge_skip_mouse_lock_type, 1 );
		ADD_VARIABLE( float, m_edge_skip_mouse_lock_strength, 100.f );
		ADD_VARIABLE( int, m_edge_skip_sound, 0 );
		ADD_VARIABLE( std::string, m_edge_skip_sound_custom, "" );
		ADD_VARIABLE( float, m_edge_skip_sound_volume, 1.f );
		ADD_VARIABLE( bool, m_edge_skip_visualize, false );
		ADD_VARIABLE( int, m_edge_bug_sound, 0 );
		ADD_VARIABLE( std::string, m_edge_bug_sound_custom, "" );
		ADD_VARIABLE( float, m_edge_bug_sound_volume, 1.f );
		ADD_VARIABLE( float, m_edgebug_predict_time, 0.35f );
		ADD_VARIABLE( bool, m_healthshot_effect, false );
		ADD_VARIABLE_VECTOR( bool, 3, m_healthshot_triggers, true );
		ADD_VARIABLE( float, m_healthshot_duration, 1.f );
		ADD_VARIABLE( bool, m_death_particles, false );
		ADD_VARIABLE( int, m_death_particles_type, 0 );
		ADD_VARIABLE( int, m_death_particles_soul_style, 0 );
		ADD_VARIABLE( c_color, m_death_particles_color, c_color( 140, 200, 255, 255 ) );
		ADD_VARIABLE( int, m_death_particles_amount, 8 );
		ADD_VARIABLE( float, m_death_particles_size, 1.f );
		ADD_VARIABLE( float, m_death_particles_lifetime, 3.f );
		ADD_VARIABLE( bool, m_death_particles_multicolor, false );
		ADD_VARIABLE( bool, m_death_particles_glow, true );
		ADD_VARIABLE( float, m_death_particles_melt_goo, 1.f );
		ADD_VARIABLE( float, m_death_particles_melt_speed, 1.f );
		ADD_VARIABLE( int, m_death_particles_mc_mode, 0 );
		ADD_VARIABLE( float, m_death_particles_mc_xp_time, 10.f );
		ADD_VARIABLE( int, m_edgebug_detection_mode, 0 );
		ADD_VARIABLE( bool, m_edgebug_autostrafe_to_edge, true );
		ADD_VARIABLE( float, m_edgebug_autostrafe_max_angle, 180.f );
		ADD_VARIABLE_VECTOR( bool, 2, m_edgebug_types, true );
		ADD_VARIABLE( int, m_edgebug_search_amount, 2 );
		ADD_VARIABLE( bool, m_edgebug_mouse_steer, true );
		ADD_VARIABLE( int, m_edgebug_paths, 3 );
		ADD_VARIABLE( bool, m_edgebug_mouse_lock, false );
		ADD_VARIABLE( int, m_edgebug_mouse_lock_type, 1 );
		ADD_VARIABLE( float, m_edgebug_mouse_lock_strength, 100.f );
		ADD_VARIABLE( int, m_edgebug_style, 0 );
		ADD_VARIABLE( int, m_edgebug_del_ticks, 64 );
		ADD_VARIABLE( bool, m_edgebug_del_advanced, false );
		ADD_VARIABLE( float, m_edgebug_del_angle_limit, 50.f );
		ADD_VARIABLE( int, m_edgebug_del_search, 5 );
		ADD_VARIABLE( bool, m_edgebug_del_silent, false );
		ADD_VARIABLE( bool, m_edgebug_del_mouse_fix, true );
		ADD_VARIABLE( float, m_edgebug_dna_scan, 0.4f );
		ADD_VARIABLE( bool, m_edgebug_dna_advanced, false );
		ADD_VARIABLE( int, m_edgebug_dna_range, 6 );
		ADD_VARIABLE( bool, m_edgebug_dna_custom_angle, false );
		ADD_VARIABLE( float, m_edgebug_dna_angle_limit, 3.f );
		ADD_VARIABLE( bool, m_edgebug_visualize, false );
		ADD_VARIABLE( c_color, m_edgebug_visualize_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( float, m_edgebug_visualize_thickness, 1.5f );
		ADD_VARIABLE( int, m_edgebug_visualize_shape, 0 );
		ADD_VARIABLE( int, m_edgebug_visualize_fill, 0 );
		ADD_VARIABLE( float, m_edgebug_visualize_fill_alpha, 0.35f );
		ADD_VARIABLE( c_color, m_edgebug_visualize_pad_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_edgebug_visualize_projected, false );
		ADD_VARIABLE( float, m_edgebug_visualize_size, 16.f );

		ADD_VARIABLE( bool, m_texture_bug, false );
		ADD_VARIABLE( key_bind_t, m_texture_bug_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( float, m_texture_bug_reach, 8.f );
		ADD_VARIABLE_VECTOR( bool, 2, m_texture_bug_types, true );
		ADD_VARIABLE( bool, m_texture_bug_wallstrafe, false );
		ADD_VARIABLE( int, m_texture_bug_sound, 0 );
		ADD_VARIABLE( std::string, m_texture_bug_sound_custom, "" );
		ADD_VARIABLE( float, m_texture_bug_sound_volume, 1.f );
		ADD_VARIABLE( bool, m_fix_air_stucks, false );

		ADD_VARIABLE( bool, m_ladder_bug, false );
		ADD_VARIABLE( key_bind_t, m_ladder_bug_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_ladder_glide, false );
		ADD_VARIABLE( key_bind_t, m_ladder_glide_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_ladder_freelook_climb, false );
		ADD_VARIABLE( key_bind_t, m_ladder_freelook_climb_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_fast_ladder, false );
		ADD_VARIABLE( key_bind_t, m_fast_ladder_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_fire_man, false );
		ADD_VARIABLE( key_bind_t, m_fire_man_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_fire_man_approach, true );
		ADD_VARIABLE( bool, m_fire_man_drop_in, false );
		ADD_VARIABLE( bool, m_fire_man_drop_in_lock, true );
		ADD_VARIABLE( bool, m_fire_man_crouch_jump, true );
		ADD_VARIABLE( bool, m_fire_man_side_only, false );
		ADD_VARIABLE( key_bind_t, m_fire_man_side_only_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( float, m_fire_man_reach, 320.f );
		ADD_VARIABLE( bool, m_fire_man_push_off, true );
		ADD_VARIABLE( bool, m_fire_man_manual_jump, false );
		ADD_VARIABLE( bool, m_fire_man_plus_jump_activates_early, false );
		ADD_VARIABLE( bool, m_auto_one_hop, false );
		ADD_VARIABLE( key_bind_t, m_auto_one_hop_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_blockbot, false );
		ADD_VARIABLE( key_bind_t, m_blockbot_key, key_bind_t( 0, 1 ) );

		ADD_VARIABLE( bool, m_auto_crouch, false );
		ADD_VARIABLE( key_bind_t, m_auto_crouch_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( int, m_auto_crouch_ticks, 6 );

		ADD_VARIABLE( bool, m_auto_bounce, false );
		ADD_VARIABLE( key_bind_t, m_auto_bounce_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( float, m_auto_bounce_min_gain, 0.f );
		ADD_VARIABLE( float, m_auto_bounce_reach, 128.f );
		ADD_VARIABLE( int, m_auto_bounce_mode, 0 );
		ADD_VARIABLE( bool, m_auto_bounce_jump, true );
		ADD_VARIABLE( bool, m_auto_bounce_crouch, false );
		ADD_VARIABLE( bool, m_auto_bounce_align, false );
		ADD_VARIABLE( bool, m_auto_bounce_visualize, false );
		ADD_VARIABLE( c_color, m_auto_bounce_visualize_color, c_color( 120, 200, 255, 255 ) );

		ADD_VARIABLE( bool, m_bouncee_assist, false );
		ADD_VARIABLE( key_bind_t, m_bounce_assist_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_bouncee_assist_brokehop, false );
		ADD_VARIABLE( bool, m_bouncee_assist_render, false );
		/* one key per point list, so bounce candidates don't steal ps
		   presses. migrated from the bounce key on load */
		ADD_VARIABLE( key_bind_t, m_pixel_surf_assist_point_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_bounce_assist_point_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_free_point_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_pixel_finder, true );
		ADD_VARIABLE( key_bind_t, m_pixel_finder_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_pixel_finder_texturebugs, true );
		ADD_VARIABLE( bool, m_pixel_finder_pixelsurfs, true );
		ADD_VARIABLE( bool, m_pixel_finder_headbounces, true );
		ADD_VARIABLE( bool, m_pixel_finder_pixeljumps, true );
		ADD_VARIABLE( bool, m_pixel_surf_assist_brokehop, false );
		ADD_VARIABLE( float, m_pixel_surf_assist_ground_angle_limit, 2.5f );
		ADD_VARIABLE( bool, m_pixel_surf_assist_ground_help, false );
		ADD_VARIABLE( float, m_pixel_surf_assist_render_height, 0.0f );
		ADD_VARIABLE( float, m_pixel_surf_assist_lockfactor, 1.0f );
		ADD_VARIABLE( c_color, m_pixel_surf_line_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_pixel_surf_line_full_trajectory, false );
		ADD_VARIABLE( bool, m_pixel_surf_approach, true );
		ADD_VARIABLE( bool, m_pixel_surf_line_render, false );
		ADD_VARIABLE( float, m_pixel_surf_line_thickness, 2.0f );
		ADD_VARIABLE( int, m_pixel_surf_line_style, 0 );

		ADD_VARIABLE( bool, m_pixel_calc, false );
		ADD_VARIABLE( key_bind_t, m_pixel_calc_aim_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_pixel_calc_solve_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_pixel_calc_show_point, true );
		ADD_VARIABLE( int, m_pixel_calc_moves, 1023 );
		ADD_VARIABLE( int, m_pixel_calc_type, 0 );
		ADD_VARIABLE( bool, m_pixel_calc_advanced_readout, false );

		ADD_VARIABLE( bool, m_route_calc, false );
		ADD_VARIABLE( key_bind_t, m_route_calc_add_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_route_calc_solve_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_route_calc_delete_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_route_calc_clear_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( int, m_route_calc_max_results, 6 );
		ADD_VARIABLE( int, m_route_calc_popup_time, 6 );
		ADD_VARIABLE( int, m_route_calc_delay_ticks, 2 );
		ADD_VARIABLE( bool, m_route_calc_start_here, true );
		ADD_VARIABLE( int, m_route_calc_start_styles, 31 );
		ADD_VARIABLE( int, m_route_calc_global_moves, 1023 );
		/* add a point the pixel finder never marked as a pixelsurf as one anyway */
		ADD_VARIABLE( bool, m_route_calc_allow_invalid, false );
		ADD_VARIABLE( bool, m_route_calc_snap_edge, false );
		ADD_VARIABLE( bool, m_route_calc_show_keys, true );
		ADD_VARIABLE( bool, m_route_calc_show_bar, true );
		ADD_VARIABLE( bool, m_route_calc_advanced_readout, false );
		ADD_VARIABLE( bool, m_route_calc_pinned, false );
		ADD_VARIABLE( bool, m_dist_calc, false );
		ADD_VARIABLE( key_bind_t, m_dist_calc_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_detections, false );
		ADD_VARIABLE_VECTOR( bool, e_detection_types::detect_max, m_detection_types, false );
		ADD_VARIABLE( std::string, m_detection_label, "botox" );
		ADD_VARIABLE( bool, m_detection_stack_eb, false );
		ADD_VARIABLE( bool, m_hit_marker, false );
		ADD_VARIABLE( float, m_hit_marker_size, 10.f );
		ADD_VARIABLE( float, m_hit_marker_speed, 1.f );
		ADD_VARIABLE( float, m_hit_marker_thickness, 1.f );
		ADD_VARIABLE( c_color, m_hit_marker_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_hit_marker_outline, false );
		ADD_VARIABLE( c_color, m_hit_marker_outline_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( float, m_hit_marker_outline_thickness, 1.f );
		ADD_VARIABLE( bool, m_hit_sound, false );
		ADD_VARIABLE( int, m_hit_sound_type, 0 );
		ADD_VARIABLE( std::string, m_hit_sound_custom, "" );
		ADD_VARIABLE( float, m_hit_sound_volume, 1.f );
		ADD_VARIABLE( bool, m_movement_fix, true );
		ADD_VARIABLE( bool, m_silent_view, true );
		ADD_VARIABLE( bool, m_tick_fix_128, false );
		ADD_VARIABLE( bool, m_nulls, false );
		ADD_VARIABLE( bool, m_hsw, false );
		ADD_VARIABLE( key_bind_t, m_hsw_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( int, m_hsw_side, 0 );
		ADD_VARIABLE( bool, m_hsw_allow_w_s, false );
		ADD_VARIABLE( bool, m_hsw_pre_onground, false );
		ADD_VARIABLE( bool, m_hsw_allow_a_d, false );
		ADD_VARIABLE( bool, m_lirili_larila, false );
		ADD_VARIABLE( key_bind_t, m_lirili_larila_key, key_bind_t( 0, 1 ) );

		ADD_VARIABLE( bool, m_velocity_indicator, false );
		ADD_VARIABLE( bool, m_velocity_indicator_show_pre_speed, false );
		ADD_VARIABLE( bool, m_velocity_indicator_fade_alpha, false );
		ADD_VARIABLE( bool, m_velocity_indicator_custom_color, false );
		ADD_VARIABLE( int, m_velocity_indicator_padding, 125 );
		ADD_VARIABLE( int, m_velocity_indicator_style, 0 );
		ADD_VARIABLE( int, m_stamina_indicator_style, 0 );
		ADD_VARIABLE( int, m_key_indicators_style, 0 );
		ADD_VARIABLE( int, m_velocity_indicator_smooth_origin, 0 );
		ADD_VARIABLE( int, m_stamina_indicator_smooth_origin, 0 );
		ADD_VARIABLE( int, m_key_indicators_smooth_origin, 0 );
		ADD_VARIABLE( float, m_key_indicators_static_fade, 0.f );
		ADD_VARIABLE( bool, m_velocity_indicator_shadow, true );
		ADD_VARIABLE( float, m_velocity_indicator_shadow_blur, 0.f );
		ADD_VARIABLE( bool, m_stamina_indicator_shadow, true );
		ADD_VARIABLE( float, m_stamina_indicator_shadow_blur, 0.f );
		ADD_VARIABLE( bool, m_key_indicators_shadow, true );
		ADD_VARIABLE( float, m_key_indicators_shadow_blur, 0.f );
		ADD_VARIABLE( float, m_velocity_indicator_scale, 1.f );
		ADD_VARIABLE( int, m_velocity_indicator_pre_layout, 0 );
		ADD_VARIABLE( float, m_velocity_indicator_pre_scale, 1.f );
		ADD_VARIABLE( int, m_velocity_indicator_pre_align, 0 );
		ADD_VARIABLE( c_color, m_indicator_kami_shadow_color, c_color( 252, 193, 255, 255 ) );
		ADD_VARIABLE( float, m_stamina_indicator_scale, 1.f );
		ADD_VARIABLE( float, m_key_indicators_scale, 1.f );
		ADD_VARIABLE( float, m_key_press_scale, 1.f );

		ADD_VARIABLE( c_color, m_velocity_indicator_color1, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_velocity_indicator_color2, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_velocity_indicator_color3, c_color( 255, 199, 89, 255 ) );
		ADD_VARIABLE( c_color, m_velocity_indicator_color4, c_color( 255, 119, 119, 255 ) );
		ADD_VARIABLE( c_color, m_velocity_indicator_color5, c_color( 30, 255, 109, 255 ) );

		ADD_VARIABLE( bool, m_stamina_indicator, false );
		ADD_VARIABLE( bool, m_stamina_indicator_show_pre_speed, false );
		ADD_VARIABLE( c_color, m_stamina_indicator_color1, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_stamina_indicator_color2, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_stamina_indicator_fade_alpha, false );
		ADD_VARIABLE( int, m_stamina_indicator_padding, 125 );

		ADD_VARIABLE( c_color, m_key_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_key_color_success, c_color( 0, 255, 0, 255 ) );
		ADD_VARIABLE( bool, m_key_indicators_enable, false );
		ADD_VARIABLE( bool, m_key_indicators_horizontal, false );
		ADD_VARIABLE( bool, m_key_indicators_shadow_detect, false );
		ADD_VARIABLE( int, m_key_indicators_position, 100 );
		ADD_VARIABLE_VECTOR( bool, e_keybind_indicators::key_max, m_key_indicators, false );
		ADD_VARIABLE( bool, m_key_indicators_custom_labels, false );
		ADD_VARIABLE_VECTOR( std::string, e_keybind_labels::label_max, m_key_indicators_labels, "" );
		ADD_VARIABLE( bool, m_key_indicators_particles, false );
		ADD_VARIABLE( c_color, m_key_indicators_particle_color, c_color( 175, 210, 170, 255 ) );
		ADD_VARIABLE( bool, m_key_indicators_particle_random, false );
		ADD_VARIABLE( int, m_key_indicators_particle_amount, 12 );
		ADD_VARIABLE( float, m_key_indicators_particle_size, 3.f );
		ADD_VARIABLE( float, m_key_indicators_particle_speed, 140.f );
		ADD_VARIABLE( float, m_key_indicators_particle_gravity, 90.f );
		ADD_VARIABLE( float, m_key_indicators_particle_life, 0.8f );
		ADD_VARIABLE( float, m_key_indicators_particle_cooldown, 0.25f );

		ADD_VARIABLE( c_color, m_velocity_indicator_shadow_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( c_color, m_stamina_indicator_shadow_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( c_color, m_key_indicators_shadow_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( bool, m_key_indicators_outline, false );
		ADD_VARIABLE( c_color, m_key_indicators_outline_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( int, m_key_indicators_outline_px, 1 );
		ADD_VARIABLE( int, m_key_indicators_shadow_x, 1 );
		ADD_VARIABLE( int, m_key_indicators_shadow_y, 1 );
		ADD_VARIABLE( bool, m_velocity_indicator_outline, false );
		ADD_VARIABLE( c_color, m_velocity_indicator_outline_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( int, m_velocity_indicator_outline_px, 1 );
		ADD_VARIABLE( int, m_velocity_indicator_shadow_x, 1 );
		ADD_VARIABLE( int, m_velocity_indicator_shadow_y, 1 );
		ADD_VARIABLE( bool, m_stamina_indicator_outline, false );
		ADD_VARIABLE( c_color, m_stamina_indicator_outline_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( int, m_stamina_indicator_outline_px, 1 );
		ADD_VARIABLE( int, m_stamina_indicator_shadow_x, 1 );
		ADD_VARIABLE( int, m_stamina_indicator_shadow_y, 1 );
		ADD_VARIABLE( bool, m_key_press_outline, false );
		ADD_VARIABLE( c_color, m_key_press_outline_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( int, m_key_press_outline_px, 1 );
		ADD_VARIABLE( int, m_key_press_shadow_x, 1 );
		ADD_VARIABLE( int, m_key_press_shadow_y, 1 );
		ADD_VARIABLE( c_color, m_key_color_predict, c_color( 0, 255, 0, 255 ) );
		ADD_VARIABLE( int, m_key_indicators_spacing, 0 );
		ADD_VARIABLE( int, m_stamina_indicator_pre_layout, 0 );
		ADD_VARIABLE( float, m_stamina_indicator_pre_scale, 1.f );
		ADD_VARIABLE( int, m_stamina_indicator_pre_align, 0 );

		ADD_VARIABLE( bool, m_velocity_graph, false );
		ADD_VARIABLE( int, m_velocity_graph_width, 300 );
		ADD_VARIABLE( int, m_velocity_graph_height, 60 );
		ADD_VARIABLE( int, m_velocity_graph_position, 80 );
		ADD_VARIABLE( int, m_velocity_graph_offset_x, 0 );
		ADD_VARIABLE( int, m_velocity_graph_length, 200 );
		ADD_VARIABLE( float, m_velocity_graph_max, 0.f );
		ADD_VARIABLE( float, m_velocity_graph_thickness, 1.5f );
		ADD_VARIABLE( c_color, m_velocity_graph_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_velocity_graph_gain_colors, false );
		ADD_VARIABLE( c_color, m_velocity_graph_color_gain, c_color( 30, 255, 109, 255 ) );
		ADD_VARIABLE( c_color, m_velocity_graph_color_loss, c_color( 255, 119, 119, 255 ) );
		ADD_VARIABLE( bool, m_velocity_graph_fade_sides, true );
		ADD_VARIABLE( float, m_velocity_graph_fade, 0.2f );
		ADD_VARIABLE( bool, m_velocity_graph_fill, false );
		ADD_VARIABLE( c_color, m_velocity_graph_fill_color, c_color( 255, 255, 255, 40 ) );
		ADD_VARIABLE( bool, m_velocity_graph_jump_speeds, true );
		ADD_VARIABLE( c_color, m_velocity_graph_text_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( float, m_velocity_graph_text_scale, 0.75f );
		ADD_VARIABLE( bool, m_velocity_graph_text_shadow, true );

		ADD_VARIABLE( bool, m_key_press_indicator, false );
		ADD_VARIABLE( int, m_key_press_layout, 0 );
		ADD_VARIABLE( int, m_key_press_released, 0 );
		ADD_VARIABLE( bool, m_key_press_duck_jump, true );
		ADD_VARIABLE( bool, m_key_press_mouse, false );
		ADD_VARIABLE( c_color, m_key_press_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_key_press_color_released, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( float, m_key_press_fade, 0.f );
		ADD_VARIABLE( int, m_key_press_gap_x, 10 );
		ADD_VARIABLE( int, m_key_press_gap_y, 4 );
		ADD_VARIABLE( int, m_key_press_position, 200 );
		ADD_VARIABLE( int, m_key_press_offset_x, 0 );
		ADD_VARIABLE( bool, m_key_press_shadow, true );
		ADD_VARIABLE( float, m_key_press_shadow_blur, 0.f );

		ADD_VARIABLE( bool, m_spectators_list, false );
		ADD_VARIABLE( c_color, m_spectators_list_text_color_one, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_spectators_list_text_color_two, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( int, m_spectators_list_type, 0 );
		ADD_VARIABLE( bool, m_spectators_avatar, false );
		ADD_VARIABLE( bool, m_spectators_always_show, false );
		ADD_VARIABLE( int, m_spectators_list_style, 0 );
		ADD_VARIABLE( float, m_spectators_chillware_max_width, 300.f );
		ADD_VARIABLE( bool, m_spectators_list_show_target, true );
		ADD_VARIABLE( bool, m_spectators_list_always_show, false );
		ADD_VARIABLE( int, m_spectators_list_x, 300 );
		ADD_VARIABLE( int, m_spectators_list_y, 100 );
		ADD_VARIABLE( int, m_spectators_list_width, 230 );
		ADD_VARIABLE( bool, m_spectators_list_interwebz_background, true );
		ADD_VARIABLE( int, m_spectators_list_interwebz_label_x, 0 );
		ADD_VARIABLE( bool, m_spectators_list_delusional_left, false );
		ADD_VARIABLE( bool, m_spectators_inba_rainbow, false );
		ADD_VARIABLE( float, m_spectators_inba_rainbow_speed, 0.6f );
		ADD_VARIABLE( int, m_spectators_lumi_type, 0 );
		ADD_VARIABLE( bool, m_spectators_lumi_hide_gotv, true );
		ADD_VARIABLE( bool, m_spectators_lumi_blur, false );
		ADD_VARIABLE( bool, m_spectators_lumi_glow, false );
		ADD_VARIABLE( bool, m_spectators_lumi_mascot, false );
		ADD_VARIABLE( std::string, m_spectators_lumi_mascot_path, "" );
		ADD_VARIABLE( float, m_spectators_lumi_mascot_size, 60.f );
		ADD_VARIABLE( float, m_spectators_lumi_mascot_x, 0.f );
		ADD_VARIABLE( float, m_spectators_lumi_mascot_y, 0.f );
		ADD_VARIABLE( bool, m_spectators_kamibebra_animate, false );
		ADD_VARIABLE( bool, m_spectators_kamibebra_dynamic, false );
		ADD_VARIABLE( bool, m_spectators_kamibebra_always_show, false );
		ADD_VARIABLE( bool, m_spectators_kamibebra_bold, false );
		ADD_VARIABLE( bool, m_spectators_kamibebra_window_shadow, false );
		ADD_VARIABLE( c_color, m_spectators_kamibebra_local_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_spectators_kamibebra_other_color, c_color( 60, 60, 60, 255 ) );
		ADD_VARIABLE( bool, m_spectators_kamibebra_local_shadow, false );
		ADD_VARIABLE( c_color, m_spectators_kamibebra_local_shadow_color, c_color( 102, 210, 184, 255 ) );
		ADD_VARIABLE( bool, m_spectators_kamibebra_other_shadow, false );
		ADD_VARIABLE( c_color, m_spectators_kamibebra_other_shadow_color, c_color( 0, 0, 0, 128 ) );
		ADD_VARIABLE( bool, m_spectators_kamibebra_custom_title, false );
		ADD_VARIABLE( std::string, m_spectators_kamibebra_title_text, "Spectators" );
		ADD_VARIABLE( bool, m_spectators_kamibebra_gif, false );
		ADD_VARIABLE( int, m_spectators_kamibebra_gif_type, 0 );
		ADD_VARIABLE( std::string, m_spectators_kamibebra_gif_path, "" );
		ADD_VARIABLE( float, m_spectators_kamibebra_gif_size, 64.f );
		ADD_VARIABLE( float, m_spectators_eyes_size, 28.f );
		ADD_VARIABLE( c_color, m_spectators_airplane_title_color, c_color( 254, 254, 254, 254 ) );
		ADD_VARIABLE( c_color, m_spectators_airplane_gradient_color, c_color( 39, 39, 39, 254 ) );
		ADD_VARIABLE( c_color, m_spectators_airplane_local_color, c_color( 0, 254, 0, 254 ) );
		ADD_VARIABLE( c_color, m_spectators_airplane_other_color, c_color( 254, 254, 254, 254 ) );
		ADD_VARIABLE( bool, m_spectators_airplane_outline, false );
		ADD_VARIABLE( int, m_spectators_airplane_title_case, 0 );
		ADD_VARIABLE( bool, m_spectators_bhopcheat_mode, false );
		ADD_VARIABLE( bool, m_scaleform, false );
		ADD_VARIABLE( int, m_scaleform_style, 0 );
		ADD_VARIABLE( bool, m_chud_hud, false );
		ADD_VARIABLE( c_color, m_chud_hud_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_chud_hud_bg, c_color( 0, 0, 0, 217 ) );
		ADD_VARIABLE( float, m_chud_hud_scale, 100.f );
		ADD_VARIABLE( float, m_chud_hud_padding, 28.f );
		ADD_VARIABLE( c_color, m_chud_hud_secondary, c_color( 170, 170, 170, 255 ) );
		ADD_VARIABLE( bool, m_chud_hud_watermark_bg, false );
		ADD_VARIABLE( bool, m_chud_hud_blur, false );
		ADD_VARIABLE( bool, m_chud_hud_spectators, false );
		ADD_VARIABLE( bool, m_chud_hud_spectator_avatars, false );
		/* px cap on a row's TEXT, longer names get panorama's text-overflow: ellipsis */
		ADD_VARIABLE( int, m_chud_hud_spectator_max_width, 300 );
		ADD_VARIABLE( bool, m_chud_hud_shadow, false );
		ADD_VARIABLE( c_color, m_chud_hud_shadow_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( float, m_chud_hud_shadow_blur, 6.f );
		ADD_VARIABLE( float, m_chud_hud_shadow_x, 2.f );
		ADD_VARIABLE( float, m_chud_hud_shadow_y, 2.f );
		ADD_VARIABLE( float, m_chud_hud_shadow_spread, 0.f );
		ADD_VARIABLE( int, m_chud_hud_font, 0 );
		ADD_VARIABLE( std::string, m_chud_hud_font_custom, "" );
		ADD_VARIABLE( float, m_chud_hud_font_nudge_x, 0.f );
		ADD_VARIABLE( float, m_chud_hud_font_nudge_y, 0.f );
		ADD_VARIABLE( float, m_chud_hud_font_scale, 100.f );

		ADD_VARIABLE( std::string, m_def_hud_font, "" );

		ADD_VARIABLE( bool, m_mc_hud, false );
		ADD_VARIABLE( bool, m_mc_hud_crosshair, true );
		ADD_VARIABLE( int, m_mc_hud_gui_scale, 0 );

		ADD_VARIABLE( bool, m_fps_warning, true );

		ADD_VARIABLE( bool, m_auto_revive, false );
		ADD_VARIABLE( int, m_auto_revive_mode, 1 );

		ADD_VARIABLE( bool, m_bot_names, false );
		ADD_VARIABLE( int, m_bot_names_mode, 3 );
		ADD_VARIABLE( std::string, m_bot_names_same, "botox" );
		ADD_VARIABLE( int, m_bot_names_length, 8 );
		ADD_VARIABLE( bool, m_bot_names_no_prefix, true );
		ADD_VARIABLE( bool, m_bot_names_ping, false );
		ADD_VARIABLE( bool, m_bot_names_profile, false );

		ADD_VARIABLE( bool, m_performance, false );
		ADD_VARIABLE( bool, m_performance_threaded_bones, true );
		ADD_VARIABLE( bool, m_performance_force_preload, true );
		ADD_VARIABLE( bool, m_performance_skip_unused_records, true );
		ADD_VARIABLE( bool, m_performance_multicore_render, true );
		ADD_VARIABLE( bool, m_performance_high_priority, false );
		ADD_VARIABLE( bool, m_performance_fast_cores, true );
		ADD_VARIABLE( bool, m_performance_no_focus_sleep, true );

		ADD_VARIABLE( bool, m_watermark, true );
		ADD_VARIABLE( bool, m_watermark_tickrate, true );
		ADD_VARIABLE( bool, m_watermark_fps, true );
		ADD_VARIABLE( bool, m_watermark_time, true );
		ADD_VARIABLE( bool, m_watermark_ping, false );
		ADD_VARIABLE( bool, m_watermark_velocity, false );
		ADD_VARIABLE( int, m_watermark_style, 0 );
		ADD_VARIABLE( std::string, m_watermark_clarity_user, "dev" );
		ADD_VARIABLE( std::string, m_watermark_text, "botox" );
		ADD_VARIABLE( bool, m_watermark_user, false );
		ADD_VARIABLE( int, m_watermark_label, 0 );
		ADD_VARIABLE( float, m_watermark_evolve_alpha, 100.f );
		ADD_VARIABLE( c_color, m_watermark_havoc_color, c_color( 115, 155, 255, 255 ) );
		ADD_VARIABLE( c_color, m_spectators_havoc_color, c_color( 115, 155, 255, 255 ) );
		ADD_VARIABLE( c_color, m_watermark_interium_bg, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( c_color, m_spectators_interium_bg, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( int, m_watermark_lumi_design, 0 );
		ADD_VARIABLE( bool, m_watermark_lumi_animated_title, false );
		ADD_VARIABLE( bool, m_watermark_lumi_no_border, false );
		ADD_VARIABLE( c_color, m_watermark_lumi_title_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( float, m_watermark_lumi_offset_x, 0.f );
		ADD_VARIABLE( float, m_watermark_lumi_offset_y, 0.f );
		ADD_VARIABLE( bool, m_watermark_lumi_glow, false );
		ADD_VARIABLE( float, m_watermark_lumi_glow_radius, 5.f );
		ADD_VARIABLE( float, m_watermark_lumi_glow_opacity, 0.45f );
		ADD_VARIABLE( bool, m_watermark_lumi_shadow_color, false );
		ADD_VARIABLE( bool, m_watermark_skebob_typing, false );
		ADD_VARIABLE( bool, m_watermark_dna_glow, true );
		ADD_VARIABLE( int, m_watermark_dna_glow_ticks, 15 );
		ADD_VARIABLE( bool, m_watermark_dna_glow_spotted, false );
		ADD_VARIABLE( bool, m_spectators_skebob_background, true );
		ADD_VARIABLE( bool, m_spectators_skebob_title, true );
		ADD_VARIABLE( bool, m_watermark_kami_outline, false );
		ADD_VARIABLE( c_color, m_watermark_kami_outline_color, c_color( 252, 194, 255, 255 ) );
		ADD_VARIABLE( bool, m_watermark_kami_rgb_mode, false );
		ADD_VARIABLE( float, m_watermark_kami_rgb_speed, 0.5f );
		ADD_VARIABLE( bool, m_watermark_kami_bold_font, false );
		ADD_VARIABLE( bool, m_watermark_kami_custom_text, false );
		ADD_VARIABLE( std::string, m_watermark_kami_custom_text_value, "botox" );
		ADD_VARIABLE( bool, m_watermark_kami_mikudere_type, false );
		ADD_VARIABLE( bool, m_watermark_kami_text_color_like_stroke, false );
		ADD_VARIABLE( bool, m_watermark_kami_rounding, false );
		ADD_VARIABLE( float, m_watermark_kami_rounding_value, 0.0f );
		ADD_VARIABLE( int, m_watermark_kami_show_flags, 0 );
		ADD_VARIABLE( bool, m_watermark_kami_typing, false );
		ADD_VARIABLE( int, m_watermark_kami_typing_speed, 5 );
		ADD_VARIABLE( int, m_watermark_kami_deleting_speed, 8 );
		ADD_VARIABLE( float, m_watermark_kami_typing_inactive, 2.0f );
		ADD_VARIABLE( float, m_watermark_kami_deleting_inactive, 0.5f );
		ADD_VARIABLE( bool, m_watermark_gif, false );
		ADD_VARIABLE( int, m_watermark_gif_type, 0 );
		ADD_VARIABLE( std::string, m_watermark_gif_path, "" );
		ADD_VARIABLE( int, m_watermark_gif_side, 0 );
		ADD_VARIABLE( float, m_watermark_gif_scale, 100.f );
		ADD_VARIABLE( float, m_watermark_gif_rotation, 0.f );
		ADD_VARIABLE( float, m_watermark_gif_gap, 4.f );
		ADD_VARIABLE( float, m_watermark_gif_offset_x, 0.f );
		ADD_VARIABLE( float, m_watermark_gif_offset_y, 0.f );
		ADD_VARIABLE( float, m_watermark_gif_alpha, 100.f );
		ADD_VARIABLE( bool, m_watermark_gif_flip, false );

		ADD_VARIABLE( bool, m_points, false );
		ADD_VARIABLE( bool, m_points_big_font, false );
		ADD_VARIABLE( float, m_points_appear_speed, 1.f );
		ADD_VARIABLE( float, m_points_speed, 1.f );
		ADD_VARIABLE( float, m_points_x_offset, 0.f );
		ADD_VARIABLE( float, m_points_y_offset, 0.f );

		ADD_VARIABLE( bool, m_media_player, false );
		ADD_VARIABLE( bool, m_media_player_cover_art, true );
		ADD_VARIABLE( int, m_media_player_position, 0 );
		ADD_VARIABLE( bool, m_media_player_background, true );
		ADD_VARIABLE( bool, m_media_player_progress_bar, true );
		ADD_VARIABLE( bool, m_media_player_simple, false );
		ADD_VARIABLE( bool, m_media_player_visualizer, false );
		ADD_VARIABLE( int, m_media_player_visualizer_bar_width, 3 );
		ADD_VARIABLE( float, m_media_player_visualizer_height, 18.f );
		ADD_VARIABLE( bool, m_media_player_visualizer_smooth, false );
		ADD_VARIABLE( bool, m_media_player_visualizer_custom_color, false );
		ADD_VARIABLE( c_color, m_media_player_visualizer_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( bool, m_media_player_lyrics, false );
		ADD_VARIABLE( int, m_media_player_lyrics_rows, 3 );
		ADD_VARIABLE( float, m_media_player_lyrics_offset, 0.f );

		ADD_VARIABLE( bool, m_web, false );
		ADD_VARIABLE( std::string, m_web_url, "https://www.youtube.com/" );
		ADD_VARIABLE( int, m_web_resolution, 720 );
		ADD_VARIABLE( int, m_web_aspect, 0 );
		ADD_VARIABLE( bool, m_web_panel, false );
		ADD_VARIABLE( bool, m_web_panel_always, false );
		ADD_VARIABLE( float, m_web_panel_opacity, 1.f );
		ADD_VARIABLE( float, m_web_scale, 1.f );
		ADD_VARIABLE( float, m_web_round, 4.f );
		ADD_VARIABLE( float, m_web_world_opacity, 1.f );
		ADD_VARIABLE( int, m_web_depth_mode, 0 );
		ADD_VARIABLE( int, m_web_back_mode, 0 );
		ADD_VARIABLE( bool, m_web_auto_play, false );
		ADD_VARIABLE( bool, m_web_play_from_start, false );
		ADD_VARIABLE( bool, m_web_auto_remove, false );
		ADD_VARIABLE( float, m_web_volume, 0.5f );
		ADD_VARIABLE( bool, m_web_volume_distance, false );
		ADD_VARIABLE( float, m_web_volume_near, 128.f );
		ADD_VARIABLE( float, m_web_volume_far, 900.f );
		ADD_VARIABLE( bool, m_web_crosshair_pointer, true );
		ADD_VARIABLE( bool, m_web_click_only, false );
		ADD_VARIABLE( bool, m_web_block_fire, true );
		ADD_VARIABLE( bool, m_web_scroll_input, true );
		ADD_VARIABLE( key_bind_t, m_web_place_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_web_remove_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_web_click_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_web_right_click_key, key_bind_t( 0x02, 1 ) );

		ADD_VARIABLE( bool, m_clantag, false );
		ADD_VARIABLE( std::string, m_clantag_text, "botox" );
		ADD_VARIABLE( int, m_clantag_animation, 0 );
		ADD_VARIABLE( float, m_clantag_speed, 1.f );
		ADD_VARIABLE( float, m_clantag_hold, 2.f );
		ADD_VARIABLE( std::string, m_clantag_frames, "b\nbo\nbot\nboto\nbotox" );

		ADD_VARIABLE( bool, m_discord_rpc, false );
		ADD_VARIABLE( std::string, m_discord_rpc_image, "" );

		ADD_VARIABLE( bool, m_sniper_crosshair, false );
		ADD_VARIABLE( int, m_sniper_crosshair_style, 0 );
		ADD_VARIABLE( c_color, m_sniper_crosshair_color, c_color( 255, 255, 255, 255 ) );
		ADD_VARIABLE( c_color, m_sniper_crosshair_outline_color, c_color( 0, 0, 0, 255 ) );
		ADD_VARIABLE( c_color, m_sniper_crosshair_dot_color, c_color( 255, 0, 0, 255 ) );
		ADD_VARIABLE( float, m_sniper_crosshair_interwebz_size, 1.7f );
		ADD_VARIABLE( int, m_sniper_crosshair_interwebz_thickness, 2 );
		ADD_VARIABLE( int, m_sniper_crosshair_interwebz_gap, 3 );
		ADD_VARIABLE( bool, m_sniper_crosshair_interwebz_arm_corners, false );
		ADD_VARIABLE( bool, m_sniper_crosshair_interwebz_dot_corners, false );
		ADD_VARIABLE( float, m_sniper_crosshair_size, 5.f );
		ADD_VARIABLE( float, m_sniper_crosshair_thickness, 0.5f );
		ADD_VARIABLE( float, m_sniper_crosshair_gap, 1.f );
		ADD_VARIABLE( bool, m_sniper_crosshair_dot, true );
		ADD_VARIABLE( bool, m_sniper_crosshair_t, false );
		ADD_VARIABLE( bool, m_sniper_crosshair_outline, true );
		ADD_VARIABLE( float, m_sniper_crosshair_outline_thickness, 1.f );

		ADD_VARIABLE( bool, m_force_crosshair, false );

		ADD_VARIABLE( bool, m_panorama_crosshair_color, false );
		ADD_VARIABLE( c_color, m_panorama_crosshair_color_value, c_color( 0, 255, 0, 255 ) );

		ADD_VARIABLE( bool, m_panorama_hud_color, false );
		ADD_VARIABLE( c_color, m_panorama_hud_color_value, c_color( 255, 255, 255, 255 ) );

		ADD_VARIABLE( bool, m_square_radar, false );

		ADD_VARIABLE( bool, m_classic_crosshair_color, false );
		ADD_VARIABLE( c_color, m_classic_crosshair_color_value, c_color( 0, 255, 0, 255 ) );
		ADD_VARIABLE( bool, m_classic_crosshair_outline_color, false );
		ADD_VARIABLE( c_color, m_classic_crosshair_outline_color_value, c_color( 0, 0, 0, 255 ) );

		ADD_VARIABLE( bool, m_chat_translator, false );
		ADD_VARIABLE( int, m_chat_translator_language, 0 );
		ADD_VARIABLE( bool, m_chat_images, false );

		ADD_VARIABLE( bool, m_practice_window, false );
		ADD_VARIABLE( key_bind_t, m_practice_cp_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_practice_tp_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( bool, m_practice_window_show, true );

		ADD_VARIABLE( bool, m_fake_pov, false );
		ADD_VARIABLE( key_bind_t, m_fake_pov_key, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( int, m_fake_pov_mode, 0 );
		ADD_VARIABLE( float, m_fake_pov_spin_speed, 120.f );
		ADD_VARIABLE( float, m_fake_pov_smooth, 6.f );
		ADD_VARIABLE( bool, m_fake_pov_snap_view, false );
		ADD_VARIABLE( bool, m_fake_pov_arrow, false );
		ADD_VARIABLE( float, m_fake_pov_arrow_size, 16.f );
		ADD_VARIABLE( float, m_fake_pov_arrow_dist, 90.f );
		ADD_VARIABLE( c_color, m_fake_pov_arrow_color, c_color( 255, 255, 255, 255 ) );

		ADD_VARIABLE( bool, m_hide_y6o_ps, false );

		ADD_VARIABLE( bool, m_jump_stats, false );
		ADD_VARIABLE( bool, m_jump_stats_show_fails, false );

		ADD_VARIABLE( bool, m_movement_rec, false );
		ADD_VARIABLE( bool, m_movement_rec_show_line, true );
		ADD_VARIABLE( bool, m_movement_rec_render, true );
		ADD_VARIABLE( int, m_movement_rec_position, 0 );
		ADD_VARIABLE( bool, m_movement_rec_lockva, true );
		ADD_VARIABLE( bool, m_movement_rec_lockgoingtostart, true );
		ADD_VARIABLE( bool, m_movement_rec_clipper_box, true );
		ADD_VARIABLE( float, m_movement_rec_clip_seconds, 15.f );
		ADD_VARIABLE( bool, m_movement_rec_stop_on_move, false );
		ADD_VARIABLE( bool, m_movement_rec_force_weapon, true );
		ADD_VARIABLE( key_bind_t, m_movement_rec_keystartrecord, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_movement_rec_keystoprecord, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_movement_rec_keysaveroute, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_movement_rec_keystartplay, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_movement_rec_keystopplay, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_movement_rec_keyclip, key_bind_t( 0, 1 ) );
		ADD_VARIABLE( key_bind_t, m_movement_rec_keyclearrecord, key_bind_t( 0, 1 ) );

		ADD_VARIABLE_VECTOR( bool, e_log_types::log_type_max, m_log_types, false );

		ADD_VARIABLE_VECTOR( bool, e_free_type_font_flags::font_flag_max, m_indicator_font_flags, false );
		ADD_VARIABLE( font_setting_t, m_indicator_font_settings, font_setting_t( "Verdanab", 29 ) );

		ADD_VARIABLE_VECTOR( bool, e_free_type_font_flags::font_flag_max, m_esp_font_flags, false );
		ADD_VARIABLE( font_setting_t, m_esp_font_settings, font_setting_t( "Verdanab", 11 ) );

		ADD_VARIABLE( bool, m_debug_log, false );
#ifdef _DEBUG
		ADD_VARIABLE( bool, m_debugger_visual, false );
		ADD_VARIABLE( bool, m_disable_interp, false );
#endif
	};
};

inline n_variables::impl_t g_variables{ };
