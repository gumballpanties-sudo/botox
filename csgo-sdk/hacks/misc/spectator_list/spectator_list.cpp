#include "spectator_list.h"
#include "kamibebra_gif_data.h"
#include "airflow_data.h"
#include "../scaleform/image_cache.h"

#include <ctime>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace Gdiplus
{
	using std::max;
	using std::min;
}
#include <gdiplus.h>

#pragma comment( lib, "gdiplus.lib" )

namespace {
	std::string utf8_truncate( const std::string& text, const std::size_t max_characters )
	{
		std::size_t characters = 0, offset = 0;

		while ( offset < text.size( ) ) {
			if ( characters >= max_characters )
				return text.substr( 0, offset );

			const auto byte = static_cast< unsigned char >( text[ offset ] );

			offset += byte < 0x80 ? 1 : byte < 0xe0 ? 2 : byte < 0xf0 ? 3 : 4;
			++characters;
		}

		return text;
	}

}

void n_misc::impl_t::draw_spectating_local( )
{
	int m_y = 5;

	std::vector< spectator_data_t > spectator_data{ };

	static std::vector< int > spectator_order{ };

	if ( !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) || !GET_VARIABLE( g_variables.m_spectators_list, bool ) ||
	     GET_VARIABLE( g_variables.m_spectators_list_style, int ) != 0  ||
	     GET_VARIABLE( g_variables.m_spectators_list_type, int ) != 1  ) {
		if ( !spectator_order.empty( ) ) {
			g_ctx.m_last_spectators_y = 5;
			spectator_order.clear( );
		}
		return;
	}

	constexpr auto get_player_spec_type = [ & ]( int obs_mode ) -> std::string {
		switch ( obs_mode ) {
		case e_obs_mode::obs_mode_deathcam:
			return "deathcam";
		case e_obs_mode::obs_mode_freezecam:
			return "freezecam";
		case e_obs_mode::obs_mode_in_eye:
			return "first person";
		case e_obs_mode::obs_mode_chase:
			return "3rd person";
		case e_obs_mode::obs_mode_roaming:
			return "roaming";
		case e_obs_mode::obs_mode_fixed:
			return "fixed";
		default:
			return "first person";
		}
	};

	g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
		if ( !entity || entity->is_alive( ) || entity->is_dormant( ) )
			return;

		const auto entity_index = entity->get_index( );
		const auto entity_team  = entity->get_team( );

		const auto spectated_player = reinterpret_cast< c_base_entity* >(
			g_interfaces.m_client_entity_list->get_client_entity_from_handle( entity->get_observer_target_handle( ) ) );

		if ( !spectated_player || spectated_player != g_ctx.m_local )
			return;

		player_info_t spectating_info{ };
		g_interfaces.m_engine_client->get_player_info( entity_index, &spectating_info );

		if ( spectating_info.m_is_hltv )
			return;

		spectator_data.push_back(
			{ std::format( "{} | {}", utf8_truncate( spectating_info.m_name, 12 ).append( "..." ),
		                                                      get_player_spec_type( entity->get_observer_mode( ) ) ),
		      spectating_info.m_fake_player ? entity_team == e_team_id::team_tt   ? g_render.m_terrorist_avatar
		                                      : entity_team == e_team_id::team_ct ? g_render.m_counter_terrorist_avatar
		                                                                          : nullptr
		                                    : g_avatar_cache[ entity_index ],
		      GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color ), static_cast< int >( entity_index ) } );
	} );

	if ( spectator_data.empty( ) ) {
		g_ctx.m_last_spectators_y = 5;
		spectator_order.clear( );
		return;
	}

	std::erase_if( spectator_order, [ & ]( int index ) {
		return std::none_of( spectator_data.begin( ), spectator_data.end( ),
		                     [ & ]( const spectator_data_t& data ) { return data.m_index == index; } );
	} );

	for ( const auto& data : spectator_data )
		if ( std::find( spectator_order.begin( ), spectator_order.end( ), data.m_index ) == spectator_order.end( ) )
			spectator_order.push_back( data.m_index );

	const auto draw_avatar            = GET_VARIABLE( g_variables.m_spectators_avatar, bool );
	constexpr static auto avatar_size = 14.f;

	g_render.queue_stretch_block_begin( );

	for ( const int index : spectator_order ) {
		const auto data = std::find_if( spectator_data.begin( ), spectator_data.end( ),
		                                [ & ]( const spectator_data_t& entry ) { return entry.m_index == index; } );

		if ( data == spectator_data.end( ) )
			continue;

		if ( draw_avatar )
			g_render.m_draw_data.emplace_back(
				e_draw_type::draw_type_texture,
				std::make_any< texture_draw_object_t >( c_vector_2d( 10, m_y ), c_vector_2d( avatar_size, avatar_size ),
			                                            ImColor( 1.f, 1.f, 1.f, 1.f ), data->m_avatar, 0.f, ImDrawFlags_::ImDrawFlags_None ) );

		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_text,
			std::make_any< text_draw_object_t >( g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ], c_vector_2d( draw_avatar ? 27 : 10, m_y ),
		                                         data->m_text.c_str( ), data->m_color.get_u32( ),
		                                         ImColor( 0.f, 0.f, 0.f, data->m_color.base< e_color_type::color_type_a >( ) ),
		                                         e_text_flags::text_flag_dropshadow ) );

		m_y += 15;

		g_ctx.m_last_spectators_y = m_y;
	}

	g_render.queue_stretch_block_end( );
}

void n_misc::impl_t::draw_spectator_list( )
{
	const ImColor accent_color = ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent );

	/* per row state, so rows slide + fade instead of popping. avatar = index, never a texture (on_remove_entity frees it) */
	struct spectator_animation_t {
		std::string m_text = { };
		c_color m_color    = { };
		int m_index        = { };
		int m_team         = { };
		bool m_fake_player = { };
		float m_progress   = { };
		bool m_seen        = { };
		bool m_removing    = { };
	};

	static std::vector< spectator_animation_t > spectator_animations{ };

	static float animated_width = 0.f, target_width = 0.f;

	static bool snap_width = true;

	if ( !g_interfaces.m_engine_client->is_in_game( ) || !GET_VARIABLE( g_variables.m_spectators_list, bool ) ) {
		if ( !spectator_animations.empty( ) )
			spectator_animations.clear( );

		animated_width = target_width = 0.f;
		snap_width                    = true;

		return;
	}

	if ( !g_ctx.m_local )
		return;

	std::vector< spectator_animation_t > spectator_data{ };

	g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
		if ( !entity || entity->is_alive( ) || entity->is_dormant( ) )
			return;

		const auto entity_index = entity->get_index( );
		const auto entity_team  = entity->get_team( );

		const auto spectated_player = reinterpret_cast< c_base_entity* >(
			g_interfaces.m_client_entity_list->get_client_entity_from_handle( entity->get_observer_target_handle( ) ) );

		if ( !spectated_player || !spectated_player->is_alive( ) )
			return;

		const int spectated_player_index = spectated_player->get_index( );

		player_info_t spectating_info{ }, spectated_info{ };
		g_interfaces.m_engine_client->get_player_info( entity_index, &spectating_info );
		g_interfaces.m_engine_client->get_player_info( spectated_player_index, &spectated_info );

		if ( spectating_info.m_is_hltv )
			return;

		spectator_animation_t data{ };
		data.m_text        = std::format( ( "{} -> {}" ), utf8_truncate( spectating_info.m_name, 24 ),
		                                  utf8_truncate( spectated_info.m_name, 24 ) );
		data.m_color       = spectated_player == g_ctx.m_local ? GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color )
		                                                       : GET_VARIABLE( g_variables.m_spectators_list_text_color_two, c_color );
		data.m_index       = entity_index;
		data.m_team        = entity_team;
		data.m_fake_player = spectating_info.m_fake_player;
		data.m_seen        = true;

		spectator_data.push_back( data );
	} );

	if ( spectator_animations.empty( ) )
		snap_width = true;

	for ( auto& animation : spectator_animations )
		animation.m_seen = false;

	for ( const auto& data : spectator_data ) {
		const auto animation = std::find_if( spectator_animations.begin( ), spectator_animations.end( ),
		                                     [ & ]( const spectator_animation_t& entry ) { return entry.m_index == data.m_index; } );

		if ( animation == spectator_animations.end( ) ) {
			spectator_animations.push_back( data );
			continue;
		}

		animation->m_text        = data.m_text;
		animation->m_color       = data.m_color;
		animation->m_team        = data.m_team;
		animation->m_fake_player = data.m_fake_player;
		animation->m_seen        = true;
		animation->m_removing    = false;
	}

	constexpr auto animation_speed = 6.f;

	for ( auto& animation : spectator_animations ) {
		if ( !animation.m_seen )
			animation.m_removing = true;

		animation.m_progress = std::clamp(
			animation.m_progress + ImGui::GetIO( ).DeltaTime * animation_speed * ( animation.m_removing ? -1.f : 1.f ), 0.f, 1.f );
	}

	std::erase_if( spectator_animations,
	               []( const spectator_animation_t& animation ) { return animation.m_removing && animation.m_progress <= 0.f; } );

	constexpr auto background_height = 25.f;

	constexpr auto title_text  = "spectators";
	const auto title_text_size = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ]->CalcTextSizeA(
		g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ]->FontSize, FLT_MAX, 0.f, title_text );

	const bool always_show = GET_VARIABLE( g_variables.m_spectators_list_always_show, bool );

	ImAnimationHelper spectators_list_ui_animation = ImAnimationHelper( ImHashStr( "botox-spectators-list-ui" ), ImGui::GetIO( ).DeltaTime );
	spectators_list_ui_animation.Update( 2.f, always_show || !spectator_animations.empty( ) ? 2.f : -2.f );

	if ( spectators_list_ui_animation.AnimationData->second < 0.02f )
		return;

	constexpr auto avatar_size    = 14.f;
	constexpr auto padding_x      = 8.f;
	constexpr auto bottom_padding = 2.f;
	constexpr auto slide_distance = 10.f;

	constexpr auto list_start_y = background_height + bottom_padding;

	const auto draw_avatar = GET_VARIABLE( g_variables.m_spectators_avatar, bool );

	const float spacing        = ImGui::GetStyle( ).ItemSpacing.x;
	const float text_height    = ImGui::GetTextLineHeight( );
	const float content_height = draw_avatar ? ImMax( avatar_size, text_height ) : text_height;
	const float row_height     = content_height + ImGui::GetStyle( ).ItemSpacing.y;
	const float text_offset_x  = draw_avatar ? avatar_size + spacing : 0.f;

	float list_height = 0.f;
	float widest_row  = 0.f;

	for ( const auto& animation : spectator_animations ) {
		list_height += row_height * animation.m_progress;
		widest_row = ImMax( widest_row, text_offset_x + ImGui::CalcTextSize( animation.m_text.c_str( ) ).x );
	}

	if ( !spectator_animations.empty( ) || always_show ) {
		target_width = ImMax( title_text_size.x + 40.f, widest_row + padding_x * 2.f );

		animated_width = snap_width ? target_width : ImLerp( animated_width, target_width, ImSaturate( ImGui::GetIO( ).DeltaTime * 12.f ) );
		snap_width     = false;
	}

	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_Alpha, spectators_list_ui_animation.AnimationData->second );

	int flags = ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_::ImGuiWindowFlags_NoResize;

	if ( !( g_ctx.m_is_window_focused ) )
		flags |= ImGuiWindowFlags_::ImGuiWindowFlags_NoMove;

	ImGui::SetNextWindowSize( ImVec2( animated_width, list_start_y + list_height + bottom_padding ), ImGuiCond_::ImGuiCond_Always );
	ImGui::Begin( ( "botox-spectators-list-ui" ), 0, flags );
	{
		const auto window = ImGui::GetCurrentWindow( );

		const auto draw_list = window->DrawList;

		const auto size     = window->Size;
		const auto position = window->Pos;

		[ & ]( ) {
			ImGui::PushClipRect( ImVec2( position.x, position.y ), ImVec2( position.x + size.x, position.y + background_height ), false );
			draw_list->AddRectFilled( ImVec2( position.x, position.y ), ImVec2( position.x + size.x, position.y + background_height ),
			                          ImColor( 25 / 255.f, 25 / 255.f, 25 / 255.f, spectators_list_ui_animation.AnimationData->second ),
			                          ImGui::GetStyle( ).WindowRounding, ImDrawFlags_RoundCornersTop );
			ImGui::PopClipRect( );

			RenderFadedGradientLine( draw_list, ImVec2( position.x, position.y + background_height - 1.f ), ImVec2( size.x, 1.f ),
			                         ImColor( accent_color.Value.x, accent_color.Value.y, accent_color.Value.z ) );

			draw_list->AddText(
				g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ], g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ]->FontSize,
				ImVec2( position.x + ( ( size.x - title_text_size.x ) / 2.f ), position.y + ( ( background_height - title_text_size.y ) / 2.f ) ),
				ImColor( 1.f, 1.f, 1.f, spectators_list_ui_animation.AnimationData->second ), title_text );

			ImGui::PushClipRect( ImVec2( position.x + 1.f, position.y + 1.f ), ImVec2( position.x + size.x - 1.f, position.y + size.y - 1.f ),
			                     false );
			draw_list->AddRect( ImVec2( position.x + 1.f, position.y + 1.f ), ImVec2( position.x + size.x - 1.f, position.y + size.y - 1.f ),
			                    ImColor( 50 / 255.f, 50 / 255.f, 50 / 255.f, spectators_list_ui_animation.AnimationData->second ),
			                    ImGui::GetStyle( ).WindowRounding );
			ImGui::PopClipRect( );
		}( );

		const float window_alpha = spectators_list_ui_animation.AnimationData->second;

		/* short rows don't set the width (title floor does): centre the row block, same gap both sides, one
		   left edge. never tighter than padding_x. */
		const float block_x = ImMax( padding_x, ( size.x - widest_row ) / 2.f );

		float row_y = list_start_y;

		for ( const auto& animation : spectator_animations ) {
			const float animated_height = row_height * animation.m_progress;

			if ( animation.m_progress > 0.f ) {
				const float alpha = window_alpha * animation.m_progress;

				const float row_x = position.x + block_x - ( 1.f - animation.m_progress ) * slide_distance;

				const float content_y = position.y + row_y + ( animated_height - content_height ) / 2.f;

				draw_list->PushClipRect( ImVec2( position.x, position.y + row_y ),
				                         ImVec2( position.x + size.x, position.y + row_y + animated_height ), true );

				if ( draw_avatar ) {
					IDirect3DTexture9* avatar = nullptr;

					if ( animation.m_fake_player )
						avatar = animation.m_team == e_team_id::team_tt   ? g_render.m_terrorist_avatar
						         : animation.m_team == e_team_id::team_ct ? g_render.m_counter_terrorist_avatar
						                                                  : nullptr;
					else if ( animation.m_index >= 0 && animation.m_index < 64 )
						avatar = g_avatar_cache[ animation.m_index ];

					if ( avatar ) {
						const float avatar_y = content_y + ( content_height - avatar_size ) / 2.f;

						draw_list->AddImageRounded( avatar, ImVec2( row_x, avatar_y ), ImVec2( row_x + avatar_size, avatar_y + avatar_size ),
						                            ImVec2( 0, 0 ), ImVec2( 1, 1 ), ImColor( 1.f, 1.f, 1.f, alpha ), 6.f );
					}
				}

				draw_list->AddText( ImVec2( row_x + text_offset_x, content_y + ( content_height - text_height ) / 2.f ),
				                    ImColor( animation.m_color.get_vec4( alpha ) ), animation.m_text.c_str( ) );

				draw_list->PopClipRect( );
			}

			row_y += animated_height;
		}
	}
	ImGui::End( );

	ImGui::PopStyleVar( );
}

void n_misc::impl_t::collect_spectators( std::vector< spectator_entry_t >& out, const bool all_players, const bool keep_hltv )
{
	out.clear( );

	if ( !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	const bool local_only = !all_players && GET_VARIABLE( g_variables.m_spectators_list_type, int ) == 1;

	g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
		if ( !entity || entity->is_alive( ) || entity->is_dormant( ) )
			return;

		const auto entity_index = entity->get_index( );

		const auto spectated_player = reinterpret_cast< c_base_entity* >(
			g_interfaces.m_client_entity_list->get_client_entity_from_handle( entity->get_observer_target_handle( ) ) );

		if ( !spectated_player || !spectated_player->is_alive( ) )
			return;

		const bool watching_local = spectated_player == g_ctx.m_local;

		if ( local_only && !watching_local )
			return;

		player_info_t spectating_info{ }, spectated_info{ };
		g_interfaces.m_engine_client->get_player_info( entity_index, &spectating_info );
		g_interfaces.m_engine_client->get_player_info( spectated_player->get_index( ), &spectated_info );

		/* gotv sits on the server as a dead player watching everyone, never a real spectator */
		if ( spectating_info.m_is_hltv && !keep_hltv )
			return;

		const int observer_mode = entity->get_observer_mode( );

		spectator_entry_t entry{ };
		entry.m_name           = spectating_info.m_name;
		entry.m_target_name    = spectated_info.m_name;
		entry.m_index          = static_cast< int >( entity_index );
		entry.m_team           = entity->get_team( );
		entry.m_fake_player    = spectating_info.m_fake_player;
		entry.m_watching_local = watching_local;
		entry.m_obs_mode       = observer_mode;

		entry.m_target_index       = spectated_player->get_index( );
		entry.m_target_team        = spectated_player->get_team( );
		entry.m_target_fake_player = spectated_info.m_fake_player;

		entry.m_mode = observer_mode == e_obs_mode::obs_mode_chase ? "3rd" : "1st";

		out.push_back( entry );
	} );
}

static std::string spectator_row_text( const n_misc::spectator_entry_t& entry )
{
	return GET_VARIABLE( g_variables.m_spectators_list_show_target, bool ) ? std::format( "{} -> {}", entry.m_name, entry.m_target_name )
	                                                                      : entry.m_name;
}

/* bots: flat team icon; players: avatar cache by index, never a stored pointer (on_remove_entity frees it).
   null = steam not ready yet, callers keep the column reserved so rows don't jump */
static IDirect3DTexture9* spectator_avatar_texture( const int index, const int team, const bool fake_player )
{
	if ( fake_player )
		return team == e_team_id::team_tt   ? g_render.m_terrorist_avatar
		       : team == e_team_id::team_ct ? g_render.m_counter_terrorist_avatar
		                                    : nullptr;

	return g_avatar_cache[ index ];
}

static std::string ellipsize_text( ImFont* font, const float font_size, const std::string& text, const float max_width )
{
	if ( max_width <= 0.f || text.empty( ) )
		return { };

	if ( font->CalcTextSizeA( font_size, FLT_MAX, 0.f, text.c_str( ) ).x <= max_width )
		return text;

	constexpr auto ellipsis = "...";

	const float ellipsis_width = font->CalcTextSizeA( font_size, FLT_MAX, 0.f, ellipsis ).x;

	std::string kept{ };

	for ( std::size_t offset = 0; offset < text.size( ); ) {
		const auto byte = static_cast< unsigned char >( text[ offset ] );

		std::size_t length = byte >= 0xf0 ? 4 : byte >= 0xe0 ? 3 : byte >= 0xc0 ? 2 : 1;

		length = ImMin( length, text.size( ) - offset );

		const std::string candidate = kept + text.substr( offset, length );

		if ( font->CalcTextSizeA( font_size, FLT_MAX, 0.f, candidate.c_str( ) ).x + ellipsis_width > max_width )
			break;

		kept = candidate;
		offset += length;
	}

	return kept.empty( ) ? std::string( ellipsis ) : kept + ellipsis;
}

static void draw_outlined_text( ImDrawList* draw_list, ImFont* font, float font_size, ImVec2 position, ImU32 color, const char* text )
{
	constexpr ImVec2 offsets[ 8 ] = { { -1.f, -1.f }, { 0.f, -1.f }, { 1.f, -1.f }, { -1.f, 0.f },
		                             { 1.f, 0.f },   { -1.f, 1.f }, { 0.f, 1.f },  { 1.f, 1.f } };

	for ( const auto& offset : offsets )
		draw_list->AddText( font, font_size, ImVec2( position.x + offset.x, position.y + offset.y ), IM_COL32( 0, 0, 0, 255 ), text );

	draw_list->AddText( font, font_size, position, color, text );
}

void n_misc::impl_t::draw_spectator_list_delusional( )
{
	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );

	if ( entries.empty( ) )
		return;

	const auto font = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];

	if ( !font )
		return;

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const float screen_width = g_ctx.m_width;

	const bool left = GET_VARIABLE( g_variables.m_spectators_list_delusional_left, bool );

	float y = 5.f;

	if ( !left && GET_VARIABLE( g_variables.m_watermark, bool ) )
		y += 6.f + 19.f;

	const ImU32 local_color = ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent );

	const float font_size = font->FontSize;

	const bool draw_avatar = GET_VARIABLE( g_variables.m_spectators_avatar, bool );

	constexpr float avatar_size = 14.f, avatar_gap = 4.f;

	const float avatar_block = draw_avatar ? avatar_size + avatar_gap : 0.f;

	int row = 0;

	for ( const auto& entry : entries ) {
		const std::string text = spectator_row_text( entry );

		const float text_width = font->CalcTextSizeA( font_size, FLT_MAX, 0.f, text.c_str( ) ).x;

		const float row_x = left ? 6.f + avatar_block : screen_width - 6.f - text_width;
		const float row_y = y + 16.f * row++;

		if ( draw_avatar ) {
			if ( const auto avatar = spectator_avatar_texture( entry.m_index, entry.m_team, entry.m_fake_player ) )
				draw_list->AddImage( avatar, ImVec2( row_x - avatar_block, row_y ),
				                     ImVec2( row_x - avatar_block + avatar_size, row_y + avatar_size ) );
		}

		draw_list->AddText( font, font_size, ImVec2( row_x + 1.f, row_y + 1.f ), IM_COL32( 0, 0, 0, 255 ), text.c_str( ) );
		draw_list->AddText( font, font_size, ImVec2( row_x, row_y ), entry.m_watching_local ? local_color : IM_COL32( 255, 255, 255, 255 ),
		                    text.c_str( ) );
	}

	g_render.end_stretch_block( block );
}

static void drag_placed_panel( const float panel_width, const float panel_height )
{
	auto& list_x = GET_VARIABLE( g_variables.m_spectators_list_x, int );
	auto& list_y = GET_VARIABLE( g_variables.m_spectators_list_y, int );

	if ( g_ctx.m_display_stable ) {
		/* area fraction, not a px floor (a short list never cleared a px floor and teleported every frame).
		   >95% gone = lost */
		constexpr float min_visible_fraction = 0.05f, pad = 10.f;

		const float px = static_cast< float >( list_x ), py = static_cast< float >( list_y );

		const float visible_x = ImMax( ImMin( px + panel_width, g_ctx.m_width ) - ImMax( px, 0.f ), 0.f );
		const float visible_y = ImMax( ImMin( py + panel_height, g_ctx.m_height ) - ImMax( py, 0.f ), 0.f );

		if ( panel_width > 0.f && panel_height > 0.f &&
		     ( visible_x / panel_width ) * ( visible_y / panel_height ) < min_visible_fraction ) {
			list_x = static_cast< int >( ImClamp( px, pad, ImMax( pad, g_ctx.m_width - panel_width - pad ) ) );
			list_y = static_cast< int >( ImClamp( py, pad, ImMax( pad, g_ctx.m_height - panel_height - pad ) ) );
		}
	}

	if ( g_menu.m_opened ) {
		static bool dragging = false;
		static ImVec2 drag_offset{ };

		const ImVec2 mouse = g_render.screen_mouse( );
		const ImVec2 size( panel_width, panel_height );
		const ImVec2 shown = g_render.dpi_panel_pos( ImVec2( static_cast< float >( list_x ), static_cast< float >( list_y ) ), size );
		const float scale  = g_render.m_dpi_panel_scale;

		const bool over = mouse.x >= shown.x && mouse.x <= shown.x + panel_width * scale && mouse.y >= shown.y &&
		                  mouse.y <= shown.y + panel_height * scale;
		const bool held = ImGui::IsMouseDown( ImGuiMouseButton_::ImGuiMouseButton_Left );

		if ( over && held ) {
			if ( !dragging )
				drag_offset = ImVec2( mouse.x - shown.x, mouse.y - shown.y );

			dragging = true;
		} else if ( !held ) {
			dragging = false;
		}

		if ( dragging ) {
			const ImVec2 layout = g_render.dpi_panel_layout_pos( ImVec2( mouse.x - drag_offset.x, mouse.y - drag_offset.y ), size );

			list_x = static_cast< int >( layout.x );
			list_y = static_cast< int >( layout.y );
		}
	}
}

void n_misc::impl_t::draw_spectator_list_interwebz( )
{
	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );

	const auto& list_x = GET_VARIABLE( g_variables.m_spectators_list_x, int );
	const auto& list_y = GET_VARIABLE( g_variables.m_spectators_list_y, int );
	const float w      = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_width, int ) );
	const int count    = static_cast< int >( entries.size( ) );

	/* paints the frame from last frame's count (stale row flashes). count first, paint once */
	const float panel_height = 20.f + count * 20.f;

	drag_placed_panel( w + 20.f, panel_height );

	const auto title_font = g_render.m_fonts[ e_font_names::font_name_tahoma_12 ];
	const auto row_font   = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];

	if ( !title_font || !row_font )
		return;

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const float x = static_cast< float >( list_x ), y = static_cast< float >( list_y );

	if ( GET_VARIABLE( g_variables.m_spectators_list_interwebz_background, bool ) ) {
		for ( int i = 1; i < 20; i++ )
			draw_list->AddLine( ImVec2( x + i + 1.f, y + 20.f - i ), ImVec2( x + i + 1.f + w - 1.f, y + 20.f - i ),
			                    IM_COL32( 59, 59, 59, 255 ) );

		for ( int i = 1; i <= count; i++ )
			draw_list->AddRectFilled( ImVec2( x, y + i * 20.f ), ImVec2( x + w, y + i * 20.f + 20.f ),
			                          i % 2 == 0 ? IM_COL32( 75, 75, 75, 255 ) : IM_COL32( 45, 45, 45, 255 ) );

		constexpr ImU32 frame_color = IM_COL32( 85, 85, 85, 255 );

		draw_list->AddLine( ImVec2( x + 20.f, y ), ImVec2( x + 20.f + w + 1.f, y ), frame_color );
		draw_list->AddLine( ImVec2( x, y + 20.f ), ImVec2( x + 20.f, y ), frame_color );
		draw_list->AddLine( ImVec2( x + w, y + 20.f ), ImVec2( x + w + 20.f, y ), frame_color );
		draw_list->AddLine( ImVec2( x, y + 20.f ), ImVec2( x + w, y + 20.f ), frame_color );
		draw_list->AddLine( ImVec2( x, y + 20.f ), ImVec2( x, y + 20.f + count * 20.f ), frame_color );
		draw_list->AddLine( ImVec2( x + w, y + 20.f ), ImVec2( x + w, y + 20.f + count * 20.f ), frame_color );
		draw_list->AddLine( ImVec2( x, y + 20.f + count * 20.f ), ImVec2( x + w, y + 20.f + count * 20.f ), frame_color );
	}

	const ImU32 accent_color = ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent );

	const float label_x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_interwebz_label_x, int ) );

	draw_outlined_text( draw_list, title_font, title_font->FontSize, ImVec2( x + ( w / 2.f ) - 42.f + label_x, y + 4.f ),
	                    accent_color, "SPECTATOR LIST" );

	draw_list->PushClipRect( ImVec2( x, y ), ImVec2( x + w, y + panel_height ), true );

	const bool draw_avatar = GET_VARIABLE( g_variables.m_spectators_avatar, bool );

	constexpr float avatar_size = 16.f, avatar_gap = 4.f;

	const float avatar_block = draw_avatar ? avatar_size + avatar_gap : 0.f;

	int row = 0;

	for ( const auto& entry : entries ) {
		const std::string text = spectator_row_text( entry );

		++row;

		if ( draw_avatar ) {
			if ( const auto avatar = spectator_avatar_texture( entry.m_index, entry.m_team, entry.m_fake_player ) ) {
				const float avatar_y = y + 2.f + row * 20.f;

				draw_list->AddImage( avatar, ImVec2( x + 4.f, avatar_y ), ImVec2( x + 4.f + avatar_size, avatar_y + avatar_size ) );
			}
		}

		draw_outlined_text( draw_list, row_font, row_font->FontSize, ImVec2( x + 4.f + avatar_block, y + 4.f + row * 20.f ),
		                    entry.m_watching_local ? accent_color : IM_COL32( 255, 255, 255, 255 ), text.c_str( ) );
	}

	draw_list->PopClipRect( );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_winxp( )
{
	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );

	const auto font = g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ];

	if ( !font )
		return;

	/* recomputes size off row overflow every frame (drawn at last frame's size, settles in one).
	   a "measure first" rewrite changes the layout */
	static float size_x = -200.f, size_y = -200.f;

	int flags = ImGuiWindowFlags_::ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_::ImGuiWindowFlags_NoResize |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse;

	if ( !g_menu.m_opened )
		flags |= ImGuiWindowFlags_::ImGuiWindowFlags_NoMove;

	ImGui::SetNextWindowSize( ImVec2( 400.f + size_x, 300.f + size_y ), ImGuiCond_::ImGuiCond_Always );
	ImGui::Begin( "botox-spectators-winxp", 0, flags );
	{
		const auto draw_list = ImGui::GetWindowDrawList( );

		const ImVec2 window_position = ImGui::GetWindowPos( );
		const ImVec2 origin( window_position.x + 1.f, window_position.y + 1.f );

		const auto filled = [ & ]( ImVec2 at, ImVec2 size, ImU32 color ) {
			draw_list->AddRectFilled( ImVec2( origin.x + at.x, origin.y + at.y ), ImVec2( origin.x + at.x + size.x, origin.y + at.y + size.y ),
			                          color, 0.f );
		};

		const auto rect = [ & ]( ImVec2 at, ImVec2 size, ImU32 color ) {
			draw_list->AddRect( ImVec2( origin.x + at.x, origin.y + at.y ), ImVec2( origin.x + at.x + size.x, origin.y + at.y + size.y ), color,
			                    0.f );
		};

		const auto line = [ & ]( ImVec2 at, ImVec2 delta, ImU32 color ) {
			draw_list->AddLine( ImVec2( origin.x + at.x, origin.y + at.y ), ImVec2( origin.x + at.x + delta.x, origin.y + at.y + delta.y ),
			                    color );
		};

		const auto text = [ & ]( const char* string, ImVec2 at, ImU32 color ) {
			draw_list->AddText( font, font->FontSize, ImVec2( origin.x + at.x, origin.y + at.y ), color, string );
		};

		const auto button_bevel = [ & ]( ImVec2 at ) {
			filled( at, ImVec2( 13.f, 11.f ), IM_COL32( 212, 208, 200, 255 ) );

			const ImVec2 o( at.x - 1.f, at.y - 1.f );

			line( o, ImVec2( 15.f, 0.f ), IM_COL32( 255, 255, 255, 255 ) );
			line( o, ImVec2( 0.f, 13.f ), IM_COL32( 255, 255, 255, 255 ) );
			line( ImVec2( o.x + 1.f, o.y + 12.f ), ImVec2( 14.f, 0.f ), IM_COL32( 128, 128, 128, 255 ) );
			line( ImVec2( o.x + 14.f, o.y + 12.f ), ImVec2( 0.f, -11.f ), IM_COL32( 128, 128, 128, 255 ) );
			line( ImVec2( o.x + 1.f, o.y + 13.f ), ImVec2( 15.f, 0.f ), IM_COL32( 0, 0, 0, 255 ) );
			line( ImVec2( o.x + 15.f, o.y + 12.f ), ImVec2( 0.f, -12.f ), IM_COL32( 0, 0, 0, 255 ) );
		};

		filled( ImVec2( 1.f, 1.f ), ImVec2( 397.f + size_x, 299.f + size_y ), IM_COL32( 255, 255, 255, 255 ) );
		rect( ImVec2( 0.f, 0.f ), ImVec2( 399.f + size_x, 300.f + size_y ), IM_COL32( 223, 223, 223, 255 ) );
		filled( ImVec2( 2.f, 2.f ), ImVec2( 395.f + size_x, 296.f + size_y ), IM_COL32( 192, 192, 192, 255 ) );

		line( ImVec2( 4.f, 294.f + size_y ), ImVec2( 391.f + size_x, 0.f ), IM_COL32( 223, 223, 223, 255 ) );
		line( ImVec2( 394.f + size_x, 294.f + size_y ), ImVec2( 0.f, -14.f ), IM_COL32( 223, 223, 223, 255 ) );
		line( ImVec2( 4.f, 280.f + size_y ), ImVec2( 0.f, 15.f ), IM_COL32( 128, 128, 128, 255 ) );
		line( ImVec2( 4.f, 280.f + size_y ), ImVec2( 391.f + size_x, 0.f ), IM_COL32( 128, 128, 128, 255 ) );

		filled( ImVec2( 4.f, 4.f ), ImVec2( 391.f + size_x, 22.f ), IM_COL32( 0, 0, 128, 255 ) );

		rect( ImVec2( 4.f, 46.f ), ImVec2( 391.f + size_x, 232.f + size_y ), IM_COL32( 128, 128, 128, 255 ) );
		rect( ImVec2( 5.f, 47.f ), ImVec2( 389.f + size_x, 230.f + size_y ), IM_COL32( 0, 0, 0, 255 ) );
		filled( ImVec2( 6.f, 48.f ), ImVec2( 387.f + size_x, 228.f + size_y ), IM_COL32( 255, 255, 255, 255 ) );

		text( "Spectators", ImVec2( 32.f, 6.f ), IM_COL32( 255, 255, 255, 255 ) );
		text( "File  Edit  Format  Help", ImVec2( 7.f, 28.f ), IM_COL32( 0, 0, 0, 255 ) );

		button_bevel( ImVec2( 378.f + size_x, 9.f ) );
		text( "X", ImVec2( 381.f + size_x, 7.f ), IM_COL32( 0, 0, 0, 255 ) );

		button_bevel( ImVec2( 344.f + size_x, 9.f ) );
		text( "_", ImVec2( 347.f + size_x, 4.f ), IM_COL32( 0, 0, 0, 255 ) );

		button_bevel( ImVec2( 360.f + size_x, 9.f ) );
		rect( ImVec2( 362.f + size_x, 10.f ), ImVec2( 9.f, 9.f ), IM_COL32( 0, 0, 0, 255 ) );

		/* rows measured against the minimum frame, which then grows by the overflow = settles at the needed size */
		size_x = size_y = -200.f;

		float overflow_x = 0.f, overflow_y = 0.f;
		int total = 0;

		const bool draw_avatar = GET_VARIABLE( g_variables.m_spectators_avatar, bool );

		constexpr float avatar_size = 13.f, avatar_gap = 3.f;

		const float avatar_block = draw_avatar ? avatar_size + avatar_gap : 0.f;

		for ( const auto& entry : entries ) {
			const std::string row = spectator_row_text( entry );

			const ImVec2 at( 35.f, 55.f + total++ * 15.f );

			if ( draw_avatar ) {
				if ( const auto avatar = spectator_avatar_texture( entry.m_index, entry.m_team, entry.m_fake_player ) )
					draw_list->AddImage( avatar, ImVec2( origin.x + at.x, origin.y + at.y + 1.f ),
					                     ImVec2( origin.x + at.x + avatar_size, origin.y + at.y + 1.f + avatar_size ) );
			}

			text( row.c_str( ), ImVec2( at.x + avatar_block, at.y ), IM_COL32( 0, 0, 0, 255 ) );

			if ( entry.m_watching_local )
				draw_list->AddCircleFilled( ImVec2( origin.x + at.x - 10.f, origin.y + at.y + 8.f ), 4.f, IM_COL32( 0, 0, 0, 255 ), 24 );

			if ( at.y > 48.f + 228.f + size_y - 25.f )
				overflow_y = at.y - ( 48.f + 228.f + size_y - 25.f );

			const float row_width = at.x + avatar_block + font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, row.c_str( ) ).x;

			if ( row_width > 387.f + size_x )
				overflow_x = row_width - ( 387.f + size_x );
		}

		if ( overflow_y > 0.f )
			size_y += overflow_y;

		if ( overflow_x > 0.f )
			size_x += overflow_x;

		text( std::format( "{} object(s)", total ).c_str( ), ImVec2( 6.f, 279.f + size_y ), IM_COL32( 0, 0, 0, 255 ) );
	}
	ImGui::End( );
}

void n_misc::impl_t::draw_spectator_list_clarity( )
{
	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );

	if ( entries.empty( ) && !GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) )
		return;

	const auto row_font   = g_render.m_fonts[ e_font_names::font_name_tahoma_12 ];
	const auto title_font = g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ];

	if ( !row_font || !title_font )
		return;

	constexpr auto title_text = "spectators";

	const float row_font_size   = row_font->FontSize;
	const float title_font_size = title_font->FontSize;

	constexpr float padding = 10.f, top_padding = 6.f, bottom_padding = 6.f;

	const float line_height = row_font_size + 6.f;

	const bool draw_avatar = GET_VARIABLE( g_variables.m_spectators_avatar, bool );

	constexpr float avatar_size = 14.f, avatar_gap = 4.f;

	const float avatar_block = draw_avatar ? avatar_size + avatar_gap : 0.f;

	float width = title_font->CalcTextSizeA( title_font_size, FLT_MAX, 0.f, title_text ).x + padding * 2.f;

	std::vector< std::string > rows{ };
	rows.reserve( entries.size( ) );

	for ( const auto& entry : entries ) {
		rows.push_back( spectator_row_text( entry ) );

		width = ImMax( width, avatar_block + row_font->CalcTextSizeA( row_font_size, FLT_MAX, 0.f, rows.back( ).c_str( ) ).x + padding * 2.f );
	}

	const float height = top_padding + static_cast< float >( entries.size( ) ) * line_height + row_font_size + bottom_padding;

	int flags = ImGuiWindowFlags_::ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_::ImGuiWindowFlags_NoResize |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse;

	if ( !g_menu.m_opened )
		flags |= ImGuiWindowFlags_::ImGuiWindowFlags_NoMove;

	ImGui::SetNextWindowSize( ImVec2( width, height ), ImGuiCond_::ImGuiCond_Always );
	ImGui::Begin( "botox-spectators-clarity", 0, flags );
	{
		const auto draw_list = ImGui::GetWindowDrawList( );

		const ImVec2 position = ImGui::GetWindowPos( ), size = ImGui::GetWindowSize( );

		draw_list->AddRectFilledMultiColor( position, ImVec2( position.x + size.x, position.y + size.y ), IM_COL32( 32, 32, 32, 255 ),
		                                    IM_COL32( 32, 32, 32, 255 ), IM_COL32( 15, 15, 15, 255 ), IM_COL32( 15, 15, 15, 255 ) );

		const ImVec2 title_size = title_font->CalcTextSizeA( title_font_size, FLT_MAX, 0.f, title_text );

		draw_list->AddText( title_font, title_font_size, ImVec2( position.x + ( size.x - title_size.x ) * 0.5f, position.y + top_padding ),
		                    IM_COL32( 255, 255, 255, 255 ), title_text );

		const ImU32 accent_color = ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent );

		for ( std::size_t i = 0; i < rows.size( ); i++ ) {
			const float row_y = position.y + top_padding + static_cast< float >( i + 1 ) * line_height;

			if ( draw_avatar ) {
				if ( const auto avatar = spectator_avatar_texture( entries[ i ].m_index, entries[ i ].m_team, entries[ i ].m_fake_player ) ) {
					const float avatar_y = row_y + ( row_font_size - avatar_size ) * 0.5f;

					draw_list->AddImage( avatar, ImVec2( position.x + padding, avatar_y ),
					                     ImVec2( position.x + padding + avatar_size, avatar_y + avatar_size ) );
				}
			}

			draw_list->AddText( row_font, row_font_size, ImVec2( position.x + padding + avatar_block, row_y ),
			                    entries[ i ].m_watching_local ? accent_color : IM_COL32( 150, 150, 150, 255 ), rows[ i ].c_str( ) );
		}
	}
	ImGui::End( );
}

void n_misc::impl_t::draw_spectator_list_chillware( )
{
	struct chillware_row_t {
		std::string m_name        = { };
		std::string m_target      = { };
		int m_index               = 0;
		int m_team                = 0;
		int m_target_index        = 0;
		int m_target_team         = 0;
		bool m_fake_player        = false;
		bool m_target_fake_player = false;
		bool m_watching_local     = false;
		float m_progress          = 0.f;
		float m_reach   = 0.f;
		bool m_seen     = false;
		bool m_removing = false;
	};

	static std::vector< chillware_row_t > rows{ };

	static float animated_width = 0.f;
	static bool snap_width      = true;

	if ( !g_interfaces.m_engine_client->is_in_game( ) ) {
		rows.clear( );
		snap_width = true;

		return;
	}

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );

	for ( auto& row : rows )
		row.m_seen = false;

	for ( const auto& entry : entries ) {
		const auto existing = std::find_if( rows.begin( ), rows.end( ),
		                                    [ & ]( const chillware_row_t& candidate ) { return candidate.m_index == entry.m_index; } );

		chillware_row_t data{ };
		data.m_name               = utf8_truncate( entry.m_name, 32 );
		data.m_target             = utf8_truncate( entry.m_target_name, 32 );
		data.m_index              = entry.m_index;
		data.m_team               = entry.m_team;
		data.m_target_index       = entry.m_target_index;
		data.m_target_team        = entry.m_target_team;
		data.m_fake_player        = entry.m_fake_player;
		data.m_target_fake_player = entry.m_target_fake_player;
		data.m_watching_local     = entry.m_watching_local;
		data.m_seen               = true;

		if ( existing == rows.end( ) ) {
			rows.push_back( data );
			continue;
		}

		data.m_progress = existing->m_progress;
		data.m_reach    = existing->m_reach;
		*existing       = data;
	}

	constexpr float animation_speed = 5.5f, reach_speed = 2.6f;

	const float delta_time = ImGui::GetIO( ).DeltaTime;

	for ( auto& row : rows ) {
		if ( !row.m_seen )
			row.m_removing = true;

		row.m_progress = std::clamp( row.m_progress + delta_time * animation_speed * ( row.m_removing ? -1.f : 1.f ), 0.f, 1.f );

		const bool reaching = !row.m_removing && row.m_progress > 0.4f;

		row.m_reach = std::clamp( row.m_reach + delta_time * reach_speed * ( reaching ? 1.f : -1.f ), 0.f, 1.f );
	}

	const auto ease = []( const float t ) -> float {
		const float clamped = ImSaturate( t );

		return clamped * clamped * ( 3.f - 2.f * clamped );
	};

	std::erase_if( rows, []( const chillware_row_t& row ) { return row.m_removing && row.m_progress <= 0.f; } );

	if ( rows.empty( ) && !GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) ) {
		snap_width = true;

		return;
	}

	const auto font = g_render.m_fonts[ e_font_names::font_name_chillware_13 ];

	if ( !font )
		return;

	const float font_size = font->FontSize;

	constexpr auto title_text = "spectator list";

	constexpr float pad_x = 10.f, pad_y = 7.f;
	constexpr float header_height = 21.f, row_height = 21.f;
	constexpr float avatar_size = 16.f, avatar_gap = 7.f;

	constexpr float link_width = 86.f;

	const bool show_avatar   = GET_VARIABLE( g_variables.m_spectators_avatar, bool );
	const float avatar_block = show_avatar ? avatar_size + avatar_gap : 0.f;

	struct chillware_group_t {
		std::string m_name    = { };
		int m_target_index    = 0;
		int m_target_team     = 0;
		bool m_fake_player    = false;
		bool m_watching_local = false;
		float m_center        = 0.f;
		float m_alpha         = 0.f;
		float m_reach = 0.f;
		int m_count   = 0;
	};

	std::vector< chillware_group_t > groups{ };

	std::vector< float > centers( rows.size( ), 0.f );

	std::vector< std::size_t > order{ };
	order.reserve( rows.size( ) );

	float list_height = 0.f, name_text_width = 0.f, target_text_width = 0.f;

	for ( std::size_t i = 0; i < rows.size( ); i++ ) {
		name_text_width = ImMax( name_text_width, font->CalcTextSizeA( font_size, FLT_MAX, 0.f, rows[ i ].m_name.c_str( ) ).x );

		const auto group = std::find_if( groups.begin( ), groups.end( ), [ & ]( const chillware_group_t& candidate ) {
			return candidate.m_target_index == rows[ i ].m_target_index;
		} );

		if ( group != groups.end( ) )
			continue;

		chillware_group_t data{ };
		data.m_name           = rows[ i ].m_target;
		data.m_target_index   = rows[ i ].m_target_index;
		data.m_target_team    = rows[ i ].m_target_team;
		data.m_fake_player    = rows[ i ].m_target_fake_player;
		data.m_watching_local = rows[ i ].m_watching_local;

		groups.push_back( data );

		target_text_width = ImMax( target_text_width, font->CalcTextSizeA( font_size, FLT_MAX, 0.f, data.m_name.c_str( ) ).x );
	}

	const float max_width = std::clamp( GET_VARIABLE( g_variables.m_spectators_chillware_max_width, float ), 190.f, 600.f );

	const float chrome_width = pad_x * 2.f + link_width + avatar_block * 2.f;

	const float text_budget = ImMax( max_width - chrome_width, 40.f );

	float name_cap = name_text_width, target_cap = target_text_width;

	if ( name_text_width + target_text_width > text_budget ) {
		constexpr float column_floor = 34.f;

		const float share = text_budget / ( name_text_width + target_text_width );

		name_cap   = ImMax( name_text_width * share, column_floor );
		target_cap = ImMax( target_text_width * share, column_floor );

		const float overrun = ( name_cap + target_cap ) - text_budget;

		if ( overrun > 0.f ) {
			if ( name_cap >= target_cap )
				name_cap = ImMax( name_cap - overrun, column_floor );
			else
				target_cap = ImMax( target_cap - overrun, column_floor );
		}
	}

	/* display strings, cut to their column. laid out off THESE, never off the full names */
	std::vector< std::string > names( rows.size( ) );

	float name_width = 0.f, target_width = 0.f;

	for ( std::size_t i = 0; i < rows.size( ); i++ ) {
		names[ i ] = ellipsize_text( font, font_size, rows[ i ].m_name, name_cap );

		name_width = ImMax( name_width, avatar_block + font->CalcTextSizeA( font_size, FLT_MAX, 0.f, names[ i ].c_str( ) ).x );
	}

	for ( auto& group : groups ) {
		group.m_name = ellipsize_text( font, font_size, group.m_name, target_cap );

		target_width = ImMax( target_width, avatar_block + font->CalcTextSizeA( font_size, FLT_MAX, 0.f, group.m_name.c_str( ) ).x );
	}

	for ( std::size_t group_index = 0; group_index < groups.size( ); group_index++ ) {
		for ( std::size_t i = 0; i < rows.size( ); i++ )
			if ( rows[ i ].m_target_index == groups[ group_index ].m_target_index )
				order.push_back( i );
	}

	for ( const auto i : order ) {
		const float height = row_height * ease( rows[ i ].m_progress );

		centers[ i ] = list_height + height * 0.5f;
		list_height += height;

		const auto group = std::find_if( groups.begin( ), groups.end( ), [ & ]( const chillware_group_t& candidate ) {
			return candidate.m_target_index == rows[ i ].m_target_index;
		} );

		if ( group == groups.end( ) )
			continue;

		group->m_center =
			( group->m_center * static_cast< float >( group->m_count ) + centers[ i ] ) / static_cast< float >( group->m_count + 1 );
		group->m_alpha = ImMax( group->m_alpha, rows[ i ].m_progress );
		group->m_reach = ImMax( group->m_reach, rows[ i ].m_reach );
		group->m_count++;
	}

	for ( std::size_t i = 1; i < groups.size( ); i++ )
		groups[ i ].m_center = ImMax( groups[ i ].m_center, groups[ i - 1 ].m_center + row_height );

	float limit = list_height - row_height * 0.5f;

	for ( std::size_t i = groups.size( ); i-- > 0; ) {
		groups[ i ].m_center = ImMin( groups[ i ].m_center, limit );
		limit                = groups[ i ].m_center - row_height;
	}

	const float title_width = font->CalcTextSizeA( font_size, FLT_MAX, 0.f, title_text ).x;

	const float width = ImMax( title_width + 40.f, pad_x * 2.f + name_width + link_width + target_width );

	animated_width = snap_width ? width : ImLerp( animated_width, width, ImSaturate( ImGui::GetIO( ).DeltaTime * 12.f ) );
	snap_width     = false;

	const float height = header_height + pad_y * 2.f + list_height;

	int flags = ImGuiWindowFlags_::ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_::ImGuiWindowFlags_NoResize |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse;

	if ( !g_menu.m_opened )
		flags |= ImGuiWindowFlags_::ImGuiWindowFlags_NoMove;

	ImGui::SetNextWindowSize( ImVec2( animated_width, height ), ImGuiCond_::ImGuiCond_Always );
	ImGui::Begin( "botox-spectators-chillware", 0, flags );
	{
		const auto draw_list = ImGui::GetWindowDrawList( );

		const ImVec2 position = ImGui::GetWindowPos( ), size = ImGui::GetWindowSize( );

		const float left = position.x, top = position.y, right = position.x + size.x, bottom = position.y + size.y;

		draw_list->Flags |= ImDrawListFlags_::ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_::ImDrawListFlags_AntiAliasedFill;

		const float saved_tessellation = draw_list->_Data->CurveTessellationTol;

		draw_list->_Data->CurveTessellationTol = 0.35f;

		constexpr float rounding = 12.f;

		constexpr ImU32 panel_color  = IM_COL32( 8, 8, 10, 248 );
		constexpr ImU32 header_color = IM_COL32( 22, 22, 27, 248 );
		constexpr ImU32 border_color = IM_COL32( 255, 255, 255, 16 );
		constexpr ImU32 title_color  = IM_COL32( 196, 196, 206, 255 );
		constexpr ImU32 placeholder_color = IM_COL32( 148, 148, 158, 255 );

		const c_color local_color = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color );
		const c_color other_color = GET_VARIABLE( g_variables.m_spectators_list_text_color_two, c_color );
		constexpr float link_thickness = 2.f;

		const auto with_alpha = []( ImU32 color, float alpha ) -> ImU32 {
			const auto scaled = static_cast< ImU32 >( static_cast< float >( ( color >> IM_COL32_A_SHIFT ) & 0xffu ) * ImSaturate( alpha ) );

			return ( color & ~IM_COL32_A_MASK ) | ( scaled << IM_COL32_A_SHIFT );
		};

		for ( int step = 5; step >= 1; step-- ) {
			const float spread = static_cast< float >( step );

			draw_list->AddRect( ImVec2( left - spread, top - spread ), ImVec2( right + spread, bottom + spread ),
			                    IM_COL32( 0, 0, 0, static_cast< int >( 46.f - spread * 7.f ) ), rounding + spread,
			                    ImDrawFlags_::ImDrawFlags_None, 1.f );
		}

		draw_list->AddRectFilled( ImVec2( left, top ), ImVec2( right, bottom ), panel_color, rounding );
		draw_list->AddRectFilled( ImVec2( left, top ), ImVec2( right, top + header_height ), header_color, rounding,
		                          ImDrawFlags_::ImDrawFlags_RoundCornersTop );

		draw_list->AddRect( ImVec2( left, top ), ImVec2( right, bottom ), border_color, rounding );

		draw_list->AddText( font, font_size, ImVec2( left + ( size.x - title_width ) * 0.5f, top + ( header_height - font_size ) * 0.5f ),
		                    title_color, title_text );

		/* resolved per draw, never cached, see the header comment */
		const auto avatar_at = [ & ]( float x, float y, int index, int team, bool fake_player, float alpha ) {
			IDirect3DTexture9* avatar = nullptr;

			if ( fake_player )
				avatar = team == e_team_id::team_tt   ? g_render.m_terrorist_avatar
				         : team == e_team_id::team_ct ? g_render.m_counter_terrorist_avatar
				                                      : nullptr;
			else if ( index >= 0 && index < 64 )
				avatar = g_avatar_cache[ index ];

			constexpr float avatar_rounding = avatar_size * 0.5f;

			if ( avatar ) {
				draw_list->AddImageRounded( avatar, ImVec2( x, y ), ImVec2( x + avatar_size, y + avatar_size ), ImVec2( 0, 0 ), ImVec2( 1, 1 ),
				                            ImColor( 1.f, 1.f, 1.f, alpha ), avatar_rounding );
				return;
			}

			draw_list->AddRectFilled( ImVec2( x, y ), ImVec2( x + avatar_size, y + avatar_size ),
			                          with_alpha( IM_COL32( 38, 38, 46, 255 ), alpha ), avatar_rounding );

			const float mark_width = font->CalcTextSizeA( font_size, FLT_MAX, 0.f, "?" ).x;

			draw_list->AddText( font, font_size, ImVec2( x + ( avatar_size - mark_width ) * 0.5f, y + ( avatar_size - font_size ) * 0.5f ),
			                    with_alpha( placeholder_color, alpha ), "?" );
		};

		const float rows_top   = top + header_height + pad_y;
		const float name_x     = left + pad_x;
		const float target_end = right - pad_x;

		const float column_start = name_x + name_width + 8.f;
		const float link_end     = ImMax( column_start + 24.f, target_end - target_width - 9.f );

		/* merge a fixed stub back from the target, never stretched with the panel */
		const float junction_x = ImMax( column_start + 10.f, link_end - 26.f );

		const float converge_run = ImMin( 34.f, ( junction_x - column_start ) * 0.9f );

		const float bend_x = junction_x - converge_run;

		const auto draw_reaching = [ & ]( const std::vector< ImVec2 >& points, const float fraction, const ImU32 color ) {
			if ( points.size( ) < 2 || fraction <= 0.001f )
				return;

			if ( fraction >= 0.999f ) {
				draw_list->AddPolyline( points.data( ), static_cast< int >( points.size( ) ), color, ImDrawFlags_::ImDrawFlags_None,
				                        link_thickness );
				return;
			}

			float total = 0.f;

			for ( std::size_t p = 1; p < points.size( ); p++ )
				total += ImSqrt( ImLengthSqr( ImVec2( points[ p ].x - points[ p - 1 ].x, points[ p ].y - points[ p - 1 ].y ) ) );

			float budget = total * fraction;

			std::vector< ImVec2 > drawn{ };
			drawn.reserve( points.size( ) );
			drawn.push_back( points.front( ) );

			for ( std::size_t p = 1; p < points.size( ); p++ ) {
				const ImVec2 delta( points[ p ].x - points[ p - 1 ].x, points[ p ].y - points[ p - 1 ].y );

				const float length = ImSqrt( ImLengthSqr( delta ) );

				if ( length <= 0.f )
					continue;

				if ( budget < length ) {
					const float t = budget / length;

					drawn.push_back( ImVec2( points[ p - 1 ].x + delta.x * t, points[ p - 1 ].y + delta.y * t ) );
					break;
				}

				budget -= length;
				drawn.push_back( points[ p ] );
			}

			if ( drawn.size( ) >= 2 )
				draw_list->AddPolyline( drawn.data( ), static_cast< int >( drawn.size( ) ), color, ImDrawFlags_::ImDrawFlags_None,
				                        link_thickness );
		};

		constexpr float stub_split = 0.72f;

		for ( std::size_t i = 0; i < rows.size( ); i++ ) {
			const auto& row = rows[ i ];

			const float alpha  = ease( row.m_progress );
			const float center = rows_top + centers[ i ];

			const ImU32 row_color = ( row.m_watching_local ? local_color : other_color ).get_u32( alpha );

			const auto group = std::find_if( groups.begin( ), groups.end( ), [ & ]( const chillware_group_t& candidate ) {
				return candidate.m_target_index == row.m_target_index;
			} );

			const float name_end = name_x + avatar_block + font->CalcTextSizeA( font_size, FLT_MAX, 0.f, names[ i ].c_str( ) ).x;

			if ( group != groups.end( ) ) {
				const float group_center = rows_top + group->m_center;

				const float link_start = ImMin( name_end + 8.f, bend_x );

				std::vector< ImVec2 > path{ };
				path.reserve( 28 );
				path.push_back( ImVec2( link_start, center ) );

				if ( std::fabs( group_center - center ) < 0.5f ) {
					path.push_back( ImVec2( junction_x, center ) );
				} else {
					if ( link_start < bend_x - 0.5f )
						path.push_back( ImVec2( bend_x, center ) );

					const float span = converge_run * 0.5f;

					const ImVec2 first( bend_x, center ), first_handle( bend_x + span, center );
					const ImVec2 second_handle( junction_x - span, group_center ), second( junction_x, group_center );

					constexpr int samples = 24;

					for ( int sample = 1; sample <= samples; sample++ )
						path.push_back( ImBezierCubicCalc( first, first_handle, second_handle, second,
						                                   static_cast< float >( sample ) / static_cast< float >( samples ) ) );
				}

				draw_reaching( path, ease( row.m_reach ) / stub_split, row_color );
			}

			if ( show_avatar )
				avatar_at( name_x, center - avatar_size * 0.5f, row.m_index, row.m_team, row.m_fake_player, alpha );

			draw_list->AddText( font, font_size, ImVec2( name_x + avatar_block, center - font_size * 0.5f ), row_color, names[ i ].c_str( ) );
		}

		for ( const auto& group : groups ) {
			const float center = rows_top + group.m_center;

			const float reach = ease( group.m_reach );

			const float stub_fraction = ImSaturate( ( reach - stub_split ) / ( 1.f - stub_split ) );

			const ImU32 line = ( group.m_watching_local ? local_color : other_color ).get_u32( ease( group.m_alpha ) );

			const float text_width = font->CalcTextSizeA( font_size, FLT_MAX, 0.f, group.m_name.c_str( ) ).x;

			const float target_link_end = ImMax( junction_x + 6.f, target_end - avatar_block - text_width - 9.f );

			if ( stub_fraction > 0.f ) {
				draw_reaching( { ImVec2( junction_x, center ), ImVec2( target_link_end, center ) }, stub_fraction, line );

				if ( group.m_count > 1 )
					draw_list->AddCircleFilled( ImVec2( junction_x, center ), link_thickness * 0.5f, line, 16 );
			}

			const float target_alpha = ease( group.m_alpha ) * stub_fraction;

			if ( target_alpha <= 0.f )
				continue;

			const ImU32 target_color = ( group.m_watching_local ? local_color : other_color ).get_u32( target_alpha );

			draw_list->AddText( font, font_size, ImVec2( target_end - avatar_block - text_width, center - font_size * 0.5f ), target_color,
			                    group.m_name.c_str( ) );

			if ( show_avatar )
				avatar_at( target_end - avatar_size, center - avatar_size * 0.5f, group.m_target_index, group.m_target_team, group.m_fake_player,
				           target_alpha );
		}

		draw_list->_Data->CurveTessellationTol = saved_tessellation;
	}
	ImGui::End( );
}

void n_misc::impl_t::draw_spectator_list_chillware_v2( )
{
	const auto title_font = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
	const auto row_font   = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];

	if ( !title_font || !row_font )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true );

	const int local_index = g_ctx.m_local ? g_ctx.m_local->get_index( ) : -1;

	int watched_index = local_index;

	if ( g_ctx.m_local && !g_ctx.m_local->is_alive( ) ) {
		const auto target = reinterpret_cast< c_base_entity* >(
			g_interfaces.m_client_entity_list->get_client_entity_from_handle( g_ctx.m_local->get_observer_target_handle( ) ) );

		watched_index = target ? target->get_index( ) : -1;
	}

	std::erase_if( entries, [ & ]( const spectator_entry_t& entry ) { return entry.m_index == local_index; } );

	if ( entries.empty( ) && !g_menu.m_opened &&
	     !( GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) && g_interfaces.m_engine_client->is_in_game( ) ) )
		return;

	const c_color accent = GET_VARIABLE( g_variables.m_accent, c_color );

	const int accent_r = accent.get< e_color_type::color_type_r >( );
	const int accent_g = accent.get< e_color_type::color_type_g >( );
	const int accent_b = accent.get< e_color_type::color_type_b >( );

	int flags = ImGuiWindowFlags_::ImGuiWindowFlags_NoResize | ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse |
	            ImGuiWindowFlags_::ImGuiWindowFlags_AlwaysAutoResize;

	if ( !g_menu.m_opened )
		flags |= ImGuiWindowFlags_::ImGuiWindowFlags_NoInputs;

	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowPadding, ImVec2( 5.f, 10.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowRounding, 0.f );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowBorderSize, 1.f );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowMinSize, ImVec2( 32.f, 32.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowTitleAlign, ImVec2( 0.f, 0.5f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_FramePadding, ImVec2( 5.f, 1.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_ItemSpacing, ImVec2( 8.f, 5.f ) );

	ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_WindowBg, IM_COL32( 7, 7, 7, 255 ) );
	ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_Border, IM_COL32( accent_r, accent_g, accent_b, 50 ) );
	ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_BorderShadow, IM_COL32( 0, 0, 0, 0 ) );
	ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_TitleBg, IM_COL32( 3, 3, 3, 255 ) );
	ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_TitleBgActive, IM_COL32( 3, 3, 3, 255 ) );
	ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_Text, IM_COL32( 255, 255, 255, 255 ) );

	ImGui::SetNextWindowPos( ImVec2( 15.f, ImGui::GetIO( ).DisplaySize.y * 0.3f ), ImGuiCond_::ImGuiCond_FirstUseEver );
	ImGui::SetNextWindowSizeConstraints( ImVec2( 100.f, 20.f ), ImVec2( 400.f, 800.f ) );

	ImGui::PushFont( title_font );
	ImGui::Begin( "spectator list##botox-spectators-chillware-v2", 0, flags );
	ImGui::PopFont( );
	{
		ImGui::PushFont( row_font );

		for ( const auto& entry : entries ) {
			const char* const mode = entry.m_obs_mode == e_obs_mode::obs_mode_in_eye    ? "fp"
			                         : entry.m_obs_mode == e_obs_mode::obs_mode_chase   ? "tp"
			                         : entry.m_obs_mode == e_obs_mode::obs_mode_roaming ? "roam"
			                                                                            : "?";

			const std::string text = entry.m_name + " -" + mode + "-> " + entry.m_target_name;

			const ImVec4 color = entry.m_target_index == watched_index ? accent.get_vec4( ) : ImColor( 90, 90, 90, 200 ).Value;

			ImGui::SetCursorPosX( ( ImGui::GetWindowSize( ).x - ImGui::CalcTextSize( text.c_str( ) ).x ) * 0.5f );
			ImGui::TextColored( color, "%s", text.c_str( ) );
		}

		ImGui::PopFont( );

		const ImVec2 position = ImGui::GetWindowPos( ), size = ImGui::GetWindowSize( );

		const float left = position.x, top = position.y, right = position.x + size.x, bottom = position.y + size.y;
		const float mid_x = left + size.x * 0.5f, mid_y = top + size.y * 0.5f;

		const ImU32 solid = IM_COL32( accent_r, accent_g, accent_b, 255 ), clear = IM_COL32( accent_r, accent_g, accent_b, 0 );

		const auto draw_list = ImGui::GetWindowDrawList( );

		draw_list->PushClipRectFullScreen( );
		draw_list->AddRectFilledMultiColor( ImVec2( left - 1.f, top - 1.f ), ImVec2( mid_x + 1.f, top ), solid, clear, clear, solid );
		draw_list->AddRectFilledMultiColor( ImVec2( left - 1.f, top ), ImVec2( left, mid_y + 1.f ), solid, solid, clear, clear );
		draw_list->AddRectFilledMultiColor( ImVec2( mid_x, bottom ), ImVec2( right, bottom + 1.f ), clear, solid, solid, clear );
		draw_list->AddRectFilledMultiColor( ImVec2( right, mid_y ), ImVec2( right + 1.f, bottom + 1.f ), clear, clear, solid, solid );
		draw_list->PopClipRect( );
	}
	ImGui::End( );

	ImGui::PopStyleColor( 6 );
	ImGui::PopStyleVar( 7 );
}

static float imgui_text_width( ImFont* font, const char* text )
{
	return IM_FLOOR( font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, text ).x + 0.99999f );
}

void n_misc::impl_t::draw_spectator_list_lobotomy( )
{
	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );

	std::erase_if( entries, []( const spectator_entry_t& entry ) { return !entry.m_watching_local; } );

	if ( entries.empty( ) && !GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) )
		return;

	const auto font = g_render.m_fonts[ e_font_names::font_name_lobotomy_13 ];

	if ( !font )
		return;

	constexpr float window_width = 200.f, title_top_margin = 5.f, title_to_list_spacing = 15.f, item_left_margin = 10.f;
	constexpr float item_height = 17.f, item_spacing = 10.f, bottom_padding = 10.f;

	constexpr float layout_font_size = 11.f;

	constexpr float list_top = title_top_margin + layout_font_size + title_to_list_spacing;

	const float count         = static_cast< float >( entries.size( ) );
	const float window_height = list_top + count * item_height + ImMax( count - 1.f, 0.f ) * item_spacing + bottom_padding;

	const bool draw_avatar = GET_VARIABLE( g_variables.m_spectators_avatar, bool );

	constexpr float avatar_size = 13.f, avatar_gap = 4.f;

	const float avatar_block = draw_avatar ? avatar_size + avatar_gap : 0.f;

	int flags = ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_::ImGuiWindowFlags_NoResize;

	if ( !g_menu.m_opened )
		flags |= ImGuiWindowFlags_::ImGuiWindowFlags_NoMove;

	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowRounding, 5.f );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowPadding, ImVec2( 0.f, 0.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowBorderSize, 0.f );
	ImGui::PushStyleColor( ImGuiCol_::ImGuiCol_WindowBg, IM_COL32( 13, 13, 13, 255 ) );

	ImGui::SetNextWindowSize( ImVec2( window_width, window_height ), ImGuiCond_::ImGuiCond_Always );
	ImGui::Begin( "botox-spectators-lobotomy", 0, flags );
	{
		const auto draw_list  = ImGui::GetWindowDrawList( );
		const ImVec2 position = ImGui::GetWindowPos( );

		const float font_size = font->FontSize;

		constexpr auto title_text = "Spectators";

		draw_list->AddText( font, font_size,
		                    ImVec2( position.x + ( window_width - imgui_text_width( font, title_text ) ) * 0.5f, position.y + title_top_margin ),
		                    IM_COL32( 255, 255, 255, 255 ), title_text );

		const ImU32 text_color = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color ).get_u32( );

		for ( std::size_t i = 0; i < entries.size( ); i++ ) {
			const auto& entry = entries[ i ];

			const float row_x = position.x + item_left_margin;
			const float row_y = position.y + list_top + static_cast< float >( i ) * ( item_height + item_spacing );

			if ( draw_avatar ) {
				if ( const auto avatar = spectator_avatar_texture( entry.m_index, entry.m_team, entry.m_fake_player ) )
					draw_list->AddImage( avatar, ImVec2( row_x, row_y ), ImVec2( row_x + avatar_size, row_y + avatar_size ) );
			}

			draw_list->AddText( font, font_size, ImVec2( row_x + avatar_block, row_y ), text_color, utf8_truncate( entry.m_name, 24 ).c_str( ) );
		}
	}
	ImGui::End( );

	ImGui::PopStyleColor( );
	ImGui::PopStyleVar( 3 );
}

void n_misc::impl_t::draw_spectator_list_clarity_v2( )
{
	const auto title_font = g_render.m_fonts[ e_font_names::font_name_clarity_inter_bold_15 ];
	const auto font       = g_render.m_fonts[ e_font_names::font_name_clarity_inter_medium_15 ];

	if ( !title_font || !font )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );

	static float alpha = 0.f;
	{
		const float target = g_menu.m_opened || !entries.empty( ) || GET_VARIABLE( g_variables.m_spectators_always_show, bool ) ? 1.f : 0.f;
		if ( alpha != target )
			alpha = std::lerp( alpha, target, std::clamp( ImGui::GetIO( ).DeltaTime * 15.f, 0.f, 1.f ) );
		if ( alpha > 0.99f )
			alpha = 1.f;
		else if ( alpha <= 0.01f )
			alpha = 0.f;
	}
	if ( alpha == 0.f )
		return;

	const auto text_size = [ ]( const ImFont* f, const std::string& text ) {
		const ImVec2 size = f->CalcTextSizeA( f->FontSize, FLT_MAX, 0.f, text.c_str( ) );
		return std::pair{ static_cast< int >( size.x ), static_cast< int >( size.y ) };
	};

	const auto fit = [ & ]( std::string& text, const int budget, const std::size_t keep ) {
		if ( text_size( font, text ).first <= budget )
			return true;

		const int ellipsis = text_size( font, "..." ).first;

		std::vector< std::size_t > starts{ };
		for ( std::size_t i = 0; i < text.size( ); ++i )
			if ( ( static_cast< unsigned char >( text[ i ] ) & 0xc0 ) != 0x80 )
				starts.push_back( i );

		for ( std::size_t chars = starts.size( ); chars > keep; ) {
			text.resize( starts[ --chars ] );
			if ( text_size( font, text ).first + ellipsis <= budget ) {
				text += "...";
				return true;
			}
		}

		text += "...";
		return false;
	};

	constexpr int window_w = 210, pad_x = 12, pad_y = 7, avatar = 20;
	constexpr int row_max  = window_w - pad_x * 2;

	const bool all_players  = GET_VARIABLE( g_variables.m_spectators_list_type, int ) == 0;
	const bool draw_avatars = GET_VARIABLE( g_variables.m_spectators_avatar, bool );

	struct row_t {
		std::string m_text;
		IDirect3DTexture9* m_avatar;
		ImU32 m_color;
		int m_height;
	};

	std::vector< row_t > rows{ };

	for ( const auto& entry : entries ) {
		IDirect3DTexture9* const texture = draw_avatars && !entry.m_fake_player ? g_avatar_cache[ entry.m_index ] : nullptr;

		const int budget = row_max - ( texture ? avatar + 5 : 0 );

		std::string name = entry.m_name + ( entry.m_fake_player ? " [bot]" : "" );

		if ( all_players ) {
			std::string target = entry.m_target_name + ( entry.m_target_fake_player ? " [bot]" : "" );

			const int avail = budget - text_size( font, " -> " ).first;
			bool ok         = fit( target, avail - text_size( font, name ).first, 5 );
			if ( !ok )
				ok = fit( name, avail - text_size( font, target ).first, 5 );

			name = name + " -> " + target;
			if ( !ok )
				fit( name, budget, 0 );
		} else
			fit( name, budget, 0 );

		const bool dim    = all_players && !entry.m_watching_local;
		const ImU32 color = dim ? IM_COL32( 255, 255, 255, static_cast< int >( 255.f * alpha * 0.5f ) )
		                        : GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color ).get_u32( alpha );

		rows.push_back( { name, texture, color, texture ? ( std::max )( static_cast< int >( font->FontSize ), avatar ) : static_cast< int >( font->FontSize ) } );
	}

	std::string empty_text = "no spectators";
	if ( rows.empty( ) ) {
		fit( empty_text, row_max, 0 );
		rows.push_back( { empty_text, nullptr, IM_COL32( 255, 255, 255, static_cast< int >( 100.f * alpha ) ), text_size( font, empty_text ).second } );
	}

	int window_h = pad_y + text_size( title_font, "spectators" ).second + 5;
	for ( const auto& row : rows )
		window_h += row.m_height + 3;
	window_h += pad_y;

	int flags = ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_::ImGuiWindowFlags_NoResize;

	if ( !g_menu.m_opened )
		flags |= ImGuiWindowFlags_::ImGuiWindowFlags_NoMove;

	ImGui::SetNextWindowBgAlpha( 0.f );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowPadding, ImVec2( 0.f, 0.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowBorderSize, 0.f );

	ImGui::SetNextWindowSize( ImVec2( static_cast< float >( window_w ), static_cast< float >( window_h ) ), ImGuiCond_::ImGuiCond_Always );
	ImGui::Begin( "botox-spectators-clarity-v2", 0, flags );
	{
		const ImGuiContext& context = *ImGui::GetCurrentContext( );
		const ImGuiWindow* window   = ImGui::GetCurrentWindow( );

		const bool dragging =
			( context.MovingWindow && context.MovingWindow->RootWindow == window->RootWindow ) || context.ActiveId == window->MoveId;
		const bool hovered = g_menu.m_opened && ( dragging || ImGui::IsWindowHovered( ) );

		const auto draw_list = ImGui::GetWindowDrawList( );
		const int x = static_cast< int >( ImGui::GetWindowPos( ).x ), y = static_cast< int >( ImGui::GetWindowPos( ).y );

		const auto faded = [ & ]( const int r, const int g, const int b, const int a ) { return IM_COL32( r, g, b, static_cast< int >( a * alpha ) ); };
		const auto rect  = [ & ]( const int grow, const int extra = 0 ) {
			return std::pair{ ImVec2( static_cast< float >( x - grow ), static_cast< float >( y - grow ) ),
				              ImVec2( static_cast< float >( x + window_w + grow + extra ), static_cast< float >( y + window_h + grow + extra ) ) };
		};

		draw_list->PushClipRectFullScreen( );

		const auto [ fill_min, fill_max ] = rect( 3 );
		draw_list->AddRectFilled( fill_min, fill_max, faded( 7, 7, 7, 255 ), 5.f );

		const ImU32 rings[ ] = { faded( 0, 0, 0, 255 ), faded( 50, 50, 50, 255 ), faded( 0, 0, 0, 255 ) };
		for ( int grow = 0; grow < 3; ++grow ) {
			const auto [ ring_min, ring_max ] = rect( grow );
			draw_list->AddRect( ring_min, ring_max, rings[ grow ], 5.f );
		}

		if ( hovered ) {
			const auto [ hover_min, hover_max ] = rect( 0, 1 );
			draw_list->AddRect( hover_min, hover_max, faded( 255, 255, 255, dragging ? 200 : 100 ), 5.f );
		}

		constexpr auto title_text = "spectators";
		const auto [ title_w, title_h ] = text_size( title_font, title_text );

		draw_list->AddText( title_font, title_font->FontSize,
		                    ImVec2( static_cast< float >( x + window_w / 2 - title_w / 2 ), static_cast< float >( y + pad_y ) ),
		                    faded( 255, 255, 255, 255 ), title_text );

		int row_y = y + pad_y + title_h + 5;
		for ( const auto& row : rows ) {
			int row_x = x + pad_x;

			if ( row.m_avatar ) {
				draw_list->AddImageRounded( row.m_avatar, ImVec2( static_cast< float >( row_x ), static_cast< float >( row_y ) ),
				                            ImVec2( static_cast< float >( row_x + avatar ), static_cast< float >( row_y + avatar ) ), ImVec2( 0.f, 0.f ),
				                            ImVec2( 1.f, 1.f ), faded( 255, 255, 255, 255 ), avatar * 0.5f );
				row_x += avatar + 5;
			}

			const int text_h = text_size( font, row.m_text ).second;
			draw_list->AddText( font, font->FontSize, ImVec2( static_cast< float >( row_x ), static_cast< float >( row_y + row.m_height / 2 - text_h / 2 ) ),
			                    row.m_color, row.m_text.c_str( ) );

			row_y += row.m_height + 3;
		}

		draw_list->PopClipRect( );
	}
	ImGui::End( );

	ImGui::PopStyleVar( 2 );
}

namespace
{
	struct spectator_gif_t {
		const unsigned char* m_data = nullptr;
		UINT m_size                 = 0;
		IStream* m_stream            = nullptr;
		Gdiplus::Bitmap* m_bitmap    = nullptr;
		std::vector< UINT > m_delays = { };
		UINT m_frame                 = 0;
		UINT m_uploaded              = ~0u;
		DWORD m_last_tick            = 0;
		IDirect3DTexture9* m_texture = nullptr;
		bool m_dynamic               = false;
		bool m_failed = false;
	};

	ULONG_PTR gdiplus_token = 0;

	spectator_gif_t spectator_gifs[ 2 ] = { { kamibebra_gif_original, sizeof( kamibebra_gif_original ) },
		                                    { kamibebra_gif_shit, sizeof( kamibebra_gif_shit ) } };

	spectator_gif_t airflow_icons[ 2 ] = { { airflow_spectators_icon, sizeof( airflow_spectators_icon ) },
		                                   { airflow_logo, sizeof( airflow_logo ) } };

	bool load_spectator_gif( spectator_gif_t& gif )
	{
		if ( gif.m_failed )
			return false;

		if ( gif.m_bitmap )
			return true;

		/* one shot: a gif that will not decode must not re-run gdi+ every frame */
		gif.m_failed = true;

		if ( !gdiplus_token ) {
			Gdiplus::GdiplusStartupInput input{ };

			if ( Gdiplus::GdiplusStartup( &gdiplus_token, &input, nullptr ) != Gdiplus::Ok ) {
				gdiplus_token = 0;
				return false;
			}
		}

		const HGLOBAL memory = GlobalAlloc( GMEM_MOVEABLE, gif.m_size );

		if ( !memory )
			return false;

		void* const locked = GlobalLock( memory );

		if ( !locked ) {
			GlobalFree( memory );
			return false;
		}

		std::memcpy( locked, gif.m_data, gif.m_size );
		GlobalUnlock( memory );

		if ( FAILED( CreateStreamOnHGlobal( memory, TRUE, &gif.m_stream ) ) ) {
			GlobalFree( memory );
			gif.m_stream = nullptr;
			return false;
		}

		gif.m_bitmap = Gdiplus::Bitmap::FromStream( gif.m_stream );

		const UINT frames = gif.m_bitmap && gif.m_bitmap->GetLastStatus( ) == Gdiplus::Ok
		                        ? ImMax( gif.m_bitmap->GetFrameCount( &Gdiplus::FrameDimensionTime ), 1u )
		                        : 0;

		if ( !frames ) {
			delete gif.m_bitmap;
			gif.m_bitmap = nullptr;

			gif.m_stream->Release( );
			gif.m_stream = nullptr;

			return false;
		}

		gif.m_delays.assign( frames, 100 );

		if ( const UINT property_size = gif.m_bitmap->GetPropertyItemSize( PropertyTagFrameDelay ); property_size ) {
			std::vector< unsigned char > property( property_size );

			const auto item = reinterpret_cast< Gdiplus::PropertyItem* >( property.data( ) );

			if ( gif.m_bitmap->GetPropertyItem( PropertyTagFrameDelay, property_size, item ) == Gdiplus::Ok && item->value ) {
				const UINT count = ImMin( frames, static_cast< UINT >( item->length / sizeof( UINT ) ) );

				for ( UINT i = 0; i < count; i++ ) {
					const UINT delay = static_cast< const UINT* >( item->value )[ i ] * 10;

					gif.m_delays[ i ] = delay ? delay : 100;
				}
			}
		}

		gif.m_frame     = 0;
		gif.m_uploaded  = ~0u;
		gif.m_last_tick = GetTickCount( );
		gif.m_failed    = false;

		return true;
	}

	IDirect3DTexture9* spectator_gif_frame( spectator_gif_t& gif )
	{
		if ( !g_interfaces.m_direct_device || !load_spectator_gif( gif ) )
			return nullptr;

		const UINT frames = static_cast< UINT >( gif.m_delays.size( ) );

		if ( frames > 1 ) {
			const DWORD now = GetTickCount( );

			if ( now - gif.m_last_tick >= gif.m_delays[ gif.m_frame ] ) {
				gif.m_frame = ( gif.m_frame + 1 ) % frames;
				gif.m_bitmap->SelectActiveFrame( &Gdiplus::FrameDimensionTime, gif.m_frame );
				gif.m_last_tick = now;
			}
		}

		const UINT width = gif.m_bitmap->GetWidth( ), height = gif.m_bitmap->GetHeight( );

		if ( !gif.m_texture ) {
			const auto device = g_interfaces.m_direct_device;

			gif.m_dynamic = false;

			HRESULT result = width && height ? device->CreateTexture( width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &gif.m_texture, nullptr )
			                                 : E_FAIL;

			if ( width && height && ( FAILED( result ) || !gif.m_texture ) ) {
				gif.m_dynamic = true;
				result = device->CreateTexture( width, height, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &gif.m_texture, nullptr );
			}

			if ( FAILED( result ) || !gif.m_texture ) {
				gif.m_texture = nullptr;
				gif.m_failed  = true;

				return nullptr;
			}

			gif.m_uploaded = ~0u;
		}

		if ( gif.m_uploaded != gif.m_frame ) {
			Gdiplus::Rect rect( 0, 0, static_cast< INT >( width ), static_cast< INT >( height ) );
			Gdiplus::BitmapData bits{ };

			if ( gif.m_bitmap->LockBits( &rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bits ) == Gdiplus::Ok ) {
				D3DLOCKED_RECT locked{ };

				if ( SUCCEEDED( gif.m_texture->LockRect( 0, &locked, nullptr, gif.m_dynamic ? D3DLOCK_DISCARD : 0 ) ) ) {
					if ( locked.pBits ) {
						for ( INT y = 0; y < static_cast< INT >( height ); y++ )
							std::memcpy( static_cast< BYTE* >( locked.pBits ) + y * locked.Pitch,
							             static_cast< const BYTE* >( bits.Scan0 ) + y * bits.Stride, width * 4 );

						gif.m_uploaded = gif.m_frame;
					}

					gif.m_texture->UnlockRect( 0 );
				}

				gif.m_bitmap->UnlockBits( &bits );
			}
		}

		return gif.m_texture;
	}

	void release_gif( spectator_gif_t& gif, const bool textures_only )
	{
		if ( gif.m_texture ) {
			gif.m_texture->Release( );
			gif.m_texture = nullptr;
		}

		gif.m_uploaded = ~0u;
		gif.m_failed   = false;

		if ( textures_only )
			return;

		if ( gif.m_bitmap ) {
			delete gif.m_bitmap;
			gif.m_bitmap = nullptr;
		}

		if ( gif.m_stream ) {
			gif.m_stream->Release( );
			gif.m_stream = nullptr;
		}

		gif.m_delays.clear( );
		gif.m_frame = 0;
	}

	struct file_image_t {
		std::vector< unsigned char > m_bytes{ };
		std::string m_path{ };
		std::string m_setting{ };
		spectator_gif_t m_gif{ };
	};

	file_image_t lumi_mascot{ };
	file_image_t kamibebra_custom_gif{ };
	file_image_t watermark_custom_gif{ };

	std::mutex url_image_mutex;
	std::unordered_set< std::string > url_image_started;

	/* imgur page / .gifv / no extension links -> the raw file on i.imgur.com */
	std::string url_image_direct( std::string url )
	{
		for ( const char* page : { "://imgur.com/", "://www.imgur.com/", "://m.imgur.com/" } ) {
			if ( const auto at = url.find( page ); at != std::string::npos ) {
				url.replace( at, strlen( page ), "://i.imgur.com/" );
				break;
			}
		}

		if ( url.find( "://i.imgur.com/" ) == std::string::npos )
			return url;

		if ( const auto query = url.find_first_of( "?#" ); query != std::string::npos )
			url.erase( query );

		if ( url.ends_with( ".gifv" ) )
			url.pop_back( );

		const auto dot = url.find_last_of( '.' );
		if ( dot == std::string::npos || dot < url.find_last_of( '/' ) )
			url += ".gif";

		return url;
	}

	/* local path -> as is. http(s) -> C:\botox\images\<hash> once downloaded off the render thread, "" until then */
	std::string url_image_path( const std::string& setting )
	{
		if ( !setting.starts_with( "http://" ) && !setting.starts_with( "https://" ) )
			return setting;

		const std::string url = url_image_direct( setting );

		uint32_t hash = 0x811C9DC5u;
		for ( const char c : url )
			hash = ( hash ^ static_cast< uint8_t >( c ) ) * 0x01000193u;

		const std::string local = std::format( "C:\\botox\\images\\{:08x}", hash );

		const DWORD attributes = GetFileAttributesA( local.c_str( ) );
		if ( attributes != INVALID_FILE_ATTRIBUTES && !( attributes & FILE_ATTRIBUTE_DIRECTORY ) )
			return local;

		std::lock_guard< std::mutex > lock( url_image_mutex );
		if ( url_image_started.insert( url ).second ) {
			g_url_image_downloads++; // before the thread runs: an eject right now must still wait for it
			std::thread( [ url, local ] {
				CreateDirectoryA( "C:\\botox", nullptr );
				CreateDirectoryA( "C:\\botox\\images", nullptr );

				n_image_cache::download_file( url, local );
				g_url_image_downloads--;
			} ).detach( );
		}

		return { };
	}

	IDirect3DTexture9* file_image_frame( file_image_t& image, const std::string& setting )
	{
		/* re-resolve only on a new setting or while a download is pending */
		const std::string path = setting == image.m_setting && !image.m_path.empty( ) ? image.m_path : url_image_path( setting );
		image.m_setting        = setting;

		if ( path != image.m_path ) {
			release_gif( image.m_gif, false );
			image.m_path = path;
			image.m_bytes.clear( );

			if ( std::ifstream file{ path, std::ios::binary } )
				image.m_bytes.assign( std::istreambuf_iterator< char >( file ), std::istreambuf_iterator< char >( ) );

			image.m_gif.m_data = image.m_bytes.data( );
			image.m_gif.m_size = static_cast< UINT >( image.m_bytes.size( ) );
		}

		if ( image.m_bytes.empty( ) )
			return nullptr;

		return spectator_gif_frame( image.m_gif );
	}

	void release_file_image( file_image_t& image, const bool textures_only )
	{
		release_gif( image.m_gif, textures_only );

		if ( !textures_only ) {
			image.m_path.clear( );
			image.m_bytes.clear( );
		}
	}
}

void n_misc::impl_t::draw_spectator_list_kamibebra( )
{
	struct kamibebra_row_t {
		std::string m_key         = { };
		spectator_entry_t m_entry = { };
		float m_progress          = 0.f;
		bool m_seen               = false;
		bool m_removing           = false;
	};

	static std::vector< kamibebra_row_t > rows{ };

	if ( !g_interfaces.m_engine_client->is_in_game( ) ) {
		rows.clear( );
		return;
	}

	if ( !g_ctx.m_local || g_ctx.m_width <= 0.f || g_ctx.m_height <= 0.f )
		return;

	const auto regular_font = g_render.m_fonts[ e_font_names::font_name_kamibebra_13 ];
	const auto bold_font    = g_render.m_fonts[ e_font_names::font_name_kamibebra_bold_13 ];

	if ( !regular_font || !bold_font )
		return;

	const bool show_all    = GET_VARIABLE( g_variables.m_spectators_list_type, int ) == 0;
	const bool animate     = GET_VARIABLE( g_variables.m_spectators_kamibebra_animate, bool );
	const bool always_show = GET_VARIABLE( g_variables.m_spectators_kamibebra_always_show, bool );
	const bool bold        = GET_VARIABLE( g_variables.m_spectators_kamibebra_bold, bool );

	std::vector< spectator_entry_t > spectators{ };
	collect_spectators( spectators );

	if ( !GET_VARIABLE( g_variables.m_spectators_kamibebra_dynamic, bool ) ) {
		for ( auto& spectator : spectators ) {
			const std::string cut = utf8_truncate( spectator.m_target_name, 9 );

			if ( cut.size( ) < spectator.m_target_name.size( ) )
				spectator.m_target_name = cut + "...";
		}
	}

	for ( auto& row : rows )
		row.m_seen = false;

	for ( const auto& spectator : spectators ) {
		const std::string key = spectator.m_name + "\n" + spectator.m_target_name;

		const auto row = std::find_if( rows.begin( ), rows.end( ), [ & ]( const kamibebra_row_t& candidate ) { return candidate.m_key == key; } );

		if ( row == rows.end( ) ) {
			rows.push_back( { key, spectator, animate ? 0.f : 1.f, true, false } );
			continue;
		}

		row->m_entry    = spectator;
		row->m_seen     = true;
		row->m_removing = false;

		if ( !animate )
			row->m_progress = 1.f;
	}

	for ( auto& row : rows )
		if ( !row.m_seen )
			row.m_removing = true;

	if ( animate ) {
		const float step = ImGui::GetIO( ).DeltaTime * 4.f;

		for ( auto& row : rows )
			row.m_progress = std::clamp( row.m_progress + ( row.m_removing ? -step : step ), 0.f, 1.f );
	}

	std::erase_if( rows, [ & ]( const kamibebra_row_t& row ) { return animate ? row.m_removing && row.m_progress <= 0.f : !row.m_seen; } );

	const bool has_spectators = !spectators.empty( );
	const bool has_rows       = animate ? !rows.empty( ) : has_spectators;

	if ( !has_rows && !always_show )
		return;

	std::vector< std::pair< const spectator_entry_t*, float > > render{ };

	if ( animate ) {
		for ( const auto& row : rows )
			render.emplace_back( &row.m_entry, row.m_progress );
	} else {
		for ( const auto& spectator : spectators )
			render.emplace_back( &spectator, 1.f );
	}

	constexpr float title_top_margin = 4.f, title_to_list_spacing = 8.f, item_left_margin = 10.f;
	constexpr float item_height = 17.f, item_spacing = 1.f, bottom_padding = 5.f;

	constexpr float layout_font_size = 12.f;

	constexpr float list_top = title_top_margin + layout_font_size + title_to_list_spacing;

	const bool draw_avatar = GET_VARIABLE( g_variables.m_spectators_avatar, bool );

	constexpr float avatar_size = 13.f, avatar_gap = 4.f;

	const float avatar_block = draw_avatar ? avatar_size + avatar_gap : 0.f;

	float max_text_width = imgui_text_width( regular_font, "Spectators" );

	for ( const auto& [ entry, progress ] : render )
		max_text_width =
			ImMax( max_text_width, avatar_block + imgui_text_width( regular_font, ( entry->m_name + "   >   " + entry->m_target_name ).c_str( ) ) );

	const float window_width = ImMax( max_text_width + item_left_margin * 2.f + 10.f, 200.f );

	float list_height = 0.f;

	if ( has_rows && animate ) {
		for ( std::size_t i = 0; i < render.size( ); i++ )
			list_height += item_height * render[ i ].second + ( i + 1 < render.size( ) ? item_spacing : 0.f );
	} else {
		const float items = has_rows ? static_cast< float >( render.size( ) ) : 1.f;

		list_height = items * item_height + ( items > 1.f ? ( items - 1.f ) * item_spacing : 0.f );
	}

	const float window_height = list_top + list_height + bottom_padding;

	int flags = ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_::ImGuiWindowFlags_NoResize |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoBackground;

	if ( !g_menu.m_opened )
		flags |= ImGuiWindowFlags_::ImGuiWindowFlags_NoMove;

	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowRounding, 5.f );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowPadding, ImVec2( 0.f, 0.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowBorderSize, 0.f );

	ImGui::SetNextWindowPos( ImVec2( 10.f, 10.f ), ImGuiCond_::ImGuiCond_FirstUseEver );
	ImGui::SetNextWindowSize( ImVec2( window_width, window_height ), ImGuiCond_::ImGuiCond_Always );
	ImGui::Begin( "botox-spectators-kamibebra", 0, flags );
	{
		const auto draw_list  = ImGui::GetWindowDrawList( );
		const ImVec2 position = ImGui::GetWindowPos( ), size = ImGui::GetWindowSize( );

		draw_list->PushClipRectFullScreen( );

		if ( GET_VARIABLE( g_variables.m_spectators_kamibebra_window_shadow, bool ) )
			draw_list->AddRectFilled( ImVec2( position.x + 3.f, position.y + 3.f ), ImVec2( position.x + window_width + 3.f, position.y + window_height + 3.f ),
			                          IM_COL32( 0, 0, 0, 128 ), 5.f, ImDrawFlags_::ImDrawFlags_RoundCornersAll );

		draw_list->AddRectFilled( position, ImVec2( position.x + size.x, position.y + size.y ), IM_COL32( 13, 13, 13, 255 ), 5.f );

		draw_list->PopClipRect( );

		const auto font       = bold ? bold_font : regular_font;
		const float font_size = font->FontSize;

		std::string title = bold ? "spectators" : "Spectators";

		if ( GET_VARIABLE( g_variables.m_spectators_kamibebra_custom_title, bool ) &&
		     !GET_VARIABLE( g_variables.m_spectators_kamibebra_title_text, std::string ).empty( ) )
			title = GET_VARIABLE( g_variables.m_spectators_kamibebra_title_text, std::string );

		draw_list->AddText( font, font_size,
		                    ImVec2( position.x + ( window_width - imgui_text_width( font, title.c_str( ) ) ) * 0.5f, position.y + title_top_margin ),
		                    IM_COL32( 255, 255, 255, 255 ), title.c_str( ) );

		if ( !has_spectators && always_show ) {
			draw_list->AddText( font, font_size, ImVec2( position.x + item_left_margin, position.y + list_top ), IM_COL32( 100, 100, 100, 255 ),
			                    bold ? "no spectators..." : "No spectators..." );
		} else {
			const ImU32 local_color        = GET_VARIABLE( g_variables.m_spectators_kamibebra_local_color, c_color ).get_u32( );
			const ImU32 other_color        = GET_VARIABLE( g_variables.m_spectators_kamibebra_other_color, c_color ).get_u32( );
			const ImU32 local_shadow_color = GET_VARIABLE( g_variables.m_spectators_kamibebra_local_shadow_color, c_color ).get_u32( );
			const ImU32 other_shadow_color = GET_VARIABLE( g_variables.m_spectators_kamibebra_other_shadow_color, c_color ).get_u32( );

			const bool local_shadow = GET_VARIABLE( g_variables.m_spectators_kamibebra_local_shadow, bool );
			const bool other_shadow = GET_VARIABLE( g_variables.m_spectators_kamibebra_other_shadow, bool );

			float row_y = position.y + list_top;

			for ( const auto& [ entry, progress ] : render ) {
				const float row_height = item_height * progress;

				if ( progress <= 0.f ) {
					row_y += row_height + item_spacing;
					continue;
				}

				const std::string text = show_all ? entry->m_name + "   >   " + entry->m_target_name : entry->m_name;

				const float row_x = position.x + item_left_margin;

				/* the reveal: a growing row uncovers the name top down, it never fades */
				draw_list->PushClipRect( ImVec2( row_x, row_y ), ImVec2( position.x + window_width - item_left_margin, row_y + row_height ), true );

				if ( draw_avatar ) {
					if ( const auto avatar = spectator_avatar_texture( entry->m_index, entry->m_team, entry->m_fake_player ) )
						draw_list->AddImage( avatar, ImVec2( row_x, row_y ), ImVec2( row_x + avatar_size, row_y + avatar_size ) );
				}

				const ImVec2 text_position( row_x + avatar_block, row_y );

				if ( entry->m_watching_local ? local_shadow : other_shadow )
					draw_list->AddText( font, font_size, ImVec2( text_position.x + 1.f, text_position.y + 1.f ),
					                    entry->m_watching_local ? local_shadow_color : other_shadow_color, text.c_str( ) );

				draw_list->AddText( font, font_size, text_position, entry->m_watching_local ? local_color : other_color, text.c_str( ) );

				draw_list->PopClipRect( );

				row_y += row_height + item_spacing;
			}
		}

		if ( GET_VARIABLE( g_variables.m_spectators_kamibebra_gif, bool ) ) {
			const float gif_size = GET_VARIABLE( g_variables.m_spectators_kamibebra_gif_size, float );

			const ImVec2 gif_min( position.x + ( size.x - gif_size ) * 0.5f, position.y - gif_size );
			const ImVec2 gif_max( gif_min.x + gif_size, gif_min.y + gif_size );

			const int gif_type = std::clamp( GET_VARIABLE( g_variables.m_spectators_kamibebra_gif_type, int ), 0, 2 );
			const auto frame   = gif_type == 2 ? file_image_frame( kamibebra_custom_gif, GET_VARIABLE( g_variables.m_spectators_kamibebra_gif_path, std::string ) )
			                                   : spectator_gif_frame( spectator_gifs[ gif_type ] );

			draw_list->PushClipRectFullScreen( );

			if ( frame )
				draw_list->AddImage( frame, gif_min, gif_max );
			else
				draw_list->AddRectFilled( gif_min, gif_max, IM_COL32( 80, 80, 80, 120 ), 4.f );

			draw_list->PopClipRect( );
		}
	}
	ImGui::End( );

	ImGui::PopStyleVar( 3 );
}

namespace
{
	constexpr float k_eye_stroke = 1.5f;

	constexpr ImVec2 k_eye_lens_start       = { 3.2749f, 15.2957f };
	constexpr ImVec2 k_eye_lens[ 8 ][ 3 ] = {
		{ { 2.4250f, 14.1915f }, { 2.0000f, 13.6394f }, { 2.0000f, 12.0000f } },
		{ { 2.0000f, 10.3606f }, { 2.4250f, 9.8085f }, { 3.2749f, 8.7043f } },
		{ { 4.9720f, 6.4996f }, { 7.8181f, 4.0000f }, { 12.0000f, 4.0000f } },
		{ { 16.1819f, 4.0000f }, { 19.0280f, 6.4996f }, { 20.7251f, 8.7043f } },
		{ { 21.5750f, 9.8085f }, { 22.0000f, 10.3606f }, { 22.0000f, 12.0000f } },
		{ { 22.0000f, 13.6394f }, { 21.5750f, 14.1915f }, { 20.7251f, 15.2957f } },
		{ { 19.0280f, 17.5004f }, { 16.1819f, 20.0000f }, { 12.0000f, 20.0000f } },
		{ { 7.8181f, 20.0000f }, { 4.9720f, 17.5004f }, { 3.2749f, 15.2957f } },
	};
	constexpr ImVec2 k_eye_iris_center = { 12.f, 12.f };
	constexpr float k_eye_iris_radius  = 3.f;

	constexpr ImVec2 k_eye_lid[ 33 ] = {
		{ 1.9999f, 7.0002f },   { 2.3629f, 7.7191f },   { 2.7732f, 8.4122f },   { 3.2219f, 9.0812f },   { 3.7069f, 9.7243f },
		{ 4.2275f, 10.3388f },  { 4.7838f, 10.9213f },  { 5.3757f, 11.4676f },  { 6.0029f, 11.9729f },  { 6.6645f, 12.4322f },
		{ 7.3588f, 12.8403f },  { 8.0833f, 13.1922f },  { 8.8341f, 13.4835f },  { 9.6066f, 13.7110f },  { 10.3955f, 13.8728f },
		{ 11.1952f, 13.9688f }, { 11.9999f, 14.0002f }, { 12.8047f, 13.9688f }, { 13.6043f, 13.8729f }, { 14.3932f, 13.7110f },
		{ 15.1658f, 13.4835f }, { 15.9166f, 13.1922f }, { 16.6410f, 12.8403f }, { 17.3353f, 12.4322f }, { 17.9969f, 11.9729f },
		{ 18.6241f, 11.4675f }, { 19.2160f, 10.9213f }, { 19.7723f, 10.3388f }, { 20.2930f, 9.7243f },  { 20.7779f, 9.0812f },
		{ 21.2266f, 8.4122f },  { 21.6369f, 7.7191f },  { 21.9999f, 7.0002f },
	};
	constexpr ImVec2 k_eye_lashes[ 5 ][ 2 ] = {
		{ { 18.9955f, 11.1248f }, { 20.4999f, 12.6290f } }, { { 15.5825f, 13.3218f }, { 16.9999f, 15.5002f } },
		{ { 11.9999f, 14.0002f }, { 11.9999f, 16.5002f } }, { { 8.4870f, 13.3488f }, { 7.0871f, 15.5003f } },
		{ { 5.0496f, 11.1666f }, { 3.5871f, 12.6291f } },
	};

	constexpr float k_eye_lid_drop = 5.f;

	constexpr float k_eye_halo_alpha = 0.4f;

	constexpr int k_eye_lens_steps = 16;

	struct eye_row_layout_t {
		int m_face = 0, m_pad = 0, m_width = 0, m_height = 0, m_pitch = 0;
		float m_radius = 0.f, m_icon = 0.f, m_stroke = 0.f, m_gap = 0.f;
	};

	eye_row_layout_t eye_row_layout( const int face )
	{
		eye_row_layout_t layout{ };

		layout.m_face   = face;
		layout.m_radius = face * 0.5f;
		layout.m_icon   = face * 0.8f;
		layout.m_stroke = k_eye_stroke * layout.m_icon / 24.f;
		layout.m_gap    = face * 0.09f;
		layout.m_pad    = static_cast< int >( std::ceil( layout.m_stroke * 0.5f + 2.f ) );
		layout.m_width  = static_cast< int >( std::ceil( layout.m_pad * 2.f + face + layout.m_gap + layout.m_icon ) );
		layout.m_height = face + layout.m_pad * 2;
		layout.m_pitch  = static_cast< int >( std::floor( face * 1.25f + 0.5f ) );

		return layout;
	}

	struct eye_row_texture_t {
		IDirect3DTexture9* m_texture = nullptr;
		int m_face                   = 0;
		bool m_failed = false;
	};

	eye_row_texture_t eye_row_textures[ 2 ] = { };

	float segment_distance( const ImVec2& p, const ImVec2& a, const ImVec2& b )
	{
		const ImVec2 ab( b.x - a.x, b.y - a.y ), ap( p.x - a.x, p.y - a.y );

		const float length = ImLengthSqr( ab );
		const float t      = length > 0.f ? ImSaturate( ( ap.x * ab.x + ap.y * ab.y ) / length ) : 0.f;

		return std::sqrt( ImLengthSqr( ImVec2( ap.x - ab.x * t, ap.y - ab.y * t ) ) );
	}

	IDirect3DTexture9* eye_row_texture( const bool open, const eye_row_layout_t& layout )
	{
		auto& row = eye_row_textures[ open ? 1 : 0 ];

		if ( row.m_face == layout.m_face && ( row.m_texture || row.m_failed ) )
			return row.m_texture;

		const auto device = g_interfaces.m_direct_device;

		if ( !device )
			return nullptr;

		if ( row.m_texture ) {
			row.m_texture->Release( );
			row.m_texture = nullptr;
		}

		row.m_face   = layout.m_face;
		row.m_failed = true;

		const float scale = layout.m_icon / 24.f;
		const float drop  = open ? 0.f : k_eye_lid_drop;
		const ImVec2 box( layout.m_pad + layout.m_face + layout.m_gap, layout.m_pad + layout.m_radius - layout.m_icon * 0.5f );

		const auto at = [ & ]( const ImVec2& point ) { return ImVec2( box.x + point.x * scale, box.y + ( point.y + drop ) * scale ); };

		std::vector< std::pair< ImVec2, ImVec2 > > segments{ };
		std::vector< std::pair< ImVec2, float > > circles{ { ImVec2( layout.m_pad + layout.m_radius, layout.m_pad + layout.m_radius ), layout.m_radius } };

		if ( open ) {
			ImVec2 from = k_eye_lens_start;

			for ( const auto& curve : k_eye_lens ) {
				ImVec2 last = at( from );

				for ( int step = 1; step <= k_eye_lens_steps; step++ ) {
					const ImVec2 point = at( ImBezierCubicCalc( from, curve[ 0 ], curve[ 1 ], curve[ 2 ], step / static_cast< float >( k_eye_lens_steps ) ) );

					segments.emplace_back( last, point );
					last = point;
				}

				from = curve[ 2 ];
			}

			circles.emplace_back( at( k_eye_iris_center ), k_eye_iris_radius * scale );
		} else {
			for ( int i = 1; i < IM_ARRAYSIZE( k_eye_lid ); i++ )
				segments.emplace_back( at( k_eye_lid[ i - 1 ] ), at( k_eye_lid[ i ] ) );

			for ( const auto& lash : k_eye_lashes )
				segments.emplace_back( at( lash[ 0 ] ), at( lash[ 1 ] ) );
		}

		const int width = layout.m_width, height = layout.m_height;
		const float half = layout.m_stroke * 0.5f;

		std::vector< std::uint32_t > pixels( static_cast< std::size_t >( width ) * height );

		for ( int y = 0; y < height; y++ ) {
			for ( int x = 0; x < width; x++ ) {
				const ImVec2 p( x + 0.5f, y + 0.5f );

				float distance = FLT_MAX;

				for ( const auto& [ a, b ] : segments )
					distance = ImMin( distance, segment_distance( p, a, b ) );

				for ( const auto& [ center, radius ] : circles )
					distance = ImMin( distance, std::fabs( std::sqrt( ImLengthSqr( ImVec2( p.x - center.x, p.y - center.y ) ) ) - radius ) );

				const float glyph = ImSaturate( half + 0.5f - distance );
				const float halo  = ImSaturate( half + 1.5f - distance ) * k_eye_halo_alpha;
				const float alpha = glyph + halo * ( 1.f - glyph );
				const float gray  = alpha > 0.f ? glyph / alpha : 0.f;

				const auto a8 = static_cast< std::uint32_t >( alpha * 255.f + 0.5f ), g8 = static_cast< std::uint32_t >( gray * 255.f + 0.5f );

				pixels[ static_cast< std::size_t >( y ) * width + x ] = ( a8 << 24 ) | ( g8 << 16 ) | ( g8 << 8 ) | g8;
			}
		}

		bool dynamic   = false;
		HRESULT result = device->CreateTexture( width, height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &row.m_texture, nullptr );

		if ( FAILED( result ) || !row.m_texture ) {
			dynamic       = true;
			row.m_texture = nullptr;
			result        = device->CreateTexture( width, height, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &row.m_texture, nullptr );
		}

		if ( FAILED( result ) || !row.m_texture ) {
			row.m_texture = nullptr;
			return nullptr;
		}

		bool uploaded = false;
		D3DLOCKED_RECT locked{ };

		if ( SUCCEEDED( row.m_texture->LockRect( 0, &locked, nullptr, dynamic ? D3DLOCK_DISCARD : 0 ) ) ) {
			if ( locked.pBits ) {
				for ( int y = 0; y < height; y++ )
					std::memcpy( static_cast< BYTE* >( locked.pBits ) + y * locked.Pitch, pixels.data( ) + static_cast< std::size_t >( y ) * width,
					             static_cast< std::size_t >( width ) * 4 );

				uploaded = true;
			}

			row.m_texture->UnlockRect( 0 );
		}

		if ( !uploaded ) {
			row.m_texture->Release( );
			row.m_texture = nullptr;
			return nullptr;
		}

		row.m_failed = false;

		return row.m_texture;
	}
}

namespace
{
	constexpr UINT airflow_blur_down_sample = 9;

	struct airflow_blur_t {
		IDirect3DTexture9* m_textures[ 2 ]{ };
		IDirect3DSurface9* m_resolve = nullptr;
		IDirect3DPixelShader9* m_shader_x = nullptr;
		IDirect3DPixelShader9* m_shader_y = nullptr;
		/* begin to end, render thread */
		IDirect3DSurface9* m_target_backup = nullptr;
		IDirect3DSurface9* m_depth_backup  = nullptr;
		DWORD m_srgb_write = 0, m_srgb_read = 0;
		UINT m_width = 0, m_height = 0, m_small_width = 0, m_small_height = 0;
		D3DFORMAT m_format                 = D3DFMT_UNKNOWN;
		D3DMULTISAMPLE_TYPE m_multi_sample = D3DMULTISAMPLE_NONE;
		/* shaders refused: never retried */
		bool m_failed = false;
	} airflow_blur{ };

	void release_airflow_blur_targets( )
	{
		for ( auto& texture : airflow_blur.m_textures ) {
			if ( texture ) {
				texture->Release( );
				texture = nullptr;
			}
		}

		if ( airflow_blur.m_resolve ) {
			airflow_blur.m_resolve->Release( );
			airflow_blur.m_resolve = nullptr;
		}

		airflow_blur.m_width = airflow_blur.m_height = 0;
	}

	void release_airflow_blur( const bool textures_only )
	{
		release_airflow_blur_targets( );

		if ( textures_only )
			return;

		for ( auto shader : { &airflow_blur.m_shader_x, &airflow_blur.m_shader_y } ) {
			if ( *shader ) {
				( *shader )->Release( );
				*shader = nullptr;
			}
		}

		airflow_blur.m_failed = false;
	}

	/* record time, end_scene thread (the device's). false = no blur this frame */
	bool ensure_airflow_blur( IDirect3DDevice9* device )
	{
		auto& blur = airflow_blur;

		if ( blur.m_failed )
			return false;

		if ( !blur.m_shader_x || !blur.m_shader_y ) {
			if ( FAILED( device->CreatePixelShader( reinterpret_cast< const DWORD* >( airflow_blur_x ), &blur.m_shader_x ) ) ||
			     FAILED( device->CreatePixelShader( reinterpret_cast< const DWORD* >( airflow_blur_y ), &blur.m_shader_y ) ) ) {
				release_airflow_blur( false );
				blur.m_failed = true;
				return false;
			}
		}

		IDirect3DSurface9* back_buffer = nullptr;

		if ( FAILED( device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer ) ) || !back_buffer )
			return false;

		D3DSURFACE_DESC description{ };
		const bool described = SUCCEEDED( back_buffer->GetDesc( &description ) );
		back_buffer->Release( );

		if ( !described )
			return false;

		if ( blur.m_textures[ 0 ] && ( blur.m_width != description.Width || blur.m_height != description.Height ||
		                               blur.m_format != description.Format || blur.m_multi_sample != description.MultiSampleType ) )
			release_airflow_blur_targets( );

		if ( blur.m_textures[ 0 ] )
			return true;

		blur.m_small_width  = ImMax( description.Width / airflow_blur_down_sample, 1u );
		blur.m_small_height = ImMax( description.Height / airflow_blur_down_sample, 1u );

		for ( auto& texture : blur.m_textures ) {
			if ( FAILED( device->CreateTexture( blur.m_small_width, blur.m_small_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT,
			                                    &texture, nullptr ) ) ) {
				texture = nullptr;
				release_airflow_blur_targets( );
				return false;
			}
		}

		if ( description.MultiSampleType != D3DMULTISAMPLE_NONE &&
		     FAILED( device->CreateRenderTarget( description.Width, description.Height, description.Format, D3DMULTISAMPLE_NONE, 0, FALSE,
		                                         &blur.m_resolve, nullptr ) ) ) {
			blur.m_resolve = nullptr;
			release_airflow_blur_targets( );
			return false;
		}

		blur.m_width        = description.Width;
		blur.m_height       = description.Height;
		blur.m_format       = description.Format;
		blur.m_multi_sample = description.MultiSampleType;

		return true;
	}

	void set_airflow_blur_target( IDirect3DDevice9* device, IDirect3DTexture9* texture )
	{
		IDirect3DSurface9* surface = nullptr;

		if ( SUCCEEDED( texture->GetSurfaceLevel( 0, &surface ) ) && surface ) {
			device->SetRenderTarget( 0, surface );
			surface->Release( );
		}
	}

	void airflow_blur_begin( const ImDrawList*, const ImDrawCmd* )
	{
		const auto device = g_interfaces.m_direct_device;
		auto& blur        = airflow_blur;

		device->GetRenderTarget( 0, &blur.m_target_backup );

		if ( FAILED( device->GetDepthStencilSurface( &blur.m_depth_backup ) ) )
			blur.m_depth_backup = nullptr;

		device->SetDepthStencilSurface( nullptr );

		IDirect3DSurface9 *back_buffer = nullptr, *low_res = nullptr;

		if ( SUCCEEDED( device->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &back_buffer ) ) && back_buffer ) {
			if ( SUCCEEDED( blur.m_textures[ 0 ]->GetSurfaceLevel( 0, &low_res ) ) && low_res ) {
				if ( !blur.m_resolve )
					device->StretchRect( back_buffer, nullptr, low_res, nullptr, D3DTEXF_LINEAR );
				else if ( SUCCEEDED( device->StretchRect( back_buffer, nullptr, blur.m_resolve, nullptr, D3DTEXF_NONE ) ) )
					device->StretchRect( blur.m_resolve, nullptr, low_res, nullptr, D3DTEXF_LINEAR );

				low_res->Release( );
			}

			back_buffer->Release( );
		}

		device->GetRenderState( D3DRS_SRGBWRITEENABLE, &blur.m_srgb_write );
		device->GetSamplerState( 0, D3DSAMP_SRGBTEXTURE, &blur.m_srgb_read );
		device->SetRenderState( D3DRS_SRGBWRITEENABLE, FALSE );
		device->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, FALSE );

		device->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		device->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		device->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );

		const D3DMATRIX projection = { { { 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f,
			                               -1.f / static_cast< float >( blur.m_small_width ), 1.f / static_cast< float >( blur.m_small_height ), 0.f,
			                               1.f } } };

		device->SetTransform( D3DTS_PROJECTION, &projection );
	}

	void airflow_blur_first_pass( const ImDrawList*, const ImDrawCmd* )
	{
		const auto device    = g_interfaces.m_direct_device;
		const float texel[4] = { 1.f / static_cast< float >( airflow_blur.m_small_width ) };

		device->SetPixelShader( airflow_blur.m_shader_x );
		device->SetPixelShaderConstantF( 0, texel, 1 );
		set_airflow_blur_target( device, airflow_blur.m_textures[ 1 ] );
	}

	void airflow_blur_second_pass( const ImDrawList*, const ImDrawCmd* )
	{
		const auto device    = g_interfaces.m_direct_device;
		const float texel[4] = { 1.f / static_cast< float >( airflow_blur.m_small_height ) };

		device->SetPixelShader( airflow_blur.m_shader_y );
		device->SetPixelShaderConstantF( 0, texel, 1 );
		set_airflow_blur_target( device, airflow_blur.m_textures[ 0 ] );
	}

	void airflow_blur_end( const ImDrawList*, const ImDrawCmd* )
	{
		const auto device = g_interfaces.m_direct_device;
		auto& blur        = airflow_blur;

		device->SetRenderTarget( 0, blur.m_target_backup );
		device->SetDepthStencilSurface( blur.m_depth_backup );

		for ( auto surface : { &blur.m_target_backup, &blur.m_depth_backup } ) {
			if ( *surface ) {
				( *surface )->Release( );
				*surface = nullptr;
			}
		}

		device->SetPixelShader( nullptr );
		device->SetRenderState( D3DRS_SRGBWRITEENABLE, blur.m_srgb_write );
		device->SetSamplerState( 0, D3DSAMP_SRGBTEXTURE, blur.m_srgb_read );
		device->SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE );
	}

	bool queue_airflow_blur( ImDrawList* list )
	{
		const auto device = g_interfaces.m_direct_device;

		if ( !device || !ensure_airflow_blur( device ) )
			return false;

		const auto texture_0 = reinterpret_cast< ImTextureID >( airflow_blur.m_textures[ 0 ] );
		const auto texture_1 = reinterpret_cast< ImTextureID >( airflow_blur.m_textures[ 1 ] );

		list->AddCallback( airflow_blur_begin, nullptr );

		for ( int i = 0; i < 8; ++i ) {
			list->AddCallback( airflow_blur_first_pass, nullptr );
			list->AddImage( texture_0, ImVec2( -1.f, -1.f ), ImVec2( 1.f, 1.f ) );
			list->AddCallback( airflow_blur_second_pass, nullptr );
			list->AddImage( texture_1, ImVec2( -1.f, -1.f ), ImVec2( 1.f, 1.f ) );
		}

		list->AddCallback( airflow_blur_end, nullptr );
		list->AddCallback( ImDrawCallback_ResetRenderState, nullptr );

		return true;
	}

	void airflow_blur_rect( ImDrawList* list, const bool blurred, const ImVec2 min, const ImVec2 max, const ImU32 color, const float rounding,
	                        const ImDrawFlags flags )
	{
		if ( !blurred )
			return;

		const ImVec2 display = ImVec2( g_ctx.m_width, g_ctx.m_height );

		list->AddImageRounded( reinterpret_cast< ImTextureID >( airflow_blur.m_textures[ 0 ] ), min, max, ImVec2( min.x / display.x, min.y / display.y ),
		                       ImVec2( max.x / display.x, max.y / display.y ), color, rounding, flags );
	}

	void airflow_lerp( float& value, const bool condition, const float speed )
	{
		value = ImClamp( ImLerp( value, condition ? 1.f : 0.f, ImGui::GetIO( ).DeltaTime * 25.f * speed ), 0.f, 1.f );
	}
}

void n_misc::impl_t::release_spectator_textures( const bool textures_only )
{
	for ( auto& row : eye_row_textures ) {
		if ( row.m_texture ) {
			row.m_texture->Release( );
			row.m_texture = nullptr;
		}

		row.m_face   = 0;
		row.m_failed = false;
	}

	release_airflow_blur( textures_only );

	for ( auto& gif : spectator_gifs )
		release_gif( gif, textures_only );

	for ( auto& icon : airflow_icons )
		release_gif( icon, textures_only );

	release_file_image( lumi_mascot, textures_only );
	release_file_image( kamibebra_custom_gif, textures_only );
	release_file_image( watermark_custom_gif, textures_only );

	if ( !textures_only && gdiplus_token ) {
		Gdiplus::GdiplusShutdown( gdiplus_token );
		gdiplus_token = 0;
	}
}

void n_misc::impl_t::draw_spectator_list_eyes( )
{
	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );

	if ( entries.empty( ) )
		return;

	const int face = static_cast< int >( std::floor( ImClamp( GET_VARIABLE( g_variables.m_spectators_eyes_size, float ), 16.f, 64.f ) + 0.5f ) );

	const auto layout = eye_row_layout( face );

	const int count = static_cast< int >( entries.size( ) );

	int flags = ImGuiWindowFlags_::ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_::ImGuiWindowFlags_NoTitleBar |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_::ImGuiWindowFlags_NoResize |
	            ImGuiWindowFlags_::ImGuiWindowFlags_NoCollapse;

	if ( !g_menu.m_opened )
		flags |= ImGuiWindowFlags_::ImGuiWindowFlags_NoMove;

	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowPadding, ImVec2( 0.f, 0.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_::ImGuiStyleVar_WindowBorderSize, 0.f );

	ImGui::SetNextWindowSize( ImVec2( static_cast< float >( layout.m_width ), static_cast< float >( layout.m_height + ( count - 1 ) * layout.m_pitch ) ),
	                          ImGuiCond_::ImGuiCond_Always );
	ImGui::Begin( "botox-spectators-eyes", 0, flags );
	{
		const auto draw_list = ImGui::GetWindowDrawList( );

		const ImVec2 position( std::floor( ImGui::GetWindowPos( ).x ), std::floor( ImGui::GetWindowPos( ).y ) );

		const c_color local_color = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color );
		const c_color other_color = GET_VARIABLE( g_variables.m_spectators_list_text_color_two, c_color );

		for ( int i = 0; i < count; i++ ) {
			const auto& entry = entries[ i ];

			const ImVec2 row_min( position.x, position.y + static_cast< float >( i * layout.m_pitch ) );
			const ImVec2 center( row_min.x + layout.m_pad + layout.m_radius, row_min.y + layout.m_pad + layout.m_radius );

			const float radius = layout.m_radius;

			if ( const auto avatar = spectator_avatar_texture( entry.m_index, entry.m_team, entry.m_fake_player ) )
				draw_list->AddImageRounded( avatar, ImVec2( center.x - radius, center.y - radius ), ImVec2( center.x + radius, center.y + radius ),
				                            ImVec2( 0.f, 0.f ), ImVec2( 1.f, 1.f ), IM_COL32_WHITE, radius );
			else
				draw_list->AddCircleFilled( center, radius, IM_COL32( 45, 45, 45, 200 ) );

			if ( const auto art = eye_row_texture( entry.m_watching_local, layout ) )
				draw_list->AddImage( art, row_min, ImVec2( row_min.x + layout.m_width, row_min.y + layout.m_height ), ImVec2( 0.f, 0.f ),
				                     ImVec2( 1.f, 1.f ), ( entry.m_watching_local ? local_color : other_color ).get_u32( ) );
		}
	}
	ImGui::End( );

	ImGui::PopStyleVar( 2 );
}

void n_misc::impl_t::draw_spectator_list_cumhacck( )
{
	if ( !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	const auto title_font = g_render.m_fonts[ e_font_names::font_name_cumhacck_35 ];
	const auto name_font  = g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ];

	if ( !title_font || !name_font )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );
	std::erase_if( entries, []( const spectator_entry_t& entry ) { return !entry.m_watching_local; } );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const float mid = std::floor( g_ctx.m_height * 0.5f );

	const ImU32 color = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color ).get_u32( );

	const ImU32 shadow = IM_COL32( 0, 0, 0, ( color >> IM_COL32_A_SHIFT ) & 0xFF );

	const auto draw_shadowed = [ & ]( ImFont* font, const float x, const float y, const char* text ) {
		draw_list->AddText( font, font->FontSize, ImVec2( x + 1.f, y + 1.f ), shadow, text );
		draw_list->AddText( font, font->FontSize, ImVec2( x, y ), color, text );
	};

	for ( std::size_t i = 0; i < entries.size( ); i++ ) {
		const auto& entry = entries[ i ];

		const std::string name = entry.m_fake_player ? entry.m_name + " (BOT)" : entry.m_name;

		draw_shadowed( name_font, 4.f, mid - 40.f + 15.f * static_cast< float >( i ), name.c_str( ) );
	}

	draw_shadowed( title_font, 4.f, mid - 75.f, "spectators:" );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_airplane( )
{
	if ( !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) ||
	     ( !g_ctx.m_local->is_alive( ) && !GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) ) )
		return;

	const auto title_font = g_render.m_fonts[ e_font_names::font_name_airplane_title ];
	const auto row_font   = g_render.m_fonts[ e_font_names::font_name_airplane_rows ];

	if ( !title_font || !row_font )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );

	std::vector< std::string > rows{ };
	rows.reserve( entries.size( ) );

	int w = 100;

	for ( const auto& entry : entries ) {
		const std::string& row = rows.emplace_back( std::format( "{} > {}", entry.m_name, entry.m_target_name ) );

		const int row_width = static_cast< int >( row_font->CalcTextSizeA( row_font->FontSize, FLT_MAX, 0.f, row.c_str( ) ).x );

		if ( row_width > w )
			w = row_width + 5;
	}

	const int h = 15 + 15 * static_cast< int >( rows.size( ) );

	drag_placed_panel( static_cast< float >( w ), static_cast< float >( h ) );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const ImU32 left_color = GET_VARIABLE( g_variables.m_spectators_airplane_gradient_color, c_color ).get_u32( );

	draw_list->AddRectFilledMultiColor( ImVec2( x, y ), ImVec2( x + w, y + h ), left_color, IM_COL32( 0, 0, 0, 0 ), IM_COL32( 0, 0, 0, 0 ),
	                                    left_color );

	const bool outline = GET_VARIABLE( g_variables.m_spectators_airplane_outline, bool );

	const auto draw_text = [ & ]( ImFont* font, const float text_x, const float text_y, const ImU32 color, const char* text ) {
		constexpr ImU32 shadow = IM_COL32( 0, 0, 0, 255 );

		if ( outline ) {
			draw_list->AddText( font, font->FontSize, ImVec2( text_x + 1.f, text_y ), shadow, text );
			draw_list->AddText( font, font->FontSize, ImVec2( text_x - 1.f, text_y ), shadow, text );
			draw_list->AddText( font, font->FontSize, ImVec2( text_x, text_y + 1.f ), shadow, text );
			draw_list->AddText( font, font->FontSize, ImVec2( text_x, text_y - 1.f ), shadow, text );
		} else
			draw_list->AddText( font, font->FontSize, ImVec2( text_x + 1.f, text_y + 1.f ), shadow, text );

		draw_list->AddText( font, font->FontSize, ImVec2( text_x, text_y ), color, text );
	};

	constexpr const char* titles[ ] = { "Spectators", "spectators", "SPECTATORS" };

	const char* const title = titles[ std::clamp( GET_VARIABLE( g_variables.m_spectators_airplane_title_case, int ), 0, 2 ) ];
	const int title_width   = static_cast< int >( title_font->CalcTextSizeA( title_font->FontSize, FLT_MAX, 0.f, title ).x );

	draw_text( title_font, x + static_cast< float >( w / 2 - title_width / 2 ), y + 3.f,
	           GET_VARIABLE( g_variables.m_spectators_airplane_title_color, c_color ).get_u32( ), title );

	const ImU32 local_color = GET_VARIABLE( g_variables.m_spectators_airplane_local_color, c_color ).get_u32( );
	const ImU32 other_color = GET_VARIABLE( g_variables.m_spectators_airplane_other_color, c_color ).get_u32( );

	for ( std::size_t i = 0; i < rows.size( ); i++ )
		draw_text( row_font, x + 3.f, y + 15.f + 15.f * static_cast< float >( i ), entries[ i ].m_watching_local ? local_color : other_color,
		           rows[ i ].c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_bhopcheat( )
{
	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	const auto font = g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ];

	if ( !font )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );
	std::erase_if( entries, []( const spectator_entry_t& entry ) { return !entry.m_watching_local; } );

	if ( entries.empty( ) )
		return;

	const auto mode_name = []( const int obs_mode ) -> const char* {
		switch ( obs_mode ) {
		case e_obs_mode::obs_mode_in_eye:
			return "first person";
		case e_obs_mode::obs_mode_chase:
			return "3rd person";
		case e_obs_mode::obs_mode_roaming:
			return "noclip";
		case e_obs_mode::obs_mode_deathcam:
			return "deathcam";
		case e_obs_mode::obs_mode_freezecam:
			return "freezecam";
		case e_obs_mode::obs_mode_fixed:
			return "fixed";
		default:
			return "";
		}
	};

	const bool show_mode   = GET_VARIABLE( g_variables.m_spectators_bhopcheat_mode, bool );
	const bool draw_avatar = GET_VARIABLE( g_variables.m_spectators_avatar, bool );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const float x = draw_avatar ? 25.f : 5.f;

	for ( std::size_t i = 0; i < entries.size( ); i++ ) {
		const auto& entry = entries[ i ];

		const float y = 5.f + font->FontSize * static_cast< float >( i );

		const std::string text =
			show_mode ? std::format( "{}{} | {}", entry.m_fake_player ? "[bot] " : "", entry.m_name, mode_name( entry.m_obs_mode ) ) : entry.m_name;

		if ( draw_avatar ) {
			if ( const auto avatar = spectator_avatar_texture( entry.m_index, entry.m_team, entry.m_fake_player ) )
				draw_list->AddImage( avatar, ImVec2( 9.f, y ), ImVec2( 21.f, y + 12.f ) );
		}

		draw_list->AddText( font, font->FontSize, ImVec2( x + 1.f, y + 1.f ), IM_COL32( 0, 0, 0, 255 ), text.c_str( ) );
		draw_list->AddText( font, font->FontSize, ImVec2( x, y ), IM_COL32( 255, 255, 255, 255 ), text.c_str( ) );
	}

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_millionware( )
{
	if ( !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	const auto font = g_render.m_fonts[ e_font_names::font_name_tahoma_12 ];

	if ( !font )
		return;

	const bool local_alive = g_ctx.m_local->is_alive( );

	int local_target = 0;

	if ( !local_alive ) {
		const auto target = reinterpret_cast< c_base_entity* >(
			g_interfaces.m_client_entity_list->get_client_entity_from_handle( g_ctx.m_local->get_observer_target_handle( ) ) );

		if ( !target )
			return;

		local_target = target->get_index( );
	}

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true );
	std::erase_if( entries, [ & ]( const spectator_entry_t& entry ) {
		if ( entry.m_obs_mode != e_obs_mode::obs_mode_in_eye && entry.m_obs_mode != e_obs_mode::obs_mode_chase )
			return true;

		return local_alive ? !entry.m_watching_local : entry.m_target_index != local_target;
	} );

	if ( entries.empty( ) )
		return;

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const float screen_width = g_ctx.m_width;

	for ( std::size_t i = 0; i < entries.size( ); i++ ) {
		const char* const name = entries[ i ].m_name.c_str( );

		const float text_width = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, name ).x;

		draw_list->AddText( font, font->FontSize, ImVec2( screen_width - text_width - 4.f, 4.f + 16.f * static_cast< float >( i ) ),
		                    IM_COL32( 255, 255, 255, 255 ), name );
	}

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_aimware( )
{
	if ( !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	const auto font = g_render.m_fonts[ e_font_names::font_name_sunflower_30 ];

	if ( !font )
		return;

	const int local_index = g_ctx.m_local->get_index( );

	int target_index = local_index;

	if ( !g_ctx.m_local->is_alive( ) ) {
		const auto target = reinterpret_cast< c_base_entity* >(
			g_interfaces.m_client_entity_list->get_client_entity_from_handle( g_ctx.m_local->get_observer_target_handle( ) ) );

		target_index = target && target->is_alive( ) && !target->is_dormant( ) ? target->get_index( ) : -1;
	}

	std::vector< spectator_entry_t > entries{ };

	if ( target_index != -1 && !g_menu.m_opened ) {
		collect_spectators( entries, true );
		std::erase_if( entries, [ & ]( const spectator_entry_t& entry ) {
			if ( entry.m_index == local_index )
				return true;

			if ( entry.m_obs_mode != e_obs_mode::obs_mode_in_eye && entry.m_obs_mode != e_obs_mode::obs_mode_chase )
				return true;

			return entry.m_target_index != target_index;
		} );
	}

	drag_placed_panel( 200.f, 24.f );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	constexpr ImU32 red = IM_COL32( 200, 40, 40, 255 ), white = IM_COL32( 255, 255, 255, 255 );

	draw_list->AddRectFilled( ImVec2( x, y ), ImVec2( x + 200.f, y + 24.f ), red, 3.f );
	draw_list->AddRectFilled( ImVec2( x, y + 23.f ), ImVec2( x + 200.f, y + 24.f ), red );

	constexpr auto title = "Spectators list";

	const float title_half_height = font->CalcTextSizeA( 15.f, FLT_MAX, 0.f, title ).y / 2.f;

	draw_list->AddText( font, 15.f, ImVec2( x + 8.f, y + ( 12.f - title_half_height ) ), white, title );

	float extension = 0.f;

	for ( const auto& entry : entries )
		extension += 8.f + font->CalcTextSizeA( 14.f, FLT_MAX, 0.f, entry.m_name.c_str( ) ).y;

	extension += 8.f;

	draw_list->AddRectFilled( ImVec2( x, y + 24.f ), ImVec2( x + 200.f, y + extension + 24.f ), IM_COL32( 0, 0, 0, 75 ) );
	draw_list->AddRectFilled( ImVec2( x, y + extension + 24.f ), ImVec2( x + 200.f, y + extension + 28.f ), red );

	extension = 0.f;

	for ( const auto& entry : entries ) {
		extension += 8.f;

		draw_list->AddText( font, 14.f, ImVec2( x + 6.f, y + extension + 24.f ), white, entry.m_name.c_str( ) );

		extension += font->CalcTextSizeA( 14.f, FLT_MAX, 0.f, entry.m_name.c_str( ) ).y;
	}

	g_render.end_stretch_block( block );
}

namespace {
	ImU32 havoc_color( const std::uint32_t var, const float alpha )
	{
		return GET_VARIABLE( var, c_color ).get_u32( alpha );
	}

	ImU32 havoc_accent2_color( const float alpha )
	{
		return GET_VARIABLE( g_variables.m_accent, c_color ).get_u32( alpha );
	}

	ImU32 havoc_grey( const int shade, const float alpha )
	{
		return IM_COL32( shade, shade, shade, static_cast< int >( 255.f * alpha ) );
	}

	void havoc_box( ImDrawList* list, const ImVec2 pos, const ImVec2 size, const float alpha )
	{
		const auto shell = [ & ]( const float inset, const int shade ) {
			list->AddRectFilled( ImVec2( pos.x + inset, pos.y + inset ), ImVec2( pos.x + size.x - inset, pos.y + size.y - inset ), havoc_grey( shade, alpha ),
			                     5.f );
		};

		shell( -1.f, 10 );
		shell( 0.f, 55 );
		shell( 1.f, 10 );
		shell( 2.f, 20 );
	}

	void havoc_line( ImDrawList* list, const ImVec2 a, const ImVec2 b, const ImU32 color )
	{
		list->AddLine( ImVec2( a.x + 0.5f, a.y + 0.5f ), ImVec2( b.x + 0.5f, b.y + 0.5f ), color );
	}

	void vgui_shadow_text( ImDrawList* list, ImFont* font, const ImVec2 pos, const ImU32 color, const char* text )
	{
		const ImVec2 at( std::floor( pos.x ), std::floor( pos.y ) );

		list->AddText( font, font->FontSize, ImVec2( at.x + 1.f, at.y + 1.f ), color & IM_COL32_A_MASK, text );
		list->AddText( font, font->FontSize, at, color, text );
	}

	float vgui_text_width( ImFont* font, const std::string& text )
	{
		return std::floor( font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, text.c_str( ) ).x );
	}
}

void n_misc::impl_t::draw_spectator_list_havoc( )
{
	static ImVec2 abs_size{ 120.f, 20.f };
	static float menu_alpha = 0.f;

	const auto font = g_render.m_fonts[ e_font_names::font_name_havoc_tahoma_13 ];

	if ( !font )
		return;

	const float dt = ImGui::GetIO( ).DeltaTime;

	menu_alpha = ImClamp( menu_alpha + ( g_menu.m_opened ? 5.f : -5.f ) * dt, 0.f, 1.f );

	struct row_t {
		std::string m_name, m_mode;
	};

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );

	std::vector< row_t > rows{ };
	ImVec2 target_size{ 120.f, 20.f };

	const bool show_target = GET_VARIABLE( g_variables.m_spectators_list_show_target, bool );

	for ( const auto& entry : entries ) {
		row_t row{ entry.m_name, show_target                                       ? "[" + entry.m_target_name + "]"
		                         : entry.m_obs_mode == e_obs_mode::obs_mode_chase ? "[Third person]"
		                                                                          : "[First person]" };

		target_size.x = ImMax( target_size.x, vgui_text_width( font, row.m_name + row.m_mode ) + 20.f + 40.f );
		rows.push_back( std::move( row ) );
	}

	constexpr float append_length = 17.f;

	if ( !rows.empty( ) || menu_alpha > 0.f )
		target_size.y += 9.f + static_cast< float >( rows.size( ) ) * append_length;

	abs_size.x += ( target_size.x - abs_size.x ) * 10.f * dt;
	abs_size.y += ( target_size.y - abs_size.y ) * 10.f * dt;

	float alpha = 1.f;

	const bool always_show = GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) && g_ctx.m_local &&
	                         g_interfaces.m_engine_client->is_in_game( );

	if ( ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) ) && menu_alpha < 1.f && !always_show )
		alpha = menu_alpha;

	if ( alpha < 0.001f )
		return;

	drag_placed_panel( ImMax( abs_size.x, 100.f ), 20.f );

	if ( abs_size.y < 1.f )
		return;

	const ImVec2 pos( static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) ),
	                  static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) ) );
	const ImVec2 size = abs_size;

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	havoc_box( draw_list, pos, size, alpha );

	const ImU32 accent = havoc_color( g_variables.m_spectators_havoc_color, alpha ), accent2 = havoc_accent2_color( alpha );

	for ( int i = 0; i < 3; ++i ) {
		const float f = static_cast< float >( i );

		havoc_line( draw_list, ImVec2( pos.x + size.x - 19.f - f, pos.y + 2.f ), ImVec2( pos.x + size.x - 4.f - f, pos.y + 17.f ), accent );
		havoc_line( draw_list, ImVec2( pos.x + size.x - 12.f - f, pos.y + 2.f ), ImVec2( pos.x + size.x - 2.f, pos.y + 12.f + f ), accent2 );

		havoc_line( draw_list, ImVec2( pos.x + 19.f + f, pos.y + 2.f ), ImVec2( pos.x + 4.f + f, pos.y + 17.f ), accent );
		havoc_line( draw_list, ImVec2( pos.x + 11.f + f, pos.y + 2.f ), ImVec2( pos.x + 1.f, pos.y + 12.f + f ), accent2 );
	}

	if ( size.y > 20.f ) {
		havoc_line( draw_list, ImVec2( pos.x + 1.f, pos.y + 17.f ), ImVec2( pos.x + size.x - 2.f, pos.y + 17.f ), havoc_grey( 10, alpha ) );
		havoc_line( draw_list, ImVec2( pos.x + 1.f, pos.y + 18.f ), ImVec2( pos.x + size.x - 2.f, pos.y + 18.f ), havoc_grey( 55, alpha ) );
		havoc_line( draw_list, ImVec2( pos.x + 1.f, pos.y + 19.f ), ImVec2( pos.x + size.x - 2.f, pos.y + 19.f ), havoc_grey( 10, alpha ) );
	}

	const std::string first = "Spec", second = "tators";
	const float title_x = std::floor( size.x / 2.f - ( vgui_text_width( font, first ) + vgui_text_width( font, second ) ) / 2.f );

	vgui_shadow_text( draw_list, font, ImVec2( pos.x + title_x, pos.y + 3.f ), havoc_grey( 255, alpha ), first.c_str( ) );
	vgui_shadow_text( draw_list, font, ImVec2( pos.x + title_x + vgui_text_width( font, first ), pos.y + 3.f ), accent, second.c_str( ) );

	draw_list->PushClipRect( ImVec2( pos.x + 2.f, pos.y + 2.f ), ImVec2( pos.x + size.x - 1.f, pos.y + size.y - 1.f ), true );

	for ( std::size_t i = 0; i < rows.size( ); i++ ) {
		const float row_y = pos.y + 25.f + append_length * static_cast< float >( i );

		vgui_shadow_text( draw_list, font, ImVec2( pos.x + 20.f, row_y + 1.f ), havoc_grey( 255, alpha ), rows[ i ].m_name.c_str( ) );
		vgui_shadow_text( draw_list, font, ImVec2( pos.x + size.x - 20.f - vgui_text_width( font, rows[ i ].m_mode ), row_y ), accent,
		                  rows[ i ].m_mode.c_str( ) );
	}

	draw_list->PopClipRect( );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_havoc( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_havoc_tahoma_13 ];

	if ( !font )
		return;

	const std::string name = watermark_brand( ) + " ", label = watermark_label( );

	const float name_width = vgui_text_width( font, name );

	const ImVec2 size( name_width + vgui_text_width( font, label ) + 37.f, 20.f );
	const ImVec2 pos( static_cast< float >( g_ctx.m_width ) - size.x - 20.f, 15.f );

	g_watermark_box = ImVec4( pos.x, pos.y, pos.x + size.x, pos.y + size.y );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	havoc_box( draw_list, pos, size, 1.f );

	const ImU32 accent = havoc_color( g_variables.m_watermark_havoc_color, 1.f ), accent2 = havoc_accent2_color( 1.f );

	for ( int i = 0; i < 3; ++i ) {
		const float x = pos.x + name_width + 8.f + static_cast< float >( i );

		havoc_line( draw_list, ImVec2( x + 19.f, pos.y + 2.f ), ImVec2( x + 4.f, pos.y + 17.f ), accent2 );
		havoc_line( draw_list, ImVec2( x + 12.f, pos.y + 2.f ), ImVec2( x - 3.f, pos.y + 17.f ), accent );
	}

	vgui_shadow_text( draw_list, font, ImVec2( pos.x + 6.f, pos.y + 3.f ), accent, name.c_str( ) );
	vgui_shadow_text( draw_list, font, ImVec2( pos.x + name_width + 33.f, pos.y + 3.f ), accent2, label.c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_onetap( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_tahoma_12 ];

	if ( !font )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true );
	std::erase_if( entries, []( const spectator_entry_t& entry ) { return !entry.m_watching_local; } );

	if ( entries.empty( ) )
		return;

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	for ( std::size_t i = 0; i < entries.size( ); i++ )
		draw_list->AddText( font, font->FontSize, ImVec2( 20.f, 200.f + 12.f * static_cast< float >( i ) ), IM_COL32( 230, 230, 230, 255 ),
		                    entries[ i ].m_name.c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_airflow( )
{
	struct airflow_row_t {
		std::string m_name  = { };
		std::string m_chase = { };
		double m_start_time = 0.0;
		float m_step        = 0.f;
		bool m_spectated = false, m_was_spectating = false;
	};

	static std::map< int, airflow_row_t > rows{ };
	static float alpha = 0.f;

	const auto font = g_render.m_fonts[ e_font_names::font_name_airflow_14 ];

	if ( !font )
		return;

	const bool in_game = g_ctx.m_local && g_interfaces.m_engine_client->is_in_game( );

	std::vector< spectator_entry_t > entries{ };

	if ( in_game )
		collect_spectators( entries );
	else
		rows.clear( );

	const auto cut = []( const std::string& text, const std::size_t characters ) {
		const std::string kept = utf8_truncate( text, characters );
		return kept.size( ) < text.size( ) ? kept + "..." : kept;
	};

	for ( auto& [ index, row ] : rows )
		row.m_spectated = false;

	for ( const auto& entry : entries ) {
		auto& row       = rows[ entry.m_index ];
		row.m_name      = cut( entry.m_name, 10 );
		row.m_chase     = "[ " + cut( entry.m_target_name, 8 ) + " ]";
		row.m_spectated = true;
	}

	const double now = ImGui::GetTime( );
	int active       = 0;

	for ( auto& [ index, row ] : rows ) {
		if ( row.m_spectated != row.m_was_spectating ) {
			row.m_start_time     = now;
			row.m_was_spectating = row.m_spectated;
		}

		active += row.m_was_spectating ? 1 : 0;
	}

	const float window_height = 40.f + 25.f * static_cast< float >( active );

	airflow_lerp( alpha, g_menu.m_opened || ( in_game && ( active > 0 || GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) ) ), 1.f );

	if ( alpha <= 0.f )
		return;

	drag_placed_panel( 184.f, window_height + 2.f );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) ) + 4.f;
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) ) + 1.f;

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const bool blurred   = queue_airflow_blur( draw_list );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const ImDrawListFlags old_flags = draw_list->Flags;
	draw_list->Flags |= ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines;

	draw_list->PushClipRect( ImVec2( x, y ), ImVec2( x + 176.f, y + window_height ), true );

	const int a = static_cast< int >( 255.f * alpha );

	airflow_blur_rect( draw_list, blurred, ImVec2( x, y ), ImVec2( x + 176.f, y + 32.f ), IM_COL32( 255, 255, 255, a ), 4.f,
	                   ImDrawFlags_RoundCornersTop );

	if ( const auto icon = spectator_gif_frame( airflow_icons[ 0 ] ) )
		draw_list->AddImage( icon, ImVec2( x + 41.f, y + 9.f ), ImVec2( x + 57.f, y + 25.f ), ImVec2( 0.f, 0.f ), ImVec2( 1.f, 1.f ),
		                     IM_COL32( 255, 255, 255, a ) );

	draw_list->AddText( font, 14.f, ImVec2( x + 65.f, y + 8.f ), IM_COL32( 255, 255, 255, a ), "spectators" );
	draw_list->AddLine( ImVec2( x, y + 31.f ), ImVec2( x + 176.f, y + 31.f ), IM_COL32( 255, 255, 255, static_cast< int >( 12.75f * alpha ) ) );

	airflow_blur_rect( draw_list, blurred, ImVec2( x, y + 32.f ), ImVec2( x + 176.f, y + 32.f + window_height ), IM_COL32( 100, 100, 100, a ), 4.f,
	                   ImDrawFlags_RoundCornersBottom );

	draw_list->AddRect( ImVec2( x, y ), ImVec2( x + 176.f, y + window_height ), IM_COL32( 100, 100, 100, static_cast< int >( 100.f * alpha ) ), 4.f );

	float max_pos = 0.f;

	for ( auto it = rows.begin( ); it != rows.end( ); ) {
		auto& row = it->second;

		float progress = ImClamp( static_cast< float >( ( now - row.m_start_time ) / 0.2 ), 0.f, 1.f );

		if ( !row.m_spectated && !row.m_was_spectating )
			progress = 1.f - progress;

		if ( progress <= 0.f ) {
			it = row.m_was_spectating ? std::next( it ) : rows.erase( it );
			continue;
		}

		const float row_y = y + 39.f + max_pos;

		draw_list->AddText( font, 14.f, ImVec2( x + 12.f, row_y ), IM_COL32( 255, 255, 255, static_cast< int >( 255.f * alpha * progress ) ),
		                    row.m_name.c_str( ) );
		draw_list->AddText( font, 14.f, ImVec2( x + 165.f - imgui_text_width( font, row.m_chase.c_str( ) ), row_y ),
		                    IM_COL32( 255, 255, 255, static_cast< int >( 0.4f * 255.f * alpha * progress ) ), row.m_chase.c_str( ) );

		airflow_lerp( row.m_step, row.m_spectated && row.m_was_spectating, 0.6f );
		max_pos += 25.f * row.m_step;

		++it;
	}

	draw_list->PopClipRect( );
	draw_list->Flags = old_flags;

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_airflow( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_airflow_14 ];

	if ( !font )
		return;

	std::time_t raw = std::time( nullptr );
	std::tm local   = { };
	localtime_s( &local, &raw );

	char clock[ 8 ] = { };
	std::strftime( clock, sizeof( clock ), "%H:%M", &local );

	const auto net_channel = g_interfaces.m_engine_client->get_net_channel_info( );
	const int ping_ms = net_channel && !net_channel->is_loopback( ) ? static_cast< int >( net_channel->get_latency( FLOW_OUTGOING ) * 1000.f ) : 0;

	const std::string prefix = watermark_brand( ), ping = std::format( "{}ms", ping_ms );
	const std::string& user  = GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string );

	const float text_width  = imgui_text_width( font, std::format( "{} {} | {}  {}", prefix, user, clock, ping ).c_str( ) );
	const float prefix_size = imgui_text_width( font, prefix.c_str( ) ) + 16.f;

	const float window_width = 75.f + text_width + 16.f;

	const float x = g_ctx.m_width - window_width - 10.f + 4.f, y = 10.f + 1.f;

	g_watermark_box = ImVec4( x, y, x + window_width - 10.f, y + 29.f );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const bool blurred   = queue_airflow_blur( draw_list );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const ImDrawListFlags old_flags = draw_list->Flags;
	draw_list->Flags |= ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines;

	constexpr ImU32 white = IM_COL32( 255, 255, 255, 255 );

	airflow_blur_rect( draw_list, blurred, ImVec2( x, y ), ImVec2( x + prefix_size + 30.f, y + 29.f ), white, 3.f, ImDrawFlags_RoundCornersLeft );

	if ( const auto logo = spectator_gif_frame( airflow_icons[ 1 ] ) )
		draw_list->AddImage( logo, ImVec2( x + 10.f, y + 7.f ), ImVec2( x + 26.f, y + 23.f ), ImVec2( 0.f, 0.f ), ImVec2( 1.f, 1.f ),
		                     ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent ) );

	draw_list->AddText( font, 14.f, ImVec2( x + 32.f, y + 6.5f ), white, prefix.c_str( ) );

	const float right_x = x + prefix_size + 30.f;

	airflow_blur_rect( draw_list, blurred, ImVec2( right_x, y ), ImVec2( right_x + window_width - prefix_size - 40.f, y + 29.f ),
	                   IM_COL32( 100, 100, 100, 255 ), 3.f, ImDrawFlags_RoundCornersRight );

	const ImVec2 avatar_min( right_x + 7.f, y + 6.f ), avatar_max( right_x + 23.f, y + 22.f );

	IDirect3DTexture9* const avatar = g_ctx.m_local ? g_avatar_cache[ g_ctx.m_local->get_index( ) ] : nullptr;

	if ( avatar )
		draw_list->AddImageRounded( avatar, avatar_min, avatar_max, ImVec2( 0.f, 0.f ), ImVec2( 1.f, 1.f ), white, 20.f );
	else
		draw_list->AddRectFilled( avatar_min, avatar_max, white, 20.f );

	draw_list->AddText( font, 14.f, ImVec2( right_x + 29.f, y + 6.5f ), white, std::format( "{} | {} {}", user, clock, ping ).c_str( ) );

	draw_list->AddRect( ImVec2( x, y ), ImVec2( x + window_width - 10.f, y + 29.f ), IM_COL32( 120, 120, 120, 100 ), 3.f );

	draw_list->Flags = old_flags;

	g_render.end_stretch_block( block );
}

namespace {
	constexpr int evolve_text = 220, evolve_bottom = 18, evolve_block = 29;

	struct evolve_anim_t {
		float m_value, m_start, m_end, m_time = 1.f;

		explicit evolve_anim_t( const float value ) : m_value( value ), m_start( value ), m_end( value ) { }

		void direct( const float to )
		{
			m_start = m_value;
			m_end   = to;
			m_time  = 0.f;
		}

		void animate( const float duration, const bool ease_out )
		{
			m_time = ImMin( m_time + ImGui::GetIO( ).DeltaTime / duration, 1.f );

			const float u = ease_out ? 1.f - ( 1.f - m_time ) * ( 1.f - m_time ) * ( 1.f - m_time ) : m_time;

			m_value = m_start + ( m_end - m_start ) * u;
		}
	};

	void evolve_shadow( ImDrawList* list, const ImVec2 min, const ImVec2 max, const float radius, const float alpha )
	{
		const ImU32 dark = IM_COL32( 0, 0, 0, static_cast< int >( 255.f * alpha ) ), clear = IM_COL32( 0, 0, 0, 0 );

		list->AddRectFilled( min, max, dark );
		list->AddRectFilledMultiColor( ImVec2( min.x - radius, min.y ), ImVec2( min.x, max.y ), clear, dark, dark, clear );
		list->AddRectFilledMultiColor( ImVec2( max.x, min.y ), ImVec2( max.x + radius, max.y ), dark, clear, clear, dark );
		list->AddRectFilledMultiColor( ImVec2( min.x, min.y - radius ), ImVec2( max.x, min.y ), clear, clear, dark, dark );
		list->AddRectFilledMultiColor( ImVec2( min.x, max.y ), ImVec2( max.x, max.y + radius ), dark, dark, clear, clear );

		const ImVec2 uv = list->_Data->TexUvWhitePixel;

		const auto corner = [ & ]( const ImVec2 center, const float from ) {
			constexpr int segments = 8;

			list->PrimReserve( segments * 3, segments * 3 );

			for ( int i = 0; i < segments; ++i ) {
				const float a0 = from + IM_PI * 0.5f * static_cast< float >( i ) / segments;
				const float a1 = from + IM_PI * 0.5f * static_cast< float >( i + 1 ) / segments;

				list->PrimVtx( center, uv, dark );
				list->PrimVtx( ImVec2( center.x + std::cos( a0 ) * radius, center.y + std::sin( a0 ) * radius ), uv, clear );
				list->PrimVtx( ImVec2( center.x + std::cos( a1 ) * radius, center.y + std::sin( a1 ) * radius ), uv, clear );
			}
		};

		corner( min, IM_PI );
		corner( ImVec2( max.x, min.y ), IM_PI * 1.5f );
		corner( max, 0.f );
		corner( ImVec2( min.x, max.y ), IM_PI * 0.5f );
	}

	void evolve_header( ImDrawList* list, const ImVec2 min, const float width, const float alpha )
	{
		list->AddRectFilled( min, ImVec2( min.x + width, min.y + 24.f ), IM_COL32( evolve_block, evolve_block, evolve_block, static_cast< int >( 255.f * alpha ) ),
		                     4.f, ImDrawFlags_RoundCornersTop );

		const ImVec4 accent = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );

		list->AddRectFilled( ImVec2( min.x, min.y + 24.f ), ImVec2( min.x + width, min.y + 25.f ), ImColor( accent.x, accent.y, accent.z, alpha ) );
	}

	float evolve_max_alpha( )
	{
		return ImClamp( GET_VARIABLE( g_variables.m_watermark_evolve_alpha, float ) / 100.f, 0.f, 1.f );
	}
}

void n_misc::impl_t::draw_spectator_list_evolve( )
{
	static evolve_anim_t alpha{ 0.f }, width{ 160.f }, height{ 60.f };
	static bool has_content = false;

	const auto font = g_render.m_fonts[ e_font_names::font_name_evolve_16 ];
	const auto bold = g_render.m_fonts[ e_font_names::font_name_evolve_bold_16 ];

	if ( !font || !bold )
		return;

	const bool in_game = g_interfaces.m_engine_client->is_in_game( );
	const bool forced  = g_menu.m_opened || ( in_game && GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) );

	alpha.animate( 0.1f, false );
	width.animate( 0.2f, true );
	height.animate( 0.2f, true );

	drag_placed_panel( width.m_value, height.m_value );

	const ImVec2 min( static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) ),
	                  static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) ) );
	const ImVec2 max( min.x + width.m_value, min.y + height.m_value );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const bool blurred   = alpha.m_value > 0.f && queue_airflow_blur( draw_list );
	const auto block     = g_render.begin_stretch_block( draw_list );

	if ( alpha.m_value > 0.f ) {
		const float body_alpha = ImMin( alpha.m_value, evolve_max_alpha( ) );

		evolve_shadow( draw_list, min, max, 12.f, body_alpha * 0.25f );
		airflow_blur_rect( draw_list, blurred, min, max, IM_COL32( 255, 255, 255, static_cast< int >( 255.f * alpha.m_value ) ), 4.f,
		                   ImDrawFlags_RoundCornersAll );
		draw_list->AddRectFilled( min, max, IM_COL32( evolve_bottom, evolve_bottom, evolve_bottom, static_cast< int >( 255.f * body_alpha ) ), 4.f );

		evolve_header( draw_list, min, width.m_value, alpha.m_value );

		constexpr auto title = "Spectators";
		const ImVec2 title_size = bold->CalcTextSizeA( bold->FontSize, FLT_MAX, 0.f, title );

		draw_list->AddText( bold, bold->FontSize,
		                    ImVec2( std::floor( min.x + ( width.m_value - title_size.x ) * 0.5f ), std::floor( min.y + ( 24.f - title_size.y ) * 0.5f ) ),
		                    IM_COL32( evolve_text, evolve_text, evolve_text, static_cast< int >( 255.f * alpha.m_value ) ), title );
	}

	struct line_t {
		std::string m_name;
		int m_index;
		bool m_fake_player;
	};

	std::vector< line_t > lines{ };
	float max_width = 0.f;

	if ( in_game && g_ctx.m_local && g_ctx.m_local->is_alive( ) ) {
		std::vector< spectator_entry_t > entries{ };
		collect_spectators( entries, true );

		for ( const auto& entry : entries ) {
			if ( !entry.m_watching_local )
				continue;

			max_width = ImMax( max_width, imgui_text_width( font, entry.m_name.c_str( ) ) + 60.f );
			lines.push_back( { entry.m_name, entry.m_index, entry.m_fake_player } );
		}
	}

	max_width = ImClamp( max_width, 160.f, 400.f );

	if ( ( ( lines.empty( ) && has_content ) || !in_game ) && !forced ) {
		has_content = false;
		width.direct( 160.f );
		height.direct( 30.f );
		alpha.direct( 0.f );

		g_render.end_stretch_block( block );
		return;
	}

	if ( ( !lines.empty( ) && !has_content && in_game ) || forced ) {
		has_content = true;
		alpha.direct( 1.f );
	}

	if ( const float h = ( static_cast< float >( lines.size( ) ) + 1.f ) * 24.f + 10.f; h != height.m_end ) {
		width.direct( max_width );
		height.direct( h );
	}

	if ( alpha.m_value > 0.f ) {
		const int a = static_cast< int >( 255.f * alpha.m_value );

		draw_list->PushClipRect( min, max, true );

		float offset = 32.f;

		for ( const auto& line : lines ) {
			if ( const auto avatar = line.m_fake_player ? nullptr : g_avatar_cache[ line.m_index ] )
				draw_list->AddImageRounded( avatar, ImVec2( min.x + 10.f, min.y + offset ), ImVec2( min.x + 30.f, min.y + offset + 20.f ),
				                            ImVec2( 0.f, 0.f ), ImVec2( 1.f, 1.f ), IM_COL32( 255, 255, 255, a ), 10.f );

			draw_list->AddText( font, font->FontSize, ImVec2( min.x + 38.f, min.y + offset ), IM_COL32( 255, 255, 255, a ), line.m_name.c_str( ) );
			offset += 24.f;
		}

		draw_list->PopClipRect( );
	}

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_evolve( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_evolve_16 ];
	const auto bold = g_render.m_fonts[ e_font_names::font_name_evolve_bold_16 ];

	if ( !font || !bold )
		return;

	const std::string brand = watermark_brand( ), label = watermark_label( );
	const std::string rest  = label.empty( ) ? std::string{ } : " | " + label;

	const float brand_width = imgui_text_width( bold, brand.c_str( ) );
	const float width       = std::floor( brand_width + imgui_text_width( font, rest.c_str( ) ) + 20.f );

	const ImVec2 min( static_cast< float >( g_ctx.m_width ) - width - 10.f, 10.f ), max( min.x + width, min.y + 25.f );

	g_watermark_box = ImVec4( min.x, min.y, max.x, max.y );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	evolve_shadow( draw_list, min, max, 12.f, evolve_max_alpha( ) * 0.25f );
	evolve_header( draw_list, min, width, 1.f );

	const ImVec4 accent = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );
	const float  text_y = std::floor( min.y + ( 24.f - bold->FontSize ) * 0.5f );

	draw_list->AddText( bold, bold->FontSize, ImVec2( min.x + 10.f, text_y ), ImColor( accent.x, accent.y, accent.z, 1.f ), brand.c_str( ) );
	draw_list->AddText( font, font->FontSize, ImVec2( min.x + 10.f + brand_width, text_y ), IM_COL32( evolve_text, evolve_text, evolve_text, 255 ),
	                    rest.c_str( ) );

	g_render.end_stretch_block( block );
}

namespace {
	void legendware_gradient( ImDrawList* list, const ImVec2 pos, const ImVec2 size, const int ( &rgb )[ 3 ], const int left, const int right )
	{
		const auto col = [ & ]( const int a ) { return IM_COL32( rgb[ 0 ], rgb[ 1 ], rgb[ 2 ], a ); };

		const ImVec2 end( pos.x + size.x, pos.y + size.y );

		list->AddRectFilledMultiColor( pos, end, col( left ), col( 0 ), col( 0 ), col( left ) );
		list->AddRectFilledMultiColor( pos, end, col( 0 ), col( right ), col( right ), col( 0 ) );
	}
}

void n_misc::impl_t::draw_watermark_legendware( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_legendware_verdana_12 ];

	if ( !font )
		return;

	std::time_t raw = std::time( nullptr );
	std::tm local   = { };
	localtime_s( &local, &raw );

	char clock[ 16 ] = { };
	std::strftime( clock, sizeof( clock ), "%H:%M:%S", &local );

	const std::string brand = watermark_brand( ), user = GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string );

	std::string text = std::format( "{} | {} | {}", brand, user, clock );

	if ( g_interfaces.m_engine_client->is_in_game( ) ) {
		if ( const auto net_channel = g_interfaces.m_engine_client->get_net_channel_info( ) ) {
			const char* address      = net_channel->get_address( );
			const std::string server = !address || !std::strcmp( address, "loopback" ) ? "Local server" : address;

			float latency = net_channel->get_avg_latency( FLOW_OUTGOING );

			if ( latency != 0.f ) {
				static auto cl_updaterate = g_convars[ HASH_BT( "cl_updaterate" ) ];

				if ( cl_updaterate && cl_updaterate->get_float( ) > 0.f )
					latency -= 0.5f / cl_updaterate->get_float( );
			}

			const int ping = static_cast< int >( ImMax( 0.f, latency ) * 1000.f );

			const float interval = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.f;
			const int tickrate   = interval > 0.f ? static_cast< int >( 1.f / interval ) : 0;

			text = std::format( "{} | {} | {} | {} ms | {} tick | {}", brand, user, server, ping, tickrate, clock );
		}
	}

	const int box_width = static_cast< int >( vgui_text_width( font, text ) ) + 10;
	const int half      = box_width / 2;
	const float x       = static_cast< float >( g_ctx.m_width - 10 - box_width );

	g_watermark_box = ImVec4( x, 10.f, x + static_cast< float >( box_width ), 30.f );

	const ImVec4 accent_v = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );
	const int accent[ 3 ] = { static_cast< int >( accent_v.x * 255.f ), static_cast< int >( accent_v.y * 255.f ), static_cast< int >( accent_v.z * 255.f ) };

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	for ( const float line_y : { 10.f, 29.f } ) {
		legendware_gradient( draw_list, ImVec2( x, line_y ), ImVec2( static_cast< float >( half ), 1.f ), accent, 170, 240 );
		legendware_gradient( draw_list, ImVec2( x + static_cast< float >( half ), line_y ), ImVec2( static_cast< float >( half ), 1.f ), accent, 240, 170 );
	}

	draw_list->AddRectFilled( ImVec2( x, 11.f ), ImVec2( x + static_cast< float >( box_width ), 29.f ), IM_COL32( 10, 10, 10, 150 ) );

	vgui_shadow_text( draw_list, font, ImVec2( x + 5.f, 13.f ), IM_COL32( 255, 255, 255, 220 ), text.c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_legendware( )
{
	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) )
		return;

	const auto font = g_render.m_fonts[ e_font_names::font_name_legendware_lucida_10 ];

	if ( !font )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true );
	std::erase_if( entries, []( const spectator_entry_t& entry ) { return !entry.m_watching_local; } );

	if ( entries.empty( ) )
		return;

	const float top = GET_VARIABLE( g_variables.m_watermark, bool ) ? 30.f : 6.f;

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	for ( std::size_t i = 0; i < entries.size( ); i++ ) {
		const float x = static_cast< float >( g_ctx.m_width ) - ( vgui_text_width( font, entries[ i ].m_name ) + 6.f );

		vgui_shadow_text( draw_list, font, ImVec2( x, top + 16.f * static_cast< float >( i ) ), IM_COL32( 255, 255, 255, 255 ),
		                  entries[ i ].m_name.c_str( ) );
	}

	g_render.end_stretch_block( block );
}

namespace {
	float interium_wm_hues[ 4 ] = { 0.f, 0.25f, 0.5f, 0.75f };

	ImU32 interium_hsv( const float hue, const int alpha )
	{
		float r = 0.f, g = 0.f, b = 0.f;
		ImGui::ColorConvertHSVtoRGB( hue, 1.f, 0.7f, r, g, b );

		return IM_COL32( static_cast< int >( r * 255.f ), static_cast< int >( g * 255.f ), static_cast< int >( b * 255.f ), alpha );
	}

	void interium_hues( float& left, float& right )
	{
		static float hue_left = 0.f, hue_right = 0.25f;
		static int last_frame = -1;

		if ( ImGui::GetFrameCount( ) != last_frame ) {
			last_frame = ImGui::GetFrameCount( );

			const float step = ImGui::GetIO( ).DeltaTime * 0.1f;

			for ( hue_left += step; hue_left > 1.f; hue_left -= 1.f ) { }
			for ( hue_right += step; hue_right > 1.f; hue_right -= 1.f ) { }
		}

		left  = hue_left;
		right = hue_right;
	}

	void interium_band( ImDrawList* list, const float x, const float y, const float width )
	{
		float left = 0.f, right = 0.f;
		interium_hues( left, right );

		list->AddRectFilledMultiColor( ImVec2( x, y + 11.f ), ImVec2( x + width, y + 22.f ), interium_hsv( left, 0 ), interium_hsv( right, 0 ),
		                               interium_hsv( right, 70 ), interium_hsv( left, 70 ) );
		list->AddRectFilledMultiColor( ImVec2( x, y + 22.f ), ImVec2( x + width, y + 24.f ), interium_hsv( left, 255 ), interium_hsv( right, 255 ),
		                               interium_hsv( right, 255 ), interium_hsv( left, 255 ) );
		list->AddRectFilledMultiColor( ImVec2( x + 2.f, y + 24.f ), ImVec2( x + width - 2.f, y + 29.f ), interium_hsv( left, 70 ),
		                               interium_hsv( right, 70 ), interium_hsv( right, 0 ), interium_hsv( left, 0 ) );
	}
}

void n_misc::impl_t::draw_spectator_list_interium( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_interium_droid_24 ];

	if ( !font )
		return;

	std::string list{ };

	if ( g_ctx.m_local && g_interfaces.m_engine_client->is_in_game( ) ) {
		const int local_index = g_ctx.m_local->get_index( );
		int target_index      = local_index;

		if ( !g_ctx.m_local->is_alive( ) ) {
			const auto target = reinterpret_cast< c_base_entity* >(
				g_interfaces.m_client_entity_list->get_client_entity_from_handle( g_ctx.m_local->get_observer_target_handle( ) ) );

			target_index = target ? target->get_index( ) : -1;
		}

		if ( target_index != -1 ) {
			std::vector< spectator_entry_t > entries{ };
			collect_spectators( entries, true );

			std::sort( entries.begin( ), entries.end( ), []( const spectator_entry_t& a, const spectator_entry_t& b ) { return a.m_index < b.m_index; } );

			for ( const auto& entry : entries ) {
				if ( entry.m_target_index != target_index || entry.m_index == local_index || entry.m_name.empty( ) )
					continue;

				if ( entry.m_obs_mode == e_obs_mode::obs_mode_in_eye || entry.m_obs_mode == e_obs_mode::obs_mode_chase )
					list += entry.m_name + "\n";
			}
		}
	}

	constexpr float width = 150.f;

	const float height = list.empty( ) ? 33.f : font->CalcTextSizeA( 16.f, FLT_MAX, 0.f, list.c_str( ) ).y + 31.f;

	drag_placed_panel( width, height );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	constexpr ImU32 white = IM_COL32( 255, 255, 255, 255 );

	const auto& bg = GET_VARIABLE( g_variables.m_spectators_interium_bg, c_color );

	draw_list->AddRectFilled( ImVec2( x, y ), ImVec2( x + width, y + 22.f ), bg.get_u32( ), 3.f, ImDrawFlags_RoundCornersTop );
	draw_list->AddRectFilled( ImVec2( x + 2.f, y + 22.f ), ImVec2( x + width - 2.f, y + height ), bg.get_u32( 127.f / 255.f ), 6.f,
	                          ImDrawFlags_RoundCornersBottom );

	constexpr auto title = "Spectator List";

	draw_list->AddText( font, 16.f, ImVec2( x + width * 0.5f - font->CalcTextSizeA( 16.f, FLT_MAX, 0.f, title ).x * 0.5f, y + 2.f ), white, title );

	interium_band( draw_list, x, y, width );

	if ( !list.empty( ) )
		draw_list->AddText( font, 16.f, ImVec2( x + 8.f, y + 26.f ), white, list.c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_interium( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_interium_droid_24 ];

	if ( !font )
		return;

	auto& hues = interium_wm_hues;

	const float step = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_time * 0.25f : 0.f;

	for ( float& hue : hues ) {
		hue += step;

		if ( hue > 1.f )
			hue -= 1.f;
	}

	const std::string label = watermark_label( );
	const std::string text  = label.empty( ) ? watermark_brand( ) : watermark_brand( ) + " | " + label;

	const ImVec2 text_size = font->CalcTextSizeA( 16.f, FLT_MAX, 0.f, text.c_str( ) );

	constexpr float x = 5.f, y = 5.f;

	const float right = x + text_size.x + 10.f, bottom = y + text_size.y + 8.f;

	g_watermark_box = ImVec4( x - 1.f, y - 1.f, right + 1.f, bottom + 1.f );

	const auto trunc = []( const float value ) { return static_cast< float >( static_cast< int >( value ) ); };

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddRectFilledMultiColor( ImVec2( x - 1.f, y - 1.f ), ImVec2( trunc( right + 1.f ), trunc( bottom + 1.f ) ), interium_hsv( hues[ 0 ], 100 ),
	                                    interium_hsv( hues[ 1 ], 100 ), interium_hsv( hues[ 2 ], 100 ), interium_hsv( hues[ 3 ], 100 ) );
	draw_list->AddRectFilledMultiColor( ImVec2( x, y ), ImVec2( trunc( right ), trunc( bottom ) ), interium_hsv( hues[ 0 ], 255 ),
	                                    interium_hsv( hues[ 1 ], 255 ), interium_hsv( hues[ 2 ], 255 ), interium_hsv( hues[ 3 ], 255 ) );
	draw_list->AddRectFilled( ImVec2( x + 1.f, y + 1.f ), ImVec2( trunc( right - 1.f ), trunc( bottom - 1.f ) ),
	                          GET_VARIABLE( g_variables.m_watermark_interium_bg, c_color ).get_u32( ) );

	draw_list->AddText( font, 16.f, ImVec2( 11.f, 10.f ), IM_COL32( 0, 0, 0, 255 ), text.c_str( ) );
	draw_list->AddText( font, 16.f, ImVec2( 10.f, 9.f ), IM_COL32( 255, 255, 255, 255 ), text.c_str( ) );

	g_render.end_stretch_block( block );
}

namespace {
	ImU32 accent_u32( const int alpha )
	{
		const ImVec4 accent = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );
		return IM_COL32( static_cast< int >( accent.x * 255.f ), static_cast< int >( accent.y * 255.f ), static_cast< int >( accent.z * 255.f ), alpha );
	}

	float text_width( ImFont* font, const float size, const std::string& text )
	{
		return font->CalcTextSizeA( size, FLT_MAX, 0.f, text.c_str( ) ).x;
	}

	bool panel_hovered( const float x, const float y, const float w, const float h )
	{
		const ImVec2 mouse = g_render.screen_mouse( );
		const ImVec2 shown = g_render.dpi_panel_pos( ImVec2( x, y ), ImVec2( w, h ) );
		const float scale  = g_render.m_dpi_panel_scale;

		return g_menu.m_opened && mouse.x >= shown.x && mouse.x <= shown.x + w * scale && mouse.y >= shown.y && mouse.y <= shown.y + h * scale;
	}
}

void n_misc::impl_t::draw_watermark_skebob( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_skebob_nunito_14 ];

	if ( !font || !g_interfaces.m_global_vars_base )
		return;

	static float frame_time   = 1.f;
	static std::string fps    = "0";
	static DWORD next_fps     = 0;
	frame_time                = frame_time * 0.9f + g_interfaces.m_global_vars_base->m_abs_frame_time * 0.1f;
	const DWORD now           = GetTickCount( );

	if ( now >= next_fps && frame_time > 0.f ) {
		fps      = std::to_string( static_cast< int >( 1.f / frame_time ) );
		next_fps = now + 1000;
	}

	const std::string brand = watermark_brand( );
	std::string shown       = brand;

	static bool was_on = false, typing = true;
	static int count   = 0;
	static DWORD next_char = 0, hold_until = 0;

	if ( GET_VARIABLE( g_variables.m_watermark_skebob_typing, bool ) ) {
		if ( !was_on ) {
			count      = 0;
			typing     = true;
			next_char  = now;
			hold_until = 0;
		}
		was_on = true;

		const int length = static_cast< int >( brand.size( ) );

		if ( typing ) {
			if ( now > next_char ) {
				++count;
				next_char = now + 150;

				if ( count > length ) {
					typing     = false;
					hold_until = now + 2000;
					count      = length;
				}
			}
			shown = brand.substr( 0, static_cast< std::size_t >( ImClamp( count, 0, length ) ) );
		} else if ( now > hold_until && now > next_char ) {
			--count;
			next_char = now + 100;

			if ( count < 0 ) {
				typing    = true;
				count     = 0;
				next_char = now;
			} else
				shown = brand.substr( 0, static_cast< std::size_t >( count ) );
		}
	} else
		was_on = false;

	const std::string user = GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string );

	const float size = font->FontSize;
	const float w_brand = text_width( font, size, shown ), w_user = text_width( font, size, user ), w_fps = text_width( font, size, fps ),
	            w_suffix = text_width( font, size, "fps" );

	const float width = w_brand + w_user + w_fps + w_suffix + 70.f;
	const float wx    = static_cast< float >( g_ctx.m_width ) - width;

	const ImVec2 min( wx + 10.f, 10.f ), max( wx + width - 5.f, 40.f );
	g_watermark_box = ImVec4( min.x, min.y, max.x, max.y );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddRectFilled( min, max, IM_COL32( 40, 40, 40, 150 ), 5.f );
	draw_list->AddRect( min, max, IM_COL32( 0, 0, 0, 200 ), 5.f, 0, 1.f );
	draw_list->AddRectFilled( ImVec2( min.x + 3.f, min.y + 3.f ), ImVec2( max.x - 3.f, max.y - 3.f ), IM_COL32( 20, 20, 20, 255 ), 5.f );
	draw_list->AddRect( ImVec2( min.x + 3.f, min.y + 3.f ), ImVec2( max.x - 3.f, max.y - 3.f ), IM_COL32( 0, 0, 0, 255 ), 5.f, 0, 1.f );

	constexpr ImU32 white = IM_COL32( 255, 255, 255, 255 );

	draw_list->AddText( font, size, ImVec2( wx + 20.f, 20.f ), accent_u32( 255 ), shown.c_str( ) );
	draw_list->AddText( font, size, ImVec2( wx + w_brand + 25.f, 20.f ), white, "|" );
	draw_list->AddText( font, size, ImVec2( wx + w_brand + 35.f, 20.f ), white, user.c_str( ) );
	draw_list->AddText( font, size, ImVec2( wx + w_brand + w_user + 40.f, 20.f ), white, "|" );
	draw_list->AddText( font, size, ImVec2( wx + w_brand + w_user + 50.f, 20.f ), white, fps.c_str( ) );
	draw_list->AddText( font, size, ImVec2( wx + w_brand + w_user + w_fps + 55.f, 20.f ), white, "fps" );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_skebob( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_skebob_arial_12 ], title_font = g_render.m_fonts[ e_font_names::font_name_skebob_arial_13 ];

	if ( !font || !title_font )
		return;

	struct row_t {
		std::string m_text;
		int m_index;
		int m_team;
		bool m_fake_player;
		bool m_highlight;
	};

	std::vector< row_t > rows{ };

	const bool show_target = GET_VARIABLE( g_variables.m_spectators_list_show_target, bool );
	const bool local_only  = !show_target || GET_VARIABLE( g_variables.m_spectators_list_type, int ) == 1;

	if ( g_ctx.m_local && g_interfaces.m_engine_client->is_in_game( ) ) {
		g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
			if ( !entity || entity->is_alive( ) || entity->is_dormant( ) )
				return;

			player_info_t info{ };
			if ( !g_interfaces.m_engine_client->get_player_info( entity->get_index( ), &info ) || info.m_is_hltv )
				return;

			const auto target =
				reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( entity->get_observer_target_handle( ) ) );

			row_t row{ info.m_name, entity->get_index( ), entity->get_team( ), info.m_fake_player, entity == g_ctx.m_local };

			if ( target ) {
				if ( !target->is_alive( ) )
					return;

				player_info_t target_info{ };
				g_interfaces.m_engine_client->get_player_info( target->get_index( ), &target_info );

				if ( show_target ) {
					row.m_text += " -> ";
					row.m_text += target_info.m_name;
				}
				row.m_highlight |= target == g_ctx.m_local;
			}

			if ( local_only && ( entity == g_ctx.m_local || target != g_ctx.m_local ) )
				return;

			rows.push_back( std::move( row ) );
		} );

		std::sort( rows.begin( ), rows.end( ), []( const row_t& a, const row_t& b ) { return a.m_index < b.m_index; } );
	}

	if ( rows.empty( ) && !GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) )
		return;

	const bool background = GET_VARIABLE( g_variables.m_spectators_skebob_background, bool );
	const bool avatars    = GET_VARIABLE( g_variables.m_spectators_avatar, bool );
	const bool title      = GET_VARIABLE( g_variables.m_spectators_skebob_title, bool );

	const float title_h = title ? 40.f : 20.f, row_h = avatars ? 25.f : 15.f, text_x = avatars ? 35.f : 10.f;

	float widest = title ? text_width( title_font, 12.f, "spectators" ) + 20.f : 0.f;
	for ( const auto& row : rows )
		widest = ImMax( widest, text_x + text_width( font, 12.f, row.m_text ) + 10.f );

	const float width  = ImMax( 210.f, widest );
	const float height = ( title ? 30.f : 10.f ) + static_cast< float >( rows.size( ) ) * row_h;

	drag_placed_panel( width, height );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	if ( background ) {
		draw_list->AddRectFilled( ImVec2( x + 1.f, y + 1.f ), ImVec2( x + width - 1.f, y + height - 1.f ), IM_COL32( 19, 19, 19, 242 ), 5.f );
		draw_list->AddRect( ImVec2( x + 1.f, y + 1.f ), ImVec2( x + width - 1.f, y + height - 1.f ),
		                    accent_u32( panel_hovered( x, y, width, height ) ? 255 : 200 ), 5.f, 0, 1.f );
	}

	const auto round_px = []( const float value ) { return std::floor( value + 0.5f ); };

	if ( title ) {
		const float tx = x + 3.f + ( width - 3.f ) / 2.f - text_width( title_font, 12.f, "spectators" ) / 2.f, ty = y + 10.f;

		draw_list->AddText( title_font, 12.f, ImVec2( round_px( tx + 1.f ), round_px( ty + 1.f ) ), IM_COL32( 0, 0, 0, 255 ), "spectators" );
		draw_list->AddText( title_font, 12.f, ImVec2( round_px( tx ), round_px( ty ) ), IM_COL32( 255, 255, 255, 255 ), "spectators" );
	}

	constexpr ImVec2 diagonals[ 4 ] = { { 1.f, 1.f }, { -1.f, -1.f }, { 1.f, -1.f }, { -1.f, 1.f } };

	for ( std::size_t i = 0; i < rows.size( ); ++i ) {
		const float row_y = y + title_h - 3.f + static_cast< float >( i ) * row_h;

		if ( avatars )
			if ( const auto avatar = spectator_avatar_texture( rows[ i ].m_index, rows[ i ].m_team, rows[ i ].m_fake_player ) )
				draw_list->AddImageRounded( avatar, ImVec2( x + 10.f, row_y - 10.f ), ImVec2( x + 30.f, row_y + 10.f ), ImVec2( 0.f, 0.f ), ImVec2( 1.f, 1.f ),
				                            IM_COL32_WHITE, 10.f );

		const float tx = x + text_x, ty = row_y - 6.f;

		for ( const auto& offset : diagonals )
			draw_list->AddText( font, 12.f, ImVec2( round_px( tx + offset.x ), round_px( ty + offset.y ) ), IM_COL32( 0, 0, 0, 255 ), rows[ i ].m_text.c_str( ) );

		draw_list->AddText( font, 12.f, ImVec2( round_px( tx ), round_px( ty ) ), rows[ i ].m_highlight ? accent_u32( 255 ) : IM_COL32( 255, 255, 255, 255 ),
		                    rows[ i ].m_text.c_str( ) );
	}

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_cumidere( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_cumidere_tahoma_16 ];

	if ( !font )
		return;

	static float fps = 0.f, last = 0.f;
	const ImGuiIO& io = ImGui::GetIO( );

	if ( static_cast< float >( ImGui::GetTime( ) ) - last >= 0.5f ) {
		fps  = fps * 0.7f + 0.3f / ImMax( io.DeltaTime, 0.0001f );
		last = static_cast< float >( ImGui::GetTime( ) );
	}

	const int label_type    = GET_VARIABLE( g_variables.m_watermark_label, int );
	const std::string brand = watermark_brand( );
	const std::string number = label_type == 1 ? std::to_string( static_cast< int >( fps ) ) : watermark_label( );
	const std::string suffix = label_type == 1 ? " fps" : "";
	const std::string sep    = number.empty( ) ? "" : " | ";

	const float size  = font->FontSize;
	const float w_b   = text_width( font, size, brand ), w_sep = text_width( font, size, sep ), w_n = text_width( font, size, number ),
	            w_fps = text_width( font, size, suffix );

	const float right = static_cast< float >( g_ctx.m_width ) - 10.f;
	const float left  = right - ( w_b + w_sep + w_n + w_fps + 16.f );
	const float bottom = size + 20.f;

	g_watermark_box = ImVec4( left, 10.f, right, bottom );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const ImU32 accent = accent_u32( 255 );

	draw_list->AddRectFilled( ImVec2( left, 10.f ), ImVec2( right, bottom ), IM_COL32( 23, 23, 23, 255 ) );
	draw_list->AddLine( ImVec2( left, 10.f ), ImVec2( right, 10.f ), accent, 1.f );
	draw_list->AddLine( ImVec2( left, 10.f ), ImVec2( left, bottom ), accent, 1.f );
	draw_list->AddLine( ImVec2( right, 10.f ), ImVec2( right, bottom ), accent, 1.f );

	float x = left + 8.f;
	draw_list->AddText( font, size, ImVec2( x, 15.f ), accent, brand.c_str( ) );
	x += w_b;
	draw_list->AddText( font, size, ImVec2( x, 15.f ), IM_COL32( 27, 27, 27, 255 ), sep.c_str( ) );
	x += w_sep;
	draw_list->AddText( font, size, ImVec2( x, 15.f ), IM_COL32( 208, 208, 208, 255 ), number.c_str( ) );
	x += w_n;
	draw_list->AddText( font, size, ImVec2( x, 15.f ), IM_COL32( 136, 136, 136, 255 ), suffix.c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_cumidere( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_cumidere_tahoma_16 ];

	if ( !font || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true );

	const bool show_target = GET_VARIABLE( g_variables.m_spectators_list_show_target, bool );
	const bool local_only  = !show_target || GET_VARIABLE( g_variables.m_spectators_list_type, int ) == 1;

	if ( local_only )
		std::erase_if( entries, []( const spectator_entry_t& entry ) { return !entry.m_watching_local; } );

	if ( entries.empty( ) && !GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) )
		return;

	std::sort( entries.begin( ), entries.end( ), []( const spectator_entry_t& a, const spectator_entry_t& b ) { return a.m_index < b.m_index; } );

	const float size  = font->FontSize;
	const float row_h = size + 4.f, header = size + 8.f;

	std::vector< std::string > texts{ };
	float widest = 0.f;

	for ( const auto& entry : entries ) {
		texts.push_back( show_target ? entry.m_name + " -> " + entry.m_target_name : entry.m_name );
		widest = ImMax( widest, text_width( font, size, texts.back( ) ) );
	}

	const float title_w = text_width( font, size, "spectators" );
	const float width   = ImMax( ImMax( widest, title_w ), title_w * 2.f ) + 16.f;
	const float height  = static_cast< float >( entries.size( ) ) * row_h + header + 1.f;

	drag_placed_panel( width, height );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const ImU32 accent = accent_u32( 255 );

	draw_list->AddRectFilled( ImVec2( x, y ), ImVec2( x + width, y + height ), IM_COL32( 23, 23, 23, 255 ) );
	draw_list->AddLine( ImVec2( x, y ), ImVec2( x + width, y ), accent, 1.f );
	draw_list->AddLine( ImVec2( x, y ), ImVec2( x, y + height ), accent, 1.f );
	draw_list->AddLine( ImVec2( x + width, y ), ImVec2( x + width, y + height ), accent, 1.f );

	draw_list->AddText( font, size, ImVec2( x + 8.f, y + 4.f ), accent, "spectators" );
	draw_list->AddLine( ImVec2( x, y + header ), ImVec2( x + width, y + header ), accent, 1.f );

	const c_color& one = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color );
	const c_color& two = GET_VARIABLE( g_variables.m_spectators_list_text_color_two, c_color );

	for ( std::size_t i = 0; i < entries.size( ); ++i )
		draw_list->AddText( font, size, ImVec2( x + 8.f, y + header + 1.f + static_cast< float >( i ) * row_h ),
		                    ( entries[ i ].m_watching_local ? one : two ).get_u32( ), texts[ i ].c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_illusory( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_illusory_tahoma_13 ];

	if ( !font || !g_interfaces.m_global_vars_base )
		return;

	static float fps = 0.f, last = 0.f;
	const float now  = g_interfaces.m_global_vars_base->m_real_time;

	if ( now - last >= 0.7f && g_interfaces.m_global_vars_base->m_abs_frame_time > 0.f ) {
		fps  = fps * 0.7f + ( 1.f / g_interfaces.m_global_vars_base->m_abs_frame_time ) * 0.3f;
		last = now;
	}

	const std::string label = GET_VARIABLE( g_variables.m_watermark_label, int ) == 1 ? std::to_string( static_cast< int >( fps ) ) + " fps" : watermark_label( );
	const std::string text  = label.empty( ) ? watermark_brand( ) : watermark_brand( ) + " | " + label;
	const ImVec2 size      = font->CalcTextSizeA( 13.f, FLT_MAX, 0.f, text.c_str( ) );
	const float w          = static_cast< float >( static_cast< int >( g_ctx.m_width ) );

	g_watermark_box = ImVec4( w - size.x - 18.f, 4.f, w - 4.f, size.y + 16.f );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddRectFilled( ImVec2( w - size.x - 17.f, 5.f ), ImVec2( w - 5.f, size.y + 15.f ), IM_COL32( 28, 28, 28, 255 ), 5.5f );
	draw_list->AddRect( ImVec2( w - size.x - 18.f, 4.f ), ImVec2( w - 4.f, size.y + 16.f ), IM_COL32( 56, 56, 56, 128 ), 5.5f, 0, 1.f );

	draw_list->AddText( font, 13.f, ImVec2( w - size.x - 10.f, 10.f ), accent_u32( 230 ), text.c_str( ) );
	draw_list->AddText( font, 13.f, ImVec2( w - size.x - 11.f, 10.f ), IM_COL32( 255, 255, 255, 255 ), text.c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_illusory( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ];

	if ( !font || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true );

	const bool show_target = GET_VARIABLE( g_variables.m_spectators_list_show_target, bool );

	if ( !show_target )
		std::erase_if( entries, []( const spectator_entry_t& entry ) { return !entry.m_watching_local; } );

	std::sort( entries.begin( ), entries.end( ), []( const spectator_entry_t& a, const spectator_entry_t& b ) { return a.m_index < b.m_index; } );

	const bool track_line = GET_VARIABLE( g_variables.m_media_player, bool ) && g_media_player.m_has_media;
	const float top       = ( GET_VARIABLE( g_variables.m_watermark, bool ) ? 30.f : 5.f ) + ( track_line ? 15.f : 0.f );
	const int w           = static_cast< int >( g_ctx.m_width );

	const c_color& one = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color );
	const c_color& two = GET_VARIABLE( g_variables.m_spectators_list_text_color_two, c_color );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	for ( std::size_t i = 0; i < entries.size( ); ++i ) {
		const std::string text = show_target ? entries[ i ].m_name + " -> " + entries[ i ].m_target_name : entries[ i ].m_name;
		const int tw           = static_cast< int >( text_width( font, 12.f, text ) );
		const float y          = top + 16.f * static_cast< float >( i );

		draw_list->AddText( font, 12.f, ImVec2( static_cast< float >( w - tw - 5 ), y + 1.f ), IM_COL32( 0, 0, 0, 255 ), text.c_str( ) );
		draw_list->AddText( font, 12.f, ImVec2( static_cast< float >( w - 6 - tw ), y ), ( entries[ i ].m_watching_local ? one : two ).get_u32( ),
		                    text.c_str( ) );
	}

	g_render.end_stretch_block( block );
}

namespace {
	void lumi_glow( ImDrawList* list, const ImVec2 min, const ImVec2 max, const float fade )
	{
		const float radius  = ImMax( GET_VARIABLE( g_variables.m_watermark_lumi_glow_radius, float ), 1.f );
		const float opacity = GET_VARIABLE( g_variables.m_watermark_lumi_glow_opacity, float );
		const float falloff = -1.25f / ( radius * 0.4f * radius );

		for ( int i = 0; i < 7; ++i ) {
			const float d    = static_cast< float >( i - 3 );
			const int kernel = static_cast< int >( std::exp( 2.f * d * d * falloff ) * 255.f ) & 0xff;
			const float a    = static_cast< float >( kernel ) * opacity * fade;

			if ( a < 0.5f )
				continue;

			const float grow = radius * 0.33333334f * static_cast< float >( i );
			list->AddRectFilled( ImVec2( min.x - grow, min.y - grow ), ImVec2( max.x + grow, max.y + grow ), accent_u32( ImMin( static_cast< int >( a ), 255 ) ),
			                     radius * 0.1f * static_cast< float >( i ) + 5.f );
		}
	}

	struct lumi_streak_t {
		float x = 0.f, y = 0.f, speed = 0.f, len = 0.f, alpha = 0.f;
	};

	std::uint32_t lumi_seed = 0x51a9c37du;

	float lumi_rand( const float scale, const float base )
	{
		lumi_seed = lumi_seed * 0x19660du + 0x3c6ef35fu;
		return static_cast< float >( lumi_seed >> 8 ) * scale + base;
	}

	void lumi_rain( ImDrawList* list, const ImVec2 min, const ImVec2 max )
	{
		static lumi_streak_t streaks[ 16 ];
		static bool seeded = false;

		if ( !seeded ) {
			for ( auto& s : streaks ) {
				s.x     = lumi_rand( 5.960465e-08f, 0.f );
				s.y     = lumi_rand( 5.960465e-08f, 0.f );
				s.speed = lumi_rand( 3.874302e-08f, 0.35f );
				s.len   = lumi_rand( 1.31130236e-08f, 0.1f );
				s.alpha = lumi_rand( 2.2649768e-08f, 0.32f );
			}
			seeded = true;
		}

		float dt = ImGui::GetIO( ).DeltaTime;
		if ( dt <= 0.f )
			dt = 0.016f;
		dt = ImMin( dt, 0.033f );

		const ImVec2 clip_min( min.x + 1.2f, min.y + 1.2f ), clip_max( max.x - 1.2f, max.y - 1.2f );
		if ( clip_min.x >= clip_max.x || clip_min.y >= clip_max.y )
			return;

		list->PushClipRect( clip_min, clip_max, true );

		const float w = max.x - min.x, h = max.y - min.y;
		const ImVec4 accent = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );

		for ( auto& s : streaks ) {
			s.y += dt * s.speed * 0.55f;

			if ( s.y > 1.15f ) {
				s.y     = -s.len;
				s.x     = lumi_rand( 5.960465e-08f, 0.f );
				s.speed = lumi_rand( 3.874302e-08f, 0.35f );
				s.len   = lumi_rand( 1.31130236e-08f, 0.1f );
				s.alpha = lumi_rand( 2.2649768e-08f, 0.32f );
			}

			const float x  = w * s.x + min.x;
			const float y0 = s.y * h + min.y, y1 = h * s.len + y0;

			if ( min.y <= y1 && y0 <= max.y )
				list->AddLine( ImVec2( x, ImMax( y0, min.y ) ), ImVec2( x, ImMin( y1, max.y ) ),
				               IM_COL32( static_cast< int >( accent.x * 255.f ), static_cast< int >( accent.y * 255.f ), static_cast< int >( accent.z * 255.f ),
				                         static_cast< int >( s.alpha * 255.f ) ),
				               1.f );
		}

		list->PopClipRect( );
	}
}

void n_misc::impl_t::draw_watermark_lumi( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_lumi_rubik_16 ];

	if ( !font )
		return;

	const ImGuiIO& io = ImGui::GetIO( );
	const float now   = static_cast< float >( ImGui::GetTime( ) );

	static float smooth_fps = 0.f, last_fps = 0.f;
	static int shown_fps    = 0;
	smooth_fps              = smooth_fps * 0.9f + io.Framerate * 0.100000024f;

	if ( now - last_fps >= 0.3f ) {
		last_fps  = now;
		shown_fps = static_cast< int >( smooth_fps + 0.5f );
	}

	std::vector< std::string > elements{ };

	if ( GET_VARIABLE( g_variables.m_watermark_user, bool ) )
		elements.push_back( GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string ) );

	/* as styles 0 / 2: in game only, outside it the interval is stale */
	if ( GET_VARIABLE( g_variables.m_watermark_tickrate, bool ) && g_interfaces.m_engine_client->is_in_game( ) && g_interfaces.m_global_vars_base &&
	     g_interfaces.m_global_vars_base->m_interval_per_tick > 0.f )
		elements.push_back( std::to_string( static_cast< int >( 1.f / g_interfaces.m_global_vars_base->m_interval_per_tick + 0.5f ) ) + " tick" );

	if ( GET_VARIABLE( g_variables.m_watermark_time, bool ) ) {
		std::time_t raw = std::time( nullptr );
		std::tm local   = { };
		localtime_s( &local, &raw );

		char clock[ 16 ] = { };
		std::strftime( clock, sizeof( clock ), "%H:%M", &local );
		elements.emplace_back( clock );
	}

	if ( GET_VARIABLE( g_variables.m_watermark_fps, bool ) )
		elements.push_back( std::to_string( shown_fps ) + " fps" );

	std::string title = watermark_brand( );

	static bool type_was_on = false, typed = false;
	static float type_start = 0.f, type_shown = 1.f;
	static std::string type_title{ };
	const bool animated = GET_VARIABLE( g_variables.m_watermark_lumi_animated_title, bool );

	if ( animated ) {
		if ( !type_was_on || title != type_title ) {
			type_title = title;
			type_shown = 1.f;
			typed      = false;
			type_start = now;
		}

		/* utf-8 characters, the cut never splits one */
		std::size_t characters = 0;
		for ( const char c : title )
			characters += ( static_cast< unsigned char >( c ) & 0xc0 ) != 0x80;

		const float length = static_cast< float >( characters );

		if ( !typed ) {
			float target = ( now - type_start ) * 6.6666665f + 1.f;

			if ( length <= target ) {
				target = length;

				if ( length - 0.1f <= type_shown ) {
					typed      = true;
					type_start = now;
				}
			}

			const float dt = io.DeltaTime > 0.f ? io.DeltaTime : 0.016f;
			type_shown     = ( 1.f - std::exp( -12.f * dt ) ) * ( target - type_shown ) + type_shown;
		} else {
			type_shown = length;

			if ( now - type_start >= 2.f ) {
				typed      = false;
				type_shown = 1.f;
				type_start = now;
			}
		}

		title = utf8_truncate( title, ImMin( characters, static_cast< std::size_t >( std::ceil( ImMax( type_shown, 0.f ) ) ) ) );
	}

	type_was_on = animated;

	const float size    = font->FontSize;
	const float title_w = text_width( font, size, title ), sep_w = text_width( font, size, " / " );
	const int design    = GET_VARIABLE( g_variables.m_watermark_lumi_design, int );

	const float gap = design == 1 ? 6.f : 27.f;

	float content = title_w;
	if ( !elements.empty( ) ) {
		content += gap + static_cast< float >( elements.size( ) - 1 ) * sep_w;
		for ( const auto& element : elements )
			content += text_width( font, size, element );
	}

	static float width = 0.f, last_ease = -10.f;
	const float target = content + 12.f;

	width     = now - last_ease > 1.f || std::abs( width - target ) > 200.f ? target : ( target - width ) * io.DeltaTime * 8.f + width;
	last_ease = now;

	const float height = size + 12.f;
	const float x      = static_cast< float >( g_ctx.m_width ) - width - 10.f + GET_VARIABLE( g_variables.m_watermark_lumi_offset_x, float );
	const float y      = 10.f + GET_VARIABLE( g_variables.m_watermark_lumi_offset_y, float );

	g_watermark_box = ImVec4( x, y, x + width, y + height );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const ImU32 accent = accent_u32( 255 );

	static float glow = 0.f;
	const float real  = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;
	const bool glowing =
		GET_VARIABLE( g_variables.m_watermark_lumi_glow, bool ) && g_trick_time >= 0.f && real >= g_trick_time && real - g_trick_time < 1.5f;

	glow = ImClamp( glow + io.DeltaTime * ( glowing ? 4.f : -4.f ), 0.f, 1.f );

	if ( glow > 0.f )
		lumi_glow( draw_list, ImVec2( x, y ), ImVec2( x + width, y + height ), glow );

	draw_list->AddRectFilled( ImVec2( x, y ), ImVec2( x + width, y + height ), IM_COL32( 20, 20, 20, 255 ), 5.f );

	if ( design == 1 )
		lumi_rain( draw_list, ImVec2( x, y ), ImVec2( x + width, y + height ) );

	if ( !GET_VARIABLE( g_variables.m_watermark_lumi_no_border, bool ) )
		draw_list->AddRect( ImVec2( x, y ), ImVec2( x + width, y + height ), IM_COL32( 0, 0, 0, 255 ), 0.f, 0, 1.f );

	if ( design != 1 ) {
		const float stripe_x = x + title_w + 11.f;
		for ( int i = 15; i < 18; ++i ) {
			const float sx = stripe_x + static_cast< float >( i );
			draw_list->AddLine( ImVec2( sx, y + 4.f ), ImVec2( sx - 12.f, y + height - 5.f ), accent, 1.f );
			draw_list->AddLine( ImVec2( sx - 5.f, y + 4.f ), ImVec2( sx - 17.f, y + height - 5.f ), accent, 1.f );
		}
	}

	ImU32 shadow = IM_COL32( 0, 0, 0, 255 );
	if ( GET_VARIABLE( g_variables.m_watermark_lumi_shadow_color, bool ) && glow > 0.f ) {
		const ImVec4 a = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );
		shadow         = IM_COL32( static_cast< int >( a.x * glow * 255.f + 0.5f ), static_cast< int >( a.y * glow * 255.f + 0.5f ),
		                           static_cast< int >( a.z * glow * 255.f + 0.5f ), 255 );
	}

	float tx       = ( width - content ) * 0.5f + x;
	const float ty = std::floor( ( height - size ) * 0.5f + y );

	const auto text = [ & ]( const float at, const ImU32 color, const char* string ) {
		draw_list->AddText( font, size, ImVec2( at + 1.f, ty + 1.f ), shadow, string );
		draw_list->AddText( font, size, ImVec2( at, ty ), color, string );
	};

	text( tx, GET_VARIABLE( g_variables.m_watermark_lumi_title_color, c_color ).get_u32( ), title.c_str( ) );
	tx += title_w + gap;

	for ( std::size_t i = 0; i < elements.size( ); ++i ) {
		if ( i > 0 ) {
			text( tx, accent, " / " );
			tx += sep_w;
		}

		text( tx, accent, elements[ i ].c_str( ) );
		tx += text_width( font, size, elements[ i ] );
	}

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_lumi( )
{
	const int type = GET_VARIABLE( g_variables.m_spectators_lumi_type, int );

	if ( type == 1 )
		return draw_spectator_list_lumi_new( );

	if ( type == 2 )
		return draw_spectator_list_lumi_interwebz( );

	const auto font = g_render.m_fonts[ e_font_names::font_name_lumi_rubik_16 ];

	struct row_t {
		std::string m_text;
		int m_index       = 0;
		int m_team        = 0;
		bool m_fake       = false;
		bool m_local      = false;
		bool m_seen       = false;
		float m_progress  = 0.f;
	};

	static std::vector< row_t > rows{ };

	if ( !font || !g_interfaces.m_engine_client->is_in_game( ) ) {
		rows.clear( );
		return;
	}

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true, !GET_VARIABLE( g_variables.m_spectators_lumi_hide_gotv, bool ) );

	const bool show_target = GET_VARIABLE( g_variables.m_spectators_list_show_target, bool );

	for ( auto& row : rows )
		row.m_seen = false;

	for ( const auto& entry : entries ) {
		auto row = std::find_if( rows.begin( ), rows.end( ), [ & ]( const row_t& r ) { return r.m_index == entry.m_index; } );
		if ( row == rows.end( ) ) {
			rows.push_back( { } );
			row          = rows.end( ) - 1;
			row->m_index = entry.m_index;
		}

		row->m_text  = show_target ? entry.m_name + "   >   " + entry.m_target_name : entry.m_name;
		row->m_team  = entry.m_team;
		row->m_fake  = entry.m_fake_player;
		row->m_local = entry.m_watching_local;
		row->m_seen  = true;
	}

	const float step = ImGui::GetIO( ).DeltaTime * 4.f;
	for ( auto& row : rows )
		row.m_progress = ImClamp( row.m_progress + ( row.m_seen ? step : -step ), 0.f, 1.f );

	std::erase_if( rows, []( const row_t& row ) { return !row.m_seen && row.m_progress <= 0.f; } );

	if ( rows.empty( ) && !GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) )
		return;

	const bool avatars = GET_VARIABLE( g_variables.m_spectators_avatar, bool );
	const float size   = font->FontSize;
	const float pad    = avatars ? 20.f : 17.f;

	float widest = std::ceil( text_width( font, size, "Spectators" ) );
	for ( const auto& row : rows )
		widest = ImMax( widest, std::ceil( text_width( font, size, row.m_text ) ) + pad );

	const float width = ImMax( 200.f, widest + 30.f );

	float rows_h = 0.f;
	if ( rows.empty( ) )
		rows_h = 17.f;
	else
		for ( std::size_t i = 0; i < rows.size( ); ++i )
			rows_h += rows[ i ].m_progress * 17.f + ( i + 1 < rows.size( ) ? 1.f : 0.f );

	const float height = size + 17.f + rows_h;

	drag_placed_panel( width, height );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const bool blur      = GET_VARIABLE( g_variables.m_spectators_lumi_blur, bool );
	const bool blurred   = blur && queue_airflow_blur( draw_list );
	const auto block     = g_render.begin_stretch_block( draw_list );

	if ( GET_VARIABLE( g_variables.m_spectators_lumi_glow, bool ) )
		draw_list->AddRectFilled( ImVec2( x + 3.f, y + 3.f ), ImVec2( x + width + 3.f, y + height + 3.f ), IM_COL32( 0, 0, 0, 128 ), 5.f );

	airflow_blur_rect( draw_list, blurred, ImVec2( x, y ), ImVec2( x + width, y + height ), IM_COL32_WHITE, 5.f, ImDrawFlags_RoundCornersAll );
	draw_list->AddRectFilled( ImVec2( x, y ), ImVec2( x + width, y + height ), IM_COL32( 13, 13, 13, blur ? 210 : 255 ), 5.f );

	const float title_w = std::ceil( text_width( font, size, "Spectators" ) );
	draw_list->AddText( font, size, ImVec2( x + ( width - title_w ) * 0.5f, y + 4.f ), IM_COL32( 255, 255, 255, 255 ), "Spectators" );

	float row_y = y + size + 12.f;

	if ( rows.empty( ) )
		draw_list->AddText( font, size, ImVec2( x + 10.f, row_y ), IM_COL32( 100, 100, 100, 255 ), "No spectators..." );

	const ImU32 local_color = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color ).get_u32( );

	for ( const auto& row : rows ) {
		const float row_h = row.m_progress * 17.f;

		if ( row_h > 0.f ) {
			draw_list->PushClipRect( ImVec2( x + 10.f, row_y ), ImVec2( x + width - 10.f, row_y + row_h ), true );

			float text_x = x + 10.f;

			if ( avatars ) {
				const ImVec2 min( text_x, row_y + 1.5f ), max( text_x + 14.f, row_y + 15.5f );

				if ( const auto avatar = spectator_avatar_texture( row.m_index, row.m_team, row.m_fake ) )
					draw_list->AddImageRounded( avatar, min, max, ImVec2( 0.f, 0.f ), ImVec2( 1.f, 1.f ), IM_COL32_WHITE, 7.f );
				else {
					draw_list->AddRectFilled( min, max, IM_COL32( 70, 70, 70, 255 ), 7.f );
					draw_list->AddRect( min, max, IM_COL32( 0, 0, 0, 220 ), 7.f, 0, 1.f );
				}

				text_x += 20.f;
			}

			draw_list->AddText( font, size, ImVec2( text_x, row_y ), row.m_local ? local_color : IM_COL32( 170, 170, 170, 255 ), row.m_text.c_str( ) );
			draw_list->PopClipRect( );
		}

		row_y += row_h + 1.f;
	}

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_lumi_new( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_lumi_rubik_16 ];

	if ( !font || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, false, !GET_VARIABLE( g_variables.m_spectators_lumi_hide_gotv, bool ) );

	if ( entries.empty( ) && !GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) )
		return;

	const bool avatars = GET_VARIABLE( g_variables.m_spectators_avatar, bool );
	const float size   = font->FontSize;
	const float pad    = avatars ? 20.f : 17.f;

	std::vector< std::string > texts{ };
	float widest = 120.f;

	for ( const auto& entry : entries ) {
		texts.push_back( entry.m_name + "   >   " + entry.m_target_name );
		widest = ImMax( widest, text_width( font, size, texts.back( ) ) + pad );
	}

	const float width  = widest + 30.f;
	const float height = ( entries.empty( ) ? 22.f : static_cast< float >( entries.size( ) ) * 22.f ) + 40.f;

	drag_placed_panel( width, height );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const bool blur      = GET_VARIABLE( g_variables.m_spectators_lumi_blur, bool );
	const bool blurred   = blur && queue_airflow_blur( draw_list );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const ImVec2 min( x, y ), max( x + width, y + height );

	airflow_blur_rect( draw_list, blurred, min, max, IM_COL32_WHITE, 6.f, ImDrawFlags_RoundCornersAll );
	draw_list->AddRectFilled( min, max, IM_COL32( 16, 16, 16, blur ? 210 : 255 ), 6.f );
	draw_list->AddRect( min, max, IM_COL32( 0, 0, 0, 255 ), 6.f, 0, 1.f );

	const float head_y      = y + ( 28.f - size ) * 0.5f;
	const std::string count = "(" + std::to_string( entries.size( ) ) + ")";

	draw_list->AddText( font, size, ImVec2( x + 10.f, head_y ), IM_COL32( 255, 255, 255, 255 ), "spectators" );
	draw_list->AddText( font, size, ImVec2( x + width - 10.f - text_width( font, size, count ), head_y ), accent_u32( 255 ), count.c_str( ) );
	draw_list->AddLine( ImVec2( x + 10.f, y + 28.f ), ImVec2( x + width - 10.f, y + 28.f ), IM_COL32( 36, 36, 36, 255 ), 1.f );

	const float rows_y  = y + 34.f;
	const float text_dy = ( 22.f - size ) * 0.5f;

	if ( entries.empty( ) )
		draw_list->AddText( font, size, ImVec2( x + ( width - text_width( font, size, "no spectators" ) ) * 0.5f, rows_y + text_dy ),
		                    IM_COL32( 170, 170, 170, 255 ), "no spectators" );

	const ImU32 local_color = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color ).get_u32( );

	for ( std::size_t i = 0; i < entries.size( ); ++i ) {
		const auto& entry = entries[ i ];
		const float row_y = rows_y + static_cast< float >( i ) * 22.f;
		float text_x      = x + 10.f;

		if ( avatars ) {
			const ImVec2 a_min( text_x, row_y + 4.f ), a_max( text_x + 14.f, row_y + 18.f );

			if ( const auto avatar = spectator_avatar_texture( entry.m_index, entry.m_team, entry.m_fake_player ) )
				draw_list->AddImageRounded( avatar, a_min, a_max, ImVec2( 0.f, 0.f ), ImVec2( 1.f, 1.f ), IM_COL32_WHITE, 7.f );
			else {
				draw_list->AddRectFilled( a_min, a_max, IM_COL32( 70, 70, 70, 255 ), 7.f );
				draw_list->AddRect( a_min, a_max, IM_COL32( 0, 0, 0, 220 ), 7.f, 0, 1.f );
			}

			text_x += 20.f;
		}

		draw_list->AddText( font, size, ImVec2( text_x, row_y + text_dy ), entry.m_watching_local ? local_color : IM_COL32( 170, 170, 170, 255 ),
		                    texts[ i ].c_str( ) );
	}

	g_render.end_stretch_block( block );

	if ( !GET_VARIABLE( g_variables.m_spectators_lumi_mascot, bool ) )
		return;

	const auto mascot = file_image_frame( lumi_mascot, GET_VARIABLE( g_variables.m_spectators_lumi_mascot_path, std::string ) );

	if ( !mascot )
		return;

	const float side = GET_VARIABLE( g_variables.m_spectators_lumi_mascot_size, float );
	auto& offset_x   = GET_VARIABLE( g_variables.m_spectators_lumi_mascot_x, float );
	auto& offset_y   = GET_VARIABLE( g_variables.m_spectators_lumi_mascot_y, float );

	offset_x = ImClamp( offset_x, -100.f, 100.f );
	offset_y = ImClamp( offset_y, -150.f, 150.f );

	const ImVec2 at( x + ( width - side ) * 0.5f + offset_x, y - side + offset_y );

	static bool dragging = false;
	static ImVec2 grab_mouse{ }, grab_offset{ };

	if ( g_menu.m_opened ) {
		const ImVec2 mouse = g_render.screen_mouse( );
		const ImVec2 shown = g_render.dpi_panel_pos( at, ImVec2( side, side ) );
		const float scale  = g_render.m_dpi_panel_scale;

		if ( !ImGui::IsMouseDown( ImGuiMouseButton_::ImGuiMouseButton_Left ) )
			dragging = false;
		else if ( !dragging && ImGui::IsMouseClicked( ImGuiMouseButton_::ImGuiMouseButton_Left ) && mouse.x >= shown.x &&
		          mouse.x <= shown.x + side * scale && mouse.y >= shown.y && mouse.y <= shown.y + side * scale ) {
			dragging    = true;
			grab_mouse  = mouse;
			grab_offset = ImVec2( offset_x, offset_y );
		}

		if ( dragging && scale > 0.f ) {
			offset_x = ImClamp( grab_offset.x + ( mouse.x - grab_mouse.x ) / scale, -100.f, 100.f );
			offset_y = ImClamp( grab_offset.y + ( mouse.y - grab_mouse.y ) / scale, -150.f, 150.f );
		}
	} else
		dragging = false;

	const auto foreground = ImGui::GetForegroundDrawList( );
	const auto top_block  = g_render.begin_stretch_block( foreground );

	foreground->AddImage( mascot, at, ImVec2( at.x + side, at.y + side ) );

	g_render.end_stretch_block( top_block );
}

void n_misc::impl_t::draw_spectator_list_lumi_interwebz( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_lumi_rubik_16 ];
	auto title_font = g_render.m_fonts[ e_font_names::font_name_lumi_proggy_13 ];

	if ( !title_font )
		title_font = font;

	if ( !font || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true, !GET_VARIABLE( g_variables.m_spectators_lumi_hide_gotv, bool ) );

	if ( entries.empty( ) && !GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) )
		return;

	const bool show_target = GET_VARIABLE( g_variables.m_spectators_list_show_target, bool );
	const float size       = font->FontSize;

	const auto width_of = [ & ]( ImFont* f, const std::string& text ) { return std::ceil( text_width( f, f->FontSize, text ) ); };

	struct row_t {
		std::string m_name, m_sep, m_target;
		float m_name_w = 0.f, m_sep_w = 0.f, m_target_w = 0.f;
		bool m_local = false;
	};

	std::vector< row_t > rows{ };
	float widest = width_of( title_font, "spectators" );

	for ( const auto& entry : entries ) {
		row_t row{ entry.m_name, show_target ? " -> " : "", show_target ? entry.m_target_name : "" };
		row.m_name_w   = width_of( font, row.m_name );
		row.m_sep_w    = row.m_sep.empty( ) ? 0.f : width_of( font, row.m_sep );
		row.m_target_w = row.m_target.empty( ) ? 0.f : width_of( font, row.m_target );
		row.m_local    = entry.m_watching_local;
		widest         = ImMax( widest, row.m_name_w + row.m_sep_w + row.m_target_w );
		rows.push_back( std::move( row ) );
	}

	const float width  = ImMax( 200.f, widest + 96.f );
	const float rows_h = static_cast< float >( rows.size( ) ) * 22.f;

	drag_placed_panel( width + 20.f, 23.f + rows_h );

	const float x      = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y      = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );
	const float bottom = y + 23.f + rows_h;

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	const ImVec2 shape[ 6 ] = { ImVec2( x + 23.f, y ),    ImVec2( x + width + 20.f, y ), ImVec2( x + width, y + 23.f ),
		                        ImVec2( x + width, bottom ), ImVec2( x, bottom ),            ImVec2( x, y + 23.f ) };
	draw_list->AddConvexPolyFilled( shape, 6, IM_COL32( 52, 52, 52, 255 ) );

	const ImU32 rim = IM_COL32( 150, 150, 150, 50 );
	draw_list->AddLine( ImVec2( x + 24.f, y + 1.f ), ImVec2( x + width + 19.f, y + 1.f ), rim, 1.f );
	draw_list->AddLine( ImVec2( x + width + 19.f, y + 1.f ), ImVec2( x + width - 1.f, y + 22.f ), rim, 1.f );
	draw_list->AddLine( ImVec2( x + 1.f, y + 22.f ), ImVec2( x + 24.f, y + 1.f ), rim, 1.f );
	draw_list->AddLine( ImVec2( x + 1.f, y + 22.f ), ImVec2( x + width - 1.f, y + 22.f ), rim, 1.f );

	if ( !rows.empty( ) ) {
		const ImVec2 frame[ 4 ] = { ImVec2( x, y + 23.f ), ImVec2( x, bottom ), ImVec2( x + width, bottom ), ImVec2( x + width, y + 23.f ) };
		draw_list->AddPolyline( frame, 4, IM_COL32( 69, 69, 69, 255 ), ImDrawFlags_None, 2.f );
	}

	const ImU32 local_color = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color ).get_u32( );
	const ImU32 other_color = GET_VARIABLE( g_variables.m_spectators_list_text_color_two, c_color ).get_u32( );

	for ( std::size_t i = 0; i < rows.size( ); ++i ) {
		const auto& row   = rows[ i ];
		const float row_y = y + 23.f + static_cast< float >( i ) * 22.f;

		draw_list->AddRectFilled( ImVec2( x, row_y ), ImVec2( x + width, row_y + 22.f ), ( i & 1 ) ? IM_COL32( 69, 69, 69, 255 ) : IM_COL32( 41, 41, 41, 255 ) );

		const ImU32 color = row.m_local ? local_color : other_color;
		const float ty    = row_y + 11.f - size * 0.5f - 1.f;
		const float tx    = x + ( width - ( row.m_name_w + row.m_sep_w + row.m_target_w ) ) * 0.5f;

		draw_list->AddText( font, size, ImVec2( tx, ty ), color, row.m_name.c_str( ) );
		if ( !row.m_sep.empty( ) ) {
			draw_list->AddText( font, size, ImVec2( tx + row.m_name_w, ty ), color, row.m_sep.c_str( ) );
			draw_list->AddText( font, size, ImVec2( tx + row.m_name_w + row.m_sep_w, ty ), color, row.m_target.c_str( ) );
		}
	}

	const float title_w = width_of( title_font, "spectators" );
	draw_list->AddText( title_font, title_font->FontSize, ImVec2( x + width * 0.5f - title_w * 0.5f, y + ( 23.f - title_font->FontSize ) * 0.5f ), local_color,
	                    "spectators" );

	g_render.end_stretch_block( block );
}

namespace {
	void billware_multicolor_rounded( ImDrawList* list, const ImVec2 min, const ImVec2 max, const ImU32 bg, const ImU32 ul, const ImU32 ur,
	                                  const ImU32 br, const ImU32 bl, const float rounding )
	{
		const float r = ImMin( rounding, ImMin( ImFabs( max.x - min.x ) * 0.5f - 1.f, ImFabs( max.y - min.y ) * 0.5f - 1.f ) );

		if ( r <= 0.f )
			return;

		list->AddRectFilledMultiColor( min, max, ul, ur, br, bl );

		const struct {
			ImVec2 m_corner, m_center;
			float m_a_min, m_a_max;
		} corners[ ] = { { min, ImVec2( min.x + r, min.y + r ), 4.82f, 3.1f },
			             { ImVec2( max.x, min.y ), ImVec2( max.x - r, min.y + r ), 6.34f, 4.62f },
			             { max, ImVec2( max.x - r, max.y - r ), 7.96f, 6.24f },
			             { ImVec2( min.x, max.y ), ImVec2( min.x + r, max.y - r ), 9.5f, 7.77f } };

		for ( const auto& corner : corners ) {
			list->PathLineTo( corner.m_corner );
			list->PathArcTo( corner.m_center, r, corner.m_a_min, corner.m_a_max, 0 );
			list->PathFillConvex( bg );
		}
	}

	void billware_box( ImDrawList* list, const ImVec2 min, const ImVec2 max, const float alpha )
	{
		const auto grey = [ & ]( const float v, const float a ) { return ImGui::ColorConvertFloat4ToU32( ImVec4( v, v, v, a ) ); };
		const auto grown = [ & ]( const float g ) { return std::pair{ ImVec2( min.x - g, min.y - g ), ImVec2( max.x + g, max.y + g ) }; };

		const ImU32 dark = grey( 0.07f, alpha ), light = grey( 0.14f, alpha );
		const auto [ fill_min, fill_max ] = grown( 2.f );

		billware_multicolor_rounded( list, fill_min, fill_max, dark, dark, dark, light, light, 5.5f );
		billware_multicolor_rounded( list, fill_min, fill_max, dark, light, light, dark, dark, 5.5f );

		const ImU32 rings[ 3 ] = { grey( 0.15f, alpha ), grey( 0.3f, alpha * 0.7f ), grey( 0.15f, alpha ) };

		for ( int i = 0; i < 3; ++i ) {
			const auto [ ring_min, ring_max ] = grown( 2.f + static_cast< float >( i ) );
			list->AddRect( ring_min, ring_max, rings[ i ], 5.5f, 0, 1.f );
		}
	}
}

void n_misc::impl_t::draw_watermark_billware( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ];

	if ( !font || !g_interfaces.m_global_vars_base )
		return;

	static float fps = 0.f, last = 0.f;
	const float now  = g_interfaces.m_global_vars_base->m_real_time;

	if ( now - last >= 0.5f && g_interfaces.m_global_vars_base->m_frame_time > 0.f ) {
		fps  = fps * 0.7f + ( 1.f / g_interfaces.m_global_vars_base->m_frame_time ) * 0.5f;
		last = now;
	}

	const std::string label = GET_VARIABLE( g_variables.m_watermark_label, int ) == 1 ? std::to_string( static_cast< int >( fps ) ) + " fps" : watermark_label( );
	const std::string text  = " " + watermark_brand( ) + ( label.empty( ) ? "" : " | " + label );
	const ImVec2 size      = font->CalcTextSizeA( 12.f, FLT_MAX, 0.f, text.c_str( ) );
	const float w          = static_cast< float >( static_cast< int >( g_ctx.m_width ) );

	const ImVec2 min( w - size.x - 13.f, 7.f ), max( w - 7.f, size.y + 13.f );

	g_watermark_box = ImVec4( min.x - 4.f, min.y - 4.f, max.x + 4.f, max.y + 4.f );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	billware_box( draw_list, min, max, 1.f );

	const float x0 = min.x - 2.f, x1 = max.x + 2.f, y0 = min.y - 2.f, y1 = max.y + 2.f;
	const auto random_in = []( const float lo, const float hi ) { return static_cast< float >( std::rand( ) % ImMax( static_cast< int >( hi - lo ), 1 ) ) + lo; };

	static std::vector< ImVec2 > dots{ };

	if ( dots.empty( ) )
		for ( int i = 0; i < 10; ++i ) {
			const float x = random_in( x0, x1 );
			dots.emplace_back( x, random_in( y0, y1 ) );
		}

	const ImU32 dot_color = accent_u32( 102 );

	for ( auto& dot : dots ) {
		draw_list->AddCircle( dot, 2.5f, dot_color, 0, 1.f );
		draw_list->AddCircleFilled( dot, 0.5f, dot_color, 0 );

		dot.y += 0.07f;
		if ( dot.y > y1 - 1.f ) {
			dot.y = random_in( y0, y1 );
			dot.x = random_in( x0, x1 );
		}
	}

	draw_list->AddText( font, 12.f, ImVec2( w - size.x - 12.f, 9.f ), accent_u32( 255 ), text.c_str( ) );
	draw_list->AddText( font, 12.f, ImVec2( w - size.x - 11.f, 10.f ), IM_COL32( 255, 255, 255, 255 ), text.c_str( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_billware( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_tahoma_bd_12 ];

	if ( !font || !g_interfaces.m_global_vars_base || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true );

	const bool show_target = GET_VARIABLE( g_variables.m_spectators_list_show_target, bool );

	if ( !show_target )
		std::erase_if( entries, []( const spectator_entry_t& entry ) { return !entry.m_watching_local; } );

	std::sort( entries.begin( ), entries.end( ), []( const spectator_entry_t& a, const spectator_entry_t& b ) { return a.m_index < b.m_index; } );

	static float fade = 0.f;
	const float step  = g_interfaces.m_global_vars_base->m_frame_time * 25.5f;

	fade = !entries.empty( ) || GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) ? ImMin( fade + step, 1.f ) : ImMax( fade - step, 0.f );

	if ( fade <= 0.f )
		return;

	std::vector< std::string > texts{ };
	float widest = 0.f;

	for ( const auto& entry : entries ) {
		texts.push_back( show_target ? entry.m_name + " -> " + entry.m_target_name : entry.m_name );
		widest = ImMax( widest, text_width( font, 12.f, texts.back( ) ) );
	}

	const float width  = widest + 14.f;
	const float height = static_cast< float >( entries.size( ) ) * 16.f + 14.f;

	drag_placed_panel( width, height );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	billware_box( draw_list, ImVec2( x, y ), ImVec2( x + width, y + height ), fade );

	const ImU32 shadow = accent_u32( static_cast< int >( ImSaturate( fade * 255.f ) * 255.f + 0.5f ) );
	const int text_a   = static_cast< int >( fade * 255.f );

	const c_color& one = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color );
	const c_color& two = GET_VARIABLE( g_variables.m_spectators_list_text_color_two, c_color );

	for ( std::size_t i = 0; i < entries.size( ); ++i ) {
		const ImVec2 pos( x + 7.f, y + 7.f + static_cast< float >( i ) * 16.f );
		const c_color& color = entries[ i ].m_watching_local ? one : two;

		draw_list->AddText( font, 12.f, ImVec2( pos.x + 1.f, pos.y + 1.f ), shadow, texts[ i ].c_str( ) );
		draw_list->AddText( font, 12.f, pos, IM_COL32( color[ 0 ], color[ 1 ], color[ 2 ], text_a * color[ 3 ] / 255 ), texts[ i ].c_str( ) );
	}

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_inkabanium( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_inba_tahoma_13 ];

	if ( !font || !g_interfaces.m_global_vars_base )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries );

	if ( entries.empty( ) && !GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) )
		return;

	const float line      = font->FontSize;
	const float text_h    = entries.empty( ) ? line : line * static_cast< float >( entries.size( ) + 1 );
	constexpr float width = 195.f, title_h = 19.f;
	const float height    = text_h + 30.f;

	drag_placed_panel( width, height );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	ImU32 color{ };

	if ( GET_VARIABLE( g_variables.m_spectators_inba_rainbow, bool ) ) {
		const float t      = g_interfaces.m_global_vars_base->m_real_time * GET_VARIABLE( g_variables.m_spectators_inba_rainbow_speed, float );
		const auto channel = [ & ]( const float phase ) { return static_cast< int >( ( std::sin( t + phase ) + 1.f ) * 0.5f * 255.f ); };

		color = IM_COL32( channel( 0.f ), channel( 2.0943952f ), channel( 4.1887903f ), 255 );
	} else {
		const c_color& one = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color );
		color              = IM_COL32( one[ 0 ], one[ 1 ], one[ 2 ], 255 );
	}

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddRectFilled( ImVec2( x, y + title_h ), ImVec2( x + width, y + height ), ImGui::ColorConvertFloat4ToU32( ImVec4( 0.05f, 0.05f, 0.05f, 0.7f ) ) );
	draw_list->AddRectFilled( ImVec2( x, y ), ImVec2( x + width, y + title_h ), ImGui::ColorConvertFloat4ToU32( ImVec4( 0.05f, 0.05f, 0.05f, 1.f ) ) );
	draw_list->AddRect( ImVec2( x, y ), ImVec2( x + width, y + height ), accent_u32( 255 ), 0.f, 0, 1.f );

	const float title_w = font->CalcTextSizeA( line, FLT_MAX, 0.f, "SPECTATORS" ).x;
	draw_list->AddText( font, line, ImVec2( std::floor( x + ( width - title_w ) * 0.5f ), y + 3.f ), color, "SPECTATORS" );

	for ( std::size_t i = 0; i < entries.size( ); ++i )
		draw_list->AddText( font, line, ImVec2( x + 8.f, y + title_h + 8.f + line * static_cast< float >( i ) ), color, entries[ i ].m_name.c_str( ) );

	g_render.end_stretch_block( block );
}

namespace {
	ImU32 dna_grey( const float v, const float a = 1.f ) { return ImGui::ColorConvertFloat4ToU32( ImVec4( v, v, v, a ) ); }

	ImU32 dna_accent( const float a )
	{
		const ImVec4 accent = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );
		return ImGui::ColorConvertFloat4ToU32( ImVec4( accent.x, accent.y, accent.z, a ) );
	}

	void dna_box( ImDrawList* list, const ImVec2 min, const ImVec2 max, const ImU32 ring, const ImU32 mid_ring )
	{
		const ImU32 corner = IM_COL32( 10, 10, 12, 50 ), dark = IM_COL32( 10, 10, 12, 255 ), light = dna_grey( 0.14f );

		billware_multicolor_rounded( list, min, max, corner, dark, dark, light, light, 5.5f );
		billware_multicolor_rounded( list, min, max, corner, light, light, dark, dark, 5.5f );

		list->AddRect( min, max, ring, 5.5f );
		list->AddRect( ImVec2( min.x - 1.f, min.y - 1.f ), ImVec2( max.x + 1.f, max.y + 1.f ), mid_ring, 5.5f );
		list->AddRect( ImVec2( min.x - 2.5f, min.y - 2.5f ), ImVec2( max.x + 2.5f, max.y + 2.5f ), ring, 5.5f );
	}

	int dna_fps( float& rate, float& last )
	{
		const float now = g_interfaces.m_global_vars_base->m_real_time;
		const float ft  = g_interfaces.m_global_vars_base->m_frame_time > 0.f ? g_interfaces.m_global_vars_base->m_frame_time : 0.0001f;

		if ( now - last >= 0.5f ) {
			rate = rate * 0.7f + 0.3f / ft;
			last = now;
		}

		return static_cast< int >( rate );
	}

	struct dna_part_t {
		std::string m_text = { };
		ImU32 m_color      = { };
	};

	std::vector< dna_part_t > dna_parts( const ImU32 brand, const ImU32 sep, const ImU32 user, const ImU32 number, const ImU32 unit, const int fps )
	{
		std::vector< dna_part_t > parts{ { n_misc::watermark_brand( ), brand } };

		if ( const std::string& name = GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string ); !name.empty( ) ) {
			parts.push_back( { " | ", sep } );
			parts.push_back( { name, user } );
		}

		if ( GET_VARIABLE( g_variables.m_watermark_label, int ) == 1 ) {
			parts.push_back( { " | ", sep } );
			parts.push_back( { std::to_string( fps ), number } );
			parts.push_back( { " fps", unit } );
		} else if ( const std::string label = n_misc::watermark_label( ); !label.empty( ) ) {
			parts.push_back( { " | ", sep } );
			parts.push_back( { label, number } );
		}

		return parts;
	}

	float dna_parts_width( ImFont* font, const std::vector< dna_part_t >& parts )
	{
		float width = 0.f;
		for ( const auto& part : parts )
			width += text_width( font, 15.f, part.m_text );
		return width;
	}

	bool dna_enemy_spotted( )
	{
		if ( !g_ctx.m_local || !g_interfaces.m_engine_client->is_in_game( ) )
			return false;

		bool spotted = false;

		g_entity_cache.enumerate( e_enumeration_type::type_players, [ & ]( c_base_entity* entity ) {
			if ( !entity || entity == g_ctx.m_local || !entity->is_alive( ) || entity->is_dormant( ) || entity->get_team( ) == g_ctx.m_local->get_team( ) )
				return;

			if ( entity->is_spotted( ) & 0xff )
				spotted = true;
		} );

		return spotted;
	}
}

void n_misc::impl_t::draw_watermark_dna( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_dna_montserrat_15 ];

	if ( !font || !g_interfaces.m_global_vars_base )
		return;

	static float rate = 0.f, last = 0.f;

	const ImU32 accent = dna_accent( 1.f ), white = dna_grey( 0.8f ), darkgrey = dna_grey( 0.4f );
	const auto parts   = dna_parts( accent, darkgrey, darkgrey, white, darkgrey, dna_fps( rate, last ) );
	const float text_w = dna_parts_width( font, parts );
	const float w      = static_cast< float >( static_cast< int >( g_ctx.m_width ) );

	const ImVec2 text_pos( w - text_w - 10.f, 10.f );
	const ImVec2 min( w - text_w - 15.f, 5.f ), max( w - 5.f, 30.f );

	g_watermark_box = ImVec4( min.x - 3.f, min.y - 3.f, max.x + 3.f, max.y + 3.f );

	const auto globals = g_interfaces.m_global_vars_base;
	const float window = static_cast< float >( GET_VARIABLE( g_variables.m_watermark_dna_glow_ticks, int ) ) * globals->m_interval_per_tick;
	const bool trick   = g_trick_time >= 0.f && globals->m_real_time >= g_trick_time && globals->m_real_time - g_trick_time < window;
	const bool lit = GET_VARIABLE( g_variables.m_watermark_dna_glow, bool ) && ( trick || ( GET_VARIABLE( g_variables.m_watermark_dna_glow_spotted, bool ) && dna_enemy_spotted( ) ) );

	static float glow = 0.f;
	glow = lit ? ImMin( 1.f, glow + 3.f * globals->m_frame_time ) : ImMax( 0.f, glow - 3.f * globals->m_frame_time );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	if ( glow > 0.01f ) {
		for ( int i = 0; i < 10; ++i ) {
			const float t = static_cast< float >( i ) / 9.f, grow = 7.f * glow * t;
			draw_list->AddRectFilled( ImVec2( min.x - grow, min.y - grow ), ImVec2( max.x + grow, max.y + grow ), dna_accent( 0.75f * glow * ( 1.f - t ) ), 5.5f,
			                          ImDrawFlags_RoundCornersAll );
		}

		draw_list->AddRectFilled( ImVec2( min.x - 2.f, min.y - 2.f ), ImVec2( max.x + 2.f, max.y + 2.f ), dna_accent( glow * 0.9f ), 5.5f, ImDrawFlags_RoundCornersAll );
	}

	dna_box( draw_list, min, max, IM_COL32( 10, 10, 12, 200 ), IM_COL32( 4, 4, 5, 50 ) );

	const float grad_w = text_w * 1.1f, center = text_pos.x + text_w * 0.5f, top = text_pos.y - 6.f;

	for ( int i = 0; i < 60; ++i ) {
		const float x0 = center - grad_w * 0.5f + grad_w / 60.f * static_cast< float >( i ), x1 = x0 + grad_w / 60.f;
		const float a  = ImMax( 0.f, 1.f - std::fabs( ( x0 + x1 ) * 0.5f - center ) / ( grad_w * 0.5f ) );

		draw_list->AddRectFilled( ImVec2( x0, top ), ImVec2( x1, top + 3.f ), dna_accent( 0.75f * a ), 2.f, 0 );
	}

	float x = text_pos.x;
	for ( std::size_t i = 0; i < parts.size( ); ++i ) {
		if ( i == 0 )
			draw_list->AddText( font, 15.f, ImVec2( x + 1.f, text_pos.y + 1.f ), white, parts[ i ].m_text.c_str( ) );

		draw_list->AddText( font, 15.f, ImVec2( x, text_pos.y ), parts[ i ].m_color, parts[ i ].m_text.c_str( ) );
		x += text_width( font, 15.f, parts[ i ].m_text );
	}

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_watermark_dna_clarity( )
{
	const auto font      = g_render.m_fonts[ e_font_names::font_name_dna_pt_root_bold_15 ];
	const auto icon_font = g_render.m_fonts[ e_font_names::font_name_dna_clarity_icon_50 ];

	if ( !font || !icon_font || !g_interfaces.m_global_vars_base )
		return;

	static float rate = 0.f, last = 0.f;

	const ImU32 accent = dna_accent( 1.f ), white = dna_grey( 0.8f ), darkgrey = dna_grey( 0.5f ), darkgrey2 = dna_grey( 0.2f );
	const auto parts   = dna_parts( accent, darkgrey2, darkgrey, white, darkgrey, dna_fps( rate, last ) );
	const float text_w = dna_parts_width( font, parts );
	const float w      = static_cast< float >( static_cast< int >( g_ctx.m_width ) );

	const ImVec2 text_pos( w - text_w - 10.f, 10.f );
	const ImVec2 min( w - text_w - 15.f, 5.f ), max( w - 5.f, 30.f );

	g_watermark_box = ImVec4( min.x - 3.f, min.y - 3.f, max.x + 3.f, max.y + 3.f );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	dna_box( draw_list, min, max, IM_COL32( 20, 20, 22, 250 ), IM_COL32( 4, 4, 5, 100 ) );

	const float icon_w = icon_font->CalcTextSizeA( 50.f, FLT_MAX, 0.f, "A" ).x;

	draw_list->PushClipRect( min, max, true );
	draw_list->AddText( icon_font, 50.f, ImVec2( min.x + 6.f - icon_w * 0.3f, text_pos.y - 17.f ), dna_accent( 0.15f ), "A" );
	draw_list->PopClipRect( );

	float x = text_pos.x;
	for ( const auto& part : parts ) {
		draw_list->AddText( font, 15.f, ImVec2( x, text_pos.y ), part.m_color, part.m_text.c_str( ) );
		x += text_width( font, 15.f, part.m_text );
	}

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_dna( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_dna_montserrat_15 ];

	if ( !font || !g_interfaces.m_global_vars_base || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true );

	const bool show_target = GET_VARIABLE( g_variables.m_spectators_list_show_target, bool );

	if ( !show_target )
		std::erase_if( entries, []( const spectator_entry_t& entry ) { return !entry.m_watching_local; } );

	std::sort( entries.begin( ), entries.end( ), []( const spectator_entry_t& a, const spectator_entry_t& b ) { return a.m_index < b.m_index; } );

	static float alpha = 0.f;
	const float step   = g_interfaces.m_global_vars_base->m_frame_time * 25.5f;

	alpha = ImClamp( alpha + ( !entries.empty( ) || GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) ? step : -step ), 0.f, 1.f );

	if ( alpha <= 0.f )
		return;

	constexpr float size = 15.f, pad = 7.f, spacing = 3.f;

	std::vector< std::string > texts{ };
	float widest = text_width( font, size, "Spectators" );

	for ( const auto& entry : entries ) {
		texts.push_back( show_target ? entry.m_name + " > " + entry.m_target_name : entry.m_name );
		widest = ImMax( widest, text_width( font, size, texts.back( ) ) );
	}

	const float width   = widest + pad * 2.f;
	const float title_h = size + pad * 2.f;
	const float body_h  = entries.empty( ) ? 0.f : static_cast< float >( entries.size( ) ) * ( size + spacing ) - spacing + pad * 2.f;

	drag_placed_panel( width, title_h + body_h );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddRectFilled( ImVec2( x, y ), ImVec2( x + width, y + title_h ), dna_grey( 0.07f ), 4.5f, ImDrawFlags_RoundCornersTop );
	draw_list->AddRectFilled( ImVec2( x, y + title_h ), ImVec2( x + width, y + title_h + body_h ), dna_grey( 0.07f, 0.8f ), 4.5f, ImDrawFlags_RoundCornersBottom );
	draw_list->AddRect( ImVec2( x, y ), ImVec2( x + width, y + title_h + body_h ), dna_grey( 0.1f, 0.7f ), 5.5f );

	const float center = x + width * 0.5f;

	for ( int i = 0; i < 60; ++i ) {
		const float x0 = x + width / 60.f * static_cast< float >( i ), x1 = x0 + width / 60.f;
		const float a  = 1.f - std::fabs( ( x0 + x1 ) * 0.5f - center ) / ( width * 0.5f );

		draw_list->AddRectFilled( ImVec2( x0, y ), ImVec2( x1, y + 3.f ), dna_accent( 0.75f * a ) );
	}

	draw_list->AddText( font, size, ImVec2( x + pad, y + pad ), dna_grey( 0.8f ), "Spectators" );

	for ( std::size_t i = 0; i < texts.size( ); ++i )
		draw_list->AddText( font, size, ImVec2( x + pad, y + title_h + static_cast< float >( i ) * ( size + spacing ) + pad ), dna_grey( 0.8f, alpha ),
		                    texts[ i ].c_str( ) );

	g_render.end_stretch_block( block );
}

namespace {
	constexpr ImU32 cucumber_bg = IM_COL32( 18, 15, 18, 255 );

	ImU32 cucumber_accent( ) { return dna_accent( 1.f ); }

	bool cucumber_fade( float& alpha, const bool on )
	{
		const float step = ImGui::GetIO( ).DeltaTime * 5.f;

		alpha = ImClamp( on ? alpha + step : alpha - step, 0.f, 1.f );
		return on || alpha > 0.f;
	}

	float cucumber_width( ImFont* font, const std::string& text )
	{
		return static_cast< float >( static_cast< int >( font->CalcTextSizeA( 14.f, FLT_MAX, 0.f, text.c_str( ) ).x + 0.95f ) );
	}

	ImU32 cucumber_fade_color( const ImU32 color, const float alpha )
	{
		return ( color & ~IM_COL32_A_MASK ) | ( static_cast< ImU32 >( static_cast< float >( color >> IM_COL32_A_SHIFT ) * alpha + 0.5f ) << IM_COL32_A_SHIFT );
	}
}

void n_misc::impl_t::draw_watermark_cucumber( )
{
	const auto font    = g_render.m_fonts[ e_font_names::font_name_cucumber_tahoma_14 ];
	const auto measure = g_render.m_fonts[ e_font_names::font_name_cucumber_verdana_14 ];

	if ( !font || !measure )
		return;

	static float alpha = 0.f;
	if ( !cucumber_fade( alpha, GET_VARIABLE( g_variables.m_watermark, bool ) ) )
		return;

	const std::string brand = watermark_brand( ), user = GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string );
	const float user_w      = cucumber_width( measure, user );

	constexpr float x = 10.f, y = 10.f;
	const ImVec2 max( x + 100.f + user_w, y + 20.f );

	g_watermark_box = ImVec4( x, y, max.x, max.y );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddRectFilled( ImVec2( x, y ), max, cucumber_bg, 15.f );

	const ImU32 white = cucumber_fade_color( IM_COL32( 255, 255, 255, 255 ), alpha );
	float text_x      = x + 16.f;

	draw_list->AddText( font, 14.f, ImVec2( text_x, y + 2.f ), cucumber_fade_color( cucumber_accent( ), alpha ), brand.c_str( ) );
	text_x += cucumber_width( font, brand ) + 8.f;

	draw_list->AddText( font, 14.f, ImVec2( text_x, y + 2.f ), white, "|" );
	text_x += cucumber_width( font, "|" ) + 8.f;

	draw_list->AddText( font, 14.f, ImVec2( text_x, y + 2.f ), white, user.c_str( ) );

	draw_list->AddRectFilled( ImVec2( x + 15.f, y + 18.f ), ImVec2( x + 85.f + user_w, y + 20.f ), cucumber_accent( ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_cucumber( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_cucumber_tahoma_14 ];

	if ( !font )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true );

	const bool show_target = GET_VARIABLE( g_variables.m_spectators_list_show_target, bool );

	if ( !show_target )
		std::erase_if( entries, []( const spectator_entry_t& entry ) { return !entry.m_watching_local; } );

	std::sort( entries.begin( ), entries.end( ), []( const spectator_entry_t& a, const spectator_entry_t& b ) { return a.m_index < b.m_index; } );

	static float alpha = 0.f;
	if ( !cucumber_fade( alpha, !entries.empty( ) || GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) ) )
		return;

	constexpr float width = 160.f, line = 14.f;
	const float text_h    = entries.empty( ) ? line : line * static_cast< float >( entries.size( ) );

	drag_placed_panel( width, entries.empty( ) ? 22.f : text_h + 27.f );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddRectFilled( ImVec2( x, y ), ImVec2( x + width, y + 22.f ), cucumber_bg, 15.f );
	draw_list->AddText( font, line, ImVec2( x + ( width - cucumber_width( font, "Spectator List" ) ) * 0.5f, y ),
	                    cucumber_fade_color( IM_COL32( 255, 255, 255, 255 ), alpha ), "Spectator List" );
	draw_list->AddRectFilled( ImVec2( x + 15.f, y + 20.f ), ImVec2( x + 145.f, y + 22.f ), cucumber_accent( ) );

	if ( !entries.empty( ) ) {
		draw_list->AddRectFilled( ImVec2( x, y + 25.f ), ImVec2( x + width, y + 27.f + text_h ), cucumber_bg, 15.f );

		const c_color& one = GET_VARIABLE( g_variables.m_spectators_list_text_color_one, c_color );
		const c_color& two = GET_VARIABLE( g_variables.m_spectators_list_text_color_two, c_color );

		for ( std::size_t i = 0; i < entries.size( ); ++i ) {
			const c_color& color   = entries[ i ].m_watching_local ? one : two;
			const std::string text = show_target ? entries[ i ].m_name + " => " + entries[ i ].m_target_name : entries[ i ].m_name;

			draw_list->AddText( font, line, ImVec2( x + 6.f, y + 26.f + line * static_cast< float >( i ) ),
			                    cucumber_fade_color( IM_COL32( color[ 0 ], color[ 1 ], color[ 2 ], color[ 3 ] ), alpha ), text.c_str( ) );
		}
	}

	g_render.end_stretch_block( block );
}

namespace {
	constexpr ImU32 howeweware_grey = IM_COL32( 225, 225, 225, 255 );

	/* donor = imgui 1.70 windows: text item width ( int )( w + 0.95 ), padding 8, item spacing 8 x 4, min size 32 */
	float howeweware_width( ImFont* font, const float size, const std::string& text )
	{
		return static_cast< float >( static_cast< int >( font->CalcTextSizeA( size, FLT_MAX, 0.f, text.c_str( ) ).x + 0.95f ) );
	}
}

void n_misc::impl_t::draw_watermark_howeweware( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_howeweware_minecraft_14 ];

	if ( !font )
		return;

	const auto engine    = g_interfaces.m_engine_client;
	const auto globals   = g_interfaces.m_global_vars_base;
	const bool in_game   = engine->is_in_game( );
	const bool connected = in_game && engine->is_connected( );

	/* rows of grey value + accent unit */
	std::vector< std::pair< std::string, std::string > > lines{ { watermark_brand( ), GET_VARIABLE( g_variables.m_watermark_clarity_user, std::string ) } };

	if ( GET_VARIABLE( g_variables.m_watermark_time, bool ) ) {
		const std::time_t raw = std::time( nullptr );
		std::tm local{ };
		localtime_s( &local, &raw );

		char clock[ 32 ] = { };
		std::strftime( clock, sizeof( clock ), "%X", &local );
		lines.emplace_back( clock, "pm" );
	}

	if ( GET_VARIABLE( g_variables.m_watermark_ping, bool ) && connected ) {
		const auto net_channel = engine->get_net_channel_info( );
		const float latency    = net_channel && !engine->is_playing_demo( ) ? net_channel->get_avg_latency( FLOW_OUTGOING ) : 0.f;
		lines.emplace_back( std::to_string( static_cast< int >( std::roundf( latency * 1000.f ) ) ), "ms" );
	}

	if ( GET_VARIABLE( g_variables.m_watermark_tickrate, bool ) && in_game && globals && globals->m_interval_per_tick > 0.f )
		lines.emplace_back( std::to_string( static_cast< int >( 1.f / globals->m_interval_per_tick ) ), " tick" );

	/* donor's smoothing: 0.9 * last + 0.1 * frame, last always 0 */
	if ( GET_VARIABLE( g_variables.m_watermark_fps, bool ) && globals ) {
		float frame = static_cast< float >( static_cast< double >( globals->m_abs_frame_time ) * 0.09999999999999998 + 0.0 );
		if ( frame <= 0.f )
			frame = 1.f;
		lines.emplace_back( std::to_string( static_cast< int >( 1.f / frame ) / 10 ), "fps" );
	}

	if ( GET_VARIABLE( g_variables.m_watermark_velocity, bool ) && connected ) {
		const int speed = g_ctx.m_local ? static_cast< int >( std::roundf( g_ctx.m_local->get_velocity( ).length_2d( ) ) ) : 0;
		lines.emplace_back( std::to_string( speed ), "vel" );
	}

	/* window at 5,5, no background: first line at pos + padding, SameLine = width + 8, line = 14 + 4 */
	constexpr float pos = 5.f, pad = 8.f, size = 14.f, line = size + 4.f, spacing = 8.f;

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );
	const ImU32 accent   = dna_accent( 1.f );

	float right = pos + pad;

	for ( std::size_t i = 0; i < lines.size( ); ++i ) {
		const float y = pos + pad + line * static_cast< float >( i );
		float x       = pos + pad;

		draw_list->AddText( font, size, ImVec2( x, y ), howeweware_grey, lines[ i ].first.c_str( ) );
		x += howeweware_width( font, size, lines[ i ].first ) + spacing;

		draw_list->AddText( font, size, ImVec2( x, y ), accent, lines[ i ].second.c_str( ) );
		right = ImMax( right, x + howeweware_width( font, size, lines[ i ].second ) );
	}

	const float bottom = pos + pad + line * static_cast< float >( lines.size( ) - 1 ) + size;
	g_watermark_box    = ImVec4( pos, pos, pos + ImMax( 32.f, right - pos + pad ), pos + ImMax( 32.f, bottom - pos + pad ) );

	g_render.end_stretch_block( block );
}

void n_misc::impl_t::draw_spectator_list_howeweware( )
{
	const auto font = g_render.m_fonts[ e_font_names::font_name_lumi_proggy_13 ];

	if ( !font )
		return;

	std::vector< spectator_entry_t > entries{ };
	collect_spectators( entries, true, true );

	const bool show_target = GET_VARIABLE( g_variables.m_spectators_list_show_target, bool );

	if ( !show_target )
		std::erase_if( entries, []( const spectator_entry_t& entry ) { return !entry.m_watching_local; } );

	if ( entries.empty( ) && !g_menu.m_opened && !GET_VARIABLE( g_variables.m_spectators_list_always_show, bool ) )
		return;

	std::sort( entries.begin( ), entries.end( ), []( const spectator_entry_t& a, const spectator_entry_t& b ) { return a.m_index < b.m_index; } );

	/* dead local: rows on the player we spectate count as ours too */
	int watched = -1;
	if ( g_ctx.m_local && !g_ctx.m_local->is_alive( ) )
		if ( const auto target = reinterpret_cast< c_base_entity* >(
				 g_interfaces.m_client_entity_list->get_client_entity_from_handle( g_ctx.m_local->get_observer_target_handle( ) ) ) )
			watched = static_cast< int >( target->get_index( ) );

	const std::string title = "         Spectators         ";
	constexpr float size = 13.f, line = size + 4.f, pad = 8.f;

	std::vector< std::string > rows{ };
	float widest = howeweware_width( font, size, title );

	for ( const auto& entry : entries ) {
		std::string text = show_target ? entry.m_name + "->" + entry.m_target_name + " " : entry.m_name;
		std::erase( text, '\n' );

		widest = ImMax( widest, howeweware_width( font, size, text ) );
		rows.push_back( std::move( text ) );
	}

	/* centred items converge on width = widest + padding both sides; title at y 2, Spacing( ), rows */
	const float width  = ImMax( 32.f, widest + pad * 2.f );
	const float height = ImMax( 32.f, 2.f + line + 4.f + line * static_cast< float >( rows.size( ) ) - 4.f + pad );

	drag_placed_panel( width, height );

	const float x = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_x, int ) );
	const float y = static_cast< float >( GET_VARIABLE( g_variables.m_spectators_list_y, int ) );

	const auto draw_list = ImGui::GetBackgroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	/* StyleColorsDark WindowBg / Border, window rounding 7, border 1 */
	const ImU32 bg = IM_COL32( 15, 15, 15, 240 ), bg_top = IM_COL32( 31, 31, 31, 240 );
	const ImU32 border = IM_COL32( 110, 110, 128, 128 ), border_clear = IM_COL32( 110, 110, 128, 0 );

	draw_list->AddRectFilled( ImVec2( x, y ), ImVec2( x + width, y + height ), bg, 7.f );
	draw_list->AddRect( ImVec2( x, y ), ImVec2( x + width, y + height ), border, 7.f, 0, 1.f );

	/* imgui 1.70 InnerClipRect: x in by padding / 2, y in by the border */
	draw_list->PushClipRect( ImVec2( x + 4.f, y + 1.f ), ImVec2( x + width - 4.f, y + height - 1.f ), true );

	const float header = y + size + 5.f;
	draw_list->AddRectFilledMultiColor( ImVec2( x, y ), ImVec2( x + width, header ), bg_top, bg_top, bg, bg );
	draw_list->AddRectFilledMultiColor( ImVec2( x, header + 1.f ), ImVec2( x + width * 0.5f, header ), border_clear, border, border, border_clear );
	draw_list->AddRectFilledMultiColor( ImVec2( x + width * 0.5f, header + 1.f ), ImVec2( x + width, header ), border, border_clear, border_clear, border );

	draw_list->AddText( font, size, ImVec2( x + width * 0.5f - howeweware_width( font, size, title ) * 0.5f, y + 2.f ), IM_COL32( 255, 255, 255, 255 ),
	                    title.c_str( ) );

	const c_color& other = GET_VARIABLE( g_variables.m_spectators_list_text_color_two, c_color );

	for ( std::size_t i = 0; i < rows.size( ); ++i ) {
		const bool ours = entries[ i ].m_watching_local || ( watched >= 0 && entries[ i ].m_target_index == watched );

		draw_list->AddText( font, size,
		                    ImVec2( x + width * 0.5f - howeweware_width( font, size, rows[ i ] ) * 0.5f, y + 2.f + line + 4.f + line * static_cast< float >( i ) ),
		                    ours ? dna_accent( 1.f ) : IM_COL32( other[ 0 ], other[ 1 ], other[ 2 ], other[ 3 ] ), rows[ i ].c_str( ) );
	}

	draw_list->PopClipRect( );

	g_render.end_stretch_block( block );
}

bool n_misc::media_player_donor_look( const int style, media_look_t& look )
{
	const ImVec4 accent = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );

	switch ( style ) {
	case 6: look = { true, havoc_color( g_variables.m_watermark_havoc_color, 1.f ), havoc_accent2_color( 1.f ), 1 }; return true;
	case 8: look = { true, IM_COL32( 255, 255, 255, 255 ), IM_COL32( 255, 255, 255, 102 ), 0 }; return true;
	case 9: look = { true, ImColor( accent.x, accent.y, accent.z, 1.f ), IM_COL32( evolve_text, evolve_text, evolve_text, 255 ), 0 }; return true;
	case 10: look = { true, IM_COL32( 255, 255, 255, 220 ), IM_COL32( 255, 255, 255, 220 ), 1 }; return true;
	case 11: look = { true, IM_COL32( 255, 255, 255, 255 ), IM_COL32( 255, 255, 255, 255 ), 1 }; return true;
	case 12: look = { false, IM_COL32( 255, 255, 255, 255 ), IM_COL32( 153, 153, 153, 255 ), 3 }; return true;
	case 13: look = { false, IM_COL32( 255, 255, 255, 255 ), IM_COL32( 153, 153, 153, 255 ), 1 }; return true;
	case 14: look = { false, IM_COL32( 255, 255, 255, 255 ), IM_COL32( 255, 255, 255, 255 ), 1 }; return true;
	case 15: look = { true, IM_COL32( 255, 255, 255, 255 ), ImColor( accent.x, accent.y, accent.z, 1.f ), 0 }; return true;
	case 16: look = { true, IM_COL32( 255, 255, 255, 255 ), ImColor( accent.x, accent.y, accent.z, 1.f ), 0 }; return true;
	case 17: look = { true, ImColor( accent.x, accent.y, accent.z, 1.f ), dna_grey( 0.8f ), 0 }; return true;
	case 18: look = { true, ImColor( accent.x, accent.y, accent.z, 1.f ), dna_grey( 0.5f ), 0 }; return true;
	case 19: look = { true, cucumber_accent( ), IM_COL32( 255, 255, 255, 255 ), 0 }; return true;
	case 20: look = { false, dna_accent( 1.f ), howeweware_grey, 0 }; return true;
	default: return false;
	}
}

void n_misc::impl_t::draw_watermark_gif( )
{
	g_watermark_gif_box = { };

	const ImVec4 box = g_watermark_box;
	if ( !GET_VARIABLE( g_variables.m_watermark_gif, bool ) || box.z <= box.x || box.w <= box.y )
		return;

	const int type       = std::clamp( GET_VARIABLE( g_variables.m_watermark_gif_type, int ), 0, 2 );
	spectator_gif_t& gif = type == 2 ? watermark_custom_gif.m_gif : spectator_gifs[ type ];
	const auto frame     = type == 2 ? file_image_frame( watermark_custom_gif, GET_VARIABLE( g_variables.m_watermark_gif_path, std::string ) )
	                                 : spectator_gif_frame( gif );

	if ( !frame || !gif.m_bitmap || !gif.m_bitmap->GetWidth( ) || !gif.m_bitmap->GetHeight( ) )
		return;

	const float aspect = static_cast< float >( gif.m_bitmap->GetWidth( ) ) / static_cast< float >( gif.m_bitmap->GetHeight( ) );
	const float h      = ( box.w - box.y ) * GET_VARIABLE( g_variables.m_watermark_gif_scale, float ) / 100.f;
	const float w      = h * aspect;
	const float gap    = GET_VARIABLE( g_variables.m_watermark_gif_gap, float );

	const int side  = GET_VARIABLE( g_variables.m_watermark_gif_side, int );
	const bool left = side == 1 || ( side != 2 && ( box.x + box.z ) * 0.5f > g_ctx.m_width * 0.5f );

	const float x = ( left ? box.x - gap - w : box.z + gap ) + GET_VARIABLE( g_variables.m_watermark_gif_offset_x, float );
	const float y = ( box.y + box.w - h ) * 0.5f + GET_VARIABLE( g_variables.m_watermark_gif_offset_y, float );

	g_watermark_gif_box = ImVec4( x, y, x + w, y + h );

	const float radians = GET_VARIABLE( g_variables.m_watermark_gif_rotation, float ) * IM_PI / 180.f;
	const float c = std::cos( radians ), s = std::sin( radians );
	const ImVec2 centre( x + w * 0.5f, y + h * 0.5f );
	const auto corner = [ & ]( const float dx, const float dy ) { return ImVec2( centre.x + dx * c - dy * s, centre.y + dx * s + dy * c ); };

	const float u0 = GET_VARIABLE( g_variables.m_watermark_gif_flip, bool ) ? 1.f : 0.f, u1 = 1.f - u0;
	const int alpha = static_cast< int >( std::clamp( GET_VARIABLE( g_variables.m_watermark_gif_alpha, float ), 0.f, 100.f ) * 2.55f );

	const auto draw_list = ImGui::GetForegroundDrawList( );
	const auto block     = g_render.begin_stretch_block( draw_list );

	draw_list->AddImageQuad( frame, corner( -w * 0.5f, -h * 0.5f ), corner( w * 0.5f, -h * 0.5f ), corner( w * 0.5f, h * 0.5f ), corner( -w * 0.5f, h * 0.5f ),
	                         ImVec2( u0, 0.f ), ImVec2( u1, 0.f ), ImVec2( u1, 1.f ), ImVec2( u0, 1.f ), IM_COL32( 255, 255, 255, alpha ) );

	g_render.end_stretch_block( block );
}

void n_misc::media_player_donor_frame( ImDrawList* list, const ImVec2 min, const ImVec2 max, const int style )
{
	const ImVec4 accent_v = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );

	switch ( style ) {
	case 6: havoc_box( list, min, ImVec2( max.x - min.x, max.y - min.y ), 1.f ); break;
	case 8:
		airflow_blur_rect( list, airflow_blur.m_textures[ 0 ] != nullptr, min, max, IM_COL32( 100, 100, 100, 255 ), 3.f, ImDrawFlags_RoundCornersAll );
		break;
	case 9:
		evolve_shadow( list, min, max, 12.f, evolve_max_alpha( ) * 0.25f );
		list->AddRectFilled( min, ImVec2( max.x, max.y - 1.f ), IM_COL32( evolve_block, evolve_block, evolve_block, 255 ), 4.f, ImDrawFlags_RoundCornersTop );
		list->AddRectFilled( ImVec2( min.x, max.y - 1.f ), max, ImColor( accent_v.x, accent_v.y, accent_v.z, 1.f ) );
		break;
	case 10: {
		const int accent[ 3 ] = { static_cast< int >( accent_v.x * 255.f ), static_cast< int >( accent_v.y * 255.f ), static_cast< int >( accent_v.z * 255.f ) };
		const float half      = std::floor( ( max.x - min.x ) / 2.f );

		for ( const float line_y : { min.y, max.y - 1.f } ) {
			legendware_gradient( list, ImVec2( min.x, line_y ), ImVec2( half, 1.f ), accent, 170, 240 );
			legendware_gradient( list, ImVec2( min.x + half, line_y ), ImVec2( max.x - min.x - half, 1.f ), accent, 240, 170 );
		}

		list->AddRectFilled( ImVec2( min.x, min.y + 1.f ), ImVec2( max.x, max.y - 1.f ), IM_COL32( 10, 10, 10, 150 ) );
		break;
	}
	case 11: {
		const auto& h = interium_wm_hues;

		list->AddRectFilledMultiColor( min, max, interium_hsv( h[ 0 ], 100 ), interium_hsv( h[ 1 ], 100 ), interium_hsv( h[ 2 ], 100 ), interium_hsv( h[ 3 ], 100 ) );
		list->AddRectFilledMultiColor( ImVec2( min.x + 1.f, min.y + 1.f ), ImVec2( max.x - 1.f, max.y - 1.f ), interium_hsv( h[ 0 ], 255 ),
		                               interium_hsv( h[ 1 ], 255 ), interium_hsv( h[ 2 ], 255 ), interium_hsv( h[ 3 ], 255 ) );
		list->AddRectFilled( ImVec2( min.x + 2.f, min.y + 2.f ), ImVec2( max.x - 2.f, max.y - 2.f ),
		                     GET_VARIABLE( g_variables.m_watermark_interium_bg, c_color ).get_u32( ) );
		break;
	}
	case 15:
		list->AddRectFilled( min, max, IM_COL32( 20, 20, 20, 255 ), 5.f );
		if ( !GET_VARIABLE( g_variables.m_watermark_lumi_no_border, bool ) )
			list->AddRect( min, max, IM_COL32( 0, 0, 0, 255 ), 0.f, 0, 1.f );
		break;
	case 16: billware_box( list, ImVec2( min.x + 4.f, min.y + 4.f ), ImVec2( max.x - 4.f, max.y - 4.f ), 1.f ); break;
	case 17: dna_box( list, ImVec2( min.x + 3.f, min.y + 3.f ), ImVec2( max.x - 3.f, max.y - 3.f ), IM_COL32( 10, 10, 12, 200 ), IM_COL32( 4, 4, 5, 50 ) ); break;
	case 18: dna_box( list, ImVec2( min.x + 3.f, min.y + 3.f ), ImVec2( max.x - 3.f, max.y - 3.f ), IM_COL32( 20, 20, 22, 250 ), IM_COL32( 4, 4, 5, 100 ) ); break;
	case 19:
		list->AddRectFilled( min, max, cucumber_bg, 15.f );
		list->AddRectFilled( ImVec2( min.x + 15.f, max.y - 2.f ), ImVec2( max.x - 15.f, max.y ), cucumber_accent( ) );
		break;
	default: break;
	}
}
