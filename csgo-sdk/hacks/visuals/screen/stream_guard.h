#pragma once
#include <d3d9.h>

namespace n_stream_guard
{
	struct state_t {
		void capture( IDirect3DDevice9* device )
		{
			if ( !device )
				return;

			if ( FAILED( device->GetStreamSource( 0, &this->m_stream, &this->m_offset, &this->m_stride ) ) )
				this->m_stream = nullptr;

			if ( FAILED( device->GetIndices( &this->m_indices ) ) )
				this->m_indices = nullptr;

			this->m_captured = true;
		}

		void restore( IDirect3DDevice9* device )
		{
			if ( !device || !this->m_captured )
				return;

			device->SetStreamSource( 0, this->m_stream, this->m_offset, this->m_stride );
			device->SetIndices( this->m_indices );

			if ( this->m_stream ) {
				this->m_stream->Release( );
				this->m_stream = nullptr;
			}

			if ( this->m_indices ) {
				this->m_indices->Release( );
				this->m_indices = nullptr;
			}

			this->m_captured = false;
		}

	private:
		IDirect3DVertexBuffer9* m_stream = nullptr;
		IDirect3DIndexBuffer9* m_indices = nullptr;
		unsigned int m_offset            = 0;
		unsigned int m_stride            = 0;
		bool m_captured                  = false;
	};
}
