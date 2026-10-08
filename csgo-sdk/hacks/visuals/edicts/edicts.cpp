#include "edicts.h"
#include "mc_sky.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"

#include "../../entity_cache/entity_cache.h"
#include "../players/sound_esp/sound_esp.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

static vcollide_t precipitation_collideable{ };

static bool g_skybox_fog_captured = false;

struct stored_fog_t {
	int m_enable, m_color;
	float m_start, m_end, m_density;
};
static std::unordered_map< c_base_entity*, stored_fog_t > g_map_fog{ };

static std::string g_map_sky{ };
static std::string g_written_sky{ };

void n_edicts::impl_t::on_frame_stage_notify( int stage )
{
	// eject: paint_traverse already restored fog / sky, don't write again
	if ( g_ctx.m_world_restore_requested.load( std::memory_order_acquire ) )
		return;

	switch ( stage ) {
	case 5: {
		this->precipitation( );
		this->fog( );
		this->skybox_fog( );
		this->skybox( );
		this->skybox_3d( );
		this->sun_angle( );
		this->fullbright( );
		this->dlights( );
		break;
	}
	}
}

void n_edicts::impl_t::on_paint_traverse( )
{
	if ( GET_VARIABLE( g_variables.m_dropped_weapons, bool ) )
		this->dropped_weapons( );
	if ( GET_VARIABLE( g_variables.m_thrown_objects, bool ) )
		this->projectiles( );

	if ( GET_VARIABLE( g_variables.m_bomb_esp, bool ) || GET_VARIABLE( g_variables.m_bomb_timer_bar, bool ) )
		this->bomb( );
}

void n_edicts::impl_t::reset( )
{
	g_skybox_fog_captured = false;
	g_map_fog.clear( );

	this->remove_precipitation( );
}

void n_edicts::impl_t::remove_precipitation( )
{
	if ( m_created ) {
		g_entity_cache.enumerate( e_enumeration_type::type_edicts, [ & ]( c_base_entity* entity ) {
			if ( !entity )
				return;

			const auto client_renderable = entity->get_client_renderable( );
			if ( !client_renderable )
				return;

			const auto client_unknown = client_renderable->get_client_unknown( );
			if ( !client_unknown )
				return;

			const auto client_networkable = client_unknown->get_client_networkable( );
			if ( !client_networkable )
				return;

			auto client_class = client_networkable->get_client_class( );
			if ( !client_class )
				return;

			if ( client_class->m_class_id == e_class_ids::c_precipitation ) {
				const auto rain_networkable = entity->get_client_networkable( );
				if ( !rain_networkable )
					return;

				rain_networkable->pre_data_update( 0 );
				rain_networkable->on_pre_data_changed( 0 );

				*( int* )( ( uintptr_t )entity + 0xA00 ) = -1;

				const auto collideable = entity->get_collideable( );
				if ( !collideable )
					return;

				collideable->get_obb_mins( ) = c_vector{ 0, 0, 0 };
				collideable->get_obb_maxs( ) = c_vector{ 0, 0, 0 };

				rain_networkable->on_data_changed( 0 );
				rain_networkable->post_data_update( 0 );
				rain_networkable->release( );
			}
		} );

		g_interfaces.m_physics_collison->v_collide_unload( &precipitation_collideable );
		m_created = false;
		m_timer   = -1;
	}
}

void* n_edicts::impl_t::get_precipitation_collideable( )
{
	return &precipitation_collideable;
}

void n_edicts::impl_t::projectiles( )
{
	if ( !g_ctx.m_local )
		return;

	const auto should_draw_icon = GET_VARIABLE( g_variables.m_thrown_objects_icon, bool );
	const auto should_draw_name = GET_VARIABLE( g_variables.m_thrown_objects_name, bool );

	ImColor icon_color = GET_VARIABLE( g_variables.m_thrown_objects_icon_color, c_color ).get_u32( 1.f );
	ImColor name_color = GET_VARIABLE( g_variables.m_thrown_objects_name_color, c_color ).get_u32( 1.f );

	g_entity_cache.enumerate( e_enumeration_type::type_edicts, [ & ]( c_base_entity* entity ) {
		if ( !entity || entity == g_ctx.m_local || entity->is_dormant( ) )
			return;

		const auto client_renderable = entity->get_client_renderable( );
		if ( !client_renderable )
			return;

		const auto client_unknown = client_renderable->get_client_unknown( );
		if ( !client_unknown )
			return;

		const auto client_networkable = client_unknown->get_client_networkable( );
		if ( !client_networkable )
			return;

		const auto client_class = client_networkable->get_client_class( );
		if ( !client_class )
			return;

		const auto class_id = client_class->m_class_id;

		auto model = entity->get_model( );

		if ( !model )
			return;

		auto model_name = g_interfaces.m_model_info->get_model_name( model );

		if ( !model_name )
			return;

		const char* const name = model->m_name;
		if ( !name )
			return;

		if ( std::strstr( name, "thrown" ) || class_id == e_class_ids::c_base_cs_grenade_projectile ||
		     class_id == e_class_ids::c_decoy_projectile || class_id == e_class_ids::c_molotov_projectile ||
		     class_id == e_class_ids::c_snowball_projectile ) {
			c_vector_2d out{ };

			if ( !g_render.world_to_screen( entity->get_abs_origin( ), out ) || out.is_zero( ) )
				return;

			auto projectile_icon = [ & ]( const int char_icon_index, const char* const name ) -> void {
				const auto text_size = g_render.m_fonts[ e_font_names::font_name_verdana_11 ]->CalcTextSizeA(
					g_render.m_fonts[ e_font_names::font_name_verdana_11 ]->FontSize, FLT_MAX, 0.f, name );

				if ( should_draw_name )
					g_render.m_draw_data.emplace_back(
						e_draw_type::draw_type_text,
						std::make_any< text_draw_object_t >( g_render.m_fonts[ e_font_names::font_name_verdana_11 ],
					                                         c_vector_2d( out.m_x - ( text_size.x / 2 ), out.m_y - 15 ), name, name_color,
					                                         c_color( 0.f, 0.f, 0.f, 1.f ).get_u32( ), e_text_flags::text_flag_dropshadow ) );

				auto icon = reinterpret_cast< const char* >( g_utilities.get_weapon_icon( char_icon_index ) );

				const auto icon_size = g_render.m_fonts[ e_font_names::font_name_icon_12 ]->CalcTextSizeA(
					g_render.m_fonts[ e_font_names::font_name_icon_12 ]->FontSize, FLT_MAX, 0.f, icon );
				if ( should_draw_icon )
					g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text,
					                                   std::make_any< text_draw_object_t >( g_render.m_fonts[ e_font_names::font_name_icon_12 ], out,
					                                                                        icon, icon_color, ImColor( 0.f, 0.f, 0.f, 1.f ),
					                                                                        text_flag_dropshadow ) );
			};

			if ( std::strstr( name, "flashbang" ) )
				projectile_icon( weapon_flashbang, "flashbang" );
			else if ( std::strstr( name, "smokegrenade" ) )
				projectile_icon( weapon_smokegrenade, "smoke grenade" );
			else if ( std::strstr( name, "incendiarygrenade" ) )
				projectile_icon( weapon_incgrenade, "incendiary grenade" );
			else if ( std::strstr( name, "molotov" ) )
				projectile_icon( weapon_molotov, "molotov" );
			else if ( std::strstr( name, "fraggrenade" ) )
				projectile_icon( weapon_hegrenade, "high explosive grenade" );
			else if ( std::strstr( name, "decoy" ) )
				projectile_icon( weapon_decoy, "decoy" );
		}
	} );
}

void n_edicts::impl_t::dropped_weapons( )
{
	g_entity_cache.enumerate( e_enumeration_type::type_edicts, [ & ]( c_base_entity* entity ) {
		if ( !entity || entity == g_ctx.m_local || entity->is_dormant( ) )
			return;

		const auto client_renderable = entity->get_client_renderable( );
		if ( !client_renderable )
			return;

		const auto client_unknown = client_renderable->get_client_unknown( );
		if ( !client_unknown )
			return;

		const auto client_networkable = client_unknown->get_client_networkable( );
		if ( !client_networkable )
			return;

		const auto client_class = client_networkable->get_client_class( );
		if ( !client_class )
			return;

		const auto class_id = client_class->m_class_id;

		if ( class_id == e_class_ids::c_base_weapon_world_model )
			return;

		if ( strstr( client_class->m_network_name, "CWeapon" ) || class_id == e_class_ids::c_deagle || class_id == e_class_ids::cak47 ) {
			const short definition_index = entity->get_item_definition_index( );
			if ( !definition_index )
				return;

			const auto weapon_data = g_interfaces.m_weapon_system->get_weapon_data( definition_index );
			if ( !weapon_data || !weapon_data->is_gun( ) )
				return;

			const auto owner_entity = g_interfaces.m_client_entity_list->get< c_base_entity >( entity->get_owner_entity_handle( ) );
			if ( owner_entity )
				return;

			bounding_box_t box{ };
			if ( !entity->get_bounding_box( &box ) )
				return;

			float distance_to_weapon_alpha = static_cast< float >( g_ctx.m_local->get_abs_origin( ).dist_to( entity->get_abs_origin( ) ) - 600.f );

			distance_to_weapon_alpha = g_math.divide_if_less< float >( distance_to_weapon_alpha, 250.f );

			if ( GET_VARIABLE( g_variables.m_dropped_weapons_box, bool ) ) {
				if ( !GET_VARIABLE( g_variables.m_dropped_weapons_box_corner, bool ) ) {
					g_render.m_draw_data.emplace_back(
						e_draw_type::draw_type_rect,
						std::make_any< rect_draw_object_t >(
							c_vector_2d( box.m_left, box.m_top ), c_vector_2d( box.m_right, box.m_bottom ),
							GET_VARIABLE( g_variables.m_dropped_weapons_box_color, c_color ).get_u32( 1.f - distance_to_weapon_alpha ),
							GET_VARIABLE( g_variables.m_dropped_weapons_box_outline_color, c_color ).get_u32( 1.f - distance_to_weapon_alpha ), false,
							0.f, ImDrawFlags_::ImDrawFlags_None, 1.f,
							GET_VARIABLE( g_variables.m_dropped_weapons_box_outline, bool )
								? e_rect_flags::rect_flag_inner_outline | e_rect_flags::rect_flag_outer_outline
								: e_rect_flags::rect_flag_none ) );
				} else {
					if ( GET_VARIABLE( g_variables.m_dropped_weapons_box_outline, bool ) ) {
						g_render.corner_rect( box.m_left, box.m_top, box.m_right, box.m_bottom,
						                      c_color( GET_VARIABLE( g_variables.m_dropped_weapons_box_outline_color, c_color ) )
						                          .get_u32( 1.f - distance_to_weapon_alpha ),
						                      2.f );
					}

					g_render.corner_rect(
						box.m_left, box.m_top, box.m_right, box.m_bottom,
						c_color( GET_VARIABLE( g_variables.m_dropped_weapons_box_color, c_color ) ).get_u32( 1.f - distance_to_weapon_alpha ) );
				}
			}

			float top_padding = 0.f;

			auto icon_color   = GET_VARIABLE( g_variables.m_dropped_weapons_icon_color, c_color );
			auto weapon_color = GET_VARIABLE( g_variables.m_dropped_weapons_name_color, c_color );

			if ( GET_VARIABLE( g_variables.m_dropped_weapons_icon, bool ) ) {
				const auto text      = reinterpret_cast< const char* >( g_utilities.get_weapon_icon( definition_index ) );
				const auto text_size = g_render.m_fonts[ e_font_names::font_name_icon_12 ]->CalcTextSizeA(
					g_render.m_fonts[ e_font_names::font_name_icon_12 ]->FontSize, FLT_MAX, 0.f, text );

				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_text,
					std::make_any< text_draw_object_t >(
						g_render.m_fonts[ e_font_names::font_name_icon_12 ],
						c_vector_2d( box.m_left + box.m_width * 0.5f - text_size.x * 0.5f, box.m_top - 3 - text_size.y ), text,
						icon_color.get_u32( 1.f - distance_to_weapon_alpha ), c_color( 0.f, 0.f, 0.f, 1.f ).get_u32( 1.f - distance_to_weapon_alpha ),
						e_text_flags::text_flag_dropshadow ) );

				top_padding -= text_size.y;
			}

			if ( GET_VARIABLE( g_variables.m_dropped_weapons_name, bool ) ) {
				const auto localized_name = g_interfaces.m_localize->find( weapon_data->m_hud_name );

				std::wstring w = localized_name;
				if ( w.empty( ) )
					return;

				std::transform( w.begin( ), w.end( ), w.begin( ), ::towlower );

				const std::string converted_name = g_utilities.to_utf8( w );
				if ( converted_name.empty( ) )
					return;

				const auto dropped_weapon_font = g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_esp ];

				const auto text_size =
					dropped_weapon_font->CalcTextSizeA( dropped_weapon_font->FontSize, FLT_MAX, 0.f, converted_name.c_str( ) );

				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_text,
					std::make_any< text_draw_object_t >(
						dropped_weapon_font,
						c_vector_2d( box.m_left + box.m_width * 0.5f - text_size.x * 0.5f, box.m_top - 3 - text_size.y - top_padding ),
						converted_name,
						GET_VARIABLE( g_variables.m_dropped_weapons_name_color, c_color )
							.get_u32( weapon_color.get< color_type_a >( ) - distance_to_weapon_alpha ),
						c_color( 0.f, 0.f, 0.f, 1.f ).get_u32( weapon_color.get< color_type_a >( ) - distance_to_weapon_alpha ),
						e_text_flags::text_flag_dropshadow ) );
			}
		}
	} );
}

void n_edicts::impl_t::precipitation( )
{
	if ( !GET_VARIABLE( g_variables.m_precipitation, bool ) ) {
		this->remove_precipitation( );
		return;
	}

	static int weather_type = 0;
	switch ( GET_VARIABLE( g_variables.m_precipitation_type, int ) ) {
	case 0: {
		weather_type = 4;
		break;
	}
	case 1: {
		weather_type = 5;
		break;
	}
	case 2: {
		weather_type = 6;
		break;
	}
	case 3: {
		weather_type = 7;
		break;
	}
	}

	if ( m_timer > -1 ) {
		--m_timer;
		if ( m_timer == 0 ) {
			this->remove_precipitation( );
		}
	}

	static std::optional< int > last_type{ };

	if ( last_type.has_value( ) && last_type.value( ) != weather_type )
		this->remove_precipitation( );

	last_type = weather_type;

	if ( m_created )
		return;

	memset( &precipitation_collideable, 0, sizeof( precipitation_collideable ) );

	static c_base_client* precipitation_client_class = nullptr;
	if ( !precipitation_client_class ) {
		for ( auto client_class = g_interfaces.m_base_client->get_all_classes( ); client_class; client_class = client_class->m_next ) {
			if ( client_class->m_class_id == e_class_ids::c_precipitation ) {
				precipitation_client_class = client_class;
				break;
			}
		}
	}

	if ( precipitation_client_class && precipitation_client_class->m_create_fn ) {
		void* rain_networkable = ( ( void* ( * )( int, int ))precipitation_client_class->m_create_fn )( 2048 - 1, 0 );
		if ( !rain_networkable )
			return;

		const auto rain_unknown = ( ( c_client_renderable* )rain_networkable )->get_client_unknown( );
		if ( !rain_unknown )
			return;

		const auto entity = rain_unknown->get_base_entity( );
		if ( !entity )
			return;

		const auto networkable = entity->get_client_networkable( );
		if ( !networkable )
			return;

		networkable->pre_data_update( 0 );
		networkable->on_pre_data_changed( 0 );
		entity->get_index( ) = -1;

		*( int* )( ( uintptr_t )entity + 0xA00 ) = weather_type;

		const auto collideable = entity->get_collideable( );
		if ( !collideable )
			return;

		collideable->get_obb_mins( ) = c_vector( -32768.f, -32768.f, -32768.f );
		collideable->get_obb_maxs( ) = c_vector( 32768.f, 32768.f, 32768.f );

		g_interfaces.m_physics_collison->v_collide_load( &precipitation_collideable, 1, ( const char* )collide_data, sizeof( collide_data ) );

		entity->get_model_index( ) = -1;

		networkable->on_data_changed( 0 );
		networkable->post_data_update( 0 );

		m_created = true;
	}
}

void n_edicts::impl_t::fog( const bool restore )
{
	const bool enabled = !restore && GET_VARIABLE( g_variables.m_fog, bool );

	if ( !enabled && g_map_fog.empty( ) )
		return;

	g_entity_cache.enumerate( e_enumeration_type::type_edicts, [ & ]( c_base_entity* entity ) {
		if ( !entity || ( enabled && entity->is_dormant( ) ) )
			return;

		const auto client_renderable = entity->get_client_renderable( );
		if ( !client_renderable )
			return;

		const auto client_unknown = client_renderable->get_client_unknown( );
		if ( !client_unknown )
			return;

		const auto client_networkable = client_unknown->get_client_networkable( );
		if ( !client_networkable )
			return;

		const auto client_class = client_networkable->get_client_class( );
		if ( !client_class )
			return;

		if ( !( client_class->m_class_id == e_class_ids::c_fog_controller ) )
			return;

		if ( !enabled ) {
			const auto it = g_map_fog.find( entity );
			if ( it == g_map_fog.end( ) )
				return;

			entity->get_fog_enable( )  = it->second.m_enable;
			entity->get_fog_start( )   = it->second.m_start;
			entity->get_fog_end( )     = it->second.m_end;
			entity->get_fog_density( ) = it->second.m_density;
			entity->get_fog_color( )   = it->second.m_color;

			g_console.print( std::format( "[fog] restore | enable {} start {:.0f} end {:.0f} density {:.2f}", it->second.m_enable,
			                              it->second.m_start, it->second.m_end, it->second.m_density )
			                     .c_str( ) );
			return;
		}

		const auto [ stored, inserted ] = g_map_fog.try_emplace(
			entity, stored_fog_t{ entity->get_fog_enable( ), entity->get_fog_color( ), entity->get_fog_start( ), entity->get_fog_end( ),
		                          entity->get_fog_density( ) } );

		if ( inserted )
			g_console.print( std::format( "[fog] capture | enable {} start {:.0f} end {:.0f} density {:.2f}", stored->second.m_enable,
			                              stored->second.m_start, stored->second.m_end, stored->second.m_density )
			                     .c_str( ) );

		const c_color color = GET_VARIABLE( g_variables.m_fog_color, c_color );

		entity->get_fog_enable( ) = 1;
		entity->get_fog_start( )  = GET_VARIABLE( g_variables.m_fog_start, float );
		entity->get_fog_end( )    = GET_VARIABLE( g_variables.m_fog_end, float );
		entity->get_fog_density( ) = GET_VARIABLE( g_variables.m_fog_density, float ) * 0.01f;
		entity->get_fog_color( )   = ImGui::ColorConvertFloat4ToU32( color.get_vec4( ) );
	} );

	if ( !enabled )
		g_map_fog.clear( );
}

void n_edicts::impl_t::skybox_fog( const bool restore )
{
	if ( !g_ctx.m_local )
		return;

	static const unsigned int enable_offset  = g_netvars[ HASH_BT( "CBasePlayer->m_skybox3d.fog.enable" ) ].m_offset;
	static const unsigned int blend_offset   = g_netvars[ HASH_BT( "CBasePlayer->m_skybox3d.fog.blend" ) ].m_offset;
	static const unsigned int start_offset   = g_netvars[ HASH_BT( "CBasePlayer->m_skybox3d.fog.start" ) ].m_offset;
	static const unsigned int end_offset     = g_netvars[ HASH_BT( "CBasePlayer->m_skybox3d.fog.end" ) ].m_offset;
	static const unsigned int density_offset = g_netvars[ HASH_BT( "CBasePlayer->m_skybox3d.fog.maxdensity" ) ].m_offset;
	static const unsigned int color_offset   = g_netvars[ HASH_BT( "CBasePlayer->m_skybox3d.fog.colorPrimary" ) ].m_offset;

	if ( !enable_offset || !blend_offset || !start_offset || !end_offset || !density_offset || !color_offset )
		return;

	const uintptr_t local = reinterpret_cast< uintptr_t >( g_ctx.m_local );

	struct stored_sky_fog_t {
		int m_enable, m_blend, m_color;
		float m_start, m_end, m_density;
	};

	static stored_sky_fog_t stored{ };

	if ( restore || !GET_VARIABLE( g_variables.m_fog, bool ) ) {
		if ( g_skybox_fog_captured ) {
			*( int* )( local + enable_offset )    = stored.m_enable;
			*( int* )( local + blend_offset )     = stored.m_blend;
			*( int* )( local + color_offset )     = stored.m_color;
			*( float* )( local + start_offset )   = stored.m_start;
			*( float* )( local + end_offset )     = stored.m_end;
			*( float* )( local + density_offset ) = stored.m_density;

			g_skybox_fog_captured = false;
		}

		return;
	}

	if ( !g_skybox_fog_captured ) {
		stored = { *( int* )( local + enable_offset ),   *( int* )( local + blend_offset ),
		           *( int* )( local + color_offset ),   *( float* )( local + start_offset ),
		           *( float* )( local + end_offset ),   *( float* )( local + density_offset ) };

		g_skybox_fog_captured = true;
	}

	const c_color color = GET_VARIABLE( g_variables.m_fog_color, c_color );

	*( int* )( local + enable_offset ) = 1;

	*( int* )( local + blend_offset ) = 0;

	*( float* )( local + start_offset )   = GET_VARIABLE( g_variables.m_fog_start, float );
	*( float* )( local + end_offset )     = GET_VARIABLE( g_variables.m_fog_end, float );
	*( float* )( local + density_offset ) = GET_VARIABLE( g_variables.m_fog_density, float ) * 0.01f;
	*( int* )( local + color_offset )     = ImGui::ColorConvertFloat4ToU32( color.get_vec4( ) );
}

void n_edicts::impl_t::skybox_3d( const bool restore )
{
	static c_cconvar* sky_3d = nullptr;
	if ( !sky_3d && !( sky_3d = g_convars[ HASH_BT( "r_3dsky" ) ] ) )
		return;

	static bool captured = false;
	static float stored  = 1.f;

	const bool enabled = !restore && GET_VARIABLE( g_variables.m_remove_3d_skybox, bool );

	if ( !enabled ) {
		if ( captured ) {
			sky_3d->set_value( stored );
			captured = false;
		}

		return;
	}

	if ( !captured ) {
		stored   = sky_3d->get_float( );
		captured = true;
	}

	if ( sky_3d->get_float( ) != 0.f )
		sky_3d->set_value( 0.f );
}

void n_edicts::impl_t::fullbright( const bool restore )
{
	static c_cconvar* convar = nullptr;
	if ( !convar && !( convar = g_convars[ HASH_BT( "mat_fullbright" ) ] ) )
		return;

	static bool captured = false;
	static float stored  = 0.f;

	if ( restore || !GET_VARIABLE( g_variables.m_fullbright, bool ) ) {
		if ( captured ) {
			convar->set_value( stored );
			captured = false;
		}

		return;
	}

	if ( !captured ) {
		stored   = convar->get_float( );
		captured = true;
	}

	if ( convar->get_float( ) != 1.f )
		convar->set_value( 1.f );
}

struct dlight_t {
	int m_flags;
	c_vector m_origin;
	float m_radius;
	unsigned char m_r, m_g, m_b;
	signed char m_exponent;
	float m_die;
	float m_decay;
	float m_min_light;
	int m_key;
	int m_style;
	c_vector m_direction;
	float m_inner_angle;
	float m_outer_angle;
};
static_assert( offsetof( dlight_t, m_radius ) == 0x10 && offsetof( dlight_t, m_die ) == 0x18 && offsetof( dlight_t, m_key ) == 0x24 &&
               offsetof( dlight_t, m_outer_angle ) == 0x3c, "dlight_t layout" );

void n_edicts::impl_t::dlights( )
{
	const bool enemies = GET_VARIABLE( g_variables.m_dlight, bool );
	const bool local   = enemies && GET_VARIABLE( g_variables.m_dlight_local, bool );

	if ( !enemies || !g_ctx.m_local )
		return;

	static void* effects = g_modules[ ENGINE_DLL ].find_interface( "VEngineEffects001" );
	if ( !effects )
		return;

	const float radius   = std::clamp( GET_VARIABLE( g_variables.m_dlight_radius, float ), 25.f, 1000.f );
	const int brightness = std::clamp( GET_VARIABLE( g_variables.m_dlight_brightness, int ), 0, 10 );
	const float now      = g_interfaces.m_global_vars_base->m_current_time;

	for ( int index = 1; index <= g_interfaces.m_global_vars_base->m_max_clients; index++ ) {
		const auto player = g_interfaces.m_client_entity_list->get< c_base_entity >( index );
		if ( !player || player->is_dormant( ) || !player->is_alive( ) )
			continue;

		const bool is_local = player == g_ctx.m_local;
		if ( is_local ? !local : ( !g_ctx.m_local->is_enemy( player ) || g_sound_esp.player_gate( player ) <= 0.f ) )
			continue;

		auto light = g_virtual.call< dlight_t* >( effects, 4, index );
		if ( !light )
			continue;

		const c_color color = is_local ? GET_VARIABLE( g_variables.m_dlight_local_color, c_color ) : GET_VARIABLE( g_variables.m_dlight_color, c_color );

		light->m_flags       = 0;
		light->m_origin      = player->get_abs_origin( ) + c_vector( 0.f, 0.f, 36.f );
		light->m_radius      = radius;
		light->m_r           = color.get< color_type_r >( );
		light->m_g           = color.get< color_type_g >( );
		light->m_b           = color.get< color_type_b >( );
		light->m_exponent    = static_cast< signed char >( brightness );
		light->m_die         = now + 0.1f;
		light->m_decay       = 0.f;
		light->m_min_light   = 0.f;
		light->m_key         = index;
		light->m_style       = 0;
		light->m_direction   = c_vector( 0.f, 0.f, 0.f );
		light->m_inner_angle = 0.f;
		light->m_outer_angle = 0.f;
	}
}

// index must match e_skybox_type, entry 0 = off
static const char* const k_skybox_names[ e_skybox_type::skybox_max ] = {
	"",
	"sky_csgo_cloudy01",
	"sky_csgo_night02",
	"sky_csgo_night02b",
	"cs_baggage_skybox_",
	"cs_tibet",
	"vietnam",
	"sky_lunacy",
	"embassy",
	"italy",
	"jungle",
	"office",
	"sky_cs15_daylight01_hdr",
	"sky_cs15_daylight02_hdr",
	"sky_cs15_daylight03_hdr",
	"sky_cs15_daylight04_hdr",
	"sky_day02_05",
	"nukeblank",
	"dustblank",
	"sky_venice",
	"vertigo",
	"vertigoblue_hdr",
	"sky_dust",
	"sky_hr_aztec",
	n_mc_sky::k_name,
	"",
};

static bool skybox_loadable( const std::string& name )
{
	static std::unordered_map< std::string, bool > cached;

	if ( const auto it = cached.find( name ); it != cached.end( ) )
		return it->second;

	if ( !g_interfaces.m_material_system )
		return false;

	bool ok = true;
	for ( const char* suffix : n_mc_sky::k_faces ) {
		const std::string face = "skybox/" + name + suffix;
		c_material* material   = g_interfaces.m_material_system->find_material( face.c_str( ), TEXTURE_GROUP_SKYBOX, false );

		if ( !material || material->is_error_material( ) ) {
			ok = false;
			break;
		}
	}

	return cached[ name ] = ok;
}

// loose csgo\materials\skybox sets ( all 6 face vmts ) that are not in the built in list
void n_edicts::impl_t::scan_custom_skyboxes( )
{
	m_custom_skyboxes.clear( );

	const std::vector< std::string > files = g_utilities.game_files( "materials/skybox", ".vmt" );
	const auto has                         = [ & ]( const std::string& rel ) { return std::binary_search( files.begin( ), files.end( ), rel ); };

	for ( const std::string& file : files ) {
		constexpr std::size_t prefix = sizeof( "materials/skybox/" ) - 1, tail = sizeof( "rt.vmt" ) - 1;
		if ( file.size( ) <= prefix + tail || file.compare( file.size( ) - tail, tail, "rt.vmt" ) != 0 )
			continue;

		const std::string name = file.substr( prefix, file.size( ) - prefix - tail );
		const bool built_in    = std::any_of( std::begin( k_skybox_names ), std::end( k_skybox_names ),
		                                      [ & ]( const char* n ) { return _stricmp( n, name.c_str( ) ) == 0; } );
		const bool complete    = std::all_of( std::begin( n_mc_sky::k_faces ), std::end( n_mc_sky::k_faces ),
		                                      [ & ]( const char* face ) { return has( "materials/skybox/" + name + face + ".vmt" ); } );
		if ( !built_in && complete )
			m_custom_skyboxes.push_back( name );
	}
}

void n_edicts::impl_t::skybox( const bool restore )
{
	static c_cconvar* sky_name = nullptr;
	if ( !sky_name && !( sky_name = g_convars[ HASH_BT( "sv_skyname" ) ] ) )
		return;

	int type         = restore ? static_cast< int >( e_skybox_type::skybox_off ) : GET_VARIABLE( g_variables.m_skybox, int );
	const char* live = sky_name->get_string( );

	if ( !live )
		return;

	if ( g_written_sky != live )
		g_map_sky = live;

	if ( type <= e_skybox_type::skybox_off || type >= e_skybox_type::skybox_max )
		type = e_skybox_type::skybox_off;

	// baked once into csgo\materials\skybox ( ~0.25 s ), files kept for next time
	static int mc_ready = 0;
	if ( type == e_skybox_type::skybox_minecraft && mc_ready == 0 ) {
		const std::string dir = g_utilities.csgo_dir( );
		mc_ready              = !dir.empty( ) && n_mc_sky::ensure( dir ) ? 1 : -1;
	}

	std::string wanted = type == e_skybox_type::skybox_custom ? GET_VARIABLE( g_variables.m_skybox_custom, std::string )
	                     : type != e_skybox_type::skybox_off  ? std::string( k_skybox_names[ type ] )
	                                                          : std::string( );

	if ( !wanted.empty( ) && ( ( type == e_skybox_type::skybox_minecraft && mc_ready < 0 ) || !skybox_loadable( wanted ) ) ) {
		g_console.print< n_console::log_level::WARNING >(
			std::format( "skybox '{}' is not installed in this build, changer set back to off", wanted ).c_str( ) );

		GET_VARIABLE( g_variables.m_skybox, int ) = e_skybox_type::skybox_off;
		wanted.clear( );
	}

	if ( wanted.empty( ) ) {
		if ( !g_written_sky.empty( ) ) {
			if ( !g_map_sky.empty( ) )
				sky_name->set_value( g_map_sky.c_str( ) );

			g_written_sky.clear( );
		}

		return;
	}

	if ( std::strcmp( live, wanted.c_str( ) ) != 0 )
		sky_name->set_value( wanted.c_str( ) );

	g_written_sky = wanted;
}

void n_edicts::impl_t::sun_angle( const bool restore )
{
	struct managed_convar_t {
		const char* m_name;
		c_cconvar* m_convar;
		float m_stored;
	};

	static managed_convar_t convars[] = {
		{ "cl_csm_shadows" }, { "cl_csm_rot_override" }, { "cl_csm_rot_x" }, { "cl_csm_rot_y" }, { "cl_csm_rot_z" },
		{ "cl_csm_max_shadow_dist" },
	};

	enum { shadows, rot_override, rot_x, rot_y, rot_z, max_shadow_dist };

	static bool captured = false;

	const bool enabled = !restore && GET_VARIABLE( g_variables.m_sun_angle, bool );

	if ( !enabled ) {
		if ( captured ) {
			for ( auto& entry : convars ) {
				if ( entry.m_convar )
					entry.m_convar->set_value( entry.m_stored );
			}

			captured = false;
		}

		return;
	}

	for ( auto& entry : convars ) {
		if ( !entry.m_convar && !( entry.m_convar = g_convars[ HASH_RT( entry.m_name ) ] ) )
			return;
	}

	if ( !captured ) {
		for ( auto& entry : convars )
			entry.m_stored = entry.m_convar->get_float( );

		captured = true;
	}

	static float last_time = 0.f;
	static float drift     = 0.f;

	const float now   = g_interfaces.m_global_vars_base->m_real_time;
	const float delta = now - last_time;
	last_time         = now;

	const float speed = GET_VARIABLE( g_variables.m_sun_angle_speed, float );

	if ( speed != 0.f && delta > 0.f && delta < 1.f )
		drift = std::fmod( drift + speed * delta, 360.f );
	else if ( speed == 0.f )
		drift = 0.f;

	const float shadow_distance = GET_VARIABLE( g_variables.m_sun_angle_shadow_distance, float );

	const float wanted[] = {
		1.f,
		1.f,
		GET_VARIABLE( g_variables.m_sun_angle_pitch, float ),
		std::fmod( GET_VARIABLE( g_variables.m_sun_angle_yaw, float ) + drift, 360.f ),
		GET_VARIABLE( g_variables.m_sun_angle_roll, float ),
		shadow_distance > 0.f ? shadow_distance : convars[ max_shadow_dist ].m_stored,
	};

	for ( int i = 0; i < static_cast< int >( sizeof( convars ) / sizeof( convars[ 0 ] ) ); i++ ) {
		if ( convars[ i ].m_convar->get_float( ) != wanted[ i ] )
			convars[ i ].m_convar->set_value( wanted[ i ] );
	}
}

void n_edicts::impl_t::bomb( )
{
	if ( !g_ctx.m_local )
		return;

	const float current_time = g_interfaces.m_global_vars_base->m_current_time;

	const bool draw_world = GET_VARIABLE( g_variables.m_bomb_esp, bool );
	const bool draw_bar   = GET_VARIABLE( g_variables.m_bomb_timer_bar, bool );

	g_entity_cache.enumerate( e_enumeration_type::type_edicts, [ & ]( c_base_entity* entity ) {
		if ( !entity )
			return;

		const auto client_networkable = entity->get_client_networkable( );
		if ( !client_networkable )
			return;

		const auto client_class = client_networkable->get_client_class( );
		if ( !client_class || client_class->m_class_id != e_class_ids::c_planted_c4 )
			return;

		if ( !entity->is_bomb_ticking( ) || entity->is_bomb_defused( ) )
			return;

		const float explode_in = entity->get_c4_blow_time( ) - current_time;
		if ( explode_in <= 0.f )
			return;

		const float defuse_in    = entity->get_defuse_end_time( ) - current_time;
		const bool  being_defused = defuse_in > 0.f;

		const c_color color = being_defused ? ( defuse_in <= explode_in ? GET_VARIABLE( g_variables.m_bomb_esp_defuse_color, c_color )
		                                                               : GET_VARIABLE( g_variables.m_bomb_esp_fail_color, c_color ) )
		                                    : GET_VARIABLE( g_variables.m_bomb_esp_color, c_color );

		const auto font = g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_esp ]
		                      ? g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_esp ]
		                      : g_render.m_fonts[ e_font_names::font_name_verdana_11 ];

		if ( draw_world && font ) {
			c_vector_2d out{ };

			if ( g_render.world_to_screen( entity->get_abs_origin( ), out ) && !out.is_zero( ) ) {
				float top_padding = 0.f;

				if ( GET_VARIABLE( g_variables.m_bomb_esp_timer, bool ) ) {
					char text[ 32 ] = { };

					if ( being_defused )
						std::snprintf( text, sizeof( text ), "%.1f / %.1f", defuse_in, explode_in );
					else
						std::snprintf( text, sizeof( text ), "%.1f", explode_in );

					const auto text_size = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, text );

					g_render.m_draw_data.emplace_back(
						e_draw_type::draw_type_text,
						std::make_any< text_draw_object_t >( font, c_vector_2d( out.m_x - text_size.x * 0.5f, out.m_y ), text, color.get_u32( ),
					                                         c_color( 0.f, 0.f, 0.f, 1.f ).get_u32( ), e_text_flags::text_flag_dropshadow ) );

					top_padding = text_size.y;
				}

				if ( GET_VARIABLE( g_variables.m_bomb_esp_icon, bool ) ) {
					const auto icon      = reinterpret_cast< const char* >( g_utilities.get_weapon_icon( e_item_definition_index::weapon_c4 ) );
					const auto icon_font = g_render.m_fonts[ e_font_names::font_name_icon_12 ];
					const auto icon_size = icon_font->CalcTextSizeA( icon_font->FontSize, FLT_MAX, 0.f, icon );

					g_render.m_draw_data.emplace_back(
						e_draw_type::draw_type_text,
						std::make_any< text_draw_object_t >( icon_font, c_vector_2d( out.m_x - icon_size.x * 0.5f, out.m_y - icon_size.y - top_padding ),
					                                         icon, color.get_u32( ), c_color( 0.f, 0.f, 0.f, 1.f ).get_u32( ),
					                                         e_text_flags::text_flag_dropshadow ) );
				}
			}
		}

		if ( draw_bar && font ) {
			const auto display_size = ImVec2( g_ctx.m_width, g_ctx.m_height );

			constexpr float bar_width = 220.f, bar_height = 8.f;

			const float left = display_size.x * 0.5f - bar_width * 0.5f;
			const float top  = static_cast< float >( GET_VARIABLE( g_variables.m_bomb_timer_bar_position, int ) );

			static c_cconvar* c4_timer = nullptr;
			if ( !c4_timer )
				c4_timer = g_convars[ HASH_BT( "mp_c4timer" ) ];

			const float fuse   = c4_timer && c4_timer->get_float( ) > 0.f ? c4_timer->get_float( ) : 40.f;
			const float factor = std::clamp( explode_in / fuse, 0.f, 1.f );

			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_rect,
				std::make_any< rect_draw_object_t >( c_vector_2d( left, top ), c_vector_2d( left + bar_width, top + bar_height ),
			                                         c_color( 0, 0, 0, 160 ).get_u32( ), c_color( 0.f, 0.f, 0.f, 1.f ).get_u32( ), true, 0.f,
			                                         ImDrawFlags_::ImDrawFlags_None, 1.f, e_rect_flags::rect_flag_outer_outline ) );

			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_rect,
				std::make_any< rect_draw_object_t >( c_vector_2d( left, top ), c_vector_2d( left + bar_width * factor, top + bar_height ),
			                                         color.get_u32( ), c_color( 0.f, 0.f, 0.f, 1.f ).get_u32( ), true, 0.f,
			                                         ImDrawFlags_::ImDrawFlags_None, 1.f, e_rect_flags::rect_flag_none ) );

			char text[ 32 ] = { };

			if ( being_defused )
				std::snprintf( text, sizeof( text ), "%.1f  (defuse %.1f)", explode_in, defuse_in );
			else
				std::snprintf( text, sizeof( text ), "%.1f", explode_in );

			const auto text_size = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, text );

			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_text,
				std::make_any< text_draw_object_t >( font, c_vector_2d( display_size.x * 0.5f - text_size.x * 0.5f, top - text_size.y - 1.f ), text,
			                                         color.get_u32( ), c_color( 0.f, 0.f, 0.f, 1.f ).get_u32( ), e_text_flags::text_flag_dropshadow ) );
		}
	} );
}
