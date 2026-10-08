#pragma once
#include <cstdint>

enum e_shader_stencil_op : int {
	shader_stencilop_keep              = 1,
	shader_stencilop_zero              = 2,
	shader_stencilop_set_to_reference  = 3,
	shader_stencilop_increment_clamp   = 4,
	shader_stencilop_decrement_clamp   = 5,
	shader_stencilop_invert            = 6,
	shader_stencilop_increment_wrap    = 7,
	shader_stencilop_decrement_wrap    = 8
};

enum e_shader_stencil_func : int {
	shader_stencilfunc_never    = 1,
	shader_stencilfunc_less     = 2,
	shader_stencilfunc_equal    = 3,
	shader_stencilfunc_lequal   = 4,
	shader_stencilfunc_greater  = 5,
	shader_stencilfunc_notequal = 6,
	shader_stencilfunc_gequal   = 7,
	shader_stencilfunc_always   = 8
};

struct shader_stencil_state_t {
	bool m_enable                     = false;
	e_shader_stencil_op m_fail_op     = shader_stencilop_keep;
	e_shader_stencil_op m_z_fail_op   = shader_stencilop_keep;
	e_shader_stencil_op m_pass_op     = shader_stencilop_keep;
	e_shader_stencil_func m_func      = shader_stencilfunc_always;
	int m_reference_value             = 0;
	std::uint32_t m_test_mask         = 0xffffffffU;
	std::uint32_t m_write_mask        = 0xffffffffU;
};
