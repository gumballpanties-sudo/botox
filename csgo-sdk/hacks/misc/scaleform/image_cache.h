#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace n_image_cache
{
	struct impl_t {
		std::string rewrite( const char* js );

		std::unordered_map< std::string, std::string > m_state;
		std::deque< std::string > m_queue;
		std::mutex m_lock;

		std::atomic< bool > m_worker_alive{ false };
		std::atomic< bool > m_shutdown{ false };
	};

	bool download_file( const std::string& url, const std::string& destination );

	/* error: http status, or WinHTTP error code when no response came back */
	bool http_get_text( const std::string& url, std::string& out, unsigned long* error = nullptr );
	bool http_post_json( const std::string& url, const std::string& body, std::string& out, unsigned long* error = nullptr );
}

inline n_image_cache::impl_t g_image_cache{ };
