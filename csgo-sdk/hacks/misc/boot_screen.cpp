#include "misc.h"
#include "../visuals/screen/fx_compat.h"
#include "../visuals/screen/stream_guard.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace
{
	constexpr const char* k_steps[] = { "modules", "interfaces", "netvars", "convars", "input", "config", "hooks", "render", "fonts", "effects" };
	constexpr int k_count           = static_cast< int >( std::size( k_steps ) );
	constexpr int k_fonts           = k_count - 2;
	constexpr int k_effects         = k_count - 1;
	constexpr const char* k_times   = "C:\\botox\\boot_times.txt";

	/* last inject's step times (ms), bar weights. defaults until one inject has finished */
	float g_expected[ k_count ] = { 50.f, 30.f, 60.f, 20.f, 10.f, 300.f, 400.f, 150.f, 2500.f, 600.f };
	std::atomic< long long > g_start{ 0 }, g_stamp[ k_count ]{ };
	std::atomic< int > g_done{ 0 };
	std::atomic< bool > g_drawn{ false }, g_finished{ false }, g_eject{ false }, g_eject_cover{ false }, g_eject_done{ false };
	std::atomic< long long > g_last_draw{ 0 };

	/* render thread only */
	float g_reveal   = 0.f;
	int g_splash_vtx = -1;

	long long now_us( )
	{
		return std::chrono::duration_cast< std::chrono::microseconds >( std::chrono::steady_clock::now( ).time_since_epoch( ) ).count( );
	}

	void save_times( )
	{
		CreateDirectoryA( "C:\\botox", nullptr );

		FILE* file = nullptr;
		if ( fopen_s( &file, k_times, "w" ) || !file )
			return;

		long long previous = g_start;
		for ( int i = 0; i < k_count; ++i ) {
			const float actual = static_cast< float >( g_stamp[ i ] - previous ) / 1000.f;
			previous           = g_stamp[ i ];
			fprintf( file, "%.1f\n", ( g_expected[ i ] + actual ) * 0.5f );
		}

		fclose( file );
	}

	/* done steps count whole, the running one creeps to 90% of its expected time */
	void progress( const long long now, float& fraction )
	{
		const int done = g_done;
		float total = 0.f, filled = 0.f;

		for ( int i = 0; i < k_count; ++i ) {
			const float weight = std::max( g_expected[ i ], 1.f );
			total += weight;

			if ( i < done )
				filled += weight;
			else if ( i == done ) {
				const float spent = static_cast< float >( now - ( i ? g_stamp[ i - 1 ].load( ) : g_start.load( ) ) ) / 1000.f;
				filled += weight * std::min( spent / weight, 0.9f );
			}
		}

		fraction = filled / total;
	}

	/* shadertoy port, warped fbm blob. iChannel0 ( 256 white noise, repeat ) = hashed value noise, smoothstep lerp.
	   uv = shadertoy ( I*2-R )/R.y, y up, square spans -2..2. luma in LEVELS steps, tinted c1 = accent.
	   c0 = time, fade ( 50% ). output on black = additive */
	constexpr char k_glow_ps[] = R"(
#define LEVELS 4.0

float4 c0 : register( c0 );
float4 c1 : register( c1 );

float hash( float2 p )
{
	float3 p3 = frac( p.xyx * 0.1031 );
	p3 += dot( p3, p3.yzx + 33.33 );
	return frac( ( p3.x + p3.y ) * p3.z );
}

float noise2( float2 p )
{
	p = p * 256.0 - 0.5;
	float2 i = floor( p ), f = frac( p );
	f = f * f * ( 3.0 - 2.0 * f );
	i -= 256.0 * floor( i / 256.0 );
	float2 j = i + 1.0;
	j -= 256.0 * floor( j / 256.0 );
	float a = hash( i ), b = hash( float2( j.x, i.y ) ), c = hash( float2( i.x, j.y ) ), d = hash( j );
	return lerp( lerp( a, b, f.x ), lerp( c, d, f.x ), f.y );
}

float fbm( float2 p )
{
	float amp = 1.0, n = 0.0, f = 1.0;
	for ( int i = 0; i < 6; i++ ) {
		n += noise2( p * f * 0.01 ) * amp;
		amp *= 0.5;
		f *= 2.0;
	}
	return n;
}

float warp( float2 p )
{
	float2 q = float2( fbm( p ), fbm( p + float2( 5.2, 2.3 ) ) );
	float2 r = float2( fbm( p + 0.1 * q + float2( 8.7, 2.2 ) ), fbm( p + 0.1 * q + float2( 3.3, 1.8 ) ) );
	return fbm( p + 4.2 * r );
}

float3 palette( float v )
{
	return sin( float3( 1.0, 2.0, 3.0 ) + v ) * 0.5 + 0.5;
}

float3 tanh3( float3 x )
{
	return 1.0 - 2.0 / ( exp( 2.0 * x ) + 1.0 );
}

float4 main( float2 uv : TEXCOORD0 ) : COLOR
{
	float t    = c0.x;
	float dist = length( uv );
	float2 dir = uv / max( dist, 1e-5 );
	float n    = warp( float2( dir.x * 0.1 - t * 0.1, dir.y * 0.1 - t * 0.1 ) + dist * 0.1 );
	float d    = max( n - dist * 1.1, 0.0 );
	float3 col = tanh3( palette( d * 5.0 + t * 2.0 ) * d * 2.0 );
	float l    = dot( col, float3( 0.299, 0.587, 0.114 ) );
	l          = min( floor( l * LEVELS ), LEVELS - 1.0 ) / ( LEVELS - 1.0 );
	l         *= 1.0 - smoothstep( 0.9, 1.7, dist );
	return float4( l * c1.rgb * c0.y, 0.0 );
}
)";

	struct glow_t {
		IDirect3DPixelShader9* m_shader = nullptr;
		bool m_failed                   = false;
		/* set by boot_screen_draw, read by the callback, both render thread */
		float m_x = 0.f, m_y = 0.f, m_size = 0.f, m_time = 0.f, m_fade = 0.f;
		ImVec4 m_color{ };
	} g_glow;

	void glow_draw( const ImDrawList*, const ImDrawCmd* )
	{
		IDirect3DDevice9* const device = g_interfaces.m_direct_device;
		if ( !device || g_glow.m_failed )
			return;

		if ( !g_glow.m_shader ) {
			ID3DXBuffer *code = nullptr, *errors = nullptr;
			const bool ok = SUCCEEDED( D3DXCompileShader( k_glow_ps, sizeof( k_glow_ps ) - 1, nullptr, nullptr, "main", "ps_3_0", 0, &code, &errors, nullptr ) ) &&
			                code && SUCCEEDED( device->CreatePixelShader( static_cast< const DWORD* >( code->GetBufferPointer( ) ), &g_glow.m_shader ) );

			if ( !ok ) {
				g_glow.m_shader = nullptr;
				g_glow.m_failed = true;
				botox_dbg_log( "BOOT: glow shader failed: %s", errors ? static_cast< const char* >( errors->GetBufferPointer( ) ) : "CreatePixelShader" );
			}

			if ( code )
				code->Release( );
			if ( errors )
				errors->Release( );
			if ( !ok )
				return;
		}

		struct vertex_t {
			float m_x, m_y, m_z, m_rhw, m_u, m_v;
		};

		const float x = g_glow.m_x - 0.5f, y = g_glow.m_y - 0.5f, s = g_glow.m_size;
		/* uv +-2: blob ends by dist ~1.8 ( fbm < 1.97 / 1.1 ), so the square edge never cuts it */
		const vertex_t quad[ 4 ] = { { x, y, 0.f, 1.f, -2.f, 2.f }, { x + s, y, 0.f, 1.f, 2.f, 2.f }, { x, y + s, 0.f, 1.f, -2.f, -2.f }, { x + s, y + s, 0.f, 1.f, 2.f, -2.f } };
		const float constants[ 2 ][ 4 ] = { { g_glow.m_time, g_glow.m_fade, 0.f, 0.f }, { g_glow.m_color.x, g_glow.m_color.y, g_glow.m_color.z, 1.f } };

		auto pass = [ & ] {
			float old_constant[ 2 ][ 4 ]{ };
			DWORD old_srgb = 0;
			device->GetPixelShaderConstantF( 0, old_constant[ 0 ], 2 );
			device->GetRenderState( D3DRS_SRGBWRITEENABLE, &old_srgb );

			n_stream_guard::state_t streams{ };
			streams.capture( device );

			device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
			device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
			device->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_ONE );
			device->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_ONE );
			device->SetPixelShader( g_glow.m_shader );
			device->SetPixelShaderConstantF( 0, constants[ 0 ], 2 );

			n_fx_compat::draw_up( device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof( vertex_t ) );

			/* DrawPrimitiveUP nulled stream 0 and the vs path swapped the declaration: imgui's next cmd needs both back.
			   blend / scissor / shaders come back with ImDrawCallback_ResetRenderState */
			device->SetPixelShaderConstantF( 0, old_constant[ 0 ], 2 );
			device->SetRenderState( D3DRS_SRGBWRITEENABLE, old_srgb );
			device->SetFVF( D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 );
			streams.restore( device );
		};

		n_fx_compat::run( device, "boot_glow", pass );
	}
}

void boot_screen_release( )
{
	if ( g_glow.m_shader )
		g_glow.m_shader->Release( );

	g_glow.m_shader = nullptr;
	g_glow.m_failed = false;
}

void boot_screen_begin( )
{
	g_start = now_us( );

	FILE* file = nullptr;
	if ( fopen_s( &file, k_times, "r" ) || !file )
		return;

	float loaded[ k_count ]{ };
	bool ok = true;
	for ( float& ms : loaded )
		ok = ok && fscanf_s( file, "%f", &ms ) == 1 && ms >= 0.f && ms < 120000.f;

	fclose( file );

	if ( ok )
		std::ranges::copy( loaded, g_expected );
}

void boot_screen_step( )
{
	const int index = g_done;
	if ( index >= k_count )
		return;

	g_stamp[ index ] = now_us( );
	g_done           = index + 1;
}

/* main thread: returns at flag, or at once when no frames come (minimised), 1.5s cap */
static void wait_for( const std::atomic< bool >& flag )
{
	for ( int i = 0; i < 150 && !flag; ++i ) {
		if ( now_us( ) - g_last_draw > 250'000 )
			return;
		Sleep( 10 );
	}
}

void boot_screen_eject_begin( )
{
	g_eject = true;
	wait_for( g_eject_cover );
}

void boot_screen_eject_end( )
{
	g_eject_done = true;
	wait_for( g_finished );
}

bool boot_screen_active( )
{
	/* eject: passes drop under the full cover, the game fades back in clean */
	if ( g_eject )
		return g_eject_cover;

	/* fonts done: passes run under the cover, their shader builds stutter there */
	if ( g_finished || g_done >= k_effects )
		return false;

	/* no overlay frame ever (no steam overlay): give effects back 10s after hooks */
	return g_drawn || g_done < k_fonts - 1 || now_us( ) - g_stamp[ k_fonts - 2 ] < 10'000'000;
}

void boot_screen_draw( )
{
	static float ramp = 0.f, shown = 0.f, clock = 0.f, hold = 0.f, warm = 0.f;
	static int calm   = 0;
	static bool ejecting = false, saved = false;

	/* every frame, finished too: eject's wait reads it as "frames are coming" */
	const long long now = now_us( );
	g_last_draw         = now;

	if ( g_eject && !ejecting ) {
		ejecting = true;
		shown = clock = hold = 0.f;
		g_finished = false;
	}

	/* clamped: a hitch frame never skips the fade */
	const float dt = std::clamp( ImGui::GetIO( ).DeltaTime, 0.f, 1.f / 30.f );

	/* rest of the overlay: in after the inject splash is gone, out while the eject splash comes in */
	if ( ejecting )
		g_reveal = std::max( g_reveal - dt / 0.35f, 0.f );
	else if ( g_finished )
		g_reveal = std::min( g_reveal + dt / 0.4f, 1.f );

	g_splash_vtx = -1;

	if ( g_finished )
		return;

	g_drawn = true;
	clock += dt;

	float fraction = 1.f;
	bool done      = false;

	if ( !ejecting ) {
		/* hooks go live inside the hooks step: wait for the init thread to stamp it */
		if ( g_done == k_fonts - 1 )
			boot_screen_step( );

		if ( g_done == k_fonts && !g_render.m_reload_fonts && !g_render.m_font_build_running && !g_render.m_uploading_fonts && !g_render.m_pending_fonts.load( ) )
			boot_screen_step( );

		/* warm = 15 frames in a row under 50 ms (a shader build resets it), 4s cap for slow pcs */
		if ( g_done == k_effects ) {
			warm += dt;
			calm = ImGui::GetIO( ).DeltaTime < 0.05f ? calm + 1 : 0;

			if ( calm >= 15 || warm > 4.f )
				boot_screen_step( );
		}

		/* a stuck step must never hide the overlay for good */
		while ( clock > 20.f && g_done < k_count )
			boot_screen_step( );

		done = g_done >= k_count;
		if ( done && !std::exchange( saved, true ) )
			save_times( );

		if ( !done )
			progress( now, fraction );
	}
	else {
		done = g_eject_done;
		if ( !done )
			fraction = 0.9f * ( 1.f - std::exp( -clock / 0.6f ) );
	}

	if ( done )
		hold += dt;

	const bool show = !done || hold < ( ejecting ? 0.25f : 0.35f );
	ramp            = show ? std::min( ramp + dt / 0.35f, 1.f ) : std::max( ramp - dt / 0.45f, 0.f );

	if ( ejecting && ramp >= 1.f )
		g_eject_cover = true;

	if ( !show && ramp <= 0.f ) {
		g_finished = true;
		return;
	}

	const float fade = ramp * ramp * ( 3.f - 2.f * ramp );

	shown += ( fraction - shown ) * std::min( dt * 12.f, 1.f );

	ImDrawList* const list = ImGui::GetForegroundDrawList( );
	g_splash_vtx           = list->VtxBuffer.Size;
	const float w = g_ctx.m_width, h = g_ctx.m_height, u = h / 1080.f;
	const auto color = [ fade ]( const float a ) { return IM_COL32( 255, 255, 255, static_cast< int >( a * fade * 255.f + 0.5f ) ); };

	screen_blur_fullscreen( list, color( 1.f ), 5 );
	list->AddRectFilled( ImVec2( 0.f, 0.f ), ImVec2( w, h ), IM_COL32( 0, 0, 0, static_cast< int >( 0.18f * fade * 255.f ) ) );

	/* content grows 94 -> 100% and rises 10 px with the fade */
	const float grow = 0.94f + 0.06f * fade;
	const ImVec2 center( w * 0.5f, h * 0.5f + 10.f * u * ( 1.f - fade ) );

	/* glow behind bar + label: own quad + shader, fade rides in c0 ( fade_others skips it, no verts ) */
	g_glow.m_size = std::round( 640.f * u * grow );
	g_glow.m_x    = std::floor( center.x - g_glow.m_size * 0.5f );
	g_glow.m_y    = std::floor( center.y - g_glow.m_size * 0.5f );
	g_glow.m_time  = clock;
	g_glow.m_fade  = fade * 0.5f;
	g_glow.m_color = GET_VARIABLE( g_variables.m_accent, c_color ).get_vec4( );

	list->AddCallback( glow_draw, nullptr );
	list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );

	char text[ 96 ];
	if ( ejecting )
		done ? strcpy_s( text, "ejected" ) : sprintf_s( text, "ejecting   %d%%", static_cast< int >( shown * 100.f ) );
	else if ( done )
		strcpy_s( text, "injected" );
	else
		sprintf_s( text, "injecting %s   %d%%", k_steps[ std::min( g_done.load( ), k_count - 1 ) ], static_cast< int >( shown * 100.f ) );

	/* menu "botox" label font, native size: monochrome verdana goes soft when scaled */
	ImFont* const font  = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ] ? g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ] : ImGui::GetFont( );
	const ImVec2 extent = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, text );
	const float dot_w   = done ? 0.f : font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, "." ).x;

	/* bar + gap + label centered as one block */
	const float bar_w = 260.f * u * grow, bar_h = std::max( std::round( 4.f * u ), 2.f ), gap = std::round( 12.f * u );
	const ImVec2 bar_min( std::floor( center.x - bar_w * 0.5f ), std::floor( center.y - ( bar_h + gap + extent.y ) * 0.5f ) );
	const ImVec2 bar_max( bar_min.x + bar_w, bar_min.y + bar_h );

	list->AddRectFilled( bar_min, bar_max, color( 0.15f ), bar_h * 0.5f );
	if ( shown > 0.001f )
		list->AddRectFilled( bar_min, ImVec2( bar_min.x + bar_w * std::min( shown, 1.f ), bar_max.y ), color( 0.95f ), bar_h * 0.5f );

	/* dots counted in the width so the label never shifts */
	const ImVec2 text_pos( std::floor( center.x - ( extent.x + 3.f * dot_w ) * 0.5f ), bar_max.y + gap );
	list->AddText( font, font->FontSize, text_pos, color( 1.f ), text );

	/* working: 3 dots pulse left to right, 1.2s loop */
	for ( int i = 0; i < 3 && !done; ++i ) {
		const float wave = 0.5f + 0.5f * std::cos( ( clock - static_cast< float >( i ) * 0.2f ) * ( 2.f * IM_PI / 1.2f ) );
		list->AddText( font, font->FontSize, ImVec2( text_pos.x + extent.x + static_cast< float >( i ) * dot_w, text_pos.y ), color( 0.2f + 0.8f * wave ), "." );
	}
}

void boot_screen_fade_others( ImDrawData* draw_data )
{
	if ( g_reveal >= 1.f || !draw_data )
		return;

	const float eased        = g_reveal * g_reveal * ( 3.f - 2.f * g_reveal );
	const ImU32 scale        = static_cast< ImU32 >( eased * 256.f );
	ImDrawList* const splash = ImGui::GetForegroundDrawList( );

	for ( int i = 0; i < draw_data->CmdListsCount; ++i ) {
		ImDrawList* const list = draw_data->CmdLists[ i ];
		const int end          = list == splash && g_splash_vtx >= 0 ? std::min( g_splash_vtx, list->VtxBuffer.Size ) : list->VtxBuffer.Size;

		for ( int v = 0; v < end; ++v ) {
			ImU32& col = list->VtxBuffer.Data[ v ].col;
			col        = ( col & ~IM_COL32_A_MASK ) | ( ( ( ( col >> IM_COL32_A_SHIFT ) & 0xFF ) * scale >> 8 ) << IM_COL32_A_SHIFT );
		}
	}
}

