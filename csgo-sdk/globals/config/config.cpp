#define _SILENCE_CXX20_CISO646_REMOVED_WARNING

#include <fstream>
#include <shlobj.h>

#include "../../dependencies/json/json.hpp"
#include "../../game/sdk/classes/c_color.h"
#include "../../hacks/menu/menu.h"
#include "../../utilities/console/console.h"
#include "../macros/macros.h"
#include "variables.h"

#include "config.h"

/* regenerate visual_variables.inc with tools/visual_config_check.py --write */
static const std::vector< bool >& visual_mask( )
{
	static const std::vector< bool > mask = [ ] {
		std::vector< bool > out( g_config.m_variables.size( ) );
		for ( const std::uint32_t index : {
#include "visual_variables.inc"
			  } )
			out[ index ] = true;
		return out;
	}( );

	return mask;
}

bool n_config::impl_t::on_attach( )
{
	if ( !std::filesystem::is_directory( this->m_path ) ) {
		std::filesystem::remove( this->m_path );
		if ( !std::filesystem::create_directories( this->m_path ) )
			return false;
	}

	std::error_code sounds_ec;
	std::filesystem::create_directories( this->m_path / "sounds", sounds_ec );
	std::filesystem::create_directories( this->folder( true ), sounds_ec );

	this->refresh( );

	if ( !std::filesystem::exists( this->m_path / ( "default.bx" ) ) ) {
		if ( this->save( ( "default.bx" ) ) )
			this->refresh( );
	}

	if ( this->load( ( "default.bx" ) ) )
		g_console.print( "loaded default.bx" );
	else
		g_console.print< n_console::log_level::WARNING >( "failed to load default.bx" );

	for ( std::size_t i = 0U; i < this->m_file_names.size( ); i++ ) {
		if ( this->m_file_names[ i ] == ( "default.bx" ) ) {
			g_menu.m_selected_config = static_cast< int >( i );
			break;
		}
	}

	return true;
}

bool n_config::impl_t::save( std::string_view file_name, const bool visuals )
{
	std::filesystem::path file_path( file_name );
	if ( file_path.extension( ) != ( ".bx" ) )
		file_path.replace_extension( ( ".bx" ) );

	std::error_code folder_ec;
	std::filesystem::create_directories( this->folder( visuals ), folder_ec );

	const std::string file = std::filesystem::path( this->folder( visuals ) / file_path ).string( );
	nlohmann::json config  = { };

	try {
		for ( std::size_t index = 0U; index < this->m_variables.size( ); index++ ) {
			if ( visuals && !visual_mask( )[ index ] )
				continue;

			auto& variable       = this->m_variables[ index ];
			nlohmann::json entry = { };

			entry[ ( "name-id" ) ] = variable.m_name_hash;
			entry[ ( "type-id" ) ] = variable.m_type_hash;

			switch ( variable.m_type_hash ) {
			case HASH_BT( "int" ): {
				entry[ ( "value" ) ] = variable.get< int >( );
				break;
			}
			case HASH_BT( "float" ): {
				entry[ ( "value" ) ] = variable.get< float >( );
				break;
			}
			case HASH_BT( "bool" ): {
				entry[ ( "value" ) ] = variable.get< bool >( );
				break;
			}
			case HASH_BT( "std::string" ): {
				entry[ ( "value" ) ] = variable.get< std::string >( );
				break;
			}
			case HASH_BT( "c_color" ): {
				const auto& color = variable.get< c_color >( );

				nlohmann::json sub = { };

				sub.push_back( color.get< e_color_type::color_type_r >( ) );
				sub.push_back( color.get< e_color_type::color_type_g >( ) );
				sub.push_back( color.get< e_color_type::color_type_b >( ) );
				sub.push_back( color.get< e_color_type::color_type_a >( ) );

				entry[ ( "value" ) ] = sub.dump( );
				break;
			}
			case HASH_BT( "key_bind_t" ): {
				const auto& bind = variable.get< key_bind_t >( );

				nlohmann::json sub = { };

				sub.push_back( bind.m_key );
				sub.push_back( bind.m_key_style );

				entry[ ( "value" ) ] = sub.dump( );
				break;
			}
			case HASH_BT( "font_setting_t" ): {
				const auto& bind = variable.get< font_setting_t >( );

				nlohmann::json sub = { };

				sub.push_back( bind.m_name );
				sub.push_back( bind.m_size );

				entry[ ( "value" ) ] = sub.dump( );
				break;
			}
			case HASH_BT( "std::vector<bool>" ): {
				const auto& booleans = variable.get< std::vector< bool > >( );

				nlohmann::json sub = { };

				for ( const auto&& value : booleans )
					sub.push_back( static_cast< bool >( value ) );

				entry[ ( "value" ) ] = sub.dump( );
				break;
			}
			case HASH_BT( "std::vector<int>" ): {
				const auto& integers = variable.get< std::vector< int > >( );

				nlohmann::json sub = { };

				for ( const auto& value : integers )
					sub.push_back( value );

				entry[ ( "value" ) ] = sub.dump( );
				break;
			}
			case HASH_BT( "std::vector<float>" ): {
				const auto& floats = variable.get< std::vector< float > >( );

				nlohmann::json sub = { };

				for ( const auto& value : floats )
					sub.push_back( value );

				entry[ ( "value" ) ] = sub.dump( );
				break;
			}
			case HASH_BT( "std::vector<std::string>" ): {
				const auto& strings = variable.get< std::vector< std::string > >( );

				nlohmann::json sub = { };

				for ( const auto& value : strings )
					sub.push_back( value );

				entry[ ( "value" ) ] = sub.dump( );
				break;
			}
			case HASH_BT( "std::vector<c_color>" ): {
				const auto& colors = variable.get< std::vector< c_color > >( );

				nlohmann::json sub = { };

				for ( const auto& color : colors ) {
					sub.push_back( color.get< e_color_type::color_type_r >( ) );
					sub.push_back( color.get< e_color_type::color_type_g >( ) );
					sub.push_back( color.get< e_color_type::color_type_b >( ) );
					sub.push_back( color.get< e_color_type::color_type_a >( ) );
				}

				entry[ ( "value" ) ] = sub.dump( );
				break;
			}
			default:
				break;
			}

			config.push_back( entry );
		}
	} catch ( const nlohmann::detail::exception& ex ) {
		g_console.print( std::vformat( "failed to save {}", std::make_format_args( *ex.what( ) ) ).c_str( ) );
		return false;
	}

	std::ofstream output_file( file, std::ios::out | std::ios::trunc );
	if ( !output_file.good( ) )
		return false;

	try {
		output_file << config.dump( 4 );
		output_file.close( );
	} catch ( std::ofstream::failure& ex ) {
		g_console.print( std::vformat( "failed to save {}", std::make_format_args( *ex.what( ) ) ).c_str( ) );
		return false;
	}

	return true;
}

bool n_config::impl_t::load( std::string_view file_name, const bool visuals )
{
	const std::string file = std::filesystem::path( this->folder( visuals ) / file_name ).string( );
	nlohmann::json config  = { };

	std::ifstream input_file( file, std::ios::in );

	if ( !input_file.good( ) )
		return false;

	try {
		config = nlohmann::json::parse( input_file, nullptr, false );

		if ( config.is_discarded( ) )
			return false;

		input_file.close( );
	} catch ( std::ifstream::failure& ex ) {
		g_console.print( std::vformat( "failed to load {}", std::make_format_args( *ex.what( ) ) ).c_str( ) );
		return false;
	}

	try {
		for ( const auto& variable : config ) {
			const unsigned int index = this->get_variable_index( variable[ ( "name-id" ) ].get< unsigned int >( ) );

			if ( index == INVALID_VARIABLE )
				continue;

			/* a full .bx dropped in the visuals folder still only touches visuals */
			if ( visuals && !visual_mask( )[ index ] )
				continue;

			auto& entry = this->m_variables[ index ];

			/* type changed between builds (int to float, say): the stale entry would swap the std::any type and
			   GET_VARIABLE would deref null. keep the default */
			if ( variable[ ( "type-id" ) ].get< unsigned int >( ) != entry.m_type_hash )
				continue;

			switch ( variable[ ( "type-id" ) ].get< unsigned int >( ) ) {
			case HASH_BT( "bool" ): {
				entry.set< bool >( variable[ ( "value" ) ].get< bool >( ) );
				break;
			}
			case HASH_BT( "float" ): {
				entry.set< float >( variable[ ( "value" ) ].get< float >( ) );
				break;
			}
			case HASH_BT( "int" ): {
				entry.set< int >( variable[ ( "value" ) ].get< int >( ) );
				break;
			}
			case HASH_BT( "std::string" ): {
				entry.set< std::string >( variable[ ( "value" ) ].get< std::string >( ) );
				break;
			}
			case HASH_BT( "c_color" ): {
				const nlohmann::json vector = nlohmann::json::parse( variable[ ( "value" ) ].get< std::string >( ) );

				if ( !vector.is_array( ) || vector.size( ) < 4U )
					break;

				entry.set< c_color >( c_color( vector[ 0 ].get< std::uint8_t >( ), vector[ 1 ].get< std::uint8_t >( ),
				                               vector[ 2 ].get< std::uint8_t >( ), vector[ 3 ].get< std::uint8_t >( ) ) );

				break;
			}
			case HASH_BT( "key_bind_t" ): {
				const nlohmann::json vector = nlohmann::json::parse( variable[ ( "value" ) ].get< std::string >( ) );

				if ( !vector.is_array( ) || vector.size( ) < 2U )
					break;

				entry.set< key_bind_t >( key_bind_t( vector[ 0 ].get< int >( ), vector[ 1 ].get< int >( ) ) );
				break;
			}
			case HASH_BT( "font_setting_t" ): {
				const nlohmann::json vector = nlohmann::json::parse( variable[ ( "value" ) ].get< std::string >( ) );

				if ( !vector.is_array( ) || vector.size( ) < 2U )
					break;

				entry.set< font_setting_t >( font_setting_t( vector[ 0 ].get< std::string >( ), vector[ 1 ].get< int >( ) ) );
				break;
			}
			case HASH_BT( "std::vector<bool>" ): {
				const nlohmann::json vector = nlohmann::json::parse( variable[ ( "value" ) ].get< std::string >( ) );
				auto& booleans              = entry.get< std::vector< bool > >( );

				if ( !vector.is_array( ) )
					break;

				for ( std::size_t i = 0U; i < vector.size( ); i++ ) {
					if ( i < booleans.size( ) )
						booleans[ i ] = vector[ i ].get< bool >( );
				}

				break;
			}
			case HASH_BT( "std::vector<int>" ): {
				const nlohmann::json vector = nlohmann::json::parse( variable[ ( "value" ) ].get< std::string >( ) );
				auto& integers              = entry.get< std::vector< int > >( );

				if ( !vector.is_array( ) )
					break;

				for ( std::size_t i = 0U; i < vector.size( ); i++ ) {
					if ( i < integers.size( ) )
						integers[ i ] = vector[ i ].get< int >( );
				}

				break;
			}
			case HASH_BT( "std::vector<float>" ): {
				const nlohmann::json vector = nlohmann::json::parse( variable[ ( "value" ) ].get< std::string >( ) );
				auto& floats                = entry.get< std::vector< float > >( );

				if ( !vector.is_array( ) )
					break;

				for ( std::size_t i = 0U; i < vector.size( ); i++ ) {
					if ( i < floats.size( ) )
						floats[ i ] = vector[ i ].get< float >( );
				}

				break;
			}
			case HASH_BT( "std::vector<std::string>" ): {
				const nlohmann::json vector = nlohmann::json::parse( variable[ ( "value" ) ].get< std::string >( ) );
				auto& strings               = entry.get< std::vector< std::string > >( );

				if ( !vector.is_array( ) )
					break;

				for ( std::size_t i = 0U; i < vector.size( ); i++ ) {
					if ( i < strings.size( ) )
						strings[ i ] = vector[ i ].get< std::string >( );
				}

				break;
			}
			case HASH_BT( "std::vector<c_color>" ): {
				const nlohmann::json vector = nlohmann::json::parse( variable[ ( "value" ) ].get< std::string >( ) );
				auto& colors                = entry.get< std::vector< c_color > >( );

				if ( !vector.is_array( ) )
					break;

				for ( std::size_t i = 0U; i + 3U < vector.size( ); i += 4U ) {
					if ( i / 4U < colors.size( ) )
						colors[ i / 4U ] = c_color( vector[ i ].get< std::uint8_t >( ), vector[ i + 1U ].get< std::uint8_t >( ),
						                            vector[ i + 2U ].get< std::uint8_t >( ), vector[ i + 3U ].get< std::uint8_t >( ) );
				}

				break;
			}
			default:
				break;
			}
		}
	} catch ( const nlohmann::detail::exception& ex ) {
		g_console.print( std::vformat( "json load failed {}", std::make_format_args( *ex.what( ) ) ).c_str( ) );
		return false;
	}

	auto& ps_point_key = GET_VARIABLE( g_variables.m_pixel_surf_assist_point_key, key_bind_t );
	const auto& bounce_point_key = GET_VARIABLE( g_variables.m_bounce_assist_point_key, key_bind_t );

	if ( ps_point_key.m_key == 0 && bounce_point_key.m_key != 0 )
		ps_point_key = bounce_point_key;

	auto& bounce_key = GET_VARIABLE( g_variables.m_bounce_assist_key, key_bind_t );
	const auto& ps_assist_key = GET_VARIABLE( g_variables.m_pixel_surf_assist_key, key_bind_t );

	if ( bounce_key.m_key == 0 && ps_assist_key.m_key != 0 )
		bounce_key = ps_assist_key;

	return true;
}

void n_config::impl_t::remove( const std::size_t index, const bool visuals )
{
	auto& names = this->file_names( visuals );
	if ( index >= names.size( ) )
		return;

	const std::string file = std::filesystem::path( this->folder( visuals ) / names[ index ] ).string( );

	std::error_code remove_ec;
	if ( std::filesystem::remove( file, remove_ec ) )
		names.erase( names.cbegin( ) + static_cast< std::ptrdiff_t >( index ) );
}

void n_config::impl_t::refresh( )
{
	for ( const bool visuals : { false, true } ) {
		auto& names = this->file_names( visuals );
		names.clear( );

		std::error_code iterate_ec;
		for ( const auto& it : std::filesystem::directory_iterator( this->folder( visuals ), iterate_ec ) )
			if ( it.path( ).filename( ).extension( ) == ( ".bx" ) )
				names.push_back( it.path( ).filename( ).string( ) );
	}
}

std::size_t n_config::impl_t::get_variable_index( const unsigned int name_hash )
{
	for ( unsigned int i = 0U; i < this->m_variables.size( ); i++ ) {
		if ( this->m_variables[ i ].m_name_hash == name_hash )
			return i;
	}

	return INVALID_VARIABLE;
}
