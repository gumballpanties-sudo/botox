#include "movement_internal.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <cwchar>
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include "../../dependencies/imgui/imgui.h"
#include "../../dependencies/json/json.hpp"
#include "../chat_hud.h"
#include "texturebug.h"
#include "edgebug.h"

float LerpFloat( float a, float b, float t )
{
	return a + ( b - a ) * t;
}
std::vector< points_check_t > m_bounce_points_check{ };
std::vector< points_check_t > m_points_check{ };

namespace
{
	[[nodiscard]] std::string pixelsurf_points_file( )
	{
		return ( g_config.m_path / "pixelsurf_points.cfg" ).string( );
	}

	[[nodiscard]] std::string bounce_points_file( )
	{
		return ( g_config.m_path / "bounce_points.cfg" ).string( );
	}

	nlohmann::json serialize_point_entry( const points_check_t& point )
	{
		nlohmann::json entry              = { };
		entry[ "pos" ]                    = { point.pos.m_x, point.pos.m_y, point.pos.m_z };
		entry[ "map" ]                    = point.map;
		entry[ "jump" ]                   = point.jump;
		entry[ "minijump" ]               = point.minijump;
		entry[ "longjump" ]               = point.longjump;
		entry[ "jumpbug" ]                = point.jumpbug;
		entry[ "crouch_hop" ]             = point.crouch_hop;
		entry[ "mini_crouch_hop" ]        = point.mini_crouch_hop;
		entry[ "c_jump" ]                 = point.c_jump;
		entry[ "c_minijump" ]             = point.c_minijump;
		entry[ "c_longjump" ]             = point.c_longjump;
		entry[ "c_jumpbug" ]              = point.c_jumpbug;
		entry[ "c_crouch_hop" ]           = point.c_crouch_hop;
		entry[ "c_mini_crouch_hop" ]      = point.c_mini_crouch_hop;
		entry[ "high_crouch_jump" ]       = point.high_crouch_jump;
		entry[ "c_high_crouch_jump" ]     = point.c_high_crouch_jump;
		entry[ "active" ]                 = point.active;
		entry[ "radius" ]                 = point.radius;
		entry[ "delta_strafe" ]           = point.delta_strafe;
		entry[ "selected_preset" ]        = point.selected_preset;
		entry[ "advanced_enabled" ]       = point.advanced_enabled;
		entry[ "selected_advanced" ]      = point.selected_advanced;
		entry[ "first_jump_radius" ]      = point.first_jump_radius;
		entry[ "use_custom_first_jump_radius" ] = point.use_custom_first_jump_radius;
		entry[ "enable_restricted_binds" ]      = point.enable_restricted_binds;
		entry[ "selected_restricted_bind" ]     = point.selected_restricted_bind;
		entry[ "restricted_binds_radius" ]      = point.restricted_binds_radius;
		entry[ "is_free_point" ]                = point.is_free_point;
		entry[ "point_type" ]                   = point.point_type;
		entry[ "show_3d_radius" ]               = point.show_3d_radius;
		return entry;
	}

	void deserialize_point_entry( const nlohmann::json& entry, points_check_t& point )
	{
		if ( const auto pos_it = entry.find( "pos" ); pos_it != entry.end( ) && pos_it->is_array( ) && pos_it->size( ) == 3 ) {
			point.pos.m_x = pos_it->at( 0 ).get< float >( );
			point.pos.m_y = pos_it->at( 1 ).get< float >( );
			point.pos.m_z = pos_it->at( 2 ).get< float >( );
		}

		point.map                   = entry.value( "map", std::string( ) );
		point.jump                  = entry.value( "jump", true );
		point.minijump              = entry.value( "minijump", true );
		point.longjump              = entry.value( "longjump", true );
		point.jumpbug               = entry.value( "jumpbug", true );
		point.crouch_hop            = entry.value( "crouch_hop", true );
		point.mini_crouch_hop       = entry.value( "mini_crouch_hop", true );
		point.c_jump                = entry.value( "c_jump", true );
		point.c_minijump            = entry.value( "c_minijump", true );
		point.c_longjump            = entry.value( "c_longjump", true );
		point.c_jumpbug             = entry.value( "c_jumpbug", true );
		point.c_crouch_hop          = entry.value( "c_crouch_hop", true );
		point.c_mini_crouch_hop     = entry.value( "c_mini_crouch_hop", true );
		point.high_crouch_jump      = entry.value( "high_crouch_jump", true );
		point.c_high_crouch_jump    = entry.value( "c_high_crouch_jump", true );
		point.active                = entry.value( "active", true );
		point.radius                = entry.value( "radius", 300.f );
		point.delta_strafe          = entry.value( "delta_strafe", 0.f );
		point.selected_preset       = entry.value( "selected_preset", 0 );
		point.advanced_enabled      = entry.value( "advanced_enabled", false );
		point.selected_advanced     = entry.value( "selected_advanced", 0 );
		point.first_jump_radius     = entry.value( "first_jump_radius", 250.f );
		point.use_custom_first_jump_radius = entry.value( "use_custom_first_jump_radius", false );
		point.enable_restricted_binds      = entry.value( "enable_restricted_binds", false );
		point.selected_restricted_bind     = entry.value( "selected_restricted_bind", 0 );
		point.restricted_binds_radius      = entry.value( "restricted_binds_radius", 200.f );
		point.is_free_point                = entry.value( "is_free_point", false );
		point.point_type                   = entry.value( "point_type", 0 );
		point.show_3d_radius               = entry.value( "show_3d_radius", false );

		point.currentScale  = 0.f;
		point.open_settings = false;
	}

	void save_points_file( const std::string& path, const std::vector< points_check_t >& points )
	{
		nlohmann::json out = nlohmann::json::array( );
		for ( const auto& point : points )
			out.push_back( serialize_point_entry( point ) );

		std::ofstream file( path, std::ios::out | std::ios::trunc );
		if ( !file.good( ) )
			return;

		try {
			file << out.dump( 4 );
		} catch ( const nlohmann::detail::exception& ) {
		}
	}

	void load_points_file( const std::string& path, std::vector< points_check_t >& points )
	{
		points.clear( );

		std::ifstream file( path, std::ios::in );
		if ( !file.good( ) )
			return;

		const nlohmann::json in = nlohmann::json::parse( file, nullptr, false );
		if ( in.is_discarded( ) || !in.is_array( ) )
			return;

		for ( const auto& entry : in ) {
			if ( !entry.is_object( ) )
				continue;

			points_check_t point{ };
			deserialize_point_entry( entry, point );
			points.push_back( point );
		}
	}
}

void n_movement::impl_t::load_pixelsurf_points( )
{
	load_points_file( pixelsurf_points_file( ), m_points_check );
	load_points_file( bounce_points_file( ), m_bounce_points_check );
}

void n_movement::impl_t::save_pixelsurf_points( )
{
	save_points_file( pixelsurf_points_file( ), m_points_check );
	save_points_file( bounce_points_file( ), m_bounce_points_check );
}

bool point_menu_is_opened( )
{
	/* stale-flag guard: a stuck open_settings wedges input at team select; dead = no popup anyway */
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return false;

	for ( size_t i = 0; i < m_bounce_points_check.size( ); ++i ) {
		points_check_t& point = m_bounce_points_check[ i ];
		if ( point.open_settings )
			return true;
	}
	for ( size_t i = 0; i < m_points_check.size( ); ++i ) {
		points_check_t& point = m_points_check[ i ];
		if ( point.open_settings )
			return true;
	}
	return false;
}
bool InCrosshair( float x, float y, float radius )
{
	float centerX = g_ctx.m_width / 2;
	float centerY = g_ctx.m_height / 2;
	float dx      = centerX - x;
	float dy      = centerY - y;
	return ( dx * dx + dy * dy ) <= ( radius * radius );
}
bool detect_opened_points = false;
unsigned long long gloabal_settings_timer = 0;
/* g_input, not GetAsyncKeyState: an Enter typed into chat / console / menus never reads down there */
bool isEnterPressed( )
{
	static bool keyWasDown = false;
	if ( g_input.is_key_down( VK_RETURN ) ) {
		keyWasDown = true;
	} else if ( keyWasDown ) {
		keyWasDown = false;
		return true;
	}
	return false;
}
static bool probe_surf_dir( const c_vector& pos, c_vector& out_dir )
{
	constexpr float reach     = 48.f;
	constexpr int ray_count   = 16;
	constexpr float normal_z  = 0.3f;
	float best_fraction       = 1.f;
	c_vector best_normal{ };
	bool found = false;
	const float heights[ 2 ] = { 0.f, -4.f };
	for ( float dz : heights ) {
		const c_vector start( pos.m_x, pos.m_y, pos.m_z + dz );
		for ( int i = 0; i < ray_count; ++i ) {
			const float a = ( 2.f * 3.14159265f / ray_count ) * i;
			const c_vector end( start.m_x + std::cos( a ) * reach, start.m_y + std::sin( a ) * reach, start.m_z );
			trace_t t{ };
			ray_t ray( start, end );
			c_trace_filter_trace_type_everything_filter_props filter;
			g_interfaces.m_engine_trace->trace_ray( ray, mask_playersolid, &filter, &t );
			if ( t.m_fraction >= 1.f || t.m_start_solid )
				continue;
			if ( std::fabs( t.m_plane.m_normal.m_z ) > normal_z )
				continue;
			if ( t.m_fraction >= best_fraction )
				continue;
			best_fraction = t.m_fraction;
			best_normal   = t.m_plane.m_normal;
			found         = true;
		}
		if ( found )
			break;
	}
	if ( !found )
		return false;
	c_vector dir( -best_normal.m_y, best_normal.m_x, 0.f );
	const float len = dir.length_2d( );
	if ( len < 0.0001f )
		return false;
	out_dir = dir / len;
	return true;
}
static bool project_world_axis( const c_vector& pos, const c_vector& dir, float probe_len, float& out_x, float& out_y, float& out_ratio )
{
	auto span = [ & ]( const c_vector& axis, float& sx, float& sy ) -> bool {
		c_vector_2d a, b;
		if ( !g_render.world_to_screen( pos + axis * probe_len, a ) || !g_render.world_to_screen( pos - axis * probe_len, b ) )
			return false;
		sx = a.m_x - b.m_x;
		sy = a.m_y - b.m_y;
		return true;
	};
	float dx, dy;
	if ( !span( dir, dx, dy ) )
		return false;
	float xx, xy, yx, yy, zx, zy;
	if ( !span( c_vector( 1.f, 0.f, 0.f ), xx, xy ) || !span( c_vector( 0.f, 1.f, 0.f ), yx, yy ) || !span( c_vector( 0.f, 0.f, 1.f ), zx, zy ) )
		return false;
	const float full = std::sqrt( ( xx * xx + xy * xy + yx * yx + yy * yy + zx * zx + zy * zy ) * 0.5f );
	if ( full < 0.0001f )
		return false;
	const float len = std::sqrt( dx * dx + dy * dy );
	out_ratio       = std::clamp( len / full, 0.f, 1.f );
	if ( len < 0.0001f ) {
		out_x = 1.f;
		out_y = 0.f;
		return true;
	}
	out_x = dx / len;
	out_y = dy / len;
	return true;
}
static float arrow_length_scale( float ratio )
{
	return 0.15f + 0.85f * ratio;
}
static float arrow_fade( float ratio )
{
	float t = std::clamp( ( ratio - 0.10f ) / 0.30f, 0.f, 1.f );
	return t * t * ( 3.f - 2.f * t );
}
static void draw_double_arrow( const c_vector_2d& centre, float half, float dir_x, float dir_y, unsigned int color, float thickness )
{
	if ( half <= 0.f )
		return;
	const float dlen = std::sqrt( dir_x * dir_x + dir_y * dir_y );
	if ( dlen < 0.0001f )
		return;
	const float ax = dir_x / dlen, ay = dir_y / dlen;
	const float px = -ay, py = ax;
	const float head = std::min( std::clamp( half * 0.4f, 2.f, 6.f ), half * 0.5f );
	auto pt          = [ & ]( float along, float across ) -> c_vector_2d {
		c_vector_2d p;
		p.m_x = centre.m_x + ax * along + px * across;
		p.m_y = centre.m_y + ay * along + py * across;
		return p;
	};
	auto line = [ & ]( const c_vector_2d& a, const c_vector_2d& b ) {
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( line_draw_object_t{ a, b, color, thickness } ) );
	};
	line( pt( -half, 0.f ), pt( half, 0.f ) );
	line( pt( -half, 0.f ), pt( -half + head, -head ) );
	line( pt( -half, 0.f ), pt( -half + head, head ) );
	line( pt( half, 0.f ), pt( half - head, -head ) );
	line( pt( half, 0.f ), pt( half - head, head ) );
}
struct point_edit_t {
	bool active                       = true;
	float radius                      = 300.f;
	bool show_3d_radius               = false;
	float delta_strafe                = 0.f;
	bool jump                         = true;
	bool c_jump                       = true;
	bool minijump                     = true;
	bool c_minijump                   = true;
	bool longjump                     = true;
	bool c_longjump                   = true;
	bool jumpbug                      = true;
	bool c_jumpbug                    = true;
	bool crouch_hop                   = true;
	bool c_crouch_hop                 = true;
	bool mini_crouch_hop              = true;
	bool c_mini_crouch_hop            = true;
	bool high_crouch_jump             = true;
	bool c_high_crouch_jump           = true;
	bool use_custom_first_jump_radius = false;
	float first_jump_radius           = 250.f;
	bool enable_restricted_binds      = false;
	int selected_restricted_bind      = 0;
	float restricted_binds_radius     = 200.f;
};
static point_edit_t s_point_edit{ };
static std::atomic< bool > s_point_edit_visible{ false };
static std::atomic< bool > s_point_edit_apply{ false };
static std::atomic< bool > s_point_edit_close{ false };
static std::atomic< int > s_point_edit_owner{ 0 };

bool point_popup_visible( )
{
	return s_point_edit_visible.load( );
}
static bool s_point_edit_dirty = false;
/* identity of the edited point ( main only ): overlapping points must not both take the edits */
static c_vector s_point_edit_pos{ };

static void point_edit_open( const points_check_t& p, int owner )
{
	s_point_edit_visible = false;
	point_edit_t e{ };
	e.active                       = p.active;
	e.radius                       = p.radius;
	e.show_3d_radius               = p.show_3d_radius;
	e.delta_strafe                 = p.delta_strafe;
	e.jump                         = p.jump;
	e.c_jump                       = p.c_jump;
	e.minijump                     = p.minijump;
	e.c_minijump                   = p.c_minijump;
	e.longjump                     = p.longjump;
	e.c_longjump                   = p.c_longjump;
	e.jumpbug                      = p.jumpbug;
	e.c_jumpbug                    = p.c_jumpbug;
	e.crouch_hop                   = p.crouch_hop;
	e.c_crouch_hop                 = p.c_crouch_hop;
	e.mini_crouch_hop              = p.mini_crouch_hop;
	e.c_mini_crouch_hop            = p.c_mini_crouch_hop;
	e.high_crouch_jump             = p.high_crouch_jump;
	e.c_high_crouch_jump           = p.c_high_crouch_jump;
	e.use_custom_first_jump_radius = p.use_custom_first_jump_radius;
	e.first_jump_radius            = p.first_jump_radius;
	e.enable_restricted_binds      = p.enable_restricted_binds;
	e.selected_restricted_bind     = p.selected_restricted_bind;
	e.restricted_binds_radius      = p.restricted_binds_radius;
	s_point_edit         = e;
	s_point_edit_pos     = p.pos;
	s_point_edit_owner   = owner;
	s_point_edit_apply   = false;
	s_point_edit_close   = false;
	s_point_edit_visible = true;
}
/* main thread, per frame per open point: take edits, obey close. no file write here ( slider drag = disk every frame ) */
static void point_edit_sync( points_check_t& p, int owner )
{
	if ( s_point_edit_owner.load( ) != owner || p.pos.m_x != s_point_edit_pos.m_x || p.pos.m_y != s_point_edit_pos.m_y ||
	     p.pos.m_z != s_point_edit_pos.m_z )
		return;
	if ( s_point_edit_apply.exchange( false ) ) {
		const point_edit_t e             = s_point_edit;
		p.active                         = e.active;
		p.radius                         = e.radius;
		p.show_3d_radius                 = e.show_3d_radius;
		p.delta_strafe                   = e.delta_strafe;
		p.jump                           = e.jump;
		p.c_jump                         = e.c_jump;
		p.minijump                       = e.minijump;
		p.c_minijump                     = e.c_minijump;
		p.longjump                       = e.longjump;
		p.c_longjump                     = e.c_longjump;
		p.jumpbug                        = e.jumpbug;
		p.c_jumpbug                      = e.c_jumpbug;
		p.crouch_hop                     = e.crouch_hop;
		p.c_crouch_hop                   = e.c_crouch_hop;
		p.mini_crouch_hop                = e.mini_crouch_hop;
		p.c_mini_crouch_hop              = e.c_mini_crouch_hop;
		p.high_crouch_jump               = e.high_crouch_jump;
		p.c_high_crouch_jump             = e.c_high_crouch_jump;
		p.use_custom_first_jump_radius   = e.use_custom_first_jump_radius;
		p.first_jump_radius              = e.first_jump_radius;
		p.enable_restricted_binds        = e.enable_restricted_binds;
		p.selected_restricted_bind       = e.selected_restricted_bind;
		p.restricted_binds_radius        = e.restricted_binds_radius;
		s_point_edit_dirty               = true;
	}
	if ( s_point_edit_close.exchange( false ) ) {
		p.open_settings      = false;
		s_point_edit_visible = false;
	}
}
void RenderPoints( std::vector< points_check_t >& points, const c_vector& playerPos, const std::string& currentMap )
{
	if ( ImGui::GetCurrentContext( ) == nullptr )
		return;
	float dt                       = ImGui::GetIO( ).DeltaTime;
	const float scaleSpeed         = 10.0f;
	/* g_input: a Backspace typed into chat / console / the popup must not delete the hovered point */
	static bool isBackspacePressed = false;
	if ( g_input.is_key_down( VK_BACK ) ) {
		isBackspacePressed = true;
	} else {
		isBackspacePressed = false;
	}
	auto accentclr = ImGui::GetStyle( ).Colors[ ImGuiCol_::ImGuiCol_Accent ];
	if ( GET_VARIABLE( g_variables.m_pixel_surf_assist, bool ) && GET_VARIABLE( g_variables.m_pixel_surf_assist_show_points, bool ) ) {
		const float maxDrawDist  = std::max( GET_VARIABLE( g_variables.m_pixel_surf_assist_point_dist, float ), 1.f );
		const float fadeStartDist = maxDrawDist * 0.6f;
		for ( size_t i = 0; i < points.size( ); ++i ) {
			points_check_t& point = points[ i ];
			/* every skip path must drop open_settings, else point_menu_is_opened eats all input */
			if ( point.map != currentMap ) {
				point.open_settings = false;
				continue;
			}
			c_vector_2d screenPos;
			if ( !g_render.world_to_screen( point.pos, screenPos ) ) {
				point.open_settings = false;
				continue;
			}
			float distance = playerPos.dist_to( point.pos );
			if ( distance > maxDrawDist ) {
				point.open_settings = false;
				continue;
			}
			float baseRadius = 11.0f;
			if ( distance > 300.0f ) {
				float t    = std::clamp( ( distance - 300.0f ) / 300.0f, 0.0f, 1.0f );
				baseRadius = LerpFloat( 11.0f, 4.0f, t );
			}
			baseRadius                   = std::max( baseRadius, 3.0f );
			const float outlineThickness = 0.5f;
			float alphaMultiplier        = 1.0f;
			if ( distance > fadeStartDist ) {
				alphaMultiplier = std::clamp( ( maxDrawDist - distance ) / ( maxDrawDist - fadeStartDist ), 0.0f, 1.0f );
			}
			const int outlineA = static_cast< int >( 255.0f * alphaMultiplier );
			unsigned int outlineColor;
			if ( point.is_free_point ) {
				outlineColor = c_color( 250, 120, 120, outlineA ).get_u32( );
			} else if ( point.active ) {
				outlineColor = c_color( static_cast< int >( accentclr.x * 255.f ), static_cast< int >( accentclr.y * 255.f ),
				                        static_cast< int >( accentclr.z * 255.f ), outlineA )
				                   .get_u32( );
			} else {
				outlineColor = c_color( 100, 100, 100, outlineA ).get_u32( );
			}
			int fillAlpha;
			unsigned int fillColor;
			if ( point.is_free_point ) {
				fillAlpha = static_cast< int >( 255.0f * alphaMultiplier );
				fillColor = c_color( 250, 120, 120, fillAlpha ).get_u32( );
			} else {
				fillAlpha = static_cast< int >( 100.0f * alphaMultiplier );
				fillColor = c_color( 0, 0, 0, fillAlpha ).get_u32( );
			}
			bool isHovered        = InCrosshair( screenPos.m_x, screenPos.m_y, baseRadius );
			float targetScale     = isHovered ? 1.5f : 1.0f;
			point.currentScale    = LerpFloat( point.currentScale, targetScale, dt * scaleSpeed );
			float effectiveRadius = baseRadius * point.currentScale;
			if ( isHovered ) {
				if ( !point.open_settings && isEnterPressed( ) ) {
					gloabal_settings_timer = GetTickCount64( ) + 500;
					point.open_settings    = true;
					point_edit_open( point, 0 );
				}
			} else {
				point.open_settings = false;
			}
			if ( point.open_settings )
				point_edit_sync( point, 0 );
			filled_circle_draw_object_t filledCircle{ screenPos, effectiveRadius, fillColor, 32 };
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle, std::make_any< filled_circle_draw_object_t >( filledCircle ) );
			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_circle,
				std::make_any< circle_draw_object_t >( circle_draw_object_t{ screenPos, effectiveRadius, outlineColor, 32, outlineThickness } ) );
			const int crossAlpha = static_cast< int >( 200.0f * alphaMultiplier );
			unsigned int crossColor;
			if ( point.is_free_point )
				crossColor = c_color( 250, 120, 120, crossAlpha ).get_u32( );
			else if ( !point.active )
				crossColor = c_color( 150, 150, 150, crossAlpha ).get_u32( );
			else
				crossColor = c_color( static_cast< int >( accentclr.x * 255.f ), static_cast< int >( accentclr.y * 255.f ),
				                      static_cast< int >( accentclr.z * 255.f ), crossAlpha )
				                 .get_u32( );
			if ( point.surf_dir_state == 0 ) {
				c_vector probed;
				if ( probe_surf_dir( point.pos, probed ) ) {
					point.surf_dir       = probed;
					point.surf_dir_state = 1;
				} else {
					point.surf_dir_state = 2;
				}
			}
			float arrowDirX = 1.0f, arrowDirY = 0.0f, arrowRatio = 1.0f;
			if ( point.surf_dir_state == 1 ) {
				float dx, dy, ratio;
				if ( project_world_axis( point.pos, point.surf_dir, 16.f, dx, dy, ratio ) ) {
					arrowDirX  = dx;
					arrowDirY  = dy;
					arrowRatio = ratio;
				}
			}
			const float arrowHalf = effectiveRadius * 0.8f * arrow_length_scale( arrowRatio );
			const int arrowAlpha  = static_cast< int >( ( ( crossColor >> 24 ) & 0xFFu ) * arrow_fade( arrowRatio ) );
			if ( arrowAlpha > 0 ) {
				const unsigned int arrowColor = ( crossColor & 0x00FFFFFFu ) | ( static_cast< unsigned int >( arrowAlpha ) << 24 );
				draw_double_arrow( screenPos, arrowHalf, arrowDirX, arrowDirY, arrowColor, 1.0f );
			}
			if ( isHovered && point.show_3d_radius ) {
				float display_radius      = point.radius;
				const int circle_segments = 64;
				const float angle_step    = 2.0f * 3.14159f / circle_segments;
				for ( int j = 0; j < circle_segments; ++j ) {
					float angle1  = j * angle_step;
					float angle2  = ( j + 1 ) * angle_step;
					c_vector pos1 = point.pos + c_vector( cos( angle1 ) * display_radius, sin( angle1 ) * display_radius, 0.f );
					c_vector pos2 = point.pos + c_vector( cos( angle2 ) * display_radius, sin( angle2 ) * display_radius, 0.f );
					c_vector_2d screen_pos1, screen_pos2;
					if ( g_render.world_to_screen( pos1, screen_pos1 ) && g_render.world_to_screen( pos2, screen_pos2 ) ) {
						unsigned int white_color = c_color( 255, 255, 255, 200 ).get_u32( );
						line_draw_object_t radius_line{ screen_pos1, screen_pos2, white_color, 1.0f };
						g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( radius_line ) );
					}
				}
				if ( point.use_custom_first_jump_radius ) {
					float first_jump_radius = point.first_jump_radius;
					for ( int j = 0; j < circle_segments; ++j ) {
						float angle1  = j * angle_step;
						float angle2  = ( j + 1 ) * angle_step;
						c_vector pos1 = point.pos + c_vector( cos( angle1 ) * first_jump_radius, sin( angle1 ) * first_jump_radius, 0.f );
						c_vector pos2 = point.pos + c_vector( cos( angle2 ) * first_jump_radius, sin( angle2 ) * first_jump_radius, 0.f );
						c_vector_2d screen_pos1, screen_pos2;
						if ( g_render.world_to_screen( pos1, screen_pos1 ) && g_render.world_to_screen( pos2, screen_pos2 ) ) {
							unsigned int green_color = c_color( 0, 255, 0, 200 ).get_u32( );
							line_draw_object_t radius_line{ screen_pos1, screen_pos2, green_color, 1.0f };
							g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( radius_line ) );
						}
					}
				}
				if ( point.enable_restricted_binds && point.selected_restricted_bind != 0 ) {
					float restricted_radius = point.restricted_binds_radius;
					for ( int j = 0; j < circle_segments; ++j ) {
						float angle1  = j * angle_step;
						float angle2  = ( j + 1 ) * angle_step;
						c_vector pos1 = point.pos + c_vector( cos( angle1 ) * restricted_radius, sin( angle1 ) * restricted_radius, 0.f );
						c_vector pos2 = point.pos + c_vector( cos( angle2 ) * restricted_radius, sin( angle2 ) * restricted_radius, 0.f );
						c_vector_2d screen_pos1, screen_pos2;
						if ( g_render.world_to_screen( pos1, screen_pos1 ) && g_render.world_to_screen( pos2, screen_pos2 ) ) {
							unsigned int red_color = c_color( 255, 0, 0, 200 ).get_u32( );
							line_draw_object_t radius_line{ screen_pos1, screen_pos2, red_color, 1.0f };
							g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( radius_line ) );
						}
					}
				}
			}
			if ( isHovered && isBackspacePressed ) {
				points.erase( points.begin( ) + i );
				save_points_file( pixelsurf_points_file( ), m_points_check );
				break;
			}
		}
	} else {
		for ( auto& point : points )
			point.open_settings = false;
	}
	if ( GET_VARIABLE( g_variables.m_bouncee_assist, bool ) ) {
		for ( size_t i = 0; i < m_bounce_points_check.size( ); ++i ) {
			points_check_t& point = m_bounce_points_check[ i ];
			/* same stale-flag rule as the pixelsurf loop above */
			if ( point.map != currentMap ) {
				point.open_settings = false;
				continue;
			}
			float assist_height   = GET_VARIABLE( g_variables.m_pixel_surf_assist_render_height, float );
			c_vector adjusted_pos = point.pos;
			adjusted_pos.m_z += assist_height;
			c_vector_2d screenPos;
			if ( !g_render.world_to_screen( adjusted_pos, screenPos ) ) {
				point.open_settings = false;
				continue;
			}
			float distance = playerPos.dist_to( adjusted_pos );
			if ( distance > 1100.f ) {
				point.open_settings = false;
				continue;
			}
			float baseRadius = 10.0f;
			if ( distance > 275.0f ) {
				if ( distance <= 650.0f ) {
					float t    = std::clamp( ( distance - 275.0f ) / 375.0f, 0.0f, 1.0f );
					t          = t * t * ( 3.0f - 2.0f * t );
					baseRadius = LerpFloat( 10.0f, 2.0f, t );
				} else {
					baseRadius = 2.0f;
				}
			}
			baseRadius             = std::max( baseRadius, 3.0f );
			float outlineThickness = 0.5f;
			if ( distance > 200.0f ) {
				float t          = std::clamp( ( distance - 200.0f ) / 200.0f, 0.0f, 1.0f );
				outlineThickness = LerpFloat( 0.5f, 1.0f, t );
			}
			const float bFadeStart = 1100.0f * 0.6f;
			float alphaMultiplier  = 1.0f;
			if ( distance > bFadeStart )
				alphaMultiplier = std::clamp( ( 1100.0f - distance ) / ( 1100.0f - bFadeStart ), 0.0f, 1.0f );
			unsigned int outlineColor;
			bool inRadius = distance <= point.radius;
			if ( point.is_free_point ) {
				int outlineA = static_cast< int >( 255.0f * alphaMultiplier );
				outlineColor = c_color( 250, 120, 120, outlineA ).get_u32( );
			} else if ( !point.active ) {
				int outlineA = static_cast< int >( 255.0f * alphaMultiplier );
				outlineColor = c_color( 150, 150, 150, outlineA ).get_u32( );
			} else if ( inRadius ) {
				int outlineR = static_cast< int >( accentclr.x * 255.f );
				int outlineG = static_cast< int >( accentclr.y * 255.f );
				int outlineB = static_cast< int >( accentclr.z * 255.f );
				int outlineA = static_cast< int >( 255.0f * alphaMultiplier );
				outlineColor = c_color( outlineR, outlineG, outlineB, outlineA ).get_u32( );
			} else {
				int outlineA = static_cast< int >( 255.0f * alphaMultiplier );
				outlineColor = c_color( 255, 255, 255, outlineA ).get_u32( );
			}
			int fillAlpha;
			unsigned int fillColor;
			if ( point.is_free_point ) {
				fillAlpha = static_cast< int >( 255.0f * alphaMultiplier );
				fillColor = c_color( 250, 120, 120, fillAlpha ).get_u32( );
			} else {
				fillAlpha = static_cast< int >( 100.0f * alphaMultiplier );
				fillColor = c_color( 0, 0, 0, fillAlpha ).get_u32( );
			}
			bool isHovered = InCrosshair( screenPos.m_x, screenPos.m_y, baseRadius );
			if ( isHovered ) {
				if ( !point.open_settings && isEnterPressed( ) ) {
					gloabal_settings_timer = GetTickCount64( ) + 500;
					point.open_settings    = true;
					point_edit_open( point, 1 );
				}
			} else {
				point.open_settings = false;
			}
			if ( point.open_settings )
				point_edit_sync( point, 1 );
			float targetScale     = isHovered ? 1.5f : 1.0f;
			point.currentScale    = LerpFloat( point.currentScale, targetScale, dt * scaleSpeed );
			float effectiveRadius = baseRadius * point.currentScale;
			filled_circle_draw_object_t filledCircle{ screenPos, effectiveRadius, fillColor, 32 };
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle, std::make_any< filled_circle_draw_object_t >( filledCircle ) );
			unsigned int grayColor = c_color( 128, 128, 128, static_cast< int >( 255.0f * alphaMultiplier ) ).get_u32( );
			const int segments     = 32;
			const float angleStep  = 2.0f * 3.14159f / segments;
			for ( int j = 0; j < segments; ++j ) {
				float angle1       = j * angleStep;
				float angle2       = ( j + 1 ) * angleStep;
				bool isGraySegment = ( angle1 >= 0.79f && angle1 <= 2.36f ) || ( angle2 >= 0.79f && angle2 <= 2.36f );
				c_vector_2d pos1, pos2;
				pos1.m_x                  = screenPos.m_x + cos( angle1 ) * effectiveRadius;
				pos1.m_y                  = screenPos.m_y + sin( angle1 ) * effectiveRadius;
				pos2.m_x                  = screenPos.m_x + cos( angle2 ) * effectiveRadius;
				pos2.m_y                  = screenPos.m_y + sin( angle2 ) * effectiveRadius;
				unsigned int segmentColor = isGraySegment ? grayColor : outlineColor;
				line_draw_object_t outlineLine{ pos1, pos2, segmentColor, outlineThickness };
				g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( outlineLine ) );
			}
			const int bArrowAlpha = static_cast< int >( 200.0f * alphaMultiplier );
			unsigned int bArrowColor;
			if ( point.is_free_point )
				bArrowColor = c_color( 250, 120, 120, bArrowAlpha ).get_u32( );
			else if ( !point.active )
				bArrowColor = c_color( 150, 150, 150, bArrowAlpha ).get_u32( );
			else
				bArrowColor = c_color( static_cast< int >( accentclr.x * 255.f ), static_cast< int >( accentclr.y * 255.f ),
				                       static_cast< int >( accentclr.z * 255.f ), bArrowAlpha )
				                  .get_u32( );
			float bDirX = 0.0f, bDirY = -1.0f, bRatio = 1.0f;
			{
				float dx, dy, ratio;
				if ( project_world_axis( adjusted_pos, c_vector( 0.f, 0.f, 1.f ), 16.f, dx, dy, ratio ) ) {
					bDirX  = dx;
					bDirY  = dy;
					bRatio = ratio;
				}
			}
			const float bArrowHalf = effectiveRadius * 0.8f * arrow_length_scale( bRatio );
			const int bFadedAlpha  = static_cast< int >( ( ( bArrowColor >> 24 ) & 0xFFu ) * arrow_fade( bRatio ) );
			if ( bFadedAlpha > 0 ) {
				const unsigned int bFadedColor = ( bArrowColor & 0x00FFFFFFu ) | ( static_cast< unsigned int >( bFadedAlpha ) << 24 );
				draw_double_arrow( screenPos, bArrowHalf, bDirX, bDirY, bFadedColor, 1.0f );
			}
			if ( isHovered && point.show_3d_radius ) {
				float display_radius      = point.radius;
				const int circle_segments = 64;
				const float angle_step    = 2.0f * 3.14159f / circle_segments;
				for ( int j = 0; j < circle_segments; ++j ) {
					float angle1  = j * angle_step;
					float angle2  = ( j + 1 ) * angle_step;
					c_vector pos1 = point.pos + c_vector( cos( angle1 ) * display_radius, sin( angle1 ) * display_radius, 0.f );
					c_vector pos2 = point.pos + c_vector( cos( angle2 ) * display_radius, sin( angle2 ) * display_radius, 0.f );
					c_vector_2d screen_pos1, screen_pos2;
					if ( g_render.world_to_screen( pos1, screen_pos1 ) && g_render.world_to_screen( pos2, screen_pos2 ) ) {
						unsigned int white_color = c_color( 255, 255, 255, 200 ).get_u32( );
						line_draw_object_t radius_line{ screen_pos1, screen_pos2, white_color, 1.0f };
						g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( radius_line ) );
					}
				}
				if ( point.use_custom_first_jump_radius ) {
					float first_jump_radius = point.first_jump_radius;
					for ( int j = 0; j < circle_segments; ++j ) {
						float angle1  = j * angle_step;
						float angle2  = ( j + 1 ) * angle_step;
						c_vector pos1 = point.pos + c_vector( cos( angle1 ) * first_jump_radius, sin( angle1 ) * first_jump_radius, 0.f );
						c_vector pos2 = point.pos + c_vector( cos( angle2 ) * first_jump_radius, sin( angle2 ) * first_jump_radius, 0.f );
						c_vector_2d screen_pos1, screen_pos2;
						if ( g_render.world_to_screen( pos1, screen_pos1 ) && g_render.world_to_screen( pos2, screen_pos2 ) ) {
							unsigned int green_color = c_color( 0, 255, 0, 200 ).get_u32( );
							line_draw_object_t radius_line{ screen_pos1, screen_pos2, green_color, 1.0f };
							g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( radius_line ) );
						}
					}
				}
				if ( point.enable_restricted_binds && point.selected_restricted_bind != 0 ) {
					float restricted_radius = point.restricted_binds_radius;
					for ( int j = 0; j < circle_segments; ++j ) {
						float angle1  = j * angle_step;
						float angle2  = ( j + 1 ) * angle_step;
						c_vector pos1 = point.pos + c_vector( cos( angle1 ) * restricted_radius, sin( angle1 ) * restricted_radius, 0.f );
						c_vector pos2 = point.pos + c_vector( cos( angle2 ) * restricted_radius, sin( angle2 ) * restricted_radius, 0.f );
						c_vector_2d screen_pos1, screen_pos2;
						if ( g_render.world_to_screen( pos1, screen_pos1 ) && g_render.world_to_screen( pos2, screen_pos2 ) ) {
							unsigned int red_color = c_color( 255, 0, 0, 200 ).get_u32( );
							line_draw_object_t radius_line{ screen_pos1, screen_pos2, red_color, 1.0f };
							g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( radius_line ) );
						}
					}
				}
			}
			if ( isHovered && isBackspacePressed ) {
				m_bounce_points_check.erase( m_bounce_points_check.begin( ) + i );
				save_points_file( bounce_points_file( ), m_bounce_points_check );
				break;
			}
		}
	} else {
		for ( auto& point : m_bounce_points_check )
			point.open_settings = false;
	}
	if ( !s_point_edit_visible.load( ) ) {
		for ( auto& p : points )
			p.open_settings = false;
		for ( auto& p : m_bounce_points_check )
			p.open_settings = false;
	}
	if ( !point_menu_is_opened( ) ) {
		s_point_edit_visible = false;
		if ( s_point_edit_dirty ) {
			save_points_file( pixelsurf_points_file( ), m_points_check );
			save_points_file( bounce_points_file( ), m_bounce_points_check );
			s_point_edit_dirty = false;
		}
	}
}
static bool point_popup_enter_pressed( )
{
	/* own edge state: isEnterPressed( ) is polled from the main thread */
	static bool was_down = false;
	if ( GetAsyncKeyState( VK_RETURN ) & 0x8000 ) {
		was_down = true;
		return false;
	}
	if ( was_down ) {
		was_down = false;
		return true;
	}
	return false;
}
void render_point_settings_window( )
{
	if ( !s_point_edit_visible.load( ) )
		return;
	if ( ImGui::GetCurrentContext( ) == nullptr )
		return;
	if ( !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) || !g_ctx.m_local->is_alive( ) ) {
		s_point_edit_visible = false;
		return;
	}
	point_edit_t& e = s_point_edit;
	bool changed    = false;

	constexpr auto background_height = 25.f;
	const auto title_text            = s_point_edit_owner.load( ) == 1 ? "bounce point settings" : "pixelsurf point settings";
	auto* title_font                 = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
	const auto title_text_size = title_font ? title_font->CalcTextSizeA( title_font->FontSize, FLT_MAX, 0.f, title_text ) : ImVec2( 0.f, 11.f );

	ImGui::SetNextWindowPos( ImVec2( ImGui::GetIO( ).DisplaySize.x * 0.5f, ImGui::GetIO( ).DisplaySize.y * 0.5f ), ImGuiCond_::ImGuiCond_Always, ImVec2( 0.5f, 0.5f ) );
	ImGui::SetNextWindowSizeConstraints( ImVec2( 250.f, 0.f ), ImVec2( 250.f, FLT_MAX ) );
	ImGui::Begin( ( "botox-point-settings-ui" ), 0,
	              ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar |
	                  ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_::ImGuiWindowFlags_NoResize |
	                  ImGuiWindowFlags_::ImGuiWindowFlags_NoMove | ImGuiWindowFlags_::ImGuiWindowFlags_AlwaysAutoResize );
	{
		const auto window    = ImGui::GetCurrentWindow( );
		const auto draw_list = window->DrawList;
		const auto size      = window->Size;
		const auto position  = window->Pos;

		[ & ]( ) {
			ImGui::PushClipRect( ImVec2( position.x, position.y ), ImVec2( position.x + size.x, position.y + background_height ), false );
			draw_list->AddRectFilled( ImVec2( position.x, position.y ), ImVec2( position.x + size.x, position.y + background_height ),
			                          ImColor( 25 / 255.f, 25 / 255.f, 25 / 255.f, 1.f ), ImGui::GetStyle( ).WindowRounding,
			                          ImDrawFlags_RoundCornersTop );
			ImGui::PopClipRect( );

			RenderFadedGradientLine( draw_list, ImVec2( position.x, position.y + background_height - 1.f ), ImVec2( size.x, 1.f ),
			                         static_cast< ImColor >( ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent ) ) );

			if ( title_font )
				draw_list->AddText( title_font, title_font->FontSize,
				                    ImVec2( position.x + ( ( size.x - title_text_size.x ) / 2.f ),
				                            position.y + ( ( background_height - title_text_size.y ) / 2.f ) ),
				                    ImColor( 1.f, 1.f, 1.f ), title_text );

			ImGui::PushClipRect( ImVec2( position.x + 1.f, position.y + 1.f ), ImVec2( position.x + size.x - 1.f, position.y + size.y - 1.f ),
			                     false );
			draw_list->AddRect( ImVec2( position.x + 1.f, position.y + 1.f ), ImVec2( position.x + size.x - 1.f, position.y + size.y - 1.f ),
			                    ImColor( 50 / 255.f, 50 / 255.f, 50 / 255.f ), ImGui::GetStyle( ).WindowRounding );
			ImGui::PopClipRect( );
		}( );

		ImGui::SetCursorPosY( 30.f );

		if ( ImGui::Button( e.active ? ( "make inactive" ) : ( "make active" ), ImVec2( -1.f, 20.f ) ) ) {
			e.active = !e.active;
			changed  = true;
		}
		changed |= ImGui::SliderFloat( "radius", &e.radius, 50.f, 1000.f, "%.0f" );
		changed |= ImGui::SliderFloat( "delta strafe", &e.delta_strafe, 0.f, 5.f, "%.2f" );
		changed |= ImGui::Checkbox( "3d radius ring", &e.show_3d_radius );

		changed |= ImGui::Checkbox( "jump", &e.jump );
		changed |= ImGui::Checkbox( "jump (crouch)", &e.c_jump );
		changed |= ImGui::Checkbox( "minijump", &e.minijump );
		changed |= ImGui::Checkbox( "minijump (crouch)", &e.c_minijump );
		changed |= ImGui::Checkbox( "longjump", &e.longjump );
		changed |= ImGui::Checkbox( "longjump (crouch)", &e.c_longjump );
		changed |= ImGui::Checkbox( "jumpbug", &e.jumpbug );
		changed |= ImGui::Checkbox( "jumpbug (crouch)", &e.c_jumpbug );
		changed |= ImGui::Checkbox( "crouch hop", &e.crouch_hop );
		changed |= ImGui::Checkbox( "crouch hop (crouch)", &e.c_crouch_hop );
		changed |= ImGui::Checkbox( "mini crouch hop", &e.mini_crouch_hop );
		changed |= ImGui::Checkbox( "mini crouch hop (crouch)", &e.c_mini_crouch_hop );
		changed |= ImGui::Checkbox( "high crouch jump", &e.high_crouch_jump );
		changed |= ImGui::Checkbox( "high crouch jump (crouch)", &e.c_high_crouch_jump );

		changed |= ImGui::Checkbox( "custom first jump radius", &e.use_custom_first_jump_radius );
		if ( e.use_custom_first_jump_radius ) {
			ImGui::IndentRow( );
			changed |= ImGui::SliderFloat( "first jump radius", &e.first_jump_radius, 50.f, 1000.f, "%.0f" );
		}

		changed |= ImGui::Checkbox( "restricted binds", &e.enable_restricted_binds );
		if ( e.enable_restricted_binds ) {
			ImGui::IndentRow( );
			/* order = is_bind_restricted_in_radius( ): 1 mj, 2 jb, 3 lj, 4 j, 5 ch, 6 mch, 7 hcj. don't reorder */
			changed |= ImGui::Combo( "restricted bind", &e.selected_restricted_bind,
			                         "none\0minijump\0jumpbug\0longjump\0jump\0crouch hop\0mini crouch hop\0high crouch jump\0" );
			ImGui::IndentRow( );
			changed |= ImGui::SliderFloat( "restricted radius", &e.restricted_binds_radius, 50.f, 1000.f, "%.0f" );
		}
	}
	ImGui::End( );

	if ( changed )
		s_point_edit_apply = true;
	if ( GetTickCount64( ) >= gloabal_settings_timer && point_popup_enter_pressed( ) )
		s_point_edit_close = true;
}

std::vector< AnimatedPoint > animated_points;

bool consume_key_edge( key_bind_t& key, bool& was_down )
{
	const bool down = g_input.check_input( &key );
	const bool edge = down && !was_down;

	was_down = down;
	return edge;
}

std::vector< AnimatedPoint > animated_points2;
std::vector< AnimatedPoint > animated_points3;
std::vector< AnimatedPoint > animated_points4;

c_vector round_pos2( c_vector point )
{
	if ( point.m_z < 0.f )
		return c_vector( point.m_x, point.m_y, ( int )point.m_z - 0.969644f );
	else
		return c_vector( point.m_x, point.m_y, ( int )point.m_z + 0.04f );
}
bool bounce_set_point = false;

namespace
{
	constexpr int k_pf_types        = 4;
	constexpr float k_pf_dot_size   = 9.f;
	constexpr float k_pf_hover_size = 12.f;
	constexpr float k_pf_fade_time  = 0.5f;
	constexpr float k_pf_top        = -3.14159265f * 0.5f;
	constexpr float k_pf_turn       = 6.2831853f;
	constexpr float k_pf_same_xy = 2.f;
	constexpr float k_pf_same_z  = 0.5f;

	struct pf_group_t {
		c_vector pos{ };
		AnimatedPoint* dot[ k_pf_types ]{ };
		int slot[ k_pf_types ]{ };
		int types      = 0;
		float progress = 0.f;
		float size     = 0.f;
		bool live      = false;
		bool drawn     = false;
		c_vector_2d screen{ };
	};

	struct pf_member_t {
		AnimatedPoint* dot;
		int group, type;
	};

	void pf_fade( std::vector< AnimatedPoint >& list, const std::vector< c_vector >& points, float dt )
	{
		for ( auto& ap : list )
			ap.is_removing = true;
		for ( const auto& p : points ) {
			const auto it = std::find_if( list.begin( ), list.end( ), [ &p ]( const AnimatedPoint& ap ) { return ap.position == p; } );
			if ( it == list.end( ) )
				list.push_back( { p } );
			else
				it->is_removing = false;
		}
		for ( auto& ap : list ) {
			ap.animation_progress = std::clamp( ap.animation_progress + ( ap.is_removing ? -dt : dt ) / k_pf_fade_time, 0.f, 1.f );
			ap.is_appearing       = !ap.is_removing && ap.animation_progress < 1.f;
		}
		list.erase( std::remove_if( list.begin( ), list.end( ), []( const AnimatedPoint& ap ) { return ap.is_removing && ap.animation_progress <= 0.f; } ),
		            list.end( ) );
	}
}

bool pf_dot_screen( const AnimatedPoint& ap, c_vector_2d& out )
{
	if ( !g_render.world_to_screen( ap.position, out ) )
		return false;
	out += ap.pick_offset;
	return true;
}

void pf_render_dots( const std::vector< c_vector >& surf, const std::vector< c_vector >& bounce, const std::vector< c_vector >& tb, const std::vector< c_vector >& pj )
{
	if ( ImGui::GetCurrentContext( ) == nullptr )
		return;

	static bool ps_key_was_down = false, hb_key_was_down = false;
	const bool place_surf   = consume_key_edge( GET_VARIABLE( g_variables.m_pixel_surf_assist_point_key, key_bind_t ), ps_key_was_down );
	const bool place_bounce = consume_key_edge( GET_VARIABLE( g_variables.m_bounce_assist_point_key, key_bind_t ), hb_key_was_down );

	const float dt = ImGui::GetIO( ).DeltaTime;
	pf_fade( animated_points, surf, dt );
	pf_fade( animated_points2, bounce, dt );
	pf_fade( animated_points3, tb, dt );
	pf_fade( animated_points4, pj, dt );

	constexpr int k_hb = 0, k_ps = 2;
	std::vector< AnimatedPoint >* const lists[ k_pf_types ] = { &animated_points2, &animated_points4, &animated_points, &animated_points3 };
	constexpr int k_rgb[ k_pf_types ][ 3 ]                  = { { 255, 0, 0 }, { 255, 220, 0 }, { 0, 255, 0 }, { 0, 200, 255 } };

	std::vector< pf_group_t > groups;
	std::vector< pf_member_t > members;
	for ( int t = 0; t < k_pf_types; ++t ) {
		for ( auto& ap : *lists[ t ] ) {
			auto g = std::find_if( groups.begin( ), groups.end( ), [ & ]( const pf_group_t& q ) {
				return std::fabs( q.pos.m_z - ap.position.m_z ) < k_pf_same_z && ( q.pos - ap.position ).length_2d( ) < k_pf_same_xy;
			} );
			if ( g == groups.end( ) ) {
				g      = groups.emplace( groups.end( ) );
				g->pos = ap.position;
			}
			if ( !g->dot[ t ] ) {
				g->dot[ t ] = &ap;
				g->slot[ t ] = g->types++;
			}
			g->progress = ( std::max )( g->progress, ap.animation_progress );
			g->live |= !ap.is_removing;
			members.push_back( { &ap, static_cast< int >( g - groups.begin( ) ), t } );
		}
	}

	/* hover ( grows ): the live ring nearest the crosshair, within the dot radius. each place key takes the
	   nearest ring holding a live dot of ITS type: a closer tb-only ring must not eat a surf place */
	int hovered = -1, pick[ k_pf_types ]{ -1, -1, -1, -1 };
	float hovered_sq = FLT_MAX, pick_sq[ k_pf_types ]{ FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
	for ( int i = 0; i < static_cast< int >( groups.size( ) ); ++i ) {
		auto& g = groups[ i ];
		g.drawn = g_render.world_to_screen( g.pos, g.screen );
		if ( !g.drawn || !g.live || !InCrosshair( g.screen.m_x, g.screen.m_y, k_pf_dot_size ) )
			continue;
		const float dx = g_ctx.m_width / 2.f - g.screen.m_x, dy = g_ctx.m_height / 2.f - g.screen.m_y;
		const float d_sq = dx * dx + dy * dy;
		if ( d_sq < hovered_sq ) {
			hovered_sq = d_sq;
			hovered    = i;
		}
		for ( const int t : { k_ps, k_hb } ) {
			if ( g.dot[ t ] && !g.dot[ t ]->is_removing && d_sq < pick_sq[ t ] ) {
				pick_sq[ t ] = d_sq;
				pick[ t ]    = i;
			}
		}
	}

	const float ease = 1.f - std::exp( -5.f * dt );
	for ( auto& m : members ) {
		const float target = ( m.group == hovered ? k_pf_hover_size : k_pf_dot_size ) * m.dot->animation_progress;
		m.dot->current_size += ( target - m.dot->current_size ) * ease;
		groups[ m.group ].size = ( std::max )( groups[ m.group ].size, m.dot->current_size );
	}
	for ( auto& m : members ) {
		const auto& g      = groups[ m.group ];
		m.dot->pick_offset = c_vector_2d( );
		c_vector_2d own;
		if ( g.drawn && g_render.world_to_screen( m.dot->position, own ) )
			m.dot->pick_offset = g.screen - own;
		if ( g.types < 2 )
			continue;
		const float mid = k_pf_top + ( g.slot[ m.type ] + 0.5f ) * k_pf_turn / g.types;
		m.dot->pick_offset += c_vector_2d( std::cos( mid ), std::sin( mid ) ) * ( g.size * 0.5f );
	}

	g_movement.m_pixelsurf_assist_t.in_crosshair = pick[ k_ps ] >= 0;
	if ( place_surf && pick[ k_ps ] >= 0 ) {
		m_points_check.emplace_back( groups[ pick[ k_ps ] ].dot[ k_ps ]->position, g_interfaces.m_engine_client->get_level_name_short( ) );
		g_movement.m_pixelsurf_assist_t.set_point = true;
		save_points_file( pixelsurf_points_file( ), m_points_check );
		botox_dbg_log( "[pf place] surf assist point z %.4f\n", groups[ pick[ k_ps ] ].dot[ k_ps ]->position.m_z );
	}
	if ( place_bounce && pick[ k_hb ] >= 0 ) {
		const c_vector& p = groups[ pick[ k_hb ] ].dot[ k_hb ]->position;
		m_bounce_points_check.emplace_back( round_pos2( c_vector( p.m_x, p.m_y, p.m_z + 1.f ) ),
		                                    g_interfaces.m_engine_client->get_level_name_short( ) );
		bounce_set_point = true;
		save_points_file( bounce_points_file( ), m_bounce_points_check );
		botox_dbg_log( "[pf place] bounce assist point z %.4f\n", p.m_z );
	}

	struct arc_t {
		float a0 = 0.f, a1 = 0.f;
		unsigned int color = 0;
	};
	for ( const auto& g : groups ) {
		if ( !g.drawn || g.size <= 0.f )
			continue;
		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_filled_circle,
			std::make_any< filled_circle_draw_object_t >( g.screen, g.size, c_color( 0, 0, 0, static_cast< int >( g.progress * 100.f ) ).get_u32( ) ) );
		arc_t arcs[ k_pf_types ]{ };
		for ( int t = 0; t < k_pf_types; ++t ) {
			if ( !g.dot[ t ] )
				continue;
			auto& a = arcs[ g.slot[ t ] ];
			a.a0    = k_pf_top + g.slot[ t ] * k_pf_turn / g.types;
			a.a1    = a.a0 + k_pf_turn / g.types;
			a.color = c_color( k_rgb[ t ][ 0 ], k_rgb[ t ][ 1 ], k_rgb[ t ][ 2 ], static_cast< int >( g.dot[ t ]->animation_progress * 255.f ) ).get_u32( );
		}
		if ( g.types == 1 ) {
			g_render.m_draw_data.emplace_back( e_draw_type::draw_type_circle, std::make_any< circle_draw_object_t >( g.screen, g.size, arcs[ 0 ].color ) );
			continue;
		}
		const ImVec2 centre( g.screen.m_x, g.screen.m_y );
		const float radius = g.size;
		const int count    = g.types;
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_callback,
		                                   std::make_any< callback_draw_object_t >( callback_draw_object_t{ [ = ]( ImDrawList* dl ) {
											   for ( int i = 0; i < count; ++i ) {
												   dl->PathArcTo( centre, radius, arcs[ i ].a0, arcs[ i ].a1, 32 / count );
												   dl->PathStroke( arcs[ i ].color, 0, 1.f );
											   }
										   } } ) );
	}
}
