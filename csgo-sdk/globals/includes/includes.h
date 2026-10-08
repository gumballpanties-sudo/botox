#pragma once
#include <Windows.h>

#undef max
#undef min

#include "../../globals/macros/macros.h"

#include "../../dependencies/lazy_importer/lazy_importer.h"

#include "../media_player/media_player.h"

#include "../config/config.h"
#include "../config/variables.h"

#include "../math/math.h"

#include "../../dependencies/fnv1a/fnv1a.h"

#include "../../hacks/menu/menu.h"

#include "../../utilities/utilities.h"

#include "../../utilities/console/console.h"

#include "../../utilities/modules/modules.h"

#include "../interfaces/interfaces.h"

#include "../../utilities/input/input.h"

#include ".././convars/convars.h"

#include "../netvars/netvars.h"

#include "../render/render.h"

#include "../../hooks/hooks.h"

#include "../../dependencies/minhook/include/MinHook.h"

#include "../../dependencies/imgui/imgui.h"
#include "../../dependencies/imgui/imgui_freetype.h"
#include "../../dependencies/imgui/imgui_impl_dx9.h"
#include "../../dependencies/imgui/imgui_impl_win32.h"
#include "../../dependencies/imgui/imgui_internal.h"

#include <format>

/* thread */
#include <thread>

#include "../../globals/globals.h"
