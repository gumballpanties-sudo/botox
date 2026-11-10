#include "old_mods.h"
#include "old_mods_manifest.h"
#include "vpk_writer.h"
#include "../misc/misc.h"
#include "../misc/scaleform/image_cache.h"
#include "../skins/skins_internal.h"
#include "players/esp_preview.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../globals/globals.h"
#include "../../utilities/perf/perf_watch.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	using namespace n_old_mods_manifest;

	constexpr const char* k_base_url  = "https://raw.githubusercontent.com/gumballpanties-sudo/botox-assets/main/old/";
	constexpr const char* k_cache_dir = "C:\\botox\\old\\";
	constexpr int k_fetch_threads     = 8;
	constexpr int k_fail_logs         = 4;
	constexpr int k_serial_frames     = 3; // CMaterialSystem::EndFrame applies mat_queue_mode, then the queue is empty

	constexpr bool same( const char* a, const char* b )
	{
		for ( ; *a && *a == *b; ++a, ++b ) { }
		return *a == *b;
	}

	// toggle( ) index = manifest row: tools/old_mods_gen.py PACKS order
	static_assert( k_pack_count == n_old_mods::k_max_packs );
	static_assert( same( k_packs[ 0 ].m_dir, "p_oldplayer" ) && same( k_packs[ 1 ].m_dir, "p_old_csgo_sounds" ) &&
	               same( k_packs[ 2 ].m_dir, "p_old_csgo_ranks" ) && same( k_packs[ 3 ].m_dir, "p_old_csgo_miscellaneous" ) &&
	               same( k_packs[ 4 ].m_dir, "p_old_csgo_scaleform_inventory_inspect" ) );

	/* CBaseFileSystem open: m_VPKFiles ( CUtlVector at +0x3c4, size +0x3d0 ) is asked in order before any search path, so a loose dir
	   never beats pak01. IFileSystem 112 AddVPKFile( path, 0 = head ), filesystem_stdio.dll binary-checked 11-10.
	   NEVER 113 RemoveVPKFile: it deletes the CPackedStore while open handles ( sounds, server, async loads ) still use it = eip 0 crash.
	   off = our store moved behind pak01 instead, it stays alive for good */
	struct vpk_list_t {
		std::uint8_t** m_memory;
		int m_allocated;
		int m_grow;
		int m_size;
	};

	vpk_list_t& vpk_list( )
	{
		return *reinterpret_cast< vpk_list_t* >( static_cast< std::uint8_t* >( g_interfaces.m_file_system ) + 0x3c4 );
	}

	// CPackedStore +0x108 = full path char array, what AddVPKFile compares its normalized name against
	int find_vpk( const std::string& path )
	{
		const vpk_list_t& list = vpk_list( );
		for ( int i = 0; i < list.m_size; ++i )
			if ( list.m_memory[ i ] && !_stricmp( reinterpret_cast< const char* >( list.m_memory[ i ] + 0x108 ), path.c_str( ) ) )
				return i;
		return -1;
	}

	// in place: no free, no realloc, readers on other threads only ever see live stores
	void place_vpk( const std::string& path, bool front )
	{
		const int from = find_vpk( path );
		if ( from < 0 ) {
			if ( front )
				g_virtual.call< void >( g_interfaces.m_file_system, 112, path.c_str( ), 0 );
			botox_dbg_log( "OLD: added %s found=%d", path.c_str( ), find_vpk( path ) );
			return;
		}

		vpk_list_t& list     = vpk_list( );
		std::uint8_t* const store = list.m_memory[ from ];
		const int to         = front ? 0 : list.m_size - 1;
		for ( int i = from; i < to; ++i )
			list.m_memory[ i ] = list.m_memory[ i + 1 ];
		for ( int i = from; i > to; --i )
			list.m_memory[ i ] = list.m_memory[ i - 1 ];
		list.m_memory[ to ] = store;
		botox_dbg_log( "OLD: moved %s %d -> %d of %d", path.c_str( ), from, to, list.m_size );
	}

	std::string pack_dir( int pack )
	{
		return std::string( k_cache_dir ) + k_packs[ pack ].m_dir + "\\";
	}

	std::string vpk_path( int pack )
	{
		std::string path = std::string( k_cache_dir ) + k_packs[ pack ].m_dir + "_" + k_packs[ pack ].m_stamp + ".vpk";
		std::transform( path.begin( ), path.end( ), path.begin( ), []( unsigned char c ) { return static_cast< char >( std::tolower( c ) ); } );
		return path;
	}

	std::string backslash( std::string s )
	{
		std::replace( s.begin( ), s.end( ), '/', '\\' );
		return s;
	}

	int frame_count( )
	{
		return g_interfaces.m_global_vars_base ? g_interfaces.m_global_vars_base->m_frame_count : 0;
	}

	/* the old player mdls predate BONE_ALWAYS_SETUP ( 0x80000 ): stock + animset_t / animset_ct flag these 22 bones, the old ones none.
	   bone merge ( CMergedMDL = esp preview, main menu ) skips unflagged bones = body never posed, invisible. in game entities set up
	   their own bones. mstudiobone_t: 216 bytes, sznameindex +0, flags +160; studiohdr_t numbones +0x9C, boneindex +0xA0 */
	void always_setup_bones( const std::string& rel, std::vector< char >& data )
	{
		static constexpr const char* k_bones[] = { "ankle_l",    "ankle_r",    "arm_lower_l", "arm_upper_l", "ball_l",       "ball_r",
			                                       "cam_driver", "clavicle_l", "hand_l",      "lean_root",   "leg_lower_l",  "leg_lower_r",
			                                       "leg_upper_l", "leg_upper_r", "lfoot_lock", "lh_ik_driver", "pelvis",      "rfoot_lock",
			                                       "spine_0",    "spine_1",    "spine_2",     "spine_3" };

		if ( !rel.starts_with( "models/player/" ) || !rel.ends_with( ".mdl" ) || data.size( ) < 0xA4 )
			return;

		const auto read_int = [ & ]( size_t at ) { return at + 4 <= data.size( ) ? *reinterpret_cast< const int* >( data.data( ) + at ) : -1; };
		const int count = read_int( 0x9C ), first = read_int( 0xA0 );
		for ( int i = 0; i < count && first > 0; ++i ) {
			const size_t bone = static_cast< size_t >( first ) + static_cast< size_t >( i ) * 216;
			const int name    = read_int( bone );
			if ( bone + 216 > data.size( ) || name <= 0 || bone + name >= data.size( ) )
				break;

			const char* const text = data.data( ) + bone + name;
			const size_t length    = strnlen( text, data.size( ) - bone - name );
			for ( const char* wanted : k_bones ) {
				if ( std::strlen( wanted ) == length && !_strnicmp( text, wanted, length ) ) {
					*reinterpret_cast< int* >( data.data( ) + bone + 160 ) |= 0x80000;
					break;
				}
			}
		}
	}

	bool build_vpk( int pack )
	{
		std::vector< std::string > files{ };
		for ( int i = 0; i < k_packs[ pack ].m_count; ++i )
			files.emplace_back( k_packs[ pack ].m_files[ i ].m_path );
		std::vector< std::pair< std::string, std::string > > aliases{ };
		for ( int i = 0; i < k_packs[ pack ].m_alias_count; ++i )
			aliases.emplace_back( k_packs[ pack ].m_aliases[ i ].m_path, k_packs[ pack ].m_aliases[ i ].m_target );
		return n_vpk_writer::write( vpk_path( pack ), pack_dir( pack ), files, aliases, &always_setup_bones );
	}

	void download( int pack )
	{
		n_perf::background_thread( );

		n_old_mods::pack_state_t& state = g_old_mods.m_packs[ pack ];
		const pack_t& info              = k_packs[ pack ];

		std::atomic< int > next{ 0 }, missing{ 0 }, logs{ 0 };
		std::vector< std::thread > pool{ };
		for ( int t = 0; t < k_fetch_threads; ++t )
			pool.emplace_back( [ & ] {
				n_perf::background_thread( );
				std::error_code ec{ };
				for ( int i; ( i = next++ ) < info.m_count; ) {
					if ( g_ctx.m_unloading ) {
						++missing;
						continue;
					}

					const file_t& file     = info.m_files[ i ];
					const std::string path = pack_dir( pack ) + backslash( file.m_path );
					if ( !std::filesystem::exists( path, ec ) ) {
						std::filesystem::create_directories( std::filesystem::path( path ).parent_path( ), ec );
						if ( !n_image_cache::download_file( std::string( k_base_url ) + info.m_dir + "/" + file.m_path, path ) ) {
							++missing;
							if ( logs++ < k_fail_logs )
								botox_dbg_log( "OLD: download failed %s/%s", info.m_dir, file.m_path );
							continue;
						}
					}
					state.m_bytes_done += file.m_size;
				}
			} );
		for ( std::thread& t : pool )
			t.join( );

		const bool packed = missing == 0 && !g_ctx.m_unloading && build_vpk( pack );
		state.m_missing   = missing.load( );
		state.m_state     = packed ? n_old_mods::state_ready : n_old_mods::state_failed;
		if ( missing )
			botox_dbg_log( "OLD: %s %d/%d files missing", info.m_dir, missing.load( ), info.m_count );
		else if ( !packed )
			botox_dbg_log( "OLD: %s vpk build FAILED %s", info.m_dir, vpk_path( pack ).c_str( ) );
		--g_url_image_downloads;
	}

	// runs every reload command even on sv_cheats 0: raw value write, no SV_CheatsChanged_f ( = no RevertAllModifiedLocalState )
	void run_cheat_cmd( const std::string& cmd )
	{
		c_cconvar* cheats = g_interfaces.m_convar->find_var( "sv_cheats" );
		const int old     = cheats ? cheats->get_int( ) : 0;
		if ( cheats )
			cheats->force_value( 1.f );
		g_interfaces.m_engine_client->execute_client_cmd( cmd.c_str( ) );
		if ( cheats )
			cheats->force_value( static_cast< float >( old ) );
	}

	struct kinds_t {
		bool m_sounds = false;
		std::set< std::string > m_materials{ };
		std::vector< std::string > m_models{ }, m_images{ }, m_particles{ };
	};

	kinds_t kinds_of( const std::vector< int >& changed )
	{
		kinds_t out{ };
		for ( const int pack : changed ) {
			for ( int i = 0; i < k_packs[ pack ].m_alias_count; ++i )
				if ( std::string_view( k_packs[ pack ].m_aliases[ i ].m_path ).ends_with( ".mdl" ) )
					out.m_models.emplace_back( k_packs[ pack ].m_aliases[ i ].m_path );
			for ( int i = 0; i < k_packs[ pack ].m_count; ++i ) {
				const std::string_view path = k_packs[ pack ].m_files[ i ].m_path;
				if ( path.starts_with( "models/" ) ) {
					if ( path.ends_with( ".mdl" ) )
						out.m_models.emplace_back( path );
				}
				else if ( path.starts_with( "sound/" ) )
					out.m_sounds = true;
				else if ( path.starts_with( "particles/" ) && path.ends_with( ".pcf" ) )
					out.m_particles.emplace_back( path );
				else if ( path.starts_with( "materials/panorama/images/" ) || path.starts_with( "resource/flash/" ) )
					out.m_images.emplace_back( path );
				else if ( path.starts_with( "materials/" ) )
					out.m_materials.emplace( path.substr( 10, path.rfind( '/' ) - 10 ) );
			}
		}
		return out;
	}

	// panorama keeps decoded images per path: CUIImageManager::ReloadChangedFile, key = backslashed path, .svg cached as .vsvg
	void reload_images( const std::vector< std::string >& images )
	{
		auto engine  = g_interfaces.m_panorama ? g_interfaces.m_panorama->access_ui_engine( ) : nullptr;
		auto manager = engine ? engine->ui_image_manager( ) : nullptr;
		if ( !manager )
			return;

		for ( std::string key : images ) {
			key = backslash( key );
			if ( key.ends_with( ".svg" ) )
				key.replace( key.size( ) - 4, 4, ".vsvg" );
			manager->reload_changed_file( key.c_str( ) );
		}
	}

	// Studio_ReloadModels' mask: MDLCACHE_FLUSH_ALL minus IGNORELOCK ( 0x8000 ) and VCOLLIDE ( 0x4, vphysics holds it )
	constexpr int k_flush_model = 0xFFFF & ~( 0x8000 | 0x4 );

	/* listen server = one model cache with the client. InvalidateMdlCache alone ( knife anims ) keeps the bone cache sized for the
	   old model: old / new player models differ in bone count = null bone read in the next hitbox trace ( server.dll+0x648ee4 crash ).
	   CBaseAnimating::SetModel( same name ) = Studio_DestroyBoneCache + InvalidateMdlCache + bone controllers + pose params.
	   live server.dll: IServerTools FirstEntity 7 / NextEntity 8, IServerEntity GetModelIndex 6, CBaseEntity::SetModel 27
	   ( "Setting CBaseAnimating to non-studio model" fn, slot 27 in every vtable holding it ), binary-checked 11-10 */
	/* server indexes come from the SERVER table: the agent changer adds client-only precache slots, so a client index can name
	   another model there ( missed holder = freed studiohdr in a bot's next SetModel ). match the shared model_t through
	   VModelInfoServer002 ( IVModelInfo base slots = c_model_info's 1..3 ) */
	/* SetModel opens with UnlockStudioHdr = reads the OLD hdr's studiohdr / virtual model: after the flush that's freed memory
	   ( 18:56:33 datacache.dll+0xae1d crash, unmount ). so hdrs drop BEFORE the flush ( 200 = CBaseAnimating::InvalidateMdlCache,
	   what the `flush` command + knife_anim_live do ), SetModel after. 200 only exists on CBaseAnimating: filtered like the
	   engine's own dynamic_cast. live server.dll 11-10: slot 200 = +0x16bc80 in CBaseAnimating / Overlay / CCSPlayer / CCSBot /
	   CCSRagdoll vtables */
	struct server_holder_t {
		void* m_entity;
		const char* m_name;
	};

	// MSVC x86 RTTI: vtable[ -1 ] = complete object locator, +16 hierarchy ( +8 base count, +12 base array ), base +0 type, name +8
	bool is_animating( void* entity )
	{
		static std::unordered_map< const void*, bool > known{ };
		const auto vtable = *static_cast< const std::uintptr_t* const* >( entity );
		if ( const auto it = known.find( vtable ); it != known.end( ) )
			return it->second;

		const auto col       = reinterpret_cast< const std::uint8_t* >( vtable[ -1 ] );
		const auto hierarchy = *reinterpret_cast< const std::uint8_t* const* >( col + 16 );
		const int count      = *reinterpret_cast< const int* >( hierarchy + 8 );
		const auto bases     = *reinterpret_cast< const std::uint8_t* const* const* >( hierarchy + 12 );
		bool animating       = false;
		for ( int i = 0; i < count && !animating; ++i )
			animating = !strcmp( *reinterpret_cast< const char* const* >( bases[ i ] ) + 8, ".?AVCBaseAnimating@@" );
		return known[ vtable ] = animating;
	}

	std::vector< server_holder_t > drop_server_models( const std::unordered_set< const model_t* >& targets )
	{
		static void* tools = nullptr;
		static c_model_info* info = nullptr;
		if ( !tools ) {
			if ( void* sv = GetModuleHandleA( "server.dll" ) )
				tools = module_t( sv, "server.dll" ).find_interface( "VSERVERTOOLS001" );
			info = static_cast< c_model_info* >( g_modules[ ENGINE_DLL ].find_interface( "VModelInfoServer002" ) );
			if ( !tools || !info ) {
				tools = nullptr;
				return { };
			}
		}

		std::vector< server_holder_t > holders{ };
		for ( void* entity = g_virtual.call< void* >( tools, 7 ); entity; entity = g_virtual.call< void*, void* >( tools, 8, entity ) ) {
			const model_t* model = info->get_model( g_virtual.call< int >( entity, 6 ) );
			if ( !model || !targets.count( model ) || !is_animating( entity ) )
				continue;
			const char* name = info->get_model_name( model );
			if ( !name || !*name )
				continue;
			g_virtual.call< void >( entity, 200 );
			holders.push_back( { entity, name } );
		}
		return holders;
	}

	// same thread as the server frame, nothing of it ran since the drop: no entity freed, the model_t names stay
	void reset_server_models( const std::vector< server_holder_t >& holders )
	{
		for ( const server_holder_t& holder : holders )
			g_virtual.call< void, const char* >( holder.m_entity, 27, holder.m_name );
	}

	/* skins knife_anim_live's live model change, for same-name models whose file just changed underneath: every holder to model 0
	   ( OnNewModel deletes its CStudioHdr, bone merge caches included ), flush the handle, model back ( fresh hdr off the new file ).
	   r_flushlod never re-runs OnNewModel: arms bone merge + entity caches kept the old model */
	void swap_models( const std::vector< std::string >& models )
	{
		if ( !g_interfaces.m_model_cache )
			return;

		std::unordered_set< const model_t* > targets{ };
		std::unordered_set< int > indexes{ };
		for ( const std::string& path : models ) {
			const int index = g_interfaces.m_model_info->get_model_index( path.c_str( ) );
			if ( index <= 0 )
				continue;
			if ( const model_t* model = g_interfaces.m_model_info->get_model( index ) ) {
				targets.insert( model );
				indexes.insert( index );
			}
		}

		struct held_t {
			c_base_entity* m_entity;
			int m_index;
		};
		std::vector< held_t > held{ };
		if ( g_interfaces.m_engine_client->is_in_game( ) ) {
			for ( int e = 1; e <= g_interfaces.m_client_entity_list->get_highest_entity_index( ); e++ ) {
				const auto entity = g_interfaces.m_client_entity_list->get< c_base_entity >( e );
				if ( !entity || !targets.count( entity->get_model( ) ) )
					continue;
				held.push_back( { entity, entity->get_model_index( ) } );
				entity->set_model_index( 0 );
			}
		}

		if ( !models.empty( ) )
			g_esp_preview.rebuild( );

		const auto nci = g_interfaces.m_engine_client->get_net_channel_info( );
		const std::vector< server_holder_t > server =
			nci && nci->is_loopback( ) && !targets.empty( ) ? drop_server_models( targets ) : std::vector< server_holder_t >{ };

		int flushed = 0;
		for ( const std::string& path : models ) {
			const unsigned short handle = g_interfaces.m_model_cache->find_mdl( path.c_str( ) );
			if ( handle == 0xffff )
				continue;
			g_interfaces.m_model_cache->flush( handle, k_flush_model );
			g_interfaces.m_model_cache->release( handle );
			flushed++;
		}

		/* skins knife_anim_live: a freed virtualmodel_t ( 0xa8 ) goes straight back to the next 0xa8 alloc = the rebuild, and a
		   same address graph keeps every activity / sequence map ( studio.cpp ValidateAgainst ) on the old model = wrong anims,
		   twisted bodies. hold the freed slots while everything rebuilds. tier0 g_pMemAlloc Alloc 1 / Free 5 */
		constexpr std::size_t k_virtual_model_size = 0xa8;
		static void** const mem_alloc = [ ]( ) -> void** {
			const auto tier0 = GetModuleHandleA( "tier0.dll" );
			return tier0 ? reinterpret_cast< void** >( GetProcAddress( tier0, "g_pMemAlloc" ) ) : nullptr;
		}( );
		void* const allocator = mem_alloc ? *mem_alloc : nullptr;
		std::vector< void* > decoys( allocator ? static_cast< size_t >( flushed ) * 2 + 16 : 0 );
		for ( void*& decoy : decoys )
			decoy = g_virtual.call< void*, std::size_t >( allocator, 1, k_virtual_model_size );

		reset_server_models( server );

		for ( const held_t& entry : held )
			entry.m_entity->set_model_index( entry.m_index );

		if ( g_ctx.m_local && g_ctx.m_local->is_alive( ) )
			rebuild_player_arms( g_ctx.m_local, nullptr, "old csgo" );

		// graphs are rebuilt lazily on first use: force the held ones now, while the decoys still sit on the old addresses
		for ( const held_t& entry : held ) {
			const model_t* model = entry.m_entity->get_model( );
			const unsigned short handle = model ? g_interfaces.m_model_cache->find_mdl( g_interfaces.m_model_info->get_model_name( model ) ) : 0xffff;
			if ( handle == 0xffff )
				continue;
			g_interfaces.m_model_cache->get_virtual_model( handle );
			g_interfaces.m_model_cache->release( handle );
		}
		for ( void* decoy : decoys )
			if ( decoy )
				g_virtual.call< void, void* >( allocator, 5, decoy );

		botox_dbg_log( "OLD: models flushed=%d loaded=%d held=%d server=%d", flushed, static_cast< int >( indexes.size( ) ),
		               static_cast< int >( held.size( ) ), static_cast< int >( server.size( ) ) );
	}

	c_cconvar* queue_mode( )
	{
		return g_interfaces.m_convar->find_var( "mat_queue_mode" );
	}

	/* IBaseFileSystem = IFileSystem +4: 0 Read( out, size, h ), 2 Open( name, options, path ), 3 Close( h ), 7 Size( h )
	   ( overloads reversed, 6 = Size( name, path ) ), filesystem_stdio.dll binary-checked 11-10 */
	bool read_game_file( const std::string& rel, std::vector< char >& out )
	{
		void* const fs = static_cast< std::uint8_t* >( g_interfaces.m_file_system ) + 4;
		void* const handle = g_virtual.call< void* >( fs, 2, rel.c_str( ), "rb", "GAME" );
		if ( !handle )
			return false;

		const unsigned int size = g_virtual.call< unsigned int >( fs, 7, handle );
		out.resize( size );
		const int got = size ? g_virtual.call< int >( fs, 0, static_cast< void* >( out.data( ) ), static_cast< int >( size ), handle ) : 0;
		g_virtual.call< void >( fs, 3, handle );
		return size && got == static_cast< int >( size );
	}

	// IClientTools 52 ReloadParticleDefintions( name, buf, len ), client.dll binary-checked 11-10 ( 2019 src = 51 ): live effects swap too
	void reload_particles( const std::vector< std::string >& files )
	{
		static void* const tools = g_modules[ CLIENT_DLL ].find_interface( "VCLIENTTOOLS001" );
		if ( !tools )
			return;

		for ( const std::string& rel : files ) {
			std::vector< char > buffer{ };
			if ( !read_game_file( rel, buffer ) ) {
				botox_dbg_log( "OLD: can't read %s", rel.c_str( ) );
				continue;
			}
			g_virtual.call< void >( tools, 52, rel.c_str( ), static_cast< const void* >( buffer.data( ) ), static_cast< int >( buffer.size( ) ) );
		}
	}
}

int n_old_mods::impl_t::count( )
{
	return k_pack_count;
}

const char* n_old_mods::impl_t::label( int pack )
{
	return k_packs[ pack ].m_label;
}

bool& n_old_mods::impl_t::toggle( int pack )
{
	switch ( pack ) {
	case 0: return GET_VARIABLE( g_variables.m_old_csgo_players, bool );
	case 1: return GET_VARIABLE( g_variables.m_old_csgo_sounds, bool );
	case 2: return GET_VARIABLE( g_variables.m_old_csgo_ranks, bool );
	case 3: return GET_VARIABLE( g_variables.m_old_csgo_misc, bool );
	default: return GET_VARIABLE( g_variables.m_old_csgo_inspect, bool );
	}
}

const char* n_old_mods::impl_t::preview_model( int& sequence ) const
{
	if ( !m_packs[ k_preview_pack ].m_mounted )
		return nullptr;
	sequence = k_preview_sequence;
	return k_preview_model;
}

float n_old_mods::impl_t::progress( int pack ) const
{
	return k_packs[ pack ].m_bytes ? std::min( 1.f, static_cast< float >( static_cast< double >( m_packs[ pack ].m_bytes_done ) /
	                                                                       static_cast< double >( k_packs[ pack ].m_bytes ) ) )
	                               : 1.f;
}

/* models keep stock names, so a loaded one only changes by flushing its data: run it with the material queue drained
   ( mat_queue_mode 0 ): queued draws from the last frame still point at the data a flush frees = studiorender crash */
void n_old_mods::impl_t::serial_step( )
{
	const int frame = frame_count( );
	if ( frame - m_serial_frame < k_serial_frames && frame >= m_serial_frame )
		return;

	if ( m_serial_phase == 1 ) {
		for ( const int pack : m_serial_remove )
			place_vpk( vpk_path( pack ), false );
		/* models first: mat_reloadmaterial skips unreferenced materials, so it must run once the swapped in models hold theirs
		   ( before = the other set's arm materials keep the last file they read, wrong textures on the next toggle ) */
		swap_models( m_serial_models );
		if ( !m_serial_cmd.empty( ) )
			run_cheat_cmd( m_serial_cmd );
		reload_particles( m_serial_particles );
		reload_images( m_serial_images );
		botox_dbg_log( "OLD: serial reload \"%s\" removed=%d particles=%d images=%d queue=%d", m_serial_cmd.c_str( ),
		               static_cast< int >( m_serial_remove.size( ) ), static_cast< int >( m_serial_particles.size( ) ),
		               static_cast< int >( m_serial_images.size( ) ), queue_mode( ) ? queue_mode( )->get_int( ) : -1 );
		m_serial_phase = 2;
		m_serial_frame = frame;
		return;
	}

	if ( c_cconvar* mode = queue_mode( ); mode && m_queue_backup != k_untouched )
		mode->set_value( m_queue_backup );
	m_queue_backup = k_untouched;
	m_serial_phase = 0;
	m_serial       = false;
}

void n_old_mods::impl_t::on_frame( )
{
	if ( !g_interfaces.m_file_system )
		return;

	if ( m_serial_phase ) {
		serial_step( );
		return;
	}

	if ( m_any_mounted && g_ctx.m_local ) {
		static const model_t* logged = nullptr;
		if ( const model_t* model = g_ctx.m_local->get_model( ); model && model != logged ) {
			logged = model;
			botox_dbg_log( "OLD: local team=%d model=%s agent=%d gloves=%d", g_ctx.m_local->get_team( ), g_interfaces.m_model_info->get_model_name( model ),
			               GET_VARIABLE( g_variables.m_agent_enable, bool ) ? 1 : 0, GET_VARIABLE( PLAYER_VAR( m_gloves_enable ), bool ) ? 1 : 0 );
		}
	}

	const bool eject  = g_ctx.m_eject_restoring;
	const bool master = GET_VARIABLE( g_variables.m_old_csgo, bool ) && !GET_VARIABLE( g_variables.m_safe_mode, bool ) && !eject;

	std::vector< int > changed{ };
	for ( int i = 0; i < k_pack_count; ++i ) {
		pack_state_t& state = m_packs[ i ];
		const bool want     = master && toggle( i );

		if ( want && state.m_state == state_idle ) {
			state.m_bytes_done = 0;
			state.m_missing    = 0;
			state.m_state      = state_downloading;
			++g_url_image_downloads; // before the thread runs: an eject right now must still wait for it
			std::thread( download, i ).detach( );
		}
		else if ( !want && state.m_state == state_failed )
			state.m_state = state_idle;

		if ( ( want && state.m_state == state_ready ) != state.m_mounted )
			changed.push_back( i );
	}

	if ( changed.empty( ) )
		return;

	m_any_mounted = true; // before the mount: async loaders may already ask for a pack file
	m_serial_remove.clear( );
	for ( const int i : changed ) {
		pack_state_t& state = m_packs[ i ];
		state.m_mounted     = !state.m_mounted;
		if ( state.m_mounted )
			place_vpk( vpk_path( i ), true );
		else
			m_serial_remove.push_back( i );
		botox_dbg_log( "OLD: %s %s", state.m_mounted ? "mount" : "unmount", vpk_path( i ).c_str( ) );
	}

	const kinds_t kinds = kinds_of( changed );
	std::string cmd     = kinds.m_sounds ? "snd_restart;" : "";
	if ( !kinds.m_materials.empty( ) ) {
		std::string list{ };
		for ( const std::string& dir : kinds.m_materials )
			list += ( list.empty( ) ? "" : "*" ) + dir;
		cmd += "mat_reloadmaterial \"" + list + "\";";
	}

	m_serial           = true;
	m_serial_cmd       = cmd;
	m_serial_particles = kinds.m_particles;
	m_serial_models    = kinds.m_models;
	m_serial_images    = kinds.m_images;
	m_serial_phase     = 1;
	m_serial_frame     = frame_count( );
	if ( c_cconvar* mode = queue_mode( ) ) {
		m_queue_backup = mode->get_int( );
		mode->set_value( 0 );
	}

	m_any_mounted = std::any_of( std::begin( m_packs ), std::end( m_packs ), []( const pack_state_t& p ) { return p.m_mounted; } );
}
