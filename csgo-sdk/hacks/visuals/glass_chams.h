#pragma once

#include <atomic>
#include <d3d9.h>

// glass chams: model's own materials stay bound (normal map), DIP swaps in the glass shaders over a copy of the bound rt
namespace n_glass_chams
{
	using draw_indexed_primitive_t = long( __stdcall* )( IDirect3DDevice9*, D3DPRIMITIVETYPE, int, unsigned int, unsigned int, unsigned int, unsigned int );

	struct impl_t {
		// main thread, around one chams layer draw with no override. tint 0..1 display, alpha pre-scaled
		void begin( const float tint[ 4 ], bool ignorez, bool lay_depth = true );
		void end( );

		bool armed( ) const
		{
			return this->m_armed.load( std::memory_order_relaxed );
		}

		// render thread, from the DrawIndexedPrimitive detour while armed
		long draw( draw_indexed_primitive_t original, IDirect3DDevice9* device, D3DPRIMITIVETYPE type, int base_vertex, unsigned int min_vertex,
		           unsigned int vertices, unsigned int start_index, unsigned int primitives );

		void on_reset( );       // frame copies (DEFAULT pool), shaders survive Reset
		void on_device_lost( ); // + shaders

		std::atomic< bool > m_armed{ false };
		float m_tint[ 4 ]   = { };
		float m_refract     = 0.f;
		float m_bump        = 0.f;
		bool m_ignorez      = false;
		bool m_translucent  = false;
		bool m_lay_depth    = true;
		bool m_copy_pending = false; // first dip of each layer copies the rt, later meshes share it
	};
}

inline n_glass_chams::impl_t g_glass_chams{ };
