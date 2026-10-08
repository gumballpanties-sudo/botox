#pragma once

enum e_image_format {
	image_format_unknown  = -1,
	image_format_rgba8888 = 0,
	image_format_abgr8888,
	image_format_rgb888,
	image_format_bgr888,
	image_format_rgb565,
	image_format_i8,
	image_format_ia88,
	image_format_p8,
	image_format_a8,
	image_format_rgb888_bluescreen,
	image_format_bgr888_bluescreen,
	image_format_argb8888,
	image_format_bgra8888,
	image_format_dxt1,
	image_format_dxt3,
	image_format_dxt5,
	image_format_bgrx8888,
	image_format_bgr565,
	image_format_bgrx5551,
	image_format_bgra4444,
	image_format_dxt1_onebitalpha,
	image_format_bgra5551,
	image_format_uv88,
	image_format_uvwq8888,
	image_format_rgba16161616f,
	image_format_rgba16161616,
	image_format_uvlx8888,
	image_format_r32f,       // single-channel 32-bit floating point
	image_format_rgb323232f, // note: d3d9 does not have this format
	image_format_rgba32323232f,
	image_format_rg1616f,
	image_format_rg3232f,
	image_format_rgbx8888,

	image_format_null,

	// compressed normal map formats
	image_format_ati2n, // one-surface ati2n / dxn format
	image_format_ati1n, // two-surface ati1n format

	image_format_rgba1010102, // 10 bit-per component render targets
	image_format_bgra1010102,
	image_format_r16f, // 16 bit fp format

	// depth-stencil texture formats
	image_format_d16,
	image_format_d15s1,
	image_format_d32,
	image_format_d24s8,
	image_format_linear_d24s8,
	image_format_d24x8,
	image_format_d24x4s4,
	image_format_d24fs8,
	image_format_d16_shadow,   // specific formats for shadow mapping
	image_format_d24x8_shadow, // specific formats for shadow mapping

	// supporting these specific formats as non-tiled for procedural cpu access (360-specific)
	image_format_linear_bgrx8888,
	image_format_linear_rgba8888,
	image_format_linear_abgr8888,
	image_format_linear_argb8888,
	image_format_linear_bgra8888,
	image_format_linear_rgb888,
	image_format_linear_bgr888,
	image_format_linear_bgrx5551,
	image_format_linear_i8,
	image_format_linear_rgba16161616,

	image_format_le_bgrx8888,
	image_format_le_bgra8888,

	num_image_formats
};