#include "movement_recorder.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../prediction/prediction.h"
#include "movement.h"

#include "../../dependencies/json/json.hpp"

using json = nlohmann::json;

extern void start_movement_fix( c_user_cmd* cmd );
extern void end_movement_fix( c_user_cmd* cmd );
extern void botox_dbg_log( const char* fmt, ... );

namespace
{
	constexpr const char* k_root_dir   = "C:\\botox\\recordings";
	constexpr float k_start_radius     = 28.f;
	constexpr float k_pos_close_eps    = 0.1f;
	constexpr float k_approach_give_up = 2.f;
	constexpr float k_indicator_lerp_k = 6.f;

	std::string sanitize_clip_name( std::string name )
	{
		for ( char& ch : name ) {
			if ( ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' || ch == '\\' || ch == '|' || ch == '?' || ch == '*' )
				ch = '_';
		}

		while ( !name.empty( ) && std::isspace( static_cast< unsigned char >( name.front( ) ) ) )
			name.erase( name.begin( ) );
		while ( !name.empty( ) && std::isspace( static_cast< unsigned char >( name.back( ) ) ) )
			name.pop_back( );

		return name;
	}

	std::filesystem::path unique_clip_path( const std::filesystem::path& root, std::string& display_name )
	{
		display_name = sanitize_clip_name( display_name );
		if ( display_name.empty( ) )
			display_name = "Clip";

		auto path = root / ( display_name + ".mr" );
		if ( !std::filesystem::exists( path ) )
			return path;

		const std::string base = display_name;
		for ( int i = 1; i < 1000; ++i ) {
			display_name = base + " " + std::to_string( i );
			path         = root / ( display_name + ".mr" );
			if ( !std::filesystem::exists( path ) )
				return path;
		}

		return path;
	}

	float wrap_180( float a )
	{
		while ( a > 180.f )
			a -= 360.f;
		while ( a < -180.f )
			a += 360.f;
		return a;
	}

	float now_real( )
	{
		return g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;
	}

	/* playback yaw != original = camera follows the route too, just turned */
	bool camera_locked( )
	{
		return GET_VARIABLE( g_variables.m_movement_rec_lockva, bool ) || GET_VARIABLE( g_variables.m_movement_rec_yaw, int ) != 0;
	}

	float smoothstep01( float t )
	{
		t = std::clamp( t, 0.f, 1.f );
		return t * t * ( 3.f - 2.f * t );
	}
}

n_movement_recorder::frame_t::frame_t( c_user_cmd* cmd, const c_vector& pos )
{
	view_pitch   = cmd->m_view_point.m_x;
	view_yaw     = cmd->m_view_point.m_y;
	forward      = cmd->m_forward_move;
	side         = cmd->m_side_move;
	up           = cmd->m_up_move;
	buttons      = cmd->m_buttons;
	mouse_dx     = cmd->m_mouse_delta_x;
	mouse_dy     = cmd->m_mouse_delta_y;
	position     = pos;
	velocity     = g_prediction.backup_data.m_velocity;
	has_velocity = true;

	if ( !g_ctx.m_local )
		return;
	const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	if ( !weapon )
		return;
	weapon_def = weapon->get_item_definition_index( );
	if ( const auto data = g_interfaces.m_weapon_system ? g_interfaces.m_weapon_system->get_weapon_data( weapon_def ) : nullptr )
		weapon_slot = data->m_slot;
}

void n_movement_recorder::frame_t::replay( c_user_cmd* cmd ) const
{
	cmd->m_view_point.m_x = view_pitch;
	cmd->m_view_point.m_y = view_yaw;
	cmd->m_forward_move   = forward;
	cmd->m_side_move      = side;
	cmd->m_up_move        = up;
	cmd->m_buttons        = buttons;
	cmd->m_mouse_delta_x  = mouse_dx;
	cmd->m_mouse_delta_y  = mouse_dy;
}

int n_movement_recorder::impl_t::server_tickrate( )
{
	const float interval = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_interval_per_tick : 0.f;
	return interval > 0.f ? static_cast< int >( 1.f / interval + 0.5f ) : 64;
}

/* frames are per tick: a clip from another tickrate replays at the wrong speed */
bool n_movement_recorder::impl_t::playable( const clip_t& clip ) const
{
	return !clip.frames.empty( ) && clip.tickrate == server_tickrate( );
}

std::filesystem::path n_movement_recorder::impl_t::get_root_path( ) const
{
	return std::filesystem::path( k_root_dir );
}

void n_movement_recorder::impl_t::ensure_root( )
{
	std::error_code ec;
	const auto root = get_root_path( );
	if ( !std::filesystem::exists( root, ec ) )
		std::filesystem::create_directories( root, ec );
}

static bool read_clip_from_file( const std::filesystem::path& file_path, n_movement_recorder::clip_t& clip_out )
{
	std::ifstream in( file_path );
	if ( !in.good( ) )
		return false;
	if ( in.peek( ) == std::ifstream::traits_type::eof( ) )
		return false;

	json j;
	try {
		in >> j;
	} catch ( ... ) {
		return false;
	}

	if ( !j.is_object( ) || !j.contains( "frames" ) || !j[ "frames" ].is_array( ) )
		return false;

	try {
		clip_out.frames.clear( );
		clip_out.map          = j.value( "map", std::string{ } );
		clip_out.tickrate     = j.value( "tick", 64 );
		clip_out.filename     = file_path.filename( ).string( );
		clip_out.display_name = file_path.stem( ).string( );

		for ( const auto& jf : j[ "frames" ] ) {
			n_movement_recorder::frame_t f{ };
			f.view_pitch   = jf.value( "vpx", 0.f );
			f.view_yaw     = jf.value( "vpy", 0.f );
			f.forward      = jf.value( "fwd", 0.f );
			f.side         = jf.value( "side", 0.f );
			f.up           = jf.value( "up", 0.f );
			f.buttons      = jf.value( "btn", 0 );
			f.mouse_dx     = static_cast< short >( jf.value( "mdx", 0 ) );
			f.mouse_dy     = static_cast< short >( jf.value( "mdy", 0 ) );
			f.jetpack      = jf.value( "jp", false );
			f.weapon_def   = static_cast< short >( jf.value( "wd", 0 ) );
			f.weapon_slot  = jf.value( "ws", -1 );
			f.position.m_x = jf.value( "px", 0.f );
			f.position.m_y = jf.value( "py", 0.f );
			f.position.m_z = jf.value( "pz", 0.f );
			f.has_velocity = jf.contains( "vx" );
			f.velocity.m_x = jf.value( "vx", 0.f );
			f.velocity.m_y = jf.value( "vy", 0.f );
			f.velocity.m_z = jf.value( "vz", 0.f );
			clip_out.frames.emplace_back( std::move( f ) );
		}
	} catch ( ... ) {
		return false;
	}

	if ( clip_out.map.empty( ) ) {
		const auto sp = clip_out.display_name.find( ' ' );
		if ( sp != std::string::npos )
			clip_out.map = clip_out.display_name.substr( 0, sp );
	}

	return true;
}

static void write_clip_to_file( const std::filesystem::path& file_path, const n_movement_recorder::clip_t& clip )
{
	json j;
	j[ "map" ]    = clip.map;
	j[ "tick" ]   = clip.tickrate;
	j[ "frames" ] = json::array( );
	for ( const auto& f : clip.frames ) {
		json jf;
		jf[ "vpx" ]  = f.view_pitch;
		jf[ "vpy" ]  = f.view_yaw;
		jf[ "fwd" ]  = f.forward;
		jf[ "side" ] = f.side;
		jf[ "up" ]   = f.up;
		jf[ "btn" ]  = f.buttons;
		jf[ "mdx" ]  = static_cast< int >( f.mouse_dx );
		jf[ "mdy" ]  = static_cast< int >( f.mouse_dy );
		jf[ "jp" ]   = f.jetpack;
		jf[ "wd" ]   = static_cast< int >( f.weapon_def );
		jf[ "ws" ]   = f.weapon_slot;
		jf[ "px" ]   = f.position.m_x;
		jf[ "py" ]   = f.position.m_y;
		jf[ "pz" ]   = f.position.m_z;
		jf[ "vx" ]   = f.velocity.m_x;
		jf[ "vy" ]   = f.velocity.m_y;
		jf[ "vz" ]   = f.velocity.m_z;
		j[ "frames" ].push_back( jf );
	}

	std::ofstream out( file_path );
	if ( out.good( ) )
		out << j.dump( 2 );
}

void n_movement_recorder::impl_t::refresh_clips( const bool only_if_changed )
{
	std::scoped_lock lock( m_clips_mutex );

	ensure_root( );

	/* listing stamp: re-parsing every .mr (json, all maps) each 5 s stalled paint 20-60 ms */
	std::vector< std::filesystem::path > files;
	std::wstring stamp( m_current_map.begin( ), m_current_map.end( ) );

	std::error_code ec;
	for ( const auto& entry : std::filesystem::directory_iterator{ get_root_path( ), ec } ) {
		std::error_code file_ec;
		if ( !entry.is_regular_file( file_ec ) )
			continue;
		if ( entry.path( ).extension( ) != ".mr" )
			continue;

		stamp += L'|' + entry.path( ).filename( ).native( ) + L':' + std::to_wstring( entry.file_size( file_ec ) ) + L':' +
		         std::to_wstring( entry.last_write_time( file_ec ).time_since_epoch( ).count( ) );
		files.emplace_back( entry.path( ) );
	}

	const std::size_t hash = std::hash< std::wstring >{ }( stamp );
	if ( only_if_changed && hash == m_clips_stamp )
		return;
	m_clips_stamp = hash;

	m_clips.clear( );

	for ( const auto& path : files ) {
		clip_t c{ };
		if ( !read_clip_from_file( path, c ) )
			continue;
		if ( !m_current_map.empty( ) && !c.map.empty( ) && c.map != m_current_map )
			continue;
		m_clips.emplace_back( std::move( c ) );
	}

	if ( m_selected_clip >= static_cast< int >( m_clips.size( ) ) )
		m_selected_clip = -1;
	if ( m_editor_clip_index >= static_cast< int >( m_clips.size( ) ) )
		m_editor_open = false;
}

void n_movement_recorder::impl_t::save_frames( const std::vector< frame_t >& frames, const char* kind )
{
	std::scoped_lock lock( m_clips_mutex );

	if ( m_current_map.empty( ) )
		return;
	if ( frames.size( ) < 2 ) {
		movement_add_window( 0, std::string( "nothing to save" ) );
		return;
	}

	ensure_root( );

	std::time_t now = std::time( nullptr );
	std::tm tm{ };
	::localtime_s( &tm, &now );
	char date_buf[ 32 ]{ };
	std::strftime( date_buf, sizeof( date_buf ), "%Y-%m-%d_%H-%M-%S", &tm );

	clip_t c{ };
	c.map          = m_current_map;
	c.tickrate     = server_tickrate( );
	c.display_name = m_current_map + " " + kind + " " + date_buf;
	c.frames       = frames;

	const auto file_path = unique_clip_path( get_root_path( ), c.display_name );
	c.display_name       = file_path.stem( ).string( );
	c.filename           = file_path.filename( ).string( );

	write_clip_to_file( file_path, c );
	movement_add_window( 0, "saved \"" + c.display_name + "\" " + std::to_string( c.frames.size( ) ) + " ticks" );
	botox_dbg_log( "REC: saved %s ticks=%d tick=%d\n", c.filename.c_str( ), static_cast< int >( c.frames.size( ) ), c.tickrate );

	m_clips.emplace_back( std::move( c ) );
	m_selected_clip = static_cast< int >( m_clips.size( ) ) - 1;
}

void n_movement_recorder::impl_t::clip_route( )
{
	const float seconds = std::clamp( GET_VARIABLE( g_variables.m_movement_rec_clip_seconds, float ), 1.f, 120.f );
	const size_t want   = static_cast< size_t >( seconds * static_cast< float >( server_tickrate( ) ) );
	const size_t take   = std::min( want, m_clip_buffer.size( ) );
	if ( take < 2 ) {
		movement_add_window( 0, std::string( "clip buffer empty" ) );
		return;
	}

	std::vector< frame_t > frames( m_clip_buffer.end( ) - static_cast< std::ptrdiff_t >( take ), m_clip_buffer.end( ) );

	/* playback walks onto frame 0 and stands: start the clip where you stood, or the replay drifts */
	size_t first = frames.size( );
	for ( size_t i = 0; i + 1 < frames.size( ); ++i ) {
		const c_vector& v = frames[ i ].velocity;
		if ( frames[ i ].has_velocity && std::sqrtf( v.m_x * v.m_x + v.m_y * v.m_y ) < 5.f && std::fabs( v.m_z ) < 5.f ) {
			first = i;
			break;
		}
	}

	if ( first < frames.size( ) && frames.size( ) - first >= 2 )
		frames.erase( frames.begin( ), frames.begin( ) + static_cast< std::ptrdiff_t >( first ) );
	else
		movement_add_window( 0, std::string( "clip starts moving: replay may drift" ) );

	save_frames( frames, "Clip" );
}

void n_movement_recorder::impl_t::delete_clip( size_t index )
{
	std::scoped_lock lock( m_clips_mutex );

	if ( index >= m_clips.size( ) )
		return;
	std::error_code ec;
	std::filesystem::remove( get_root_path( ) / m_clips[ index ].filename, ec );
	m_clips.erase( m_clips.begin( ) + index );
	if ( m_selected_clip >= static_cast< int >( m_clips.size( ) ) )
		m_selected_clip = -1;
	if ( m_editor_clip_index == static_cast< int >( index ) )
		m_editor_open = false;
	else if ( m_editor_clip_index > static_cast< int >( index ) )
		--m_editor_clip_index;
}

void n_movement_recorder::impl_t::rename_clip( size_t index, const std::string& new_name )
{
	if ( index >= m_clips.size( ) )
		return;

	auto& clip               = m_clips[ index ];
	std::string display_name = sanitize_clip_name( new_name );
	if ( display_name.empty( ) )
		display_name = clip.display_name.empty( ) ? "Clip" : clip.display_name;

	const auto old_path = get_root_path( ) / clip.filename;
	auto new_path       = get_root_path( ) / ( display_name + ".mr" );
	if ( new_path != old_path && std::filesystem::exists( new_path ) )
		new_path = unique_clip_path( get_root_path( ), display_name );

	if ( new_path != old_path ) {
		std::error_code ec;
		std::filesystem::rename( old_path, new_path, ec );
		if ( ec )
			std::filesystem::remove( old_path, ec );
	}

	clip.display_name = display_name;
	clip.filename     = display_name + ".mr";
}

void n_movement_recorder::impl_t::open_clip_editor( size_t index )
{
	std::scoped_lock lock( m_clips_mutex );

	if ( index >= m_clips.size( ) || m_clips[ index ].frames.empty( ) )
		return;

	m_editor_open                 = true;
	m_editor_ignore_enter_release = true;
	m_editor_clip_index           = static_cast< int >( index );
	m_editor_trim_start           = 0;
	m_editor_trim_end             = static_cast< int >( m_clips[ index ].frames.size( ) ) - 1;
	std::memset( m_editor_name, 0, sizeof( m_editor_name ) );
	strncpy_s( m_editor_name, m_clips[ index ].display_name.c_str( ), sizeof( m_editor_name ) - 1 );
}

void n_movement_recorder::impl_t::apply_clip_editor( )
{
	if ( m_editor_clip_index < 0 || m_editor_clip_index >= static_cast< int >( m_clips.size( ) ) )
		return;

	auto& clip = m_clips[ m_editor_clip_index ];
	if ( clip.frames.empty( ) )
		return;
	const std::string old_filename = clip.filename;

	const int max_frame = static_cast< int >( clip.frames.size( ) ) - 1;
	m_editor_trim_start = std::clamp( m_editor_trim_start, 0, max_frame );
	m_editor_trim_end   = std::clamp( m_editor_trim_end, 0, max_frame );
	if ( m_editor_trim_start > m_editor_trim_end )
		std::swap( m_editor_trim_start, m_editor_trim_end );

	if ( m_editor_trim_start > 0 || m_editor_trim_end < max_frame ) {
		std::vector< frame_t > trimmed( clip.frames.begin( ) + m_editor_trim_start, clip.frames.begin( ) + m_editor_trim_end + 1 );
		clip.frames         = std::move( trimmed );
		m_editor_trim_start = 0;
		m_editor_trim_end   = static_cast< int >( clip.frames.size( ) ) - 1;
	}

	rename_clip( static_cast< size_t >( m_editor_clip_index ), m_editor_name );
	write_clip_to_file( get_root_path( ) / m_clips[ m_editor_clip_index ].filename, m_clips[ m_editor_clip_index ] );
	m_editor_open = false;
	movement_add_window( 0, "edited \"" + old_filename + "\" to \"" + m_clips[ m_editor_clip_index ].filename + "\"" );
}

bool n_movement_recorder::impl_t::play_clip( size_t index )
{
	std::scoped_lock lock( m_clips_mutex );

	if ( index >= m_clips.size( ) )
		return false;
	const auto& c = m_clips[ index ];
	if ( c.frames.empty( ) )
		return false;
	if ( !playable( c ) ) {
		movement_add_window( 0, "\"" + c.display_name + "\" is " + std::to_string( c.tickrate ) + " tick, server is " +
		                            std::to_string( server_tickrate( ) ) );
		return false;
	}
	if ( !g_ctx.m_local )
		return false;
	if ( g_prediction.backup_data.m_origin.dist_to( c.frames[ 0 ].position ) >= k_start_radius )
		return false;

	m_play_frames       = c.frames;
	m_active_route_name = c.display_name;
	m_selected_clip     = static_cast< int >( index );
	m_wish_to_start     = true;
	m_approach_ticks    = 0;
	m_smooth_t0         = now_real( );
	g_interfaces.m_engine_client->get_view_angles( m_smooth_from );
	apply_replay_jetpack( false );

	botox_dbg_log( "REC: play %s ticks=%d dist=%.2f yaw=%d in=%.2f out=%.2f\n", c.filename.c_str( ), static_cast< int >( c.frames.size( ) ),
	               g_prediction.backup_data.m_origin.dist_to( c.frames[ 0 ].position ), GET_VARIABLE( g_variables.m_movement_rec_yaw, int ),
	               GET_VARIABLE( g_variables.m_movement_rec_smooth_start, float ), GET_VARIABLE( g_variables.m_movement_rec_smooth_end, float ) );
	return true;
}

void n_movement_recorder::impl_t::reset_all( )
{
	force_stop( );
	m_recording = false;
	m_clip_buffer.clear( );
}

void n_movement_recorder::impl_t::check_map_change( )
{
	static bool s_was_in_game = false;

	const bool in_game = g_interfaces.m_engine_client && g_interfaces.m_engine_client->is_in_game( );
	if ( !in_game ) {
		s_was_in_game = false;
		m_current_map.clear( );
		m_clips.clear( );
		m_selected_clip = -1;
		m_recording_frames.clear( );
		m_editor_open = false;
		reset_all( );
		return;
	}

	const char* level = g_interfaces.m_engine_client->get_level_name_short( );
	if ( !level || !*level )
		return;

	if ( s_was_in_game && m_current_map == level )
		return;

	s_was_in_game  = true;
	m_current_map  = level;
	m_round_frozen = false;
	m_selected_clip = -1;
	m_editor_open   = false;
	m_recording_frames.clear( );
	reset_all( );
	refresh_clips( );

	movement_add_window( 0, std::string{ "joined to: " } + m_current_map );
}

void n_movement_recorder::impl_t::force_stop( bool keep_played )
{
	const bool was_playing = m_playing;
	const size_t played    = m_play_idx;

	apply_replay_jetpack( false );
	m_playing        = false;
	m_wish_to_start  = false;
	m_play_idx       = 0;
	m_approach_ticks = 0;

	/* take over: the ticks already sent are the newest in the clip buffer, keep recording from there */
	if ( keep_played && was_playing && played > 0 ) {
		const size_t n = std::min( played, m_clip_buffer.size( ) );
		m_recording_frames.assign( m_clip_buffer.end( ) - static_cast< std::ptrdiff_t >( n ), m_clip_buffer.end( ) );
		m_recording = true;
		movement_add_window( 0, "took over at tick " + std::to_string( n ) + ", recording" );
	}
}

void n_movement_recorder::impl_t::set_round_frozen( bool frozen )
{
	m_round_frozen = frozen;
	if ( frozen )
		reset_all( );
}

void n_movement_recorder::impl_t::apply_replay_jetpack( bool active )
{
	if ( m_replay_jetpack_active == active )
		return;
	m_replay_jetpack_active = active;
	if ( g_interfaces.m_engine_client )
		g_interfaces.m_engine_client->execute_client_cmd( active ? "+jetpack" : "-jetpack" );
}

/* true = already holding it ( or nothing recorded / owned ); false = cmd now asks for the switch */
bool n_movement_recorder::impl_t::select_weapon( c_user_cmd* cmd, const frame_t& frame )
{
	if ( frame.weapon_slot < 0 || !g_ctx.m_local )
		return true;

	const auto active    = g_interfaces.m_client_entity_list->get< c_base_entity >( g_ctx.m_local->get_active_weapon_handle( ) );
	const short held_def = active ? active->get_item_definition_index( ) : 0;
	if ( held_def == frame.weapon_def )
		return true;

	int same_def = 0, same_slot = 0;
	const auto weapons = g_ctx.m_local->get_weapons_handle( );
	for ( int i = 0; i < 64; ++i ) {
		const unsigned int handle = weapons[ i ];
		if ( !handle || handle == 0xFFFFFFFF )
			continue;

		const auto entity = reinterpret_cast< c_base_entity* >( g_interfaces.m_client_entity_list->get_client_entity_from_handle( handle ) );
		if ( !entity )
			continue;

		const short def = entity->get_item_definition_index( );
		if ( def == frame.weapon_def ) {
			same_def = entity->get_index( );
			break;
		}

		const auto data = g_interfaces.m_weapon_system ? g_interfaces.m_weapon_system->get_weapon_data( def ) : nullptr;
		if ( !same_slot && data && data->m_slot == frame.weapon_slot )
			same_slot = entity->get_index( );
	}

	const int target = same_def ? same_def : same_slot;
	if ( !target || ( active && active->get_index( ) == target ) )
		return true;

	cmd->m_weapon_select = target;
	return false;
}

void n_movement_recorder::impl_t::capture_user_input( c_user_cmd* cmd )
{
	if ( !cmd )
		return;

	m_user_input          = frame_t( cmd, g_prediction.backup_data.m_origin );
	m_user_move_mask_prev = m_user_move_mask;
	m_user_move_mask      = cmd->m_buttons & ( in_forward | in_back | in_moveleft | in_moveright | in_jump | in_duck );
}

/* cmd-only: create_move calls it early too, so features + prediction see the replayed inputs */
void n_movement_recorder::impl_t::apply_playback( c_user_cmd* cmd ) const
{
	if ( !cmd || !m_playing || m_play_idx >= m_play_frames.size( ) || !GET_VARIABLE( g_variables.m_movement_rec, bool ) )
		return;

	const c_angle backup_view = cmd->m_view_point;
	m_play_frames[ m_play_idx ].replay( cmd );

	if ( GET_VARIABLE( g_variables.m_movement_rec_force_weapon, bool ) ) {
		cmd->m_weapon_select   = 0;
		cmd->m_weapon_sub_type = 0;
		select_weapon( cmd, m_play_frames[ m_play_idx ] );
	}

	/* movement fix keeps the recorded world wish dir under any sent view; same view = untouched, bit exact replay */
	const c_angle view = camera_locked( ) ? camera_at( m_play_idx, now_real( ) ) : backup_view;
	if ( view.m_x == cmd->m_view_point.m_x && view.m_y == cmd->m_view_point.m_y )
		return;
	start_movement_fix( cmd );
	cmd->m_view_point = view;
	end_movement_fix( cmd );
}

/* smooth end: offset eases to 0, spin decelerates to a stop over the last n seconds */
float n_movement_recorder::impl_t::yaw_offset( size_t idx ) const
{
	const int mode = GET_VARIABLE( g_variables.m_movement_rec_yaw, int );
	if ( mode <= 0 || m_play_frames.empty( ) )
		return 0.f;

	const float n = static_cast< float >( m_play_frames.size( ) - 1 );
	const float m = std::clamp( GET_VARIABLE( g_variables.m_movement_rec_smooth_end, float ) * static_cast< float >( server_tickrate( ) ), 0.f, n );
	const float i = std::min( static_cast< float >( idx ), n );

	if ( mode == 4 ) {
		const float s = GET_VARIABLE( g_variables.m_movement_rec_spin_speed, float ) / static_cast< float >( server_tickrate( ) );
		const float e = n - m;
		if ( m < 1.f || i <= e )
			return s * i;
		const float d = i - e;
		return s * ( e + d - d * d / ( 2.f * m ) );
	}

	const float base = mode == 1 ? 90.f : mode == 2 ? -90.f : 180.f;
	return m < 1.f ? base : base * smoothstep01( ( n - i ) / m );
}

/* smooth start: blend from the view at play press to the route camera */
c_angle n_movement_recorder::impl_t::camera_at( size_t idx, float now ) const
{
	const frame_t& f = m_play_frames[ std::min( idx, m_play_frames.size( ) - 1 ) ];

	const float dur = GET_VARIABLE( g_variables.m_movement_rec_smooth_start, float );
	const float w   = dur > 0.01f ? smoothstep01( ( now - m_smooth_t0 ) / dur ) : 1.f;
	if ( w >= 1.f )
		return c_angle( f.view_pitch, std::remainder( f.view_yaw + yaw_offset( idx ), 360.f ), 0.f );

	const float pitch = m_smooth_from.m_x + ( f.view_pitch - m_smooth_from.m_x ) * w;
	const float yaw   = m_smooth_from.m_y + std::remainder( f.view_yaw - m_smooth_from.m_y, 360.f ) * w + yaw_offset( idx ) * w;
	return c_angle( pitch, std::remainder( yaw, 360.f ), 0.f );
}

void n_movement_recorder::impl_t::on_create_move( c_user_cmd* cmd )
{
	if ( !cmd )
		return;

	const unsigned requests = m_requests.exchange( 0u );

	if ( !GET_VARIABLE( g_variables.m_movement_rec, bool ) ) {
		if ( m_playing || m_wish_to_start || m_recording || !m_clip_buffer.empty( ) )
			reset_all( );
		return;
	}

	std::scoped_lock lock( m_clips_mutex );

	/* freeze time = can't move: moving means a missed round_freeze_end */
	if ( m_round_frozen ) {
		const c_vector& v = g_prediction.backup_data.m_velocity;
		if ( v.m_x * v.m_x + v.m_y * v.m_y > 1.f ) {
			m_round_frozen = false;
			botox_dbg_log( "REC: unfrozen by movement\n" );
		}
	}

	if ( !g_ctx.m_local || !g_ctx.m_local->is_alive( ) || m_round_frozen ) {
		reset_all( );
		return;
	}

	struct edge_t {
		std::uint32_t m_var;
		bool m_was = false;

		bool pressed( )
		{
			const int key   = GET_VARIABLE( m_var, key_bind_t ).m_key;
			const bool now  = key > 0 && key < 256 && g_input.is_key_down( static_cast< std::uint32_t >( key ) ) && !g_input.keys_blocked( );
			const bool edge = now && !m_was;
			m_was           = now;
			return edge;
		}
	};

	static edge_t k_start_rec{ g_variables.m_movement_rec_keystartrecord }, k_stop_rec{ g_variables.m_movement_rec_keystoprecord },
		k_save{ g_variables.m_movement_rec_keysaveroute }, k_start_play{ g_variables.m_movement_rec_keystartplay },
		k_stop_play{ g_variables.m_movement_rec_keystopplay }, k_clear{ g_variables.m_movement_rec_keyclearrecord },
		k_clip{ g_variables.m_movement_rec_keyclip };

	const bool menu_open  = g_menu.m_opened;
	const bool start_rec  = ( k_start_rec.pressed( ) && !menu_open ) || ( requests & action_start_rec );
	const bool stop_rec   = ( k_stop_rec.pressed( ) && !menu_open ) || ( requests & action_stop_rec );
	const bool save       = ( k_save.pressed( ) && !menu_open ) || ( requests & action_save );
	const bool start_play = ( k_start_play.pressed( ) && !menu_open ) || ( requests & action_start_play );
	const bool stop_play  = ( k_stop_play.pressed( ) && !menu_open ) || ( requests & action_stop_play );
	const bool clear      = ( k_clear.pressed( ) && !menu_open ) || ( requests & action_clear );
	const bool clip       = ( k_clip.pressed( ) && !menu_open ) || ( requests & action_clip );

	/* fresh press only: still holding w from before play must not kill it */
	const bool user_moved = GET_VARIABLE( g_variables.m_movement_rec_stop_on_move, bool ) && ( m_user_move_mask & ~m_user_move_mask_prev ) != 0;

	if ( ( stop_play || user_moved ) && owns_cmd( ) ) {
		botox_dbg_log( "REC: stop play idx=%d/%d moved=%d\n", static_cast< int >( m_play_idx ), static_cast< int >( m_play_frames.size( ) ),
		               user_moved ? 1 : 0 );
		force_stop( true );
		if ( user_moved )
			m_user_input.replay( cmd );
	}

	if ( start_rec && !owns_cmd( ) && !m_recording ) {
		m_recording_frames.clear( );
		m_recording = true;
		movement_add_window( 0, std::string( "recording" ) );
	}

	if ( stop_rec && m_recording ) {
		m_recording = false;
		movement_add_window( 0, "stopped recording, " + std::to_string( m_recording_frames.size( ) ) + " ticks" );
	}

	if ( clear && !m_recording && !owns_cmd( ) ) {
		m_recording_frames.clear( );
		movement_add_window( 0, std::string( "cleared route" ) );
	}

	if ( save ) {
		m_recording = false;
		save_frames( m_recording_frames, "Route" );
	}

	if ( clip )
		clip_route( );

	if ( start_play && !m_recording && !owns_cmd( ) ) {
		const c_vector& cur_origin = g_prediction.backup_data.m_origin;
		size_t target_idx          = static_cast< size_t >( -1 );

		if ( m_selected_clip >= 0 && m_selected_clip < static_cast< int >( m_clips.size( ) ) && playable( m_clips[ m_selected_clip ] ) &&
		     cur_origin.dist_to( m_clips[ m_selected_clip ].frames[ 0 ].position ) < k_start_radius ) {
			target_idx = static_cast< size_t >( m_selected_clip );
		} else {
			float best_dist = k_start_radius;
			for ( size_t i = 0; i < m_clips.size( ); ++i ) {
				if ( m_clips[ i ].frames.empty( ) )
					continue;
				const float d = cur_origin.dist_to( m_clips[ i ].frames[ 0 ].position );
				if ( d < best_dist && ( playable( m_clips[ i ] ) || target_idx == static_cast< size_t >( -1 ) ) ) {
					target_idx = i;
					best_dist  = d;
				}
			}
		}

		if ( target_idx != static_cast< size_t >( -1 ) )
			play_clip( target_idx );
		else
			movement_add_window( 0, std::string( "no route start nearby" ) );
	}

	if ( m_wish_to_start && !m_play_frames.empty( ) ) {
		const frame_t& first    = m_play_frames.front( );
		const c_vector& cur_pos = g_prediction.backup_data.m_origin;
		const float dist        = cur_pos.dist_to( first.position );

		if ( camera_locked( ) ) {
			c_angle start_view = camera_at( 0, now_real( ) );
			cmd->m_view_point    = start_view;
			cmd->m_mouse_delta_x = 0;
			cmd->m_mouse_delta_y = 0;
			g_interfaces.m_engine_client->set_view_angles( start_view );
		}

		const float give_up_ticks = k_approach_give_up * static_cast< float >( server_tickrate( ) );
		const bool timed_out      = static_cast< float >( ++m_approach_ticks ) > give_up_ticks;
		const bool at_start       = dist <= k_pos_close_eps || ( timed_out && dist < 1.f );

		/* weapon sets max speed: start only once the recorded one is out */
		bool weapon_ok = true;
		if ( GET_VARIABLE( g_variables.m_movement_rec_force_weapon, bool ) ) {
			cmd->m_weapon_select   = 0;
			cmd->m_weapon_sub_type = 0;
			weapon_ok              = select_weapon( cmd, first );
		}

		if ( at_start && weapon_ok ) {
			m_wish_to_start           = false;
			m_playing                 = true;
			m_play_idx                = 0;
			m_route_render_start_time = g_interfaces.m_global_vars_base->m_current_time;
			botox_dbg_log( "REC: start dist=%.3f approach=%d wpn=%d\n", dist, m_approach_ticks, static_cast< int >( first.weapon_def ) );
		} else if ( timed_out && !at_start ) {
			botox_dbg_log( "REC: approach gave up dist=%.2f\n", dist );
			movement_add_window( 0, std::string( "couldn't reach route start" ) );
			force_stop( );
		} else {
			const c_vector diff = cur_pos - first.position;
			const float yaw_rad = cmd->m_view_point.m_y * 3.14159265358979323846f / 180.f;
			const float c       = std::cos( yaw_rad );
			const float s       = std::sin( yaw_rad );

			cmd->m_forward_move = -( diff.m_x * c + diff.m_y * s ) * 20.f;
			cmd->m_side_move    = ( diff.m_y * c - diff.m_x * s ) * 20.f;
			cmd->m_up_move      = 0.f;
			cmd->m_buttons &= ~( in_jump | in_duck | in_forward | in_back | in_moveleft | in_moveright | in_speed );
			cmd->m_buttons |= first.buttons & in_duck;
		}
	}

	if ( m_playing ) {
		if ( m_play_idx >= m_play_frames.size( ) ) {
			botox_dbg_log( "REC: done ticks=%d\n", static_cast< int >( m_play_frames.size( ) ) );
			force_stop( );
		} else {
			apply_replay_jetpack( m_play_frames[ m_play_idx ].jetpack );
			apply_playback( cmd );
			++m_play_idx;
			m_step_realtime = now_real( );
		}
	}

	cmd->m_view_point.normalize( );

	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
	frame_t f( cmd, g_ctx.m_local->get_origin( ) );
	f.jetpack = m_replay_jetpack_active;

	const size_t cap = static_cast< size_t >( std::clamp( GET_VARIABLE( g_variables.m_movement_rec_clip_seconds, float ), 1.f, 120.f ) *
	                                          static_cast< float >( server_tickrate( ) ) );
	m_clip_buffer.push_back( f );
	while ( m_clip_buffer.size( ) > cap )
		m_clip_buffer.pop_front( );

	if ( m_recording )
		m_recording_frames.push_back( f );
}

void n_movement_recorder::impl_t::on_frame_stage( int stage )
{
	if ( stage != static_cast< int >( e_client_frame_stage::start ) )
		return;
	if ( !camera_locked( ) || m_play_frames.empty( ) )
		return;

	const float now = now_real( );

	/* approach: per frame so smooth start eases at fps, not tick rate */
	if ( m_wish_to_start ) {
		c_angle view = camera_at( 0, now );
		g_interfaces.m_engine_client->set_view_angles( view );
		return;
	}

	if ( !m_playing || m_play_idx == 0 || m_play_idx >= m_play_frames.size( ) )
		return;

	/* sent frame idx-1 last tick, idx goes out next: lerp by real time so the camera is smooth at any fps */
	const c_angle from = camera_at( m_play_idx - 1, now );
	const c_angle to   = camera_at( m_play_idx, now );

	const float interval = g_interfaces.m_global_vars_base->m_interval_per_tick;
	const float progress = std::clamp( interval > 1e-6f ? ( now - m_step_realtime ) / interval : 1.f, 0.f, 1.f );

	c_angle view{ from.m_x + ( to.m_x - from.m_x ) * progress, wrap_180( from.m_y + wrap_180( to.m_y - from.m_y ) * progress ), 0.f };
	g_interfaces.m_engine_client->set_view_angles( view );
}

void n_movement_recorder::impl_t::camera_lock( float* x, float* y )
{
	if ( !x || !y || !GET_VARIABLE( g_variables.m_movement_rec, bool ) )
		return;

	const bool lockva = camera_locked( );
	if ( ( m_playing && lockva ) || ( m_wish_to_start && ( lockva || GET_VARIABLE( g_variables.m_movement_rec_lockgoingtostart, bool ) ) ) ) {
		*x = 0.f;
		*y = 0.f;
	}
}

static unsigned int rgba( int r, int g, int b, int a )
{
	return ImGui::GetColorU32( ImVec4( r / 255.f, g / 255.f, b / 255.f, a / 255.f ) );
}

static unsigned int accent_u32( float alpha = 1.f )
{
	const ImVec4 c = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );
	return ImGui::GetColorU32( ImVec4( c.x, c.y, c.z, alpha ) );
}

static void enqueue_line( const c_vector_2d& a, const c_vector_2d& b, unsigned int col, float th )
{
	line_draw_object_t obj{ };
	obj.m_start     = a;
	obj.m_end       = b;
	obj.m_color     = col;
	obj.m_thickness = th;
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_line, std::make_any< line_draw_object_t >( obj ) );
}

static void enqueue_rect( const c_vector_2d& min, const c_vector_2d& max, unsigned int col )
{
	rect_draw_object_t obj{ };
	obj.m_min           = min;
	obj.m_max           = max;
	obj.m_color         = col;
	obj.m_outline_color = col;
	obj.m_filled        = true;
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_rect, std::make_any< rect_draw_object_t >( obj ) );
}

static void enqueue_dot( const c_vector_2d& c, float r, unsigned int fill, unsigned int outline )
{
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle,
	                                   std::make_any< filled_circle_draw_object_t >( filled_circle_draw_object_t( c, r, fill, 32 ) ) );
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_circle, std::make_any< circle_draw_object_t >( circle_draw_object_t( c, r, outline, 32 ) ) );
}

static void enqueue_world_floor_disc( const c_vector& center, float radius, unsigned int fill_col, unsigned int outline_col, int seg,
                                      float outline_th = 1.5f )
{
	std::vector< ImVec2 > points;
	points.reserve( seg );

	for ( int i = 0; i < seg; ++i ) {
		const float a = ( static_cast< float >( i ) / static_cast< float >( seg ) ) * 6.28318530718f;
		const c_vector p{ center.m_x + std::cos( a ) * radius, center.m_y + std::sin( a ) * radius, center.m_z + 1.f };
		c_vector_2d screen{ };
		if ( !g_render.world_to_screen( p, screen ) )
			return;
		points.emplace_back( screen.m_x, screen.m_y );
	}

	if ( points.size( ) < 3 )
		return;

	/* render thread: capture owns the points */
	callback_draw_object_t obj{ };
	obj.m_callback = [ points = std::move( points ), fill_col, outline_col, outline_th ]( ImDrawList* list ) {
		list->AddConvexPolyFilled( points.data( ), static_cast< int >( points.size( ) ), fill_col );
		list->AddPolyline( points.data( ), static_cast< int >( points.size( ) ), outline_col, ImDrawFlags_Closed, outline_th );
	};
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_callback, std::make_any< callback_draw_object_t >( std::move( obj ) ) );
}

static void enqueue_text( ImFont* font, const c_vector_2d& pos, const std::string& text, unsigned int col, float scale = 1.f )
{
	text_draw_object_t obj{ };
	obj.m_font          = font;
	obj.m_position      = pos;
	obj.m_text          = text;
	obj.m_color         = col;
	obj.m_outline_color = ImGui::GetColorU32( ImVec4( 0.f, 0.f, 0.f, 1.f ) );
	obj.m_draw_flags    = static_cast< e_text_flags >( text_flag_outline | text_flag_dropshadow );
	obj.m_font_size     = font->FontSize * scale;
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text, std::make_any< text_draw_object_t >( obj ) );
}

void n_movement_recorder::impl_t::render_path( const std::vector< frame_t >& frames, float start_time )
{
	if ( !GET_VARIABLE( g_variables.m_movement_rec_show_line, bool ) )
		return;
	if ( frames.size( ) < 2 )
		return;

	const float progress = std::clamp( ( g_interfaces.m_global_vars_base->m_current_time - start_time ) * 4.f, 0.f, 1.f );

	const ImVec4 acc       = ImGui::GetStyleColorVec4( ImGuiCol_::ImGuiCol_Accent );
	const int alpha        = static_cast< int >( 255.f * progress );
	const float r          = 255.f + ( acc.x * 255.f - 255.f ) * progress;
	const float g          = 255.f + ( acc.y * 255.f - 255.f ) * progress;
	const float b          = 255.f + ( acc.z * 255.f - 255.f ) * progress;
	const unsigned int col = rgba( static_cast< int >( r ), static_cast< int >( g ), static_cast< int >( b ), alpha );

	enqueue_world_floor_disc( frames[ 0 ].position, k_start_radius * 0.45f, rgba( 0, 0, 0, 100 ), accent_u32( 0.85f ), 64, 1.5f );

	c_vector_2d prev{ };
	bool has_prev = false;
	for ( const frame_t& f : frames ) {
		c_vector_2d cur{ };
		const bool on_screen = g_render.world_to_screen( f.position, cur );
		if ( on_screen && has_prev )
			enqueue_line( prev, cur, col, 0.5f );
		prev     = cur;
		has_prev = on_screen;
	}
}

void n_movement_recorder::impl_t::render_route_preview( )
{
	if ( m_clips.empty( ) )
		return;

	const float dt = ImGui::GetIO( ).DeltaTime;
	if ( m_point_progress.size( ) != m_clips.size( ) )
		m_point_progress.assign( m_clips.size( ), 1.f );

	const bool enter_pressed = !m_editor_open && !g_menu.m_opened && !g_input.keys_blocked( ) && g_input.is_key_released( VK_RETURN );

	ImFont* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];

	for ( size_t i = 0; i < m_clips.size( ); ++i ) {
		const auto& route = m_clips[ i ];
		if ( route.frames.empty( ) )
			continue;

		const frame_t& first      = route.frames[ 0 ];
		const c_vector& start_pos = first.position;
		const float distance      = g_ctx.m_local->get_origin( ).dist_to( start_pos );
		if ( enter_pressed && distance < k_start_radius ) {
			m_selected_clip = static_cast< int >( i );
			open_clip_editor( i );
			return;
		}

		if ( distance < k_start_radius && font ) {
			c_vector forward{ };
			g_math.angle_vectors( c_angle( first.view_pitch, first.view_yaw, 0.f ), &forward );
			c_vector_2d aim{ };
			if ( g_render.world_to_screen( g_ctx.m_local->get_eye_position( false ) + forward * 100.f, aim ) ) {
				enqueue_dot( aim, 4.f, rgba( 0, 0, 0, 100 ), rgba( 255, 255, 255, 255 ) );
				const ImVec2 ts = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, "aim here" );
				enqueue_text( font, c_vector_2d{ aim.m_x + 7.f, aim.m_y - ts.y * 0.5f }, "aim here", rgba( 255, 255, 255, 255 ) );
			}
		}

		c_vector_2d screen{ };
		if ( !g_render.world_to_screen( start_pos, screen ) )
			continue;

		m_point_progress[ i ] += ( 1.f - m_point_progress[ i ] ) * std::clamp( dt * 5.f, 0.f, 1.f );

		float alpha_mul = 1.f;
		if ( distance > 900.f )
			alpha_mul = std::clamp( ( 1000.f - distance ) / 100.f, 0.f, 1.f );
		if ( alpha_mul <= 0.f )
			continue;

		const float effective_r = k_start_radius * 0.45f * m_point_progress[ i ];

		enqueue_world_floor_disc( start_pos, effective_r, rgba( 0, 0, 0, static_cast< int >( 100.f * alpha_mul ) ), accent_u32( 0.85f * alpha_mul ), 64,
		                          1.5f );

		if ( font && !route.display_name.empty( ) ) {
			constexpr float title_scale = 0.82f;
			const std::string title     = route.label( );
			const ImVec2 text_size      = font->CalcTextSizeA( font->FontSize * title_scale, FLT_MAX, 0.f, title.c_str( ) );
			enqueue_text( font, c_vector_2d{ screen.m_x - text_size.x * 0.5f, screen.m_y - 34.f }, title,
			              rgba( 225, 225, 225, static_cast< int >( 255.f * alpha_mul ) ), title_scale );
		}
	}
}

void n_movement_recorder::impl_t::render_indicator( )
{
	if ( !GET_VARIABLE( g_variables.m_movement_rec_render, bool ) )
		return;

	const float dt     = ImGui::GetIO( ).DeltaTime;
	const float target = ( m_recording || owns_cmd( ) ) ? 1.f : 0.f;
	m_indicator_alpha  = std::clamp( m_indicator_alpha + ( target - m_indicator_alpha ) * ( 1.f - std::exp( -k_indicator_lerp_k * dt ) ), 0.f, 1.f );
	if ( m_indicator_alpha <= 0.01f )
		return;

	ImFont* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	if ( !font )
		return;

	const std::string text = m_recording ? "rec " + std::to_string( m_recording_frames.size( ) )
	                                     : std::to_string( m_play_idx ) + " / " + std::to_string( m_play_frames.size( ) );

	const ImVec2 ts = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, text.c_str( ) );
	const ImVec2 padding{ 7.f, 7.f };
	const ImVec2 margin{ 4.f, 3.f };
	const ImVec2 size{ ts.x + margin.x * 2.f, ts.y + margin.y * 2.f };
	const ImVec2 display{ g_ctx.m_width, g_ctx.m_height };

	c_vector_2d pos{ };
	switch ( GET_VARIABLE( g_variables.m_movement_rec_position, int ) ) {
	default:
	case 0: pos = { padding.x, padding.y }; break;
	case 1: pos = { padding.x, display.y - size.y - padding.y }; break;
	case 2: pos = { display.x - size.x - padding.x, display.y - size.y - padding.y }; break;
	}

	g_render.queue_stretch_block_begin( );
	enqueue_rect( pos, c_vector_2d{ pos.m_x + size.x, pos.m_y + size.y }, ImGui::GetColorU32( ImVec4( 0.08f, 0.08f, 0.08f, m_indicator_alpha ) ) );
	enqueue_rect( c_vector_2d{ pos.m_x, pos.m_y - 1.f }, c_vector_2d{ pos.m_x + size.x, pos.m_y }, accent_u32( m_indicator_alpha ) );
	enqueue_text( font, c_vector_2d{ pos.m_x + margin.x, pos.m_y + margin.y }, text, ImGui::GetColorU32( ImVec4( 1.f, 1.f, 1.f, m_indicator_alpha ) ) );
	g_render.queue_stretch_block_end( );
}

void n_movement_recorder::impl_t::render_clipper_box( )
{
	if ( !GET_VARIABLE( g_variables.m_movement_rec_clipper_box, bool ) )
		return;

	const float dt = g_interfaces.m_global_vars_base->m_interval_per_tick;

	char buf[ 48 ]{ };
	if ( m_playing )
		std::snprintf( buf, sizeof( buf ), "%.1f / %.1f", static_cast< float >( m_play_idx ) * dt, static_cast< float >( m_play_frames.size( ) ) * dt );
	else if ( m_recording )
		std::snprintf( buf, sizeof( buf ), "rec %.1f", static_cast< float >( m_recording_frames.size( ) ) * dt );
	else if ( !m_clip_buffer.empty( ) )
		std::snprintf( buf, sizeof( buf ), "clip %.1f", static_cast< float >( m_clip_buffer.size( ) ) * dt );
	else
		return;

	ImFont* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	if ( !font )
		return;

	ImGui::PushFont( font );
	const float width = ImGui::CalcTextSize( buf ).x + 14.f;
	ImGui::SetNextWindowPos( ImVec2( 24.f, 96.f ), ImGuiCond_FirstUseEver );
	ImGui::SetNextWindowSize( ImVec2( width, 22.f ), ImGuiCond_Always );
	ImGui::PushStyleColor( ImGuiCol_WindowBg, ImVec4( 0.04f, 0.04f, 0.04f, 1.f ) );
	ImGui::PushStyleColor( ImGuiCol_Border, ImVec4( 0.f, 0.f, 0.f, 1.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 3.f );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 5.f, 4.f ) );
	if ( ImGui::Begin( "##mr_clipper_box", nullptr,
	                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar ) ) {
		const ImVec2 text_size = ImGui::CalcTextSize( buf );
		ImGui::SetCursorPosX( ( ImGui::GetWindowSize( ).x - text_size.x ) * 0.5f );
		ImGui::TextUnformatted( buf );
	}
	ImGui::End( );
	ImGui::PopStyleVar( 2 );
	ImGui::PopStyleColor( 2 );
	ImGui::PopFont( );
}

void n_movement_recorder::impl_t::render_clip_editor( )
{
	if ( !m_editor_open )
		return;

	std::scoped_lock lock( m_clips_mutex );

	if ( m_editor_ignore_enter_release ) {
		m_editor_ignore_enter_release = false;
	} else if ( g_input.is_key_released( VK_ESCAPE ) || ( g_input.is_key_released( VK_RETURN ) && !ImGui::IsAnyItemActive( ) ) ) {
		m_editor_open = false;
		return;
	}
	if ( m_editor_clip_index < 0 || m_editor_clip_index >= static_cast< int >( m_clips.size( ) ) ) {
		m_editor_open = false;
		return;
	}

	auto& clip = m_clips[ m_editor_clip_index ];
	if ( clip.frames.empty( ) ) {
		m_editor_open = false;
		return;
	}

	ImFont* font         = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	const ImVec2 display = ImGui::GetIO( ).DisplaySize;
	ImGui::SetNextWindowPos( ImVec2( display.x - 330.f, 20.f ), ImGuiCond_FirstUseEver );
	ImGui::SetNextWindowSize( ImVec2( 310.f, 165.f ), ImGuiCond_FirstUseEver );
	ImGui::PushStyleColor( ImGuiCol_WindowBg, ImVec4( 0.04f, 0.04f, 0.04f, 1.f ) );
	ImGui::PushStyleColor( ImGuiCol_TitleBg, ImVec4( 0.08f, 0.08f, 0.08f, 1.f ) );
	ImGui::PushStyleColor( ImGuiCol_TitleBgActive, ImVec4( 0.08f, 0.08f, 0.08f, 1.f ) );
	ImGui::PushStyleColor( ImGuiCol_TitleBgCollapsed, ImVec4( 0.08f, 0.08f, 0.08f, 1.f ) );
	ImGui::PushStyleColor( ImGuiCol_FrameBg, ImVec4( 0.02f, 0.02f, 0.02f, 1.f ) );
	ImGui::PushStyleColor( ImGuiCol_Button, ImVec4( 0.02f, 0.02f, 0.02f, 1.f ) );
	ImGui::PushStyleColor( ImGuiCol_Border, ImVec4( 0.f, 0.f, 0.f, 1.f ) );
	ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 3.f );
	if ( font )
		ImGui::PushFont( font );

	if ( ImGui::Begin( "recorder clip", &m_editor_open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize ) ) {
		ImGui::Text( "%zu ticks  %d tick", clip.frames.size( ), clip.tickrate );
		ImGui::SetNextItemWidth( -1.f );
		ImGui::InputText( "##mr_editor_name", m_editor_name, sizeof( m_editor_name ) );

		const int max_frame = static_cast< int >( clip.frames.size( ) ) - 1;
		m_editor_trim_start = std::clamp( m_editor_trim_start, 0, max_frame );
		m_editor_trim_end   = std::clamp( m_editor_trim_end, 0, max_frame );
		ImGui::SetNextItemWidth( -1.f );
		ImGui::SliderInt( "##mr_start_frame", &m_editor_trim_start, 0, max_frame, "start tick %d" );
		ImGui::SetNextItemWidth( -1.f );
		ImGui::SliderInt( "##mr_end_frame", &m_editor_trim_end, 0, max_frame, "end tick %d" );

		if ( ImGui::Button( "apply", ImVec2( -1.f, 15.f ) ) )
			apply_clip_editor( );
		if ( ImGui::Button( "close", ImVec2( -1.f, 15.f ) ) )
			m_editor_open = false;
	}
	ImGui::End( );
	if ( font )
		ImGui::PopFont( );
	ImGui::PopStyleVar( );
	ImGui::PopStyleColor( 7 );
}

void n_movement_recorder::impl_t::on_end_scene( )
{
	if ( !GET_VARIABLE( g_variables.m_movement_rec, bool ) )
		return;
	if ( !g_interfaces.m_engine_client || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	render_clip_editor( );
	render_clipper_box( );
}

void n_movement_recorder::impl_t::on_paint_traverse( )
{
	std::scoped_lock lock( m_clips_mutex );

	check_map_change( );

	if ( !GET_VARIABLE( g_variables.m_movement_rec, bool ) )
		return;
	if ( !g_render.m_initialised )
		return;
	if ( !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	if ( !owns_cmd( ) ) {
		const float now = g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_current_time : 0.f;
		if ( now - m_last_auto_refresh > 5.f || now < m_last_auto_refresh ) {
			m_last_auto_refresh = now;
			refresh_clips( true );
		}
	}

	if ( g_ctx.m_local && g_ctx.m_local->is_alive( ) ) {
		if ( owns_cmd( ) )
			render_path( m_play_frames, m_route_render_start_time );
		else if ( m_recording )
			render_path( m_recording_frames, 0.f );
		else
			render_route_preview( );
	}

	render_indicator( );
}
