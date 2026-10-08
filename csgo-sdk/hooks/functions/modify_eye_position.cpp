#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

/* callers: C_CSPlayer::CalcView ( camera, skipped = no head-height pull ) and Weapon_ShootPosition ( slot 285 ).
   the shoot position keeps the engine's own: it sets bones up, m_CachedBoneData of the first-person local is stale */
void __fastcall n_detoured_functions::modify_eye_position( c_animation_state* anim_state, void* edx, c_vector& input_eye_pos )
{
	static auto original = g_hooks.m_modify_eye_position.get_original< void( __thiscall* )( void*, std::reference_wrapper< const c_vector > ) >( );
	HOOK_SCOPE_OR_BAIL( original( anim_state, input_eye_pos ) );

	static auto calc_view_return_address = reinterpret_cast< void* >( g_modules[ CLIENT_DLL ].find_pattern( "8B ? ? ? ? ? 30 ? ? ? ? C0 ? 50 " ) );

	if ( g_ctx.m_local && anim_state && _ReturnAddress( ) == calc_view_return_address )
		return;

	return original( anim_state, input_eye_pos );
}
