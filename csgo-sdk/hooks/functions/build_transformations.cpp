#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

#include "../../hacks/lua/lua_api.h"

/* C_BaseAnimating::SetupBones builds attachments right after this ( csgosrc2019 c_baseanimating.cpp:3398 then :3442 ), so a pose
   written here reaches the bone cache, attachments, bone merged children and parented props alike. CStudioHdr has no vtable,
   its studiohdr_t* is the first member */
void __fastcall n_detoured_functions::build_transformations( void* ecx, void* edx, void* studio_hdr, void* pos, void* q,
                                                             const matrix3x4_t& camera_transform, int bone_mask, void* bone_computed )
{
	static auto original = g_hooks.m_build_transformations.get_original< decltype( &n_detoured_functions::build_transformations ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, studio_hdr, pos, q, camera_transform, bone_mask, bone_computed ) );

	original( ecx, edx, studio_hdr, pos, q, camera_transform, bone_mask, bone_computed );

	if ( ecx && studio_hdr )
		n_lua_api::pose_entity_bones( ecx, *static_cast< studiohdr_t* const* >( studio_hdr ), camera_transform, bone_mask );
}
