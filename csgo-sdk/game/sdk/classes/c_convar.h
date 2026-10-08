#pragma once
#include "../../../utilities/memory/virtual.h"
#include "../../datatypes/fn_change_callback.h"
#include "c_app_system.h"

using cvar_dll_identifier_t = int;
class c_con_base;
class c_cconvar;
class c_con_cmd;
class c_color;
struct c_unsigned_char_color;
class c_cvar_query;

class c_convar : public c_app_system
{
public:
	virtual cvar_dll_identifier_t allocate_dll_identifier( )                                               = 0;
	virtual void register_con_command( c_cconvar* command_base, int default_value = 1 )                    = 0;
	virtual void unregister_con_command( c_cconvar* command_base )                                         = 0;
	virtual void unregister_con_commands( cvar_dll_identifier_t id )                                       = 0;
	virtual const char* get_command_line_value( const char* variable_name )                                = 0;
	virtual c_con_base* find_command_base( const char* name )                                              = 0;
	virtual const c_con_base* find_command_base( const char* name ) const                                  = 0;
	virtual c_cconvar* find_var( const char* variable_name )                                               = 0;
	virtual const c_cconvar* find_var( const char* variable_name ) const                                   = 0;
	virtual c_con_cmd* find_command( const char* name )                                                    = 0;
	virtual const c_con_cmd* find_command( const char* name ) const                                        = 0;
	virtual void install_global_change_callback( fn_change_callback_t callback )                           = 0;
	virtual void remove_global_change_callback( fn_change_callback_t callback )                            = 0;
	virtual void call_global_change_callbacks( c_cconvar* var, const char* old_string, float old_value )   = 0;
	virtual void install_console_display_func( void* display_func )                                        = 0;
	virtual void remove_console_display_func( void* display_func )                                         = 0;
	virtual void console_color_printf( const c_unsigned_char_color& color, const char* format, ... ) const = 0;
	virtual void console_printf( const char* format, ... ) const                                           = 0;
	virtual void console_d_printf( const char* format, ... ) const                                         = 0;
	virtual void revert_flagged_con_vars( int flag )                                                       = 0;
};
