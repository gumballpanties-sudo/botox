#pragma once

#include <string>

#include "../../dependencies/lua/lauxlib.h"
#include "../../dependencies/lua/lua.h"
#include "../../dependencies/lua/lualib.h"

#include "lua.h"

/* lua is built as c++ (lua_unity.cpp): include lua.h directly, never lua.hpp (extern "C" = link errors) */
namespace n_lua_api
{
	/* set only while the matching callback runs, null after: a kept cmd / event errors instead of dangling */
	inline c_user_cmd* s_cmd        = nullptr;
	inline bool* s_send_packet      = nullptr;
	inline game_event_t* s_event    = nullptr;
	inline bool s_in_paint          = false;

	void open( lua_State* L );
	int traceback( lua_State* L );
	/* game console (tier0 Msg) */
	void console_msg( const std::string& text );

	void push_cmd( lua_State* L );
	void push_event( lua_State* L );

	inline n_lua::script_t* script_of( lua_State* L )
	{
		return *static_cast< n_lua::script_t** >( lua_getextraspace( L ) );
	}
}
