#pragma once
#include <d3d9.h>

namespace n_frame_copy
{
	inline bool s_direct_failed = false;

	// msaa frame -> rt texture level is a legal resolve, one blit instead of two. hop = old two blit route, kept as fallback
	inline HRESULT copy( IDirect3DDevice9* device, IDirect3DSurface9* frame, IDirect3DSurface9* hop, IDirect3DSurface9* destination )
	{
		if ( !hop || !s_direct_failed ) {
			const HRESULT direct = device->StretchRect( frame, nullptr, destination, nullptr, D3DTEXF_NONE );

			if ( SUCCEEDED( direct ) || !hop )
				return direct;

			s_direct_failed = true;
		}

		const HRESULT resolved = device->StretchRect( frame, nullptr, hop, nullptr, D3DTEXF_NONE );

		return FAILED( resolved ) ? resolved : device->StretchRect( hop, nullptr, destination, nullptr, D3DTEXF_NONE );
	}
}
