#include "aimbot.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../utilities/input/input.h"
#include "../auto_wall/auto_wall.h"
#include "../entity_cache/entity_cache.h"
#include "../misc/misc.h"
#include "../movement/edge_skip.h"
#include "../movement/edgebug.h"
#include "../prediction/prediction.h"
#include "../web/websurface.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

extern void botox_dbg_log( const char* fmt, ... );

namespace
{
	const char* g_dbg_gate = nullptr;
	float g_dbg_best = -2.f, g_dbg_need = -2.f;
	int g_dbg_gated  = 0;
	int g_dbg_players = 0, g_dbg_valid = 0, g_dbg_immune = 0, g_dbg_nobones = 0, g_dbg_void = 0;

	void dbg_gate( const char* gate )
	{
		if ( gate == g_dbg_gate )
			return;

		g_dbg_gate = gate;

		if ( !g_ctx.m_local )
			return;

		const c_angle punch = g_ctx.m_local->get_punch( );

		botox_dbg_log( "AIM: gate=%s ns=%d air=%d shots=%d best=%.3f need=%.3f punch=%.1f/%.1f pl=%d ok=%d imm=%d nob=%d void=%d", gate,
		                   ( int )GET_VARIABLE( g_variables.m_nospread_enable, bool ), ( int )!( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ),
		                   g_ctx.m_local->get_shots_fired( ), g_dbg_best, g_dbg_need, punch.m_x, punch.m_y, g_dbg_players, g_dbg_valid,
		                   g_dbg_immune, g_dbg_nobones, g_dbg_void );
	}

	struct aim_settings_t {
		float m_fov = 0.f, m_smooth = 1.f, m_rcs = 0.f;
	};

	struct class_vars_t {
		std::uint32_t m_override = 0, m_fov = 0, m_smooth = 0, m_rcs = 0, m_hitboxes = 0;
		bool m_sniper = false;
	};

	const class_vars_t* weapon_class_vars( const short id )
	{
		static const class_vars_t pistol{ g_variables.m_aimbot_pistol_override, g_variables.m_aimbot_pistol_fov, g_variables.m_aimbot_pistol_smooth,
			                              g_variables.m_aimbot_pistol_rcs, g_variables.m_aimbot_pistol_hitboxes };
		static const class_vars_t heavy_pistol{ g_variables.m_aimbot_heavy_pistol_override, g_variables.m_aimbot_heavy_pistol_fov,
			                                    g_variables.m_aimbot_heavy_pistol_smooth, g_variables.m_aimbot_heavy_pistol_rcs,
			                                    g_variables.m_aimbot_heavy_pistol_hitboxes };
		static const class_vars_t smg{ g_variables.m_aimbot_smg_override, g_variables.m_aimbot_smg_fov, g_variables.m_aimbot_smg_smooth,
			                           g_variables.m_aimbot_smg_rcs, g_variables.m_aimbot_smg_hitboxes };
		static const class_vars_t rifle{ g_variables.m_aimbot_rifle_override, g_variables.m_aimbot_rifle_fov, g_variables.m_aimbot_rifle_smooth,
			                             g_variables.m_aimbot_rifle_rcs, g_variables.m_aimbot_rifle_hitboxes };
		static const class_vars_t sniper{ g_variables.m_aimbot_sniper_override, g_variables.m_aimbot_sniper_fov, g_variables.m_aimbot_sniper_smooth,
			                              g_variables.m_aimbot_sniper_rcs, g_variables.m_aimbot_sniper_hitboxes, true };
		static const class_vars_t scout{ g_variables.m_aimbot_scout_override, g_variables.m_aimbot_scout_fov, g_variables.m_aimbot_scout_smooth,
			                             g_variables.m_aimbot_scout_rcs, g_variables.m_aimbot_scout_hitboxes, true };
		static const class_vars_t heavy{ g_variables.m_aimbot_heavy_override, g_variables.m_aimbot_heavy_fov, g_variables.m_aimbot_heavy_smooth,
			                             g_variables.m_aimbot_heavy_rcs, g_variables.m_aimbot_heavy_hitboxes };

		switch ( id ) {
		case weapon_glock:
		case weapon_elite:
		case weapon_p250:
		case weapon_tec9:
		case weapon_cz75a:
		case weapon_usp_silencer:
		case weapon_hkp2000:
		case weapon_fiveseven:
			return &pistol;
		case weapon_deagle:
		case weapon_revolver:
			return &heavy_pistol;
		case weapon_mac10:
		case weapon_mp7:
		case weapon_ump45:
		case weapon_p90:
		case weapon_bizon:
		case weapon_mp9:
		case weapon_mp5sd:
			return &smg;
		case weapon_ak47:
		case weapon_aug:
		case weapon_famas:
		case weapon_galilar:
		case weapon_m4a1:
		case weapon_m4a1_silencer:
		case weapon_sg556:
			return &rifle;
		case weapon_awp:
		case weapon_g3sg1:
		case weapon_scar20:
			return &sniper;
		case weapon_ssg08:
			return &scout;
		case weapon_nova:
		case weapon_xm1014:
		case weapon_sawedoff:
		case weapon_m249:
		case weapon_negev:
		case weapon_mag7:
			return &heavy;
		default:
			return nullptr;
		}
	}

	float scale_fov_with_scope( const float fov_degrees )
	{
		if ( fov_degrees <= 0.f || !GET_VARIABLE( g_variables.m_aimbot_scope_fov_scale, bool ) || !g_ctx.m_local )
			return fov_degrees;

		const int default_fov = g_ctx.m_local->get_default_fov( );
		const float base      = default_fov > 0 ? static_cast< float >( default_fov ) : 90.f;
		const float view      = static_cast< float >( g_ctx.m_local->get_zoom_fov( ) );

		if ( view <= 0.f || view >= base )
			return fov_degrees;

		const float ratio = std::tanf( deg2rad( view * 0.5f ) ) / std::tanf( deg2rad( base * 0.5f ) );

		return rad2deg( std::atanf( std::tanf( deg2rad( std::min( fov_degrees, 89.f ) ) ) * ratio ) );
	}

	aim_settings_t weapon_settings( const class_vars_t* vars )
	{
		if ( !vars )
			return { 0.f, 1.f, 0.f };

		if ( GET_VARIABLE( vars->m_override, bool ) )
			return { scale_fov_with_scope( GET_VARIABLE( vars->m_fov, float ) ), GET_VARIABLE( vars->m_smooth, float ),
			         GET_VARIABLE( vars->m_rcs, float ) };

		return { scale_fov_with_scope( GET_VARIABLE( g_variables.m_aimbot_general_fov, float ) ),
		         GET_VARIABLE( g_variables.m_aimbot_general_smooth, float ), GET_VARIABLE( g_variables.m_aimbot_general_rcs, float ) };
	}

	struct hitbox_list_t {
		int m_boxes[ 16 ] = { };
		int m_count       = 0;

		void add( const int box )
		{
			if ( m_count < 16 )
				m_boxes[ m_count++ ] = box;
		}

		const int* begin( ) const { return m_boxes; }
		const int* end( ) const { return m_boxes + m_count; }
	};

	hitbox_list_t enabled_hitboxes( const class_vars_t* vars )
	{
		std::uint32_t groups_var = g_variables.m_aimbot_hitboxes;

		if ( vars && GET_VARIABLE( vars->m_override, bool ) )
			groups_var = vars->m_hitboxes;

		const auto& groups = GET_VARIABLE( groups_var, std::vector< bool > );

		hitbox_list_t out{ };

		const auto add = [ & ]( const int group, const std::initializer_list< int > boxes ) {
			if ( group < static_cast< int >( groups.size( ) ) && groups[ group ] ) {
				for ( const int box : boxes )
					out.add( box );
			}
		};

		add( aim_hitbox_head, { hitbox_head } );
		add( aim_hitbox_neck, { hitbox_neck } );
		add( aim_hitbox_chest, { hitbox_chest } );
		add( aim_hitbox_stomach, { hitbox_stomach } );
		add( aim_hitbox_pelvis, { hitbox_pelvis } );
		add( aim_hitbox_arms, { hitbox_right_upper_arm, hitbox_left_upper_arm } );
		add( aim_hitbox_legs, { hitbox_right_thigh, hitbox_left_thigh } );

		/* never scan nothing */
		if ( out.m_count == 0 )
			out.add( hitbox_head );

		return out;
	}

	matrix3x4_t* live_bones( c_base_entity* entity )
	{
		auto& cached = entity->get_cached_bone_data( );

		return cached.count( ) > 0 ? cached.base( ) : nullptr;
	}

	c_base_entity* firing_weapon( )
	{
		if ( !g_ctx.m_cmd || !g_ctx.m_local )
			return nullptr;

		const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
		if ( !weapon )
			return nullptr;

		const short definition_index = weapon->get_item_definition_index( );

		const auto weapon_data = g_interfaces.m_weapon_system->get_weapon_data( definition_index );
		if ( !weapon_data || !weapon_data->is_gun( ) )
			return nullptr;

		int fire_buttons = in_attack;
		if ( definition_index == e_item_definition_index::weapon_revolver )
			fire_buttons |= in_second_attack;

		return ( g_ctx.m_cmd->m_buttons & fire_buttons ) ? weapon : nullptr;
	}

	float recoil_scale( )
	{
		static const auto convar = g_convars[ HASH_BT( "weapon_recoil_scale" ) ];

		return convar ? convar->get_float( ) : 2.f;
	}

	c_angle rcs_offset( const float class_rcs, const int burst_shots )
	{
		if ( class_rcs <= 0.f || !g_ctx.m_local )
			return { };

		if ( burst_shots < GET_VARIABLE( g_variables.m_aimbot_rcs_start, int ) )
			return { };

		const float master  = ( class_rcs / 100.f ) * recoil_scale( );
		const c_angle punch = g_ctx.m_local->get_punch( );

		return { punch.m_x * master * ( GET_VARIABLE( g_variables.m_aimbot_rcs_pitch, float ) / 100.f ),
		         punch.m_y * master * ( GET_VARIABLE( g_variables.m_aimbot_rcs_yaw, float ) / 100.f ), 0.f };
	}

	float rcs_unwind( const float applied, const float want, const float dt )
	{
		if ( std::fabsf( want ) >= std::fabsf( applied ) )
			return want;

		const float max_step = ( std::max )( std::fabsf( applied ) * ( 1.f - std::exp( -20.f * dt ) ), 20.f * dt );

		if ( std::fabsf( applied - want ) <= max_step )
			return want;

		return applied + ( want > applied ? max_step : -max_step );
	}

	struct fov_scorer_t {
		c_vector m_view_forward{ };
		c_angle m_punch{ };
		bool m_has_punch  = false;
		float m_punch_tan = 0.f;

		void setup( const c_angle& view_point, const c_angle& punch )
		{
			g_math.angle_vectors( view_point, &m_view_forward );

			m_punch     = punch;
			m_has_punch = punch.m_x != 0.f || punch.m_y != 0.f || punch.m_z != 0.f;

			m_punch_tan = std::tanf( std::min( deg2rad( std::fabsf( punch.m_x ) + std::fabsf( punch.m_y ) ), deg2rad( 60.f ) ) );
		}

		float cosine( const c_vector& eye_position, const c_vector& point ) const
		{
			const c_vector delta = point - eye_position;

			c_vector forward = delta.normalized( );

			if ( m_has_punch ) {
				c_angle angle{ };
				g_math.vector_angles( delta, angle );
				angle.normalize( );
				angle -= m_punch;

				g_math.angle_vectors( angle, &forward );
			}

			return std::clamp( m_view_forward.dot_product( forward ), -1.f, 1.f );
		}

		static float cosine_of( const float fov_degrees ) { return fov_degrees >= 180.f ? -1.f : std::cosf( deg2rad( fov_degrees ) ); }
	};

	/* every hitbox fits in this ball around origin + k_body_center. deliberately oversized: the cull
	   must never cut a real candidate. */
	constexpr float k_body_center = 36.f;
	constexpr float k_body_radius = 72.f;

	float best_possible_cosine( const fov_scorer_t& scorer, const c_vector& eye_position, const c_vector& origin )
	{
		const c_vector delta = ( origin + c_vector( 0.f, 0.f, k_body_center ) ) - eye_position;

		const float distance_squared = delta.length_squared( );

		if ( distance_squared <= k_body_radius * k_body_radius )
			return 1.f;

		const float distance = std::sqrtf( distance_squared );

		const float sin_spread = std::min( ( k_body_radius + distance * scorer.m_punch_tan ) / distance, 1.f );
		if ( sin_spread >= 1.f )
			return 1.f;

		const float cos_center = std::clamp( scorer.m_view_forward.dot_product( delta / distance ), -1.f, 1.f );
		const float sin_center = std::sqrtf( 1.f - cos_center * cos_center );
		const float cos_spread = std::sqrtf( 1.f - sin_spread * sin_spread );

		if ( cos_center >= cos_spread )
			return 1.f;

		return cos_center * cos_spread + sin_center * sin_spread;
	}

	struct shortlist_t {
		static constexpr int k_max_boxes    = 16;
		static constexpr int k_keep_per_box = 3;
		static constexpr int k_max_entries  = k_max_boxes * k_keep_per_box;

		struct entry_t {
			float m_cosine                        = -2.f;
			n_lagcomp::impl_t::record_t* m_record = nullptr;
			c_vector m_position{ };
			int m_hitbox = 0;
		};

		entry_t m_entries[ k_max_entries ]{ };

		void clear( )
		{
			for ( auto& entry : m_entries )
				entry = { };
		}

		void add( const int slot, const float cosine, n_lagcomp::impl_t::record_t* record, const c_vector& position, const int hitbox )
		{
			entry_t* row = &m_entries[ slot * k_keep_per_box ];

			int i = k_keep_per_box - 1;
			if ( cosine <= row[ i ].m_cosine )
				return;

			for ( ; i > 0 && cosine > row[ i - 1 ].m_cosine; i-- )
				row[ i ] = row[ i - 1 ];

			row[ i ] = { cosine, record, position, hitbox };
		}

		int sorted( entry_t* out ) const
		{
			int count = 0;

			for ( const auto& entry : m_entries ) {
				if ( entry.m_cosine <= -2.f )
					continue;

				int i = count++;
				for ( ; i > 0 && entry.m_cosine > out[ i - 1 ].m_cosine; i-- )
					out[ i ] = out[ i - 1 ];

				out[ i ] = entry;
			}

			return count;
		}
	};

	/* autowall: hitbox must take min damage (through walls too). else per-HITBOX line of sight
	   (can_see_matrix only traces the head) */
	bool passes_gate( c_base_entity* entity, const c_vector& position, const c_vector& eye_position, const bool record, const int hit_group )
	{
		if ( GET_VARIABLE( g_variables.m_aimbot_autowall, bool ) ) {
			fire_bullet_data_t data = { };
			const float damage = record ? g_auto_wall.get_record_damage( position, entity, hit_group ) : g_auto_wall.get_damage( position, &data );
			return damage >= static_cast< float >( GET_VARIABLE( g_variables.m_aimbot_min_damage, int ) );
		}

		if ( !record )
			return g_ctx.m_local->can_see_position( entity, position, eye_position );

		/* record point: the live model is not there on the server, it must not count as "seen" */
		c_game_trace trace;
		c_trace_filter filter( [ entity ]( c_base_entity* hit, int ) { return hit != g_ctx.m_local && hit != entity; } );
		ray_t ray( eye_position, position );

		g_interfaces.m_engine_trace->trace_ray( ray, mask_shot | contents_grate, &filter, &trace );

		return trace.m_fraction > 0.99f;
	}

	/* hitbox centre vs the origin its matrix belongs to: standing head ~64 up, feet ~4, arms/lean < 30 sideways.
	   anything outside is a skeleton from somewhere else ( stale render cache, bad record ) */
	bool on_body( const c_vector& position, const c_vector& origin )
	{
		const c_vector offset = position - origin;
		return std::isfinite( offset.m_z ) && offset.m_z > -12.f && offset.m_z < 88.f && offset.length_2d( ) < 40.f;
	}

	void log_void( c_base_entity* entity, const bool record, const int hitbox, const c_vector& position, const c_vector& origin )
	{
		g_dbg_void++;

		/* 1/s, a stale skeleton trips every hitbox every cmd */
		static float last = -1.f;
		const float now   = g_interfaces.m_global_vars_base->m_real_time;
		if ( now >= last && now - last < 1.f )
			return;

		last = now;

		const c_vector offset = position - origin;
		botox_dbg_log( "AIM: void ent=%d rec=%d hb=%d off=%.0f/%.0f/%.0f dist2d=%.0f", entity->get_index( ), ( int )record, hitbox, offset.m_x,
		               offset.m_y, offset.m_z, offset.length_2d( ) );
	}

	struct target_t {
		c_base_entity* m_entity               = nullptr;
		n_lagcomp::impl_t::record_t* m_record = nullptr;
		int m_hitbox                          = hitbox_head;
		c_vector m_position{ };
	};

	struct scan_t {
		fov_scorer_t m_scorer{ };
		hitbox_list_t m_hitboxes{ };
		float m_fov    = 180.f;
		bool m_live    = true;
		bool m_records = false;
		int m_only     = 0;
	};

	target_t scan_targets( const scan_t& scan )
	{
		target_t best{ };
		const float cosine_limit = fov_scorer_t::cosine_of( scan.m_fov );
		float best_cosine        = cosine_limit;

		g_dbg_need = cosine_limit;

		const auto eye_position = g_ctx.m_local->get_eye_position( false );

		shortlist_t shortlist{ };
		shortlist_t::entry_t ranked[ shortlist_t::k_max_entries ]{ };

		g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
			if ( scan.m_only && entity && entity->get_index( ) != scan.m_only )
				return;

			g_dbg_players++;

			if ( !entity || !entity->is_valid_aim_target( ) || !player_list_aim_allowed( entity->get_index( ) ) )
				return;

			g_dbg_valid++;

			if ( entity->has_immunity( ) ) {
				g_dbg_immune++;
				return;
			}

			const auto record_list = scan.m_records ? g_lagcomp.m_records[ entity->get_index( ) ] : nullptr;
			const int record_count = record_list ? g_ctx.m_max_allocations : 0;

			hitbox_resolver_t resolver{ };
			bool resolver_ready = false;

			shortlist.clear( );

			for ( int i = scan.m_live ? -1 : 0; i < record_count; i++ ) {
				auto* record = i < 0 ? nullptr : &record_list[ i ];

				/* fresh on THIS cmd's clock ( m_valid is a frame stale ). rejected record = rewound to window centre = miss */
				if ( record && !g_lagcomp.usable( entity, record->m_sim_time ) )
					continue;

				auto* matrix = record ? record->m_matrix : live_bones( entity );
				if ( !matrix ) {
					g_dbg_nobones++;
					continue;
				}

				const float required = best_cosine;

				const c_vector body_origin = record ? record->m_vec_origin : entity->get_abs_origin( );
				const float body_cosine    = best_possible_cosine( scan.m_scorer, eye_position, body_origin );

				if ( body_cosine > g_dbg_best )
					g_dbg_best = body_cosine;

				if ( body_cosine <= required )
					continue;

				if ( !resolver_ready ) {
					if ( !resolver.setup( entity ) )
						return;

					resolver_ready = true;
				}

				int slot = 0;
				for ( const int hitbox : scan.m_hitboxes ) {
					const auto position = resolver.position( hitbox, matrix );

					/* point must sit on the body it claims. the render bone cache only rebuilds when the model draws, so an
					   enemy off screen / culled keeps bones where it WAS = aim + shot at empty air */
					if ( !on_body( position, body_origin ) ) {
						log_void( entity, record != nullptr, hitbox, position, body_origin );
						slot++;
						continue;
					}

					if ( const float cosine = scan.m_scorer.cosine( eye_position, position ); cosine > required )
						shortlist.add( slot, cosine, record, position, hitbox );

					slot++;
				}
			}

			const int ranked_count = shortlist.sorted( ranked );

			for ( int i = 0; i < ranked_count && ranked[ i ].m_cosine > best_cosine; i++ ) {
				const auto& candidate = ranked[ i ];

				const auto box = resolver.m_set ? resolver.m_set->get_hitbox( candidate.m_hitbox ) : nullptr;

				if ( !passes_gate( entity, candidate.m_position, eye_position, candidate.m_record != nullptr, box ? box->m_group : hitgroup_generic ) ) {
					g_dbg_gated++;
					continue;
				}

				best_cosine = candidate.m_cosine;
				best        = { entity, candidate.m_record, candidate.m_hitbox, candidate.m_position };
				break;
			}
		} );

		return best;
	}

	constexpr float k_ray_range = 8192.f;

	float ray_segment_distance_sq( const c_vector& eye, const c_vector& dir, const float range, const c_vector& a, const c_vector& b )
	{
		const c_vector axis   = b - a;
		const c_vector offset = eye - a;

		const float e = axis.length_squared( ), f = axis.dot_product( offset ), c = dir.dot_product( offset ), d = dir.dot_product( axis );

		float s = 0.f, t = 0.f;

		if ( e <= 1e-4f )
			s = std::clamp( -c, 0.f, range );
		else {
			const float denom = e - d * d;

			s = denom > 1e-4f ? std::clamp( ( d * f - c * e ) / denom, 0.f, range ) : 0.f;
			t = ( d * s + f ) / e;

			if ( t < 0.f ) {
				t = 0.f;
				s = std::clamp( -c, 0.f, range );
			} else if ( t > 1.f ) {
				t = 1.f;
				s = std::clamp( d - c, 0.f, range );
			}
		}

		return ( ( eye + dir * s ) - ( a + axis * t ) ).length_squared( );
	}

	float ray_depth( const hitbox_resolver_t& resolver, matrix3x4_t* matrix, const c_vector& eye, const c_vector& dir, int& hitbox )
	{
		float best = -1e9f;

		for ( int i = 0; i < resolver.m_set->m_hit_boxes; i++ ) {
			const auto box = resolver.m_set->get_hitbox( i );

			if ( !box || box->m_radius <= 0.f || box->m_bone < 0 || box->m_bone >= 128 )
				continue;

			const auto& bone     = matrix[ box->m_bone ];
			const float distance = std::sqrtf( ray_segment_distance_sq( eye, dir, k_ray_range, g_math.vector_transform( box->m_bb_min, bone ),
			                                                            g_math.vector_transform( box->m_bb_max, bone ) ) );

			if ( box->m_radius - distance > best ) {
				best   = box->m_radius - distance;
				hitbox = i;
			}
		}

		return best;
	}
}

float n_aimbot::impl_t::active_fov( )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return 0.f;

	const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	if ( !weapon )
		return 0.f;

	return weapon_settings( weapon_class_vars( weapon->get_item_definition_index( ) ) ).m_fov;
}

void n_aimbot::impl_t::on_level_init( )
{
	m_ns_sent_on   = false;
	m_ns_last_send = 0.f;

	m_ns_window_end = 0.f;
	nospread_apply( false );
}

void n_aimbot::impl_t::nospread_apply( const bool on )
{
	if ( on == m_ns_held )
		return;

	if ( !m_ns_looked_up ) {
		m_ns_looked_up = true;
		m_ns_convar    = g_interfaces.m_convar ? g_interfaces.m_convar->find_var( "weapon_accuracy_nospread" ) : nullptr;
	}

	if ( !m_ns_convar )
		return;

	m_ns_convar->force_value( on ? 1.f : 0.f );
	m_ns_held = on;
}

void n_aimbot::impl_t::run_nospread( )
{
	if ( !g_interfaces.m_engine_client )
		return;

	const bool enabled = GET_VARIABLE( g_variables.m_nospread_enable, bool );

	if ( !g_interfaces.m_engine_client->is_in_game( ) ) {
		m_ns_sent_on    = false;
		m_ns_last_send  = 0.f;
		m_ns_window_end = 0.f;
		nospread_apply( false );
		return;
	}

	const float real_time = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	if ( !enabled || real_time >= m_ns_window_end ) {
		m_ns_window_end = 0.f;
		nospread_apply( false );
	}

	if ( !GET_VARIABLE( g_variables.m_nospread_sourcemod, bool ) ) {
		if ( m_ns_sent_on ) {
			g_interfaces.m_engine_client->execute_client_cmd( "sm_nospread 0" );
			m_ns_sent_on = false;
		}
		return;
	}

	const bool want = enabled;

	if ( want == m_ns_sent_on )
		return;

	const float now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_current_time : 0.f;

	/* 0.05s throttle (curtime 0 always passes). curtime going backwards = map restart, don't throttle. */
	if ( now != 0.f && m_ns_last_send != 0.f && now >= m_ns_last_send && now - m_ns_last_send < 0.05f )
		return;

	g_interfaces.m_engine_client->execute_client_cmd( want ? "sm_nospread 1" : "sm_nospread 0" );

	m_ns_sent_on   = want;
	m_ns_last_send = now;
}

void n_aimbot::impl_t::nospread_pre_move( )
{
	if ( !GET_VARIABLE( g_variables.m_nospread_enable, bool ) )
		return;

	if ( !firing_weapon( ) )
		return;

	nospread_apply( true );
}

void n_aimbot::impl_t::nospread_mark_shot( )
{
	if ( !g_interfaces.m_global_vars_base )
		return;

	const auto weapon = firing_weapon( );
	if ( !weapon )
		return;

	if ( !g_ctx.m_local->can_shoot( weapon ) )
		return;

	{
		const float server_now = g_lagcomp.server_time( );
		const bool record_shot = g_lagcomp.m_shot == n_lagcomp::e_shot::record && g_ctx.m_record;
		const bool live_shot   = g_lagcomp.m_shot == n_lagcomp::e_shot::live;
		const float record_age = record_shot ? ( server_now - g_ctx.m_record->m_sim_time ) * 1000.f : -1.f;
		const bool accepted    = record_shot ? g_lagcomp.is_valid( g_ctx.m_record->m_sim_time, 0.f )
		                                     : ( g_lagcomp.live_accepted( ) || ( live_shot && g_lagcomp.is_hosting( ) ) );

		float latency_in = 0.f, latency_out = 0.f;
		if ( const auto net_channel = g_interfaces.m_engine_client->get_net_channel_info( ) ) {
			latency_in  = net_channel->get_latency( FLOW_INCOMING ) * 1000.f;
			latency_out = net_channel->get_latency( FLOW_OUTGOING ) * 1000.f;
		}

		const float off_pitch = std::remainderf( g_ctx.m_cmd->m_view_point.m_x - g_ctx.old_view_point.m_x, 360.f );
		const float off_yaw   = std::remainderf( g_ctx.m_cmd->m_view_point.m_y - g_ctx.old_view_point.m_y, 360.f );

		botox_dbg_log( "SHOT: gate=%s hb=%d rec=%d lv=%d gated=%d age=%.1fms ok=%d centre=%.0fms tick=%d srv=%.3f push=%d ns=%d lat=%.0f/%.0f "
		               "off=%.1f pen=%.3f gnd=%d vz=%.0f wpn=%d spd=%.0f inacc=%.4f",
		                   g_dbg_gate ? g_dbg_gate : "-", m_target_hitbox, ( int )record_shot, ( int )live_shot, g_dbg_gated, record_age,
		                   ( int )accepted, g_lagcomp.window_center( ) * 1000.f, g_ctx.m_cmd->m_tick_count, server_now,
		                   g_lagcomp.push_ticks( ), ( int )GET_VARIABLE( g_variables.m_nospread_enable, bool ), latency_in, latency_out,
		                   std::sqrtf( off_pitch * off_pitch + off_yaw * off_yaw ), weapon->get_accuracy_penalty( ),
		                   ( g_ctx.m_local->get_flags( ) & e_flags::fl_onground ) ? 1 : 0, g_ctx.m_local->get_velocity( ).m_z,
		                   ( int )weapon->get_item_definition_index( ), g_ctx.m_local->get_velocity( ).length_2d( ), weapon->get_inaccuracy( ) );
	}

	if ( !GET_VARIABLE( g_variables.m_nospread_enable, bool ) )
		return;

	float lead = g_interfaces.m_global_vars_base->m_interval_per_tick * 2.f;

	if ( const auto net_channel = g_interfaces.m_engine_client->get_net_channel_info( ) )
		lead += net_channel->get_latency( FLOW_OUTGOING );

	m_ns_window_end = g_interfaces.m_global_vars_base->m_real_time + std::clamp( lead, 0.03f, 0.5f );

	nospread_apply( true );
}

void n_aimbot::impl_t::nospread_frame_stage( const int stage )
{
	if ( stage == e_client_frame_stage::render_start ) {
		nospread_apply( false );
		return;
	}

	if ( stage != e_client_frame_stage::start )
		return;

	const float real_time = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;

	if ( m_ns_window_end != 0.f && real_time < m_ns_window_end )
		nospread_apply( true );
	else
		m_ns_window_end = 0.f;
}

bool n_aimbot::impl_t::can_aimbot( )
{
	if ( !GET_VARIABLE( g_variables.m_aimbot_enable, bool ) || g_ctx.m_cmd->m_tick_count == 0 )
		return false;

	auto& key = GET_VARIABLE( g_variables.m_aimbot_key, key_bind_t );
	if ( GET_VARIABLE( g_variables.m_aimbot_on_key, bool ) && key.m_key_style != 0 )
		return g_input.check_input( &key );

	return firing_weapon( ) != nullptr;
}

bool n_aimbot::impl_t::shot_leaves( )
{
	if ( !g_ctx.m_cmd || !g_ctx.m_local || !( g_ctx.m_cmd->m_buttons & ( in_attack | in_second_attack ) ) )
		return false;

	const auto active = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	const auto data   = active ? g_interfaces.m_weapon_system->get_weapon_data( active->get_item_definition_index( ) ) : nullptr;
	if ( !data || !data->is_gun( ) )
		return true;

	const auto weapon = firing_weapon( );
	if ( !weapon || !g_ctx.m_local->can_shoot( weapon ) )
		return false;

	/* weapon_csbase.cpp:992, m_iShotsFired clears on release */
	return data->m_full_auto || g_ctx.m_local->get_shots_fired( ) == 0 || weapon->get_burst_shots_remaining( ) > 0 ||
	       weapon->get_item_definition_index( ) == e_item_definition_index::weapon_revolver;
}

void n_aimbot::impl_t::on_create_move_post( )
{
	if ( !g_ctx.m_local || !g_ctx.m_cmd || !g_ctx.m_local->is_alive( ) )
		return;

	const bool silent    = GET_VARIABLE( g_variables.m_aimbot_silent, bool );
	const bool trigger   = firing_weapon( ) != nullptr;
	const float interval = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const auto weapon    = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	const bool can_fire  = weapon && g_ctx.m_local->can_shoot( weapon );

	if ( !trigger )
		m_visible_first = false;

	const bool first_bullet = !silent && can_fire && trigger && !m_visible_first;
	if ( first_bullet )
		m_visible_first = true;

	c_angle camera{ };
	g_interfaces.m_engine_client->get_view_angles( camera );

	aim_settings_t settings{ };

	const auto find_target = [ & ]( ) -> target_t {
		if ( !can_aimbot( ) ) {
			dbg_gate( "off" );
			return { };
		}

		const auto class_vars = weapon ? weapon_class_vars( weapon->get_item_definition_index( ) ) : nullptr;
		settings              = weapon_settings( class_vars );

		if ( !class_vars || settings.m_fov <= 0.f ) {
			dbg_gate( "fov0" );
			return { };
		}

		if ( weapon->get_ammo( ) <= 0 ) {
			dbg_gate( "noammo" );
			return { };
		}

		if ( silent && !can_fire ) {
			const auto data = g_interfaces.m_weapon_system->get_weapon_data( weapon->get_item_definition_index( ) );

			if ( !data || !data->m_full_auto || m_rcs_shots <= 0 || !trigger ) {
				dbg_gate( "cantshoot" );
				return { };
			}
		}

		const bool backtrack = GET_VARIABLE( g_variables.m_backtrack_enable, bool );
		const bool live_ok   = !backtrack || g_lagcomp.live_accepted( );

		scan_t scan{ };
		scan.m_scorer.setup( silent ? g_ctx.old_view_point : camera, rcs_offset( m_rcs_stale_punch ? 0.f : settings.m_rcs, m_rcs_shots ) );
		scan.m_fov     = settings.m_fov;
		scan.m_live    = live_ok;
		scan.m_records = backtrack && ( GET_VARIABLE( g_variables.m_aimbot_aim_at_backtrack, bool ) || !live_ok );

		if ( class_vars->m_sniper && !g_ctx.m_local->is_scoped( ) ) {
			scan.m_hitboxes.add( hitbox_stomach );
			scan.m_hitboxes.add( hitbox_chest );
			scan.m_hitboxes.add( hitbox_pelvis );
		} else
			scan.m_hitboxes = enabled_hitboxes( class_vars );

		g_dbg_best  = -2.f;
		g_dbg_gated = 0;
		g_dbg_players = g_dbg_valid = g_dbg_immune = g_dbg_nobones = g_dbg_void = 0;

		const auto scan_all = [ & ]( ) {
			target_t found{ };
			if ( m_lock_index ) {
				scan.m_only = m_lock_index;
				found       = scan_targets( scan );
				scan.m_only = 0;
			}
			return found.m_entity ? found : scan_targets( scan );
		};

		target_t found = scan_all( );

		if ( !found.m_entity && backtrack && !scan.m_records ) {
			scan.m_live    = false;
			scan.m_records = true;
			found          = scan_all( );
		}

		if ( !found.m_entity && !live_ok && g_lagcomp.is_hosting( ) ) {
			scan.m_live    = true;
			scan.m_records = false;
			found          = scan_all( );
		}

		g_ctx.m_record = found.m_record;

		if ( !found.m_entity ) {
			dbg_gate( "notarget" );
			return { };
		}

		dbg_gate( "aim" );
		return found;
	};

	const target_t target = find_target( );
	const bool aiming     = target.m_entity != nullptr;

	if ( !aiming )
		m_lock_index = 0;
	else {
		if ( const int index = target.m_entity->get_index( ); index != m_lock_index ) {
			botox_dbg_log( "AIM: lock %d -> %d hb=%d silent=%d fire=%d rec=%d", m_lock_index, index, target.m_hitbox, ( int )silent,
			               ( int )trigger, target.m_record ? 1 : 0 );
			m_lock_index = index;
		}

		m_target_hitbox = target.m_hitbox;
	}

	m_rcs_aim_burst |= aiming;

	const float strength = m_rcs_stale_punch ? 0.f : ( aiming ? settings.m_rcs : rcs_strength( true ) );
	const c_angle want   = rcs_offset( strength, m_rcs_shots );

	if ( const bool comp = !want.is_zero( ); comp != m_rcs_comping ) {
		m_rcs_comping = comp;

		if ( comp )
			botox_dbg_log( "RCS: on str=%.0f silent=%d aim=%d shots=%d scale=%.2f want=%.2f/%.2f", strength, ( int )silent, ( int )aiming,
			               m_rcs_shots, recoil_scale( ), want.m_x, want.m_y );
	}

	c_angle aim{ };
	if ( aiming ) {
		aim = g_math.calculate_angle( g_ctx.m_local->get_eye_position( false ), target.m_position ) - want;
		aim.normalize( );
	}

	const auto publish = [ & ]( c_angle shot ) {
		shot.normalize( );
		shot.clamp( );

		/* NaN survives normalize / clamp and would ride the cmd out */
		if ( !std::isfinite( shot.m_x ) || !std::isfinite( shot.m_y ) )
			return false;

		m_shot_pitch      = shot.m_x;
		m_shot_yaw        = shot.m_y;
		m_shot_view_valid = true;
		m_shot_is_aim     = aiming;
		return true;
	};

	if ( silent ) {
		m_rcs_applied_x = m_rcs_applied_y = 0.f;

		if ( !aiming ) {
			publish( g_ctx.old_view_point - want );
			return;
		}

		/* no smoothing: the base is your mouse view every tick ( camera never moves ), a fraction would send every
		   bullet a fixed part of the way = a miss */
		if ( const float step = GET_VARIABLE( g_variables.m_aimbot_silent_step, float ); step > 0.f ) {
			c_angle delta = aim - g_ctx.old_view_point;
			delta.normalize( );
			delta.m_x = std::clamp( delta.m_x, -step, step );
			delta.m_y = std::clamp( delta.m_y, -step, step );
			aim       = g_ctx.old_view_point + delta;
		}

		if ( publish( aim ) && can_fire && trigger )
			g_lagcomp.commit_shot( target.m_record );

		return;
	}

	if ( ( g_edgebug.m_found && GET_VARIABLE( g_variables.m_edgebug_mouse_lock, bool ) ) || g_edge_skip.lock_amount( ) > 0.f ) {
		publish( g_ctx.old_view_point );
		return;
	}

	const c_angle start = camera;

	/* rcs step: the camera carries `applied`, walked to `want`: growing exact ( bullets must land ), shrinking paced ( rcs_unwind ).
	   only this step writes applied, so once the punch is gone the camera is back where the player put it */
	const float next_x = rcs_unwind( m_rcs_applied_x, want.m_x, interval );
	const float next_y = rcs_unwind( m_rcs_applied_y, want.m_y, interval );

	camera.m_x -= next_x - m_rcs_applied_x;
	camera.m_y -= next_y - m_rcs_applied_y;

	m_rcs_applied_x = next_x;
	m_rcs_applied_y = next_y;

	if ( aiming ) {
		c_angle delta = aim - camera;
		delta.normalize( );

		if ( !first_bullet && settings.m_smooth > 1.f )
			delta = delta * ( 1.f / settings.m_smooth );

		camera += delta;
	}

	camera.normalize( );
	camera.clamp( );

	if ( !std::isfinite( camera.m_x ) || !std::isfinite( camera.m_y ) )
		return;

	c_angle moved = camera - start;
	moved.normalize( );

	/* engine yaw runs unbounded ( ApplyMouse never wraps it ): normalize alone leaves float noise, not a turn */
	if ( std::fabsf( moved.m_x ) > 1e-4f || std::fabsf( moved.m_y ) > 1e-4f ) {
		g_interfaces.m_engine_client->set_view_angles( camera );

		if ( std::fabsf( moved.m_x ) >= 2.f || std::fabsf( moved.m_y ) >= 2.f )
			botox_dbg_log( "AIM: view %.2f/%.2f -> %.2f/%.2f aim=%d first=%d rec=%d want=%.2f/%.2f applied=%.2f/%.2f punch=%.2f/%.2f", start.m_x,
			               start.m_y, camera.m_x, camera.m_y, ( int )aiming, ( int )first_bullet, target.m_record ? 1 : 0, want.m_x, want.m_y,
			               m_rcs_applied_x, m_rcs_applied_y, g_ctx.m_local->get_punch( ).m_x, g_ctx.m_local->get_punch( ).m_y );
	}

	publish( aiming ? camera : g_ctx.old_view_point + moved );
}

void n_aimbot::impl_t::run_backtrack( )
{
	if ( !GET_VARIABLE( g_variables.m_backtrack_enable, bool ) || g_lagcomp.m_shot != n_lagcomp::e_shot::none )
		return;

	const auto weapon = firing_weapon( );
	if ( !weapon || !g_ctx.m_local->can_shoot( weapon ) )
		return;

	c_angle bullet = m_shot_view_valid ? c_angle( m_shot_pitch, m_shot_yaw, 0.f ) : g_ctx.m_cmd->m_view_point;
	bullet += g_ctx.m_local->get_punch( ) * recoil_scale( );
	bullet.normalize( );

	c_vector dir{ };
	g_math.angle_vectors( bullet, &dir );

	const auto eye     = g_ctx.m_local->get_eye_position( false );
	const bool live_ok = g_lagcomp.live_accepted( );

	constexpr float k_near_miss = 4.f;

	n_lagcomp::impl_t::record_t* best_record = nullptr;
	float best_depth  = -k_near_miss;
	int best_hitbox   = -1;
	bool best_is_live = false;
	bool live_hit     = false;

	g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
		if ( live_hit || !entity || !entity->is_valid_aim_target( ) || entity->has_immunity( ) || !player_list_aim_allowed( entity->get_index( ) ) )
			return;

		hitbox_resolver_t resolver{ };
		if ( !resolver.setup( entity ) )
			return;

		int hitbox = -1;

		if ( live_ok ) {
			if ( auto* bones = live_bones( entity ) ) {
				if ( const float depth = ray_depth( resolver, bones, eye, dir, hitbox ); depth >= 0.f ) {
					live_hit        = true;
					m_target_hitbox = hitbox;
					return;
				} else if ( depth > best_depth ) {
					best_depth   = depth;
					best_record  = nullptr;
					best_hitbox  = hitbox;
					best_is_live = true;
				}
			}
		}

		const auto record_list = g_lagcomp.m_records[ entity->get_index( ) ];
		if ( !record_list )
			return;

		for ( int i = 0; i < g_ctx.m_max_allocations; i++ ) {
			auto* record = &record_list[ i ];

			if ( !g_lagcomp.usable( entity, record->m_sim_time ) )
				continue;

			const c_vector body = record->m_vec_origin + c_vector( 0.f, 0.f, k_body_center );
			if ( ray_segment_distance_sq( eye, dir, k_ray_range, body, body ) > ( k_body_radius + k_near_miss ) * ( k_body_radius + k_near_miss ) )
				continue;

			if ( const float depth = ray_depth( resolver, record->m_matrix, eye, dir, hitbox ); depth > best_depth ) {
				best_depth   = depth;
				best_record  = record;
				best_hitbox  = hitbox;
				best_is_live = false;
			}
		}
	} );

	if ( live_hit || best_is_live ) {
		if ( best_is_live && !live_hit )
			m_target_hitbox = best_hitbox;

		g_lagcomp.commit_shot( nullptr );
		return;
	}

	if ( best_record ) {
		m_target_hitbox = best_hitbox;
		g_lagcomp.commit_shot( best_record );
	}
}

void n_aimbot::impl_t::run_zeusbug( )
{
	const auto cmd = g_ctx.m_cmd;

	if ( m_zb_select ) {
		const int select = m_zb_select;
		const bool back  = m_zb_back;
		const bool next  = cmd->m_command_number == m_zb_cmd + 1;
		m_zb_select      = 0;

		if ( next ) {
			cmd->m_weapon_select = select;
			botox_dbg_log( "ZB: cmd=%d select=%d %s", cmd->m_command_number, select, back ? "back" : "zeus" );

			if ( !back && GET_VARIABLE( g_variables.m_zeusbug_swapback, bool ) ) {
				if ( const auto held = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) ) ) {
					m_zb_select = held->get_index( );
					m_zb_cmd    = cmd->m_command_number;
					m_zb_back   = true;
				}
			}
			return;
		}
	}

	if ( !GET_VARIABLE( g_variables.m_zeusbug, bool ) || !g_input.check_input( &GET_VARIABLE( g_variables.m_zeusbug_key, key_bind_t ) ) )
		return;

	const auto weapon = firing_weapon( );
	if ( !weapon || !g_ctx.m_local->can_shoot( weapon ) || Web_BlockFire( ) )
		return;

	const auto weapons = g_ctx.m_local->get_weapons_handle( );
	for ( int i = 0; i < 64; ++i ) {
		const auto handle = weapons[ i ];
		if ( !handle || handle == 0xFFFFFFFF )
			continue;

		const auto entity = reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( handle ) );
		if ( !entity || entity->get_item_definition_index( ) != e_item_definition_index::weapon_taser )
			continue;

		m_zb_select = entity->get_index( );
		m_zb_cmd    = cmd->m_command_number;
		m_zb_back   = false;
		return;
	}
}

void n_aimbot::impl_t::rcs_track( )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) ) {
		m_rcs_shots = m_rcs_last_game_shots = 0;
		m_rcs_applied_x = m_rcs_applied_y = 0.f;
		m_rcs_stale_punch = false;
		m_rcs_aim_burst   = false;
		return;
	}

	const auto weapon     = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	const short weapon_id = weapon ? weapon->get_item_definition_index( ) : 0;

	const c_angle punch = g_ctx.m_local->get_punch( );
	const bool punch_up = std::fabsf( punch.m_x ) + std::fabsf( punch.m_y ) >= 0.02f;

	if ( weapon_id != m_rcs_last_weapon ) {
		botox_dbg_log( "RCS: weapon %d -> %d punch=%.2f/%.2f stale=%d", m_rcs_last_weapon, weapon_id, punch.m_x, punch.m_y,
		                   ( int )punch_up );

		m_rcs_last_weapon = weapon_id;
		m_rcs_shots       = 0;
		m_rcs_stale_punch = punch_up;
	}

	if ( !punch_up ) {
		m_rcs_shots       = 0;
		m_rcs_stale_punch = false;
		m_rcs_aim_burst   = false;
	}

	const int game_shots = g_ctx.m_local->get_shots_fired( );

	if ( game_shots > m_rcs_last_game_shots )
		m_rcs_shots += game_shots - m_rcs_last_game_shots;

	m_rcs_last_game_shots = game_shots;
}

float n_aimbot::impl_t::rcs_strength( const bool gated )
{
	if ( !g_ctx.m_local || !g_ctx.m_cmd || !g_ctx.m_local->is_alive( ) )
		return 0.f;

	if ( gated && GET_VARIABLE( g_variables.m_aimbot_rcs_only_with_aimbot, bool ) && !m_rcs_aim_burst )
		return 0.f;

	const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	if ( !weapon )
		return 0.f;

	return weapon_settings( weapon_class_vars( weapon->get_item_definition_index( ) ) ).m_rcs;
}
