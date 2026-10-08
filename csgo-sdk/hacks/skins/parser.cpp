#include "skins.h"
#include "skin_names.h"
#include "sticker_names.h"

#include "../../globals/includes/includes.h"

#include <format>

void n_skins::impl_t::init_parser( )
{
	if ( this->m_parser_done )
		return;

	this->m_parser_done = true;

	this->m_parser_skins.assign( SKIN_NAME_TABLE, SKIN_NAME_TABLE + SKIN_NAME_COUNT );
	this->m_parser_gloves.assign( GLOVE_NAME_TABLE, GLOVE_NAME_TABLE + GLOVE_NAME_COUNT );
	this->m_parser_stickers.assign( STICKER_NAME_TABLE, STICKER_NAME_TABLE + STICKER_NAME_COUNT );

	// make_format_args binds non-const lvalue refs, don't const these
	std::size_t skin_count    = this->m_parser_skins.size( );
	std::size_t glove_count   = this->m_parser_gloves.size( );
	std::size_t sticker_count = this->m_parser_stickers.size( );

	g_console.print( std::vformat( "skins | name table: {:d} skins, {:d} glove kits, {:d} sticker kits",
	                               std::make_format_args( skin_count, glove_count, sticker_count ) )
	                     .c_str( ) );
}
