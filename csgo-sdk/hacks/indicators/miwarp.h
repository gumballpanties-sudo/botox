#pragma once
#include <math.h>

namespace miw
{
constexpr float kStretch = 0.04f;
constexpr float kPi      = 3.14159265f;

struct Warp
{
    float piv;
    float sa;
    float sp;
};

inline Warp SplitWarp(float c, float e, float p, float t)
{
    const float ct = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    const float sa = 1.f + kStretch * sinf(kPi * ct);
    const float dir = (p - c) >= 0.f ? 1.f : -1.f;
    return { c - dir * e, sa, 1.f / sa };
}
}
