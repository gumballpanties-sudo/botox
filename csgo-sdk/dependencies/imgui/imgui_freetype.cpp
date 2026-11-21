// dear imgui: FreeType font builder (used as a replacement for the stb_truetype builder)
// (code)

// Get the latest version at https://github.com/ocornut/imgui/tree/master/misc/freetype
// Original code by @vuhdo (Aleksei Skriabin). Improvements by @mikesart. Maintained since 2019 by @ocornut.

// CHANGELOG
// (minor and older changes stripped away, please see git history for details)
//  2021/08/23: fixed crash when FT_Render_Glyph() fails to render a glyph and returns NULL.
//  2021/03/05: added ImGuiFreeTypeBuilderFlags_Bitmap to load bitmap glyphs.
//  2021/03/02: set 'atlas->TexPixelsUseColors = true' to help some backends with deciding of a prefered texture format.
//  2021/01/28: added support for color-layered glyphs via ImGuiFreeTypeBuilderFlags_LoadColor (require Freetype 2.10+).
//  2021/01/26: simplified integration by using '#define IMGUI_ENABLE_FREETYPE'.
//              renamed ImGuiFreeType::XXX flags to ImGuiFreeTypeBuilderFlags_XXX for consistency with other API. removed
//              ImGuiFreeType::BuildFontAtlas().
//  2020/06/04: fix for rare case where FT_Get_Char_Index() succeed but FT_Load_Glyph() fails.
//  2019/02/09: added RasterizerFlags::Monochrome flag to disable font anti-aliasing (combine with ::MonoHinting for best results!)
//  2019/01/15: added support for imgui allocators + added FreeType only override function SetAllocatorFunctions().
//  2019/01/10: re-factored to match big update in STB builder. fixed texture height waste. fixed redundant glyphs when merging. support for glyph
//  padding. 2018/06/08: added support for ImFontConfig::GlyphMinAdvanceX, GlyphMaxAdvanceX. 2018/02/04: moved to main imgui repository (away from
//  http://www.github.com/ocornut/imgui_club) 2018/01/22: fix for addition of ImFontAtlas::TexUvscale member. 2017/10/22: minor inconsequential change
//  to match change in master (removed an unnecessary statement). 2017/09/26: fixes for imgui internal changes. 2017/08/26: cleanup, optimizations,
//  support for ImFontConfig::RasterizerFlags, ImFontConfig::RasterizerMultiply. 2017/08/16: imported from https://github.com/Vuhdo/imgui_freetype
//  into http://www.github.com/ocornut/imgui_club, updated for latest changes in ImFontAtlas, minor tweaks.

// About Gamma Correct Blending:
// - FreeType assumes blending in linear space rather than gamma space.
// - See https://www.freetype.org/freetype2/docs/reference/ft2-base_interface.html#FT_Render_Glyph
// - For correct results you need to be using sRGB and convert to linear space in the pixel shader output.
// - The default dear imgui styles will be impacted by this change (alpha values will need tweaking).

// FIXME: cfg.OversampleH, OversampleV are not supported (but perhaps not so necessary with this rasterizer).

#include "imgui_freetype.h"
#include <freetype/freetype.h>
#include <freetype/ftglyph.h>
#include <freetype/ftmodapi.h>
#include <freetype/ftstroke.h>
#include <freetype/ftsynth.h>

#include "imgui_internal.h" // ImMin,ImMax,ImFontAtlasBuild*,
#include "imgui_px_slot.h"
#include <ft2build.h>
#include <stdint.h>

#ifdef _MSC_VER
#	pragma warning( disable : 4505 )  // unreferenced local function has been removed (stb stuff)
#	pragma warning( disable : 26812 ) // [Static Analyzer] The enum type 'xxx' is unscoped. Prefer 'enum class' over 'enum' (Enum.3).
#endif

#if defined( __GNUC__ )
#	pragma GCC diagnostic ignored "-Wpragmas"         // warning: unknown option after '#pragma GCC diagnostic' kind
#	pragma GCC diagnostic ignored "-Wunused-function" // warning: 'xxxx' defined but not used
#endif

//-------------------------------------------------------------------------
// Data
//-------------------------------------------------------------------------

// Default memory allocators
static void* ImGuiFreeTypeDefaultAllocFunc( size_t size, void* user_data )
{
	IM_UNUSED( user_data );
	return IM_ALLOC( size );
}
static void ImGuiFreeTypeDefaultFreeFunc( void* ptr, void* user_data )
{
	IM_UNUSED( user_data );
	IM_FREE( ptr );
}

// Current memory allocators
static void* ( *GImGuiFreeTypeAllocFunc )( size_t size, void* user_data ) = ImGuiFreeTypeDefaultAllocFunc;
static void ( *GImGuiFreeTypeFreeFunc )( void* ptr, void* user_data )     = ImGuiFreeTypeDefaultFreeFunc;
static void* GImGuiFreeTypeAllocatorUserData                              = NULL;

/* botox: why the last build returned false (render.cpp prints it). one writer at a time: builds never overlap */
char g_ft_build_fail[ 160 ] = "";
static FT_Error g_ft_face_error = 0;
static const char* g_ft_face_step = "";

void botox_dbg_log( const char* fmt, ... ); // globals/logger/debug_log.cpp

/* botox: zeroed alpha pixels. ImFontAtlasFlags_BotoxPixelSlot atlas = the session's reserved range first ( imgui_px_slot.h ) */
static unsigned char* ImFontAtlasAllocAlpha8( ImFontAtlas* atlas, size_t size )
{
	if ( atlas->Flags & ImFontAtlasFlags_BotoxPixelSlot ) {
		void* const slot = n_px_slot::take( size );
		botox_dbg_log( "FONT: pixels %.1f mb %s %p reserved %.1f mb", size / ( 1024.0 * 1024.0 ), slot ? "slot" : "heap", slot,
		               n_px_slot::reserved( ) / ( 1024.0 * 1024.0 ) );
		if ( slot )
			return ( unsigned char* )slot;
	}
	unsigned char* const px = ( unsigned char* )IM_ALLOC( size );
	if ( px )
		memset( px, 0, size );
	return px;
}

/* botox ImGuiFreeTypeBuilderFlags_File: freetype reads the file through a few small pages, a cjk .ttc is never one 13-19 mb block */
static const int k_font_file_pages = 4;
static const unsigned long k_font_file_page = 16 * 1024;

struct ImFontFileStream {
	FT_StreamRec Rec; // first: freetype hands this pointer back
	HANDLE File;
	unsigned long PageAt[ k_font_file_pages ], PageLen[ k_font_file_pages ];
	int Next;
	unsigned char Page[ k_font_file_pages ][ k_font_file_page ];
};

static bool ImFontFileRead( HANDLE file, unsigned long at, unsigned char* dst, unsigned long count )
{
	OVERLAPPED at_offset = { };
	at_offset.Offset     = at;
	DWORD got            = 0;
	return ReadFile( file, dst, count, &got, &at_offset ) && got == count;
}

static unsigned long ImFontFileStreamIo( FT_Stream stream, unsigned long offset, unsigned char* buffer, unsigned long count )
{
	ImFontFileStream* const s = ( ImFontFileStream* )stream;
	if ( count == 0 )
		return offset > stream->size; // seek, 0 = ok
	if ( offset >= stream->size )
		return 0;
	count = ImMin( count, stream->size - offset );

	const unsigned long page = k_font_file_page;
	if ( count >= page )
		return ImFontFileRead( s->File, offset, buffer, count ) ? count : 0;

	unsigned long done = 0;
	while ( done < count ) {
		const unsigned long at = offset + done;
		int p                  = 0;
		while ( p < k_font_file_pages && !( at >= s->PageAt[ p ] && at - s->PageAt[ p ] < s->PageLen[ p ] ) )
			p++;
		if ( p == k_font_file_pages ) {
			p       = s->Next;
			s->Next = ( s->Next + 1 ) % k_font_file_pages;
			const unsigned long base = at - at % page, len = ImMin( page, stream->size - base );
			s->PageLen[ p ] = 0;
			if ( !ImFontFileRead( s->File, base, s->Page[ p ], len ) )
				return done;
			s->PageAt[ p ]  = base;
			s->PageLen[ p ] = len;
		}
		const unsigned long n = ImMin( count - done, s->PageAt[ p ] + s->PageLen[ p ] - at );
		memcpy( buffer + done, s->Page[ p ] + ( at - s->PageAt[ p ] ), n );
		done += n;
	}
	return done;
}

static void ImFontFileStreamClose( FT_Stream stream )
{
	ImFontFileStream* const s = ( ImFontFileStream* )stream;
	CloseHandle( s->File );
	IM_FREE( s );
}

/* freetype owns it from FT_Open_Face on: closed by FT_Done_Face, or by FT_Open_Face itself on failure */
static FT_Stream ImFontFileStreamOpen( const char* path )
{
	const HANDLE file = CreateFileA( path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
	if ( file == INVALID_HANDLE_VALUE )
		return NULL;
	LARGE_INTEGER size = { };
	ImFontFileStream* const s = GetFileSizeEx( file, &size ) && size.QuadPart > 0 && size.QuadPart < 0x7fffffff
	                                ? ( ImFontFileStream* )IM_ALLOC( sizeof( ImFontFileStream ) )
	                                : NULL;
	if ( !s ) {
		CloseHandle( file );
		return NULL;
	}
	memset( s, 0, offsetof( ImFontFileStream, Page ) );
	s->File      = file;
	s->Rec.size  = ( unsigned long )size.QuadPart;
	s->Rec.read  = ImFontFileStreamIo;
	s->Rec.close = ImFontFileStreamClose;
	return &s->Rec;
}

//-------------------------------------------------------------------------
// Code
//-------------------------------------------------------------------------

namespace
{
	// Glyph metrics:
	// --------------
	//
	//                       xmin                     xmax
	//                        |                         |
	//                        |<-------- width -------->|
	//                        |                         |
	//              |         +-------------------------+----------------- ymax
	//              |         |    ggggggggg   ggggg    |     ^        ^
	//              |         |   g:::::::::ggg::::g    |     |        |
	//              |         |  g:::::::::::::::::g    |     |        |
	//              |         | g::::::ggggg::::::gg    |     |        |
	//              |         | g:::::g     g:::::g     |     |        |
	//    offsetX  -|-------->| g:::::g     g:::::g     |  offsetY     |
	//              |         | g:::::g     g:::::g     |     |        |
	//              |         | g::::::g    g:::::g     |     |        |
	//              |         | g:::::::ggggg:::::g     |     |        |
	//              |         |  g::::::::::::::::g     |     |      height
	//              |         |   gg::::::::::::::g     |     |        |
	//  baseline ---*---------|---- gggggggg::::::g-----*--------      |
	//            / |         |             g:::::g     |              |
	//     origin   |         | gggggg      g:::::g     |              |
	//              |         | g:::::gg   gg:::::g     |              |
	//              |         |  g::::::ggg:::::::g     |              |
	//              |         |   gg:::::::::::::g      |              |
	//              |         |     ggg::::::ggg        |              |
	//              |         |         gggggg          |              v
	//              |         +-------------------------+----------------- ymin
	//              |                                   |
	//              |------------- advanceX ----------->|

	// A structure that describe a glyph.
	struct GlyphInfo {
		int Width;      // Glyph's width in pixels.
		int Height;     // Glyph's height in pixels.
		FT_Int OffsetX; // The distance from the origin ("pen position") to the left of the glyph.
		FT_Int OffsetY; // The distance from the origin to the top of the glyph. This is usually a value < 0.
		float AdvanceX; // The distance from the origin to the origin of the next glyph. This is usually a value > 0.
		bool IsColored; // The glyph is colored
	};

	// Font parameters and metrics.
	struct FontInfo {
		uint32_t PixelHeight; // Size this font was generated with.
		float Ascender;       // The pixel extents above the baseline in pixels (typically positive).
		float Descender;      // The extents below the baseline in pixels (typically negative).
		float LineSpacing;    // The baseline-to-baseline distance. Note that it usually is larger than the sum of the ascender and descender taken as
		                      // absolute values. There is also no guarantee that no glyphs extend above or below subsequent baselines when using this
		                      // distance. Think of it as a value the designer of the font finds appropriate.
		float LineGap;        // The spacing in pixels between one row's descent and the next row's ascent.
		float MaxAdvanceWidth; // This field gives the maximum horizontal cursor advance for all glyphs in the font.
	};

	// FreeType glyph rasterizer.
	// NB: No ctor/dtor, explicitly call Init()/Shutdown()
	struct FreeTypeFont {
		bool InitFont( FT_Library ft_library, const ImFontConfig& cfg,
		               unsigned int extra_user_flags ); // Initialize from an external data buffer. Doesn't copy data, and you must ensure it stays
		                                                // valid up to this object lifetime.
		void CloseFont( );
		void SetPixelHeight( int pixel_height ); // Change font pixel size. All following calls to RasterizeGlyph() will use this size
		const FT_Glyph_Metrics* LoadGlyph( uint32_t in_codepoint );
		const FT_Bitmap* RenderGlyphAndGetInfo( GlyphInfo* out_glyph_info );
		void BlitGlyph( const FT_Bitmap* ft_bitmap, void* dst, uint32_t dst_pitch, unsigned char* multiply_table = NULL, bool a8 = false );
		~FreeTypeFont( )
		{
			CloseFont( );
		}

		// [Internals]
		FontInfo Info; // Font descriptor of the current font.
		FT_Face Face;
		unsigned int UserFlags; // = ImFontConfig::RasterizerFlags
		FT_Int32 LoadFlags;
		FT_Render_Mode RenderMode;
		/* botox Stroke: zeroed with the build's src array, freed in CloseFont. Stroked = last LoadGlyph's outline, rendered in place */
		FT_Stroker Stroker;
		FT_Glyph Stroked;
	};

// From SDL_ttf: Handy routines for converting from fixed point
#define FT_CEIL( X ) ( ( ( X + 63 ) & -64 ) / 64 )

	bool FreeTypeFont::InitFont( FT_Library ft_library, const ImFontConfig& cfg, unsigned int extra_font_builder_flags )
	{
		FT_Error error;
		if ( cfg.FontBuilderFlags & ImGuiFreeTypeBuilderFlags_File )
			error = ImGuiFreeType::OpenFileFace( ft_library, ( const char* )cfg.FontData, ( long )cfg.FontNo, ( void** )&Face );
		else
			error = FT_New_Memory_Face( ft_library, ( uint8_t* )cfg.FontData, ( uint32_t )cfg.FontDataSize, ( uint32_t )cfg.FontNo, &Face );
		g_ft_face_error = error;
		g_ft_face_step  = "new_face";
		if ( error != 0 )
			return false;
		error = FT_Select_Charmap( Face, FT_ENCODING_UNICODE );
		g_ft_face_error = error;
		g_ft_face_step  = "charmap";
		if ( error != 0 )
			return false;

		// Convert to FreeType flags (NB: Bold and Oblique are processed separately)
		UserFlags = cfg.FontBuilderFlags | extra_font_builder_flags;

		LoadFlags = 0;
		if ( ( UserFlags & ImGuiFreeTypeBuilderFlags_Bitmap ) == 0 )
			LoadFlags |= FT_LOAD_NO_BITMAP;

		if ( UserFlags & ImGuiFreeTypeBuilderFlags_NoHinting )
			LoadFlags |= FT_LOAD_NO_HINTING;
		if ( UserFlags & ImGuiFreeTypeBuilderFlags_NoAutoHint )
			LoadFlags |= FT_LOAD_NO_AUTOHINT;
		if ( UserFlags & ImGuiFreeTypeBuilderFlags_ForceAutoHint )
			LoadFlags |= FT_LOAD_FORCE_AUTOHINT;
		if ( UserFlags & ImGuiFreeTypeBuilderFlags_LightHinting )
			LoadFlags |= FT_LOAD_TARGET_LIGHT;
		else if ( UserFlags & ImGuiFreeTypeBuilderFlags_MonoHinting )
			LoadFlags |= FT_LOAD_TARGET_MONO;
		else
			LoadFlags |= FT_LOAD_TARGET_NORMAL;

		if ( UserFlags & ImGuiFreeTypeBuilderFlags_Monochrome )
			RenderMode = FT_RENDER_MODE_MONO;
		else
			RenderMode = FT_RENDER_MODE_NORMAL;

		if ( UserFlags & ImGuiFreeTypeBuilderFlags_LoadColor )
			LoadFlags |= FT_LOAD_COLOR;

		/* 1px (64 in 26.6), round joins: same pixel size as the old 1px offset copies */
		if ( ( UserFlags & ImGuiFreeTypeBuilderFlags_Stroke ) && FT_Stroker_New( ft_library, &Stroker ) == 0 )
			FT_Stroker_Set( Stroker, 64, FT_STROKER_LINECAP_ROUND, FT_STROKER_LINEJOIN_ROUND, 0 );

		memset( &Info, 0, sizeof( Info ) );
		SetPixelHeight( ( uint32_t )cfg.SizePixels );

		return true;
	}

	void FreeTypeFont::CloseFont( )
	{
		if ( Stroked ) {
			FT_Done_Glyph( Stroked );
			Stroked = NULL;
		}
		if ( Stroker ) {
			FT_Stroker_Done( Stroker );
			Stroker = NULL;
		}
		if ( Face ) {
			FT_Done_Face( Face );
			Face = NULL;
		}
	}

	void FreeTypeFont::SetPixelHeight( int pixel_height )
	{
		// Vuhdo: I'm not sure how to deal with font sizes properly. As far as I understand, currently ImGui assumes that the 'pixel_height'
		// is a maximum height of an any given glyph, i.e. it's the sum of font's ascender and descender. Seems strange to me.
		// NB: FT_Set_Pixel_Sizes() doesn't seem to get us the same result.
		FT_Size_RequestRec req;
		req.type           = ( UserFlags & ImGuiFreeTypeBuilderFlags_Bitmap ) ? FT_SIZE_REQUEST_TYPE_NOMINAL : FT_SIZE_REQUEST_TYPE_REAL_DIM;
		req.width          = 0;
		req.height         = ( uint32_t )pixel_height * 64;
		req.horiResolution = 0;
		req.vertResolution = 0;
		FT_Request_Size( Face, &req );

		// Update font info
		FT_Size_Metrics metrics = Face->size->metrics;
		Info.PixelHeight        = ( uint32_t )pixel_height;
		Info.Ascender           = ( float )FT_CEIL( metrics.ascender );
		Info.Descender          = ( float )FT_CEIL( metrics.descender );
		Info.LineSpacing        = ( float )FT_CEIL( metrics.height );
		Info.LineGap            = ( float )FT_CEIL( metrics.height - metrics.ascender + metrics.descender );
		Info.MaxAdvanceWidth    = ( float )FT_CEIL( metrics.max_advance );
	}

	const FT_Glyph_Metrics* FreeTypeFont::LoadGlyph( uint32_t codepoint )
	{
		if ( Stroked ) {
			FT_Done_Glyph( Stroked );
			Stroked = NULL;
		}

		uint32_t glyph_index = FT_Get_Char_Index( Face, codepoint );
		if ( glyph_index == 0 )
			return NULL;

		// If this crash for you: FreeType 2.11.0 has a crash bug on some bitmap/colored fonts.
		// - https://gitlab.freedesktop.org/freetype/freetype/-/issues/1076
		// - https://github.com/ocornut/imgui/issues/4567
		// - https://github.com/ocornut/imgui/issues/4566
		// You can use FreeType 2.10, or the patched version of 2.11.0 in VcPkg, or probably any upcoming FreeType version.
		FT_Error error = FT_Load_Glyph( Face, glyph_index, LoadFlags );
		if ( error )
			return NULL;

		// Need an outline for this to work
		FT_GlyphSlot slot = Face->glyph;
		IM_ASSERT( slot->format == FT_GLYPH_FORMAT_OUTLINE || slot->format == FT_GLYPH_FORMAT_BITMAP );

		// Apply convenience transform (this is not picking from real "Bold"/"Italic" fonts! Merely applying FreeType helper transform. Oblique ==
		// Slanting)
		if ( UserFlags & ImGuiFreeTypeBuilderFlags_Bold )
			FT_GlyphSlot_Embolden( slot );
		if ( UserFlags & ImGuiFreeTypeBuilderFlags_Oblique ) {
			FT_GlyphSlot_Oblique( slot );
			// FT_BBox bbox;
			// FT_Outline_Get_BBox(&slot->outline, &bbox);
			// slot->metrics.width = bbox.xMax - bbox.xMin;
			// slot->metrics.height = bbox.yMax - bbox.yMin;
		}

		/* outer border only = the glyph grown by the stroke radius; bitmap glyphs stay plain */
		if ( Stroker && slot->format == FT_GLYPH_FORMAT_OUTLINE && FT_Get_Glyph( slot, &Stroked ) == 0 &&
		     FT_Glyph_StrokeBorder( &Stroked, Stroker, 0, 1 ) != 0 ) {
			FT_Done_Glyph( Stroked );
			Stroked = NULL;
		}

		return &slot->metrics;
	}

	const FT_Bitmap* FreeTypeFont::RenderGlyphAndGetInfo( GlyphInfo* out_glyph_info )
	{
		FT_GlyphSlot slot = Face->glyph;

		if ( Stroked ) {
			if ( FT_Glyph_To_Bitmap( &Stroked, RenderMode, NULL, 1 ) != 0 )
				return NULL;

			const FT_BitmapGlyph bitmap_glyph = ( FT_BitmapGlyph )Stroked;
			out_glyph_info->Width             = ( int )bitmap_glyph->bitmap.width;
			out_glyph_info->Height            = ( int )bitmap_glyph->bitmap.rows;
			out_glyph_info->OffsetX           = bitmap_glyph->left;
			out_glyph_info->OffsetY           = -bitmap_glyph->top;
			out_glyph_info->AdvanceX          = ( float )FT_CEIL( slot->advance.x );
			out_glyph_info->IsColored         = false;
			return &bitmap_glyph->bitmap;
		}

		FT_Error error    = FT_Render_Glyph( slot, RenderMode );
		if ( error != 0 )
			return NULL;

		FT_Bitmap* ft_bitmap      = &Face->glyph->bitmap;
		out_glyph_info->Width     = ( int )ft_bitmap->width;
		out_glyph_info->Height    = ( int )ft_bitmap->rows;
		out_glyph_info->OffsetX   = Face->glyph->bitmap_left;
		out_glyph_info->OffsetY   = -Face->glyph->bitmap_top;
		out_glyph_info->AdvanceX  = ( float )FT_CEIL( slot->advance.x );
		out_glyph_info->IsColored = ( ft_bitmap->pixel_mode == FT_PIXEL_MODE_BGRA );

		return ft_bitmap;
	}

	/* botox: a8 = alpha byte only ( alpha atlases ), 1 byte a pixel instead of 4 */
	void FreeTypeFont::BlitGlyph( const FT_Bitmap* ft_bitmap, void* dst_void, uint32_t dst_pitch, unsigned char* multiply_table, bool a8 )
	{
		IM_ASSERT( ft_bitmap != NULL );
		const uint32_t w         = ft_bitmap->width;
		const uint32_t h         = ft_bitmap->rows;
		const uint8_t* src       = ft_bitmap->buffer;
		const uint32_t src_pitch = ft_bitmap->pitch;

		uint32_t* dst = ( uint32_t* )dst_void;
		uint8_t* dst8 = ( uint8_t* )dst_void;
		const auto put = [ & ]( const uint32_t x, const ImU32 c ) {
			if ( a8 )
				dst8[ x ] = ( uint8_t )( ( c >> IM_COL32_A_SHIFT ) & 0xFF );
			else
				dst[ x ] = c;
		};
		const auto next_row = [ & ]( ) {
			src += src_pitch;
			dst += dst_pitch;
			dst8 += dst_pitch;
		};

		switch ( ft_bitmap->pixel_mode ) {
		case FT_PIXEL_MODE_GRAY: // Grayscale image, 1 byte per pixel.
		{
			if ( multiply_table == NULL ) {
				for ( uint32_t y = 0; y < h; y++, next_row( ) )
					for ( uint32_t x = 0; x < w; x++ )
						put( x, IM_COL32( 255, 255, 255, src[ x ] ) );
			} else {
				for ( uint32_t y = 0; y < h; y++, next_row( ) )
					for ( uint32_t x = 0; x < w; x++ )
						put( x, IM_COL32( 255, 255, 255, multiply_table[ src[ x ] ] ) );
			}
			break;
		}
		case FT_PIXEL_MODE_MONO: // Monochrome image, 1 bit per pixel. The bits in each byte are ordered from MSB to LSB.
		{
			uint8_t color0 = multiply_table ? multiply_table[ 0 ] : 0;
			uint8_t color1 = multiply_table ? multiply_table[ 255 ] : 255;
			for ( uint32_t y = 0; y < h; y++, next_row( ) ) {
				uint8_t bits            = 0;
				const uint8_t* bits_ptr = src;
				for ( uint32_t x = 0; x < w; x++, bits <<= 1 ) {
					if ( ( x & 7 ) == 0 )
						bits = *bits_ptr++;
					put( x, IM_COL32( 255, 255, 255, ( bits & 0x80 ) ? color1 : color0 ) );
				}
			}
			break;
		}
		case FT_PIXEL_MODE_BGRA: {
// FIXME: Converting pre-multiplied alpha to straight. Doesn't smell good.
#define DE_MULTIPLY( color, alpha ) ( ImU32 )( 255.0f * ( float )color / ( float )alpha + 0.5f )
			if ( multiply_table == NULL ) {
				for ( uint32_t y = 0; y < h; y++, next_row( ) )
					for ( uint32_t x = 0; x < w; x++ ) {
						uint8_t r = src[ x * 4 + 2 ], g = src[ x * 4 + 1 ], b = src[ x * 4 ], a = src[ x * 4 + 3 ];
						put( x, IM_COL32( DE_MULTIPLY( r, a ), DE_MULTIPLY( g, a ), DE_MULTIPLY( b, a ), a ) );
					}
			} else {
				for ( uint32_t y = 0; y < h; y++, next_row( ) ) {
					for ( uint32_t x = 0; x < w; x++ ) {
						uint8_t r = src[ x * 4 + 2 ], g = src[ x * 4 + 1 ], b = src[ x * 4 ], a = src[ x * 4 + 3 ];
						put( x, IM_COL32( multiply_table[ DE_MULTIPLY( r, a ) ], multiply_table[ DE_MULTIPLY( g, a ) ],
						                  multiply_table[ DE_MULTIPLY( b, a ) ], multiply_table[ a ] ) );
					}
				}
			}
#undef DE_MULTIPLY
			break;
		}
		default:
			IM_ASSERT( 0 && "FreeTypeFont::BlitGlyph(): Unknown bitmap pixel mode!" );
		}
	}
} // namespace

#ifndef STB_RECT_PACK_IMPLEMENTATION // in case the user already have an implementation in the _same_ compilation unit (e.g. unity builds)
#	ifndef IMGUI_DISABLE_STB_RECT_PACK_IMPLEMENTATION
#		define STBRP_ASSERT( x )                                                                                                                    \
			do {                                                                                                                                     \
				IM_ASSERT( x );                                                                                                                      \
			} while ( 0 )
#		define STBRP_STATIC
#		define STB_RECT_PACK_IMPLEMENTATION
#	endif
#	ifdef IMGUI_STB_RECT_PACK_FILENAME
#		include IMGUI_STB_RECT_PACK_FILENAME
#	else
#		include "imstb_rectpack.h"
#	endif
#endif

struct ImFontBuildSrcGlyphFT {
	GlyphInfo Info;
	uint32_t Codepoint;
	bool Rendered;             // botox: has a pack rect ( rects are made after every face closed )
	unsigned char* BitmapData; // Point within one of the dst_tmp_bitmap_buffers[] array ( botox: a8 unless the atlas loads colour )

	ImFontBuildSrcGlyphFT( )
	{
		memset( ( void* )this, 0, sizeof( *this ) );
	}
};

struct ImFontBuildSrcDataFT {
	FreeTypeFont Font;
	stbrp_rect* Rects;        // Rectangle to pack. We first fill in their size and the packer will give us their position.
	const ImWchar* SrcRanges; // Ranges as requested by user (user is allowed to request too much, e.g. 0x0020..0xFFFF)
	int DstIndex;             // Index into atlas->Fonts[] and dst_tmp_array[]
	int GlyphsHighest;        // Highest requested codepoint
	int GlyphsCount;          // Glyph count (excluding missing glyphs and glyphs already set by an earlier source font)
	ImBitVector GlyphsSet;    // Glyph bit map (random access, 1-bit per codepoint. This will be a maximum of 8KB)
	ImVector< ImFontBuildSrcGlyphFT > GlyphsList;
};

// Temporary data for one destination ImFont* (multiple source fonts can be merged into one destination ImFont)
struct ImFontBuildDstDataFT {
	int SrcCount; // Number of source fonts targeting this destination font.
	int GlyphsHighest;
	int GlyphsCount;
	ImBitVector GlyphsSet; // This is used to resolve collision when multiple sources are merged into a same destination font.
};

bool ImFontAtlasBuildWithFreeTypeEx( FT_Library ft_library, ImFontAtlas* atlas, unsigned int extra_flags )
{
	IM_ASSERT( atlas->ConfigData.Size > 0 );

	ImFontAtlasBuildInit( atlas );

	// Clear atlas
	atlas->TexID    = ( ImTextureID )NULL;
	atlas->TexWidth = atlas->TexHeight = 0;
	atlas->TexUvScale                  = ImVec2( 0.0f, 0.0f );
	atlas->TexUvWhitePixel             = ImVec2( 0.0f, 0.0f );
	atlas->ClearTexData( );

	// Temporary storage for building
	bool src_load_color = false;
	ImVector< ImFontBuildSrcDataFT > src_tmp_array;
	ImVector< ImFontBuildDstDataFT > dst_tmp_array;
	src_tmp_array.resize( atlas->ConfigData.Size );
	dst_tmp_array.resize( atlas->Fonts.Size );
	memset( ( void* )src_tmp_array.Data, 0, ( size_t )src_tmp_array.size_in_bytes( ) );
	memset( ( void* )dst_tmp_array.Data, 0, ( size_t )dst_tmp_array.size_in_bytes( ) );

	// 1. Initialize font loading structure ( botox: faces open one at a time in 2-4 )
	for ( int src_i = 0; src_i < atlas->ConfigData.Size; src_i++ ) {
		ImFontBuildSrcDataFT& src_tmp = src_tmp_array[ src_i ];
		ImFontConfig& cfg             = atlas->ConfigData[ src_i ];
		IM_ASSERT( cfg.DstFont && ( !cfg.DstFont->IsLoaded( ) || cfg.DstFont->ContainerAtlas == atlas ) );

		// Find index from cfg.DstFont (we allow the user to set cfg.DstFont. Also it makes casual debugging nicer than when storing indices)
		src_tmp.DstIndex = -1;
		for ( int output_i = 0; output_i < atlas->Fonts.Size && src_tmp.DstIndex == -1; output_i++ )
			if ( cfg.DstFont == atlas->Fonts[ output_i ] )
				src_tmp.DstIndex = output_i;
		IM_ASSERT( src_tmp.DstIndex != -1 ); // cfg.DstFont not pointing within atlas->Fonts[] array?
		if ( src_tmp.DstIndex == -1 ) {
			ImFormatString( g_ft_build_fail, sizeof( g_ft_build_fail ), "src %d/%d: DstFont not in Fonts[]", src_i, atlas->ConfigData.Size );
			return false;
		}

		// Measure highest codepoints
		src_load_color |= ( cfg.FontBuilderFlags & ImGuiFreeTypeBuilderFlags_LoadColor ) != 0;
		ImFontBuildDstDataFT& dst_tmp = dst_tmp_array[ src_tmp.DstIndex ];
		src_tmp.SrcRanges             = cfg.GlyphRanges ? cfg.GlyphRanges : atlas->GetGlyphRangesDefault( );
		for ( const ImWchar* src_range = src_tmp.SrcRanges; src_range[ 0 ] && src_range[ 1 ]; src_range += 2 )
			src_tmp.GlyphsHighest = ImMax( src_tmp.GlyphsHighest, ( int )src_range[ 1 ] );
		dst_tmp.SrcCount++;
		dst_tmp.GlyphsHighest = ImMax( dst_tmp.GlyphsHighest, src_tmp.GlyphsHighest );
	}

	// 2. - 4. botox: per source, in order: open its face, find its codepoints ( an earlier source keeps an overlap ), render them, close it.
	// one face ( one font file ) open at a time
	const int BITMAP_BUFFERS_CHUNK_SIZE = 256 * 1024;
	int buf_bitmap_current_used_bytes   = 0;
	ImVector< unsigned char* > buf_bitmap_buffers;
	buf_bitmap_buffers.push_back( ( unsigned char* )IM_ALLOC( BITMAP_BUFFERS_CHUNK_SIZE ) );

	int total_glyphs_count = 0;
	int total_surface      = 0;
	const int padding      = atlas->TexGlyphPadding;
	for ( int src_i = 0; src_i < src_tmp_array.Size; src_i++ ) {
		ImFontBuildSrcDataFT& src_tmp = src_tmp_array[ src_i ];
		ImFontBuildDstDataFT& dst_tmp = dst_tmp_array[ src_tmp.DstIndex ];
		ImFontConfig& cfg             = atlas->ConfigData[ src_i ];

		if ( !src_tmp.Font.InitFont( ft_library, cfg, extra_flags ) ) {
			ImFormatString( g_ft_build_fail, sizeof( g_ft_build_fail ), "src %d/%d: %s error 0x%x, data %p size %d no %d px %.0f merge %d",
			                src_i, atlas->ConfigData.Size, g_ft_face_step, ( unsigned )g_ft_face_error, cfg.FontData, cfg.FontDataSize,
			                cfg.FontNo, cfg.SizePixels, cfg.MergeMode ? 1 : 0 );
			for ( int buf_i = 0; buf_i < buf_bitmap_buffers.Size; buf_i++ )
				IM_FREE( buf_bitmap_buffers[ buf_i ] );
			for ( int dst_i = 0; dst_i < dst_tmp_array.Size; dst_i++ )
				dst_tmp_array[ dst_i ].GlyphsSet.Clear( );
			src_tmp_array.clear_destruct( );
			return false;
		}

		// 2. For every requested codepoint, check for their presence in the font data, and handle redundancy or overlaps between source fonts
		src_tmp.GlyphsSet.Create( src_tmp.GlyphsHighest + 1 );
		if ( dst_tmp.GlyphsSet.Storage.empty( ) )
			dst_tmp.GlyphsSet.Create( dst_tmp.GlyphsHighest + 1 );

		for ( const ImWchar* src_range = src_tmp.SrcRanges; src_range[ 0 ] && src_range[ 1 ]; src_range += 2 )
			for ( int codepoint = src_range[ 0 ]; codepoint <= ( int )src_range[ 1 ]; codepoint++ ) {
				if ( dst_tmp.GlyphsSet.TestBit( codepoint ) ) // Don't overwrite existing glyphs. We could make this an option (e.g. MergeOverwrite)
					continue;
				uint32_t glyph_index = FT_Get_Char_Index(
					src_tmp.Font.Face, codepoint ); // It is actually in the font? (FIXME-OPT: We are not storing the glyph_index..)
				if ( glyph_index == 0 )
					continue;

				// Add to avail set/counters
				src_tmp.GlyphsCount++;
				dst_tmp.GlyphsCount++;
				src_tmp.GlyphsSet.SetBit( codepoint );
				dst_tmp.GlyphsSet.SetBit( codepoint );
				total_glyphs_count++;
			}

		// 3. Unpack our bit map into a flat list
		src_tmp.GlyphsList.reserve( src_tmp.GlyphsCount );

		IM_ASSERT( sizeof( src_tmp.GlyphsSet.Storage.Data[ 0 ] ) == sizeof( ImU32 ) );
		const ImU32* it_begin = src_tmp.GlyphsSet.Storage.begin( );
		const ImU32* it_end   = src_tmp.GlyphsSet.Storage.end( );
		for ( const ImU32* it = it_begin; it < it_end; it++ )
			if ( ImU32 entries_32 = *it )
				for ( ImU32 bit_n = 0; bit_n < 32; bit_n++ )
					if ( entries_32 & ( ( ImU32 )1 << bit_n ) ) {
						ImFontBuildSrcGlyphFT src_glyph;
						src_glyph.Codepoint = ( ImWchar )( ( ( it - it_begin ) << 5 ) + bit_n );
						src_tmp.GlyphsList.push_back( src_glyph );
					}
		src_tmp.GlyphsSet.Clear( );
		IM_ASSERT( src_tmp.GlyphsList.Size == src_tmp.GlyphsCount );

		// 4. Render glyphs into 256 KB temporary chunks, keep their sizes for packing
		const bool multiply_enabled = ( cfg.RasterizerMultiply != 1.0f );
		unsigned char multiply_table[ 256 ];
		if ( multiply_enabled )
			ImFontAtlasBuildMultiplyCalcLookupTable( multiply_table, cfg.RasterizerMultiply );

		for ( int glyph_i = 0; glyph_i < src_tmp.GlyphsList.Size; glyph_i++ ) {
			ImFontBuildSrcGlyphFT& src_glyph = src_tmp.GlyphsList[ glyph_i ];

			const FT_Glyph_Metrics* metrics = src_tmp.Font.LoadGlyph( src_glyph.Codepoint );
			if ( metrics == NULL )
				continue;

			// Render glyph into a bitmap (currently held by FreeType)
			const FT_Bitmap* ft_bitmap = src_tmp.Font.RenderGlyphAndGetInfo( &src_glyph.Info );
			if ( ft_bitmap == NULL )
				continue;

			// Allocate new temporary chunk if needed
			const int bitmap_size_in_bytes = src_glyph.Info.Width * src_glyph.Info.Height * ( src_load_color ? 4 : 1 );
			if ( buf_bitmap_current_used_bytes + bitmap_size_in_bytes > BITMAP_BUFFERS_CHUNK_SIZE ) {
				buf_bitmap_current_used_bytes = 0;
				buf_bitmap_buffers.push_back( ( unsigned char* )IM_ALLOC( BITMAP_BUFFERS_CHUNK_SIZE ) );
			}
			IM_ASSERT( buf_bitmap_current_used_bytes + bitmap_size_in_bytes <=
			           BITMAP_BUFFERS_CHUNK_SIZE ); // We could probably allocate custom-sized buffer instead.

			// Blit rasterized pixels to our temporary buffer and keep a pointer to it.
			src_glyph.BitmapData = buf_bitmap_buffers.back( ) + buf_bitmap_current_used_bytes;
			buf_bitmap_current_used_bytes += bitmap_size_in_bytes;
			src_tmp.Font.BlitGlyph( ft_bitmap, src_glyph.BitmapData, src_glyph.Info.Width, multiply_enabled ? multiply_table : NULL, !src_load_color );

			src_glyph.Rendered = true;
			total_surface += ( stbrp_coord )( src_glyph.Info.Width + padding ) * ( stbrp_coord )( src_glyph.Info.Height + padding );
		}

		src_tmp.Font.CloseFont( );
	}
	for ( int dst_i = 0; dst_i < dst_tmp_array.Size; dst_i++ )
		dst_tmp_array[ dst_i ].GlyphsSet.Clear( );
	dst_tmp_array.clear( );

	// Allocate packing character data and flag packed characters buffer as non-packed (x0=y0=x1=y1=0)
	ImVector< stbrp_rect > buf_rects;
	buf_rects.resize( total_glyphs_count );
	memset( buf_rects.Data, 0, ( size_t )buf_rects.size_in_bytes( ) );
	int buf_rects_out_n = 0;
	for ( int src_i = 0; src_i < src_tmp_array.Size; src_i++ ) {
		ImFontBuildSrcDataFT& src_tmp = src_tmp_array[ src_i ];
		if ( src_tmp.GlyphsCount == 0 )
			continue;

		src_tmp.Rects = &buf_rects[ buf_rects_out_n ];
		buf_rects_out_n += src_tmp.GlyphsCount;
		for ( int glyph_i = 0; glyph_i < src_tmp.GlyphsList.Size; glyph_i++ )
			if ( const ImFontBuildSrcGlyphFT& src_glyph = src_tmp.GlyphsList[ glyph_i ]; src_glyph.Rendered ) {
				src_tmp.Rects[ glyph_i ].w = ( stbrp_coord )( src_glyph.Info.Width + padding );
				src_tmp.Rects[ glyph_i ].h = ( stbrp_coord )( src_glyph.Info.Height + padding );
			}
	}

	// We need a width for the skyline algorithm, any width!
	// The exact width doesn't really matter much, but some API/GPU have texture size limitations and increasing width can decrease height.
	// User can override TexDesiredWidth and TexGlyphPadding if they wish, otherwise we use a simple heuristic to select the width based on expected
	// surface.
	const int surface_sqrt = ( int )ImSqrt( ( float )total_surface ) + 1;
	atlas->TexHeight       = 0;
	if ( atlas->TexDesiredWidth > 0 )
		atlas->TexWidth = atlas->TexDesiredWidth;
	else
		atlas->TexWidth = ( surface_sqrt >= 4096 * 0.7f )   ? 4096
		                  : ( surface_sqrt >= 2048 * 0.7f ) ? 2048
		                  : ( surface_sqrt >= 1024 * 0.7f ) ? 1024
		                                                    : 512;

	// 5. Start packing
	// Pack our extra data rectangles first, so it will be on the upper-left corner of our texture (UV will have small values).
	const int TEX_HEIGHT_MAX                  = 1024 * 32;
	const int num_nodes_for_packing_algorithm = atlas->TexWidth - atlas->TexGlyphPadding;
	ImVector< stbrp_node > pack_nodes;
	pack_nodes.resize( num_nodes_for_packing_algorithm );
	stbrp_context pack_context;
	stbrp_init_target( &pack_context, atlas->TexWidth, TEX_HEIGHT_MAX, pack_nodes.Data, pack_nodes.Size );
	ImFontAtlasBuildPackCustomRects( atlas, &pack_context );

	// 6. Pack each source font. No rendering yet, we are working with rectangles in an infinitely tall texture at this point.
	for ( int src_i = 0; src_i < src_tmp_array.Size; src_i++ ) {
		ImFontBuildSrcDataFT& src_tmp = src_tmp_array[ src_i ];
		if ( src_tmp.GlyphsCount == 0 )
			continue;

		stbrp_pack_rects( &pack_context, src_tmp.Rects, src_tmp.GlyphsCount );

		// Extend texture height and mark missing glyphs as non-packed so we won't render them.
		// FIXME: We are not handling packing failure here (would happen if we got off TEX_HEIGHT_MAX or if a single if larger than TexWidth?)
		for ( int glyph_i = 0; glyph_i < src_tmp.GlyphsCount; glyph_i++ )
			if ( src_tmp.Rects[ glyph_i ].was_packed )
				atlas->TexHeight = ImMax( atlas->TexHeight, src_tmp.Rects[ glyph_i ].y + src_tmp.Rects[ glyph_i ].h );
	}

	// 7. Allocate texture
	atlas->TexHeight  = ( atlas->Flags & ImFontAtlasFlags_NoPowerOfTwoHeight ) ? ( atlas->TexHeight + 1 ) : ImUpperPowerOfTwo( atlas->TexHeight );
	atlas->TexUvScale = ImVec2( 1.0f / atlas->TexWidth, 1.0f / atlas->TexHeight );
	if ( src_load_color ) {
		size_t tex_size        = ( size_t )atlas->TexWidth * atlas->TexHeight * 4;
		atlas->TexPixelsRGBA32 = ( unsigned int* )IM_ALLOC( tex_size );
		if ( atlas->TexPixelsRGBA32 )
			memset( atlas->TexPixelsRGBA32, 0, tex_size );
	} else
		atlas->TexPixelsAlpha8 = ImFontAtlasAllocAlpha8( atlas, ( size_t )atlas->TexWidth * atlas->TexHeight );

	/* botox: no room for the one big block with the glyph chunks still held: free them, try again and render the
	   glyphs a second time straight into the texture ( alpha only ). peak = the texture alone */
	bool rerender = false;
	if ( !src_load_color && !atlas->TexPixelsAlpha8 ) {
		for ( int buf_i = 0; buf_i < buf_bitmap_buffers.Size; buf_i++ )
			IM_FREE( buf_bitmap_buffers[ buf_i ] );
		buf_bitmap_buffers.clear( );

		atlas->TexPixelsAlpha8 = ImFontAtlasAllocAlpha8( atlas, ( size_t )atlas->TexWidth * atlas->TexHeight );
		rerender               = atlas->TexPixelsAlpha8 != NULL;
	}

	/* botox: one big block in a 32 bit process - fail the build (caller keeps its old fonts) instead
	   of memset through null. same cleanup as the success path below. */
	if ( !atlas->TexPixelsRGBA32 && !atlas->TexPixelsAlpha8 ) {
		ImFormatString( g_ft_build_fail, sizeof( g_ft_build_fail ), "pixel alloc failed %dx%d", atlas->TexWidth, atlas->TexHeight );
		for ( int buf_i = 0; buf_i < buf_bitmap_buffers.Size; buf_i++ )
			IM_FREE( buf_bitmap_buffers[ buf_i ] );
		src_tmp_array.clear_destruct( );
		return false;
	}

	// 8. Copy rasterized font characters back into the main texture
	// 9. Setup ImFont and glyphs for runtime
	bool tex_use_colors = false;
	for ( int src_i = 0; src_i < src_tmp_array.Size; src_i++ ) {
		ImFontBuildSrcDataFT& src_tmp = src_tmp_array[ src_i ];
		if ( src_tmp.GlyphsCount == 0 )
			continue;

		// When merging fonts with MergeMode=true:
		// - We can have multiple input fonts writing into a same destination font.
		// - dst_font->ConfigData is != from cfg which is our source configuration.
		ImFontConfig& cfg = atlas->ConfigData[ src_i ];
		ImFont* dst_font  = cfg.DstFont;

		const float ascent  = src_tmp.Font.Info.Ascender;
		const float descent = src_tmp.Font.Info.Descender;
		ImFontAtlasBuildSetupFont( atlas, dst_font, &cfg, ascent, descent );
		const float font_off_x = cfg.GlyphOffset.x;
		const float font_off_y = cfg.GlyphOffset.y + IM_ROUND( dst_font->Ascent );

		unsigned char multiply_table[ 256 ];
		const bool multiply_enabled = rerender && cfg.RasterizerMultiply != 1.0f;
		if ( multiply_enabled )
			ImFontAtlasBuildMultiplyCalcLookupTable( multiply_table, cfg.RasterizerMultiply );

		/* faces were closed after step 4: a re-render opens this one again. gone meanwhile = fail, a glyphless font crashes BuildLookupTable */
		const bool face_open = rerender && src_tmp.Font.InitFont( ft_library, cfg, extra_flags );
		if ( rerender && !face_open ) {
			ImFormatString( g_ft_build_fail, sizeof( g_ft_build_fail ), "src %d/%d: re-render %s error 0x%x", src_i, atlas->ConfigData.Size,
			                g_ft_face_step, ( unsigned )g_ft_face_error );
			src_tmp_array.clear_destruct( );
			return false;
		}
		for ( int glyph_i = 0; glyph_i < src_tmp.GlyphsCount; glyph_i++ ) {
			ImFontBuildSrcGlyphFT& src_glyph = src_tmp.GlyphsList[ glyph_i ];
			stbrp_rect& pack_rect            = src_tmp.Rects[ glyph_i ];
			IM_ASSERT( pack_rect.was_packed );
			if ( pack_rect.w == 0 && pack_rect.h == 0 )
				continue;

			/* same glyph, same size as pass one ( freetype is deterministic ): no metrics = it had no rect either */
			const FT_Bitmap* rerender_bitmap = NULL;
			if ( rerender && ( !src_tmp.Font.LoadGlyph( src_glyph.Codepoint ) || !( rerender_bitmap = src_tmp.Font.RenderGlyphAndGetInfo( &src_glyph.Info ) ) ) )
				continue;

			GlyphInfo& info = src_glyph.Info;
			/* re-render reads the file again: one changed on disk meanwhile must not blit past its rect ( asserts are off in release ) */
			if ( info.Width + padding > pack_rect.w || info.Height + padding > pack_rect.h )
				continue;
			const int tx = pack_rect.x + padding;
			const int ty = pack_rect.y + padding;

			// Register glyph
			float x0 = info.OffsetX + font_off_x;
			float y0 = info.OffsetY + font_off_y;
			float x1 = x0 + info.Width;
			float y1 = y0 + info.Height;
			float u0 = ( tx ) / ( float )atlas->TexWidth;
			float v0 = ( ty ) / ( float )atlas->TexHeight;
			float u1 = ( tx + info.Width ) / ( float )atlas->TexWidth;
			float v1 = ( ty + info.Height ) / ( float )atlas->TexHeight;
			dst_font->AddGlyph( &cfg, ( ImWchar )src_glyph.Codepoint, x0, y0, x1, y1, u0, v0, u1, v1, info.AdvanceX );

			ImFontGlyph* dst_glyph = &dst_font->Glyphs.back( );
			IM_ASSERT( dst_glyph->Codepoint == src_glyph.Codepoint );
			if ( src_glyph.Info.IsColored )
				dst_glyph->Colored = tex_use_colors = true;

			// Blit from temporary buffer to final texture
			size_t blit_src_stride = ( size_t )src_glyph.Info.Width;
			size_t blit_dst_stride = ( size_t )atlas->TexWidth;
			if ( rerender ) {
				src_tmp.Font.BlitGlyph( rerender_bitmap, atlas->TexPixelsAlpha8 + ( ty * blit_dst_stride ) + tx, ( uint32_t )blit_dst_stride,
				                        multiply_enabled ? multiply_table : NULL, true );
			} else if ( atlas->TexPixelsAlpha8 != NULL ) {
				const unsigned char* blit_src = src_glyph.BitmapData;
				unsigned char* blit_dst       = atlas->TexPixelsAlpha8 + ( ty * blit_dst_stride ) + tx;
				for ( int y = 0; y < info.Height; y++, blit_dst += blit_dst_stride, blit_src += blit_src_stride )
					memcpy( blit_dst, blit_src, ( size_t )info.Width );
			} else {
				const unsigned int* blit_src = ( const unsigned int* )src_glyph.BitmapData;
				unsigned int* blit_dst       = atlas->TexPixelsRGBA32 + ( ty * blit_dst_stride ) + tx;
				for ( int y = 0; y < info.Height; y++, blit_dst += blit_dst_stride, blit_src += blit_src_stride )
					for ( int x = 0; x < info.Width; x++ )
						blit_dst[ x ] = blit_src[ x ];
			}
		}
		if ( face_open )
			src_tmp.Font.CloseFont( );

		src_tmp.Rects = NULL;
	}
	atlas->TexPixelsUseColors = tex_use_colors;

	// Cleanup
	for ( int buf_i = 0; buf_i < buf_bitmap_buffers.Size; buf_i++ )
		IM_FREE( buf_bitmap_buffers[ buf_i ] );
	src_tmp_array.clear_destruct( );

	ImFontAtlasBuildFinish( atlas );

	return true;
}

// FreeType memory allocation callbacks
static void* FreeType_Alloc( FT_Memory /*memory*/, long size )
{
	return GImGuiFreeTypeAllocFunc( ( size_t )size, GImGuiFreeTypeAllocatorUserData );
}

static void FreeType_Free( FT_Memory /*memory*/, void* block )
{
	GImGuiFreeTypeFreeFunc( block, GImGuiFreeTypeAllocatorUserData );
}

static void* FreeType_Realloc( FT_Memory /*memory*/, long cur_size, long new_size, void* block )
{
	// Implement realloc() as we don't ask user to provide it.
	if ( block == NULL )
		return GImGuiFreeTypeAllocFunc( ( size_t )new_size, GImGuiFreeTypeAllocatorUserData );

	if ( new_size == 0 ) {
		GImGuiFreeTypeFreeFunc( block, GImGuiFreeTypeAllocatorUserData );
		return NULL;
	}

	if ( new_size > cur_size ) {
		void* new_block = GImGuiFreeTypeAllocFunc( ( size_t )new_size, GImGuiFreeTypeAllocatorUserData );
		memcpy( new_block, block, ( size_t )cur_size );
		GImGuiFreeTypeFreeFunc( block, GImGuiFreeTypeAllocatorUserData );
		return new_block;
	}

	return block;
}

static bool ImFontAtlasBuildWithFreeType( ImFontAtlas* atlas )
{
	// FreeType memory management: https://www.freetype.org/freetype2/docs/design/design-4.html
	FT_MemoryRec_ memory_rec = { };
	memory_rec.user          = NULL;
	memory_rec.alloc         = &FreeType_Alloc;
	memory_rec.free          = &FreeType_Free;
	memory_rec.realloc       = &FreeType_Realloc;

	// https://www.freetype.org/freetype2/docs/reference/ft2-module_management.html#FT_New_Library
	FT_Library ft_library;
	g_ft_build_fail[ 0 ] = '\0';
	FT_Error error       = FT_New_Library( &memory_rec, &ft_library );
	if ( error != 0 ) {
		ImFormatString( g_ft_build_fail, sizeof( g_ft_build_fail ), "FT_New_Library error 0x%x", ( unsigned )error );
		return false;
	}

	// If you don't call FT_Add_Default_Modules() the rest of code may work, but FreeType won't use our custom allocator.
	FT_Add_Default_Modules( ft_library );

	bool ret = ImFontAtlasBuildWithFreeTypeEx( ft_library, atlas, atlas->FontBuilderFlags );
	FT_Done_Library( ft_library );

	return ret;
}

const ImFontBuilderIO* ImGuiFreeType::GetBuilderForFreeType( )
{
	static ImFontBuilderIO io;
	io.FontBuilder_Build = ImFontAtlasBuildWithFreeType;
	return &io;
}

bool ImGuiFreeType::GiveBackPixels( void* pixels ) { return n_px_slot::give_back( pixels ); }

void ImGuiFreeType::ReleasePixelSlot( ) { n_px_slot::release( ); }

int ImGuiFreeType::OpenFileFace( void* ft_library, const char* path, long face_index, void** face )
{
	FT_Open_Args args = { };
	args.flags        = FT_OPEN_STREAM;
	args.stream       = ImFontFileStreamOpen( path );
	return args.stream ? FT_Open_Face( ( FT_Library )ft_library, &args, face_index, ( FT_Face* )face ) : FT_Err_Cannot_Open_Resource;
}

void ImGuiFreeType::SetAllocatorFunctions( void* ( *alloc_func )( size_t sz, void* user_data ), void ( *free_func )( void* ptr, void* user_data ),
                                           void* user_data )
{
	GImGuiFreeTypeAllocFunc         = alloc_func;
	GImGuiFreeTypeFreeFunc          = free_func;
	GImGuiFreeTypeAllocatorUserData = user_data;
}