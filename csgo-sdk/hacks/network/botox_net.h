#pragma once
#include "../../game/sdk/classes/c_vector.h"
#include "../skins/skins.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

class c_net_message;
class c_base_entity;

namespace n_botox_net
{
	constexpr std::uint16_t MAGIC   = 0xB07C;
	constexpr std::uint8_t VERSION  = 2;
	constexpr int PAYLOAD_BYTES     = 16;
	constexpr int STICKERS          = 5;
	constexpr int NAME_MAX          = 40;
	constexpr int NAME_CHUNK        = 11;
	constexpr float ESP_TIMEOUT     = 1.5f;
	constexpr float USER_TIMEOUT    = 5.f;
	constexpr float SKIN_TIMEOUT    = 15.f;
	constexpr float ITEM_PATIENCE   = 4.f;

	enum e_packet_type : std::uint8_t {
		packet_hello   = 1,
		packet_esp     = 2,
		packet_skin    = 3,
		packet_extra   = 4,
		packet_sticker = 5,
		packet_name    = 6,
		packet_glove   = 7,
		packet_last    = packet_glove,
	};

	enum e_hello_flags : std::uint8_t {
		hello_esp   = 1 << 0,
		hello_skins = 1 << 1,
	};

#pragma pack( push, 1 )
	/* rides the 5 u32 a server copies verbatim: xuid lo/hi, sequence_bytes, section_number, uncompressed_sample_offset */
	struct packet_t {
		std::uint16_t m_magic;
		std::uint8_t m_type;
		std::uint8_t m_version;
		std::uint8_t m_data[ PAYLOAD_BYTES ];
	};

	struct esp_data_t {
		std::uint8_t m_index;
		std::uint8_t m_health;
		float m_origin[ 3 ];
	};

	struct skin_data_t {
		std::uint32_t m_handle;
		std::int16_t m_definition;
		std::int16_t m_paint_kit;
		std::int16_t m_seed;
		float m_wear;
	};

	struct extra_data_t {
		std::uint32_t m_handle;
		std::int32_t m_stattrak;
		std::uint8_t m_quality;
		std::uint8_t m_name_length;
		std::uint8_t m_sticker_mask;
	};

	struct sticker_data_t {
		std::uint32_t m_handle;
		std::uint8_t m_slot;
		std::int16_t m_kit;
		std::int16_t m_scale_milli;
		std::int16_t m_rotation_deci;
		float m_wear;
	};

	struct name_data_t {
		std::uint32_t m_handle;
		std::uint8_t m_chunk;
		char m_text[ NAME_CHUNK ];
	};

	struct glove_data_t {
		std::uint8_t m_model;
		std::int16_t m_paint_kit;
		std::int16_t m_seed;
		float m_wear;
	};
#pragma pack( pop )

	static_assert( sizeof( packet_t ) == 20 );
	static_assert( offsetof( packet_t, m_data ) == 4 );
	static_assert( sizeof( esp_data_t ) <= PAYLOAD_BYTES );
	static_assert( sizeof( skin_data_t ) <= PAYLOAD_BYTES );
	static_assert( sizeof( extra_data_t ) <= PAYLOAD_BYTES );
	static_assert( sizeof( sticker_data_t ) <= PAYLOAD_BYTES );
	static_assert( sizeof( name_data_t ) <= PAYLOAD_BYTES );
	static_assert( sizeof( glove_data_t ) <= PAYLOAD_BYTES );
	static_assert( NAME_CHUNK * 4 >= NAME_MAX );

	struct impl_t {
		void on_create_move( );
		void on_frame_stage_notify( int stage );
		void on_level_init( );

		/* CClientState::SVCMsg_VoiceData. true = swallow (ours, or real audio the user muted) */
		bool on_voice_data( const void* message );

		/* CNetChan::SendNetMsg net_StringCmd: keeps the server relaying everyone, mutes go local */
		void on_string_cmd( c_net_message* message );

		/* CNetChan::SendNetMsg clc_VoiceData: real mic audio pauses our packets so voice chat keeps the bandwidth */
		void on_voice_send( c_net_message* message );

		bool shared_player( int index, c_vector& origin, int& health ) const;

		/* KNIFE_MODELS slot a botox user's knife shows as, 0 = none. read by the CBaseViewModel recv proxies */
		int knife_slot_of( int player_index ) const;

		std::atomic< int > m_users{ 0 };

	private:
		bool active( ) const;
		bool skins_on( ) const;
		bool send( const packet_t& packet );
		void receive( int sender, const packet_t& packet );
		void send_esp( );
		void send_skins( );
		void queue_skins( );
		void apply_items( );
		void apply_gloves( );
		void apply_spectate( );
		void release_glove( int index );
		void release_gloves( );
		void force_server_masks( );

		struct shared_t {
			c_vector m_origin = { };
			int m_health      = 0;
			float m_time      = -1.f;
		} m_shared[ 65 ]{ };

		struct user_t {
			float m_time         = -1.f;
			std::uint8_t m_flags = 0;
			int m_knife_slot     = 0;
			float m_knife_time   = -1.f;
		} m_user_seen[ 65 ]{ };

		struct item_t {
			int m_sender                 = 0;
			bool m_has_skin              = false;
			bool m_has_extra             = false;
			short m_definition           = 0;
			short m_paint_kit            = 0;
			short m_seed                 = 0;
			float m_wear                 = 0.f;
			int m_stattrak               = -1;
			int m_quality                = 4;
			std::uint8_t m_sticker_mask  = 0;
			std::uint8_t m_sticker_have  = 0;
			std::uint8_t m_name_length   = 0;
			std::uint8_t m_name_have     = 0;
			n_skins::sticker_t m_stickers[ STICKERS ]{ };
			char m_name[ NAME_MAX + 1 ]{ };
			float m_time                 = -1.f;
			float m_first                = -1.f;
			std::uint32_t m_applied      = 0;
			c_base_entity* m_applied_to  = nullptr;
		};
		std::unordered_map< std::uint32_t, item_t > m_items{ };

		struct glove_t {
			std::uint8_t m_model    = 0;
			short m_paint_kit       = 0;
			short m_seed            = 0;
			float m_wear            = 0.f;
			float m_time            = -1.f;
			unsigned int m_handle   = 0;
			std::uint32_t m_applied = 0;
			float m_settle_at       = 0.f;
		} m_gloves[ 65 ]{ };
		bool m_gloves_live = false;

		int m_spec_target          = 0;
		unsigned int m_spec_weapon = 0;
		float m_spec_rebuild_at[ 2 ] = { };

		std::vector< packet_t > m_queue{ };
		std::size_t m_queue_cursor = 0;

		/* client resends vban every ~1s while its mask != the server's (always 0 after our rewrite), silent = no mutes */
		std::uint32_t m_ban_mask[ 2 ] = { };
		float m_ban_time              = -1.f;
		bool m_masks_forced           = false;

		float m_mic_time   = -1.f;
		float m_last_hello = -1.f;
		float m_last_skin  = -1.f;
		int m_esp_cursor   = 1;
	};
}

inline n_botox_net::impl_t g_botox_net{ };
