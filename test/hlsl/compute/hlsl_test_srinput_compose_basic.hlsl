// hlsl_test_srinput_compose_basic.hlsl
// Runtime test: reads from the ExtendedPass composed namespace (b0 = FrameConsts,
// b1 = PassConsts, b2 = SceneConsts). The composition pattern re-exposes base
// cbuffers alongside new ones. Tests m_Time (from FrameConsts at b0) and
// m_Intensity (from SceneConsts at b2) via the ExtendedPass accessors.

#include "test_srinput_compose_basic.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::FrameConsts frame = srrhi::ExtendedPass::GetFrame();
    srrhi::SceneConsts scene = srrhi::ExtendedPass::GetScene();
    g_output[0] = asuint(frame.m_Time);
    g_output[1] = asuint(scene.m_Intensity);
}
