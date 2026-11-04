#pragma once

struct ImDrawList;
struct ImVec2;

namespace n_custom_crosshair
{
	/* game thread, local valid: queues the in-game crosshair onto the render thread */
	void on_paint_traverse( );

	/* render thread (menu): the real renderer into [min,max]. test_speed < 0 = standing */
	void draw_preview( ImDrawList* list, const ImVec2& min, const ImVec2& max, float test_speed );
	void draw_preview_scene( ImDrawList* list, const ImVec2& p0, float side );

	/* returns true while the closed combo is hovered */
	bool blend_combo( const char* id, int* mode );

	/* render thread, before Reset and on unload: blend shaders + DEFAULT pool frame copy */
	void release_textures( );
}
