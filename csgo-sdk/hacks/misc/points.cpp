#include "misc.h"
#include "../menu/menu.h"
#include "../../globals/includes/includes.h"
#include "../../game/sdk/classes/c_global_vars_base.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <utility>

namespace
{
	/* written in create_move, read + zeroed in end_scene ( render thread ) */
	std::atomic< int > points_total{ 0 };
	std::atomic< unsigned > points_events{ 0 };

	std::pair< ImFont*, float > points_font( )
	{
		ImFont* font = nullptr;
		float size   = 0.f;

		if ( GET_VARIABLE( g_variables.m_points_big_font, bool ) ) {
			font = g_render.m_fonts[ e_font_names::font_name_points_montserrat_italic_18 ];
			size = 21.f;
		}
		else if ( GET_VARIABLE( g_variables.m_watermark, bool ) ) {
			switch ( GET_VARIABLE( g_variables.m_watermark_style, int ) ) {
			case 1: font = g_render.m_fonts[ e_font_names::font_name_kamibebra_bold_13 ]; break;
			case 3: font = g_render.m_fonts[ e_font_names::font_name_clarity_inter_semibold_14 ]; break;
			case 4:
			case 7: font = g_render.m_fonts[ e_font_names::font_name_tahoma_12 ]; break;
			case 5: font = g_render.m_fonts[ e_font_names::font_name_interwebz_calibri ]; break;
			case 6: font = g_render.m_fonts[ e_font_names::font_name_havoc_tahoma_13 ]; break;
			case 8: font = g_render.m_fonts[ e_font_names::font_name_airflow_14 ]; break;
			case 9: font = g_render.m_fonts[ e_font_names::font_name_evolve_bold_16 ]; break;
			case 10: font = g_render.m_fonts[ e_font_names::font_name_legendware_verdana_12 ]; break;
			case 11:
				font = g_render.m_fonts[ e_font_names::font_name_interium_droid_24 ];
				size = 16.f;
				break;
			case 12: font = g_render.m_fonts[ e_font_names::font_name_skebob_nunito_14 ]; break;
			case 13: font = g_render.m_fonts[ e_font_names::font_name_cumidere_tahoma_16 ]; break;
			case 14: font = g_render.m_fonts[ e_font_names::font_name_illusory_tahoma_13 ]; break;
			case 15: font = g_render.m_fonts[ e_font_names::font_name_lumi_rubik_16 ]; break;
			case 16: font = g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ]; break;
			case 17: font = g_render.m_fonts[ e_font_names::font_name_dna_montserrat_15 ]; break;
			case 18: font = g_render.m_fonts[ e_font_names::font_name_dna_pt_root_bold_15 ]; break;
			case 19: font = g_render.m_fonts[ e_font_names::font_name_cucumber_tahoma_14 ]; break;
			case 20: font = g_render.m_fonts[ e_font_names::font_name_howeweware_minecraft_14 ]; break;
			default: break;
			}
		}

		if ( !font ) {
			font = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
			size = 0.f;
		}

		return { font, size > 0.f ? size : font ? font->FontSize : 0.f };
	}
}

void points_on_trick( const e_points_trick trick )
{
	if ( g_interfaces.m_global_vars_base )
		g_trick_time = g_interfaces.m_global_vars_base->m_real_time;
	if ( trick < e_points_trick::pixel_surf_ride )
		++g_trick_fired[ static_cast< int >( trick ) ];

	if ( !GET_VARIABLE( g_variables.m_points, bool ) || g_menu.m_opened || !g_interfaces.m_global_vars_base )
		return;

	constexpr int k_held_first = static_cast< int >( e_points_trick::pixel_surf );
	constexpr int k_ride_first = static_cast< int >( e_points_trick::pixel_surf_ride );
	static_assert( static_cast< int >( e_points_trick::wall_climb_ride ) - k_ride_first == k_ride_first - 1 - k_held_first, "held / ride blocks out of step" );
	static float next_ride_bonus[ k_ride_first - k_held_first ] = { };
	const float now = g_interfaces.m_global_vars_base->m_real_time;
	const int index = static_cast< int >( trick );

	int amount = 0;
	if ( index >= k_ride_first ) {
		float& next = next_ride_bonus[ index - k_ride_first ];
		if ( now < next )
			return;
		amount = 25;
		next   = now + 0.35f;
	} else if ( index >= k_held_first ) {
		amount                                  = 125;
		next_ride_bonus[ index - k_held_first ] = now + 0.35f;
	} else {
		switch ( trick ) {
		case e_points_trick::edge_bug: amount = 125; break;
		case e_points_trick::fireman: amount = 275; break;
		default: amount = 50; break;
		}
	}

	points_total.fetch_add( amount, std::memory_order_relaxed );
	points_events.fetch_add( 1u, std::memory_order_release );
}

void points_draw( )
{
	constexpr float k_inactivity = 3.f, k_fade = 0.35f, k_quick_anim = 0.25f, k_reveal = 0.22f;
	constexpr float k_alpha_out = 0.08f, k_alpha_in = 0.08f, k_slide = 50.f, k_gap = 6.f;
	constexpr float k_spring = 200.f, k_damping = 40.f, k_impulse = 6.f;
	constexpr float k_bob = 3.75f, k_drop_in = 17.f;

	static int shown = 0;
	static unsigned seen_events = 0;
	static float display_timer = 0.f, fade_timer = 0.f, anim_timer = 0.f, reveal_timer = 0.f, reset_timer = 0.f;
	static bool fading = false, anim_active = false, revealing = false;
	static float offset = 0.f, offset_vel = 0.f, bob_phase = 0.f, drop = 0.f, trail = 0.f;

	const auto reset = [ & ] {
		shown = 0;
		display_timer = fade_timer = anim_timer = reveal_timer = reset_timer = 0.f;
		fading = anim_active = revealing = false;
		offset = offset_vel = bob_phase = drop = trail = 0.f;
	};

	if ( !GET_VARIABLE( g_variables.m_points, bool ) ) {
		points_total.store( 0, std::memory_order_relaxed );
		reset( );
		return;
	}

	const float dt    = ImGui::GetIO( ).DeltaTime > 0.f ? ImGui::GetIO( ).DeltaTime : 1.f / 60.f;
	const float speed = std::clamp( GET_VARIABLE( g_variables.m_points_appear_speed, float ), 0.1f, 20.f );

	const int total         = points_total.load( std::memory_order_relaxed );
	const unsigned events   = points_events.load( std::memory_order_acquire );
	const bool new_event    = events != seen_events;
	seen_events             = events;

	if ( new_event && ( total > 0 || shown > 0 ) ) {
		display_timer = k_inactivity;
		fading        = false;
		fade_timer    = 0.f;
	}

	if ( total > shown ) {
		if ( shown <= 0 ) {
			revealing    = true;
			reveal_timer = 0.f;
			anim_active  = false;
			offset = offset_vel = 0.f;
			drop         = -k_drop_in;
		} else {
			revealing   = false;
			offset_vel += k_slide * k_impulse;
			anim_active = true;
			anim_timer  = 0.f;
		}
		shown = total;
	}

	if ( shown <= 0 && !fading )
		return;

	if ( display_timer > 0.f )
		display_timer = std::max( display_timer - dt, 0.f );
	else if ( !fading ) {
		fading     = true;
		fade_timer = 0.f;
	}

	if ( anim_active && ( anim_timer += dt * speed ) >= k_quick_anim ) {
		anim_timer  = k_quick_anim;
		anim_active = false;
	}
	if ( revealing && ( reveal_timer += dt * speed ) >= k_reveal ) {
		reveal_timer = k_reveal;
		revealing    = false;
	}
	if ( fading )
		fade_timer = std::min( fade_timer + dt, k_fade );

	const auto ease_out = []( const float x ) { return 1.f - ( 1.f - x ) * ( 1.f - x ); };
	const auto ease_in  = []( const float x ) { return x * x; };

	float alpha = 1.f, target = 0.f;
	if ( fading ) {
		target = k_slide;
		alpha  = 1.f - ease_in( std::clamp( fade_timer / k_alpha_out, 0.f, 1.f ) );
	} else if ( revealing ) {
		const float t = ease_out( std::clamp( reveal_timer / k_reveal, 0.f, 1.f ) );
		target        = k_slide * ( 1.f - t ) * 0.9f;
		alpha         = t;
	} else if ( anim_active ) {
		if ( anim_timer < k_quick_anim * 0.5f ) {
			target = k_slide * 0.95f;
			alpha  = 1.f - ease_in( std::clamp( anim_timer / k_alpha_out, 0.f, 1.f ) );
		} else
			alpha = ease_in( std::clamp( ( anim_timer - k_quick_anim * 0.5f ) / k_alpha_in, 0.f, 1.f ) );
	}

	offset_vel += ( k_spring * ( target - offset ) - k_damping * offset_vel ) * dt;
	offset += offset_vel * dt;

	drop += ( 0.f - drop ) * std::min( 8.5f * dt, 1.f );

	bob_phase = std::fmod( bob_phase + dt * std::clamp( GET_VARIABLE( g_variables.m_points_speed, float ), 0.1f, 10.f ) * 3.6f, 6.2831853f );
	const float bob = std::sin( bob_phase ) * k_bob;

	if ( fading && fade_timer >= k_fade && alpha <= 0.001f ) {
		if ( ( reset_timer += dt ) >= 0.2f ) {
			int expected = total;
			points_total.compare_exchange_strong( expected, 0, std::memory_order_relaxed );
			reset( );
		}
		return;
	}
	reset_timer = 0.f;

	const auto [ font, font_size ] = points_font( );
	if ( !font || g_ctx.m_width <= 0.f )
		return;

	const std::string text = std::to_string( shown ) + " pts";
	const ImVec2 size      = font->CalcTextSizeA( font_size, FLT_MAX, 0.f, text.c_str( ) );

	ImVec4 box          = n_misc::g_watermark_box;
	const bool has_box  = box.z > box.x;

	if ( const ImVec4 gif = n_misc::g_watermark_gif_box; has_box && gif.z > gif.x ) {
		box.x = ( std::min )( box.x, gif.x );
		box.z = ( std::max )( box.z, gif.z );
	}
	const int style     = GET_VARIABLE( g_variables.m_watermark_style, int );
	const bool right_of = has_box && ( style == 11 || style == 19 || style == 20 );
	const bool screen   = !has_box || style == 5;

	float x = screen ? g_ctx.m_width - 8.f - size.x : right_of ? box.z + k_gap : box.x - k_gap - size.x;
	float y = ( has_box && !screen ? ( box.y + box.w ) * 0.5f : 20.f ) - size.y * 0.5f;

	x += ( right_of ? -offset : offset ) + GET_VARIABLE( g_variables.m_points_x_offset, float );
	y += drop + bob + GET_VARIABLE( g_variables.m_points_y_offset, float );
	x = std::floor( x );
	y = std::floor( y );

	const bool bobbing = !anim_active && !fading && alpha >= 0.99f;
	trail += ( ( bobbing ? -bob * 1.15f : 0.f ) - trail ) * std::clamp( dt * 5.f, 0.f, 1.f );

	const c_color accent = GET_VARIABLE( g_variables.m_accent, c_color );
	const auto accent_a  = [ & ]( const float a ) {
		return IM_COL32( accent[ 0 ], accent[ 1 ], accent[ 2 ], static_cast< int >( std::clamp( alpha * a, 0.f, 255.f ) ) );
	};

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddText( font, font_size, ImVec2( x + 1.8f, y + 1.8f + trail * 1.35f ), accent_a( 90.f ), text.c_str( ) );
	draw_list->AddText( font, font_size, ImVec2( x + 1.25f, y + 1.25f + trail ), accent_a( 135.f ), text.c_str( ) );
	draw_list->AddText( font, font_size, ImVec2( x + 1.f, y + 1.f + trail ), accent_a( 180.f ), text.c_str( ) );
	draw_list->AddText( font, font_size, ImVec2( x + 0.5f, y + 0.5f + trail * 0.55f ), accent_a( 180.f ), text.c_str( ) );
	draw_list->AddText( font, font_size, ImVec2( x, y ), IM_COL32( 255, 255, 255, static_cast< int >( std::clamp( alpha * 255.f, 0.f, 255.f ) ) ),
	                    text.c_str( ) );

	g_render.end_stretch_block( block );
}
