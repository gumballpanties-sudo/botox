#include "../../globals/includes/includes.h"
#include "../../hacks/visuals/screen/depth_source.h"
#include "../../hacks/visuals/screen/resolution_spoof.h"
#include "../hooks.h"
#include "flash_probe.h"

/* res spoofer lives here: the back buffer at end_scene is a frame stale, Present is after everything */
long __stdcall n_detoured_functions::present( IDirect3DDevice9* device, const RECT* source, const RECT* dest, HWND window_override,
                                              const RGNDATA* dirty_region )
{
	static auto original = g_hooks.m_present.get_original< decltype( &n_detoured_functions::present ) >( );

	HOOK_SCOPE_OR_BAIL( original( device, source, dest, window_override, dirty_region ) );

	const bool overlay_pass = g_resolution_spoof.wants_overlay_pass( );

	g_resolution_spoof.on_present( device, overlay_pass ? std::function< void( ) >( [ device ]( ) {
		n_detoured_functions::draw_overlay_frame( device );
	} )
	                                                    : std::function< void( ) >( ) );

	flash_probe_sample( device, 2 );

	g_depth_source.on_present( device );

	static int missed    = 0;
	missed               = s_overlay_frame_seen ? 0 : ( missed < 3 ? missed + 1 : 3 );
	s_overlay_frame_seen = false;

	static auto end_scene_original = g_hooks.m_end_scene.get_original< decltype( &n_detoured_functions::end_scene ) >( );
	if ( missed >= 3 && end_scene_original && SUCCEEDED( device->BeginScene( ) ) ) {
		static bool logged = false;
		if ( !logged ) {
			logged = true;
			g_console.print( "no steam overlay EndScene (HLAE / overlay off): frame drawn from Present" );
		}

		overlay_end_scene( device, end_scene_original );
	}

	return original( device, source, dest, window_override, dirty_region );
}
