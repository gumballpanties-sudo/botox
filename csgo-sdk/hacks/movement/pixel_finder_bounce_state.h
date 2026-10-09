#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

// Copies of prediction datamap value fields for finder/pixelsurf trials. No engine history slot
// is borrowed, and no snapshot is retained in the live player between commands.
namespace n_pf_bounce
{
	struct layout_t {
		// Native CS:GO build 8802 has one field offset. The shared SDK's second
		// offset shifts count/embedded by four bytes; ordinary offset lookup did
		// not expose that mismatch, but snapshots must use the native layout.
		struct field_t {
			e_field_types type;
			const char* name;
			int offset;
			unsigned short count;
			short flags;
			unsigned char metadata[ 12 ];
			data_map_t* embedded;
			unsigned char tail[ 28 ];
		};
		static_assert( sizeof( field_t ) == 60 && offsetof( field_t, count ) == 12 && offsetof( field_t, embedded ) == 28 );
		struct range_t { int offset, bytes; };
		std::vector< range_t > ranges;
		int bytes = 0;
		static int width( e_field_types type ) {
			switch ( type ) {
			case field_boolean: case field_character: return 1;
			case field_short: return 2;
			case field_float: case field_integer: case field_ehandle: case field_time: case field_tick:
			case field_color32: case field_modelindex: case field_materialindex: return 4;
			case field_vector2d: case field_integer64: return 8;
			case field_vector: case field_position_vector: return 12;
			case field_quaternion: case field_vector4d: return 16;
			default: return 0; // Never copy ownership pointers, custom objects or engine callbacks.
			}
		}
		void add( data_map_t* map, int parent = 0, int depth = 0 ) {
			if ( !map || !map->m_data_desc || depth > 8 || map->m_data_fields < 0 || map->m_data_fields > 2048 ) return;
			for ( int i = 0; i < map->m_data_fields; ++i ) {
				field_t f{ };
				std::memcpy( &f, reinterpret_cast< const unsigned char* >( map->m_data_desc ) + i * sizeof( f ), sizeof( f ) );
				const int offset = parent + f.offset;
				if ( offset <= 0 || offset > 65536 ) continue;
				if ( f.type == field_embedded && f.count == 1 ) add( f.embedded, offset, depth + 1 );
				const int size = width( f.type ) * f.count;
				if ( size <= 0 || size > 4096 || bytes + size > 32768 ) continue;
				if ( std::any_of( ranges.begin( ), ranges.end( ), [ & ]( const range_t& r ) { return r.offset == offset; } ) ) continue;
				ranges.push_back( { offset, size } ); bytes += size;
			}
			add( map->m_base_map, parent, depth + 1 );
		}
	};
	struct snapshot_t {
		std::vector< unsigned char > data;
		c_vector origin{ }, absolute{ }, velocity{ }, mins{ }, maxs{ };
		c_user_cmd last{ };
		c_user_cmd* current = nullptr;
		int tick = 0, flags = 0;
		void save( c_base_entity* local, const layout_t& layout ) {
			data.resize( layout.bytes ); int cursor = 0;
			for ( const auto& r : layout.ranges ) {
				std::memcpy( data.data( ) + cursor, reinterpret_cast< unsigned char* >( local ) + r.offset, r.bytes ); cursor += r.bytes;
			}
			origin = local->get_origin( ); absolute = local->get_abs_origin( ); velocity = local->get_velocity( );
			mins = local->get_collideable( )->get_obb_mins( ); maxs = local->get_collideable( )->get_obb_maxs( );
			last = local->get_last_command( ); current = *local->get_current_command( ); tick = local->get_tick_base( ); flags = local->get_flags( );
		}
		void load( c_base_entity* local, const layout_t& layout ) const {
			int cursor = 0;
			for ( const auto& r : layout.ranges ) {
				std::memcpy( reinterpret_cast< unsigned char* >( local ) + r.offset, data.data( ) + cursor, r.bytes ); cursor += r.bytes;
			}
			local->set_abs_origin( absolute ); local->get_origin( ) = origin; local->get_velocity( ) = velocity;
			const_cast< c_vector& >( local->get_collideable( )->get_obb_mins( ) ) = mins;
			const_cast< c_vector& >( local->get_collideable( )->get_obb_maxs( ) ) = maxs;
			local->get_last_command( ) = last; *local->get_current_command( ) = current; local->get_tick_base( ) = tick; local->get_flags( ) = flags;
		}
	};
}
