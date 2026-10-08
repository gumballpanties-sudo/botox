#pragma once
#include "../../dependencies/fnv1a/fnv1a.h"

#define HASH_RT( s ) fnv1a::hash( s )

#define HASH_BT( s ) fnv1a::hash_const( s )

#define CLIENT_DLL              HASH_BT( "client.dll" )
#define ENGINE_DLL              HASH_BT( "engine.dll" )
#define LOCALIZE_DLL            HASH_BT( "localize.dll" )
#define MATERIALSYSTEM_DLL      HASH_BT( "materialsystem.dll" )
#define VGUI_DLL                HASH_BT( "vguimatsurface.dll" )
#define VGUI2_DLL               HASH_BT( "vgui2.dll" )
#define SHADERAPIDX9_DLL        HASH_BT( "shaderapidx9.dll" )
#define GAMEOVERLAYRENDERER_DLL HASH_BT( "gameoverlayrenderer.dll" )
#define PHYSICS_DLL             HASH_BT( "vphysics.dll" )
#define VSTDLIB_DLL             HASH_BT( "vstdlib.dll" )
#define TIER0_DLL               HASH_BT( "tier0.dll" )
#define INPUTSYSTEM_DLL         HASH_BT( "inputsystem.dll" )
#define STUDIORENDER_DLL        HASH_BT( "studiorender.dll" )
#define DATACACHE_DLL           HASH_BT( "datacache.dll" )
#define STEAM_API_DLL           HASH_BT( "steam_api.dll" )
#define MATCHMAKING_DLL         HASH_BT( "matchmaking.dll" )
#define SERVER_DLL              HASH_BT( "server.dll" )
#define SERVERBROWSER_DLL       HASH_BT( "serverbrowser.dll" )
#define FILESYSTEM_DLL          HASH_BT( "filesystem_stdio.dll" )
#define PANORAMA_DLL            HASH_BT( "panorama.dll" )

#define EVENT_DEBUG_ID_INIT     42
#define EVENT_DEBUG_ID_SHUTDOWN 13

#define COMBINE( lhs, rhs ) lhs##rhs
#define CONCAT( lhs, rhs )  COMBINE( lhs, rhs )
#define PAD( size )                                                                                                                                  \
private:                                                                                                                                             \
	std::uint8_t CONCAT( pad, __COUNTER__ )[ size ];                                                                                                 \
                                                                                                                                                     \
public:

#define MAX_COORD_FLOAT ( 16384.0f )
#define MIN_COORD_FLOAT ( -MAX_COORD_FLOAT )

#define LAG_COMPENSATION_TELEPORTED_DISTANCE_SQR 64.0f * 64.0f

inline static const char* FILTERED_KEY_NAMES[] = {
	"-",   "m1",  "m2",  "cn",  "m3",  "m4",  "m5",  "-",   "bac", "tab", "-",   "-",   "clr", "ret", "-",   "-",   "shi", "ctl", "men", "pau", "cap",
	"kan", "-",   "jun", "fin", "kan", "-",   "esc", "con", "nco", "acc", "mad", "spa", "pgu", "pgd", "end", "hom", "lef", "up",  "rig", "dow", "sel",
	"pri", "exe", "pri", "ins", "del", "hel", "0",   "1",   "2",   "3",   "4",   "5",   "6",   "7",   "8",   "9",   "-",   "-",   "-",   "-",   "-",
	"-",   "-",   "a",   "b",   "c",   "d",   "e",   "f",   "g",   "h",   "i",   "j",   "k",   "l",   "m",   "n",   "o",   "p",   "q",   "r",   "s",
	"t",   "u",   "v",   "w",   "x",   "y",   "z",   "win", "win", "app", "-",   "sle", "num", "num", "num", "num", "num", "num", "num", "num", "num",
	"num", "mul", "add", "sep", "min", "dec", "div", "f1",  "f2",  "f3",  "f4",  "f5",  "f6",  "f7",  "f8",  "f9",  "f10", "f11", "f12", "f13", "f14",
	"f15", "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23", "f24", "-",   "-",   "-",   "-",   "-",   "-",   "-",   "-",   "num", "scr", "equ",
	"mas", "toy", "oya", "oya", "-",   "-",   "-",   "-",   "-",   "-",   "-",   "-",   "-",   "shi", "shi", "ctr", "ctr", "alt", "alt"
};
