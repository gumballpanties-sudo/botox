#pragma once
#include <d3d9.h>

namespace n_frame_dump
{
	/* false = not a 32 bit format, readback failed, or file wouldn't open.
	   d3d thread only; stalls the pipeline, one shot only. */
	bool save( IDirect3DDevice9* device, IDirect3DSurface9* surface, const char* file_name );
}
