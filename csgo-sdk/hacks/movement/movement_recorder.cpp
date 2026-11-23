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
	constexpr float k_preview_line_dist = 300.f;
	constexpr float k_pos_close_eps    = 0.1f;
	constexpr float k_approach_give_up = 3.f;
	constexpr float k_approach_brake   = 400.f; /* u/s^2, under duck accel + friction: lands without overshoot */
	constexpr float k_indicator_lerp_k = 6.f;
	constexpr float k_clip_min_seconds = 0.2f;
	constexpr size_t k_clip_history    = 8;

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

	/* clipper: under 1 u/s before this cmd on ground ( ladder / noclip too when only ending: the approach can't steer there ) */
	bool at_rest( const bool ground_only )
	{
		const auto& b     = g_prediction.backup_data;
		const bool ground = ( b.m_flags & e_flags::fl_onground ) != 0;
		const bool hang   = b.m_move_type == move_type_ladder || b.m_move_type == move_type_noclip;
		return ( ground || ( !ground_only && hang ) ) && b.m_velocity.length( ) < 1.f;
	}

	bool move_input( const c_user_cmd* cmd )
	{
		return std::fabs( cmd->m_forward_move ) > 0.5f || std::fabs( cmd->m_side_move ) > 0.5f || std::fabs( cmd->m_up_move ) > 0.5f ||
		       ( cmd->m_buttons & in_jump );
	}

	/* the buttons movement features write; attack etc. always replay from the sent cmd */
	constexpr int k_move_mask = in_jump | in_duck | in_forward | in_back | in_moveleft | in_moveright;

	using frame_t = n_movement_recorder::frame_t;

	bool cmd_changed( const frame_t& f )
	{
		return f.raw_pitch != f.view_pitch || f.raw_yaw != f.view_yaw || f.raw_forward != f.forward || f.raw_side != f.side || f.raw_up != f.up ||
		       ( ( f.raw_buttons ^ f.buttons ) & k_move_mask ) != 0;
	}

	/* live = a feature bind was armed or the features changed the cmd: playback flies it again from the raw input */
	void stamp_live( frame_t& f, const frame_t& raw, const std::uint64_t* keys, const bool armed )
	{
		f.raw_pitch   = raw.view_pitch;
		f.raw_yaw     = raw.view_yaw;
		f.raw_forward = raw.forward;
		f.raw_side    = raw.side;
		f.raw_up      = raw.up;
		f.raw_buttons = raw.buttons;
		for ( int i = 0; i < 4; ++i )
			f.keys[ i ] = keys[ i ];
		f.live = armed || cmd_changed( f );
	}

	void copy_live( frame_t& f, const frame_t& from )
	{
		f.strafe_mx   = from.strafe_mx;
		f.strafe_my   = from.strafe_my;
		f.live        = from.live;
		f.raw_pitch   = from.raw_pitch;
		f.raw_yaw     = from.raw_yaw;
		f.raw_forward = from.raw_forward;
		f.raw_side    = from.raw_side;
		f.raw_up      = from.raw_up;
		f.raw_buttons = from.raw_buttons;
		for ( int i = 0; i < 4; ++i )
			f.keys[ i ] = from.keys[ i ];
	}

	/* cs:go ground accelerate is a speed command ( along wish dir: v -> min( v + step, wish ) ), so ask for the speed that
	   still brakes to 0 on the spot; last tick = exact remaining distance. world wish, duck crop undone */
	c_vector approach_wish( const c_vector& delta, const c_vector& vel, const float surface, const float duck_mod )
	{
		static auto sv_friction  = g_interfaces.m_convar->find_var( "sv_friction" );
		static auto sv_stopspeed = g_interfaces.m_convar->find_var( "sv_stopspeed" );
		const float dt           = g_interfaces.m_global_vars_base->m_interval_per_tick;

		/* friction runs first */
		c_vector v( vel.m_x, vel.m_y, 0.f );
		if ( const float s = v.length_2d( ); s >= 0.1f ) {
			const float fric = ( sv_friction ? sv_friction->get_float( ) : 5.2f ) * surface;
			const float stop = sv_stopspeed ? sv_stopspeed->get_float( ) : 80.f;
			v *= std::max( s - std::max( s, stop ) * fric * dt, 0.f ) / s;
		}

		const float dist = delta.length_2d( );
		c_vector target{ };
		if ( dist > 1e-4f )
			target = c_vector( delta.m_x, delta.m_y, 0.f ) * ( std::min( dist / dt, std::sqrt( 2.f * k_approach_brake * dist ) ) / dist );

		c_vector dir   = target - v;
		const float e = dir.length_2d( );
		if ( e < 1e-3f )
			return { };
		dir /= e;

		/* walk move zeroes < 1 u/s: 0.5 = dead stop */
		const float wish = std::max( target.dot_product( dir ), 0.5f ) / std::max( duck_mod, 0.1f );
		return dir * std::min( wish, 450.f );
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
	duck_amount  = g_prediction.backup_data.m_duck_amount;
	has_duck     = true;

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
			f.has_duck     = jf.contains( "da" );
			f.duck_amount  = jf.value( "da", 0.f );
			f.live         = jf.value( "lv", false );
			f.raw_pitch    = jf.value( "rvx", 0.f );
			f.raw_yaw      = jf.value( "rvy", 0.f );
			f.raw_forward  = jf.value( "rf", 0.f );
			f.raw_side     = jf.value( "rs", 0.f );
			f.raw_up       = jf.value( "ru", 0.f );
			f.raw_buttons  = jf.value( "rb", 0 );
			f.keys[ 0 ]    = jf.value( "k0", std::uint64_t{ 0 } );
			f.keys[ 1 ]    = jf.value( "k1", std::uint64_t{ 0 } );
			f.keys[ 2 ]    = jf.value( "k2", std::uint64_t{ 0 } );
			f.keys[ 3 ]    = jf.value( "k3", std::uint64_t{ 0 } );
			f.strafe_mx    = jf.value( "smx", 0.f );
			f.strafe_my    = jf.value( "smy", 0.f );
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

/* tmp + rename: a crash or full disk mid write never leaves a half .mr */
static bool write_clip_to_file( const std::filesystem::path& file_path, const n_movement_recorder::clip_t& clip )
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
		if ( f.has_duck )
			jf[ "da" ] = f.duck_amount;
		if ( f.live ) {
			static const char* const k_key_names[ 4 ] = { "k0", "k1", "k2", "k3" };
			jf[ "lv" ]  = true;
			jf[ "rvx" ] = f.raw_pitch;
			jf[ "rvy" ] = f.raw_yaw;
			jf[ "rf" ]  = f.raw_forward;
			jf[ "rs" ]  = f.raw_side;
			jf[ "ru" ]  = f.raw_up;
			jf[ "rb" ]  = f.raw_buttons;
			for ( int i = 0; i < 4; ++i )
				if ( f.keys[ i ] )
					jf[ k_key_names[ i ] ] = f.keys[ i ];
			if ( f.strafe_mx != 0.f || f.strafe_my != 0.f ) {
				jf[ "smx" ] = f.strafe_mx;
				jf[ "smy" ] = f.strafe_my;
			}
		}
		j[ "frames" ].push_back( jf );
	}

	auto tmp = file_path;
	tmp += ".tmp";
	{
		std::ofstream out( tmp, std::ios::trunc );
		if ( !out.good( ) )
			return false;
		out << j.dump( );
		out.close( );
		if ( out.fail( ) )
			return false;
	}

	std::error_code ec;
	std::filesystem::rename( tmp, file_path, ec );
	if ( ec ) {
		std::filesystem::remove( tmp, ec );
		return false;
	}
	return true;
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

/* ponytail: json write under m_clips_mutex, a menu save of a 300 s 128 tick clip stalls create_move that long; copy + write unlocked if it bites */
bool n_movement_recorder::impl_t::save_frames( const std::vector< frame_t >& frames, const char* kind, std::string* saved_as )
{
	std::scoped_lock lock( m_clips_mutex );

	if ( m_current_map.empty( ) )
		return false;
	if ( frames.size( ) < 2 ) {
		movement_add_window( 0, std::string( "nothing to save" ) );
		return false;
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

	if ( !write_clip_to_file( file_path, c ) ) {
		movement_add_window( 0, "couldn't write \"" + c.filename + "\"" );
		botox_dbg_log( "REC: save FAILED %s\n", c.filename.c_str( ) );
		return false;
	}
	movement_add_window( 0, "saved \"" + c.display_name + "\" " + std::to_string( c.frames.size( ) ) + " ticks" );
	botox_dbg_log( "REC: saved %s ticks=%d tick=%d\n", c.filename.c_str( ), static_cast< int >( c.frames.size( ) ), c.tickrate );

	if ( saved_as )
		*saved_as = c.display_name;
	m_clips.emplace_back( std::move( c ) );
	m_selected_clip = static_cast< int >( m_clips.size( ) ) - 1;
	return true;
}

void n_movement_recorder::impl_t::drop_clip_session( )
{
	m_clip_active      = false;
	m_clip_rolled      = false;
	m_clip_still_ticks = 0;
	m_clip_session.clear( );
}

/* session starts on input from rest ( frame 0 = standing, playback walks there and stands ), ends after the stop delay.
   moving without a rest start ( after playback, bhop chain ) never opens one */
void n_movement_recorder::impl_t::clip_tick( const frame_t& f, const bool can_start, const bool still )
{
	const float tick = static_cast< float >( server_tickrate( ) );

	/* teleport / respawn: more than the move allows ( + 18u step up ) = the run before it is the clip */
	if ( m_clip_active && !m_clip_session.empty( ) ) {
		const frame_t& last = m_clip_session.back( );
		const float reach   = std::max( last.velocity.length( ), f.velocity.length( ) ) / tick * 1.5f + 32.f;
		if ( const float jump = f.position.dist_to( last.position ); jump > reach ) {
			botox_dbg_log( "REC: clip teleport %.1fu, session closed\n", jump );
			finish_clip( );
		}
	}

	if ( !m_clip_active ) {
		if ( !can_start )
			return;
		m_clip_active      = true;
		m_clip_rolled      = false;
		m_clip_still_ticks = 0;
		m_clip_session.clear( );
	}

	m_clip_session.push_back( f );
	m_clip_still_ticks = still ? m_clip_still_ticks + 1 : 0;

	const size_t cap = static_cast< size_t >( std::clamp( GET_VARIABLE( g_variables.m_movement_rec_clip_max, float ), 5.f, 300.f ) * tick );
	while ( m_clip_session.size( ) > cap ) {
		m_clip_session.pop_front( );
		m_clip_rolled = true;
	}

	const int stop_ticks =
		std::max( 1, static_cast< int >( std::lround( std::clamp( GET_VARIABLE( g_variables.m_movement_rec_clip_stop_delay, float ), 0.f, 2.f ) * tick ) ) );
	if ( m_clip_still_ticks >= stop_ticks )
		finish_clip( );
}

/* trailing still ticks cut; too short = a tap, dropped ( false ) */
bool n_movement_recorder::impl_t::finish_clip( )
{
	std::scoped_lock lock( m_clips_mutex );

	const size_t keep = m_clip_session.size( ) - std::min( static_cast< size_t >( m_clip_still_ticks ), m_clip_session.size( ) );
	const size_t min_ticks = std::max< size_t >( 2, static_cast< size_t >( k_clip_min_seconds * static_cast< float >( server_tickrate( ) ) ) );
	const bool kept        = keep >= min_ticks;

	if ( kept ) {
		buffered_clip_t c{ };
		c.frames.assign( m_clip_session.begin( ), m_clip_session.begin( ) + static_cast< std::ptrdiff_t >( keep ) );
		c.rolled = m_clip_rolled;
		m_clip_history.push_front( std::move( c ) );
		if ( m_clip_history.size( ) > k_clip_history )
			m_clip_history.pop_back( );
		if ( m_clip_selected >= 0 && ++m_clip_selected >= static_cast< int >( m_clip_history.size( ) ) )
			m_clip_selected = -1;
		botox_dbg_log( "REC: clip ticks=%d rolled=%d\n", static_cast< int >( keep ), m_clip_rolled ? 1 : 0 );
	}

	m_clip_active      = false;
	m_clip_rolled      = false;
	m_clip_still_ticks = 0;
	m_clip_session.clear( );
	return kept;
}

/* key / button: still moving = cut the session here, then save the newest */
void n_movement_recorder::impl_t::save_clip( )
{
	/* a tap in progress must not quietly save the clip before it */
	if ( m_clip_active && !finish_clip( ) ) {
		movement_add_window( 0, std::string( "clip too short" ) );
		return;
	}
	if ( m_clip_history.empty( ) ) {
		movement_add_window( 0, std::string( "clip buffer empty" ) );
		return;
	}
	save_buffered( 0 );
}

void n_movement_recorder::impl_t::save_buffered( const size_t index )
{
	std::scoped_lock lock( m_clips_mutex );

	if ( index >= m_clip_history.size( ) )
		return;
	auto& c = m_clip_history[ index ];
	if ( !c.saved_as.empty( ) ) {
		movement_add_window( 0, "already saved as \"" + c.saved_as + "\"" );
		return;
	}
	if ( c.rolled )
		movement_add_window( 0, std::string( "clip hit max length, starts moving: replay may drift" ) );

	std::string name;
	if ( save_frames( c.frames, "Clip", &name ) )
		c.saved_as = name;
}

void n_movement_recorder::impl_t::clear_clip_history( )
{
	std::scoped_lock lock( m_clips_mutex );
	m_clip_history.clear( );
	m_clip_selected = -1;
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

	/* rename failed = keep the old name, the edit write goes there ( deleting it lost the clip if that write failed too ) */
	if ( new_path != old_path ) {
		std::error_code ec;
		std::filesystem::rename( old_path, new_path, ec );
		std::error_code exists_ec;
		if ( ec && std::filesystem::exists( old_path, exists_ec ) ) {
			movement_add_window( 0, "couldn't rename \"" + clip.filename + "\"" );
			return;
		}
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
	m_editor_open = false;
	if ( !write_clip_to_file( get_root_path( ) / m_clips[ m_editor_clip_index ].filename, m_clips[ m_editor_clip_index ] ) ) {
		movement_add_window( 0, "couldn't write \"" + m_clips[ m_editor_clip_index ].filename + "\"" );
		return;
	}
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
	m_play_max_drift    = 0.f;
	m_play_drift_tick   = -1;
	m_play_capture.clear( );
	m_play_capture.reserve( c.frames.size( ) );
	g_interfaces.m_engine_client->get_view_angles( m_smooth_from );
	apply_replay_jetpack( false );

	/* the approach stands still on frame 0: a moving start can't line up */
	if ( const frame_t& f0 = c.frames[ 0 ]; f0.has_velocity && f0.velocity.length( ) >= 1.f )
		movement_add_window( 0, "\"" + c.display_name + "\" starts moving: replay may drift" );

	/* no live frame ( old clip ) = the old replay: binds follow the keyboard, every cmd re-laid */
	const int live  = static_cast< int >( std::count_if( c.frames.begin( ), c.frames.end( ), [ ]( const frame_t& f ) { return f.live; } ) );
	m_play_has_live = live > 0;
	m_play_live     = 0;

	botox_dbg_log( "REC: play %s ticks=%d live=%d dist=%.2f yaw=%d in=%.2f out=%.2f\n", c.filename.c_str( ), static_cast< int >( c.frames.size( ) ), live,
	               g_prediction.backup_data.m_origin.dist_to( c.frames[ 0 ].position ), GET_VARIABLE( g_variables.m_movement_rec_yaw, int ),
	               GET_VARIABLE( g_variables.m_movement_rec_smooth_start, float ), GET_VARIABLE( g_variables.m_movement_rec_smooth_end, float ) );
	return true;
}

/* finished clips survive death / round reset, only the session in progress goes */
void n_movement_recorder::impl_t::reset_all( )
{
	std::scoped_lock lock( m_clips_mutex );
	force_stop( );
	m_recording = false;
	drop_clip_session( );
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
		clear_clip_history( );
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
	clear_clip_history( );
	refresh_clips( );

	movement_add_window( 0, std::string{ "joined to: " } + m_current_map );
}

void n_movement_recorder::impl_t::force_stop( bool keep_played )
{
	std::scoped_lock lock( m_clips_mutex );

	const bool was_playing = m_playing;
	const size_t played    = m_play_idx;

	if ( was_playing )
		botox_dbg_log( "REC: end idx=%d/%d drift max=%.3f first>1u=%d take=%d live=%d\n", static_cast< int >( played ),
		               static_cast< int >( m_play_frames.size( ) ), m_play_max_drift, m_play_drift_tick, keep_played ? 1 : 0, m_play_live );

	apply_replay_jetpack( false );
	m_playing        = false;
	m_wish_to_start  = false;
	m_play_idx       = 0;
	m_approach_ticks = 0;
	m_cmd_live       = false;

	/* take over: keep the ticks really sent ( m_play_capture ) and go on recording, clipper session too */
	if ( keep_played && was_playing && !m_play_capture.empty( ) ) {
		m_recording_frames = m_play_capture;
		m_recording        = true;
		if ( GET_VARIABLE( g_variables.m_movement_rec_clipper, bool ) ) {
			m_clip_session.assign( m_play_capture.begin( ), m_play_capture.end( ) );
			m_clip_active      = true;
			m_clip_rolled      = false;
			m_clip_still_ticks = 0;
		}
		movement_add_window( 0, "took over at tick " + std::to_string( m_play_capture.size( ) ) + ", recording" );
	}
	m_play_capture.clear( );
	sync_binds( );
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

	m_feat_keys[ 0 ] = m_feat_keys[ 1 ] = m_feat_keys[ 2 ] = m_feat_keys[ 3 ] = 0ull;
	m_feat_armed                                                               = false;
	m_cmd_mouse_x = m_cmd_mouse_y = 0.f;
	m_cmd_live = m_playing && m_play_idx < m_play_frames.size( ) && m_play_frames[ m_play_idx ].live && GET_VARIABLE( g_variables.m_movement_rec, bool );
	sync_binds( );
}

void n_movement_recorder::impl_t::begin_cmd( c_user_cmd* cmd )
{
	capture_user_input( cmd );
	if ( !cmd || !m_cmd_live )
		return;

	apply_playback( cmd );
	const frame_t& f = m_play_frames[ m_play_idx ];
	m_cmd_mouse_x    = f.strafe_mx;
	m_cmd_mouse_y    = f.strafe_my;
	/* last tick's view as the recording had it: the sent view may be the turned route camera */
	if ( m_play_idx > 0 ) {
		const frame_t& prev   = m_play_frames[ m_play_idx - 1 ];
		g_ctx.m_last_tick_yaw = prev.view_yaw;
		g_ctx.last_view_point = c_angle( prev.view_pitch, prev.view_yaw, 0.f );
	}
}

void n_movement_recorder::impl_t::strafe_mouse( float& x, float& y )
{
	if ( m_cmd_live ) {
		x = m_cmd_mouse_x;
		y = m_cmd_mouse_y;
		return;
	}
	m_cmd_mouse_x = x;
	m_cmd_mouse_y = y;
}

n_movement_recorder::feat_scope_t::feat_scope_t( )
{
	g_movement_recorder.m_feat_thread = GetCurrentThreadId( );
}

n_movement_recorder::feat_scope_t::~feat_scope_t( )
{
	g_movement_recorder.m_feat_thread = 0ul;
}

bool n_movement_recorder::impl_t::in_feat_scope( ) const
{
	return m_feat_thread.load( ) == GetCurrentThreadId( );
}

/* -1 = ask the keyboard. approach / raw frame = nothing was armed there; live frame = the recorded key, style 0 = on */
int n_movement_recorder::impl_t::bind_play( const int key, const int style ) const
{
	const int state = m_bind_state.load( );
	if ( state < 0 || !m_bind_force.load( ) )
		return -1;
	if ( state == 0 )
		return 0;
	if ( style != 1 && style != 2 )
		return 1;
	return key > 0 && key < 256 && ( ( m_bind_keys[ key >> 6 ].load( ) >> ( key & 63 ) ) & 1ull ) ? 1 : 0;
}

void n_movement_recorder::impl_t::bind_note( const int key, const int style )
{
	m_feat_armed = true;
	if ( ( style == 1 || style == 2 ) && key > 0 && key < 256 )
		m_feat_keys[ key >> 6 ] |= 1ull << ( key & 63 );
}

/* state of the frame this cmd sends; the tick after the last frame is the player's again */
void n_movement_recorder::impl_t::sync_binds( )
{
	const bool frame = m_playing && m_play_idx < m_play_frames.size( ) && GET_VARIABLE( g_variables.m_movement_rec, bool );
	if ( frame )
		for ( int i = 0; i < 4; ++i )
			m_bind_keys[ i ] = m_play_frames[ m_play_idx ].keys[ i ];
	m_bind_force = m_play_has_live;
	m_bind_state = frame ? ( m_play_frames[ m_play_idx ].live ? 1 : 0 ) : m_wish_to_start ? 0 : -1;
}

/* movement fix keeps the cmd's world wish dir under the sent view; same view = untouched, bit exact */
void n_movement_recorder::impl_t::playback_view( c_user_cmd* cmd, const c_angle& own ) const
{
	const c_angle view = camera_locked( ) ? camera_at( m_play_idx, now_real( ) ) : own;
	if ( view.m_x == cmd->m_view_point.m_x && view.m_y == cmd->m_view_point.m_y )
		return;
	start_movement_fix( cmd );
	cmd->m_view_point = view;
	end_movement_fix( cmd );
}

/* cmd-only: create_move calls it early too, so features + prediction see the replayed inputs.
   live frame: the raw input goes on once ( first call ), the features fly it, relay = no-op */
void n_movement_recorder::impl_t::apply_playback( c_user_cmd* cmd, const bool relay ) const
{
	if ( !cmd || !m_playing || m_play_idx >= m_play_frames.size( ) || !GET_VARIABLE( g_variables.m_movement_rec, bool ) )
		return;
	if ( relay && m_cmd_live )
		return;

	const c_angle backup_view = cmd->m_view_point;
	const frame_t& f          = m_play_frames[ m_play_idx ];
	f.replay( cmd );
	if ( m_cmd_live ) {
		cmd->m_view_point.m_x = f.raw_pitch;
		cmd->m_view_point.m_y = f.raw_yaw;
		cmd->m_forward_move   = f.raw_forward;
		cmd->m_side_move      = f.raw_side;
		cmd->m_up_move        = f.raw_up;
		cmd->m_buttons        = ( cmd->m_buttons & ~k_move_mask ) | ( f.raw_buttons & k_move_mask );
	}

	if ( GET_VARIABLE( g_variables.m_movement_rec_force_weapon, bool ) ) {
		cmd->m_weapon_select   = 0;
		cmd->m_weapon_sub_type = 0;
		select_weapon( cmd, f );
	}

	if ( !m_cmd_live )
		playback_view( cmd, backup_view );
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

	std::scoped_lock lock( m_clips_mutex );

	if ( !GET_VARIABLE( g_variables.m_movement_rec, bool ) ) {
		if ( m_playing || m_wish_to_start || m_recording || m_clip_active )
			reset_all( );
		if ( !m_clip_history.empty( ) )
			clear_clip_history( );
		return;
	}

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

	/* this cmd is the recorder's ( approach steer / a frame went on ): its stored frame is no feature's */
	const bool flown = m_wish_to_start || ( m_playing && m_play_idx < m_play_frames.size( ) );
	int stop_idx     = -1;

	if ( ( stop_play || user_moved ) && owns_cmd( ) ) {
		botox_dbg_log( "REC: stop play idx=%d/%d moved=%d\n", static_cast< int >( m_play_idx ), static_cast< int >( m_play_frames.size( ) ),
		               user_moved ? 1 : 0 );
		/* stop key, or your move on a live frame ( its features already own this cmd, their stamps would put it back ): the cmd going out still is that frame */
		const bool keep = !user_moved || m_cmd_live;
		if ( keep && m_playing && m_play_idx < m_play_frames.size( ) )
			stop_idx = static_cast< int >( m_play_idx );
		force_stop( true );
		if ( !keep )
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

	const bool clipper = GET_VARIABLE( g_variables.m_movement_rec_clipper, bool );
	if ( clip && clipper )
		save_clip( );

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
		const auto& pre         = g_prediction.backup_data;
		const c_vector& cur_pos = pre.m_origin;
		const c_vector delta    = first.position - cur_pos;
		const float dist        = cur_pos.dist_to( first.position );
		const float dist_2d     = delta.length_2d( );
		const bool grounded     = ( pre.m_flags & e_flags::fl_onground ) != 0;

		/* frame 0 fully ducked / standing: match it before go, mid duck can't be matched */
		bool duck    = ( first.buttons & in_duck ) != 0;
		bool duck_ok = true;
		if ( first.has_duck && first.duck_amount >= 0.99f ) {
			duck    = true;
			duck_ok = pre.m_duck_amount >= 0.99f;
		} else if ( first.has_duck && first.duck_amount <= 0.01f ) {
			duck    = false;
			duck_ok = pre.m_duck_amount <= 0.01f;
		}

		if ( camera_locked( ) ) {
			c_angle start_view = camera_at( 0, now_real( ) );
			cmd->m_view_point    = start_view;
			cmd->m_mouse_delta_x = 0;
			cmd->m_mouse_delta_y = 0;
			g_interfaces.m_engine_client->set_view_angles( start_view );
		}

		const float give_up_ticks = k_approach_give_up * static_cast< float >( server_tickrate( ) );
		const bool timed_out      = static_cast< float >( ++m_approach_ticks ) > give_up_ticks;
		/* standing on it: a start mid stride ( old p steer crossed the spot at up to 130 u/s ) replays from the wrong speed */
		const bool exact    = dist_2d <= k_pos_close_eps && std::fabs( delta.m_z ) <= 1.f && grounded && pre.m_velocity.length( ) < 1.f && duck_ok;
		const bool at_start = exact || ( timed_out && dist < 1.f );

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
			botox_dbg_log( "REC: start %s dist=%.3f v=%.2f duck=%.2f/%.2f approach=%d wpn=%d\n", exact ? "exact" : "INEXACT", dist,
			               pre.m_velocity.length( ), pre.m_duck_amount, first.duck_amount, m_approach_ticks, static_cast< int >( first.weapon_def ) );
		} else if ( timed_out && !at_start ) {
			botox_dbg_log( "REC: approach gave up dist=%.2f dz=%.2f v=%.2f duck_ok=%d\n", dist, delta.m_z, pre.m_velocity.length( ), duck_ok ? 1 : 0 );
			movement_add_window( 0, std::string( "couldn't reach route start" ) );
			force_stop( );
		} else {
			c_vector wish{ };
			/* air: no steer, land first */
			if ( grounded ) {
				const float da  = pre.m_duck_amount;
				const float mod = ( duck || da > 0.f ) ? 0.34f * da + 1.f - da : 1.f;
				wish            = approach_wish( delta, pre.m_velocity, g_ctx.m_local->get_surface_friction( ), mod );
			}

			const float yaw = deg2rad( cmd->m_view_point.m_y );
			const float c   = std::cos( yaw );
			const float s   = std::sin( yaw );

			cmd->m_forward_move = wish.m_x * c + wish.m_y * s;
			cmd->m_side_move    = wish.m_x * s - wish.m_y * c;
			cmd->m_up_move      = 0.f;
			cmd->m_buttons &= ~( in_jump | in_duck | in_forward | in_back | in_moveleft | in_moveright | in_speed );
			if ( duck )
				cmd->m_buttons |= in_duck;
		}
	}

	if ( m_playing ) {
		if ( m_play_idx >= m_play_frames.size( ) ) {
			botox_dbg_log( "REC: done ticks=%d\n", static_cast< int >( m_play_frames.size( ) ) );
			force_stop( );
		} else {
			apply_replay_jetpack( m_play_frames[ m_play_idx ].jetpack );
			/* live: the features flew the raw input off the real hull, their cmd goes out under the sent view */
			if ( m_cmd_live ) {
				playback_view( cmd, c_angle( m_user_input.view_pitch, m_user_input.view_yaw, cmd->m_view_point.m_z ) );
				++m_play_live;
			} else
				apply_playback( cmd );
			++m_play_idx;
			m_step_realtime = now_real( );
		}
	}

	cmd->m_view_point.normalize( );

	g_prediction.restore_entity_to_predicted_frame( g_interfaces.m_prediction->m_commands_predicted - 1 );
	frame_t f( cmd, g_ctx.m_local->get_origin( ) );
	f.jetpack = m_replay_jetpack_active;
	/* a replayed tick ( stop key tick too ) keeps the route's live half, your own tick gets this cmd's */
	if ( m_playing && m_play_idx > 0 )
		copy_live( f, m_play_frames[ m_play_idx - 1 ] );
	else if ( stop_idx >= 0 )
		copy_live( f, m_play_frames[ stop_idx ] );
	else if ( !flown && !owns_cmd( ) ) {
		stamp_live( f, m_user_input, m_feat_keys, m_feat_armed );
		f.strafe_mx = m_cmd_mouse_x;
		f.strafe_my = m_cmd_mouse_y;
	}

	/* frame m_play_idx - 1 went out this tick: its recorded start pos vs ours = desync */
	if ( m_playing && m_play_idx > 0 ) {
		m_play_capture.push_back( f );
		const size_t idx  = m_play_idx - 1;
		const float drift = f.position.dist_to( m_play_frames[ idx ].position );
		m_play_max_drift  = std::max( m_play_max_drift, drift );
		if ( drift > 1.f && m_play_drift_tick < 0 ) {
			m_play_drift_tick = static_cast< int >( idx );
			botox_dbg_log( "REC: desync tick=%d/%d drift=%.3f v=%.1f rec_v=%.1f\n", m_play_drift_tick, static_cast< int >( m_play_frames.size( ) ), drift,
			               g_prediction.backup_data.m_velocity.length( ), m_play_frames[ idx ].velocity.length( ) );
			movement_add_window( 0, "replay desynced at tick " + std::to_string( idx ) );
		}
	}

	/* approach / playback ticks are not yours; take over hands the played ticks to the session */
	if ( !clipper || owns_cmd( ) )
		drop_clip_session( );
	else {
		const bool input = move_input( cmd );
		clip_tick( f, input && at_rest( true ), !input && at_rest( false ) );
	}

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
	obj.m_draw_flags    = e_text_flags::text_flag_dropshadow;
	obj.m_font_size     = font->FontSize * scale;
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text, std::make_any< text_draw_object_t >( obj ) );
}

/* frames [ begin, end ) as one draw object; strip breaks where w2s fails */
static void enqueue_path( const std::vector< n_movement_recorder::frame_t >& frames, size_t begin, size_t end, unsigned int col, float th )
{
	end = std::min( end, frames.size( ) );
	if ( begin + 1 >= end )
		return;

	std::vector< std::vector< ImVec2 > > strips( 1 );
	for ( size_t i = begin; i < end; ++i ) {
		c_vector_2d s{ };
		if ( !g_render.world_to_screen( frames[ i ].position, s ) ) {
			if ( !strips.back( ).empty( ) )
				strips.emplace_back( );
			continue;
		}
		strips.back( ).emplace_back( s.m_x, s.m_y );
	}

	callback_draw_object_t obj{ };
	obj.m_callback = [ strips = std::move( strips ), col, th ]( ImDrawList* list ) {
		for ( const auto& s : strips )
			if ( s.size( ) >= 2 )
				list->AddPolyline( s.data( ), static_cast< int >( s.size( ) ), col, ImDrawFlags_None, th );
	};
	g_render.m_draw_data.emplace_back( e_draw_type::draw_type_callback, std::make_any< callback_draw_object_t >( std::move( obj ) ) );
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
	enqueue_path( frames, 0, frames.size( ), col, 0.5f );
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

		/* editor open: kept ticks bright, trimmed ends faint, start/end move with the sliders */
		if ( m_editor_open && m_editor_clip_index == static_cast< int >( i ) ) {
			const int last = static_cast< int >( route.frames.size( ) ) - 1;
			int ts         = std::clamp( m_editor_trim_start, 0, last );
			int te         = std::clamp( m_editor_trim_end, 0, last );
			if ( ts > te )
				std::swap( ts, te );

			const unsigned int cut = rgba( 255, 255, 255, 40 );
			enqueue_path( route.frames, 0, ts + 1, cut, 1.f );
			enqueue_path( route.frames, te, route.frames.size( ), cut, 1.f );
			enqueue_path( route.frames, ts, te + 1, accent_u32( 1.f ), 1.5f );
			enqueue_world_floor_disc( route.frames[ ts ].position, k_start_radius * 0.45f, rgba( 0, 0, 0, 100 ), accent_u32( 0.85f ), 64, 1.5f );

			c_vector_2d end_screen{ };
			if ( g_render.world_to_screen( route.frames[ te ].position, end_screen ) ) {
				enqueue_dot( end_screen, 4.f, rgba( 0, 0, 0, 100 ), accent_u32( 1.f ) );
				if ( font )
					enqueue_text( font, c_vector_2d{ end_screen.m_x + 7.f, end_screen.m_y - font->FontSize * 0.5f }, "end", rgba( 255, 255, 255, 255 ) );
			}
			continue;
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

		float alpha_mul = 1.f;
		if ( distance > 900.f )
			alpha_mul = std::clamp( ( 1000.f - distance ) / 100.f, 0.f, 1.f );
		if ( alpha_mul <= 0.f )
			continue;

		/* idle line = own checkbox, only near the start */
		if ( GET_VARIABLE( g_variables.m_movement_rec_preview_line, bool ) && distance < k_preview_line_dist ) {
			const float line_mul = std::clamp( ( k_preview_line_dist - distance ) / 50.f, 0.f, 1.f );
			enqueue_path( route.frames, 0, route.frames.size( ), accent_u32( 0.55f * line_mul ), 1.f );
		}

		c_vector_2d screen{ };
		if ( !g_render.world_to_screen( start_pos, screen ) )
			continue;

		m_point_progress[ i ] += ( 1.f - m_point_progress[ i ] ) * std::clamp( dt * 5.f, 0.f, 1.f );

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

namespace
{
	constexpr float k_panel_band = 25.f;

	/* same chrome as practice / point settings / route calc windows */
	void panel_chrome( const char* title, const float alpha = 1.f )
	{
		auto* bold           = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
		auto* window         = ImGui::GetCurrentWindow( );
		auto* draw_list      = window->DrawList;
		const ImVec2 pos     = window->Pos;
		const ImVec2 size    = window->Size;
		const float rounding = ImGui::GetStyle( ).WindowRounding;

		ImGui::PushClipRect( pos, ImVec2( pos.x + size.x, pos.y + k_panel_band ), false );
		draw_list->AddRectFilled( pos, ImVec2( pos.x + size.x, pos.y + k_panel_band ), ImColor( 25 / 255.f, 25 / 255.f, 25 / 255.f, alpha ), rounding,
		                          ImDrawFlags_RoundCornersTop );
		ImGui::PopClipRect( );

		RenderFadedGradientLine( draw_list, ImVec2( pos.x, pos.y + k_panel_band - 1.f ), ImVec2( size.x, 1.f ),
		                         static_cast< ImColor >( ImGui::GetColorU32( ImGuiCol_::ImGuiCol_Accent ) ) );

		if ( bold ) {
			const ImVec2 ts = bold->CalcTextSizeA( bold->FontSize, FLT_MAX, 0.f, title );
			draw_list->AddText( bold, bold->FontSize, ImVec2( pos.x + ( size.x - ts.x ) / 2.f, pos.y + ( k_panel_band - ts.y ) / 2.f ),
			                    ImColor( 1.f, 1.f, 1.f, alpha ), title );
		}

		ImGui::PushClipRect( ImVec2( pos.x + 1.f, pos.y + 1.f ), ImVec2( pos.x + size.x - 1.f, pos.y + size.y - 1.f ), false );
		draw_list->AddRect( ImVec2( pos.x + 1.f, pos.y + 1.f ), ImVec2( pos.x + size.x - 1.f, pos.y + size.y - 1.f ),
		                    ImColor( 50 / 255.f, 50 / 255.f, 50 / 255.f, alpha ), rounding );
		ImGui::PopClipRect( );
	}

	/* title band + one grey label / white value row ( route calc keys panel layout ) */
	constexpr float k_panel_pad = 8.f;

	ImVec2 row_panel_size( ImFont* font, const char* title, const char* label, const char* value )
	{
		ImFont* bold        = g_render.m_fonts[ e_font_names::font_name_verdana_bd_11 ];
		const float label_w = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, label ).x;
		const float value_w = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, value ).x;
		const float title_w = bold ? bold->CalcTextSizeA( bold->FontSize, FLT_MAX, 0.f, title ).x : 0.f;
		return ImVec2( std::max( label_w + 22.f + value_w + k_panel_pad * 2.f, title_w + 40.f ), k_panel_band + k_panel_pad + font->FontSize + 2.f );
	}

	void row_panel_body( ImFont* font, const char* title, const char* label, const char* value, const float alpha )
	{
		panel_chrome( title, alpha );

		auto* window        = ImGui::GetCurrentWindow( );
		const float value_w = font->CalcTextSizeA( font->FontSize, FLT_MAX, 0.f, value ).x;
		const float row_y   = window->Pos.y + k_panel_band + 2.f;
		window->DrawList->AddText( font, font->FontSize, ImVec2( window->Pos.x + k_panel_pad, row_y ), ImColor( 0.63f, 0.63f, 0.63f, alpha ), label );
		window->DrawList->AddText( font, font->FontSize, ImVec2( window->Pos.x + window->Size.x - k_panel_pad - value_w, row_y ),
		                           ImColor( 1.f, 1.f, 1.f, alpha ), value );
	}

	int panel_flags( )
	{
		int flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize;
		if ( !g_ctx.m_is_window_focused )
			flags |= ImGuiWindowFlags_NoMove;
		return flags;
	}
}

void n_movement_recorder::impl_t::render_clipper_box( )
{
	if ( !GET_VARIABLE( g_variables.m_movement_rec_clipper, bool ) || !GET_VARIABLE( g_variables.m_movement_rec_clipper_box, bool ) )
		return;

	const float dt = g_interfaces.m_global_vars_base->m_interval_per_tick;

	/* render thread: game thread grows these */
	const char* label = nullptr;
	char value[ 48 ]{ };
	{
		std::scoped_lock lock( m_clips_mutex );
		if ( m_playing ) {
			label = "playback";
			std::snprintf( value, sizeof( value ), "%.1f / %.1f s", static_cast< float >( m_play_idx ) * dt, static_cast< float >( m_play_frames.size( ) ) * dt );
		} else if ( m_recording ) {
			label = "recording";
			std::snprintf( value, sizeof( value ), "%.1f s", static_cast< float >( m_recording_frames.size( ) ) * dt );
		} else if ( m_clip_active ) {
			label = "clipping";
			std::snprintf( value, sizeof( value ), "%.1f s",
			               static_cast< float >( m_clip_session.size( ) - std::min( m_clip_session.size( ), static_cast< size_t >( m_clip_still_ticks ) ) ) * dt );
		} else if ( !m_clip_history.empty( ) ) {
			label = "last clip";
			std::snprintf( value, sizeof( value ), "%.1f s%s", static_cast< float >( m_clip_history.front( ).frames.size( ) ) * dt,
			               m_clip_history.front( ).saved_as.empty( ) ? "" : "  saved" );
		} else
			return;
	}

	ImFont* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	if ( !font )
		return;

	ImGui::SetNextWindowPos( ImVec2( 24.f, 96.f ), ImGuiCond_FirstUseEver );
	ImGui::SetNextWindowSize( row_panel_size( font, "recorder", label, value ), ImGuiCond_Always );
	ImGui::Begin( "botox-recorder-clipper", nullptr, panel_flags( ) );
	row_panel_body( font, "recorder", label, value, 1.f );
	ImGui::End( );
}

void n_movement_recorder::impl_t::render_indicator( )
{
	if ( !GET_VARIABLE( g_variables.m_movement_rec_render, bool ) )
		return;

	/* render thread: game thread grows these */
	const char* label = nullptr;
	char value[ 48 ]{ };
	{
		std::scoped_lock lock( m_clips_mutex );
		const float target = ( m_recording || owns_cmd( ) ) ? 1.f : 0.f;
		m_indicator_alpha  = std::clamp( m_indicator_alpha + ( target - m_indicator_alpha ) * ( 1.f - std::exp( -k_indicator_lerp_k * ImGui::GetIO( ).DeltaTime ) ),
		                                 0.f, 1.f );
		if ( m_indicator_alpha <= 0.01f )
			return;

		if ( m_recording ) {
			label = "recording";
			std::snprintf( value, sizeof( value ), "%zu ticks", m_recording_frames.size( ) );
		} else {
			label = m_wish_to_start ? "going to start" : "playback";
			std::snprintf( value, sizeof( value ), "%zu / %zu", m_play_idx, m_play_frames.size( ) );
		}
	}

	ImFont* font = g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	if ( !font )
		return;

	constexpr float edge = 7.f;
	const ImVec2 size    = row_panel_size( font, "recorder", label, value );
	const ImVec2 display = ImGui::GetIO( ).DisplaySize;

	ImVec2 pos{ edge, edge };
	switch ( GET_VARIABLE( g_variables.m_movement_rec_position, int ) ) {
	case 1: pos = { edge, display.y - size.y - edge }; break;
	case 2: pos = { display.x - size.x - edge, display.y - size.y - edge }; break;
	default: break;
	}

	ImGui::PushStyleVar( ImGuiStyleVar_Alpha, m_indicator_alpha );
	ImGui::SetNextWindowPos( pos, ImGuiCond_Always );
	ImGui::SetNextWindowSize( size, ImGuiCond_Always );
	ImGui::Begin( "botox-recorder-timer", nullptr, panel_flags( ) | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs );
	row_panel_body( font, "recorder", label, value, m_indicator_alpha );
	ImGui::End( );
	ImGui::PopStyleVar( );
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

	const ImVec2 display = ImGui::GetIO( ).DisplaySize;
	const float tick     = static_cast< float >( std::max( clip.tickrate, 1 ) );
	ImGui::SetNextWindowPos( ImVec2( display.x - 270.f, 20.f ), ImGuiCond_FirstUseEver );
	ImGui::SetNextWindowSizeConstraints( ImVec2( 250.f, 0.f ), ImVec2( 250.f, FLT_MAX ) );
	ImGui::Begin( "botox-recorder-editor", nullptr, panel_flags( ) | ImGuiWindowFlags_AlwaysAutoResize );
	{
		panel_chrome( "edit route" );
		ImGui::SetCursorPosY( 30.f );

		const int max_frame = static_cast< int >( clip.frames.size( ) ) - 1;
		m_editor_trim_start = std::clamp( m_editor_trim_start, 0, max_frame );
		m_editor_trim_end   = std::clamp( m_editor_trim_end, 0, max_frame );

		ImGui::TextDisabled( "%zu ticks   %.1f s   %d tick", clip.frames.size( ), static_cast< float >( clip.frames.size( ) ) / tick, clip.tickrate );
		ImGui::InputText( "name##mr_editor", m_editor_name, sizeof( m_editor_name ) );
		ImGui::SliderInt( "start tick##mr_editor", &m_editor_trim_start, 0, max_frame, "%d" );
		ImGui::SliderInt( "end tick##mr_editor", &m_editor_trim_end, 0, max_frame, "%d" );
		ImGui::TextDisabled( "kept %.1f s", static_cast< float >( std::abs( m_editor_trim_end - m_editor_trim_start ) + 1 ) / tick );

		if ( ImGui::Button( "apply", ImVec2( -1.f, 15.f ) ) )
			apply_clip_editor( );
		if ( ImGui::Button( "close", ImVec2( -1.f, 15.f ) ) )
			m_editor_open = false;
	}
	ImGui::End( );
}

void n_movement_recorder::impl_t::on_end_scene( )
{
	if ( !GET_VARIABLE( g_variables.m_movement_rec, bool ) )
		return;
	if ( !g_interfaces.m_engine_client || !g_interfaces.m_engine_client->is_in_game( ) )
		return;

	render_clip_editor( );
	render_clipper_box( );
	render_indicator( );
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

		/* menu: picked clip buffer entry shown in the world */
		if ( g_menu.m_opened && m_clip_selected >= 0 && m_clip_selected < static_cast< int >( m_clip_history.size( ) ) ) {
			const auto& frames = m_clip_history[ m_clip_selected ].frames;
			enqueue_path( frames, 0, frames.size( ), rgba( 255, 255, 255, 200 ), 1.5f );
			enqueue_world_floor_disc( frames[ 0 ].position, k_start_radius * 0.45f, rgba( 0, 0, 0, 100 ), rgba( 255, 255, 255, 200 ), 64, 1.5f );
		}
	}
}
