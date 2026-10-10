#include "../hooks.h"

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

#include <ranges>

#define PARTICLE_ATTRIBUTE_TINT_RGB 6
#define PARTICLE_ATTRIBUTE_ALPHA    7

#define MAX_PARTICLE_ATTRIBUTES 24

struct c_particle_attribute_address_table {
	float* m_attributes[ MAX_PARTICLE_ATTRIBUTES ];
	unsigned int m_float_strides[ MAX_PARTICLE_ATTRIBUTES ];

	float* float_attribute( int attribute, int particle_number ) const
	{
		int blockofs = particle_number / 4;
		return m_attributes[ attribute ] + m_float_strides[ attribute ] * blockofs + ( particle_number & 3 );
	}

	void modulate_color( const c_color& color, int num )
	{
		auto rgb = this->float_attribute( PARTICLE_ATTRIBUTE_TINT_RGB, num );
		auto a   = this->float_attribute( PARTICLE_ATTRIBUTE_ALPHA, num );

		rgb[ 0 ] = color.base< e_color_type::color_type_r >( );
		rgb[ 4 ] = color.base< e_color_type::color_type_g >( );
		rgb[ 8 ] = color.base< e_color_type::color_type_b >( );

		*a = color.base< e_color_type::color_type_a >( );
	}

};

class c_particle_system_definition
{
public:
	unsigned char pad0[ 308 ];
	c_utl_string m_name;
};

class c_particle_collection
{
public:
	unsigned char pad0[ 48 ];
	int m_active_particles;
	unsigned char pad1[ 12 ];
	c_utl_reference< c_particle_system_definition > m_def;
	unsigned char pad2[ 60 ];
	c_particle_collection* m_parent;
	unsigned char pad3[ 84 ];
	c_particle_attribute_address_table m_particle_attributes;
};

void __fastcall n_detoured_functions::particle_collection_simulate( void* ecx, void* edx )
{
	static auto original = g_hooks.m_particle_collection_simulate.get_original< void( __fastcall* )( void* ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx ) );

	original( ecx );

	const bool want_precipitation = GET_VARIABLE( g_variables.m_custom_precipitation, bool );
	const bool want_smoke         = GET_VARIABLE( g_variables.m_custom_smoke, bool );
	const bool want_molotov       = GET_VARIABLE( g_variables.m_custom_molotov, bool );
	const bool want_blood         = GET_VARIABLE( g_variables.m_custom_blood, bool );

	if ( !want_precipitation && !want_smoke && !want_molotov && !want_blood )
		return;

	const auto particle_collection = reinterpret_cast< c_particle_collection* >( ecx );

	c_particle_collection* root = reinterpret_cast< c_particle_collection* >( ecx );
	while ( root->m_parent )
		root = root->m_parent;

	if ( !root->m_def.m_obj || !root->m_def.m_obj->m_name.m_buffer )
		return;

	const auto hash = HASH_RT( root->m_def.m_obj->m_name.m_buffer );

	const auto tint_all = [ & ]( const c_color& color ) {
		for ( auto iterator : std::views::iota( 0, particle_collection->m_active_particles ) )
			particle_collection->m_particle_attributes.modulate_color( color, iterator );
	};

	if ( want_precipitation && ( hash == HASH_BT( "rain" ) || hash == HASH_BT( "rain_storm" ) || hash == HASH_BT( "snow" ) || hash == HASH_BT( "ash" ) ) )
		tint_all( GET_VARIABLE( g_variables.m_custom_precipitation_color, c_color ) );

	if ( want_smoke && ( hash == HASH_BT( "explosion_smokegrenade" ) || hash == HASH_BT( "explosion_smokegrenade_fallback" ) ) )
		tint_all( GET_VARIABLE( g_variables.m_custom_smoke_color, c_color ) );

	if ( want_molotov &&
	     ( hash == HASH_BT( "explosion_molotov_air" ) || hash == HASH_BT( "extinguish_fire" ) || hash == HASH_BT( "molotov_groundfire" ) ||
	       hash == HASH_BT( "molotov_groundfire_fallback" ) || hash == HASH_BT( "molotov_groundfire_fallback2" ) ||
	       hash == HASH_BT( "molotov_explosion" ) || hash == HASH_BT( "weapon_molotov_held" ) || hash == HASH_BT( "weapon_molotov_fp" ) ||
	       hash == HASH_BT( "weapon_molotov_thrown" ) ) )
		tint_all( GET_VARIABLE( g_variables.m_custom_molotov_color, c_color ) );

	if ( want_blood && ( hash == HASH_BT( "blood_impact_light" ) || hash == HASH_BT( "blood_impact_medium" ) ||
	                     hash == HASH_BT( "blood_impact_heavy" ) || hash == HASH_BT( "blood_impact_light_headshot" ) ) )
		tint_all( GET_VARIABLE( g_variables.m_custom_blood_color, c_color ) );
}
