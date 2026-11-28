#pragma once

#include <string>

/* png / jpg / bmp / gif -> csgo\materials\models\weapons\customization\stickers\bx\<hash>.vtf + .vmt ( stock paper sticker
   vmt = stock wear ), then a spray sticker kit's sticker_material is pointed at it in memory ( carrier ): the game's own
   GenerateStickerMaterials builds it like any sticker. loose files = sv_pure servers need the bypass */
namespace n_sticker_import
{
	// game thread. kit id for a sticker slot, 0 = can't ( status says why ). decodes once per image, cached
	int kit_for( const std::string& path );

	// any thread, menu line
	std::string status( const std::string& path );

	// fsn render_start: on eject puts every carrier's sticker_material back
	void eject_restore( );
}
