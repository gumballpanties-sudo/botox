#pragma once
// custom names (fpsoverlay names.cpp) pure half: cycle step + which text shows. Same file in
// botox csgo (hacks/misc/names_pure.h) -- edit one, copy to the other. Test: botox-tf2
// tests/names_test.cpp.
#include <cstring>

namespace n_names
{
    constexpr int kMaxNames = 16;   // fpsoverlay k_maxNames

    // one cycle list's position, fpsoverlay g_nameIdx / g_nameLastChange pairs
    struct Cycle { int idx = 0; double last = 0.0; };

    inline int Index(const Cycle& c, int count) { return (c.idx >= 0 && c.idx < count) ? c.idx : 0; }

    // on-kill trigger (fpsoverlay h_FireEvent): idx = (idx + 1) % count
    inline void Step(Cycle& c, int count)
    {
        if (count > 0) c.idx = (Index(c, count) + 1) % count;
    }

    // timer trigger (fpsoverlay GetStreamedPlayerName): first read arms, interval elapsed = next
    // entry. now = game seconds; a clock that went back (map change) re-arms instead of jumping.
    inline void Tick(Cycle& c, int count, double now, float interval)
    {
        if (count <= 0) return;
        if (c.last <= 0.0 || now < c.last) { c.last = now; return; }
        if (now - c.last >= (interval > 0.1f ? interval : 0.1f)) { Step(c, count); c.last = now; }
    }

    // fpsoverlay order: cycle entry if cycling and non-empty, else the fixed text. An empty
    // result = keep the real name.
    template <class Row>
    inline const char* Choose(const char* fixed, bool cycle, const Cycle& c, int count, Row row)
    {
        if (cycle && count > 0)
        {
            const char* e = row(Index(c, count));
            if (e && e[0]) return e;
        }
        return fixed ? fixed : "";
    }
}
