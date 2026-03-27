// hlsl_test_scene.hlsl
// Runtime test: reads from a large SceneConstants cbuffer (312 bytes, spanning
// two 256-byte D3D12 alignment blocks). Tests fields that live past the 256-byte
// mark: exposure (offset 268), frameNumber (offset 304), deltaTime (offset 308).

#include "test_scene.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::SceneConstants d = srrhi::MainInputs::GetScene();
    g_output[0] = asuint(d.exposure);
    g_output[1] = d.frameNumber;
    g_output[2] = asuint(d.deltaTime);
}
