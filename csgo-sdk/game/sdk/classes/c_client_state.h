#pragma once
#include "../../../globals/macros/macros.h"
#include "../classes/c_angle.h"

class bf_write;
class c_net_channel
{
public:
	PAD( 0x14 );                // 0x0000
	bool m_processing_messages; // 0x0014
	bool m_should_delete;       // 0x0015
	bool m_stop_processing;     // 0x0016
	PAD( 0x1 );                 // 0x0017
	int m_out_sequence_nr;      // 0x0018 last send outgoing sequence number
	int m_in_sequence_nr;       // 0x001C last received incoming sequence number
	int m_out_sequence_nr_ack;  // 0x0020 last received acknowledge outgoing sequence number
	int m_out_reliable_state;   // 0x0024 state of outgoing reliable data (0/1) flip flop used for loss detection
	int m_in_reliable_state;    // 0x0028 state of incoming reliable data
	int m_choked_packets;       // 0x002C number of choked packets
	PAD( 0x414 );               // 0x0030

	int send_datagram( bf_write* datagram )
	{
		return g_virtual.call< int >( this, 46, datagram );
	}

	bool transmit( bool only_reliable = false )
	{
		return g_virtual.call< bool >( this, 49, only_reliable );
	}
};
class c_event_info
{
public:
	enum {
		EVENT_INDEX_BITS    = 8,
		EVENT_DATA_LEN_BITS = 11,
		MAX_EVENT_DATA      = 192,
	};

	short m_class_id;
	PAD( 0x2 );
	float m_fire_delay;
	const void* m_send_table;
	const void* m_client_class;
	int m_packed;
	int m_flags;
	int m_filter[ 8 ];
	c_event_info* m_next;
};

class c_client_state
{
public:
	unsigned char pad0[ 0x9c ];
	c_net_channel* m_net_channel;
	int m_challenge_nr;
	unsigned char pad1[ 0x64 ];
	int m_signon_state;
	unsigned char pad2[ 0x8 ];
	float m_next_cmd_time;
	int m_server_count;
	int m_current_sequence;
	unsigned char pad3[ 0x54 ];
	int m_delta_tick;
	bool m_paused;
	unsigned char pad4[ 0x7 ];
	int m_view_entity;
	int m_player_slot;
	char m_level_name[ 260 /* MAX_PATH */ ];
	char m_level_name_short[ 80 ];
	char m_map_group_name[ 80 ];
	char m_last_level_name_short[ 80 ];
	unsigned char pad5[ 0xc ];
	int m_max_clients;
	unsigned char pad6[ 0x498c ];
	float m_last_server_tick_time;
	bool m_in_simulation;
	unsigned char pad7[ 0x3 ];
	int m_old_tickcount;
	float m_tick_remainder;
	float m_frame_time;
	int m_last_outgoing_command;
	int m_choked_commands;
	int m_last_command_ack;
	int m_command_ack;
	int m_sound_sequence;
	unsigned char pad8[ 0x50 ];
	c_angle m_view_point;
	unsigned char pad9[ 0xd0 ];
	c_event_info* m_events;
};
