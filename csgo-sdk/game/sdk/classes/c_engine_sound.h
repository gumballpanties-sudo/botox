#pragma once
#include "../../../utilities/memory/virtual.h"
#include "c_utl_vector.h"
#include "c_vector.h"

struct sound_info_t {
	int m_uid;
	void* m_name_handle;
	int m_sound_source;
	int m_channel;
	int m_speaker_ent;
	float m_volume;
	float m_last_spatialized_volume;
	float m_radius;
	int m_pitch;
	c_vector* m_origin;
	c_vector* m_direction;
	bool m_update_positions;
	bool m_is_sentence;
	bool m_dry_mix;
	bool m_speaker;
	bool m_from_server;
};

class c_engine_sound
{
public:
	void get_active_sounds( c_utl_vector< sound_info_t >& sound_list )
	{
		g_virtual.call< void*, c_utl_vector< sound_info_t >& >( this, 19, sound_list );
	}

	void emit_ambient_sound( const char* sample, float volume, int pitch = 100 )
	{
		g_virtual.call< void, const char*, float, int, int, float >( this, 12, sample, volume, pitch, 0, 0.f );
	}
};
