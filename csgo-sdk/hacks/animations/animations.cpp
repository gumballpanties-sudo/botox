#include "animations.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../entity_cache/entity_cache.h"

void n_animations::impl_t::on_frame_stage_notify( int stage )
{
	[ & ]( ) {
		if ( stage == e_client_frame_stage::render_start ) {
			g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
				if ( !entity->is_valid_player( ) )
					return;

				*reinterpret_cast< int* >( reinterpret_cast< unsigned int >( entity ) + 0xA30 ) =
					g_interfaces.m_global_vars_base->m_frame_count;
				*reinterpret_cast< int* >( reinterpret_cast< unsigned int >( entity ) + 0xA28 ) = 0;
			} );
		}
	}( );
}
