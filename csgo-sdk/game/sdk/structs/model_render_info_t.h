#pragma once
#include "../../../globals/macros/macros.h"
#include "../classes/c_model_info.h"
#include "../classes/c_vector.h"

class i_client_renderable;

struct model_render_info_t {
	c_vector origin;
	c_vector angles;
	PAD( 0x4 );
	c_client_renderable* renderable;
	model_t* model;
	matrix3x4_t* model_to_world;
	matrix3x4_t* lighting_offset;
	c_vector* lighting_origin;
	int flags;
	int entity_index;
	int skin;
	int body;
	int hitbox_set;
	void* instance;
};
