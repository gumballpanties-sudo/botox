#include "grenade_live.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"

#include "../../entity_cache/entity_cache.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <string>

namespace
{
	constexpr float k_he_lifetime     = 1.5f;
	constexpr float k_he_think        = 0.2f;
	constexpr float k_inferno_life    = 7.f;
	constexpr float k_max_sim_time    = 5.f;
	constexpr float k_elasticity      = 0.45f;
	constexpr float k_warning_range   = 400.f;
	constexpr float k_fire_range      = 150.f;
	constexpr float k_he_range_sq     = 122500.f;
	constexpr unsigned int k_mask     = 0x4208608B;
	constexpr unsigned int k_mask_deb = 0x8400B;

	template< typename t >
	t& field( c_base_entity* entity, const unsigned int offset )
	{
		return *reinterpret_cast< t* >( reinterpret_cast< std::uintptr_t >( entity ) + offset );
	}

	struct offsets_t {
		unsigned int m_thrower, m_velocity, m_dmg_radius, m_initial_velocity, m_bounces, m_explode_tick;
		unsigned int m_fire_x, m_fire_y, m_fire_z, m_fire_burning, m_fire_count, m_fire_tick;
	};

	const offsets_t& offsets( )
	{
		static const offsets_t o{ g_netvars[ HASH_BT( "CBaseGrenade->m_hThrower" ) ].m_offset,
			                      g_netvars[ HASH_BT( "CBaseGrenade->m_vecVelocity" ) ].m_offset,
			                      g_netvars[ HASH_BT( "CBaseGrenade->m_DmgRadius" ) ].m_offset,
			                      g_netvars[ HASH_BT( "CBaseCSGrenadeProjectile->m_vInitialVelocity" ) ].m_offset,
			                      g_netvars[ HASH_BT( "CBaseCSGrenadeProjectile->m_nBounces" ) ].m_offset,
			                      g_netvars[ HASH_BT( "CBaseCSGrenadeProjectile->m_nExplodeEffectTickBegin" ) ].m_offset,
			                      g_netvars[ HASH_BT( "CInferno->m_fireXDelta" ) ].m_offset,
			                      g_netvars[ HASH_BT( "CInferno->m_fireYDelta" ) ].m_offset,
			                      g_netvars[ HASH_BT( "CInferno->m_fireZDelta" ) ].m_offset,
			                      g_netvars[ HASH_BT( "CInferno->m_bFireIsBurning" ) ].m_offset,
			                      g_netvars[ HASH_BT( "CInferno->m_fireCount" ) ].m_offset,
			                      g_netvars[ HASH_BT( "CInferno->m_nFireEffectTickBegin" ) ].m_offset };
		return o;
	}

	int class_of( c_base_entity* entity )
	{
		const auto networkable  = entity ? entity->get_client_networkable( ) : nullptr;
		const auto client_class = networkable ? networkable->get_client_class( ) : nullptr;
		return client_class ? static_cast< int >( client_class->m_class_id ) : -1;
	}

	/* index reuse: same slot + address can be a new entity of another class */
	bool still_live( const n_grenade_live::record_t& record )
	{
		c_base_entity* const live = g_interfaces.m_client_entity_list->get< c_base_entity >( record.m_index );
		return live && live == record.m_entity && !live->is_dormant( ) && class_of( live ) == record.m_class;
	}

	bool exploded( c_base_entity* entity )
	{
		const unsigned int offset = offsets( ).m_explode_tick;
		return offset && field< int >( entity, offset ) != 0;
	}

	c_base_entity* thrower_of( c_base_entity* entity, const int kind )
	{
		const unsigned int handle =
			kind == n_grenade_live::kind_inferno ? entity->get_owner_entity_handle( ) : field< unsigned int >( entity, offsets( ).m_thrower );
		if ( handle == 0xFFFFFFFF )
			return nullptr;
		return reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( handle ) );
	}

	float team_scale( c_base_entity* thrower, const int kind )
	{
		if ( !thrower || !thrower->is_player( ) )
			return 1.f;

		const bool fire = kind != n_grenade_live::kind_he;

		if ( thrower == g_ctx.m_local )
			return fire ? 1.f : g_convars.float_or( HASH_BT( "ff_damage_reduction_grenade_self" ), 1.f );

		if ( thrower->is_enemy( g_ctx.m_local ) || g_convars.float_or( HASH_BT( "mp_teammates_are_enemies" ), 0.f ) != 0.f )
			return 1.f;

		if ( g_convars.float_or( HASH_BT( "mp_friendlyfire" ), 0.f ) == 0.f )
			return 0.f;

		return fire ? g_convars.float_or( HASH_BT( "ff_damage_reduction_other" ), 0.4f )
		            : g_convars.float_or( HASH_BT( "ff_damage_reduction_grenade" ), 0.85f );
	}

	float view_height( const float duck )
	{
		const float smooth = duck * duck * 3.f - duck * duck * 2.f * duck;
		return ( 1.f - smooth ) * 18.f + 46.f;
	}

	c_vector clip_velocity( const c_vector& in, const c_vector& normal )
	{
		c_vector out = in - normal * ( in.dot_product( normal ) * 2.f );
		if ( std::fabsf( out.m_x ) < 0.1f )
			out.m_x = 0.f;
		if ( std::fabsf( out.m_y ) < 0.1f )
			out.m_y = 0.f;
		if ( std::fabsf( out.m_z ) < 0.1f )
			out.m_z = 0.f;
		return out;
	}

	bool in_smoke( const c_vector& fire )
	{
		bool inside = false;
		g_entity_cache.enumerate( e_enumeration_type::type_edicts, [ & ]( c_base_entity* entity ) {
			if ( inside || !entity || entity->is_dormant( ) )
				return;

			const auto client_class = entity->get_client_networkable( )->get_client_class( );
			if ( !client_class || client_class->m_class_id != e_class_ids::c_smoke_grenade_projectile || !entity->did_smoke_effect( ) )
				return;

			const c_vector smoke = entity->get_abs_origin( );
			if ( fire.dist_to_squared( smoke ) > 166.f * 166.f )
				return;

			const float height = fire.m_z - smoke.m_z;
			if ( height < -64.f || height > 120.f )
				return;

			float radius_sq = 22500.f;
			if ( height > 72.f ) {
				const float f = ( height - 72.f ) / 48.f;
				radius_sq -= 12500.f * f * f;
			} else if ( height < 18.f ) {
				const float f = ( height - 12.f ) / -76.f;
				radius_sq -= 6116.f * f * f;
			}

			const float dx = fire.m_x - smoke.m_x, dy = fire.m_y - smoke.m_y;
			inside = dx * dx + dy * dy < radius_sq;
		} );
		return inside;
	}

	float transmission( const c_vector& explosion, const c_vector& sample, c_base_entity* target )
	{
		trace_t trace{ };
		c_trace_filter filter( target );
		g_interfaces.m_engine_trace->trace_ray( ray_t( explosion, sample ), 0x46004003, &filter, &trace );
		if ( trace.m_fraction == 1.f )
			return 1.f;

		const auto obstacle = trace.m_hit_entity;
		if ( !obstacle || obstacle->get_index( ) == 0 || obstacle == target )
			return 0.f;

		if ( reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( obstacle->get_owner_entity_handle( ) ) ) ==
		     target )
			return 0.f;

		trace_t solid{ };
		c_trace_filter_world world{ };
		g_interfaces.m_engine_trace->trace_ray( ray_t( explosion, sample ), e_contents::contents_solid, &world, &solid );
		return solid.m_fraction == 1.f ? 0.75f : 0.f;
	}

	float exposure( const c_vector& position, const std::array< c_vector, 5 >& samples, c_base_entity* target )
	{
		return transmission( position, samples[ 0 ], target ) * 0.4f + transmission( position, samples[ 1 ], target ) * 0.2f +
		       transmission( position, samples[ 2 ], target ) * 0.2f + transmission( position, samples[ 3 ], target ) * 0.1f +
		       transmission( position, samples[ 4 ], target ) * 0.1f;
	}

	ImU32 hsv_lerp_red( const float t, const int alpha )
	{
		const float v = 50.f / 255.f + ( 1.f - 50.f / 255.f ) * t;
		const float g = v * ( 1.f - t );
		return IM_COL32( static_cast< int >( v * 255.f + 0.5f ), static_cast< int >( g * 255.f + 0.5f ), static_cast< int >( g * 255.f + 0.5f ),
		                 alpha );
	}

	struct mesh_desc_t {
		int m_vertex_size[ 18 ];
		int m_actual_vertex_size;
		int m_compression;
		int m_bone_weights;
		void* m_data[ 18 ];
		int m_first_vertex;
		unsigned int m_vertex_offset;
		unsigned short* m_indices;
		unsigned int m_index_offset;
		unsigned int m_first_index;
		int m_index_size;
	};
	static_assert( sizeof( mesh_desc_t ) == 0xB4 );

	void write_channel( mesh_desc_t& desc, const int channel, const int vertex, const void* source, const std::size_t size )
	{
		if ( desc.m_vertex_size[ channel ] <= 0 || !desc.m_data[ channel ] )
			return;
		std::memcpy( static_cast< std::uint8_t* >( desc.m_data[ channel ] ) + desc.m_vertex_size[ channel ] * vertex, source, size );
	}

	c_vector unit( const c_vector& v )
	{
		const float length = v.length( );
		return v * ( 1.f / ( length + 1.1920929e-07f ) );
	}

	c_vector cross( const c_vector& a, const c_vector& b )
	{
		return c_vector( a.m_y * b.m_z - a.m_z * b.m_y, a.m_z * b.m_x - a.m_x * b.m_z, a.m_x * b.m_y - a.m_y * b.m_x );
	}

	struct ribbon_vertex_t {
		c_vector m_position;
		float m_u;
		c_vector m_tangent_s, m_tangent_t;
	};

	/* CBeamSegDraw::NextSeg: width axis = to_next x view, averaged with the last one from the third point */
	void build_ribbon( const std::vector< c_vector >& points, const c_vector& camera, const float width, std::vector< ribbon_vertex_t >& out )
	{
		out.clear( );
		const std::size_t count = points.size( );
		c_vector previous_axis{ };

		for ( std::size_t i = 0; i < count; ++i ) {
			const c_vector& point = points[ i ];
			const c_vector view   = point - camera;
			c_vector axis         = previous_axis;

			if ( i + 1 < count ) {
				const c_vector current = unit( cross( point - points[ i + 1 ], view ) );
				axis                   = i < 1 ? current : unit( ( previous_axis + current ) * 0.5f );
				previous_axis          = current;
			}

			const c_vector tangent = unit( cross( view, axis ) );
			out.push_back( { point + axis * ( width * 0.5f ), 0.f, axis, tangent } );
			out.push_back( { point - axis * ( width * 0.5f ), 1.f, axis, tangent } );
		}
	}
}

void n_grenade_live::impl_t::reset( )
{
	this->m_records.clear( );
}

void n_grenade_live::impl_t::release_material( )
{
	if ( this->m_material )
		this->m_material->decrement_reference_count( );
	this->m_material = nullptr;
}

bool n_grenade_live::impl_t::admit( record_t& record, c_base_entity* entity, const int class_id )
{
	const float interval = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float gravity  = g_convars.float_or( HASH_BT( "sv_gravity" ), 800.f ) * 0.4f;

	if ( class_id == static_cast< int >( e_class_ids::c_inferno ) ) {
		record.m_kind = kind_inferno;
		const unsigned int tick_offset = offsets( ).m_fire_tick;
		const int tick                 = tick_offset ? field< int >( entity, tick_offset ) : 0;
		record.m_creation              = tick > 0 ? tick * interval : g_interfaces.m_global_vars_base->m_tick_count * interval;
		record.m_end                   = record.m_creation + k_inferno_life;
		record.m_point                 = entity->get_abs_origin( );
		record.m_has_point             = true;
		record.m_threat                = team_scale( thrower_of( entity, kind_inferno ), kind_inferno ) != 0.f;
		return true;
	}

	if ( class_id == static_cast< int >( e_class_ids::c_base_cs_grenade_projectile ) ) {
		if ( !offsets( ).m_dmg_radius || field< float >( entity, offsets( ).m_dmg_radius ) != 350.f )
			return false;
		record.m_kind = kind_he;
	} else
		record.m_kind = kind_molotov;

	const auto& o = offsets( );
	if ( !o.m_thrower || !o.m_velocity || !o.m_initial_velocity || exploded( entity ) || gravity <= 0.f || interval <= 0.f )
		return false;

	if ( field< unsigned int >( entity, o.m_thrower ) == 0xFFFFFFFF )
		return false;

	const float lifetime = record.m_kind == kind_he ? k_he_lifetime : g_convars.float_or( HASH_BT( "molotov_throw_detonate_time" ), 2.f );
	const bool bounced   = o.m_bounces && field< int >( entity, o.m_bounces ) > 0;
	const float initial  = field< c_vector >( entity, o.m_initial_velocity ).m_z;
	const float current  = field< c_vector >( entity, o.m_velocity ).m_z;
	const float elapsed  = bounced ? 0.f : std::roundf( ( initial - current ) / ( gravity * interval ) ) * interval;
	if ( elapsed >= lifetime )
		return false;

	record.m_creation = entity->get_simulation_time( ) - elapsed;
	record.m_end      = record.m_creation + lifetime;

	this->simulate( record );
	record.m_threat = team_scale( thrower_of( entity, record.m_kind ), record.m_kind ) != 0.f && ( record.m_kind == kind_he || record.m_has_point );
	return true;
}

void n_grenade_live::impl_t::simulate( record_t& record )
{
	c_base_entity* const entity = record.m_entity;
	const float interval        = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float gravity         = g_convars.float_or( HASH_BT( "sv_gravity" ), 800.f ) * 0.4f;
	c_base_entity* const thrower = thrower_of( entity, record.m_kind );

	c_vector position = entity->get_origin( );
	c_vector velocity = field< c_vector >( entity, offsets( ).m_velocity );
	record.m_sim_origin   = position;
	record.m_sim_velocity = velocity;
	record.m_path.clear( );

	const c_vector initial = field< c_vector >( entity, offsets( ).m_initial_velocity );
	float time = ( velocity - initial ).length_squared( ) < 1.f ? g_interfaces.m_global_vars_base->m_tick_count * interval : entity->get_simulation_time( );

	const bool molotov       = record.m_kind == kind_molotov;
	const unsigned int mask  = entity->get_collision_group( ) == 1 ? k_mask_deb : k_mask;
	const float slope        = std::cosf( g_convars.float_or( HASH_BT( "weapon_molotov_maxdetonateslope" ), 30.f ) * std::numbers::pi_v< float > / 180.f );
	const int think_ticks    = std::max( 1, static_cast< int >( std::roundf( k_he_think / interval ) ) );
	const int max_ticks      = static_cast< int >( std::roundf( k_max_sim_time / interval ) );
	int bounces              = offsets( ).m_bounces ? field< int >( entity, offsets( ).m_bounces ) : 0;
	bool landed              = false;

	c_base_entity* broken[ 8 ]{ };
	int broken_count = 0;
	const auto trace_step = [ & ]( const c_vector& from, const c_vector& to, trace_t& out ) {
		const c_vector mins( -2.f, -2.f, -2.f ), maxs( 2.f, 2.f, 2.f );
		c_trace_filter filter( [ & ]( c_base_entity* hit, int ) {
			if ( hit == entity || hit == thrower )
				return false;
			for ( int i = 0; i < broken_count; ++i )
				if ( broken[ i ] == hit )
					return false;
			return true;
		} );
		g_interfaces.m_engine_trace->trace_ray( ray_t( from, to, mins, maxs ), mask, &filter, &out );
		if ( out.m_hit_entity && out.m_hit_entity->is_player( ) )
			g_interfaces.m_engine_trace->trace_ray( ray_t( from, to ), mask, &filter, &out );
	};

	for ( int tick = 0; tick < max_ticks; ++tick ) {
		if ( std::fabsf( velocity.m_x ) < 0.01f && std::fabsf( velocity.m_y ) < 0.01f && std::fabsf( velocity.m_z ) < 0.01f )
			break;

		const int since_creation = static_cast< int >( std::roundf( ( time - record.m_creation ) / interval ) );
		const bool think         = !molotov ? since_creation % think_ticks == 0 : true;
		if ( think && time > record.m_end )
			break;

		const float new_z = velocity.m_z - gravity * interval;
		const c_vector move( velocity.m_x * interval, velocity.m_y * interval, ( velocity.m_z + new_z ) * 0.5f * interval );
		velocity.m_z = new_z;

		trace_t trace{ };
		trace_step( position, position + move, trace );
		position = trace.m_end;
		time += interval;

		if ( trace.m_fraction < 1.f ) {
			const c_vector normal = trace.m_plane.m_normal;
			c_base_entity* const hit = trace.m_hit_entity;
			const bool world         = !hit || hit->get_index( ) == 0;

			if ( molotov && normal.m_z >= slope ) {
				record.m_path.push_back( position );
				landed = true;
				break;
			}

			if ( !world && hit->is_breakable( ) && broken_count < 8 ) {
				broken[ broken_count++ ] = hit;
				velocity                 = velocity * 0.4f;
				record.m_path.push_back( position );
				continue;
			}

			const float elasticity = std::clamp( k_elasticity * ( !world && hit->is_player( ) ? 0.3f : 1.f ), 0.f, 0.9f );
			velocity               = clip_velocity( velocity, normal ) * elasticity;

			const float speed_sq = velocity.length_squared( );
			if ( normal.m_z > 0.7f || ( normal.m_z > 0.1f && speed_sq < 400.f ) ) {
				if ( speed_sq > 96000.f ) {
					const float along = velocity.normalized( ).dot_product( normal );
					if ( along > 0.5f )
						velocity = velocity * ( 1.5f - along );
				}
				if ( speed_sq < 400.f )
					velocity = c_vector( );
			}

			if ( ++bounces > 20 )
				velocity = c_vector( );

			if ( !velocity.is_zero( ) ) {
				trace_t rest{ };
				trace_step( position, position + velocity * ( ( 1.f - trace.m_fraction ) * interval ), rest );
				position = rest.m_end;
			}
		}

		record.m_path.push_back( position );
	}

	c_trace_filter filter( [ entity, thrower ]( c_base_entity* hit, int ) { return hit != entity && hit != thrower && !hit->is_player( ); } );
	trace_t ground{ };

	c_vector point = position;
	bool has_point = true;

	if ( record.m_kind == kind_he ) {
		g_interfaces.m_engine_trace->trace_ray( ray_t( position + c_vector( 0.f, 0.f, 8.f ), position - c_vector( 0.f, 0.f, 24.f ) ), mask, &filter,
		                                        &ground );
		point = ground.m_fraction < 1.f ? ground.m_end + ground.m_plane.m_normal * 0.6f : position;
		point.m_z += 1.f;
		record.m_path.push_back( point );
	} else {
		g_interfaces.m_engine_trace->trace_ray( ray_t( position + c_vector( 0.f, 0.f, 10.f ), position - c_vector( 0.f, 0.f, 128.f ) ), mask, &filter,
		                                        &ground );
		has_point = landed || ( ground.m_fraction < 1.f && !( ground.surface.m_flags & e_surf_type::surf_sky ) );
		point     = ground.m_fraction < 1.f ? ground.m_end : position;
		if ( record.m_path.empty( ) || !record.m_path.back( ).is_equal( point ) )
			record.m_path.push_back( point );
	}

	if ( !record.m_has_point || ( record.m_sim_velocity.length_squared( ) > 900.f && point.dist_to_squared( record.m_point ) > 64.f * 64.f ) )
		record.m_point = point;
	record.m_has_point  = has_point;
	record.m_suppressed = molotov && has_point && in_smoke( record.m_point );
}

void n_grenade_live::impl_t::update_records( )
{
	const int tick       = g_interfaces.m_global_vars_base->m_tick_count;
	const float interval = g_interfaces.m_global_vars_base->m_interval_per_tick;

	for ( auto it = this->m_records.begin( ); it != this->m_records.end( ); ) {
		const bool gone      = !still_live( *it );
		const bool expired   = it->m_kind != kind_ignored && tick > static_cast< int >( ( it->m_end + 1.f ) / interval + 0.5f );
		const bool detonated = !gone && ( it->m_kind == kind_he || it->m_kind == kind_molotov ) && exploded( it->m_entity );

		if ( gone ) {
			it = this->m_records.erase( it );
			continue;
		}

		if ( expired || detonated )
			it->m_kind = kind_ignored;
		++it;
	}

	g_entity_cache.enumerate( e_enumeration_type::type_edicts, [ & ]( c_base_entity* entity ) {
		if ( !entity || entity->is_dormant( ) )
			return;

		const int index = static_cast< int >( entity->get_index( ) );
		if ( index <= g_interfaces.m_global_vars_base->m_max_clients )
			return;

		const int class_id = class_of( entity );
		if ( class_id != static_cast< int >( e_class_ids::c_base_cs_grenade_projectile ) && class_id != static_cast< int >( e_class_ids::c_molotov_projectile ) &&
		     class_id != static_cast< int >( e_class_ids::c_inferno ) )
			return;

		for ( auto& record : this->m_records ) {
			if ( record.m_index != index || record.m_entity != entity || record.m_class != class_id )
				continue;

			if ( ( record.m_kind == kind_he || record.m_kind == kind_molotov ) &&
			     ( !record.m_sim_origin.is_equal( entity->get_origin( ) ) ||
			       !record.m_sim_velocity.is_equal( field< c_vector >( entity, offsets( ).m_velocity ) ) ) )
				this->simulate( record );
			return;
		}

		record_t record{ };
		record.m_entity = entity;
		record.m_index  = index;
		record.m_class  = class_id;
		if ( !this->admit( record, entity, class_id ) )
			record.m_kind = kind_ignored;
		this->m_records.push_back( std::move( record ) );
	} );
}

float n_grenade_live::impl_t::damage( record_t& record )
{
	const int tick = g_interfaces.m_global_vars_base->m_tick_count;
	if ( record.m_damage_tick == tick )
		return record.m_damage;
	record.m_damage_tick = tick;
	record.m_damage      = 0.f;

	c_base_entity* const local = g_ctx.m_local;
	const c_vector origin      = local->get_abs_origin( );

	if ( record.m_kind == kind_molotov ) {
		record.m_damage = record.m_point.dist_to( origin ) < k_fire_range ? 100.f : 0.f;
		return record.m_damage;
	}

	if ( record.m_kind == kind_inferno ) {
		float best = record.m_entity->get_abs_origin( ).dist_to( origin );
		const auto& o = offsets( );
		if ( o.m_fire_x && o.m_fire_count ) {
			const int count  = std::clamp( field< int >( record.m_entity, o.m_fire_count ), 0, 100 );
			const c_vector base = record.m_entity->get_abs_origin( );
			for ( int i = 0; i < count; ++i ) {
				if ( o.m_fire_burning && !field< bool >( record.m_entity, o.m_fire_burning + i ) )
					continue;
				const c_vector cell( base.m_x + static_cast< float >( field< int >( record.m_entity, o.m_fire_x + i * 4 ) ),
				                     base.m_y + static_cast< float >( field< int >( record.m_entity, o.m_fire_y + i * 4 ) ),
				                     base.m_z + static_cast< float >( field< int >( record.m_entity, o.m_fire_z + i * 4 ) ) );
				best = std::min( best, cell.dist_to( origin ) );
			}
		}
		record.m_damage = best < k_fire_range ? 100.f : 0.f;
		return record.m_damage;
	}

	const c_vector explosion = record.m_point;
	const float duck         = local->get_duck_amount( );
	c_vector distance_origin = origin;

	const c_vector velocity = local->get_velocity( );
	if ( velocity.m_x * velocity.m_x + velocity.m_y * velocity.m_y > 400.f ) {
		const float left = std::clamp( record.m_end - g_interfaces.m_global_vars_base->m_current_time, 0.2f, 1.6f );
		trace_t trace{ };
		c_trace_filter_world world{ };
		g_interfaces.m_engine_trace->trace_ray( ray_t( origin, origin + velocity * left ), e_contents::contents_solid, &world, &trace );
		if ( trace.m_end.dist_to_squared( explosion ) < origin.dist_to_squared( explosion ) )
			distance_origin = trace.m_end;
	}

	const c_vector distance_point = distance_origin + c_vector( 0.f, 0.f, view_height( duck ) * 0.85f );
	if ( distance_point.dist_to_squared( explosion ) >= k_he_range_sq )
		return 0.f;

	const float eye_height = view_height( duck );
	const float yaw        = local->get_abs_angles( ).m_y * std::numbers::pi_v< float > / 180.f;
	const float right_x = std::sinf( yaw ) * 16.f, right_y = -std::cosf( yaw ) * 16.f;
	const std::array< c_vector, 5 > samples{ origin + c_vector( 0.f, 0.f, eye_height ), origin + c_vector( 0.f, 0.f, 71.f ), origin,
		                                     c_vector( origin.m_x - right_x, origin.m_y - right_y, origin.m_z + eye_height ),
		                                     c_vector( origin.m_x + right_x, origin.m_y + right_y, origin.m_z + eye_height ) };

	float best           = exposure( explosion, samples, local );
	c_vector best_sample = explosion;

	if ( best < 1.f ) {
		constexpr float k_directions[ 13 ][ 3 ] = { { 1, 0, 0 },  { -1, 0, 0 }, { 0, 1, 0 },   { 0, -1, 0 },  { 0, 0, 1 },  { 0, 0, -1 }, { 1, 1, 1 },
			                                        { 1, -1, 1 }, { -1, 1, 1 }, { -1, -1, 1 }, { 1, 1, -1 }, { 1, -1, -1 }, { -1, -1, -1 } };
		c_trace_filter no_players( []( c_base_entity* hit, int ) { return !hit->is_player( ); } );

		for ( const auto& direction : k_directions ) {
			const c_vector end = explosion + unit( c_vector( direction[ 0 ], direction[ 1 ], direction[ 2 ] ) ) * 8.f;
			trace_t trace{ };
			g_interfaces.m_engine_trace->trace_ray( ray_t( explosion, end ), 0x42004003, &no_players, &trace );

			c_vector candidate = trace.m_end;
			if ( trace.m_fraction < 1.f || trace.m_all_solid || trace.m_start_solid )
				candidate = trace.m_end + ( explosion - trace.m_end ) * 0.05f;

			const float next = exposure( candidate, samples, local );
			if ( next > best ) {
				best        = next;
				best_sample = candidate;
			}
		}
	}

	if ( best <= 0.f )
		return 0.f;

	const float distance = distance_point.dist_to( best_sample );
	float result         = std::expf( -distance * distance / 27222.2207f ) * 99.f * best;
	if ( local->get_armor( ) > 0 )
		result *= 0.6f;

	record.m_damage = result * team_scale( thrower_of( record.m_entity, kind_he ), kind_he );
	return record.m_damage;
}

void n_grenade_live::impl_t::draw_warning( record_t& record )
{
	c_base_entity* const local = g_ctx.m_local;
	const c_vector origin      = local->get_abs_origin( );

	float distance = record.m_point.dist_to( origin );
	if ( record.m_kind == kind_inferno ) {
		const auto& o = offsets( );
		const c_vector base = record.m_entity->get_abs_origin( );
		distance            = base.dist_to( origin );
		if ( o.m_fire_x && o.m_fire_count ) {
			const int count = std::clamp( field< int >( record.m_entity, o.m_fire_count ), 0, 100 );
			for ( int i = 0; i < count; ++i ) {
				const c_vector cell( base.m_x + static_cast< float >( field< int >( record.m_entity, o.m_fire_x + i * 4 ) ),
				                     base.m_y + static_cast< float >( field< int >( record.m_entity, o.m_fire_y + i * 4 ) ),
				                     base.m_z + static_cast< float >( field< int >( record.m_entity, o.m_fire_z + i * 4 ) ) );
				distance = std::min( distance, cell.dist_to( origin ) );
			}
		}
	}

	if ( distance > k_warning_range )
		return;

	const auto& matrix = g_interfaces.m_engine_client->get_world_to_screen_matrix( );
	const c_vector& p  = record.m_point;
	const float w      = matrix[ 3 ][ 0 ] * p.m_x + matrix[ 3 ][ 1 ] * p.m_y + matrix[ 3 ][ 2 ] * p.m_z + matrix[ 3 ][ 3 ];
	if ( w == 0.f )
		return;

	const float inverse = 1.f / std::fabsf( w );
	float x = ( matrix[ 0 ][ 0 ] * p.m_x + matrix[ 0 ][ 1 ] * p.m_y + matrix[ 0 ][ 2 ] * p.m_z + matrix[ 0 ][ 3 ] ) * inverse;
	float y = ( matrix[ 1 ][ 0 ] * p.m_x + matrix[ 1 ][ 1 ] * p.m_y + matrix[ 1 ][ 2 ] * p.m_z + matrix[ 1 ][ 3 ] ) * inverse;

	const float width = g_ctx.m_width, height = g_ctx.m_height;
	x = width * 0.5f + x * width * 0.5f;
	y = w < 0.f ? height : height * 0.5f - y * height * 0.5f;
	x = g_render.to_overlay_x( std::clamp( x, 50.f, std::max( 50.f, width - 50.f ) ) );
	y = std::clamp( y, 50.f, std::max( 50.f, height - 50.f ) );

	const float radius = std::roundf( 50.f - ( std::clamp( distance, 36.f, 200.f ) - 36.f ) * 20.f / 164.f );

	const float dmg  = this->damage( record );
	const int health = local->get_health( );
	float fraction   = -1.f;
	if ( dmg > 0.f && health > 0 )
		fraction = std::clamp( dmg / static_cast< float >( std::min( 50, health ) ), 0.f, 1.f );

	const float now      = ( g_interfaces.m_global_vars_base->m_tick_count - 1 + g_interfaces.m_global_vars_base->m_interpolation_amount ) *
	                  g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float span     = record.m_end - record.m_creation;
	const float progress = span > 0.f && span < 8.f ? std::clamp( ( now - record.m_creation ) / span, 0.f, 1.f ) : -1.f;

	const bool he = record.m_kind == kind_he;
	const auto glyph = g_utilities.get_weapon_icon( he ? e_item_definition_index::weapon_hegrenade : e_item_definition_index::weapon_molotov );
	std::string icon = glyph ? reinterpret_cast< const char* >( glyph ) : "";
	std::string number =
		he ? std::to_string( static_cast< int >( dmg ) ) : std::to_string( static_cast< int >( std::roundf( distance / 12.f ) ) );

	ImFont* const icon_font = g_render.m_fonts[ e_font_names::font_name_cod4_weapon_icon_48 ];
	ImFont* const text_font = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];

	const auto paint = [ = ]( ImDrawList* list ) {
		const ImVec2 center( x, y );
		list->AddCircleFilled( center, radius, IM_COL32( 0, 0, 0, 210 ), 62 );

		if ( fraction >= 0.f ) {
			const float t     = std::sqrtf( fraction );
			const ImU32 inner = hsv_lerp_red( t, fraction >= 1.f ? 190 : 255 );
			const ImU32 outer = inner & ~IM_COL32_A_MASK;
			const float r     = radius - 4.f;
			constexpr int segments = 62;
			list->PrimReserve( segments * 3, segments * 3 );
			const ImVec2 uv = ImGui::GetFontTexUvWhitePixel( );
			for ( int i = 0; i < segments; ++i ) {
				const float a0 = 2.f * std::numbers::pi_v< float > * i / segments;
				const float a1 = 2.f * std::numbers::pi_v< float > * ( i + 1 ) / segments;
				list->PrimWriteVtx( ImVec2( x + std::cosf( a0 ) * r, y + std::sinf( a0 ) * r ), uv, outer );
				list->PrimWriteVtx( ImVec2( x + std::cosf( a1 ) * r, y + std::sinf( a1 ) * r ), uv, outer );
				list->PrimWriteVtx( center, uv, inner );
				const ImDrawIdx base = static_cast< ImDrawIdx >( list->_VtxCurrentIdx - 3 );
				list->PrimWriteIdx( base );
				list->PrimWriteIdx( static_cast< ImDrawIdx >( base + 1 ) );
				list->PrimWriteIdx( static_cast< ImDrawIdx >( base + 2 ) );
			}
		}

		if ( progress >= 0.f && progress < 1.f ) {
			list->PathArcTo( center, radius - 1.f, 0.f, ( 1.f - progress ) * 2.f * std::numbers::pi_v< float >, 62 );
			list->PathStroke( IM_COL32( 255, 255, 255, 200 ), 0, 2.f );
		}

		float icon_height = 20.f;
		ImFont* const ifont = g_render.live_font( icon_font );
		if ( ifont && !icon.empty( ) ) {
			const ImVec2 size = ifont->CalcTextSizeA( 20.f, FLT_MAX, 0.f, icon.c_str( ) );
			icon_height       = size.y;
			list->AddText( ifont, 20.f, ImVec2( std::floorf( x - size.x * 0.5f ), std::floorf( y - 10.f - size.y * 0.5f ) ), IM_COL32( 255, 255, 255, 200 ),
			               icon.c_str( ) );
		}

		ImFont* const tfont = g_render.live_font( text_font );
		if ( tfont ) {
			const ImVec2 size = tfont->CalcTextSizeA( tfont->FontSize, FLT_MAX, 0.f, number.c_str( ) );
			list->AddText( tfont, tfont->FontSize, ImVec2( std::floorf( x - size.x * 0.5f ), std::floorf( y + icon_height * 0.5f - size.y * 0.5f ) ),
			               IM_COL32( 255, 255, 255, 200 ), number.c_str( ) );
		}
	};

	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_callback, std::make_any< callback_draw_object_t >( callback_draw_object_t{ paint } ) );
}

void n_grenade_live::impl_t::on_paint_traverse( )
{
	const bool warning = GET_VARIABLE( g_variables.m_grenade_warning, bool );
	const bool paths   = GET_VARIABLE( g_variables.m_grenade_trajectories, bool );

	if ( ( !warning && !paths ) || !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) ) {
		this->m_records.clear( );
		return;
	}

	this->update_records( );

	if ( !warning || !g_ctx.m_local->is_alive( ) )
		return;

	for ( auto& record : this->m_records )
		if ( record.m_kind != kind_ignored && record.m_threat && record.m_has_point && !record.m_suppressed )
			this->draw_warning( record );
}

void n_grenade_live::impl_t::on_post_screen_space_effects( const c_view_setup* setup )
{
	if ( !setup || !GET_VARIABLE( g_variables.m_grenade_trajectories, bool ) || this->m_records.empty( ) || !g_interfaces.m_material_system )
		return;

	if ( !this->m_material ) {
		if ( !c_key_values::usable( ) )
			return;

		auto* key_values = new c_key_values( "Sprite" );
		key_values->load_from_buffer( "botox_grenade_ribbon", R"("Sprite"
{
	"$basetexture"      "particle/beam_taser"
	"$spriterendermode" "5"
	"$vertexcolor"      "1"
	"$vertexalpha"      "1"
	"$nocull"           "1"
	"$translucent"      "1"
})" );

		this->m_material = g_interfaces.m_material_system->create_material( "botox_grenade_ribbon", key_values );
		if ( !this->m_material ) {
			delete key_values;
			return;
		}
		this->m_material->increment_reference_count( );
	}

	const c_color color = GET_VARIABLE( g_variables.m_grenade_trajectories_color, c_color );
	const std::uint32_t bgra = ( static_cast< std::uint32_t >( color[ 3 ] ) << 24 ) | ( static_cast< std::uint32_t >( color[ 0 ] ) << 16 ) |
	                           ( static_cast< std::uint32_t >( color[ 1 ] ) << 8 ) | static_cast< std::uint32_t >( color[ 2 ] );

	c_material_render_context* const context = g_interfaces.m_material_system->get_render_context( );
	if ( !context )
		return;
	context->begin_render( );

	std::vector< c_vector > points{ };
	std::vector< ribbon_vertex_t > vertices{ };

	for ( const auto& record : this->m_records ) {
		/* entities freed at net update since paint traverse queued the record */
		if ( ( record.m_kind != kind_he && record.m_kind != kind_molotov ) || record.m_suppressed || record.m_path.empty( ) || !still_live( record ) ||
		     exploded( record.m_entity ) || ( record.m_entity->get_effects( ) & 0x20 ) )
			continue;

		points.clear( );
		points.push_back( record.m_entity->get_abs_origin( ) );
		points.insert( points.end( ), record.m_path.begin( ), record.m_path.end( ) );
		if ( points.size( ) <= 2 )
			continue;

		build_ribbon( points, setup->m_origin, 10.f, vertices );

		void* const mesh = context->get_dynamic_mesh( true, nullptr, nullptr, this->m_material );
		if ( !mesh )
			continue;

		const int count = static_cast< int >( vertices.size( ) );
		g_virtual.call< void >( mesh, 10, 3 );

		mesh_desc_t desc{ };
		g_virtual.call< void >( mesh, 17, count, count, &desc, nullptr );

		for ( int i = 0; i < count; ++i ) {
			const auto& vertex   = vertices[ i ];
			const float uv[ 2 ] = { vertex.m_u, 0.f };
			write_channel( desc, 0, i, &vertex.m_position, sizeof( float ) * 3 );
			write_channel( desc, 4, i, &bgra, sizeof( bgra ) );
			write_channel( desc, 6, i, uv, sizeof( uv ) );
			write_channel( desc, 7, i, uv, sizeof( uv ) );
			write_channel( desc, 14, i, &vertex.m_tangent_s, sizeof( float ) * 3 );
			write_channel( desc, 15, i, &vertex.m_tangent_t, sizeof( float ) * 3 );
		}

		int written = 0;
		if ( desc.m_index_size && desc.m_indices ) {
			for ( int i = 0; i < count; ++i ) {
				desc.m_indices[ written ] = static_cast< unsigned short >( desc.m_first_vertex + i );
				written += desc.m_index_size;
			}
		}

		g_virtual.call< void >( mesh, 20, count, written, &desc );
		g_virtual.call< void >( mesh, 12, -1, 0 );
	}

	context->end_render( );
	context->release( );
}
