#pragma once
#include <cmath>

// lua render.blur strength -> passes of the one blur a frame. Same bytes in botox csgo-sdk hacks/misc/spectator_list,
// botox-tf2 + botox-css src/hacks/screen; gate csgo-sdk tools/lua_blur_check.py.
// strength 1 = gamesense: 1 x + 1 y pass at 1/4 res. n passes at spread s / sqrt( n ) = s x that radius, taps <= 1 texel apart
struct BlurPlan { int passes; float spread; };

inline BlurPlan BlurPlanFor(float strength)
{
    int n = 1;
    while (n < 9 && static_cast<float>(n) < strength * strength)
        ++n;
    return { n, strength / std::sqrt(static_cast<float>(n)) };
}
