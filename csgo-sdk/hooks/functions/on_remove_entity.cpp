#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

#include "../../hacks/entity_cache/entity_cache.h"
#include "../../hacks/avatar_cache/avatar_cache.h"

void __fastcall n_detoured_functions::on_remove_entity( void* ecx, void* edx, void* handle_entity, unsigned int entity_handle )
{
	static auto original = g_hooks.m_on_remove_entity.get_original< decltype( &n_detoured_functions::on_remove_entity ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, handle_entity, entity_handle ) );

	const auto entity = reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( entity_handle ) );

	g_entity_cache.on_remove_entity( entity );
	g_avatar_cache.on_remove_entity( entity );

	original( ecx, edx, handle_entity, entity_handle );
}