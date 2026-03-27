// hlsl_test_srinput_basic.hlsl
// Runtime test: reads from two cbuffers (b0 = FrameConsts, b1 = PassConsts)
// via the BasePassInputs namespace. Tests that multi-cbuffer srinput scopes
// bind and read correctly.

#include "test_srinput_basic.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::FrameConsts frame = srrhi::BasePassInputs::GetFrameConsts();
    srrhi::PassConsts  pass  = srrhi::BasePassInputs::GetPassConsts();
    g_output[0] = asuint(frame.m_Time);
    g_output[1] = pass.m_PassIdx;
}
