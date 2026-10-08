#pragma once

#include <atomic>
#include <d3d9.h>

namespace n_weapon_sheen
{
	using draw_indexed_primitive_t = long( __stdcall* )( IDirect3DDevice9*, D3DPRIMITIVETYPE, int, unsigned int, unsigned int, unsigned int, unsigned int );

	struct params_t {
		int m_frame        = 0;
		float m_tint[ 4 ]  = { };
		float m_u[ 4 ]     = { };
		float m_v[ 4 ]     = { };
		float m_band[ 4 ]  = { };
	};

	struct impl_t {
		// main thread, around the carrier redraw of a v_ weapon in draw_model_execute
		bool begin( const char* model_name );
		void end( );

		bool armed( ) const
		{
			return this->m_armed.load( std::memory_order_relaxed );
		}

		// render thread, from the DrawIndexedPrimitive detour while armed
		long draw( draw_indexed_primitive_t original, IDirect3DDevice9* device, D3DPRIMITIVETYPE type, int base_vertex, unsigned int min_vertex,
		           unsigned int vertices, unsigned int start_index, unsigned int primitives );

		void on_device_lost( );
		void release_material( );

		std::atomic< bool > m_armed{ false };
		params_t m_params{ };
		float m_next_start = 0.f;
	};
}

inline n_weapon_sheen::impl_t g_weapon_sheen{ };
