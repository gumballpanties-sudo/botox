#pragma once

#include <atomic>
#include <string>
#include <vector>

/* aventum ports: chat translator, image links as <img> */
namespace n_chat_extras
{
	inline std::atomic< bool > g_worker_alive{ false };

	inline constexpr const char* k_language_names = "english\0russian\0german\0french\0spanish\0portuguese\0polish\0turkish\0ukrainian\0"
	                                                "chinese\0japanese\0korean\0italian\0dutch\0swedish\0finnish\0czech\0hungarian\0"
	                                                "greek\0arabic\0hindi\0indonesian\0vietnamese\0romanian\0";

	/* CS_UM_SayText (5) / SayText2 (6) raw protobuf from DispatchUserMessage */
	void on_chat_message( int type, const void* msg, int size );

	/* game thread, every cmd: prints finished lines */
	void on_create_move( );

	/* push_notice CHAT_TOKEN_RAW: next queued html line */
	bool pop_line( std::string& html );

	std::vector< std::string > image_urls( const std::string& text );
	std::string image_html( const std::string& url );
} // namespace n_chat_extras
