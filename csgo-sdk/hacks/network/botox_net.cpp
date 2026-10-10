#include "botox_net.h"

#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../skins/skins_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
	/* CNetMessagePB<10, CCLCMsg_VoiceData> (engine 0x1019F550): protobuf at +0x8, 0x54 bytes */
	constexpr std::size_t CLC_DATA           = 0x10;
	constexpr std::size_t CLC_XUID           = 0x18;
	constexpr std::size_t CLC_FORMAT         = 0x20;
	constexpr std::size_t CLC_SEQUENCE_BYTES = 0x24;
	constexpr std::size_t CLC_SECTION        = 0x28;
	constexpr std::size_t CLC_SAMPLE_OFFSET  = 0x2C;
	constexpr std::size_t CLC_HAS_BITS       = 0x34;

	/* CSVCMsg_VoiceData protobuf (ctor engine 0x1018ED70) */
	constexpr std::size_t SVC_CLIENT         = 0x08;
	constexpr std::size_t SVC_XUID           = 0x10;
	constexpr std::size_t SVC_VOICE_DATA     = 0x18;
	constexpr std::size_t SVC_FORMAT         = 0x20;
	constexpr std::size_t SVC_SEQUENCE_BYTES = 0x24;
	constexpr std::size_t SVC_SECTION        = 0x28;
	constexpr std::size_t SVC_SAMPLE_OFFSET  = 0x2C;

	/* CNetMessagePB<5, CNETMsg_StringCmd>: protobuf at +0x4, command_ std::string* at protobuf +0x8 */
	constexpr std::size_t STRING_CMD_COMMAND = 0x0C;

	constexpr int VOICE_FORMAT_STEAM = 0;
	constexpr std::size_t MAX_ITEMS  = 512;

	static_assert( n_botox_net::STICKERS == STICKER_SLOTS );
	static_assert( n_botox_net::NAME_MAX == CUSTOM_NAME_MAX );

	struct msvc_string_t {
		union {
			char m_buffer[ 16 ];
			char* m_pointer;
		};
		std::size_t m_size;
		std::size_t m_capacity;

		char* data( ) { return m_capacity >= 16 ? m_pointer : m_buffer; }
	};
	static_assert( sizeof( msvc_string_t ) == 0x18 );

	template< class t >
	t& field( std::uintptr_t base, std::size_t offset )
	{
		return *reinterpret_cast< t* >( base + offset );
	}

	float now( )
	{
		return g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_real_time : 0.f;
	}

	bool fresh( float stamp, float timeout )
	{
		const float age = now( ) - stamp;
		return stamp >= 0.f && age >= 0.f && age < timeout;
	}

	n_botox_net::packet_t make_packet( n_botox_net::e_packet_type type, const void* data, std::size_t size )
	{
		n_botox_net::packet_t packet{ };
		packet.m_magic   = n_botox_net::MAGIC;
		packet.m_type    = type;
		packet.m_version = n_botox_net::VERSION;
		std::memcpy( packet.m_data, data, ( std::min )( size, sizeof( packet.m_data ) ) );
		return packet;
	}

	bool is_default_knife( short definition )
	{
		return definition == weapon_knife || definition == weapon_knife_t || definition == weapon_knife_gg;
	}

	int knife_slot( short definition )
	{
		for ( int i = 1; i < KNIFE_COUNT; i++ ) {
			if ( KNIFE_IDS[ i ] == definition )
				return i;
		}
		return 0;
	}

	struct hash_t {
		std::uint32_t m_value = 2166136261u;

		void add( const void* data, std::size_t size )
		{
			const auto bytes = static_cast< const std::uint8_t* >( data );
			for ( std::size_t i = 0; i < size; i++ ) {
				m_value ^= bytes[ i ];
				m_value *= 16777619u;
			}
		}

		template< class t >
		void add( const t& value )
		{
			add( &value, sizeof( value ) );
		}
	};

	/* nested prop: unresolved netvar = offset 0 = entity base */
	const char* custom_name_of( c_base_entity* item )
	{
		const char* name = item->get_custom_name( );
		return name && reinterpret_cast< const void* >( name ) != static_cast< const void* >( item ) ? name : nullptr;
	}

	int account_id_of( int index )
	{
		player_info_t info{ };
		return g_interfaces.m_engine_client->get_player_info( index, &info ) ? static_cast< int >( info.m_xuid_low ) : 0;
	}
}

bool n_botox_net::impl_t::active( ) const
{
	return GET_VARIABLE( g_variables.m_botox_network, bool ) && !GET_VARIABLE( g_variables.m_safe_mode, bool );
}

bool n_botox_net::impl_t::skins_on( ) const
{
	return active( ) && GET_VARIABLE( g_variables.m_botox_network_skins, bool );
}

bool n_botox_net::impl_t::send( const packet_t& packet )
{
	using construct_t = void( __fastcall* )( void* self, void* edx );

	static const auto construct =
		reinterpret_cast< construct_t >( g_modules[ ENGINE_DLL ].find_pattern( "56 57 8B F9 8D 4F 08 C7 07 ? ? ? ? E8 ? ? ? ? C7" ) );

	const auto channel = g_interfaces.m_client_state ? g_interfaces.m_client_state->m_net_channel : nullptr;
	if ( !construct || !channel )
		return false;

	/* never destructed: data_ stays kEmptyString and the wrapper string is SSO, nothing to free */
	alignas( 8 ) std::uint8_t message[ 0x100 ]{ };
	construct( message, nullptr );

	std::uint32_t words[ 5 ]{ };
	std::memcpy( words, &packet, sizeof( words ) );

	const auto base = reinterpret_cast< std::uintptr_t >( message );
	field< std::uint32_t >( base, CLC_XUID )           = words[ 0 ];
	field< std::uint32_t >( base, CLC_XUID + 4 )       = words[ 1 ];
	field< std::uint32_t >( base, CLC_SEQUENCE_BYTES ) = words[ 2 ];
	field< std::uint32_t >( base, CLC_SECTION )        = words[ 3 ];
	field< std::uint32_t >( base, CLC_SAMPLE_OFFSET )  = words[ 4 ];
	field< int >( base, CLC_FORMAT )                   = VOICE_FORMAT_STEAM;
	field< std::uint32_t >( base, CLC_HAS_BITS )       = 0x3F;

	return g_virtual.call< bool >( channel, 40, message, false, true );
}

void n_botox_net::impl_t::force_server_masks( )
{
	if ( m_masks_forced )
		return;

	m_masks_forced = true;
	g_interfaces.m_engine_client->client_cmd_unrestricted( "cmd VModEnable 1; cmd vban 0 0 0 0" );
	botox_dbg_log( "NET: forced VModEnable 1 + vban 0" );
}

void n_botox_net::impl_t::on_create_move( )
{
	if ( !active( ) ) {
		m_masks_forced = false;
		m_users.store( 0, std::memory_order_relaxed );
		return;
	}

	if ( !g_ctx.m_local || !g_interfaces.m_client_state || !g_interfaces.m_client_state->m_net_channel )
		return;

	force_server_masks( );

	int users = 0;
	for ( int i = 1; i <= 64; i++ ) {
		if ( fresh( m_user_seen[ i ].m_time, USER_TIMEOUT ) )
			users++;
	}
	m_users.store( users, std::memory_order_relaxed );

	const bool share_esp   = GET_VARIABLE( g_variables.m_botox_network_esp, bool );
	const bool share_skins = GET_VARIABLE( g_variables.m_botox_network_skins, bool );

	if ( !fresh( m_last_hello, 1.f ) ) {
		const std::uint8_t flags = ( share_esp ? hello_esp : 0 ) | ( share_skins ? hello_skins : 0 );
		if ( !send( make_packet( packet_hello, &flags, sizeof( flags ) ) ) )
			botox_dbg_log( "NET: hello send failed" );
		m_last_hello = now( );
	}

	if ( fresh( m_mic_time, 0.3f ) )
		return;

	if ( share_esp )
		send_esp( );

	if ( share_skins )
		send_skins( );
}

void n_botox_net::impl_t::send_esp( )
{
	const int max_clients = std::clamp( g_interfaces.m_global_vars_base->m_max_clients, 1, 64 );

	for ( int step = 0; step < max_clients; step++ ) {
		const int index = std::clamp( m_esp_cursor, 1, max_clients );
		m_esp_cursor    = index % max_clients + 1;

		const auto player = g_interfaces.m_client_entity_list->get< c_base_entity >( index );
		if ( !player || player->is_dormant( ) || !player->is_alive( ) )
			continue;

		const c_vector& origin = player->get_abs_origin( );

		esp_data_t esp{ };
		esp.m_index       = static_cast< std::uint8_t >( index );
		esp.m_health      = static_cast< std::uint8_t >( std::clamp( player->get_health( ), 0, 255 ) );
		esp.m_origin[ 0 ] = origin.m_x;
		esp.m_origin[ 1 ] = origin.m_y;
		esp.m_origin[ 2 ] = origin.m_z;

		send( make_packet( packet_esp, &esp, sizeof( esp ) ) );
		return;
	}
}

void n_botox_net::impl_t::send_skins( )
{
	if ( fresh( m_last_skin, 0.05f ) )
		return;

	m_last_skin = now( );

	if ( m_queue_cursor >= m_queue.size( ) ) {
		queue_skins( );
		m_queue_cursor = 0;
	}

	if ( m_queue_cursor < m_queue.size( ) )
		send( m_queue[ m_queue_cursor++ ] );
}

/* one cycle of everything our own items look like, rebuilt each time it runs dry (picks up config changes) */
void n_botox_net::impl_t::queue_skins( )
{
	m_queue.clear( );

	const int glove_model = GET_VARIABLE( PLAYER_VAR( m_gloves_model ), int );
	if ( GET_VARIABLE( PLAYER_VAR( m_gloves_enable ), bool ) && glove_model > 0 && glove_model < GLOVE_COUNT ) {
		glove_data_t glove{ };
		glove.m_model     = static_cast< std::uint8_t >( glove_model );
		glove.m_paint_kit = static_cast< std::int16_t >( std::clamp( GET_VARIABLE( PLAYER_VAR( m_gloves_paint_kit ), int ), 0, 0x7FFF ) );
		glove.m_seed      = static_cast< std::int16_t >( std::clamp( GET_VARIABLE( PLAYER_VAR( m_gloves_seed ), int ), 0, 0x7FFF ) );
		glove.m_wear      = GET_VARIABLE( PLAYER_VAR( m_gloves_wear ), float );
		m_queue.push_back( make_packet( packet_glove, &glove, sizeof( glove ) ) );
	}

	const auto local = g_ctx.m_local;
	if ( !local || !local->is_alive( ) )
		return;

	const auto weapons = local->get_weapons_handle( );
	if ( !weapons )
		return;

	for ( int w = 0; w < 64; w++ ) {
		const unsigned int handle = weapons[ w ];
		if ( handle == 0xFFFFFFFF )
			continue;

		const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( handle );
		if ( !weapon )
			continue;

		const short definition  = weapon->get_item_definition_index( );
		const int paint_kit     = weapon->get_fall_back_paint_kit( );
		const int stattrak      = weapon->get_fall_back_stat_trak( );
		const bool knife        = is_knife_class( class_id_of( weapon ) );
		const bool custom_knife = knife && !is_default_knife( definition ) && knife_slot( definition );

		char name[ NAME_MAX + 1 ]{ };
		if ( const char* custom = custom_name_of( weapon ) )
			std::memcpy( name, custom, strnlen( custom, NAME_MAX ) );
		const auto name_length = static_cast< std::uint8_t >( strnlen( name, NAME_MAX ) );

		n_skins::sticker_t stickers[ STICKERS ]{ };
		std::uint8_t sticker_mask = 0;
		if ( !knife ) {
			const int gun = gun_index_of( definition );
			for ( int slot = 0; slot < STICKERS; slot++ ) {
				stickers[ slot ] = config_sticker( gun, slot );
				if ( stickers[ slot ].m_kit > 0 )
					sticker_mask |= 1 << slot;
			}
		}

		if ( paint_kit <= 0 && !custom_knife && stattrak < 0 && !name_length && !sticker_mask )
			continue;

		skin_data_t skin{ };
		skin.m_handle     = handle;
		skin.m_definition = definition;
		skin.m_paint_kit  = static_cast< std::int16_t >( std::clamp( paint_kit, 0, 0x7FFF ) );
		skin.m_seed       = static_cast< std::int16_t >( std::clamp( weapon->get_fall_back_seed( ), 0, 0x7FFF ) );
		skin.m_wear       = weapon->get_fall_back_wear( );
		m_queue.push_back( make_packet( packet_skin, &skin, sizeof( skin ) ) );

		extra_data_t extra{ };
		extra.m_handle       = handle;
		extra.m_stattrak     = stattrak;
		extra.m_quality      = static_cast< std::uint8_t >( std::clamp( weapon->get_entity_quality( ), 0, 255 ) );
		extra.m_name_length  = name_length;
		extra.m_sticker_mask = sticker_mask;
		m_queue.push_back( make_packet( packet_extra, &extra, sizeof( extra ) ) );

		for ( int chunk = 0; chunk * NAME_CHUNK < name_length; chunk++ ) {
			name_data_t part{ };
			part.m_handle = handle;
			part.m_chunk  = static_cast< std::uint8_t >( chunk );
			std::memcpy( part.m_text, name + chunk * NAME_CHUNK, ( std::min )( NAME_CHUNK, name_length - chunk * NAME_CHUNK ) );
			m_queue.push_back( make_packet( packet_name, &part, sizeof( part ) ) );
		}

		for ( int slot = 0; slot < STICKERS; slot++ ) {
			if ( !( sticker_mask & ( 1 << slot ) ) )
				continue;

			sticker_data_t sticker{ };
			sticker.m_handle        = handle;
			sticker.m_slot          = static_cast< std::uint8_t >( slot );
			sticker.m_kit           = static_cast< std::int16_t >( std::clamp( stickers[ slot ].m_kit, 0, 0x7FFF ) );
			sticker.m_scale_milli   = static_cast< std::int16_t >( std::clamp( stickers[ slot ].m_scale * 1000.f, -32000.f, 32000.f ) );
			sticker.m_rotation_deci = static_cast< std::int16_t >( std::clamp( stickers[ slot ].m_rotation * 10.f, -32000.f, 32000.f ) );
			sticker.m_wear          = stickers[ slot ].m_wear;
			m_queue.push_back( make_packet( packet_sticker, &sticker, sizeof( sticker ) ) );
		}
	}
}

bool n_botox_net::impl_t::on_voice_data( const void* message )
{
	const auto base  = reinterpret_cast< std::uintptr_t >( message );
	const int client = field< int >( base, SVC_CLIENT );
	if ( client < 0 || client >= 64 )
		return false;

	const auto voice     = field< msvc_string_t* >( base, SVC_VOICE_DATA );
	const bool has_audio = voice && voice->m_size > 0;

	if ( !has_audio ) {
		if ( !active( ) || field< int >( base, SVC_FORMAT ) != VOICE_FORMAT_STEAM )
			return false;

		const std::uint32_t words[ 5 ] = { field< std::uint32_t >( base, SVC_XUID ), field< std::uint32_t >( base, SVC_XUID + 4 ),
		                                   field< std::uint32_t >( base, SVC_SEQUENCE_BYTES ), field< std::uint32_t >( base, SVC_SECTION ),
		                                   field< std::uint32_t >( base, SVC_SAMPLE_OFFSET ) };

		packet_t packet{ };
		std::memcpy( &packet, words, sizeof( packet ) );

		if ( packet.m_magic == MAGIC && packet.m_version != VERSION && packet.m_version < 16 && packet.m_type >= packet_hello &&
		     packet.m_type <= packet_last ) {
			if ( !fresh( m_user_seen[ client + 1 ].m_time, USER_TIMEOUT ) ) {
				botox_dbg_log( "NET: user %d runs botox net v%d, we run v%d: update both", client + 1, packet.m_version, VERSION );
				m_user_seen[ client + 1 ].m_time = now( );
			}
			return true;
		}

		/* magic + type + version = the whole first u32, a real xuid's account id never matches by chance */
		if ( packet.m_magic != MAGIC || packet.m_version != VERSION || packet.m_type < packet_hello || packet.m_type > packet_last )
			return false;

		receive( client + 1, packet );
		return true;
	}

	if ( !active( ) )
		return false;

	if ( g_convars.int_or( HASH_BT( "voice_modenable" ), 1 ) == 0 )
		return true;

	return fresh( m_ban_time, 2.5f ) && ( m_ban_mask[ client / 32 ] & ( 1u << ( client % 32 ) ) ) != 0;
}

void n_botox_net::impl_t::receive( int sender, const packet_t& packet )
{
	const int local = g_interfaces.m_engine_client->get_local_player( );
	if ( sender == local || sender < 1 || sender > 64 )
		return;

	auto& user = m_user_seen[ sender ];
	if ( !fresh( user.m_time, USER_TIMEOUT ) ) {
		player_info_t info{ };
		const bool named = g_interfaces.m_engine_client->get_player_info( sender, &info );
		botox_dbg_log( "NET: botox user %d %s", sender, named ? info.m_name : "?" );
	}
	user.m_time = now( );

	if ( packet.m_type == packet_hello ) {
		user.m_flags = packet.m_data[ 0 ];
		return;
	}

	if ( packet.m_type == packet_esp ) {
		if ( !GET_VARIABLE( g_variables.m_botox_network_esp, bool ) )
			return;

		esp_data_t esp{ };
		std::memcpy( &esp, packet.m_data, sizeof( esp ) );

		if ( esp.m_index < 1 || esp.m_index > 64 || esp.m_index == local )
			return;

		const c_vector origin( esp.m_origin[ 0 ], esp.m_origin[ 1 ], esp.m_origin[ 2 ] );
		if ( !std::isfinite( origin.m_x ) || !std::isfinite( origin.m_y ) || !std::isfinite( origin.m_z ) ||
		     std::fabs( origin.m_x ) > 65536.f || std::fabs( origin.m_y ) > 65536.f || std::fabs( origin.m_z ) > 65536.f )
			return;

		auto& shared    = m_shared[ esp.m_index ];
		shared.m_origin = origin;
		shared.m_health = esp.m_health;
		shared.m_time   = now( );
		return;
	}

	if ( !GET_VARIABLE( g_variables.m_botox_network_skins, bool ) )
		return;

	if ( packet.m_type == packet_glove ) {
		glove_data_t data{ };
		std::memcpy( &data, packet.m_data, sizeof( data ) );

		if ( data.m_model >= GLOVE_COUNT || data.m_paint_kit < 0 || data.m_seed < 0 || !std::isfinite( data.m_wear ) || data.m_wear < 0.f ||
		     data.m_wear > 1.f )
			return;

		auto& glove = m_gloves[ sender ];
		if ( !fresh( glove.m_time, SKIN_TIMEOUT ) )
			botox_dbg_log( "NET: glove packet from %d model %d paint %d", sender, data.m_model, data.m_paint_kit );

		glove.m_model     = data.m_model;
		glove.m_paint_kit = data.m_paint_kit;
		glove.m_seed      = data.m_seed;
		glove.m_wear      = data.m_wear;
		glove.m_time      = now( );
		return;
	}

	std::uint32_t handle = 0;
	std::memcpy( &handle, packet.m_data, sizeof( handle ) );

	auto found = m_items.find( handle );
	if ( found == m_items.end( ) ) {
		if ( m_items.size( ) >= MAX_ITEMS || packet.m_type != packet_skin )
			return;
		found = m_items.emplace( handle, item_t{ } ).first;
	}

	auto& item = found->second;
	if ( item.m_sender != sender ) {
		item          = { };
		item.m_sender = sender;
	}

	item.m_time = now( );
	if ( item.m_first < 0.f )
		item.m_first = item.m_time;

	switch ( packet.m_type ) {
	case packet_skin: {
		skin_data_t data{ };
		std::memcpy( &data, packet.m_data, sizeof( data ) );

		if ( data.m_paint_kit < 0 || data.m_seed < 0 || !std::isfinite( data.m_wear ) || data.m_wear < 0.f || data.m_wear > 1.f )
			return;

		item.m_has_skin   = true;
		item.m_definition = data.m_definition;
		item.m_paint_kit  = data.m_paint_kit;
		item.m_seed       = data.m_seed;
		item.m_wear       = data.m_wear;

		if ( const int slot = knife_slot( data.m_definition ); slot && !is_default_knife( data.m_definition ) ) {
			user.m_knife_slot = slot;
			user.m_knife_time = now( );
		}
		break;
	}

	case packet_extra: {
		extra_data_t data{ };
		std::memcpy( &data, packet.m_data, sizeof( data ) );

		if ( data.m_stattrak < -1 || data.m_stattrak > 9999999 || data.m_name_length > NAME_MAX || data.m_sticker_mask >= ( 1 << STICKERS ) )
			return;

		if ( data.m_name_length != item.m_name_length ) {
			std::memset( item.m_name, 0, sizeof( item.m_name ) );
			item.m_name_have = 0;
		}

		item.m_has_extra    = true;
		item.m_stattrak     = data.m_stattrak;
		item.m_quality      = data.m_quality;
		item.m_name_length  = data.m_name_length;
		item.m_sticker_mask = data.m_sticker_mask;
		item.m_sticker_have &= data.m_sticker_mask;
		break;
	}

	case packet_sticker: {
		sticker_data_t data{ };
		std::memcpy( &data, packet.m_data, sizeof( data ) );

		if ( data.m_slot >= STICKERS || data.m_kit < 0 || !std::isfinite( data.m_wear ) )
			return;

		auto& sticker      = item.m_stickers[ data.m_slot ];
		sticker.m_kit      = data.m_kit;
		sticker.m_wear     = std::clamp( data.m_wear, 0.f, 1.f );
		sticker.m_scale    = data.m_scale_milli / 1000.f;
		sticker.m_rotation = data.m_rotation_deci / 10.f;
		item.m_sticker_have |= 1 << data.m_slot;
		break;
	}

	case packet_name: {
		name_data_t data{ };
		std::memcpy( &data, packet.m_data, sizeof( data ) );

		const int offset = data.m_chunk * NAME_CHUNK;
		if ( offset >= NAME_MAX )
			return;

		std::memcpy( item.m_name + offset, data.m_text, ( std::min )( NAME_CHUNK, NAME_MAX - offset ) );
		item.m_name[ NAME_MAX ] = '\0';
		item.m_name_have |= 1 << data.m_chunk;
		break;
	}

	default:
		break;
	}
}

void n_botox_net::impl_t::on_string_cmd( c_net_message* message )
{
	if ( !message || !active( ) )
		return;

	const auto command = field< msvc_string_t* >( reinterpret_cast< std::uintptr_t >( message ), STRING_CMD_COMMAND );
	if ( !command || command->m_size < 4 || command->m_size >= 256 )
		return;

	char* text = command->data( );

	if ( command->m_size == 12 && !std::strncmp( text, "VModEnable ", 11 ) ) {
		text[ 11 ] = '1';
		return;
	}

	if ( std::strncmp( text, "vban", 4 ) || ( command->m_size > 4 && text[ 4 ] != ' ' ) )
		return;

	char copy[ 256 ]{ };
	std::memcpy( copy, text, command->m_size );

	std::uint32_t masks[ 2 ] = { };
	int count                = 0;

	for ( char* cursor = copy + 4; *cursor && count < 32; ) {
		char* end                 = nullptr;
		const unsigned long value = std::strtoul( cursor, &end, 16 );
		if ( end == cursor )
			break;

		if ( count < 2 )
			masks[ count ] = static_cast< std::uint32_t >( value );

		count++;
		cursor = end;
	}

	m_ban_mask[ 0 ] = masks[ 0 ];
	m_ban_mask[ 1 ] = masks[ 1 ];
	m_ban_time      = now( );

	/* every value was >= 1 char, " 0" per value never outgrows the original */
	std::size_t length = 4;
	for ( int i = 0; i < count; i++ ) {
		text[ length++ ] = ' ';
		text[ length++ ] = '0';
	}

	text[ length ]  = '\0';
	command->m_size = length;
}

void n_botox_net::impl_t::on_voice_send( c_net_message* message )
{
	if ( !message || !active( ) )
		return;

	const auto data = field< msvc_string_t* >( reinterpret_cast< std::uintptr_t >( message ), CLC_DATA );
	if ( data && data->m_size > 0 )
		m_mic_time = now( );
}

bool n_botox_net::impl_t::shared_player( int index, c_vector& origin, int& health ) const
{
	if ( index < 1 || index > 64 || !active( ) || !GET_VARIABLE( g_variables.m_botox_network_esp, bool ) )
		return false;

	const auto& shared = m_shared[ index ];
	if ( !fresh( shared.m_time, ESP_TIMEOUT ) || shared.m_health <= 0 )
		return false;

	origin = shared.m_origin;
	health = shared.m_health;
	return true;
}

int n_botox_net::impl_t::knife_slot_of( int player_index ) const
{
	if ( player_index < 1 || player_index > 64 || !skins_on( ) )
		return 0;

	const auto& user = m_user_seen[ player_index ];
	return fresh( user.m_knife_time, SKIN_TIMEOUT ) ? user.m_knife_slot : 0;
}

void n_botox_net::impl_t::on_frame_stage_notify( int stage )
{
	if ( stage != net_update_postdataupdate_start )
		return;

	if ( !skins_on( ) ) {
		if ( m_gloves_live )
			release_gloves( );
		return;
	}

	apply_spectate( );
	apply_items( );
	apply_gloves( );
}

/* in-eye on a botox user: their knife on the view model, and a rebuild of the held gun / arms after every
   target or weapon change, so the composite is made while the game counts it as first person spectated */
void n_botox_net::impl_t::apply_spectate( )
{
	const auto local = g_ctx.m_local;

	c_base_entity* target = nullptr;
	if ( local && !local->is_alive( ) && local->get_observer_mode( ) == obs_mode_in_eye )
		target = g_interfaces.m_client_entity_list->get< c_base_entity >( local->get_observer_target_handle( ) );

	const int index = target ? target->get_index( ) : 0;
	if ( index < 1 || index > 64 || !fresh( m_user_seen[ index ].m_time, USER_TIMEOUT ) ) {
		m_spec_target = 0;
		m_spec_weapon = 0;
		return;
	}

	const unsigned int weapon_handle = target->get_active_weapon_handle( );
	if ( index != m_spec_target || weapon_handle != m_spec_weapon ) {
		m_spec_target          = index;
		m_spec_weapon          = weapon_handle;
		m_spec_rebuild_at[ 0 ] = now( );
		m_spec_rebuild_at[ 1 ] = now( ) + 0.5f;
	}

	const auto viewmodel = g_interfaces.m_client_entity_list->get< c_base_entity >( target->get_view_model_handle( ) );
	const auto weapon    = g_interfaces.m_client_entity_list->get< c_base_entity >( weapon_handle );

	if ( const int slot = knife_slot_of( index ); viewmodel && weapon && slot > 0 && is_knife_class( class_id_of( weapon ) ) ) {
		const int model_index = precache_model( KNIFE_MODELS[ slot ] );
		if ( model_index > 0 && viewmodel->get_model_index( ) != model_index )
			viewmodel->get_model_index( ) = model_index;
	}

	for ( auto& at : m_spec_rebuild_at ) {
		if ( at <= 0.f || now( ) < at )
			continue;

		at = 0.f;

		const auto item = m_items.find( weapon_handle );
		if ( item != m_items.end( ) )
			item->second.m_applied = 0;

		const auto glove = m_gloves[ index ].m_handle ? g_interfaces.m_client_entity_list->get< c_base_entity >( m_gloves[ index ].m_handle ) : nullptr;
		if ( glove )
			rebuild_player_arms( target, glove, "net spec" );

		botox_dbg_log( "NET: spec %d weapon 0x%X item %d glove %d vm %p", index, weapon_handle, item != m_items.end( ) ? 1 : 0, glove ? 1 : 0,
		               viewmodel );
	}
}

void n_botox_net::impl_t::apply_items( )
{
	bool rebuilt = false;

	for ( auto it = m_items.begin( ); it != m_items.end( ); ) {
		const std::uint32_t handle = it->first;
		item_t& item               = it->second;

		if ( !fresh( item.m_time, SKIN_TIMEOUT ) ) {
			it = m_items.erase( it );
			continue;
		}

		++it;

		const bool stickers_done = ( item.m_sticker_have & item.m_sticker_mask ) == item.m_sticker_mask;
		const int name_chunks    = ( item.m_name_length + NAME_CHUNK - 1 ) / NAME_CHUNK;
		const bool name_done     = ( item.m_name_have & ( ( 1 << name_chunks ) - 1 ) ) == ( 1 << name_chunks ) - 1;
		const bool complete      = item.m_has_extra && stickers_done && name_done;

		if ( !item.m_has_skin || ( !complete && fresh( item.m_first, ITEM_PATIENCE ) ) )
			continue;

		const auto weapon = g_interfaces.m_client_entity_list->get< c_base_entity >( static_cast< unsigned int >( handle ) );
		if ( !weapon || weapon->is_dormant( ) )
			continue;

		const auto owner = g_interfaces.m_client_entity_list->get< c_base_entity >( weapon->get_owner_entity_handle( ) );
		if ( !owner || owner->get_index( ) != item.m_sender )
			continue;

		const bool knife = is_knife_class( class_id_of( weapon ) );
		if ( !knife && weapon->get_item_definition_index( ) != item.m_definition )
			continue;

		int slot             = 0;
		int model_index      = 0;
		int world_index      = 0;
		c_base_entity* world = nullptr;

		if ( knife ) {
			slot = knife_slot( item.m_definition );
			if ( !slot )
				continue;

			model_index = precache_model( KNIFE_MODELS[ slot ] );
			if ( model_index <= 0 )
				continue;

			world_index = knife_world_model_index( KNIFE_MODELS[ slot ], model_index );

			if ( const auto world_handle = weapon->get_world_model_handle( ) )
				world = g_interfaces.m_client_entity_list->get< c_base_entity >( world_handle );
		}

		n_skins::sticker_t stickers[ STICKERS ]{ };
		for ( int s = 0; s < STICKERS; s++ ) {
			if ( item.m_has_extra && ( item.m_sticker_mask & ( 1 << s ) ) )
				stickers[ s ] = item.m_stickers[ s ];
		}

		const int stattrak = item.m_has_extra ? item.m_stattrak : STATTRAK_OFF;
		const std::string name( item.m_name, strnlen( item.m_name, item.m_name_length ) );

		hash_t hash{ };
		hash.add( item.m_definition );
		hash.add( item.m_paint_kit );
		hash.add( item.m_seed );
		hash.add( item.m_wear );
		hash.add( stattrak );
		hash.add( item.m_quality );
		hash.add( name.data( ), name.size( ) );
		for ( const auto& sticker : stickers ) {
			hash.add( sticker.m_kit );
			hash.add( sticker.m_wear );
			hash.add( sticker.m_scale );
			hash.add( sticker.m_rotation );
		}

		const bool same = item.m_applied == hash.m_value && item.m_applied_to == weapon && weapon->get_fall_back_paint_kit( ) == item.m_paint_kit &&
		                  weapon->get_fall_back_seed( ) == item.m_seed && weapon->get_fall_back_wear( ) == item.m_wear &&
		                  weapon->get_fall_back_stat_trak( ) == stattrak && weapon->get_item_id_high( ) == -1 &&
		                  ( !knife || ( weapon->get_item_definition_index( ) == item.m_definition && weapon->get_model_index( ) == model_index &&
		                                ( !world || world->get_model_index( ) == world_index ) ) );

		if ( same || rebuilt )
			continue;

		weapon->get_owner_xuid_low( )      = 0;
		weapon->get_owner_xuid_high( )     = 0;
		weapon->get_item_id_high( )        = -1;
		weapon->get_account_id( )          = account_id_of( item.m_sender );
		weapon->get_fall_back_paint_kit( ) = item.m_paint_kit;
		weapon->get_fall_back_seed( )      = item.m_seed;
		weapon->get_fall_back_wear( )      = item.m_wear;
		weapon->get_fall_back_stat_trak( ) = stattrak;

		if ( item.m_has_extra ) {
			weapon->get_entity_quality( ) = item.m_quality;
			apply_custom_name( weapon, name );
		}

		if ( knife ) {
			weapon->get_item_definition_index( ) = item.m_definition;
			weapon->get_model_index( )           = model_index;

			if ( world )
				world->get_model_index( ) = world_index;
		}

		rebuild_custom_materials( weapon, stickers );

		if ( item.m_sender == m_spec_target ) {
			if ( const auto target = g_interfaces.m_client_entity_list->get< c_base_entity >( item.m_sender ) ) {
				if ( const auto viewmodel = g_interfaces.m_client_entity_list->get< c_base_entity >( target->get_view_model_handle( ) ) )
					static_cast< c_client_networkable* >( viewmodel )->on_data_changed( 0 );
			}
		}

		botox_dbg_log( "NET: apply 0x%X owner %d def %d paint %d st %d stickers 0x%X name %d knife %d world %d", handle, item.m_sender,
		               item.m_definition, item.m_paint_kit, stattrak, item.m_sticker_mask, static_cast< int >( name.size( ) ), slot, world_index );

		item.m_applied    = hash.m_value;
		item.m_applied_to = weapon;
		rebuilt           = true;
	}
}

void n_botox_net::impl_t::apply_gloves( )
{
	const int local = g_interfaces.m_engine_client->get_local_player( );
	bool rebuilt    = false;

	for ( int i = 1; i <= 64; i++ ) {
		auto& glove_state = m_gloves[ i ];
		const auto player = g_interfaces.m_client_entity_list->get< c_base_entity >( i );

		const bool want = i != local && player && fresh( glove_state.m_time, SKIN_TIMEOUT ) && glove_state.m_model > 0 &&
		                  glove_state.m_model < GLOVE_COUNT;

		if ( !want ) {
			if ( glove_state.m_handle )
				release_glove( i );
			continue;
		}

		if ( player->is_dormant( ) )
			continue;

		const auto wearables = player->get_wearables_handle( );
		if ( !wearables )
			continue;

		auto glove = g_interfaces.m_client_entity_list->get< c_base_entity >( wearables[ 0 ] );

		/* a server-made wearable is theirs to keep: never stomp what we can't restore */
		if ( glove && wearables[ 0 ] != glove_state.m_handle ) {
			static int log_left = 8;
			if ( log_left > 0 && log_left-- )
				botox_dbg_log( "NET: glove skip %d, server wearable 0x%X", i, wearables[ 0 ] );
			continue;
		}

		if ( !glove && glove_state.m_handle ) {
			if ( const auto ours = g_interfaces.m_client_entity_list->get< c_base_entity >( glove_state.m_handle ) ) {
				wearables[ 0 ] = glove_state.m_handle;
				glove          = ours;
			} else
				glove_state.m_handle = 0;
		}

		if ( !glove ) {
			if ( rebuilt )
				continue;

			const auto entry  = g_interfaces.m_client_entity_list->get_highest_entity_index( ) + 1;
			const auto serial = rand( ) % 0x1000;
			glove             = make_glove( entry, serial );
			if ( !glove )
				continue;

			wearables[ 0 ]        = entry | ( serial << 16 );
			glove_state.m_handle  = wearables[ 0 ];
			glove_state.m_applied = 0;
			m_gloves_live         = true;
			botox_dbg_log( "NET: glove made for %d at %d", i, entry );
		}

		/* v_ arms mesh bonemerged on a third person body = no matching bones = invisible: w_ unless we watch them in-eye */
		const bool first_person = i == m_spec_target;
		const int model_index   = first_person ? precache_model( GLOVE_MODELS[ glove_state.m_model ] )
		                                       : glove_world_model_index( GLOVE_MODELS[ glove_state.m_model ] );
		if ( model_index <= 0 )
			continue;

		if ( glove->get_model_index( ) != model_index )
			glove_state.m_applied = 0;

		apply_glove_model( glove, i );

		glove->get_item_definition_index( ) = GLOVE_IDS[ glove_state.m_model ];
		glove->get_fall_back_paint_kit( )   = glove_state.m_paint_kit;
		glove->set_model_index( model_index );
		glove->get_entity_quality( )      = quality_unusual;
		glove->get_fall_back_wear( )      = glove_state.m_wear;
		glove->get_item_id_high( )        = -1;
		glove->get_fall_back_seed( )      = glove_state.m_seed;
		glove->get_fall_back_stat_trak( ) = STATTRAK_OFF;

		hash_t hash{ };
		hash.add( glove_state.m_model );
		hash.add( glove_state.m_paint_kit );
		hash.add( glove_state.m_seed );
		hash.add( glove_state.m_wear );

		const bool regen = glove_state.m_applied != hash.m_value && !rebuilt;
		if ( regen )
			regen_glove_material( glove, glove_state.m_paint_kit, glove_state.m_seed, glove_state.m_wear );

		static_cast< c_client_networkable* >( glove )->pre_data_update( 0 );

		const float time_now = now( );
		if ( regen ) {
			rebuild_player_arms( player, glove, "net" );
			glove_state.m_applied   = hash.m_value;
			glove_state.m_settle_at = time_now + 0.5f;
			rebuilt                 = true;
		} else if ( glove_state.m_settle_at > 0.f && ( time_now >= glove_state.m_settle_at || time_now < glove_state.m_settle_at - 1.f ) ) {
			rebuild_player_arms( player, nullptr, "net settle" );
			glove_state.m_settle_at = 0.f;
		}
	}
}

void n_botox_net::impl_t::release_glove( int index )
{
	auto& glove_state = m_gloves[ index ];
	const auto player = g_interfaces.m_client_entity_list->get< c_base_entity >( index );

	if ( const auto glove = g_interfaces.m_client_entity_list->get< c_base_entity >( glove_state.m_handle ) ) {
		if ( player ) {
			if ( const auto wearables = player->get_wearables_handle( ); wearables && wearables[ 0 ] == glove_state.m_handle )
				wearables[ 0 ] = 0xFFFFFFFF;
		}

		const auto networkable = static_cast< c_client_networkable* >( glove );
		networkable->set_destroyed_on_recreate_entities( );
		networkable->release( );

		if ( player && player->is_alive( ) && !player->is_dormant( ) )
			rebuild_player_arms( player, nullptr, "net off" );

		botox_dbg_log( "NET: glove released for %d", index );
	}

	glove_state = { };
}

void n_botox_net::impl_t::release_gloves( )
{
	for ( int i = 1; i <= 64; i++ ) {
		if ( m_gloves[ i ].m_handle )
			release_glove( i );
	}

	m_gloves_live = false;
}

void n_botox_net::impl_t::on_level_init( )
{
	for ( auto& shared : m_shared )
		shared = { };

	for ( auto& user : m_user_seen )
		user = { };

	/* level change already destroyed every client entity, the handles are dangling: forget, never release */
	for ( auto& glove : m_gloves )
		glove = { };

	m_gloves_live = false;
	m_items.clear( );
	m_queue.clear( );
	m_queue_cursor  = 0;
	m_spec_target   = 0;
	m_spec_weapon   = 0;
	m_spec_rebuild_at[ 0 ] = m_spec_rebuild_at[ 1 ] = 0.f;
	m_ban_mask[ 0 ] = m_ban_mask[ 1 ] = 0;
	m_ban_time      = -1.f;
	m_masks_forced  = false;
	m_mic_time      = -1.f;
	m_last_hello    = -1.f;
	m_last_skin     = -1.f;
	m_esp_cursor    = 1;
	m_users.store( 0, std::memory_order_relaxed );
}
