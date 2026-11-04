// Blend modes for the custom crosshair. SOURCE OF RECORD for fx_chblend_ps.h.
//
// Rebuild after ANY edit here (runs fxc once per mode, writes the header + its table):
//   bash src/features/fx_chblend_build.sh
//
// One shader PER MODE, picked by /D MODE=n. A single shader holding every mode behind a
// runtime id blows ps_3_0's 32 temp registers (X4505); with MODE a compile-time constant
// fxc folds the whole dispatch away, so each one is a handful of instructions and the GPU
// never branches on the mode per pixel.
//
// Formulas are Krita's, function for function: libs/pigment/compositeops/
// KoCompositeOpFunctions.h plus the HSX helpers at the bottom of KoColorSpaceMaths.h.
// s = crosshair colour, d = the frame under it, both sRGB-encoded 0..1 -- the same numbers
// Krita works on for an 8-bit sRGB layer. Every formula keeps Krita's argument order, so a
// "(d, s)" call below is Krita swapping them on purpose (overlay = hard light swapped, ...).
//
// Opacity is NOT done here. rgb out = the blended colour, alpha out = the crosshair's alpha,
// and ImGui's SRCALPHA / INVSRCALPHA does lerp(dst, blend, alpha) -- which is exactly the
// composite Krita runs over an opaque layer.
//
// Mode ids are stable (saved in the cfg) and must match kChBlend[] in crosshair.cpp.
// 0 and 1 (normal / additive) never reach this shader.
//
// Divisions are guarded with max(.., tiny) BEFORE the select, never after: fxc may compile
// a ?: into a lerp, and a NaN in the unpicked arm would leak through one.

sampler2D s_tex : register(s0);   // ImGui's bound texture (atlas white pixel for rects)
sampler2D s_dst : register(s1);   // copy of the render target taken just before the arms
float4    g_p   : register(c0);   // yz = 1 / render target size

#ifndef MODE
#define MODE 2
#endif

static const float Z    = 0.5 / 255.0;      // "is 0" / "is 1" on 8-bit input
static const float ONE  = 1.0 - 0.5 / 255.0;
static const float TINY = 1e-6;
static const float EPSF = 1.1920929e-7;     // numeric_limits<float>::epsilon, Krita's HSX guard
static const float PI   = 3.14159265358979;

// ---- shared pieces --------------------------------------------------------------------
float3 Screen(float3 s, float3 d)  { return s + d - s * d; }
float3 To8(float3 x) { return floor(saturate(x) * 255.0 + 0.5); }
// CFHardMixPhotoshop: an integer sum > 255 in Krita, so 11 + 244 is NOT over. On floats
// 11/255 + 244/255 can come out 1.0000001; compare the 8-bit values instead.
float3 HardMixPS(float3 s, float3 d) { return (To8(s) + To8(d) > 255.5) ? 1.0 : 0.0; }

// pow that matches C for a 0 base: 0^y = 0 (y > 0), 0^0 = 1. HLSL's exp(y*log x) is NaN there.
float3 Pw(float3 x, float3 y)
{
    return (x > 0.0) ? pow(max(x, 1e-30), y) : ((y > 0.0) ? 0.0 : 1.0);
}

float3 Dodge(float3 s, float3 d)   // CFColorDodge
{
    return (s >= ONE) ? ((d <= Z) ? 0.0 : 1.0) : saturate(d / max(1.0 - s, TINY));
}

float3 Burn(float3 s, float3 d)    // CFColorBurn
{
    return (d >= ONE) ? 1.0 : ((s <= Z) ? 0.0 : 1.0 - saturate((1.0 - d) / max(s, TINY)));
}

float3 HardLight(float3 s, float3 d)   // CFHardLight
{
    return (s > 0.5) ? Screen(2.0 * s - 1.0, d) : 2.0 * s * d;
}

float3 SoftLightPS(float3 s, float3 d)  // CFSoftLight
{
    return (s > 0.5) ? d + (2.0 * s - 1.0) * (sqrt(d) - d)
                     : d - (1.0 - 2.0 * s) * d * (1.0 - d);
}

float3 SoftLightSvg(float3 s, float3 d) // CFSoftLightSvg
{
    float3 D = (d > 0.25) ? sqrt(d) : ((16.0 * d - 12.0) * d + 4.0) * d;
    return (s > 0.5) ? d + (2.0 * s - 1.0) * (D - d)
                     : d - (1.0 - 2.0 * s) * d * (1.0 - d);
}

float3 VividLight(float3 s, float3 d)   // CFVividLight, SDR clamp
{
    float3 lo = (s <= Z) ? ((d >= ONE) ? d : 0.0)
                         : saturate(1.0 - (1.0 - d) / max(2.0 * s, TINY));
    float3 hi = (s >= ONE) ? ((d <= Z) ? 0.0 : 1.0)
                           : saturate(d / max(2.0 * (1.0 - s), TINY));
    return (s < 0.5) ? lo : hi;
}

float3 PinLight(float3 s, float3 d) { return max(2.0 * s - 1.0, min(d, 2.0 * s)); }

float3 ArcTan(float3 s, float3 d)       // cfArcTangent: atan2(0,0) = 0 like C
{
    return 2.0 * atan2(s, max(d, 1e-7)) / PI;
}

float3 PenumbraB(float3 s, float3 d)    // CFPenumbraB
{
    float3 r = (s <= Z) ? 0.0 : 1.0 - saturate((1.0 - d) / max(s, TINY) * 0.5);
    r = (d + s < 1.0) ? Dodge(d, s) * 0.5 : r;
    return (d >= ONE) ? 1.0 : r;
}
float3 PenumbraA(float3 s, float3 d) { return PenumbraB(d, s); }
float3 PenumbraD(float3 s, float3 d) { return (d >= ONE) ? 1.0 : ArcTan(s, 1.0 - d); }
float3 PenumbraC(float3 s, float3 d) { return PenumbraD(d, s); }

float3 GammaDark(float3 s, float3 d)    // CFGammaDark
{
    float3 r = (s <= Z) ? 0.0 : pow(max(d, 1e-30), 1.0 / max(s, TINY));
    r = (d >= ONE) ? 1.0 : r;
    return (d <= Z) ? 0.0 : r;
}

float3 GammaLight(float3 s, float3 d)   // CFGammaLight
{
    float3 r = pow(max(d, 1e-30), s);
    r = (d >= ONE) ? 1.0 : r;
    return (d <= Z) ? 0.0 : r;
}

// The quadratic family runs Krita's quint8 integer mul/div, not real arithmetic: mul rounds
// the square to 8 bits FIRST (mul(11,11) = 0), and that rounding decides whole results.
// UINT8_MULT(a,b) = round(a*b/255); UINT8_DIVIDE(a,b) = (a*255 + b/2) / b, integer.
float3 Mul8(float3 a, float3 b) { return floor(a * b / 255.0 + 0.5); }
float3 Div8(float3 a, float3 b) { return floor((a * 255.0 + floor(b * 0.5)) / max(b, 1.0)); }

float3 Glow(float3 s, float3 d)         // cfGlow
{
    float3 a = To8(s), id = 255.0 - To8(d);
    return (d >= ONE) ? 1.0 : min(Div8(Mul8(a, a), id), 255.0) / 255.0;
}
float3 Reflect(float3 s, float3 d) { return Glow(d, s); }
float3 Heat(float3 s, float3 d)         // cfHeat
{
    float3 is = 255.0 - To8(s);
    float3 r = (d <= Z) ? 0.0 : 1.0 - min(Div8(Mul8(is, is), To8(d)), 255.0) / 255.0;
    return (s >= ONE) ? 1.0 : r;
}
float3 Freeze(float3 s, float3 d) { return Heat(d, s); }

float3 Interp(float3 s, float3 d) { return 0.5 - 0.25 * cos(PI * s) - 0.25 * cos(PI * d); }

// Krita's Arithmetic::mod(a, 1) with its double eps: a - floor(a / (1 + eps)) * (1 + eps).
//
// EXACT MULTIPLES ARE A COIN TOSS IN KRITA. Its inputs are float32 k/255 promoted to double,
// so d/s = 7 really arrives as 6.99999997 or 7.00000003 depending on how k/255 rounded, and
// mod() then says ~1 or ~0. Float32 on the gpu has already thrown that bit away, so no shader
// reproduces it; each tie rule below is simply the one that agrees with Krita most often
// (tools/chblend_test counts the ties apart). Off an exact multiple every 8-bit ratio sits
// >= 1/255 away from an integer and the nudges move nothing.
float3 ModUp(float3 a)   { return a - max(floor(a - 1e-4), 0.0); }   // exact multiple -> 1, 0 stays 0
float3 ModDown(float3 a) { return a - floor(a + 1e-4); }   // exact multiple -> 0
float3 IsOdd(float3 x)   { return (frac(abs(x) * 0.5) > 0.25) ? 1.0 : 0.0; }

float3 DivMod(float3 s, float3 d)       // cfDivisiveModulo; s == 0 scales by 1/epsilon<quint8> = 1
{
    return (s <= Z) ? ModUp(d) : ModUp(d / max(s, TINY));
}
float3 DivModC(float3 s, float3 d)      // cfDivisiveModuloContinuous
{
    float3 m = DivMod(s, d);
    float3 r = (IsOdd(ceil(d / max(s, TINY) - 1e-4)) > 0.5) ? m : 1.0 - m;
    // s == 0: Krita recurses with T = double, 1/epsilon = 2^52, and mod(d * 2^52, 1) is 0 for
    // every 8-bit d (dumped from its own code, all 256)
    r = (s <= Z) ? 0.0 : r;
    return (d <= Z) ? 0.0 : r;
}
float3 ModShift(float3 s, float3 d)     // cfModuloShift
{
    float3 r = ModDown(d + s);
    r = (s <= Z) ? ModUp(d) : r;         // 0 + d is exact, so here Krita's 1.0 mod 1 = 1 is clean
    return (s >= ONE) * (d <= Z) > 0.5 ? 0.0 : r;
}
float3 ModShiftC(float3 s, float3 d)
{
    float3 m = ModShift(s, d);
    // Krita's parity at an exact integer k lands on "k odd -> 1" from either side; with
    // ModDown(k) = 0 that needs ceil to read k + 1
    float3 r = (max(IsOdd(ceil(d + s + 1e-4)), (float3)(d <= Z)) > 0.5) ? m : 1.0 - m;
    r = (s <= Z) ? m : r;                // s == 0: ceil(d) is 1, odd, so Krita keeps m
    return (s >= ONE) * (d <= Z) > 0.5 ? 1.0 : r;
}

// 8-bit bitwise op: 0 = and, 1 = or, 2 = xor
float3 Bits(float3 a, float3 b, float op)
{
    a = floor(saturate(a) * 255.0 + 0.5);
    b = floor(saturate(b) * 255.0 + 0.5);
    float3 r = 0.0;
    float p = 128.0;
    for (int i = 0; i < 8; i++)
    {
        float3 ab = (a >= p) ? 1.0 : 0.0;
        float3 bb = (b >= p) ? 1.0 : 0.0;
        a -= ab * p;
        b -= bb * p;
        float3 v = (op < 0.5) ? ab * bb : ((op < 1.5) ? max(ab, bb) : abs(ab - bb));
        r += v * p;
        p *= 0.5;
    }
    return r / 255.0;
}

// ---- HSX (t: 0 = HSY, 1 = HSI, 2 = HSL, 3 = HSV) ------------------------------------------
float Mx(float3 c) { return max(c.r, max(c.g, c.b)); }
float Mn(float3 c) { return min(c.r, min(c.g, c.b)); }

float GetL(float3 c, float t)
{
    if (t < 0.5) return dot(c, float3(0.299, 0.587, 0.114));
    if (t < 1.5) return (c.r + c.g + c.b) * 0.33333333;
    if (t < 2.5) return (Mx(c) + Mn(c)) * 0.5;
    return Mx(c);
}

float GetS(float3 c, float t)
{
    float mx = Mx(c), mn = Mn(c), ch = mx - mn;
    if (t < 0.5) return ch;
    if (t < 1.5) return (ch > EPSF) ? 1.0 - mn / max(GetL(c, 1.0), TINY) : 0.0;
    if (t < 2.5)
    {
        float dv = 1.0 - abs(mx + mn - 1.0);
        return (dv > EPSF) ? ch / dv : 0.0;
    }
    return (mx > EPSF) ? ch / max(mx, TINY) : 0.0;
}

float3 SetS(float3 c, float sat)        // setSaturation: min -> 0, max -> sat, mid scaled
{
    float mx = Mx(c), mn = Mn(c), ch = mx - mn;
    return (ch > EPSF) ? (c - mn) * sat / max(ch, TINY) : 0.0;
}

float3 ToneMap(float3 c, float t)       // ToneMapping<HSXType>
{
    float l = GetL(c, t), n = Mn(c), x = Mx(c);
    if (n < 0.0)
    {
        float st = l - n;
        c = (l <= 0.0 || st < EPSF) ? 0.0 : l + (c - l) * l / max(st, TINY);
    }
    if (x > 1.0)
    {
        float st = x - l;
        float3 fb = (t > 2.5) ? min(c, 1.0) : 1.0;    // HSV is the one non-average lightness
        c = (l > 1.0 || st < EPSF) ? fb : l + (c - l) * (1.0 - l) / max(st, TINY);
    }
    return c;
}

float3 AddL(float3 c, float l, float t) { return ToneMap(c + l, t); }
float3 SetL(float3 c, float l, float t) { return AddL(c, l - GetL(c, t), t); }

// op: 0 color, 1 hue, 2 saturation, 3 lightness, 4 dec sat, 5 inc sat, 6 dec light, 7 inc light
float3 Hsx(float op, float t, float3 s, float3 d)
{
    float ld = GetL(d, t);
    if (op < 0.5) return SetL(s, ld, t);
    if (op < 1.5) return SetL(SetS(s, GetS(d, t)), ld, t);
    if (op < 2.5) return SetL(SetS(d, GetS(s, t)), ld, t);
    if (op < 3.5) return SetL(d, GetL(s, t), t);
    if (op < 4.5) return SetL(SetS(d, GetS(d, t) * GetS(s, t)), ld, t);
    if (op < 5.5) return SetL(SetS(d, lerp(GetS(d, t), 1.0, GetS(s, t))), ld, t);
    if (op < 6.5) return AddL(d, GetL(s, t) - 1.0, t);
    return AddL(d, GetL(s, t), t);
}

float3 Lambert(float3 s, float3 d, float k, float q)   // CFLambertLighting*, HSI tone map
{
    float3 v = s * d * k;
    v = (v > 1.0) ? 1.0 + (v - 1.0) * (v - 1.0) * q : v;
    return ToneMap(v, 1.0);
}

// ---- dispatch -------------------------------------------------------------------------
float3 Blend(float m, float3 s, float3 d)
{
    // HSX blocks first: one call covers 32 ids
    if (m >= 89.5 && m < 113.5)
    {
        float k = m - 90.0;
        return Hsx(fmod(k, 8.0), floor(k / 8.0) + 1.0, s, d);
    }
    if (m >= 80.5 && m < 89.5)
    {
        // HSY, in Krita's menu order: color, hue, tint, saturation, luminosity,
        // dec sat, inc sat, dec lum, inc lum
        if (m < 81.5) return Hsx(0.0, 0.0, s, d);
        if (m < 82.5) return Hsx(1.0, 0.0, s, d);
        if (m < 83.5) { float l = GetL(d, 0.0); return l + s * (1.0 - l); }   // CFTint
        if (m < 84.5) return Hsx(2.0, 0.0, s, d);
        if (m < 85.5) return Hsx(3.0, 0.0, s, d);
        if (m < 86.5) return Hsx(4.0, 0.0, s, d);
        if (m < 87.5) return Hsx(5.0, 0.0, s, d);
        if (m < 88.5) return Hsx(6.0, 0.0, s, d);
        return Hsx(7.0, 0.0, s, d);
    }
    if (m >= 6.5 && m < 16.5)
    {
        // binary. Krita's own cfAnd/cfNor/... come out as each other's names (cfNor returns
        // and(), cfAnd inverts both into it); these are the operations the names say.
        if (m < 7.5)  return Bits(s, d, 2.0);             // xor
        if (m < 8.5)  return Bits(s, d, 1.0);             // or
        if (m < 9.5)  return Bits(s, d, 0.0);             // and
        if (m < 10.5) return 1.0 - Bits(s, d, 0.0);       // nand
        if (m < 11.5) return 1.0 - Bits(s, d, 1.0);       // nor
        if (m < 12.5) return 1.0 - Bits(s, d, 2.0);       // xnor
        if (m < 13.5) return Bits(1.0 - s, d, 1.0);       // implication  s -> d
        if (m < 14.5) return Bits(s, 1.0 - d, 0.0);       // not implication
        if (m < 15.5) return Bits(s, 1.0 - d, 1.0);       // converse     d -> s
        return Bits(1.0 - s, d, 0.0);                     // not converse
    }

    // arithmetic
    if (m < 2.5)  return saturate(s + d);
    if (m < 3.5)  return saturate(d - s);
    if (m < 4.5)  return s * d;
    if (m < 5.5)  return (d <= Z) ? 0.0 : ((s <= Z) ? 1.0 : saturate(d / max(s, TINY)));
    if (m < 6.5)  return saturate(d - (1.0 - s));

    // darken
    if (m < 17.5) return Burn(s, d);
    if (m < 18.5) return saturate(s + d - 1.0);
    if (m < 19.5) return min(s, d);
    if (m < 20.5) return GammaDark(s, d);
    if (m < 21.5) return (GetL(d, 0.0) > GetL(s, 0.0)) ? s : d;
    if (m < 22.5) return 1.0 - ((1.0 - d) * s + sqrt(1.0 - s));
    if (m < 23.5) return (s < 0.5) ? (1.0 - s) * s + s * d : s * d + s - s * s;
    if (m < 24.5) return 1.0 - Pw((s >= ONE) ? 1e-12 : 1.0 - s, d * 1.039999999);   // Krita: s = 1 -> .999999999999

    // lighten
    if (m < 25.5) return Dodge(s, d);
    if (m < 26.5) return saturate(s + d);
    if (m < 27.5) return max(s, d);
    if (m < 28.5) return saturate(2.0 * s + d - 1.0);
    if (m < 29.5) return Screen(s, d);
    if (m < 30.5) return PinLight(s, d);
    if (m < 31.5) return VividLight(s, d);
    if (m < 32.5)
    {
        float3 r = (HardMixPS(1.0 - s, d) > 0.5) ? PenumbraB(s, d) : PenumbraA(s, d);
        return (s <= Z) ? 0.0 : saturate(r);
    }
    if (m < 33.5) return HardLight(s, d);
    if (m < 34.5) return Pw(d, pow(2.0, 2.0 * (0.5 - s)));
    if (m < 35.5) return saturate(d * Screen(s, d) + s * d * (1.0 - d));
    if (m < 36.5) return SoftLightPS(s, d);
    if (m < 37.5) return SoftLightSvg(s, d);
    if (m < 38.5) return GammaLight(s, d);
    if (m < 39.5) return 1.0 - GammaDark(1.0 - s, 1.0 - d);
    if (m < 40.5) return (GetL(d, 0.0) < GetL(s, 0.0)) ? s : d;
    if (m < 41.5) return saturate(Pw(Pw(d, 2.3333333) + Pw(s, 2.3333333), 0.428571428571434));
    if (m < 42.5) return saturate(Pw(d * d * d * d + s * s * s * s, 0.25));
    if (m < 43.5)
    {
        float3 lo = 1.0 - Pw(Pw(1.0 - d, 2.875) + Pw(1.0 - 2.0 * s, 2.875), 1.0 / 2.875);
        float3 hi = Pw(Pw(d, 2.875) + Pw(2.0 * s - 1.0, 2.875), 1.0 / 2.875);
        return (s < 0.5) ? lo : hi;
    }
    if (m < 44.5) return s * (1.0 - d) + sqrt(d);
    if (m < 45.5)
    {
        float3 lo = (1.0 - (1.0 - s) * s) - (1.0 - d) * (1.0 - s);
        float3 hi = s - (1.0 - d) * (1.0 - s) + (1.0 - s) * (1.0 - s);
        return (s < 0.5) ? lo : hi;
    }
    if (m < 46.5) return (s >= ONE) ? 1.0 : Pw(d, (1.0 - s) * 1.039999999);

    // modulo
    if (m < 47.5)   // cfModulo on quint8 is integer a % (b + 1); the +1e-4 keeps an exact
    {               // multiple from flooring one short on float noise
        float3 b = s + 1.0 / 255.0;
        return d - floor(d / b + 1e-4) * b;
    }
    if (m < 48.5) return DivModC(s, d) * s;
    if (m < 49.5) return DivMod(s, d);
    if (m < 50.5) return DivModC(s, d);
    if (m < 51.5) return ModShift(s, d);
    if (m < 52.5) return ModShiftC(s, d);

    // negative
    if (m < 53.5) return abs(s - d);
    if (m < 54.5) return 1.0 - abs(d - s);   // Krita ships |d - s| (a dup of difference, marked
                                             // TODO); this is the 1 - |d - s| its comment means
    if (m < 55.5) return abs(sqrt(d) - sqrt(s));
    if (m < 56.5) return saturate(d + s - 2.0 * s * d);
    if (m < 57.5) return ArcTan(s, d);
    if (m < 58.5) return 1.0 - abs(1.0 - s - d);
    if (m < 59.5) return 1.0 - d;            // invert: not Krita, the frame flipped under the shape

    // mix
    if (m < 60.5) return HardLight(d, s);
    if (m < 61.5) return Lambert(s, d, 1.0 / 0.215686, 0.01925);
    if (m < 62.5) return Lambert(s, d, 2.0, 0.4);
    if (m < 63.5) return (d > 0.5) ? Dodge(s, d) : Burn(s, d);
    if (m < 64.5) return HardMixPS(s, d);
    if (m < 65.5) return saturate(3.0 * d - 2.0 * (1.0 - s));
    if (m < 66.5) return saturate(d + s - 0.5);
    if (m < 67.5) return saturate(d - s + 0.5);
    if (m < 68.5) return (min(s, d) <= Z) ? 0.0 : saturate(2.0 / (1.0 / max(d, TINY) + 1.0 / max(s, TINY)));
    if (m < 69.5) return (s + d) * 0.5;
    if (m < 70.5) return sqrt(d * s);
    if (m < 71.5)
    {
        float3 r = (s >= 0.5) ? saturate(d / max(2.0 * (1.0 - s), TINY)) : saturate(2.0 * s * d);
        r = (s >= ONE) ? 1.0 : r;
        return (d <= Z) ? 0.0 : r;
    }
    if (m < 72.5) return Interp(s, d);
    if (m < 73.5) { float3 i = Interp(s, d); return Interp(i, i); }
    if (m < 74.5) return PenumbraA(s, d);
    if (m < 75.5) return PenumbraB(s, d);
    if (m < 76.5) return PenumbraC(s, d);
    if (m < 77.5) return PenumbraD(s, d);

    // misc: copy one channel
    if (m < 78.5) return float3(s.r, d.g, d.b);
    if (m < 79.5) return float3(d.r, s.g, d.b);
    if (m < 80.5) return float3(d.r, d.g, s.b);

    // quadratic
    if (m < 114.5) return Reflect(s, d);
    if (m < 115.5) return Glow(s, d);
    if (m < 116.5) return Freeze(s, d);
    if (m < 117.5) return Heat(s, d);
    if (m < 118.5) return (d >= ONE) ? 1.0 : ((HardMixPS(s, d) > 0.5) ? Glow(s, d) : Heat(s, d));   // glow-heat
    if (m < 119.5)                                                                                      // heat-glow
    {
        float3 r = (s <= Z) ? 0.0 : Glow(s, d);
        return (HardMixPS(s, d) > 0.5) ? Heat(s, d) : r;
    }
    if (m < 120.5) return (s >= ONE) ? 1.0 : ((HardMixPS(d, s) > 0.5) ? Glow(d, s) : Heat(d, s));   // reflect-freeze
    {                                                                                                  // freeze-reflect
        float3 r = (d <= Z) ? 0.0 : Reflect(s, d);
        return (HardMixPS(s, d) > 0.5) ? Freeze(s, d) : r;
    }
}

float4 main(float4 col : COLOR0, float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR
{
    // both samples up top: no gradient ops inside the dynamic branches
    float4 src = tex2D(s_tex, uv) * col;
    float3 dst = tex2D(s_dst, (vpos + 0.5) * g_p.yz).rgb;
    return float4(saturate(Blend(MODE, saturate(src.rgb), dst)), src.a);
}
