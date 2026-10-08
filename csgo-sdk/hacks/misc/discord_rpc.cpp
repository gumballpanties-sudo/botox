#include "misc.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <format>
#include <mutex>
#include <string>
#include <thread>

void botox_dbg_log( const char* fmt, ... );

namespace
{
	constexpr const char* k_client_id = "1554733253632720926";
	constexpr const char* k_image     = "https://i.imgur.com/KDUzwZf.png";

	enum e_op : uint32_t { op_handshake = 0, op_frame = 1, op_close = 2, op_ping = 3, op_pong = 4 };

	HANDLE s_pipe = INVALID_HANDLE_VALUE;

	struct rpc_state_t {
		bool m_enabled = false;
		std::string m_map, m_image;

		bool operator==( const rpc_state_t& ) const = default;
	};

	std::mutex s_state_mutex;
	rpc_state_t s_state;

	rpc_state_t current_state( )
	{
		std::lock_guard< std::mutex > lock( s_state_mutex );
		return s_state;
	}

	void close_pipe( )
	{
		if ( s_pipe != INVALID_HANDLE_VALUE )
			CloseHandle( s_pipe );

		s_pipe = INVALID_HANDLE_VALUE;
	}

	bool send_frame( const uint32_t op, const std::string& json )
	{
		const uint32_t length = static_cast< uint32_t >( json.size( ) );

		std::string frame( 8, '\0' );
		memcpy( frame.data( ), &op, 4 );
		memcpy( frame.data( ) + 4, &length, 4 );
		frame += json;

		unsigned long written = 0;
		if ( WriteFile( s_pipe, frame.data( ), static_cast< unsigned long >( frame.size( ) ), &written, nullptr ) && written == frame.size( ) )
			return true;

		close_pipe( );
		return false;
	}

	void open_pipe( )
	{
		for ( int i = 0; i < 10; i++ ) {
			const std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string( i );

			s_pipe = CreateFileA( name.c_str( ), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr );
			if ( s_pipe == INVALID_HANDLE_VALUE )
				continue;

			if ( send_frame( op_handshake, std::format( R"({{"v":1,"client_id":"{}"}})", k_client_id ) ) )
				botox_dbg_log( "RPC: pipe %d open", i );

			return;
		}
	}

	/* never blocks: reads only frames already whole in the pipe. true = discord answered (READY / echo) */
	bool drain( )
	{
		bool answered = false;

		while ( s_pipe != INVALID_HANDLE_VALUE ) {
			uint32_t header[ 2 ]    = { };
			unsigned long peeked    = 0;
			unsigned long available = 0;

			if ( !PeekNamedPipe( s_pipe, header, 8, &peeked, &available, nullptr ) ) {
				close_pipe( );
				break;
			}

			if ( peeked < 8 || available < 8 + header[ 1 ] )
				break;

			if ( header[ 1 ] > 64 * 1024 ) {
				close_pipe( );
				break;
			}

			std::string body( header[ 1 ], '\0' );
			unsigned long read = 0;

			if ( !ReadFile( s_pipe, header, 8, &read, nullptr ) || ( !body.empty( ) && !ReadFile( s_pipe, body.data( ), header[ 1 ], &read, nullptr ) ) ) {
				close_pipe( );
				break;
			}

			switch ( header[ 0 ] ) {
			case op_frame:
				answered = true;
				break;
			case op_ping:
				send_frame( op_pong, body );
				break;
			case op_close:
				botox_dbg_log( "RPC: closed %s", body.c_str( ) );
				close_pipe( );
				break;
			}
		}

		return answered && s_pipe != INVALID_HANDLE_VALUE;
	}

	std::string json_escape( const char* text )
	{
		std::string out;
		for ( ; *text; text++ ) {
			if ( *text == '"' || *text == '\\' )
				out += '\\';
			if ( static_cast< unsigned char >( *text ) >= 0x20 )
				out += *text;
		}
		return out;
	}

	unsigned long __stdcall rpc_thread( void* )
	{
		const long long start = static_cast< long long >( std::time( nullptr ) );

		std::string sent = "\x01";
		bool ready       = false;
		int nonce        = 0;
		int retry        = 0;

		while ( !g_ctx.m_unloading ) {
			const rpc_state_t state = current_state( );

			if ( !state.m_enabled ) {
				/* discord drops the presence when the pipe closes */
				if ( s_pipe != INVALID_HANDLE_VALUE )
					botox_dbg_log( "RPC: off" );

				close_pipe( );
				ready = false;
				sent  = "\x01";
				retry = 0;
			} else if ( s_pipe == INVALID_HANDLE_VALUE ) {
				ready = false;
				sent  = "\x01";

				if ( --retry <= 0 ) {
					retry = 15;
					open_pipe( );
				}
			} else {
				ready |= drain( );

				if ( s_pipe == INVALID_HANDLE_VALUE )
					ready = false;

				if ( ready ) {
					const std::string where = state.m_map.empty( ) ? "in menu" : "playing on " + state.m_map;

					const std::string activity =
						std::format( R"({{"details":"{}","timestamps":{{"start":{}}},"assets":{{"large_image":"{}","large_text":"botox"}}}})",
					                 where, start, state.m_image.empty( ) ? k_image : state.m_image );

					if ( activity != sent ) {
						const std::string json = std::format( R"({{"cmd":"SET_ACTIVITY","args":{{"pid":{},"activity":{}}},"nonce":"{}"}})",
						                                      GetCurrentProcessId( ), activity, ++nonce );

						if ( send_frame( op_frame, json ) ) {
							sent = activity;
							botox_dbg_log( "RPC: %s", activity.c_str( ) );
						}
					}
				}
			}

			for ( int i = 0; i < 4 && !g_ctx.m_unloading; i++ )
				std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
		}

		close_pipe( );
		g_discord_rpc_alive = false;

		return 0;
	}
}

void discord_rpc_frame( )
{
	const char* level = g_interfaces.m_engine_client->is_in_game( ) ? g_interfaces.m_engine_client->get_level_name_short( ) : nullptr;

	rpc_state_t next;
	next.m_enabled = GET_VARIABLE( g_variables.m_discord_rpc, bool );
	next.m_map     = json_escape( level ? level : "" );
	next.m_image   = json_escape( GET_VARIABLE( g_variables.m_discord_rpc_image, std::string ).c_str( ) );

	static rpc_state_t s_last;
	if ( s_last == next )
		return;

	s_last = next;

	std::lock_guard< std::mutex > lock( s_state_mutex );
	s_state = std::move( next );
}

void discord_rpc_start( )
{
	g_discord_rpc_alive = true; // before the thread runs: an eject right now must still wait for it
	g_utilities.create_thread( rpc_thread, nullptr );
}
