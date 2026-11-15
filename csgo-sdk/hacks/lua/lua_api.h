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
	inline n_lua::draw_model_t* s_draw = nullptr;
	/* bumped per draw: a model kept from an earlier draw must not write into this one */
	inline unsigned int s_draw_serial = 0;

	void open( lua_State* L );
	int traceback( lua_State* L );
	/* game console (tier0 Msg) */
	void console_msg( const std::string& text );

	void push_cmd( lua_State* L );
	void push_event( lua_State* L );
	void push_draw( lua_State* L );

	/* draw_model swaps: paths a script asked for load at frame_stage start ( never inside a draw ),
	   reset on level change ( the engine purges client models ). lua thread only */
	void load_models( );
	void reset_models( );

	inline n_lua::script_t* script_of( lua_State* L )
	{
		return *static_cast< n_lua::script_t** >( lua_getextraspace( L ) );
	}
}
