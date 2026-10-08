#pragma once
#include "../../../utilities/memory/virtual.h"

class c_surface
{
public:
	void unlock_cursor() {
		g_virtual.call< void >( this, 66 );
	}

	void play_sound( const char* sound_path ) {
		g_virtual.call< void >( this, 82, sound_path );
	}
};