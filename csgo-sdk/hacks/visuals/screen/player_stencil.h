#pragma once
#include <atomic>
#include <d3d9.h>
#include <vector>

#include "../../../game/sdk/classes/c_color.h"
#include "../players/players.h"

namespace n_player_stencil
{
	/* one mask pass: frame px whose stencil passes ( func, ref ) write 1 into channels ( r g b = class 1 2 3 ); zero = clear them
	   there instead */
	struct fill_t {
		DWORD m_func     = D3DCMP_EQUAL;
		DWORD m_ref      = 0;
		DWORD m_channels = D3DCOLORWRITEENABLE_RED;
		bool m_zero      = false;
		DWORD m_mask     = 0xffffffff; /* stencil test mask */
	};

	/* stencil band model_glow.cpp stamps in ( ( stencil & test ) == ref = 0x50..0x57, low bits only ). the player mask zeroes it too,
	   so refs a skipped pass never cleared can't read as a visible player */
	inline constexpr DWORD k_model_band_test = 0x78;
	inline constexpr DWORD k_model_band_ref  = 0x50;

	/* wipe a source's refs after the read: where ( stencil & m_test ) == m_ref the m_write bits go to 0, other users' bits
	   ( web surface / tonemap 0x80 ) stay. m_write 0 = no clear */
	struct clear_t {
		DWORD m_test  = 0;
		DWORD m_ref   = 0;
		DWORD m_write = 0;
	};

	/* what a source wants drawn, per class ( players: visible, hidden, - / viewmodel: weapon, gloves, sleeves ). class colour
	   alpha 0 = that class keeps its coverage ( others' glow stops at it ) but draws nothing */
	struct look_t {
		bool m_glow    = false;
		bool m_outline = false;

		c_color m_glow_inner[ 3 ]{ };
		c_color m_glow_outer[ 3 ]{ };
		bool m_gradient        = false;
		float m_gradient_inner = 60.f;
		bool m_normal_blend    = false;
		float m_glow_thickness = 15.f;

		bool m_wave = false;
		c_color m_wave_color{ };
		float m_wave_speed = 0.5f, m_wave_size = 0.35f, m_wave_strength = 0.8f, m_wave_angle = 0.f;

		bool m_pulse        = false;
		float m_pulse_speed = 1.f, m_pulse_min = 35.f;

		c_color m_outline_color[ 3 ]{ };
		float m_outline_thickness = 1.5f;

		bool m_outline_wave = false;
		c_color m_outline_wave_color{ };
		float m_outline_wave_speed = 0.5f, m_outline_wave_size = 0.35f, m_outline_wave_strength = 0.8f, m_outline_wave_angle = 0.f;
	};

	struct impl_t {
		bool enabled( ) const;

		bool glow_enabled( ) const;

		// outline, glow or inner glow would run
		bool wanted( ) const;

		// main thread, right after the original DoPostScreenSpaceEffects ( = RenderGlowEffects )
		void after_glow( );

		/* main thread, esp preview: the bound target's depth-stencil holds glow refs. bones = inner glow capsules seen through
		   world_to_screen, scale = target px per screen px ( radius / thickness ) */
		void after_glow( const view_matrix_t& world_to_screen, const std::vector< n_players::hitbox_capsule_t >& bones, float scale );

		/* main thread, model_glow.cpp: the same passes over the bound target, mask from fills ( up to 4 ) instead of the glow refs.
		   queued behind whatever stencil draws came first, clear right after the read so nothing later ( engine halo tests == 0,
		   the player mask reads 2..127 ) sees the refs */
		void submit( const look_t& look, const fill_t* fills, int fill_count, const clear_t& clear );

		/* split version for refs that do not live until the passes ( viewmodel: ironsight clears stencil before post ): capture
		   builds only the mask ( + the clear ), submit_captured runs the passes off it. same frame id, nothing else may run between */
		void capture( const fill_t* fills, int fill_count, const clear_t& clear, int frame );
		void submit_captured( const look_t& look, int frame );

		/* main thread: shaders built and never failed = a queued pass will read ( and clear ) the refs. stamp nothing before.
		   a pass that still skips leaves them: the player mask ignores that band ( k_model_band_* ) */
		bool ready( ) const { return !this->m_failed && this->m_ready; }

		// main thread: a model_glow pass faulted ( n_fx_compat marks it dead for good ). own flag: fx_compat's list is d3d thread only
		bool model_dead( ) const { return this->m_model_dead; }

		/* main thread: the last model_glow pass reached draw ( = its clear ran ). false after a target / depth-stencil mismatch or a
		   failed build: stamp nothing ( halos test == 0 ), probe( ) until the checks pass again */
		bool model_live( ) const { return this->m_model_live; }
		void probe( );

		// main thread: builds the shaders ( one per call ) when no player pass would. model_glow calls it until ready( )
		void warm( );

		// d3d thread, before Reset / on unload: everything here is D3DPOOL_DEFAULT
		void on_device_lost( );

	private:
		struct settings_t {
			bool m_outline = false;
			bool m_glow    = false;

			// players: glow liveness feeds the engine halo fallback ( glow_enabled )
			bool m_track_live = false;

			// n_fx_compat name: a driver fault in one source never turns the others off
			const char* m_name = "player_stencil";

			enum e_phase : int { phase_full, phase_capture, phase_captured };
			int m_phase         = phase_full;
			int m_capture_frame = -1;

			static constexpr int k_max_fills = 4;
			fill_t m_fills[ k_max_fills ]{ };
			int m_fill_count = 0;

			clear_t m_clear{ };

			float m_visible[ 4 ]   = { };
			float m_invisible[ 4 ] = { };
			float m_third[ 4 ]     = { }; /* c80 */
			float m_thickness      = 1.f;
			float m_outline_wave[ 4 ]       = { };
			float m_outline_wave_color[ 4 ] = { };

			float m_glow_visible[ 4 ]         = { };
			float m_glow_invisible[ 4 ]       = { };
			float m_glow_visible_outer[ 4 ]   = { };
			float m_glow_invisible_outer[ 4 ] = { };
			float m_glow_third[ 4 ]           = { }; /* c81 */
			float m_glow_third_outer[ 4 ]     = { }; /* c82 */
			float m_gradient[ 4 ] = { };
			float m_wave[ 4 ]       = { };
			float m_wave_color[ 4 ] = { };

			/* wave anchored to each player's body, not the screen: c15 = glow anchor, box count, outline anchor.
			   boxes = ndc bounds per player ( x0 y0 x1 y1, main thread ), phase = per-player wave offset */
			static constexpr int k_max_boxes = 32;
			float m_wave_anchor[ 4 ]              = { };
			float m_wave_angle[ 4 ]               = { 1.f, 0.f, 1.f, 0.f }; /* c14: glow cos sin, outline cos sin */
			float m_boxes[ k_max_boxes ][ 4 ]     = { };
			float m_box_phase[ k_max_boxes ][ 4 ] = { };

			bool m_inner                 = false;
			float m_inner_reach[ 4 ]     = { };
			float m_inner_visible[ 4 ]   = { };
			float m_inner_invisible[ 4 ] = { };

			// bones mode: hitbox capsules projected on the main thread. ends in ndc, sigma in ndc y units per end
			bool m_inner_bones = false;
			struct capsule_t {
				float m_a[ 2 ], m_b[ 2 ], m_sigma[ 2 ];
			};
			std::vector< capsule_t > m_capsules;
		};

		bool glow_wanted( ) const;
		bool inner_enabled( ) const;

		void run( const view_matrix_t* world_to_screen, const std::vector< n_players::hitbox_capsule_t >* given, float scale );

		static void apply_look( settings_t& settings, const look_t& look, float scale );

		void queue( const settings_t& settings );

		// render thread when queued, else calling thread. draw returns whether the column passes ran
		void execute( const settings_t& settings );
		bool draw( IDirect3DDevice9* device, IDirect3DSurface9* target, IDirect3DSurface9* depth_stencil, const settings_t& settings );

		bool build( IDirect3DDevice9* device, const D3DSURFACE_DESC& description );
		bool ensure_shaders( IDirect3DDevice9* device );
		void release_targets( );

		IDirect3DPixelShader9* m_fill_shader    = nullptr;
		IDirect3DPixelShader9* m_rows_shader    = nullptr;
		IDirect3DPixelShader9* m_blur_shader    = nullptr;
		IDirect3DPixelShader9* m_glow_shader    = nullptr;
		IDirect3DPixelShader9* m_columns_shader = nullptr;
		IDirect3DPixelShader9* m_inner_shader   = nullptr;
		IDirect3DPixelShader9* m_capsule_shader = nullptr;
		IDirect3DPixelShader9* m_bone_shader    = nullptr;

		IDirect3DTexture9* m_mask_texture     = nullptr;
		IDirect3DSurface9* m_mask_surface     = nullptr;
		IDirect3DTexture9* m_distance_texture = nullptr;
		IDirect3DSurface9* m_distance_surface = nullptr;
		IDirect3DTexture9* m_blur_texture     = nullptr;
		IDirect3DSurface9* m_blur_surface     = nullptr;

		IDirect3DSurface9* m_msaa_surface = nullptr;

		unsigned int m_width               = 0;
		unsigned int m_height              = 0;
		D3DFORMAT m_format                 = D3DFMT_UNKNOWN;
		D3DMULTISAMPLE_TYPE m_multi_sample = D3DMULTISAMPLE_NONE;
		DWORD m_multi_sample_quality       = 0;

		// failed compile is final. render thread writes, main reads
		std::atomic< bool > m_failed{ false };

		// every shader built. render thread writes, main reads
		std::atomic< bool > m_ready{ false };

		// a "model_glow" pass came back dead. render thread writes, main reads
		std::atomic< bool > m_model_dead{ false };

		// last model_glow pass reached draw / probe passed. render thread writes, main reads
		std::atomic< bool > m_model_live{ true };

		// target + depth-stencil fit and targets built: draw may run. render thread
		bool drawable( IDirect3DDevice9* device, IDirect3DSurface9*& target, IDirect3DSurface9*& depth_stencil );

		// glow ran on the last frame it was wanted. render thread writes, main reads
		std::atomic< bool > m_glow_live{ true };

		// frame id whose mask m_mask_texture holds ( capture ), -1 = none. render thread only
		int m_captured_frame = -1;
	};
}

inline n_player_stencil::impl_t g_player_stencil{ };

/* own targets: the preview target is smaller than the screen, one shared set would rebuild twice a frame */
inline n_player_stencil::impl_t g_preview_stencil{ };
