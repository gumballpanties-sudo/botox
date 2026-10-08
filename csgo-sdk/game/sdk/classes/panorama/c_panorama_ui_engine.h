#pragma once
#include "../c_app_system.h"
#include "c_uipanel.h"

class c_utl_abstract_delegate;

// IUIImageManager = CImageResourceManager primary base (panorama.dll RTTI, one vtable, this-off 0)
class c_uiimagemanager
{
public:
	virtual void* load_image_from_url( )           = 0; // 0
	virtual void* load_image_file_from_memory( )   = 0;
	virtual void* load_image_from_memory( )        = 0;
	virtual void* load_image_from_engine_rt( )     = 0;
	virtual void get_pch_image_source_path( )      = 0;
	virtual void reload_changed_image( void* src ) = 0; // 5
	/* 6, rva 0x9e3f0: every cached url whose path strcmp's equal is decoded again (async, through the
	   set_image_data detour). path = cache key = "materials\panorama\images\...\x.vsvg", same as that detour gets */
	virtual void reload_changed_file( const char* path ) = 0;
};

class c_ui_engine
{
public:
	virtual void destroy( )                                                = 0; // 0
	virtual void startup_subsystems( c_uisettings*, void* PlatWindow_t__ ) = 0;
	virtual void con_command_init( void* IConCommandBaseAccessor )         = 0;
	virtual void shutdown( void )                                          = 0;
	virtual void request_shutdown( void )                                  = 0;
	virtual void run( void )                                               = 0; // 5
	virtual void run_frame( void )                                         = 0;
	virtual void set_aggressive_frame_limit( bool )                        = 0;
	virtual bool is_running( void )                                        = 0;
	virtual bool has_focus( void )                                         = 0;
	virtual double get_current_frametime( void )                           = 0; // 10
	virtual void unk0( )                                                   = 0; // getter
	virtual void unk1( )                                                   = 0; // setter ^
	virtual void int3_wrapper( )                                           = 0;
	virtual void int3_wrapper2( )                                          = 0;
	virtual void int3_wrapper3( )                                          = 0; // 15
	virtual void int3_wrapper4( )                                          = 0;
	virtual void int3_wrapper5( )                                          = 0;
	virtual void create_text_layout( char const* text, char const*, float, float, font_weight_t, font_style_t, text_align_t, bool, bool, int, float,
	                                 float )                               = 0;
	virtual void create_wtext_layout( wchar_t const* text, char const*, float, float, font_weight_t, font_style_t, text_align_t, bool, bool, int,
	                                  float, float )                       = 0;
	virtual void free_text_layout( c_uitextlayout* )                       = 0; // 20
	virtual void get_some_font_thing( void )                               = 0; // similar to function below
	virtual void get_sorted_valid_font_names( void )                       = 0;
	virtual c_uiinputengine* ui_input_engine( void )                       = 0;
	virtual c_uilocalize* ui_localize( void )                              = 0;
	virtual c_uisoundsystem* ui_sound_system( void )                       = 0; // 25
	virtual c_uisettings* ui_settings( void )                              = 0;
	virtual c_uilayoutmanager* ui_layout_manager( void )                   = 0;
	virtual c_uifilesystem* ui_filesystem( void )                          = 0;
	virtual c_uiimagemanager* ui_image_manager( void )                     = 0; // 29, mov eax,[ecx+0x868] (binary-checked)
	virtual void reload_layout_file( c_panoramasymbol )                    = 0; // 30
	virtual void toggle_debug_mode( void )                                 = 0;
	virtual const char* get_console_history( void )                        = 0;
	virtual panel_wrapper* create_panel( void )                            = 0;
	virtual void panel_destroyed( c_uipanel*, c_uipanel* )                 = 0;
	virtual bool is_valid_panel_ptr( c_uipanel const* panel )              = 0; // 35
	virtual panel_handle_t* get_panel_handle( c_uipanel const* panel )     = 0;
	virtual c_uipanel* get_panel_ptr( panel_handle_t const& panel_handle ) = 0;
	virtual void call_before_style_and_layout( c_uipanel* )                = 0;
	virtual void register_for_panel_destroyed( )                           = 0;
	virtual void unregister_for_panel_destroyed( )                         = 0; // CUtlDelegate<void ()(c_uipanel const*,c_uipanel*)>) = 0; // 40
	virtual void register_event_handler( c_panoramasymbol, c_uipanel*, /*CUtlAbstractDelegate*/ int )                        = 0;
	virtual void unregister_event_handler( c_panoramasymbol, c_uipanel*, /*CUtlAbstractDelegate*/ int )                      = 0;
	virtual void register_event_handler( c_panoramasymbol, c_uipanelclient*, /*CUtlAbstractDelegate*/ int )                  = 0;
	virtual void unregister_event_handler( c_panoramasymbol, c_uipanelclient*, /*CUtlAbstractDelegate*/ int )                = 0;
	virtual void unregister_event_handlers_for_panel( c_uipanel* )                                                           = 0; // 45
	virtual void register_for_unhandled_event( c_panoramasymbol, /*CUtlAbstractDelegate*/ int )                              = 0;
	virtual void unregister_for_unhandled_event( c_panoramasymbol, /*CUtlAbstractDelegate*/ int )                            = 0;
	virtual void unregister_for_unhandled_events( void* )                                                                    = 0;
	virtual bool have_event_handlers_registered_for_type( c_panoramasymbol type )                                            = 0;
	virtual void register_panel_type_event_handler( c_panoramasymbol, c_panoramasymbol, /*CUtlAbstractDelegate*/ int, bool ) = 0; // 50
	virtual void dispatch_event( c_uievent* )                                                                                = 0;
	virtual void dispatch_event_async( float, c_uievent* )                                                                   = 0;
	virtual bool any_handler_registered_for_event( c_panoramasymbol const& )                                                 = 0;
	virtual c_panoramasymbol* get_last_dispatched_event_symbol( void )                                                       = 0;
	virtual c_uipanel*
	get_last_dispatched_event_target_panel( void ) = 0; // This function can fail sometimes and you need to check the result/call it later (YUCK!) 55
	virtual void register_event_filter( /*CUtlAbstractDelegate*/ int )                          = 0;
	virtual void unregister_event_filter( /*CUtlAbstractDelegate*/ int ) = 0;
	virtual void layout_and_paint_windows( void )                                               = 0;
	virtual const char* get_application_install_path( void )                                    = 0;
	virtual const char* get_application_user_data_path( void )                                  = 0; // 60
	virtual void register_named_local_path( char const*, char const*, bool, bool )              = 0;
	virtual void register_named_user_path( char const*, char const*, bool, bool )               = 0;
	virtual void register_custom_font_path( char const* path )                                  = 0;
	virtual void get_local_path_for_named_path( char const* )                                   = 0;
	virtual void get_local_path_for_relative_path( char const*, char const*, void* CUtlString ) = 0; // 65
	virtual void register_named_remote_host( char const*, char const* )                         = 0;
	virtual void get_remote_host_for_named_host( char const* )                                  = 0;
	virtual void register_x_header( char const*, char const* )                                  = 0;
	virtual int get_x_header_count( void )                                                      = 0;
	virtual void get_x_header( int, void* CUtlString, void* CUtlString2 )                       = 0; // 70
	virtual void set_cookie_header_for_named_remote_host( ) = 0;
	virtual void set_cookie_header_for_remote_host( )       = 0;
	virtual void get_cookie_headers_for_named_remote_host( char const* )                          = 0;
	virtual void get_cookie_headers_for_remote_host( char const* )                                = 0;
	virtual void get_cookie_value_for_remote_host( char const*, char const*, void* CUtlString )   = 0; // 75
	virtual void b_set_cookie_for_web_requests( char const*, char const*, char const* )           = 0;
	virtual void b_clear_cookie_for_web_requests( char const*, char const*, char const* )         = 0;
	virtual void* access_html_controller( void )                                                  = 0;
	virtual void unk( )                                                                           = 0; // _SDL_ShowSimpleMessageBox with yes/no
	virtual void add_frame_listener( c_uiengineframelistener* )                                   = 0; // 80
	virtual void remove_frame_listener( c_uiengineframelistener* )                                = 0;
	virtual void register_mouse_can_activate_parent( c_uipanel*, char const* )                    = 0;
	virtual void unregister_mouse_can_activate_parent( c_uipanel* )                               = 0;
	virtual void get_mouse_can_activate_parent( c_uipanel* )                                      = 0;
	virtual bool any_window_has_focus( void )                                                     = 0; // 85
	virtual bool any_visible_window_has_focus( void )                                             = 0;
	virtual bool any_overlay_window_has_focus( void )                                             = 0;
	virtual c_uiwindow* get_focused_window( void )                                                = 0;
	virtual float get_last_input_time( void )                                                     = 0;
	virtual float update_last_input_time( void )                                                  = 0; // 90
	virtual void clear_clipboard( void )                                                          = 0;
	virtual void copy_to_clipboard( char const*, char const* )                                    = 0;
	virtual void get_clipboard_text( void* CUtlString, void* CUtlString2 )                        = 0;
	virtual const char* get_display_language( void )                                              = 0;
	virtual void unk2( )                                                                          = 0; // 95
	virtual void unk3( )                                                                          = 0;
	virtual void unk4( )                                                                          = 0;
	virtual bool has_overlay_for_app( unsigned long long, unsigned long long )                    = 0;
	virtual void track_overlay_for_app( unsigned long long, unsigned long long, void* )           = 0;
	virtual void delete_overlay_instance_for_app( unsigned long long, unsigned long long, void* ) = 0; // 100
	virtual void overlay_for_app( unsigned long long, unsigned long long )                        = 0;
	virtual c_uipanelstyle* alloc_panel_style( c_uipanel*, float )                                = 0;
	virtual void free_panel_style( c_uipanelstyle* )                                              = 0;
	virtual void set_panel_waiting_async_delete( c_uipanel* )                                     = 0;
	virtual bool is_panel_waiting_async_delete( c_uipanel* )                                      = 0; // 105
	virtual void pulse_active_controller_haptic( )      = 0;
	virtual void something_controller_haptic_related( ) = 0;
	virtual void mark_layer_to_repaint_thread_safe( unsigned long long )                        = 0;
	virtual void add_directory_change_watch( char const* )                                      = 0;
	virtual int get_wheel_scroll_lines( void )                                                  = 0; // default 3 // 110
	virtual int run_script( c_uipanel* panel, char const* entireJSCode, char const* pathToXMLContext, int nSourceBeginLine, int nSourceBeginCol,
	                        bool alreadyCompiled, bool bPrintRetValue )                         = 0;
	virtual void unk5( )                                                                        = 0;
	virtual void expose_object_type_to_javascript( char const*, /*CUtlAbstractDelegate*/ int& ) = 0;
	virtual bool is_object_type_exposed_to_javascript( char const* )                            = 0;
	virtual void expose_global_object_to_javascript( char const*, void*, char const*, bool )    = 0; // 115
	virtual void clear_global_object_for_javascript( char const*, void* )                       = 0;
	virtual void delete_js_object_instance( c_uijsobject* )                                     = 0;
	virtual void* get_javascript_context_for_panel( c_uipanel* )                                = 0;
	virtual c_uipanel* get_panel_for_javascript_context( )                                      = 0;
	virtual void output_js_exception_to_console( )                                              = 0; // v8::TryCatch &,c_uipanel *) = 0; // 120
	virtual void add_global_v8_function_template( )                             = 0;
	virtual void* get_v8_global_context( void )                                 = 0;
	virtual void get_current_v8_object_template_to_setup( void )                = 0; // gets offset.
	virtual void get_current_v8_object_template_to_setup2( void )               = 0; // gets offset and calls some function.
	virtual void grabber3( void )                                               = 0; // returns offset in class // 125
	virtual void grabber4( void )                                               = 0; // returns offset in class.
	virtual void ui_style_factory( void )                                       = 0;
	virtual void get_v8_isolate( void )                                         = 0;
	virtual void run_function( )                                                = 0;
	virtual void run_function2( )                                               = 0; // different args // 130
	virtual void create_v8_panel_instance( c_uipanel* )                         = 0;
	virtual void create_v8_panel_style_instance( c_uipanelstyle* )              = 0;
	virtual void create_v8_object_instance( char const*, void*, c_uijsobject* ) = 0;
	virtual void _unk( )                                                        = 0;
	virtual void initiate_async_json_web_api_request( ) = 0;
	virtual void initiate_async_json_web_api_request( int overloadMeme ) = 0; // EHTTPMethod,CUtlString,CUtlDelegate<void ()(unsigned long
	virtual void cancel_async_json_web_api_request( unsigned int )                                         = 0;
	virtual void resolve_path( char const* )                                                               = 0;
	virtual void register_event_with_engine( c_panoramasymbol, ui_eventfactory )                           = 0;
	virtual bool is_valid_event_name( c_panoramasymbol event_symbol )                                      = 0;
	virtual bool is_valid_panel_event( c_panoramasymbol, int* )                                            = 0;
	virtual void create_input_event_from_symbol( c_panoramasymbol, c_uipanel*, panel_event_source_t, int ) = 0;
	virtual void create_event_from_string( c_uipanel*, char const*, char const** )                         = 0;
	virtual void unk11( )                                                                                  = 0;
	virtual void register_panel_factory_with_engine( c_panoramasymbol, c_panel2dfactory* )                 = 0;
	virtual bool registered_panel_type( c_panoramasymbol )                                                 = 0;
	virtual panel_wrapper* create_panel( c_panoramasymbol* panelType, char const* id,
	                                     c_uipanel* createInThisParent ) = 0; // paneltype (ex: Panel, Label), id (ex: <Label id="swag">)
	virtual void create_debugger_window( void )                          = 0;
	virtual void close_debugger_window( void )                           = 0;
	virtual void register_scheduled_delegate( )                          = 0;
	virtual void cancel_scheduled_delegate( int )                        = 0;
	virtual float get_last_scheduled_delegate_run_time( void )           = 0;
	virtual c_panoramasymbol make_symbol( char const* )                  = 0; // if the symbol already exists, you'll get that one.
	virtual const char* resolve_symbol( c_panoramasymbol symbol )        = 0; // if you pass an invalid number into here, it can crash the program.
	virtual void queue_decrement_ref_next_frame( )                       = 0;
	virtual void register_js_generic_callback( )                         = 0;
	virtual void invoke_js_generic_callback( )                           = 0;
	virtual void unregister_js_generic_callback( int )                   = 0;
	virtual int get_num_register_js_scopes( void )                       = 0;
	virtual void get_register_js_scope_info( int, register_js_scope_info_t* )                        = 0;
	virtual void get_register_js_entry_info( int, int, register_js_entry_info_t* )                   = 0;
	virtual void start_register_js_scope( char const*, char const* )                                 = 0;
	virtual void end_register_js_scope( void )                                                       = 0;
	virtual void new_register_js_entry( char const*, unsigned int, char const*, register_js_type_t ) = 0;
	virtual void set_register_js_entry_params( int, unsigned char, register_js_type_t* )             = 0;
	virtual void _padding( )                                                                         = 0;
	virtual void clear_file_cache( void )                                                            = 0;
	virtual void print_cache_status( void )                                                          = 0;
	virtual void get_windows_for_debugger( )              = 0;
	virtual void set_paint_count_tracking_enabled( bool ) = 0;
	virtual bool get_paint_count_tracking_enabled( void ) = 0;
	virtual void increment_paint_count_for_panel( unsigned long long, bool, double )       = 0;
	virtual void get_panel_paint_info( unsigned long long, unsigned int&, bool&, double& ) = 0;
	virtual bool has_any_windows( void )                                                   = 0;
	virtual void text_entry_focus_change( c_uipanel* )                                     = 0;
	virtual void text_entry_invalid( c_uipanel* )                                          = 0;
	virtual void set_clipboard_text( const char* text )                                    = 0; // wrapper for _SDL_SetClipboardText
	virtual const char* get_clipboard_text( )                                              = 0; // wrapper for _SDL_GetClipboardText
	virtual void on_file_cache_removed( c_panoramasymbol )                                 = 0;
	virtual void run_platform_frame( void )                                                = 0;
	virtual void create_sound_system( void )                                               = 0;
};

class c_panorama_ui_engine : c_app_system
{
public:
	virtual void setup_ui_engine( void )          = 0;
	virtual void shutdown_ui_engine( void )       = 0;
	virtual c_ui_engine* access_ui_engine( void ) = 0;
	virtual void handle_input_event( )            = 0;
	virtual void unk1( )                          = 0;
	virtual void unk2( )                          = 0;
};
