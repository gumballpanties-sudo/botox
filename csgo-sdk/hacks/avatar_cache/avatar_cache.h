#pragma once
#include <array>
class c_base_entity;
struct IDirect3DTexture9;

namespace n_avatar_cache
{
	struct impl_t {
		IDirect3DTexture9* operator[]( const int index );

		/* still one of ours: a game thread queued draw holds a raw pointer that think( ) may have freed */
		bool live( const void* texture ) const;

		void on_add_entity( c_base_entity* entity );
		void on_remove_entity( c_base_entity* entity );

		/* render thread, once a frame: steam fetch + every texture free */
		void think( );

		/* device reset, render thread: frees NOW. a DEFAULT-pool avatar (d3d9ex) must be gone before
		   Reset or the reset fails */
		void release_all( );

		void reset( );

	private:
		void release( const int index );

		std::array< IDirect3DTexture9*, 64 > m_cached_avatars{ };
		/* owner of each slot's picture (reused index must not keep the leaver's face) */
		std::array< unsigned long long, 64 > m_steam_ids{ };
		std::array< unsigned long long, 64 > m_next_try{ };
		/* steam image handle the texture was built from. steam hands out its grey "?" first and the real
		   face later under a NEW handle, so a cached texture is re-checked, never final */
		std::array< int, 64 > m_handles{ };
		/* set on the GAME thread, handled by think( ) on the render thread */
		std::array< bool, 64 > m_pending_release{ };
	};
}

inline n_avatar_cache::impl_t g_avatar_cache{ };
