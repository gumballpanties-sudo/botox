#pragma once

struct IDirect3DDevice9;

/* flashbang diagnostic: view_draw_fade fills it, flash_debug( ) prints + clears. plain ints, no
   lock: main thread writes counters, d3d thread writes samples */
struct flash_probe_t {
	int m_flash_fades;
	int m_other_fades;
	int m_last_color[ 4 ];
	char m_last_name[ 64 ];

	/* 5 back buffer points x 3 stages, -1 = unread. [ 0 ] centre, [ 1..4 ] 25% / 75% points.
	   FINAL counts: the back buffer at end_scene is a frame stale, only Present is what was seen */
	int m_pre[ 5 ][ 3 ];
	int m_post[ 5 ][ 3 ];
	int m_final[ 5 ][ 3 ];

	bool m_blind;

	bool m_fresh;
};

inline flash_probe_t g_flash_probe{ };

/* stage 0 = end_scene game frame, 1 = after our grade, 2 = Present (after spoofer). scene must be
   CLOSED (readback fails inside BeginScene). stage 0 paces, all three = one frame */
void flash_probe_sample( IDirect3DDevice9* device, int stage );
