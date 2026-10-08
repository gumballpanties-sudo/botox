#pragma once
#include "c_uipanel.h"

class c_uievent
{
public:
	c_uipanel* pos1;      // 0x0000
	char pad_0008[ 8 ];   // 0x0008
	c_uipanel* pos2;      // 0x0010
	c_uipanel* pos3;      // 0x0018
	char pad_0020[ 24 ];  // 0x0020
	c_uipanel* pos4;      // 0x0038
	char pad_0040[ 8 ];   // 0x0040
	c_uipanel* pos5;      // 0x0048
	c_uipanel* pos6;      // 0x0050
	char pad_0058[ 8 ];   // 0x0058
	c_uipanel* pos7;      // 0x0060
	c_uipanel* pos8;      // 0x0068
	char pad_0070[ 16 ];  // 0x0070
	c_uipanel* pos9;      // 0x0080
	c_uipanel* pos10;     // 0x0088
	c_uipanel* pos11;     // 0x0090
	char pad_0098[ 24 ];  // 0x0098
	c_uipanel* pos12;     // 0x00B0
	c_uipanel* pos13;     // 0x00B8
	char pad_00C0[ 8 ];   // 0x00C0
	c_uipanel* pos14;     // 0x00C8
	char pad_00D0[ 16 ];  // 0x00D0
	c_uipanel* pos15;     // 0x00E0
	char pad_00E8[ 8 ];   // 0x00E8
	char* eventName;      // 0x00F0
	char pad_00F8[ 16 ];  // 0x00F8
	c_uipanel* pos16;     // 0x0108
	char pad_0110[ 8 ];   // 0x0110
	c_uipanel* pos17;     // 0x0118
	char pad_0120[ 40 ];  // 0x0120
	c_uipanel* pos18;     // 0x0148
	char pad_0150[ 8 ];   // 0x0150
	c_uipanel* pos19;     // 0x0158
	char pad_0160[ 8 ];   // 0x0160
	c_uipanel* pos20;     // 0x0168
	char pad_0170[ 32 ];  // 0x0170
	c_uipanel* pos21;     // 0x0190
	char pad_0198[ 360 ]; // 0x0198
};
