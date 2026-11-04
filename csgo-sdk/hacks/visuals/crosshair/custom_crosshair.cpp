#include "custom_crosshair.h"

#include <cmath>
#include <cstring>
#include <vector>
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "../../../dependencies/imgui/imgui_internal.h"
#include "../screen/resolution_spoof.h"
#include "ch_blend_modes.h"
#include "fx_chblend_vs.h"
#include "fx_chblend_ps.h"

extern void botox_dbg_log( const char* fmt, ... );

namespace
{
	constexpr float k_unit_to_px = 2.0f;

	struct settings_t {
		bool aa;
		int blend;
		float color[ 4 ], outline_color[ 4 ];
		float thickness, length, gap;
		bool sides_only, sides[ 4 ];         /* L R B T */
		bool dot, dot_circle;
		bool outline;
		float outline_thick;
		bool outline_sides_only, outline_sides[ 4 ]; /* L R T B */
		bool outline_on_lines, outline_on_dot, outline_blur;
		float outline_blur_radius;
		float aspect, gap_aspect;
		bool dynamic;
		float dynamic_min, dynamic_max, dynamic_ref;
		bool gradient;
		float grad_center[ 4 ], grad_outer[ 4 ], grad_start, grad_end;
		bool taper;
		float taper_near, taper_far;
		bool dot_custom_color;
		float dot_color[ 4 ];
		float rotation;
	};

	struct arm_t {
		float x1, y1, x2, y2;
	};

	/* render thread only: in-game callback and menu preview never overlap */
	bool s_aa = false;

	IDirect3DDevice9* device( )
	{
		return g_interfaces.m_direct_device;
	}

	void to_float4( const c_color& c, float out[ 4 ] )
	{
		for ( int i = 0; i < 4; ++i )
			out[ i ] = c[ i ] / 255.f;
	}

	int clamp_blend( const int m )
	{
		return ( m >= 0 && m < kChBlendCount ) ? m : 0;
	}

	settings_t read_settings( )
	{
		settings_t s{ };
		s.aa    = GET_VARIABLE( g_variables.m_custom_crosshair_antialias, bool );
		s.blend = clamp_blend( GET_VARIABLE( g_variables.m_custom_crosshair_blend, int ) );

		to_float4( GET_VARIABLE( g_variables.m_custom_crosshair_color, c_color ), s.color );
		to_float4( GET_VARIABLE( g_variables.m_custom_crosshair_outline_color, c_color ), s.outline_color );

		s.thickness  = GET_VARIABLE( g_variables.m_custom_crosshair_thickness, float );
		s.length     = GET_VARIABLE( g_variables.m_custom_crosshair_length, float );
		s.gap        = GET_VARIABLE( g_variables.m_custom_crosshair_gap, float );
		s.sides_only = GET_VARIABLE( g_variables.m_custom_crosshair_sides_only, bool );
		s.sides[ 0 ] = GET_VARIABLE( g_variables.m_custom_crosshair_side_left, bool );
		s.sides[ 1 ] = GET_VARIABLE( g_variables.m_custom_crosshair_side_right, bool );
		s.sides[ 2 ] = GET_VARIABLE( g_variables.m_custom_crosshair_side_bottom, bool );
		s.sides[ 3 ] = GET_VARIABLE( g_variables.m_custom_crosshair_side_top, bool );
		s.dot        = GET_VARIABLE( g_variables.m_custom_crosshair_dot, bool );
		s.dot_circle = GET_VARIABLE( g_variables.m_custom_crosshair_dot_circle, bool );

		s.outline                = GET_VARIABLE( g_variables.m_custom_crosshair_outline, bool );
		s.outline_thick          = GET_VARIABLE( g_variables.m_custom_crosshair_outline_thickness, float );
		s.outline_sides_only     = GET_VARIABLE( g_variables.m_custom_crosshair_outline_sides_only, bool );
		s.outline_sides[ 0 ]     = GET_VARIABLE( g_variables.m_custom_crosshair_outline_side_left, bool );
		s.outline_sides[ 1 ]     = GET_VARIABLE( g_variables.m_custom_crosshair_outline_side_right, bool );
		s.outline_sides[ 2 ]     = GET_VARIABLE( g_variables.m_custom_crosshair_outline_side_top, bool );
		s.outline_sides[ 3 ]     = GET_VARIABLE( g_variables.m_custom_crosshair_outline_side_bottom, bool );
		s.outline_on_lines       = GET_VARIABLE( g_variables.m_custom_crosshair_outline_on_lines, bool );
		s.outline_on_dot         = GET_VARIABLE( g_variables.m_custom_crosshair_outline_on_dot, bool );
		s.outline_blur           = GET_VARIABLE( g_variables.m_custom_crosshair_outline_blur, bool );
		s.outline_blur_radius    = GET_VARIABLE( g_variables.m_custom_crosshair_outline_blur_radius, float );

		s.aspect      = GET_VARIABLE( g_variables.m_custom_crosshair_aspect, float );
		s.gap_aspect  = GET_VARIABLE( g_variables.m_custom_crosshair_gap_aspect, float );
		s.dynamic     = GET_VARIABLE( g_variables.m_custom_crosshair_dynamic, bool );
		s.dynamic_min = GET_VARIABLE( g_variables.m_custom_crosshair_dynamic_min, float );
		s.dynamic_max = GET_VARIABLE( g_variables.m_custom_crosshair_dynamic_max, float );
		s.dynamic_ref = GET_VARIABLE( g_variables.m_custom_crosshair_dynamic_ref, float );

		s.gradient = GET_VARIABLE( g_variables.m_custom_crosshair_gradient, bool );
		to_float4( GET_VARIABLE( g_variables.m_custom_crosshair_gradient_center, c_color ), s.grad_center );
		to_float4( GET_VARIABLE( g_variables.m_custom_crosshair_gradient_outer, c_color ), s.grad_outer );
		s.grad_start = GET_VARIABLE( g_variables.m_custom_crosshair_gradient_start, float );
		s.grad_end   = GET_VARIABLE( g_variables.m_custom_crosshair_gradient_end, float );

		s.taper      = GET_VARIABLE( g_variables.m_custom_crosshair_taper, bool );
		s.taper_near = GET_VARIABLE( g_variables.m_custom_crosshair_taper_near, float );
		s.taper_far  = GET_VARIABLE( g_variables.m_custom_crosshair_taper_far, float );

		s.dot_custom_color = GET_VARIABLE( g_variables.m_custom_crosshair_dot_custom_color, bool );
		to_float4( GET_VARIABLE( g_variables.m_custom_crosshair_dot_color, c_color ), s.dot_color );
		s.rotation = GET_VARIABLE( g_variables.m_custom_crosshair_rotation, float );
		return s;
	}

	ImU32 lerp_color( const ImU32 c0, const ImU32 c1, const float t )
	{
		if ( t <= 0.f )
			return c0;
		if ( t >= 1.f )
			return c1;
		auto l = []( int a, int b, float t ) { return static_cast< int >( a + ( b - a ) * t + 0.5f ); };
		return IM_COL32( l( ( c0 >> 0 ) & 0xFF, ( c1 >> 0 ) & 0xFF, t ), l( ( c0 >> 8 ) & 0xFF, ( c1 >> 8 ) & 0xFF, t ),
		                 l( ( c0 >> 16 ) & 0xFF, ( c1 >> 16 ) & 0xFF, t ), l( ( c0 >> 24 ) & 0xFF, ( c1 >> 24 ) & 0xFF, t ) );
	}

	ImU32 scale_alpha( const ImU32 col, const float t )
	{
		if ( t <= 0.f )
			return col & 0x00FFFFFFu;
		if ( t >= 1.f )
			return col;
		const int a = static_cast< int >( ( ( col >> 24 ) & 0xFF ) * t + 0.5f );
		return ( col & 0x00FFFFFFu ) | ( static_cast< ImU32 >( a ) << 24 );
	}

	/* sub-pixel rect: solid core + 1px fringe at fractional coverage */
	void draw_rect( ImDrawList* dl, const float x1, const float y1, const float x2, const float y2, const ImU32 col )
	{
		if ( !s_aa ) {
			dl->AddRectFilled( ImVec2( floorf( x1 ), floorf( y1 ) ), ImVec2( ceilf( x2 ), ceilf( y2 ) ), col );
			return;
		}

		const float cx1 = ceilf( x1 ), cy1 = ceilf( y1 );
		const float cx2 = floorf( x2 ), cy2 = floorf( y2 );

		const float fl = cx1 - x1;
		const float fr = x2 - cx2;
		const float ft = cy1 - y1;
		const float fb = y2 - cy2;

		if ( cx1 < cx2 && cy1 < cy2 )
			dl->AddRectFilled( ImVec2( cx1, cy1 ), ImVec2( cx2, cy2 ), col );

		if ( fl > 0.001f )
			dl->AddRectFilled( ImVec2( cx1 - 1.f, cy1 ), ImVec2( cx1, cy2 ), scale_alpha( col, fl ) );
		if ( fr > 0.001f )
			dl->AddRectFilled( ImVec2( cx2, cy1 ), ImVec2( cx2 + 1.f, cy2 ), scale_alpha( col, fr ) );
		if ( ft > 0.001f )
			dl->AddRectFilled( ImVec2( cx1, cy1 - 1.f ), ImVec2( cx2, cy1 ), scale_alpha( col, ft ) );
		if ( fb > 0.001f )
			dl->AddRectFilled( ImVec2( cx1, cy2 ), ImVec2( cx2, cy2 + 1.f ), scale_alpha( col, fb ) );

		if ( fl > 0.001f && ft > 0.001f )
			dl->AddRectFilled( ImVec2( cx1 - 1.f, cy1 - 1.f ), ImVec2( cx1, cy1 ), scale_alpha( col, fl * ft ) );
		if ( fr > 0.001f && ft > 0.001f )
			dl->AddRectFilled( ImVec2( cx2, cy1 - 1.f ), ImVec2( cx2 + 1.f, cy1 ), scale_alpha( col, fr * ft ) );
		if ( fl > 0.001f && fb > 0.001f )
			dl->AddRectFilled( ImVec2( cx1 - 1.f, cy2 ), ImVec2( cx1, cy2 + 1.f ), scale_alpha( col, fl * fb ) );
		if ( fr > 0.001f && fb > 0.001f )
			dl->AddRectFilled( ImVec2( cx2, cy2 ), ImVec2( cx2 + 1.f, cy2 + 1.f ), scale_alpha( col, fr * fb ) );
	}

	arm_t make_arm( const float cx, const float cy, const float th_px, const float len, const float gap, const int dir )
	{
		const float lo = s_aa ? ( th_px * 0.5f ) : floorf( th_px * 0.5f );
		const float hi = s_aa ? ( th_px * 0.5f ) : ( th_px - lo );
		arm_t a{ };
		switch ( dir ) {
		case 0: a = { cx - gap - len, cy - lo, cx - gap, cy + hi }; break;
		case 1: a = { cx + gap, cy - lo, cx + gap + len, cy + hi }; break;
		case 2: a = { cx - lo, cy + gap, cx + hi, cy + gap + len }; break;
		case 3: a = { cx - lo, cy - gap - len, cx + hi, cy - gap }; break;
		}
		return a;
	}

	/* 1px axial slices so draw_rect AAs tapered edges. dir 0=L 1=R 2=B 3=T */
	void draw_arm_fancy( ImDrawList* dl, const arm_t& a, const int dir, const ImU32 center_col, const ImU32 outer_col, float grad_start, float grad_end,
	                     const float near_scale, const float far_scale )
	{
		grad_start = ImClamp( grad_start, 0.f, 1.f );
		grad_end   = ImClamp( grad_end, grad_start, 1.f );

		const float base_half   = ( dir <= 1 ) ? ( a.y2 - a.y1 ) * 0.5f : ( a.x2 - a.x1 ) * 0.5f;
		const float inner_half  = base_half * near_scale;
		const float outer_half  = base_half * far_scale;
		const float center_line = ( dir <= 1 ) ? ( a.y1 + a.y2 ) * 0.5f : ( a.x1 + a.x2 ) * 0.5f;
		const float g_range     = grad_end - grad_start;
		const bool hz           = ( dir <= 1 );

		float ax_start, ax_end;
		switch ( dir ) {
		case 0: ax_start = a.x2; ax_end = a.x1; break;
		case 1: ax_start = a.x1; ax_end = a.x2; break;
		case 2: ax_start = a.y1; ax_end = a.y2; break;
		default: ax_start = a.y2; ax_end = a.y1; break;
		}

		const float x_min   = fminf( ax_start, ax_end );
		const float x_max   = fmaxf( ax_start, ax_end );
		const float ax_span = ax_end - ax_start;

		for ( float x = floorf( x_min ); x < x_max - 1e-4f; x += 1.f ) {
			const float rx1   = fmaxf( x, x_min );
			const float rx2   = fminf( x + 1.f, x_max );
			const float x_mid = ( rx1 + rx2 ) * 0.5f;

			const float t   = ( fabsf( ax_span ) > 1e-5f ) ? ImClamp( ( x_mid - ax_start ) / ax_span, 0.f, 1.f ) : 0.5f;
			const float h   = inner_half + t * ( outer_half - inner_half );
			const float g   = ( g_range > 0.001f ) ? ImClamp( ( t - grad_start ) / g_range, 0.f, 1.f ) : ( t >= grad_start ? 1.f : 0.f );
			const ImU32 col = lerp_color( center_col, outer_col, g );

			if ( hz )
				draw_rect( dl, rx1, center_line - h, rx2, center_line + h, col );
			else
				draw_rect( dl, center_line - h, rx1, center_line + h, rx2, col );
		}
	}

	/* ot < 1 = 1px, else floor(ot * 2) px on each enabled side */
	void draw_outline( ImDrawList* dl, const float ax1, const float ay1, const float ax2, const float ay2, const float ot_units, const ImU32 col,
	                   const bool side_l, const bool side_r, const bool side_t, const bool side_b )
	{
		const float px = s_aa ? ( ot_units * k_unit_to_px ) : ( ( ot_units < 1.0f ) ? 1.0f : floorf( ot_units * k_unit_to_px ) );

		draw_rect( dl, ax1 - ( side_l ? px : 0.f ), ay1 - ( side_t ? px : 0.f ), ax2 + ( side_r ? px : 0.f ), ay2 + ( side_b ? px : 0.f ), col );
	}

	/* per-layer alpha inverts OVER compositing so cumulative alpha at r = base * gauss(r) */
	void draw_outline_blur( ImDrawList* dl, const float ax1, const float ay1, const float ax2, const float ay2, const float ot_units, const ImU32 col,
	                        const float blur_radius, const bool side_l, const bool side_r, const bool side_t, const bool side_b )
	{
		const float base_a     = static_cast< float >( ( col >> 24 ) & 0xFF ) / 255.f;
		const ImU32 rgb        = col & 0x00FFFFFFu;
		const float max_expand = blur_radius * k_unit_to_px;
		const float sigma      = max_expand * 0.4f + 0.5f;
		const int n            = 16;
		const float base_thick = s_aa ? ( ot_units * k_unit_to_px ) : ( ( ot_units < 1.0f ) ? 1.0f : floorf( ot_units * k_unit_to_px ) );

		const bool all_sides = side_l && side_r && side_t && side_b;
		float cum_transp     = 1.f;
		for ( int i = n - 1; i >= 0; --i ) {
			const float br     = ( static_cast< float >( i + 1 ) / n ) * max_expand;
			const float target = base_a * expf( -0.5f * ( br / sigma ) * ( br / sigma ) );
			if ( cum_transp < 1e-4f )
				break;
			const float layer_a = 1.f - ( 1.f - target ) / cum_transp;
			if ( layer_a <= 0.f )
				continue;
			const int ai = static_cast< int >( layer_a * 255.f + 0.5f );
			if ( ai <= 0 )
				continue;
			cum_transp *= ( 1.f - layer_a );
			const float px = base_thick + br;
			if ( all_sides )
				dl->AddRectFilled( ImVec2( ax1 - px, ay1 - px ), ImVec2( ax2 + px, ay2 + px ), rgb | ( static_cast< ImU32 >( ai ) << 24 ), px );
			else
				draw_rect( dl, ax1 - ( side_l ? px : 0.f ), ay1 - ( side_t ? px : 0.f ), ax2 + ( side_r ? px : 0.f ), ay2 + ( side_b ? px : 0.f ),
				           rgb | ( static_cast< ImU32 >( ai ) << 24 ) );
		}
		draw_outline( dl, ax1, ay1, ax2, ay2, ot_units, col, side_l, side_r, side_t, side_b );
	}

	void blend_add_on( const ImDrawList*, const ImDrawCmd* )
	{
		if ( device( ) )
			device( )->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_ONE );
	}

	void blend_add_off( const ImDrawList*, const ImDrawCmd* )
	{
		if ( device( ) )
			device( )->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
	}

	/* modes 2+: copy the frame under the arms, blend in fx_chblend_ps (krita formulas) at VPOS.
	   ONE copy per imgui frame per render target: re-copying would blend onto our own last pass */
	IDirect3DDevice9* s_bm_dev                        = nullptr;
	IDirect3DVertexShader9* s_bm_vs                   = nullptr;
	IDirect3DPixelShader9* s_bm_ps[ kChBlendCount ]   = { };
	bool s_bm_ps_bad[ kChBlendCount ]                 = { };
	IDirect3DTexture9* s_bm_tex                       = nullptr;
	IDirect3DSurface9* s_bm_surf                      = nullptr;
	UINT s_bm_w = 0, s_bm_h = 0;
	IDirect3DSurface9* s_bm_resolve                   = nullptr;
	D3DSURFACE_DESC s_bm_resolve_desc{ };
	UINT s_bm_fail_w = 0, s_bm_fail_h = 0;
	int s_bm_copy_frame                               = -1;
	IDirect3DSurface9* s_bm_copy_rt                   = nullptr; /* identity only, never dereferenced */
	bool s_bm_dead                                    = false;
	bool s_bm_live                                    = false;
	/* engine caches bound textures + shader constants: what we overwrite must go back exactly, the pixel state block misses these */
	IDirect3DBaseTexture9* s_bm_saved_tex             = nullptr;
	float s_bm_saved_vsc[ 4 ]                         = { };
	float s_bm_saved_psc[ 4 ]                         = { };

	void blend_release_targets( )
	{
		if ( s_bm_resolve ) {
			s_bm_resolve->Release( );
			s_bm_resolve = nullptr;
		}
		if ( s_bm_surf ) {
			s_bm_surf->Release( );
			s_bm_surf = nullptr;
		}
		if ( s_bm_tex ) {
			s_bm_tex->Release( );
			s_bm_tex = nullptr;
		}
		s_bm_w = s_bm_h     = 0;
		s_bm_resolve_desc   = D3DSURFACE_DESC{ };
		s_bm_copy_frame     = -1;
		s_bm_copy_rt        = nullptr;
	}

	void blend_release( )
	{
		blend_release_targets( );
		for ( IDirect3DPixelShader9*& p : s_bm_ps )
			if ( p ) {
				p->Release( );
				p = nullptr;
			}
		for ( bool& b : s_bm_ps_bad )
			b = false;
		if ( s_bm_vs ) {
			s_bm_vs->Release( );
			s_bm_vs = nullptr;
		}
		s_bm_fail_w = s_bm_fail_h = 0;
		s_bm_dev                  = nullptr;
		s_bm_dead                 = false;
		s_bm_live                 = false;
	}

	void* blend_pack( const int mode )
	{
		return reinterpret_cast< void* >( ( static_cast< intptr_t >( ImGui::GetFrameCount( ) ) << 8 ) | static_cast< intptr_t >( mode ) );
	}

	IDirect3DPixelShader9* blend_shader( IDirect3DDevice9* dev, const int mode )
	{
		if ( dev != s_bm_dev ) {
			blend_release( );
			s_bm_dev = dev;
		}
		if ( s_bm_dead || s_bm_ps_bad[ mode ] )
			return nullptr;

		if ( !s_bm_vs ) {
			D3DCAPS9 caps{ };
			if ( FAILED( dev->GetDeviceCaps( &caps ) ) || caps.PixelShaderVersion < D3DPS_VERSION( 3, 0 ) ||
			     caps.VertexShaderVersion < D3DVS_VERSION( 3, 0 ) ) {
				botox_dbg_log( "chblend: no shader model 3 -- blend modes draw normal" );
				s_bm_dead = true;
				return nullptr;
			}

			std::vector< DWORD > code( ( sizeof( k_fxChBlendVsBytes ) + 3 ) / 4 ); /* BYTE arrays carry no alignment */
			memcpy( code.data( ), k_fxChBlendVsBytes, sizeof( k_fxChBlendVsBytes ) );
			if ( FAILED( dev->CreateVertexShader( code.data( ), &s_bm_vs ) ) ) {
				botox_dbg_log( "chblend: vertex shader refused" );
				s_bm_vs   = nullptr;
				s_bm_dead = true;
				return nullptr;
			}
		}

		if ( !s_bm_ps[ mode ] ) {
			const FxChBlendPs& b = k_fxChBlendPs[ mode ];
			std::vector< DWORD > code( ( b.size + 3 ) / 4 );
			if ( b.bytes )
				memcpy( code.data( ), b.bytes, b.size );
			if ( !b.bytes || FAILED( dev->CreatePixelShader( code.data( ), &s_bm_ps[ mode ] ) ) ) {
				botox_dbg_log( "chblend: pixel shader for mode %d refused", mode );
				s_bm_ps[ mode ]     = nullptr;
				s_bm_ps_bad[ mode ] = true;
				return nullptr;
			}
		}
		return s_bm_ps[ mode ];
	}

	/* X8R8G8B8 keyed on size only: game frame and the A8 menu layer both blend in one frame. DEFAULT pool, release_textures on Reset */
	bool blend_targets( IDirect3DDevice9* dev, const D3DSURFACE_DESC& rt )
	{
		if ( !s_bm_surf || s_bm_w != rt.Width || s_bm_h != rt.Height ) {
			if ( s_bm_fail_w == rt.Width && s_bm_fail_h == rt.Height )
				return false;
			blend_release_targets( );
			HRESULT hr =
				dev->CreateTexture( rt.Width, rt.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &s_bm_tex, nullptr );
			if ( FAILED( hr ) )
				hr = dev->CreateTexture( rt.Width, rt.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_bm_tex, nullptr );
			if ( SUCCEEDED( hr ) )
				hr = s_bm_tex->GetSurfaceLevel( 0, &s_bm_surf );
			if ( FAILED( hr ) ) {
				botox_dbg_log( "chblend: copy target %ux%u failed 0x%08lx", rt.Width, rt.Height, static_cast< unsigned long >( hr ) );
				blend_release_targets( );
				s_bm_fail_w = rt.Width;
				s_bm_fail_h = rt.Height;
				return false;
			}
			s_bm_w = rt.Width;
			s_bm_h = rt.Height;
		}

		if ( rt.MultiSampleType == D3DMULTISAMPLE_NONE )
			return true;

		if ( s_bm_resolve && s_bm_resolve_desc.Width == rt.Width && s_bm_resolve_desc.Height == rt.Height && s_bm_resolve_desc.Format == rt.Format )
			return true;
		if ( s_bm_resolve ) {
			s_bm_resolve->Release( );
			s_bm_resolve = nullptr;
		}
		if ( FAILED( dev->CreateRenderTarget( rt.Width, rt.Height, rt.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &s_bm_resolve, nullptr ) ) ) {
			botox_dbg_log( "chblend: msaa resolve target %ux%u fmt %d failed", rt.Width, rt.Height, static_cast< int >( rt.Format ) );
			s_bm_resolve = nullptr;
			blend_release_targets( );
			s_bm_fail_w = rt.Width;
			s_bm_fail_h = rt.Height;
			return false;
		}
		s_bm_resolve_desc = rt;
		return true;
	}

	void blend_on( const ImDrawList*, const ImDrawCmd* cmd )
	{
		s_bm_live                = false;
		IDirect3DDevice9* dev    = device( );
		const intptr_t packed    = reinterpret_cast< intptr_t >( cmd->UserCallbackData );
		const int mode           = static_cast< int >( packed & 0xFF );
		const int frame          = static_cast< int >( packed >> 8 );
		if ( !dev || mode < 2 || mode >= kChBlendCount )
			return;

		/* any failure below leaves imgui's state alone: arms draw normal */
		IDirect3DPixelShader9* ps = blend_shader( dev, mode );
		if ( !ps )
			return;

		IDirect3DSurface9* rt = nullptr;
		if ( FAILED( dev->GetRenderTarget( 0, &rt ) ) || !rt )
			return;
		D3DSURFACE_DESC desc{ };
		HRESULT hr = rt->GetDesc( &desc );
		if ( SUCCEEDED( hr ) && !blend_targets( dev, desc ) )
			hr = E_FAIL;
		if ( SUCCEEDED( hr ) && ( s_bm_copy_frame != frame || s_bm_copy_rt != rt ) ) {
			hr = dev->StretchRect( rt, nullptr, s_bm_resolve ? s_bm_resolve : s_bm_surf, nullptr, D3DTEXF_NONE );
			if ( SUCCEEDED( hr ) && s_bm_resolve )
				hr = dev->StretchRect( s_bm_resolve, nullptr, s_bm_surf, nullptr, D3DTEXF_NONE );
			s_bm_copy_frame = SUCCEEDED( hr ) ? frame : -1;
			s_bm_copy_rt    = SUCCEEDED( hr ) ? rt : nullptr;
		}
		rt->Release( );
		if ( FAILED( hr ) ) {
			static bool logged = false;
			if ( !logged ) {
				logged = true;
				botox_dbg_log( "chblend: frame copy failed 0x%08lx", static_cast< unsigned long >( hr ) );
			}
			return;
		}

		/* vs must rebuild imgui_impl_dx9's ortho, which comes from DisplaySize */
		const ImVec2 ds     = ImGui::GetIO( ).DisplaySize;
		const float vsc[ 4 ] = { ds.x, ds.y, 0.f, 0.f };
		const float psc[ 4 ] = { 0.f, 1.f / static_cast< float >( s_bm_w ), 1.f / static_cast< float >( s_bm_h ), 0.f };

		if ( FAILED( dev->GetTexture( 1, &s_bm_saved_tex ) ) )
			s_bm_saved_tex = nullptr;
		dev->GetVertexShaderConstantF( 0, s_bm_saved_vsc, 1 );
		dev->GetPixelShaderConstantF( 0, s_bm_saved_psc, 1 );

		dev->SetVertexShader( s_bm_vs );
		dev->SetPixelShader( ps );
		dev->SetVertexShaderConstantF( 0, vsc, 1 );
		dev->SetPixelShaderConstantF( 0, psc, 1 );
		/* texture first: the game's wrapper turns srgb decode back on inside SetTexture */
		dev->SetTexture( 1, s_bm_tex );
		dev->SetSamplerState( 1, D3DSAMP_MINFILTER, D3DTEXF_POINT );
		dev->SetSamplerState( 1, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
		dev->SetSamplerState( 1, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
		dev->SetSamplerState( 1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		dev->SetSamplerState( 1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		dev->SetSamplerState( 1, D3DSAMP_SRGBTEXTURE, FALSE );
		s_bm_live = true;
	}

	void blend_off( const ImDrawList*, const ImDrawCmd* )
	{
		if ( !s_bm_live || !device( ) )
			return;
		s_bm_live = false;
		device( )->SetVertexShader( nullptr );
		device( )->SetPixelShader( nullptr );
		device( )->SetVertexShaderConstantF( 0, s_bm_saved_vsc, 1 );
		device( )->SetPixelShaderConstantF( 0, s_bm_saved_psc, 1 );
		device( )->SetTexture( 1, s_bm_saved_tex );
		if ( s_bm_saved_tex ) {
			s_bm_saved_tex->Release( ); /* GetTexture AddRef'd */
			s_bm_saved_tex = nullptr;
		}
	}

	ImU32 pack_rgba( const float c[ 4 ] )
	{
		return IM_COL32( static_cast< int >( c[ 0 ] * 255.f ), static_cast< int >( c[ 1 ] * 255.f ), static_cast< int >( c[ 2 ] * 255.f ),
		                 static_cast< int >( c[ 3 ] * 255.f ) );
	}

	void draw_custom( ImDrawList* dl, const settings_t& s, float cx, float cy, const float sc, const float speed )
	{
		s_aa                 = s.aa;
		const int vtx_start  = dl->VtxBuffer.Size;
		const int blend      = s.blend;
		if ( blend == 1 )
			dl->AddCallback( blend_add_on, nullptr );

		const bool sub_pixel = ( s.thickness < 1.0f );
		const float th_px    = s_aa ? ( s.thickness * k_unit_to_px * sc ) : ( sub_pixel ? 1.0f : floorf( s.thickness * k_unit_to_px * sc ) );

		if ( !s_aa ) {
			const float off = ( static_cast< int >( th_px ) % 2 != 0 ) ? 0.5f : 0.0f;
			cx              = floorf( cx ) + off;
			cy              = floorf( cy ) + off;
		}

		const float len = s_aa ? ( s.length * k_unit_to_px * sc ) : floorf( s.length * k_unit_to_px * sc );
		const float gap = s_aa ? ( s.gap * k_unit_to_px * sc ) : floorf( s.gap * k_unit_to_px * sc );

		float dyn_gap_px = 0.f;
		if ( s.dynamic && s.dynamic_ref > 0.f && speed >= 0.f && speed > s.dynamic_min ) {
			const float range = s.dynamic_ref - s.dynamic_min;
			float t           = ( range > 0.f ) ? ( speed - s.dynamic_min ) / range : 1.f;
			if ( t > 1.f )
				t = 1.f;
			dyn_gap_px = s_aa ? ( t * s.dynamic_max * k_unit_to_px * sc ) : floorf( t * s.dynamic_max * k_unit_to_px * sc );
		}
		const float effective_gap = gap + dyn_gap_px;
		const float gap_asp       = s.gap_aspect > 0.01f ? s.gap_aspect : 0.01f;
		const float h_gap         = s_aa ? ( effective_gap * ( gap_asp >= 1.0f ? gap_asp : 1.0f ) )
		                                 : floorf( effective_gap * ( gap_asp >= 1.0f ? gap_asp : 1.0f ) );
		const float v_gap         = s_aa ? ( effective_gap * ( gap_asp <= 1.0f ? ( 1.0f / gap_asp ) : 1.0f ) )
		                                 : floorf( effective_gap * ( gap_asp <= 1.0f ? ( 1.0f / gap_asp ) : 1.0f ) );

		const float aspect = s.aspect > 0.01f ? s.aspect : 0.01f;
		const float h_len  = s_aa ? ( len * ( aspect >= 1.0f ? aspect : 1.0f ) ) : floorf( len * ( aspect >= 1.0f ? aspect : 1.0f ) );
		const float v_len  = s_aa ? ( len * ( aspect <= 1.0f ? ( 1.0f / aspect ) : 1.0f ) ) : floorf( len * ( aspect <= 1.0f ? ( 1.0f / aspect ) : 1.0f ) );

		const ImU32 col = pack_rgba( s.color );

		const bool fancy_arm = !sub_pixel && ( s.gradient || s.taper );
		ImU32 grad_center_col = col, grad_outer_col = col;
		float grad_start = 0.f, grad_end = 1.f;
		float taper_near = 1.0f, taper_far = 1.0f;
		if ( fancy_arm ) {
			if ( s.gradient ) {
				grad_center_col = pack_rgba( s.grad_center );
				grad_outer_col  = pack_rgba( s.grad_outer );
				grad_start      = s.grad_start;
				grad_end        = s.grad_end;
			}
			taper_near = s.taper ? s.taper_near : 1.0f;
			taper_far  = s.taper ? s.taper_far : 1.0f;
		}

		const ImU32 outl = pack_rgba( s.outline_color );

		const bool sl = !s.outline_sides_only || s.outline_sides[ 0 ];
		const bool sr = !s.outline_sides_only || s.outline_sides[ 1 ];
		const bool st = !s.outline_sides_only || s.outline_sides[ 2 ];
		const bool sb = !s.outline_sides_only || s.outline_sides[ 3 ];

		auto arm_len     = [ & ]( int dir ) { return ( dir <= 1 ) ? h_len : v_len; };
		auto arm_gap     = [ & ]( int dir ) { return ( dir <= 1 ) ? h_gap : v_gap; };
		auto arm_enabled = [ & ]( int dir ) { return !s.sides_only || s.sides[ dir ]; };

		if ( s.outline ) {
			const float ot            = s.outline_thick < 0.1f ? 0.1f : s.outline_thick;
			const float ot_px         = s_aa ? ( ot * k_unit_to_px ) : ( ( ot < 1.f ) ? 1.f : floorf( ot * k_unit_to_px ) );
			const float arm_base_half = th_px * 0.5f;
			if ( s.outline_on_lines ) {
				for ( int i = 0; i < 4; ++i ) {
					if ( !arm_enabled( i ) )
						continue;
					const arm_t a = make_arm( cx, cy, th_px, arm_len( i ), arm_gap( i ), i );
					if ( fancy_arm && !s.outline_blur ) {
						arm_t out_a = a;
						if ( i <= 1 ) {
							out_a.x1 -= ot_px;
							out_a.x2 += ot_px;
						} else {
							out_a.y1 -= ot_px;
							out_a.y2 += ot_px;
						}
						const float add_scale = arm_base_half > 0.f ? ot_px / arm_base_half : 0.f;
						draw_arm_fancy( dl, out_a, i, outl, outl, 0.f, 0.f, taper_near + add_scale, taper_far + add_scale );
					} else if ( s.outline_blur )
						draw_outline_blur( dl, a.x1, a.y1, a.x2, a.y2, ot, outl, s.outline_blur_radius, sl, sr, st, sb );
					else
						draw_outline( dl, a.x1, a.y1, a.x2, a.y2, ot, outl, sl, sr, st, sb );
				}
			}
			if ( s.dot && s.outline_on_dot ) {
				const float r = th_px * 0.5f;
				if ( s.dot_circle && s.outline_blur ) {
					const float base_a  = static_cast< float >( ( outl >> 24 ) & 0xFF ) / 255.f;
					const ImU32 o_rgb   = outl & 0x00FFFFFFu;
					const float max_exp = s.outline_blur_radius * k_unit_to_px;
					const float sigma   = max_exp * 0.4f + 0.5f;
					float cum_t         = 1.f;
					for ( int i = 15; i >= 0; --i ) {
						const float br  = ( static_cast< float >( i + 1 ) / 16 ) * max_exp;
						const float tgt = base_a * expf( -0.5f * ( br / sigma ) * ( br / sigma ) );
						if ( cum_t < 1e-4f )
							break;
						const float la = 1.f - ( 1.f - tgt ) / cum_t;
						if ( la <= 0.f )
							continue;
						const int ai = static_cast< int >( la * 255.f + 0.5f );
						if ( ai <= 0 )
							continue;
						cum_t *= ( 1.f - la );
						dl->AddCircleFilled( ImVec2( cx, cy ), r + ot_px + br, o_rgb | ( static_cast< ImU32 >( ai ) << 24 ) );
					}
					dl->AddCircleFilled( ImVec2( cx, cy ), r + ot_px, outl );
				} else if ( s.dot_circle )
					dl->AddCircleFilled( ImVec2( cx, cy ), r + ot_px, outl );
				else if ( s.outline_blur )
					draw_outline_blur( dl, cx - r, cy - r, cx + r, cy + r, ot, outl, s.outline_blur_radius, sl, sr, st, sb );
				else
					draw_outline( dl, cx - r, cy - r, cx + r, cy + r, ot, outl, sl, sr, st, sb );
			}
		}

		/* frame copy lands here: outline is in it, arms blend on top */
		if ( blend >= 2 )
			dl->AddCallback( blend_on, blend_pack( blend ) );

		for ( int i = 0; i < 4; ++i ) {
			if ( !arm_enabled( i ) )
				continue;
			const arm_t a = make_arm( cx, cy, th_px, arm_len( i ), arm_gap( i ), i );
			if ( sub_pixel ) {
				draw_rect( dl, a.x1, a.y1, a.x2, a.y1 + 1.f, col );
				draw_rect( dl, a.x1, a.y1, a.x1 + 1.f, a.y2, col );
			} else if ( fancy_arm )
				draw_arm_fancy( dl, a, i, grad_center_col, grad_outer_col, grad_start, grad_end, taper_near, taper_far );
			else
				draw_rect( dl, a.x1, a.y1, a.x2, a.y2, col );
		}

		if ( s.dot ) {
			const float lo     = th_px * 0.5f;
			const ImU32 dot_col = s.dot_custom_color ? pack_rgba( s.dot_color ) : ( ( s.gradient && !sub_pixel ) ? grad_center_col : col );
			if ( s.dot_circle )
				dl->AddCircleFilled( ImVec2( cx, cy ), lo, dot_col );
			else
				draw_rect( dl, cx - lo, cy - lo, cx + lo, cy + lo, dot_col );
		}

		if ( fabsf( s.rotation ) > 0.01f ) {
			const float rad = s.rotation * IM_PI / 180.f;
			const float ca = cosf( rad ), sa = sinf( rad );
			for ( int vi = vtx_start; vi < dl->VtxBuffer.Size; ++vi ) {
				const float dx           = dl->VtxBuffer[ vi ].pos.x - cx;
				const float dy           = dl->VtxBuffer[ vi ].pos.y - cy;
				dl->VtxBuffer[ vi ].pos.x = cx + dx * ca - dy * sa;
				dl->VtxBuffer[ vi ].pos.y = cy + dx * sa + dy * ca;
			}
		}

		if ( blend == 1 )
			dl->AddCallback( blend_add_off, nullptr );
		else if ( blend >= 2 )
			dl->AddCallback( blend_off, nullptr );
	}
} // namespace

void n_custom_crosshair::on_paint_traverse( )
{
	if ( !GET_VARIABLE( g_variables.m_custom_crosshair, bool ) || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || g_ctx.m_local->is_scoped( ) )
		return;

	if ( !GET_VARIABLE( g_variables.m_force_crosshair, bool ) ) {
		const auto handle = g_ctx.m_local->get_active_weapon_handle( );
		const auto weapon = handle ? g_interfaces.m_client_entity_list->get< c_base_entity >( handle ) : nullptr;
		if ( weapon && g_utilities.is_in< short >( weapon->get_item_definition_index( ),
		                                           { e_item_definition_index::weapon_awp, e_item_definition_index::weapon_ssg08,
		                                             e_item_definition_index::weapon_scar20, e_item_definition_index::weapon_g3sg1 } ) )
			return;
	}

	const settings_t s = read_settings( );
	const float speed  = g_ctx.m_local->get_velocity( ).length_2d( );
	const float cx     = g_ctx.m_width * 0.5f;
	const float cy     = g_ctx.m_height * 0.5f;

	/* same x squeeze push_crosshair_rect does when the overlay list is not stretched already */
	const bool stretched = g_resolution_spoof.aspect_cheat_overlay( ) || g_resolution_spoof.upscale_stretches( );
	const float aspect   = stretched ? 1.f : g_resolution_spoof.overlay_aspect_scale( );

	g_render.m_draw_data.emplace_back(
		e_draw_type::draw_type_callback, std::make_any< callback_draw_object_t >( callback_draw_object_t{ [ s, speed, cx, cy, aspect ]( ImDrawList* list ) {
			const int first = list->VtxBuffer.Size;
			draw_custom( list, s, cx, cy, 1.f, speed );

			if ( fabsf( aspect - 1.f ) >= 0.001f )
				for ( int v = first; v < list->VtxBuffer.Size; ++v )
					list->VtxBuffer[ v ].pos.x = cx + ( list->VtxBuffer[ v ].pos.x - cx ) * aspect;
		} } ) );
}

void n_custom_crosshair::draw_preview( ImDrawList* list, const ImVec2& min, const ImVec2& max, const float test_speed )
{
	const float w = max.x - min.x, h = max.y - min.y;
	if ( w < 4.f || h < 4.f )
		return;

	const settings_t s = read_settings( );

	/* shrink until the widest arm fits the box */
	const float aspect       = s.aspect > 0.01f ? s.aspect : 0.01f;
	const float longer_scale = aspect >= 1.f ? aspect : 1.f / aspect;
	const float extent_px    = fmaxf( 0.f, floorf( s.gap * k_unit_to_px ) ) + floorf( s.length * k_unit_to_px * longer_scale ) +
	                        floorf( s.thickness * k_unit_to_px * 0.5f );
	const float max_extent = ( w < h ? w : h ) * 0.45f;
	const float scale      = ( extent_px > max_extent && extent_px > 0.f ) ? max_extent / extent_px : 1.f;

	list->PushClipRect( min, max, true );
	draw_custom( list, s, min.x + w * 0.5f, min.y + h * 0.5f, scale, test_speed );
	list->PopClipRect( );
}

void n_custom_crosshair::draw_preview_scene( ImDrawList* list, const ImVec2& p0, const float side )
{
	static float s_t = 0.f;
	s_t += ImMin( ImGui::GetIO( ).DeltaTime, 0.1f );

	auto fract = []( double x ) { return static_cast< float >( x - floor( x ) ); };
	auto smooth = []( float a, float b, float x ) {
		const float t = ImClamp( ( x - a ) / ( b - a ), 0.f, 1.f );
		return t * t * ( 3.f - 2.f * t );
	};
	/* iq gradient noise, 0..1 */
	auto noise = [ & ]( float px, float py ) {
		const float ix = floorf( px ), iy = floorf( py ), fx = px - ix, fy = py - iy;
		const float ux = fx * fx * ( 3.f - 2.f * fx ), uy = fy * fy * ( 3.f - 2.f * fy );
		auto g = [ & ]( float ox, float oy ) {
			const double hx = ( ix + ox ) * 2127.1 + ( iy + oy ) * 81.17;
			const double hy = ( ix + ox ) * 1269.5 + ( iy + oy ) * 283.37;
			return ( fract( sin( hx ) * 43758.5453 ) * 2.f - 1.f ) * ( fx - ox ) + ( fract( sin( hy ) * 43758.5453 ) * 2.f - 1.f ) * ( fy - oy );
		};
		const float n = ImLerp( ImLerp( g( 0.f, 0.f ), g( 1.f, 0.f ), ux ), ImLerp( g( 0.f, 1.f ), g( 1.f, 1.f ), ux ), uy );
		return 0.5f + 0.5f * n;
	};

	const float c5 = cosf( -5.f * IM_PI / 180.f ), s5 = sinf( -5.f * IM_PI / 180.f );
	auto shade = [ & ]( float u, float v ) {
		float x = u - 0.5f, y = v - 0.5f;
		const float deg = noise( s_t * 0.1f, x * y );
		const float a   = ( ( deg - 0.5f ) * 720.f + 180.f ) * IM_PI / 180.f;
		const float c = cosf( a ), s = sinf( a );
		const float rx = x * c - y * s, ry = x * s + y * c;
		x = rx;
		y = ry;

		const float speed = s_t * 2.f;
		x += sinf( y * 5.f + speed ) / 30.f;
		y += sinf( x * 7.5f + speed ) / 15.f;

		const float k = smooth( -0.3f, 0.2f, x * c5 - y * s5 );
		const float m = smooth( 0.5f, -0.3f, y );
		const float l1[ 3 ] = { ImLerp( 0.800f, 0.550f, k ), ImLerp( 0.660f, 0.700f, k ), ImLerp( 0.470f, 0.820f, k ) };
		const float l2[ 3 ] = { ImLerp( 0.660f, 0.330f, k ), ImLerp( 0.250f, 0.450f, k ), ImLerp( 0.220f, 0.550f, k ) };
		return IM_COL32( static_cast< int >( ImLerp( l1[ 0 ], l2[ 0 ], m ) * 255.f ), static_cast< int >( ImLerp( l1[ 1 ], l2[ 1 ], m ) * 255.f ),
		                 static_cast< int >( ImLerp( l1[ 2 ], l2[ 2 ], m ) * 255.f ), 255 );
	};

	constexpr int n = 32;
	ImU32 col[ n + 1 ][ n + 1 ];
	for ( int j = 0; j <= n; ++j )
		for ( int i = 0; i <= n; ++i )
			col[ j ][ i ] = shade( static_cast< float >( i ) / n, 1.f - static_cast< float >( j ) / n );

	const float cell = side / n;
	for ( int j = 0; j < n; ++j )
		for ( int i = 0; i < n; ++i )
			list->AddRectFilledMultiColor( ImVec2( p0.x + i * cell, p0.y + j * cell ), ImVec2( p0.x + ( i + 1 ) * cell, p0.y + ( j + 1 ) * cell ),
			                               col[ j ][ i ], col[ j ][ i + 1 ], col[ j + 1 ][ i + 1 ], col[ j + 1 ][ i ] );
}

bool n_custom_crosshair::blend_combo( const char* id, int* mode )
{
	/* shown subset of kChBlend; id = kChBlend index, so saved configs and shaders stay valid */
	struct entry_t {
		const char* cat;
		const char* name;
		int id;
	};
	static constexpr entry_t k_shown[] = {
		{ "basic", "normal", 0 },        { "basic", "additive", 1 },          { "darken", "darken", 19 },
		{ "darken", "multiply", 4 },     { "darken", "color burn", 17 },      { "lighten", "lighten", 27 },
		{ "lighten", "screen", 29 },     { "lighten", "color dodge", 25 },    { "contrast", "overlay", 60 },
		{ "contrast", "soft light", 36 }, { "contrast", "hard light", 33 },   { "difference", "difference", 53 },
		{ "difference", "exclusion", 56 }, { "difference", "subtract", 3 },   { "color", "hue", 82 },
		{ "color", "color", 81 },        { "color", "luminosity", 85 },
	};

	const int cur        = clamp_blend( *mode );
	const char* preview  = kChBlend[ cur ].name;
	for ( const entry_t& e : k_shown )
		if ( e.id == cur )
			preview = e.name;

	const bool open = ImGui::BeginCombo( id, preview, ImGuiComboFlags_HeightLargest );
	/* read now: after EndCombo the last item is a popup row */
	const bool hovered = !open && ImGui::IsItemHovered( );
	if ( !open )
		return hovered;

	const char* cat = nullptr;
	for ( const entry_t& e : k_shown ) {
		if ( !cat || strcmp( cat, e.cat ) != 0 ) {
			if ( cat )
				ImGui::TextDisabled( "%s", e.cat );
			cat = e.cat;
		}
		ImGui::PushID( e.id );
		if ( ImGui::Selectable( e.name, e.id == cur ) )
			*mode = e.id;
		ImGui::PopID( );
		if ( e.id == cur )
			ImGui::SetItemDefaultFocus( );
	}
	ImGui::EndCombo( );
	return false;
}

void n_custom_crosshair::release_textures( )
{
	blend_release( );
}
