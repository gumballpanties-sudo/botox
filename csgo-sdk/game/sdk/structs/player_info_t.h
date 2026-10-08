#pragma once

struct player_info_t {
	unsigned long long m_ull_version = 0ull;
	union {
		unsigned long long m_ull_xuid;
		struct {
			unsigned int m_xuid_low;
			unsigned int m_xuid_high;
		};
	};

	char m_name[ 128 ];
	int m_user_id;
	char m_steam_id[ 33 ];
	unsigned int m_friends_id;
	char m_friends_name[ 128 ];
	bool m_fake_player;
	bool m_is_hltv;
	unsigned int m_custom_files[ 4 ];
	unsigned char m_files_downloaded;
};