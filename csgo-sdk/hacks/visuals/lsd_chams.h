#pragma once

#include <atomic>
#include <d3d9.h>

namespace n_lsd_chams
{
	using draw_indexed_primitive_t = long( __stdcall* )( IDirect3DDevice9*, D3DPRIMITIVETYPE, int, unsigned int, unsigned int, unsigned int, unsigned int );

	struct impl_t {
		// main thread, around one chams layer draw (lsd slot material already forced)
		void begin( float alpha );
		void end( );

		bool armed( ) const
		{
			return this->m_armed.load( std::memory_order_relaxed );
		}

		// render thread, from the DrawIndexedPrimitive detour while armed
		long draw( draw_indexed_primitive_t original, IDirect3DDevice9* device, D3DPRIMITIVETYPE type, int base_vertex, unsigned int min_vertex,
		           unsigned int vertices, unsigned int start_index, unsigned int primitives );

		void on_device_lost( );

		std::atomic< bool > m_armed{ false };
		float m_params[ 4 ] = { };
	};
}

inline n_lsd_chams::impl_t g_lsd_chams{ };
