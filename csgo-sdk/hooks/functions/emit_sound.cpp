#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../hooks.h"

#include <cstring>

extern void botox_dbg_log( const char* fmt, ... );

/* returns the sound guid callers stop by later: garbage here = wrong sounds stopped. 0 = nothing played */
int __fastcall n_detoured_functions::emit_sound( void* ecx, void* edx, void* filter, int idx, int channel, const char* sound_entry,
                                                 unsigned int sound_entry_hash, const char* sample, float volume, int seed, float attenuation,
                                                 int flags, int pitch, const c_vector* origin, const c_vector* direction, void* vec_origins,
                                                 bool update_pos, float soundtime, int speakerentity, int unk )
{
	static auto original = g_hooks.m_emit_sound.get_original< decltype( &n_detoured_functions::emit_sound ) >( );
	HOOK_SCOPE_OR_BAIL( original( ecx, edx, filter, idx, channel, sound_entry, sound_entry_hash, sample, volume, seed, attenuation, flags, pitch,
	                              origin, direction, vec_origins, update_pos, soundtime, speakerentity, unk ) );

	/* SND_CHANGE_VOL | SND_CHANGE_PITCH | SND_STOP */
	constexpr int k_modify_flags = ( 1 << 0 ) | ( 1 << 1 ) | ( 1 << 2 );

	const bool drop = g_interfaces.m_prediction->m_in_prediction && !g_interfaces.m_prediction->m_is_first_time_predicted &&
	                  !( flags & k_modify_flags ) && g_interfaces.m_engine_client->is_in_game( );

	if ( const char* name = sound_entry ? sound_entry : sample; name && std::strstr( name, "Weapon_" ) )
		botox_dbg_log( "SND: %s drop=%d first=%d vol=%.2f pitch=%d flags=%x cur=%.3f", name, drop ? 1 : 0,
		               g_interfaces.m_prediction->m_is_first_time_predicted ? 1 : 0, volume, pitch, flags, g_interfaces.m_global_vars_base->m_current_time );

	if ( drop )
		return 0;

	return original( ecx, edx, filter, idx, channel, sound_entry, sound_entry_hash, sample, volume, seed, attenuation, flags, pitch, origin, direction,
	                 vec_origins, update_pos, soundtime, speakerentity, unk );
}
