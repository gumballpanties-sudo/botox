#pragma once

template< typename FuncType >
__forceinline static FuncType CallVFunction( void* ppClass, int index )
{
	int* pVTable  = *( int** )ppClass;
	int dwAddress = pVTable[ index ];
	return ( FuncType )( dwAddress );
}

#include <cstdarg>
#include <string>
#include <stdio.h>
#define MAX_BUFFER_SIZE 1024

#define CHAT_COLOR_DEFAULT        "\x01"
#define CHAT_COLOR_RED            "\x02"
#define CHAT_COLOR_LIGHTPURPLE    "\x03"
#define CHAT_COLOR_GREEN          "\x04"
#define CHAT_COLOR_LIME           "\x05"
#define CHAT_COLOR_LIGHTGREEN     "\x06"
#define CHAT_COLOR_LIGHTRED       "\x07"
#define CHAT_COLOR_GRAY           "\x08"
#define CHAT_COLOR_LIGHTOLIVE     "\x09"
#define CHAT_COLOR_LIGHTSTEELBLUE "\x0A"
#define CHAT_COLOR_LIGHTBLUE      "\x0B"
#define CHAT_COLOR_BLUE           "\x0C"
#define CHAT_COLOR_PURPLE         "\x0D"
#define CHAT_COLOR_PINK           "\x0E"
#define CHAT_COLOR_LIGHTRED2      "\x0F"
#define CHAT_COLOR_OLIVE          "\x10"

#define CHAT_TOKEN_EDGEBUG    "#botox#_print_edgebugged"
#define CHAT_TOKEN_TEXTUREBUG "#botox#_print_texturebugged"
#define CHAT_TOKEN_AIRSTUCK   "#botox#_print_airstucked"
#define CHAT_TOKEN_PIXELSURF  "#botox#_print_pixelsurfed"
#define CHAT_TOKEN_WALLCLIMB  "#botox#_print_wallclimbed"
#define CHAT_TOKEN_JUMPSTATS  "#botox#_print_jumpstats"
#define CHAT_TOKEN_RAW        "#botox#_print_raw"

inline std::string g_jump_stats_line;
inline int g_edge_bug_chain = 1;

class c_hudchat
{
public:
	void chatprintf( int iPlayerIndex, int iFilter, const char* format, ... )
	{
		static char buf[ MAX_BUFFER_SIZE ] = "";
		va_list va;
		va_start( va, format );
		vsnprintf_s( buf, MAX_BUFFER_SIZE, format, va );
		va_end( va );
		CallVFunction< void( __cdecl* )( void*, int, int, const char*, ... ) >( this, 27 )( this, iPlayerIndex, iFilter, buf );
	}
};
