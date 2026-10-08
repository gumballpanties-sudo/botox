#include "avatar_cache.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

#include <algorithm>

IDirect3DTexture9* n_avatar_cache::impl_t::operator[]( const int index )
{
	if ( index < 0 || index >= static_cast< int >( this->m_cached_avatars.size( ) ) )
		return nullptr;

	return this->m_cached_avatars[ index ];
}

bool n_avatar_cache::impl_t::live( const void* texture ) const
{
	return texture && std::find( this->m_cached_avatars.begin( ), this->m_cached_avatars.end( ), texture ) != this->m_cached_avatars.end( );
}

void n_avatar_cache::impl_t::release( const int index )
{
	if ( index < 0 || index >= static_cast< int >( this->m_cached_avatars.size( ) ) )
		return;

	if ( this->m_cached_avatars[ index ] != nullptr ) {
		this->m_cached_avatars[ index ]->Release( );
		this->m_cached_avatars[ index ] = nullptr;
	}

	this->m_steam_ids[ index ] = 0ull;
	this->m_next_try[ index ]  = 0ull;
	this->m_handles[ index ]   = 0;
}

void n_avatar_cache::impl_t::on_add_entity( c_base_entity* entity )
{
	if ( !entity )
		return;

	const int index = entity->get_index( );

	if ( !( index > 0 && index < static_cast< int >( this->m_cached_avatars.size( ) ) ) )
		return;

	/* arm only; think( ) does the rest on the render thread */
	this->m_next_try[ index ] = 0ull;
}

void n_avatar_cache::impl_t::on_remove_entity( c_base_entity* entity )
{
	if ( !entity )
		return;

	const int index = entity->get_index( );

	/* no get_player_info gate: info is gone on disconnect (leaked a texture). release by index alone */
	if ( !( index > 0 && index < static_cast< int >( this->m_cached_avatars.size( ) ) ) )
		return;

	this->m_pending_release[ index ] = true;
}

void n_avatar_cache::impl_t::think( )
{
	const int slots = static_cast< int >( this->m_cached_avatars.size( ) );

	for ( int index = 0; index < slots; index++ ) {
		if ( !this->m_pending_release[ index ] )
			continue;

		this->m_pending_release[ index ] = false;

		this->release( index );
	}

	if ( !SteamFriends || !SteamUtils || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	static unsigned long long next_scan = 0ull;

	const unsigned long long now = GetTickCount64( );

	if ( now < next_scan )
		return;

	next_scan = now + 250ull;

	const int max_clients = g_interfaces.m_engine_client->get_max_clients( );

	for ( int index = 1; index < slots; index++ ) {
		player_info_t player_info{ };

		if ( index > max_clients || !g_interfaces.m_engine_client->get_player_info( index, &player_info ) ) {
			this->release( index );
			continue;
		}

		if ( player_info.m_fake_player || player_info.m_ull_xuid == 0ull ) {
			this->release( index );
			continue;
		}

		if ( this->m_steam_ids[ index ] != player_info.m_ull_xuid ) {
			this->release( index );

			this->m_steam_ids[ index ] = player_info.m_ull_xuid;
		}

		if ( now < this->m_next_try[ index ] )
			continue;

		this->m_next_try[ index ] = now + 1000ull;

		const CSteamID steam_id( static_cast< uint64 >( player_info.m_ull_xuid ) );

		if ( this->m_cached_avatars[ index ] == nullptr && SteamFriends->RequestUserInformation( steam_id, false ) )
			continue;

		const int handle = SteamFriends->GetSmallFriendAvatar( steam_id );
		if ( handle <= 0 || ( this->m_cached_avatars[ index ] != nullptr && handle == this->m_handles[ index ] ) )
			continue;

		if ( this->m_cached_avatars[ index ] != nullptr ) {
			this->m_cached_avatars[ index ]->Release( );
			this->m_cached_avatars[ index ] = nullptr;
		}

		const int old_handle            = this->m_handles[ index ];
		this->m_cached_avatars[ index ] = g_render.steam_image( steam_id );
		this->m_handles[ index ]        = this->m_cached_avatars[ index ] ? handle : 0;

		g_console.print( std::format( "[avatar] idx {} '{}' xuid {} handle {} -> {}{}", index, player_info.m_name, player_info.m_ull_xuid, old_handle,
		                              handle, this->m_cached_avatars[ index ] ? "" : " NO TEXTURE" )
		                     .c_str( ) );
	}
}

void n_avatar_cache::impl_t::release_all( )
{
	for ( int index = 0; index < static_cast< int >( this->m_cached_avatars.size( ) ); index++ ) {
		this->m_pending_release[ index ] = false;

		this->release( index );
	}
}

void n_avatar_cache::impl_t::reset( )
{
	for ( int index = 0; index < static_cast< int >( this->m_cached_avatars.size( ) ); index++ ) {
		if ( this->m_cached_avatars[ index ] != nullptr )
			this->m_pending_release[ index ] = true;

		this->m_steam_ids[ index ] = 0ull;
		this->m_next_try[ index ]  = 0ull;
	}
}
