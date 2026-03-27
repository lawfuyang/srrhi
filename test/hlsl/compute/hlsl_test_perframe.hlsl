// hlsl_test_perframe.hlsl
// Runtime test: reads from PerFrame cbuffer (3 float4x4 matrices + per-frame scalars)
// via MainInputs::GetPerFrame(). Tests Time, CameraPosition.y, NearPlane, FarPlane.

#include "test_perframe.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::PerFrame d = srrhi::MainInputs::GetPerFrame();
    g_output[0] = asuint(d.Time);
    g_output[1] = asuint(d.CameraPosition.y);
    g_output[2] = asuint(d.NearPlane);
    g_output[3] = asuint(d.FarPlane);
}
