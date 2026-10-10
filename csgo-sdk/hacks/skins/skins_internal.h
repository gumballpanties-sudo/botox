#pragma once
#include "skins.h"
#include "../network/botox_net.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
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

void botox_dbg_log( const char* fmt, ... );

inline const char* const KNIFE_MODELS[] = {
	"",
	"models/weapons/v_knife_bayonet.mdl",
	"models/weapons/v_knife_m9_bay.mdl",
	"models/weapons/v_knife_karam.mdl",
	"models/weapons/v_knife_survival_bowie.mdl",
	"models/weapons/v_knife_butterfly.mdl",
	"models/weapons/v_knife_falchion_advanced.mdl",
	"models/weapons/v_knife_flip.mdl",
	"models/weapons/v_knife_gut.mdl",
	"models/weapons/v_knife_tactical.mdl",
	"models/weapons/v_knife_push.mdl",
	"models/weapons/v_knife_gypsy_jackknife.mdl",
	"models/weapons/v_knife_stiletto.mdl",
	"models/weapons/v_knife_widowmaker.mdl",
	"models/weapons/v_knife_ursus.mdl",
	"models/weapons/v_knife_default_ct.mdl",
	"models/weapons/v_knife_default_t.mdl",
	"models/weapons/v_knife_gg.mdl",
	"models/weapons/v_knife_css.mdl",
	"models/weapons/v_knife_outdoor.mdl",
	"models/weapons/v_knife_canis.mdl",
	"models/weapons/v_knife_cord.mdl",
	"models/weapons/v_knife_skeleton.mdl",
};

inline const short KNIFE_IDS[] = {
	0,
	weapon_knife_bayonet,
	weapon_knife_m9_bayonet,
	weapon_knife_karambit,
	weapon_knife_survival_bowie,
	weapon_knife_butterfly,
	weapon_knife_falchion,
	weapon_knife_flip,
	weapon_knife_gut,
	weapon_knife_tactical,
	weapon_knife_push,
	weapon_knife_gypsy_jackknife,
	weapon_knife_stiletto,
	weapon_knife_widowmaker,
	weapon_knife_ursus,
	weapon_knife,
	weapon_knife_t,
	weapon_knife_gg,
	weapon_knife_css,
	weapon_knife_outdoor,
	weapon_knife_canis,
	weapon_knife_cord,
	weapon_knife_skeleton,
};
constexpr int KNIFE_COUNT = sizeof( KNIFE_IDS ) / sizeof( KNIFE_IDS[ 0 ] );

inline const char* const KNIFE_KILLFEED_NAMES[] = {
	"",
	"bayonet",
	"knife_m9_bayonet",
	"knife_karambit",
	"knife_survival_bowie",
	"knife_butterfly",
	"knife_falchion",
	"knife_flip",
	"knife_gut",
	"knife_tactical",
	"knife_push",
	"knife_gypsy_jackknife",
	"knife_stiletto",
	"knife_widowmaker",
	"knife_ursus",
	"knife",
	"knife_t",
	"knife",
	"knife_css",
	"knife_outdoor",
	"knife_canis",
	"knife_cord",
	"knife_skeleton",
};
static_assert( sizeof( KNIFE_KILLFEED_NAMES ) / sizeof( KNIFE_KILLFEED_NAMES[ 0 ] ) == KNIFE_COUNT,
               "knife killfeed names out of sync with KNIFE_IDS" );

inline const char* const GLOVE_MODELS[] = {
	"",
	"models/weapons/v_models/arms/glove_bloodhound/v_glove_bloodhound_brokenfang.mdl",
	"models/weapons/v_models/arms/glove_bloodhound/v_glove_bloodhound.mdl",
	"models/weapons/v_models/arms/glove_sporty/v_glove_sporty.mdl",
	"models/weapons/v_models/arms/glove_slick/v_glove_slick.mdl",
	"models/weapons/v_models/arms/glove_handwrap_leathery/v_glove_handwrap_leathery.mdl",
	"models/weapons/v_models/arms/glove_motorcycle/v_glove_motorcycle.mdl",
	"models/weapons/v_models/arms/glove_specialist/v_glove_specialist.mdl",
	"models/weapons/v_models/arms/glove_bloodhound/v_glove_bloodhound_hydra.mdl",
};
inline const short GLOVE_IDS[] = {
	0,
	glove_studded_brokenfang,
	glove_studded_bloodhound,
	glove_sporty,
	glove_slick,
	glove_leather_handwraps,
	glove_motorcycle,
	glove_specialist,
	glove_studded_hydra,
};
constexpr int GLOVE_COUNT = sizeof( GLOVE_IDS ) / sizeof( GLOVE_IDS[ 0 ] );

inline const short WEAPON_IDS[] = {
	weapon_usp_silencer, weapon_hkp2000, weapon_glock, weapon_p250, weapon_fiveseven,
	weapon_tec9, weapon_cz75a, weapon_elite, weapon_deagle, weapon_revolver,
	weapon_famas, weapon_galilar, weapon_m4a1, weapon_m4a1_silencer, weapon_ak47,
	weapon_sg556, weapon_aug, weapon_ssg08, weapon_awp, weapon_scar20,
	weapon_g3sg1, weapon_sawedoff, weapon_m249, weapon_negev, weapon_mag7,
	weapon_xm1014, weapon_nova, weapon_bizon, weapon_mp5sd, weapon_mp7,
	weapon_mp9, weapon_mac10, weapon_p90, weapon_ump45,
};
constexpr int WEAPON_COUNT = sizeof( WEAPON_IDS ) / sizeof( WEAPON_IDS[ 0 ] );

inline const char* const WEAPON_MATERIAL_NAMES[] = {
	"pist_223", "pist_hkp2000", "pist_glock18", "pist_p250", "pist_fiveseven",
	"pist_tec9", "pist_cz_75", "pist_elite", "pist_deagle", "pist_revolver",
	"rif_famas", "rif_galilar", "rif_m4a1", "rif_m4a1_s", "rif_ak47",
	"rif_sg556", "rif_aug", "snip_ssg08", "snip_awp", "snip_scar20",
	"snip_g3sg1", "shot_sawedoff", "mach_m249para", "mach_negev", "shot_mag7",
	"shot_xm1014", "shot_nova", "smg_bizon", "smg_mp5sd", "smg_mp7",
	"smg_mp9", "smg_mac10", "smg_p90", "smg_ump45",
};
static_assert( sizeof( WEAPON_MATERIAL_NAMES ) / sizeof( WEAPON_MATERIAL_NAMES[ 0 ] ) == WEAPON_COUNT,
               "WEAPON_MATERIAL_NAMES / WEAPON_IDS out of sync" );

inline const char* const AGENT_MODELS[] = {
	"",
	"models/player/custom_player/legacy/ctm_diver_varianta.mdl",
	"models/player/custom_player/legacy/ctm_diver_variantb.mdl",
	"models/player/custom_player/legacy/ctm_diver_variantc.mdl",
	"models/player/custom_player/legacy/ctm_fbi_varianth.mdl",
	"models/player/custom_player/legacy/ctm_fbi_variantf.mdl",
	"models/player/custom_player/legacy/ctm_fbi_variantb.mdl",
	"models/player/custom_player/legacy/ctm_fbi_variantg.mdl",
	"models/player/custom_player/legacy/ctm_gendarmerie_varianta.mdl",
	"models/player/custom_player/legacy/ctm_gendarmerie_variantb.mdl",
	"models/player/custom_player/legacy/ctm_gendarmerie_variantc.mdl",
	"models/player/custom_player/legacy/ctm_gendarmerie_variantd.mdl",
	"models/player/custom_player/legacy/ctm_gendarmerie_variante.mdl",
	"models/player/custom_player/legacy/ctm_sas_variantg.mdl",
	"models/player/custom_player/legacy/ctm_sas_variantf.mdl",
	"models/player/custom_player/legacy/ctm_st6_variante.mdl",
	"models/player/custom_player/legacy/ctm_st6_variantg.mdl",
	"models/player/custom_player/legacy/ctm_st6_varianti.mdl",
	"models/player/custom_player/legacy/ctm_st6_variantj.mdl",
	"models/player/custom_player/legacy/ctm_st6_variantk.mdl",
	"models/player/custom_player/legacy/ctm_st6_variantl.mdl",
	"models/player/custom_player/legacy/ctm_st6_variantm.mdl",
	"models/player/custom_player/legacy/ctm_st6_variantn.mdl",
	"models/player/custom_player/legacy/ctm_swat_variante.mdl",
	"models/player/custom_player/legacy/ctm_swat_variantf.mdl",
	"models/player/custom_player/legacy/ctm_swat_variantg.mdl",
	"models/player/custom_player/legacy/ctm_swat_varianth.mdl",
	"models/player/custom_player/legacy/ctm_swat_varianti.mdl",
	"models/player/custom_player/legacy/ctm_swat_variantj.mdl",
	"models/player/custom_player/legacy/ctm_swat_variantk.mdl",
	"models/player/custom_player/legacy/tm_professional_varj.mdl",
	"models/player/custom_player/legacy/tm_professional_vari.mdl",
	"models/player/custom_player/legacy/tm_professional_varh.mdl",
	"models/player/custom_player/legacy/tm_professional_varg.mdl",
	"models/player/custom_player/legacy/tm_professional_varf5.mdl",
	"models/player/custom_player/legacy/tm_professional_varf4.mdl",
	"models/player/custom_player/legacy/tm_professional_varf3.mdl",
	"models/player/custom_player/legacy/tm_professional_varf2.mdl",
	"models/player/custom_player/legacy/tm_professional_varf1.mdl",
	"models/player/custom_player/legacy/tm_professional_varf.mdl",
	"models/player/custom_player/legacy/tm_phoenix_varianti.mdl",
	"models/player/custom_player/legacy/tm_phoenix_varianth.mdl",
	"models/player/custom_player/legacy/tm_phoenix_variantg.mdl",
	"models/player/custom_player/legacy/tm_phoenix_variantf.mdl",
	"models/player/custom_player/legacy/tm_leet_variantj.mdl",
	"models/player/custom_player/legacy/tm_leet_varianti.mdl",
	"models/player/custom_player/legacy/tm_leet_varianth.mdl",
	"models/player/custom_player/legacy/tm_leet_variantg.mdl",
	"models/player/custom_player/legacy/tm_leet_variantf.mdl",
	"models/player/custom_player/legacy/tm_jungle_raider_variantf2.mdl",
	"models/player/custom_player/legacy/tm_jungle_raider_variantf.mdl",
	"models/player/custom_player/legacy/tm_jungle_raider_variante.mdl",
	"models/player/custom_player/legacy/tm_jungle_raider_variantd.mdl",
	"models/player/custom_player/legacy/tm_jungle_raider_variantb2.mdl",
	"models/player/custom_player/legacy/tm_jungle_raider_variantb.mdl",
	"models/player/custom_player/legacy/tm_jungle_raider_varianta.mdl",
	"models/player/custom_player/legacy/tm_balkan_varianth.mdl",
	"models/player/custom_player/legacy/tm_balkan_variantj.mdl",
	"models/player/custom_player/legacy/tm_balkan_varianti.mdl",
	"models/player/custom_player/legacy/tm_balkan_variantf.mdl",
	"models/player/custom_player/legacy/tm_balkan_variantg.mdl",
	"models/player/custom_player/legacy/tm_balkan_variantk.mdl",
	"models/player/custom_player/legacy/tm_balkan_variantl.mdl",
	"models/player/custom_player/legacy/tm_phoenix.mdl",
	"models/player/custom_player/legacy/tm_leet_variantf.mdl",
	"models/player/custom_player/legacy/tm_separatist.mdl",
	"models/player/custom_player/legacy/tm_balkan_variantf.mdl",
	"models/player/custom_player/legacy/tm_professional.mdl",
	"models/player/custom_player/legacy/tm_anarchist.mdl",
	"models/player/custom_player/legacy/tm_pirate.mdl",
	"models/player/custom_player/legacy/ctm_st6.mdl",
	"models/player/custom_player/legacy/ctm_idf.mdl",
	"models/player/custom_player/legacy/ctm_gign.mdl",
	"models/player/custom_player/legacy/ctm_swat.mdl",
	"models/player/custom_player/legacy/ctm_gsg9.mdl",
	"models/player/custom_player/legacy/ctm_sas.mdl",
	"models/player/custom_player/legacy/ctm_fbi.mdl",
	"models/player/custom_player/legacy/tm_phoenix_varianta.mdl",
	"models/player/custom_player/legacy/tm_phoenix_variantb.mdl",
	"models/player/custom_player/legacy/tm_phoenix_variantc.mdl",
	"models/player/custom_player/legacy/tm_phoenix_variantd.mdl",
	"models/player/custom_player/legacy/tm_phoenix_heavy.mdl",
	"models/player/custom_player/legacy/tm_leet_varianta.mdl",
	"models/player/custom_player/legacy/tm_leet_variantb.mdl",
	"models/player/custom_player/legacy/tm_leet_variantc.mdl",
	"models/player/custom_player/legacy/tm_leet_variantd.mdl",
	"models/player/custom_player/legacy/tm_leet_variante.mdl",
	"models/player/custom_player/legacy/tm_separatist_varianta.mdl",
	"models/player/custom_player/legacy/tm_separatist_variantb.mdl",
	"models/player/custom_player/legacy/tm_separatist_variantc.mdl",
	"models/player/custom_player/legacy/tm_separatist_variantd.mdl",
	"models/player/custom_player/legacy/tm_balkan_varianta.mdl",
	"models/player/custom_player/legacy/tm_balkan_variantb.mdl",
	"models/player/custom_player/legacy/tm_balkan_variantc.mdl",
	"models/player/custom_player/legacy/tm_balkan_variantd.mdl",
	"models/player/custom_player/legacy/tm_balkan_variante.mdl",
	"models/player/custom_player/legacy/tm_professional_var1.mdl",
	"models/player/custom_player/legacy/tm_professional_var2.mdl",
	"models/player/custom_player/legacy/tm_professional_var3.mdl",
	"models/player/custom_player/legacy/tm_professional_var4.mdl",
	"models/player/custom_player/legacy/tm_anarchist_varianta.mdl",
	"models/player/custom_player/legacy/tm_anarchist_variantb.mdl",
	"models/player/custom_player/legacy/tm_anarchist_variantc.mdl",
	"models/player/custom_player/legacy/tm_anarchist_variantd.mdl",
	"models/player/custom_player/legacy/tm_pirate_varianta.mdl",
	"models/player/custom_player/legacy/tm_pirate_variantb.mdl",
	"models/player/custom_player/legacy/tm_pirate_variantc.mdl",
	"models/player/custom_player/legacy/tm_pirate_variantd.mdl",
	"models/player/custom_player/legacy/tm_jumpsuit_varianta.mdl",
	"models/player/custom_player/legacy/tm_jumpsuit_variantb.mdl",
	"models/player/custom_player/legacy/tm_jumpsuit_variantc.mdl",
	"models/player/custom_player/legacy/ctm_st6_varianta.mdl",
	"models/player/custom_player/legacy/ctm_st6_variantb.mdl",
	"models/player/custom_player/legacy/ctm_st6_variantc.mdl",
	"models/player/custom_player/legacy/ctm_st6_variantd.mdl",
	"models/player/custom_player/legacy/ctm_idf_variantb.mdl",
	"models/player/custom_player/legacy/ctm_idf_variantc.mdl",
	"models/player/custom_player/legacy/ctm_idf_variantd.mdl",
	"models/player/custom_player/legacy/ctm_idf_variante.mdl",
	"models/player/custom_player/legacy/ctm_idf_variantf.mdl",
	"models/player/custom_player/legacy/ctm_gign_varianta.mdl",
	"models/player/custom_player/legacy/ctm_gign_variantb.mdl",
	"models/player/custom_player/legacy/ctm_gign_variantc.mdl",
	"models/player/custom_player/legacy/ctm_gign_variantd.mdl",
	"models/player/custom_player/legacy/ctm_swat_varianta.mdl",
	"models/player/custom_player/legacy/ctm_swat_variantb.mdl",
	"models/player/custom_player/legacy/ctm_swat_variantc.mdl",
	"models/player/custom_player/legacy/ctm_swat_variantd.mdl",
	"models/player/custom_player/legacy/ctm_gsg9_varianta.mdl",
	"models/player/custom_player/legacy/ctm_gsg9_variantb.mdl",
	"models/player/custom_player/legacy/ctm_gsg9_variantc.mdl",
	"models/player/custom_player/legacy/ctm_gsg9_variantd.mdl",
	"models/player/custom_player/legacy/ctm_sas_varianta.mdl",
	"models/player/custom_player/legacy/ctm_sas_variantb.mdl",
	"models/player/custom_player/legacy/ctm_sas_variantc.mdl",
	"models/player/custom_player/legacy/ctm_sas_variantd.mdl",
	"models/player/custom_player/legacy/ctm_fbi_varianta.mdl",
	"models/player/custom_player/legacy/ctm_fbi_variantc.mdl",
	"models/player/custom_player/legacy/ctm_fbi_variantd.mdl",
	"models/player/custom_player/legacy/ctm_fbi_variante.mdl",
	"models/player/custom_player/legacy/ctm_heavy.mdl",
};
constexpr int AGENT_COUNT = sizeof( AGENT_MODELS ) / sizeof( AGENT_MODELS[ 0 ] );

inline std::unordered_set< std::string > g_agent_tried{ };
inline int g_agent_last_table_count = 0;

inline std::unordered_set< std::string > g_precache_tried{ };
inline int g_precache_last_count = 0;

/* model index, loading + adding it to modelprecache first if the map never did (agent changer's order:
   load FIRST, add_string second, a dead networked entry poisons later lookups). -1 = unavailable */
inline int precache_model( const char* path )
{
	if ( !path || !*path )
		return -1;

	if ( const int index = g_interfaces.m_model_info->get_model_index( path ); index > 0 )
		return index;

	const auto table = g_interfaces.m_string_tables ? g_interfaces.m_string_tables->find_table( "modelprecache" ) : nullptr;
	if ( !table )
		return -1;

	const int count = table->get_num_strings( );
	if ( count < g_precache_last_count )
		g_precache_tried.clear( );
	g_precache_last_count = count;

	if ( !g_precache_tried.insert( path ).second )
		return -1;

	if ( !g_interfaces.m_model_info->find_or_load_model( path ) ) {
		botox_dbg_log( "NET: precache load failed %s", path );
		return -1;
	}

	if ( const int max = table->get_max_strings( ); max > 0 && count >= max - 16 ) {
		botox_dbg_log( "NET: precache table full %d/%d %s", count, max, path );
		return -1;
	}

	const int added = table->add_string( false, path );
	const int index = g_interfaces.m_model_info->get_model_index( path );
	botox_dbg_log( "NET: precache %s slot=%d index=%d", path, added, index );
	return index > 0 ? index : -1;
}

/* models/weapons/v_models/arms/glove_x/v_glove_x.mdl -> models/weapons/w_models/arms/w_glove_x.mdl (all 8 in pak01) */
inline int glove_world_model_index( const char* view_model )
{
	const char* file = view_model ? std::strstr( view_model, "/v_glove" ) : nullptr;
	if ( !file )
		return -1;

	char path[ 128 ]{ };
	std::snprintf( path, sizeof( path ), "models/weapons/w_models/arms/w%s", file + 2 );
	return precache_model( path );
}
inline bool g_agent_prev_t          = false;
inline bool g_agent_prev_ct         = false;

inline int g_agent_orig_idx    = 0;
inline int g_agent_applied_idx = 0;

// last custom path checked by is_player_mdl ( file read, so once per path )
inline std::string g_agent_custom_checked{ };
inline bool g_agent_custom_ok = false;
inline int g_agent_log_left   = 64;

/* no AGENT_ARMS precache table on purpose: precaching faction arms via the modelprecache
   string table CRASHES (per-frame and one-shot both). */

struct arm_fallback_t {
	const char* m_model_substr;
	const char* m_own;
	const char* m_hands[ 3 ];
	const char* m_sleeves[ 3 ];
};

// base v_sleeve_balkan.mdl is never precached (only _v2_variant*), same for swat, list variants
inline const arm_fallback_t ARM_FALLBACKS[] = {
	{ "tm_anarchist", "models/weapons/v_models/arms/anarchist/v_sleeve_anarchist.mdl",
	  { "models/weapons/v_models/arms/glove_fullfinger/v_glove_fullfinger.mdl",
	    "models/weapons/v_models/arms/glove_fingerless/v_glove_fingerless.mdl", nullptr },
	  { "models/weapons/v_models/arms/balkan/v_sleeve_balkan_v2_variantk.mdl",
	    "models/weapons/v_models/arms/balkan/v_sleeve_balkan_v2_variantf.mdl", nullptr } },

	{ "tm_pirate", "models/weapons/v_models/arms/pirate/v_pirate_watch.mdl",
	  { "models/weapons/v_models/arms/glove_fingerless/v_glove_fingerless.mdl",
	    "models/weapons/v_models/arms/glove_fullfinger/v_glove_fullfinger.mdl", nullptr },
	  { nullptr, nullptr, nullptr } },

	{ "ctm_gign", "models/weapons/v_models/arms/gign/v_sleeve_gign.mdl",
	  { "models/weapons/v_models/arms/glove_hardknuckle/v_glove_hardknuckle.mdl",
	    "models/weapons/v_models/arms/glove_hardknuckle/v_glove_hardknuckle_black.mdl", nullptr },
	  { "models/weapons/v_models/arms/gendarmerie/v_sleeve_gendarmerie.mdl",
	    "models/weapons/v_models/arms/gendarmerie/v_sleeve_gendarmerie_variantc.mdl", nullptr } },

	{ "ctm_gsg9", "models/weapons/v_models/arms/gsg9/v_sleeve_gsg9.mdl",
	  { "models/weapons/v_models/arms/glove_hardknuckle/v_glove_hardknuckle.mdl",
	    "models/weapons/v_models/arms/glove_hardknuckle/v_glove_hardknuckle_black.mdl", nullptr },
	  { "models/weapons/v_models/arms/swat/v_sleeve_swat_generic.mdl",
	    "models/weapons/v_models/arms/sas/v_sleeve_sas.mdl", nullptr } },
};
constexpr int ARM_FALLBACK_COUNT = sizeof( ARM_FALLBACKS ) / sizeof( ARM_FALLBACKS[ 0 ] );

struct arm_config_t {
	const char* m_model_substr;
	const char* m_glove;
	const char* m_sleeve;
};
inline const arm_config_t ARM_CONFIGS[] = {
	{ "tm_anarchist", "models/weapons/v_models/arms/anarchist/v_glove_anarchist.mdl",
	  "models/weapons/v_models/arms/anarchist/v_sleeve_anarchist.mdl" },
	{ "tm_pirate", "models/weapons/v_models/arms/bare/v_bare_hands.mdl",
	  "models/weapons/v_models/arms/pirate/v_pirate_watch.mdl" },
	{ "ctm_gign", "models/weapons/v_models/arms/glove_hardknuckle/v_glove_hardknuckle_blue.mdl",
	  "models/weapons/v_models/arms/gign/v_sleeve_gign.mdl" },
	{ "ctm_gsg9", "models/weapons/v_models/arms/glove_hardknuckle/v_glove_hardknuckle_blue.mdl",
	  "models/weapons/v_models/arms/gsg9/v_sleeve_gsg9.mdl" },
};
constexpr int ARM_CONFIG_COUNT = sizeof( ARM_CONFIGS ) / sizeof( ARM_CONFIGS[ 0 ] );

inline const char* const ARM_GENERIC[] = {
	"models/weapons/v_models/arms/glove_hardknuckle/v_glove_hardknuckle.mdl",
	"models/weapons/v_models/arms/glove_fullfinger/v_glove_fullfinger.mdl",
	"models/weapons/v_models/arms/glove_fingerless/v_glove_fingerless.mdl",
	"models/weapons/v_models/arms/glove_hardknuckle/v_glove_hardknuckle_black.mdl",
};
constexpr int ARM_GENERIC_COUNT = sizeof( ARM_GENERIC ) / sizeof( ARM_GENERIC[ 0 ] );

static_assert( AGENT_COUNT == n_skins::AGENT_NAME_COUNT, "AGENT_MODELS / AGENT_NAMES out of sync" );

enum e_item_quality {
	quality_unusual = 3,
	quality_default = 4,
	quality_strange = 9,
};

constexpr int STATTRAK_OFF = -1;

/* MAX_ITEM_CUSTOM_NAME_LENGTH; netvar buffer is 161 bytes, 40 + null never overruns */
constexpr int CUSTOM_NAME_MAX = 40;

inline void apply_custom_name( c_base_entity* item, const std::string& name )
{
	const auto buffer = item->get_custom_name( );

	/* nested prop; unresolved netvar = offset 0 = entity base, writing would stomp the vtable */
	if ( !buffer || reinterpret_cast< void* >( buffer ) == static_cast< void* >( item ) )
		return;

	if ( name.empty( ) ) {
		buffer[ 0 ] = '\0';
		return;
	}

	const auto length = ( std::min )( name.size( ), static_cast< std::size_t >( CUSTOM_NAME_MAX ) );
	std::memcpy( buffer, name.c_str( ), length );
	buffer[ length ] = '\0';
}

inline void apply_extras( c_base_entity* item, bool stattrak, int kills, const std::string& name, int base_quality )
{
	item->get_fall_back_stat_trak( ) = stattrak ? ( std::max )( kills, 0 ) : STATTRAK_OFF;
	item->get_entity_quality( )      = stattrak ? quality_strange : base_quality;

	apply_custom_name( item, name );
}

inline int local_account_id( )
{
	player_info_t info{ };
	if ( !g_interfaces.m_engine_client->get_player_info( g_interfaces.m_engine_client->get_local_player( ), &info ) )
		return 0;

	return info.m_xuid_low;
}

inline std::unordered_map< unsigned int, bool > g_weapon_owned;

inline bool weapon_is_ours( c_base_entity* weapon, unsigned int handle )
{
	if ( const auto cached = g_weapon_owned.find( handle ); cached != g_weapon_owned.end( ) )
		return cached->second;

	player_info_t info{ };
	if ( !g_interfaces.m_engine_client->get_player_info( g_interfaces.m_engine_client->get_local_player( ), &info ) )
		return true; // no steam id to compare with, don't latch, decide again next frame

	const auto xuid_low  = static_cast< unsigned int >( weapon->get_owner_xuid_low( ) );
	const auto xuid_high = static_cast< unsigned int >( weapon->get_owner_xuid_high( ) );

	/* offset 0 = netvar gone; reading it would return the vtable pointer */
	static const bool prev_owner_ok = g_netvars[ HASH_BT( "CWeaponCSBase->m_hPrevOwner" ) ].m_offset != 0U;

	const bool ours = ( xuid_low || xuid_high )
		? ( xuid_low == info.m_xuid_low && xuid_high == info.m_xuid_high )
		: ( !prev_owner_ok || weapon->get_prev_owner_handle( ) == 0xFFFFFFFF );

	g_weapon_owned[ handle ] = ours;
	return ours;
}

/* custom colors cache buster: composite cache key = (kit, seed, wear, lod, model), no palette
   (CBaseVisualsDataCompare::SerializeToBuffer). fold palette hash into wear, max +0.00128 (invisible). */
inline float wear_with_color_key( float wear, const c_color* const colors[ 4 ] )
{
	unsigned int hash = 2166136261u;

	for ( int i = 0; i < 4; i++ ) {
		for ( int c = e_color_type::color_type_r; c <= e_color_type::color_type_b; c++ ) {
			hash ^= static_cast< unsigned int >( ( *colors[ i ] )[ c ] );
			hash *= 16777619u;
		}
	}

	return ( std::min )( wear + static_cast< float >( hash % 128u ) * 0.00001f, 1.f );
}

inline e_class_ids class_id_of( c_base_entity* entity )
{
	if ( !entity )
		return static_cast< e_class_ids >( -1 );

	const auto networkable  = static_cast< c_client_networkable* >( entity );
	const auto client_class = networkable->get_client_class( );
	return client_class ? client_class->m_class_id : static_cast< e_class_ids >( -1 );
}

inline bool is_knife_class( e_class_ids id )
{
	return id == e_class_ids::c_knife || id == e_class_ids::c_knife_gg;
}

inline void fill_knife_indexes( int out[ KNIFE_COUNT ] )
{
	for ( int i = 0; i < KNIFE_COUNT; i++ )
		out[ i ] = KNIFE_MODELS[ i ][ 0 ] ? g_interfaces.m_model_info->get_model_index( KNIFE_MODELS[ i ] ) : 0;
}

inline create_client_class_fn get_wearable_create_fn( )
{
	for ( auto client_class = g_interfaces.m_base_client->get_all_classes( ); client_class; client_class = client_class->m_next ) {
		if ( client_class->m_class_id == e_class_ids::c_econ_wearable )
			return client_class->m_create_fn;
	}
	return nullptr;
}

inline c_base_entity* make_glove( int entry, int serial )
{
	static auto create_fn = get_wearable_create_fn( );
	if ( !create_fn )
		return nullptr;

	create_fn( entry, serial );

	const auto glove = g_interfaces.m_client_entity_list->get< c_base_entity >( entry );
	if ( glove )
		glove->set_abs_origin( c_vector( 10000.f, 10000.f, 10000.f ) );

	return glove;
}

/* owner = player whose account id the glove carries (0 = local) */
inline void apply_glove_model( c_base_entity* glove, int owner = 0 )
{
	player_info_t info{ };
	g_interfaces.m_engine_client->get_player_info( owner > 0 ? owner : g_interfaces.m_engine_client->get_local_player( ), &info );

	glove->get_account_id( )                                                                   = info.m_xuid_low;
	*reinterpret_cast< int* >( reinterpret_cast< std::uintptr_t >( glove ) + 0x64  ) = -1;
}

using clear_custom_materials_t = void( __thiscall* )( void* owner, bool purge );

constexpr std::uintptr_t CMO_COUNT_OFFSET  = 0x10;
constexpr std::uintptr_t ITEM_DEF_IN_VIEW  = 0x1EA;
constexpr std::uintptr_t ITEM_VIEW_CMO     = 0x10;

/* CEconItemView::UpdateGeneratedMaterial; game calls ( 0, 0xFFFF, 9 ) from InitializeAttributes.
   9 = COMPOSITE_TEXTURE_SIZE_512; anything else purges + re-composites smaller. */
using update_generated_material_t = void( __thiscall* )( void* item_view, int a1, int a2, int diffuse_size );

inline clear_custom_materials_t get_clear_custom_materials( )
{
	static const auto fn = reinterpret_cast< clear_custom_materials_t >(
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 51 53 56 8B F1 33 DB 39 5E 10 7E 3C 57" ) );
	return fn;
}

/* CEconItemView::SetOrAddAttributeValueByName( name ), VALUE IN XMM2.
   this = attribute list at view + 0x244, name pushed, ret 4. */
inline std::uintptr_t get_set_attribute_value( )
{
	static const std::uintptr_t fn = reinterpret_cast< std::uintptr_t >(
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 EC 30 53 56 8B F1 F3 0F 11 55 F8 57" ) );
	return fn;
}

#ifdef _M_IX86
// xmm2 can't be set from C++, needs asm. cdecl; callee cleans the pushed name ( ret 4 ).
static __declspec( naked ) void call_set_attribute( void* , const char* , float ,
                                                    std::uintptr_t  )
{
	__asm {
		push ebp
		mov ebp, esp
		movss xmm2, dword ptr [ebp + 0x10]
		mov ecx, dword ptr [ebp + 0x08]
		push dword ptr [ebp + 0x0C]
		call dword ptr [ebp + 0x14]
		mov esp, ebp
		pop ebp
		ret
	}
}
#else
inline void call_set_attribute( void*, const char*, float, std::uintptr_t )
{
}
#endif

constexpr std::uintptr_t ITEM_VIEW_ATTRIBUTE_LIST = 0x244;

inline void set_view_attribute( std::uintptr_t item_view, const char* name, float value )
{
	const auto fn = get_set_attribute_value( );
	if ( !fn )
		return;

	call_set_attribute( reinterpret_cast< void* >( item_view + ITEM_VIEW_ATTRIBUTE_LIST ), name, value, fn );
}

inline float int_attribute_as_float( int value )
{
	const unsigned int raw = static_cast< unsigned int >( value );
	return *reinterpret_cast< const float* >( &raw );
}

inline std::uintptr_t kill_eater_cached_offset( )
{
	static const std::uintptr_t off = [ ]( ) -> std::uintptr_t {
		const auto p = g_modules[ CLIENT_DLL ].find_pattern( "3D FE FF 0F 00 77" );
		if ( !p )
			return 0;

		for ( int i = 0; i < 0x80; i++ ) {
			const unsigned char* bytes = p + i;

			if ( bytes[ 0 ] == 0xC6 && ( bytes[ 1 ] & 0xF8 ) == 0x40 && bytes[ 3 ] == 0x00 )
				return bytes[ 2 ];
		}

		return 0;
	}( );
	return off;
}

inline void apply_kill_eater( c_base_entity* weapon, std::uintptr_t item_view )
{
	const auto cached = kill_eater_cached_offset( );
	if ( !cached )
		return;

	const int kills = weapon->get_fall_back_stat_trak( );

	/* score written every pass, -1 included, or a stale count keeps the module alive */
	set_view_attribute( item_view, "kill eater", int_attribute_as_float( kills >= 0 ? kills : -1 ) );

	if ( kills >= 0 )
		set_view_attribute( item_view, "kill eater score type", 0.f );

	*reinterpret_cast< bool* >( item_view + cached ) = false;
}

inline update_generated_material_t get_update_generated_material( )
{
	static const auto fn = reinterpret_cast< update_generated_material_t >(
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 53 56 57 8B 7D 10 8B F1" ) );
	return fn;
}

using generate_sticker_materials_t = void( __thiscall* )( void* item_view );

inline generate_sticker_materials_t get_generate_sticker_materials( )
{
	static const auto fn = reinterpret_cast< generate_sticker_materials_t >(
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 81 EC 84 02 00 00 53 56 8B D9 57 89 5D EC" ) );
	return fn;
}

constexpr int STICKER_SLOTS = 5;

inline void refresh_world_stickers( c_base_entity* weapon )
{
	const auto world_model = g_interfaces.m_client_entity_list->get< c_base_entity >( weapon->get_world_model_handle( ) );
	if ( !world_model )
		return;

	if ( g_interfaces.m_model_render ) {
		const auto instance = static_cast< c_client_renderable* >( world_model )->get_model_instance( );
		if ( instance != 0xFFFF )
			g_interfaces.m_model_render->remove_all_decals( instance );
	}

	static constexpr unsigned int parent_hash = HASH_BT( "CBaseWeaponWorldModel->m_hCombatWeaponParent" );
	static const auto parent_prop             = g_netvars[ parent_hash ];

	if ( !parent_prop.m_recv_prop || !parent_prop.m_recv_prop->m_proxy_fn || !parent_prop.m_offset )
		return;

	const auto field = reinterpret_cast< std::uintptr_t >( world_model ) + parent_prop.m_offset;

	c_recv_proxy_data data{ };
	data.m_recv_prop   = parent_prop.m_recv_prop;
	data.m_value.m_int = *reinterpret_cast< int* >( field );
	data.m_element     = 0;
	data.m_object_id   = static_cast< c_client_networkable* >( world_model )->get_index( );

	parent_prop.m_recv_prop->m_proxy_fn( &data, world_model, reinterpret_cast< void* >( field ) );
}

inline int gun_index_of( short definition )
{
	for ( int i = 0; i < WEAPON_COUNT; i++ ) {
		if ( definition == WEAPON_IDS[ i ] )
			return i;
	}
	return -1;
}

/* our config for this gun's slots (all kit 0 when weapon skins are off) */
inline n_skins::sticker_t config_sticker( int gun, int slot )
{
	n_skins::sticker_t sticker{ };

	const int index = gun * STICKER_SLOTS + slot;
	if ( gun < 0 || !GET_VARIABLE( WEAPON_VAR( m_weapon_skins_enable ), bool ) )
		return sticker;

	auto& kits      = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_sticker_kit ), std::vector< int > );
	auto& wears     = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_sticker_wear ), std::vector< float > );
	auto& scales    = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_sticker_scale ), std::vector< float > );
	auto& rotations = GET_VARIABLE( WEAPON_VAR( m_weapon_skins_sticker_rotation ), std::vector< float > );

	sticker.m_kit = index < static_cast< int >( kits.size( ) ) ? kits[ index ] : 0;
	if ( sticker.m_kit <= 0 )
		return sticker;

	sticker.m_wear     = index < static_cast< int >( wears.size( ) ) ? wears[ index ] : 0.f;
	sticker.m_scale    = index < static_cast< int >( scales.size( ) ) ? scales[ index ] : 1.f;
	sticker.m_rotation = index < static_cast< int >( rotations.size( ) ) ? rotations[ index ] : 0.f;
	return sticker;
}

/* remote = STICKER_SLOTS entries from another botox user, never our config */
inline void apply_stickers( c_base_entity* weapon, std::uintptr_t item_view, const n_skins::sticker_t* remote = nullptr )
{
	const auto generate = get_generate_sticker_materials( );
	if ( !generate )
		return;

	const int gun = gun_index_of( weapon->get_item_definition_index( ) );
	if ( gun < 0 )
		return;

	for ( int slot = 0; slot < STICKER_SLOTS; slot++ ) {
		const n_skins::sticker_t sticker = remote ? remote[ slot ] : config_sticker( gun, slot );

		const int kit        = sticker.m_kit > 0 ? sticker.m_kit : 0;
		const float wear     = kit > 0 ? sticker.m_wear : 0.f;
		const float scale    = kit > 0 ? sticker.m_scale : 1.f;
		const float rotation = kit > 0 ? sticker.m_rotation : 0.f;

		char name[ 32 ];

		std::snprintf( name, sizeof( name ), "sticker slot %d id", slot );
		set_view_attribute( item_view, name, int_attribute_as_float( kit ) );

		std::snprintf( name, sizeof( name ), "sticker slot %d wear", slot );
		set_view_attribute( item_view, name, wear );

		std::snprintf( name, sizeof( name ), "sticker slot %d scale", slot );
		set_view_attribute( item_view, name, scale );

		std::snprintf( name, sizeof( name ), "sticker slot %d rotation", slot );
		set_view_attribute( item_view, name, rotation );
	}

	generate( reinterpret_cast< void* >( item_view ) );
}

inline std::uintptr_t item_view_of( c_base_entity* weapon )
{
	return reinterpret_cast< std::uintptr_t >( &weapon->get_item_definition_index( ) ) - ITEM_DEF_IN_VIEW;
}

inline int custom_material_count( std::uintptr_t owner )
{
	return *reinterpret_cast< int* >( owner + CMO_COUNT_OFFSET );
}

inline std::uintptr_t weapon_cmo_offset( )
{
	static const std::uintptr_t off = [ ]( ) -> std::uintptr_t {
		const auto p = g_modules[ CLIENT_DLL ].find_pattern( "83 BE ? ? ? ? ? 7F 67" );
		return p ? *reinterpret_cast< unsigned int* >( p + 2 ) - CMO_COUNT_OFFSET : 0;
	}( );
	return off;
}

inline std::uintptr_t weapon_material_init_offset( )
{
	static const std::uintptr_t off = [ ]( ) -> std::uintptr_t {
		const auto p = g_modules[ CLIENT_DLL ].find_pattern( "C6 86 ? ? ? ? ? FF 50 04" );
		return p ? *reinterpret_cast< unsigned int* >( p + 2 ) : 0;
	}( );
	return off;
}

inline void clear_owner( void* owner )
{
	const auto clear = get_clear_custom_materials( );
	if ( !clear || !owner )
		return;

	// sanity: vtable + plausible count, clear() Releases every entry
	const auto vtable = *reinterpret_cast< void** >( owner );
	const int count   = custom_material_count( reinterpret_cast< std::uintptr_t >( owner ) );
	if ( !vtable || count <= 0 || count > 32 )
		return;

	clear( owner, false  );
}

/* drop the stale composite so the next OnDataChanged( DATA_UPDATE_CREATED ) regenerates it */
inline void invalidate_custom_materials( c_base_entity* weapon )
{
	if ( !weapon )
		return;

	const auto base = reinterpret_cast< std::uintptr_t >( weapon );

	clear_owner( reinterpret_cast< void* >( item_view_of( weapon ) + ITEM_VIEW_CMO ) );

	if ( const auto cmo = weapon_cmo_offset( ) )
		clear_owner( reinterpret_cast< void* >( base + cmo ) );

	// make CWeaponCSBase::UpdateCustomMaterial re-run its copy instead of thinking it's done
	if ( const auto init = weapon_material_init_offset( ) )
		*reinterpret_cast< bool* >( base + init ) = false;
}

inline void rebuild_weapon_materials( c_base_entity* weapon, const n_skins::sticker_t* remote_stickers )
{
	const auto item_view   = item_view_of( weapon );
	const auto view_cmo    = item_view + ITEM_VIEW_CMO;
	const auto networkable = static_cast< c_client_networkable* >( weapon );

	const int paint = weapon->get_fall_back_paint_kit( );

	set_view_attribute( item_view, "set item texture prefab", static_cast< float >( paint > 0 ? paint : 0 ) );
	set_view_attribute( item_view, "set item texture seed", static_cast< float >( paint > 0 ? weapon->get_fall_back_seed( ) : 0 ) );
	set_view_attribute( item_view, "set item texture wear", paint > 0 ? weapon->get_fall_back_wear( ) : 0.f );

	apply_stickers( weapon, item_view, remote_stickers );

	apply_kill_eater( weapon, item_view );

	networkable->on_data_changed( 0  );

	if ( paint <= 0 ) {
		invalidate_custom_materials( weapon );
		return;
	}

	if ( custom_material_count( view_cmo ) <= 0 ) {
		if ( const auto generate = get_update_generated_material( ) ) {
			generate( reinterpret_cast< void* >( item_view ), 0, 0xFFFF, 9 );
			networkable->on_data_changed( 0  );
		}
	}

	const auto weapon_cmo = weapon_cmo_offset( );
	if ( !weapon_cmo || custom_material_count( view_cmo ) <= 0 )
		return;

	/* copy didn't land, fall back to CCustomMaterialOwner::DuplicateCustomMaterialsToOther (vtable slot 3), view to weapon */
	const auto weapon_owner = reinterpret_cast< std::uintptr_t >( weapon ) + weapon_cmo;
	if ( custom_material_count( weapon_owner ) > 0 )
		return;

	using duplicate_t   = void( __thiscall* )( void* owner, void* other );
	const auto vtable   = *reinterpret_cast< void*** >( view_cmo );
	if ( vtable && vtable[ 3 ] )
		reinterpret_cast< duplicate_t >( vtable[ 3 ] )( reinterpret_cast< void* >( view_cmo ),
		                                                reinterpret_cast< void* >( weapon_owner ) );
}

/* remote_stickers: someone else's gun (botox network), their STICKER_SLOTS stickers instead of our config.
   world model copies the weapon's materials only when the counts differ: sync at 0 (clears it), again once the weapon has them */
inline void rebuild_custom_materials( c_base_entity* weapon, const n_skins::sticker_t* remote_stickers = nullptr )
{
	if ( !weapon )
		return;

	invalidate_custom_materials( weapon );
	refresh_world_stickers( weapon );

	rebuild_weapon_materials( weapon, remote_stickers );
	refresh_world_stickers( weapon );
}

/* game composites a glove once (InitializeAttributes), arms copy the old one off the entity */
inline void regen_glove_material( c_base_entity* glove, int paint_kit, int seed, float wear )
{
	const auto glove_view = item_view_of( glove );
	const auto view_cmo   = glove_view + ITEM_VIEW_CMO;
	const auto cmo        = weapon_cmo_offset( );
	const auto glove_cmo  = cmo ? reinterpret_cast< std::uintptr_t >( glove ) + cmo : 0;

	set_view_attribute( glove_view, "set item texture prefab", static_cast< float >( paint_kit > 0 ? paint_kit : 0 ) );
	set_view_attribute( glove_view, "set item texture seed", static_cast< float >( paint_kit > 0 ? seed : 0 ) );
	set_view_attribute( glove_view, "set item texture wear", paint_kit > 0 ? wear : 0.f );

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

// hands loadout lookup (0x3E0840) reads CS game rules unchecked: null while loading / after disconnect = crash
inline bool game_rules_up( )
{
	static const auto site =
		g_modules[ CLIENT_DLL ].find_pattern( "8B 4D 04 E8 ? ? ? ? 8B 0D ? ? ? ? 8B 01 FF 90 98 04 00 00 C6 45 FC 00" );
	return site && **reinterpret_cast< void*** >( site + 10 );
}

/* evolve's glove apply. unique on the final client.dll: CEconWearable::Equip (0x723170),
   C_CSPlayer::InvalidateViewModelArmConfig (0x3EF290, nulls cfg + drops arms on all 3 viewmodels),
   C_BaseViewModel::UpdateAllViewmodelAddons (0x215080, rebuilds arms from the equipped glove). glove null = no equip. */
inline void rebuild_player_arms( c_base_entity* player, c_base_entity* glove, const char* why )
{
	if ( !g_interfaces.m_engine_client->is_in_game( ) || !game_rules_up( ) ) {
		botox_dbg_log( "GLV: arms %s skipped, game rules down", why );
		return;
	}

	using equip_wearable_t = void( __thiscall* )( void* wearable, void* owner );
	using this_only_t      = void( __thiscall* )( void* self );

	static const auto equip = reinterpret_cast< equip_wearable_t >(
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 EC 10 53 8B 5D 08 57 8B F9" ) );
	static const auto invalidate_arms = reinterpret_cast< this_only_t >(
		g_modules[ CLIENT_DLL ].find_pattern( "51 56 57 8B F9 33 F6 C7 87 ? ? ? ? 00 00 00 00 56 8B CF E8" ) );
	static const auto update_addons = reinterpret_cast< this_only_t >(
		g_modules[ CLIENT_DLL ].find_pattern( "55 8B EC 83 E4 F8 83 EC 2C 53 8B D9 56 57 8B 03 FF 90 F0 03 00 00 8B F8" ) );

	const auto viewmodel = g_interfaces.m_client_entity_list->get< c_base_entity >( player->get_view_model_handle( ) );

	botox_dbg_log( "GLV: arms %s player %d equip %p inval %p addons %p vm %p glove %p", why, player->get_index( ), equip, invalidate_arms,
	               update_addons, viewmodel, glove );

	if ( glove && equip )
		equip( glove, player );
	if ( invalidate_arms )
		invalidate_arms( player );
	if ( viewmodel && update_addons )
		update_addons( viewmodel );
}

/* models/weapons/v_knife_x.mdl -> w_knife_x.mdl (every knife in pak01 pairs 1:1). not precached = old +1 guess */
inline int knife_world_model_index( const char* view_model, int view_index )
{
	char path[ 128 ]{ };
	std::snprintf( path, sizeof( path ), "%s", view_model ? view_model : "" );

	if ( char* found = std::strstr( path, "/v_knife" ) )
		found[ 1 ] = 'w';

	const int index = precache_model( path );
	if ( index > 0 )
		return index;

	static int log_left = 8;
	if ( log_left > 0 && log_left-- )
		botox_dbg_log( "NET: knife world model %s not precached, using %d", path, view_index + 1 );

	return view_index + 1;
}

inline bool apply_knife_model( c_base_entity* weapon, const char* model )
{
	const auto local = g_ctx.m_local;
	if ( !local )
		return false;

	const auto viewmodel = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_view_model_handle( ) );
	if ( !viewmodel )
		return false;

	const auto vm_weapon_handle = viewmodel->get_weapon_handle( );
	if ( !vm_weapon_handle )
		return false;

	const auto vm_weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( vm_weapon_handle );
	if ( vm_weapon != weapon )
		return false;

	const int model_index      = g_interfaces.m_model_info->get_model_index( model );
	viewmodel->get_model_index( ) = model_index;

	if ( const auto world_model_handle = vm_weapon->get_world_model_handle( ) ) {
		if ( const auto world_model = g_interfaces.m_client_entity_list->get< c_base_entity >( world_model_handle ) )
			world_model->get_model_index( ) = knife_world_model_index( model, model_index );
	}

	if ( std::strstr( model, "knife_gg" ) )
		viewmodel->get_view_model_body( ) = ( local->get_team( ) == team_tt ) ? 0 : 1;

	return true;
}

inline void apply_knife_skin( c_base_entity* weapon, short item_def, int paint_kit, int model_index, float wear )
{
	weapon->get_item_definition_index( ) = item_def;
	weapon->get_fall_back_paint_kit( )   = paint_kit;
	weapon->get_model_index( )           = model_index;
	weapon->get_fall_back_wear( )        = wear;
}

constexpr int SEQUENCE_DEFAULT_COUNT  = 13;
constexpr int SEQUENCE_MAX_CANDIDATES = 6;

struct knife_anim_map_t {
	const char* m_anim_model;
	short m_count;
	signed char m_seq[ SEQUENCE_DEFAULT_COUNT ][ SEQUENCE_MAX_CANDIDATES ];
};

inline const knife_anim_map_t KNIFE_ANIM_MAPS[] = {
	{ "models/weapons/v_ct_knife_anim.mdl", 13,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1} } },
	{ "models/weapons/v_t_knife_anim.mdl", 13,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_bayonet_anim.mdl", 13,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_m9_bay_anim.mdl", 13,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_karam_anim.mdl", 13,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_survival_bowie_anim.mdl", 12,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1},
	    {4, -1, -1, -1, -1, -1}, {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1},
	    {9, -1, -1, -1, -1, -1}, {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_butterfly_anim.mdl", 16,
	  { {0,1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1}, {5, -1, -1, -1, -1, -1},
	    {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1}, {10, -1, -1, -1, -1, -1},
	    {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1}, {13,14,15, -1, -1, -1} } },
	{ "models/weapons/v_knife_falchion_advanced_anim.mdl", 14,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1},
	    {4, -1, -1, -1, -1, -1}, {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8,9, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {12,13, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_flip_anim.mdl", 13,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_gut_anim.mdl", 13,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_push_anim.mdl", 16,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2,3,4,5,6, -1}, {2,3,4,5,6, -1},
	    {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1}, {10, -1, -1, -1, -1, -1}, {11,12, -1, -1, -1, -1},
	    {13, -1, -1, -1, -1, -1}, {14, -1, -1, -1, -1, -1}, {15, -1, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_gypsy_jackknife_anim.mdl", 13,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_stiletto_anim.mdl", 14,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {12,13, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_widowmaker_anim.mdl", 16,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {14,15, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_ursus_anim.mdl", 15,
	  { {0,1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1}, {5, -1, -1, -1, -1, -1},
	    {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1}, {10, -1, -1, -1, -1, -1},
	    {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1}, {13,14, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_gg.mdl", 12,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_css_anim.mdl", 16,
	  { {0, -1, -1, -1, -1, -1}, {1, -1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1},
	    {5, -1, -1, -1, -1, -1}, {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1},
	    {10, -1, -1, -1, -1, -1}, {11, -1, -1, -1, -1, -1}, {12,15, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_outdoor_anim.mdl", 15,
	  { {0,1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1}, {5, -1, -1, -1, -1, -1},
	    {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1}, {10, -1, -1, -1, -1, -1},
	    {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1}, {13,14, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_canis_anim.mdl", 15,
	  { {0,1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1}, {5, -1, -1, -1, -1, -1},
	    {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1}, {10, -1, -1, -1, -1, -1},
	    {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1}, {13,14, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_cord_anim.mdl", 15,
	  { {0,1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1}, {5, -1, -1, -1, -1, -1},
	    {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1}, {10, -1, -1, -1, -1, -1},
	    {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1}, {13,14, -1, -1, -1, -1} } },
	{ "models/weapons/v_knife_skeleton_anim.mdl", 15,
	  { {0,1, -1, -1, -1, -1}, {2, -1, -1, -1, -1, -1}, {3, -1, -1, -1, -1, -1}, {4, -1, -1, -1, -1, -1}, {5, -1, -1, -1, -1, -1},
	    {6, -1, -1, -1, -1, -1}, {7, -1, -1, -1, -1, -1}, {8, -1, -1, -1, -1, -1}, {9, -1, -1, -1, -1, -1}, {10, -1, -1, -1, -1, -1},
	    {11, -1, -1, -1, -1, -1}, {12, -1, -1, -1, -1, -1}, {13,14, -1, -1, -1, -1} } },
};

/* each KNIFE_MODELS mesh's $includemodel. gold knife has no include (own 12 sequences):
   can donate anims, never receive. */
inline const char* const KNIFE_ANIM_MODELS[] = {
	"",
	"models/weapons/v_knife_bayonet_anim.mdl",
	"models/weapons/v_knife_m9_bay_anim.mdl",
	"models/weapons/v_knife_karam_anim.mdl",
	"models/weapons/v_knife_survival_bowie_anim.mdl",
	"models/weapons/v_knife_butterfly_anim.mdl",
	"models/weapons/v_knife_falchion_advanced_anim.mdl",
	"models/weapons/v_knife_flip_anim.mdl",
	"models/weapons/v_knife_gut_anim.mdl",
	"models/weapons/v_knife_gut_anim.mdl",
	"models/weapons/v_knife_push_anim.mdl",
	"models/weapons/v_knife_gypsy_jackknife_anim.mdl",
	"models/weapons/v_knife_stiletto_anim.mdl",
	"models/weapons/v_knife_widowmaker_anim.mdl",
	"models/weapons/v_knife_ursus_anim.mdl",
	"models/weapons/v_ct_knife_anim.mdl",
	"models/weapons/v_t_knife_anim.mdl",
	"models/weapons/v_knife_gg.mdl",
	"models/weapons/v_knife_css_anim.mdl",
	"models/weapons/v_knife_outdoor_anim.mdl",
	"models/weapons/v_knife_canis_anim.mdl",
	"models/weapons/v_knife_cord_anim.mdl",
	"models/weapons/v_knife_skeleton_anim.mdl",
};
static_assert( sizeof( KNIFE_ANIM_MODELS ) / sizeof( KNIFE_ANIM_MODELS[ 0 ] ) == KNIFE_COUNT,
               "knife anim models out of sync with KNIFE_MODELS" );

inline const unsigned int KNIFE_ANIM_COMPAT[] = {
	0x00000000,
	0x004FFBFE,
	0x004FFBFE,
	0x004FFBFE,
	0x004FFBFE,
	0x00000020,
	0x000818C0,
	0x000818C0,
	0x004FFBFE,
	0x004FFBFE,
	0x00000400,
	0x000818C0,
	0x000818C0,
	0x004FFBFE,
	0x004FFBFE,
	0x004FFBFE,
	0x004FFBFE,
	0x004FFBFE,
	0x004FFBFE,
	0x000818C0,
	0x00300000,
	0x00300000,
	0x004FFBFE,
};
static_assert( sizeof( KNIFE_ANIM_COMPAT ) / sizeof( KNIFE_ANIM_COMPAT[ 0 ] ) == KNIFE_COUNT,
               "knife anim compat out of sync with KNIFE_MODELS" );

inline const char* model_file_name( const char* path )
{
	if ( !path )
		return nullptr;

	const char* name = path;
	for ( const char* it = path; *it; it++ ) {
		if ( *it == '/' || *it == '\\' )
			name = it + 1;
	}

	return name;
}

inline bool same_model_file( const char* first, const char* second )
{
	const char* a = model_file_name( first );
	const char* b = model_file_name( second );

	return a && b && _stricmp( a, b ) == 0;
}

inline int remap_knife_sequence( const char* anim_model, int seq )
{
	if ( !anim_model || seq < 0 )
		return seq;

	for ( const auto& map : KNIFE_ANIM_MAPS ) {
		if ( !same_model_file( map.m_anim_model, anim_model ) )
			continue;

		if ( seq >= SEQUENCE_DEFAULT_COUNT )
			return seq < map.m_count ? seq : map.m_count - 1;

		const signed char* candidates = map.m_seq[ seq ];

		int count = 0;
		while ( count < SEQUENCE_MAX_CANDIDATES && candidates[ count ] >= 0 )
			count++;

		if ( count <= 0 )
			return seq;

		return candidates[ count == 1 ? 0 : rand( ) % count ];
	}

	return seq;
}

inline bool knife_sequence_is( const char* anim_model, int seq, int default_seq )
{
	for ( const auto& map : KNIFE_ANIM_MAPS ) {
		if ( !same_model_file( map.m_anim_model, anim_model ) )
			continue;

		for ( int i = 0; i < SEQUENCE_MAX_CANDIDATES && map.m_seq[ default_seq ][ i ] >= 0; i++ ) {
			if ( map.m_seq[ default_seq ][ i ] == seq )
				return true;
		}
		return false;
	}

	return false;
}

inline bool knife_sequence_is_idle( const char* anim_model, int seq )
{
	return knife_sequence_is( anim_model, seq, 1 ) || knife_sequence_is( anim_model, seq, 2 );
}

struct knife_view_t {
	c_base_entity* m_view_model = nullptr;

	int m_net_seq     = -1;
	float m_net_time  = 0.f;
	int m_seq         = -1;
	float m_time      = 0.f;
	float m_cycle     = 1.f;
	bool m_pending    = false;
	bool m_own_clock  = false; // shown time is ours (idle moved), don't follow the networked one

	int m_parity = 0;

	bool m_applied      = false;
	int m_saved_seq     = 0;
	float m_saved_time  = 0.f;
	float m_saved_cycle = 0.f;
	int m_saved_parity  = 0;
};
inline knife_view_t g_knife_view{ };

inline std::atomic< const char* > g_anim_donor{ nullptr };

inline std::atomic< const char* > g_anim_pick{ nullptr };

inline std::atomic< unsigned int > g_anim_targets{ 0 };
inline std::atomic< unsigned int > g_anim_targets_pick{ 0 };

inline std::atomic< const char* > g_mesh_anim[ KNIFE_COUNT ] = { };

inline std::atomic< const char* > g_graph_donor{ reinterpret_cast< const char* >( 1 ) };

/* never cache MDLHandle_t: level load frees unreferenced models and the index is reused by the
   next FindMDL, so a stale handle is a different model. on_level_pre_load resolves fresh. */

inline int knife_index_for_mesh( const char* mesh_model )
{
	for ( int i = 1; i < KNIFE_COUNT; i++ ) {
		if ( same_model_file( KNIFE_MODELS[ i ], mesh_model ) )
			return i;
	}

	return -1;
}

inline const char* stock_anim_model( const char* mesh_model )
{
	for ( int i = 1; i < KNIFE_COUNT; i++ ) {
		if ( same_model_file( KNIFE_MODELS[ i ], mesh_model ) )
			return KNIFE_ANIM_MODELS[ i ];
	}

	return nullptr;
}

inline const char* intended_knife_mesh( const char* current_model_name )
{
	if ( !GET_VARIABLE( WEAPON_VAR( m_knife_enable ), bool ) )
		return current_model_name;

	const int chosen = GET_VARIABLE( WEAPON_VAR( m_knife_model ), int );
	if ( chosen <= 0 || chosen >= KNIFE_COUNT )
		return current_model_name;

	bool holding_knife = stock_anim_model( current_model_name ) != nullptr;

	if ( !holding_knife && g_ctx.m_local ) {
		if ( const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) ) )
			holding_knife = is_knife_class( class_id_of( weapon ) );
	}

	return holding_knife ? KNIFE_MODELS[ chosen ] : current_model_name;
}

inline const char* effective_anim_model( const char* mesh_model )
{
	const char* stock = stock_anim_model( mesh_model );
	if ( !stock || !*stock )
		return nullptr;

	if ( same_model_file( stock, mesh_model ) )
		return stock;

	/* table really loaded for THIS mesh (last resolve), never the menu pick (graph is cached) */
	const int index = knife_index_for_mesh( mesh_model );
	if ( index > 0 ) {
		if ( const char* resolved = g_mesh_anim[ index ].load( std::memory_order_relaxed ) )
			return resolved;
	}

	return stock;
}

inline const char* server_sequence_table( )
{
	const auto nci = g_interfaces.m_engine_client->get_net_channel_info( );
	if ( !nci || !nci->is_loopback( ) )
		return nullptr;

	return effective_anim_model( KNIFE_MODELS[ 15 ] );
}

constexpr int k_deagle_idle     = 0;
constexpr int k_deagle_lookat01 = 7;
constexpr int k_deagle_lookat02 = 8;
constexpr float k_spin_duration = 8.5f;
constexpr float k_spin_loop_at  = 0.8235294f;
// server: one LookAtHeldWeapon per 0.3s (cs_player.cpp CS_COMMAND_MAX_RATE), + jitter
constexpr float k_roll_gap      = 0.35f;

inline bool want_spin_hold( bool key, bool held, float cycle, float lead, float freeze )
{
	if ( cycle >= k_spin_loop_at )
		return false;

	const float left = ( k_spin_loop_at - cycle ) * k_spin_duration;
	// a change now reaches the client's loop event late (stale networked hold): client and server must loop alike
	if ( left <= freeze )
		return held;

	return key || left > lead;
}

inline recv_var_proxy_fn g_original_sequence_proxy    = nullptr;
inline recv_var_proxy_fn g_original_model_index_proxy = nullptr;

/* view model of a botox network user we spectate: their knife slot, 0 = none / ours / not theirs */
inline int remote_view_knife( c_base_entity* viewmodel )
{
	if ( !viewmodel || !g_ctx.m_local )
		return 0;

	const auto owner = g_interfaces.m_client_entity_list->get< c_base_entity >( viewmodel->get_owner_handle( ) );
	if ( !owner || owner == g_ctx.m_local )
		return 0;

	const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( owner->get_active_weapon_handle( ) );
	if ( !weapon || !is_knife_class( class_id_of( weapon ) ) )
		return 0;

	return g_botox_net.knife_slot_of( owner->get_index( ) );
}

inline void __cdecl model_index_proxy( const c_recv_proxy_data* data, void* structure, void* output )
{
	const bool enabled = GET_VARIABLE( WEAPON_VAR( m_knife_enable ), bool );
	const int chosen   = GET_VARIABLE( WEAPON_VAR( m_knife_model ), int );

	if ( const int remote = g_interfaces.m_engine_client->is_connected( ) ? remote_view_knife( static_cast< c_base_entity* >( structure ) ) : 0 ) {
		int knife_indexes[ KNIFE_COUNT ];
		fill_knife_indexes( knife_indexes );

		const auto mutable_data = const_cast< c_recv_proxy_data* >( data );
		for ( int i = 1; i < KNIFE_COUNT; i++ ) {
			if ( mutable_data->m_value.m_int == knife_indexes[ i ] && knife_indexes[ remote ] > 0 ) {
				mutable_data->m_value.m_int = knife_indexes[ remote ];
				break;
			}
		}
	} else if ( enabled && chosen > 0 && chosen < KNIFE_COUNT && g_ctx.m_local && g_ctx.m_local->is_alive( ) &&
		 g_interfaces.m_engine_client->is_connected( ) ) {
		int knife_indexes[ KNIFE_COUNT ];
		fill_knife_indexes( knife_indexes );

		const auto mutable_data = const_cast< c_recv_proxy_data* >( data );
		for ( int i = 1; i < KNIFE_COUNT; i++ ) {
			if ( mutable_data->m_value.m_int == knife_indexes[ i ] ) {
				mutable_data->m_value.m_int = knife_indexes[ chosen ];
				break;
			}
		}
	}

	if ( g_original_model_index_proxy )
		g_original_model_index_proxy( data, structure, output );
}

inline void __cdecl sequence_proxy( const c_recv_proxy_data* data, void* structure, void* output )
{
	const auto viewmodel = static_cast< c_base_entity* >( structure );
	const auto local     = g_ctx.m_local;

	/* your deagle view model only (demo = the recorder's), never a spectated player's. owner's weapon, not
	   m_hWeapon: player entities decode before view models, sibling props can race.
	   lookat01 = a missed server roll, deagle_spinner cancels it: show idle so only the real spin is seen */
	if ( viewmodel && local && data->m_value.m_int == k_deagle_lookat01 && GET_VARIABLE( g_variables.m_deagle_spinner, bool ) ) {
		const auto owner  = g_interfaces.m_client_entity_list->get< c_base_entity >( viewmodel->get_owner_handle( ) );
		const auto weapon = owner == local ? g_interfaces.m_client_entity_list->get< c_base_entity >( owner->get_active_weapon_handle( ) ) : nullptr;

		if ( weapon && weapon->get_item_definition_index( ) == weapon_deagle )
			const_cast< c_recv_proxy_data* >( data )->m_value.m_int = k_deagle_idle;
	}

	if ( viewmodel && local ) {
		const auto owner = g_interfaces.m_client_entity_list->get< c_base_entity >( viewmodel->get_owner_handle( ) );
		if ( owner == local ) {
			if ( const auto model = g_interfaces.m_model_info->get_model( viewmodel->get_model_index( ) ) ) {
				if ( const char* model_name = g_interfaces.m_model_info->get_model_name( model ) ) {
					const auto mutable_data = const_cast< c_recv_proxy_data* >( data );
					const char* anim_model  = effective_anim_model( intended_knife_mesh( model_name ) );
					const char* from        = server_sequence_table( );
					int incoming            = mutable_data->m_value.m_int;

					const bool translate = !from || !anim_model || !same_model_file( from, anim_model );

					if ( translate ) {
						const auto& view = g_knife_view;
						const int predicted = view.m_view_model == viewmodel ? view.m_net_seq : -1;
						const bool keep     = incoming >= 0 && incoming < SEQUENCE_DEFAULT_COUNT && predicted >= 0 &&
						                  knife_sequence_is( anim_model, predicted, incoming );

						mutable_data->m_value.m_int = keep ? predicted : remap_knife_sequence( anim_model, incoming );
						botox_dbg_log( "KSEQ: in=%d pred=%d out=%d keep=%d", incoming, predicted, mutable_data->m_value.m_int, keep );
					}
				}
			}
		} else if ( const int remote = remote_view_knife( viewmodel ) ) {
			/* server picks sequences off the stock knife table, the spectated botox user's knife numbers them differently */
			const char* anim_model = effective_anim_model( KNIFE_MODELS[ remote ] );
			const char* from       = server_sequence_table( );

			if ( anim_model && ( !from || !same_model_file( from, anim_model ) ) ) {
				const auto mutable_data     = const_cast< c_recv_proxy_data* >( data );
				const int incoming          = mutable_data->m_value.m_int;
				mutable_data->m_value.m_int = remap_knife_sequence( anim_model, incoming );
				botox_dbg_log( "NET: kseq %d in=%d out=%d", remote, incoming, mutable_data->m_value.m_int );
			}
		}
	}

	if ( g_original_sequence_proxy )
		g_original_sequence_proxy( data, structure, output );
}
