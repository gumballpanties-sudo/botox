#pragma once
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>
#include "../../dependencies/imgui/imgui.h"
#include "../../dependencies/imgui/imgui_internal.h"
#include "miwarp.h"

namespace mig
{
constexpr float kFar = 1e9f;
constexpr float kOut = 1e4f;

inline float Sat(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

inline void Chamfer(float* d, int w, int h, float* s = nullptr)
{
    constexpr float kOrtho = 1.f, kDiag = 1.41421356f;
    const auto relax = [&](int i, int j, float step, float& v)
    {
        if (d[j] + step < v) { v = d[j] + step; if (s) s[i] = s[j]; }
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int i = y * w + x;
            float v = d[i];
            if (x > 0)              relax(i, i - 1,     kOrtho, v);
            if (y > 0)              relax(i, i - w,     kOrtho, v);
            if (y > 0 && x > 0)     relax(i, i - w - 1, kDiag,  v);
            if (y > 0 && x + 1 < w) relax(i, i - w + 1, kDiag,  v);
            d[i] = v;
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x)
        {
            const int i = y * w + x;
            float v = d[i];
            if (x + 1 < w)              relax(i, i + 1,     kOrtho, v);
            if (y + 1 < h)              relax(i, i + w,     kOrtho, v);
            if (y + 1 < h && x + 1 < w) relax(i, i + w + 1, kDiag,  v);
            if (y + 1 < h && x > 0)     relax(i, i + w - 1, kDiag,  v);
            d[i] = v;
        }
}

inline float AtlasTap(const unsigned char* tex, int tw, float u, float v,
                      float cu0, float cu1, float cv0, float cv1)
{
    const auto T = [&](float x, float y) -> float
    {
        if (x < cu0) x = cu0; if (x > cu1) x = cu1;
        if (y < cv0) y = cv0; if (y > cv1) y = cv1;
        return tex[(int)y * tw + (int)x];
    };
    const float x0 = floorf(u), y0 = floorf(v);
    const float fu = u - x0,    fv = v - y0;
    const float a = T(x0, y0) * (1.f - fu) + T(x0 + 1.f, y0) * fu;
    const float b = T(x0, y0 + 1.f) * (1.f - fu) + T(x0 + 1.f, y0 + 1.f) * fu;
    return a * (1.f - fv) + b * fv;
}

inline bool BlitLabel(unsigned char* cov, int bw, int bh, ImVec2 org, ImFont* font, float fs,
                      const char* text, ImVec2 pos)
{
    ImFontAtlas* atlas = font->ContainerAtlas;
    const unsigned char* tex = atlas ? atlas->TexPixelsAlpha8 : nullptr;
    if (!tex) return false;
    const int   tw = atlas->TexWidth, th = atlas->TexHeight;
    const float scale = fs / font->FontSize;

    float pen = pos.x;
    for (const char* s = text; *s; )
    {
        unsigned int c = (unsigned char)*s;
        if (c < 0x80) ++s;
        else { s += ImTextCharFromUtf8(&c, s, nullptr); if (c == 0) break; }
        const ImFontGlyph* g = font->FindGlyph((ImWchar)c);
        if (!g) continue;
        const float x0 = pen + g->X0 * scale - org.x,   x1 = pen + g->X1 * scale - org.x;
        const float y0 = pos.y + g->Y0 * scale - org.y, y1 = pos.y + g->Y1 * scale - org.y;
        pen += g->AdvanceX * scale;
        if (x1 - x0 < 0.01f || y1 - y0 < 0.01f) continue;

        const float u0 = g->U0 * tw, u1 = g->U1 * tw;
        const float v0 = g->V0 * th, v1 = g->V1 * th;
        const float ku = (u1 - u0) / (x1 - x0), kv = (v1 - v0) / (y1 - y0);
        const float qx0 = x0 - 0.5f / ku, qx1 = x1 + 0.5f / ku;
        const float qy0 = y0 - 0.5f / kv, qy1 = y1 + 0.5f / kv;
        const float cu0 = fmaxf(u0 - 1.f, 0.f), cu1 = fminf(u1, tw - 1.f);
        const float cv0 = fmaxf(v0 - 1.f, 0.f), cv1 = fminf(v1, th - 1.f);
        int px0 = (int)floorf(qx0), px1 = (int)ceilf(qx1);
        int py0 = (int)floorf(qy0), py1 = (int)ceilf(qy1);
        if (px0 < 0) px0 = 0; if (px1 > bw) px1 = bw;
        if (py0 < 0) py0 = 0; if (py1 > bh) py1 = bh;

        for (int py = py0; py < py1; ++py)
        {
            const float cy = py + 0.5f;
            if (cy < qy0 || cy >= qy1) continue;
            const float sv = v0 + (cy - y0) * kv - 0.5f;
            for (int px = px0; px < px1; ++px)
            {
                const float cx = px + 0.5f;
                if (cx < qx0 || cx >= qx1) continue;
                const float su = u0 + (cx - x0) * ku - 0.5f;
                const float val = AtlasTap(tex, tw, su, sv, cu0, cu1, cv0, cv1);
                unsigned char& dst = cov[py * bw + px];
                if (val > dst) dst = (unsigned char)(val + 0.5f);
            }
        }
    }
    return true;
}

inline void SignedDist(const unsigned char* cov, int w, int h, float* dOut, float* dIn,
                       float* sOut, float* sIn, float* sdf, float* slope)
{
    const int n = w * h;
    for (int i = 0; i < n; ++i) { dOut[i] = dIn[i] = kFar; sOut[i] = sIn[i] = 1.f; }
    const auto C = [&](int i) { return cov[i] / 255.f; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int   i  = y * w + x;
            const float c  = C(i);
            const bool  in = cov[i] >= 128;
            float t[2] = { kFar, kFar }, g[2] = { 0.f, 0.f };
            const auto cross = [&](int j, int axis)
            {
                if ((cov[j] >= 128) == in) return;
                const float dc = fabsf(C(j) - c), tt = fabsf(0.5f - c) / dc;
                if (tt < t[axis]) { t[axis] = tt; g[axis] = dc; }
            };
            if (x > 0)     cross(i - 1, 0);
            if (x + 1 < w) cross(i + 1, 0);
            if (y > 0)     cross(i - w, 1);
            if (y + 1 < h) cross(i + w, 1);
            if (t[0] >= kFar && t[1] >= kFar) continue;
            float d, s;
            if (t[0] < kFar && t[1] < kFar)
            {
                d = t[0] * t[1] / fmaxf(sqrtf(t[0] * t[0] + t[1] * t[1]), 1e-6f);
                s = sqrtf(g[0] * g[0] + g[1] * g[1]);
            }
            else { const int a = t[0] < kFar ? 0 : 1; d = t[a]; s = g[a]; }
            dOut[i] = in ? -d : d;
            dIn[i]  = in ? d : -d;
            sOut[i] = sIn[i] = s;
        }
    Chamfer(dOut, w, h, sOut);
    Chamfer(dIn, w, h, sIn);
    for (int i = 0; i < n; ++i)
    {
        const bool in = cov[i] >= 128;
        sdf[i]   = in ? -dIn[i] : dOut[i];
        slope[i] = in ? sIn[i] : sOut[i];
        const float c = C(i);
        if (cov[i] > 0 && cov[i] < 255 && fabsf(0.5f - c) > 0.02f && fabsf(sdf[i]) > 1e-3f)
            slope[i] = fabsf(0.5f - c) / fabsf(sdf[i]);
    }
}

inline float Smooth(float e0, float e1, float x)
{
    const float t = Sat((x - e0) / (e1 - e0));
    return t * t * (3.f - 2.f * t);
}

inline float SMin(float a, float b, float r)
{
    if (r <= 0.f) return a < b ? a : b;
    const float h = Sat(0.5f + 0.5f * (b - a) / r);
    return b + (a - b) * h - r * h * (1.f - h);
}

constexpr float kSnap = 0.55f;
constexpr float kPull = 1.12f;
inline float SlideAt(float t)
{
    if (t <= kSnap) { const float x = 1.f - Sat(t / kSnap); return kPull * (1.f - x * x * x); }
    return 1.f + (kPull - 1.f) * (1.f - Smooth(kSnap, 0.85f, t));
}
inline float DepthAt(float t) { return Smooth(0.2f, kSnap, t); }
inline float CutAt(float t) { return Smooth(kSnap, 0.75f, t); }
inline float MorphAt(float t) { return Smooth(0.f, 0.5f, t); }
constexpr float kBack = 0.9f;
inline float EaseOut(float x)
{
    const float p = x - 1.f;
    return 1.f + (kBack + 1.f) * p * p * p + kBack * p * p;
}

constexpr float kSlotRate = 20.f;
inline float Follow(float cur, float want, float dt, float rate)
{
    const float r = cur + (want - cur) * (1.f - expf(-rate * (dt > 0.f ? dt : 0.f)));
    return fabsf(want - r) < 1e-4f ? want : r;
}

inline void SubPixelText(ImDrawList* dl, int vtx0, ImVec2 pos, const ImFont* font)
{
    const float fx = pos.x - floorf(pos.x), fy = pos.y - floorf(pos.y);
    const ImFontAtlas* at = font->ContainerAtlas;
    const float hu = 0.5f / at->TexWidth, hv = 0.5f / at->TexHeight;
    ImDrawVert* v   = dl->VtxBuffer.Data + vtx0;
    ImDrawVert* end = dl->VtxBuffer.Data + dl->VtxBuffer.Size;
    for (; v + 3 < end; v += 4)
    {
        const float cx = (v[0].pos.x + v[2].pos.x) * 0.5f, cy = (v[0].pos.y + v[2].pos.y) * 0.5f;
        const float du = fabsf(v[2].uv.x - v[0].uv.x), dv = fabsf(v[2].uv.y - v[0].uv.y);
        const float hx = du > 0.f ? hu * fabsf(v[2].pos.x - v[0].pos.x) / du : 0.f;
        const float hy = dv > 0.f ? hv * fabsf(v[2].pos.y - v[0].pos.y) / dv : 0.f;
        for (int k = 0; k < 4; ++k)
        {
            const float sx = v[k].pos.x < cx ? -1.f : 1.f, sy = v[k].pos.y < cy ? -1.f : 1.f;
            v[k].pos.x += fx + sx * hx; v[k].pos.y += fy + sy * hy;
            v[k].uv.x  += sx * hu;      v[k].uv.y  += sy * hv;
        }
    }
}

inline float FadeStep(float s, bool on, float dt, float speed)
{
    if (!(dt > 0.f)) return s;
    if (on) return Sat(s + dt * speed);
    const float r = sqrtf(Sat(s)) - dt * speed;
    return r > 0.f ? r * r : 0.f;
}

constexpr float kSplitEm = 1.5f, kSplitMaxSlow = 2.f;
inline float SplitSpeed(float base, float w, float fs)
{
    const float r = fs > 0.f ? w / (fs * kSplitEm) : 1.f;
    const float k = r > 1.f ? sqrtf(r) : 1.f;
    return base / (k < kSplitMaxSlow ? k : kSplitMaxSlow);
}

inline float PopAt(float t)
{
    const float x = 1.f - Sat(t);
    return 0.5f + 0.5f * (1.f - x * x * x);
}

inline int PickParent(const float* raw, int n, int i)
{
    int   best = -1;
    float br   = raw[i] + 0.02f;
    if (i > 0     && raw[i - 1] > br) { best = i - 1; br = raw[i - 1]; }
    if (i + 1 < n && raw[i + 1] > br)   best = i + 1;
    return best;
}

struct Field { float x0 = 0.f, y0 = 0.f; int w = 0, h = 0; const float* d = nullptr; const float* s = nullptr; };

inline float Sample(const Field& f, float x, float y, float* slope = nullptr)
{
    if (slope) *slope = 1.f;
    const float fx = x - f.x0 - 0.5f, fy = y - f.y0 - 0.5f;
    if (!f.d || fx < 0.f || fy < 0.f || fx > f.w - 1.f || fy > f.h - 1.f) return kOut;
    int ix = (int)fx, iy = (int)fy;
    if (ix > f.w - 2) ix = f.w - 2 < 0 ? 0 : f.w - 2;
    if (iy > f.h - 2) iy = f.h - 2 < 0 ? 0 : f.h - 2;
    const float ax = fx - ix, ay = fy - iy;
    const int   o  = ix + 1 < f.w ? 1 : 0, row = iy + 1 < f.h ? f.w : 0;
    const auto  at = [&](const float* g)
    {
        const float* r0 = g + iy * f.w + ix;
        const float  a  = r0[0] + (r0[o] - r0[0]) * ax;
        const float  b  = r0[row] + (r0[row + o] - r0[row]) * ax;
        return a + (b - a) * ay;
    };
    if (slope && f.s) *slope = at(f.s);
    return at(f.d);
}

struct Comp
{
    Field self;
    Field from;
    int   parent = -2;
    float m = 1.f;
    float col[3] = { 1.f, 1.f, 1.f };
    float px = 0.f, py = 0.f, sx = 1.f, sy = 1.f;
    // where its fields answer at all, box ss px (warped). Outside it reads kOut: never the
    // nearest shape, so Compose leaves it out of that pixel -- a 16 tag stack stays ~2 tags a pixel
    float rx0 = -1e9f, ry0 = -1e9f, rx1 = 1e9f, ry1 = 1e9f;
};

inline float CompDist(const Comp& c, float x, float y, float* slope = nullptr)
{
    const float qx = c.px + (x - c.px) / c.sx, qy = c.py + (y - c.py) / c.sy;
    float s = 1.f;
    float d = Sample(c.self, qx, qy, slope ? &s : nullptr);
    if (c.parent >= 0)
    {
        float s0 = 1.f;
        const float f0 = Sample(c.from, qx, qy, slope ? &s0 : nullptr);
        d = f0 + (d - f0) * c.m;
        s = s0 + (s - s0) * c.m;
    }
    if (slope) *slope = s;
    return d * fminf(c.sx, c.sy);
}

struct Bridge
{
    float eP = 0.f, eC = 0.f;
    float side = 1.f;
    float cv = 0.f;
    float hs = 0.f;
    float depth = 0.f;
    float cut = 0.f;
    float r = 0.f;
    int   parent = -1, child = -1;
};

inline float BridgeDist(const Bridge& br, float u, float v, float* kOut_ = nullptr)
{
    const float span = (br.eC - br.eP) * br.side;
    if (span <= 0.f || br.hs <= 0.f || br.cut >= 1.f) return kOut;
    const float w = (u - br.eP) * br.side / span;
    if (w < 0.f || w > 1.f) return kOut;
    const float x = 1.f - fabsf(2.f * w - 1.f);
    float k;
    if (br.cut > 0.f)
    {
        if (x >= 1.f - br.cut) return kOut;
        const float xs = x / (1.f - br.cut);
        k = xs * xs;
    }
    else
        k = br.depth * x * x;
    if (kOut_) *kOut_ = k;
    return fabsf(v - br.cv) - br.hs * (1.f - 1.35f * k);
}

struct Scene
{
    const Comp*   comps = nullptr;   int nc = 0;
    const Bridge* bridges = nullptr; int nb = 0;
    bool axisX = true;
    int  w = 0, h = 0, ss = 1;
    const float* cull = nullptr;     int ncull = 0;
};

inline void Compose(const Scene& s, unsigned int* out)
{
    constexpr int kMax = 16;
    const int nc = s.nc < kMax ? s.nc : kMax;
    const int ss = s.ss, ow = s.w / ss, oh = s.h / ss;
    const float inv = 1.f / (float)(ss * ss);
    int all[kMax], use[kMax];
    for (int k = 0; k < nc; ++k) all[k] = k;
    for (int oy = 0; oy < oh; ++oy)
        for (int ox = 0; ox < ow; ++ox)
        {
            const int* idx = all;
            int        ni  = nc;
            if (s.cull)
            {
                const float bx0 = (float)(ox * ss), bx1 = bx0 + ss, by0 = (float)(oy * ss), by1 = by0 + ss;
                bool near = false;
                for (int k = 0; k < s.ncull && !near; ++k)
                {
                    const float* b = s.cull + 4 * k;
                    near = b[0] < bx1 && b[2] > bx0 && b[1] < by1 && b[3] > by0;
                }
                if (!near) { out[oy * ow + ox] = 0; continue; }
                ni = 0;
                for (int k = 0; k < nc; ++k)
                {
                    const Comp& c = s.comps[k];
                    if (c.rx0 < bx1 && c.rx1 > bx0 && c.ry0 < by1 && c.ry1 > by0) use[ni++] = k;
                }
                idx = use;
            }
            float a = 0.f, r = 0.f, g = 0.f, b = 0.f;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx)
                {
                    const float x = (float)(ox * ss + sx) + 0.5f, y = (float)(oy * ss + sy) + 0.5f;
                    const float u = s.axisX ? x : y, v = s.axisX ? y : x;

                    float best = kOut, slope = 1.f;
                    int   own  = 0;
                    for (int j = 0; j < ni; ++j)
                    {
                        float sk;
                        const int   k = idx[j];
                        const float d = CompDist(s.comps[k], x, y, &sk);
                        if (d < best) { best = d; own = k; slope = sk; }
                    }
                    for (int i = 0; i < s.nb; ++i)
                    {
                        const Bridge& br = s.bridges[i];
                        float k = 0.f;
                        const float   db = BridgeDist(br, u, v, &k);
                        if (db >= kOut) continue;
                        if (db < best && br.child >= 0 && br.child < nc && br.parent >= 0 && br.parent < nc)
                            own = (u - 0.5f * (br.eP + br.eC)) * br.side >= 0.f ? br.child : br.parent;
                        const float fr = br.r * (1.f - k);
                        const float wl = fr > 0.f ? Sat(0.5f + 0.5f * (db - best) / fr) : (best < db ? 1.f : 0.f);
                        slope = 1.f + (slope - 1.f) * wl;
                        best  = SMin(best, db, fr);
                    }
                    const float cov = Sat(0.5f - best * slope);
                    if (cov <= 0.f) continue;
                    const float* col = s.comps[own].col;
                    a += cov; r += cov * col[0]; g += cov * col[1]; b += cov * col[2];
                }
            unsigned int& o = out[oy * ow + ox];
            if (a <= 0.f) { o = 0; continue; }
            const float al = a * inv;
            r /= a; g /= a; b /= a;
            o = ((unsigned int)(Sat(al) * 255.f + 0.5f) << 24) |
                ((unsigned int)(Sat(r)  * 255.f + 0.5f) << 16) |
                ((unsigned int)(Sat(g)  * 255.f + 0.5f) <<  8) |
                 (unsigned int)(Sat(b)  * 255.f + 0.5f);
        }
}

constexpr int kMaxTags = 16;

struct Tag { const char* text; ImVec2 pos, size; float col[3]; float raw; int parent; ImFont* font = nullptr; float fs = 0.f; };

struct CachedField
{
    std::string   text;
    const ImFont* font = nullptr;   const void* pixels = nullptr;   const void* glyphs = nullptr;
    int tw = 0, th = 0;
    float fs = 0.f, hw = 0.f, pad = 0.f;   int ss = 0;
    int   w = 0, h = 0;             float cx = 0.f, cy = 0.f;
    int   i0 = 0, i1 = -1, j0 = 0, j1 = -1;
    float smin = 1.f;
    std::vector<float> d, s;
    unsigned stamp = 0;
};
constexpr int kFieldCache = 12;

struct Work
{
    std::vector<unsigned char> cov;
    std::vector<float>         dOut, dIn, sOut, sIn;
    std::vector<unsigned int>  px;
    std::vector<CachedField>   cache;
    unsigned clock = 0;
    // the gate's references, each must match the default to the bit: cull false = Compose every
    // pixel, reuse false = every field built fresh (no cache lookup: a key missing a field shows)
    bool     cull  = true;
    bool     reuse = true;
    Comp   comps[kMaxTags];
    Bridge bridges[kMaxTags];
};

inline const CachedField* GetField(Work& wk, unsigned frame0, ImFont* font, float fs, int ss, float pad,
                                   const char* text, float textW, float hw)
{
    const ImFontAtlas* at = font->ContainerAtlas;
    const void* pixels = at ? at->TexPixelsAlpha8 : nullptr;
    const void* glyphs = font->Glyphs.Data;
    const int   tw = at ? at->TexWidth : 0, th = at ? at->TexHeight : 0;
    const unsigned now = ++wk.clock;
    for (CachedField& c : wk.cache)
        if (wk.reuse && c.font == font && c.pixels == pixels && c.glyphs == glyphs && c.tw == tw && c.th == th && c.fs == fs &&
            c.ss == ss && c.hw == hw && c.pad == pad && c.text == text)
        { c.stamp = now; return &c; }

    CachedField* c = nullptr;
    for (CachedField& e : wk.cache)
        if (e.stamp <= frame0 && (!c || e.stamp < c->stamp)) c = &e;
    if (!c || (int)wk.cache.size() < kFieldCache) c = &wk.cache.emplace_back();

    const float f = (float)ss;
    c->text = text; c->font = font; c->pixels = pixels; c->glyphs = glyphs; c->tw = tw; c->th = th;
    c->fs = fs; c->hw = hw; c->pad = pad; c->ss = ss; c->stamp = now;
    c->cx = (hw + pad) * f;  c->cy = (fs * 0.5f + pad) * f;
    c->w  = (int)ceilf(2.f * c->cx); c->h = (int)ceilf(2.f * c->cy);
    const size_t a = (size_t)c->w * c->h;
    wk.cov.assign(a, 0);
    wk.dOut.resize(a); wk.dIn.resize(a); wk.sOut.resize(a); wk.sIn.resize(a);
    if (!BlitLabel(wk.cov.data(), c->w, c->h, ImVec2(0.f, 0.f), font, fs * f, text,
                   ImVec2(c->cx - textW * f * 0.5f, c->cy - fs * f * 0.5f)))
    { c->font = nullptr; return nullptr; }
    c->d.resize(a); c->s.resize(a);
    SignedDist(wk.cov.data(), c->w, c->h, wk.dOut.data(), wk.dIn.data(), wk.sOut.data(), wk.sIn.data(),
               c->d.data(), c->s.data());

    c->i0 = c->w; c->i1 = -1; c->j0 = c->h; c->j1 = -1; c->smin = 1.f;
    for (int y = 0; y < c->h; ++y)
        for (int x = 0; x < c->w; ++x)
        {
            const size_t k = (size_t)y * c->w + x;
            if (c->s[k] < c->smin) c->smin = c->s[k];
            if (c->d[k] * c->s[k] >= 1.f) continue;
            if (x < c->i0) c->i0 = x; if (x > c->i1) c->i1 = x;
            if (y < c->j0) c->j0 = y; if (y > c->j1) c->j1 = y;
        }
    if (c->smin < 1e-3f) c->smin = 1e-3f;
    return c;
}

inline bool Render(const Tag* tags, int n, ImFont* font, float fs, bool axisX, Work& wk,
                   int& ox, int& oy, int& w, int& h)
{
    if (n <= 0 || n > kMaxTags) return false;
    const auto fontOf = [&](int i) { return tags[i].font ? tags[i].font : font; };
    const auto fsOf   = [&](int i) { return tags[i].fs > 0.f ? tags[i].fs : fs; };
    float fsMax = fs;
    for (int i = 0; i < n; ++i) fsMax = fmaxf(fsMax, fsOf(i));

    const int   ss  = fsMax < 20.f ? 3 : 2;
    const float f   = (float)ss;
    const float pad = ceilf(fsMax * 0.16f + 3.f);

    float xTop[kMaxTags], xBot[kMaxTags], stem[kMaxTags];
    for (int i = 0; i < n; ++i)
    {
        ImFont* const fo = fontOf(i);
        const float   s  = fsOf(i);
        xTop[i] = s * 0.35f; xBot[i] = s * 0.8f;
        if (const ImFontGlyph* gx = fo->FindGlyph((ImWchar)'x'))
        {
            const float sc = s / fo->FontSize;
            if (gx->Y1 > gx->Y0) { xTop[i] = gx->Y0 * sc; xBot[i] = gx->Y1 * sc; }
        }
        stem[i] = s * 0.16f;
        if (const ImFontGlyph* gl = fo->FindGlyph((ImWchar)'l'))
            if (ImFontAtlas* at = fo->ContainerAtlas)
                if (at->TexPixelsAlpha8 && gl->X1 > gl->X0)
                {
                    const int row = (int)(((gl->V0 + gl->V1) * 0.5f) * at->TexHeight);
                    const int c0  = (int)(gl->U0 * at->TexWidth), c1 = (int)(gl->U1 * at->TexWidth);
                    float ink = 0.f;
                    for (int c = c0; c < c1; ++c) ink += at->TexPixelsAlpha8[row * at->TexWidth + c] / 255.f;
                    if (ink > 0.5f) stem[i] = ink * (s / fo->FontSize);
                }
    }

    const auto slot = [&](int i)
    { return ImVec2(tags[i].pos.x + tags[i].size.x * 0.5f, tags[i].pos.y + fsOf(i) * 0.5f); };
    ImVec2 ctr[kMaxTags];
    for (int i = 0; i < n; ++i)
    {
        ctr[i] = slot(i);
        const int p = tags[i].parent;
        if (p < 0) continue;
        const ImVec2 cp = slot(p);
        const float  k  = SlideAt(tags[i].raw);
        ctr[i] = ImVec2(cp.x + (ctr[i].x - cp.x) * k, cp.y + (ctr[i].y - cp.y) * k);
    }

    float hws[kMaxTags];
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    for (int i = 0; i < n; ++i)
    {
        const int p = tags[i].parent;
        float hw = tags[i].size.x * 0.5f;
        if (p >= 0) hw = fmaxf(hw, tags[p].size.x * 0.5f);
        hws[i] = hw;
        bx0 = fminf(bx0, ctr[i].x - hw - pad);        bx1 = fmaxf(bx1, ctr[i].x + hw + pad);
        by0 = fminf(by0, ctr[i].y - fsOf(i) * 0.5f - pad); by1 = fmaxf(by1, ctr[i].y + fsOf(i) * 0.5f + pad);
    }
    ox = (int)floorf(bx0); oy = (int)floorf(by0);
    w  = (int)ceilf(bx1) - ox; h = (int)ceilf(by1) - oy;
    if (w < 2 || h < 2 || w > 1024 || h > 1024) return false;

    if (wk.cache.capacity() < 2 * kMaxTags) wk.cache.reserve(2 * kMaxTags);
    const unsigned frame0 = wk.clock;
    const auto field = [&](Field& F, int i, int j) -> const CachedField*
    {
        const CachedField* c = GetField(wk, frame0, fontOf(j), fsOf(j), ss, pad, tags[j].text, tags[j].size.x, hws[i]);
        if (!c) return nullptr;
        F.x0 = (ctr[i].x - ox) * f - c->cx; F.y0 = (ctr[i].y - oy) * f - c->cy;
        F.w = c->w; F.h = c->h; F.d = c->d.data(); F.s = c->s.data();
        return c;
    };
    const CachedField* cSelf[kMaxTags] = {};
    const CachedField* cFrom[kMaxTags] = {};

    const auto extent = [&](int i, float halfW, float& lo, float& hi)
    {
        if (axisX) { lo = ctr[i].x - halfW; hi = ctr[i].x + halfW; }
        else       { const float top = ctr[i].y - fsOf(i) * 0.5f; lo = top + xTop[i]; hi = top + xBot[i]; }
    };

    float wPiv[kMaxTags], wSa[kMaxTags];
    for (int i = 0; i < n; ++i) { wPiv[i] = axisX ? ctr[i].x : ctr[i].y; wSa[i] = 1.f; }
    for (int i = 0; i < n; ++i)
    {
        const int p = tags[i].parent;
        if (p < 0) continue;
        const float uP = axisX ? ctr[p].x : ctr[p].y, uC = axisX ? ctr[i].x : ctr[i].y;
        const float m  = MorphAt(tags[i].raw);
        const float eC = axisX ? (tags[p].size.x + (tags[i].size.x - tags[p].size.x) * m) * 0.5f : fsOf(i) * 0.5f;
        const float eP = axisX ? tags[p].size.x * 0.5f : fsOf(p) * 0.5f;
        const miw::Warp wc = miw::SplitWarp(uC, eC, uP, tags[i].raw);
        const miw::Warp wp = miw::SplitWarp(uP, eP, uC, tags[i].raw);
        wPiv[i] = wc.piv; wSa[i] = wc.sa;
        wPiv[p] = wp.piv; wSa[p] = wp.sa;
    }
    const auto warpU = [&](int i, float u) { return wPiv[i] + (u - wPiv[i]) * wSa[i]; };

    for (int i = 0; i < n; ++i)
    {
        const Tag& t = tags[i];
        Comp& c = wk.comps[i];
        c = Comp{};
        c.parent = t.parent;
        for (int k = 0; k < 3; ++k) c.col[k] = t.col[k];
        if (!(cSelf[i] = field(c.self, i, i))) return false;
        if (axisX) { c.px = (wPiv[i] - ox) * f; c.sx = wSa[i]; c.py = (ctr[i].y - oy) * f; c.sy = 1.f / wSa[i]; }
        else       { c.py = (wPiv[i] - oy) * f; c.sy = wSa[i]; c.px = (ctr[i].x - ox) * f; c.sx = 1.f / wSa[i]; }

        if (t.parent >= 0)
        {
            const Tag& p = tags[t.parent];
            if (!(cFrom[i] = field(c.from, i, t.parent))) return false;
            c.m = MorphAt(t.raw);
            for (int k = 0; k < 3; ++k) c.col[k] = p.col[k] + (t.col[k] - p.col[k]) * c.m;
        }
        else if (t.parent == -1)
        {
            c.px = (ctr[i].x - ox) * f; c.py = (ctr[i].y - oy) * f;
            c.sx = c.sy = PopAt(t.raw);
        }
    }

    int nb = 0;
    for (int i = 0; i < n; ++i)
    {
        const Tag& t = tags[i];
        if (t.parent < 0) continue;
        const Tag&  p    = tags[t.parent];
        const float m    = wk.comps[i].m;
        const float uP   = axisX ? ctr[t.parent].x : ctr[t.parent].y;
        const float uC   = axisX ? ctr[i].x : ctr[i].y;
        const float side = uC >= uP ? 1.f : -1.f;
        const int   q    = t.parent;
        const float fsB  = fminf(fsOf(q), fsOf(i));
        float pLo, pHi, cLo, cHi;
        extent(t.parent, p.size.x * 0.5f, pLo, pHi);
        extent(i, (p.size.x + (t.size.x - p.size.x) * m) * 0.5f, cLo, cHi);
        const float o   = axisX ? ox : oy;
        const float bP  = warpU(t.parent, side > 0.f ? pHi : pLo);
        const float bC  = warpU(i, side > 0.f ? cLo : cHi);
        float cy = ctr[q].y - fsOf(q) * 0.5f + (xTop[q] + xBot[q]) * 0.5f;
        if (fsOf(q) != fsOf(i) || fontOf(q) != fontOf(i))
        {
            const float topP = ctr[q].y - fsOf(q) * 0.5f, topC = ctr[i].y - fsOf(i) * 0.5f;
            const float lo = fmaxf(topP + xTop[q], topC + xTop[i]), hi = fminf(topP + xBot[q], topC + xBot[i]);
            cy = lo < hi ? (lo + hi) * 0.5f : (cy + topC + (xTop[i] + xBot[i]) * 0.5f) * 0.5f;
        }
        const float cv  = axisX ? (cy - oy) * f : (ctr[t.parent].x - ox) * f;
        const auto ink = [&](int ci, float edge, float dir, float reach, bool& hit) -> float
        {
            const float from = (edge - o) * f;
            const float back = fminf(fmaxf((bC - bP) * side, 0.f) * 0.5f, fsB * 0.1f) * f;
            const auto  at   = [&](float u) { return CompDist(wk.comps[ci], axisX ? u : cv, axisX ? cv : u); };
            float prev = kOut;
            for (float s = -back; s <= reach * f; s += 1.f)
            {
                const float u = from + dir * s;
                const float d = at(u);
                if (d < 0.f)
                {
                    hit = true;
                    if (prev >= kOut) return u;
                    float a = u - dir, da = prev, b = u, db = d;
                    for (int it = 0; it < 4; ++it)
                    {
                        const float mid = 0.5f * (a + b), dm = at(mid);
                        if (dm < 0.f) { b = mid; db = dm; } else { a = mid; da = dm; }
                    }
                    return a + (b - a) * da / (da - db);
                }
                prev = d;
            }
            hit = false;
            return from;
        };
        const float reachP = axisX ? p.size.x * 0.5f : xBot[q] - xTop[q];
        const float reachC = axisX ? (p.size.x + (t.size.x - p.size.x) * m) * 0.5f : xBot[i] - xTop[i];
        bool hitP, hitC;
        const float iP  = ink(t.parent, bP, -side, reachP, hitP);
        const float iC  = ink(i, bC, side, reachC, hitC);
        const float gap = (iC - iP) * side / f;
        const float tP  = hitP ? stem[q] * 0.5f : fsOf(q) * 0.08f, tC = hitC ? stem[i] * 0.5f : fsOf(i) * 0.08f;

        Bridge& br = wk.bridges[nb++];
        br = Bridge{};
        br.eP = iP - side * tP * f;
        br.eC = iC + side * tC * f;
        br.side = side;
        br.cv = cv;
        br.hs     = gap > 0.f ? fminf(stem[q], stem[i]) * 0.5f * Smooth(0.f, fsB * 0.05f, gap) * f : 0.f;
        br.depth  = DepthAt(t.raw);
        br.cut    = CutAt(t.raw);
        br.r      = fsB * 0.18f * f;
        br.parent = t.parent; br.child = i;
    }

    float cull[4 * 2 * kMaxTags];
    int   nCull = 0;
    float smin = 1.f, rmax = 0.f;
    for (int i = 0; i < n; ++i)
    {
        Comp& c = wk.comps[i];
        float qx0 = 1e9f, qy0 = 1e9f, qx1 = -1e9f, qy1 = -1e9f;
        float fx0 = 1e9f, fy0 = 1e9f, fx1 = -1e9f, fy1 = -1e9f;
        const auto take = [&](const CachedField* cf, const Field& F)
        {
            if (!cf) return;
            smin = fminf(smin, cf->smin);
            fx0 = fminf(fx0, F.x0); fx1 = fmaxf(fx1, F.x0 + F.w);
            fy0 = fminf(fy0, F.y0); fy1 = fmaxf(fy1, F.y0 + F.h);
            if (cf->i1 < cf->i0) return;
            qx0 = fminf(qx0, F.x0 + cf->i0 - 0.5f); qx1 = fmaxf(qx1, F.x0 + cf->i1 + 1.5f);
            qy0 = fminf(qy0, F.y0 + cf->j0 - 0.5f); qy1 = fmaxf(qy1, F.y0 + cf->j1 + 1.5f);
        };
        take(cSelf[i], c.self);
        if (c.parent >= 0) take(cFrom[i], c.from);
        c.rx0 = c.px + (fx0 - c.px) * c.sx; c.rx1 = c.px + (fx1 - c.px) * c.sx;
        c.ry0 = c.py + (fy0 - c.py) * c.sy; c.ry1 = c.py + (fy1 - c.py) * c.sy;
        if (qx1 < qx0) continue;
        float* b = cull + 4 * nCull++;
        b[0] = c.px + (qx0 - c.px) * c.sx; b[2] = c.px + (qx1 - c.px) * c.sx;
        b[1] = c.py + (qy0 - c.py) * c.sy; b[3] = c.py + (qy1 - c.py) * c.sy;
    }
    for (int k = 0; k < nb; ++k) rmax = fmaxf(rmax, wk.bridges[k].r);
    const float M = 0.5f / smin + rmax * 0.25f;
    for (int k = 0; k < nb; ++k)
    {
        const Bridge& br = wk.bridges[k];
        if (br.hs <= 0.f || br.cut >= 1.f || (br.eC - br.eP) * br.side <= 0.f) continue;
        const float u0 = fminf(br.eP, br.eC), u1 = fmaxf(br.eP, br.eC), half = br.hs + br.r + M;
        float* b = cull + 4 * nCull++;
        if (axisX) { b[0] = u0; b[2] = u1; b[1] = br.cv - half; b[3] = br.cv + half; }
        else       { b[1] = u0; b[3] = u1; b[0] = br.cv - half; b[2] = br.cv + half; }
    }

    Scene s;
    s.comps = wk.comps; s.nc = n;
    s.bridges = wk.bridges; s.nb = nb;
    s.axisX = axisX;
    s.w = w * ss; s.h = h * ss; s.ss = ss;
    if (wk.cull) { s.cull = cull; s.ncull = nCull; }
    wk.px.resize((size_t)w * h);
    Compose(s, wk.px.data());
    return true;
}
}
