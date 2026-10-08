#pragma once

class c_user_cmd;
class c_base_entity;

#include "../game/sdk/classes/c_angle.h"
#include "../hacks/lagcomp/lagcomp.h"

#include <atomic> // the unload flag + in-flight counter are read on every game thread

namespace n_ctx
{
	struct impl_t {
		c_user_cmd* m_cmd                     = nullptr;
		c_base_entity* m_local                = nullptr;
		n_lagcomp::impl_t::record_t* m_record = nullptr;

		float m_extend_applied = 0.f;

		std::atomic< bool > m_unloading{ false };
		std::atomic< int > m_hooks_in_flight{ 0 };

		// raised by whichever thread ran g_hooks.release_render_resources( ) first
		std::atomic< bool > m_render_release_done{ false };

		std::atomic< bool > m_world_restore_requested{ false };
		std::atomic< bool > m_world_restore_done{ false };

		bool m_is_window_focused{ };

		/* settings "unload" button, the only way out (a stray key press must never eject) */
		bool m_eject_requested{ };

		bool m_low_fps = false;

		bool m_is_glow_being_drawn = false;

		bool m_is_console_being_drawn = false;

		bool m_is_scope_being_drawn = false;

		bool m_is_hud_weapon_being_drawn = false;
		bool m_crosshair_outline_next    = true;

		float m_width{ }, m_height{ };

		bool m_display_stable{ };

		float m_last_tick_yaw = 0.f;

		c_angle m_first_view_angles{ };
		int m_previous_tick    = 0;
		float m_target_velocity_z = 0.f;

		int m_input_buttons    = 0;
		short m_input_mouse_dx = 0;

		c_angle old_view_point{ };
		c_angle last_view_point{ };

		int m_max_allocations = 0;

		int m_last_spectators_y = 5;

		char m_windows_directory[ 64 ]{ };

		float gravity_per_tick{ };

		float inverse_half_gravity_per_tick{ };
	};

}

inline n_ctx::impl_t g_ctx;

/* top of every detour = "a game thread is in our code", eject spins on the count. HOOK_SCOPE_OR_BAIL:
   take the ticket, then pass straight to the original if unloading. */
namespace n_ctx
{
	struct hook_scope_t {
		hook_scope_t( )
		{
			g_ctx.m_hooks_in_flight.fetch_add( 1, std::memory_order_acquire );
		}

		~hook_scope_t( )
		{
			g_ctx.m_hooks_in_flight.fetch_sub( 1, std::memory_order_release );
		}

		hook_scope_t( const hook_scope_t& )            = delete;
		hook_scope_t& operator=( const hook_scope_t& ) = delete;
	};
}

#define HOOK_SCOPE_OR_BAIL( original_call )                                  \
	const n_ctx::hook_scope_t hook_scope_guard;                              \
	if ( g_ctx.m_unloading.load( std::memory_order_relaxed ) )               \
	return original_call
