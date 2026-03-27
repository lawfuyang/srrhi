// hlsl_test_srinput_inherit_basic.hlsl
// Runtime test: reads from the Derived inherited namespace (b0 = FrameConsts,
// b1 = SceneConsts). The inheritance pattern copies base cbuffer bindings into
// the derived scope alongside new ones. Tests m_Time (FrameConsts at b0) and
// m_Intensity (SceneConsts at b1) via the Derived accessors.

#include "test_srinput_inherit_basic.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::FrameConsts frame = srrhi::Derived::GetFrame();
    srrhi::SceneConsts scene = srrhi::Derived::GetScene();
    g_output[0] = asuint(frame.m_Time);
    g_output[1] = asuint(scene.m_Intensity);
}
