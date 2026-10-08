#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../../hacks/menu/menu.h"
#include "../../hacks/visuals/players/sound_esp/sound_esp.h"
#include "../../hacks/visuals/screen/player_stencil.h"
#include "../hooks.h"
#include <algorithm>
#include <cmath>

bool __cdecl n_detoured_functions::glow_effect_spectator( c_base_entity* player, c_base_entity* local, e_glow_style& style, c_vector& glow_color,
                                                          float& alpha_start, float& alpha, float& time_start, float& time_target, bool& animate )
{
	HOOK_SCOPE_OR_BAIL( g_hooks.m_glow_effect_spectator.get_original< decltype( &n_detoured_functions::glow_effect_spectator ) >( )(
		player, local, style, glow_color, alpha_start, alpha, time_start, time_target, animate ) );

	const bool glow_enabled = GET_VARIABLE( g_variables.m_glow_enable, bool );
	const bool glow_wanted  = GET_VARIABLE( g_variables.m_players, bool ) && ( glow_enabled || GET_VARIABLE( g_variables.m_players_stencil, bool ) || GET_VARIABLE( g_variables.m_inner_glow, bool ) ) &&
	                         player && local && player != local && local->is_enemy( player );
	if ( !glow_wanted )
		return false;

	const float sound_gate = g_sound_esp.player_gate( player );
	if ( sound_gate <= 0.f )
		return false;

	/* stencil / internal glow only / our full-res glow (player_stencil draws it): glow must still run for its stencil
	   pass; black at alpha 0 adds nothing. engine's quarter-res halo below = fallback only */
	if ( !glow_enabled || g_player_stencil.glow_enabled( ) ) {
		glow_color = c_vector( 0.f, 0.f, 0.f );
		alpha      = 0.f;
		return true;
	}

	g_ctx.m_is_glow_being_drawn = true;
	const auto m_vis_color      = GET_VARIABLE( g_variables.m_glow_vis_color, c_color );
	const auto m_invis_color    = GET_VARIABLE( g_variables.m_glow_invis_color, c_color );

	const bool can_see_player = local->can_see_player( player );

	glow_color = ( can_see_player
	                   ? c_vector( m_vis_color.base< color_type_r >( ), m_vis_color.base< color_type_g >( ), m_vis_color.base< color_type_b >( ) )
	                   : c_vector( m_invis_color.base< color_type_r >( ), m_invis_color.base< color_type_g >( ), m_invis_color.base< color_type_b >( ) ) );
	alpha      = ( can_see_player ? m_vis_color.base< color_type_a >( ) : m_invis_color.base< color_type_a >( ) ) * sound_gate;

	/* engine halo is one colour per player: the wave becomes a timed sweep, offset per player so they don't flash together */
	const float now = g_interfaces.m_global_vars_base->m_real_time;

	if ( GET_VARIABLE( g_variables.m_glow_wave, bool ) ) {
		const auto wave_color = GET_VARIABLE( g_variables.m_glow_wave_color, c_color );
		const float phase     = now * GET_VARIABLE( g_variables.m_glow_wave_speed, float ) - static_cast< float >( player->get_index( ) ) * 0.17f;
		const float w         = 0.5f + 0.5f * std::cos( 6.2831853f * phase );
		const float k         = w * w * w * std::clamp( GET_VARIABLE( g_variables.m_glow_wave_strength, float ), 0.f, 1.f );

		glow_color.m_x += ( wave_color.base< color_type_r >( ) - glow_color.m_x ) * k;
		glow_color.m_y += ( wave_color.base< color_type_g >( ) - glow_color.m_y ) * k;
		glow_color.m_z += ( wave_color.base< color_type_b >( ) - glow_color.m_z ) * k;
	}

	if ( GET_VARIABLE( g_variables.m_glow_pulse, bool ) ) {
		const float floor = std::clamp( GET_VARIABLE( g_variables.m_glow_pulse_min, float ), 0.f, 100.f ) / 100.f;
		const float wave  = 0.5f + 0.5f * std::sin( 6.2831853f * now * GET_VARIABLE( g_variables.m_glow_pulse_speed, float ) );
		alpha *= floor + ( 1.f - floor ) * wave;
	}

	if ( GET_VARIABLE( g_variables.m_glow_legacy, bool ) )
		style = static_cast< e_glow_style >( std::clamp( GET_VARIABLE( g_variables.m_glow_legacy_style, int ), 0, glow_style_count - 1 ) );

	g_ctx.m_is_glow_being_drawn = false;
	return true;
}
