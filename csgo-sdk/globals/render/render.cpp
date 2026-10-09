#include "render.h"
#include "../../dependencies/avatar_data/avatar_data.h"
#include "../../dependencies/imgui/helpers/fonts.h"
#include "../../dependencies/fonts/spectator_fonts.h"
#include "../fonts/fonts.h"

#include "../../game/sdk/includes/includes.h"
#include "../../hacks/avatar_cache/avatar_cache.h"
#include "../../hacks/mc_hud/mc_font_ttf.h"
#include "../../hacks/visuals/screen/resolution_spoof.h"
#include "../../utilities/perf/perf_watch.h"
#include "../includes/includes.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <utility>

extern char g_ft_build_fail[ 160 ];

static unsigned largest_free_block_mb( )
{
	MEMORY_BASIC_INFORMATION region{ };
	std::uintptr_t cursor = 0, best = 0;

	while ( VirtualQuery( reinterpret_cast< void* >( cursor ), &region, sizeof( region ) ) ) {
		if ( region.State == MEM_FREE )
			best = std::max< std::uintptr_t >( best, region.RegionSize );

		const std::uintptr_t next = reinterpret_cast< std::uintptr_t >( region.BaseAddress ) + region.RegionSize;
		if ( next <= cursor )
			break;
		cursor = next;
	}

	return static_cast< unsigned >( best / ( 1024 * 1024 ) );
}

struct font_set_t {
	ImFontAtlas* m_atlas = nullptr;
	ImFont* m_fonts[ e_font_names::font_name_max ]{ };
	ImFont* m_custom_fonts[ e_custom_font_names::custom_font_name_max ]{ };
	float m_build_ms = 0.f;
};

/* side atlas of scaled indicator fonts. m_base = the indicator font it was baked against (a main rebuild
   retires it). TexID = our own d3d texture, made on the render thread (update_indicator_fonts) */
struct indicator_font_set_t {
	ImFontAtlas* m_atlas = nullptr;
	const ImFont* m_base = nullptr;
	int m_count          = 0;
	float m_scales[ 4 ]{ };
	ImFont* m_fonts[ 4 ]{ };
	std::vector< unsigned char > m_data{ };
	float m_build_ms = 0.f;
};

/* dpi twins: main atlas fonts that drew on a dpi scaled list, re-rasterized at size x scale (antialiased) in a
   side atlas. imgui_draw RenderText takes glyph quads from the twin (botox_dpi_twin), layout from the base */
constexpr int k_dpi_twin_max = 16;

struct dpi_font_request_t {
	float m_scale = 1.f;
	int m_count   = 0;
	const ImFont* m_bases[ k_dpi_twin_max ]{ };

	bool operator==( const dpi_font_request_t& ) const = default;
};

struct dpi_font_set_t {
	ImFontAtlas* m_atlas = nullptr;
	float m_scale        = 1.f;
	int m_count          = 0;
	const ImFont* m_bases[ k_dpi_twin_max ]{ };
	ImFont* m_fonts[ k_dpi_twin_max ]{ };
	float m_build_ms = 0.f;
};

/* render thread, except the pending hand-off. wanted = every base seen on a scaled list at m_scale */
static std::atomic< dpi_font_set_t* > g_pending_dpi_fonts{ nullptr };
static dpi_font_set_t* g_dpi_fonts = nullptr;
static dpi_font_request_t g_dpi_wanted{ }, g_dpi_seen{ }, g_dpi_baked{ };
static std::chrono::steady_clock::time_point g_dpi_changed{ };

/* bg / fg lists inside an open panel stretch block this frame (dpi scaled when "dpi scale panels" is on) */
static std::vector< const ImDrawList* > g_open_panel_lists{ };

static void discard_dpi_set( dpi_font_set_t* set )
{
	if ( !set )
		return;

	if ( set->m_atlas && set->m_atlas->TexID ) {
		ImGui_ImplDX9_SetTwinTextureA8( nullptr );
		static_cast< IDirect3DTexture9* >( set->m_atlas->TexID )->Release( );
	}

	IM_DELETE( set->m_atlas );
	delete set;
}

namespace
{

	constexpr ImWchar k_latin_ext_ranges[] = {
		0x0020, 0x024F,
		0x0250, 0x02AF,
		0x02B0, 0x02FF,
		0x0300, 0x036F,
		0x0370, 0x03FF,
		0x0400, 0x04FF,
		0x0500, 0x052F,
		0x1E00, 0x1EFF,
		0x1F00, 0x1FFF,
		0,
	};

	constexpr ImWchar k_symbol_ranges[] = {
		0x2000, 0x206F,
		0x2070, 0x209F,
		0x20A0, 0x20CF,
		0x2100, 0x214F,
		0x2150, 0x218F,
		0x2190, 0x21FF,
		0x2200, 0x22FF,
		0x2300, 0x23FF,
		0x2460, 0x24FF,
		0x2500, 0x257F,
		0x2580, 0x259F,
		0x25A0, 0x25FF,
		0x2600, 0x26FF,
		0x2700, 0x27BF,
		0x2B00, 0x2BFF,
		0,
	};

	constexpr ImWchar k_rtl_ranges[] = {
		0x0590, 0x05FF,
		0x0600, 0x06FF,
		0x0750, 0x077F,
		0xFB00, 0xFB4F,
		0xFB50, 0xFDFF,
		0xFE70, 0xFEFF,
		0,
	};

	constexpr ImWchar k_thai_ranges[] = {
		0x0E00, 0x0E7F,
		0,
	};

	constexpr ImWchar k_indic_ranges[] = {
		0x0900, 0x097F,
		0x0980, 0x09FF,
		0x0A00, 0x0A7F,
		0x0A80, 0x0AFF,
		0x0B00, 0x0B7F,
		0x0C00, 0x0C7F,
		0x0D00, 0x0D7F,
		0,
	};

	constexpr ImWchar k_georgian_ranges[] = {
		0x10A0, 0x10FF,
		0x2D00, 0x2D2F,
		0,
	};

	constexpr ImWchar k_latin_more_ranges[] = {
		0x1D00, 0x1DBF,
		0x1DC0, 0x1DFF,
		0x2C60, 0x2C7F,
		0xA4D0, 0xA4FF,
		0xA720, 0xA7FF,
		0,
	};

	constexpr ImWchar k_symbol_more_ranges[] = {
		0x20D0,  0x20FF,
		0x2400,  0x245F,
		0x27C0,  0x27FF,
		0x2900,  0x2AFF,
		0x2E00,  0x2E7F,
		0x1D100, 0x1D1FF,
		0x1D400, 0x1D7FF,
		0,
	};

	constexpr ImWchar k_cjk_more_ranges[] = {
		0x2E80, 0x2FDF,
		0xFE50, 0xFE6F,
		0,
	};

	constexpr ImWchar k_yi_ranges[] = {
		0xA000, 0xA4CF,
		0,
	};

	constexpr ImWchar k_syllabics_ranges[] = {
		0x1400, 0x167F,
		0x18B0, 0x18FF,
		0,
	};

	constexpr ImWchar k_glagolitic_ranges[] = {
		0x2C00, 0x2C5F,
		0,
	};

	const std::vector< unsigned char >& font_file( const std::string& path )
	{
		static std::unordered_map< std::string, std::vector< unsigned char > > cache{ };

		const auto [ it, inserted ] = cache.try_emplace( path );

		if ( inserted ) {
			std::ifstream file( path, std::ios::binary | std::ios::ate );
			const std::streamoff size = file ? static_cast< std::streamoff >( file.tellg( ) ) : 0;

			if ( size > 0 ) {
				it->second.resize( static_cast< std::size_t >( size ) );
				file.seekg( 0 );

				if ( !file.read( reinterpret_cast< char* >( it->second.data( ) ), size ) )
					it->second.clear( );
			}
		}

		return it->second;
	}

	ImFont* add_system_font( ImFontAtlas* atlas, const char* path, const float size, ImFontConfig config, const ImWchar* ranges = nullptr )
	{
		const std::vector< unsigned char >& bytes = font_file( path );

		if ( bytes.empty( ) )
			return nullptr;

		config.FontDataOwnedByAtlas = false;

		return atlas->AddFontFromMemoryTTF( const_cast< unsigned char* >( bytes.data( ) ), static_cast< int >( bytes.size( ) ), size, &config, ranges );
	}

	bool merge_font( ImFontAtlas* atlas, const char* const* candidate_paths, const float size, const unsigned int builder_flags,
	                 const ImWchar* ranges )
	{
		ImFontConfig font_config     = { };
		font_config.MergeMode        = true;
		font_config.FontBuilderFlags = builder_flags;

		for ( int i = 0; candidate_paths[ i ]; ++i )
			if ( add_system_font( atlas, candidate_paths[ i ], size, font_config, ranges ) )
				return true;

		return false;
	}

	std::string system_font_path( const char* file_name )
	{
		return std::vformat( "{}\\Fonts\\{}", std::make_format_args( g_ctx.m_windows_directory, file_name ) );
	}

	struct font_request_t {
		std::vector< bool > m_indicator_flags, m_esp_flags;
		font_setting_t m_indicator, m_esp;
	};

	font_request_t current_font_request( )
	{
		return { GET_VARIABLE( g_variables.m_indicator_font_flags, std::vector< bool > ),
		         GET_VARIABLE( g_variables.m_esp_font_flags, std::vector< bool > ),
		         GET_VARIABLE( g_variables.m_indicator_font_settings, font_setting_t ),
		         GET_VARIABLE( g_variables.m_esp_font_settings, font_setting_t ) };
	}

	/* a set that never got installed (unload, or a newer build replaced it) */
	void discard_font_set( font_set_t* set )
	{
		if ( !set )
			return;

		IM_DELETE( set->m_atlas );
		delete set;
	}

	struct indicator_font_request_t {
		std::vector< bool > m_flags;
		std::string m_name;
		float m_base_size    = 0.f;
		const ImFont* m_base = nullptr;
		int m_count          = 0;
		float m_scales[ 4 ]{ };

		bool operator==( const indicator_font_request_t& ) const = default;
	};

	indicator_font_request_t wanted_indicator_fonts( )
	{
		indicator_font_request_t request{ };

		ImFont* const base = g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_indicator ];
		if ( !base )
			return request;

		request.m_base      = base;
		request.m_base_size = base->FontSize;

		if ( base != g_render.m_fonts[ e_font_names::font_name_indicator_29 ] ) {
			request.m_name  = GET_VARIABLE( g_variables.m_indicator_font_settings, font_setting_t ).m_name;
			request.m_flags = GET_VARIABLE( g_variables.m_indicator_font_flags, std::vector< bool > );
		}

		for ( float scale : { GET_VARIABLE( g_variables.m_velocity_indicator_scale, float ), GET_VARIABLE( g_variables.m_stamina_indicator_scale, float ),
		                      GET_VARIABLE( g_variables.m_key_indicators_scale, float ), GET_VARIABLE( g_variables.m_key_press_scale, float ) } ) {
			scale = std::clamp( scale, k_indicator_scale_min, k_indicator_scale_max );

			float* const end = request.m_scales + request.m_count;
			if ( std::fabs( scale - 1.f ) < 0.001f || std::find( request.m_scales, end, scale ) != end )
				continue;

			request.m_scales[ request.m_count++ ] = scale;
		}

		return request;
	}

	/* texture first (render thread only when it holds one; worker-parked sets never do) */
	void discard_indicator_set( indicator_font_set_t* set )
	{
		if ( !set )
			return;

		if ( set->m_atlas && set->m_atlas->TexID )
			static_cast< IDirect3DTexture9* >( set->m_atlas->TexID )->Release( );

		IM_DELETE( set->m_atlas );
		delete set;
	}

	IDirect3DTexture9* upload_indicator_atlas( IDirect3DDevice9* device, const ImFontAtlas* atlas )
	{
		const unsigned char* const alpha = atlas->TexPixelsAlpha8;
		const int width = atlas->TexWidth, height = atlas->TexHeight;

		if ( !device || !alpha || width <= 0 || height <= 0 )
			return nullptr;

		IDirect3DTexture9* texture = nullptr;
		if ( FAILED( device->CreateTexture( width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr ) ) || !texture ) {
			texture = nullptr;
			if ( FAILED( device->CreateTexture( width, height, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr ) ) ||
			     !texture )
				return nullptr;
		}

		D3DLOCKED_RECT locked{ };
		if ( FAILED( texture->LockRect( 0, &locked, nullptr, 0 ) ) ) {
			texture->Release( );
			return nullptr;
		}

		for ( int y = 0; y < height; ++y ) {
			unsigned int* const dst       = reinterpret_cast< unsigned int* >( static_cast< unsigned char* >( locked.pBits ) + locked.Pitch * y );
			const unsigned char* const src = alpha + static_cast< std::size_t >( width ) * y;

			for ( int x = 0; x < width; ++x )
				dst[ x ] = IM_COL32( 255, 255, 255, src[ x ] );
		}

		texture->UnlockRect( 0 );
		return texture;
	}
}

static void add_font_flags( const std::vector< bool >& font_flags, ImFontConfig& font_config )
{
	if ( font_flags.size( ) < e_free_type_font_flags::font_flag_max )
		return;

	if ( font_flags[ e_free_type_font_flags::font_flag_nohinting ] )
		font_config.FontBuilderFlags += 1;

	if ( font_flags[ e_free_type_font_flags::font_flag_noautohint ] )
		font_config.FontBuilderFlags += 2;

	if ( font_flags[ e_free_type_font_flags::font_flag_forceautohint ] )
		font_config.FontBuilderFlags += 4;

	if ( font_flags[ e_free_type_font_flags::font_flag_lighthinting ] )
		font_config.FontBuilderFlags += 8;

	if ( font_flags[ e_free_type_font_flags::font_flag_monohinting ] )
		font_config.FontBuilderFlags += 16;

	if ( font_flags[ e_free_type_font_flags::font_flag_bold ] )
		font_config.FontBuilderFlags += 32;

	if ( font_flags[ e_free_type_font_flags::font_flag_oblique ] )
		font_config.FontBuilderFlags += 64;

	if ( font_flags[ e_free_type_font_flags::font_flag_monochrome ] )
		font_config.FontBuilderFlags += 128;
}

/* merge system fallback fonts into the last added font so non-ascii names render, not '?'. call right
   after the owning AddFont*; size + builder_flags must match the base font. */
static void merge_extended_glyphs( ImFontAtlas* atlas, const float size, const unsigned int builder_flags, const e_glyph_tier tier )
{
	if ( size <= 0.f )
		return;

	{
		const std::string bold = system_font_path( "segoeuib.ttf" ), regular = system_font_path( "segoeui.ttf" ),
		                  verdana = system_font_path( "verdana.ttf" ), tahoma = system_font_path( "tahoma.ttf" );
		const char* candidates[] = { bold.c_str( ), regular.c_str( ), verdana.c_str( ), tahoma.c_str( ), nullptr };

		merge_font( atlas, candidates, size, builder_flags, k_latin_ext_ranges );
	}

	{
		const std::string symbol = system_font_path( "seguisym.ttf" ), arial = system_font_path( "arial.ttf" );
		const char* candidates[] = { symbol.c_str( ), arial.c_str( ), nullptr };

		merge_font( atlas, candidates, size, builder_flags, k_symbol_ranges );
	}

	if ( tier != e_glyph_tier::glyph_tier_full )
		return;

	{
		const std::string arial = system_font_path( "arial.ttf" ), tahoma = system_font_path( "tahoma.ttf" );
		const char* candidates[] = { arial.c_str( ), tahoma.c_str( ), nullptr };

		merge_font( atlas, candidates,size, builder_flags, k_rtl_ranges );
	}

	{
		const std::string leelawadee = system_font_path( "leelawui.ttf" ), tahoma = system_font_path( "tahoma.ttf" );
		const char* candidates[] = { leelawadee.c_str( ), tahoma.c_str( ), nullptr };

		merge_font( atlas, candidates,size, builder_flags, k_thai_ranges );
	}

	{
		const std::string nirmala_ttc = system_font_path( "Nirmala.ttc" ), nirmala = system_font_path( "Nirmala.ttf" );
		const char* candidates[] = { nirmala_ttc.c_str( ), nirmala.c_str( ), nullptr };

		merge_font( atlas, candidates,size, builder_flags, k_indic_ranges );
	}

	{
		const std::string sylfaen = system_font_path( "sylfaen.ttf" );
		const char* candidates[] = { sylfaen.c_str( ), nullptr };

		merge_font( atlas, candidates, size, builder_flags, k_georgian_ranges );
	}

	{
		const std::string meiryo = system_font_path( "meiryo.ttc" ), yu_gothic = system_font_path( "YuGothM.ttc" ),
		                  ms_yahei = system_font_path( "msyh.ttc" );
		const char* candidates[] = { meiryo.c_str( ), yu_gothic.c_str( ), ms_yahei.c_str( ), nullptr };

		merge_font( atlas, candidates,size, builder_flags, atlas->GetGlyphRangesJapanese( ) );
	}

	{
		const std::string ms_yahei = system_font_path( "msyh.ttc" ), simsun = system_font_path( "simsun.ttc" );
		const char* candidates[] = { ms_yahei.c_str( ), simsun.c_str( ), nullptr };

		merge_font( atlas, candidates,size, builder_flags, atlas->GetGlyphRangesChineseSimplifiedCommon( ) );
	}

	{
		const std::string malgun = system_font_path( "malgun.ttf" ), gulim = system_font_path( "gulim.ttc" );
		const char* candidates[] = { malgun.c_str( ), gulim.c_str( ), nullptr };

		merge_font( atlas, candidates,size, builder_flags, atlas->GetGlyphRangesKorean( ) );
	}

	{
		const std::string bold = system_font_path( "segoeuib.ttf" ), regular = system_font_path( "segoeui.ttf" ),
		                  arial = system_font_path( "arial.ttf" );
		const char* candidates[] = { bold.c_str( ), regular.c_str( ), arial.c_str( ), nullptr };

		merge_font( atlas, candidates, size, builder_flags, k_latin_more_ranges );
	}

	{
		const std::string symbol = system_font_path( "seguisym.ttf" ), cambria = system_font_path( "cambria.ttc" );
		const char* candidates[] = { symbol.c_str( ), cambria.c_str( ), nullptr };

		merge_font( atlas, candidates, size, builder_flags, k_symbol_more_ranges );
	}

	{
		const std::string yu_gothic = system_font_path( "YuGothM.ttc" ), ms_yahei = system_font_path( "msyh.ttc" );
		const char* candidates[] = { yu_gothic.c_str( ), ms_yahei.c_str( ), nullptr };

		merge_font( atlas, candidates, size, builder_flags, k_cjk_more_ranges );
	}

	{
		const std::string yi = system_font_path( "msyi.ttf" );
		const char* candidates[] = { yi.c_str( ), nullptr };

		merge_font( atlas, candidates, size, builder_flags, k_yi_ranges );
	}

	{
		const std::string gadugi = system_font_path( "gadugi.ttf" );
		const char* candidates[] = { gadugi.c_str( ), nullptr };

		merge_font( atlas, candidates, size, builder_flags, k_syllabics_ranges );
	}

	{
		const std::string historic = system_font_path( "seguihis.ttf" );
		const char* candidates[] = { historic.c_str( ), nullptr };

		merge_font( atlas, candidates, size, builder_flags, k_glagolitic_ranges );
	}
}

static bool build_fonts( font_set_t& set, const font_request_t& request )
{
	ImFontAtlas* const atlas = set.m_atlas = IM_NEW( ImFontAtlas )( );
	ImFont** const fonts     = set.m_fonts;

	/* merged blocks make the atlas wide: cap it so the driver never refuses the texture, no pow2 height */
	atlas->TexDesiredWidth = 4096;
	atlas->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	ImFontConfig verdana_font_config = { };
	verdana_font_config.FontBuilderFlags =
		ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_Monochrome | ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_MonoHinting;

	fonts[ e_font_names::font_name_verdana_11 ] =
		atlas->AddFontFromMemoryCompressedTTF( verdana_compressed_data, verdana_compressed_size, 11.f, &verdana_font_config );
	merge_extended_glyphs( atlas,11.f, verdana_font_config.FontBuilderFlags, e_glyph_tier::glyph_tier_full );

	fonts[ e_font_names::font_name_verdana_bd_11 ] =
		atlas->AddFontFromMemoryCompressedTTF( verdana_bold_compressed_data, verdana_bold_compressed_size, 11.f, &verdana_font_config );
	merge_extended_glyphs( atlas,11.f, verdana_font_config.FontBuilderFlags, e_glyph_tier::glyph_tier_full );

	fonts[ e_font_names::font_name_indicator_29 ] =
		atlas->AddFontFromMemoryCompressedTTF( verdana_bold_compressed_data, verdana_bold_compressed_size, 29.f );
	merge_extended_glyphs( atlas,29.f, 0, e_glyph_tier::glyph_tier_basic );

	ImFontConfig tahoma_font_config = { };
	tahoma_font_config.FontBuilderFlags =
		ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_Monochrome | ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_MonoHinting;

	fonts[ e_font_names::font_name_tahoma_12 ] =
		atlas->AddFontFromMemoryCompressedTTF( tahoma_compressed_data, tahoma_compressed_size, 12.f, &tahoma_font_config );
	merge_extended_glyphs( atlas,12.f, tahoma_font_config.FontBuilderFlags, e_glyph_tier::glyph_tier_full );

	fonts[ e_font_names::font_name_tahoma_bd_12 ] =
		atlas->AddFontFromMemoryCompressedTTF( tahoma_bold_compressed_data, tahoma_bold_compressed_size, 12.f, &tahoma_font_config );
	merge_extended_glyphs( atlas,12.f, tahoma_font_config.FontBuilderFlags, e_glyph_tier::glyph_tier_full );

	ImFontConfig chillware_font_config     = { };
	chillware_font_config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_NoHinting;

	fonts[ e_font_names::font_name_chillware_13 ] =
		atlas->AddFontFromMemoryCompressedTTF( verdana_bold_compressed_data, verdana_bold_compressed_size, 13.f, &chillware_font_config );
	merge_extended_glyphs( atlas,13.f, chillware_font_config.FontBuilderFlags, e_glyph_tier::glyph_tier_full );

	const auto add_donor_font = [ & ]( const unsigned char* data, const int size, const float pixels, const unsigned int flags,
	                                   const bool merge = true ) -> ImFont* {
		ImFontConfig config         = { };
		config.FontDataOwnedByAtlas = false;
		config.PixelSnapH           = true;
		config.OversampleH          = 5;
		config.OversampleV          = 1;
		config.FontBuilderFlags     = flags;

		ImFont* const font = atlas->AddFontFromMemoryTTF( const_cast< unsigned char* >( data ), size, pixels, &config,
		                                                     atlas->GetGlyphRangesCyrillic( ) );
		if ( merge )
			merge_extended_glyphs( atlas,pixels, flags, e_glyph_tier::glyph_tier_full );

		return font;
	};

	fonts[ e_font_names::font_name_lobotomy_13 ] = add_donor_font( sf_pro_display_semibold, sizeof( sf_pro_display_semibold ), 13.f,
	                                                                 ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_Bitmap );
	fonts[ e_font_names::font_name_lobotomy_bold_13 ] = add_donor_font( sf_pro_display_bold, sizeof( sf_pro_display_bold ), 13.f,
	                                                                      ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_Bitmap, false );
	fonts[ e_font_names::font_name_kamibebra_13 ] =
		add_donor_font( rubik_regular, sizeof( rubik_regular ), 13.f, ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_Bitmap );
	fonts[ e_font_names::font_name_kamibebra_bold_13 ] =
		add_donor_font( rubik_medium, sizeof( rubik_medium ), 13.f,
	                    ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_Bitmap | ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_LightHinting );

	fonts[ e_font_names::font_name_cumhacck_35 ] =
		atlas->AddFontFromMemoryCompressedTTF( verdana_bold_compressed_data, verdana_bold_compressed_size, 35.f, &tahoma_font_config );

	fonts[ e_font_names::font_name_airplane_title ] = add_donor_font( montserrat_regular, sizeof( montserrat_regular ), 10.f, 0, false );

	ImFontConfig airplane_rows_config     = { };
	airplane_rows_config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_MonoHinting;

	fonts[ e_font_names::font_name_airplane_rows ] =
		atlas->AddFontFromMemoryCompressedTTF( tahoma_bold_compressed_data, tahoma_bold_compressed_size, 11.f, &airplane_rows_config );
	merge_extended_glyphs( atlas, 11.f, airplane_rows_config.FontBuilderFlags, e_glyph_tier::glyph_tier_full );

	fonts[ e_font_names::font_name_clarity_icon_50 ] =
		add_donor_font( clarity_icon_font, sizeof( clarity_icon_font ), 50.f, ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_NoHinting, false );

	fonts[ e_font_names::font_name_clarity_inter_semibold_14 ] = add_donor_font( inter_semibold, sizeof( inter_semibold ), 14.f, 0, false );
	merge_extended_glyphs( atlas, 14.f, 0, e_glyph_tier::glyph_tier_basic );
	fonts[ e_font_names::font_name_clarity_inter_bold_15 ]     = add_donor_font( inter_bold, sizeof( inter_bold ), 15.f, 0, false );
	fonts[ e_font_names::font_name_clarity_inter_medium_15 ]   = add_donor_font( inter_medium, sizeof( inter_medium ), 15.f, 0 );

	fonts[ e_font_names::font_name_sunflower_30 ] = add_donor_font( sunflower_medium, sizeof( sunflower_medium ), 30.f, 0, false );
	merge_extended_glyphs( atlas, 30.f, 0, e_glyph_tier::glyph_tier_basic );

	fonts[ e_font_names::font_name_interwebz_calibri ] =
		add_system_font( atlas, system_font_path( "calibri.ttf" ).c_str( ), 16.f, tahoma_font_config );

	fonts[ e_font_names::font_name_havoc_tahoma_13 ] =
		atlas->AddFontFromMemoryCompressedTTF( tahoma_compressed_data, tahoma_compressed_size, 13.f, &tahoma_font_config );
	merge_extended_glyphs( atlas, 13.f, tahoma_font_config.FontBuilderFlags, e_glyph_tier::glyph_tier_full );

	{
		ImFontConfig evolve_config     = { };
		evolve_config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_NoHinting;

		fonts[ e_font_names::font_name_evolve_16 ] = add_system_font( atlas, system_font_path( "segoeui.ttf" ).c_str( ), 16.f, evolve_config );
		if ( fonts[ e_font_names::font_name_evolve_16 ] )
			merge_extended_glyphs( atlas, 16.f, evolve_config.FontBuilderFlags, e_glyph_tier::glyph_tier_full );

		fonts[ e_font_names::font_name_evolve_bold_16 ] =
			add_system_font( atlas, system_font_path( "segoeuib.ttf" ).c_str( ), 16.f, evolve_config );
		if ( fonts[ e_font_names::font_name_evolve_bold_16 ] )
			merge_extended_glyphs( atlas, 16.f, evolve_config.FontBuilderFlags, e_glyph_tier::glyph_tier_basic );
	}

	{
		ImFontConfig legendware_config     = { };
		legendware_config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_MonoHinting;

		fonts[ e_font_names::font_name_legendware_verdana_12 ] =
			atlas->AddFontFromMemoryCompressedTTF( verdana_compressed_data, verdana_compressed_size, 12.f, &legendware_config );
		merge_extended_glyphs( atlas, 12.f, legendware_config.FontBuilderFlags, e_glyph_tier::glyph_tier_basic );
	}

	fonts[ e_font_names::font_name_legendware_lucida_10 ] = add_system_font( atlas, system_font_path( "lucon.ttf" ).c_str( ), 10.f, tahoma_font_config );
	if ( fonts[ e_font_names::font_name_legendware_lucida_10 ] )
		merge_extended_glyphs( atlas, 10.f, tahoma_font_config.FontBuilderFlags, e_glyph_tier::glyph_tier_full );

	{
		ImFontConfig interium_config         = { };
		interium_config.FontDataOwnedByAtlas = false;

		fonts[ e_font_names::font_name_interium_droid_24 ] = atlas->AddFontFromMemoryTTF(
			const_cast< unsigned char* >( droid_sans_bold ), sizeof( droid_sans_bold ), 24.f, &interium_config, atlas->GetGlyphRangesCyrillic( ) );
		merge_extended_glyphs( atlas, 24.f, 0, e_glyph_tier::glyph_tier_basic );
	}

	{
		ImFontConfig skebob_config         = { };
		skebob_config.FontDataOwnedByAtlas = false;

		fonts[ e_font_names::font_name_skebob_nunito_14 ] = atlas->AddFontFromMemoryTTF(
			const_cast< unsigned char* >( nunito_medium ), sizeof( nunito_medium ), 14.f, &skebob_config, atlas->GetGlyphRangesCyrillic( ) );
		merge_extended_glyphs( atlas, 14.f, 0, e_glyph_tier::glyph_tier_basic );

		fonts[ e_font_names::font_name_skebob_arial_12 ] =
			add_system_font( atlas, system_font_path( "arial.ttf" ).c_str( ), 12.f, ImFontConfig{ }, atlas->GetGlyphRangesCyrillic( ) );
		if ( fonts[ e_font_names::font_name_skebob_arial_12 ] )
			merge_extended_glyphs( atlas, 12.f, 0, e_glyph_tier::glyph_tier_full );

		fonts[ e_font_names::font_name_skebob_arial_13 ] =
			add_system_font( atlas, system_font_path( "arial.ttf" ).c_str( ), 13.f, ImFontConfig{ }, atlas->GetGlyphRangesCyrillic( ) );
		if ( fonts[ e_font_names::font_name_skebob_arial_13 ] )
			merge_extended_glyphs( atlas, 13.f, 0, e_glyph_tier::glyph_tier_basic );

		ImFontConfig cumidere_config     = { };
		cumidere_config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_LightHinting;

		fonts[ e_font_names::font_name_cumidere_tahoma_16 ] =
			atlas->AddFontFromMemoryCompressedTTF( tahoma_compressed_data, tahoma_compressed_size, 16.f, &cumidere_config );
		merge_extended_glyphs( atlas, 16.f, cumidere_config.FontBuilderFlags, e_glyph_tier::glyph_tier_basic );

		ImFontConfig illusory_config     = { };
		illusory_config.FontBuilderFlags =
			ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_MonoHinting | ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_Bold;

		fonts[ e_font_names::font_name_illusory_tahoma_13 ] =
			atlas->AddFontFromMemoryCompressedTTF( tahoma_compressed_data, tahoma_compressed_size, 13.f, &illusory_config );
		merge_extended_glyphs( atlas, 13.f, illusory_config.FontBuilderFlags, e_glyph_tier::glyph_tier_basic );

		fonts[ e_font_names::font_name_lumi_rubik_16 ] =
			add_donor_font( rubik_regular, sizeof( rubik_regular ), 16.f, ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_MonoHinting, false );
		merge_extended_glyphs( atlas, 16.f, ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_MonoHinting, e_glyph_tier::glyph_tier_basic );

		ImFontConfig lumi_proggy_config     = { };
		lumi_proggy_config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_MonoHinting |
		                                      ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_Monochrome;
		fonts[ e_font_names::font_name_lumi_proggy_13 ] = atlas->AddFontDefault( &lumi_proggy_config );

		ImFontConfig inba_config     = { };
		inba_config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_NoHinting;

		fonts[ e_font_names::font_name_inba_tahoma_13 ] =
			atlas->AddFontFromMemoryCompressedTTF( tahoma_compressed_data, tahoma_compressed_size, 13.f, &inba_config );
		merge_extended_glyphs( atlas, 13.f, inba_config.FontBuilderFlags, e_glyph_tier::glyph_tier_basic );

		ImFontConfig dna_config         = { };
		dna_config.FontDataOwnedByAtlas = false;
		dna_config.FontBuilderFlags     = ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_NoHinting;

		fonts[ e_font_names::font_name_dna_montserrat_15 ] = atlas->AddFontFromMemoryTTF(
			const_cast< unsigned char* >( dna_montserrat_medium ), sizeof( dna_montserrat_medium ), 15.f, &dna_config, atlas->GetGlyphRangesCyrillic( ) );
		merge_extended_glyphs( atlas, 15.f, dna_config.FontBuilderFlags, e_glyph_tier::glyph_tier_basic );

		fonts[ e_font_names::font_name_dna_pt_root_bold_15 ] = atlas->AddFontFromMemoryTTF(
			const_cast< unsigned char* >( pt_root_ui_bold ), sizeof( pt_root_ui_bold ), 15.f, &dna_config, atlas->GetGlyphRangesCyrillic( ) );
		merge_extended_glyphs( atlas, 15.f, dna_config.FontBuilderFlags, e_glyph_tier::glyph_tier_basic );

		fonts[ e_font_names::font_name_dna_clarity_icon_50 ] = atlas->AddFontFromMemoryTTF(
			const_cast< unsigned char* >( dna_clarity_icon ), sizeof( dna_clarity_icon ), 50.f, &dna_config );

		fonts[ e_font_names::font_name_cucumber_tahoma_14 ] =
			atlas->AddFontFromMemoryCompressedTTF( tahoma_compressed_data, tahoma_compressed_size, 14.f, &inba_config );
		merge_extended_glyphs( atlas, 14.f, inba_config.FontBuilderFlags, e_glyph_tier::glyph_tier_basic );

		fonts[ e_font_names::font_name_cucumber_verdana_14 ] =
			atlas->AddFontFromMemoryCompressedTTF( verdana_compressed_data, verdana_compressed_size, 14.f, &inba_config );

		fonts[ e_font_names::font_name_howeweware_minecraft_14 ] =
			atlas->AddFontFromMemoryTTF( const_cast< unsigned char* >( n_mc_assets::k_minecraft_ttf ), sizeof( n_mc_assets::k_minecraft_ttf ), 14.f, &dna_config,
		                                 atlas->GetGlyphRangesCyrillic( ) );
		merge_extended_glyphs( atlas, 14.f, dna_config.FontBuilderFlags, e_glyph_tier::glyph_tier_basic );
	}

	fonts[ e_font_names::font_name_points_montserrat_italic_18 ] = add_donor_font(
		montserrat_medium_italic, sizeof( montserrat_medium_italic ), 18.f, ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_Bitmap, false );

	{
		ImFontConfig airflow_config         = { };
		airflow_config.FontDataOwnedByAtlas = false;

		fonts[ e_font_names::font_name_airflow_14 ] = atlas->AddFontFromMemoryTTF(
			const_cast< unsigned char* >( sf_ui_display_semibold ), sizeof( sf_ui_display_semibold ), 14.f, &airflow_config,
			atlas->GetGlyphRangesCyrillic( ) );
		merge_extended_glyphs( atlas, 14.f, 0, e_glyph_tier::glyph_tier_full );
	}

	static const ImWchar icon_ranges[] = { 0xe005, 0xf8ff, 0 };

	fonts[ e_font_names::font_name_icon_13 ] =
		atlas->AddFontFromMemoryCompressedTTF( icon_font_compressed_data, icon_font_compressed_size, 13.f, 0, icon_ranges );

	ImFontConfig icon_font_config     = { };
	icon_font_config.FontBuilderFlags = ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_LightHinting |
	                                    ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_Monochrome |
	                                    ImGuiFreeTypeBuilderFlags::ImGuiFreeTypeBuilderFlags_MonoHinting;

	constexpr ImWchar weapon_icon_ranges[] = { 0xe000, 0xf8ff, 0 };
	fonts[ e_font_names::font_name_icon_12 ] = atlas->AddFontFromMemoryCompressedTTF(
		weapon_icons_compressed_data, weapon_icons_compressed_size, 12.f, &icon_font_config, weapon_icon_ranges );

	auto reload_custom_font = [ & ]( const std::vector< bool >& font_flags, const font_setting_t& font_setting, const int slot,
	                                 ImFont* const fallback_font, const e_glyph_tier tier ) {
		ImFontConfig font_config = { };
		add_font_flags( font_flags, font_config );

		const std::string font_path = g_fonts.find_path( font_setting.m_name );

		ImFont* const font = ( font_setting.m_size > 0 && std::filesystem::exists( font_path ) )
		                          ? atlas->AddFontFromFileTTF( font_path.c_str( ), static_cast< float >( font_setting.m_size ), &font_config )
		                          : nullptr;

		if ( font ) {
			merge_extended_glyphs( atlas, static_cast< float >( font_setting.m_size ), font_config.FontBuilderFlags, tier );
			set.m_custom_fonts[ slot ] = font;
		}
		else
			set.m_custom_fonts[ slot ] = fallback_font;
	};

	reload_custom_font( request.m_indicator_flags, request.m_indicator, e_custom_font_names::custom_font_name_indicator,
	                    fonts[ e_font_names::font_name_indicator_29 ], e_glyph_tier::glyph_tier_basic );

	reload_custom_font( request.m_esp_flags, request.m_esp, e_custom_font_names::custom_font_name_esp,
	                    fonts[ e_font_names::font_name_verdana_bd_11 ], e_glyph_tier::glyph_tier_full );

	return ImGuiFreeType::BuildFontAtlas( atlas, 0x0 );
}

/* worker body: one build, parked for on_end_scene. the flag drops LAST - it is what eject and the
   next reload wait on, so nothing of ours may run after it but the thread's own return */
static void build_fonts_worker( const font_request_t request )
{
	font_set_t* const set = new font_set_t{ };

	const auto start = std::chrono::steady_clock::now( );

	if ( !build_fonts( *set, request ) ) {
		IM_DELETE( set->m_atlas );
		set->m_atlas = nullptr;
	}

	set->m_build_ms = std::chrono::duration< float, std::milli >( std::chrono::steady_clock::now( ) - start ).count( );

	discard_font_set( g_render.m_pending_fonts.exchange( set ) );

	g_render.m_font_build_running = false;
}

/* swap a finished set in, render thread, before NewFrame. the write lock covers the pointer copies
   only; the retired atlas is freed after it, outside. */
void n_render::impl_t::install_fonts( font_set_t* set )
{
	if ( !set->m_atlas ) {
		g_console.print< n_console::log_level::WARNING >( "font rebuild failed (out of memory or unreadable font) - kept the old fonts" );
		delete set;
		return;
	}

	ImGuiIO& io                = ImGui::GetIO( );
	ImFontAtlas* const retired = io.Fonts;

	{
		std::unique_lock< std::shared_mutex > font_lock( this->m_font_mutex );

		ImGui_ImplDX9_DestroyFontsTexture( );

		io.Fonts = set->m_atlas;
		std::ranges::copy( set->m_fonts, this->m_fonts );
		std::ranges::copy( set->m_custom_fonts, this->m_custom_fonts );
	}

	/* queued text carries the retired atlas' ImFont pointers: drop it, the game thread refills next pass */
	{
		std::unique_lock< std::shared_mutex > lock( this->m_mutex );

		this->m_thread_safe_draw_data.clear( );
	}

	/* twins are keyed by the retired fonts: drop them, the next scaled frame asks again */
	discard_dpi_set( g_pending_dpi_fonts.exchange( nullptr ) );
	discard_dpi_set( std::exchange( g_dpi_fonts, nullptr ) );
	g_dpi_wanted = g_dpi_seen = g_dpi_baked = { };

	IM_DELETE( retired );

	if ( const float build_ms = set->m_build_ms; build_ms > 0.f ) {
		const int width = set->m_atlas->TexWidth, height = set->m_atlas->TexHeight;
		const float mb  = static_cast< float >( width ) * static_cast< float >( height ) / ( 1024.f * 1024.f );
		g_console.print( std::vformat( "fonts rebuilt off the render thread in {:.0f} ms, atlas {}x{} = {:.1f} mb",
		                               std::make_format_args( build_ms, width, height, mb ) )
		                     .c_str( ) );
	}

	delete set;
}

static bool build_indicator_fonts( indicator_font_set_t& set, const indicator_font_request_t& request )
{
	ImFontAtlas* const atlas = set.m_atlas = IM_NEW( ImFontAtlas )( );
	atlas->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	set.m_base  = request.m_base;
	set.m_count = request.m_count;
	std::ranges::copy( request.m_scales, set.m_scales );

	ImFontConfig config = { };

	if ( !request.m_name.empty( ) ) {
		add_font_flags( request.m_flags, config );
		config.FontDataOwnedByAtlas = false;

		std::ifstream file( g_fonts.find_path( request.m_name ), std::ios::binary | std::ios::ate );
		const std::streamoff size = file ? static_cast< std::streamoff >( file.tellg( ) ) : 0;
		if ( size <= 0 )
			return false;

		set.m_data.resize( static_cast< std::size_t >( size ) );
		file.seekg( 0 );
		if ( !file.read( reinterpret_cast< char* >( set.m_data.data( ) ), size ) )
			return false;
	}

	for ( int i = 0; i < set.m_count; ++i ) {
		const float size = std::max( request.m_base_size * set.m_scales[ i ], 1.f );

		set.m_fonts[ i ] = set.m_data.empty( )
		                       ? atlas->AddFontFromMemoryCompressedTTF( verdana_bold_compressed_data, verdana_bold_compressed_size, size )
		                       : atlas->AddFontFromMemoryTTF( set.m_data.data( ), static_cast< int >( set.m_data.size( ) ), size, &config );
	}

	return ImGuiFreeType::BuildFontAtlas( atlas, 0x0 );
}

/* same shape as build_fonts_worker, same flag: bakes and main rebuilds run one after another, never together */
static void build_indicator_fonts_worker( const indicator_font_request_t request )
{
	indicator_font_set_t* const set = new indicator_font_set_t{ };

	const auto start = std::chrono::steady_clock::now( );

	if ( !build_indicator_fonts( *set, request ) ) {
		IM_DELETE( set->m_atlas );
		set->m_atlas = nullptr;
	}

	set->m_build_ms = std::chrono::duration< float, std::milli >( std::chrono::steady_clock::now( ) - start ).count( );

	discard_indicator_set( g_render.m_pending_indicator_fonts.exchange( set ) );

	g_render.m_font_build_running = false;
}

void n_render::impl_t::update_indicator_fonts( IDirect3DDevice9* device )
{
	if ( indicator_font_set_t* const set = this->m_pending_indicator_fonts.exchange( nullptr ) ) {
		if ( !set->m_atlas ) {
			g_console.print< n_console::log_level::WARNING >( "indicator font bake failed (unreadable font) - drawing the base font scaled" );
			delete set;
		}
		else {
			indicator_font_set_t* retired;
			{
				std::unique_lock< std::shared_mutex > font_lock( this->m_font_mutex );
				retired = std::exchange( this->m_indicator_fonts, set );
			}

			{
				std::unique_lock< std::shared_mutex > lock( this->m_mutex );
				this->m_thread_safe_draw_data.clear( );
			}

			discard_indicator_set( retired );

			const int width = set->m_atlas->TexWidth, height = set->m_atlas->TexHeight, count = set->m_count;
			g_console.print( std::vformat( "indicator fonts baked: {} sizes in {:.0f} ms, atlas {}x{}",
			                               std::make_format_args( count, set->m_build_ms, width, height ) )
			                     .c_str( ) );
		}
	}

	if ( indicator_font_set_t* const live = this->m_indicator_fonts; live && !live->m_atlas->TexID )
		live->m_atlas->TexID = upload_indicator_atlas( device, live->m_atlas );

	static indicator_font_request_t s_seen{ }, s_baked{ };
	static auto s_changed = std::chrono::steady_clock::now( );

	const auto now                 = std::chrono::steady_clock::now( );
	indicator_font_request_t want = wanted_indicator_fonts( );

	if ( !( want == s_seen ) ) {
		s_seen    = want;
		s_changed = now;
	}

	if ( !want.m_count || want == s_baked || now - s_changed < std::chrono::milliseconds( 250 ) || this->m_reload_fonts ||
	     this->m_font_build_running )
		return;

	s_baked                    = want;
	this->m_font_build_running = true;

	std::thread( build_indicator_fonts_worker, std::move( want ) ).detach( );
}

static bool build_dpi_fonts( dpi_font_set_t& set, const dpi_font_request_t& request )
{
	ImFontAtlas* const atlas = set.m_atlas = IM_NEW( ImFontAtlas )( );
	atlas->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;

	set.m_scale = request.m_scale;
	set.m_count = request.m_count;

	const float scale        = set.m_scale;
	constexpr unsigned mono  = ImGuiFreeTypeBuilderFlags_Monochrome | ImGuiFreeTypeBuilderFlags_MonoHinting;

	for ( int i = 0; i < set.m_count; ++i ) {
		const ImFont* const base = set.m_bases[ i ] = request.m_bases[ i ];

		for ( int c = 0; c < base->ConfigDataCount; ++c ) {
			ImFontConfig config = base->ConfigData[ c ];

			/* full glyph tiers (cjk, indic ..) stay 100 %: a string needing one draws the base font */
			if ( c > 0 && config.GlyphRanges != k_latin_ext_ranges && config.GlyphRanges != k_symbol_ranges )
				continue;

			config.FontDataOwnedByAtlas = false;
			config.DstFont              = nullptr;
			config.SizePixels *= scale;
			config.GlyphOffset       = ImVec2( config.GlyphOffset.x * scale, config.GlyphOffset.y * scale );
			config.GlyphExtraSpacing = ImVec2( config.GlyphExtraSpacing.x * scale, config.GlyphExtraSpacing.y * scale );
			config.GlyphMinAdvanceX *= scale;
			if ( config.GlyphMaxAdvanceX < FLT_MAX )
				config.GlyphMaxAdvanceX *= scale;

			/* 1 bit glyphs only fit the size they were hinted at: rescaled = antialiased */
			if ( config.FontBuilderFlags & mono )
				config.FontBuilderFlags = ( config.FontBuilderFlags & ~mono ) | ImGuiFreeTypeBuilderFlags_LightHinting;

			ImFont* const font = atlas->AddFont( &config );
			if ( c == 0 )
				set.m_fonts[ i ] = font;
		}
	}

	return ImGuiFreeType::BuildFontAtlas( atlas, 0x0 );
}

/* 4096 x 4096 = 16 mb as A8. a 30 px font at 200 % alone is ~5.5 m px, 32 bit process */
constexpr float k_dpi_atlas_max_px = 4096.f * 4096.f;

/* same flag as the other bakes: reads the live atlas' font bytes, so no main rebuild may swap it meanwhile */
static void build_dpi_fonts_worker( dpi_font_request_t request )
{
	const auto start = std::chrono::steady_clock::now( );

	dpi_font_set_t* set = nullptr;

	/* over budget: drop the biggest font (it stays on the 100 % atlas) and bake again */
	for ( ;; ) {
		set = new dpi_font_set_t{ };

		if ( !build_dpi_fonts( *set, request ) ) {
			IM_DELETE( set->m_atlas );
			set->m_atlas = nullptr;
			break;
		}

		if ( request.m_count <= 1 ||
		     static_cast< float >( set->m_atlas->TexWidth ) * static_cast< float >( set->m_atlas->TexHeight ) <= k_dpi_atlas_max_px )
			break;

		discard_dpi_set( set );

		const ImFont** const end = request.m_bases + request.m_count;
		const ImFont** const big = std::max_element( request.m_bases, end, []( const ImFont* a, const ImFont* b ) { return a->FontSize < b->FontSize; } );
		std::move( big + 1, end, big );
		request.m_bases[ --request.m_count ] = nullptr;
	}

	set->m_build_ms = std::chrono::duration< float, std::milli >( std::chrono::steady_clock::now( ) - start ).count( );

	discard_dpi_set( g_pending_dpi_fonts.exchange( set ) );

	g_render.m_font_build_running = false;
}

/* A8 (1/4 of argb), the backend takes its colour from the vertex. no A8 = the argb indicator upload */
static IDirect3DTexture9* upload_dpi_atlas( IDirect3DDevice9* device, const ImFontAtlas* atlas )
{
	const unsigned char* const alpha = atlas->TexPixelsAlpha8;
	const int width = atlas->TexWidth, height = atlas->TexHeight;

	if ( !device || !alpha || width <= 0 || height <= 0 )
		return nullptr;

	IDirect3DTexture9* texture = nullptr;
	if ( FAILED( device->CreateTexture( width, height, 1, 0, D3DFMT_A8, D3DPOOL_MANAGED, &texture, nullptr ) ) || !texture ) {
		texture = nullptr;
		if ( FAILED( device->CreateTexture( width, height, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8, D3DPOOL_DEFAULT, &texture, nullptr ) ) || !texture )
			return upload_indicator_atlas( device, atlas );
	}

	D3DLOCKED_RECT locked{ };
	if ( FAILED( texture->LockRect( 0, &locked, nullptr, 0 ) ) ) {
		texture->Release( );
		return nullptr;
	}

	for ( int y = 0; y < height; ++y )
		std::memcpy( static_cast< unsigned char* >( locked.pBits ) + locked.Pitch * y, alpha + static_cast< std::size_t >( width ) * y, width );

	texture->UnlockRect( 0 );
	ImGui_ImplDX9_SetTwinTextureA8( texture );
	return texture;
}

/* render thread, before NewFrame: install a baked set, (re)upload its texture, bake again once the wanted fonts settle */
static void update_dpi_fonts( IDirect3DDevice9* device )
{
	if ( dpi_font_set_t* const set = g_pending_dpi_fonts.exchange( nullptr ) ) {
		if ( !set->m_atlas ) {
			g_console.print< n_console::log_level::WARNING >( "dpi font bake failed - scaled text stays on the 100 % fonts" );
			delete set;
		}
		else {
			discard_dpi_set( std::exchange( g_dpi_fonts, set ) );

			const int width = set->m_atlas->TexWidth, height = set->m_atlas->TexHeight, count = set->m_count;
			const float percent = set->m_scale * 100.f;
			g_console.print( std::vformat( "dpi fonts baked: {} fonts at {:.0f} % in {:.0f} ms, atlas {}x{}",
			                               std::make_format_args( count, percent, set->m_build_ms, width, height ) )
			                     .c_str( ) );
		}
	}

	if ( g_dpi_fonts && !g_dpi_fonts->m_atlas->TexID ) {
		g_dpi_fonts->m_atlas->TexID = upload_dpi_atlas( device, g_dpi_fonts->m_atlas );

		/* dropped, not retried a frame: wanted == baked, so it stays on the base fonts until the scale or fonts change */
		if ( !g_dpi_fonts->m_atlas->TexID ) {
			const int width = g_dpi_fonts->m_atlas->TexWidth, height = g_dpi_fonts->m_atlas->TexHeight;
			g_console.print< n_console::log_level::WARNING >(
				std::vformat( "dpi font texture {}x{} failed - scaled text stays on the 100 % fonts", std::make_format_args( width, height ) )
					.c_str( ) );
			discard_dpi_set( std::exchange( g_dpi_fonts, nullptr ) );
		}
	}

	if ( g_render.m_dpi_scale == 1.f ) {
		discard_dpi_set( std::exchange( g_dpi_fonts, nullptr ) );
		g_dpi_wanted = g_dpi_seen = g_dpi_baked = { };
		return;
	}

	const auto now = std::chrono::steady_clock::now( );

	if ( !( g_dpi_wanted == g_dpi_seen ) ) {
		g_dpi_seen    = g_dpi_wanted;
		g_dpi_changed = now;
	}

	if ( !g_dpi_wanted.m_count || g_dpi_wanted == g_dpi_baked || now - g_dpi_changed < std::chrono::milliseconds( 250 ) ||
	     g_render.m_reload_fonts || g_render.m_font_build_running )
		return;

	g_dpi_baked                   = g_dpi_wanted;
	g_render.m_font_build_running = true;

	std::thread( build_dpi_fonts_worker, g_dpi_wanted ).detach( );
}

/* device reset: both side atlases re-upload on the next frame */
void n_render::impl_t::release_indicator_texture( )
{
	if ( g_dpi_fonts && g_dpi_fonts->m_atlas->TexID ) {
		ImGui_ImplDX9_SetTwinTextureA8( nullptr );
		static_cast< IDirect3DTexture9* >( g_dpi_fonts->m_atlas->TexID )->Release( );
		g_dpi_fonts->m_atlas->TexID = nullptr;
	}

	indicator_font_set_t* const live = this->m_indicator_fonts;
	if ( !live || !live->m_atlas || !live->m_atlas->TexID )
		return;

	static_cast< IDirect3DTexture9* >( live->m_atlas->TexID )->Release( );
	live->m_atlas->TexID = nullptr;
}

ImFont* n_render::impl_t::indicator_font( const float scale, float& font_size )
{
	ImFont* const base = this->m_custom_fonts[ e_custom_font_names::custom_font_name_indicator ];
	const float s      = std::clamp( scale, k_indicator_scale_min, k_indicator_scale_max );

	font_size = base ? base->FontSize * s : 0.f;

	const indicator_font_set_t* const set = this->m_indicator_fonts;
	if ( !base || !set || set->m_base != base || !set->m_atlas->TexID )
		return base;

	for ( int i = 0; i < set->m_count; ++i )
		if ( set->m_scales[ i ] == s && set->m_fonts[ i ] )
			return set->m_fonts[ i ];

	return base;
}

ImFont* n_render::impl_t::live_font( ImFont* font )
{
	if ( font ) {
		for ( ImFont* it : ImGui::GetIO( ).Fonts->Fonts )
			if ( it == font )
				return font;

		if ( const indicator_font_set_t* const set = this->m_indicator_fonts )
			for ( ImFont* it : set->m_atlas->Fonts )
				if ( it == font )
					return font;
	}

	return this->m_fonts[ e_font_names::font_name_verdana_11 ];
}

static void keep_windows_on_screen( )
{
	const auto context = ImGui::GetCurrentContext( );

	if ( !context )
		return;

	const ImVec2 display = ImGui::GetIO( ).DisplaySize;

	constexpr int settle_frames = 30;

	static ImVec2 last_display{ }, saved_display{ };
	static int same_frames      = 0;
	static bool restore_pending = false;

	struct saved_pos_t {
		ImGuiID m_id;
		ImVec2 m_pos;
	};
	static std::vector< saved_pos_t > saved{ };

	same_frames  = display.x == last_display.x && display.y == last_display.y ? same_frames + 1 : 0;
	last_display = display;

	const bool iconic = g_input.m_window && IsIconic( g_input.m_window );
	const bool usable = !iconic && display.x > 0.f && display.y > 0.f;

	g_ctx.m_display_stable = usable && same_frames >= settle_frames;

	if ( !usable || display.x != saved_display.x || display.y != saved_display.y )
		restore_pending = !saved.empty( );

	if ( restore_pending && usable && display.x == saved_display.x && display.y == saved_display.y ) {
		for ( const auto& entry : saved )
			if ( ImGuiWindow* const window = ImGui::FindWindowByID( entry.m_id ) )
				ImGui::SetWindowPos( window, entry.m_pos, ImGuiCond_::ImGuiCond_Always );

		restore_pending = false;
	}

	if ( !g_ctx.m_display_stable )
		return;

	constexpr float min_visible_fraction = 0.05f, pad = 10.f;

	constexpr int skip_flags = ImGuiWindowFlags_::ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_::ImGuiWindowFlags_Popup |
	                           ImGuiWindowFlags_::ImGuiWindowFlags_Tooltip | ImGuiWindowFlags_::ImGuiWindowFlags_ChildMenu;

	for ( ImGuiWindow* window : context->Windows ) {
		if ( !window || ( window->Flags & skip_flags ) )
			continue;

		if ( context->MovingWindow && context->MovingWindow->RootWindow == window )
			continue;

		const ImVec2 size = window->SizeFull;

		if ( size.x <= 0.f || size.y <= 0.f )
			continue;

		const float visible_x = ImMax( ImMin( window->Pos.x + size.x, display.x ) - ImMax( window->Pos.x, 0.f ), 0.f );
		const float visible_y = ImMax( ImMin( window->Pos.y + size.y, display.y ) - ImMax( window->Pos.y, 0.f ), 0.f );

		if ( ( visible_x / size.x ) * ( visible_y / size.y ) >= min_visible_fraction )
			continue;

		const ImVec2 position( ImClamp( window->Pos.x, pad, ImMax( pad, display.x - size.x - pad ) ),
		                       ImClamp( window->Pos.y, pad, ImMax( pad, display.y - size.y - pad ) ) );

		ImGui::SetWindowPos( window, position, ImGuiCond_::ImGuiCond_Always );
	}

	saved.clear( );
	for ( ImGuiWindow* window : context->Windows )
		if ( window && !( window->Flags & skip_flags ) )
			saved.push_back( { window->ID, window->Pos } );

	saved_display   = display;
	restore_pending = false;
}

n_render::impl_t::stretch_block_t n_render::impl_t::begin_stretch_block( ImDrawList* list, const bool panel )
{
	if ( panel )
		g_open_panel_lists.push_back( list );

	return { list, list->VtxBuffer.Size, 0, ImMax( list->CmdBuffer.Size - 1, 0 ), 0, panel };
}

void n_render::impl_t::end_stretch_block( stretch_block_t block )
{
	if ( block.m_panel ) {
		if ( const auto it = std::find( g_open_panel_lists.rbegin( ), g_open_panel_lists.rend( ), block.m_list ); it != g_open_panel_lists.rend( ) )
			g_open_panel_lists.erase( std::next( it ).base( ) );
	}

	block.m_vertex_end  = block.m_list->VtxBuffer.Size;
	block.m_command_end = block.m_list->CmdBuffer.Size;

	if ( block.m_vertex_end > block.m_vertex_begin )
		this->m_stretch_blocks.push_back( block );
}

/* the markers run in draw_cached_data (end_scene thread), so the open block never crosses threads */
static n_render::impl_t::stretch_block_t g_queued_block{ };

void n_render::impl_t::queue_stretch_block_begin( )
{
	const auto begin = []( ImDrawList* list ) { g_queued_block = g_render.begin_stretch_block( list ); };

	this->m_draw_data.emplace_back( e_draw_type::draw_type_callback, std::make_any< callback_draw_object_t >( callback_draw_object_t{ begin } ) );
}

void n_render::impl_t::queue_stretch_block_end( )
{
	const auto end = []( ImDrawList* ) {
		if ( g_queued_block.m_list )
			g_render.end_stretch_block( std::exchange( g_queued_block, { } ) );
	};

	this->m_draw_data.emplace_back( e_draw_type::draw_type_callback, std::make_any< callback_draw_object_t >( callback_draw_object_t{ end } ) );
}

static float stretch_pivot( float lo, float hi, float screen_width )
{
	const float room = screen_width - ( hi - lo );

	return lo + ( hi - lo ) * ( room > 1.f ? ImSaturate( lo / room ) : 0.5f );
}

static void stretch_vertices( ImDrawList* list, int begin, int end, float pivot, float scale )
{
	for ( int v = begin, last = ImMin( end, list->VtxBuffer.Size ); v < last; ++v )
		list->VtxBuffer[ v ].pos.x = pivot + ( list->VtxBuffer[ v ].pos.x - pivot ) * scale;
}

/* clip rects travel with the vertices (screen space, used raw by the backend) or widened panels get cut.
   grow_only: rect also covers draws outside the range (full screen), may widen, never narrow */
static void stretch_clip_rects( ImDrawList* list, int begin, int end, float pivot, float scale, bool grow_only )
{
	for ( int c = begin, last = ImMin( end, list->CmdBuffer.Size ); c < last; ++c ) {
		ImVec4& clip  = list->CmdBuffer[ c ].ClipRect;
		const float x = pivot + ( clip.x - pivot ) * scale;
		const float z = pivot + ( clip.z - pivot ) * scale;

		clip.x = grow_only ? ImMin( clip.x, x ) : x;
		clip.z = grow_only ? ImMax( clip.z, z ) : z;
	}
}

static bool is_stretched_window( const ImGuiWindow* window )
{
	const std::string_view name = window->Name;

	return name.find( "botox-spectators" ) != std::string_view::npos || name == "botox-practice-window-ui" ||
	       name == "botox-route-calc-keys" || name == "##websurf_bare" || name == "##mr_clipper_box";
}

static bool window_unscaled( const ImGuiWindow* root )
{
	return root && g_render.m_dpi_scale != 1.f && g_render.m_dpi_panel_scale == 1.f && is_stretched_window( root );
}

static float unscaled_axis_shown( const float lo, const float size, const float screen, const float scale )
{
	const float room_logical = screen / scale - size, room_screen = screen - size;

	return room_logical > 1.f && room_screen > 0.f ? lo * room_screen / room_logical : lo * scale;
}

static float unscaled_axis_layout( const float shown, const float size, const float screen, const float scale )
{
	const float room_logical = screen / scale - size, room_screen = screen - size;

	return room_logical > 1.f && room_screen > 0.f ? shown * room_logical / room_screen : shown / scale;
}

static ImVec2 unscaled_window_offset( const ImGuiWindow* root, const ImVec2 screen, const float scale )
{
	return ImVec2( unscaled_axis_shown( root->Pos.x, root->Size.x, screen.x, scale ) - root->Pos.x,
	               unscaled_axis_shown( root->Pos.y, root->Size.y, screen.y, scale ) - root->Pos.y );
}

static ImRect window_drawn_rect( const ImGuiWindow* window, ImRect rect, const ImVec2 screen )
{
	const float scale = g_render.m_dpi_scale;

	if ( window_unscaled( window->RootWindow ) ) {
		rect.Translate( unscaled_window_offset( window->RootWindow, screen, scale ) );
		return rect;
	}

	return ImRect( rect.Min.x * scale, rect.Min.y * scale, rect.Max.x * scale, rect.Max.y * scale );
}

float botox_dpi_window_scale( const ImGuiWindow* root )
{
	return window_unscaled( root ) ? 1.f : g_render.m_dpi_scale;
}

/* true = dpi_scale_overlays grows this list's vertices by m_dpi_scale (scaled windows, panel blocks on bg / fg) */
static bool list_dpi_scaled( const ImDrawList* list )
{
	if ( list == ImGui::GetBackgroundDrawList( ) || list == ImGui::GetForegroundDrawList( ) )
		return g_render.m_dpi_panel_scale != 1.f && std::ranges::find( g_open_panel_lists, list ) != g_open_panel_lists.end( );

	const ImGuiContext& g     = *GImGui;
	const ImGuiWindow* window = g.CurrentWindow && g.CurrentWindow->DrawList == list ? g.CurrentWindow : nullptr;

	for ( int i = 0; !window && i < g.Windows.Size; ++i )
		if ( g.Windows[ i ]->DrawList == list )
			window = g.Windows[ i ];

	return window && !window_unscaled( window->RootWindow );
}

/* imgui_draw RenderText / RenderChar: the twin to draw font from on this list, null = the base atlas. a missing
   twin is wanted for the next bake (update_dpi_fonts) */
const ImFont* botox_dpi_twin( const ImFont* font, const ImDrawList* list, const char* text, const char* text_end )
{
	const float scale = g_render.m_dpi_scale;
	if ( scale == 1.f || !font || !GImGui || font->ContainerAtlas != ImGui::GetIO( ).Fonts || !list_dpi_scaled( list ) )
		return nullptr;

	const ImFont* twin = nullptr;

	if ( const dpi_font_set_t* const set = g_dpi_fonts; set && set->m_scale == scale && set->m_atlas->TexID )
		for ( int i = 0; i < set->m_count && !twin; ++i )
			if ( set->m_bases[ i ] == font )
				twin = set->m_fonts[ i ];

	if ( !twin ) {
		if ( g_dpi_wanted.m_scale != scale )
			g_dpi_wanted = { scale };

		const ImFont** const end = g_dpi_wanted.m_bases + g_dpi_wanted.m_count;
		if ( g_dpi_wanted.m_count < k_dpi_twin_max && std::find( g_dpi_wanted.m_bases, end, font ) == end )
			g_dpi_wanted.m_bases[ g_dpi_wanted.m_count++ ] = font;

		return nullptr;
	}

	/* one texture per draw: a glyph only the base has (full tiers) keeps the whole string on the base */
	for ( const char* s = text; s < text_end; ) {
		unsigned int c = static_cast< unsigned char >( *s );
		s += c < 0x80 ? 1 : ImTextCharFromUtf8( &c, s, text_end );

		if ( c >= 32 && !twin->FindGlyphNoFallback( static_cast< ImWchar >( c ) ) && font->FindGlyphNoFallback( static_cast< ImWchar >( c ) ) )
			return nullptr;
	}

	return twin;
}

static ImVec2 dpi_logical_mouse( const ImVec2 mouse, const ImVec2 screen, const float scale )
{
	ImGuiContext& g = *GImGui;

	if ( g.MovingWindow && window_unscaled( g.MovingWindow->RootWindow ) ) {
		const ImVec2 size = g.MovingWindow->RootWindow->Size, k = g.ActiveIdClickOffset;

		return ImVec2( unscaled_axis_layout( mouse.x - k.x, size.x, screen.x, scale ) + k.x,
		               unscaled_axis_layout( mouse.y - k.y, size.y, screen.y, scale ) + k.y );
	}

	const ImGuiWindow* owner = g.MovingWindow ? g.MovingWindow : g.ActiveId ? g.ActiveIdWindow : nullptr;

	for ( int i = g.Windows.Size - 1; !owner && i >= 0; --i ) {
		const ImGuiWindow* window = g.Windows[ i ];
		if ( !window->Active || window->Hidden || ( window->Flags & ImGuiWindowFlags_NoMouseInputs ) )
			continue;

		if ( window_drawn_rect( window, window->OuterRectClipped, screen ).Contains( mouse ) )
			owner = window;
	}

	if ( owner && window_unscaled( owner->RootWindow ) ) {
		const ImVec2 offset = unscaled_window_offset( owner->RootWindow, screen, scale );

		return ImVec2( mouse.x - offset.x, mouse.y - offset.y );
	}

	return ImVec2( mouse.x / scale, mouse.y / scale );
}

static bool overlay_stretch( float& scale, float& center_x )
{
	if ( !GET_VARIABLE( g_variables.m_aspect_ratio_enable, bool ) || !GET_VARIABLE( g_variables.m_aspect_ratio_cheat_overlay, bool ) )
		return false;

	scale = g_resolution_spoof.aspect_scale( );
	if ( fabsf( scale - 1.f ) < 0.001f )
		return false;

	center_x = ( g_resolution_spoof.m_screen_width > 0 ? static_cast< float >( g_resolution_spoof.m_screen_width ) : g_ctx.m_width ) * 0.5f;

	return center_x > 0.f;
}

float n_render::impl_t::to_overlay_x( float screen_x )
{
	float scale, center_x;

	return overlay_stretch( scale, center_x ) ? center_x + ( screen_x - center_x ) / scale : screen_x;
}

static void stretch_overlays( )
{
	float scale, center_x;
	if ( !overlay_stretch( scale, center_x ) )
		return;

	const float screen_width = center_x * 2.f;

	for ( const auto& block : g_render.m_stretch_blocks ) {
		float lo = FLT_MAX, hi = -FLT_MAX;

		for ( int v = block.m_vertex_begin, last = ImMin( block.m_vertex_end, block.m_list->VtxBuffer.Size ); v < last; ++v ) {
			lo = ImMin( lo, block.m_list->VtxBuffer[ v ].pos.x );
			hi = ImMax( hi, block.m_list->VtxBuffer[ v ].pos.x );
		}

		if ( lo > hi )
			continue;

		const float pivot = stretch_pivot( lo, hi, screen_width );

		stretch_vertices( block.m_list, block.m_vertex_begin, block.m_vertex_end, pivot, scale );
		stretch_clip_rects( block.m_list, block.m_command_begin, block.m_command_end, pivot, scale, true );
	}

	for ( ImGuiWindow* window : GImGui->Windows ) {
		if ( !window->Active || window->Hidden || !window->DrawList || !is_stretched_window( window->RootWindow ) )
			continue;

		const ImGuiWindow* root = window->RootWindow;
		const ImRect drawn      = window_drawn_rect( root, root->Rect( ), ImVec2( g_ctx.m_width, g_ctx.m_height ) );
		const float pivot       = stretch_pivot( drawn.Min.x, drawn.Max.x, screen_width );

		stretch_vertices( window->DrawList, 0, INT_MAX, pivot, scale );
		stretch_clip_rects( window->DrawList, 0, INT_MAX, pivot, scale, false );
	}

	ImDrawList* background = ImGui::GetBackgroundDrawList( );

	for ( int v = 0; v < background->VtxBuffer.Size; ++v ) {
		if ( std::ranges::any_of( g_render.m_stretch_blocks, [ & ]( const auto& block ) {
				 return block.m_list == background && v >= block.m_vertex_begin && v < block.m_vertex_end;
			 } ) )
			continue;

		background->VtxBuffer[ v ].pos.x = center_x + ( background->VtxBuffer[ v ].pos.x - center_x ) * scale;
	}

	stretch_clip_rects( background, 0, INT_MAX, center_x, scale, true );
}

static float wanted_dpi_scale( )
{
	float percent = GET_VARIABLE( g_variables.m_dpi_scale, float );
	if ( !std::isfinite( percent ) )
		percent = 100.f;

	const float scale = ImClamp( percent, 25.f, 200.f ) / 100.f;

	return fabsf( scale - 1.f ) < 0.001f ? 1.f : scale;
}

/* dpi pivot per axis from the block's centre: outer 20 % of the screen = that edge, so corner-stacked panels grow as one
   and never overlap; middle 60 % slides edge to edge. slope 1 / 0.6 < 2 keeps the shown position monotonic up to 200 % */
constexpr float dpi_edge_zone = 0.2f;

static float dpi_pivot( const float centre, const float screen )
{
	if ( screen <= 0.f )
		return 0.f;

	const float edge_lo = screen * dpi_edge_zone, edge_hi = screen * ( 1.f - dpi_edge_zone );

	return ImClamp( ( centre - edge_lo ) / ( edge_hi - edge_lo ), 0.f, 1.f ) * screen;
}

static float dpi_axis_shown( const float lo, const float size, const float screen, const float scale )
{
	const float pivot = dpi_pivot( lo + size * 0.5f, screen );

	return pivot + ( lo - pivot ) * scale;
}

static float dpi_axis_layout( const float shown, const float size, const float screen, const float scale )
{
	const float half = size * 0.5f, edge_lo = screen * dpi_edge_zone, edge_hi = screen * ( 1.f - dpi_edge_zone );

	if ( shown <= dpi_axis_shown( edge_lo - half, size, screen, scale ) )
		return shown / scale;

	if ( shown >= dpi_axis_shown( edge_hi - half, size, screen, scale ) )
		return screen + ( shown - screen ) / scale;

	const float slope = screen / ( edge_hi - edge_lo );

	return ( shown - slope * ( half - edge_lo ) * ( 1.f - scale ) ) / ( scale + slope * ( 1.f - scale ) );
}

ImVec2 n_render::impl_t::dpi_panel_pos( const ImVec2 pos, const ImVec2 size )
{
	if ( this->m_dpi_panel_scale == 1.f )
		return pos;

	return ImVec2( dpi_axis_shown( pos.x, size.x, g_ctx.m_width, this->m_dpi_panel_scale ),
	               dpi_axis_shown( pos.y, size.y, g_ctx.m_height, this->m_dpi_panel_scale ) );
}

ImVec2 n_render::impl_t::dpi_panel_layout_pos( const ImVec2 shown, const ImVec2 size )
{
	if ( this->m_dpi_panel_scale == 1.f )
		return shown;

	return ImVec2( dpi_axis_layout( shown.x, size.x, g_ctx.m_width, this->m_dpi_panel_scale ),
	               dpi_axis_layout( shown.y, size.y, g_ctx.m_height, this->m_dpi_panel_scale ) );
}

ImVec2 n_render::impl_t::screen_mouse( )
{
	return ImVec2( this->m_screen_mouse_x, this->m_screen_mouse_y );
}

static void dpi_scale_overlays( ImDrawData* draw_data, const ImVec2 screen )
{
	const float scale = g_render.m_dpi_scale;
	if ( scale == 1.f )
		return;

	ImDrawList* const background = ImGui::GetBackgroundDrawList( );
	ImDrawList* const foreground = ImGui::GetForegroundDrawList( );

	static ImVector< ImDrawList* > shifted;
	static ImVector< ImVec2 > offsets;
	shifted.resize( 0 );
	offsets.resize( 0 );

	for ( const ImGuiWindow* window : GImGui->Windows ) {
		if ( !window->Active || !window->DrawList || !window_unscaled( window->RootWindow ) )
			continue;

		shifted.push_back( window->DrawList );
		offsets.push_back( unscaled_window_offset( window->RootWindow, screen, scale ) );
	}

	for ( int i = 0; i < draw_data->CmdListsCount; ++i ) {
		ImDrawList* const list = draw_data->CmdLists[ i ];
		if ( list == background || list == foreground )
			continue;

		if ( ImDrawList* const* const found = shifted.find( list ); found != shifted.end( ) ) {
			const ImVec2 offset = offsets[ static_cast< int >( found - shifted.begin( ) ) ];

			for ( ImDrawVert& vertex : list->VtxBuffer ) {
				vertex.pos.x += offset.x;
				vertex.pos.y += offset.y;
			}

			for ( ImDrawCmd& command : list->CmdBuffer ) {
				command.ClipRect.x += offset.x;
				command.ClipRect.y += offset.y;
				command.ClipRect.z += offset.x;
				command.ClipRect.w += offset.y;
			}

			continue;
		}

		for ( ImDrawVert& vertex : list->VtxBuffer ) {
			vertex.pos.x *= scale;
			vertex.pos.y *= scale;
		}

		for ( ImDrawCmd& command : list->CmdBuffer ) {
			command.ClipRect.x *= scale;
			command.ClipRect.y *= scale;
			command.ClipRect.z *= scale;
			command.ClipRect.w *= scale;
		}
	}

	for ( const auto& block : g_render.m_stretch_blocks ) {
		if ( g_render.m_dpi_panel_scale == 1.f || !block.m_panel || ( block.m_list != background && block.m_list != foreground ) )
			continue;

		ImVector< ImDrawVert >& vertices = block.m_list->VtxBuffer;
		const int last                   = ImMin( block.m_vertex_end, vertices.Size );

		ImVec2 lo( FLT_MAX, FLT_MAX ), hi( -FLT_MAX, -FLT_MAX );

		for ( int v = block.m_vertex_begin; v < last; ++v ) {
			lo = ImMin( lo, vertices[ v ].pos );
			hi = ImMax( hi, vertices[ v ].pos );
		}

		if ( lo.x > hi.x )
			continue;

		/* pivot x ( 1 - scale ) whole: twin glyphs snapped to whole scaled px land on whole screen px ( <= 0.5 px shift ) */
		const auto snap = [ scale ]( const float pivot ) { return ImFloorSigned( pivot * ( 1.f - scale ) + 0.5f ) / ( 1.f - scale ); };
		const ImVec2 pivot( snap( dpi_pivot( ( lo.x + hi.x ) * 0.5f, screen.x ) ), snap( dpi_pivot( ( lo.y + hi.y ) * 0.5f, screen.y ) ) );

		for ( int v = block.m_vertex_begin; v < last; ++v ) {
			ImVec2& pos = vertices[ v ].pos;
			pos         = ImVec2( pivot.x + ( pos.x - pivot.x ) * scale, pivot.y + ( pos.y - pivot.y ) * scale );
		}

		for ( int c = block.m_command_begin, end = ImMin( block.m_command_end, block.m_list->CmdBuffer.Size ); c < end; ++c ) {
			ImVec4& clip = block.m_list->CmdBuffer[ c ].ClipRect;

			clip.x = ImMin( clip.x, pivot.x + ( clip.x - pivot.x ) * scale );
			clip.y = ImMin( clip.y, pivot.y + ( clip.y - pivot.y ) * scale );
			clip.z = ImMax( clip.z, pivot.x + ( clip.z - pivot.x ) * scale );
			clip.w = ImMax( clip.w, pivot.y + ( clip.w - pivot.y ) * scale );
		}
	}

	draw_data->DisplaySize = screen;
}

static void render_lists( const ImDrawData* source, ImVector< ImDrawList* >& lists, float fade )
{
	if ( lists.empty( ) )
		return;

	ImDrawData part    = *source;
	part.CmdLists      = lists.Data;
	part.CmdListsCount = lists.Size;
	part.TotalVtxCount = 0;
	part.TotalIdxCount = 0;

	for ( const ImDrawList* list : lists ) {
		part.TotalVtxCount += list->VtxBuffer.Size;
		part.TotalIdxCount += list->IdxBuffer.Size;
	}

	if ( fade < 1.f )
		ImGui_ImplDX9_RenderDrawDataFaded( &part, fade );
	else
		ImGui_ImplDX9_RenderDrawData( &part );
}

void n_render::impl_t::on_end_scene( const std::function< void( ) >& function, IDirect3DDevice9* device )
{
	if ( !this->m_initialised ) {
		ImGui::CreateContext( );

		static const std::string ini_path = ( g_config.m_path / ( "imgui.ini" ) ).string( );

		ImGui::GetIO( ).IniFilename = ini_path.c_str( );

		ImGui_ImplWin32_Init( g_input.m_window );
		ImGui_ImplDX9_Init( device );

		if ( !g_render.m_terrorist_avatar )
			D3DXCreateTextureFromFileInMemory( device, &terrorist_avatar_data, sizeof( terrorist_avatar_data ), &this->m_terrorist_avatar );

		if ( !g_render.m_counter_terrorist_avatar )
			D3DXCreateTextureFromFileInMemory( device, &counter_terrorist_avatar_data, sizeof( counter_terrorist_avatar_data ),
			                                   &this->m_counter_terrorist_avatar );

		auto& style = ImGui::GetStyle( );

		[ & ]( ) {
			style.WindowRounding    = 5.f;
			style.ChildRounding     = 0.f;
			style.FrameRounding     = 2.f;
			style.GrabRounding      = 3.f;
			style.PopupRounding     = 3.f;
			style.ScrollbarRounding = 0.f;

			style.FrameBorderSize  = 0.f;
			style.WindowBorderSize = 0.f;
			style.PopupBorderSize  = 0.f;
			style.ScrollbarSize    = 8.f;
			style.GrabMinSize      = 0.f;

			style.WindowPadding   = ImVec2( 8, 8 );
			style.FramePadding    = ImVec2( 0, 0 );
			style.ButtonTextAlign = ImVec2( 0.5f, 0.5f );
			style.ItemSpacing     = ImVec2( 8, 8 );

			style.AntiAliasedFill        = true;
			style.AntiAliasedLines       = true;
			style.AntiAliasedLinesUseTex = true;
		}( );

		[ & ]( ) {
			style.Colors[ ImGuiCol_::ImGuiCol_WindowBg ]  = ImVec4( 10 / 255.f, 10 / 255.f, 10 / 255.f, 1.f );
			style.Colors[ ImGuiCol_::ImGuiCol_ChildBg ]   = ImVec4( 15 / 255.f, 15 / 255.f, 15 / 255.f, 1.f );
			style.Colors[ ImGuiCol_::ImGuiCol_FrameBg ]   = ImVec4( 25 / 255.f, 25 / 255.f, 25 / 255.f, 1.f );
			style.Colors[ ImGuiCol_::ImGuiCol_PopupBg ]   = ImVec4( 20 / 255.f, 20 / 255.f, 20 / 255.f, 1.f );
			style.Colors[ ImGuiCol_::ImGuiCol_CheckMark ] = ImVec4( 0 / 255.f, 0 / 255.f, 0 / 255.f, 1.f );
			style.Colors[ ImGuiCol_::ImGuiCol_Button ]    = ImVec4( 20 / 255.f, 20 / 255.f, 20 / 255.f, 1.f );

			style.Colors[ ImGuiCol_::ImGuiCol_Border ]       = ImVec4( 0 / 255.f, 0 / 255.f, 0 / 255.f, 0.f );
			style.Colors[ ImGuiCol_::ImGuiCol_BorderShadow ] = ImVec4( 0 / 255.f, 0 / 255.f, 0 / 255.f, 0.f );
		}( );

		/* same build path as reload, but on the render thread: NewFrame needs a built atlas */
		font_set_t* const set = new font_set_t{ };
		if ( !build_fonts( *set, current_font_request( ) ) ) {
			const std::string reason = g_ft_build_fail;
			const unsigned free_mb   = largest_free_block_mb( );

			IM_DELETE( set->m_atlas );
			set->m_atlas           = IM_NEW( ImFontAtlas )( );
			ImFont* const fallback = set->m_atlas->AddFontDefault( );
			const bool fallback_ok = ImGuiFreeType::BuildFontAtlas( set->m_atlas, 0x0 );

			const std::string fallback_reason = fallback_ok ? "ok" : g_ft_build_fail;
			g_console.print< n_console::log_level::WARNING >(
				std::vformat( "font atlas build failed: {} | largest free block {} mb | fallback: {}",
				              std::make_format_args( reason, free_mb, fallback_reason ) )
					.c_str( ) );

			std::ranges::fill( set->m_fonts, fallback );
			std::ranges::fill( set->m_custom_fonts, fallback );
		}
		this->install_fonts( set );

		this->m_reload_fonts = false;
		this->m_initialised  = true;
	}

	/* swap a finished worker atlas in before NewFrame: imgui must never see it change mid-frame */
	if ( font_set_t* const set = this->m_pending_fonts.exchange( nullptr ) )
		this->install_fonts( set );

	/* reload starts a worker, never builds here. pressed mid-build: flag stays up, next build starts after */
	if ( this->m_reload_fonts && !this->m_font_build_running ) {
		this->m_font_build_running = true;
		this->m_reload_fonts       = false;

		std::thread( build_fonts_worker, current_font_request( ) ).detach( );
	}

	this->update_indicator_fonts( device );
	update_dpi_fonts( device );

	if ( const ImFontAtlas* const live = ImGui::GetIO( ).Fonts; live->Fonts.empty( ) || !live->Fonts[ 0 ]->ContainerAtlas ) {
		static bool said = false;
		if ( !std::exchange( said, true ) )
			g_console.print< n_console::log_level::WARNING >( "no built font, overlay off (see font atlas line above)" );
		return;
	}

	ImGui_ImplDX9_NewFrame( );
	ImGui_ImplWin32_NewFrame( );

	g_resolution_spoof.on_imgui_new_frame( );

	ImGuiIO& io         = ImGui::GetIO( );
	const ImVec2 screen = io.DisplaySize;

	this->m_dpi_scale       = wanted_dpi_scale( );
	this->m_dpi_panel_scale = GET_VARIABLE( g_variables.m_dpi_scale_panels, bool ) ? this->m_dpi_scale : 1.f;

	io.DisplaySize = ImVec2( screen.x / this->m_dpi_scale, screen.y / this->m_dpi_scale );
	io.DisplayFramebufferScale = ImVec2( this->m_dpi_scale, this->m_dpi_scale );

	/* curves tessellate in logical px: finer when they draw bigger, so rounded corners stay round on screen */
	ImGui::GetStyle( ).CircleTessellationMaxError = 0.30f / ImMax( this->m_dpi_scale, 1.f );

	this->m_screen_mouse_x = io.MousePos.x;
	this->m_screen_mouse_y = io.MousePos.y;

	if ( this->m_dpi_scale != 1.f && ImGui::IsMousePosValid( &io.MousePos ) )
		io.MousePos = dpi_logical_mouse( io.MousePos, screen, this->m_dpi_scale );

	ImGui::NewFrame( );

	g_ctx.m_width  = screen.x;
	g_ctx.m_height = screen.y;

	if ( this->m_dpi_scale != 1.f ) {
		for ( ImDrawList* list : { ImGui::GetBackgroundDrawList( ), ImGui::GetForegroundDrawList( ) } )
			list->PushClipRect( ImVec2( 0.f, 0.f ), ImMax( screen, io.DisplaySize ), false );
	}

	keep_windows_on_screen( );

	this->m_stretch_blocks.clear( );
	g_open_panel_lists.clear( );

	/* steam avatars: fetch + free before any draw list takes a pointer, else a list holds a freed texture ( double free, c0000374 ) */
	{
		PERF_ZONE( zone_avatars );
		g_avatar_cache.think( );
	}

	this->draw_cached_data( );

	function( );

	ImGui::EndFrame( );
	ImGui::Render( );

	ImDrawData* draw_data = ImGui::GetDrawData( );

	dpi_scale_overlays( draw_data, screen );
	stretch_overlays( );

	static ImVector< ImDrawList* > owned, before, menu, after;

	const float fade = g_menu.fade_draw_lists( owned );

	if ( owned.empty( ) ) {
		ImGui_ImplDX9_RenderDrawData( draw_data );
		return;
	}

	before.resize( 0 );
	menu.resize( 0 );
	after.resize( 0 );

	for ( int i = 0; i < draw_data->CmdListsCount; ++i ) {
		ImDrawList* list = draw_data->CmdLists[ i ];

		if ( owned.contains( list ) )
			menu.push_back( list );
		else
			( menu.empty( ) ? before : after ).push_back( list );
	}

	render_lists( draw_data, before, 1.f );
	render_lists( draw_data, menu, fade );
	render_lists( draw_data, after, 1.f );
}

void n_render::impl_t::on_release( )
{
	this->clear_draw_data( );

	/* team avatar textures: the device outlives the module, release or every re-inject leaks a pair */
	for ( IDirect3DTexture9** texture : { &this->m_terrorist_avatar, &this->m_counter_terrorist_avatar } ) {
		if ( *texture ) {
			( *texture )->Release( );
			*texture = nullptr;
		}
	}

	this->m_initialised = false;

	for ( int i = 0; i < 1000 && this->m_font_build_running; ++i )
		std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );

	discard_font_set( this->m_pending_fonts.exchange( nullptr ) );
	discard_indicator_set( this->m_pending_indicator_fonts.exchange( nullptr ) );
	discard_indicator_set( std::exchange( this->m_indicator_fonts, nullptr ) );
	discard_dpi_set( g_pending_dpi_fonts.exchange( nullptr ) );
	discard_dpi_set( std::exchange( g_dpi_fonts, nullptr ) );
	g_dpi_wanted = g_dpi_seen = g_dpi_baked = { };

	ImGui_ImplDX9_Shutdown( );
	ImGui_ImplWin32_Shutdown( );
	ImGui::DestroyContext( );
}

void n_render::impl_t::draw_cached_data( )
{
	const auto draw_list = ImGui::GetBackgroundDrawList( );

	std::unique_lock< std::shared_mutex > lock( this->m_mutex );

	if ( this->m_thread_safe_draw_data.empty( ) )
		return;

	for ( const draw_object_t& data : this->m_thread_safe_draw_data ) {
		switch ( data.m_type ) {
		case e_draw_type::draw_type_text: {
			const auto* obj = std::any_cast< text_draw_object_t >( &data.m_obj );
			if ( !obj )
				break;

			this->text( draw_list, obj->m_font, obj->m_position, obj->m_text, obj->m_color, obj->m_outline_color, obj->m_draw_flags,
			            obj->m_font_size );
			break;
		}
		case e_draw_type::draw_type_line: {
			const auto* obj = std::any_cast< line_draw_object_t >( &data.m_obj );
			if ( !obj )
				break;

			draw_list->AddLine( ImVec2( obj->m_start.m_x, obj->m_start.m_y ), ImVec2( obj->m_end.m_x, obj->m_end.m_y ), obj->m_color,
			                    obj->m_thickness );
			break;
		}
		case e_draw_type::draw_type_rect: {
			const auto* obj = std::any_cast< rect_draw_object_t >( &data.m_obj );
			if ( !obj )
				break;

			this->rect( draw_list, obj->m_min, obj->m_max, obj->m_color, obj->m_outline_color, obj->m_filled, obj->m_rounding,
			            obj->m_corner_rounding_flags, obj->m_thickness, obj->m_outline_flags, obj->m_outline_thickness );
			break;
		}
		case e_draw_type::draw_type_gradient_rect: {
			const auto* obj = std::any_cast< gradient_rect_draw_object_t >( &data.m_obj );
			if ( !obj )
				break;

			if ( obj->m_rounding <= 0.f ) {
				draw_list->AddRectFilledMultiColor( ImVec2( obj->m_min.m_x, obj->m_min.m_y ), ImVec2( obj->m_max.m_x, obj->m_max.m_y ),
				                                    obj->m_top_color, obj->m_top_color, obj->m_bottom_color, obj->m_bottom_color );
				break;
			}

			const int first_vertex = draw_list->VtxBuffer.Size;
			draw_list->AddRectFilled( ImVec2( obj->m_min.m_x, obj->m_min.m_y ), ImVec2( obj->m_max.m_x, obj->m_max.m_y ), IM_COL32_WHITE,
			                          obj->m_rounding );

			const float height = std::max( obj->m_max.m_y - obj->m_min.m_y, 1.f );
			const ImVec4 top   = ImGui::ColorConvertU32ToFloat4( obj->m_top_color );
			const ImVec4 bottom = ImGui::ColorConvertU32ToFloat4( obj->m_bottom_color );

			for ( int i = first_vertex; i < draw_list->VtxBuffer.Size; ++i ) {
				ImDrawVert& vertex = draw_list->VtxBuffer[ i ];
				const float t      = std::clamp( ( vertex.pos.y - obj->m_min.m_y ) / height, 0.f, 1.f );
				const float coverage = static_cast< float >( ( vertex.col >> IM_COL32_A_SHIFT ) & 0xFF ) / 255.f;

				vertex.col = ImGui::ColorConvertFloat4ToU32( ImVec4( top.x + ( bottom.x - top.x ) * t, top.y + ( bottom.y - top.y ) * t,
				                                                     top.z + ( bottom.z - top.z ) * t, ( top.w + ( bottom.w - top.w ) * t ) * coverage ) );
			}
			break;
		}
		case e_draw_type::draw_type_texture: {
			const auto* obj = std::any_cast< texture_draw_object_t >( &data.m_obj );
			if ( !obj )
				break;

			/* queued on the game thread, replayed until its next swap: the avatar may be freed since. live_font for textures */
			if ( obj->m_texture_id != this->m_terrorist_avatar && obj->m_texture_id != this->m_counter_terrorist_avatar &&
			     !g_avatar_cache.live( obj->m_texture_id ) )
				break;

			draw_list->AddImageRounded( obj->m_texture_id, ImVec2( obj->m_position.m_x, obj->m_position.m_y ),
			                            ImVec2( obj->m_position.m_x + obj->m_size.m_x, obj->m_position.m_y + obj->m_size.m_y ), ImVec2( 0, 0 ),
			                            ImVec2( 1, 1 ), obj->m_color, obj->m_rounding, obj->m_draw_flags );
			break;
		}
		case e_draw_type::draw_type_circle: {
			const auto* obj = std::any_cast< circle_draw_object_t >( &data.m_obj );
			if ( !obj )
				break;

			draw_list->AddCircle( ImVec2( obj->m_center.m_x, obj->m_center.m_y ), obj->m_radius, obj->m_color, obj->m_segments, obj->m_thickness );
			break;
		}
		case e_draw_type::draw_type_filled_circle: {
			const auto* obj = std::any_cast< filled_circle_draw_object_t >( &data.m_obj );
			if ( !obj )
				break;

			draw_list->AddCircleFilled( ImVec2( obj->m_center.m_x, obj->m_center.m_y ), obj->m_radius, obj->m_color, obj->m_segments );
			break;
		}
		case e_draw_type::draw_type_poly: {
			const auto* obj = std::any_cast< poly_draw_object_t >( &data.m_obj );
			if ( !obj || obj->m_count < 3 || obj->m_count > 32 )
				break;

			ImVec2 points[ 32 ];
			for ( int i = 0; i < obj->m_count; ++i )
				points[ i ] = ImVec2( obj->m_points[ i ].m_x, obj->m_points[ i ].m_y );

			draw_list->AddConvexPolyFilled( points, obj->m_count, obj->m_color );

			if ( obj->m_outline )
				draw_list->AddPolyline( points, obj->m_count, obj->m_outline_color, ImDrawFlags_::ImDrawFlags_Closed, obj->m_thickness );
			break;
		}
		case e_draw_type::draw_type_corner_rect: {
			const auto* obj = std::any_cast< corner_rect_draw_object_t >( &data.m_obj );
			if ( !obj )
				break;

			const float l = obj->m_min.m_x, t = obj->m_min.m_y, r = obj->m_max.m_x, b = obj->m_max.m_y;
			const float ax = obj->m_arm_x, ay = obj->m_arm_y, radius = obj->m_rounding;

			const auto corner = [ & ]( const ImVec2& from, const ImVec2& centre, const int a_min, const int a_max, const ImVec2& to ) {
				draw_list->PathLineTo( from );
				draw_list->PathArcToFast( centre, radius, a_min, a_max );
				draw_list->PathLineTo( to );
				draw_list->PathStroke( obj->m_color, 0, obj->m_thickness );
			};

			corner( ImVec2( l, t + ay ), ImVec2( l + radius, t + radius ), 6, 9, ImVec2( l + ax, t ) );
			corner( ImVec2( r - ax, t ), ImVec2( r - radius, t + radius ), 9, 12, ImVec2( r, t + ay ) );
			corner( ImVec2( r, b - ay ), ImVec2( r - radius, b - radius ), 0, 3, ImVec2( r - ax, b ) );
			corner( ImVec2( l + ax, b ), ImVec2( l + radius, b - radius ), 3, 6, ImVec2( l, b - ay ) );
			break;
		}
		case e_draw_type::draw_type_triangle: {
			const auto* obj = std::any_cast< triangle_draw_object_t >( &data.m_obj );
			if ( !obj )
				break;

			if ( obj->m_draw_flags & e_triangle_flags::triangle_flag_filled )
				draw_list->AddTriangleFilled( ImVec2( obj->m_first.m_x, obj->m_first.m_y ), ImVec2( obj->m_second.m_x, obj->m_second.m_y ),
				                              ImVec2( obj->m_third.m_x, obj->m_third.m_y ), obj->m_color );
			else
				draw_list->AddTriangle( ImVec2( obj->m_first.m_x, obj->m_first.m_y ), ImVec2( obj->m_second.m_x, obj->m_second.m_y ),
				                        ImVec2( obj->m_third.m_x, obj->m_third.m_y ), obj->m_color, obj->m_thickness );

			if ( obj->m_draw_flags & e_triangle_flags::triangle_flag_outline )
				draw_list->AddTriangle( ImVec2( obj->m_first.m_x, obj->m_first.m_y ), ImVec2( obj->m_second.m_x, obj->m_second.m_y ),
				                        ImVec2( obj->m_third.m_x, obj->m_third.m_y ), obj->m_outline_color, obj->m_thickness + 1.0f );
			break;
		}
		case e_draw_type::draw_type_callback: {
			const auto* obj = std::any_cast< callback_draw_object_t >( &data.m_obj );
			if ( !obj || !obj->m_callback )
				break;

			obj->m_callback( draw_list );
			break;
		}
		default:
			break;
		}
	}
}

static view_matrix_t g_cached_view_matrix{ };

void n_render::impl_t::cache_view_matrix( )
{
	g_cached_view_matrix = g_interfaces.m_engine_client->get_world_to_screen_matrix( );
}

bool n_render::impl_t::world_to_screen( const c_vector& origin, c_vector_2d& screen )
{
	const auto& world_to_screen_matrix = g_cached_view_matrix;

	const float width = world_to_screen_matrix[ 3 ][ 0 ] * origin.m_x + world_to_screen_matrix[ 3 ][ 1 ] * origin.m_y +
	                    world_to_screen_matrix[ 3 ][ 2 ] * origin.m_z + world_to_screen_matrix[ 3 ][ 3 ];

	if ( width < 0.001f )
		return false;

	const float inverse = 1.0f / width;
	screen.m_x          = ( world_to_screen_matrix[ 0 ][ 0 ] * origin.m_x + world_to_screen_matrix[ 0 ][ 1 ] * origin.m_y +
                   world_to_screen_matrix[ 0 ][ 2 ] * origin.m_z + world_to_screen_matrix[ 0 ][ 3 ] ) *
	             inverse;
	screen.m_y = ( world_to_screen_matrix[ 1 ][ 0 ] * origin.m_x + world_to_screen_matrix[ 1 ][ 1 ] * origin.m_y +
	               world_to_screen_matrix[ 1 ][ 2 ] * origin.m_z + world_to_screen_matrix[ 1 ][ 3 ] ) *
	             inverse;

	screen.m_x = this->to_overlay_x( ( g_ctx.m_width * 0.5f ) + ( screen.m_x * g_ctx.m_width ) * 0.5f );
	screen.m_y = ( g_ctx.m_height * 0.5f ) - ( screen.m_y * g_ctx.m_height ) * 0.5f;
	return true;
}

void n_render::impl_t::text( ImDrawList* draw_list, ImFont* font, const c_vector_2d& position, const std::string& text, const unsigned int& color,
                             const unsigned int& outline_color, e_text_flags draw_flags, float font_size )
{
	font = this->live_font( font );

	if ( !font )
		return;

	if ( font_size <= 0.f )
		font_size = font->FontSize;

	draw_list->PushTextureID( font->ContainerAtlas->TexID );

	if ( draw_flags & e_text_flags::text_flag_dropshadow )
		draw_list->AddText( font, font_size, ImVec2( position.m_x + 1.f, position.m_y + 1.f ), outline_color, text.c_str( ) );
	else if ( draw_flags & e_text_flags::text_flag_outline ) {
		draw_list->AddText( font, font_size, ImVec2( position.m_x + 1.f, position.m_y - 1.f ), outline_color, text.c_str( ) );
		draw_list->AddText( font, font_size, ImVec2( position.m_x - 1.f, position.m_y + 1.f ), outline_color, text.c_str( ) );
	}

	draw_list->AddText( font, font_size, ImVec2( position.m_x, position.m_y ), color, text.c_str( ) );

	draw_list->PopTextureID( );
}

void n_render::impl_t::rect( ImDrawList* draw_list, const c_vector_2d& min, const c_vector_2d& max, const unsigned int& color,
                             const unsigned int& outline_color, bool filled, float rounding, int corner_rounding_flags, float thickness,
                             unsigned int outline_flags, float outline_thickness )
{
	const float inset = ( thickness - 1.f ) * 0.5f;

	if ( filled )
		draw_list->AddRectFilled( ImVec2( min.m_x, min.m_y ), ImVec2( max.m_x, max.m_y ), color, rounding, corner_rounding_flags );
	else
		draw_list->AddRect( ImVec2( min.m_x + inset, min.m_y + inset ), ImVec2( max.m_x - inset, max.m_y - inset ), color,
		                    std::max( rounding - inset, 0.f ), corner_rounding_flags, thickness );

	const float offset = 0.5f + outline_thickness * 0.5f, inner_offset = offset + 2.f * inset;

	if ( outline_flags & e_rect_flags::rect_flag_inner_outline )
		draw_list->AddRect( ImVec2( min.m_x + inner_offset, min.m_y + inner_offset ), ImVec2( max.m_x - inner_offset, max.m_y - inner_offset ),
		                    outline_color, std::max( rounding - inner_offset, 0.f ), corner_rounding_flags, outline_thickness );

	if ( outline_flags & e_rect_flags::rect_flag_outer_outline )
		draw_list->AddRect( ImVec2( min.m_x - offset, min.m_y - offset ), ImVec2( max.m_x + offset, max.m_y + offset ), outline_color,
		                    rounding > 0.f ? rounding + offset : 0.f, corner_rounding_flags, outline_thickness );
}

void n_render::impl_t::corner_rect( float x1, float y1, float x2, float y2, const unsigned int& color, float thickness, float rounding )
{
	int w = x2 - x1;
	int h = y2 - y1;

	int iw = w / 3;
	int ih = h / 5;

	corner_rect_draw_object_t corners{ };
	corners.m_min       = c_vector_2d( x1 + 0.5f, y1 + 0.5f );
	corners.m_max       = c_vector_2d( x2 - 0.5f, y2 + 0.5f );
	corners.m_arm_x     = static_cast< float >( iw );
	corners.m_arm_y     = static_cast< float >( ih );
	corners.m_rounding  = std::min( std::max( rounding, 0.f ), static_cast< float >( std::min( iw, ih ) ) );
	corners.m_color     = color;
	corners.m_thickness = thickness;

	this->m_draw_data.emplace_back( e_draw_type::draw_type_corner_rect, std::make_any< corner_rect_draw_object_t >( corners ) );
}

void n_render::impl_t::copy_and_convert( const uint8_t* rgba_data, uint8_t* out, const size_t size )
{
	auto in     = reinterpret_cast< const uint32_t* >( rgba_data );
	auto buffer = reinterpret_cast< uint32_t* >( out );
	for ( auto i = 0u; i < ( size / 4 ); ++i ) {
		const auto pixel = *in++;
		*buffer++        = ( pixel & 0xFF00FF00 ) | ( ( pixel & 0xFF0000 ) >> 16 ) | ( ( pixel & 0xFF ) << 16 );
	}
}

IDirect3DTexture9* n_render::impl_t::steam_image( CSteamID steam_id )
{
	IDirect3DTexture9* created_texture = { };

	int image_index = SteamFriends->GetSmallFriendAvatar( steam_id );
	if ( image_index <= 0 )
		return nullptr;

	unsigned int avatar_width = { }, avatar_height = { };

	if ( !SteamUtils->GetImageSize( image_index, &avatar_width, &avatar_height ) )
		return nullptr;

	const int image_size_in_bytes = avatar_width * avatar_height * 4;
	unsigned char* avatar_rgba    = new unsigned char[ image_size_in_bytes ];

	if ( !SteamUtils->GetImageRGBA( image_index, avatar_rgba, image_size_in_bytes ) ) {
		delete[] avatar_rgba;
		return nullptr;
	}

	long result =
		g_interfaces.m_direct_device->CreateTexture( avatar_width, avatar_height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &created_texture, nullptr );

	bool dynamic_pool = false;

	if ( FAILED( result ) || !created_texture ) {
		dynamic_pool = true;

		result = g_interfaces.m_direct_device->CreateTexture( avatar_width, avatar_height, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
		                                                      &created_texture, nullptr );
	}

	if ( FAILED( result ) || !created_texture ) {
		g_console.print( std::format( "[avatar] CreateTexture failed both pools, hr {:#x}", static_cast< unsigned long >( result ) ).c_str( ) );

		delete[] avatar_rgba;

		return nullptr;
	}

	D3DLOCKED_RECT locked_rect = { };

	result = created_texture->LockRect( 0, &locked_rect, nullptr, dynamic_pool ? D3DLOCK_DISCARD : 0 );

	if ( FAILED( result ) || !locked_rect.pBits ) {
		created_texture->Release( );
		delete[] avatar_rgba;

		return nullptr;
	}

	auto src = avatar_rgba;
	auto dst = reinterpret_cast< unsigned char* >( locked_rect.pBits );

	for ( auto y = 0u; y < avatar_height; ++y ) {
		this->copy_and_convert( src, dst, avatar_width * 4U );

		src += avatar_width * 4;
		dst += locked_rect.Pitch;
	}

	result = created_texture->UnlockRect( 0 );
	delete[] avatar_rgba;

	return created_texture;
}
