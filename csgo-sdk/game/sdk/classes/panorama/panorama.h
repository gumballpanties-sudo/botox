#pragma once
#include "c_uipanel.h"

class c_uipanel;
class c_uievent;
class c_uiengineframelistener;
class c_uipanelstyle;
class c_uijsobject;
class c_uiwindow;
class c_uisettings;
class c_uitextlayout;
class c_uiinputengine;
class c_uilocalize;
class c_uisoundsystem;
class c_uisettings;
class c_uilayoutmanager;
class c_uifilesystem;
class c_uipanelclient;

class font_style_t;
class font_weight_t;
class text_align_t;
class panel_repaint_t;
class style_repaint_t;
class style_flags_t;
class focus_move_direction_t;
class mouse_can_activate_t;
class animation_timing_function;
class animation_direction;

class c_uilength;
class c_layoutfile;
typedef unsigned short c_panoramasymbol;
class panel_handle_t;
class c_jsonwebapiparams;
class ui_eventfactory;
class panel_event_source_t;
class c_panel2dfactory;
class register_js_type_t;
class register_js_scope_info_t;
class register_js_entry_info_t;

class panel_wrapper
{
public:
	void* vtable;
	c_uipanel* panel;
};

struct scroll_behavior_t;