// Relais - HLSL shaders, compiled at startup with d3dcompiler_47.dll (ships with Windows 10/11)
// SPDX-License-Identifier: MIT
#pragma once

static const char kShaderSource[] = R"HLSL(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

// One triangle that covers the whole viewport.
VSOut VSMain(uint id : SV_VertexID) {
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t;
    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

#ifdef PASS_DECODE
// ---------------------------------------------------------------------------
// Pass 1: decode the captured frame (NV12 / YUY2 / RGB) into RGB, pixel for pixel.
// ---------------------------------------------------------------------------
Texture2D<float>  texY      : register(t0);
Texture2D<float2> texUV     : register(t1);
Texture2D<float4> texPacked : register(t2);
SamplerState linearClamp : register(s0);

cbuffer Decode : register(b0) {
    int layout;     // 0 = NV12, 1 = YUY2, 2 = RGB32
    int fullRange;  // 1 = 0-255, 0 = 16-235
    int bt709;      // 1 = BT.709, 0 = BT.601
    int pad0;
};

float3 YuvToRgb(float y, float u, float v) {
    if (fullRange == 0) {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        u = (u - 16.0 / 255.0) * (255.0 / 224.0);
        v = (v - 16.0 / 255.0) * (255.0 / 224.0);
    }
    u -= 0.5;
    v -= 0.5;
    float3 rgb;
    if (bt709 != 0) rgb = float3(y + 1.5748 * v, y - 0.1873 * u - 0.4681 * v, y + 1.8556 * u);
    else            rgb = float3(y + 1.4020 * v, y - 0.3441 * u - 0.7141 * v, y + 1.7720 * u);
    return saturate(rgb);
}

float4 PSDecode(VSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    if (layout == 0) {
        // Luma at full resolution, chroma (half resolution) interpolated instead of blocky.
        float y = texY.Load(int3(p, 0));
        float w, h;
        texY.GetDimensions(w, h);
        float2 uv = texUV.SampleLevel(linearClamp, i.pos.xy / float2(w, h), 0);
        return float4(YuvToRgb(y, uv.x, uv.y), 1.0);
    }
    if (layout == 1) {
        // R8G8B8A8 texel = Y0 U Y1 V; chroma interpolated between neighbouring pairs.
        float4 t = texPacked.Load(int3(p.x >> 1, p.y, 0));
        float y = (p.x & 1) ? t.b : t.r;
        float w, h;
        texPacked.GetDimensions(w, h);
        float4 c = texPacked.SampleLevel(linearClamp, float2(i.pos.x / (2.0 * w), i.pos.y / h), 0);
        return float4(YuvToRgb(y, c.g, c.a), 1.0);
    }
    float3 c = texPacked.Load(int3(p, 0)).rgb;
    if (fullRange == 0) c = saturate((c - 16.0 / 255.0) * (255.0 / 219.0));
    return float4(c, 1.0);
}

#endif

#ifdef PASS_PICTURE
// ---------------------------------------------------------------------------
// Pass 2: scale to the screen with picture settings.
// ---------------------------------------------------------------------------
Texture2D<float4> texRgb : register(t0);
SamplerState linearClamp : register(s0);

cbuffer Picture : register(b0) {
    float brightness;   // -1 .. 1
    float contrast;     // 0 .. 2
    float saturation;   // 0 .. 2
    float sharpness;    // 0 .. 1
    float2 texel;       // 1 / source size
    float2 pad1;
};

float4 PSPicture(VSOut i) : SV_Target {
    float3 c = texRgb.Sample(linearClamp, i.uv).rgb;
    if (sharpness > 0.0) {
        float3 n = texRgb.Sample(linearClamp, i.uv + float2(texel.x, 0)).rgb
                 + texRgb.Sample(linearClamp, i.uv - float2(texel.x, 0)).rgb
                 + texRgb.Sample(linearClamp, i.uv + float2(0, texel.y)).rgb
                 + texRgb.Sample(linearClamp, i.uv - float2(0, texel.y)).rgb;
        c += (c - n * 0.25) * (sharpness * 1.5);
    }
    c = (c - 0.5) * contrast + 0.5 + brightness;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(l, l, l), c, saturation);
    return float4(saturate(c), 1.0);
}
#endif
)HLSL";
