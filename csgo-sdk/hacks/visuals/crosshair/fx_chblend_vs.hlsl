// Vertex shader for the custom crosshair's blend modes. SOURCE OF RECORD for fx_chblend_vs.h.
//
// Rebuild after ANY edit here:
//   fxc /T vs_3_0 /E main /Fh src/features/fx_chblend_vs.h /Vn k_fxChBlendVsBytes src/features/fx_chblend_vs.hlsl
//
// Only exists because the blend pixel shader is ps_3_0 (VPOS, flow control) and D3D9 will
// not pair that with fixed-function vertex processing. It eats ImGui's own FVF stream
// (XYZ | DIFFUSE | TEX1) as-is and redoes the ortho imgui_impl_dx9 hands the fixed
// pipeline: L = 0.5, R = W + 0.5, T = 0.5, B = H + 0.5 -- the dx9 half-pixel shift.

float4 g_vp : register(c0);   // xy = viewport size ImGui set

struct VS_IN  { float3 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };
struct VS_OUT { float4 pos : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };

VS_OUT main(VS_IN i)
{
    VS_OUT o;
    o.pos = float4((2.0 * i.pos.x - g_vp.x - 1.0) / g_vp.x,
                   (g_vp.y + 1.0 - 2.0 * i.pos.y) / g_vp.y,
                   0.5, 1.0);
    o.col = i.col;
    o.uv  = i.uv;
    return o;
}
