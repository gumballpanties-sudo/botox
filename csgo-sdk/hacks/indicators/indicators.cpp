#include "indicators.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <random>
#include "migoo.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../movement/edgebug.h"
#include "../movement/edge_skip.h"
#include "../movement/movement.h"
#include "../misc/misc.h"
#include "../aimbot/aimbot.h"
#include "../lagcomp/lagcomp.h"
#include "../visuals/screen/resolution_spoof.h"
#include "../visuals/crosshair/custom_crosshair.h"

extern bool HITGODA;
extern bool HITGODA2;
extern bool HITGODA3;
extern bool g_edge_surf_active;

extern void botox_dbg_log( const char* fmt, ... );

static void key_particles( );

void n_indicators::impl_t::on_paint_traverse( )
{
	const bool on_ground = g_ctx.m_local->get_flags( ) & e_flags::fl_onground;

	if ( GET_VARIABLE( g_variables.m_velocity_indicator, bool ) )
		this->velocity( on_ground );

	if ( GET_VARIABLE( g_variables.m_stamina_indicator, bool ) )
		this->stamina( on_ground );

	this->velocity_graph( on_ground );

	if ( GET_VARIABLE( g_variables.m_key_indicators_enable, bool ) )
		this->keybind_indicators( );
	key_particles( );

	if ( GET_VARIABLE( g_variables.m_key_press_indicator, bool ) )
		this->key_press( );

	n_custom_crosshair::on_paint_traverse( );

	if ( GET_VARIABLE( g_variables.m_sniper_crosshair, bool ) )
		this->sniper_crosshair( );

	if ( GET_VARIABLE( g_variables.m_aimbot_fov_circle, bool ) )
		this->aimbot_fov_circle( );

	this->fps_warning( );
}

void n_indicators::impl_t::fps_warning( )
{
	if ( !GET_VARIABLE( g_variables.m_fps_warning, bool ) || !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	[ & ]( const char* fps_warning, const c_color& color, const bool active ) {
		ImAnimationHelper fps_animation = ImAnimationHelper( HASH_RT( fps_warning ), ImGui::GetIO( ).DeltaTime );
		fps_animation.Update( 2.f, active ? 2.f : -2.f );

		if ( fps_animation.AnimationData->second <= 0.f )
			return;

		const auto text_size = g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ]->CalcTextSizeA(
			g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ]->FontSize, FLT_MAX, 0.f, fps_warning );

		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_text,
			std::make_any< text_draw_object_t >(
				g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ], c_vector_2d( ( g_ctx.m_width / 2 ) - ( text_size.x / 2 ), 200 ),
				fps_warning, color.get_u32( fps_animation.AnimationData->second ),
				ImColor( 0.f, 0.f, 0.f, color.base< e_color_type::color_type_a >( ) * fps_animation.AnimationData->second ),
				e_text_flags::text_flag_dropshadow ) );

		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_text,
			std::make_any< text_draw_object_t >(
				g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ], c_vector_2d( ( g_ctx.m_width / 2 ) - text_size.x, 200 ), "!",
				ImColor( 1.f, 0.f, 0.f, 1.f ),
				ImColor( 0.f, 0.f, 0.f, color.base< e_color_type::color_type_a >( ) * fps_animation.AnimationData->second ),
				e_text_flags::text_flag_dropshadow ) );

		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_text,
			std::make_any< text_draw_object_t >(
				g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ], c_vector_2d( ( g_ctx.m_width / 2 ) + text_size.x, 200 ), "!",
				ImColor( 1.f, 0.f, 0.f, 1.f ),
				ImColor( 0.f, 0.f, 0.f, color.base< e_color_type::color_type_a >( ) * fps_animation.AnimationData->second ),
				e_text_flags::text_flag_dropshadow ) );

		const std::string current_fps_text =
			std::format( "current fps: {} needed fps: {}+",
		                  static_cast< int >( ImGui::GetIO( ).Framerate + 0.5f ),
		                                         static_cast< int >( 1.f / g_interfaces.m_global_vars_base->m_interval_per_tick ) );

		const auto current_fps_text_size = g_render.m_fonts[ e_font_names::font_name_tahoma_12 ]->CalcTextSizeA(
			g_render.m_fonts[ e_font_names::font_name_tahoma_12 ]->FontSize, FLT_MAX, 0.f, current_fps_text.c_str( ) );

		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_text,
			std::make_any< text_draw_object_t >(
				g_render.m_fonts[ e_font_names::font_name_tahoma_12 ], c_vector_2d( ( g_ctx.m_width / 2 ) - ( current_fps_text_size.x / 2 ), 215 ),
				current_fps_text, ImColor( .7f, .7f, .7f, fps_animation.AnimationData->second ),
				ImColor( 0.f, 0.f, 0.f, color.base< e_color_type::color_type_a >( ) * fps_animation.AnimationData->second ),
				e_text_flags::text_flag_dropshadow ) );
	}( "LOW FPS", c_color( 1.f, 1.f, 1.f, 1.f ), g_ctx.m_low_fps );
}

static void push_crosshair_rect( float x1, const float y1, float x2, const float y2, const unsigned int color )
{
	if ( x2 <= x1 || y2 <= y1 )
		return;

	const bool overlay_list_stretched = g_resolution_spoof.aspect_cheat_overlay( ) || g_resolution_spoof.upscale_stretches( );

	if ( const float aspect = overlay_list_stretched ? 1.f : g_resolution_spoof.overlay_aspect_scale( );
	     fabsf( aspect - 1.f ) >= 0.001f && g_ctx.m_width > 0.f ) {
		const float center = g_ctx.m_width * 0.5f;

		x1 = center + ( x1 - center ) * aspect;
		x2 = center + ( x2 - center ) * aspect;
	}

	g_render.m_draw_data.emplace_back(
		e_draw_type::draw_type_rect,
		std::make_any< rect_draw_object_t >( c_vector_2d( x1, y1 ), c_vector_2d( x2, y2 ), color, ImColor( 0.f, 0.f, 0.f, 0.f ), true, 0.f,
	                                        ImDrawFlags_::ImDrawFlags_None, 1.f, e_rect_flags::rect_flag_none ) );
}

struct crosshair_arm_t {
	float m_x1, m_y1, m_x2, m_y2;
};

void n_indicators::impl_t::sniper_crosshair_clarity( const float cx, const float cy, const bool outline )
{
	const float x = std::floor( cx ), y = std::floor( cy );

	if ( outline ) {
		const c_color outline_color = GET_VARIABLE( g_variables.m_sniper_crosshair_outline_color, c_color );
		push_crosshair_rect( x - 3.f, y - 3.f, x + 3.f, y + 3.f, outline_color.get_u32( 0.15f ) );
		push_crosshair_rect( x - 2.f, y - 2.f, x + 2.f, y + 2.f, outline_color.get_u32( ) );
	}

	push_crosshair_rect( x - 1.f, y - 1.f, x + 1.f, y + 1.f, GET_VARIABLE( g_variables.m_sniper_crosshair_color, c_color ).get_u32( ) );
}

static void push_crosshair_ring( const float x1, const float y1, const float x2, const float y2, const unsigned int color,
                                 const bool corners )
{
	if ( corners ) {
		push_crosshair_rect( x1 - 1.f, y1 - 1.f, x2 + 1.f, y2 + 1.f, color );
		return;
	}

	push_crosshair_rect( x1, y1 - 1.f, x2, y1, color );
	push_crosshair_rect( x1, y2, x2, y2 + 1.f, color );
	push_crosshair_rect( x1 - 1.f, y1, x1, y2, color );
	push_crosshair_rect( x2, y1, x2 + 1.f, y2, color );
}

void n_indicators::impl_t::sniper_crosshair_interwebz( const float cx, const float cy )
{
	/* band = lo..lo+t, lo = centre - floor(t/2): even t on the pixel EDGE, odd t on the centre
	   pixel, never a half pixel */
	const float size      = GET_VARIABLE( g_variables.m_sniper_crosshair_interwebz_size, float );
	const float length    = std::floor( 7.f * size + 0.5f );
	const float gap       = static_cast< float >( GET_VARIABLE( g_variables.m_sniper_crosshair_interwebz_gap, int ) );
	const int   thickness_i = GET_VARIABLE( g_variables.m_sniper_crosshair_interwebz_thickness, int );
	const float thickness   = static_cast< float >( thickness_i < 1 ? 1 : thickness_i );

	const float lo_x = std::floor( cx ) - std::floor( thickness * 0.5f ), hi_x = lo_x + thickness;
	const float lo_y = std::floor( cy ) - std::floor( thickness * 0.5f ), hi_y = lo_y + thickness;

	const unsigned int color         = GET_VARIABLE( g_variables.m_sniper_crosshair_color, c_color ).get_u32( );
	const unsigned int outline_color = GET_VARIABLE( g_variables.m_sniper_crosshair_outline_color, c_color ).get_u32( );

	const bool arm_corners = GET_VARIABLE( g_variables.m_sniper_crosshair_interwebz_arm_corners, bool );

	/* gap is measured from the core box edge, so raising thickness never eats it */
	const crosshair_arm_t arms[ 4 ] = {
		{ lo_x - gap - length, lo_y, lo_x - gap, hi_y },
		{ hi_x + gap, lo_y, hi_x + gap + length, hi_y },
		{ lo_x, lo_y - gap - length, hi_x, lo_y - gap },
		{ lo_x, hi_y + gap, hi_x, hi_y + gap + length },
	};

	for ( const auto& arm : arms )
		push_crosshair_ring( arm.m_x1, arm.m_y1, arm.m_x2, arm.m_y2, outline_color, arm_corners );

	for ( const auto& arm : arms )
		push_crosshair_rect( arm.m_x1, arm.m_y1, arm.m_x2, arm.m_y2, color );

	push_crosshair_ring( lo_x, lo_y, hi_x, hi_y, outline_color, GET_VARIABLE( g_variables.m_sniper_crosshair_interwebz_dot_corners, bool ) );
	push_crosshair_rect( lo_x, lo_y, hi_x, hi_y, GET_VARIABLE( g_variables.m_sniper_crosshair_dot_color, c_color ).get_u32( ) );
}

static int csgo_round( const float value )
{
	return static_cast< int >( std::floor( value + 0.5f ) );
}

static void csgo_crosshair_rect( const int x0, const int y0, const int x1, const int y1, const unsigned int color,
                                 const bool outline, const float outline_thickness, const unsigned int outline_color )
{
	if ( outline ) {
		push_crosshair_rect( static_cast< float >( static_cast< int >( x0 - outline_thickness ) ),
		                     static_cast< float >( static_cast< int >( y0 - outline_thickness ) ),
		                     static_cast< float >( static_cast< int >( x1 + outline_thickness ) ),
		                     static_cast< float >( static_cast< int >( y1 + outline_thickness ) ), outline_color );
	}

	push_crosshair_rect( static_cast< float >( x0 ), static_cast< float >( y0 ), static_cast< float >( x1 ), static_cast< float >( y1 ), color );
}

void n_indicators::impl_t::sniper_crosshair_custom( )
{
	const float yres = static_cast< float >( g_ctx.m_height ) / 480.f;

	const c_color color_var = GET_VARIABLE( g_variables.m_sniper_crosshair_color, c_color );

	c_color outline_var                       = GET_VARIABLE( g_variables.m_sniper_crosshair_outline_color, c_color );
	outline_var[ e_color_type::color_type_a ] = color_var[ e_color_type::color_type_a ];

	const unsigned int color         = color_var.get_u32( );
	const unsigned int outline_color = outline_var.get_u32( );

	const bool outline      = GET_VARIABLE( g_variables.m_sniper_crosshair_outline, bool );
	float outline_thickness = GET_VARIABLE( g_variables.m_sniper_crosshair_outline_thickness, float );
	outline_thickness = outline_thickness < 0.1f ? 0.1f : ( outline_thickness > 3.f ? 3.f : outline_thickness );

	/* style 4 never expands: distance is the fixed 4 unit minimum + the gap, truncated */
	const int distance = static_cast< int >( 4.f + GET_VARIABLE( g_variables.m_sniper_crosshair_gap, float ) );
	const int bar_size = csgo_round( GET_VARIABLE( g_variables.m_sniper_crosshair_size, float ) * yres );
	int bar_thickness  = csgo_round( GET_VARIABLE( g_variables.m_sniper_crosshair_thickness, float ) * yres );
	if ( bar_thickness < 1 )
		bar_thickness = 1;

	const int center_x = static_cast< int >( g_ctx.m_width ) / 2;
	const int center_y = static_cast< int >( g_ctx.m_height ) / 2;

	const bool t_style = GET_VARIABLE( g_variables.m_sniper_crosshair_t, bool );

	const int inner_left  = center_x - distance - bar_thickness / 2;
	const int inner_right = inner_left + 2 * distance + bar_thickness;
	const int outer_left  = inner_left - bar_size;
	const int outer_right = inner_right + bar_size;
	int y0                = center_y - bar_thickness / 2;
	int y1                = y0 + bar_thickness;

	csgo_crosshair_rect( outer_left, y0, inner_left, y1, color, outline, outline_thickness, outline_color );
	csgo_crosshair_rect( inner_right, y0, outer_right, y1, color, outline, outline_thickness, outline_color );

	const int inner_top    = center_y - distance - bar_thickness / 2;
	const int inner_bottom = inner_top + 2 * distance + bar_thickness;
	const int outer_top    = inner_top - bar_size;
	const int outer_bottom = inner_bottom + bar_size;
	int x0                 = center_x - bar_thickness / 2;
	int x1                 = x0 + bar_thickness;

	if ( !t_style )
		csgo_crosshair_rect( x0, outer_top, x1, inner_top, color, outline, outline_thickness, outline_color );

	csgo_crosshair_rect( x0, inner_bottom, x1, outer_bottom, color, outline, outline_thickness, outline_color );

	if ( GET_VARIABLE( g_variables.m_sniper_crosshair_dot, bool ) ) {
		x0 = center_x - bar_thickness / 2;
		x1 = x0 + bar_thickness;
		y0 = center_y - bar_thickness / 2;
		y1 = y0 + bar_thickness;

		csgo_crosshair_rect( x0, y0, x1, y1, color, outline, outline_thickness, outline_color );
	}
}

void n_indicators::impl_t::sniper_crosshair( )
{
	if ( !g_ctx.m_local->is_alive( ) )
		return;

	auto weapon_handle = g_ctx.m_local->get_active_weapon_handle( );
	if ( !weapon_handle )
		return;

	const auto active_weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( weapon_handle );

	if ( !active_weapon )
		return;

	const auto definition_index = active_weapon->get_item_definition_index( );

	if ( g_ctx.m_local->is_scoped( ) )
		return;

	if ( !g_utilities.is_in< short >( definition_index, { e_item_definition_index::weapon_awp, e_item_definition_index::weapon_ssg08,
	                                                      e_item_definition_index::weapon_scar20, e_item_definition_index::weapon_g3sg1 } ) )
		return;

	const float cx = static_cast< float >( g_ctx.m_width ) * 0.5f;
	const float cy = static_cast< float >( g_ctx.m_height ) * 0.5f;

	switch ( GET_VARIABLE( g_variables.m_sniper_crosshair_style, int ) ) {
	case 1:
		this->sniper_crosshair_clarity( cx, cy, true );
		break;
	case 2:
		this->sniper_crosshair_interwebz( cx, cy );
		break;
	case 3:
		this->sniper_crosshair_custom( );
		break;
	default:
		this->sniper_crosshair_clarity( cx, cy, false );
		break;
	}
}

void n_indicators::impl_t::aimbot_fov_circle( )
{
	if ( !GET_VARIABLE( g_variables.m_aimbot_enable, bool ) || !g_ctx.m_local->is_alive( ) )
		return;

	if ( GET_VARIABLE( g_variables.m_aimbot_fov_circle_key_only, bool ) && GET_VARIABLE( g_variables.m_aimbot_on_key, bool ) &&
	     !g_input.check_input( &GET_VARIABLE( g_variables.m_aimbot_key, key_bind_t ) ) )
		return;

	const float fov = std::min( g_aimbot.active_fov( ), 88.f );
	if ( fov <= 0.f )
		return;

	c_angle view_angles{ };
	g_interfaces.m_engine_client->get_view_angles( view_angles );

	constexpr float k_probe_degrees = 5.f;
	constexpr float k_probe_range   = 8192.f;

	c_angle probe_angles = view_angles;
	probe_angles.m_x -= k_probe_degrees;

	c_vector forward{ }, probe{ };
	g_math.angle_vectors( view_angles, &forward );
	g_math.angle_vectors( probe_angles, &probe );

	const c_vector eye = g_ctx.m_local->get_eye_position( false );

	c_vector_2d forward_screen{ }, probe_screen{ };
	if ( !g_render.world_to_screen( eye + forward * k_probe_range, forward_screen ) ||
	     !g_render.world_to_screen( eye + probe * k_probe_range, probe_screen ) )
		return;

	const c_vector_2d center{ g_ctx.m_width * 0.5f, g_ctx.m_height * 0.5f };

	const float pixels_per_tan = forward_screen.distance( probe_screen ) / std::tanf( deg2rad( k_probe_degrees ) );
	const float radius         = pixels_per_tan * std::tanf( deg2rad( fov ) );

	if ( radius < 1.f )
		return;

	const int segments = std::clamp( static_cast< int >( radius * 0.5f ), 32, 128 );

	float thickness = GET_VARIABLE( g_variables.m_aimbot_fov_circle_thickness, float );
	if ( thickness < 0.5f )
		thickness = 0.5f;

	g_render.m_draw_data.emplace_back(
		e_draw_type::draw_type_circle,
		std::make_any< circle_draw_object_t >( center, radius, GET_VARIABLE( g_variables.m_aimbot_fov_circle_color, c_color ).get_u32( ),
	                                          segments, thickness ) );
}

namespace
{
	float rido_lerp( const float current, const float target, const float smooth, const float dt )
	{
		return current + ( target - current ) * ( 1.f - std::exp( -smooth * 0.96f * dt ) );
	}

	/* label + drop shadow colour. shadow keeps the label's alpha so the fade stays in sync */
	struct indicator_colors_t {
		c_color m_text;
		c_color m_shadow;
		c_color m_outline;
	};

	struct shadow_opts_t {
		bool m_on;
		float m_blur;
		c_color m_color   = c_color( 0.f, 0.f, 0.f, 1.f );
		bool m_outline    = false;
		c_color m_outline_color = c_color( 0.f, 0.f, 0.f, 1.f );
		int m_outline_px  = 1;
		float m_off_x     = 1.f;
		float m_off_y     = 1.f;
	};

	c_color shadow_at( const c_color& tint, const float a )
	{
		return c_color( tint.base< e_color_type::color_type_r >( ), tint.base< e_color_type::color_type_g >( ), tint.base< e_color_type::color_type_b >( ),
		                a * tint.base< e_color_type::color_type_a >( ) );
	}

	ImU32 shadow_u32( const c_color& tint, const float a )
	{
		return ImColor( tint.base< e_color_type::color_type_r >( ), tint.base< e_color_type::color_type_g >( ), tint.base< e_color_type::color_type_b >( ),
		                a * tint.base< e_color_type::color_type_a >( ) );
	}

	/* outline / outline color / outline px / shadow x / shadow y are variable ids, same shape on every indicator */
	shadow_opts_t shadow_opts_of( const bool on, const float blur, const c_color& color, const std::uint32_t outline, const std::uint32_t outline_color,
	                              const std::uint32_t outline_px, const std::uint32_t shadow_x, const std::uint32_t shadow_y )
	{
		/* indicator font flags dropshadow / outline add on top of the indicator's own */
		const auto& font_flags = GET_VARIABLE( g_variables.m_indicator_font_flags, std::vector< bool > );
		const bool font_on      = font_flags.size( ) == e_font_flags::font_flag_max && font_flags[ e_font_flags::font_flag_dropshadow ];
		const bool font_outline = font_flags.size( ) == e_font_flags::font_flag_max && font_flags[ e_font_flags::font_flag_outline ];

		return { on || font_on,
			     blur,
			     color,
			     GET_VARIABLE( outline, bool ) || font_outline,
			     GET_VARIABLE( outline_color, c_color ),
			     std::clamp( GET_VARIABLE( outline_px, int ), 1, 5 ),
			     static_cast< float >( GET_VARIABLE( shadow_x, int ) ),
			     static_cast< float >( GET_VARIABLE( shadow_y, int ) ) };
	}

	indicator_colors_t key_colors_plain( const c_color& shadow_tint, const c_color& outline_tint )
	{
		const c_color base = GET_VARIABLE( g_variables.m_key_color, c_color );
		const float a      = base.base< e_color_type::color_type_a >( );
		return { base, shadow_at( shadow_tint, a ), shadow_at( outline_tint, a ) };
	}

	indicator_colors_t key_colors_at( const float t, const bool shadow_detect, const c_color& shadow_tint, const bool predicted,
	                                  const c_color& outline_tint )
	{
		const c_color base = GET_VARIABLE( g_variables.m_key_color, c_color );
		const c_color hit  = predicted ? GET_VARIABLE( g_variables.m_key_color_predict, c_color ) : GET_VARIABLE( g_variables.m_key_color_success, c_color );

		const auto blend = [ & ]( const c_color& from, const c_color& to, const float alpha ) {
			return c_color( std::lerp( from.base< e_color_type::color_type_r >( ), to.base< e_color_type::color_type_r >( ), t ),
			                std::lerp( from.base< e_color_type::color_type_g >( ), to.base< e_color_type::color_type_g >( ), t ),
			                std::lerp( from.base< e_color_type::color_type_b >( ), to.base< e_color_type::color_type_b >( ), t ), alpha );
		};

		if ( shadow_detect ) {
			const float a = base.base< e_color_type::color_type_a >( );
			return { base, blend( shadow_tint, hit, a * shadow_tint.base< e_color_type::color_type_a >( ) ),
				     blend( outline_tint, hit, a * outline_tint.base< e_color_type::color_type_a >( ) ) };
		}

		const c_color text = blend( base, hit, std::lerp( base.base< e_color_type::color_type_a >( ), hit.base< e_color_type::color_type_a >( ), t ) );

		return { text, shadow_at( shadow_tint, text.base< e_color_type::color_type_a >( ) ), shadow_at( outline_tint, text.base< e_color_type::color_type_a >( ) ) };
	}

	struct key_pending_t {
		e_keybind_labels m_id;
		std::string m_text;
		bool m_detect;
		bool m_success;
		bool m_active;
		bool m_predicted = false;
	};
}

struct mito_ent_t {
	char m_text[ 56 ];
	ImU32 m_col;
	ImU32 m_shadow;
	ImU32 m_outline;
	float m_anim;
	float m_raw;
	ImVec2 m_size;
	ImVec2 m_pos;
	float m_rgb[ 3 ];
	ImVec2 m_piv;
	float m_sx;
	float m_sy;
	ImFont* m_font;
	float m_fs;
};

/* one frame of tags: laid out and split on the game thread, drawn on the render thread */
struct mito_frame_t {
	ImFont* m_font = nullptr;
	float m_fs     = 0.f;
	bool m_axis_x  = true;
	float m_blur   = 0.f;
	bool m_squash  = false;
	ImVec2 m_off   = ImVec2( 1.f, 1.f );
	int m_ring_px  = 1;
	int m_n        = 0;
	mito_ent_t m_ents[ label_max ]{ };
	bool m_in[ label_max ]{ };
	int m_parent[ label_max ]{ };
};

struct frame_tex_t {
	IDirect3DTexture9* m_tex = nullptr;
	IDirect3DDevice9* m_dev  = nullptr;
	int m_dim                = 0;
	bool m_dyn               = false;
	int m_w = 0, m_h = 0;
	std::uint64_t m_key = 0;
};

static frame_tex_t s_frame_tex[ 64 ];
static int s_frame_tex_next = 0, s_frame_tex_frame = -1;

static void frame_tex_release( frame_tex_t& t )
{
	if ( t.m_tex )
		t.m_tex->Release( );

	t = { };
}

void n_indicators::impl_t::release_textures( )
{
	for ( frame_tex_t& t : s_frame_tex )
		frame_tex_release( t );

	g_render.release_indicator_texture( );
}

static frame_tex_t* frame_tex_next( )
{
	if ( ImGui::GetFrameCount( ) != s_frame_tex_frame ) {
		s_frame_tex_frame = ImGui::GetFrameCount( );
		s_frame_tex_next  = 0;
	}

	return s_frame_tex_next < static_cast< int >( std::size( s_frame_tex ) ) ? &s_frame_tex[ s_frame_tex_next ] : nullptr;
}

/* next slot if it still holds `key` from last frame: no blit / blur / upload. key 0 never matches */
static const frame_tex_t* frame_tex_cached( const std::uint64_t key )
{
	frame_tex_t* const t = frame_tex_next( );
	if ( !t || !key || t->m_key != key || !t->m_tex || t->m_dev != g_interfaces.m_direct_device )
		return nullptr;

	++s_frame_tex_next;
	return t;
}

/* this frame's next texture holding px (w x h A8R8G8B8), drawn with uv ( w, h ) / m_dim, remembered as
   `key` (0 = never reused). null = pool dry or d3d refused it: stage + hr say which (mitosis logs them) */
static const frame_tex_t* frame_tex_upload( const unsigned int* px, const int w, const int h, const std::uint64_t key, const char*& stage,
                                            HRESULT& hr )
{
	stage = "pool dry";
	hr    = E_OUTOFMEMORY;
	if ( !frame_tex_next( ) )
		return nullptr;

	IDirect3DDevice9* const device = g_interfaces.m_direct_device;
	stage                          = "no device";
	hr                             = E_POINTER;
	if ( !device )
		return nullptr;

	int dim = 32;
	while ( dim < w || dim < h )
		dim <<= 1;

	stage = "too big";
	hr    = E_INVALIDARG;
	if ( dim > 1024 )
		return nullptr;

	frame_tex_t& t = s_frame_tex[ s_frame_tex_next ];
	if ( t.m_dev != device || t.m_dim < dim ) {
		frame_tex_release( t );

		stage = "CreateTexture";
		hr    = device->CreateTexture( dim, dim, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.m_tex, nullptr );
		if ( FAILED( hr ) || !t.m_tex ) {
			t.m_tex = nullptr;
			t.m_dyn = true;
			hr      = device->CreateTexture( dim, dim, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t.m_tex, nullptr );
		}

		if ( FAILED( hr ) || !t.m_tex ) {
			frame_tex_release( t );
			return nullptr;
		}

		t.m_dev = device;
		t.m_dim = dim;
	}

	/* WHOLE level, never a sub-rect: DISCARD with a rect is D3DERR_INVALIDCALL on the DYNAMIC one */
	stage   = "LockRect";
	t.m_key = 0;
	D3DLOCKED_RECT lr{ };
	hr = t.m_tex->LockRect( 0, &lr, nullptr, t.m_dyn ? D3DLOCK_DISCARD : 0 );
	if ( FAILED( hr ) )
		return nullptr;

	/* DISCARD leaves the rest undefined: clear the one texel column and row just past the box, the
	   only ones its edge can ever reach */
	for ( int y = 0; y < h; ++y ) {
		unsigned int* row = reinterpret_cast< unsigned int* >( static_cast< unsigned char* >( lr.pBits ) + static_cast< std::size_t >( lr.Pitch ) * y );
		std::memcpy( row, px + static_cast< std::size_t >( w ) * y, static_cast< std::size_t >( w ) * 4 );
		if ( w < t.m_dim )
			row[ w ] = 0;
	}
	if ( h < t.m_dim )
		std::memset( static_cast< unsigned char* >( lr.pBits ) + static_cast< std::size_t >( lr.Pitch ) * h, 0,
		             static_cast< std::size_t >( w < t.m_dim ? w + 1 : w ) * 4 );
	t.m_tex->UnlockRect( 0 );

	t.m_w   = w;
	t.m_h   = h;
	t.m_key = key;
	++s_frame_tex_next;
	return &t;
}

static std::vector< unsigned char > s_soft_cov;
static std::vector< float > s_soft_a, s_soft_b;
static std::vector< unsigned int > s_soft_px;

static int soft_radii( const float sigma, int r[ 3 ] )
{
	const float ideal = std::sqrt( 4.f * sigma * sigma + 1.f );
	int wl            = static_cast< int >( std::floor( ideal ) );
	if ( wl % 2 == 0 )
		--wl;

	const int m = static_cast< int >( std::round( ( 12.f * sigma * sigma - 3.f * wl * wl - 12.f * wl - 9.f ) / ( -4.f * wl - 4.f ) ) );
	for ( int i = 0; i < 3; ++i )
		r[ i ] = ( ( i < m ? wl : wl + 2 ) - 1 ) / 2;

	return r[ 0 ] + r[ 1 ] + r[ 2 ];
}

static void box_pass( const float* src, float* dst, const int count, const int n, const int step, const int line_step, const int r )
{
	const float inv = 1.f / static_cast< float >( 2 * r + 1 );

	for ( int l = 0; l < count; ++l ) {
		const float* s = src + static_cast< std::size_t >( l ) * line_step;
		float* d       = dst + static_cast< std::size_t >( l ) * line_step;

		float acc = 0.f;
		for ( int i = 0; i < r && i < n; ++i )
			acc += s[ i * step ];

		for ( int i = 0; i < n; ++i ) {
			if ( i + r < n )
				acc += s[ ( i + r ) * step ];
			d[ i * step ] = acc * inv;
			if ( i - r >= 0 )
				acc -= s[ ( i - r ) * step ];
		}
	}
}

static void soft_image( ImDrawList* dl, const frame_tex_t& tex, const ImVec2 org, const ImU32 color )
{
	const float d = static_cast< float >( tex.m_dim );
	dl->AddImage( static_cast< ImTextureID >( tex.m_tex ), org, ImVec2( org.x + tex.m_w, org.y + tex.m_h ), ImVec2( 0.f, 0.f ),
	              ImVec2( tex.m_w / d, tex.m_h / d ), color );
}

static bool soft_draw( ImDrawList* dl, const int bw, const int bh, const ImVec2 org, const ImU32 color, const int r[ 3 ], const std::uint64_t key )
{
	const std::size_t n = static_cast< std::size_t >( bw ) * bh;
	s_soft_b.resize( n );

	float *a = s_soft_a.data( ), *b = s_soft_b.data( );
	for ( int i = 0; i < 3; ++i ) {
		box_pass( a, b, bh, bw, 1, bw, r[ i ] );
		std::swap( a, b );
	}
	for ( int i = 0; i < 3; ++i ) {
		box_pass( a, b, bw, bh, bw, 1, r[ i ] );
		std::swap( a, b );
	}

	s_soft_px.resize( n );
	for ( std::size_t i = 0; i < n; ++i )
		s_soft_px[ i ] = ( static_cast< unsigned int >( std::clamp( a[ i ], 0.f, 1.f ) * 255.f + 0.5f ) << 24 ) | 0x00FFFFFFu;

	const char* stage = nullptr;
	HRESULT hr        = S_OK;
	const frame_tex_t* const tex = frame_tex_upload( s_soft_px.data( ), bw, bh, key, stage, hr );
	if ( !tex )
		return false;

	soft_image( dl, *tex, org, color );
	return true;
}

static bool soft_text( ImDrawList* dl, ImFont* font, const float fs, const ImVec2 pos, const char* text, const ImU32 color, const float blur )
{
	int r[ 3 ];
	const int pad = soft_radii( blur * 0.5f, r );
	if ( pad <= 0 || !font->ContainerAtlas || !font->ContainerAtlas->TexPixelsAlpha8 )
		return false;

	if ( !( color & IM_COL32_A_MASK ) )
		return true;

	const ImVec2 size = font->CalcTextSizeA( fs, FLT_MAX, 0.f, text );
	const int margin  = pad + 1 + static_cast< int >( std::ceil( fs * 0.125f ) );
	const int x0 = static_cast< int >( std::floor( pos.x ) ) - margin, y0 = static_cast< int >( std::floor( pos.y ) ) - margin;
	const int bw = static_cast< int >( std::ceil( pos.x + size.x ) ) + margin - x0;
	const int bh = static_cast< int >( std::ceil( pos.y + size.y ) ) + margin - y0;
	if ( bw > 1024 || bh > 1024 )
		return false;

	std::uint64_t key = 0xCBF29CE484222325ull;
	const auto mix    = [ & ]( const void* data, const std::size_t bytes ) {
		for ( std::size_t i = 0; i < bytes; ++i )
			key = ( key ^ static_cast< const unsigned char* >( data )[ i ] ) * 0x100000001B3ull;
	};
	const float frac[ 2 ] = { pos.x - std::floor( pos.x ), pos.y - std::floor( pos.y ) };
	const void* atlas[ 2 ] = { font->ContainerAtlas, font->ContainerAtlas->TexID };
	mix( text, std::strlen( text ) + 1 );
	mix( atlas, sizeof( atlas ) );
	mix( &fs, sizeof( fs ) );
	mix( &size.x, sizeof( size.x ) );
	mix( &blur, sizeof( blur ) );
	mix( frac, sizeof( frac ) );

	const ImVec2 org( static_cast< float >( x0 ), static_cast< float >( y0 ) );
	if ( const frame_tex_t* const hit = frame_tex_cached( key ) ) {
		soft_image( dl, *hit, org, color );
		return true;
	}

	const std::size_t n = static_cast< std::size_t >( bw ) * bh;
	s_soft_cov.assign( n, 0 );
	if ( !mig::BlitLabel( s_soft_cov.data( ), bw, bh, org, font, fs, text, pos ) )
		return false;

	s_soft_a.resize( n );
	for ( std::size_t i = 0; i < n; ++i )
		s_soft_a[ i ] = s_soft_cov[ i ] / 255.f;

	return soft_draw( dl, bw, bh, org, color, r, key );
}

static bool soft_picture( ImDrawList* dl, const unsigned int* px, const int w, const int h, const ImVec2 at, const ImU32 color, const float blur )
{
	int r[ 3 ];
	const int border = soft_radii( blur * 0.5f, r ) + 1;
	if ( border <= 1 )
		return false;

	const int bw = w + 2 * border, bh = h + 2 * border;
	s_soft_a.assign( static_cast< std::size_t >( bw ) * bh, 0.f );
	for ( int y = 0; y < h; ++y )
		for ( int x = 0; x < w; ++x )
			s_soft_a[ static_cast< std::size_t >( y + border ) * bw + x + border ] = ( px[ static_cast< std::size_t >( y ) * w + x ] >> 24 ) / 255.f;

	return soft_draw( dl, bw, bh, ImVec2( at.x - border, at.y - border ), color, r, 0 );
}

/* every offset inside a px-radius disc (round corners), centre skipped. px 1 = 8 neighbours */
template < class F >
static void ring_each( const int px, F&& f )
{
	for ( int dy = -px; dy <= px; ++dy )
		for ( int dx = -px; dx <= px; ++dx )
			if ( ( dx || dy ) && dx * dx + dy * dy <= px * px + px )
				f( static_cast< float >( dx ), static_cast< float >( dy ) );
}

/* queue one label. blur 0 = hard 1px shadow; blur > 0 = render-thread callback draws the soft copy
   (texture upload is render thread only). fs = draw size (font size x the indicator's scale) */
static void push_label( ImFont* font, const float fs, const c_vector_2d& pos, const std::string& text, const ImU32 color, const ImU32 shadow_color,
                        const shadow_opts_t& shadow, ImU32 outline_color = 0u )
{
	/* no lit colour from the caller = plain outline colour, faded with the label */
	if ( shadow.m_outline && !outline_color )
		outline_color = shadow_u32( shadow.m_outline_color, static_cast< float >( ( color >> IM_COL32_A_SHIFT ) & 0xFFu ) / 255.f );

	const bool ring = shadow.m_outline && ( outline_color & IM_COL32_A_MASK );
	const bool drop = shadow.m_on;
	if ( !ring && ( !drop || ( shadow.m_blur <= 0.f && shadow.m_off_x == 1.f && shadow.m_off_y == 1.f ) ) ) {
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text,
		                                   std::make_any< text_draw_object_t >( font, pos, text, color, shadow_color,
		                                                                        drop ? e_text_flags::text_flag_dropshadow : e_text_flags::text_flag_none,
		                                                                        fs ) );
		return;
	}

	/* order: shadow, ring, label */
	const float blur = drop ? shadow.m_blur : 0.f;
	const ImVec2 off( shadow.m_off_x, shadow.m_off_y );
	const int ring_px = ring ? shadow.m_outline_px : 0;
	g_render.m_draw_data.emplace_back(
		e_draw_type::draw_type_callback,
		std::make_any< callback_draw_object_t >( callback_draw_object_t{ [ = ]( ImDrawList* dl ) {
			ImFont* const live = g_render.live_font( font );
			if ( !live )
				return;

			const float size = fs > 0.f ? fs : live->FontSize;
			const ImVec2 sp( std::floor( pos.m_x ) + off.x, std::floor( pos.m_y ) + off.y );
			const bool soft = drop && blur > 0.f && soft_text( dl, live, size, sp, text.c_str( ), shadow_color, blur );

			dl->PushTextureID( live->ContainerAtlas->TexID );
			if ( drop && !soft )
				dl->AddText( live, size, ImVec2( pos.m_x + off.x, pos.m_y + off.y ), shadow_color, text.c_str( ) );
			ring_each( ring_px, [ & ]( const float dx, const float dy ) {
				dl->AddText( live, size, ImVec2( pos.m_x + dx, pos.m_y + dy ), outline_color, text.c_str( ) );
			} );
			dl->AddText( live, size, ImVec2( pos.m_x, pos.m_y ), color, text.c_str( ) );
			dl->PopTextureID( );
		} } ) );
}

constexpr float k_kami_ghost_life = 0.5f;

static float kami_rate( float dt )
{
	if ( dt <= 0.f || dt > 0.1f )
		dt = 0.016f;
	return 1.f - std::exp( -12.f * dt );
}

static void kami_ghost_draw( ImFont* font, const float fs, const std::string& text, const c_vector_2d& at, const float age, const float rise )
{
	const float t = std::clamp( age / k_kami_ghost_life, 0.f, 1.f );
	push_label( font, fs, c_vector_2d( at.m_x, at.m_y - rise * t ), text, GET_VARIABLE( g_variables.m_indicator_kami_shadow_color, c_color ).get_u32( 1.f - t ),
	            0u, { false, 0.f } );
}

static bool draw_mitosis( ImDrawList* dl, ImFont* font, const float fs, const mig::Tag* tags, const int n, const bool axis_x, const ImU32 tint,
                          const ImU32 shadow_tint, const float blur, const ImVec2 off )
{
	static mig::Work s_wk;
	static unsigned s_noted = 0;
	const auto first        = [ & ]( const unsigned bit ) {
        const bool f = !( s_noted & bit );
        s_noted |= bit;
        return f;
	};

	ImFontAtlas* atlas = font ? font->ContainerAtlas : nullptr;
	if ( !atlas || !atlas->TexPixelsAlpha8 ) {
		if ( first( 1 ) )
			g_console.print( std::format( "mitosis: text fallback -- no atlas CPU pixels (atlas {:p})", static_cast< void* >( atlas ) ).c_str( ) );
		return false;
	}

	int ox = 0, oy = 0, w = 0, h = 0;
	if ( !mig::Render( tags, n, font, fs, axis_x, s_wk, ox, oy, w, h ) ) {
		if ( first( 2 ) )
			g_console.print( std::format( "mitosis: text fallback -- Render refused ({} tags, box {}x{}, fs {:.1f})", n, w, h, fs ).c_str( ) );
		return false;
	}

	const char* stage = nullptr;
	HRESULT hr        = S_OK;
	const frame_tex_t* const tex = frame_tex_upload( s_wk.px.data( ), w, h, 0, stage, hr );
	if ( !tex ) {
		if ( first( 4 ) )
			g_console.print(
				std::format( "mitosis: text fallback -- texture {}x{} {} (hr 0x{:08X})", w, h, stage, static_cast< unsigned long >( hr ) ).c_str( ) );
		return false;
	}

	if ( first( 16 ) )
		g_console.print( std::format( "mitosis: drawing ({} tags, box {}x{} at {},{}, tex {}x{} {})", n, w, h, ox, oy, tex->m_dim, tex->m_dim,
		                              tex->m_dyn ? "dynamic" : "managed" )
		                     .c_str( ) );

	const ImTextureID id = static_cast< ImTextureID >( tex->m_tex );
	const ImVec2 p0( static_cast< float >( ox ), static_cast< float >( oy ) ), p1( static_cast< float >( ox + w ), static_cast< float >( oy + h ) );
	const ImVec2 uv1( static_cast< float >( w ) / tex->m_dim, static_cast< float >( h ) / tex->m_dim );

	if ( shadow_tint & IM_COL32_A_MASK ) {
		const ImVec2 s0( p0.x + off.x, p0.y + off.y );
		if ( !( blur > 0.f && soft_picture( dl, s_wk.px.data( ), w, h, s0, shadow_tint, blur ) ) )
			dl->AddImage( id, s0, ImVec2( p1.x + off.x, p1.y + off.y ), ImVec2( 0.f, 0.f ), uv1, shadow_tint );
	}
	dl->AddImage( id, p0, p1, ImVec2( 0.f, 0.f ), uv1, tint );
	return true;
}

static void scale_verts( ImDrawList* dl, const int vtx0, const ImVec2 piv, const float sx, const float sy )
{
	ImDrawVert* v   = dl->VtxBuffer.Data + vtx0;
	ImDrawVert* end = dl->VtxBuffer.Data + dl->VtxBuffer.Size;

	for ( ; v < end; ++v ) {
		v->pos.x = piv.x + ( v->pos.x - piv.x ) * sx;
		v->pos.y = piv.y + ( v->pos.y - piv.y ) * sy;
	}
}

/* render thread: picture for split tags, text pass for the
   rest. shadow + label share one warp; SubPixelText stops gliding tags stair-stepping */
static void draw_mito_frame( ImDrawList* dl, const mito_frame_t& f )
{
	ImFont* const font = g_render.live_font( f.m_font );
	if ( !font || font != f.m_font )
		return;

	const float fs = f.m_fs;
	const int n    = f.m_n;

	for ( int i = 0; i < n; ++i )
		if ( f.m_ents[ i ].m_font != font && g_render.live_font( f.m_ents[ i ].m_font ) != f.m_ents[ i ].m_font )
			return;

	mig::Tag tags[ label_max ];
	int map[ label_max ]{ };
	int nt           = 0;
	unsigned tint_a  = 0;
	unsigned shade_a = 0;

	for ( int i = 0; i < n; ++i )
		if ( f.m_in[ i ] )
			map[ i ] = nt++;

	for ( int i = 0; i < n; ++i ) {
		if ( !f.m_in[ i ] )
			continue;

		const mito_ent_t& e = f.m_ents[ i ];
		mig::Tag& t         = tags[ map[ i ] ];
		t.text              = e.m_text;
		t.pos               = e.m_pos;
		t.size              = e.m_size;
		t.raw               = e.m_raw;
		t.col[ 0 ]          = e.m_rgb[ 0 ];
		t.col[ 1 ]          = e.m_rgb[ 1 ];
		t.col[ 2 ]          = e.m_rgb[ 2 ];
		t.parent            = f.m_parent[ i ] >= 0 ? map[ f.m_parent[ i ] ] : f.m_parent[ i ];
		t.font              = e.m_font;
		t.fs                = e.m_fs;

		tint_a  = std::max( tint_a, ( e.m_col >> IM_COL32_A_SHIFT ) & 0xFFu );
		shade_a = std::max( shade_a, ( e.m_shadow >> IM_COL32_A_SHIFT ) & 0xFFu );
	}

	bool imaged[ label_max ]{ };
	if ( nt && !f.m_squash && draw_mitosis( dl, font, fs, tags, nt, f.m_axis_x, IM_COL32( 255, 255, 255, tint_a ), IM_COL32( 0, 0, 0, shade_a ), f.m_blur, f.m_off ) )
		for ( int i = 0; i < n; ++i )
			imaged[ i ] = f.m_in[ i ];

	for ( int i = 0; i < n; ++i ) {
		const mito_ent_t& e = f.m_ents[ i ];
		if ( imaged[ i ] || e.m_raw <= 0.001f )
			continue;

		ImFont* const ef = e.m_font;
		const float efs  = e.m_fs;

		const int vs    = dl->VtxBuffer.Size;
		const ImVec2 sp = ImVec2( e.m_pos.x + f.m_off.x, e.m_pos.y + f.m_off.y );
		const bool hard = ( e.m_shadow & IM_COL32_A_MASK ) && !( f.m_blur > 0.f && soft_text( dl, ef, efs, sp, e.m_text, e.m_shadow, f.m_blur ) );

		dl->PushTextureID( ef->ContainerAtlas->TexID );
		const int v0 = dl->VtxBuffer.Size;
		if ( hard )
			dl->AddText( ef, efs, sp, e.m_shadow, e.m_text );
		if ( e.m_outline & IM_COL32_A_MASK )
			ring_each( f.m_ring_px, [ & ]( const float dx, const float dy ) {
				dl->AddText( ef, efs, ImVec2( e.m_pos.x + dx, e.m_pos.y + dy ), e.m_outline, e.m_text );
			} );
		dl->AddText( ef, efs, e.m_pos, e.m_col, e.m_text );
		mig::SubPixelText( dl, v0, e.m_pos, ef );
		if ( e.m_sx != 1.f || e.m_sy != 1.f )
			scale_verts( dl, vs, e.m_piv, e.m_sx, e.m_sy );
		dl->PopTextureID( );
	}
}

constexpr float k_squash_stretch = 0.35f;
constexpr float k_squash_recoil  = 0.12f;

static void mito_squash( mito_frame_t& f )
{
	constexpr float pi = 3.14159265f;
	const bool ax      = f.m_axis_x;

	ImVec2 ctr[ label_max ];
	for ( int i = 0; i < f.m_n; ++i )
		ctr[ i ] = ImVec2( f.m_ents[ i ].m_pos.x + f.m_ents[ i ].m_size.x * 0.5f, f.m_ents[ i ].m_pos.y + f.m_ents[ i ].m_fs * 0.5f );

	for ( int i = 0; i < f.m_n; ++i ) {
		const int par = f.m_parent[ i ];
		if ( par == -2 )
			continue;

		mito_ent_t& e     = f.m_ents[ i ];
		const float t     = mig::Sat( e.m_raw );
		const float g     = std::max( mig::EaseOut( t ), 0.f );
		const float s     = 1.f + k_squash_stretch * std::sin( pi * t );
		ImVec2 c          = ctr[ i ];

		if ( par >= 0 ) {
			const float k = 1.f - ( 1.f - t ) * ( 1.f - t ) * ( 1.f - t );
			c             = ImVec2( ctr[ par ].x + ( ctr[ i ].x - ctr[ par ].x ) * k, ctr[ par ].y + ( ctr[ i ].y - ctr[ par ].y ) * k );
			e.m_pos.x += c.x - ctr[ i ].x;
			e.m_pos.y += c.y - ctr[ i ].y;

			mito_ent_t& p = f.m_ents[ par ];
			if ( f.m_parent[ par ] == -2 ) {
				const float r    = 1.f + k_squash_recoil * std::sin( pi * t );
				const float pc   = ax ? ctr[ par ].x : ctr[ par ].y;
				const float oc   = ax ? ctr[ i ].x : ctr[ i ].y;
				const float half = ax ? p.m_size.x * 0.5f : p.m_fs * 0.5f;
				const float piv  = pc - ( oc >= pc ? half : -half );
				p.m_piv          = ax ? ImVec2( piv, ctr[ par ].y ) : ImVec2( ctr[ par ].x, piv );
				p.m_sx           = ax ? r : 1.f / r;
				p.m_sy           = ax ? 1.f / r : r;
			}
		}

		e.m_piv = c;
		e.m_sx  = ax ? g * s : g / s;
		e.m_sy  = ax ? g / s : g * s;
	}
}

static void mitosis_queue( const std::shared_ptr< mito_frame_t >& frame )
{
	mito_frame_t& f = *frame;
	const int n     = f.m_n;
	if ( n <= 0 )
		return;

	const float fs    = f.m_fs;
	mito_ent_t* ents  = f.m_ents;
	const auto centre = [ & ]( const int i ) { return ImVec2( ents[ i ].m_pos.x + ents[ i ].m_size.x * 0.5f, ents[ i ].m_pos.y + ents[ i ].m_fs * 0.5f ); };

	float raw[ label_max ];
	for ( int i = 0; i < n; ++i ) {
		raw[ i ]    = ents[ i ].m_raw;
		f.m_in[ i ] = false;
	}

	for ( int i = 0; i < n; ++i ) {
		f.m_parent[ i ] = -2;
		if ( raw[ i ] >= 0.995f )
			continue;

		const int par   = mig::PickParent( raw, n, i );
		f.m_parent[ i ] = par;

		if ( par == -1 ) {
			ents[ i ].m_piv = centre( i );
			ents[ i ].m_sx = ents[ i ].m_sy = mig::PopAt( raw[ i ] );
			f.m_in[ i ]                     = raw[ i ] > 0.001f;
			continue;
		}

		f.m_in[ i ] = f.m_in[ par ] = true;

		for ( const int k : { i, par } ) {
			const ImVec2 c = centre( k ), o = centre( k == i ? par : i );
			mito_ent_t& e  = ents[ k ];

			if ( f.m_axis_x ) {
				const miw::Warp w = miw::SplitWarp( c.x, e.m_size.x * 0.5f, o.x, raw[ i ] );
				e.m_piv           = ImVec2( w.piv, c.y );
				e.m_sx            = w.sa;
				e.m_sy            = w.sp;
			} else {
				const miw::Warp w = miw::SplitWarp( c.y, e.m_fs * 0.5f, o.y, raw[ i ] );
				e.m_piv           = ImVec2( c.x, w.piv );
				e.m_sy            = w.sa;
				e.m_sx            = w.sp;
			}
		}
	}

	if ( f.m_squash )
		mito_squash( f );

	static bool s_was_moving[ 2 ]{ };
	bool moving = false;
	for ( int i = 0; i < n; ++i )
		moving |= f.m_parent[ i ] != -2;
	if ( moving && !s_was_moving[ f.m_axis_x ] )
		for ( int i = 0; i < n; ++i )
			if ( f.m_parent[ i ] != -2 )
				botox_dbg_log( "[mito] %s start n=%d i=%d '%s' par=%d raw=%.2f pos=%.1f,%.1f size=%.1fx%.1f fs=%.1f", f.m_axis_x ? "row" : "stack", n, i,
				               ents[ i ].m_text, f.m_parent[ i ], raw[ i ], ents[ i ].m_pos.x, ents[ i ].m_pos.y, ents[ i ].m_size.x, ents[ i ].m_size.y, fs );
	s_was_moving[ f.m_axis_x ] = moving;

	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_callback,
	                                   std::make_any< callback_draw_object_t >(
										   callback_draw_object_t{ [ frame ]( ImDrawList* dl ) { draw_mito_frame( dl, *frame ); } } ) );
}

static void mito_add_ent( mito_frame_t& f, const char* text, const ImU32 col, const ImU32 shadow, const float r, const float g, const float b,
                          const float raw, const float anim, ImFont* const font = nullptr, const float fs = 0.f, const ImU32 outline = 0u )
{
	if ( f.m_n >= label_max )
		return;

	mito_ent_t& e = f.m_ents[ f.m_n++ ];
	std::snprintf( e.m_text, sizeof( e.m_text ), "%s", text );
	e.m_col      = col;
	e.m_shadow   = shadow;
	e.m_outline  = outline;
	e.m_raw      = raw;
	e.m_anim     = anim;
	e.m_font     = font ? font : f.m_font;
	e.m_fs       = font ? fs : f.m_fs;
	e.m_size     = e.m_font->CalcTextSizeA( e.m_fs, FLT_MAX, 0.f, e.m_text );
	e.m_rgb[ 0 ] = r;
	e.m_rgb[ 1 ] = g;
	e.m_rgb[ 2 ] = b;
	e.m_piv      = ImVec2( 0.f, 0.f );
	e.m_sx = e.m_sy = 1.f;
}

static void mito_layout_row( mito_frame_t& f, const float spacing_raw, const float y_raw )
{
	const auto even     = []( const float v ) { return 2.f * std::ceil( v * 0.5f ); };
	const float spacing = even( spacing_raw );

	float total = 0.f;
	for ( int i = 0; i < f.m_n; ++i ) {
		if ( i )
			total += spacing * f.m_ents[ i ].m_anim;
		total += even( f.m_ents[ i ].m_size.x ) * f.m_ents[ i ].m_anim;
	}

	const float y = std::floor( y_raw );
	float x       = std::floor( g_ctx.m_width * 0.5f ) - total * 0.5f;

	for ( int i = 0; i < f.m_n; ++i ) {
		mito_ent_t& e = f.m_ents[ i ];
		if ( i )
			x += spacing * e.m_anim;

		const float w    = even( e.m_size.x );
		const float slot = w * e.m_anim;
		e.m_pos          = ImVec2( x + ( slot - w ) * 0.5f + std::floor( ( w - e.m_size.x ) * 0.5f ), y );
		x += slot;
	}
}

struct key_particle_t {
	float m_x, m_y, m_vx, m_vy;
	float m_life, m_max_life;
	float m_radius;
	ImVec4 m_color;
};

constexpr float k_part_gap = 0.25f;
constexpr std::size_t k_part_max = 600;

static std::vector< key_particle_t > s_key_particles;
static bool s_part_was[ label_max ]{ };
static bool s_part_want[ label_max ]{ };
static float s_part_want_at[ label_max ]{ };
static float s_part_last[ label_max ]{ };
static bool s_part_seen[ label_max ]{ };
static c_vector_2d s_part_at[ label_max ]{ };

static float part_rand( const float lo, const float hi )
{
	static std::mt19937 gen( std::random_device{ }( ) );
	return std::uniform_real_distribution< float >( lo, hi )( gen );
}

static void part_mark( const int id, const float x, const float y, const ImVec2& size )
{
	s_part_seen[ id ] = true;
	s_part_at[ id ]   = c_vector_2d( x + size.x * 0.5f, y + size.y * 0.5f );
}

/* the label's PERFORMED count: run_detections' fires off cmd STARTs ( points_on_trick ), the same ones chat / sound use, so a
   plan / sim lighting the label never bursts. -1 = no physical detect, the label's success edge bursts */
static int part_fired( const int id )
{
	const auto trick = [ ]( const e_points_trick t ) { return g_trick_fired[ static_cast< int >( t ) ]; };
	switch ( id ) {
	case label_eb:   return trick( e_points_trick::edge_bug );
	case label_ps:
	case label_ast:
	case label_tung: return trick( e_points_trick::pixel_surf );
	case label_jb:   return trick( e_points_trick::jump_bug );
	case label_tb:
	case label_esurf: return trick( e_points_trick::texture_bug );
	case label_air:  return trick( e_points_trick::air_stuck );
	case label_wc:   return trick( e_points_trick::wall_climb );
	case label_fr:   return trick( e_points_trick::fireman );
	case label_es:   return g_es_fired;
	default:         return -1;
	}
}
static int s_part_fired[ label_max ]{ };

static void part_track( const std::vector< key_pending_t >& pending, const float real )
{
	const float cooldown = std::clamp( GET_VARIABLE( g_variables.m_key_indicators_particle_cooldown, float ), 0.f, 5.f );

	/* every label, listed or not: a fire while hidden never bursts later */
	bool fired[ label_max ]{ };
	for ( int id = 0; id < label_max; ++id ) {
		const int n        = part_fired( id );
		fired[ id ]        = n >= 0 && n != s_part_fired[ id ];
		s_part_fired[ id ] = n;
	}

	bool listed[ label_max ]{ };
	for ( const auto& p : pending ) {
		const int id = p.m_id;
		listed[ id ] = true;

		const bool phys = s_part_fired[ id ] >= 0;
		const bool hit  = p.m_detect && p.m_active && ( phys ? fired[ id ] : p.m_success );
		if ( hit && ( phys || !s_part_was[ id ] ) && ( real - s_part_last[ id ] >= cooldown || real < s_part_last[ id ] ) ) {
			s_part_want[ id ]    = true;
			s_part_want_at[ id ] = real;
			s_part_last[ id ]    = real;
		}
		s_part_was[ id ] = hit;
	}

	for ( int id = 0; id < label_max; ++id )
		if ( !listed[ id ] )
			s_part_was[ id ] = s_part_want[ id ] = false;
}

static void part_burst( const c_vector_2d& at )
{
	const int amount    = std::clamp( GET_VARIABLE( g_variables.m_key_indicators_particle_amount, int ), 1, 60 );
	const float size    = GET_VARIABLE( g_variables.m_key_indicators_particle_size, float );
	const float speed   = GET_VARIABLE( g_variables.m_key_indicators_particle_speed, float );
	const bool random   = GET_VARIABLE( g_variables.m_key_indicators_particle_random, bool );
	const float life    = std::clamp( GET_VARIABLE( g_variables.m_key_indicators_particle_life, float ), 0.05f, 10.f );
	const ImVec4 picked = GET_VARIABLE( g_variables.m_key_indicators_particle_color, c_color ).get_vec4( );

	const std::size_t over = s_key_particles.size( ) + static_cast< std::size_t >( amount );
	if ( over > k_part_max )
		s_key_particles.erase( s_key_particles.begin( ), s_key_particles.begin( ) + static_cast< std::ptrdiff_t >( over - k_part_max ) );

	for ( int i = 0; i < amount; ++i ) {
		key_particle_t p;
		const float ang = part_rand( 0.f, 6.2831853f );
		const float mag = speed * part_rand( 0.45f, 1.f );
		p.m_x           = at.m_x;
		p.m_y           = at.m_y;
		p.m_vx          = std::cos( ang ) * mag;
		p.m_vy          = std::sin( ang ) * mag;
		p.m_life = p.m_max_life = life * part_rand( 0.7f, 1.f );
		p.m_radius              = std::max( 1.f, size * part_rand( 0.7f, 1.3f ) );

		if ( random ) {
			p.m_color = ImVec4( 0.f, 0.f, 0.f, 1.f );
			ImGui::ColorConvertHSVtoRGB( part_rand( 0.f, 1.f ), part_rand( 0.6f, 0.9f ), 1.f, p.m_color.x, p.m_color.y, p.m_color.z );
		} else
			p.m_color = picked;

		s_key_particles.push_back( p );
	}
}

static void key_particles( )
{
	if ( !GET_VARIABLE( g_variables.m_key_indicators_enable, bool ) || !GET_VARIABLE( g_variables.m_key_indicators_particles, bool ) ) {
		s_key_particles.clear( );
		std::fill( std::begin( s_part_was ), std::end( s_part_was ), false );
		std::fill( std::begin( s_part_want ), std::end( s_part_want ), false );
		std::fill( std::begin( s_part_seen ), std::end( s_part_seen ), false );
		/* fires while off never burst once it's back on */
		for ( int id = 0; id < label_max; ++id )
			s_part_fired[ id ] = part_fired( id );
		return;
	}

	const float real = g_interfaces.m_global_vars_base->m_real_time;
	for ( int id = 0; id < label_max; ++id ) {
		if ( !s_part_want[ id ] )
			continue;

		const float age = real - s_part_want_at[ id ];
		if ( s_part_seen[ id ] && age >= 0.f && age <= k_part_gap )
			part_burst( s_part_at[ id ] );
		if ( s_part_seen[ id ] || age < 0.f || age > k_part_gap )
			s_part_want[ id ] = false;
	}
	std::fill( std::begin( s_part_seen ), std::end( s_part_seen ), false );

	const float dt      = std::min( ImGui::GetIO( ).DeltaTime, 0.1f );
	const float gravity = GET_VARIABLE( g_variables.m_key_indicators_particle_gravity, float );

	std::size_t live = 0;
	for ( auto& p : s_key_particles ) {
		p.m_life -= dt;
		if ( p.m_life <= 0.f )
			continue;

		p.m_vy += gravity * dt;
		p.m_x += p.m_vx * dt;
		p.m_y += p.m_vy * dt;

		ImVec4 c = p.m_color;
		c.w *= std::clamp( p.m_life / p.m_max_life, 0.f, 1.f );
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle,
		                                   std::make_any< filled_circle_draw_object_t >( c_vector_2d( p.m_x, p.m_y ), p.m_radius,
		                                                                                 ImGui::ColorConvertFloat4ToU32( c ), 0 ) );
		s_key_particles[ live++ ] = p;
	}
	s_key_particles.resize( live );
}

struct smooth_tag_t {
	int m_id = 0;
	std::string m_text = { };
	ImVec2 m_size      = { };
	bool m_has_detect  = false;
	bool m_predicted   = false;

	float m_opacity = 0.f, m_target_opacity = 1.f;
	float m_x = 0.f, m_target_x = 0.f, m_spawn_x = 0.f;
	float m_y = 0.f, m_target_y = 0.f;
	float m_spawn_y = 0.f;
	bool m_removing = false;
	bool m_fresh    = false;

	float m_detect = 0.f, m_target_detect = 0.f;
	float m_appear_until = 0.f, m_detect_hold_until = 0.f;
};

static std::vector< smooth_tag_t > s_smooth_tags;
static float s_smooth_clock = 0.f;

constexpr float k_smooth_opacity_in = 22.f, k_smooth_opacity_out = 23.5f, k_smooth_position = 9.5f, k_smooth_appear_delay = 0.f;
constexpr float k_smooth_spawn_base = 900.f, k_smooth_spawn_step = 25.f;
constexpr float k_smooth_spawn_drop = 60.f;

static float smooth_spawn_x( const std::size_t active )
{
	const float W     = g_ctx.m_width;
	const float scale = W > 0.f ? W / 1920.f : 1.f;
	return W - ( k_smooth_spawn_base - k_smooth_spawn_step * static_cast< float >( active > 1 ? active - 2 : 0 ) ) * scale;
}

static void smooth_tag_step( smooth_tag_t& t, const bool company, const float now, const float dt, const bool axis_y = false )
{
	float position_smooth = k_smooth_position;
	if ( t.m_removing && company ) {
		const float to_go          = axis_y ? t.m_target_y - t.m_y : t.m_target_x - t.m_x;
		const float total_distance = std::max( 1.f, std::fabs( to_go ) + 220.f );
		const float remaining      = std::clamp( to_go / total_distance, 0.f, 1.f );
		position_smooth *= 0.7f + 1.5f * remaining;
	}

	const float opacity_target = ( !t.m_removing && now < t.m_appear_until ) ? 0.f : t.m_target_opacity;
	t.m_opacity = rido_lerp( t.m_opacity, opacity_target, t.m_removing ? k_smooth_opacity_out : k_smooth_opacity_in, dt );
	t.m_x       = rido_lerp( t.m_x, t.m_target_x, axis_y ? k_smooth_position : position_smooth, dt );
	t.m_y       = rido_lerp( t.m_y, t.m_target_y, axis_y ? position_smooth : k_smooth_position, dt );

	if ( std::fabs( t.m_target_x - t.m_x ) <= 0.75f )
		t.m_x = t.m_target_x;
	if ( std::fabs( t.m_target_y - t.m_y ) <= 0.75f )
		t.m_y = t.m_target_y;
	if ( std::fabs( t.m_target_opacity - t.m_opacity ) <= 0.015f )
		t.m_opacity = t.m_target_opacity;
}

static void keybinds_smooth( const std::vector< key_pending_t >& pending, ImFont* font, const float fs, const float dt, const float base,
                             const bool horizontal, const bool shadow_detect, const shadow_opts_t& shadow )
{
	constexpr float k_detect_smooth = 20.f, k_detect_hold = 0.25f;

	s_smooth_clock += dt;
	const float now = s_smooth_clock;
	const float W   = g_ctx.m_width;

	const auto find = [ & ]( const int id ) -> int {
		for ( std::size_t i = 0; i < s_smooth_tags.size( ); ++i )
			if ( s_smooth_tags[ i ].m_id == id )
				return static_cast< int >( i );
		return -1;
	};

	const auto live_count = [ & ]( ) {
		std::size_t count = 0;
		for ( const auto& t : s_smooth_tags )
			if ( !t.m_removing )
				++count;
		return count;
	};

	std::size_t final_active = live_count( );
	for ( const auto& p : pending ) {
		if ( !p.m_active )
			continue;

		const int i = find( p.m_id );
		if ( i < 0 || s_smooth_tags[ i ].m_removing )
			++final_active;
	}
	const float batch_spawn = smooth_spawn_x( final_active );

	const float drop = k_smooth_spawn_drop * ( g_ctx.m_height > 0.f ? g_ctx.m_height / 1080.f : 1.f );

	const int origin       = GET_VARIABLE( g_variables.m_key_indicators_smooth_origin, int );
	const bool from_bottom = origin == smooth_origin_bottom || ( origin != smooth_origin_right && !horizontal );

	const auto start_removing = [ & ]( smooth_tag_t& t ) {
		t.m_removing       = true;
		t.m_target_opacity = 0.f;
		if ( !from_bottom )
			t.m_target_x = t.m_spawn_x != 0.f ? t.m_spawn_x : smooth_spawn_x( live_count( ) );
		else
			t.m_target_y = t.m_spawn_y != 0.f ? t.m_spawn_y : t.m_y + drop;
	};

	bool listed[ label_max ]{ };
	for ( const auto& p : pending ) {
		listed[ p.m_id ] = true;
		int i            = find( p.m_id );

		if ( p.m_active ) {
			if ( i < 0 ) {
				smooth_tag_t t{ };
				t.m_id                = p.m_id;
				t.m_opacity           = 0.f;
				t.m_target_opacity    = 1.f;
				t.m_appear_until      = now + k_smooth_appear_delay;
				t.m_detect_hold_until = p.m_success ? now + k_detect_hold : 0.f;
				t.m_target_detect     = p.m_success ? 1.f : 0.f;
				t.m_x = t.m_target_x = t.m_spawn_x = batch_spawn;
				t.m_fresh                          = true;
				s_smooth_tags.push_back( std::move( t ) );
				i = static_cast< int >( s_smooth_tags.size( ) ) - 1;
			} else {
				smooth_tag_t& t = s_smooth_tags[ i ];

				if ( t.m_removing ) {
					t.m_removing       = false;
					t.m_opacity        = 0.f;
					t.m_target_opacity = 1.f;
					t.m_appear_until   = now + k_smooth_appear_delay;
					t.m_spawn_x        = batch_spawn;
					t.m_x              = batch_spawn;
					t.m_fresh          = true;
				}

				if ( p.m_success )
					t.m_detect_hold_until = now + k_detect_hold;
				t.m_target_detect = ( p.m_success || t.m_detect_hold_until > now ) ? 1.f : 0.f;
			}

			s_smooth_tags[ i ].m_text       = p.m_text;
			s_smooth_tags[ i ].m_has_detect = p.m_detect;
			s_smooth_tags[ i ].m_predicted  = p.m_predicted;
		} else if ( i >= 0 && !s_smooth_tags[ i ].m_removing )
			start_removing( s_smooth_tags[ i ] );
	}

	for ( auto& t : s_smooth_tags )
		if ( !listed[ t.m_id ] && !t.m_removing )
			start_removing( t );

	for ( auto& t : s_smooth_tags )
		t.m_size = font->CalcTextSizeA( fs, FLT_MAX, 0.f, t.m_text.c_str( ) );

	const float gap = static_cast< float >( GET_VARIABLE( g_variables.m_key_indicators_spacing, int ) );
	const float y0  = g_ctx.m_height - base;
	float spawn_y   = y0 + drop;
	if ( horizontal ) {
		const float spacing = fs * 0.2f + gap;
		float total         = -spacing;
		for ( const auto& t : s_smooth_tags )
			if ( !t.m_removing )
				total += t.m_size.x + spacing;

		float x = W * 0.5f - total * 0.5f;
		for ( auto& t : s_smooth_tags ) {
			if ( t.m_removing )
				continue;

			t.m_target_y = y0;
			t.m_target_x = x;
			x += t.m_size.x + spacing;
		}
	} else {
		float y = y0;
		for ( auto& t : s_smooth_tags ) {
			if ( t.m_removing )
				continue;

			t.m_target_x = ( W - t.m_size.x ) * 0.5f;
			t.m_target_y = y;
			y += t.m_size.y + gap;
		}
		spawn_y = y + drop;
	}

	for ( auto& t : s_smooth_tags ) {
		if ( t.m_fresh ) {
			if ( !from_bottom )
				t.m_y = t.m_target_y;
			else {
				t.m_x = t.m_target_x;
				t.m_y = t.m_spawn_y = spawn_y;
			}
			t.m_fresh = false;
		}
	}

	std::size_t visible = 0;
	for ( const auto& t : s_smooth_tags )
		if ( t.m_opacity > 0.01f || !t.m_removing )
			++visible;

	for ( auto it = s_smooth_tags.begin( ); it != s_smooth_tags.end( ); ) {
		smooth_tag_t& t = *it;

		smooth_tag_step( t, visible > 1, now, dt, from_bottom );

		t.m_detect = rido_lerp( t.m_detect, t.m_target_detect, k_detect_smooth, dt );

		if ( t.m_opacity > 0.01f ) {
			const indicator_colors_t colors =
				t.m_has_detect ? key_colors_at( t.m_detect, shadow_detect, shadow.m_color, t.m_predicted, shadow.m_outline_color )
				               : key_colors_plain( shadow.m_color, shadow.m_outline_color );

			push_label( font, fs, c_vector_2d( std::round( t.m_x + 0.2f ), std::round( t.m_y ) ), t.m_text, colors.m_text.get_u32( t.m_opacity ),
			            colors.m_shadow.get_u32( t.m_opacity ), shadow, colors.m_outline.get_u32( t.m_opacity ) );
			if ( !t.m_removing )
				part_mark( t.m_id, t.m_x, t.m_y, t.m_size );
		}

		if ( t.m_removing && t.m_opacity < 0.02f )
			it = s_smooth_tags.erase( it );
		else
			++it;
	}
}

static unsigned long long s_mito_fire[ label_max ]{ };
static float s_mito_show[ label_max ]{ };
static float s_mito_hot[ label_max ]{ };
static float s_mito_slot[ label_max ]{ };

static void mito_reset_keybinds( )
{
	for ( int i = 0; i < label_max; ++i )
		s_mito_show[ i ] = s_mito_hot[ i ] = s_mito_slot[ i ] = 0.f;
}

static void keybinds_mitosis( const std::vector< key_pending_t >& pending, ImFont* font, const float fs, const float dt, const float base,
                              const bool horizontal, const bool shadow_detect, const shadow_opts_t& shadow, const bool squash )
{
	constexpr unsigned long long k_flash_ms = 180;
	constexpr float k_show_speed = 4.f;
	constexpr float k_hot_speed  = 26.f;

	const unsigned long long now = GetTickCount64( );

	bool listed[ label_max ]{ };
	for ( const auto& p : pending )
		listed[ p.m_id ] = true;
	for ( int id = 0; id < label_max; ++id )
		if ( !listed[ id ] )
			s_mito_show[ id ] = s_mito_hot[ id ] = s_mito_slot[ id ] = 0.f;

	const auto frame = std::make_shared< mito_frame_t >( );
	mito_frame_t& f  = *frame;
	f.m_font         = font;
	f.m_fs           = fs;
	f.m_axis_x       = horizontal;
	f.m_blur         = shadow.m_blur;
	f.m_squash       = squash;
	f.m_off          = ImVec2( shadow.m_off_x, shadow.m_off_y );
	f.m_ring_px      = shadow.m_outline_px;

	int ent_id[ label_max ]{ };

	for ( const auto& p : pending ) {
		const int slot = p.m_id;

		if ( p.m_success )
			s_mito_fire[ slot ] = now;
		const bool flash = s_mito_fire[ slot ] != 0 && now - s_mito_fire[ slot ] < k_flash_ms;

		const float width   = font->CalcTextSizeA( fs, FLT_MAX, 0.f, p.m_text.c_str( ) ).x;
		s_mito_show[ slot ] = mig::FadeStep( s_mito_show[ slot ], p.m_active, dt, mig::SplitSpeed( k_show_speed, width, fs ) );
		s_mito_hot[ slot ]  = mig::Sat( s_mito_hot[ slot ] + dt * k_hot_speed * ( flash ? 1.f : -1.f ) );
		s_mito_slot[ slot ] = mig::Follow( s_mito_slot[ slot ], mig::EaseOut( s_mito_show[ slot ] ), dt, mig::kSlotRate );

		if ( s_mito_show[ slot ] <= 0.001f && s_mito_slot[ slot ] <= 0.002f ) {
			s_mito_slot[ slot ] = 0.f;
			continue;
		}

		const indicator_colors_t colors =
			p.m_detect ? key_colors_at( s_mito_hot[ slot ], shadow_detect, shadow.m_color, p.m_predicted, shadow.m_outline_color )
			           : key_colors_plain( shadow.m_color, shadow.m_outline_color );

		if ( f.m_n < label_max )
			ent_id[ f.m_n ] = slot;
		mito_add_ent( f, p.m_text.c_str( ), colors.m_text.get_u32( ), shadow.m_on ? colors.m_shadow.get_u32( ) : 0u,
		              colors.m_text.base< e_color_type::color_type_r >( ),
		              colors.m_text.base< e_color_type::color_type_g >( ), colors.m_text.base< e_color_type::color_type_b >( ), s_mito_show[ slot ],
		              s_mito_slot[ slot ], nullptr, 0.f, shadow.m_outline ? colors.m_outline.get_u32( ) : 0u );
	}

	if ( f.m_n <= 0 )
		return;

	const float gap = static_cast< float >( GET_VARIABLE( g_variables.m_key_indicators_spacing, int ) );
	if ( horizontal )
		mito_layout_row( f, f.m_fs * 0.2f + gap, g_ctx.m_height - base );
	else {
		float y = std::floor( g_ctx.m_height - base );
		for ( int i = 0; i < f.m_n; ++i ) {
			mito_ent_t& e = f.m_ents[ i ];
			e.m_pos       = ImVec2( std::floor( ( g_ctx.m_width - e.m_size.x ) * 0.5f ), y );
			y += ( e.m_size.y + gap ) * e.m_anim;
		}
	}

	for ( int i = 0; i < f.m_n; ++i )
		part_mark( ent_id[ i ], f.m_ents[ i ].m_pos.x, f.m_ents[ i ].m_pos.y, f.m_ents[ i ].m_size );

	mitosis_queue( frame );
}

static void kamibebra_text( ImFont* font, const float fs, const float x, const float y, const std::string& text, const ImVec4& color )
{
	push_label( font, fs, c_vector_2d( std::round( x ), std::round( y ) ), text, ImGui::ColorConvertFloat4ToU32( color ), 0u, { false, 0.f } );
}

/* own 2 layer drop shadow while the shadow option is off; on = the shared shadow / outline options */
static void kamibebra_shadowed( ImFont* font, const float fs, const float s, const float x, const float y, const std::string& text, const ImVec4& color,
                                const shadow_opts_t& shadow, const float shadow_alpha )
{
	if ( !shadow.m_on ) {
		const float a = ( 200.f / 255.f ) * color.w;
		kamibebra_text( font, fs, x + std::round( s ), y + std::round( s ), text, ImVec4( 0.f, 0.f, 0.f, a ) );
		kamibebra_text( font, fs, x + std::round( 2.f * s ), y + std::round( 2.f * s ), text, ImVec4( 0.f, 0.f, 0.f, a * 0.25f ) );
	}
	push_label( font, fs, c_vector_2d( std::round( x ), std::round( y ) ), text, ImGui::ColorConvertFloat4ToU32( color ), shadow_u32( shadow.m_color, shadow_alpha ),
	            shadow );
}

struct kamibebra_tag_t {
	int m_id           = 0;
	std::string m_text = { };
	ImVec2 m_size      = { };
	float m_opacity = 0.f, m_target_opacity = 1.f;
	float m_x = 0.f, m_target_x = 0.f, m_spawn_x = 0.f;
	float m_y = 0.f, m_target_y = 0.f;
	bool m_removing = false, m_detect = false, m_predicted = false;
	float m_detect_t = 0.f, m_target_detect = 0.f;
	float m_appear_until = 0.f, m_detect_hold_until = 0.f;
};

static std::vector< kamibebra_tag_t > s_kamibebra_tags;
static float s_kamibebra_clock = 0.f;

static void keybinds_kamibebra( const std::vector< key_pending_t >& pending, ImFont* font, const float fs, const float scale, const float dt, const float base,
                                const bool horizontal, const shadow_opts_t& shadow, const bool shadow_detect )
{
	constexpr float k_opacity = 10.f, k_position = 9.5f, k_detect = 4.5f, k_detect_hold = 0.25f, k_detect_max = 0.75f, k_appear = 0.09f,
	                k_remove_opacity = 2.35f, k_spacing = 4.f;

	const float s = std::clamp( scale, k_indicator_scale_min, k_indicator_scale_max );

	s_kamibebra_clock += dt;
	const float now = s_kamibebra_clock;
	const float W   = g_ctx.m_width;
	const float gap = static_cast< float >( GET_VARIABLE( g_variables.m_key_indicators_spacing, int ) );
	const float y0  = g_ctx.m_height - base;

	const auto find = [ & ]( const int id ) -> kamibebra_tag_t* {
		for ( auto& t : s_kamibebra_tags )
			if ( t.m_id == id )
				return &t;
		return nullptr;
	};

	const auto live_count = [ & ]( ) {
		std::size_t count = 0;
		for ( const auto& t : s_kamibebra_tags )
			if ( !t.m_removing )
				++count;
		return count;
	};

	const auto recalc = [ & ]( ) {
		if ( !horizontal ) {
			float y = y0;
			for ( auto& t : s_kamibebra_tags ) {
				if ( t.m_removing )
					continue;
				t.m_target_x = std::round( ( W - t.m_size.x ) * 0.5f );
				t.m_target_y = y;
				y += t.m_size.y + gap;
			}
			return;
		}

		const float spacing = std::round( k_spacing * s ) + gap;
		float total         = -spacing;
		for ( const auto& t : s_kamibebra_tags )
			if ( !t.m_removing )
				total += t.m_size.x + spacing;

		float x = W * 0.5f - total * 0.5f;
		for ( auto& t : s_kamibebra_tags ) {
			if ( t.m_removing )
				continue;
			t.m_target_x = x;
			t.m_target_y = y0;
			x += t.m_size.x + spacing;
		}
	};

	const auto start_removing = [ & ]( kamibebra_tag_t& t ) {
		t.m_removing       = true;
		t.m_target_opacity = 0.f;
		recalc( );
		t.m_target_x = t.m_spawn_x != 0.f ? t.m_spawn_x : smooth_spawn_x( live_count( ) );
	};

	std::size_t final_active = live_count( );
	for ( const auto& p : pending ) {
		if ( !p.m_active )
			continue;
		const kamibebra_tag_t* const t = find( p.m_id );
		if ( !t || t->m_removing )
			++final_active;
	}
	const float batch_spawn = smooth_spawn_x( final_active );

	bool listed[ label_max ]{ };
	for ( const auto& p : pending ) {
		listed[ p.m_id ]     = true;
		kamibebra_tag_t* tag = find( p.m_id );

		if ( p.m_active ) {
			if ( !tag ) {
				kamibebra_tag_t t{ };
				t.m_id                = p.m_id;
				t.m_text              = p.m_text;
				t.m_size              = font->CalcTextSizeA( fs, FLT_MAX, 0.f, t.m_text.c_str( ) );
				t.m_appear_until      = now + k_appear;
				t.m_detect_hold_until = p.m_success ? now + k_detect_hold : 0.f;
				t.m_target_detect     = p.m_success ? 1.f : 0.f;
				t.m_predicted         = p.m_predicted;
				t.m_x = t.m_target_x = t.m_spawn_x = batch_spawn;
				s_kamibebra_tags.push_back( std::move( t ) );
				recalc( );
				s_kamibebra_tags.back( ).m_x = batch_spawn;
				s_kamibebra_tags.back( ).m_y = s_kamibebra_tags.back( ).m_target_y;
			} else {
				if ( tag->m_removing ) {
					tag->m_removing       = false;
					tag->m_opacity        = 0.f;
					tag->m_target_opacity = 1.f;
					tag->m_appear_until   = now + k_appear;
					tag->m_spawn_x        = batch_spawn;
					recalc( );
					tag->m_x = batch_spawn;
					tag->m_y = tag->m_target_y;
				}
				if ( p.m_success )
					tag->m_detect_hold_until = now + k_detect_hold;
				tag->m_detect        = p.m_success || tag->m_detect_hold_until > now;
				if ( p.m_success )
					tag->m_predicted = p.m_predicted;
				tag->m_target_detect = tag->m_detect ? 1.f : 0.f;
				tag->m_text          = p.m_text;
			}
		} else if ( tag && !tag->m_removing )
			start_removing( *tag );
	}

	for ( auto& t : s_kamibebra_tags )
		if ( !listed[ t.m_id ] && !t.m_removing )
			start_removing( t );

	if ( !s_kamibebra_tags.empty( ) ) {
		for ( auto& t : s_kamibebra_tags )
			t.m_size = font->CalcTextSizeA( fs, FLT_MAX, 0.f, t.m_text.c_str( ) );
		recalc( );
	}

	std::size_t visible = 0;
	for ( const auto& t : s_kamibebra_tags )
		if ( t.m_opacity > 0.01f || !t.m_removing )
			++visible;

	const c_color accent = GET_VARIABLE( g_variables.m_indicator_kamibebra_accent, c_color );
	const ImVec4 acc( accent.base< e_color_type::color_type_r >( ), accent.base< e_color_type::color_type_g >( ), accent.base< e_color_type::color_type_b >( ),
	                  1.f );

	for ( auto it = s_kamibebra_tags.begin( ); it != s_kamibebra_tags.end( ); ) {
		kamibebra_tag_t& t = *it;

		float position_smooth = k_position;
		if ( t.m_removing && visible > 1 ) {
			const float total     = std::max( std::round( s ), std::fabs( t.m_target_x - t.m_x ) + 220.f * s );
			const float remaining = std::clamp( ( t.m_target_x - t.m_x ) / total, 0.f, 1.f );
			position_smooth *= 0.7f + 1.5f * remaining;
		}

		const float opacity_target = ( !t.m_removing && now < t.m_appear_until ) ? 0.f : t.m_target_opacity;
		t.m_opacity                = rido_lerp( t.m_opacity, opacity_target, k_opacity * ( t.m_removing ? k_remove_opacity : 1.f ), dt );
		t.m_x                      = rido_lerp( t.m_x, t.m_target_x, position_smooth, dt );
		t.m_y                      = rido_lerp( t.m_y, t.m_target_y, position_smooth, dt );
		if ( std::fabs( t.m_target_x - t.m_x ) <= std::round( 0.75f * s ) )
			t.m_x = t.m_target_x;
		if ( std::fabs( t.m_target_y - t.m_y ) <= std::round( 0.75f * s ) )
			t.m_y = t.m_target_y;
		if ( std::fabs( t.m_target_opacity - t.m_opacity ) <= 0.015f )
			t.m_opacity = t.m_target_opacity;
		t.m_detect_t = rido_lerp( t.m_detect_t, t.m_target_detect, k_detect, dt );

		const float tx = std::round( t.m_x + 0.2f * s ), ty = std::round( t.m_y );
		if ( t.m_opacity > 0.01f ) {
			const float d = std::min( t.m_detect_t, k_detect_max );
			if ( !shadow.m_on ) {
				ImVec4 shade( acc.x * d, acc.y * d, acc.z * d, ( t.m_detect ? 0.85f : 200.f / 255.f ) * t.m_opacity );
				kamibebra_text( font, fs, tx + std::round( s ), ty + std::round( s ), t.m_text, shade );
				shade.w *= d > 0.01f ? 0.45f : 0.25f;
				kamibebra_text( font, fs, tx + std::round( 2.f * s ), ty + std::round( 2.f * s ), t.m_text, shade );
			}

			if ( d > 0.01f ) {
				const float a = d * t.m_opacity;
				const ImVec4 accent_glow( acc.x, acc.y, acc.z, a * 0.2f ), white_glow( 1.f, 1.f, 1.f, a * 0.3f );
				static constexpr float k_dirs[ 4 ][ 2 ] = { { -1.f, 0.f }, { 1.f, 0.f }, { 0.f, -1.f }, { 0.f, 1.f } };
				for ( const auto& dir : k_dirs ) {
					kamibebra_text( font, fs, tx + dir[ 0 ] * 0.5f * s, ty + dir[ 1 ] * 0.5f * s, t.m_text, accent_glow );
					kamibebra_text( font, fs, tx + dir[ 0 ] * 0.25f * s, ty + dir[ 1 ] * 0.25f * s, t.m_text, white_glow );
				}
			}
		}

		const indicator_colors_t colors =
			key_colors_at( std::min( t.m_detect_t, 1.f ), shadow_detect, shadow.m_color, t.m_predicted, shadow.m_outline_color );
		push_label( font, fs, c_vector_2d( tx, ty ), t.m_text, colors.m_text.get_u32( t.m_opacity ), colors.m_shadow.get_u32( t.m_opacity ), shadow,
		            colors.m_outline.get_u32( t.m_opacity ) );
		if ( !t.m_removing )
			part_mark( t.m_id, tx, ty, t.m_size );

		if ( t.m_removing && t.m_opacity < 0.02f )
			it = s_kamibebra_tags.erase( it );
		else
			++it;
	}
}

struct kami_ghost_t {
	std::string m_text;
	c_vector_2d m_at;
	float m_born;
};

static std::vector< kami_ghost_t > s_kami_key_ghosts;
static bool s_kami_key_was[ label_max ]{ };
static c_vector_2d s_kami_key_at[ label_max ]{ };

static float s_static_key_alpha[ label_max ]{ };

void n_indicators::impl_t::keybind_indicators( )
{
	std::vector< key_pending_t > pending;

	const auto& key_indicators = g_config.get< std::vector< bool > >( g_variables.m_key_indicators );

	const bool custom_labels = GET_VARIABLE( g_variables.m_key_indicators_custom_labels, bool );
	const auto& labels       = GET_VARIABLE( g_variables.m_key_indicators_labels, std::vector< std::string > );

	/* same trick states the chat detections use, so they never disagree */
	const auto trick_states = g_movement.get_trick_states( );

	const shadow_opts_t shadow =
		shadow_opts_of( GET_VARIABLE( g_variables.m_key_indicators_shadow, bool ), GET_VARIABLE( g_variables.m_key_indicators_shadow_blur, float ),
	                    GET_VARIABLE( g_variables.m_key_indicators_shadow_color, c_color ), g_variables.m_key_indicators_outline,
	                    g_variables.m_key_indicators_outline_color, g_variables.m_key_indicators_outline_px, g_variables.m_key_indicators_shadow_x,
	                    g_variables.m_key_indicators_shadow_y );

	const bool shadow_detect = shadow.m_on && GET_VARIABLE( g_variables.m_key_indicators_shadow_detect, bool );

	const auto push = [ & ]( const e_keybind_labels id, const bool detect, const bool success, const bool active, const bool predicted ) {
		const char* name      = k_keybind_labels[ id ][ 1 ];
		const bool use_custom = custom_labels && static_cast< std::size_t >( id ) < labels.size( ) && !labels[ id ].empty( );
		pending.push_back( { id, use_custom ? labels[ id ] : std::string( name ), detect, success, active, predicted } );
	};

	/* lit reason latched per label: the fade back to base keeps the colour it was lit in (a dropped ps
	   prediction never fades out in the success colour) */
	static bool s_lit_predicted[ label_max ]{ };

	const auto add     = [ & ]( const e_keybind_labels id, const bool active ) { push( id, false, false, active, false ); };
	const auto add_hit = [ & ]( const e_keybind_labels id, const bool success, const bool active, const bool predicted = false ) {
		if ( success )
			s_lit_predicted[ id ] = predicted;
		push( id, true, success, active, s_lit_predicted[ id ] );
	};

	if ( GET_VARIABLE( g_variables.edge_bug, bool ) && key_indicators[ e_keybind_indicators::key_eb ] )
		add_hit( label_eb, trick_states.m_edge_bug_queued, g_input.check_input( &GET_VARIABLE( g_variables.edge_bug_key, key_bind_t ) ),
		         trick_states.m_edge_bug_tick > g_interfaces.m_global_vars_base->m_tick_count );

	if ( GET_VARIABLE( g_variables.m_pixel_surf, bool ) && key_indicators[ e_keybind_indicators::key_ps ] )
		add_hit( label_ps, g_movement.m_pixelsurf_data.m_predicted_succesful || trick_states.m_pixel_surf_now,
		         g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_surf_key, key_bind_t ) ), !trick_states.m_pixel_surf_now );

	if ( GET_VARIABLE( g_variables.m_edge_jump, bool ) && key_indicators[ e_keybind_indicators::key_ej ] )
		add( label_ej, g_input.check_input( &GET_VARIABLE( g_variables.m_edge_jump_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_long_jump, bool ) && key_indicators[ e_keybind_indicators::key_lj ] )
		add( label_lj, g_input.check_input( &GET_VARIABLE( g_variables.m_long_jump_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_delay_hop, bool ) && key_indicators[ e_keybind_indicators::key_dh ] )
		add( label_dh, g_input.check_input( &GET_VARIABLE( g_variables.m_delay_hop_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_mini_jump, bool ) && key_indicators[ e_keybind_indicators::key_mj ] )
		add( label_mj, g_input.check_input( &GET_VARIABLE( g_variables.m_mini_jump_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_jump_bug, bool ) && key_indicators[ e_keybind_indicators::key_jb ] )
		add_hit( label_jb, g_movement.m_jumpbug_data.m_can_jb, g_input.check_input( &GET_VARIABLE( g_variables.m_jump_bug_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_texture_bug, bool ) && key_indicators.size( ) > e_keybind_indicators::key_tb &&
	     key_indicators[ e_keybind_indicators::key_tb ] )
		add_hit( label_tb, trick_states.m_texture_bug_now, g_input.check_input( &GET_VARIABLE( g_variables.m_texture_bug_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_air_stuck, bool ) && key_indicators.size( ) > e_keybind_indicators::key_as &&
	     key_indicators[ e_keybind_indicators::key_as ] )
		add_hit( label_air, trick_states.m_air_stuck_now, g_input.check_input( &GET_VARIABLE( g_variables.m_air_stuck_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_wall_climb, bool ) && key_indicators.size( ) > e_keybind_indicators::key_wc &&
	     key_indicators[ e_keybind_indicators::key_wc ] )
		add_hit( label_wc, trick_states.m_wall_climb_now, g_input.check_input( &GET_VARIABLE( g_variables.m_wall_climb_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_fire_man, bool ) && key_indicators.size( ) > e_keybind_indicators::key_fr &&
	     key_indicators[ e_keybind_indicators::key_fr ] )
		add_hit( label_fr, g_movement.m_fireman_data.is_ladder, g_input.check_input( &GET_VARIABLE( g_variables.m_fire_man_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_auto_strafe, bool ) && key_indicators.size( ) > e_keybind_indicators::key_st &&
	     key_indicators[ e_keybind_indicators::key_st ] )
		add( label_as, g_input.check_input( &GET_VARIABLE( g_variables.m_auto_strafe_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_pixel_surf_assist, bool ) && key_indicators.size( ) > e_keybind_indicators::key_psa &&
	     key_indicators[ e_keybind_indicators::key_psa ] )
		add_hit( label_ast, HITGODA, g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_surf_assist_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_bouncee_assist, bool ) && key_indicators.size( ) > e_keybind_indicators::key_psa &&
	     key_indicators[ e_keybind_indicators::key_psa ] )
		add_hit( label_bast, HITGODA2, g_input.check_input( &GET_VARIABLE( g_variables.m_bounce_assist_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_pixel_jump_assist, bool ) && key_indicators.size( ) > e_keybind_indicators::key_psa &&
	     key_indicators[ e_keybind_indicators::key_psa ] )
		add_hit( label_pjast, HITGODA3, g_input.check_input( &GET_VARIABLE( g_variables.m_pixel_jump_assist_key, key_bind_t ) ) );

	const auto add_plain = [ & ]( const bool enabled, const e_keybind_indicators toggle, const e_keybind_labels id, const std::uint32_t key ) {
		if ( enabled && key_indicators.size( ) > toggle && key_indicators[ toggle ] )
			add( id, g_input.check_input( &GET_VARIABLE( key, key_bind_t ) ) );
	};

	add_plain( GET_VARIABLE( g_variables.m_ladder_bug, bool ), key_lb, label_lb, g_variables.m_ladder_bug_key );
	add_plain( GET_VARIABLE( g_variables.m_ladder_glide, bool ), key_lg, label_lg, g_variables.m_ladder_glide_key );
	add_plain( GET_VARIABLE( g_variables.m_ladder_freelook_climb, bool ), key_lfc, label_lfc, g_variables.m_ladder_freelook_climb_key );
	add_plain( GET_VARIABLE( g_variables.m_fast_ladder, bool ), key_fl, label_fl, g_variables.m_fast_ladder_key );
	add_plain( GET_VARIABLE( g_variables.m_auto_one_hop, bool ), key_oh, label_oh, g_variables.m_auto_one_hop_key );
	add_plain( GET_VARIABLE( g_variables.m_auto_crouch, bool ), key_ac, label_ac, g_variables.m_auto_crouch_key );
	add_plain( GET_VARIABLE( g_variables.m_auto_bounce, bool ), key_ab, label_ab, g_variables.m_auto_bounce_key );

	if ( GET_VARIABLE( g_variables.m_tung_surf, bool ) && key_indicators.size( ) > e_keybind_indicators::key_tung &&
	     key_indicators[ e_keybind_indicators::key_tung ] )
		add_hit( label_tung, trick_states.m_pixel_surf_now, g_input.check_input( &GET_VARIABLE( g_variables.m_tung_surf_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_edge_surf, bool ) && key_indicators.size( ) > e_keybind_indicators::key_esurf &&
	     key_indicators[ e_keybind_indicators::key_esurf ] )
		add_hit( label_esurf, trick_states.m_texture_bug_now || g_edge_surf_active, g_input.check_input( &GET_VARIABLE( g_variables.m_edge_surf_key, key_bind_t ) ) );

	if ( GET_VARIABLE( g_variables.m_edgebug_edge_skip, bool ) && key_indicators.size( ) > e_keybind_indicators::key_es &&
	     key_indicators[ e_keybind_indicators::key_es ] )
		add_hit( label_es, g_edge_skip.active( ), g_input.check_input( &GET_VARIABLE( g_variables.m_edge_skip_key, key_bind_t ) ), true );

	if ( GET_VARIABLE( g_variables.m_hsw, bool ) && key_indicators.size( ) > e_keybind_indicators::key_hsw &&
	     key_indicators[ e_keybind_indicators::key_hsw ] ) {
		add( label_hsw, g_input.check_input( &GET_VARIABLE( g_variables.m_hsw_key, key_bind_t ) ) );
		if ( !( custom_labels && static_cast< std::size_t >( label_hsw ) < labels.size( ) && !labels[ label_hsw ].empty( ) ) )
			pending.back( ).m_text += GET_VARIABLE( g_variables.m_hsw_side, int ) == 0 ? "l" : "r";
	}

	if ( GET_VARIABLE( g_variables.m_zeusbug, bool ) && key_indicators.size( ) > e_keybind_indicators::key_zb &&
	     key_indicators[ e_keybind_indicators::key_zb ] )
		add_hit( label_zb, g_aimbot.zeusbug_busy( ), g_input.check_input( &GET_VARIABLE( g_variables.m_zeusbug_key, key_bind_t ) ) );

	add_plain( GET_VARIABLE( g_variables.m_air_freeze, bool ), key_az, label_az, g_variables.m_air_freeze_key );

	float fs              = 0.f;
	const auto font       = g_render.indicator_font( GET_VARIABLE( g_variables.m_key_indicators_scale, float ), fs );
	const float dt        = ImGui::GetIO( ).DeltaTime;
	const float base      = static_cast< float >( GET_VARIABLE( g_variables.m_key_indicators_position, int ) );
	const bool horizontal = GET_VARIABLE( g_variables.m_key_indicators_horizontal, bool );
	const int style       = GET_VARIABLE( g_variables.m_key_indicators_style, int );

	if ( !font )
		return;

	{
		static const void *s_font = nullptr, *s_atlas = nullptr;
		static float s_fs = 0.f;
		if ( font != s_font || font->ContainerAtlas != s_atlas || fs != s_fs ) {
			s_font  = font;
			s_atlas = font->ContainerAtlas;
			s_fs    = fs;
			botox_dbg_log( "FONT: keys font=%p atlas=%p main=%d base=%.1f fs=%.1f style=%d", s_font, s_atlas, font->ContainerAtlas == ImGui::GetIO( ).Fonts,
			               font->FontSize, fs, style );
		}
	}

	if ( GET_VARIABLE( g_variables.m_key_indicators_particles, bool ) )
		part_track( pending, g_interfaces.m_global_vars_base->m_real_time );

	if ( style != indicator_style_smooth )
		s_smooth_tags.clear( );
	const bool mito = style == indicator_style_mitosis || style == indicator_style_squash;
	if ( !mito )
		mito_reset_keybinds( );
	const bool kami = style == indicator_style_kamidere;
	if ( !kami ) {
		s_kami_key_ghosts.clear( );
		std::fill( std::begin( s_kami_key_was ), std::end( s_kami_key_was ), false );
	}

	if ( style != indicator_style_kamibebra )
		s_kamibebra_tags.clear( );

	if ( style == indicator_style_smooth ) {
		keybinds_smooth( pending, font, fs, dt, base, horizontal, shadow_detect, shadow );
		return;
	}

	if ( style == indicator_style_kamibebra ) {
		keybinds_kamibebra( pending, font, fs, GET_VARIABLE( g_variables.m_key_indicators_scale, float ), dt, base, horizontal, shadow, shadow_detect );
		return;
	}

	if ( mito ) {
		keybinds_mitosis( pending, font, fs, dt, base, horizontal, shadow_detect, shadow, style == indicator_style_squash );
		return;
	}

	const c_color shadow_tint = kami ? GET_VARIABLE( g_variables.m_indicator_kami_shadow_color, c_color ) : shadow.m_color;
	const float real          = g_interfaces.m_global_vars_base->m_real_time;

	if ( kami ) {
		bool listed[ label_max ]{ };
		for ( const auto& p : pending ) {
			listed[ p.m_id ] = true;
			if ( s_kami_key_was[ p.m_id ] && !p.m_active )
				s_kami_key_ghosts.push_back( { p.m_text, s_kami_key_at[ p.m_id ], real } );
			s_kami_key_was[ p.m_id ] = p.m_active;
		}
		for ( int id = 0; id < label_max; ++id )
			if ( !listed[ id ] )
				s_kami_key_was[ id ] = false;
	}

	std::erase_if( s_kami_key_ghosts, [ & ]( const kami_ghost_t& g ) {
		const float age = real - g.m_born;
		if ( age < 0.f || age >= k_kami_ghost_life )
			return true;
		kami_ghost_draw( font, fs, g.m_text, g.m_at, age, 25.f * GET_VARIABLE( g_variables.m_key_indicators_scale, float ) );
		return false;
	} );

	const auto kami_track = [ & ]( const key_pending_t& p, const c_vector_2d& pos ) {
		if ( kami )
			s_kami_key_at[ p.m_id ] = c_vector_2d( pos.m_x + 1.f, pos.m_y + 1.f );
	};

	const bool animate = style != indicator_style_static && !kami;

	const float static_fade = style == indicator_style_static ? GET_VARIABLE( g_variables.m_key_indicators_static_fade, float ) : 0.f;
	if ( static_fade > 0.f ) {
		bool listed[ label_max ]{ };
		for ( const auto& p : pending )
			listed[ p.m_id ] = true;
		for ( int id = 0; id < label_max; ++id )
			if ( !listed[ id ] )
				s_static_key_alpha[ id ] = 0.f;
	}
	else
		std::fill( std::begin( s_static_key_alpha ), std::end( s_static_key_alpha ), 0.f );

	const auto slot_of = [ & ]( const float v ) { return static_fade > 0.f ? 1.f : v; };

	const auto show_of = [ & ]( const key_pending_t& p ) -> float {
		if ( !animate ) {
			if ( static_fade <= 0.f )
				return p.m_active ? 1.f : 0.f;

			float& a         = s_static_key_alpha[ p.m_id ];
			const float step = dt / static_fade;
			a                = p.m_active ? std::min( a + step, 1.f ) : std::max( a - step, 0.f );
			return a;
		}

		ImAnimationHelper anim = ImAnimationHelper( HASH_RT( k_keybind_labels[ p.m_id ][ 1 ] ), dt );
		anim.Update( 3.f, p.m_active ? 3.f : -3.f );
		return anim.AnimationData->second;
	};

	const auto colors_of = [ & ]( const key_pending_t& p ) -> indicator_colors_t {
		if ( !p.m_detect )
			return key_colors_plain( shadow_tint, shadow.m_outline_color );

		if ( !animate )
			return key_colors_at( p.m_success ? 1.f : 0.f, shadow_detect, shadow_tint, p.m_predicted, shadow.m_outline_color );

		constexpr float fade_speed = 10.f;

		ImAnimationHelper fade = ImAnimationHelper( HASH_RT( k_keybind_labels[ p.m_id ][ 1 ] ) + 0x9E3779B9u, dt );
		return key_colors_at( fade.Update( fade_speed, p.m_success ? 1.f : -1.f ), shadow_detect, shadow_tint, p.m_predicted, shadow.m_outline_color );
	};

	if ( horizontal ) {
		struct render_entry_t {
			const key_pending_t* m_p;
			c_color m_color;
			c_color m_shadow;
			c_color m_outline;
			float m_anim;
			float m_slot;
			ImVec2 m_size;
		};

		std::vector< render_entry_t > entries;

		for ( const auto& p : pending ) {
			const indicator_colors_t colors = colors_of( p );

			const float v = show_of( p );
			if ( v <= 0.f )
				continue;

			entries.push_back( { &p, colors.m_text, colors.m_shadow, colors.m_outline, v, slot_of( v ), font->CalcTextSizeA( fs, FLT_MAX, 0.f, p.m_text.c_str( ) ) } );
		}

		const float spacing = fs * 0.2f + static_cast< float >( GET_VARIABLE( g_variables.m_key_indicators_spacing, int ) );

		float total     = 0.f;
		bool first_pass = true;
		for ( const auto& e : entries ) {
			if ( !first_pass )
				total += spacing * e.m_slot;
			total += e.m_size.x * e.m_slot;
			first_pass = false;
		}

		const float y = g_ctx.m_height - base;
		float x       = ( g_ctx.m_width - total ) / 2.f;

		bool first_draw = true;
		for ( const auto& e : entries ) {
			if ( !first_draw )
				x += spacing * e.m_slot;

			const float slot_width  = e.m_size.x * e.m_slot;
			const float slot_center = x + slot_width / 2.f;

			const c_vector_2d pos( slot_center - e.m_size.x / 2.f, y );
			push_label( font, fs, pos, e.m_p->m_text, e.m_color.get_u32( e.m_anim ), e.m_shadow.get_u32( e.m_anim ), shadow, e.m_outline.get_u32( e.m_anim ) );
			kami_track( *e.m_p, pos );
			part_mark( e.m_p->m_id, pos.m_x, pos.m_y, e.m_size );

			x += slot_width;
			first_draw = false;
		}

		return;
	}

	const float gap = static_cast< float >( GET_VARIABLE( g_variables.m_key_indicators_spacing, int ) );
	float offset    = 0.f;
	for ( const auto& p : pending ) {
		const indicator_colors_t colors = colors_of( p );

		const float v = show_of( p );
		if ( v <= 0.f )
			continue;

		const auto text_size = font->CalcTextSizeA( fs, FLT_MAX, 0.f, p.m_text.c_str( ) );

		const c_vector_2d pos( ( g_ctx.m_width - text_size.x ) / 2.f, g_ctx.m_height - offset - base );
		push_label( font, fs, pos, p.m_text, colors.m_text.get_u32( v ), colors.m_shadow.get_u32( v ), shadow, colors.m_outline.get_u32( v ) );
		kami_track( p, pos );
		part_mark( p.m_id, pos.m_x, pos.m_y, text_size );

		offset -= ( text_size.y + gap ) * slot_of( v );
	}
}

struct key_press_cell_t {
	const char* m_text;
	int m_button;
};

static constexpr key_press_cell_t k_key_press_grid[ 2 ][ 2 ][ 3 ] = {
	{ { { "A", in_moveleft }, { "W", in_forward }, { "D", in_moveright } }, { { "C", in_duck }, { "S", in_back }, { "J", in_jump } } },
	{ { { "C", in_duck }, { "W", in_forward }, { "J", in_jump } }, { { "A", in_moveleft }, { "S", in_back }, { "D", in_moveright } } },
};

static float s_key_press_anim[ 3 ][ 3 ]{ };

void n_indicators::impl_t::key_press( )
{
	float fs        = 0.f;
	const auto font = g_render.indicator_font( GET_VARIABLE( g_variables.m_key_press_scale, float ), fs );
	if ( !font || !g_ctx.m_cmd || !g_ctx.m_local->is_alive( ) )
		return;

	const int buttons    = g_ctx.m_input_buttons;
	const short mouse_dx = g_ctx.m_input_mouse_dx;

	const auto& grid       = k_key_press_grid[ std::clamp( GET_VARIABLE( g_variables.m_key_press_layout, int ), 0, 1 ) ];
	const int released     = GET_VARIABLE( g_variables.m_key_press_released, int );
	const bool duck_jump   = GET_VARIABLE( g_variables.m_key_press_duck_jump, bool );
	const ImVec4 on_color  = GET_VARIABLE( g_variables.m_key_press_color, c_color ).get_vec4( );
	const ImVec4 off_color = GET_VARIABLE( g_variables.m_key_press_color_released, c_color ).get_vec4( );
	const shadow_opts_t shadow =
		shadow_opts_of( GET_VARIABLE( g_variables.m_key_press_shadow, bool ), GET_VARIABLE( g_variables.m_key_press_shadow_blur, float ),
	                    c_color( 0.f, 0.f, 0.f, 1.f ), g_variables.m_key_press_outline, g_variables.m_key_press_outline_color,
	                    g_variables.m_key_press_outline_px, g_variables.m_key_press_shadow_x, g_variables.m_key_press_shadow_y );

	const float fade = GET_VARIABLE( g_variables.m_key_press_fade, float );
	const float step = fade > 0.f ? ImGui::GetIO( ).DeltaTime / fade : 1.f;

	const auto width_of = [ & ]( const char* text ) { return font->CalcTextSizeA( fs, FLT_MAX, 0.f, text ).x; };

	/* fixed pitch off the widest glyph: cells never move, whatever is pressed */
	float cell_w = std::max( { width_of( "_" ), width_of( "<" ), width_of( ">" ) } );
	for ( const auto& row : grid )
		for ( const auto& cell : row )
			cell_w = std::max( cell_w, width_of( cell.m_text ) );

	const float pitch_x = cell_w + static_cast< float >( GET_VARIABLE( g_variables.m_key_press_gap_x, int ) );
	const float pitch_y = fs + static_cast< float >( GET_VARIABLE( g_variables.m_key_press_gap_y, int ) );
	const float cx      = g_ctx.m_width / 2.f + static_cast< float >( GET_VARIABLE( g_variables.m_key_press_offset_x, int ) );
	const float top     = g_ctx.m_height - static_cast< float >( GET_VARIABLE( g_variables.m_key_press_position, int ) );

	const auto draw_cell = [ & ]( const int row, const int col, const char* text, const bool down ) {
		float& a = s_key_press_anim[ row ][ col ];
		a        = std::clamp( a + ( down ? step : -step ), 0.f, 1.f );

		const float x = cx + static_cast< float >( col - 1 ) * pitch_x;
		const float y = std::round( top + static_cast< float >( row ) * pitch_y );

		const auto put = [ & ]( const char* t, const ImVec4& c ) {
			if ( c.w <= 0.f )
				return;
			push_label( font, fs, c_vector_2d( std::round( x - width_of( t ) * 0.5f ), y ), t, ImGui::ColorConvertFloat4ToU32( c ),
			            ImGui::ColorConvertFloat4ToU32( ImVec4( 0.f, 0.f, 0.f, c.w ) ), shadow );
		};
		const auto faded = []( ImVec4 c, const float k ) {
			c.w *= k;
			return c;
		};

		if ( released == 0 ) {
			put( "_", faded( off_color, 1.f - a ) );
			put( text, faded( on_color, a ) );
		} else if ( released == 1 )
			put( text, ImVec4( std::lerp( off_color.x, on_color.x, a ), std::lerp( off_color.y, on_color.y, a ), std::lerp( off_color.z, on_color.z, a ),
			                   std::lerp( off_color.w, on_color.w, a ) ) );
		else
			put( text, faded( on_color, a ) );
	};

	for ( int row = 0; row < 2; row++ )
		for ( int col = 0; col < 3; col++ ) {
			const key_press_cell_t& cell = grid[ row ][ col ];
			if ( !duck_jump && ( cell.m_button & ( in_duck | in_jump ) ) ) {
				s_key_press_anim[ row ][ col ] = 0.f;
				continue;
			}
			draw_cell( row, col, cell.m_text, ( buttons & cell.m_button ) != 0 );
		}

	if ( GET_VARIABLE( g_variables.m_key_press_mouse, bool ) ) {
		draw_cell( 2, 0, "<", mouse_dx < 0 );
		draw_cell( 2, 2, ">", mouse_dx > 0 );
	}
}

struct speed_smooth_t {
	smooth_tag_t m_main, m_pre;
	bool m_has_main = false, m_has_pre = false;
	float m_clock = 0.f, m_last_time = 0.f;
};

struct speed_ghost_t {
	std::string m_main, m_pre;
	float m_born;
	float m_x, m_pre_x;
};

struct speed_kami_t {
	bool m_init = false;
	float m_last_time = 0.f;
	float m_x = 0.f, m_pre_x = 0.f;
	float m_main_w = 0.f, m_pre_w = 0.f, m_pre_a = 0.f;
	std::vector< speed_ghost_t > m_ghosts;
};

struct speed_kamibebra_t {
	bool m_init = false;
	float m_last_time = 0.f;
	float m_x = 0.f;
	bool m_has_pre = false, m_pre_removing = false;
	std::string m_pre = { };
	float m_pre_opacity = 0.f, m_pre_x = 0.f, m_pre_target_x = 0.f;
	ImVec4 m_pre_color = ImVec4( 1.f, 1.f, 1.f, 0.f );
};

struct speed_pair_state_t {
	speed_smooth_t m_smooth;

	float m_show = 0.f;
	float m_slot = 0.f;

	speed_kami_t m_kami;
	speed_kamibebra_t m_kamibebra;
};

static speed_pair_state_t s_velocity_pair, s_stamina_pair;

static void kami_touch( speed_kami_t& k )
{
	const float real = g_interfaces.m_global_vars_base->m_real_time;
	if ( real - k.m_last_time > 0.25f || real < k.m_last_time )
		k = { };
	k.m_last_time = real;
}

struct speed_pair_opts_t {
	int m_pre_layout  = pre_layout_right;
	float m_pre_scale = 1.f;
	int m_pre_align   = pre_align_bottom;
	bool m_took_off   = false;
	bool m_kami_glide = false;
	int m_smooth_origin = smooth_origin_auto;
};

static void speed_pair( speed_pair_state_t& st, const int style, const char* anim_key, const std::string& main_text, const std::string& pre_value,
                        const ImColor& color, const float shadow_alpha, const float y, const bool show_pre, const shadow_opts_t& shadow,
                        const float scale, const speed_pair_opts_t& opts = { } )
{
	float fs        = 0.f;
	const auto font = g_render.indicator_font( scale, fs );
	if ( !font )
		return;

	float pfs        = fs;
	ImFont* pfont    = font;
	const float pscl = std::clamp( scale * opts.m_pre_scale, k_indicator_scale_min, k_indicator_scale_max );
	if ( opts.m_pre_scale != 1.f ) {
		float f = 0.f;
		if ( ImFont* const pf = g_render.indicator_font( pscl, f ) ) {
			pfont = pf;
			pfs   = f;
		}
	}

	const float dt = ImGui::GetIO( ).DeltaTime;
	const float W  = g_ctx.m_width;

	if ( style != indicator_style_smooth )
		st.m_smooth = { };
	const bool mito = style == indicator_style_mitosis || style == indicator_style_squash;
	if ( !mito )
		st.m_show = st.m_slot = 0.f;
	if ( style != indicator_style_kamidere )
		st.m_kami = { };
	if ( style != indicator_style_kamibebra )
		st.m_kamibebra = { };

	const auto ink = [ ]( const ImFont* f, const float size ) {
		const ImFontGlyph* const g = f->FindGlyph( '0' );
		const float s              = size / f->FontSize;
		return g ? ImVec2( g->Y0 * s, g->Y1 * s ) : ImVec2( 0.f, size );
	};

	if ( style == indicator_style_kamibebra ) {
		const float s = std::clamp( scale, k_indicator_scale_min, k_indicator_scale_max );

		speed_kamibebra_t& k = st.m_kamibebra;
		const float real     = g_interfaces.m_global_vars_base->m_real_time;
		if ( real - k.m_last_time > 0.25f || real < k.m_last_time )
			k = { };
		k.m_last_time = real;

		if ( opts.m_took_off && show_pre ) {
			if ( !k.m_has_pre ) {
				k.m_pre_opacity = 0.f;
				k.m_pre_color   = ImVec4( 1.f, 1.f, 1.f, 0.f );
			} else
				k.m_pre_opacity = 1.f;
			k.m_has_pre      = true;
			k.m_pre_removing = false;
			k.m_pre          = pre_value;
		}
		if ( !show_pre && k.m_has_pre )
			k.m_pre_removing = true;

		const bool stacked     = opts.m_pre_layout == pre_layout_above || opts.m_pre_layout == pre_layout_below;
		const float spacing    = 5.f * s;
		const ImVec2 main_size = font->CalcTextSizeA( fs, FLT_MAX, 0.f, main_text.c_str( ) );
		const ImVec2 pre_size  = k.m_has_pre ? pfont->CalcTextSizeA( pfs, FLT_MAX, 0.f, k.m_pre.c_str( ) ) : ImVec2( 0.f, 0.f );

		const float target_x = k.m_has_pre && !k.m_pre_removing && !stacked ? ( W - ( main_size.x + std::round( spacing ) + pre_size.x ) ) * 0.5f
		                                                                    : ( W - main_size.x ) * 0.5f;
		if ( !k.m_init ) {
			k.m_x    = target_x;
			k.m_init = true;
		}
		k.m_x = rido_lerp( k.m_x, target_x, 8.5f, dt );

		const ImVec4 main_color = color.Value;
		kamibebra_shadowed( font, fs, s, k.m_x + 0.2f * s, y, main_text, main_color, shadow, shadow_alpha );

		if ( k.m_has_pre ) {
			k.m_pre_opacity = rido_lerp( k.m_pre_opacity, k.m_pre_removing ? 0.f : 1.f, 16.f * ( k.m_pre_removing ? 1.6f : 1.f ), dt );
			if ( stacked )
				k.m_pre_x = k.m_pre_target_x = std::round( ( W - pre_size.x ) * 0.5f );
			else if ( !k.m_pre_removing )
				k.m_pre_x = k.m_pre_target_x = k.m_x + main_size.x + spacing;
			else
				k.m_pre_x = rido_lerp( k.m_pre_x, k.m_pre_target_x, 12.f, dt );

			const ImVec4 target( main_color.x, main_color.y, main_color.z, k.m_pre_opacity * main_color.w );
			k.m_pre_color.x = rido_lerp( k.m_pre_color.x, target.x, 14.f * 0.96f, dt );
			k.m_pre_color.y = rido_lerp( k.m_pre_color.y, target.y, 14.f * 0.96f, dt );
			k.m_pre_color.z = rido_lerp( k.m_pre_color.z, target.z, 14.f * 0.96f, dt );
			k.m_pre_color.w = target.w;

			float pre_y = opts.m_pre_layout == pre_layout_above ? y - pre_size.y : y + main_size.y;
			if ( !stacked ) {
				const ImVec2 mi = ink( font, fs ), pi = ink( pfont, pfs );
				pre_y           = std::round( opts.m_pre_align == pre_align_top ? y + mi.x - pi.x : y + mi.y - pi.y );
			}

			if ( k.m_pre_opacity > 0.01f )
				kamibebra_shadowed( pfont, pfs, pscl, k.m_pre_x + 0.2f * s, pre_y, k.m_pre, k.m_pre_color, shadow, shadow_alpha * k.m_pre_opacity );

			if ( k.m_pre_removing && k.m_pre_opacity < 0.01f )
				k.m_has_pre = k.m_pre_removing = false;
		}
		return;
	}

	const bool stacked        = opts.m_pre_layout == pre_layout_above || opts.m_pre_layout == pre_layout_below;
	const std::string pre_text = stacked ? pre_value : " " + pre_value;
	const ImVec2 main_size     = font->CalcTextSizeA( fs, FLT_MAX, 0.f, main_text.c_str( ) );
	const ImVec2 pre_size      = pfont->CalcTextSizeA( pfs, FLT_MAX, 0.f, pre_text.c_str( ) );

	float pre_y = opts.m_pre_layout == pre_layout_above ? y - pre_size.y : y + main_size.y;
	if ( !stacked ) {
		const ImVec2 mi = ink( font, fs ), pi = ink( pfont, pfs );
		pre_y           = std::round( opts.m_pre_align == pre_align_top ? y + mi.x - pi.x : y + mi.y - pi.y );
	}
	const float pre_centre_x = std::round( ( W - pre_size.x ) * 0.5f );

	const auto push = [ & ]( const bool pre, const c_vector_2d& position, const std::string& text, const ImColor& text_color, const float text_shadow_alpha ) {
		push_label( pre ? pfont : font, pre ? pfs : fs, position, text, text_color, shadow_u32( shadow.m_color, text_shadow_alpha ), shadow );
	};

	if ( style == indicator_style_smooth ) {
		speed_smooth_t& s = st.m_smooth;
		smooth_tag_t& m   = s.m_main;
		smooth_tag_t& p   = s.m_pre;

		/* not drawn a while (dead, toggled off): start clean, never resume a stale half-fade */
		const float real = g_interfaces.m_global_vars_base->m_real_time;
		if ( real - s.m_last_time > 0.25f || real < s.m_last_time )
			s = { };
		s.m_last_time = real;

		s.m_clock += dt;
		const float now = s.m_clock;

		const float x0 = ( W - main_size.x - ( show_pre && !stacked ? pre_size.x : 0.f ) ) * 0.5f;
		m.m_target_x   = x0;
		m.m_target_y   = y;

		/* the number never comes or goes: first frame takes its place outright */
		if ( !s.m_has_main ) {
			m.m_x = x0;
			m.m_y = y;
			m.m_opacity = m.m_target_opacity = 1.f;
			s.m_has_main                     = true;
		}

		const bool from_bottom = opts.m_smooth_origin == smooth_origin_bottom;
		const float drop       = k_smooth_spawn_drop * ( g_ctx.m_height > 0.f ? g_ctx.m_height / 1080.f : 1.f );

		if ( show_pre && ( !s.m_has_pre || p.m_removing ) ) {
			p                  = { };
			p.m_target_opacity = 1.f;
			p.m_appear_until   = now + k_smooth_appear_delay;
			p.m_spawn_x        = smooth_spawn_x( 2 );
			p.m_x              = stacked ? p.m_spawn_x : p.m_spawn_x - ( m.m_x + main_size.x );
			p.m_y              = pre_y;
			if ( from_bottom ) {
				p.m_x = stacked ? pre_centre_x : 0.f;
				p.m_y = p.m_spawn_y = pre_y + drop;
			}
			s.m_has_pre = true;
			botox_dbg_log( "[spd] %s in spawn=%.0f,%.0f", anim_key, p.m_spawn_x, p.m_spawn_y );
		}

		if ( !show_pre && s.m_has_pre && !p.m_removing ) {
			p.m_removing       = true;
			p.m_target_opacity = 0.f;
			if ( !stacked )
				p.m_x = std::round( m.m_x + 0.2f ) + std::round( main_size.x + p.m_x ) - 0.2f;
			p.m_target_x = from_bottom ? p.m_x : p.m_spawn_x;
			if ( from_bottom )
				p.m_target_y = p.m_spawn_y != 0.f ? p.m_spawn_y : p.m_y + drop;
			botox_dbg_log( "[spd] %s out a=%.2f x=%.0f text_a=%.2f shadow_a=%.2f", anim_key, p.m_opacity, p.m_x + 0.2f, color.Value.w, shadow_alpha );
		}

		if ( s.m_has_pre && !p.m_removing ) {
			p.m_target_x = stacked ? pre_centre_x : 0.f;
			p.m_text     = pre_text;
		}
		if ( !p.m_removing || !from_bottom )
			p.m_target_y = pre_y;

		smooth_tag_step( m, false, now, dt );
		if ( s.m_has_pre )
			smooth_tag_step( p, p.m_opacity > 0.01f, now, dt, from_bottom );

		const float main_x = std::round( m.m_x + 0.2f );
		push( false, c_vector_2d( main_x, std::round( m.m_y ) ), main_text, color, shadow_alpha );

		if ( s.m_has_pre ) {
			const float pre_x = p.m_removing || stacked ? std::round( p.m_x + 0.2f ) : main_x + std::round( main_size.x + p.m_x );
			if ( p.m_opacity > 0.01f )
				push( true, c_vector_2d( pre_x, std::round( p.m_y ) ), p.m_text,
				      ImColor( color.Value.x, color.Value.y, color.Value.z, color.Value.w * p.m_opacity ), shadow_alpha * p.m_opacity );

			if ( p.m_removing ) {
				botox_dbg_log( "[spd] %s a=%.3f x=%.0f", anim_key, p.m_opacity, pre_x );
				if ( p.m_opacity < 0.02f ) {
					s.m_has_pre = false;
					botox_dbg_log( "[spd] %s gone", anim_key );
				}
			}
		}

		return;
	}

	if ( mito ) {
		st.m_show = mig::FadeStep( st.m_show, show_pre, dt, mig::SplitSpeed( 4.f, pre_size.x, fs ) );
		st.m_slot = mig::Follow( st.m_slot, mig::EaseOut( st.m_show ), dt, mig::kSlotRate );

		const bool pre_on = !( st.m_show <= 0.001f && st.m_slot <= 0.002f );
		if ( !pre_on )
			st.m_slot = 0.f;

		const auto frame = std::make_shared< mito_frame_t >( );
		mito_frame_t& f  = *frame;
		f.m_font         = font;
		f.m_fs           = fs;
		f.m_axis_x       = !stacked;
		f.m_blur         = shadow.m_blur;
		f.m_squash       = style == indicator_style_squash;
		f.m_off          = ImVec2( shadow.m_off_x, shadow.m_off_y );
		f.m_ring_px      = shadow.m_outline_px;

		const ImU32 col       = color;
		const ImU32 shade_col = shadow.m_on ? shadow_u32( shadow.m_color, shadow_alpha ) : 0u;
		const ImU32 ring_col  = shadow.m_outline ? shadow_u32( shadow.m_outline_color, color.Value.w ) : 0u;

		const bool pre_first = opts.m_pre_layout == pre_layout_above;
		if ( pre_on && pre_first )
			mito_add_ent( f, pre_value.c_str( ), col, shade_col, color.Value.x, color.Value.y, color.Value.z, st.m_show, st.m_slot, pfont, pfs, ring_col );
		mito_add_ent( f, main_text.c_str( ), col, shade_col, color.Value.x, color.Value.y, color.Value.z, 1.f, 1.f, nullptr, 0.f, ring_col );
		if ( pre_on && !pre_first )
			mito_add_ent( f, pre_value.c_str( ), col, shade_col, color.Value.x, color.Value.y, color.Value.z, st.m_show, st.m_slot, pfont, pfs, ring_col );

		if ( !stacked ) {
			mito_layout_row( f, pfont->CalcTextSizeA( pfs, FLT_MAX, 0.f, " " ).x, y );
			if ( pre_on )
				f.m_ents[ 1 ].m_pos.y = std::floor( pre_y );
		} else {
			for ( int i = 0; i < f.m_n; ++i ) {
				mito_ent_t& e   = f.m_ents[ i ];
				const bool pre = pre_on && i == ( pre_first ? 0 : 1 );
				const float ey = !pre ? y : pre_first ? y - e.m_size.y * e.m_anim : y + main_size.y * e.m_anim;
				e.m_pos        = ImVec2( std::floor( ( W - e.m_size.x ) * 0.5f ), std::floor( ey ) );
			}
		}
		mitosis_queue( frame );
		return;
	}

	if ( style == indicator_style_kamidere ) {
		speed_kami_t& k = st.m_kami;
		kami_touch( k );

		const float real   = g_interfaces.m_global_vars_base->m_real_time;
		const float rate   = opts.m_kami_glide ? kami_rate( dt ) : 1.f;
		const c_color tint = GET_VARIABLE( g_variables.m_indicator_kami_shadow_color, c_color );
		const auto tint_at = [ & ]( const float a ) { return static_cast< ImU32 >( shadow_at( tint, a ).get_u32( ) ); };
		const auto ease    = [ & ]( float& v, const float to ) { v += ( to - v ) * rate; };

		const bool first = !k.m_init;
		k.m_init         = true;
		if ( first ) {
			k.m_main_w = main_size.x;
			k.m_pre_w  = pre_size.x;
		}
		ease( k.m_pre_a, show_pre ? 1.f : 0.f );
		ease( k.m_main_w, main_size.x );
		ease( k.m_pre_w, pre_size.x );

		const bool pre_on = k.m_pre_a > 0.01f;
		const float tx    = W * 0.5f - ( k.m_main_w + ( pre_on && !stacked ? k.m_pre_w : 0.f ) ) * 0.5f;
		if ( first )
			k.m_x = tx;
		ease( k.m_x, tx );

		const float tpx = stacked ? W * 0.5f - k.m_pre_w * 0.5f : k.m_x + k.m_main_w;
		if ( first )
			k.m_pre_x = tpx;
		ease( k.m_pre_x, tpx );

		if ( opts.m_took_off )
			k.m_ghosts.push_back( { main_text, pre_on ? pre_text : std::string( ), real, k.m_x, k.m_pre_x } );

		const float rise = 15.f * scale;
		std::erase_if( k.m_ghosts, [ & ]( const speed_ghost_t& g ) { return real - g.m_born < 0.f || real - g.m_born >= k_kami_ghost_life; } );
		for ( speed_ghost_t& g : k.m_ghosts ) {
			const float age = real - g.m_born;
			ease( g.m_x, k.m_x );
			ease( g.m_pre_x, k.m_pre_x );
			kami_ghost_draw( font, fs, g.m_main, c_vector_2d( std::round( g.m_x ) + 1.f, y + 1.f ), age, rise );
			if ( !g.m_pre.empty( ) )
				kami_ghost_draw( pfont, pfs, g.m_pre, c_vector_2d( std::round( g.m_pre_x ) + 1.f, pre_y + 1.f ), age, rise );
		}

		push_label( font, fs, c_vector_2d( std::round( k.m_x ), y ), main_text, color, tint_at( shadow_alpha ), shadow );
		if ( pre_on )
			push_label( pfont, pfs, c_vector_2d( std::round( k.m_pre_x ), pre_y ), pre_text,
			            ImColor( color.Value.x, color.Value.y, color.Value.z, color.Value.w * k.m_pre_a ), tint_at( shadow_alpha * k.m_pre_a ), shadow );
		return;
	}

	float pre_alpha = show_pre ? 1.f : 0.f;
	if ( style != indicator_style_static ) {
		ImAnimationHelper pre_speed_animation = ImAnimationHelper( HASH_RT( anim_key ), dt );
		pre_speed_animation.Update( 3.f, show_pre ? 3.f : -3.f );
		pre_alpha = pre_speed_animation.AnimationData->second;
	}

	const ImColor pre_color = ImColor( color.Value.x, color.Value.y, color.Value.z, color.Value.w * pre_alpha );

	if ( stacked ) {
		push( false, c_vector_2d( ( W - main_size.x ) / 2.f, y ), main_text, color, shadow_alpha );
		if ( pre_alpha > 0.f )
			push( true, c_vector_2d( pre_centre_x, pre_y ), pre_text, pre_color, shadow_alpha * pre_alpha );
		return;
	}

	const float total_x = main_size.x + pre_size.x * pre_alpha;
	const float start_x = ( W - total_x ) / 2.f;

	push( false, c_vector_2d( start_x, y ), main_text, color, shadow_alpha );

	if ( pre_alpha > 0.f )
		push( true, c_vector_2d( start_x + main_size.x, pre_y ), pre_text, pre_color, shadow_alpha * pre_alpha );
}

void n_indicators::impl_t::velocity( const bool on_ground )
{
	const int velocity = static_cast< int >( std::round( g_ctx.m_local->get_velocity( ).length_2d( ) ) );
	if ( on_ground != this->m_indicator_data.m_last_on_ground_velocity )
		botox_dbg_log( "[spd] ground=%d cur=%.3f pre_until=%.3f", on_ground, g_interfaces.m_global_vars_base->m_current_time,
		               this->m_indicator_data.m_take_off_time_velocity );
	if ( this->m_indicator_data.m_last_on_ground_velocity && !on_ground ) {
		this->m_indicator_data.m_take_off_velocity      = velocity;
		this->m_indicator_data.m_take_off_time_velocity = g_interfaces.m_global_vars_base->m_current_time + 2.f;
	}

	const int style      = GET_VARIABLE( g_variables.m_velocity_indicator_style, int );
	const bool kamibebra = style == indicator_style_kamibebra;
	const float speed    = g_ctx.m_local->get_velocity( ).length_2d( );

	float alpha = std::min( speed / 250.f, 1.f );
	if ( kamibebra ) {
		alpha = 1.f;
		if ( GET_VARIABLE( g_variables.m_velocity_indicator_fade_alpha, bool ) ) {
			if ( speed <= 0.f )
				alpha = 0.f;
			else if ( speed < 30.f )
				alpha = std::lerp( 0.f, 0.12f, speed / 30.f );
			else if ( speed < 150.f )
				alpha = std::lerp( 0.12f, 1.f, std::pow( ( speed - 30.f ) / 120.f, 2.8f ) );
		}
	}

	const ImColor blended_color = ImColor::Blend( GET_VARIABLE( g_variables.m_velocity_indicator_color1, c_color ).get_u32( ),
	                                              GET_VARIABLE( g_variables.m_velocity_indicator_color2, c_color ).get_u32( ), alpha );

	const ImColor speed_based_color = velocity == this->m_indicator_data.m_last_velocity
	                                      ? GET_VARIABLE( g_variables.m_velocity_indicator_color3, c_color )
	                                            .get_u32( GET_VARIABLE( g_variables.m_velocity_indicator_fade_alpha, bool ) ? alpha : 1.f )
	                                  : velocity < this->m_indicator_data.m_last_velocity
	                                      ? GET_VARIABLE( g_variables.m_velocity_indicator_color4, c_color )
	                                            .get_u32( GET_VARIABLE( g_variables.m_velocity_indicator_fade_alpha, bool ) ? alpha : 1.f )
	                                      : GET_VARIABLE( g_variables.m_velocity_indicator_color5, c_color )
	                                            .get_u32( GET_VARIABLE( g_variables.m_velocity_indicator_fade_alpha, bool ) ? alpha : 1.f );

	const bool should_draw_take_off =
		( !on_ground || ( this->m_indicator_data.m_take_off_time_velocity > g_interfaces.m_global_vars_base->m_current_time ) ) &&
		( GET_VARIABLE( g_variables.m_velocity_indicator_show_pre_speed, bool ) );

	ImColor text_color =
		GET_VARIABLE( g_variables.m_velocity_indicator_custom_color, bool )
			? ImColor( blended_color.Value.x, blended_color.Value.y, blended_color.Value.z,
	                   blended_color.Value.w * ( GET_VARIABLE( g_variables.m_velocity_indicator_fade_alpha, bool ) ? alpha : 1.f ) )
			: ImColor( speed_based_color.Value.x, speed_based_color.Value.y, speed_based_color.Value.z, speed_based_color.Value.w );
	if ( kamibebra )
		text_color.Value.w = alpha;

	const float shadow_alpha = GET_VARIABLE( g_variables.m_velocity_indicator_fade_alpha, bool ) ? alpha : 1.f;

	const float y = g_ctx.m_height - static_cast< float >( GET_VARIABLE( g_variables.m_velocity_indicator_padding, int ) );

	const bool took_off = this->m_indicator_data.m_last_on_ground_velocity && !on_ground;

	speed_pair( s_velocity_pair, style, "velocity_pre_speed", std::format( "{:d}", velocity ),
	            std::format( "({:d})", kamibebra ? std::min( this->m_indicator_data.m_take_off_velocity, 287 ) : this->m_indicator_data.m_take_off_velocity ),
	            text_color, shadow_alpha, y,
	            kamibebra ? GET_VARIABLE( g_variables.m_velocity_indicator_show_pre_speed, bool ) && !on_ground : should_draw_take_off,
	            shadow_opts_of( GET_VARIABLE( g_variables.m_velocity_indicator_shadow, bool ), GET_VARIABLE( g_variables.m_velocity_indicator_shadow_blur, float ),
	                            GET_VARIABLE( g_variables.m_velocity_indicator_shadow_color, c_color ), g_variables.m_velocity_indicator_outline,
	                            g_variables.m_velocity_indicator_outline_color, g_variables.m_velocity_indicator_outline_px,
	                            g_variables.m_velocity_indicator_shadow_x, g_variables.m_velocity_indicator_shadow_y ),
	            GET_VARIABLE( g_variables.m_velocity_indicator_scale, float ),
	            { GET_VARIABLE( g_variables.m_velocity_indicator_pre_layout, int ), GET_VARIABLE( g_variables.m_velocity_indicator_pre_scale, float ),
	              GET_VARIABLE( g_variables.m_velocity_indicator_pre_align, int ), took_off, false,
	              GET_VARIABLE( g_variables.m_velocity_indicator_smooth_origin, int ) } );

	if ( this->m_indicator_data.m_tick_prev_velocity + 5 < g_interfaces.m_global_vars_base->m_tick_count ) {
		this->m_indicator_data.m_last_velocity      = velocity;
		this->m_indicator_data.m_tick_prev_velocity = g_interfaces.m_global_vars_base->m_tick_count;
	}

	this->m_indicator_data.m_last_on_ground_velocity = on_ground;
}

void n_indicators::impl_t::stamina( const bool on_ground )
{
	const float stamina = g_ctx.m_local->get_stamina( );
	if ( this->m_indicator_data.m_last_on_ground_stamina && !on_ground ) {
		this->m_indicator_data.m_take_off_stamina      = stamina;
		this->m_indicator_data.m_take_off_time_stamina = g_interfaces.m_global_vars_base->m_current_time + 2.f;
	}

	const float alpha = std::min( stamina / 40.f, 1.f );

	const ImColor blended_color = ImColor::Blend( GET_VARIABLE( g_variables.m_stamina_indicator_color1, c_color ).get_u32( ),
	                                              GET_VARIABLE( g_variables.m_stamina_indicator_color2, c_color ).get_u32( ), alpha );

	const bool should_draw_take_off =
		( !on_ground || ( this->m_indicator_data.m_take_off_time_stamina > g_interfaces.m_global_vars_base->m_current_time ) ) &&
		( GET_VARIABLE( g_variables.m_stamina_indicator_show_pre_speed, bool ) );

	const ImColor text_color = ImColor( blended_color.Value.x, blended_color.Value.y, blended_color.Value.z,
	                                    blended_color.Value.w * ( GET_VARIABLE( g_variables.m_stamina_indicator_fade_alpha, bool ) ? alpha : 1.f ) );

	const float shadow_alpha = GET_VARIABLE( g_variables.m_stamina_indicator_fade_alpha, bool ) ? alpha : 1.f;

	const float y = g_ctx.m_height - static_cast< float >( GET_VARIABLE( g_variables.m_stamina_indicator_padding, int ) );

	const int style = GET_VARIABLE( g_variables.m_stamina_indicator_style, int );

	speed_pair( s_stamina_pair, style, "stamina_pre_speed", std::format( "{:.1f}", stamina ),
	            std::format( "({:.1f})", this->m_indicator_data.m_take_off_stamina ), text_color, shadow_alpha, y,
	            style == indicator_style_kamibebra ? GET_VARIABLE( g_variables.m_stamina_indicator_show_pre_speed, bool ) && !on_ground : should_draw_take_off,
	            shadow_opts_of( GET_VARIABLE( g_variables.m_stamina_indicator_shadow, bool ), GET_VARIABLE( g_variables.m_stamina_indicator_shadow_blur, float ),
	                            GET_VARIABLE( g_variables.m_stamina_indicator_shadow_color, c_color ), g_variables.m_stamina_indicator_outline,
	                            g_variables.m_stamina_indicator_outline_color, g_variables.m_stamina_indicator_outline_px,
	                            g_variables.m_stamina_indicator_shadow_x, g_variables.m_stamina_indicator_shadow_y ),
	            GET_VARIABLE( g_variables.m_stamina_indicator_scale, float ),
	            { GET_VARIABLE( g_variables.m_stamina_indicator_pre_layout, int ), GET_VARIABLE( g_variables.m_stamina_indicator_pre_scale, float ),
	              GET_VARIABLE( g_variables.m_stamina_indicator_pre_align, int ), this->m_indicator_data.m_last_on_ground_stamina && !on_ground, false,
	              GET_VARIABLE( g_variables.m_stamina_indicator_smooth_origin, int ) } );

	if ( this->m_indicator_data.m_tick_prev_stamina + 5 < g_interfaces.m_global_vars_base->m_tick_count ) {
		this->m_indicator_data.m_last_stamina      = stamina;
		this->m_indicator_data.m_tick_prev_stamina = g_interfaces.m_global_vars_base->m_tick_count;
	}

	this->m_indicator_data.m_last_on_ground_stamina = on_ground;
}

struct graph_sample_t {
	float m_speed;
	bool m_take_off;
};

static std::deque< graph_sample_t > s_graph;
static int s_graph_tick     = 0;
static bool s_graph_ground  = true;
static float s_graph_top    = 300.f;

void n_indicators::impl_t::velocity_graph( const bool on_ground )
{
	if ( !GET_VARIABLE( g_variables.m_velocity_graph, bool ) || !g_ctx.m_local->is_alive( ) ) {
		s_graph.clear( );
		return;
	}

	const int length = std::clamp( GET_VARIABLE( g_variables.m_velocity_graph_length, int ), 2, 2048 );
	const int tick   = g_interfaces.m_global_vars_base->m_tick_count;
	const float now  = g_ctx.m_local->get_velocity( ).length_2d( );

	if ( tick < s_graph_tick || tick - s_graph_tick > length )
		s_graph.clear( );

	const int add = s_graph.empty( ) ? 1 : tick - s_graph_tick;
	for ( int i = 0; i < add; ++i )
		s_graph.push_back( { now, i == add - 1 && s_graph_ground && !on_ground } );
	if ( add > 0 ) {
		s_graph_tick   = tick;
		s_graph_ground = on_ground;
	}
	while ( static_cast< int >( s_graph.size( ) ) > length )
		s_graph.pop_front( );

	const float width  = static_cast< float >( std::max( GET_VARIABLE( g_variables.m_velocity_graph_width, int ), 10 ) );
	const float height = static_cast< float >( std::max( GET_VARIABLE( g_variables.m_velocity_graph_height, int ), 5 ) );
	const float left   = std::round( g_ctx.m_width * 0.5f + GET_VARIABLE( g_variables.m_velocity_graph_offset_x, int ) - width * 0.5f );
	const float base_y = std::round( g_ctx.m_height - static_cast< float >( GET_VARIABLE( g_variables.m_velocity_graph_position, int ) ) );
	const float step   = width / static_cast< float >( length - 1 );

	float top = GET_VARIABLE( g_variables.m_velocity_graph_max, float );
	if ( top <= 0.f ) {
		float peak = 300.f;
		for ( const graph_sample_t& g : s_graph )
			peak = std::max( peak, g.m_speed );
		s_graph_top += ( peak - s_graph_top ) * ( 1.f - std::exp( -6.f * ImGui::GetIO( ).DeltaTime ) );
		top = s_graph_top;
	}

	const bool fade_sides = GET_VARIABLE( g_variables.m_velocity_graph_fade_sides, bool );
	const float fade      = GET_VARIABLE( g_variables.m_velocity_graph_fade, float );
	const auto edge_alpha = [ & ]( const float x ) {
		if ( !fade_sides || fade <= 0.f )
			return 1.f;
		const float t = ( x - left ) / width;
		return std::clamp( std::min( t, 1.f - t ) / fade, 0.f, 1.f );
	};

	struct graph_pt_t {
		ImVec2 m_at;
		float m_a;
		int m_speed;
	};
	std::vector< graph_pt_t > pts;
	pts.reserve( s_graph.size( ) );
	const int count = static_cast< int >( s_graph.size( ) );
	for ( int i = 0; i < count; ++i ) {
		const float x = left + width - static_cast< float >( count - 1 - i ) * step;
		const float y = base_y - std::min( s_graph[ i ].m_speed / top, 1.f ) * height;
		pts.push_back( { ImVec2( x, y ), edge_alpha( x ), static_cast< int >( std::round( s_graph[ i ].m_speed ) ) } );
	}

	const ImVec4 line      = GET_VARIABLE( g_variables.m_velocity_graph_color, c_color ).get_vec4( );
	const ImVec4 gain      = GET_VARIABLE( g_variables.m_velocity_graph_color_gain, c_color ).get_vec4( );
	const ImVec4 loss      = GET_VARIABLE( g_variables.m_velocity_graph_color_loss, c_color ).get_vec4( );
	const ImVec4 fill      = GET_VARIABLE( g_variables.m_velocity_graph_fill_color, c_color ).get_vec4( );
	const bool gain_colors = GET_VARIABLE( g_variables.m_velocity_graph_gain_colors, bool );
	const bool fill_on     = GET_VARIABLE( g_variables.m_velocity_graph_fill, bool );
	const float thickness  = std::max( GET_VARIABLE( g_variables.m_velocity_graph_thickness, float ), 0.5f );

	g_render.m_draw_data.emplace_back(
		e_draw_type::draw_type_callback,
		std::make_any< callback_draw_object_t >( callback_draw_object_t{ [ = ]( ImDrawList* dl ) {
			const auto with_a = []( ImVec4 c, const float a ) {
				c.w *= a;
				return ImGui::ColorConvertFloat4ToU32( c );
			};

			if ( fill_on ) {
				const ImVec2 uv = ImGui::GetFontTexUvWhitePixel( );
				for ( std::size_t i = 1; i < pts.size( ); ++i ) {
					const graph_pt_t& a = pts[ i - 1 ];
					const graph_pt_t& b = pts[ i ];
					/* reserve first: a 16-bit index overflow moves to a new vtx offset and resets _VtxCurrentIdx */
					dl->PrimReserve( 6, 4 );
					const ImDrawIdx idx = static_cast< ImDrawIdx >( dl->_VtxCurrentIdx );
					dl->PrimWriteIdx( idx );
					dl->PrimWriteIdx( static_cast< ImDrawIdx >( idx + 1 ) );
					dl->PrimWriteIdx( static_cast< ImDrawIdx >( idx + 2 ) );
					dl->PrimWriteIdx( idx );
					dl->PrimWriteIdx( static_cast< ImDrawIdx >( idx + 2 ) );
					dl->PrimWriteIdx( static_cast< ImDrawIdx >( idx + 3 ) );
					dl->PrimWriteVtx( a.m_at, uv, with_a( fill, a.m_a ) );
					dl->PrimWriteVtx( b.m_at, uv, with_a( fill, b.m_a ) );
					dl->PrimWriteVtx( ImVec2( b.m_at.x, base_y ), uv, with_a( fill, 0.f ) );
					dl->PrimWriteVtx( ImVec2( a.m_at.x, base_y ), uv, with_a( fill, 0.f ) );
				}
			}

			for ( std::size_t i = 1; i < pts.size( ); ++i ) {
				const graph_pt_t& a = pts[ i - 1 ];
				const graph_pt_t& b = pts[ i ];
				const ImVec4& c     = !gain_colors || b.m_speed == a.m_speed ? line : b.m_speed > a.m_speed ? gain : loss;
				dl->AddLine( a.m_at, b.m_at, with_a( c, ( a.m_a + b.m_a ) * 0.5f ), thickness );
			}
		} } ) );

	if ( !GET_VARIABLE( g_variables.m_velocity_graph_jump_speeds, bool ) )
		return;

	float fs        = 0.f;
	const auto font = g_render.indicator_font( GET_VARIABLE( g_variables.m_velocity_graph_text_scale, float ), fs );
	if ( !font )
		return;

	const ImVec4 text          = GET_VARIABLE( g_variables.m_velocity_graph_text_color, c_color ).get_vec4( );
	const shadow_opts_t shadow = { GET_VARIABLE( g_variables.m_velocity_graph_text_shadow, bool ), 0.f };
	for ( int i = 0; i < count; ++i ) {
		if ( !s_graph[ i ].m_take_off || pts[ i ].m_a <= 0.f )
			continue;
		const std::string label = std::format( "{:d}", pts[ i ].m_speed );
		const ImVec2 size       = font->CalcTextSizeA( fs, FLT_MAX, 0.f, label.c_str( ) );
		ImVec4 c                = text;
		c.w *= pts[ i ].m_a;
		push_label( font, fs, c_vector_2d( std::round( pts[ i ].m_at.x - size.x * 0.5f ), std::round( pts[ i ].m_at.y - size.y - 2.f ) ), label,
		            ImGui::ColorConvertFloat4ToU32( c ), ImGui::ColorConvertFloat4ToU32( ImVec4( 0.f, 0.f, 0.f, c.w ) ), shadow );
	}
}

namespace
{
	enum e_net_graph_item {
		net_graph_fps = 0,
		net_graph_frame_var,
		net_graph_ping,
		net_graph_up,
		net_graph_loss,
		net_graph_choke,
		net_graph_cmd,
		net_graph_tick,
		net_graph_sv,
		net_graph_sv_var,
		net_graph_server
	};

	/* CCSGameRules*, null while loading (same site as cod4_hud game_rules) */
	std::uintptr_t net_graph_game_rules( )
	{
		static const auto site = g_modules[ CLIENT_DLL ].find_pattern( "A1 ? ? ? ? 85 C0 0F 84 ? ? ? ? 80 B8 ? ? ? ? ? 74 7A" );
		return site ? reinterpret_cast< std::uintptr_t >( **reinterpret_cast< void*** >( site + 1 ) ) : 0;
	}

	/* CNetGraphPanel::DrawServerType */
	const char* net_graph_server_type( c_net_channel_info* nci )
	{
		const auto engine = g_interfaces.m_engine_client;

		if ( engine->is_playing_demo( ) )
			return "demo";
		if ( nci && nci->is_loopback( ) )
			return "local";
		if ( engine->is_in_game( ) ) {
			const char* const address = nci ? nci->get_address( ) : nullptr;
			const char* const port    = address ? std::strrchr( address, ':' ) : nullptr;
			const bool p2p            = port && std::strcmp( port, ":1" ) == 0;

			static const unsigned int valve_ds_ofs = g_netvars[ HASH_BT( "CCSGameRulesProxy->m_bIsValveDS" ) ].m_offset;
			const std::uintptr_t rules             = valve_ds_ofs ? net_graph_game_rules( ) : 0;
			const bool valve_ds                    = rules && *reinterpret_cast< bool* >( rules + valve_ds_ofs );

			if ( engine->is_hltv( ) )
				return valve_ds ? "Official GOTV" : p2p ? "P2P GOTV" : "GOTV";
			return valve_ds ? "Official DS" : p2p ? "P2P" : "online";
		}
		if ( engine->is_connected( ) )
			return "loading";
		return "offline";
	}
}

void n_indicators::impl_t::net_graph( const bool restore )
{
	static c_cconvar *net_graph_cvar = nullptr, *net_graphtext = nullptr, *net_graphpos = nullptr, *net_graphheight = nullptr;
	static c_cconvar *cl_updaterate = nullptr, *cl_cmdrate = nullptr;
	static int stored_text = -1;

	if ( !net_graphtext && !( net_graphtext = g_convars[ HASH_BT( "net_graphtext" ) ] ) )
		return;

	const bool on = !restore && GET_VARIABLE( g_variables.m_net_graph, bool );
	if ( !on ) {
		if ( stored_text != -1 ) {
			net_graphtext->set_value( stored_text );
			stored_text = -1;
		}
		return;
	}

	/* engine text off, ours drawn in its place; graph lines (net_graph 3) stay the engine's */
	if ( stored_text == -1 )
		stored_text = net_graphtext->get_int( );
	if ( net_graphtext->get_int( ) != 0 )
		net_graphtext->set_value( 0 );

	if ( !net_graph_cvar ) {
		net_graph_cvar  = g_convars[ HASH_BT( "net_graph" ) ];
		net_graphpos    = g_convars[ HASH_BT( "net_graphpos" ) ];
		net_graphheight = g_convars[ HASH_BT( "net_graphheight" ) ];
		cl_updaterate   = g_convars[ HASH_BT( "cl_updaterate" ) ];
		cl_cmdrate      = g_convars[ HASH_BT( "cl_cmdrate" ) ];
	}
	if ( !net_graph_cvar || !cl_updaterate || !cl_cmdrate )
		return;

	const auto globals = g_interfaces.m_global_vars_base;

	static float framerate = 0.f;
	framerate             = 0.9f * framerate + 0.1f * globals->m_abs_frame_time;
	if ( framerate <= 0.f )
		framerate = 1.f;

	if ( net_graph_cvar->get_int( ) < 1 )
		return;

	ImFont* font = g_render.m_custom_fonts[ e_custom_font_names::custom_font_name_net_graph ];
	if ( !font )
		font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	if ( !font )
		return;
	const float fs = font->FontSize;

	const auto engine = g_interfaces.m_engine_client;
	const auto nci    = engine->is_connected( ) ? engine->get_net_channel_info( ) : nullptr;

	float latency = 0.f, loss = 0.f, choke = 0.f, in_data = 0.f, out_data = 0.f, in_packets = 0.f, out_packets = 0.f;
	float sv_time = 0.f, sv_dev = 0.f, sv_start_dev = 0.f;
	int in_bytes = 0;
	static int out_bytes = 0;

	const float update_rate = cl_updaterate->get_float( );
	const float cmd_rate    = cl_cmdrate->get_float( );

	/* CNetGraphPanel::GetFrameData */
	if ( nci ) {
		loss        = nci->get_avg_loss( FLOW_INCOMING );
		choke       = nci->get_avg_choke( FLOW_INCOMING );
		latency     = nci->get_avg_latency( FLOW_OUTGOING );
		in_data     = nci->get_avg_data( FLOW_INCOMING ) / 1024.f;
		out_data    = nci->get_avg_data( FLOW_OUTGOING ) / 1024.f;
		in_packets  = nci->get_avg_packets( FLOW_INCOMING );
		out_packets = nci->get_avg_packets( FLOW_OUTGOING );
		in_bytes    = nci->get_packet_bytes( FLOW_INCOMING, nci->get_sequence_nr( FLOW_INCOMING ), c_net_channel_info::total );
		if ( const int out = nci->get_packet_bytes( FLOW_OUTGOING, nci->get_sequence_nr( FLOW_OUTGOING ) - 1, c_net_channel_info::total ) )
			out_bytes = out;
		nci->get_remote_framerate( &sv_time, &sv_dev, &sv_start_dev );

		if ( update_rate > 0.001f )
			latency -= 0.5f / update_rate;
		latency = std::max( latency, 0.f );
	}
	if ( engine->is_playing_demo( ) )
		latency = 0.f;

	const float tick_interval = globals->m_interval_per_tick;
	const float tick_rate     = tick_interval > 0.f ? std::max( 1.f / tick_interval, 0.000001f ) : 0.000001f;
	const float interp        = g_lagcomp.lerp_time( );

	const bool warn      = GET_VARIABLE( g_variables.m_net_graph_warn_colors, bool );
	const c_color normal = GET_VARIABLE( g_variables.m_net_graph_color, c_color );
	const c_color warn1  = GET_VARIABLE( g_variables.m_net_graph_warn1, c_color );
	const c_color warn2  = GET_VARIABLE( g_variables.m_net_graph_warn2, c_color );
	const c_color warn3  = GET_VARIABLE( g_variables.m_net_graph_warn3, c_color );

	/* CNetGraphPanel::GetColorFromVariance */
	const auto variance = [ & ]( const float value, const float base, const float v1, const float v2, const float v3 ) -> c_color {
		if ( !warn || base == 0.f )
			return normal;
		const float delta = std::fabs( base - value ) / base;
		return delta > v3 ? warn3 : delta > v2 ? warn2 : delta > v1 ? warn1 : normal;
	};
	const auto rate_color = [ & ]( const float rate ) -> c_color {
		if ( rate < tick_rate )
			return variance( rate, tick_rate, 0.f, 0.2f, 0.5f );
		return rate > tick_rate && warn ? warn3 : normal;
	};

	c_color interp_color = normal;
	if ( warn && interp > 0.001f ) {
		if ( update_rate > 0.f && interp < 2.f / update_rate )
			interp_color = variance( interp, 2.f / update_rate, 0.f, 0.2f, 0.5f );
		if ( interp < tick_interval )
			interp_color = warn3;
	}
	const c_color server_color = sv_time > 1.f / tick_rate + 0.0001f ? variance( sv_time, 1.f / tick_rate, 0.25f, 0.5f, 0.75f ) : normal;

	const std::vector< bool >& items = GET_VARIABLE( g_variables.m_net_graph_items, std::vector< bool > );
	const auto shown                 = [ & ]( const int item ) { return item < static_cast< int >( items.size( ) ) && items[ item ]; };
	const bool in_out                = GET_VARIABLE( g_variables.m_net_graph_in_out, bool );
	const bool lerp                  = GET_VARIABLE( g_variables.m_net_graph_lerp, bool );

	struct piece_t {
		const char* m_gap;
		std::string m_text;
		c_color m_color;
	};
	struct row_t {
		std::vector< piece_t > m_left;
		piece_t m_right{ "", "", { } };
	};
	std::vector< row_t > rows;

	char sz[ 128 ];
	const auto fmt = [ & ]( const char* format, auto... args ) {
		std::snprintf( sz, sizeof( sz ), format, args... );
		return std::string( sz );
	};

	/* CNetGraphPanel::DrawTextFields, row by row */
	{
		row_t& r = rows.emplace_back( );
		if ( shown( net_graph_fps ) )
			r.m_left.push_back( { "", fmt( "fps: %5i", static_cast< int >( 1.f / framerate ) ), normal } );
		if ( shown( net_graph_frame_var ) )
			r.m_left.push_back( { "  ", fmt( "var: %4.1f ms", globals->m_abs_frame_start_time * 1000.f ), normal } );
		if ( shown( net_graph_ping ) )
			r.m_left.push_back( { "  ", fmt( "ping: %i ms", static_cast< int >( latency * 1000.f ) ), normal } );
		if ( lerp && !in_out )
			r.m_left.push_back( { "  ", fmt( "lerp: %4.1fms", interp * 1000.f ), interp_color } );
		if ( shown( net_graph_up ) )
			r.m_right = { "", fmt( "up:%5.1f/s", update_rate ), rate_color( update_rate ) };
	}
	if ( in_out ) {
		row_t& in = rows.emplace_back( );
		in.m_left.push_back( { "", fmt( "in:  %5i  %5.2fk/s", in_bytes, in_data ), normal } );
		if ( lerp )
			in.m_left.push_back( { "  ", fmt( "lerp: %4.1fms", interp * 1000.f ), interp_color } );
		in.m_right = { "", fmt( "%3.1f/s", in_packets ), normal };

		row_t& out = rows.emplace_back( );
		out.m_left.push_back( { "", fmt( "out: %5i  %5.2fk/s", out_bytes, out_data ), normal } );
		out.m_right = { "", fmt( "%3.1f/s", out_packets ), normal };
	}
	{
		row_t& r = rows.emplace_back( );
		if ( shown( net_graph_loss ) )
			r.m_left.push_back( { "", fmt( "loss: %3i%%", static_cast< int >( loss * 100.f ) ), normal } );
		if ( shown( net_graph_choke ) )
			r.m_left.push_back( { "  ", fmt( "choke: %2i%%", static_cast< int >( choke * 100.f ) ), normal } );
		if ( shown( net_graph_cmd ) )
			r.m_right = { "", fmt( "cmd:%5.1f/s", cmd_rate ), rate_color( cmd_rate ) };
	}
	{
		row_t& r = rows.emplace_back( );
		if ( shown( net_graph_tick ) )
			r.m_left.push_back( { "", fmt( "tick:%5.1f", tick_rate ), normal } );
		if ( shown( net_graph_sv ) )
			r.m_left.push_back( { "  ", fmt( "sv:%5.1f +-%4.1f ms", sv_time * 1000.f, sv_dev * 1000.f ), server_color } );
		if ( shown( net_graph_sv_var ) )
			r.m_left.push_back( { "   ", fmt( "var: %6.3f ms", sv_start_dev * 1000.f ), server_color } );
		if ( shown( net_graph_server ) )
			r.m_right = { "", net_graph_server_type( nci ), normal };
	}

	std::erase_if( rows, []( const row_t& r ) { return r.m_left.empty( ) && r.m_right.m_text.empty( ); } );
	if ( rows.empty( ) )
		return;

	const auto text_w = [ & ]( const std::string& text ) { return font->CalcTextSizeA( fs, FLT_MAX, 0.f, text.c_str( ) ).x; };

	/* CNetGraphPanel::OnFontChanged width sample */
	float w = text_w( "fps:  435  var: 4.091 ms  ping: 533 ms lerp 112.3 ms   0/0 offline" );
	for ( const row_t& r : rows ) {
		std::string line;
		for ( const piece_t& p : r.m_left )
			line += ( line.empty( ) ? "" : p.m_gap ) + p.m_text;
		w = std::max( w, text_w( line ) + ( r.m_right.m_text.empty( ) ? 0.f : text_w( "  " + r.m_right.m_text ) ) );
	}
	const int sw = static_cast< int >( g_ctx.m_width ), sh = static_cast< int >( g_ctx.m_height );
	w            = std::min( w, static_cast< float >( sw - 10 ) );

	/* CNetGraphPanel::GraphGetXY + ComputeNetgraphHeight, used when x / y = -1 */
	int x = GET_VARIABLE( g_variables.m_net_graph_x, int );
	int y = GET_VARIABLE( g_variables.m_net_graph_y, int );
	if ( x < 0 ) {
		const int pos = net_graphpos ? net_graphpos->get_int( ) : 1;
		const int wi  = static_cast< int >( w );
		x             = pos == 0   ? 5
		                : pos == 1 ? sw - 5 - wi
		                : pos == 2 ? ( sw - 10 - wi ) / 2
		                           : std::clamp( pos * sw / 640, 5, std::max( 5, sw - wi - 5 ) );
	}
	if ( y < 0 ) {
		const int lines  = in_out ? 6 : 4;
		const int height = std::max( lines * static_cast< int >( fs ), net_graphheight ? net_graphheight->get_int( ) : 64 );
		y                = sh - 24 - 5 - height;
	}

	if ( GET_VARIABLE( g_variables.m_net_graph_background, bool ) )
		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_rect,
			std::make_any< rect_draw_object_t >( c_vector_2d( x - 4.f, y - 2.f ), c_vector_2d( x + w + 4.f, y + fs * rows.size( ) + 2.f ),
		                                         GET_VARIABLE( g_variables.m_net_graph_background_color, c_color ).get_u32( ), ImColor( 0.f, 0.f, 0.f, 0.f ),
		                                         true, 0.f, ImDrawFlags_::ImDrawFlags_None, 1.f, e_rect_flags::rect_flag_none ) );

	const int style            = GET_VARIABLE( g_variables.m_net_graph_text_style, int );
	const e_text_flags flags   = style == 1 ? e_text_flags::text_flag_dropshadow : style == 2 ? e_text_flags::text_flag_outline : e_text_flags::text_flag_none;
	const unsigned int outline = GET_VARIABLE( g_variables.m_net_graph_outline_color, c_color ).get_u32( );
	const auto put             = [ & ]( const float px, const float py, const piece_t& p ) {
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text, std::make_any< text_draw_object_t >( font, c_vector_2d( std::round( px ), std::round( py ) ),
		                                                                                                    p.m_text, p.m_color.get_u32( ), outline, flags, fs ) );
	};

	float row_y = static_cast< float >( y );
	for ( const row_t& r : rows ) {
		float px = static_cast< float >( x );
		for ( std::size_t i = 0; i < r.m_left.size( ); ++i ) {
			if ( i )
				px += text_w( r.m_left[ i ].m_gap );
			put( px, row_y, r.m_left[ i ] );
			px += text_w( r.m_left[ i ].m_text );
		}
		if ( !r.m_right.m_text.empty( ) )
			put( x + w - text_w( r.m_right.m_text ) - 1.f, row_y, r.m_right );
		row_y += fs;
	}
}
