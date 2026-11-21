#pragma once
#include <d3d9.h>

/* one frame copy shared by every screen pass ( bloom, cc, dof, depth view, depth warp, ssr, mblur, flip ). d3d thread only.
   passes run one after another and none reads its copy once it returns ( cc's web restore runs right after cc ), so one
   copy serves all: was one full frame texture + one msaa resolve surface per pass.
   two slots: cc copies the back buffer, the rest the scene target, sizes differ under the resolution spoofer.
   msaa: frame -> rt texture level is a legal resolve, one blit. the old two blit hop is only made if the driver refuses it */
namespace n_frame_copy
{
	struct slot_t {
		IDirect3DTexture9* m_texture = nullptr;
		IDirect3DSurface9* m_surface = nullptr;
		UINT m_width                 = 0;
		UINT m_height                = 0;
		D3DFORMAT m_format           = D3DFMT_UNKNOWN;
		unsigned long long m_order   = 0;
		unsigned long long m_tick    = 0;
	};

	inline slot_t s_slots[ 2 ]{ };
	inline IDirect3DSurface9* s_hop       = nullptr;
	inline unsigned long long s_hop_tick  = 0;
	inline unsigned long long s_order     = 0;
	inline bool s_direct_failed           = false;

	inline constexpr unsigned long long k_idle_ms = 1000;

	inline void drop( slot_t& slot )
	{
		if ( slot.m_surface )
			slot.m_surface->Release( );

		if ( slot.m_texture )
			slot.m_texture->Release( );

		slot = { };
	}

	inline void drop_hop( )
	{
		if ( s_hop )
			s_hop->Release( );

		s_hop = nullptr;
	}

	// d3d thread, before Reset + at eject: everything here is D3DPOOL_DEFAULT
	inline void release( )
	{
		for ( slot_t& slot : s_slots )
			drop( slot );

		drop_hop( );
	}

	// once a frame: every pass off for a second = memory back
	inline void trim( )
	{
		const unsigned long long now = GetTickCount64( );

		for ( slot_t& slot : s_slots )
			if ( slot.m_texture && now - slot.m_tick > k_idle_ms )
				drop( slot );

		if ( s_hop && now - s_hop_tick > k_idle_ms )
			drop_hop( );
	}

	inline HRESULT copy( IDirect3DDevice9* device, IDirect3DSurface9* frame, const D3DSURFACE_DESC& description, IDirect3DSurface9* destination )
	{
		const bool msaa = description.MultiSampleType != D3DMULTISAMPLE_NONE;

		if ( !msaa || !s_direct_failed ) {
			const HRESULT direct = device->StretchRect( frame, nullptr, destination, nullptr, D3DTEXF_NONE );

			if ( SUCCEEDED( direct ) || !msaa )
				return direct;

			s_direct_failed = true;
		}

		// ponytail: one hop. driver refusing the direct resolve + two msaa sizes a frame ( spoofer ) = recreate per size change. hop per slot if seen
		D3DSURFACE_DESC hop{ };

		if ( s_hop && ( FAILED( s_hop->GetDesc( &hop ) ) || hop.Width != description.Width || hop.Height != description.Height ||
		                hop.Format != description.Format ) )
			drop_hop( );

		if ( !s_hop ) {
			const HRESULT made = device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
			                                                 &s_hop, nullptr );

			if ( FAILED( made ) ) {
				s_hop = nullptr;
				return made;
			}
		}

		s_hop_tick = GetTickCount64( );

		const HRESULT resolved = device->StretchRect( frame, nullptr, s_hop, nullptr, D3DTEXF_NONE );

		return FAILED( resolved ) ? resolved : device->StretchRect( s_hop, nullptr, destination, nullptr, D3DTEXF_NONE );
	}

	// glass chams keeps its own copies ( read across draws ) and only borrows the hop through copy( )
	// copy of frame ( described by description ), valid until the next grab. nullptr = failed, result says why
	inline IDirect3DTexture9* grab( IDirect3DDevice9* device, IDirect3DSurface9* frame, const D3DSURFACE_DESC& description, HRESULT* result = nullptr )
	{
		HRESULT local    = E_FAIL;
		HRESULT& outcome = result ? *result : local;

		slot_t* slot = nullptr;

		for ( slot_t& candidate : s_slots )
			if ( candidate.m_texture && candidate.m_width == description.Width && candidate.m_height == description.Height &&
			     candidate.m_format == description.Format )
				slot = &candidate;

		if ( !slot ) {
			// empty slot first ( order 0 ), else the one grabbed longest ago
			slot = s_slots[ 0 ].m_order <= s_slots[ 1 ].m_order ? &s_slots[ 0 ] : &s_slots[ 1 ];
			drop( *slot );

			outcome = device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_RENDERTARGET, description.Format, D3DPOOL_DEFAULT,
			                                 &slot->m_texture, nullptr );

			if ( SUCCEEDED( outcome ) )
				outcome = slot->m_texture->GetSurfaceLevel( 0, &slot->m_surface );

			if ( FAILED( outcome ) ) {
				drop( *slot );
				return nullptr;
			}

			slot->m_width  = description.Width;
			slot->m_height = description.Height;
			slot->m_format = description.Format;
		}

		slot->m_order = ++s_order;
		slot->m_tick  = GetTickCount64( );

		outcome = copy( device, frame, description, slot->m_surface );

		return SUCCEEDED( outcome ) ? slot->m_texture : nullptr;
	}
}
