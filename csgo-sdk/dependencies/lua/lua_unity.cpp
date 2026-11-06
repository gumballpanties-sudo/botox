/* lua 5.4.7 as one c++ tu: lua_error throws c++ exceptions, so destructors in bindings run */
#define _CRT_SECURE_NO_WARNINGS
#define MAKE_LIB
#include "onelua.c"
