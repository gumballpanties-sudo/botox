#pragma once
#include "../../../globals/macros/macros.h"
#include "c_vector.h"

class c_radar_player
{
public:
	c_vector pos;
	c_vector angle;
	c_vector spotted_map_angle_related;
	unsigned long tab_related;
	PAD( 0xC );
	float spotted_time;
	float spotted_fraction;
	float time;
	PAD( 0x4 );
	__int32 player_index;
	__int32 entity_index;
	PAD( 0x4 );
	__int32 health;
	char name[ 32 ];
	PAD( 0x75 );
	unsigned char spotted;
	PAD( 0x8A );
};

class c_csgo_hud_radar
{
public:
	PAD( 0x14C );
	c_radar_player radar_info[ 65 ];
};
