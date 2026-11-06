#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/logger/logger.h"
#include "../chat_hud.h"

#include "lua_api.h"

using n_lua::element_t;
using n_lua::script_t;
using word_t = std::uintptr_t;

namespace
{
	constexpr const char* k_element_meta = "botox.element";
	constexpr const char* k_entity_meta  = "botox.entity";
	constexpr const char* k_cmd_meta     = "botox.cmd";
	constexpr const char* k_event_meta   = "botox.event";

	struct element_ref_t {
		int m_index;
	};

	struct entity_ref_t {
		int m_index;
	};

	/* ---- seh: plain functions, no c++ objects, so __try is legal under /EHsc ---- */

	bool seh_copy( void* destination, const void* source, const std::size_t size )
	{
		__try {
			std::memcpy( destination, source, size );
			return true;
		} __except ( EXCEPTION_EXECUTE_HANDLER ) {
			return false;
		}
	}

	int seh_strlen( const char* text, const int max )
	{
		__try {
			int length = 0;
			while ( length < max && text[ length ] )
				length++;
			return length;
		} __except ( EXCEPTION_EXECUTE_HANDLER ) {
			return -1;
		}
	}

	bool seh_write( void* destination, const void* source, const std::size_t size )
	{
		if ( seh_copy( destination, source, size ) )
			return true;

		DWORD old_protect = 0;
		if ( !VirtualProtect( destination, size, PAGE_EXECUTE_READWRITE, &old_protect ) )
			return false;

		const bool ok = seh_copy( destination, source, size );
		VirtualProtect( destination, size, old_protect, &old_protect );
		FlushInstructionCache( GetCurrentProcess( ), destination, size );
		return ok;
	}

	template< std::size_t >
	using word_n = word_t;

	template< std::size_t... I >
	word_t call_cdecl( word_t fn, const word_t* a, std::index_sequence< I... > )
	{
		return reinterpret_cast< word_t( __cdecl* )( word_n< I >... ) >( fn )( a[ I ]... );
	}

	template< std::size_t... I >
	word_t call_stdcall( word_t fn, const word_t* a, std::index_sequence< I... > )
	{
		return reinterpret_cast< word_t( __stdcall* )( word_n< I >... ) >( fn )( a[ I ]... );
	}

	template< std::size_t... I >
	word_t call_thiscall( word_t fn, const word_t* a, std::index_sequence< I... > )
	{
		return reinterpret_cast< word_t( __thiscall* )( word_t, word_n< I >... ) >( fn )( a[ 0 ], a[ I + 1 ]... );
	}

	template< std::size_t... I >
	word_t call_fastcall( word_t fn, const word_t* a, std::index_sequence< I... > )
	{
		return reinterpret_cast< word_t( __fastcall* )( word_t, word_t, word_n< I >... ) >( fn )( a[ 0 ], a[ 1 ], a[ I + 2 ]... );
	}

	using invoker_t = word_t ( * )( word_t, const word_t* );

	template< int convention, std::size_t count >
	word_t invoke( word_t fn, const word_t* a )
	{
		if constexpr ( convention == 0 )
			return call_cdecl( fn, a, std::make_index_sequence< count >{ } );
		else if constexpr ( convention == 1 )
			return call_stdcall( fn, a, std::make_index_sequence< count >{ } );
		else if constexpr ( convention == 2 )
			return call_thiscall( fn, a, std::make_index_sequence< count >{ } );
		else
			return call_fastcall( fn, a, std::make_index_sequence< count >{ } );
	}

	constexpr std::size_t k_max_call_args = 12;

	template< int convention, std::size_t... N >
	constexpr std::array< invoker_t, sizeof...( N ) > invoker_row( std::index_sequence< N... > )
	{
		return { &invoke< convention, N >... };
	}

	/* [ convention ][ args after this / ecx+edx ] */
	const std::array< std::array< invoker_t, k_max_call_args + 1 >, 4 > k_invokers = {
		invoker_row< 0 >( std::make_index_sequence< k_max_call_args + 1 >{ } ),
		invoker_row< 1 >( std::make_index_sequence< k_max_call_args + 1 >{ } ),
		invoker_row< 2 >( std::make_index_sequence< k_max_call_args + 1 >{ } ),
		invoker_row< 3 >( std::make_index_sequence< k_max_call_args + 1 >{ } ),
	};

	bool seh_invoke( invoker_t invoker, word_t fn, const word_t* args, word_t* out )
	{
		__try {
			*out = invoker( fn, args );
			return true;
		} __except ( EXCEPTION_EXECUTE_HANDLER ) {
			return false;
		}
	}

	/* ---- helpers ---- */

	word_t check_address( lua_State* L, const int index )
	{
		return static_cast< word_t >( luaL_checkinteger( L, index ) );
	}

	void push_address( lua_State* L, const void* pointer )
	{
		if ( pointer )
			lua_pushinteger( L, static_cast< lua_Integer >( reinterpret_cast< word_t >( pointer ) ) );
		else
			lua_pushnil( L );
	}

	float check_float( lua_State* L, const int index )
	{
		return static_cast< float >( luaL_checknumber( L, index ) );
	}

	float opt_float( lua_State* L, const int index, const float fallback )
	{
		return static_cast< float >( luaL_optnumber( L, index, fallback ) );
	}

	unsigned int opt_color( lua_State* L, const int index )
	{
		return static_cast< unsigned int >( luaL_optinteger( L, index, 0xFFFFFFFF ) );
	}

	void push_vector( lua_State* L, const c_vector& vector )
	{
		lua_pushnumber( L, vector.m_x );
		lua_pushnumber( L, vector.m_y );
		lua_pushnumber( L, vector.m_z );
	}

	void set_field( lua_State* L, const char* key, const lua_Number value )
	{
		lua_pushnumber( L, value );
		lua_setfield( L, -2, key );
	}

	void set_field( lua_State* L, const char* key, const lua_Integer value )
	{
		lua_pushinteger( L, value );
		lua_setfield( L, -2, key );
	}

	void set_field( lua_State* L, const char* key, const char* value )
	{
		lua_pushstring( L, value );
		lua_setfield( L, -2, key );
	}

	void set_bool( lua_State* L, const char* key, const bool value )
	{
		lua_pushboolean( L, value );
		lua_setfield( L, -2, key );
	}

	void add_table( lua_State* L, const char* name, const luaL_Reg* functions )
	{
		lua_newtable( L );
		luaL_setfuncs( L, functions, 0 );
		lua_setglobal( L, name );
	}

	c_base_entity* local_player( )
	{
		if ( !g_interfaces.m_engine_client->is_in_game( ) )
			return nullptr;

		return g_interfaces.m_client_entity_list->get< c_base_entity >( g_interfaces.m_engine_client->get_local_player( ) );
	}

	/* ---- callbacks ---- */

	int l_callbacks_register( lua_State* L )
	{
		const char* name = luaL_checkstring( L, 1 );
		luaL_checktype( L, 2, LUA_TFUNCTION );

		lua_getfield( L, LUA_REGISTRYINDEX, "botox.callbacks" );
		if ( lua_getfield( L, -1, name ) != LUA_TTABLE ) {
			lua_pop( L, 1 );
			lua_newtable( L );
			lua_pushvalue( L, -1 );
			lua_setfield( L, -3, name );
		}

		const lua_Integer size = static_cast< lua_Integer >( lua_rawlen( L, -1 ) );
		lua_pushvalue( L, 2 );
		lua_rawseti( L, -2, size + 1 );
		return 0;
	}

	int l_callbacks_unregister( lua_State* L )
	{
		const char* name = luaL_checkstring( L, 1 );
		luaL_checktype( L, 2, LUA_TFUNCTION );

		lua_getfield( L, LUA_REGISTRYINDEX, "botox.callbacks" );
		if ( lua_getfield( L, -1, name ) != LUA_TTABLE )
			return 0;

		const int list         = lua_gettop( L );
		const lua_Integer size = static_cast< lua_Integer >( lua_rawlen( L, list ) );

		for ( lua_Integer i = 1; i <= size; i++ ) {
			lua_rawgeti( L, list, i );
			const bool same = lua_rawequal( L, -1, 2 );
			lua_pop( L, 1 );
			if ( !same )
				continue;

			for ( lua_Integer j = i; j < size; j++ ) {
				lua_rawgeti( L, list, j + 1 );
				lua_rawseti( L, list, j );
			}
			lua_pushnil( L );
			lua_rawseti( L, list, size );
			break;
		}

		return 0;
	}

	/* ---- ui ---- */

	int create_element( lua_State* L, element_t&& element )
	{
		script_t* script = n_lua_api::script_of( L );
		int index        = 0;

		{
			std::lock_guard< std::mutex > lock( g_lua.m_mutex );
			g_lua.apply_saved( *script, element );
			script->m_elements.push_back( std::move( element ) );
			index = static_cast< int >( script->m_elements.size( ) ) - 1;
		}

		auto* ref    = static_cast< element_ref_t* >( lua_newuserdatauv( L, sizeof( element_ref_t ), 0 ) );
		ref->m_index = index;
		luaL_setmetatable( L, k_element_meta );
		return 1;
	}

	element_t make_element( lua_State* L, const n_lua::e_element_type type )
	{
		element_t element{ };
		element.m_type  = type;
		element.m_label = luaL_checkstring( L, 1 );
		return element;
	}

	int l_ui_checkbox( lua_State* L )
	{
		element_t element = make_element( L, n_lua::element_checkbox );
		element.m_bool    = lua_toboolean( L, 2 );
		return create_element( L, std::move( element ) );
	}

	int l_ui_slider_int( lua_State* L )
	{
		element_t element  = make_element( L, n_lua::element_slider_int );
		element.m_int_min  = static_cast< int >( luaL_checkinteger( L, 2 ) );
		element.m_int_max  = static_cast< int >( luaL_checkinteger( L, 3 ) );
		element.m_int      = std::clamp( static_cast< int >( luaL_optinteger( L, 4, element.m_int_min ) ), element.m_int_min, element.m_int_max );
		element.m_format   = luaL_optstring( L, 5, "%d" );
		return create_element( L, std::move( element ) );
	}

	int l_ui_slider_float( lua_State* L )
	{
		element_t element   = make_element( L, n_lua::element_slider_float );
		element.m_float_min = check_float( L, 2 );
		element.m_float_max = check_float( L, 3 );
		element.m_float     = std::clamp( opt_float( L, 4, element.m_float_min ), element.m_float_min, element.m_float_max );
		element.m_format    = luaL_optstring( L, 5, "%.2f" );
		return create_element( L, std::move( element ) );
	}

	int l_ui_combo( lua_State* L )
	{
		element_t element = make_element( L, n_lua::element_combo );
		luaL_checktype( L, 2, LUA_TTABLE );

		const lua_Integer size = static_cast< lua_Integer >( lua_rawlen( L, 2 ) );
		for ( lua_Integer i = 1; i <= size; i++ ) {
			lua_rawgeti( L, 2, i );
			element.m_items.emplace_back( luaL_checkstring( L, -1 ) );
			lua_pop( L, 1 );
		}

		if ( element.m_items.empty( ) )
			return luaL_error( L, "ui.combo: items table is empty" );

		element.m_int = std::clamp( static_cast< int >( luaL_optinteger( L, 3, 1 ) ) - 1, 0, static_cast< int >( element.m_items.size( ) ) - 1 );
		return create_element( L, std::move( element ) );
	}

	int l_ui_color( lua_State* L )
	{
		element_t element = make_element( L, n_lua::element_color );
		for ( int i = 0; i < 4; i++ )
			element.m_color[ i ] = std::clamp( static_cast< float >( luaL_optinteger( L, 2 + i, 255 ) ) / 255.f, 0.f, 1.f );
		return create_element( L, std::move( element ) );
	}

	int l_ui_keybind( lua_State* L )
	{
		element_t element = make_element( L, n_lua::element_keybind );
		element.m_bind    = key_bind_t{ std::clamp( static_cast< int >( luaL_optinteger( L, 2, 0 ) ), 0, 255 ),
                                     std::clamp( static_cast< int >( luaL_optinteger( L, 3, 1 ) ), 0, 2 ) };
		return create_element( L, std::move( element ) );
	}

	int l_ui_button( lua_State* L )
	{
		element_t element = make_element( L, n_lua::element_button );
		if ( !lua_isnoneornil( L, 2 ) ) {
			luaL_checktype( L, 2, LUA_TFUNCTION );
			lua_pushvalue( L, 2 );
			element.m_callback = luaL_ref( L, LUA_REGISTRYINDEX );
		}
		return create_element( L, std::move( element ) );
	}

	int l_ui_text( lua_State* L )
	{
		return create_element( L, make_element( L, n_lua::element_text ) );
	}

	int l_ui_input( lua_State* L )
	{
		element_t element = make_element( L, n_lua::element_input );
		strncpy_s( element.m_text, luaL_optstring( L, 2, "" ), _TRUNCATE );
		return create_element( L, std::move( element ) );
	}

	int element_index( lua_State* L )
	{
		return static_cast< const element_ref_t* >( luaL_checkudata( L, 1, k_element_meta ) )->m_index;
	}

	/* never call lua under g_lua.m_mutex: any push / error can run a __gc finalizer that re-enters ui ( std::mutex is not recursive ) */
	bool copy_element( lua_State* L, const int index, element_t& out, bool* active = nullptr )
	{
		std::lock_guard< std::mutex > lock( g_lua.m_mutex );
		auto& elements = n_lua_api::script_of( L )->m_elements;
		if ( index < 0 || index >= static_cast< int >( elements.size( ) ) )
			return false;

		element_t& element = elements[ index ];
		/* toggle state is keyed on the live element address, never the copy */
		if ( active && element.m_type == n_lua::element_keybind )
			*active = ( element.m_bind.m_key_style == 0 || element.m_bind.m_key ) && g_input.check_input( &element.m_bind );

		out = element;
		return true;
	}

	int l_element_get( lua_State* L )
	{
		element_t element{ };
		bool active = false;
		if ( !copy_element( L, element_index( L ), element, &active ) )
			return luaL_error( L, "ui element is gone" );

		switch ( element.m_type ) {
		case n_lua::element_checkbox: lua_pushboolean( L, element.m_bool ); return 1;
		case n_lua::element_slider_int: lua_pushinteger( L, element.m_int ); return 1;
		case n_lua::element_slider_float: lua_pushnumber( L, element.m_float ); return 1;
		case n_lua::element_combo:
			lua_pushinteger( L, element.m_int + 1 );
			lua_pushstring( L, element.m_items[ element.m_int ].c_str( ) );
			return 2;
		case n_lua::element_color:
			for ( const float channel : element.m_color )
				lua_pushinteger( L, static_cast< lua_Integer >( channel * 255.f + 0.5f ) );
			return 4;
		case n_lua::element_keybind:
			lua_pushboolean( L, active );
			lua_pushinteger( L, element.m_bind.m_key );
			lua_pushinteger( L, element.m_bind.m_key_style );
			return 3;
		case n_lua::element_input: lua_pushstring( L, element.m_text ); return 1;
		case n_lua::element_text: lua_pushstring( L, element.m_label.c_str( ) ); return 1;
		default: lua_pushnil( L ); return 1;
		}
	}

	int l_element_set( lua_State* L )
	{
		const int index = element_index( L );
		element_t element{ };
		if ( !copy_element( L, index, element ) )
			return luaL_error( L, "ui element is gone" );

		switch ( element.m_type ) {
		case n_lua::element_checkbox: element.m_bool = lua_toboolean( L, 2 ); break;
		case n_lua::element_slider_int:
			element.m_int = std::clamp( static_cast< int >( luaL_checkinteger( L, 2 ) ), element.m_int_min, element.m_int_max );
			break;
		case n_lua::element_slider_float: element.m_float = std::clamp( check_float( L, 2 ), element.m_float_min, element.m_float_max ); break;
		case n_lua::element_combo:
			element.m_int = std::clamp( static_cast< int >( luaL_checkinteger( L, 2 ) ) - 1, 0, static_cast< int >( element.m_items.size( ) ) - 1 );
			break;
		case n_lua::element_color:
			for ( int i = 0; i < 4; i++ )
				element.m_color[ i ] = std::clamp( static_cast< float >( luaL_optinteger( L, 2 + i, 255 ) ) / 255.f, 0.f, 1.f );
			break;
		case n_lua::element_keybind:
			element.m_bind.m_key       = std::clamp( static_cast< int >( luaL_checkinteger( L, 2 ) ), 0, 255 );
			element.m_bind.m_key_style = std::clamp( static_cast< int >( luaL_optinteger( L, 3, element.m_bind.m_key_style ) ), 0, 2 );
			break;
		case n_lua::element_input: strncpy_s( element.m_text, luaL_checkstring( L, 2 ), _TRUNCATE ); break;
		case n_lua::element_text: element.m_label = luaL_checkstring( L, 2 ); break;
		default: break;
		}

		script_t* script = n_lua_api::script_of( L );
		std::lock_guard< std::mutex > lock( g_lua.m_mutex );
		if ( index >= static_cast< int >( script->m_elements.size( ) ) )
			return 0;

		element_t& live = script->m_elements[ index ];
		live.m_bool     = element.m_bool;
		live.m_int      = element.m_int;
		live.m_float    = element.m_float;
		live.m_bind     = element.m_bind;
		std::memcpy( live.m_color, element.m_color, sizeof( live.m_color ) );
		std::memcpy( live.m_text, element.m_text, sizeof( live.m_text ) );
		if ( live.m_type == n_lua::element_text )
			live.m_label = std::move( element.m_label );

		script->m_dirty = true;
		return 0;
	}

	int l_element_visible( lua_State* L )
	{
		const int index   = element_index( L );
		const bool change = lua_gettop( L ) >= 2;
		const bool value  = change && lua_toboolean( L, 2 );

		bool found = false, shown = false;
		{
			std::lock_guard< std::mutex > lock( g_lua.m_mutex );
			auto& elements = n_lua_api::script_of( L )->m_elements;
			if ( index >= 0 && index < static_cast< int >( elements.size( ) ) ) {
				if ( change )
					elements[ index ].m_visible = value;
				shown = elements[ index ].m_visible;
				found = true;
			}
		}

		if ( !found )
			return luaL_error( L, "ui element is gone" );

		lua_pushboolean( L, shown );
		return 1;
	}

	int l_element_label( lua_State* L )
	{
		element_t element{ };
		if ( !copy_element( L, element_index( L ), element ) )
			return luaL_error( L, "ui element is gone" );

		lua_pushstring( L, element.m_label.c_str( ) );
		return 1;
	}

	/* ---- render: paint callback only (game thread queue, swapped to the render thread) ---- */

	void require_paint( lua_State* L )
	{
		if ( !n_lua_api::s_in_paint )
			luaL_error( L, "render functions only work inside the paint callback" );
	}

	ImFont* font_of( lua_State* L, const int index )
	{
		const lua_Integer id = luaL_optinteger( L, index, 0 );
		ImFont* font         = nullptr;

		if ( id >= 0 && id < e_font_names::font_name_max )
			font = g_render.m_fonts[ id ];
		else if ( id >= 100 && id < 100 + e_custom_font_names::custom_font_name_max )
			font = g_render.m_custom_fonts[ id - 100 ];

		return font ? font : g_render.m_fonts[ e_font_names::font_name_verdana_11 ];
	}

	int l_color( lua_State* L )
	{
		const auto channel = [ L ]( const int index, const lua_Integer fallback ) {
			return static_cast< unsigned int >( std::clamp< lua_Integer >( luaL_optinteger( L, index, fallback ), 0, 255 ) );
		};

		lua_pushinteger( L, static_cast< lua_Integer >( IM_COL32( channel( 1, 255 ), channel( 2, 255 ), channel( 3, 255 ), channel( 4, 255 ) ) ) );
		return 1;
	}

	int l_render_screen_size( lua_State* L )
	{
		lua_pushnumber( L, g_ctx.m_width );
		lua_pushnumber( L, g_ctx.m_height );
		return 2;
	}

	int l_render_text( lua_State* L )
	{
		require_paint( L );

		ImFont* font = font_of( L, 5 );
		if ( !font )
			return 0;

		const unsigned int color = opt_color( L, 4 );
		const auto flags         = static_cast< e_text_flags >( luaL_optinteger( L, 6, e_text_flags::text_flag_dropshadow ) );
		const unsigned int outline = IM_COL32( 0, 0, 0, ( color >> IM_COL32_A_SHIFT ) & 0xFF );

		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_text,
		                                   std::make_any< text_draw_object_t >( text_draw_object_t{ font, c_vector_2d( check_float( L, 1 ), check_float( L, 2 ) ),
		                                                                                            luaL_checkstring( L, 3 ), color, outline, flags,
		                                                                                            opt_float( L, 7, 0.f ) } ) );
		return 0;
	}

	int l_render_text_size( lua_State* L )
	{
		require_paint( L );

		ImFont* font     = font_of( L, 2 );
		const char* text = luaL_checkstring( L, 1 );
		if ( !font ) {
			lua_pushnumber( L, 0 );
			lua_pushnumber( L, 0 );
			return 2;
		}

		const float size  = opt_float( L, 3, 0.f );
		const ImVec2 dims = font->CalcTextSizeA( size > 0.f ? size : font->FontSize, FLT_MAX, 0.f, text );
		lua_pushnumber( L, dims.x );
		lua_pushnumber( L, dims.y );
		return 2;
	}

	int l_render_line( lua_State* L )
	{
		require_paint( L );
		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_line,
			std::make_any< line_draw_object_t >( line_draw_object_t{ c_vector_2d( check_float( L, 1 ), check_float( L, 2 ) ),
		                                                             c_vector_2d( check_float( L, 3 ), check_float( L, 4 ) ), opt_color( L, 5 ), opt_float( L, 6, 1.f ) } ) );
		return 0;
	}

	int push_rect( lua_State* L, const bool filled )
	{
		require_paint( L );

		rect_draw_object_t rect{ };
		rect.m_min      = c_vector_2d( check_float( L, 1 ), check_float( L, 2 ) );
		rect.m_max      = c_vector_2d( check_float( L, 3 ), check_float( L, 4 ) );
		rect.m_color    = opt_color( L, 5 );
		rect.m_filled   = filled;
		rect.m_rounding = opt_float( L, filled ? 6 : 7, 0.f );
		if ( !filled )
			rect.m_thickness = opt_float( L, 6, 1.f );

		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_rect, std::make_any< rect_draw_object_t >( rect ) );
		return 0;
	}

	int l_render_rect( lua_State* L )
	{
		return push_rect( L, false );
	}

	int l_render_rect_filled( lua_State* L )
	{
		return push_rect( L, true );
	}

	int l_render_gradient( lua_State* L )
	{
		require_paint( L );
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_gradient_rect,
		                                   std::make_any< gradient_rect_draw_object_t >(
											   c_vector_2d( check_float( L, 1 ), check_float( L, 2 ) ), c_vector_2d( check_float( L, 3 ), check_float( L, 4 ) ),
											   opt_color( L, 5 ), opt_color( L, 6 ), opt_float( L, 7, 0.f ) ) );
		return 0;
	}

	int l_render_circle( lua_State* L )
	{
		require_paint( L );
		g_render.m_draw_data.emplace_back(
			e_draw_type::draw_type_circle,
			std::make_any< circle_draw_object_t >( c_vector_2d( check_float( L, 1 ), check_float( L, 2 ) ), check_float( L, 3 ), opt_color( L, 4 ),
		                                           static_cast< int >( luaL_optinteger( L, 5, 32 ) ), opt_float( L, 6, 1.f ) ) );
		return 0;
	}

	int l_render_circle_filled( lua_State* L )
	{
		require_paint( L );
		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_filled_circle,
		                                   std::make_any< filled_circle_draw_object_t >( c_vector_2d( check_float( L, 1 ), check_float( L, 2 ) ),
		                                                                                 check_float( L, 3 ), opt_color( L, 4 ),
		                                                                                 static_cast< int >( luaL_optinteger( L, 5, 32 ) ) ) );
		return 0;
	}

	void push_poly( const c_vector_2d* points, const int count, const unsigned int fill, const unsigned int outline, const bool has_outline,
	                const float thickness )
	{
		poly_draw_object_t poly{ };
		poly.m_count = std::min( count, 32 );
		for ( int i = 0; i < poly.m_count; i++ )
			poly.m_points[ i ] = points[ i ];
		poly.m_color         = fill;
		poly.m_outline_color = outline;
		poly.m_outline       = has_outline;
		poly.m_thickness     = thickness;

		g_render.m_draw_data.emplace_back( e_draw_type::draw_type_poly, std::make_any< poly_draw_object_t >( poly ) );
	}

	int l_render_triangle( lua_State* L )
	{
		require_paint( L );

		const c_vector_2d points[ 3 ] = { c_vector_2d( check_float( L, 1 ), check_float( L, 2 ) ), c_vector_2d( check_float( L, 3 ), check_float( L, 4 ) ),
			                              c_vector_2d( check_float( L, 5 ), check_float( L, 6 ) ) };
		const unsigned int color      = opt_color( L, 7 );
		const bool filled             = lua_isnoneornil( L, 8 ) ? true : lua_toboolean( L, 8 );

		push_poly( points, 3, filled ? color : 0u, color, !filled, opt_float( L, 9, 1.f ) );
		return 0;
	}

	int l_render_poly( lua_State* L )
	{
		require_paint( L );
		luaL_checktype( L, 1, LUA_TTABLE );

		c_vector_2d points[ 32 ];
		const int count = std::min( static_cast< int >( lua_rawlen( L, 1 ) ) / 2, 32 );
		if ( count < 3 )
			return luaL_error( L, "render.poly needs at least 3 points ( x1, y1, x2, y2, x3, y3 )" );

		for ( int i = 0; i < count; i++ ) {
			lua_rawgeti( L, 1, i * 2 + 1 );
			lua_rawgeti( L, 1, i * 2 + 2 );
			points[ i ] = c_vector_2d( static_cast< float >( luaL_checknumber( L, -2 ) ), static_cast< float >( luaL_checknumber( L, -1 ) ) );
			lua_pop( L, 2 );
		}

		const bool has_outline = !lua_isnoneornil( L, 3 );
		push_poly( points, count, opt_color( L, 2 ), has_outline ? opt_color( L, 3 ) : 0u, has_outline, opt_float( L, 4, 1.f ) );
		return 0;
	}

	int l_render_world_to_screen( lua_State* L )
	{
		require_paint( L );

		c_vector_2d screen{ };
		if ( !g_render.world_to_screen( c_vector( check_float( L, 1 ), check_float( L, 2 ), check_float( L, 3 ) ), screen ) ) {
			lua_pushnil( L );
			return 1;
		}

		lua_pushnumber( L, screen.m_x );
		lua_pushnumber( L, screen.m_y );
		return 2;
	}

	/* netvars.cpp keeps a prop only under the class that declares it ( baseclass is skipped ), so
	   "CCSPlayer->m_iHealth" is not in it: walk the class's recv table and every table below it instead */
	int recv_offset( recv_table_t* table, const char* prop, const int depth )
	{
		if ( !table || depth > 32 )
			return 0;

		for ( int i = 0; i < table->m_props_count; i++ ) {
			const recv_prop_t& entry = table->m_props[ i ];
			if ( !entry.m_var_name )
				continue;
			if ( std::strcmp( entry.m_var_name, prop ) == 0 )
				return entry.m_offset;
			if ( entry.m_data_table && entry.m_data_table->m_props_count > 0 )
				if ( const int inner = recv_offset( entry.m_data_table, prop, depth + 1 ) )
					return inner + entry.m_offset;
		}
		return 0;
	}

	/* game thread only (lua) */
	int class_netvar( const char* class_name, const char* prop )
	{
		static std::unordered_map< std::string, int > s_cache;
		std::string key = std::string( class_name ) + "->" + prop;
		if ( const auto it = s_cache.find( key ); it != s_cache.end( ) )
			return it->second;

		int offset = 0;
		for ( c_base_client* entry = g_interfaces.m_base_client->get_all_classes( ); entry; entry = entry->m_next )
			if ( entry->m_network_name && std::strcmp( entry->m_network_name, class_name ) == 0 ) {
				offset = recv_offset( entry->m_recv_table, prop, 0 );
				break;
			}

		s_cache.emplace( std::move( key ), offset );
		return offset;
	}

	/* "Class->m_prop" */
	int netvar_offset( const char* name )
	{
		const char* split = std::strstr( name, "->" );
		if ( !split )
			return 0;
		return class_netvar( std::string( name, split ).c_str( ), split + 2 );
	}

	/* ---- entities: userdata keeps the index, every call re-resolves it ---- */

	void push_entity( lua_State* L, c_base_entity* entity )
	{
		if ( !entity ) {
			lua_pushnil( L );
			return;
		}

		auto* ref    = static_cast< entity_ref_t* >( lua_newuserdatauv( L, sizeof( entity_ref_t ), 0 ) );
		ref->m_index = static_cast< int >( entity->get_index( ) );
		luaL_setmetatable( L, k_entity_meta );
	}

	int entity_index( lua_State* L, const int index )
	{
		return static_cast< const entity_ref_t* >( luaL_checkudata( L, index, k_entity_meta ) )->m_index;
	}

	c_base_entity* check_entity( lua_State* L, const int index = 1 )
	{
		const int entity_number = entity_index( L, index );
		auto* entity            = g_interfaces.m_client_entity_list->get< c_base_entity >( entity_number );
		if ( !entity )
			luaL_error( L, "entity %d is gone", entity_number );
		return entity;
	}

	c_base_entity* opt_entity( lua_State* L, const int index )
	{
		if ( lua_isnoneornil( L, index ) )
			return nullptr;

		return g_interfaces.m_client_entity_list->get< c_base_entity >( entity_index( L, index ) );
	}

	int l_entities_local( lua_State* L )
	{
		push_entity( L, local_player( ) );
		return 1;
	}

	int l_entities_get( lua_State* L )
	{
		push_entity( L, g_interfaces.m_client_entity_list->get< c_base_entity >( static_cast< int >( luaL_checkinteger( L, 1 ) ) ) );
		return 1;
	}

	int l_entities_from_handle( lua_State* L )
	{
		push_entity( L, g_interfaces.m_client_entity_list->get< c_base_entity >( static_cast< unsigned int >( luaL_checkinteger( L, 1 ) ) ) );
		return 1;
	}

	int l_entities_highest_index( lua_State* L )
	{
		lua_pushinteger( L, g_interfaces.m_client_entity_list->get_highest_entity_index( ) );
		return 1;
	}

	int l_entities_players( lua_State* L )
	{
		const bool include_dead    = lua_toboolean( L, 1 );
		const bool include_dormant = lua_toboolean( L, 2 );

		lua_newtable( L );
		lua_Integer count = 0;

		if ( !g_interfaces.m_engine_client->is_in_game( ) )
			return 1;

		for ( int i = 1; i <= g_interfaces.m_global_vars_base->m_max_clients; i++ ) {
			auto* player = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
			if ( !player || !player->is_player( ) )
				continue;
			if ( !include_dead && !player->is_alive( ) )
				continue;
			if ( !include_dormant && player->is_dormant( ) )
				continue;

			push_entity( L, player );
			lua_rawseti( L, -2, ++count );
		}

		return 1;
	}

	int l_entities_by_class( lua_State* L )
	{
		const char* wanted = luaL_checkstring( L, 1 );

		lua_newtable( L );
		lua_Integer count = 0;

		const int highest = g_interfaces.m_client_entity_list->get_highest_entity_index( );
		for ( int i = 0; i <= highest; i++ ) {
			auto* entity = g_interfaces.m_client_entity_list->get< c_base_entity >( i );
			if ( !entity )
				continue;

			c_base_client* client_class = entity->get_client_class( );
			if ( !client_class || !client_class->m_network_name || std::strcmp( client_class->m_network_name, wanted ) != 0 )
				continue;

			push_entity( L, entity );
			lua_rawseti( L, -2, ++count );
		}

		return 1;
	}

	int l_entity_index( lua_State* L )
	{
		lua_pushinteger( L, entity_index( L, 1 ) );
		return 1;
	}

	int l_entity_ptr( lua_State* L )
	{
		push_address( L, check_entity( L ) );
		return 1;
	}

	int l_entity_is_valid( lua_State* L )
	{
		lua_pushboolean( L, g_interfaces.m_client_entity_list->get< c_base_entity >( entity_index( L, 1 ) ) != nullptr );
		return 1;
	}

	int l_entity_is_player( lua_State* L )
	{
		lua_pushboolean( L, check_entity( L )->is_player( ) );
		return 1;
	}

	int l_entity_is_alive( lua_State* L )
	{
		lua_pushboolean( L, check_entity( L )->is_alive( ) );
		return 1;
	}

	int l_entity_is_dormant( lua_State* L )
	{
		lua_pushboolean( L, check_entity( L )->is_dormant( ) );
		return 1;
	}

	int l_entity_is_enemy( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );
		c_base_entity* local  = local_player( );
		lua_pushboolean( L, local && local != entity && local->is_enemy( entity ) );
		return 1;
	}

	int l_entity_team( lua_State* L )
	{
		lua_pushinteger( L, check_entity( L )->get_team( ) );
		return 1;
	}

	int l_entity_health( lua_State* L )
	{
		lua_pushinteger( L, check_entity( L )->get_health( ) );
		return 1;
	}

	/* this entity's own class has the prop: a getter bound to a fixed-class offset on any other entity reads past it */
	bool class_prop( c_base_entity* entity, const char* prop )
	{
		c_base_client* client_class = entity->get_client_class( );
		return client_class && client_class->m_network_name && class_netvar( client_class->m_network_name, prop ) != 0;
	}

	int l_entity_armor( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );
		lua_pushinteger( L, class_prop( entity, "m_ArmorValue" ) ? entity->get_armor( ) : 0 );
		return 1;
	}

	int l_entity_flags( lua_State* L )
	{
		lua_pushinteger( L, check_entity( L )->get_flags( ) );
		return 1;
	}

	int l_entity_move_type( lua_State* L )
	{
		lua_pushinteger( L, check_entity( L )->get_move_type( ) );
		return 1;
	}

	int l_entity_origin( lua_State* L )
	{
		push_vector( L, check_entity( L )->get_abs_origin( ) );
		return 3;
	}

	int l_entity_velocity( lua_State* L )
	{
		push_vector( L, check_entity( L )->get_velocity( ) );
		return 3;
	}

	/* get_eye_position = vtable 169, a player slot: anything else gets its origin */
	int l_entity_eye_position( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );
		push_vector( L, entity->is_player( ) ? entity->get_eye_position( ) : entity->get_abs_origin( ) );
		return 3;
	}

	int l_entity_mins_maxs( lua_State* L )
	{
		c_collideable* collideable = check_entity( L )->get_collideable( );
		if ( !collideable )
			return 0;

		push_vector( L, collideable->get_obb_mins( ) );
		push_vector( L, collideable->get_obb_maxs( ) );
		return 6;
	}

	int l_entity_class_name( lua_State* L )
	{
		c_base_client* client_class = check_entity( L )->get_client_class( );
		lua_pushstring( L, client_class && client_class->m_network_name ? client_class->m_network_name : "" );
		return 1;
	}

	int l_entity_class_id( lua_State* L )
	{
		c_base_client* client_class = check_entity( L )->get_client_class( );
		lua_pushinteger( L, client_class ? static_cast< int >( client_class->m_class_id ) : -1 );
		return 1;
	}

	int l_entity_name( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );

		player_info_t info{ };
		if ( entity->is_player( ) && g_interfaces.m_engine_client->get_player_info( static_cast< int >( entity->get_index( ) ), &info ) )
			lua_pushstring( L, info.m_name );
		else
			lua_pushstring( L, entity->get_class_name( ) ? entity->get_class_name( ) : "" );
		return 1;
	}

	int l_entity_weapon( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );
		push_entity( L, class_prop( entity, "m_hActiveWeapon" )
		                    ? g_interfaces.m_client_entity_list->get< c_base_entity >( entity->get_active_weapon_handle( ) )
		                    : nullptr );
		return 1;
	}

	int l_entity_weapon_id( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );
		lua_pushinteger( L, class_prop( entity, "m_iItemDefinitionIndex" ) ? entity->get_item_definition_index( ) : -1 );
		return 1;
	}

	/* own read, not c_base_entity::get_bone_position: that memcpy's the cache's count into a 128 stack array
	   and returns row 3 of a 3 row matrix. studio models only ( m_nHitboxSet = C_BaseAnimating ) */
	int l_entity_bone_position( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );
		const int bone        = static_cast< int >( luaL_checkinteger( L, 2 ) );
		if ( bone < 0 || bone >= 128 )
			return luaL_error( L, "bone %d out of range", bone );
		if ( !class_prop( entity, "m_nHitboxSet" ) )
			return 0;

		auto& cache = entity->get_cached_bone_data( );
		if ( bone >= cache.count( ) || !cache.get_elements( ) )
			return 0;

		matrix3x4_t matrix{ };
		if ( !seh_copy( &matrix, &cache.get_elements( )[ bone ], sizeof( matrix ) ) )
			return 0;

		push_vector( L, matrix.at( 3 ) );
		return 3;
	}

	int l_entity_hitbox_position( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );
		if ( !class_prop( entity, "m_nHitboxSet" ) )
			return 0;
		push_vector( L, entity->get_hitbox_position( static_cast< int >( luaL_checkinteger( L, 2 ) ) ) );
		return 3;
	}

	/* type names shared by get_prop / set_prop / get_datafield */
	enum e_value_type { value_int, value_float, value_bool, value_short, value_byte, value_vector, value_string, value_handle };

	e_value_type check_value_type( lua_State* L, const int index )
	{
		static const char* const names[ ] = { "int", "float", "bool", "short", "byte", "vector", "string", "handle", nullptr };
		return static_cast< e_value_type >( luaL_checkoption( L, index, "int", names ) );
	}

	int push_value( lua_State* L, const word_t address, const e_value_type type )
	{
		switch ( type ) {
		case value_float: {
			float value = 0.f;
			if ( !seh_copy( &value, reinterpret_cast< const void* >( address ), sizeof( value ) ) )
				return luaL_error( L, "bad read" );
			lua_pushnumber( L, value );
			return 1;
		}
		case value_bool: {
			unsigned char value = 0;
			if ( !seh_copy( &value, reinterpret_cast< const void* >( address ), sizeof( value ) ) )
				return luaL_error( L, "bad read" );
			lua_pushboolean( L, value != 0 );
			return 1;
		}
		case value_short: {
			short value = 0;
			if ( !seh_copy( &value, reinterpret_cast< const void* >( address ), sizeof( value ) ) )
				return luaL_error( L, "bad read" );
			lua_pushinteger( L, value );
			return 1;
		}
		case value_byte: {
			unsigned char value = 0;
			if ( !seh_copy( &value, reinterpret_cast< const void* >( address ), sizeof( value ) ) )
				return luaL_error( L, "bad read" );
			lua_pushinteger( L, value );
			return 1;
		}
		case value_vector: {
			c_vector value{ };
			if ( !seh_copy( &value, reinterpret_cast< const void* >( address ), sizeof( value ) ) )
				return luaL_error( L, "bad read" );
			push_vector( L, value );
			return 3;
		}
		case value_string: {
			const int length = seh_strlen( reinterpret_cast< const char* >( address ), 4096 );
			if ( length < 0 )
				return luaL_error( L, "bad read" );
			lua_pushlstring( L, reinterpret_cast< const char* >( address ), static_cast< std::size_t >( length ) );
			return 1;
		}
		case value_handle: {
			unsigned int value = 0;
			if ( !seh_copy( &value, reinterpret_cast< const void* >( address ), sizeof( value ) ) )
				return luaL_error( L, "bad read" );
			push_entity( L, g_interfaces.m_client_entity_list->get< c_base_entity >( value ) );
			return 1;
		}
		default: {
			int value = 0;
			if ( !seh_copy( &value, reinterpret_cast< const void* >( address ), sizeof( value ) ) )
				return luaL_error( L, "bad read" );
			lua_pushinteger( L, value );
			return 1;
		}
		}
	}

	void write_value( lua_State* L, const word_t address, const e_value_type type, const int value_index )
	{
		void* const target = reinterpret_cast< void* >( address );
		bool ok            = false;

		switch ( type ) {
		case value_float: {
			const float value = check_float( L, value_index );
			ok                = seh_write( target, &value, sizeof( value ) );
			break;
		}
		case value_bool: {
			const unsigned char value = lua_toboolean( L, value_index ) ? 1 : 0;
			ok                        = seh_write( target, &value, sizeof( value ) );
			break;
		}
		case value_short: {
			const short value = static_cast< short >( luaL_checkinteger( L, value_index ) );
			ok                = seh_write( target, &value, sizeof( value ) );
			break;
		}
		case value_byte: {
			const unsigned char value = static_cast< unsigned char >( luaL_checkinteger( L, value_index ) );
			ok                        = seh_write( target, &value, sizeof( value ) );
			break;
		}
		case value_vector: {
			const c_vector value( check_float( L, value_index ), check_float( L, value_index + 1 ), check_float( L, value_index + 2 ) );
			ok = seh_write( target, &value, sizeof( value ) );
			break;
		}
		case value_string: luaL_error( L, "string props are read only" ); return;
		default: {
			const int value = static_cast< int >( luaL_checkinteger( L, value_index ) );
			ok              = seh_write( target, &value, sizeof( value ) );
			break;
		}
		}

		if ( !ok )
			luaL_error( L, "bad write" );
	}

	unsigned int check_netvar( lua_State* L, const int index )
	{
		const char* name          = luaL_checkstring( L, index );
		const unsigned int offset = static_cast< unsigned int >( netvar_offset( name ) );
		if ( !offset )
			luaL_error( L, "no netvar '%s' ( format: \"CCSPlayer->m_iHealth\" )", name );
		return offset;
	}

	/* entity:get_prop( "CBasePlayer->m_iHealth", "int", extra_offset ) */
	int l_entity_get_prop( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );
		const word_t address  = reinterpret_cast< word_t >( entity ) + check_netvar( L, 2 ) + static_cast< word_t >( luaL_optinteger( L, 4, 0 ) );
		return push_value( L, address, check_value_type( L, 3 ) );
	}

	/* entity:set_prop( name, type, value... ) */
	int l_entity_set_prop( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );
		const word_t address  = reinterpret_cast< word_t >( entity ) + check_netvar( L, 2 );
		write_value( L, address, check_value_type( L, 3 ), 4 );
		return 0;
	}

	unsigned int check_datafield( lua_State* L, c_base_entity* entity, const int index )
	{
		const char* name         = luaL_checkstring( L, index );
		const unsigned int hash  = HASH_RT( name );
		unsigned int offset      = g_netvars.find_in_data_map( entity->get_prediction_desc_map( ), hash );
		if ( !offset )
			offset = g_netvars.find_in_data_map( entity->get_data_desc_map( ), hash );
		if ( !offset )
			luaL_error( L, "no datamap field '%s'", name );
		return offset;
	}

	int l_entity_get_datafield( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );
		return push_value( L, reinterpret_cast< word_t >( entity ) + check_datafield( L, entity, 2 ), check_value_type( L, 3 ) );
	}

	int l_entity_set_datafield( lua_State* L )
	{
		c_base_entity* entity = check_entity( L );
		write_value( L, reinterpret_cast< word_t >( entity ) + check_datafield( L, entity, 2 ), check_value_type( L, 3 ), 4 );
		return 0;
	}

	int l_entity_eq( lua_State* L )
	{
		lua_pushboolean( L, entity_index( L, 1 ) == entity_index( L, 2 ) );
		return 1;
	}

	int l_entity_tostring( lua_State* L )
	{
		lua_pushfstring( L, "entity(%d)", entity_index( L, 1 ) );
		return 1;
	}

	/* ---- engine / globals ---- */

	int l_engine_exec( lua_State* L )
	{
		g_interfaces.m_engine_client->client_cmd_unrestricted( luaL_checkstring( L, 1 ) );
		return 0;
	}

	int l_engine_in_game( lua_State* L )
	{
		lua_pushboolean( L, g_interfaces.m_engine_client->is_in_game( ) );
		return 1;
	}

	int l_engine_connected( lua_State* L )
	{
		lua_pushboolean( L, g_interfaces.m_engine_client->is_connected( ) );
		return 1;
	}

	int l_engine_local_index( lua_State* L )
	{
		lua_pushinteger( L, g_interfaces.m_engine_client->get_local_player( ) );
		return 1;
	}

	int l_engine_max_clients( lua_State* L )
	{
		lua_pushinteger( L, g_interfaces.m_engine_client->get_max_clients( ) );
		return 1;
	}

	int l_engine_map_name( lua_State* L )
	{
		const char* name = g_interfaces.m_engine_client->get_level_name_short( );
		lua_pushstring( L, name ? name : "" );
		return 1;
	}

	int l_engine_level_name( lua_State* L )
	{
		const char* name = g_interfaces.m_engine_client->get_level_name( );
		lua_pushstring( L, name ? name : "" );
		return 1;
	}

	int l_engine_view_angles( lua_State* L )
	{
		c_angle angles{ };
		g_interfaces.m_engine_client->get_view_angles( angles );
		lua_pushnumber( L, angles.m_x );
		lua_pushnumber( L, angles.m_y );
		lua_pushnumber( L, angles.m_z );
		return 3;
	}

	int l_engine_set_view_angles( lua_State* L )
	{
		c_angle angles( check_float( L, 1 ), check_float( L, 2 ), opt_float( L, 3, 0.f ) );
		angles.normalize( );
		angles.clamp( );
		g_interfaces.m_engine_client->set_view_angles( angles );
		return 0;
	}

	int l_engine_player_info( lua_State* L )
	{
		player_info_t info{ };
		if ( !g_interfaces.m_engine_client->get_player_info( static_cast< int >( luaL_checkinteger( L, 1 ) ), &info ) ) {
			lua_pushnil( L );
			return 1;
		}

		lua_newtable( L );
		set_field( L, "name", info.m_name );
		set_field( L, "user_id", static_cast< lua_Integer >( info.m_user_id ) );
		set_field( L, "steam_id", info.m_steam_id );
		set_field( L, "xuid", static_cast< lua_Integer >( info.m_ull_xuid ) );
		set_field( L, "friends_id", static_cast< lua_Integer >( info.m_friends_id ) );
		set_bool( L, "bot", info.m_fake_player );
		set_bool( L, "hltv", info.m_is_hltv );
		return 1;
	}

	int l_engine_player_for_user_id( lua_State* L )
	{
		lua_pushinteger( L, g_interfaces.m_engine_client->get_player_for_user_id( static_cast< int >( luaL_checkinteger( L, 1 ) ) ) );
		return 1;
	}

	int l_engine_playing_demo( lua_State* L )
	{
		lua_pushboolean( L, g_interfaces.m_engine_client->is_playing_demo( ) );
		return 1;
	}

	int l_engine_latency( lua_State* L )
	{
		c_net_channel_info* channel = g_interfaces.m_engine_client->get_net_channel_info( );
		lua_pushnumber( L, channel ? channel->get_latency( 0 ) : 0.f );
		lua_pushnumber( L, channel ? channel->get_latency( 1 ) : 0.f );
		return 2;
	}

	int l_globals_realtime( lua_State* L )
	{
		lua_pushnumber( L, g_interfaces.m_global_vars_base->m_real_time );
		return 1;
	}

	int l_globals_curtime( lua_State* L )
	{
		lua_pushnumber( L, g_interfaces.m_global_vars_base->m_current_time );
		return 1;
	}

	int l_globals_frametime( lua_State* L )
	{
		lua_pushnumber( L, g_interfaces.m_global_vars_base->m_frame_time );
		return 1;
	}

	int l_globals_absframetime( lua_State* L )
	{
		lua_pushnumber( L, g_interfaces.m_global_vars_base->m_abs_frame_time );
		return 1;
	}

	int l_globals_tickcount( lua_State* L )
	{
		lua_pushinteger( L, g_interfaces.m_global_vars_base->m_tick_count );
		return 1;
	}

	int l_globals_tickinterval( lua_State* L )
	{
		lua_pushnumber( L, g_interfaces.m_global_vars_base->m_interval_per_tick );
		return 1;
	}

	int l_globals_framecount( lua_State* L )
	{
		lua_pushinteger( L, g_interfaces.m_global_vars_base->m_frame_count );
		return 1;
	}

	int l_globals_maxclients( lua_State* L )
	{
		lua_pushinteger( L, g_interfaces.m_global_vars_base->m_max_clients );
		return 1;
	}

	/* ---- cvar ---- */

	c_cconvar* find_cvar( lua_State* L )
	{
		return g_interfaces.m_convar->find_var( luaL_checkstring( L, 1 ) );
	}

	int l_cvar_exists( lua_State* L )
	{
		lua_pushboolean( L, find_cvar( L ) != nullptr );
		return 1;
	}

	int l_cvar_get_int( lua_State* L )
	{
		c_cconvar* var = find_cvar( L );
		if ( !var )
			lua_pushnil( L );
		else
			lua_pushinteger( L, var->get_int( ) );
		return 1;
	}

	int l_cvar_get_float( lua_State* L )
	{
		c_cconvar* var = find_cvar( L );
		if ( !var )
			lua_pushnil( L );
		else
			lua_pushnumber( L, var->get_float( ) );
		return 1;
	}

	int l_cvar_get_string( lua_State* L )
	{
		c_cconvar* var = find_cvar( L );
		const char* value = var ? var->get_string( ) : nullptr;
		if ( !value )
			lua_pushnil( L );
		else
			lua_pushstring( L, value );
		return 1;
	}

	int l_cvar_set( lua_State* L )
	{
		c_cconvar* var = find_cvar( L );
		if ( !var )
			return luaL_error( L, "no cvar '%s'", lua_tostring( L, 1 ) );

		if ( lua_isinteger( L, 2 ) )
			var->set_value( static_cast< int >( lua_tointeger( L, 2 ) ) );
		else if ( lua_type( L, 2 ) == LUA_TNUMBER )
			var->set_value( static_cast< float >( lua_tonumber( L, 2 ) ) );
		else if ( lua_isboolean( L, 2 ) )
			var->set_value( lua_toboolean( L, 2 ) ? 1 : 0 );
		else
			var->set_value( luaL_checkstring( L, 2 ) );
		return 0;
	}

	/* ---- config: every botox variable by its variables.h name ---- */

	variable_object_t& check_variable( lua_State* L )
	{
		const char* name        = luaL_checkstring( L, 1 );
		const std::size_t index = g_config.get_variable_index( HASH_RT( name ) );
		if ( index == INVALID_VARIABLE )
			luaL_error( L, "config: no variable '%s'", name );
		return g_config.m_variables[ index ];
	}

	const char* type_name( const unsigned int type_hash )
	{
		switch ( type_hash ) {
		case HASH_BT( "bool" ): return "bool";
		case HASH_BT( "int" ): return "int";
		case HASH_BT( "float" ): return "float";
		case HASH_BT( "std::string" ): return "string";
		case HASH_BT( "c_color" ): return "color";
		case HASH_BT( "key_bind_t" ): return "keybind";
		case HASH_BT( "font_setting_t" ): return "font";
		case HASH_BT( "std::vector<bool>" ): return "bool[]";
		case HASH_BT( "std::vector<int>" ): return "int[]";
		case HASH_BT( "std::vector<float>" ): return "float[]";
		case HASH_BT( "std::vector<std::string>" ): return "string[]";
		case HASH_BT( "std::vector<c_color>" ): return "color[]";
		default: return "unknown";
		}
	}

	void push_color( lua_State* L, c_color& color )
	{
		for ( const std::uint8_t channel : color.get_colors( ) )
			lua_pushinteger( L, channel );
	}

	template< class T, class push_t >
	int push_vector_table( lua_State* L, std::vector< T >& values, push_t&& push )
	{
		lua_createtable( L, static_cast< int >( values.size( ) ), 0 );
		for ( std::size_t i = 0; i < values.size( ); i++ ) {
			push( values[ i ] );
			lua_rawseti( L, -2, static_cast< lua_Integer >( i + 1 ) );
		}
		return 1;
	}

	int l_config_get( lua_State* L )
	{
		variable_object_t& variable = check_variable( L );

		switch ( variable.m_type_hash ) {
		case HASH_BT( "bool" ): lua_pushboolean( L, variable.get< bool >( ) ); return 1;
		case HASH_BT( "int" ): lua_pushinteger( L, variable.get< int >( ) ); return 1;
		case HASH_BT( "float" ): lua_pushnumber( L, variable.get< float >( ) ); return 1;
		case HASH_BT( "std::string" ): lua_pushstring( L, variable.get< std::string >( ).c_str( ) ); return 1;
		case HASH_BT( "c_color" ): push_color( L, variable.get< c_color >( ) ); return 4;
		case HASH_BT( "key_bind_t" ): {
			const key_bind_t& bind = variable.get< key_bind_t >( );
			lua_pushinteger( L, bind.m_key );
			lua_pushinteger( L, bind.m_key_style );
			return 2;
		}
		case HASH_BT( "font_setting_t" ): {
			const font_setting_t& font = variable.get< font_setting_t >( );
			lua_pushstring( L, font.m_name.c_str( ) );
			lua_pushinteger( L, font.m_size );
			return 2;
		}
		case HASH_BT( "std::vector<bool>" ): {
			auto& values = variable.get< std::vector< bool > >( );
			lua_createtable( L, static_cast< int >( values.size( ) ), 0 );
			for ( std::size_t i = 0; i < values.size( ); i++ ) {
				lua_pushboolean( L, values[ i ] );
				lua_rawseti( L, -2, static_cast< lua_Integer >( i + 1 ) );
			}
			return 1;
		}
		case HASH_BT( "std::vector<int>" ):
			return push_vector_table( L, variable.get< std::vector< int > >( ), [ L ]( const int value ) { lua_pushinteger( L, value ); } );
		case HASH_BT( "std::vector<float>" ):
			return push_vector_table( L, variable.get< std::vector< float > >( ), [ L ]( const float value ) { lua_pushnumber( L, value ); } );
		case HASH_BT( "std::vector<std::string>" ):
			return push_vector_table( L, variable.get< std::vector< std::string > >( ),
			                          [ L ]( const std::string& value ) { lua_pushstring( L, value.c_str( ) ); } );
		case HASH_BT( "std::vector<c_color>" ):
			/* each colour = { r, g, b, a } */
			return push_vector_table( L, variable.get< std::vector< c_color > >( ), [ L ]( c_color& value ) {
				lua_createtable( L, 4, 0 );
				for ( int i = 0; i < 4; i++ ) {
					lua_pushinteger( L, value.get_colors( )[ i ] );
					lua_rawseti( L, -2, i + 1 );
				}
			} );
		default: lua_pushnil( L ); return 1;
		}
	}

	std::uint8_t color_channel( lua_State* L, const int index )
	{
		return static_cast< std::uint8_t >( std::clamp< lua_Integer >( luaL_optinteger( L, index, 255 ), 0, 255 ) );
	}

	template< class T, class read_t >
	void set_vector_table( lua_State* L, std::vector< T >& values, read_t&& read )
	{
		luaL_checktype( L, 2, LUA_TTABLE );
		for ( std::size_t i = 0; i < values.size( ); i++ ) {
			if ( lua_rawgeti( L, 2, static_cast< lua_Integer >( i + 1 ) ) != LUA_TNIL )
				values[ i ] = read( );
			lua_pop( L, 1 );
		}
	}

	int l_config_set( lua_State* L )
	{
		variable_object_t& variable = check_variable( L );

		switch ( variable.m_type_hash ) {
		case HASH_BT( "bool" ): variable.get< bool >( ) = lua_toboolean( L, 2 ); break;
		case HASH_BT( "int" ): variable.get< int >( ) = static_cast< int >( luaL_checkinteger( L, 2 ) ); break;
		case HASH_BT( "float" ): variable.get< float >( ) = check_float( L, 2 ); break;
		case HASH_BT( "std::string" ): variable.get< std::string >( ) = luaL_checkstring( L, 2 ); break;
		case HASH_BT( "c_color" ):
			variable.get< c_color >( ) = c_color( color_channel( L, 2 ), color_channel( L, 3 ), color_channel( L, 4 ), color_channel( L, 5 ) );
			break;
		case HASH_BT( "key_bind_t" ): {
			key_bind_t& bind = variable.get< key_bind_t >( );
			bind.m_key       = std::clamp( static_cast< int >( luaL_checkinteger( L, 2 ) ), 0, 255 );
			bind.m_key_style = std::clamp( static_cast< int >( luaL_optinteger( L, 3, bind.m_key_style ) ), 0, 2 );
			break;
		}
		case HASH_BT( "font_setting_t" ): {
			font_setting_t& font = variable.get< font_setting_t >( );
			font.m_name          = luaL_checkstring( L, 2 );
			font.m_size          = static_cast< int >( luaL_optinteger( L, 3, font.m_size ) );
			g_render.m_reload_fonts = true;
			break;
		}
		case HASH_BT( "std::vector<bool>" ): {
			auto& values = variable.get< std::vector< bool > >( );
			luaL_checktype( L, 2, LUA_TTABLE );
			for ( std::size_t i = 0; i < values.size( ); i++ ) {
				if ( lua_rawgeti( L, 2, static_cast< lua_Integer >( i + 1 ) ) != LUA_TNIL )
					values[ i ] = lua_toboolean( L, -1 );
				lua_pop( L, 1 );
			}
			break;
		}
		case HASH_BT( "std::vector<int>" ):
			set_vector_table( L, variable.get< std::vector< int > >( ), [ L ]( ) { return static_cast< int >( luaL_checkinteger( L, -1 ) ); } );
			break;
		case HASH_BT( "std::vector<float>" ):
			set_vector_table( L, variable.get< std::vector< float > >( ), [ L ]( ) { return check_float( L, -1 ); } );
			break;
		case HASH_BT( "std::vector<std::string>" ):
			set_vector_table( L, variable.get< std::vector< std::string > >( ), [ L ]( ) { return std::string( luaL_checkstring( L, -1 ) ); } );
			break;
		case HASH_BT( "std::vector<c_color>" ):
			set_vector_table( L, variable.get< std::vector< c_color > >( ), [ L ]( ) {
				luaL_checktype( L, -1, LUA_TTABLE );
				std::uint8_t channels[ 4 ] = { 255, 255, 255, 255 };
				for ( int i = 0; i < 4; i++ ) {
					if ( lua_rawgeti( L, -1, i + 1 ) == LUA_TNUMBER )
						channels[ i ] = static_cast< std::uint8_t >( std::clamp< lua_Integer >( lua_tointeger( L, -1 ), 0, 255 ) );
					lua_pop( L, 1 );
				}
				return c_color( channels[ 0 ], channels[ 1 ], channels[ 2 ], channels[ 3 ] );
			} );
			break;
		default: return luaL_error( L, "config: cannot set this type" );
		}

		return 0;
	}

	int l_config_type( lua_State* L )
	{
		lua_pushstring( L, type_name( check_variable( L ).m_type_hash ) );
		return 1;
	}

	int l_config_reset( lua_State* L )
	{
		variable_object_t& variable = check_variable( L );
		variable.m_value            = variable.m_default;
		return 0;
	}

	int l_config_list( lua_State* L )
	{
		lua_createtable( L, 0, static_cast< int >( g_config.m_variables.size( ) ) );
		for ( const variable_object_t& variable : g_config.m_variables ) {
			lua_pushstring( L, type_name( variable.m_type_hash ) );
			lua_setfield( L, -2, variable.m_name );
		}
		return 1;
	}

	/* ---- trace ---- */

	int run_trace( lua_State* L, const ray_t& ray, const int mask_index, const int skip_index )
	{
		const unsigned int mask = static_cast< unsigned int >( luaL_optinteger( L, mask_index, e_mask::mask_shot ) );
		c_base_entity* skip     = lua_isnoneornil( L, skip_index ) ? local_player( ) : opt_entity( L, skip_index );

		c_trace_filter filter( skip );
		c_game_trace trace{ };
		g_interfaces.m_engine_trace->trace_ray( ray, mask, &filter, &trace );

		lua_newtable( L );
		set_field( L, "fraction", static_cast< lua_Number >( trace.m_fraction ) );
		set_field( L, "x", static_cast< lua_Number >( trace.m_end.m_x ) );
		set_field( L, "y", static_cast< lua_Number >( trace.m_end.m_y ) );
		set_field( L, "z", static_cast< lua_Number >( trace.m_end.m_z ) );
		set_field( L, "normal_x", static_cast< lua_Number >( trace.m_plane.m_normal.m_x ) );
		set_field( L, "normal_y", static_cast< lua_Number >( trace.m_plane.m_normal.m_y ) );
		set_field( L, "normal_z", static_cast< lua_Number >( trace.m_plane.m_normal.m_z ) );
		set_field( L, "contents", static_cast< lua_Integer >( trace.m_contents ) );
		set_field( L, "hitgroup", static_cast< lua_Integer >( trace.m_hit_group ) );
		set_field( L, "hitbox", static_cast< lua_Integer >( trace.m_hitbox ) );
		set_field( L, "surface", trace.surface.m_name ? trace.surface.m_name : "" );
		set_bool( L, "hit", trace.did_hit( ) );
		set_bool( L, "start_solid", trace.m_start_solid );
		set_bool( L, "all_solid", trace.m_all_solid );

		push_entity( L, trace.m_hit_entity );
		lua_setfield( L, -2, "entity" );
		return 1;
	}

	c_vector check_vector( lua_State* L, const int index )
	{
		return c_vector( check_float( L, index ), check_float( L, index + 1 ), check_float( L, index + 2 ) );
	}

	int l_trace_line( lua_State* L )
	{
		return run_trace( L, ray_t( check_vector( L, 1 ), check_vector( L, 4 ) ), 7, 8 );
	}

	int l_trace_hull( lua_State* L )
	{
		return run_trace( L, ray_t( check_vector( L, 1 ), check_vector( L, 4 ), check_vector( L, 7 ), check_vector( L, 10 ) ), 13, 14 );
	}

	int l_trace_point_contents( lua_State* L )
	{
		lua_pushinteger( L, g_interfaces.m_engine_trace->get_point_contents( check_vector( L, 1 ),
		                                                                     static_cast< int >( luaL_optinteger( L, 4, e_mask::mask_all ) ) ) );
		return 1;
	}

	/* ---- input ---- */

	int l_input_is_key_down( lua_State* L )
	{
		const lua_Integer key = luaL_checkinteger( L, 1 );
		lua_pushboolean( L, key >= 0 && key < 256 && g_input.is_key_down( static_cast< std::uint32_t >( key ) ) );
		return 1;
	}

	int l_input_mouse_pos( lua_State* L )
	{
		POINT point{ };
		GetCursorPos( &point );
		if ( g_input.m_window )
			ScreenToClient( g_input.m_window, &point );
		lua_pushinteger( L, point.x );
		lua_pushinteger( L, point.y );
		return 2;
	}

	int l_input_menu_open( lua_State* L )
	{
		lua_pushboolean( L, g_menu.m_opened );
		return 1;
	}

	int l_input_keys_blocked( lua_State* L )
	{
		lua_pushboolean( L, g_input.keys_blocked( ) );
		return 1;
	}

	/* ---- mem: raw access, every read/write seh guarded ---- */

	int l_mem_module( lua_State* L )
	{
		push_address( L, GetModuleHandleA( luaL_checkstring( L, 1 ) ) );
		return 1;
	}

	int l_mem_find_pattern( lua_State* L )
	{
		const char* module_name = luaL_checkstring( L, 1 );
		void* base              = GetModuleHandleA( module_name );
		if ( !base ) {
			lua_pushnil( L );
			return 1;
		}

		push_address( L, module_t( base, module_name ).find_pattern( luaL_checkstring( L, 2 ) ) );
		return 1;
	}

	int l_mem_interface( lua_State* L )
	{
		const char* module_name = luaL_checkstring( L, 1 );
		void* base              = GetModuleHandleA( module_name );
		if ( !base ) {
			lua_pushnil( L );
			return 1;
		}

		push_address( L, module_t( base, module_name ).find_interface( luaL_checkstring( L, 2 ) ) );
		return 1;
	}

	int l_mem_export( lua_State* L )
	{
		HMODULE base = GetModuleHandleA( luaL_checkstring( L, 1 ) );
		push_address( L, base ? reinterpret_cast< void* >( GetProcAddress( base, luaL_checkstring( L, 2 ) ) ) : nullptr );
		return 1;
	}

	template< class T >
	int l_mem_read( lua_State* L )
	{
		const word_t address = check_address( L, 1 );

		T value{ };
		if ( !seh_copy( &value, reinterpret_cast< const void* >( address ), sizeof( T ) ) )
			return luaL_error( L, "mem: bad read at %p", reinterpret_cast< void* >( address ) );

		if constexpr ( std::is_floating_point_v< T > )
			lua_pushnumber( L, value );
		else
			lua_pushinteger( L, static_cast< lua_Integer >( value ) );
		return 1;
	}

	template< class T >
	int l_mem_write( lua_State* L )
	{
		const word_t address = check_address( L, 1 );

		T value{ };
		if constexpr ( std::is_floating_point_v< T > )
			value = static_cast< T >( luaL_checknumber( L, 2 ) );
		else
			value = static_cast< T >( luaL_checkinteger( L, 2 ) );

		if ( !seh_write( reinterpret_cast< void* >( address ), &value, sizeof( T ) ) )
			return luaL_error( L, "mem: bad write at %p", reinterpret_cast< void* >( address ) );
		return 0;
	}

	int l_mem_read_string( lua_State* L )
	{
		const word_t address = check_address( L, 1 );
		const int max        = static_cast< int >( std::clamp< lua_Integer >( luaL_optinteger( L, 2, 256 ), 0, 1 << 20 ) );
		const int length     = seh_strlen( reinterpret_cast< const char* >( address ), max );
		if ( length < 0 )
			return luaL_error( L, "mem: bad read at %p", reinterpret_cast< void* >( address ) );

		lua_pushlstring( L, reinterpret_cast< const char* >( address ), static_cast< std::size_t >( length ) );
		return 1;
	}

	int l_mem_write_string( lua_State* L )
	{
		const word_t address = check_address( L, 1 );
		std::size_t length   = 0;
		const char* text     = luaL_checklstring( L, 2, &length );

		if ( !seh_write( reinterpret_cast< void* >( address ), text, length + 1 ) )
			return luaL_error( L, "mem: bad write at %p", reinterpret_cast< void* >( address ) );
		return 0;
	}

	int l_mem_read_bytes( lua_State* L )
	{
		const word_t address   = check_address( L, 1 );
		const std::size_t size = static_cast< std::size_t >( std::clamp< lua_Integer >( luaL_checkinteger( L, 2 ), 0, 1 << 20 ) );

		std::string out( size, '\0' );
		if ( size && !seh_copy( out.data( ), reinterpret_cast< const void* >( address ), size ) )
			return luaL_error( L, "mem: bad read at %p", reinterpret_cast< void* >( address ) );

		lua_pushlstring( L, out.data( ), out.size( ) );
		return 1;
	}

	int l_mem_write_bytes( lua_State* L )
	{
		const word_t address = check_address( L, 1 );
		std::size_t length   = 0;
		const char* bytes    = luaL_checklstring( L, 2, &length );

		if ( length && !seh_write( reinterpret_cast< void* >( address ), bytes, length ) )
			return luaL_error( L, "mem: bad write at %p", reinterpret_cast< void* >( address ) );
		return 0;
	}

	int l_mem_vfunc( lua_State* L )
	{
		const word_t object = check_address( L, 1 );
		const lua_Integer index = luaL_checkinteger( L, 2 );

		word_t table = 0, function = 0;
		if ( !seh_copy( &table, reinterpret_cast< const void* >( object ), sizeof( table ) ) ||
		     !seh_copy( &function, reinterpret_cast< const void* >( table + static_cast< word_t >( index ) * sizeof( word_t ) ), sizeof( function ) ) )
			return luaL_error( L, "mem: bad vtable at %p", reinterpret_cast< void* >( object ) );

		lua_pushinteger( L, static_cast< lua_Integer >( function ) );
		return 1;
	}

	/* mem.call( address, "thiscall", this, args... ): integer args, eax back. floats go in as mem.f2i bits */
	int l_mem_call( lua_State* L )
	{
		static const char* const conventions[ ] = { "cdecl", "stdcall", "thiscall", "fastcall", nullptr };

		const word_t function = check_address( L, 1 );
		const int convention  = luaL_checkoption( L, 2, "cdecl", conventions );
		const int args        = lua_gettop( L ) - 2;
		const int fixed       = convention == 2 ? 1 : convention == 3 ? 2 : 0;

		if ( args < fixed )
			return luaL_error( L, "mem.call: %s needs %d leading register arg(s)", conventions[ convention ], fixed );
		if ( args - fixed > static_cast< int >( k_max_call_args ) )
			return luaL_error( L, "mem.call: at most %d args", static_cast< int >( k_max_call_args ) );

		word_t values[ k_max_call_args + 2 ] = { };
		for ( int i = 0; i < args; i++ ) {
			const int index = i + 3;
			if ( lua_isboolean( L, index ) )
				values[ i ] = lua_toboolean( L, index ) ? 1 : 0;
			else if ( lua_isnil( L, index ) )
				values[ i ] = 0;
			else if ( lua_type( L, index ) == LUA_TSTRING )
				values[ i ] = reinterpret_cast< word_t >( lua_tostring( L, index ) );
			else
				values[ i ] = static_cast< word_t >( luaL_checkinteger( L, index ) );
		}

		if ( !function )
			return luaL_error( L, "mem.call: null function" );

		word_t result = 0;
		if ( !seh_invoke( k_invokers[ convention ][ args - fixed ], function, values, &result ) )
			return luaL_error( L, "mem.call: crashed inside %p", reinterpret_cast< void* >( function ) );

		lua_pushinteger( L, static_cast< lua_Integer >( result ) );
		return 1;
	}

	int l_mem_alloc( lua_State* L )
	{
		const std::size_t size = static_cast< std::size_t >( std::clamp< lua_Integer >( luaL_checkinteger( L, 1 ), 1, 64 << 20 ) );
		void* memory           = std::calloc( 1, size );
		if ( !memory )
			return luaL_error( L, "mem.alloc: out of memory" );

		n_lua_api::script_of( L )->m_allocations.push_back( memory );
		push_address( L, memory );
		return 1;
	}

	int l_mem_free( lua_State* L )
	{
		void* memory      = reinterpret_cast< void* >( check_address( L, 1 ) );
		auto& allocations = n_lua_api::script_of( L )->m_allocations;

		const auto it = std::find( allocations.begin( ), allocations.end( ), memory );
		if ( it == allocations.end( ) )
			return luaL_error( L, "mem.free: not a mem.alloc block" );

		std::free( memory );
		allocations.erase( it );
		return 0;
	}

	int l_mem_protect( lua_State* L )
	{
		DWORD old_protect = 0;
		if ( !VirtualProtect( reinterpret_cast< void* >( check_address( L, 1 ) ), static_cast< SIZE_T >( luaL_checkinteger( L, 2 ) ),
		                      static_cast< DWORD >( luaL_checkinteger( L, 3 ) ), &old_protect ) ) {
			lua_pushnil( L );
			return 1;
		}

		lua_pushinteger( L, old_protect );
		return 1;
	}

	int l_mem_netvar( lua_State* L )
	{
		lua_pushinteger( L, netvar_offset( luaL_checkstring( L, 1 ) ) );
		return 1;
	}

	int l_mem_f2i( lua_State* L )
	{
		const float value = check_float( L, 1 );
		std::uint32_t bits = 0;
		std::memcpy( &bits, &value, sizeof( bits ) );
		lua_pushinteger( L, bits );
		return 1;
	}

	int l_mem_i2f( lua_State* L )
	{
		const std::uint32_t bits = static_cast< std::uint32_t >( luaL_checkinteger( L, 1 ) );
		float value              = 0.f;
		std::memcpy( &value, &bits, sizeof( value ) );
		lua_pushnumber( L, value );
		return 1;
	}

	int l_mem_interfaces( lua_State* L )
	{
		const std::pair< const char*, const void* > list[ ] = {
			{ "base_client", g_interfaces.m_base_client },
			{ "engine_client", g_interfaces.m_engine_client },
			{ "client_entity_list", g_interfaces.m_client_entity_list },
			{ "input_system", g_interfaces.m_input_system },
			{ "material_system", g_interfaces.m_material_system },
			{ "engine_trace", g_interfaces.m_engine_trace },
			{ "debug_overlay", g_interfaces.m_debug_overlay },
			{ "convar", g_interfaces.m_convar },
			{ "model_info", g_interfaces.m_model_info },
			{ "game_movement", g_interfaces.m_game_movement },
			{ "prediction", g_interfaces.m_prediction },
			{ "model_render", g_interfaces.m_model_render },
			{ "string_tables", g_interfaces.m_string_tables },
			{ "model_cache", g_interfaces.m_model_cache },
			{ "engine_sound", g_interfaces.m_engine_sound },
			{ "physics_surface_props", g_interfaces.m_physics_surface_props },
			{ "surface", g_interfaces.m_surface },
			{ "game_types", g_interfaces.m_game_types },
			{ "localize", g_interfaces.m_localize },
			{ "panel", g_interfaces.m_panel },
			{ "physics_collision", g_interfaces.m_physics_collison },
			{ "studio_render", g_interfaces.m_studio_render },
			{ "render_view", g_interfaces.m_render_view },
			{ "game_event_manager", g_interfaces.m_game_event_manager },
			{ "key_values_system", g_interfaces.m_key_values_system },
			{ "global_vars", g_interfaces.m_global_vars_base },
			{ "client_mode", g_interfaces.m_client_mode },
			{ "weapon_system", g_interfaces.m_weapon_system },
			{ "client_state", g_interfaces.m_client_state },
			{ "input", g_interfaces.m_input },
			{ "move_helper", g_interfaces.m_move_helper },
			{ "d3d_device", g_interfaces.m_direct_device },
			{ "panorama", g_interfaces.m_panorama },
			{ "view_render_beams", g_interfaces.m_view_render_beams },
			{ "file_system", g_interfaces.m_file_system },
			{ "hud_chat", n_interfaces::chat_element },
		};

		lua_createtable( L, 0, static_cast< int >( std::size( list ) ) );
		for ( const auto& [ name, pointer ] : list ) {
			push_address( L, pointer );
			lua_setfield( L, -2, name );
		}
		return 1;
	}

	/* ---- client ---- */

	int l_client_log( lua_State* L )
	{
		g_logger.print( luaL_checkstring( L, 1 ), "[" + n_lua_api::script_of( L )->m_name + "]" );
		return 0;
	}

	int l_client_chat( lua_State* L )
	{
		if ( !n_interfaces::chat_element )
			return 0;

		std::string text = luaL_checkstring( L, 1 );
		for ( std::size_t at = text.find( '%' ); at != std::string::npos; at = text.find( '%', at + 2 ) )
			text.insert( at, 1, '%' );

		n_interfaces::chat_element->chatprintf( 0, 0, "%s", text.c_str( ) );
		return 0;
	}

	int l_client_name( lua_State* L )
	{
		lua_pushstring( L, n_lua_api::script_of( L )->m_name.c_str( ) );
		return 1;
	}

	int l_client_folder( lua_State* L )
	{
		lua_pushstring( L, g_lua.folder( ).string( ).c_str( ) );
		return 1;
	}

	int l_client_unload( lua_State* L )
	{
		g_lua.request( n_lua::request_unload, n_lua_api::script_of( L )->m_name );
		return 0;
	}

	int l_print( lua_State* L )
	{
		std::string line;
		const int count = lua_gettop( L );
		for ( int i = 1; i <= count; i++ ) {
			std::size_t length = 0;
			const char* text   = luaL_tolstring( L, i, &length );
			if ( i > 1 )
				line += '\t';
			line.append( text, length );
			lua_pop( L, 1 );
		}

		n_lua_api::console_msg( "[" + n_lua_api::script_of( L )->m_name + "] " + line );
		return 0;
	}

	/* ---- cmd ---- */

	c_user_cmd* check_cmd( lua_State* L )
	{
		luaL_checkudata( L, 1, k_cmd_meta );
		if ( !n_lua_api::s_cmd )
			luaL_error( L, "cmd is only valid inside its create_move / pre_prediction callback" );
		return n_lua_api::s_cmd;
	}

	int l_cmd_index( lua_State* L )
	{
		c_user_cmd* cmd = check_cmd( L );
		const std::string_view key = luaL_checkstring( L, 2 );

		if ( key == "command_number" ) lua_pushinteger( L, cmd->m_command_number );
		else if ( key == "tick_count" ) lua_pushinteger( L, cmd->m_tick_count );
		else if ( key == "pitch" ) lua_pushnumber( L, cmd->m_view_point.m_x );
		else if ( key == "yaw" ) lua_pushnumber( L, cmd->m_view_point.m_y );
		else if ( key == "roll" ) lua_pushnumber( L, cmd->m_view_point.m_z );
		else if ( key == "forwardmove" ) lua_pushnumber( L, cmd->m_forward_move );
		else if ( key == "sidemove" ) lua_pushnumber( L, cmd->m_side_move );
		else if ( key == "upmove" ) lua_pushnumber( L, cmd->m_up_move );
		else if ( key == "buttons" ) lua_pushinteger( L, cmd->m_buttons );
		else if ( key == "impulse" ) lua_pushinteger( L, cmd->m_impulse );
		else if ( key == "weaponselect" ) lua_pushinteger( L, cmd->m_weapon_select );
		else if ( key == "weaponsubtype" ) lua_pushinteger( L, cmd->m_weapon_sub_type );
		else if ( key == "random_seed" ) lua_pushinteger( L, cmd->m_random_seed );
		else if ( key == "mousedx" ) lua_pushinteger( L, cmd->m_mouse_delta_x );
		else if ( key == "mousedy" ) lua_pushinteger( L, cmd->m_mouse_delta_y );
		else if ( key == "has_been_predicted" ) lua_pushboolean( L, cmd->m_has_been_predicted );
		else if ( key == "send_packet" ) lua_pushboolean( L, n_lua_api::s_send_packet ? *n_lua_api::s_send_packet : true );
		else if ( key == "ptr" ) push_address( L, cmd );
		else lua_pushnil( L );
		return 1;
	}

	int l_cmd_newindex( lua_State* L )
	{
		c_user_cmd* cmd = check_cmd( L );
		const std::string_view key = luaL_checkstring( L, 2 );

		if ( key == "command_number" ) cmd->m_command_number = static_cast< int >( luaL_checkinteger( L, 3 ) );
		else if ( key == "tick_count" ) cmd->m_tick_count = static_cast< int >( luaL_checkinteger( L, 3 ) );
		else if ( key == "pitch" ) cmd->m_view_point.m_x = check_float( L, 3 );
		else if ( key == "yaw" ) cmd->m_view_point.m_y = check_float( L, 3 );
		else if ( key == "roll" ) cmd->m_view_point.m_z = check_float( L, 3 );
		else if ( key == "forwardmove" ) cmd->m_forward_move = check_float( L, 3 );
		else if ( key == "sidemove" ) cmd->m_side_move = check_float( L, 3 );
		else if ( key == "upmove" ) cmd->m_up_move = check_float( L, 3 );
		else if ( key == "buttons" ) cmd->m_buttons = static_cast< int >( luaL_checkinteger( L, 3 ) );
		else if ( key == "impulse" ) cmd->m_impulse = static_cast< unsigned char >( luaL_checkinteger( L, 3 ) );
		else if ( key == "weaponselect" ) cmd->m_weapon_select = static_cast< int >( luaL_checkinteger( L, 3 ) );
		else if ( key == "weaponsubtype" ) cmd->m_weapon_sub_type = static_cast< int >( luaL_checkinteger( L, 3 ) );
		else if ( key == "random_seed" ) cmd->m_random_seed = static_cast< int >( luaL_checkinteger( L, 3 ) );
		else if ( key == "mousedx" ) cmd->m_mouse_delta_x = static_cast< short >( luaL_checkinteger( L, 3 ) );
		else if ( key == "mousedy" ) cmd->m_mouse_delta_y = static_cast< short >( luaL_checkinteger( L, 3 ) );
		else if ( key == "send_packet" ) {
			if ( n_lua_api::s_send_packet )
				*n_lua_api::s_send_packet = lua_toboolean( L, 3 );
		} else
			return luaL_error( L, "cmd has no writable field '%s'", std::string( key ).c_str( ) );
		return 0;
	}

	/* ---- event ---- */

	game_event_t* check_event( lua_State* L )
	{
		luaL_checkudata( L, 1, k_event_meta );
		if ( !n_lua_api::s_event )
			luaL_error( L, "event is only valid inside its callback" );
		return n_lua_api::s_event;
	}

	int l_event_name( lua_State* L )
	{
		lua_pushstring( L, check_event( L )->get_name( ) );
		return 1;
	}

	int l_event_get_int( lua_State* L )
	{
		lua_pushinteger( L, check_event( L )->get_int( luaL_checkstring( L, 2 ), static_cast< int >( luaL_optinteger( L, 3, 0 ) ) ) );
		return 1;
	}

	int l_event_get_float( lua_State* L )
	{
		lua_pushnumber( L, check_event( L )->get_float( luaL_checkstring( L, 2 ), opt_float( L, 3, 0.f ) ) );
		return 1;
	}

	int l_event_get_bool( lua_State* L )
	{
		lua_pushboolean( L, check_event( L )->get_bool( luaL_checkstring( L, 2 ), lua_toboolean( L, 3 ) ) );
		return 1;
	}

	int l_event_get_string( lua_State* L )
	{
		const char* value = check_event( L )->get_string( luaL_checkstring( L, 2 ), luaL_optstring( L, 3, "" ) );
		lua_pushstring( L, value ? value : "" );
		return 1;
	}

	int l_event_get_uint64( lua_State* L )
	{
		lua_pushinteger( L, static_cast< lua_Integer >( check_event( L )->get_uint64( luaL_checkstring( L, 2 ), 0 ) ) );
		return 1;
	}

	/* event:get_player( "userid" ) -> entity of that user id */
	int l_event_get_player( lua_State* L )
	{
		const int user_id = check_event( L )->get_int( luaL_optstring( L, 2, "userid" ), 0 );
		const int index   = g_interfaces.m_engine_client->get_player_for_user_id( user_id );
		push_entity( L, index > 0 ? g_interfaces.m_client_entity_list->get< c_base_entity >( index ) : nullptr );
		return 1;
	}

	/* ---- registration ---- */

	void new_meta( lua_State* L, const char* name, const luaL_Reg* methods, const luaL_Reg* metamethods )
	{
		luaL_newmetatable( L, name );
		if ( metamethods )
			luaL_setfuncs( L, metamethods, 0 );
		if ( methods ) {
			lua_newtable( L );
			luaL_setfuncs( L, methods, 0 );
			lua_setfield( L, -2, "__index" );
		}
		lua_pushstring( L, "locked" );
		lua_setfield( L, -2, "__metatable" );
		lua_pop( L, 1 );
	}

	void add_constants( lua_State* L, const char* name, const std::pair< const char*, lua_Integer >* values, const std::size_t count )
	{
		lua_createtable( L, 0, static_cast< int >( count ) );
		for ( std::size_t i = 0; i < count; i++ ) {
			lua_pushinteger( L, values[ i ].second );
			lua_setfield( L, -2, values[ i ].first );
		}
		lua_setglobal( L, name );
	}
}

void n_lua_api::console_msg( const std::string& text )
{
	using msg_fn         = void( __cdecl* )( const char*, ... );
	static const auto fn = reinterpret_cast< msg_fn >( GetProcAddress( GetModuleHandleA( "tier0.dll" ), "Msg" ) );
	if ( fn )
		fn( "%s\n", text.c_str( ) );
}

int n_lua_api::traceback( lua_State* L )
{
	const char* message = lua_tostring( L, 1 );
	if ( !message )
		message = luaL_tolstring( L, 1, nullptr );

	luaL_traceback( L, L, message, 1 );
	return 1;
}

void n_lua_api::push_cmd( lua_State* L )
{
	lua_newuserdatauv( L, 1, 0 );
	luaL_setmetatable( L, k_cmd_meta );
}

void n_lua_api::push_event( lua_State* L )
{
	lua_newuserdatauv( L, 1, 0 );
	luaL_setmetatable( L, k_event_meta );
}

void n_lua_api::open( lua_State* L )
{
	lua_newtable( L );
	lua_setfield( L, LUA_REGISTRYINDEX, "botox.callbacks" );

	/* require( "name" ) -> C:\botox\lua\lib\name.lua ( lib is not scanned, so modules never show as scripts ) */
	if ( lua_getglobal( L, "package" ) == LUA_TTABLE ) {
		const std::string folder = g_lua.folder( ).string( );
		lua_pushstring( L, ( folder + "\\lib\\?.lua;" + folder + "\\lib\\?\\init.lua" ).c_str( ) );
		lua_setfield( L, -2, "path" );
	}
	lua_pop( L, 1 );

	lua_pushcfunction( L, l_print );
	lua_setglobal( L, "print" );

	lua_pushcfunction( L, l_color );
	lua_setglobal( L, "color" );

	static const luaL_Reg callbacks[ ] = { { "register", l_callbacks_register }, { "unregister", l_callbacks_unregister }, { nullptr, nullptr } };
	add_table( L, "callbacks", callbacks );

	static const luaL_Reg ui[ ] = { { "checkbox", l_ui_checkbox }, { "slider_int", l_ui_slider_int }, { "slider_float", l_ui_slider_float },
		                            { "combo", l_ui_combo },       { "color", l_ui_color },           { "keybind", l_ui_keybind },
		                            { "button", l_ui_button },     { "text", l_ui_text },             { "input", l_ui_input },
		                            { nullptr, nullptr } };
	add_table( L, "ui", ui );

	static const luaL_Reg element_methods[ ] = { { "get", l_element_get },
		                                         { "set", l_element_set },
		                                         { "visible", l_element_visible },
		                                         { "label", l_element_label },
		                                         { nullptr, nullptr } };
	new_meta( L, k_element_meta, element_methods, nullptr );

	static const luaL_Reg render[ ] = { { "screen_size", l_render_screen_size },
		                                { "text", l_render_text },
		                                { "text_size", l_render_text_size },
		                                { "line", l_render_line },
		                                { "rect", l_render_rect },
		                                { "rect_filled", l_render_rect_filled },
		                                { "gradient", l_render_gradient },
		                                { "circle", l_render_circle },
		                                { "circle_filled", l_render_circle_filled },
		                                { "triangle", l_render_triangle },
		                                { "poly", l_render_poly },
		                                { "world_to_screen", l_render_world_to_screen },
		                                { nullptr, nullptr } };
	add_table( L, "render", render );

	static const luaL_Reg entities[ ] = { { "local_player", l_entities_local },
		                                  { "get", l_entities_get },
		                                  { "from_handle", l_entities_from_handle },
		                                  { "highest_index", l_entities_highest_index },
		                                  { "players", l_entities_players },
		                                  { "by_class", l_entities_by_class },
		                                  { nullptr, nullptr } };
	add_table( L, "entities", entities );

	static const luaL_Reg entity_methods[ ] = { { "index", l_entity_index },
		                                        { "ptr", l_entity_ptr },
		                                        { "is_valid", l_entity_is_valid },
		                                        { "is_player", l_entity_is_player },
		                                        { "is_alive", l_entity_is_alive },
		                                        { "is_dormant", l_entity_is_dormant },
		                                        { "is_enemy", l_entity_is_enemy },
		                                        { "team", l_entity_team },
		                                        { "health", l_entity_health },
		                                        { "armor", l_entity_armor },
		                                        { "flags", l_entity_flags },
		                                        { "move_type", l_entity_move_type },
		                                        { "origin", l_entity_origin },
		                                        { "velocity", l_entity_velocity },
		                                        { "eye_position", l_entity_eye_position },
		                                        { "mins_maxs", l_entity_mins_maxs },
		                                        { "class_name", l_entity_class_name },
		                                        { "class_id", l_entity_class_id },
		                                        { "name", l_entity_name },
		                                        { "weapon", l_entity_weapon },
		                                        { "weapon_id", l_entity_weapon_id },
		                                        { "bone_position", l_entity_bone_position },
		                                        { "hitbox_position", l_entity_hitbox_position },
		                                        { "get_prop", l_entity_get_prop },
		                                        { "set_prop", l_entity_set_prop },
		                                        { "get_datafield", l_entity_get_datafield },
		                                        { "set_datafield", l_entity_set_datafield },
		                                        { nullptr, nullptr } };
	static const luaL_Reg entity_meta[ ]    = { { "__eq", l_entity_eq }, { "__tostring", l_entity_tostring }, { nullptr, nullptr } };
	new_meta( L, k_entity_meta, entity_methods, entity_meta );

	static const luaL_Reg engine[ ] = { { "exec", l_engine_exec },
		                                { "in_game", l_engine_in_game },
		                                { "connected", l_engine_connected },
		                                { "local_index", l_engine_local_index },
		                                { "max_clients", l_engine_max_clients },
		                                { "map_name", l_engine_map_name },
		                                { "level_name", l_engine_level_name },
		                                { "view_angles", l_engine_view_angles },
		                                { "set_view_angles", l_engine_set_view_angles },
		                                { "player_info", l_engine_player_info },
		                                { "player_for_user_id", l_engine_player_for_user_id },
		                                { "playing_demo", l_engine_playing_demo },
		                                { "latency", l_engine_latency },
		                                { nullptr, nullptr } };
	add_table( L, "engine", engine );

	static const luaL_Reg globals[ ] = { { "realtime", l_globals_realtime },         { "curtime", l_globals_curtime },
		                                 { "frametime", l_globals_frametime },       { "absframetime", l_globals_absframetime },
		                                 { "tickcount", l_globals_tickcount },       { "tickinterval", l_globals_tickinterval },
		                                 { "framecount", l_globals_framecount },     { "maxclients", l_globals_maxclients },
		                                 { nullptr, nullptr } };
	add_table( L, "globals", globals );

	static const luaL_Reg cvar[ ] = { { "exists", l_cvar_exists },         { "get_int", l_cvar_get_int }, { "get_float", l_cvar_get_float },
		                              { "get_string", l_cvar_get_string }, { "set", l_cvar_set },         { nullptr, nullptr } };
	add_table( L, "cvar", cvar );

	static const luaL_Reg config[ ] = { { "get", l_config_get },   { "set", l_config_set },   { "type", l_config_type },
		                                { "reset", l_config_reset }, { "list", l_config_list }, { nullptr, nullptr } };
	add_table( L, "config", config );

	static const luaL_Reg trace[ ] = { { "line", l_trace_line }, { "hull", l_trace_hull }, { "point_contents", l_trace_point_contents }, { nullptr, nullptr } };
	add_table( L, "trace", trace );

	static const luaL_Reg input[ ] = { { "is_key_down", l_input_is_key_down },
		                               { "mouse_pos", l_input_mouse_pos },
		                               { "menu_open", l_input_menu_open },
		                               { "keys_blocked", l_input_keys_blocked },
		                               { nullptr, nullptr } };
	add_table( L, "input", input );

	static const luaL_Reg mem[ ] = { { "module", l_mem_module },
		                             { "find_pattern", l_mem_find_pattern },
		                             { "interface", l_mem_interface },
		                             { "export", l_mem_export },
		                             { "interfaces", l_mem_interfaces },
		                             { "netvar", l_mem_netvar },
		                             { "read_i8", l_mem_read< std::int8_t > },
		                             { "read_u8", l_mem_read< std::uint8_t > },
		                             { "read_i16", l_mem_read< std::int16_t > },
		                             { "read_u16", l_mem_read< std::uint16_t > },
		                             { "read_i32", l_mem_read< std::int32_t > },
		                             { "read_u32", l_mem_read< std::uint32_t > },
		                             { "read_i64", l_mem_read< std::int64_t > },
		                             { "read_float", l_mem_read< float > },
		                             { "read_double", l_mem_read< double > },
		                             { "read_ptr", l_mem_read< word_t > },
		                             { "read_string", l_mem_read_string },
		                             { "read_bytes", l_mem_read_bytes },
		                             { "write_i8", l_mem_write< std::int8_t > },
		                             { "write_u8", l_mem_write< std::uint8_t > },
		                             { "write_i16", l_mem_write< std::int16_t > },
		                             { "write_u16", l_mem_write< std::uint16_t > },
		                             { "write_i32", l_mem_write< std::int32_t > },
		                             { "write_u32", l_mem_write< std::uint32_t > },
		                             { "write_i64", l_mem_write< std::int64_t > },
		                             { "write_float", l_mem_write< float > },
		                             { "write_double", l_mem_write< double > },
		                             { "write_ptr", l_mem_write< word_t > },
		                             { "write_string", l_mem_write_string },
		                             { "write_bytes", l_mem_write_bytes },
		                             { "vfunc", l_mem_vfunc },
		                             { "call", l_mem_call },
		                             { "alloc", l_mem_alloc },
		                             { "free", l_mem_free },
		                             { "protect", l_mem_protect },
		                             { "f2i", l_mem_f2i },
		                             { "i2f", l_mem_i2f },
		                             { nullptr, nullptr } };
	add_table( L, "mem", mem );

	static const luaL_Reg client[ ] = { { "log", l_client_log },       { "chat", l_client_chat },     { "name", l_client_name },
		                                { "folder", l_client_folder }, { "unload", l_client_unload }, { nullptr, nullptr } };
	add_table( L, "client", client );

	static const luaL_Reg cmd_meta[ ] = { { "__index", l_cmd_index }, { "__newindex", l_cmd_newindex }, { nullptr, nullptr } };
	new_meta( L, k_cmd_meta, nullptr, cmd_meta );

	static const luaL_Reg event_methods[ ] = { { "name", l_event_name },           { "get_int", l_event_get_int },
		                                       { "get_float", l_event_get_float }, { "get_bool", l_event_get_bool },
		                                       { "get_string", l_event_get_string }, { "get_uint64", l_event_get_uint64 },
		                                       { "get_player", l_event_get_player }, { nullptr, nullptr } };
	new_meta( L, k_event_meta, event_methods, nullptr );

	static const std::pair< const char*, lua_Integer > buttons[ ] = {
		{ "attack", in_attack },        { "jump", in_jump },         { "duck", in_duck },           { "forward", in_forward },
		{ "back", in_back },            { "use", in_use },           { "left", in_left },           { "right", in_right },
		{ "moveleft", in_moveleft },    { "moveright", in_moveright }, { "attack2", in_second_attack }, { "reload", in_reload },
		{ "score", in_score },          { "speed", in_speed },       { "walk", in_walk },           { "zoom", in_zoom },
		{ "bullrush", in_bullrush },    { "lookatweapon", in_use_or_reload },
	};
	add_constants( L, "buttons", buttons, std::size( buttons ) );

	static const std::pair< const char*, lua_Integer > flags[ ] = {
		{ "onground", fl_onground }, { "ducking", fl_ducking },   { "animducking", fl_animducking }, { "waterjump", fl_waterjump },
		{ "frozen", fl_frozen },     { "inwater", fl_inwater },   { "fakeclient", fl_fakeclient },   { "godmode", fl_godmode },
		{ "partialground", fl_partialground },
	};
	add_constants( L, "flags", flags, std::size( flags ) );

	static const std::pair< const char*, lua_Integer > stages[ ] = {
		{ "start", e_client_frame_stage::start },
		{ "net_update_start", e_client_frame_stage::net_update_start },
		{ "net_update_postdataupdate_start", e_client_frame_stage::net_update_postdataupdate_start },
		{ "net_update_postdataupdate_end", e_client_frame_stage::net_update_postdataupdate_end },
		{ "net_update_end", e_client_frame_stage::net_update_end },
		{ "render_start", e_client_frame_stage::render_start },
		{ "render_end", e_client_frame_stage::render_end },
	};
	add_constants( L, "stages", stages, std::size( stages ) );

	static const std::pair< const char*, lua_Integer > fonts[ ] = {
		{ "verdana", e_font_names::font_name_verdana_11 },       { "verdana_bold", e_font_names::font_name_verdana_bd_11 },
		{ "tahoma", e_font_names::font_name_tahoma_12 },         { "tahoma_bold", e_font_names::font_name_tahoma_bd_12 },
		{ "icons", e_font_names::font_name_icon_12 },            { "indicator", e_font_names::font_name_indicator_29 },
		{ "weapon_icons", e_font_names::font_name_cod4_weapon_icon_48 },
		{ "custom_indicator", 100 + e_custom_font_names::custom_font_name_indicator },
		{ "custom_esp", 100 + e_custom_font_names::custom_font_name_esp },
		{ "custom_net_graph", 100 + e_custom_font_names::custom_font_name_net_graph },
	};
	add_constants( L, "fonts", fonts, std::size( fonts ) );

	static const std::pair< const char*, lua_Integer > text_flags[ ] = {
		{ "none", e_text_flags::text_flag_none }, { "shadow", e_text_flags::text_flag_dropshadow }, { "outline", e_text_flags::text_flag_outline } };
	add_constants( L, "text_flags", text_flags, std::size( text_flags ) );

	static const std::pair< const char*, lua_Integer > masks[ ] = { { "all", e_mask::mask_all },
		                                                             { "solid", e_mask::mask_solid },
		                                                             { "playersolid", e_mask::mask_playersolid },
		                                                             { "shot", e_mask::mask_shot } };
	add_constants( L, "masks", masks, std::size( masks ) );
}
