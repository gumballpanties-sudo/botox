#include "overlays.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"

#include "../../auto_wall/auto_wall.h"
#include "../players/players.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace
{
	constexpr int k_weapon_type_sniper = 5;

	c_base_entity* active_weapon( )
	{
		return g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	}

	const c_weapon_data* weapon_data( c_base_entity* weapon )
	{
		return weapon ? g_interfaces.m_weapon_system->get_weapon_data( weapon->get_item_definition_index( ) ) : nullptr;
	}

	void filled_rect( const float x0, const float y0, const float x1, const float y1, const unsigned int color )
	{
		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_rect,
			std::make_any< rect_draw_object_t >( rect_draw_object_t{ c_vector_2d( x0, y0 ), c_vector_2d( x1, y1 ), color, 0u, true } ) );
	}

	/* edge = picked color, center = same at alpha 0, +0.5 px on the rim like the engine fan */
	void gradient_disc( const float x, const float y, const float radius, const int segments, const ImU32 edge )
	{
		const auto paint = [ = ]( ImDrawList* list ) {
			const ImU32 inner = edge & ~IM_COL32_A_MASK;
			const ImVec2 uv   = ImGui::GetFontTexUvWhitePixel( );
			list->PrimReserve( segments * 3, segments * 3 );
			for ( int i = 0; i < segments; ++i ) {
				const float a0 = 2.f * std::numbers::pi_v< float > * i / segments;
				const float a1 = 2.f * std::numbers::pi_v< float > * ( i + 1 ) / segments;
				list->PrimWriteVtx( ImVec2( x + std::cosf( a0 ) * radius + 0.5f, y + std::sinf( a0 ) * radius + 0.5f ), uv, edge );
				list->PrimWriteVtx( ImVec2( x + std::cosf( a1 ) * radius + 0.5f, y + std::sinf( a1 ) * radius + 0.5f ), uv, edge );
				list->PrimWriteVtx( ImVec2( x, y ), uv, inner );
				const ImDrawIdx base = static_cast< ImDrawIdx >( list->_VtxCurrentIdx - 3 );
				list->PrimWriteIdx( base );
				list->PrimWriteIdx( static_cast< ImDrawIdx >( base + 1 ) );
				list->PrimWriteIdx( static_cast< ImDrawIdx >( base + 2 ) );
			}
		};
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_callback, std::make_any< callback_draw_object_t >( callback_draw_object_t{ paint } ) );
	}
}

bool n_overlays::impl_t::hides_scope( )
{
	if ( !GET_VARIABLE( g_variables.m_remove_scope, bool ) || !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return false;

	const auto data = weapon_data( active_weapon( ) );
	return data && data->m_weapon_type == k_weapon_type_sniper;
}

void n_overlays::impl_t::on_frame_stage_notify( const int stage )
{
	if ( stage != e_client_frame_stage::render_start || !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	this->instant_scope( );
	this->line_of_sight( );
}

void n_overlays::impl_t::instant_scope( )
{
	static const unsigned int fov_start = g_netvars[ HASH_BT( "CBasePlayer->m_iFOVStart" ) ].m_offset;
	if ( !GET_VARIABLE( g_variables.m_instant_scope, bool ) || !fov_start || !g_ctx.m_local->is_alive( ) )
		return;

	const int fov = g_ctx.m_local->get_zoom_fov( ) ? g_ctx.m_local->get_zoom_fov( ) : g_ctx.m_local->get_default_fov( );
	int& start    = *reinterpret_cast< int* >( reinterpret_cast< std::uintptr_t >( g_ctx.m_local ) + fov_start );
	if ( fov > 0 && start != fov )
		start = fov;
}

void n_overlays::impl_t::line_of_sight( )
{
	static const unsigned int eye_angles = g_netvars[ HASH_BT( "CCSPlayer->m_angEyeAngles[0]" ) ].m_offset;
	if ( !GET_VARIABLE( g_variables.m_players_line_of_sight, bool ) || !eye_angles || !g_interfaces.m_debug_overlay || !n_players::esp_on( ) )
		return;

	/* duration 0 lives until the overlay tick moves: once a tick, a paused demo never stacks lines */
	const int tick = g_interfaces.m_global_vars_base->m_tick_count;
	if ( tick == this->m_sight_tick )
		return;
	this->m_sight_tick = tick;

	const c_color color  = GET_VARIABLE( g_variables.m_players_line_of_sight_color, c_color );
	const bool teammates = GET_VARIABLE( g_variables.m_players_teammates, bool );

	for ( int index = 1; index <= g_interfaces.m_global_vars_base->m_max_clients; ++index ) {
		c_base_entity* const player = g_interfaces.m_client_entity_list->get< c_base_entity >( index );
		if ( !player || player == g_ctx.m_local || player->is_dormant( ) || !player->is_player( ) || !player->is_alive( ) || player->get_health( ) <= 0 )
			continue;

		if ( !teammates && !player->is_enemy( g_ctx.m_local ) )
			continue;

		const float duck   = player->get_duck_amount( );
		const float smooth = duck * duck * 3.f - duck * duck * 2.f * duck;
		const c_vector start = player->get_abs_origin( ) + c_vector( 0.f, 0.f, 46.f + 18.f * ( 1.f - smooth ) );

		const float* const angles = reinterpret_cast< const float* >( reinterpret_cast< std::uintptr_t >( player ) + eye_angles );
		c_vector forward{ };
		g_math.angle_vectors( c_angle( angles[ 0 ], angles[ 1 ], 0.f ), &forward );

		trace_t trace{ };
		c_trace_filter_world filter{ };
		g_interfaces.m_engine_trace->trace_ray( ray_t( start, start + forward * 2048.f ), e_contents::contents_solid, &filter, &trace );

		g_interfaces.m_debug_overlay->add_line_overlay_alpha( start, trace.m_end, static_cast< int >( color[ 0 ] ), static_cast< int >( color[ 1 ] ),
		                                                      static_cast< int >( color[ 2 ] ), static_cast< int >( color[ 3 ] ), false, 0.f );
	}
}

void n_overlays::impl_t::spread_circle( )
{
	const bool recoil     = GET_VARIABLE( g_variables.m_recoil_overlay, bool );
	const bool inaccuracy = GET_VARIABLE( g_variables.m_inaccuracy_overlay, bool );
	if ( !recoil && !inaccuracy )
		return;

	c_base_entity* const weapon = active_weapon( );
	const auto data             = weapon_data( weapon );
	if ( !data || !data->is_gun( ) )
		return;

	const float height = g_ctx.m_height;
	float x = g_render.to_overlay_x( std::floorf( g_ctx.m_width * 0.5f ) ), y = std::floorf( height * 0.5f );

	bool punched = false;
	if ( recoil ) {
		static const unsigned int recoil_index = g_netvars[ HASH_BT( "CWeaponCSBase->m_flRecoilIndex" ) ].m_offset;
		const float index = recoil_index ? *reinterpret_cast< float* >( reinterpret_cast< std::uintptr_t >( weapon ) + recoil_index ) : 1.f;
		const c_angle punch = g_ctx.m_local->get_punch( ) * g_convars.float_or( HASH_BT( "weapon_recoil_scale" ), 2.f );

		if ( index >= 1.f && punch.m_x * punch.m_x + punch.m_y * punch.m_y > 0.001f ) {
			c_angle view{ };
			g_interfaces.m_engine_client->get_view_angles( view );

			c_vector forward{ };
			g_math.angle_vectors( view + punch, &forward );

			c_vector_2d screen{ };
			if ( g_render.world_to_screen( g_ctx.m_local->get_eye_position( ) + forward * 10000.f, screen ) ) {
				const float dx = std::truncf( screen.m_x - x ), dy = std::truncf( screen.m_y - y );
				if ( std::fabsf( dx ) >= 2.f )
					x += dx;
				if ( std::fabsf( dy ) >= 2.f )
					y += dy;
				punched = true;
			}
		}
	}

	const ImU32 color = GET_VARIABLE( g_variables.m_inaccuracy_overlay_color, c_color ).get_u32( );

	if ( !inaccuracy ) {
		if ( punched )
			gradient_disc( x, y, 10.f, 31, color );
		return;
	}

	if ( g_convars.float_or( HASH_BT( "weapon_accuracy_nospread" ), 0.f ) != 0.f )
		return;

	/* CWeaponCSBase::GetReticleWeaponSpread: YRES( spread * 320 / tan( GetFOV / 2 ) ), GetFOV = m_iFOV or default */
	const int zoom     = g_ctx.m_local->get_zoom_fov( );
	const float fov    = static_cast< float >( zoom > 0 ? zoom : g_ctx.m_local->get_default_fov( ) );
	if ( fov <= 0.f || fov >= 180.f )
		return;

	const float spread = weapon->get_inaccuracy( ) + weapon->get_spread( );
	float radius       = ( height / 480.f ) * ( spread * 320.f ) / std::tanf( fov * std::numbers::pi_v< float > / 180.f * 0.5f );
	if ( g_interfaces.m_input && g_interfaces.m_input->m_camera_in_third_person )
		radius *= 0.9f;

	if ( !( radius >= 4.f ) )
		return;

	gradient_disc( x, y, radius, std::min( static_cast< int >( radius * std::numbers::pi_v< float > ), 512 ), color );
}

void n_overlays::impl_t::penetration_reticle( )
{
	if ( !GET_VARIABLE( g_variables.m_penetration_reticle, bool ) )
		return;

	c_base_entity* const weapon = active_weapon( );
	const auto data             = weapon_data( weapon );
	if ( !data || !data->is_gun( ) )
		return;

	const int tick = g_interfaces.m_global_vars_base->m_tick_count;
	if ( tick != this->m_reticle_tick ) {
		this->m_reticle_tick = tick;

		c_angle view{ };
		g_interfaces.m_engine_client->get_view_angles( view );

		fire_bullet_data_t bullet{ };
		bullet.m_position = g_ctx.m_local->get_eye_position( );
		g_math.angle_vectors( view, &bullet.m_direction );

		const bool hit = g_auto_wall.simulate_fire_bullet( weapon, bullet );
		if ( hit && bullet.m_current_damage > 0.f )
			this->m_reticle_color = IM_COL32( 18, 96, 255, 255 );
		else if ( bullet.m_penetrate_count < 4 && bullet.m_current_damage >= 1.f )
			this->m_reticle_color = IM_COL32( 0, 255, 50, 255 );
		else
			this->m_reticle_color = IM_COL32( 255, 0, 50, 255 );
	}

	const float cx = g_render.to_overlay_x( std::floorf( g_ctx.m_width * 0.5f ) ), cy = std::floorf( g_ctx.m_height * 0.5f );
	filled_rect( cx - 1.f, cy - 1.f, cx + 2.f, cy + 2.f, IM_COL32( 0, 0, 0, 150 ) );
	filled_rect( cx, cy - 1.f, cx + 1.f, cy + 2.f, this->m_reticle_color );
	filled_rect( cx - 1.f, cy, cx + 2.f, cy + 1.f, this->m_reticle_color );
}

void n_overlays::impl_t::scope_lines( )
{
	if ( !this->hides_scope( ) || !g_ctx.m_local->is_scoped( ) )
		return;

	const float width = g_ctx.m_width, height = g_ctx.m_height;
	const float cx = g_render.to_overlay_x( std::floorf( width * 0.5f ) ), cy = std::floorf( height * 0.5f );
	filled_rect( cx, 0.f, cx + 1.f, height, IM_COL32( 0, 0, 0, 255 ) );
	filled_rect( g_render.to_overlay_x( 0.f ), cy, g_render.to_overlay_x( width ), cy + 1.f, IM_COL32( 0, 0, 0, 255 ) );
}

void n_overlays::impl_t::on_paint_traverse( )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	this->scope_lines( );
	this->spread_circle( );
	this->penetration_reticle( );
}
