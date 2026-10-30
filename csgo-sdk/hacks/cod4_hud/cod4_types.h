#pragma once

/* tables tools/cod4_hud_gen.py fills (cod4_data.h). names / numbers straight from the COD4 1.7 install */
namespace n_cod4_data
{
	struct menu_item_t {
		const char* m_name;
		float m_rect[ 4 ];
		int m_horz_align, m_vert_align; // 0 = take the menu's
		int m_type;                     // 8 ownerdraw, 19 game message window
		int m_style;                    // 3 = shader (background * forecolor)
		int m_ownerdraw;
		float m_text_scale;
		int m_text_style; // 3 shadowed, 6 shadowed more
		int m_text_font;  // 0 default (by scale), 1 normal, 2 big, 3 small, 4 bold, 5 console, 6 objective
		int m_text_align; // 0 left, 1 center, 2 right
		float m_forecolor[ 4 ];
		const char* m_background;
		const char* m_visible; // menu expression source, "1" = always, "0" = no visible line (Item_Paint skips it)
		const char* m_exp_x;
		const char* m_exp_y;
		const char* m_exp_w;
		const char* m_exp_h;
		const char* m_exp_text;
		const char* m_exp_material;
		const char* m_exp_alpha;
		const char* m_text;
		int m_window_index;
		int m_window_mode;
	};

	struct menu_t {
		const char* m_name;
		float m_rect[ 4 ];
		int m_horz_align, m_vert_align;
		const char* m_visible;
		const char* m_exp_y;
		int m_first, m_count;
	};

	struct weapon_t {
		const char* m_cs;     // csgo weapon_ name
		const char* m_cod4;   // cod4 weapon def
		const char* m_name;   // localized displayName
		const char* m_hud_icon;
		int m_hud_icon_ratio; // 0 1:1, 1 2:1, 2 4:1
		const char* m_kill_icon;
		int m_kill_icon_ratio;
		int m_flip_kill_icon;
		int m_clip;           // 0 none 1 magazine 2 short magazine 3 shotgun 4 rocket 5 beltfed 6 alt
		int m_clip_size;
		float m_low_ammo;     // lowAmmoWarningThreshold
		const char* m_reticle_center;
		const char* m_reticle_side;
		int m_reticle_center_size, m_reticle_side_size, m_reticle_min_ofs;
		float m_hip_stand_min, m_hip_ducked_min, m_hip_max, m_hip_ducked_max;
		const char* m_dpad_icon;
		int m_dpad_icon_ratio;
		const char* m_ads_overlay; // adsOverlayShader, virtual w x h centered
		int m_ads_overlay_w, m_ads_overlay_h;
	};

	struct glyph_t {
		unsigned short m_letter;
		signed char m_x0, m_y0;
		unsigned char m_dx, m_w, m_h;
		float m_s0, m_t0, m_s1, m_t1;
	};

	struct font_t {
		const char* m_name;
		int m_pixel_height;
		const char* m_material;
		const char* m_glow_material;
		int m_first, m_count;
	};

	struct material_t {
		const char* m_name;
		const char* m_image; // iwd images/<m_image>.iwi, "$white" = solid
		int m_additive;
	};

	struct loc_t {
		const char* m_key;
		const char* m_text;
	};

	struct dvar_t {
		const char* m_name;
		int m_kind; // 0 bool 1 int 2 float 3 color 4 string
		float m_value[ 4 ];
		float m_min, m_max;
		const char* m_string;
		const char* m_desc;
	};

	struct faction_t {
		const char* m_name;
		const char* m_icon;
		float m_color[ 3 ];
	};
}
