#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "../../../globals/render/render.h"

class c_texture;

/* visuals > esp "esp preview" box right of the menu: a CMergedMDL agent ( the game's own inventory / main menu model renderer, no
   entity, works in the main menu ) drawn into an own render target with the live chams passes, the 2d esp from n_players::draw_player
   on top. backtrack = 64 fake records shifted right ( a strafing target ), thinned by the live ghost cap */
namespace n_esp_preview
{
	constexpr float k_width  = 260.f;
	constexpr float k_height = 400.f;

	struct impl_t {
		/* game thread, MatSystemTopPanel paint: model + chams into the target, esp into the draw list */
		void on_paint_traverse( );
		void on_eject( );

		/* render thread, inside the preview window: image + esp, drag rotates */
		void draw_body( );

	private:
		bool init( );
		void render( );

		std::uint8_t* m_model = nullptr; /* CMergedMDL, 0x75C */
		c_texture* m_texture  = nullptr;
		c_texture* m_cubemap  = nullptr;
		bool m_failed         = false;
		bool m_ready          = false;
		bool m_constructed    = false;
		int m_stage           = 0;

		int m_bone_map[ 256 ]{ }; /* agent bone -> animset bone, -1 = parent's */
		int m_agent_bones = 0;

		int m_seen_frame = -1;
		float m_last_render = -1.f;
		std::atomic< int > m_frame{ -1 };
		std::atomic< float > m_yaw{ 180.f };
		std::atomic< unsigned int > m_clear{ 0xFF191919u };
		std::atomic< c_texture* > m_shown_texture{ nullptr };

		std::mutex m_mutex;
		std::vector< draw_object_t > m_draw_data, m_shown;
		const void* m_shown_atlas = nullptr; /* text holds this atlas' ImFont*: install_fonts retires it */
	};
}

inline n_esp_preview::impl_t g_esp_preview{ };
