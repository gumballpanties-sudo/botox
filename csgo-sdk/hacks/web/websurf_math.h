#pragma once
#include <cctype>
#include <cmath>
#include <string>

struct WsVec3
{
    float x = 0.f, y = 0.f, z = 0.f;
    WsVec3() {}
    WsVec3(float X, float Y, float Z) : x(X), y(Y), z(Z) {}
    WsVec3 operator+(const WsVec3& v) const { return { x + v.x, y + v.y, z + v.z }; }
    WsVec3 operator-(const WsVec3& v) const { return { x - v.x, y - v.y, z - v.z }; }
    WsVec3 operator*(float f)         const { return { x * f, y * f, z * f }; }
};

inline float  WsDot(const WsVec3& a, const WsVec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline WsVec3 WsCross(const WsVec3& a, const WsVec3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float  WsLen(const WsVec3& v) { return sqrtf(WsDot(v, v)); }
inline WsVec3 WsNorm(const WsVec3& v)
{
    const float l = WsLen(v);
    return l > 1e-6f ? v * (1.f / l) : WsVec3(0.f, 0.f, 1.f);
}

inline void WsAngleVectors(const WsVec3& ang, WsVec3& fwd, WsVec3& right, WsVec3& up)
{
    const float k = 3.14159265f / 180.f;
    const float sp = sinf(ang.x * k), cp = cosf(ang.x * k);
    const float sy = sinf(ang.y * k), cy = cosf(ang.y * k);
    const float sr = sinf(ang.z * k), cr = cosf(ang.z * k);

    fwd   = WsVec3(cp * cy, cp * sy, -sp);
    right = WsVec3(-1.f * sr * sp * cy + -1.f * cr * -sy,
                   -1.f * sr * sp * sy + -1.f * cr *  cy,
                   -1.f * sr * cp);
    up    = WsVec3(cr * sp * cy + -sr * -sy,
                   cr * sp * sy + -sr *  cy,
                   cr * cp);
}

inline void WsBasisFromNormal(const WsVec3& normal, const WsVec3& viewFwd,
                              WsVec3& right, WsVec3& up)
{
    const WsVec3 n = WsNorm(normal);
    WsVec3 upRef(0.f, 0.f, 1.f);
    if (fabsf(WsDot(n, upRef)) > 0.99f) {
        WsVec3 flat = viewFwd - n * WsDot(viewFwd, n);
        if (WsLen(flat) < 1e-3f) flat = (fabsf(n.x) < 0.9f) ? WsVec3(1.f, 0.f, 0.f)
                                                            : WsVec3(0.f, 1.f, 0.f);
        upRef = WsNorm(flat);
        right = WsNorm(WsCross(upRef, n));
        up    = WsNorm(WsCross(n, right));
        return;
    }
    right = WsNorm(WsCross(upRef, n));
    up    = WsNorm(WsCross(n, right));
}

inline bool WsRayPlane(const WsVec3& ro, const WsVec3& rd,
                       const WsVec3& p0, const WsVec3& n, float& t)
{
    const float denom = WsDot(rd, n);
    if (fabsf(denom) < 1e-6f) return false;
    t = WsDot(p0 - ro, n) / denom;
    return t > 0.f;
}

inline void WsQuadUV(const WsVec3& hit, const WsVec3& p0,
                     const WsVec3& right, const WsVec3& up,
                     float width, float height, float& u, float& v)
{
    const WsVec3 d = hit - p0;
    u = 0.5f + WsDot(d, right) / (width  > 1e-6f ? width  : 1e-6f);
    v = 0.5f - WsDot(d, up)    / (height > 1e-6f ? height : 1e-6f);
}

inline bool WsInQuad(float u, float v) { return u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f; }

struct WsAspect { const char* name; int w, h; };
inline constexpr WsAspect k_wsAspects[] = {
    { "16:9", 16, 9 }, { "9:16", 9, 16 }, { "4:3", 4, 3 }, { "3:4", 3, 4 }, { "21:9", 21, 9 }, { "1:1", 1, 1 },
};
inline constexpr int k_wsAspectCount = (int)(sizeof(k_wsAspects) / sizeof(k_wsAspects[0]));
inline const WsAspect& WsAspectAt(int i) { return k_wsAspects[(i >= 0 && i < k_wsAspectCount) ? i : 0]; }

inline void WsAspectRes(int aspect, int shortSide, int& w, int& h)
{
    const WsAspect& a = WsAspectAt(aspect);
    if (shortSide < 144) shortSide = 144;
    const int lo = a.w < a.h ? a.w : a.h, hi = a.w < a.h ? a.h : a.w;
    const int longSide = ((shortSide * hi / lo) + 1) & ~1;
    if (a.w >= a.h) { w = longSide; h = shortSide; } else { w = shortSide; h = longSide; }
}

inline void WsAspectDims(int aspect, float scale, float& w, float& h)
{
    const WsAspect& a = WsAspectAt(aspect);
    if (scale < 0.01f) scale = 0.01f;
    const float longSide = 128.f * scale;
    if (a.w >= a.h) { w = longSide; h = longSide * (float)a.h / (float)a.w; }
    else            { h = longSide; w = longSide * (float)a.w / (float)a.h; }
}

inline std::string WsNormalizeUrl(const std::string& in)
{
    size_t b = 0, e = in.size();
    while (b < e && (unsigned char)in[b] <= ' ') ++b;
    while (e > b && (unsigned char)in[e - 1] <= ' ') --e;
    const std::string s = in.substr(b, e - b);
    if (s.empty()) return s;

    size_t c = 0;
    while (c < s.size() && (isalnum((unsigned char)s[c]) || s[c] == '+' || s[c] == '-' || s[c] == '.')) ++c;
    if (c > 0 && c < s.size() && s[c] == ':' && isalpha((unsigned char)s[0])) {
        std::string scheme = s.substr(0, c);
        for (char& ch : scheme) ch = (char)tolower((unsigned char)ch);
        if (s.compare(c + 1, 2, "//") == 0 || scheme == "about" || scheme == "data" || scheme == "javascript" ||
            scheme == "mailto" || scheme == "view-source")
            return s;
    }

    bool space = false, dot = false;
    for (char ch : s) { space |= (ch == ' ' || ch == '\t'); dot |= (ch == '.'); }
    std::string lower = s;
    for (char& ch : lower) ch = (char)tolower((unsigned char)ch);
    const bool localhost = lower.compare(0, 9, "localhost") == 0 &&
                           (lower.size() == 9 || lower[9] == ':' || lower[9] == '/');
    if (!space && ((dot && s.front() != '.' && s.back() != '.') || localhost)) return "https://" + s;

    static const char* hex = "0123456789ABCDEF";
    std::string q = "https://www.google.com/search?q=";
    for (unsigned char ch : s) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') q += (char)ch;
        else if (ch == ' ') q += '+';
        else { q += '%'; q += hex[ch >> 4]; q += hex[ch & 15]; }
    }
    return q;
}

inline WsVec3 WsQuadPoint(float u, float v,
                          const WsVec3& p0, const WsVec3& right, const WsVec3& up,
                          float width, float height)
{
    return p0 + right * ((u - 0.5f) * width) + up * ((0.5f - v) * height);
}

inline int WsRoundedRectOutline(float rx, float ry, int seg, float* outUV, int maxPts)
{
    if (rx < 0.f) rx = 0.f;
    if (ry < 0.f) ry = 0.f;
    if (rx > 0.5f) rx = 0.5f;
    if (ry > 0.5f) ry = 0.5f;

    const bool square = (rx <= 1e-5f || ry <= 1e-5f || seg < 1);
    if (square) {
        if (maxPts < 4) return 0;
        const float corners[4][2] = { { 0.f, 0.f }, { 1.f, 0.f }, { 1.f, 1.f }, { 0.f, 1.f } };
        for (int i = 0; i < 4; ++i) { outUV[i * 2] = corners[i][0]; outUV[i * 2 + 1] = corners[i][1]; }
        return 4;
    }

    const float cx[4] = { rx,        1.f - rx,  1.f - rx,  rx       };
    const float cy[4] = { ry,        ry,        1.f - ry,  1.f - ry };
    const float a0[4] = { 3.14159265f, 4.71238898f, 0.f, 1.57079633f };

    int n = 0;
    for (int c = 0; c < 4; ++c) {
        for (int i = 0; i <= seg; ++i) {
            if (n >= maxPts) return n;
            const float a = a0[c] + (1.57079633f * (float)i / (float)seg);
            outUV[n * 2]     = cx[c] + rx * cosf(a);
            outUV[n * 2 + 1] = cy[c] + ry * sinf(a);
            ++n;
        }
    }
    return n;
}

inline void WsTranspose4(const float* src, float* dst)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            dst[r * 4 + c] = src[c * 4 + r];
}

inline bool WsIsProjection(const float* m)
{
    return m[12] != 0.f || m[13] != 0.f || m[14] != 0.f;
}
