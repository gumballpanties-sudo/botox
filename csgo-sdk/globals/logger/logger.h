#pragma once

#include <deque>
#include <string>

#include "../../dependencies/imgui/imgui.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../interfaces/interfaces.h"

void botox_dbg_log( const char* fmt, ... );

namespace n_logger
{
	struct text_t {
		explicit text_t( const std::string& text, const std::string& prefix, float time ) : m_text( text ), m_prefix( prefix ), m_time( time ) { }

		std::string m_text{ }, m_prefix{ };
		float m_time{ };
		c_unsigned_char_color m_color = c_unsigned_char_color( 255.f, 255.f, 255.f, 255.f );
	};

	struct impl_t {
		impl_t( ) : m_text( ){ };

		void print( const std::string& text, const std::string& prefix = "[log]", float time = 8.f )
		{
			m_text.push_back( n_logger::text_t( text, prefix, time ) );

			std::size_t length = text.size( );
			while ( length && ( text[ length - 1 ] == '\n' || text[ length - 1 ] == '\r' ) )
				--length;
			botox_dbg_log( "LOG %s %.*s", prefix.c_str( ), static_cast< int >( length ), text.c_str( ) );
		};

		void on_paint_traverse( )
		{
			int m_x = 10, m_y = g_ctx.m_last_spectators_y, m_size = 15;

			float m_time_left{ };

			for ( std::size_t i = 0; i < m_text.size( ); ) {
				auto& m_curr_noti = m_text[ i ];

				m_curr_noti.m_time -= g_interfaces.m_global_vars_base->m_frame_time;

				if ( m_curr_noti.m_time <= 0.f
				     || i > 15 )
				{
					m_text.erase( m_text.begin( ) + i );
					continue;
				}

				i++;
			}

			if ( m_text.empty( ) )
				return;

			g_render.queue_stretch_block_begin( );

			for ( std::size_t i = 0; i < m_text.size( ); i++ ) {
				auto& m_curr_noti = m_text[ i ];

				m_time_left = m_curr_noti.m_time;

				if ( m_time_left < .5f ) {
					float f = m_time_left;
					f       = std::clamp( f, 0.f, .5f );

					f /= .5f;

					m_curr_noti.m_color.a = static_cast< int >( f * 255.f );

					if ( i == 0 && f < 0.2f )
						m_y -= m_size * ( 1.f - f / 0.2f );
				}

				const ImColor accent_color = ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent );

				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_text,
					std::make_any< text_draw_object_t >(
						g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ], c_vector_2d( m_x, m_y ), m_curr_noti.m_prefix,
						ImColor( accent_color.Value.x, accent_color.Value.y, accent_color.Value.z, m_curr_noti.m_color.a / 255.f ),
						ImColor( 0.f, 0.f, 0.f, m_curr_noti.m_color.a / 255.f ), e_text_flags::text_flag_dropshadow ) );

				const auto text_size = g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ]->CalcTextSizeA(
					g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ]->FontSize, FLT_MAX, 0.f, m_curr_noti.m_prefix.c_str( ) );

				g_render.m_draw_data.emplace_back(
					e_draw_type::draw_type_text,
					std::make_any< text_draw_object_t >(
						g_render.m_fonts[ e_font_names::font_name_tahoma_12 ], c_vector_2d( m_x + text_size.x + 3, m_y ), m_curr_noti.m_text,
						ImColor( m_curr_noti.m_color.r / 255.f, m_curr_noti.m_color.g / 255.f, m_curr_noti.m_color.b / 255.f,
				                 m_curr_noti.m_color.a / 255.f ),
						ImColor( 0.f, 0.f, 0.f, m_curr_noti.m_color.a / 255.f ), e_text_flags::text_flag_dropshadow ) );

				m_y += m_size;
			}

			g_render.queue_stretch_block_end( );
		}

	private:
		std::deque< n_logger::text_t > m_text{ };
	};
}

inline n_logger::impl_t g_logger = { };
