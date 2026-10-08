#include "convars.h"

#include "../../game/sdk/classes/c_cconvar.h"
#include "../../game/sdk/classes/c_con_base.h"
#include "../../game/sdk/classes/c_convar.h"
#include "../../game/sdk/enums/e_convar_flag.h"
#include "../interfaces/interfaces.h"

#include "../macros/macros.h"
#include <unordered_map>

std::unordered_map< unsigned int, c_cconvar* > convars = { };

static void scan_convar_list( )
{
	c_con_base* iterator = **reinterpret_cast< c_con_base*** >( reinterpret_cast< unsigned long >( g_interfaces.m_convar ) + 0x34 );
	if ( !iterator )
		return;

	for ( auto c = iterator->m_next; c; c = c->m_next ) {
		c->m_flags &= ~( fcvar_cheat | fcvar_developmentonly | fcvar_hidden );

		convars[ HASH_RT( c->m_name ) ] = reinterpret_cast< c_cconvar* >( c->m_accessor );
	}
}

bool n_convars::impl_t::on_attach( )
{
	scan_convar_list( );

	return !( convars.empty( ) );
}

void n_convars::impl_t::rescan( )
{
	scan_convar_list( );
}

int n_convars::impl_t::int_or( unsigned int hash, int fallback )
{
	c_cconvar* convar = ( *this )[ hash ];

	return convar ? convar->get_int( ) : fallback;
}

float n_convars::impl_t::float_or( unsigned int hash, float fallback )
{
	c_cconvar* convar = ( *this )[ hash ];

	return convar ? convar->get_float( ) : fallback;
}

void n_convars::impl_t::set_if_present( unsigned int hash, float value )
{
	if ( c_cconvar* convar = ( *this )[ hash ] )
		convar->set_value( value );
}

c_cconvar* n_convars::impl_t::operator[]( unsigned int hash )
{
	if ( const auto it = convars.find( hash ); it != convars.end( ) && it->second )
		return it->second;

	static int misses = 0;

	if ( ++misses < 64 )
		return nullptr;

	misses = 0;

	scan_convar_list( );

	const auto it = convars.find( hash );

	return it != convars.end( ) ? it->second : nullptr;
}
