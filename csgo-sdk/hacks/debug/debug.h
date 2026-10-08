#pragma once

#include "../../dependencies/imgui/imgui.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"
#include "../movement/movement.h"
#include <format>

#ifdef _DEBUG

namespace n_debugger
{
	struct impl_t {
		void draw_server_hitbox_model( )
		{
			/* server.dll only exists on a listen server: online both stay null */
			static const auto util_player_by_index = reinterpret_cast< uintptr_t( __fastcall* )( unsigned int ) >(
				g_modules[ SERVER_DLL ].find_pattern( "85 C9 7E 32 A1 ? ? ? ?" ) );

			const auto get_player_by_index = []( int index ) { return util_player_by_index( index ); };

			static void* pattern =
				reinterpret_cast< void* >( g_modules[ SERVER_DLL ].find_pattern( "55 8B EC 81 EC ? ? ? ? 53 56 8B 35 ? ? ? ? 8B D9 57 8B CE" ) );

			if ( !util_player_by_index || !pattern )
				return;

			for ( int i = 1; i <= g_interfaces.m_global_vars_base->m_max_clients; i++ ) {
				uintptr_t player = get_player_by_index( i );
				if ( !player )
					return;

				static float duration = -1.0f;

				__asm
				{
					pushad
					movss xmm1, duration
					push 1
					mov ecx, player
					call pattern
					popad
				}
			}
		}

		void on_paint_traverse( )
		{
			if ( !GET_VARIABLE( g_variables.m_debugger_visual, bool ) )
				return;
		}

		void on_frame_stage_notify( int stage )
		{
			if ( !GET_VARIABLE( g_variables.m_debugger_visual, bool ) || !g_interfaces.m_engine_client->is_connected_safe( ) || !g_ctx.m_local ||
			     !g_ctx.m_local->is_alive( ) )
				return;

			if ( stage == e_client_frame_stage::net_update_end )
				draw_server_hitbox_model( );
		}
	};
}

inline n_debugger::impl_t g_debugger;
#endif
