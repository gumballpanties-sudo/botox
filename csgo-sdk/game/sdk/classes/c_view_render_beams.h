#pragma once
#include "c_vector.h"

struct beam_info_t {
	int m_type              = 0; // TE_BEAMPOINTS
	void* m_start_entity    = nullptr;
	int m_start_attachment  = 0;
	void* m_end_entity      = nullptr;
	int m_end_attachment    = 0;
	c_vector m_start        = { };
	c_vector m_end          = { };
	int m_model_index       = -1;
	const char* m_model_name = nullptr;
	int m_halo_index        = -1;
	const char* m_halo_name = nullptr;
	float m_halo_scale      = 0.f;
	float m_life            = 0.f;
	float m_width           = 0.f;
	float m_end_width       = 0.f;
	float m_fade_length     = 0.f;
	float m_amplitude       = 0.f;
	float m_brightness      = 0.f;
	float m_speed           = 0.f;
	int m_start_frame       = 0;
	float m_frame_rate      = 0.f;
	float m_red             = 0.f;
	float m_green           = 0.f;
	float m_blue            = 0.f;
	bool m_renderable       = true;
	int m_segments          = -1;
	int m_flags             = 0;
	c_vector m_center       = { };
	float m_start_radius    = 0.f;
	float m_end_radius      = 0.f;
};

enum e_beam_flags : int {
	beam_flag_fade_in         = 0x4,
	beam_flag_only_noise_once = 0x100,
	beam_flag_no_tile         = 0x200,
};

/* IViewRenderBeams. MSVC groups each name's overloads in reverse, so CreateBeamPoints( BeamInfo_t& ) = 12.
   renderable beams are drawn by the engine, no DrawBeam call needed */
class c_view_render_beams
{
public:
	void* create_beam_points( beam_info_t* info )
	{
		using fn = void*( __thiscall* )( void*, beam_info_t* );
		return ( *reinterpret_cast< fn** >( this ) )[ 12 ]( this, info );
	}
};
