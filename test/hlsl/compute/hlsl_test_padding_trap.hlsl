// hlsl_test_padding_trap.hlsl
// Runtime test: reads from a cbuffer containing float3 + float + float4.
// The "padding trap" is that float3 (12 bytes) is naturally followed by float
// (radius at offset 12), then float4 begins at the next 16-byte boundary (offset 16).
// Tests that position and color are correctly separated.

#include "test_padding_trap.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::Test d = srrhi::MainInputs::GetTest();
    g_output[0] = asuint(d.position.x);
    g_output[1] = asuint(d.position.y);
    g_output[2] = asuint(d.position.z);
    g_output[3] = asuint(d.radius);
    g_output[4] = asuint(d.color.x);
    g_output[5] = asuint(d.color.y);
}
