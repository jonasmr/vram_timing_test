// shaders.h: the HLSL of vramtiming, compiled at startup with D3DCompile.
//
//   FullscreenVS : fullscreen triangle, used by both passes
//   ChainPS      : chain pass, RT[i] = noise(seed, index, RT[i-1])
//   ShowPS       : show pass, the last RT scaled into the window

#pragma once

inline const char* kShaders = R"(
cbuffer Constants : register(b0)
{
    uint g_seed;   // new random value every frame
    uint g_index;  // index of the render target being drawn
};
Texture2D<float4> g_previous : register(t0); // chain pass: RT[index - 1]; show pass: the last RT
SamplerState g_bilinear : register(s0);

struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

// Fullscreen triangle generated from the vertex index; no vertex buffer.
VSOut FullscreenVS(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

uint Hash(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Chain pass: noise from (pixel, seed, index), mixed with the same pixel of the previous render target.
float4 ChainPS(VSOut i) : SV_Target
{
    uint2 p = uint2(i.pos.xy);
    uint h = Hash(p.x + p.y * 2048u + g_seed * 0x9E3779B9u + g_index * 7919u);
    if (g_index > 0)
    {
        uint4 b = uint4(g_previous.Load(int3(p, 0)) * 255.0 + 0.5);
        h = Hash(h ^ (b.x | (b.y << 8) | (b.z << 16) | (b.w << 24)));
    }
    return float4(h & 255u, (h >> 8) & 255u, (h >> 16) & 255u, h >> 24) / 255.0;
}

// Show pass: the last render target, scaled to the window.
float4 ShowPS(VSOut i) : SV_Target
{
    return float4(g_previous.SampleLevel(g_bilinear, i.uv, 0).rgb, 1);
}
)";
