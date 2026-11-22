#include "misc.h"
#include "../menu/menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

extern void botox_dbg_log( const char* fmt, ... );

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

	/* dna menu particles ( random size + alpha, sine twinkle ), flying out of the center. dna alpha 0..40 was invisible here */
	struct particle_t {
		ImVec2 m_dir;
		float m_dist, m_speed, m_size, m_alpha, m_phase, m_rate;
	};

	particle_t g_particles[ 75 ];
	bool g_particles_init = false;

	float frand( )
	{
		return static_cast< float >( rand( ) ) / static_cast< float >( RAND_MAX );
	}

	void spawn( particle_t& p, const float dist )
	{
		const float angle = frand( ) * 2.f * IM_PI;
		p.m_dir           = ImVec2( std::cos( angle ), std::sin( angle ) );
		p.m_dist          = dist;
		p.m_speed         = 60.f + frand( ) * 160.f;
		p.m_size          = 2.5f + frand( ) * 2.5f;
		p.m_alpha         = 0.4f + frand( ) * 0.5f;
		p.m_phase         = static_cast< float >( rand( ) % 1000 ) * 0.01f;
		p.m_rate          = 0.6f + static_cast< float >( rand( ) % 40 ) / 100.f;
	}

	/* scale = alpha x 256 */
	void scale_alpha( ImDrawList* list, const int begin, const int end, const ImU32 scale )
	{
		for ( int v = begin; v < end; ++v ) {
			ImU32& col = list->VtxBuffer.Data[ v ].col;
			col        = ( col & ~IM_COL32_A_MASK ) | ( ( ( ( col >> IM_COL32_A_SHIFT ) & 0xFF ) * scale >> 8 ) << IM_COL32_A_SHIFT );
		}
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

	screen_blur_fullscreen( list, IM_COL32( 255, 255, 255, static_cast< int >( fade * 255.f + 0.5f ) ), 5 );
	list->AddRectFilled( ImVec2( 0.f, 0.f ), ImVec2( w, h ), IM_COL32( 0, 0, 0, static_cast< int >( 0.18f * fade * 255.f ) ) );

	const ImVec2 center( w * 0.5f, h * 0.5f );

	/* first frame spreads them along their rays so the stream is already full; off screen = back to the center */
	const float reach = std::hypot( w, h ) * 0.5f + 4.f * u;
	const ImVec4 accent = GET_VARIABLE( g_variables.m_accent, c_color ).get_vec4( );
	if ( !std::exchange( g_particles_init, true ) ) {
		for ( auto& p : g_particles )
			spawn( p, frand( ) * reach );
		botox_dbg_log( "BOOT: particles %d reach=%.0f u=%.2f accent=%.2f %.2f %.2f", static_cast< int >( std::size( g_particles ) ), reach, u, accent.x, accent.y, accent.z );
	}

	for ( auto& p : g_particles ) {
		p.m_dist += p.m_speed * u * dt;
		if ( p.m_dist > reach )
			spawn( p, 0.f );

		p.m_phase += dt * p.m_rate;
		/* 40 px fade in off the center so they never pop */
		const float alpha = p.m_alpha * ( 0.6f + 0.4f * std::sin( p.m_phase ) ) * std::min( p.m_dist / ( 40.f * u ), 1.f ) * fade;
		list->AddCircleFilled( ImVec2( center.x + p.m_dir.x * p.m_dist, center.y + p.m_dir.y * p.m_dist ), p.m_size * u,
		                       ImGui::GetColorU32( ImVec4( accent.x, accent.y, accent.z, alpha ) ) );
	}

	char label[ 64 ], percent[ 8 ], date[ 48 ];
	if ( done )
		strcpy_s( label, ejecting ? "ejected" : "injected" );
	else if ( ejecting )
		strcpy_s( label, "ejecting" );
	else
		sprintf_s( label, "injecting %s", k_steps[ std::min( g_done.load( ), k_count - 1 ) ] );
	sprintf_s( percent, "%d%%", done ? 100 : static_cast< int >( std::clamp( shown, 0.f, 1.f ) * 100.f ) );
	sprintf_s( date, " | %s", menu_build_date( ) );

	/* the default menu window in small, never the picked theme: band with its title, accent lines, slider look bar.
	   drawn opaque, alpha scaled after; one block = dpi panel scale + aspect stretch */
	const auto block     = g_render.begin_stretch_block( list );
	const int card_begin = list->VtxBuffer.Size;

	ImFont* const bold    = g_render.menu_font( true ) ? g_render.menu_font( true ) : ImGui::GetFont( );
	ImFont* const regular = g_render.menu_font( false ) ? g_render.menu_font( false ) : ImGui::GetFont( );
	const auto width      = []( ImFont* font, const char* text ) { return font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, text ).x; };

	constexpr float k_width = 280.f, k_pad = 12.f, k_gap = 7.f, k_bar = 8.f;
	const ImGuiStyle& style = ImGui::GetStyle( );
	const ImU32 accent_col  = ImGui::GetColorU32( ImGuiCol_Accent ), band_col = ImGui::GetColorU32( ImGuiCol_MenuBand );
	const ImU32 text_col    = ImGui::GetColorU32( ImGuiCol_Text );

	/* slides up 10 px with the fade, whole px: text stays sharp */
	const ImVec2 size( k_width, menu_band_height + k_pad + regular->FontSize + k_gap + k_bar + k_pad + menu_bottom_band_height );
	const ImVec2 pos( std::floor( center.x - size.x * 0.5f ), std::floor( center.y - size.y * 0.5f + 10.f * ( 1.f - fade ) ) );
	const ImVec2 max( pos.x + size.x, pos.y + size.y );

	list->AddRectFilled( pos, max, ImGui::GetColorU32( ImGuiCol_WindowBg ), style.WindowRounding );
	list->AddRectFilled( pos, ImVec2( max.x, pos.y + menu_band_height ), band_col, style.WindowRounding, ImDrawFlags_RoundCornersTop );
	list->AddRectFilled( ImVec2( pos.x, max.y - menu_bottom_band_height ), max, band_col, style.WindowRounding, ImDrawFlags_RoundCornersBottom );
	list->AddRect( pos, max, ImGui::GetColorU32( ImGuiCol_MenuWindowOutline ), style.WindowRounding );
	RenderFadedGradientLine( list, ImVec2( pos.x, pos.y + menu_band_height - 1.f ), ImVec2( size.x, 1.f ), accent_col );
	RenderFadedGradientLine( list, ImVec2( pos.x, max.y - menu_bottom_band_height ), ImVec2( size.x, 1.f ), accent_col );

	/* menu title: bold name, accent build date */
	const ImVec2 title_size = bold->CalcTextSizeA( bold->FontSize, FLT_MAX, 0.f, "botox" );
	const ImVec2 title_pos( pos.x + ( size.x - title_size.x - width( regular, date ) ) / 2.f, pos.y + ( menu_band_height - title_size.y ) / 2.f );
	list->AddText( bold, bold->FontSize, title_pos, text_col, "botox" );
	list->AddText( regular, regular->FontSize, ImVec2( title_pos.x + title_size.x, title_pos.y ), accent_col, date );

	const float left = pos.x + k_pad, right = max.x - k_pad, row_y = pos.y + menu_band_height + k_pad;
	list->AddText( regular, regular->FontSize, ImVec2( left, row_y ), text_col, label );
	list->AddText( regular, regular->FontSize, ImVec2( right - width( regular, percent ), row_y ), ImGui::GetColorU32( ImGuiCol_TextDisabled ), percent );

	/* working: 3 dots pulse left to right, 1.2s loop */
	const float label_w = width( regular, label ), dot_w = width( regular, "." );
	for ( int i = 0; i < 3 && !done; ++i ) {
		const float wave = 0.5f + 0.5f * std::cos( ( clock - static_cast< float >( i ) * 0.2f ) * ( 2.f * IM_PI / 1.2f ) );
		list->AddText( regular, regular->FontSize, ImVec2( left + label_w + static_cast< float >( i ) * dot_w, row_y ),
		               ImGui::GetColorU32( ImGuiCol_Text, 0.2f + 0.8f * wave ), "." );
	}

	/* menu slider: frame, outline, solid accent fill over both */
	const ImVec2 bar_min( left, row_y + regular->FontSize + k_gap ), bar_max( right, bar_min.y + k_bar );
	const float fill = std::floor( ( bar_max.x - bar_min.x ) * std::clamp( shown, 0.f, 1.f ) );
	list->AddRectFilled( bar_min, bar_max, ImGui::GetColorU32( ImGuiCol_MenuFrame ), style.FrameRounding );
	list->AddRect( bar_min, bar_max, ImGui::GetColorU32( ImGuiCol_MenuOutline ), style.FrameRounding );
	if ( fill >= 1.f )
		list->AddRectFilled( bar_min, ImVec2( bar_min.x + fill, bar_max.y ), accent_col | IM_COL32_A_MASK, fill > 1.f ? style.FrameRounding : 0.f );

	scale_alpha( list, card_begin, list->VtxBuffer.Size, static_cast< ImU32 >( fade * 256.f ) );
	g_render.end_stretch_block( block );
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
		scale_alpha( list, 0, list == splash && g_splash_vtx >= 0 ? std::min( g_splash_vtx, list->VtxBuffer.Size ) : list->VtxBuffer.Size, scale );
	}
}

