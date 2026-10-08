#include "bullets.h"
#include "../../../game/sdk/classes/c_view_render_beams.h"
#include "../../../game/sdk/includes/includes.h"
#include "../../../globals/includes/includes.h"
#include "../../../utilities/console/console.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>

extern void botox_dbg_log( const char* fmt, ... );

namespace
{
	constexpr const char* k_tracer_sprites[] = { "sprites/purplelaser1.vmt", "sprites/physbeam.vmt", "sprites/laserbeam.vmt",
		                                         "sprites/white.vmt" };

	constexpr std::uintptr_t k_client_verify_list = 0x11C50;

	struct client_hit_verify_t {
		c_vector m_position;
		float m_timestamp;
		float m_expire_time;
	};

	struct raw_utl_vector_t {
		client_hit_verify_t* m_memory;
		int m_allocation_count;
		int m_grow_size;
		int m_size;
		client_hit_verify_t* m_elements;
	};

	static_assert( sizeof( client_hit_verify_t ) == 20, "clientHitVerify_t is Vector + 2 floats" );
	static_assert( sizeof( beam_info_t ) == 0x90, "BeamInfo_t layout drifted" );

	constexpr float k_world_limit = 32768.f;

	bool sane_position( const c_vector& position )
	{
		return std::isfinite( position.m_x ) && std::isfinite( position.m_y ) && std::isfinite( position.m_z ) &&
		       std::fabs( position.m_x ) < k_world_limit && std::fabs( position.m_y ) < k_world_limit && std::fabs( position.m_z ) < k_world_limit;
	}

	void impact_box( const c_vector& position, const c_color& color )
	{
		if ( !g_interfaces.m_debug_overlay )
			return;

		const float size     = std::clamp( GET_VARIABLE( g_variables.m_bullet_impacts_size, float ), 0.5f, 10.f );
		const float duration = std::clamp( GET_VARIABLE( g_variables.m_bullet_impacts_duration, float ), 0.1f, 30.f );

		g_interfaces.m_debug_overlay->add_box_overlay( position, c_vector( -size, -size, -size ), c_vector( size, size, size ), c_angle( ),
		                                              color[ 0 ], color[ 1 ], color[ 2 ], color[ 3 ], duration );
	}
}

void n_bullets::impl_t::register_listener( )
{
	if ( m_listening || !g_interfaces.m_game_event_manager )
		return;

	m_listening = g_interfaces.m_game_event_manager->add_listener( &m_listener, "bullet_impact", false );

	// god esp reads "impact, no hurt": a hurt we never receive would latch god on everyone
	if ( m_listening )
		g_interfaces.m_game_event_manager->add_listener( &m_listener, "player_hurt", false );

	g_console.print( m_listening ? "listening for bullet_impact" : "FAILED TO LISTEN FOR BULLET_IMPACT" );
}

void n_bullets::impl_t::on_bullet_impact( game_event_t* event )
{
	if ( !g_ctx.m_local )
		return;

	const int shooter_index = g_interfaces.m_engine_client->get_player_for_user_id( event->get_int( "userid" ) );
	if ( shooter_index < 1 || shooter_index > 64 )
		return;

	const auto shooter = g_interfaces.m_client_entity_list->get< c_base_entity >( shooter_index );
	if ( !shooter )
		return;

	const c_vector position( event->get_float( "x" ), event->get_float( "y" ), event->get_float( "z" ) );
	if ( !sane_position( position ) )
		return;

	const bool is_local = shooter == g_ctx.m_local;

	if ( is_local ) {
		if ( GET_VARIABLE( g_variables.m_bullet_impacts, bool ) )
			impact_box( position, GET_VARIABLE( g_variables.m_bullet_impacts_server_color, c_color ) );

		m_local_impacts.push_back( { position, g_interfaces.m_global_vars_base->m_real_time } );
		while ( m_local_impacts.size( ) > 64 )
			m_local_impacts.pop_front( );
	}

	if ( !GET_VARIABLE( g_variables.m_bullet_tracers, bool ) )
		return;

	const bool is_enemy = !is_local && g_ctx.m_local->is_enemy( shooter );

	const bool wanted = is_local ? GET_VARIABLE( g_variables.m_bullet_tracers_local, bool )
	                    : is_enemy ? GET_VARIABLE( g_variables.m_bullet_tracers_enemy, bool )
	                               : GET_VARIABLE( g_variables.m_bullet_tracers_team, bool );
	if ( !wanted )
		return;

	auto& tracer   = m_tracers[ shooter_index ];
	tracer.m_valid = true;
	tracer.m_start = shooter->get_abs_origin( ) + shooter->get_view_offset( );
	tracer.m_end   = position;
}

void n_bullets::impl_t::on_player_hurt( const int victim_index )
{
	if ( !GET_VARIABLE( g_variables.m_world_hit_marker, bool ) )
		return;

	const auto victim = g_interfaces.m_client_entity_list->get< c_base_entity >( victim_index );
	if ( !victim )
		return;

	const float now      = g_interfaces.m_global_vars_base->m_real_time;
	const c_vector chest = victim->get_abs_origin( ) + c_vector( 0.f, 0.f, 40.f );

	const impact_t* best = nullptr;
	float best_distance  = 128.f;

	for ( const auto& impact : m_local_impacts ) {
		if ( now - impact.m_time > 0.5f || now < impact.m_time )
			continue;

		const float distance = impact.m_position.dist_to( chest );
		if ( distance < best_distance ) {
			best_distance = distance;
			best          = &impact;
		}
	}

	if ( !best )
		return;

	m_markers.push_back( { best->m_position, now } );
	while ( m_markers.size( ) > 32 )
		m_markers.pop_front( );
}

void n_bullets::impl_t::flush_tracers( )
{
	const auto beams = g_interfaces.m_view_render_beams;

	const int sprite = std::clamp( GET_VARIABLE( g_variables.m_bullet_tracers_sprite, int ), 0,
	                               static_cast< int >( std::size( k_tracer_sprites ) ) - 1 );

	for ( int index = 1; index < static_cast< int >( m_tracers.size( ) ); ++index ) {
		auto& tracer = m_tracers[ index ];
		if ( !tracer.m_valid )
			continue;

		tracer.m_valid = false;

		if ( !beams || !*reinterpret_cast< void** >( beams ) || !GET_VARIABLE( g_variables.m_bullet_tracers, bool ) )
			continue;

		const auto shooter = g_interfaces.m_client_entity_list->get< c_base_entity >( index );
		if ( !shooter )
			continue;

		const bool is_local = shooter == g_ctx.m_local;
		const bool is_enemy = !is_local && g_ctx.m_local->is_enemy( shooter );

		const c_color color = is_local   ? GET_VARIABLE( g_variables.m_bullet_tracers_local_color, c_color )
		                      : is_enemy ? GET_VARIABLE( g_variables.m_bullet_tracers_enemy_color, c_color )
		                                 : GET_VARIABLE( g_variables.m_bullet_tracers_team_color, c_color );

		const float width = std::clamp( GET_VARIABLE( g_variables.m_bullet_tracers_width, float ), 0.5f, 10.f );

		beam_info_t info{ };
		info.m_start       = tracer.m_start;
		info.m_end         = tracer.m_end;
		info.m_model_name  = k_tracer_sprites[ sprite ];
		info.m_life        = std::clamp( GET_VARIABLE( g_variables.m_bullet_tracers_life, float ), 0.1f, 10.f );
		info.m_width       = width;
		info.m_end_width   = width;
		info.m_fade_length = 0.1f;
		info.m_brightness  = static_cast< float >( color[ 3 ] );
		info.m_speed       = 1.f;
		info.m_red         = static_cast< float >( color[ 0 ] );
		info.m_green       = static_cast< float >( color[ 1 ] );
		info.m_blue        = static_cast< float >( color[ 2 ] );
		info.m_segments    = 2;
		info.m_flags       = beam_flag_fade_in | beam_flag_only_noise_once | beam_flag_no_tile;

		beams->create_beam_points( &info );
	}
}

void n_bullets::impl_t::client_impacts( )
{
	if ( m_client_list_bad || !GET_VARIABLE( g_variables.m_bullet_impacts, bool ) || !g_ctx.m_local )
		return;

	const auto list = reinterpret_cast< const raw_utl_vector_t* >( reinterpret_cast< std::uintptr_t >( g_ctx.m_local ) + k_client_verify_list );

	if ( list->m_size <= 0 )
		return;

	const bool layout_ok = list->m_size <= 65536 && list->m_allocation_count >= list->m_size && list->m_memory &&
	                       list->m_elements == list->m_memory && sane_position( list->m_memory[ list->m_size - 1 ].m_position );
	if ( !layout_ok ) {
		m_client_list_bad = true;
		g_console.print( "client impact list failed its layout check, client impacts off (offset 0x11C50 stale?)" );
		botox_dbg_log( "IMPACTS: verify list bad size=%d alloc=%d", list->m_size, list->m_allocation_count );
		return;
	}

	const float now   = g_interfaces.m_global_vars_base->m_real_time;
	int first_new     = list->m_size;
	while ( first_new > 0 && list->m_memory[ first_new - 1 ].m_timestamp > m_last_client_impact )
		--first_new;

	const c_color color = GET_VARIABLE( g_variables.m_bullet_impacts_client_color, c_color );
	c_vector last_boxed = { };

	for ( int i = first_new; i < list->m_size; ++i ) {
		const auto& hit = list->m_memory[ i ];
		m_last_client_impact = std::max( m_last_client_impact, hit.m_timestamp );

		// first read after a map load / toggle: old shots, don't flash them all at once
		if ( now - hit.m_timestamp > 1.f || !sane_position( hit.m_position ) )
			continue;

		if ( hit.m_position.dist_to( last_boxed ) < 0.01f )
			continue;

		impact_box( hit.m_position, color );
		last_boxed = hit.m_position;
	}
}

/* delusional/airplane jump trail: beam segment old -> new origin. spawned per step distance, not per tick or
   frame, so beam count stays ~speed * life / step whatever the fps */
void n_bullets::impl_t::movement_trail( )
{
	constexpr float k_step     = 4.f;
	constexpr float k_teleport = 256.f;

	const auto beams = g_interfaces.m_view_render_beams;
	if ( !GET_VARIABLE( g_variables.m_movement_trail, bool ) || !beams || !*reinterpret_cast< void** >( beams ) || !g_ctx.m_local->is_alive( ) ) {
		m_trail_valid = false;
		return;
	}

	const int move_type = g_ctx.m_local->get_move_type( );
	const int flags     = g_ctx.m_local->get_flags( );
	const bool skip     = move_type == move_type_ladder || move_type == move_type_noclip || move_type == move_type_observer ||
	                  ( GET_VARIABLE( g_variables.m_movement_trail_air_only, bool ) && ( flags & fl_onground ) );

	const c_vector origin = g_ctx.m_local->get_abs_origin( );

	if ( skip || !m_trail_valid ) {
		m_trail_last  = origin;
		m_trail_valid = !skip;
		return;
	}

	const float distance = origin.dist_to( m_trail_last );
	if ( distance < k_step )
		return;

	if ( distance > k_teleport || !sane_position( origin ) ) {
		m_trail_last = origin;
		return;
	}

	c_color color = GET_VARIABLE( ( flags & fl_ducking ) ? g_variables.m_movement_trail_crouch_color : g_variables.m_movement_trail_color, c_color );
	if ( GET_VARIABLE( g_variables.m_movement_trail_rainbow, bool ) ) {
		const float t      = g_interfaces.m_global_vars_base->m_real_time * 1.5f;
		const auto channel = [ & ]( const float phase ) { return static_cast< int >( ( std::sin( t + phase ) + 1.f ) * 0.5f * 255.f ); };
		color              = c_color( channel( 0.f ), channel( 2.0943952f ), channel( 4.1887903f ), static_cast< int >( color[ 3 ] ) );
	}

	const int sprite  = std::clamp( GET_VARIABLE( g_variables.m_movement_trail_sprite, int ), 0, static_cast< int >( std::size( k_tracer_sprites ) ) - 1 );
	const float width = std::clamp( GET_VARIABLE( g_variables.m_movement_trail_width, float ), 0.5f, 20.f );

	beam_info_t info{ };
	info.m_start      = m_trail_last;
	info.m_end        = origin;
	info.m_model_name = k_tracer_sprites[ sprite ];
	info.m_life       = std::clamp( GET_VARIABLE( g_variables.m_movement_trail_life, float ), 0.1f, 10.f );
	info.m_width      = width;
	info.m_end_width  = width;
	info.m_brightness = static_cast< float >( color[ 3 ] );
	info.m_red        = static_cast< float >( color[ 0 ] );
	info.m_green      = static_cast< float >( color[ 1 ] );
	info.m_blue       = static_cast< float >( color[ 2 ] );
	info.m_segments   = 2;
	info.m_flags      = beam_flag_no_tile;

	beams->create_beam_points( &info );
	m_trail_last = origin;
}

void n_bullets::impl_t::on_frame_stage_notify( const int stage )
{
	register_listener( );

	if ( stage != e_client_frame_stage::render_start )
		return;

	flush_tracers( );
	client_impacts( );
	movement_trail( );
}

void n_bullets::impl_t::on_paint_traverse( )
{
	if ( m_markers.empty( ) )
		return;

	if ( !GET_VARIABLE( g_variables.m_world_hit_marker, bool ) ) {
		m_markers.clear( );
		return;
	}

	const float now      = g_interfaces.m_global_vars_base->m_real_time;
	const float duration = std::max( GET_VARIABLE( g_variables.m_world_hit_marker_duration, float ), 0.1f );
	const float size     = GET_VARIABLE( g_variables.m_world_hit_marker_size, float );
	const auto color     = GET_VARIABLE( g_variables.m_world_hit_marker_color, c_color );

	constexpr float k_gap = 2.f;

	for ( auto it = m_markers.begin( ); it != m_markers.end( ); ) {
		const float elapsed = now - it->m_time;

		if ( elapsed < 0.f || elapsed >= duration ) {
			it = m_markers.erase( it );
			continue;
		}

		c_vector_2d screen = { };
		if ( g_render.world_to_screen( it->m_position, screen ) ) {
			const float fraction = elapsed / duration;
			const float alpha    = fraction < 0.7f ? 1.f : 1.f - ( fraction - 0.7f ) / 0.3f;
			const unsigned int u = color.get_u32( alpha );

			for ( const float sx : { -1.f, 1.f } ) {
				for ( const float sy : { -1.f, 1.f } ) {
					const c_vector_2d a( screen.m_x + sx * k_gap, screen.m_y + sy * k_gap );
					const c_vector_2d b( screen.m_x + sx * ( k_gap + size ), screen.m_y + sy * ( k_gap + size ) );
					g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line,
					                                   std::make_any< line_draw_object_t >( line_draw_object_t{ a, b, u, 1.f } ) );
				}
			}
		}

		++it;
	}
}

void n_bullets::impl_t::on_level_init( )
{
	for ( auto& tracer : m_tracers )
		tracer.m_valid = false;

	m_local_impacts.clear( );
	m_markers.clear( );
	m_last_client_impact = 0.f;
	m_client_list_bad    = false;
	m_trail_valid        = false;
}

void n_bullets::impl_t::on_release( )
{
	if ( m_listening && g_interfaces.m_game_event_manager )
		g_interfaces.m_game_event_manager->remove_listener( &m_listener );

	m_listening = false;
}
