#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class c_uipanel;
class c_ui_engine;

/* default hud font, live: no fonts.conf edit, no restart.
   1. picked font FILE ( same list as the other pickers, g_fonts ) -> FcFreeTypeQuery -> pattern -> FcFontSetAdd into the
      application set of the fontconfig config panorama already uses. NOT FcConfigAppFontAddFile: that one runs valve's
      <fontpattern> whitelist ( fonts.conf ) and returned 0 for every system ttf.
   2. pango caches its family list / fontsets in the fontmap: the pango hooks ( hooks/functions/pango_fonts.cpp ) hand us
      the fontmap, pango_fc_font_map_cache_clear( ) after every add.
   3. the family swap happens in those hooks: panorama builds every request as pango_font_description_from_string(
      "Stratum2 Bold, 'Arial Unicode MS,Arial" ) / pango_attr_family_new( "Stratum2" ); rewrite_request( ) swaps the
      leading Stratum2* token for the picked family and keeps the fallbacks. sizes / weights never touched.
      ( the js style route is dead: fontFamily is a write-only alias and every write re-bakes the size from the computed
      one x ui-scale -> shrink; the `font` shorthand setter rejects an unset size; labels with an inherited size bake to 0 )
   4. js walk under CSGOHud only POKES text panels after a change ( letter-spacing on/off = text relayout = new pango
      request ) and carries the scale / nudge transform ( visual only ). counts come back as a child panel id
      `bxlog_...`, read natively ( get_child / get_id ) -> `HUDFONT: js`.
   off / eject = swap table cleared + poke. Var: m_def_hud_font = file display name, empty = off. Log `HUDFONT:` */
namespace n_hud_font
{
	struct impl_t {
		/* game thread, after g_scaleform.on_createmove. also the eject restore pass ( reads g_ctx.m_eject_restoring as off ) */
		void on_createmove( );

		/* menu: re-add + re-poke even if nothing changed */
		void apply_now( );

		/* pango hooks ( panorama's text thread ) */
		void note_fontmap( void* fontmap, int n_families );
		void note_layout( void* layout );
		/* "Stratum2 Bold, 'Arial Unicode MS,Arial" -> "<family>, 'Arial Unicode MS,Arial". logs every distinct request once */
		const char* rewrite_request( const char* str, bool with_size );
		/* pango size ( 1/1024 pt ) x font scale while a font is active; a transform scale would just blow the bitmap up */
		int scale_size( int size ) const;
		/* FT_Load_Glyph flags -> m_def_hud_font_quality: 0 stock, 1 sharp ( full hinting ), 2 smooth ( light ), 3 unhinted */
		int glyph_flags( int load_flags ) const;

	private:
		bool resolve_root( );
		std::string add_file( const std::string& display_name ); /* -> family name, "" = fontconfig refused */
		bool clear_pango_cache( );
		/* mode: "poke" = relayout every text panel + transform, "tick" = transform on untagged rows only, "off" = undo */
		void walk( const char* mode, bool report );
		void read_js_log( );
		std::string transform_js( ) const; /* scale / nudge vars -> quoted css transform, '' = none */

		std::atomic< void* > m_fontmap{ nullptr }; /* PangoFT2FontMap panorama made */
		std::atomic< int > m_list_calls{ 0 };
		bool m_need_clear = false;

		c_ui_engine* m_uiengine = nullptr;
		c_uipanel* m_root       = nullptr;

		std::string m_added{ };   /* display name last pushed into fontconfig */
		std::string m_family{ };  /* its family */
		std::string m_applied{ }; /* family the hud was last poked for */
		std::string m_pending{ }; /* debounce: typed name waits 0.5 s */
		bool m_apply_at_once = false; /* list click / apply button: no wait */
		std::string m_last_js_log{ };
		float m_pending_since = 0.f;
		float m_next_walk     = 0.f;
		float m_poke_at       = 0.f; /* second poke after the swap table changed ( late relayouts ) */

		std::string m_xf{ };      /* transform ( nudge only ) last walked */
		bool m_xf_dirty = false;  /* slider moved since the last walk */
		std::atomic< float > m_hook_scale{ 1.f }; /* read by scale_size on the text thread */
		std::atomic< int > m_hook_quality{ 0 };   /* read by glyph_flags on the text thread */
		float m_scale_dirty_at = 0.f;             /* scale / quality changed: poke 0.15 s after the last move ( relayout, fresh glyphs ) */
		bool m_quality_dirty = false;             /* quality changed: pango cache clear first ( glyph bitmaps live in the fonts ) */

		std::vector< std::string > m_in_set{ }; /* paths already in the app set, FcFontSetAdd owns them */

		/* hook side: family swap + distinct request log. text thread vs game thread */
		std::mutex m_hook_mutex;
		std::string m_hook_family{ };
		std::string m_hook_out{ }; /* rewritten string handed back to pango; valid until the next call ( pango copies it at once ) */
		std::vector< std::string > m_seen_requests{ };
	};
}

inline n_hud_font::impl_t g_hud_font{ };
