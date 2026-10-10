#pragma once
#include "players.h"
#include "../../../game/sdk/classes/c_csgo_hud_radar.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "../../auto_wall/auto_wall.h"
#include "../../avatar_cache/avatar_cache.h"
#include "../../entity_cache/entity_cache.h"
#include "../../network/botox_net.h"
#include "dormancy/dormancy.h"
#include "sound_esp/sound_esp.h"

#include <cstdio>


/* notched ring arrow, shared by player + sound arrows. ( x, y ) = unit direction, position = spot on
   the ring. tip must stay point 0: imgui fan-fills from it. */
static void push_ring_arrow( const c_vector_2d& position, const float x, const float y, const float width, const float height,
                             const unsigned int color )
{
	const bool interium = GET_VARIABLE( g_variables.m_out_of_fov_arrows_style, int ) == 1;

	const float gap = interium ? 0.f : 3.f;
	const c_vector_2d base = c_vector_2d( position.m_x + x * gap, position.m_y + y * gap );
	const float half_width = width * 0.5f;
	const float notch      = height * ( interium ? 0.2f : 0.15f );

	const c_vector_2d tip   = c_vector_2d( base.m_x + x * height, base.m_y + y * height );
	const c_vector_2d left  = c_vector_2d( base.m_x - y * half_width, base.m_y + x * half_width );
	const c_vector_2d mid   = c_vector_2d( base.m_x + x * notch, base.m_y + y * notch );
	const c_vector_2d right = c_vector_2d( base.m_x + y * half_width, base.m_y - x * half_width );

	poly_draw_object_t arrow{ };
	arrow.m_color = color;

	if ( !interium ) {
		arrow.m_count = 4;
		arrow.m_points[ 0 ] = tip, arrow.m_points[ 1 ] = left, arrow.m_points[ 2 ] = mid, arrow.m_points[ 3 ] = right;
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_poly, std::make_any< poly_draw_object_t >( arrow ) );
		return;
	}

	/* pyramid seen from above minus back face: see-through notched arrow, rim, ridge notch -> tip */
	const unsigned int alpha = static_cast< unsigned int >( ( ( color >> 24 ) & 0xffu ) * 0.45f );
	arrow.m_count = 4;
	arrow.m_points[ 0 ] = tip, arrow.m_points[ 1 ] = left, arrow.m_points[ 2 ] = mid, arrow.m_points[ 3 ] = right;
	arrow.m_color         = ( color & 0x00ffffffu ) | ( alpha << 24 );
	arrow.m_outline_color = color;
	arrow.m_outline       = true;
	arrow.m_thickness     = 1.5f;
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_poly, std::make_any< poly_draw_object_t >( arrow ) );

	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( line_draw_object_t{ mid, tip, color, 1.5f } ) );
}

static c_vector skeleton_spline( const c_vector& p0, const c_vector& p1, const c_vector& p2, const c_vector& p3, const float u )
{
	const auto knot = []( const c_vector& a, const c_vector& b ) { return std::max( std::sqrt( ( b - a ).length( ) ), 1e-3f ); };
	const auto mix  = []( const c_vector& a, const c_vector& b, const float ta, const float tb, const float t ) {
		return a * ( ( tb - t ) / ( tb - ta ) ) + b * ( ( t - ta ) / ( tb - ta ) );
	};
	const float t1 = knot( p0, p1 ), t2 = t1 + knot( p1, p2 ), t3 = t2 + knot( p2, p3 );
	const float t  = t1 + ( t2 - t1 ) * u;
	const c_vector a1 = mix( p0, p1, 0.f, t1, t ), a2 = mix( p1, p2, t1, t2, t ), a3 = mix( p2, p3, t2, t3, t );
	return mix( mix( a1, a2, 0.f, t2, t ), mix( a2, a3, t1, t3, t ), t1, t2, t );
}

struct skeleton_edge_t {
	int m_child, m_parent;
	c_vector m_child_pos, m_parent_pos;
};

static void push_smooth_skeleton( const std::vector< skeleton_edge_t >& edges, const unsigned int color, const float thickness )
{
	constexpr int k_nodes = 129, k_samples = 8;
	int degree[ k_nodes ]{ };
	for ( const auto& e : edges )
		degree[ e.m_child ]++, degree[ e.m_parent ]++;

	std::vector< bool > used( edges.size( ), false );
	std::vector< std::vector< c_vector_2d > > lines;

	const auto emit = [ & ]( const std::vector< c_vector >& p ) {
		const int m = static_cast< int >( p.size( ) );
		std::vector< c_vector_2d > line;
		const auto add = [ & ]( const c_vector& w ) {
			c_vector_2d s{ };
			if ( g_render.world_to_screen( w, s ) )
				line.push_back( s );
			else if ( !line.empty( ) ) /* off screen: split, never bridge */
				lines.push_back( std::exchange( line, { } ) );
		};
		for ( int j = 0; j + 1 < m; j++ ) {
			const c_vector p0 = j > 0 ? p[ j - 1 ] : p[ 0 ] * 2.f - p[ 1 ];
			const c_vector p3 = j + 2 < m ? p[ j + 2 ] : p[ m - 1 ] * 2.f - p[ m - 2 ];
			for ( int s = 0; s < k_samples; s++ )
				add( skeleton_spline( p0, p[ j ], p[ j + 1 ], p3, static_cast< float >( s ) / k_samples ) );
		}
		add( p[ m - 1 ] );
		if ( line.size( ) >= 2 )
			lines.push_back( std::move( line ) );
	};

	for ( int pass = 0; pass < 2; pass++ ) {
		for ( std::size_t first = 0; first < edges.size( ); first++ ) {
			if ( used[ first ] )
				continue;
			const auto& e0 = edges[ first ];
			const bool from_child = degree[ e0.m_child ] != 2;
			if ( pass == 0 && !from_child && degree[ e0.m_parent ] == 2 )
				continue;
			int node = from_child || pass == 1 ? e0.m_child : e0.m_parent;
			std::vector< c_vector > chain{ node == e0.m_child ? e0.m_child_pos : e0.m_parent_pos };
			for ( std::size_t cur = first;; ) {
				used[ cur ] = true;
				const auto& e  = edges[ cur ];
				const bool fwd = e.m_child == node;
				node           = fwd ? e.m_parent : e.m_child;
				chain.push_back( fwd ? e.m_parent_pos : e.m_child_pos );
				if ( degree[ node ] != 2 )
					break;
				std::size_t next = edges.size( );
				for ( std::size_t k = 0; k < edges.size( ); k++ )
					if ( !used[ k ] && ( edges[ k ].m_child == node || edges[ k ].m_parent == node ) )
						next = k;
				if ( next == edges.size( ) )
					break;
				cur = next;
			}
			emit( chain );
		}
	}

	if ( lines.empty( ) )
		return;
	g_render.m_draw_data.emplace_back(
		e_draw_type::draw_type_callback,
		std::make_any< callback_draw_object_t >( callback_draw_object_t{ [ lines = std::move( lines ), color, thickness ]( ImDrawList* draw_list ) {
			std::vector< ImVec2 > points;
			for ( const auto& line : lines ) {
				points.clear( );
				for ( const auto& p : line )
					points.emplace_back( p.m_x, p.m_y );
				draw_list->AddPolyline( points.data( ), static_cast< int >( points.size( ) ), color, ImDrawFlags_None, thickness );
				if ( thickness > 1.5f ) {
					draw_list->AddCircleFilled( points.front( ), thickness * 0.5f, color, 12 );
					draw_list->AddCircleFilled( points.back( ), thickness * 0.5f, color, 12 );
				}
			}
		} } ) );
}

static std::vector< n_players::impl_t::hitbox_capsule_t > hitbox_capsules( c_base_entity* entity )
{
	std::vector< n_players::impl_t::hitbox_capsule_t > capsules;

	hitbox_resolver_t resolver{ };
	auto& bones           = entity->get_cached_bone_data( );
	const int bone_count  = bones.count( );
	matrix3x4_t* matrices = bones.get_elements( );

	if ( !resolver.setup( entity ) || !matrices )
		return capsules;

	for ( int i = 0; i < resolver.m_set->m_hit_boxes; ++i ) {
		const mstudiobbox_t* box = resolver.m_set->get_hitbox( i );

		if ( !box || box->m_radius <= 0.f || box->m_bone < 0 || box->m_bone >= bone_count )
			continue;

		capsules.push_back( { g_math.vector_transform( box->m_bb_min, matrices[ box->m_bone ] ),
		                      g_math.vector_transform( box->m_bb_max, matrices[ box->m_bone ] ), box->m_radius } );
	}

	return capsules;
}

/* wire capsule: end rings, 4 side lines, 2 half circles per cap */
static void push_hitbox_capsules( const std::vector< n_players::impl_t::hitbox_capsule_t >& capsules, const unsigned int color,
                                  const float thickness )
{
	constexpr int k_ring = 16, k_arc = 8;
	constexpr float k_pi = 3.14159265f;

	std::vector< std::vector< c_vector_2d > > lines;
	std::vector< c_vector_2d > line;

	const auto flush = [ & ]( ) {
		if ( line.size( ) >= 2 )
			lines.push_back( line );
		line.clear( );
	};
	const auto add = [ & ]( const c_vector& w ) {
		c_vector_2d s{ };
		if ( g_render.world_to_screen( w, s ) )
			line.push_back( s );
		else
			flush( );
	};

	for ( const auto& capsule : capsules ) {
		c_vector axis      = capsule.m_b - capsule.m_a;
		const float length = axis.length( );
		axis               = length > 1e-3f ? axis * ( 1.f / length ) : c_vector( 0.f, 0.f, 1.f );

		const c_vector helper = std::fabs( axis.m_z ) < 0.9f ? c_vector( 0.f, 0.f, 1.f ) : c_vector( 1.f, 0.f, 0.f );
		const c_vector u      = axis.cross_product( helper ).normalized( );
		const c_vector v      = axis.cross_product( u );
		const float r         = capsule.m_radius;

		for ( const c_vector& end : { capsule.m_a, capsule.m_b } ) {
			for ( int s = 0; s <= k_ring; ++s ) {
				const float t = 2.f * k_pi * s / k_ring;
				add( end + ( u * std::cos( t ) + v * std::sin( t ) ) * r );
			}
			flush( );
		}

		for ( const c_vector& side : { u, u * -1.f, v, v * -1.f } ) {
			add( capsule.m_a + side * r );
			add( capsule.m_b + side * r );
			flush( );
		}

		for ( const c_vector& side : { u, v } ) {
			for ( const float cap : { -1.f, 1.f } ) {
				const c_vector& end = cap < 0.f ? capsule.m_a : capsule.m_b;
				for ( int s = 0; s <= k_arc; ++s ) {
					const float t = k_pi * s / k_arc;
					add( end + ( side * std::cos( t ) + axis * ( cap * std::sin( t ) ) ) * r );
				}
				flush( );
			}
		}
	}

	if ( lines.empty( ) )
		return;

	g_render.m_draw_data.emplace_back(
		e_draw_type::draw_type_callback,
		std::make_any< callback_draw_object_t >( callback_draw_object_t{ [ lines = std::move( lines ), color, thickness ]( ImDrawList* draw_list ) {
			std::vector< ImVec2 > points;
			for ( const auto& line : lines ) {
				points.clear( );
				for ( const auto& p : line )
					points.emplace_back( p.m_x, p.m_y );
				draw_list->AddPolyline( points.data( ), static_cast< int >( points.size( ) ), color, ImDrawFlags_None, thickness );
			}
		} } ) );
}

void n_players::impl_t::on_paint_traverse( )
{
	if ( !GET_VARIABLE( g_variables.m_players, bool ) ) {
		this->m_damage_numbers.clear( );
		this->m_hit_hitboxes.clear( );
		return;
	}

	this->players( );
	this->sound_arrows( );
	this->damage_numbers( );
	this->hit_hitboxes( );
}

void n_players::impl_t::hitboxes_on_hurt( const int victim )
{
	if ( !GET_VARIABLE( g_variables.m_players, bool ) || !GET_VARIABLE( g_variables.m_players_hitboxes, bool ) ||
	     GET_VARIABLE( g_variables.m_players_hitboxes_mode, int ) != 1 )
		return;

	const auto entity = g_interfaces.m_client_entity_list->get< c_base_entity >( victim );
	if ( !entity )
		return;

	auto capsules = hitbox_capsules( entity );
	if ( capsules.empty( ) )
		return;

	if ( this->m_hit_hitboxes.size( ) >= 16 )
		this->m_hit_hitboxes.erase( this->m_hit_hitboxes.begin( ) );

	this->m_hit_hitboxes.push_back( { std::move( capsules ), g_interfaces.m_global_vars_base->m_real_time } );
}

void n_players::impl_t::hit_hitboxes( )
{
	if ( this->m_hit_hitboxes.empty( ) )
		return;

	if ( !GET_VARIABLE( g_variables.m_players_hitboxes, bool ) || GET_VARIABLE( g_variables.m_players_hitboxes_mode, int ) != 1 ) {
		this->m_hit_hitboxes.clear( );
		return;
	}

	const float duration  = std::max( GET_VARIABLE( g_variables.m_players_hitboxes_duration, float ), 0.1f );
	const float thickness = GET_VARIABLE( g_variables.m_players_hitboxes_thickness, float );
	const auto color      = GET_VARIABLE( g_variables.m_players_hitboxes_color, c_color );
	const float now       = g_interfaces.m_global_vars_base->m_real_time;

	for ( auto it = this->m_hit_hitboxes.begin( ); it != this->m_hit_hitboxes.end( ); ) {
		const float elapsed = now - it->m_time;

		if ( elapsed < 0.f || elapsed >= duration ) {
			it = this->m_hit_hitboxes.erase( it );
			continue;
		}

		const float fraction = elapsed / duration;
		const float alpha    = fraction < 0.7f ? 1.f : 1.f - ( fraction - 0.7f ) / 0.3f;

		push_hitbox_capsules( it->m_capsules, color.get_u32( alpha ), thickness );
		++it;
	}
}

void n_players::impl_t::god_resolve( const int index )
{
	auto& god       = this->m_god[ index ];
	const float now = g_interfaces.m_global_vars_base->m_real_time;

	if ( god.m_pending == 0.f || ( now >= god.m_pending && now - god.m_pending < 0.1f ) )
		return;

	if ( now >= god.m_pending )
		god.m_strikes += god.m_hits;

	god.m_pending = 0.f;
	god.m_hits    = 0;
}

void n_players::impl_t::god_on_impact( const int shooter, const c_vector& position )
{
	if ( shooter < 1 || shooter > 64 )
		return;

	const auto shooter_ent = g_interfaces.m_client_entity_list->get< c_base_entity >( shooter );
	if ( !shooter_ent )
		return;

	int hit_index = 0;
	for ( int index = 1; index <= 64; ++index ) {
		const auto victim = g_interfaces.m_client_entity_list->get< c_base_entity >( index );
		if ( !victim || victim == shooter_ent || victim->is_dormant( ) || !victim->is_alive( ) || !shooter_ent->is_enemy( victim ) )
			continue;

		const auto collideable = victim->get_collideable( );
		if ( !collideable )
			continue;

		/* hull pulled in 2u: a wall / floor they touch sits on the hull face, hitboxes sit inside.
		   other shooters are lag compensated to a different spot: miss = no strike, never a false one */
		const c_vector origin = victim->get_abs_origin( );
		const c_vector mins   = origin + collideable->get_obb_mins( ) + c_vector( 2.f, 2.f, 2.f );
		const c_vector maxs   = origin + collideable->get_obb_maxs( ) - c_vector( 2.f, 2.f, 0.f );

		if ( position.m_x < mins.m_x || position.m_y < mins.m_y || position.m_z < mins.m_z || position.m_x > maxs.m_x ||
		     position.m_y > maxs.m_y || position.m_z > maxs.m_z )
			continue;

		if ( hit_index )
			return;
		hit_index = index;
	}

	if ( !hit_index )
		return;

	this->god_resolve( hit_index );

	auto& god = this->m_god[ hit_index ];
	if ( god.m_pending == 0.f )
		god.m_pending = g_interfaces.m_global_vars_base->m_real_time;
	god.m_hits++;
}

void n_players::impl_t::god_on_hurt( const int victim )
{
	if ( victim >= 1 && victim <= 64 )
		this->m_god[ victim ] = { };
}

bool n_players::impl_t::is_god( const int index )
{
	if ( index < 1 || index > 64 )
		return false;

	this->god_resolve( index );
	return this->m_god[ index ].m_strikes >= 2;
}

/* autowall from local eye with held weapon: some hitbox hit only after going through a wall. once per tick */
bool n_players::impl_t::wallbangable( c_base_entity* entity, const int index )
{
	if ( index < 1 || index > 64 )
		return false;

	auto& cache    = this->m_wallbang[ index ];
	const int tick = g_interfaces.m_global_vars_base->m_tick_count;
	if ( cache.m_tick == tick )
		return cache.m_result;

	cache.m_tick   = tick;
	cache.m_result = false;

	if ( !g_ctx.m_local->is_alive( ) )
		return false;

	const auto active = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	const auto data   = active ? g_interfaces.m_weapon_system->get_weapon_data( active->get_item_definition_index( ) ) : nullptr;
	if ( !data || !data->is_gun( ) )
		return false;

	const auto bones = entity->get_cached_bone_data( ).base( );
	if ( !bones )
		return false;

	for ( const int hitbox : { e_hitboxes::hitbox_head, e_hitboxes::hitbox_upper_chest, e_hitboxes::hitbox_stomach, e_hitboxes::hitbox_pelvis } ) {
		fire_bullet_data_t data = { };
		if ( g_auto_wall.get_damage( entity->get_hitbox_position( hitbox, bones ), &data ) >= 1.f && data.m_penetrate_count < 4 &&
		     data.m_enter_trace.m_hit_entity == entity )
			return cache.m_result = true;
	}

	return false;
}

void n_players::impl_t::on_player_hurt( const c_vector& origin, const int victim, const int damage )
{
	if ( !GET_VARIABLE( g_variables.m_players, bool ) || !GET_VARIABLE( g_variables.m_damage_numbers, bool ) || damage <= 0 )
		return;

	const float current_time = g_interfaces.m_global_vars_base->m_real_time;

	if ( GET_VARIABLE( g_variables.m_damage_numbers_stack, bool ) ) {
		for ( auto& entry : this->m_damage_numbers ) {
			if ( entry.m_victim != victim || current_time - entry.m_spawn_time > 0.35f )
				continue;

			entry.m_damage += damage;
			entry.m_spawn_time = current_time;
			return;
		}
	}

	if ( this->m_damage_numbers.size( ) >= 32 )
		this->m_damage_numbers.erase( this->m_damage_numbers.begin( ) );

	this->m_damage_numbers.push_back( { origin, damage, victim, current_time } );
}

void n_players::impl_t::damage_numbers( )
{
	if ( this->m_damage_numbers.empty( ) )
		return;

	if ( !GET_VARIABLE( g_variables.m_damage_numbers, bool ) ) {
		this->m_damage_numbers.clear( );
		return;
	}

	const float duration = GET_VARIABLE( g_variables.m_damage_numbers_duration, float );
	const float rise     = GET_VARIABLE( g_variables.m_damage_numbers_speed, float );
	const auto color     = GET_VARIABLE( g_variables.m_damage_numbers_color, c_color );

	const float current_time = g_interfaces.m_global_vars_base->m_real_time;
	const auto font          = g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_esp ]
	                      ? g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_esp ]
	                      : g_render.m_fonts[ e_font_names::font_name_verdana_11 ];

	if ( !font )
		return;

	for ( auto it = this->m_damage_numbers.begin( ); it != this->m_damage_numbers.end( ); ) {
		const float elapsed = current_time - it->m_spawn_time;

		if ( elapsed < 0.f || elapsed >= duration || duration <= 0.f ) {
			it = this->m_damage_numbers.erase( it );
			continue;
		}

		c_vector position = it->m_origin;
		position.m_z += elapsed * rise;

		c_vector_2d out = { };
		if ( !g_render.world_to_screen( position, out ) ) {
			++it;
			continue;
		}

		const float fraction = elapsed / duration;
		const float alpha    = fraction < 0.7f ? 1.f : 1.f - ( fraction - 0.7f ) / 0.3f;

		const std::string text = std::to_string( it->m_damage );
		const auto text_size   = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, text.c_str( ) );

		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_text,
			std::make_any< text_draw_object_t >( font, c_vector_2d( out.m_x - text_size.x * 0.5f, out.m_y - text_size.y * 0.5f ), text,
		                                         color.get_u32( alpha ), c_color( 0.f, 0.f, 0.f, 1.f ).get_u32( alpha ),
		                                         e_text_flags::text_flag_dropshadow ) );

		++it;
	}
}

void n_players::impl_t::players( )
{
	c_angle view_angles = { };
	g_interfaces.m_engine_client->get_view_angles( view_angles );

	const float yaw = deg2rad( view_angles.m_y );

	const auto display_size = ImVec2( g_ctx.m_width, g_ctx.m_height );

	const bool  draw_teammates = GET_VARIABLE( g_variables.m_players_teammates, bool );
	const float max_distance   = GET_VARIABLE( g_variables.m_players_max_distance, float );
	const bool  distance_fade  = GET_VARIABLE( g_variables.m_players_distance_fade, bool );

	g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
		if ( !entity || entity == g_ctx.m_local || !entity->is_alive( ) )
			return;

		if ( !g_ctx.m_local->is_enemy( entity ) && !draw_teammates )
			return;

		const auto index = entity->get_index( );

		/* dormant + botox network position: drawn at it, bone-based parts skipped (bones are stale).
		   origin / health put back when this player's draw ends, nothing else sees peer data */
		struct restore_t {
			c_base_entity* m_entity = nullptr;
			c_vector m_origin       = { };
			int m_health            = 0;

			~restore_t( )
			{
				if ( !m_entity )
					return;

				m_entity->set_abs_origin( m_origin );
				m_entity->get_health( ) = m_health;
			}
		} restore{ };

		bool shared = false;

		if ( entity->is_dormant( ) ) {
			c_vector shared_origin{ };
			int shared_health = 0;

			if ( !g_botox_net.shared_player( index, shared_origin, shared_health ) ) {
				this->m_fading_alpha[ index ] = 0.f;
				g_dormancy.m_sound_players[ index ].reset( );
				return;
			}

			restore.m_origin = entity->get_abs_origin( );
			restore.m_health = entity->get_health( );
			restore.m_entity = entity;

			entity->set_abs_origin( shared_origin );
			entity->get_health( ) = shared_health;
			shared                = true;
		} else
			g_dormancy.m_sound_players[ index ].reset( true, entity->get_abs_origin( ), entity->get_flags( ) );

		const float distance = g_ctx.m_local->get_abs_origin( ).dist_to( entity->get_abs_origin( ) );

		if ( max_distance > 0.f && distance > max_distance ) {
			this->m_fading_alpha[ index ] = 0.f;
			return;
		}

		float alpha = 1.f;

		if ( distance_fade && max_distance > 0.f ) {
			const float fade_start = max_distance * 0.75f;

			if ( distance > fade_start )
				alpha = 1.f - ( distance - fade_start ) / ( max_distance - fade_start );
		}

		alpha *= shared ? 1.f : g_sound_esp.player_gate( entity );

		if ( alpha <= 0.f ) {
			this->m_fading_alpha[ index ] = 0.f;
			return;
		}

		if ( this->m_fading_alpha[ index ] < 1.f )
			m_stored_cur_time[ index ] = g_interfaces.m_global_vars_base->m_current_time;

		this->m_fading_alpha[ index ] = alpha;

		const auto client_renderable = entity->get_client_renderable( );
		if ( !client_renderable )
			return;

		const auto client_unknown = client_renderable->get_client_unknown( );
		if ( !client_unknown )
			return;

		const auto collideable = client_unknown->get_collideable( );
		if ( !collideable )
			return;

		const auto client_entity = client_unknown->get_client_entity( );
		if ( !client_entity )
			return;

		if ( this->m_fading_alpha[ index ] >= 1.0f )
			this->m_fading_alpha[ index ] = 1.0f;

		if ( this->m_fading_alpha[ index ] <= 0.0f ) {
			this->m_fading_alpha[ index ] = 0.0f;
			return;
		}

		const bool in_view_frustrum = !g_interfaces.m_engine_client->cull_box( collideable->get_obb_mins( ) + client_entity->get_abs_origin( ),
		                                                                       collideable->get_obb_maxs( ) + client_entity->get_abs_origin( ) );

		const auto client_networkable = client_unknown->get_client_networkable( );
		if ( !client_networkable )
			return;

		[ & ]( ) {
			if ( !GET_VARIABLE( g_variables.m_out_of_fov_arrows, bool ) || in_view_frustrum ||
			     GET_VARIABLE( g_variables.m_out_of_fov_arrows_type, int ) == 1 )
				return;

			const c_vector position_difference = g_ctx.m_local->get_abs_origin( ) - client_entity->get_abs_origin( );

			float x = std::cos( yaw ) * position_difference.m_y - std::sin( yaw ) * position_difference.m_x;
			float y = std::cos( yaw ) * position_difference.m_x + std::sin( yaw ) * position_difference.m_y;
			if ( const auto len = std::sqrt( x * x + y * y ); len ) {
				x /= len;
				y /= len;
			}

			// off-frustum = bones stale, so trace to the networked eye instead of a hitbox
			const bool behind_wall = [ & ]( ) {
				trace_t trace{ };
				c_trace_filter filter( g_ctx.m_local );

				ray_t ray( g_ctx.m_local->get_eye_position( false ), client_entity->get_abs_origin( ) + entity->get_view_offset( ) );
				g_interfaces.m_engine_trace->trace_ray( ray, mask_shot | contents_grate, &filter, &trace );

				return !( trace.m_hit_entity == entity || trace.m_fraction > 0.97f );
			}( );

			const unsigned int arrow_color =
				( behind_wall ? GET_VARIABLE( g_variables.m_out_of_fov_arrows_wall_color, c_color )
				              : GET_VARIABLE( g_variables.m_out_of_fov_arrows_color, c_color ) )
					.get_u32( this->m_fading_alpha[ index ] );

			const float arrow_width  = GET_VARIABLE( g_variables.m_out_of_fov_arrows_width, float );
			const float arrow_height = GET_VARIABLE( g_variables.m_out_of_fov_arrows_height, float );
			const float ring_radius  = static_cast< float >( GET_VARIABLE( g_variables.m_out_of_fov_arrows_distance, int ) );

			float radius_x = ring_radius;
			const float radius_y = ring_radius;
			if ( GET_VARIABLE( g_variables.m_out_of_fov_arrows_aspect, bool ) && display_size.y > 0.f )
				radius_x = ring_radius * ( display_size.x / display_size.y );

			const c_vector_2d position = c_vector_2d( display_size.x * 0.5f + x * radius_x, display_size.y * 0.5f + y * radius_y );

			if ( GET_VARIABLE( g_variables.m_out_of_fov_arrows_avatar, bool ) ) {
				player_info_t arrow_player_info{ };
				IDirect3DTexture9* avatar = nullptr;

				if ( g_interfaces.m_engine_client->get_player_info( index, &arrow_player_info ) ) {
					const int arrow_team = entity->get_team( );

					avatar = arrow_player_info.m_fake_player ? arrow_team == e_team_id::team_tt   ? g_render.m_terrorist_avatar
					                                           : arrow_team == e_team_id::team_ct ? g_render.m_counter_terrorist_avatar
					                                                                              : nullptr
					                                         : g_avatar_cache[ index ];
				}

				if ( avatar ) {
					const float avatar_size   = arrow_width < arrow_height ? arrow_width : arrow_height;
					const float avatar_radius = avatar_size * 0.5f;

					g_render.m_draw_data.emplace_back(
						e_draw_type::draw_type_texture,
						std::make_any< texture_draw_object_t >( c_vector_2d( position.m_x - avatar_radius, position.m_y - avatar_radius ),
						                                        c_vector_2d( avatar_size, avatar_size ),
						                                        ImColor( 1.f, 1.f, 1.f, this->m_fading_alpha[ index ] ), avatar, avatar_radius,
						                                        ImDrawFlags_::ImDrawFlags_RoundCornersAll ) );

					g_render.m_draw_data.emplace_back(
						e_draw_type::draw_type_circle,
						std::make_any< circle_draw_object_t >( position, avatar_radius, arrow_color, 32, 1.5f ) );
					return;
				}
			}

			push_ring_arrow( position, x, y, arrow_width, arrow_height, arrow_color );
		}( );

		bounding_box_t box{ };
		if ( !entity->get_bounding_box( &box ) )
			return;

		player_info_t player_info{ };
		if ( !g_interfaces.m_engine_client->get_player_info( index, &player_info ) )
			return;

		std::string hp_text = std::to_string( entity->get_health( ) );
		if ( GET_VARIABLE( g_variables.m_players_health_suffix, bool ) )
			hp_text.append( "hp" );

		const int team = entity->get_team( );

		if ( this->m_backup_player_data[ index ].m_animated_health > entity->get_health( ) )
			this->m_backup_player_data[ index ].m_animated_health -=
				( 100.f * g_interfaces.m_global_vars_base->m_frame_time ) ;
		else
			this->m_backup_player_data[ index ].m_animated_health = entity->get_health( );

		const float factor = static_cast< float >( this->m_backup_player_data[ index ].m_animated_health ) / entity->get_max_health( );
		const float hue    = ( factor * 120.f ) / 360.f;

		float padding[ e_padding_direction::padding_direction_max ] = { 0.f, 0.f, 0.f, 0.f };

		bool visibility_traced = false, player_visible = false;

		const auto is_player_visible = [ & ]( ) -> bool {
			if ( visibility_traced )
				return player_visible;

			visibility_traced = true;

			trace_t trace{ };
			c_trace_filter filter( g_ctx.m_local );

			ray_t ray( g_ctx.m_local->get_eye_position( false ), client_entity->get_abs_origin( ) + entity->get_view_offset( ) );
			g_interfaces.m_engine_trace->trace_ray( ray, mask_shot | contents_grate, &filter, &trace );

			player_visible = trace.m_hit_entity == entity || trace.m_fraction > 0.97f;

			return player_visible;
		};

		float box_rounding = GET_VARIABLE( g_variables.m_players_box, bool )
		                         ? std::max( GET_VARIABLE( g_variables.m_players_box_rounding, float ), 0.f ) *
		                               std::min( static_cast< float >( box.m_bottom - box.m_top ) / 200.f, 1.f )
		                         : 0.f;

		if ( GET_VARIABLE( g_variables.m_players_box_corner, bool ) )
			box_rounding = std::min( box_rounding, static_cast< float >( std::min( static_cast< int >( box.m_right - box.m_left ) / 3,
			                                                                        static_cast< int >( box.m_bottom - box.m_top ) / 5 ) ) );

		if ( GET_VARIABLE( g_variables.m_players_box_fill, bool ) ) {
			const auto top = GET_VARIABLE( g_variables.m_players_box_fill_color, c_color );

			if ( GET_VARIABLE( g_variables.m_players_box_fill_gradient, bool ) ) {
				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_gradient_rect,
					std::make_any< gradient_rect_draw_object_t >(
						c_vector_2d( box.m_left, box.m_top ), c_vector_2d( box.m_right, box.m_bottom ), top.get_u32( this->m_fading_alpha[ index ] ),
						GET_VARIABLE( g_variables.m_players_box_fill_bottom_color, c_color ).get_u32( this->m_fading_alpha[ index ] ), box_rounding ) );
			} else {
				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_rect,
					std::make_any< rect_draw_object_t >( c_vector_2d( box.m_left, box.m_top ), c_vector_2d( box.m_right, box.m_bottom ),
				                                         top.get_u32( this->m_fading_alpha[ index ] ), ImColor( ), true, box_rounding,
				                                         ImDrawFlags_::ImDrawFlags_None, 1.f, e_rect_flags::rect_flag_none ) );
			}
		}

		if ( GET_VARIABLE( g_variables.m_players_box, bool ) ) {
			const c_color box_color = GET_VARIABLE( g_variables.m_players_box_visibility_colors, bool )
			                              ? ( is_player_visible( ) ? GET_VARIABLE( g_variables.m_players_box_visible_color, c_color )
			                                                       : GET_VARIABLE( g_variables.m_players_box_invisible_color, c_color ) )
			                              : GET_VARIABLE( g_variables.m_players_box_color, c_color );

			const unsigned int outline_flags = [ ]( ) -> unsigned int {
				if ( !GET_VARIABLE( g_variables.m_players_box_outline, bool ) )
					return e_rect_flags::rect_flag_none;

				switch ( GET_VARIABLE( g_variables.m_players_box_outline_side, int ) ) {
				case e_box_outline_side::box_outline_side_inside:
					return e_rect_flags::rect_flag_inner_outline;
				case e_box_outline_side::box_outline_side_outside:
					return e_rect_flags::rect_flag_outer_outline;
				default:
					return e_rect_flags::rect_flag_inner_outline | e_rect_flags::rect_flag_outer_outline;
				}
			}( );

			const float outline_thickness = std::max( GET_VARIABLE( g_variables.m_players_box_outline_thickness, float ), 1.f );

			const float box_thickness = std::clamp(
				GET_VARIABLE( g_variables.m_players_box_thickness, float ), 1.f,
				std::max( std::floor( static_cast< float >( std::min( box.m_right - box.m_left, box.m_bottom - box.m_top ) ) / 4.f ), 1.f ) );

			if ( GET_VARIABLE( g_variables.m_players_box_3d, bool ) ) {
				const auto collideable = client_unknown->get_collideable( );
				if ( collideable ) {
					const c_vector mins = collideable->get_obb_mins( ), maxs = collideable->get_obb_maxs( );
					const matrix3x4_t& frame = entity->get_coordinate_frame( );

					std::array< c_vector_2d, 8U > corners{ };
					bool on_screen = true;
					for ( std::size_t i = 0U; i < 8U && on_screen; i++ ) {
						const c_vector point( ( i & 1U ) ? maxs.m_x : mins.m_x, ( i & 2U ) ? maxs.m_y : mins.m_y, ( i & 4U ) ? maxs.m_z : mins.m_z );
						on_screen = g_render.world_to_screen( g_math.vector_transform( point, frame ), corners[ i ] );
					}

					if ( on_screen ) {
						constexpr std::size_t edges[ 12 ][ 2 ] = { { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 }, { 4, 5 }, { 5, 7 },
							                                       { 7, 6 }, { 6, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };

						if ( outline_flags != e_rect_flags::rect_flag_none ) {
							const unsigned int outline_color =
								GET_VARIABLE( g_variables.m_players_box_outline_color, c_color ).get_u32( this->m_fading_alpha[ index ] );
							for ( const auto& edge : edges )
								g_render.m_draw_data.emplace_back(
									e_draw_type::draw_type_line,
									std::make_any< line_draw_object_t >( line_draw_object_t{ corners[ edge[ 0 ] ], corners[ edge[ 1 ] ], outline_color,
								                                                             box_thickness + outline_thickness * 2.f } ) );
						}

						const unsigned int line_color = box_color.get_u32( this->m_fading_alpha[ index ] );
						for ( const auto& edge : edges )
							g_render.m_draw_data.emplace_back(
								e_draw_type::draw_type_line,
								std::make_any< line_draw_object_t >( line_draw_object_t{ corners[ edge[ 0 ] ], corners[ edge[ 1 ] ], line_color, box_thickness } ) );
					}
				}
			} else if ( !GET_VARIABLE( g_variables.m_players_box_corner, bool ) )
				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_rect,
					std::make_any< rect_draw_object_t >(
						c_vector_2d( box.m_left, box.m_top ), c_vector_2d( box.m_right, box.m_bottom ),
						box_color.get_u32( this->m_fading_alpha[ index ] ),
						GET_VARIABLE( g_variables.m_players_box_outline_color, c_color ).get_u32( this->m_fading_alpha[ index ] ), false,
						box_rounding, ImDrawFlags_::ImDrawFlags_None, box_thickness, outline_flags, outline_thickness ) );
			else {
				const float inset = ( box_thickness - 1.f ) * 0.5f, corner_rounding = std::max( box_rounding - inset, 0.f );

				if ( outline_flags != e_rect_flags::rect_flag_none ) {
					g_render.corner_rect(
						box.m_left + inset, box.m_top + inset, box.m_right - inset, box.m_bottom - inset,
						c_color( GET_VARIABLE( g_variables.m_players_box_outline_color, c_color ) ).get_u32( this->m_fading_alpha[ index ] ),
						box_thickness + outline_thickness, corner_rounding );
				}

				g_render.corner_rect( box.m_left + inset, box.m_top + inset, box.m_right - inset, box.m_bottom - inset,
				                      box_color.get_u32( this->m_fading_alpha[ index ] ), box_thickness, corner_rounding );
			}
		}

		float name_right = 0.f, name_top = 0.f;
		bool name_drawn = false;

		if ( GET_VARIABLE( g_variables.m_players_name, bool ) ) {
			const std::string full_name = player_info.m_name;
			std::string converted_name  = g_utilities.truncate_utf8( full_name, 24U );

			if ( converted_name.length( ) < full_name.length( ) )
				converted_name.append( "..." );

			if ( player_info.m_fake_player )
				converted_name.insert( 0, "[bot] " );

			const auto name_font = g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_esp ];

			const auto text_size = name_font->CalcTextSizeA( name_font->FontSize, FLT_MAX, 0.f, converted_name.c_str( ) );

			const int name_position = GET_VARIABLE( g_variables.m_players_name_position, int );

			const float name_x = box.m_left + box.m_width * 0.5f - text_size.x * 0.5f;

			const float name_y = [ & ]( ) -> float {
				switch ( name_position ) {
				case e_name_position::name_position_below:
					return box.m_bottom + 2 + padding[ e_padding_direction::padding_direction_bottom ];
				case e_name_position::name_position_inside_top:
					return box.m_top + 2;
				case e_name_position::name_position_inside_bottom:
					return box.m_bottom - 2 - text_size.y;
				default:
					return box.m_top - 3 - text_size.y;
				}
			}( );

			const c_color name_color = GET_VARIABLE( g_variables.m_players_name_visibility_colors, bool )
			                               ? ( is_player_visible( ) ? GET_VARIABLE( g_variables.m_players_name_visible_color, c_color )
			                                                        : GET_VARIABLE( g_variables.m_players_name_invisible_color, c_color ) )
			                               : GET_VARIABLE( g_variables.m_players_name_color, c_color );

			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_text,
				std::make_any< text_draw_object_t >( name_font, c_vector_2d( name_x, name_y ), converted_name,
			                                         name_color.get_u32( this->m_fading_alpha[ index ] ),
			                                         c_color( 0.f, 0.f, 0.f, 1.f ).get_u32( this->m_fading_alpha[ index ] ),
			                                         e_text_flags::text_flag_dropshadow ) );

			if ( GET_VARIABLE( g_variables.m_players_avatar, bool ) &&
			     GET_VARIABLE( g_variables.m_players_avatar_position, int ) == e_avatar_position::avatar_position_name ) {
				const float avatar_size = GET_VARIABLE( g_variables.m_players_avatar_size, float );

				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_texture,
					std::make_any< texture_draw_object_t >(
						c_vector_2d( name_x - avatar_size - 3.f, name_y + ( text_size.y - avatar_size ) * 0.5f ),
						c_vector_2d( avatar_size, avatar_size ),
						ImColor( 1.f, 1.f, 1.f, this->m_fading_alpha[ index ] ),
						player_info.m_fake_player ? team == e_team_id::team_tt   ? g_render.m_terrorist_avatar
						                            : team == e_team_id::team_ct ? g_render.m_counter_terrorist_avatar
						                                                         : nullptr
						                          : g_avatar_cache[ index ],
						avatar_size * 0.5f, ImDrawFlags_::ImDrawFlags_RoundCornersAll ) );
			}

			if ( name_position == e_name_position::name_position_above )
				padding[ e_padding_direction::padding_direction_top ] -= text_size.y - 20;
			else if ( name_position == e_name_position::name_position_below )
				padding[ e_padding_direction::padding_direction_bottom ] += ( text_size.y + 1.f );

			name_right = name_x + text_size.x;
			name_top   = name_y;
			name_drawn = true;
		}

		if ( entity->has_immunity( ) || this->is_god( index ) ) {
			const auto god_font = g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_esp ];
			const auto text     = "(GOD)";
			const auto text_size = god_font->CalcTextSizeA( god_font->FontSize, FLT_MAX, 0.f, text );

			const float god_x = name_drawn ? name_right + 3.f : box.m_left + box.m_width * 0.5f - text_size.x * 0.5f;
			const float god_y = name_drawn ? name_top : box.m_top - 3 - text_size.y;

			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_text,
				std::make_any< text_draw_object_t >( god_font, c_vector_2d( god_x, god_y ), text,
			                                         c_color( 1.f, 0.35f, 0.35f ).get_u32( this->m_fading_alpha[ index ] ),
			                                         c_color( 0.f, 0.f, 0.f, 1.f ).get_u32( this->m_fading_alpha[ index ] ),
			                                         e_text_flags::text_flag_dropshadow ) );

			if ( !name_drawn )
				padding[ e_padding_direction::padding_direction_top ] -= text_size.y - 20;
		}

		const bool health_text_custom = GET_VARIABLE( g_variables.m_players_health_text_custom_color, bool );

		const auto health_text_color = [ & ]( const c_color& fallback ) -> c_color {
			return health_text_custom ? GET_VARIABLE( g_variables.m_players_health_text_color, c_color ) : fallback;
		};

		const auto health_font = g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_esp ]
		                             ? g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_esp ]
		                             : g_render.m_fonts[ e_font_names::font_name_verdana_11 ];

		if ( GET_VARIABLE( g_variables.m_players_health_text, bool ) && GET_VARIABLE( g_variables.m_players_health_text_style, int ) == 0 &&
		     health_font ) {
			const auto text_size = health_font->CalcTextSizeA( health_font->FontSize, FLT_MAX, 0.f, hp_text.c_str( ) );

			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_text,
				std::make_any< text_draw_object_t >( health_font,
			                                         c_vector_2d( box.m_left + ( box.m_width - text_size.x ) / 2,
			                                                      box.m_bottom + 2 + padding[ e_padding_direction::padding_direction_bottom ] ),
			                                         hp_text,
			                                         health_text_color( c_color::from_hsb( hue, 1.f, 1.f, 1.f ) )
			                                             .get_u32( this->m_fading_alpha[ index ] ),
			                                         ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ), text_flag_dropshadow ) );

			padding[ e_padding_direction::padding_direction_bottom ] += ( text_size.y + 1.f );
		}

		const auto active_weapon = reinterpret_cast< c_base_entity* >(
			g_interfaces.m_client_entity_list->get_client_entity_from_handle( entity->get_active_weapon_handle( ) ) );
		this->m_backup_player_data[ index ].m_active_weapon = active_weapon;

		if ( this->m_backup_player_data[ index ].m_active_weapon ) {
			const auto item_definition_index = this->m_backup_player_data[ index ].m_active_weapon->get_item_definition_index( );
			const auto weapon_data           = g_interfaces.m_weapon_system->get_weapon_data( item_definition_index );

			if ( weapon_data )
				this->m_backup_player_data[ index ].m_weapon_data = weapon_data;

			if ( this->m_backup_player_data[ index ].m_weapon_data ) {
				[ & ]( ) {
					if ( GET_VARIABLE( g_variables.m_player_ammo_bar, bool ) ) {
						if ( !this->m_backup_player_data[ index ].m_weapon_data->is_gun( ) )
							return;

						const float ammo = static_cast< float >( this->m_backup_player_data[ index ].m_active_weapon->get_ammo( ) );

						if ( this->m_backup_player_data[ index ].m_ammo > ammo )
							this->m_backup_player_data[ index ].m_ammo -= ( 2.f * g_interfaces.m_global_vars_base->m_frame_time );
						else
							this->m_backup_player_data[ index ].m_ammo = ammo;

						const int max_ammo = this->m_backup_player_data[ index ].m_weapon_data->m_max_clip1;

						float factor = this->m_backup_player_data[ index ].m_ammo / static_cast< float >( max_ammo );
						factor       = std::clamp< float >( factor, 0.f, 1.f );

						g_render.m_draw_data.emplace_back(
							e_draw_type::draw_type_rect,
							std::make_any< rect_draw_object_t >(
								c_vector_2d( box.m_left, box.m_bottom + 3.f ), c_vector_2d( box.m_left + box.m_width, box.m_bottom + 5.f ),
								ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ), ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ),
								false, 0.f, ImDrawFlags_::ImDrawFlags_None, 1.f, rect_flag_outer_outline ) );

						g_render.m_draw_data.emplace_back(
							e_draw_type::draw_type_rect,
							std::make_any< rect_draw_object_t >(
								c_vector_2d( box.m_left, box.m_bottom + 3.f ), c_vector_2d( box.m_left + box.m_width * factor, box.m_bottom + 5.f ),
								GET_VARIABLE( g_variables.m_player_ammo_bar_color, c_color ).get_u32( this->m_fading_alpha[ index ] ),
								ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ), false, 0.f, ImDrawFlags_::ImDrawFlags_None, 1.f,
								rect_flag_outer_outline ) );

						padding[ e_padding_direction::padding_direction_bottom ] += 6.f;
					};
				}( );

				[ & ]( ) {
					if ( GET_VARIABLE( g_variables.m_weapon_name, bool ) ) {
						const auto localized_name = g_interfaces.m_localize->find( this->m_backup_player_data[ index ].m_weapon_data->m_hud_name );

						std::wstring w = localized_name;
						if ( w.empty( ) )
							return;

						std::transform( w.begin( ), w.end( ), w.begin( ), ::towlower );

						const std::string converted_name( w.begin( ), w.end( ) );
						if ( converted_name.empty( ) )
							return;

						const auto weapon_font = g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_esp ];

						const auto text_size = weapon_font->CalcTextSizeA( weapon_font->FontSize, FLT_MAX, 0.f, converted_name.c_str( ) );

						g_render.m_draw_data.emplace_back(
							e_draw_type::draw_type_text,
							std::make_any< text_draw_object_t >(
								weapon_font, c_vector_2d( box.m_left + ( box.m_width - text_size.x ) / 2,
						                     box.m_bottom + 2 + padding[ e_padding_direction::padding_direction_bottom ] ),
								converted_name, GET_VARIABLE( g_variables.m_weapon_name_color, c_color ).get_u32( this->m_fading_alpha[ index ] ),
								ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ), text_flag_dropshadow ) );

						padding[ e_padding_direction::padding_direction_bottom ] += ( text_size.y + 1.f );
					}
				}( );

				if ( GET_VARIABLE( g_variables.m_weapon_icon, bool ) ) {
					if ( !( item_definition_index == e_item_definition_index::weapon_shield ||
					        item_definition_index == e_item_definition_index::weapon_breachcharge ||
					        item_definition_index == e_item_definition_index::weapon_bumpmine ) ) {
						const auto text      = reinterpret_cast< const char* >( g_utilities.get_weapon_icon( item_definition_index ) );
						const auto text_size = g_render.m_fonts[ e_font_names::font_name_icon_12 ]->CalcTextSizeA(
							g_render.m_fonts[ e_font_names::font_name_icon_12 ]->FontSize, FLT_MAX, 0.f, text );

						g_render.m_draw_data.emplace_back(
							e_draw_type::draw_type_text,
							std::make_any< text_draw_object_t >(
								g_render.m_fonts[ e_font_names::font_name_icon_12 ],
								c_vector_2d( box.m_left + ( box.m_width - text_size.x ) / 2,
						                     box.m_bottom + 2 + padding[ e_padding_direction::padding_direction_bottom ] ),
								text, GET_VARIABLE( g_variables.m_weapon_icon_color, c_color ).get_u32( this->m_fading_alpha[ index ] ),
								ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ), text_flag_dropshadow ) );

						padding[ e_padding_direction::padding_direction_bottom ] += text_size.y;
					}
				}
			}
		}

		if ( GET_VARIABLE( g_variables.m_players_distance, bool ) && health_font ) {
			char distance_text[ 16 ] = { };

			switch ( GET_VARIABLE( g_variables.m_players_distance_unit, int ) ) {
			case e_distance_unit::distance_unit_units:
				std::snprintf( distance_text, sizeof( distance_text ), "%.0fu", distance );
				break;
			case e_distance_unit::distance_unit_feet:
				std::snprintf( distance_text, sizeof( distance_text ), "%.0fft", distance / 16.f );
				break;
			default:
				std::snprintf( distance_text, sizeof( distance_text ), "%.0fm", distance / 52.49f );
				break;
			}

			const auto text_size = health_font->CalcTextSizeA( health_font->FontSize, FLT_MAX, 0.f, distance_text );

			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_text,
				std::make_any< text_draw_object_t >( health_font,
			                                         c_vector_2d( box.m_left + ( box.m_width - text_size.x ) * 0.5f,
			                                                      box.m_bottom + 2 + padding[ e_padding_direction::padding_direction_bottom ] ),
			                                         distance_text,
			                                         GET_VARIABLE( g_variables.m_players_distance_color, c_color )
			                                             .get_u32( this->m_fading_alpha[ index ] ),
			                                         ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ), text_flag_dropshadow ) );

			padding[ e_padding_direction::padding_direction_bottom ] += ( text_size.y + 1.f );
		}

		if ( GET_VARIABLE( g_variables.m_players_flags, bool ) && health_font ) {
			const auto& enabled_flags = GET_VARIABLE( g_variables.m_player_flags, std::vector< bool > );

			const auto flag_enabled = [ & ]( const int flag ) -> bool {
				return flag < static_cast< int >( enabled_flags.size( ) ) && enabled_flags[ flag ];
			};

			const bool carrying_bomb = [ & ]( ) -> bool {
				if ( !flag_enabled( e_player_flags::player_flag_bomb ) )
					return false;

				const auto weapons = entity->get_weapons_handle( );
				if ( !weapons )
					return false;

				for ( int i = 0; i < 64; i++ ) {
					const auto handle = weapons[ i ];
					if ( !handle || handle == 0xFFFFFFFF )
						continue;

					const auto weapon = reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( handle ) );
					if ( weapon && weapon->get_item_definition_index( ) == e_item_definition_index::weapon_c4 )
						return true;
				}

				return false;
			}( );

			char money_text[ 16 ] = { };
			std::snprintf( money_text, sizeof( money_text ), "$%i", entity->get_money( ) );

			const c_color plain = GET_VARIABLE( g_variables.m_players_flags_color, c_color );
			const c_color alert = GET_VARIABLE( g_variables.m_players_flags_alert_color, c_color );

			const struct {
				const char* m_label;
				bool m_shown;
				bool m_alert;
			} rows[ e_player_flags::player_flags_max ] = {
				{ money_text, entity->get_money( ) > 0, false },
				{ "armor", entity->get_armor( ) > 0, false },
				{ "helmet", entity->has_helmet( ), false },
				{ "kit", entity->has_defuser( ), false },
				{ "defusing", entity->is_defusing( ), true },
				{ "walking", entity->is_walking( ), false },
				{ "scoped", entity->is_scoped( ), false },
				{ "flashed", entity->get_flash_duration( ) > 0.f, true },
				{ "bomb", carrying_bomb, true },
				{ "hostage", entity->is_grabbing_hostage( ), false },
				{ "immune", entity->has_immunity( ) || this->is_god( index ), false },
				{ "wallbangable", flag_enabled( e_player_flags::player_flag_wallbangable ) && !shared && this->wallbangable( entity, index ), true },
			};

			float flag_y = box.m_top;

			for ( int flag = 0; flag < e_player_flags::player_flags_max; flag++ ) {
				if ( !flag_enabled( flag ) || !rows[ flag ].m_shown )
					continue;

				const auto text_size = health_font->CalcTextSizeA( health_font->FontSize, FLT_MAX, 0.f, rows[ flag ].m_label );

				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_text,
					std::make_any< text_draw_object_t >( health_font, c_vector_2d( box.m_right + 4.f, flag_y ), rows[ flag ].m_label,
				                                         ( rows[ flag ].m_alert ? alert : plain ).get_u32( this->m_fading_alpha[ index ] ),
				                                         ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ), text_flag_dropshadow ) );

				flag_y += text_size.y + 1.f;
			}
		}

		if ( GET_VARIABLE( g_variables.m_players_health_bar, bool ) &&
		     GET_VARIABLE( g_variables.m_players_health_bar_side, int ) == e_bar_side::bar_side_corner ) {
			const float bar_thickness = GET_VARIABLE( g_variables.m_players_health_bar_thickness, float );
			const bool  bar_outline   = GET_VARIABLE( g_variables.m_players_health_bar_outline, bool );
			const bool  box_on        = GET_VARIABLE( g_variables.m_players_box, bool );

			const float corner_size = std::clamp( GET_VARIABLE( g_variables.m_players_health_bar_corner_size, float ), 0.1f, 1.f );
			const int   box_w = box.m_right - box.m_left, box_h = box.m_bottom - box.m_top;

			const float even_arm = box_on ? static_cast< float >( std::min( box_w / 3, box_h / 5 ) )
			                              : static_cast< float >( std::min( box_w, box_h ) ) * corner_size;
			const bool  ride_box = box_on && GET_VARIABLE( g_variables.m_players_box_corner, bool ) && !GET_VARIABLE( g_variables.m_players_box_3d, bool );
			const float arm_x    = ride_box ? static_cast< float >( box_w / 3 ) : even_arm;
			const float arm_y    = ride_box ? static_cast< float >( box_h / 5 ) : even_arm;

			const float off = 3.f + bar_thickness * 0.5f;

			const float base_rounding = box_on ? box_rounding
			                                   : std::max( GET_VARIABLE( g_variables.m_players_health_bar_corner_rounding, float ), 0.f ) *
			                                         std::min( static_cast< float >( box.m_bottom - box.m_top ) / 200.f, 1.f );

			const float radius = base_rounding > 0.f ? std::min( base_rounding, std::min( arm_x, arm_y ) ) + off : 0.f;

			const float cx = static_cast< float >( box.m_left ) - off, cy = static_cast< float >( box.m_bottom ) + off;

			const int  corner_pick = std::clamp( GET_VARIABLE( g_variables.m_players_health_bar_corner_pick, int ), 0, 3 );
			const bool flip_x = corner_pick & 1, flip_y = corner_pick >= 2;
			const auto place  = [ & ]( ImVec2 p ) {
				if ( flip_x )
					p.x = static_cast< float >( box.m_left + box.m_right ) - p.x;
				if ( flip_y )
					p.y = static_cast< float >( box.m_top + box.m_bottom ) - p.y;
				return p;
			};

			const auto build_path = [ & ]( const float ext ) {
				std::vector< ImVec2 > path{ ImVec2( cx, static_cast< float >( box.m_bottom ) - arm_y - ext ) };

				const auto push = [ &path ]( const ImVec2& p ) {
					if ( std::abs( p.x - path.back( ).x ) + std::abs( p.y - path.back( ).y ) > 0.01f )
						path.push_back( p );
				};

				const int segments = radius > 0.f ? 12 : 0;
				for ( int i = 0; i <= segments; i++ ) {
					const float angle = 3.14159265f - 1.57079633f * ( segments ? static_cast< float >( i ) / segments : 0.f );
					push( ImVec2( cx + radius + radius * std::cos( angle ), cy - radius + radius * std::sin( angle ) ) );
				}

				push( ImVec2( static_cast< float >( box.m_left ) + arm_x + ext, cy ) );
				for ( auto& p : path )
					p = place( p );
				return path;
			};

			const std::vector< ImVec2 > track = build_path( 0.f ), outer = build_path( 1.f );

			std::vector< ImVec2 > fill{ track.back( ) };
			{
				float total = 0.f;
				for ( std::size_t i = 1; i < track.size( ); i++ )
					total += std::hypot( track[ i ].x - track[ i - 1 ].x, track[ i ].y - track[ i - 1 ].y );

				float keep = total * std::clamp( factor, 0.f, 1.f );
				for ( std::size_t i = track.size( ) - 1; i > 0 && keep > 0.f; i-- ) {
					const ImVec2 a = track[ i ], b = track[ i - 1 ];
					const float segment = std::hypot( b.x - a.x, b.y - a.y );

					if ( segment >= keep ) {
						fill.emplace_back( a.x + ( b.x - a.x ) * ( keep / segment ), a.y + ( b.y - a.y ) * ( keep / segment ) );
						break;
					}

					fill.push_back( b );
					keep -= segment;
				}
			}

			const ImU32 outline_color = ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] );
			const ImU32 bg_color      = GET_VARIABLE( g_variables.m_players_health_bar_bg_color, c_color ).get_u32( this->m_fading_alpha[ index ] );
			const ImU32 fill_color    = GET_VARIABLE( g_variables.m_players_health_bar_custom_color, bool )
			                                ? GET_VARIABLE( g_variables.m_players_health_bar_color, c_color ).get_u32( this->m_fading_alpha[ index ] )
			                                : c_color::from_hsb( hue, 1.f, 1.f, 1.f ).get_u32( this->m_fading_alpha[ index ] );
			const bool  gradient      = GET_VARIABLE( g_variables.m_players_health_bar_custom_color, bool ) &&
			                       GET_VARIABLE( g_variables.m_players_health_bar_gradient, bool );
			const ImU32 bottom_color = GET_VARIABLE( g_variables.m_players_health_bar_bottom_color, c_color ).get_u32( this->m_fading_alpha[ index ] );

			float track_top = FLT_MAX, track_bottom = -FLT_MAX;
			for ( const auto& p : track ) {
				track_top    = std::min( track_top, p.y );
				track_bottom = std::max( track_bottom, p.y );
			}
			track_top -= bar_thickness * 0.5f;
			track_bottom += bar_thickness * 0.5f;

			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_callback,
				std::make_any< callback_draw_object_t >( callback_draw_object_t{
					[ track, outer, fill, bar_outline, bar_thickness, outline_color, bg_color, fill_color, gradient, bottom_color, track_top,
				      track_bottom ]( ImDrawList* draw_list ) {
						if ( bar_outline )
							draw_list->AddPolyline( outer.data( ), static_cast< int >( outer.size( ) ), outline_color, ImDrawFlags_None,
						                            bar_thickness + 2.f );

						draw_list->AddPolyline( track.data( ), static_cast< int >( track.size( ) ), bg_color, ImDrawFlags_None, bar_thickness );

						if ( fill.size( ) < 2 )
							return;

						const int first_vertex = draw_list->VtxBuffer.Size;
						draw_list->AddPolyline( fill.data( ), static_cast< int >( fill.size( ) ), gradient ? IM_COL32_WHITE : fill_color,
					                            ImDrawFlags_None, bar_thickness );
						if ( !gradient )
							return;

						const ImVec4 top = ImGui::ColorConvertU32ToFloat4( fill_color ), bottom = ImGui::ColorConvertU32ToFloat4( bottom_color );
						const float  height = std::max( track_bottom - track_top, 1.f );

						for ( int i = first_vertex; i < draw_list->VtxBuffer.Size; ++i ) {
							ImDrawVert& vertex  = draw_list->VtxBuffer[ i ];
							const float t       = std::clamp( ( vertex.pos.y - track_top ) / height, 0.f, 1.f );
							const float coverage = static_cast< float >( ( vertex.col >> IM_COL32_A_SHIFT ) & 0xFF ) / 255.f;

							vertex.col = ImGui::ColorConvertFloat4ToU32( ImVec4( top.x + ( bottom.x - top.x ) * t, top.y + ( bottom.y - top.y ) * t,
						                                                         top.z + ( bottom.z - top.z ) * t,
						                                                         ( top.w + ( bottom.w - top.w ) * t ) * coverage ) );
						}
					} } ) );

			if ( GET_VARIABLE( g_variables.m_players_health_text, bool ) && GET_VARIABLE( g_variables.m_players_health_text_style, int ) == 1 &&
			     health_font ) {
				const auto text_size = health_font->CalcTextSizeA( health_font->FontSize, FLT_MAX, 0.f, hp_text.c_str( ) );

				const float reach = radius + bar_thickness * 0.5f + ( bar_outline ? 1.f : 0.f ) + 2.f;

				const ImVec2 tip = place( ImVec2( cx + radius - reach * 0.70710678f, cy - radius + reach * 0.70710678f ) );

				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_text,
					std::make_any< text_draw_object_t >(
						health_font, c_vector_2d( flip_x ? tip.x : tip.x - text_size.x, flip_y ? tip.y - text_size.y : tip.y ), hp_text,
						health_text_color( c_color( 1.f, 1.f, 1.f ) ).get_u32( this->m_fading_alpha[ index ] ),
						ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ), text_flag_dropshadow ) );
			}
		} else if ( GET_VARIABLE( g_variables.m_players_health_bar, bool ) ) {
			const int   bar_side      = GET_VARIABLE( g_variables.m_players_health_bar_side, int );
			const float bar_thickness = GET_VARIABLE( g_variables.m_players_health_bar_thickness, float );
			const bool  bar_outline   = GET_VARIABLE( g_variables.m_players_health_bar_outline, bool );

			const bool vertical = bar_side == e_bar_side::bar_side_left || bar_side == e_bar_side::bar_side_right;

			c_vector_2d track_min{ }, track_max{ };

			switch ( bar_side ) {
			case e_bar_side::bar_side_right:
				track_min = c_vector_2d( box.m_right + 3.f, box.m_top );
				track_max = c_vector_2d( box.m_right + 3.f + bar_thickness, box.m_bottom );
				break;
			case e_bar_side::bar_side_top:
				track_min = c_vector_2d( box.m_left, box.m_top - 3.f - bar_thickness );
				track_max = c_vector_2d( box.m_right, box.m_top - 3.f );
				break;
			case e_bar_side::bar_side_bottom:
				track_min = c_vector_2d( box.m_left, box.m_bottom + 3.f );
				track_max = c_vector_2d( box.m_right, box.m_bottom + 3.f + bar_thickness );
				break;
			default:
				track_min = c_vector_2d( box.m_left - 3.f - bar_thickness, box.m_top );
				track_max = c_vector_2d( box.m_left - 3.f, box.m_bottom );
				break;
			}

			c_vector_2d fill_min = track_min, fill_max = track_max;

			if ( vertical )
				fill_min.m_y = track_max.m_y - ( track_max.m_y - track_min.m_y ) * factor;
			else
				fill_max.m_x = track_min.m_x + ( track_max.m_x - track_min.m_x ) * factor;

			const unsigned int outline_flags = bar_outline ? e_rect_flags::rect_flag_outer_outline : e_rect_flags::rect_flag_none;

			const bool  custom_color = GET_VARIABLE( g_variables.m_players_health_bar_custom_color, bool );
			const bool  gradient     = custom_color && GET_VARIABLE( g_variables.m_players_health_bar_gradient, bool );
			const ImU32 top_color    = GET_VARIABLE( g_variables.m_players_health_bar_color, c_color ).get_u32( this->m_fading_alpha[ index ] );

			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_rect,
				std::make_any< rect_draw_object_t >(
					track_min, track_max, GET_VARIABLE( g_variables.m_players_health_bar_bg_color, c_color ).get_u32( this->m_fading_alpha[ index ] ),
					ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ), true, 0.f, ImDrawFlags_::ImDrawFlags_None, 1.f, outline_flags ) );

			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_rect,
				std::make_any< rect_draw_object_t >(
					fill_min, fill_max,
					gradient ? 0u : custom_color ? top_color : c_color::from_hsb( hue, 1.f, 1.f, 1.f ).get_u32( this->m_fading_alpha[ index ] ),
					ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ), true, 0.f, ImDrawFlags_::ImDrawFlags_None, 1.f, outline_flags ) );

			if ( gradient ) {
				// gradient pinned to track, fill reveals it
				const ImVec4 top    = ImGui::ColorConvertU32ToFloat4( top_color );
				const ImVec4 bottom = ImGui::ColorConvertU32ToFloat4(
					GET_VARIABLE( g_variables.m_players_health_bar_bottom_color, c_color ).get_u32( this->m_fading_alpha[ index ] ) );
				const float t = std::clamp( ( fill_min.m_y - track_min.m_y ) / std::max( track_max.m_y - track_min.m_y, 1.f ), 0.f, 1.f );

				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_gradient_rect,
					std::make_any< gradient_rect_draw_object_t >(
						fill_min, fill_max,
						ImGui::ColorConvertFloat4ToU32( ImVec4( top.x + ( bottom.x - top.x ) * t, top.y + ( bottom.y - top.y ) * t,
				                                                top.z + ( bottom.z - top.z ) * t, top.w + ( bottom.w - top.w ) * t ) ),
						ImGui::ColorConvertFloat4ToU32( bottom ) ) );
			}

			if ( GET_VARIABLE( g_variables.m_players_health_text, bool ) && GET_VARIABLE( g_variables.m_players_health_text_style, int ) == 1 &&
			     health_font ) {
				const auto text_size = health_font->CalcTextSizeA( health_font->FontSize, FLT_MAX, 0.f, hp_text.c_str( ) );

				const c_vector_2d text_position =
					vertical ? c_vector_2d( bar_side == e_bar_side::bar_side_right ? track_max.m_x + 2.f : track_min.m_x - 2.f - text_size.x,
					                        fill_min.m_y )
					         : c_vector_2d( fill_max.m_x - text_size.x * 0.5f, track_min.m_y - text_size.y );

				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_text,
					std::make_any< text_draw_object_t >( health_font, text_position, hp_text,
				                                         health_text_color( c_color( 1.f, 1.f, 1.f ) ).get_u32( this->m_fading_alpha[ index ] ),
				                                         ImColor( 0.f, 0.f, 0.f, this->m_fading_alpha[ index ] ), text_flag_dropshadow ) );
			}
		}

		[ & ]( const bool draw_skeleton ) {
			if ( !draw_skeleton )
				return;

			const c_color skeleton_color = GET_VARIABLE( g_variables.m_players_skeleton_visibility_colors, bool )
			                                    ? ( is_player_visible( ) ? GET_VARIABLE( g_variables.m_players_skeleton_visible_color, c_color )
			                                                             : GET_VARIABLE( g_variables.m_players_skeleton_invisible_color, c_color ) )
			                                    : GET_VARIABLE( g_variables.m_players_skeleton_color, c_color );

			auto draw_skeleton_matrix = [ & ]( matrix3x4_t* bone_matrix ) {
				if ( !bone_matrix )
					return;

				const auto model = client_renderable->get_model( );
				if ( model ) {
					const auto studio_model = g_interfaces.m_model_info->get_studio_model( model );

					if ( studio_model ) {
						c_vector child_position = { }, parent_position = { };
						c_vector_2d child_screen_position = { }, parent_screen_position = { };

						c_vector upper_direction = c_vector( bone_matrix[ 7 ][ 0 ][ 3 ], bone_matrix[ 7 ][ 1 ][ 3 ], bone_matrix[ 7 ][ 2 ][ 3 ] ) -
						                           c_vector( bone_matrix[ 6 ][ 0 ][ 3 ], bone_matrix[ 6 ][ 1 ][ 3 ], bone_matrix[ 6 ][ 2 ][ 3 ] );
						c_vector breast_bone =
							c_vector( bone_matrix[ 6 ][ 0 ][ 3 ], bone_matrix[ 6 ][ 1 ][ 3 ], bone_matrix[ 6 ][ 2 ][ 3 ] ) + upper_direction * 0.5f;

						const bool smooth = GET_VARIABLE( g_variables.m_players_skeleton_type, int ) == 2;
						std::vector< skeleton_edge_t > edges;

						for ( int i = 0; i < studio_model->n_bones && i < 128; i++ ) {
							mstudiobone_t* bone = studio_model->get_bone( i );
							if ( !bone )
								continue;

							if ( bone->m_parent == -1 )
								continue;

							if ( !( bone->m_flags & 0x00000100  ) )
								continue;

							child_position  = c_vector( bone_matrix[ i ][ 0 ][ 3 ], bone_matrix[ i ][ 1 ][ 3 ], bone_matrix[ i ][ 2 ][ 3 ] );
							parent_position = c_vector( bone_matrix[ bone->m_parent ][ 0 ][ 3 ], bone_matrix[ bone->m_parent ][ 1 ][ 3 ],
							                            bone_matrix[ bone->m_parent ][ 2 ][ 3 ] );

							c_vector delta_child  = child_position - breast_bone;
							c_vector delta_parent = parent_position - breast_bone;
							int parent_node       = bone->m_parent;

							if ( delta_parent.length( ) < 9.0f && delta_child.length( ) < 9.0f ) {
								parent_position = breast_bone;
								parent_node     = 128;
							}

							if ( i == 5 )
								child_position = breast_bone;

							if ( fabs( delta_child.m_z ) < 5.0f && delta_parent.length( ) < 5.0f && delta_child.length( ) < 5.0f || i == 6 )
								continue;

							if ( smooth ) {
								const int child_node = i == 5 ? 128 : i;
								if ( child_node != parent_node && parent_node >= 0 && parent_node <= 128 )
									edges.push_back( { child_node, parent_node, child_position, parent_position } );
								continue;
							}

							if ( g_render.world_to_screen( child_position, child_screen_position ) &&
							     g_render.world_to_screen( parent_position, parent_screen_position ) ) {
								g_render.m_draw_data.emplace_back(
									e_draw_type::draw_type_line,
									std::make_any< line_draw_object_t >(
										child_screen_position, parent_screen_position,
										skeleton_color.get_u32( this->m_fading_alpha[ index ] ),
										GET_VARIABLE( g_variables.m_players_skeleton_thickness, float ) ) );
							}
						}

						if ( smooth )
							push_smooth_skeleton( edges, skeleton_color.get_u32( this->m_fading_alpha[ index ] ),
							                      GET_VARIABLE( g_variables.m_players_skeleton_thickness, float ) );
					}
				}
			};

			if ( GET_VARIABLE( g_variables.m_players_skeleton_type, int ) == 1 && GET_VARIABLE( g_variables.m_backtrack_enable, bool ) ) {
				auto* record = g_lagcomp.oldest_record( entity->get_index( ) );

				if ( !record )
					return;

				auto bone_matrix = record->m_matrix;

				draw_skeleton_matrix( bone_matrix );

			} else {
				matrix3x4_t bone_matrix[ 128 ]{ };

				memcpy( bone_matrix, entity->get_cached_bone_data( ).get_elements( ),
				        entity->get_cached_bone_data( ).count( ) * sizeof( matrix3x4_t ) );

				draw_skeleton_matrix( bone_matrix );
			}
		}( GET_VARIABLE( g_variables.m_players_skeleton, bool ) && !shared );

		if ( !shared && GET_VARIABLE( g_variables.m_players_hitboxes, bool ) && GET_VARIABLE( g_variables.m_players_hitboxes_mode, int ) == 0 )
			push_hitbox_capsules( hitbox_capsules( entity ), GET_VARIABLE( g_variables.m_players_hitboxes_color, c_color ).get_u32( this->m_fading_alpha[ index ] ),
			                      GET_VARIABLE( g_variables.m_players_hitboxes_thickness, float ) );

		if ( !shared && GET_VARIABLE( g_variables.m_players_backtrack_trail, bool ) ) {
			hitbox_resolver_t hitbox_resolver{ };

			if ( const auto record_list = hitbox_resolver.setup( entity ) ? g_lagcomp.m_records[ index ] : nullptr ) {
				for ( int i = 0; i < g_ctx.m_max_allocations; i++ ) {
					auto& record = record_list[ i ];

					if ( !record.m_valid )
						continue;

					c_vector_2d out{ };

					if ( !g_render.world_to_screen( hitbox_resolver.position( hitbox_head, record.m_matrix ), out ) )
						continue;

					g_render.m_draw_data.emplace_back( e_draw_type::draw_type_rect,
					                                   std::make_any< rect_draw_object_t >( out, c_vector_2d( out.m_x + 1, out.m_y + 1 ),
					                                                                        ImColor( 1.f, 1.f, 1.f, this->m_fading_alpha[ index ] ),
					                                                                        ImColor( ), true, 0.f, ImDrawFlags_::ImDrawFlags_None,
					                                                                        4.f, e_rect_flags::rect_flag_none ) );
				}
			}
		}

		if ( GET_VARIABLE( g_variables.m_players_avatar, bool ) ) {
			const int avatar_position = GET_VARIABLE( g_variables.m_players_avatar_position, int );

			const auto avatar_texture = player_info.m_fake_player ? team == e_team_id::team_tt   ? g_render.m_terrorist_avatar
			                                                        : team == e_team_id::team_ct ? g_render.m_counter_terrorist_avatar
			                                                                                     : nullptr
			                                                      : g_avatar_cache[ index ];

			if ( avatar_position == e_avatar_position::avatar_position_head ) {
				hitbox_resolver_t hitbox_resolver{ };

				if ( hitbox_resolver.setup( entity ) ) {
					if ( const auto head_hitbox = hitbox_resolver.m_set->get_hitbox( hitbox_head ) ) {
						const c_vector head_position =
							hitbox_resolver.position( hitbox_head, entity->get_cached_bone_data( ).get_elements( ) );

						c_angle view_angles{ };
						g_interfaces.m_engine_client->get_view_angles( view_angles );

						c_vector right{ };
						g_math.angle_vectors( view_angles, nullptr, &right );

						c_vector_2d head_screen{ }, edge_screen{ };

						if ( g_render.world_to_screen( head_position, head_screen ) &&
						     g_render.world_to_screen( head_position + right * head_hitbox->m_radius, edge_screen ) ) {
							const float delta = edge_screen.m_x - head_screen.m_x;

							const float radius =
								( delta < 0.f ? -delta : delta ) * GET_VARIABLE( g_variables.m_players_avatar_head_scale, float );

							if ( radius > 0.5f )
								g_render.m_draw_data.emplace_back(
									e_draw_type::draw_type_texture,
									std::make_any< texture_draw_object_t >(
										c_vector_2d( head_screen.m_x - radius, head_screen.m_y - radius ),
										c_vector_2d( radius * 2.f, radius * 2.f ),
										ImColor( 1.f, 1.f, 1.f, this->m_fading_alpha[ index ] ), avatar_texture, radius,
										ImDrawFlags_::ImDrawFlags_RoundCornersAll ) );
						}
					}
				}
			}
			else if ( avatar_position != e_avatar_position::avatar_position_name ) {
				const float avatar_size = GET_VARIABLE( g_variables.m_players_avatar_size, float );
				const bool  below       = avatar_position == e_avatar_position::avatar_position_bottom;

				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_texture,
					std::make_any< texture_draw_object_t >(
						c_vector_2d( box.m_left + ( box.m_width - avatar_size ) * 0.5f,
				                     below ? box.m_bottom + 2 + padding[ e_padding_direction::padding_direction_bottom ]
				                           : box.m_top - 5 - avatar_size - padding[ e_padding_direction::padding_direction_top ] ),
						c_vector_2d( avatar_size, avatar_size ), ImColor( 1.f, 1.f, 1.f, this->m_fading_alpha[ index ] ), avatar_texture,
						avatar_size * 0.5f, ImDrawFlags_::ImDrawFlags_RoundCornersAll ) );

				if ( below )
					padding[ e_padding_direction::padding_direction_bottom ] += avatar_size + 1.f;
				else
					padding[ e_padding_direction::padding_direction_top ] -= avatar_size;
			}
		}

	} );
}

void n_players::impl_t::sound_arrows( )
{
	const int arrows_type = GET_VARIABLE( g_variables.m_out_of_fov_arrows_type, int );

	if ( !GET_VARIABLE( g_variables.m_out_of_fov_arrows, bool ) || arrows_type == 0 || !g_ctx.m_local )
		return;

	const float duration = GET_VARIABLE( g_variables.m_out_of_fov_arrows_sound_duration, float );
	const bool sound_fade = GET_VARIABLE( g_variables.m_out_of_fov_arrows_sound_fade, bool );

	c_angle view_angles = { };
	g_interfaces.m_engine_client->get_view_angles( view_angles );

	const float yaw = deg2rad( view_angles.m_y );

	const auto display_size = ImVec2( g_ctx.m_width, g_ctx.m_height );

	const float arrow_width  = GET_VARIABLE( g_variables.m_out_of_fov_arrows_width, float );
	const float arrow_height = GET_VARIABLE( g_variables.m_out_of_fov_arrows_height, float );
	const float ring_radius  = static_cast< float >( GET_VARIABLE( g_variables.m_out_of_fov_arrows_distance, int ) );

	float radius_x       = ring_radius;
	const float radius_y = ring_radius;
	if ( GET_VARIABLE( g_variables.m_out_of_fov_arrows_aspect, bool ) && display_size.y > 0.f )
		radius_x = ring_radius * ( display_size.x / display_size.y );

	const c_color sound_color = GET_VARIABLE( g_variables.m_out_of_fov_arrows_sound_color, c_color );
	const c_vector local_origin = g_ctx.m_local->get_abs_origin( );

	for ( int index = 1; index <= 64; index++ ) {
		const float alpha = g_sound_esp.alpha( index, duration, sound_fade );
		if ( alpha <= 0.f )
			continue;

		const c_vector sound_origin = g_sound_esp.m_origin[ index ];
		if ( sound_origin.is_zero( ) )
			continue;

		c_vector_2d screen = { };
		if ( g_render.world_to_screen( sound_origin, screen ) && screen.m_x >= g_render.to_overlay_x( 0.f ) && screen.m_y >= 0.f &&
		     screen.m_x <= g_render.to_overlay_x( display_size.x ) && screen.m_y <= display_size.y )
			continue;

		const c_vector position_difference = local_origin - sound_origin;

		float x = std::cos( yaw ) * position_difference.m_y - std::sin( yaw ) * position_difference.m_x;
		float y = std::cos( yaw ) * position_difference.m_x + std::sin( yaw ) * position_difference.m_y;
		if ( const auto len = std::sqrt( x * x + y * y ); len ) {
			x /= len;
			y /= len;
		}

		const c_vector_2d position = c_vector_2d( display_size.x * 0.5f + x * radius_x, display_size.y * 0.5f + y * radius_y );

		push_ring_arrow( position, x, y, arrow_width, arrow_height, sound_color.get_u32( alpha ) );
	}
}
