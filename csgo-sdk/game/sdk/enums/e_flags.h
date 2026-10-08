#pragma once

enum e_flags : int {
	fl_onground              = ( 1 << 0 ),
	fl_ducking               = ( 1 << 1 ),
	fl_animducking           = ( 1 << 2 ),
	fl_waterjump             = ( 1 << 3 ),
	fl_ontrain               = ( 1 << 4 ),
	fl_inrain                = ( 1 << 5 ),
	fl_frozen                = ( 1 << 6 ),
	fl_atcontrols            = ( 1 << 7 ),
	fl_client                = ( 1 << 8 ),
	fl_fakeclient            = ( 1 << 9 ),
	fl_inwater               = ( 1 << 10 ),
	fl_fly                   = ( 1 << 11 ),
	fl_swim                  = ( 1 << 12 ),
	fl_conveyor              = ( 1 << 13 ),
	fl_npc                   = ( 1 << 14 ),
	fl_godmode               = ( 1 << 15 ),
	fl_notarget              = ( 1 << 16 ),
	fl_aimtarget             = ( 1 << 17 ),
	fl_partialground         = ( 1 << 18 ),
	fl_staticprop            = ( 1 << 19 ),
	fl_graphed               = ( 1 << 20 ),
	fl_grenade               = ( 1 << 21 ),
	fl_stepmovement          = ( 1 << 22 ),
	fl_donttouch             = ( 1 << 23 ),
	fl_basevelocity          = ( 1 << 24 ),
	fl_worldbrush            = ( 1 << 25 ),
	fl_object                = ( 1 << 26 ),
	fl_killme                = ( 1 << 27 ),
	fl_onfire                = ( 1 << 28 ),
	fl_dissolving            = ( 1 << 29 ),
	fl_transragdoll          = ( 1 << 30 ),
	fl_unblockable_by_player = ( 1 << 31 )
};

inline constexpr static const std::initializer_list< int > invalid_flags = { fl_onground, fl_frozen, fl_waterjump, fl_inwater };
