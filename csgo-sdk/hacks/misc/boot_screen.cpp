#include "misc.h"

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

	float cubic( const float a, const float b, const float s )
	{
		const float r = 1.f - s;
		return 3.f * a * s * r * r + 3.f * b * s * s * r + s * s * s;
	}

	/* svg keySplines = css cubic-bezier: x -> y */
	float ease( const float x1, const float y1, const float x2, const float y2, const float x )
	{
		float lo = 0.f, hi = 1.f;
		for ( int i = 0; i < 16; ++i ) {
			const float mid = ( lo + hi ) * 0.5f;
			( cubic( x1, x2, mid ) < x ? lo : hi ) = mid;
		}
		return cubic( y1, y2, ( lo + hi ) * 0.5f );
	}

	/* 6-dots-scale: r 0 -> 2 at 20% -> 0 over 0.6s, then frozen at 0 */
	float dot_radius( const float t )
	{
		const float u = t / 0.6f;
		if ( u >= 1.f )
			return 0.f;
		if ( u < 0.2f )
			return 2.f * ease( 0.f, 1.f, 0.f, 1.f, u / 0.2f );
		return 2.f * ( 1.f - ease( 0.53f, 0.f, 0.61f, 0.73f, ( u - 0.2f ) / 0.8f ) );
	}
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

	/* svg: 24 unit box, 12 dots on r 9 clockwise from top, 0.1s apart, 1.2s loop */
	const float scale = 52.f * u * grow / 24.f;
	const ImVec2 spin( center.x, center.y - 22.f * u * grow );

	for ( int i = 0; i < 12; ++i ) {
		const float r = dot_radius( std::fmod( clock + 1.2f - static_cast< float >( i ) * 0.1f, 1.2f ) ) * scale;
		if ( r < 0.25f )
			continue;

		const float angle = static_cast< float >( i ) * ( IM_PI / 6.f );
		list->AddCircleFilled( ImVec2( spin.x + std::sin( angle ) * 9.f * scale, spin.y - std::cos( angle ) * 9.f * scale ), r, color( 1.f ) );
	}

	const float bar_w = 260.f * u * grow, bar_h = std::max( std::round( 4.f * u ), 2.f );
	const ImVec2 bar_min( std::floor( center.x - bar_w * 0.5f ), std::floor( center.y + 24.f * u * grow ) );
	const ImVec2 bar_max( bar_min.x + bar_w, bar_min.y + bar_h );

	list->AddRectFilled( bar_min, bar_max, color( 0.15f ), bar_h * 0.5f );
	if ( shown > 0.001f )
		list->AddRectFilled( bar_min, ImVec2( bar_min.x + bar_w * std::min( shown, 1.f ), bar_max.y ), color( 0.95f ), bar_h * 0.5f );

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

	list->AddText( font, font->FontSize, ImVec2( std::floor( center.x - extent.x * 0.5f ), std::floor( bar_max.y + 12.f * u ) ), color( 1.f ), text );
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

