#pragma once
// killfeed air kill ( cs2 jumpshot wings ) for the imgui huds: killer off the ground + the wings drawn as a mesh
#include "air_wings_mesh.h"
#include "../../dependencies/imgui/imgui.h"
#include "../../game/sdk/includes/includes.h"
#include "../../globals/includes/includes.h"

#include <iterator>

namespace n_air_wings
{
	/* killer off the ground, per the server: others off the entity, local from the copy taken at net_update_end before
	   prediction rewrote its flags. ponytail: last snapshot, 1 tick before the kill tick */
	inline bool killer_in_air( int attacker, int victim )
	{
		if ( !GET_VARIABLE( g_variables.m_killfeed_air_icon, bool ) || attacker <= 0 || attacker == victim )
			return false;
		if ( attacker == g_interfaces.m_engine_client->get_local_player( ) )
			return g_ctx.m_local_net_air;
		const auto killer = g_interfaces.m_client_entity_list->get< c_base_entity >( attacker );
		return killer && !killer->is_dormant( ) && killer->is_player( ) && !( killer->get_flags( ) & fl_onground );
	}

	// square box, top left x y. fill aliased ( fringes would seam between triangles ), outline anti aliased on top
	inline void draw( ImDrawList* list, float x, float y, float size, ImU32 fill, ImU32 outline, float thickness )
	{
		if ( !list || size <= 0.f )
			return;
		ImVec2 pts[ std::size( k_outline ) ];
		for ( size_t i = 0; i < std::size( k_outline ); ++i )
			pts[ i ] = ImVec2( x + k_outline[ i ].x * size, y + k_outline[ i ].y * size );

		const ImDrawListFlags old = list->Flags;
		list->Flags &= ~ImDrawListFlags_AntiAliasedFill;
		for ( size_t i = 0; i + 2 < std::size( k_tris ); i += 3 )
			list->AddTriangleFilled( pts[ k_tris[ i ] ], pts[ k_tris[ i + 1 ] ], pts[ k_tris[ i + 2 ] ], fill );
		list->Flags = old;

		if ( outline & IM_COL32_A_MASK )
			list->AddPolyline( pts, static_cast< int >( std::size( pts ) ), outline, ImDrawFlags_Closed, thickness );
	}
}
