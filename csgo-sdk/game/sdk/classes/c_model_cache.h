#pragma once
#include "../../../utilities/memory/virtual.h"

struct studiohdr_t;

class c_model_cache
{
public:
	void begin_lock( )
	{
		g_virtual.call< void >( this, 33 );
	}

	void end_lock( )
	{
		g_virtual.call< void >( this, 34 );
	}

	static constexpr int flush_virtual_model = 0x10;

	void flush( unsigned short handle, int flags )
	{
		g_virtual.call< void, unsigned short, int >( this, 28, handle, flags );
	}

	unsigned short find_mdl( const char* path )
	{
		return g_virtual.call< unsigned short, const char* >( this, 10, path );
	}

	// slot 12 (AddRef 11, Release 12, GetRef 13). drops what find_mdl took
	int release( unsigned short handle )
	{
		return g_virtual.call< int, unsigned short >( this, 12, handle );
	}

	/* slot 14 = FindMDL + 4 (SetCacheNotify, FindMDL, AddRef, Release, GetRef, GetStudioHdr). loads the mdl
	   if needed: game thread only, never from inside the FindMDL hook. */
	studiohdr_t* get_studio_hdr( unsigned short handle )
	{
		return g_virtual.call< studiohdr_t*, unsigned short >( this, 14, handle );
	}

	/* slot 22 = GetStudioHdr + 8 ( GetHardwareData, GetVCollide x2, GetAnimBlock, HasAnimBlockBeenPreloaded, GetVirtualModel,
	   GetAutoplayList ). the vvd ( vertexFileHeader_t ), loads it if evicted; may be null while it streams. hold begin_lock */
	const void* get_vertex_data( unsigned short handle )
	{
		return g_virtual.call< const void*, unsigned short >( this, 22, handle );
	}
};