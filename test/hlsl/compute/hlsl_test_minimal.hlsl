// hlsl_test_minimal.hlsl
// Runtime test: reads g_mvp row 0 (float4) and g_time from the cb cbuffer
// via the generated MainInputs::GetCb() accessor, then writes result to UAV.

#include "test_minimal.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::cb d = srrhi::MainInputs::GetCb();
    g_output[0] = asuint(d.g_mvp[0].x);
    g_output[1] = asuint(d.g_mvp[0].y);
    g_output[2] = asuint(d.g_mvp[0].z);
    g_output[3] = asuint(d.g_mvp[0].w);
    g_output[4] = asuint(d.g_time);
}
