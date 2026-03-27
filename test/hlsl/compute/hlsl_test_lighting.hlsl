// hlsl_test_lighting.hlsl
// Runtime test: reads from LightingConstants cbuffer via MainInputs::GetLighting().
// Tests float4 array indexing (LightData), float4 field, int, and uint.

#include "test_lighting.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::LightingConstants d = srrhi::MainInputs::GetLighting();
    g_output[0] = asuint(d.LightData[0].x);
    g_output[1] = asuint(d.LightData[7].z);
    g_output[2] = asuint(d.ShadowCascadeSplits.y);
    g_output[3] = asuint((float)d.ActiveLightCount);
    g_output[4] = d.FrameIndex;
}
