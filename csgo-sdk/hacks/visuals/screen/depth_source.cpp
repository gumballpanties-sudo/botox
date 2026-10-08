#include "depth_source.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"

#include "stream_guard.h"

#include <cmath>
#include <cstring>
#include <d3dx9.h>
#include <string>

static const D3DFORMAT k_format_intz = static_cast< D3DFORMAT >( MAKEFOURCC( 'I', 'N', 'T', 'Z' ) );
static const D3DFORMAT k_format_resz = static_cast< D3DFORMAT >( MAKEFOURCC( 'R', 'E', 'S', 'Z' ) );

static constexpr unsigned long k_resz_code = 0x7fa05000;

/* written over our INTZ before a check. 0.25, not 0 or 1: a broken probe ( zeros / garbage ) reads
   "changed" and keeps the route, so the check can only ever drop a route that truly never wrote */
static constexpr float k_sentinel = 0.25f;

static constexpr int k_probe_size = 8;

static const char* k_probe_shader = "sampler s0 : register( s0 );\n"
                                    "float4 main( float2 uv : TEXCOORD0 ) : COLOR { return tex2D( s0, uv ).rrrr; }\n";

static const char* k_route_names[ ] = { "none", "resz", "nvapi", "z swap" };

using nvapi_query_interface_t = void*( __cdecl* )( unsigned int );
using nvapi_initialize_t      = int( __cdecl* )( );
using nvapi_register_t        = int( __cdecl* )( IDirect3DResource9* );
using nvapi_unregister_t      = int( __cdecl* )( IDirect3DResource9* );
using nvapi_stretch_rect_ex_t = int( __cdecl* )( IDirect3DDevice9*, IDirect3DResource9*, const RECT*, IDirect3DResource9*, const RECT*,
                                                 D3DTEXTUREFILTERTYPE );

static nvapi_register_t g_nvapi_register          = nullptr;
static nvapi_unregister_t g_nvapi_unregister      = nullptr;
static nvapi_stretch_rect_ex_t g_nvapi_stretch_ex = nullptr;

bool n_depth_source::impl_t::nvapi_ready( )
{
	if ( this->m_nvapi_checked )
		return this->m_nvapi_available;

	this->m_nvapi_checked = true;

	HMODULE module = GetModuleHandleA( "nvapi.dll" );

	if ( !module )
		module = LoadLibraryA( "nvapi.dll" );

	if ( !module ) {
		g_console.print( "depth source: no nvapi.dll — depth resolve has no nvidia route" );
		return false;
	}

	const auto query = reinterpret_cast< nvapi_query_interface_t >( GetProcAddress( module, "nvapi_QueryInterface" ) );

	if ( !query )
		return false;

	const auto initialise = reinterpret_cast< nvapi_initialize_t >( query( 0x0150E828 ) );

	g_nvapi_register   = reinterpret_cast< nvapi_register_t >( query( 0xA064BDFC ) );
	g_nvapi_unregister = reinterpret_cast< nvapi_unregister_t >( query( 0xBB2B17AA ) );
	g_nvapi_stretch_ex = reinterpret_cast< nvapi_stretch_rect_ex_t >( query( 0x22DE03AA ) );

	if ( !initialise || !g_nvapi_register || !g_nvapi_stretch_ex ) {
		g_console.print< n_console::log_level::WARNING >( "depth source: nvapi entry points missing" );
		return false;
	}

	const int status = initialise( );

	this->m_nvapi_available = status == 0;

	g_console.print( std::vformat( "depth source: nvapi init {:d}", std::make_format_args( status ) ).c_str( ) );

	return this->m_nvapi_available;
}

void n_depth_source::impl_t::resolve_nvapi( IDirect3DDevice9* device, IDirect3DSurface9* depth_surface )
{
	if ( !device || !depth_surface || !this->m_own_depth || !g_nvapi_stretch_ex || !g_nvapi_register )
		return;

	if ( this->m_nvapi_registered_texture != this->m_own_depth ) {
		if ( this->m_nvapi_registered_texture && g_nvapi_unregister )
			g_nvapi_unregister( static_cast< IDirect3DResource9* >( this->m_nvapi_registered_texture ) );

		const int status = g_nvapi_register( this->m_own_depth );

		this->m_nvapi_registered_texture = this->m_own_depth;

		g_console.print( std::vformat( "depth source: nvapi register texture {:d}", std::make_format_args( status ) ).c_str( ) );
	}

	if ( this->m_nvapi_registered_surface != depth_surface ) {
		if ( this->m_nvapi_registered_surface && g_nvapi_unregister )
			g_nvapi_unregister( static_cast< IDirect3DResource9* >( this->m_nvapi_registered_surface ) );

		const int status = g_nvapi_register( depth_surface );

		this->m_nvapi_registered_surface = depth_surface;

		g_console.print( std::vformat( "depth source: nvapi register depth surface {:d}", std::make_format_args( status ) ).c_str( ) );
	}

	if ( !this->m_nvapi_registered_texture || !this->m_nvapi_registered_surface )
		return;

	const int status = g_nvapi_stretch_ex( device, depth_surface, nullptr, this->m_own_depth, nullptr, D3DTEXF_POINT );

	static int last_status = -1;

	if ( status != last_status ) {
		last_status = status;

		g_console.print( std::vformat( "depth source: nvapi resolve {:d}", std::make_format_args( status ) ).c_str( ) );
	}
}

// RESZ resolves an MSAA z buffer only. amd ( amdxn32 ) faults later on a 1 sample or missing one
static bool resz_source_ok( IDirect3DSurface9* depth_surface, const unsigned int width, const unsigned int height )
{
	D3DSURFACE_DESC description{ };

	return depth_surface && SUCCEEDED( depth_surface->GetDesc( &description ) ) && description.MultiSampleType != D3DMULTISAMPLE_NONE &&
	       description.Width == width && description.Height == height;
}

void n_depth_source::impl_t::resolve_resz( IDirect3DDevice9* device, IDirect3DSurface9* depth_surface )
{
	if ( !device || !this->m_own_depth || !resz_source_ok( depth_surface, this->m_width, this->m_height ) )
		return;

	// never rasterised, colour writes off
	static const float dummy_vertex[ 3 ] = { 0.f, 0.f, 0.f };

	device->SetVertexShader( nullptr );
	device->SetPixelShader( nullptr );
	device->SetVertexDeclaration( nullptr );
	device->SetFVF( D3DFVF_XYZ );
	device->SetTexture( 0, this->m_own_depth );
	device->SetRenderState( D3DRS_ZENABLE, FALSE );
	device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, 0 );

	device->DrawPrimitiveUP( D3DPT_POINTLIST, 1, dummy_vertex, 3 * sizeof( float ) );

	device->SetRenderState( D3DRS_ZWRITEENABLE, TRUE );
	device->SetRenderState( D3DRS_ZENABLE, TRUE );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, 0x0f );

	device->SetRenderState( D3DRS_POINTSIZE, k_resz_code );
	device->SetRenderState( D3DRS_POINTSIZE, 0 );

	device->SetTexture( 0, nullptr );
}

void n_depth_source::impl_t::release( )
{
	if ( g_nvapi_unregister ) {
		if ( this->m_nvapi_registered_texture )
			g_nvapi_unregister( static_cast< IDirect3DResource9* >( this->m_nvapi_registered_texture ) );

		if ( this->m_nvapi_registered_surface )
			g_nvapi_unregister( static_cast< IDirect3DResource9* >( this->m_nvapi_registered_surface ) );
	}

	this->m_nvapi_registered_texture = nullptr;
	this->m_nvapi_registered_surface = nullptr;

	/* route 3: hand the engine its own z buffer back first. a bound depth surface is referenced by the
	   device, would outlive this release and fail Reset. rest of this frame draws on stale depth. */
	this->m_live   = false;
	this->m_route  = route_none;
	this->m_verify = verify_none;

	if ( this->m_own_surface ) {
		IDirect3DDevice9* device = nullptr;

		if ( SUCCEEDED( this->m_own_surface->GetDevice( &device ) ) && device ) {
			IDirect3DSurface9* bound = nullptr;

			if ( SUCCEEDED( device->GetDepthStencilSurface( &bound ) ) && bound ) {
				if ( bound == this->m_own_surface )
					device->SetDepthStencilSurface( this->m_auto_depth );

				bound->Release( );
			}

			device->Release( );
		}

		this->m_own_surface->Release( );
		this->m_own_surface = nullptr;
	}

	if ( this->m_auto_depth ) {
		this->m_auto_depth->Release( );
		this->m_auto_depth = nullptr;
	}

	if ( this->m_own_depth ) {
		this->m_own_depth->Release( );
		this->m_own_depth = nullptr;
	}

	this->m_width  = 0;
	this->m_height = 0;
}

void n_depth_source::impl_t::on_device_lost( )
{
	this->release( );
	this->release_probe( );

	this->m_failed = 0;

	if ( this->m_depth_texture ) {
		this->m_depth_texture->Release( );
		this->m_depth_texture = nullptr;
	}
}

void n_depth_source::impl_t::on_created_texture( IDirect3DDevice9* device, IDirect3DTexture9* texture, const unsigned int width,
                                                 const unsigned int height, const D3DFORMAT format )
{
	if ( !device || !texture || format != k_format_intz || this->m_ignore_capture )
		return;

	IDirect3DSurface9* back_buffer = nullptr;

	if ( SUCCEEDED( device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer ) ) && back_buffer ) {
		D3DSURFACE_DESC description{ };
		const bool described = SUCCEEDED( back_buffer->GetDesc( &description ) );

		back_buffer->Release( );

		if ( described && ( description.Width != width || description.Height != height ) )
			return;
	}

	if ( this->m_depth_texture )
		this->m_depth_texture->Release( );

	texture->AddRef( );
	this->m_depth_texture = texture;

	const unsigned long pointer = reinterpret_cast< unsigned long >( texture );

	g_console.print( std::vformat( "depth source: INTZ target {:#x} {:d}x{:d}", std::make_format_args( pointer, width, height ) ).c_str( ) );

	static bool logged_vendor = false;

	if ( !logged_vendor ) {
		logged_vendor = true;

		IDirect3D9* d3d = nullptr;

		if ( SUCCEEDED( device->GetDirect3D( &d3d ) ) && d3d ) {
			D3DADAPTER_IDENTIFIER9 identifier{ };

			if ( SUCCEEDED( d3d->GetAdapterIdentifier( D3DADAPTER_DEFAULT, 0, &identifier ) ) ) {
				const char* description    = identifier.Description;
				const unsigned long vendor = identifier.VendorId;

				g_console.print( std::vformat( "depth source: adapter {:s} vendor {:#x}", std::make_format_args( description, vendor ) ).c_str( ) );
			}

			d3d->Release( );
		}
	}
}

bool n_depth_source::impl_t::ensure( IDirect3DDevice9* device, const D3DSURFACE_DESC& description )
{
	if ( !device )
		return false;

	// at 0 there's no resolve support and _rt_FullFrameDepth is unreadable R32F, not INTZ.
	// must be 1 before the target is built.
	if ( c_cconvar* resolve_depth = g_convars[ HASH_BT( "mat_resolveFullFrameDepth" ) ]; resolve_depth && resolve_depth->get_int( ) != 1 )
		resolve_depth->set_value( 1 );

	const bool forced = GET_VARIABLE( g_variables.m_depth_force_live, bool );

	if ( this->m_own_depth && !this->m_rebuild && this->m_width == description.Width && this->m_height == description.Height &&
	     forced == this->m_live_forced )
		return this->texture( ) != nullptr;

	this->release( );

	this->m_rebuild     = false;
	this->m_live_forced = forced;

	IDirect3D9* d3d = nullptr;
	D3DDEVICE_CREATION_PARAMETERS parameters{ };
	D3DDISPLAYMODE mode{ };

	if ( SUCCEEDED( device->GetDirect3D( &d3d ) ) && d3d && SUCCEEDED( device->GetCreationParameters( &parameters ) ) &&
	     SUCCEEDED( device->GetDisplayMode( 0, &mode ) ) ) {
		const bool resz = SUCCEEDED( d3d->CheckDeviceFormat( parameters.AdapterOrdinal, parameters.DeviceType, mode.Format,
		                                                     D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, k_format_resz ) );
		const bool intz = SUCCEEDED( d3d->CheckDeviceFormat( parameters.AdapterOrdinal, parameters.DeviceType, mode.Format,
		                                                     D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, k_format_intz ) );

		this->m_resz_supported = resz && intz;

		g_console.print( std::vformat( "depth source: resz {:d} intz {:d}", std::make_format_args( resz, intz ) ).c_str( ) );
	}

	if ( d3d )
		d3d->Release( );

	// capture hook must not adopt this one
	this->m_ignore_capture = true;

	const HRESULT own = device->CreateTexture( description.Width, description.Height, 1, D3DUSAGE_DEPTHSTENCIL, k_format_intz, D3DPOOL_DEFAULT,
	                                          &this->m_own_depth, nullptr );

	this->m_ignore_capture = false;

	if ( FAILED( own ) || !this->m_own_depth ) {
		this->m_own_depth      = nullptr;
		this->m_resz_supported = false;

		g_console.print< n_console::log_level::WARNING >( "depth source: own INTZ target creation failed, falling back to the engine resolve" );
	}

	const auto usable = [ this ]( const route_t route ) { return !( this->m_failed & ( 1 << route ) ); };

	if ( this->m_own_depth ) {
		IDirect3DSurface9* bound = nullptr;

		if ( FAILED( device->GetDepthStencilSurface( &bound ) ) )
			bound = nullptr;

		const bool resz_source = resz_source_ok( bound, description.Width, description.Height );

		if ( bound )
			bound->Release( );

		if ( this->m_resz_supported && !resz_source )
			g_console.print( "depth source: resz skipped, z buffer not multisampled ( msaa off )" );

		if ( this->m_resz_supported && resz_source && usable( route_resz ) )
			this->m_route = route_resz;
		else if ( usable( route_nvapi ) && this->nvapi_ready( ) )
			this->m_route = route_nvapi;

		if ( ( forced || this->m_route == route_none ) && usable( route_live ) && this->pick_live_route( device, description ) )
			this->m_route = route_live;
	}

	this->m_width  = description.Width;
	this->m_height = description.Height;

	if ( this->m_route != route_none ) {
		this->m_verify = verify_clear;

		const char* name = k_route_names[ this->m_route ];
		g_console.print( std::vformat( "depth source: route {:s}, checking it", std::make_format_args( name ) ).c_str( ) );
	} else if ( this->m_own_depth ) {
		const int failed = this->m_failed;
		g_console.print< n_console::log_level::WARNING >(
			std::vformat( "depth source: no working route ( failed mask {:d} ), depth effects off. msaa off lets z swap run on any gpu",
		                  std::make_format_args( failed ) )
				.c_str( ) );
	}

	return this->texture( ) != nullptr;
}

bool n_depth_source::impl_t::pick_live_route( IDirect3DDevice9* device, const D3DSURFACE_DESC& description )
{
	IDirect3DSurface9* bound = nullptr;

	if ( FAILED( device->GetDepthStencilSurface( &bound ) ) || !bound ) {
		g_console.print< n_console::log_level::WARNING >( "depth source: no z buffer bound, universal route skipped" );
		return false;
	}

	D3DSURFACE_DESC bound_description{ };
	const bool described = SUCCEEDED( bound->GetDesc( &bound_description ) );

	// INTZ can't be multisampled, and a stand in must match what every rt it pairs with expects
	if ( !described || bound_description.MultiSampleType != D3DMULTISAMPLE_NONE || bound_description.Format == k_format_intz ||
	     bound_description.Width != description.Width || bound_description.Height != description.Height ||
	     FAILED( this->m_own_depth->GetSurfaceLevel( 0, &this->m_own_surface ) ) || !this->m_own_surface ) {
		bound->Release( );
		this->m_own_surface = nullptr;

		g_console.print< n_console::log_level::WARNING >(
			"depth source: universal route needs MSAA off ( video settings ). no RESZ / NvAPI either = no depth effects" );
		return false;
	}

	this->m_auto_depth = bound;

	g_console.print( "depth source: universal route, own INTZ becomes the z buffer next frame" );

	return true;
}

void n_depth_source::impl_t::on_present( IDirect3DDevice9* device )
{
	if ( !device || this->m_route == route_none )
		return;

	// sentinel first: route 3 must be cleared before it is the live z buffer
	if ( this->m_verify == verify_clear ) {
		this->m_verify        = this->clear_sentinel( device ) ? verify_wait : verify_skipped;
		this->m_verify_frames = 0;
		this->m_resolves      = 0;
	}

	if ( this->m_route == route_live && !this->m_live && this->m_own_surface && this->m_auto_depth ) {
		this->m_live = true;

		IDirect3DSurface9* bound = nullptr;

		if ( SUCCEEDED( device->GetDepthStencilSurface( &bound ) ) && bound ) {
			if ( bound == this->m_auto_depth )
				device->SetDepthStencilSurface( this->m_own_surface );

			bound->Release( );
		}

		return;
	}

	if ( this->m_verify != verify_wait )
		return;

	const bool ready = this->m_route == route_live ? ++this->m_verify_frames >= 3 : this->m_resolves >= 2;

	if ( ready )
		this->verify( device );
}

bool n_depth_source::impl_t::clear_sentinel( IDirect3DDevice9* device )
{
	IDirect3DSurface9* level = nullptr;

	if ( !this->m_own_depth || FAILED( this->m_own_depth->GetSurfaceLevel( 0, &level ) ) || !level )
		return false;

	IDirect3DSurface9* scratch = nullptr;

	if ( FAILED( device->CreateRenderTarget( this->m_width, this->m_height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &scratch, nullptr ) ) ||
	     !scratch ) {
		level->Release( );
		return false;
	}

	IDirect3DSurface9* old_target = nullptr;
	IDirect3DSurface9* old_depth  = nullptr;
	D3DVIEWPORT9 old_viewport{ };
	DWORD scissor = FALSE;

	device->GetRenderTarget( 0, &old_target );
	device->GetDepthStencilSurface( &old_depth );
	device->GetViewport( &old_viewport );
	device->GetRenderState( D3DRS_SCISSORTESTENABLE, &scissor );

	device->SetRenderTarget( 0, scratch );
	device->SetDepthStencilSurface( level );
	device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );

	const bool cleared = SUCCEEDED( device->Clear( 0, nullptr, D3DCLEAR_ZBUFFER, 0, k_sentinel, 0 ) );

	device->SetRenderTarget( 0, old_target );
	device->SetDepthStencilSurface( old_depth );
	device->SetViewport( &old_viewport );
	device->SetRenderState( D3DRS_SCISSORTESTENABLE, scissor );

	if ( old_target )
		old_target->Release( );

	if ( old_depth )
		old_depth->Release( );

	scratch->Release( );
	level->Release( );

	if ( !cleared )
		g_console.print< n_console::log_level::WARNING >( "depth source: sentinel clear failed, route left unchecked" );

	return cleared;
}

bool n_depth_source::impl_t::ensure_probe( IDirect3DDevice9* device )
{
	if ( !this->m_probe_surface ) {
		if ( FAILED( device->CreateTexture( k_probe_size, k_probe_size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT,
		                                    &this->m_probe_texture, nullptr ) ) ||
		     FAILED( this->m_probe_texture->GetSurfaceLevel( 0, &this->m_probe_surface ) ) ||
		     FAILED( device->CreateOffscreenPlainSurface( k_probe_size, k_probe_size, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM,
		                                                  &this->m_probe_sysmem, nullptr ) ) ) {
			this->release_probe( );
			return false;
		}
	}

	if ( !this->m_probe_ps ) {
		ID3DXBuffer* code = nullptr;

		if ( FAILED( D3DXCompileShader( k_probe_shader, static_cast< UINT >( std::strlen( k_probe_shader ) ), nullptr, nullptr, "main", "ps_2_0", 0,
		                                &code, nullptr, nullptr ) ) ||
		     !code )
			return false;

		const HRESULT created = device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &this->m_probe_ps );
		code->Release( );

		if ( FAILED( created ) ) {
			this->m_probe_ps = nullptr;
			return false;
		}
	}

	return true;
}

void n_depth_source::impl_t::release_probe( )
{
	if ( this->m_probe_sysmem ) {
		this->m_probe_sysmem->Release( );
		this->m_probe_sysmem = nullptr;
	}

	if ( this->m_probe_surface ) {
		this->m_probe_surface->Release( );
		this->m_probe_surface = nullptr;
	}

	if ( this->m_probe_texture ) {
		this->m_probe_texture->Release( );
		this->m_probe_texture = nullptr;
	}

	if ( this->m_probe_ps ) {
		this->m_probe_ps->Release( );
		this->m_probe_ps = nullptr;
	}
}

void n_depth_source::impl_t::verify( IDirect3DDevice9* device )
{
	// any early out = route kept, unchecked. never drop a route on a probe that couldn't run
	this->m_verify = verify_skipped;

	if ( !this->m_own_depth || !this->ensure_probe( device ) ) {
		g_console.print< n_console::log_level::WARNING >( "depth source: route check could not run, route kept" );
		return;
	}

	n_stream_guard::state_t streams{ };
	streams.capture( device );

	IDirect3DSurface9* old_target                       = nullptr;
	IDirect3DSurface9* old_depth                        = nullptr;
	IDirect3DPixelShader9* old_pixel_shader             = nullptr;
	IDirect3DVertexShader9* old_vertex_shader           = nullptr;
	IDirect3DVertexDeclaration9* old_vertex_declaration = nullptr;
	IDirect3DBaseTexture9* old_texture                  = nullptr;
	D3DVIEWPORT9 old_viewport{ };
	DWORD fvf = 0;

	// D3DSBT_ALL Apply on csgo's pure device writes stale state under the engine's cache = broken world textures
	constexpr D3DRENDERSTATETYPE k_states[ ] = { D3DRS_ZENABLE,          D3DRS_ZWRITEENABLE,    D3DRS_STENCILENABLE,   D3DRS_ALPHABLENDENABLE,
		                                         D3DRS_ALPHATESTENABLE,  D3DRS_FOGENABLE,       D3DRS_CULLMODE,        D3DRS_SCISSORTESTENABLE,
		                                         D3DRS_CLIPPLANEENABLE,  D3DRS_FILLMODE,        D3DRS_SRGBWRITEENABLE, D3DRS_COLORWRITEENABLE };
	constexpr D3DSAMPLERSTATETYPE k_samplers[ ] = { D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,  D3DSAMP_MINFILTER,
		                                            D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };

	DWORD old_states[ std::size( k_states ) ]{ };
	DWORD old_samplers[ std::size( k_samplers ) ]{ };

	device->GetRenderTarget( 0, &old_target );
	device->GetDepthStencilSurface( &old_depth );
	device->GetPixelShader( &old_pixel_shader );
	device->GetVertexShader( &old_vertex_shader );
	device->GetVertexDeclaration( &old_vertex_declaration );
	device->GetFVF( &fvf );
	device->GetTexture( 0, &old_texture );
	device->GetViewport( &old_viewport );

	for ( size_t i = 0; i < std::size( k_states ); ++i )
		device->GetRenderState( k_states[ i ], &old_states[ i ] );

	for ( size_t i = 0; i < std::size( k_samplers ); ++i )
		device->GetSamplerState( 0, k_samplers[ i ], &old_samplers[ i ] );

	device->SetRenderTarget( 0, this->m_probe_surface );
	device->SetDepthStencilSurface( nullptr );

	const D3DVIEWPORT9 viewport{ 0, 0, k_probe_size, k_probe_size, 0.f, 1.f };
	device->SetViewport( &viewport );

	device->SetVertexShader( nullptr );
	device->SetPixelShader( this->m_probe_ps );
	device->SetVertexDeclaration( nullptr );
	device->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
	device->SetTexture( 0, this->m_own_depth );
	device->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
	device->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_POINT );
	device->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
	device->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
	device->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, FALSE );
	device->SetRenderState( D3DRS_ZENABLE, FALSE );
	device->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_STENCILENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
	device->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
	device->SetRenderState( D3DRS_FOGENABLE, FALSE );
	device->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
	device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
	device->SetRenderState( D3DRS_CLIPPLANEENABLE, 0 );
	device->SetRenderState( D3DRS_FILLMODE, D3DFILL_SOLID );
	device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
	device->SetRenderState( D3DRS_COLORWRITEENABLE, 0x0f );

	struct vertex_t {
		float m_x, m_y, m_z, m_rhw, m_u, m_v;
	};

	constexpr float edge = k_probe_size - 0.5f;

	const vertex_t quad[ 4 ] = {
		{ -0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f },
		{ edge, -0.5f, 0.f, 1.f, 1.f, 0.f },
		{ -0.5f, edge, 0.f, 1.f, 0.f, 1.f },
		{ edge, edge, 0.f, 1.f, 1.f, 1.f },
	};

	const bool scene   = SUCCEEDED( device->BeginScene( ) );
	const bool draw_ok = SUCCEEDED( device->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) ) );

	if ( scene )
		device->EndScene( );

	device->SetRenderTarget( 0, old_target );
	device->SetDepthStencilSurface( old_depth );
	device->SetViewport( &old_viewport );
	device->SetPixelShader( old_pixel_shader );
	device->SetVertexShader( old_vertex_shader );

	if ( fvf )
		device->SetFVF( fvf );

	device->SetVertexDeclaration( old_vertex_declaration );
	device->SetTexture( 0, old_texture );

	for ( size_t i = 0; i < std::size( k_states ); ++i )
		device->SetRenderState( k_states[ i ], old_states[ i ] );

	for ( size_t i = 0; i < std::size( k_samplers ); ++i )
		device->SetSamplerState( 0, k_samplers[ i ], old_samplers[ i ] );

	streams.restore( device );

	for ( IUnknown* held : { static_cast< IUnknown* >( old_target ), static_cast< IUnknown* >( old_depth ),
	                         static_cast< IUnknown* >( old_pixel_shader ), static_cast< IUnknown* >( old_vertex_shader ),
	                         static_cast< IUnknown* >( old_vertex_declaration ), static_cast< IUnknown* >( old_texture ) } ) {
		if ( held )
			held->Release( );
	}

	D3DLOCKED_RECT locked{ };

	if ( !draw_ok || FAILED( device->GetRenderTargetData( this->m_probe_surface, this->m_probe_sysmem ) ) ||
	     FAILED( this->m_probe_sysmem->LockRect( &locked, nullptr, D3DLOCK_READONLY ) ) ) {
		g_console.print< n_console::log_level::WARNING >( "depth source: route check readback failed, route kept" );
		return;
	}

	int changed = 0;
	float low   = 1e9f;
	float high  = -1e9f;

	for ( int y = 0; y < k_probe_size; ++y ) {
		const float* row = reinterpret_cast< const float* >( static_cast< const unsigned char* >( locked.pBits ) + y * locked.Pitch );

		for ( int x = 0; x < k_probe_size; ++x ) {
			const float value = row[ x * 4 ];

			low  = value < low ? value : low;
			high = value > high ? value : high;

			if ( !( std::fabs( value - k_sentinel ) < 1e-4f ) )
				++changed;
		}
	}

	this->m_probe_sysmem->UnlockRect( );

	const char* name = k_route_names[ this->m_route ];
	const int total  = k_probe_size * k_probe_size;

	if ( changed ) {
		this->m_verify = verify_ok;

		g_console.print( std::vformat( "depth source: route {:s} ok, {:d}/{:d} written, depth {:.4f}..{:.4f}",
		                               std::make_format_args( name, changed, total, low, high ) )
		                     .c_str( ) );
		return;
	}

	// never wrote: drop it, next ensure( ) builds the next route
	this->m_failed |= 1 << this->m_route;
	this->m_rebuild = true;
	this->m_verify  = verify_none;

	g_console.print< n_console::log_level::WARNING >(
		std::vformat( "depth source: route {:s} FAILED, depth never written. trying the next one", std::make_format_args( name ) ).c_str( ) );
}

const char* n_depth_source::impl_t::route_name( ) const
{
	return k_route_names[ this->m_route ];
}

const char* n_depth_source::impl_t::verify_name( ) const
{
	switch ( this->m_verify ) {
	case verify_clear:
	case verify_wait:
		return "checking";
	case verify_ok:
		return "ok";
	case verify_skipped:
		return "unchecked";
	default:
		if ( !this->m_own_depth || this->m_route != route_none )
			return "";

		return this->m_failed ? "every route failed" : "msaa on + no driver route";
	}
}

IDirect3DTexture9* n_depth_source::impl_t::texture( ) const
{
	switch ( this->m_route ) {
	case route_live:
		return this->m_live ? this->m_own_depth : nullptr;
	case route_resz:
	case route_nvapi:
		return this->m_own_depth;
	default:
		return this->m_own_depth ? nullptr : this->m_depth_texture;
	}
}

void n_depth_source::impl_t::resolve( IDirect3DDevice9* device, IDirect3DSurface9* bound_depth, const int frame )
{
	if ( !device || this->m_route == route_live )
		return;

	if ( this->m_skip_resolve ) {
		this->m_skip_resolve = false;
		return;
	}

	if ( frame == this->m_resolved_frame )
		return;

	this->m_resolved_frame = frame;

	if ( this->m_own_depth ) {
		if ( this->m_route == route_resz )
			this->resolve_resz( device, bound_depth );
		else if ( this->m_route == route_nvapi )
			this->resolve_nvapi( device, bound_depth );
		else
			return;

		++this->m_resolves;
		return;
	}

	c_material_system* materials = g_interfaces.m_material_system;
	c_texture* depth             = materials ? materials->find_texture( "_rt_FullFrameDepth", TEXTURE_GROUP_RENDER_TARGET ) : nullptr;

	// -1 = z buffer. before any rt swap, scene depth must still be bound
	if ( depth )
		materials->get_render_context( )->copy_render_target_to_texture_ex( depth, -1, nullptr, nullptr );
}
