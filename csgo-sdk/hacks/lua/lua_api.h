#pragma once

#include <string>

#include "../../dependencies/lua/lauxlib.h"
#include "../../dependencies/lua/lua.h"
#include "../../dependencies/lua/lualib.h"

#include "lua.h"

struct matrix3x4_t;
struct studiohdr_t;

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
	/* eject: model:animate roots hold mdlcache refs */
	void free_models( );

	/* engine.swap_viewmodel: your viewmodel on a swap's from model ( swap made mid hold, fresh viewmodel, the client's own deploy )
	   -> its to model, an ended swap's to -> back to from. network_frame = postdataupdate_start ( before prediction copies the
	   network state, so it holds ), else render_start. forget_script = a script's swaps end with it. lua thread only */
	void apply_viewmodel_swaps( bool network_frame );
	void forget_script( const void* script );

	/* model:animate pose for one draw of `target` ( the drawn or swapped studiohdr_t ): the root plays its sequence at `to_world`,
	   target bones take the root bone of the same name ( bone merge ), the rest keep their rest offset from their parent.
	   one static buffer, valid until the next call ( studiorender copies it inside the draw ). null = draw as is */
	const matrix3x4_t* animate_bones( const n_lua::draw_model_t& draw, const studiohdr_t* target, const matrix3x4_t& to_world );

	/* entity:animate, from C_CSPlayer::BuildTransformations right after the engine's: rewrites the entity's bone cache with the
	   script's pose ( bones in bone_mask only ) before SetupBones builds attachments off it. no-op unless a script asked this frame */
	void pose_entity_bones( void* entity, const studiohdr_t* hdr, const matrix3x4_t& to_world, int bone_mask );

	inline n_lua::script_t* script_of( lua_State* L )
	{
		return *static_cast< n_lua::script_t** >( lua_getextraspace( L ) );
	}
}
