#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../../game/sdk/includes/includes.h"
#include "../../globals/config/variables.h"
#include "../../globals/interfaces/interfaces.h"
#include "../../utilities/perf/perf_watch.h"

namespace n_chams
{
	enum e_chams_material : int {
		chams_material_original = 0,
		chams_material_flat,
		chams_material_shaded,
		chams_material_wireframe,
		chams_material_glow,
		chams_material_metallic,
		chams_material_pearlescent,
		chams_material_ambient,
		chams_material_gold,
		chams_material_damascus,
		chams_material_crystal,
		chams_material_wet,
		chams_material_max
	};

	inline const char* material_names[ chams_material_max ] = { "original", "flat",     "shaded",      "wireframe", "glow",     "metallic",
		                                                        "pearlescent", "ambient", "gold",      "damascus",  "crystal", "wet" };

	constexpr float gold_envmap_tint[ 3 ] = { 0.55f, 0.3f, 0.05f };
	constexpr float gold_phong_tint[ 3 ]  = { 0.6f, 0.35f, 0.08f };

	struct material_t {
		c_material* m_material      = nullptr;
		unsigned int m_envmap_token = 0U;
		unsigned int m_phong_token  = 0U;
		bool m_has_envmap_tint      = false;
		bool m_has_phong_tint       = false;
		bool m_probed               = false;

		std::uint32_t m_last_rgba = 0U;
		float m_last_alpha_scale  = -1.f;
		bool m_color_valid        = false;

		bool m_shared = false;
	};

	struct impl_t {
		material_t m_materials[ chams_material_max ][ 2 ] = { };
		bool m_created                                    = false;

		int m_shared_flag_state[ chams_material_max ] = { };

		int m_build_index      = 0;
		int m_last_build_frame = -1;

		/* built lazily on the render thread (material system alive; never create_material off-thread) */
		void create( )
		{
			if ( this->m_created || !g_interfaces.m_material_system )
				return;

			const int frame = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : this->m_last_build_frame + 1;

			if ( frame == this->m_last_build_frame )
				return;

			this->m_last_build_frame = frame;

			if ( this->m_build_index == 0 ) {
				for ( int i = 0; i < chams_material_max; i++ )
					this->m_shared_flag_state[ i ] = -1;
			}

			const auto from_keyvalues = []( const char* name, const char* shader, const std::string& vmt ) -> c_material* {
				if ( !c_key_values::usable( ) )
					return nullptr;

				c_key_values* key_values = new c_key_values( shader );
				key_values->load_from_buffer( name, vmt.c_str( ) );

				c_material* material = g_interfaces.m_material_system->create_material( name, key_values );

				if ( material )
					material->increment_reference_count( );
				else
					delete key_values;

				return material;
			};

			const auto from_name = []( const char* name ) -> c_material* {
				c_material* material = g_interfaces.m_material_system->find_material( name, TEXTURE_GROUP_MODEL );
				if ( material )
					material->increment_reference_count( );

				return material;
			};

			/* both variants of one slot; depth/fog flags appended per variant. never put $ignorez /
			   $znearer / $nofog in `keys`: FindKey returns the FIRST match, a duplicate would win. */
			const auto make_variants = [ & ]( const int index, const char* name, const char* shader, const std::string& keys ) {
				const auto build = []( const auto& maker, const char* material_name, const char* shader_name, const std::string& body,
				                       const char* flags ) -> c_material* {
					const std::string vmt = std::string( "\"" ) + shader_name + "\"\n{\n" + body + flags + "}\n";

					return maker( material_name, shader_name, vmt );
				};

				this->m_materials[ index ][ 0 ].m_material =
					build( from_keyvalues, name, shader, keys, "\t\"$ignorez\" \"0\"\n\t\"$znearer\" \"0\"\n\t\"$nofog\" \"0\"\n" );

				const std::string xqz_name = std::string( name ) + "_xqz";

				this->m_materials[ index ][ 1 ].m_material =
					build( from_keyvalues, xqz_name.c_str( ), shader, keys, "\t\"$ignorez\" \"1\"\n\t\"$znearer\" \"1\"\n\t\"$nofog\" \"1\"\n" );
			};

			switch ( this->m_build_index ) {
			case 0:
				make_variants( chams_material_flat, "botox_flat", "UnlitGeneric", R"(	"$basetexture" "vgui/white"
	"$model"       "1"
)" );
				break;

			case 1:
				make_variants( chams_material_shaded, "botox_shaded", "VertexLitGeneric", R"(	"$basetexture" "vgui/white"
	"$model"       "1"
	"$halflambert" "1"
)" );
				break;

			case 2:
				make_variants( chams_material_wireframe, "botox_wireframe", "UnlitGeneric", R"(	"$basetexture" "vgui/white"
	"$model"       "1"
	"$wireframe"   "1"
)" );
				break;

			case 3:
				make_variants( chams_material_glow, "botox_glow", "VertexLitGeneric", R"(	"$additive"                "1"
	"$model"                   "1"
	"$envmap"                  "models/effects/cube_white"
	"$envmaptint"              "[1 1 1]"
	"$envmapfresnel"           "1"
	"$envmapfresnelminmaxexp"  "[0 1 2]"
)" );
				break;

			case 4:
				make_variants( chams_material_metallic, "botox_metallic", "VertexLitGeneric", R"(	"$additive"       "1"
	"$model"          "1"
	"$envmap"         "models/effects/cube_white"
	"$envmaptint"     "[1 1 1]"
	"$envmapcontrast" "1"
)" );
				break;

			case 5:
				make_variants( chams_material_pearlescent, "botox_pearlescent", "VertexLitGeneric", R"(	"$basetexture"            "vgui/white_additive"
	"$model"                  "1"
	"$phong"                  "1"
	"$pearlescent"            "1"
	"$basemapalphaphongmask"  "1"
	"$phongboost"             "2"
	"$phongfresnelranges"     "[0.5 0.5 0.5]"
	"$phongexponent"          "10"
	"$phongtint"              "[1 1 1]"
)" );
				break;

			case 6: {
				c_material* ambient = from_name( "debug/debugambientcube" );

				for ( int variant = 0; variant < 2; variant++ ) {
					this->m_materials[ chams_material_ambient ][ variant ].m_material = ambient;
					this->m_materials[ chams_material_ambient ][ variant ].m_shared   = true;
				}

				break;
			}

			case 7:
				make_variants( chams_material_gold, "botox_gold", "VertexLitGeneric", R"(	"$basetexture"             "vgui/white"
	"$bumpmap"                 "effects/flat_normal"
	"$model"                   "1"
	"$color2"                  "[.2 .11 .02]"
	"$envmap"                  "editor/cube_vertigo"
	"$envmaptint"              "[.55 .3 .05]"
	"$envmapfresnel"           ".6"
	"$phong"                   "1"
	"$phongfresnelranges"      "[.7 .8 1]"
	"$phongtint"               "[.6 .35 .08]"
	"$phongboost"              "6"
	"$phongexponent"           "128"
	"$phongdisablehalflambert" "1"
)" );
				break;

			case 8:
				make_variants( chams_material_damascus, "botox_damascus", "VertexLitGeneric", R"(	"$basetexture"            "models/weapons/customization/paints/antiqued/damascus"
	"$model"                  "1"
	"$phong"                  "1"
	"$phongboost"             "4"
	"$phongexponent"          "16"
	"$phongfresnelranges"     "[.5 .75 1]"
	"$phongtint"              "[1 1 1]"
	"$basemapalphaphongmask"  "1"
)" );
				break;

			case 9:
				make_variants( chams_material_crystal, "botox_crystal", "VertexLitGeneric", R"(	"$basetexture"      "black"
	"$bumpmap"          "effects/flat_normal"
	"$model"            "1"
	"$translucent"      "1"
	"$envmap"           "models/effects/crystal_cube_vertigo_hdr"
	"$envmaptint"       "[1 1 1]"
	"$envmapsaturation" "0.1"
	"$phong"            "1"
	"$phongexponent"    "16"
	"$phongboost"       "2"
	"$phongtint"        "[1 1 1]"
)" );
				break;

			case 10:
				make_variants( chams_material_wet, "botox_wet", "VertexLitGeneric", R"(	"$basetexture"             "vgui/white"
	"$bumpmap"                 "effects/flat_normal"
	"$model"                   "1"
	"$additive"                "1"
	"$color2"                  "[0 0 0]"
	"$envmap"                  "env_cubemap"
	"$envmaptint"              "[1 1 1]"
	"$envmapfresnel"           "1"
	"$phong"                   "1"
	"$basemapalphaphongmask"   "1"
	"$phongexponent"           "200"
	"$phongboost"              "10"
	"$phongfresnelranges"      "[.2 .5 1]"
	"$phongtint"               "[1 1 1]"
	"$phongdisablehalflambert" "1"
)" );
				break;
			}

			if ( ++this->m_build_index >= chams_material_max - 1 )
				this->m_created = true;
		}

		void release( )
		{
			if ( g_interfaces.m_model_render )
				g_interfaces.m_model_render->forced_material_override( nullptr );

			for ( int i = 0; i < chams_material_max; i++ ) {
				for ( int variant = 0; variant < 2; variant++ ) {
					material_t& entry = this->m_materials[ i ][ variant ];

					if ( entry.m_material && !( entry.m_shared && variant == 1 ) )
						entry.m_material->decrement_reference_count( );

					entry = material_t{ };
				}

				this->m_shared_flag_state[ i ] = -1;
			}

			this->m_created          = false;
			this->m_build_index      = 0;
			this->m_last_build_frame = -1;
		}

		c_material* get( const int index, const bool ignorez = false )
		{
			if ( index <= chams_material_original || index >= chams_material_max )
				return nullptr;

			this->create( );

			return this->m_materials[ index ][ ignorez ? 1 : 0 ].m_material;
		}

		/* CMaterials::SetColor: modulation for basetexture materials, envmap / phong tint for additive */
		void apply_color( const int index, const bool ignorez, const c_color& color, const float alpha_scale = 1.f )
		{
			c_material* material = this->get( index, ignorez );
			if ( !material )
				return;

			material_t& entry = this->m_materials[ index ][ ignorez ? 1 : 0 ];

			const std::uint8_t r8 = color.get< e_color_type::color_type_r >( );
			const std::uint8_t g8 = color.get< e_color_type::color_type_g >( );
			const std::uint8_t b8 = color.get< e_color_type::color_type_b >( );
			const std::uint8_t a8 = color.get< e_color_type::color_type_a >( );

			const std::uint32_t rgba = ( std::uint32_t( r8 ) << 24 ) | ( std::uint32_t( g8 ) << 16 ) | ( std::uint32_t( b8 ) << 8 ) | a8;

			if ( !entry.m_shared && entry.m_color_valid && entry.m_last_rgba == rgba && entry.m_last_alpha_scale == alpha_scale )
				return;

			entry.m_last_rgba        = rgba;
			entry.m_last_alpha_scale = alpha_scale;
			entry.m_color_valid      = true;

			if ( !entry.m_probed ) {
				entry.m_probed = true;

				bool found = false;
				material->find_var( ( "$envmaptint" ), &found, false );
				entry.m_has_envmap_tint = found;

				found = false;
				material->find_var( ( "$phongtint" ), &found, false );
				entry.m_has_phong_tint = found;
			}

			const float r = color.base< e_color_type::color_type_r >( );
			const float g = color.base< e_color_type::color_type_g >( );
			const float b = color.base< e_color_type::color_type_b >( );

			material->color_modulate( r, g, b );
			material->alpha_modulate( color.base< e_color_type::color_type_a >( ) * alpha_scale );

			const bool gold = index == chams_material_gold;

			if ( entry.m_has_envmap_tint ) {
				if ( c_material_var* var = material->find_var_fast( ( "$envmaptint" ), &entry.m_envmap_token ) ) {
					if ( gold )
						var->set_vector( r * gold_envmap_tint[ 0 ], g * gold_envmap_tint[ 1 ], b * gold_envmap_tint[ 2 ] );
					else
						var->set_vector( r, g, b );
				}
			}

			if ( entry.m_has_phong_tint ) {
				if ( c_material_var* var = material->find_var_fast( ( "$phongtint" ), &entry.m_phong_token ) ) {
					if ( gold )
						var->set_vector( r * gold_phong_tint[ 0 ], g * gold_phong_tint[ 1 ], b * gold_phong_tint[ 2 ] );
					else
						var->set_vector( r, g, b );
				}
			}
		}

		void reset_color_cache( )
		{
			for ( int i = 0; i < chams_material_max; i++ ) {
				for ( int variant = 0; variant < 2; variant++ ) {
					this->m_materials[ i ][ variant ].m_color_valid      = false;
					this->m_materials[ i ][ variant ].m_last_alpha_scale = -1.f;
				}

				this->m_shared_flag_state[ i ] = -1;
			}
		}

		static bool has_layers( const std::vector< int >& slots )
		{
			for ( const int index : slots ) {
				if ( index >= 0 && index < chams_material_max )
					return true;
			}

			return false;
		}

		static void apply_original_color( const c_color& color, const float alpha_scale )
		{
			if ( !g_interfaces.m_render_view )
				return;

			g_interfaces.m_render_view->set_color_modulation( color.base< e_color_type::color_type_r >( ),
			                                                  color.base< e_color_type::color_type_g >( ),
			                                                  color.base< e_color_type::color_type_b >( ) );

			g_interfaces.m_render_view->set_blend( color.base< e_color_type::color_type_a >( ) * alpha_scale );
		}

		static void reset_original_color( )
		{
			if ( !g_interfaces.m_render_view )
				return;

			g_interfaces.m_render_view->set_color_modulation( 1.f, 1.f, 1.f );
			g_interfaces.m_render_view->set_blend( 1.f );
		}

		template< typename t_draw_pass >
		void draw_layers( const std::vector< int >& slots, const std::vector< c_color >& colors, const bool ignorez, const float alpha_scale,
		                  t_draw_pass draw_pass )
		{
			PERF_ZONE( zone_draw_model );

			for ( std::size_t i = 0U; i < slots.size( ); i++ ) {
				const int index = slots[ i ];
				if ( index < 0 || index >= chams_material_max )
					continue;

				c_material* material = this->get( index, ignorez );

				const c_color layer_color = i < colors.size( ) ? colors[ i ] : c_color( 255, 255, 255, 255 );

				if ( index == chams_material_original ) {
					apply_original_color( layer_color, alpha_scale );

					g_interfaces.m_model_render->forced_material_override( nullptr );

					if ( layer_color.base< e_color_type::color_type_a >( ) * alpha_scale > 0.f )
						draw_pass( );

					reset_original_color( );

					continue;
				}

				if ( !material )
					continue;

				this->apply_color( index, ignorez, layer_color, alpha_scale );

				if ( this->m_materials[ index ][ ignorez ? 1 : 0 ].m_shared && this->m_shared_flag_state[ index ] != ( ignorez ? 1 : 0 ) ) {
					this->m_shared_flag_state[ index ] = ignorez ? 1 : 0;

					material->set_material_var_flag( material_var_ignorez, ignorez );
					material->set_material_var_flag( material_var_znearer, ignorez );
					material->set_material_var_flag( material_var_nofog, ignorez );
				}

				g_interfaces.m_model_render->forced_material_override( material );
				draw_pass( );
			}

			g_interfaces.m_model_render->forced_material_override( nullptr );
		}
	};
};

inline n_chams::impl_t g_chams{ };
