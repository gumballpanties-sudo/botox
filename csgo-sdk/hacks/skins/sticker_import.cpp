#include "sticker_import.h"
#include "skins_internal.h"
#include "../../globals/logger/logger.h"

// gdi+, not wic: wincodec.h needs the new dxgitype.h, the old dx sdk include dir shadows it
#include <shlwapi.h>
#include <gdiplus.h>

#include <filesystem>
#include <mutex>

#pragma comment( lib, "gdiplus.lib" )
#pragma comment( lib, "shlwapi.lib" )

namespace
{
	/* legacy client.dll, CEconItemView::GenerateStickerMaterials:
	     call ItemSystem; push edi; lea ecx, [eax+4]; call CEconItemSchema::GetStickerKitDefinition; mov [ebp-1Ch], eax
	   then [kit+44h] ( sMaterialPath length ) != 0, edi = [kit+38h] ( name part of "sticker_%s_%s_%i_1st" ), and the
	   material params = MakeCopy of [kit+0D4h], the sticker vmt KeyValues CStickerKit::InitFromKeyValues loaded from
	   materials/models/weapons/customization/stickers/<path>.vmt. graffiti ( spray ) kits skip that load = null there */
	constexpr std::size_t KIT_ITEM_NAME = 0x28, KIT_PATH = 0x38, KIT_VMT = 0xD4;
	// CUtlString = CUtlMemory< char > { ptr, alloc, grow } + length ( strlen + 1 )
	constexpr std::size_t STR_ALLOC = 4, STR_GROW = 8, STR_LENGTH = 12;

	constexpr int k_max_kit_id = 30000;
	constexpr int k_min_size = 64, k_max_size = 1024;

	struct carrier_t {
		int m_id;
		std::uint8_t* m_kit;
		std::string m_original;
		int m_original_length;
		unsigned int m_hash = 0; // image on it, 0 = free
		c_key_values* m_vmt = nullptr;
	};

	struct image_t {
		int m_kit = 0;
		std::string m_status;
	};

	std::mutex g_mutex;
	std::unordered_map< std::string, image_t > g_images;
	std::vector< carrier_t > g_carriers;
	bool g_carriers_scanned = false;
	bool g_restored         = false;

	using item_system_t = void*( __cdecl* )( );
	using get_sticker_kit_t = std::uint8_t*( __thiscall* )( void* schema, int id );

	std::uint8_t* sticker_kit( int id )
	{
		static const auto site = g_modules[ CLIENT_DLL ].find_pattern( "E8 ? ? ? ? 57 8D 48 04 E8 ? ? ? ? 89 45 E4 85 C0" );
		if ( !site )
			return nullptr;

		const auto item_system = reinterpret_cast< item_system_t >( site + 5 + *reinterpret_cast< const std::int32_t* >( site + 1 ) );
		const auto get_kit     = reinterpret_cast< get_sticker_kit_t >( site + 14 + *reinterpret_cast< const std::int32_t* >( site + 10 ) );

		const auto system = static_cast< std::uint8_t* >( item_system( ) );
		return system ? get_kit( system + 4, id ) : nullptr;
	}

	bool writable( const void* address, std::size_t size )
	{
		MEMORY_BASIC_INFORMATION info{ };
		if ( !address || !VirtualQuery( address, &info, sizeof( info ) ) || info.State != MEM_COMMIT ||
		     ( info.Protect & ( PAGE_NOACCESS | PAGE_GUARD ) ) || !( info.Protect & ( PAGE_READWRITE | PAGE_EXECUTE_READWRITE ) ) )
			return false;
		return reinterpret_cast< std::uintptr_t >( address ) + size <= reinterpret_cast< std::uintptr_t >( info.BaseAddress ) + info.RegionSize;
	}

	// heap CUtlString with its terminator where the length says, else nullptr
	char* kit_string( std::uint8_t* kit, std::size_t field )
	{
		char* const text = *reinterpret_cast< char** >( kit + field );
		const int alloc  = *reinterpret_cast< int* >( kit + field + STR_ALLOC );
		const int grow   = *reinterpret_cast< int* >( kit + field + STR_GROW );
		const int length = *reinterpret_cast< int* >( kit + field + STR_LENGTH );
		if ( length <= 1 || length > alloc || grow < 0 || !writable( text, static_cast< std::size_t >( alloc ) ) || text[ length - 1 ] != '\0' ||
		     std::strlen( text ) != static_cast< std::size_t >( length - 1 ) )
			return nullptr;
		return text;
	}

	// spray kits never drawn on a gun: their sticker_material + vmt slot carry our images
	void scan_carriers( )
	{
		g_carriers_scanned = true;

		for ( int id = 1; id < k_max_kit_id; id++ ) {
			std::uint8_t* const kit = sticker_kit( id );
			if ( !kit || !writable( kit, KIT_VMT + 4 ) || *reinterpret_cast< int* >( kit ) != id || *reinterpret_cast< void** >( kit + KIT_VMT ) )
				continue;

			const char* const item_name = kit_string( kit, KIT_ITEM_NAME );
			char* const path            = kit_string( kit, KIT_PATH );
			// "bx/" + 8 hex fits in place
			if ( !item_name || std::strncmp( item_name, "#SprayKit_", 10 ) != 0 || !path || std::strlen( path ) < 11 )
				continue;

			g_carriers.push_back( { id, kit, path, *reinterpret_cast< int* >( kit + KIT_PATH + STR_LENGTH ) } );
		}

		if ( g_carriers.empty( ) )
			g_logger.print( "no spray sticker kits found to carry imported stickers ( game update? )\n", "[sticker]" );
	}

	void write_path( std::uint8_t* kit, const std::string& text, int length )
	{
		std::memcpy( *reinterpret_cast< char** >( kit + KIT_PATH ), text.c_str( ), text.size( ) + 1 );
		*reinterpret_cast< int* >( kit + KIT_PATH + STR_LENGTH ) = length;
	}

	std::string vtf_dir( )
	{
		char exe[ MAX_PATH ]{ };
		if ( !GetModuleFileNameA( nullptr, exe, MAX_PATH ) )
			return { };
		std::string root = exe;
		root.erase( root.find_last_of( "\\/" ) );
		return root + "\\csgo\\materials\\botox_stickers\\";
	}

	/* rgba straight alpha out of premultiplied mips ( box filter right in premultiplied ). see-through texels take the
	   color of the coarser mip under them: bilinear at the cutout edge blends in the sticker's color, not black */
	std::vector< std::vector< std::uint8_t > > build_mips( std::vector< std::uint8_t > top, int size )
	{
		std::vector< std::vector< std::uint8_t > > mips{ std::move( top ) };
		for ( int s = size / 2; s >= 1; s /= 2 ) {
			const auto& up = mips.back( );
			std::vector< std::uint8_t > down( static_cast< std::size_t >( s ) * s * 4 );
			for ( int y = 0; y < s; y++ ) {
				for ( int x = 0; x < s; x++ ) {
					for ( int c = 0; c < 4; c++ ) {
						const auto at = [ & ]( int dx, int dy ) { return up[ ( ( static_cast< std::size_t >( y * 2 + dy ) * s * 2 ) + x * 2 + dx ) * 4 + c ]; };
						down[ ( static_cast< std::size_t >( y ) * s + x ) * 4 + c ] =
							static_cast< std::uint8_t >( ( at( 0, 0 ) + at( 1, 0 ) + at( 0, 1 ) + at( 1, 1 ) + 2 ) / 4 );
					}
				}
			}
			mips.push_back( std::move( down ) );
		}

		for ( auto& mip : mips ) {
			for ( std::size_t i = 0; i < mip.size( ); i += 4 ) {
				const unsigned int a = mip[ i + 3 ];
				for ( int c = 0; c < 3; c++ )
					mip[ i + c ] = a ? static_cast< std::uint8_t >( ( std::min )( 255u, ( mip[ i + c ] * 255u + a / 2 ) / a ) ) : 0;
			}
		}

		for ( int level = static_cast< int >( mips.size( ) ) - 2, s = 2; level >= 0; level--, s *= 2 ) {
			auto& mip         = mips[ level ];
			const auto& below = mips[ level + 1 ];
			for ( int y = 0; y < s; y++ ) {
				for ( int x = 0; x < s; x++ ) {
					std::uint8_t* const p = &mip[ ( static_cast< std::size_t >( y ) * s + x ) * 4 ];
					if ( !p[ 3 ] )
						std::memcpy( p, &below[ ( static_cast< std::size_t >( y / 2 ) * ( s / 2 ) + x / 2 ) * 4 ], 3 );
				}
			}
		}
		return mips;
	}

	// vtf 7.2 ( mc_sky.h layout ), BGRA8888, full mip chain smallest first, clamp s/t + aniso + 8 bit alpha like stock stickers
	bool write_vtf( const std::string& file, int size, const std::vector< std::vector< std::uint8_t > >& mips )
	{
		std::uint8_t header[ 80 ]{ };
		const auto put = [ & ]( int at, auto value ) { std::memcpy( header + at, &value, sizeof( value ) ); };
		std::memcpy( header, "VTF", 4 );
		put( 4, std::uint32_t( 7 ) );
		put( 8, std::uint32_t( 2 ) );
		put( 12, std::uint32_t( 80 ) );
		put( 16, std::uint16_t( size ) );
		put( 18, std::uint16_t( size ) );
		put( 20, std::uint32_t( 0x4 | 0x8 | 0x10 | 0x2000 ) );
		put( 24, std::uint16_t( 1 ) );
		put( 48, 1.f );
		put( 52, std::uint32_t( 12 ) ); // IMAGE_FORMAT_BGRA8888
		header[ 56 ] = static_cast< std::uint8_t >( mips.size( ) );
		put( 57, std::uint32_t( 0xffffffff ) );
		put( 63, std::uint16_t( 1 ) );

		const std::string temp = file + ".tmp";
		{
			std::ofstream out( temp, std::ios::binary | std::ios::trunc );
			out.write( reinterpret_cast< const char* >( header ), sizeof( header ) );
			for ( auto mip = mips.rbegin( ); mip != mips.rend( ); ++mip )
				out.write( reinterpret_cast< const char* >( mip->data( ) ), static_cast< std::streamsize >( mip->size( ) ) );
			if ( !out.good( ) ) {
				out.close( );
				DeleteFileA( temp.c_str( ) );
				return false;
			}
		}
		return MoveFileExA( temp.c_str( ), file.c_str( ), MOVEFILE_REPLACE_EXISTING ) != 0;
	}

	/* png / jpg / bmp / gif ( 1st frame ) -> premultiplied BGRA, fit centered in a square power of two 64..1024 */
	bool decode( const std::vector< std::uint8_t >& file, std::vector< std::uint8_t >& out, int& size )
	{
		ULONG_PTR token = 0;
		const Gdiplus::GdiplusStartupInput input{ };
		if ( Gdiplus::GdiplusStartup( &token, &input, nullptr ) != Gdiplus::Ok )
			return false;

		bool ok = false;
		if ( IStream* const stream = SHCreateMemStream( file.data( ), static_cast< UINT >( file.size( ) ) ) ) {
			{
				Gdiplus::Bitmap source( stream );
				const UINT width = source.GetWidth( ), height = source.GetHeight( );

				if ( source.GetLastStatus( ) == Gdiplus::Ok && width && height && width <= 4096 && height <= 4096 ) { // 32 bit VA: 4096^2 decodes to 64 mb
					const UINT longest = ( std::max )( width, height );
					size               = k_min_size;
					while ( size < k_max_size && static_cast< UINT >( size ) < longest )
						size *= 2;

					const INT scaled_w = ( std::max )( 1, static_cast< INT >( static_cast< unsigned long long >( width ) * size / longest ) );
					const INT scaled_h = ( std::max )( 1, static_cast< INT >( static_cast< unsigned long long >( height ) * size / longest ) );

					Gdiplus::Bitmap target( size, size, PixelFormat32bppPARGB );
					{
						Gdiplus::Graphics graphics( &target );
						graphics.Clear( Gdiplus::Color( 0, 0, 0, 0 ) );
						graphics.SetInterpolationMode( Gdiplus::InterpolationModeHighQualityBicubic );
						graphics.SetPixelOffsetMode( Gdiplus::PixelOffsetModeHalf );

						// no fade into the cleared border at the image edge
						Gdiplus::ImageAttributes wrap;
						wrap.SetWrapMode( Gdiplus::WrapModeTileFlipXY );
						ok = graphics.DrawImage( &source, Gdiplus::Rect( ( size - scaled_w ) / 2, ( size - scaled_h ) / 2, scaled_w, scaled_h ), 0, 0,
						                         static_cast< INT >( width ), static_cast< INT >( height ), Gdiplus::UnitPixel, &wrap ) == Gdiplus::Ok;
					}

					// PARGB in memory = B G R A premultiplied
					Gdiplus::BitmapData bits{ };
					const Gdiplus::Rect all( 0, 0, size, size );
					if ( ok && target.LockBits( &all, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &bits ) == Gdiplus::Ok ) {
						out.resize( static_cast< std::size_t >( size ) * size * 4 );
						for ( int y = 0; y < size; y++ )
							std::memcpy( &out[ static_cast< std::size_t >( y ) * size * 4 ], static_cast< const std::uint8_t* >( bits.Scan0 ) + y * bits.Stride,
							             static_cast< std::size_t >( size ) * 4 );
						target.UnlockBits( &bits );
					} else
						ok = false;
				}
			}
			stream->Release( );
		}

		Gdiplus::GdiplusShutdown( token );
		return ok;
	}

	// stock paper sticker ( community01/skull.vmt ) on our texture = stock wear / scratches / peel
	std::string sticker_vmt( const std::string& texture )
	{
		return "\"WeaponDecal\"\n{\n"
		       "\t\"$basetexture\" \"" + texture + "\"\n"
		       "\t\"$decalstyle\" \"1\"\n"
		       "\t\"$unwearstrength\" \"0.2\"\n"
		       "\t\"$envmap\" \"env_cubemap\"\n"
		       "\t\"$envmaptint\" \"[ .07 .07 .07 ]\"\n"
		       "\t\"$phong\" \"1\"\n"
		       "\t\"$phongexponent\" \"2\"\n"
		       "\t\"$phongfresnelranges\" \"[ .2 .5 1 ]\"\n"
		       "\t\"$phongalbedotint\" \"1\"\n"
		       "\t\"$phongboost\" \"1\"\n"
		       "\t\"$phongalbedoboost\" \"10\"\n"
		       "}\n";
	}

	// same file saved over = new key = imported again
	std::string cache_key( const std::string& path )
	{
		std::error_code ec;
		const auto stamp = std::filesystem::last_write_time( path, ec );
		return path + '|' + std::to_string( ec ? 0 : stamp.time_since_epoch( ).count( ) );
	}

	void import( const std::string& path, image_t& image )
	{
		if ( path.find( "://" ) != std::string::npos ) {
			image.m_status = "local files only ( no links )";
			return;
		}

		std::vector< std::uint8_t > file;
		{
			std::ifstream in( path, std::ios::binary | std::ios::ate );
			const auto bytes = in ? static_cast< long long >( in.tellg( ) ) : -1;
			if ( bytes <= 0 || bytes > 64ll * 1024 * 1024 ) {
				image.m_status = "can't read file";
				return;
			}
			file.resize( static_cast< std::size_t >( bytes ) );
			in.seekg( 0 );
			if ( !in.read( reinterpret_cast< char* >( file.data( ) ), bytes ) ) {
				image.m_status = "can't read file";
				return;
			}
		}

		// name = image bytes: a new picture never hits the game's material cache of an old one ( names, not paths )
		unsigned int hash = 2166136261u ^ 0x62783176u; // "bx1v", bump on a vtf format change
		for ( const std::uint8_t byte : file ) {
			hash ^= byte;
			hash *= 16777619u;
		}
		hash |= 1; // 0 = free carrier

		char name[ 16 ];
		std::snprintf( name, sizeof( name ), "%08x", hash );
		const std::string texture = std::string( "botox_stickers/" ) + name;

		const std::string dir = vtf_dir( );
		std::error_code ec;
		std::filesystem::create_directories( dir, ec );
		const std::string vtf = dir + name + ".vtf";

		if ( std::filesystem::file_size( vtf, ec ) <= 80 || ec ) {
			std::vector< std::uint8_t > pixels;
			int size = 0;
			if ( !decode( file, pixels, size ) ) {
				image.m_status = "can't decode image ( png / jpg / bmp / gif, max 4096 px )";
				return;
			}
			if ( dir.empty( ) || !write_vtf( vtf, size, build_mips( std::move( pixels ), size ) ) ) {
				image.m_status = "can't write csgo\\materials\\botox_stickers";
				g_logger.print( "can't write " + vtf + "\n", "[sticker]" );
				return;
			}
		}

		if ( !g_carriers_scanned )
			scan_carriers( );

		carrier_t* carrier = nullptr;
		for ( auto& entry : g_carriers ) {
			if ( entry.m_hash == hash ) {
				carrier = &entry;
				break;
			}
			if ( !carrier && !entry.m_hash )
				carrier = &entry;
		}
		if ( !carrier ) {
			image.m_status = g_carriers.empty( ) ? "sticker kits not found ( game update? )" : "too many imported stickers, reinject";
			return;
		}

		if ( carrier->m_hash != hash ) {
			if ( !c_key_values::usable( ) ) {
				image.m_status = "keyvalues sig missing";
				return;
			}
			if ( !carrier->m_vmt )
				carrier->m_vmt = new c_key_values( "WeaponDecal" );
			carrier->m_vmt->load_from_buffer( "botox_sticker", sticker_vmt( texture ).c_str( ) );

			const std::string kit_path = std::string( "bx/" ) + name;
			write_path( carrier->m_kit, kit_path, static_cast< int >( kit_path.size( ) ) + 1 );
			*reinterpret_cast< c_key_values** >( carrier->m_kit + KIT_VMT ) = carrier->m_vmt;
			carrier->m_hash                                                 = hash;
		}

		image.m_kit = carrier->m_id;

		c_texture* const loaded = g_interfaces.m_material_system->find_texture( texture.c_str( ), TEXTURE_GROUP_MODEL, false );
		image.m_status          = loaded && !loaded->is_error( ) ? "ready" : "csgo won't load it ( sv_pure server: turn on sv_pure bypass )";
	}
}

int n_sticker_import::kit_for( const std::string& path )
{
	if ( path.empty( ) || g_ctx.m_eject_restoring )
		return 0;

	std::lock_guard< std::mutex > lock( g_mutex );

	auto [ it, fresh ] = g_images.try_emplace( cache_key( path ) );
	if ( fresh )
		import( path, it->second );

	const int kit = it->second.m_kit;
	if ( !kit )
		return 0;

	// schema rebuilt under us ( kit freed ) = never write there again
	for ( const auto& carrier : g_carriers ) {
		if ( carrier.m_id == kit && sticker_kit( kit ) != carrier.m_kit ) {
			it->second.m_kit    = 0;
			it->second.m_status = "game reloaded sticker kits, reinject";
			return 0;
		}
	}
	return kit;
}

std::string n_sticker_import::status( const std::string& path )
{
	const std::string key = cache_key( path );
	std::lock_guard< std::mutex > lock( g_mutex );
	const auto it = g_images.find( key );
	return it == g_images.end( ) ? "loads when a gun with it spawns" : it->second.m_status;
}

void n_sticker_import::eject_restore( )
{
	if ( !g_ctx.m_eject_restoring || g_restored )
		return;

	std::lock_guard< std::mutex > lock( g_mutex );
	g_restored = true;

	// ponytail: our KeyValues leak on eject ( 0x2C bytes each, game materials already copied them )
	for ( auto& carrier : g_carriers ) {
		if ( !carrier.m_hash || sticker_kit( carrier.m_id ) != carrier.m_kit ||
		     *reinterpret_cast< c_key_values** >( carrier.m_kit + KIT_VMT ) != carrier.m_vmt )
			continue;

		write_path( carrier.m_kit, carrier.m_original, carrier.m_original_length );
		*reinterpret_cast< void** >( carrier.m_kit + KIT_VMT ) = nullptr;
		carrier.m_hash                                         = 0;
	}
}
