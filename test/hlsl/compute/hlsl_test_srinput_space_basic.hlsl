// hlsl_test_srinput_space_basic.hlsl
// Runtime test: reads from two cbuffers bound to register space1
// (b0 space1 = FrameConsts, b1 space1 = PassConsts) via the BasePassInputs
// namespace. Validates that non-zero register spaces are correctly propagated
// from the generated .hlsli into the shader binding.

#include "test_srinput_space_basic.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::FrameConsts frame = srrhi::BasePassInputs::GetFrameConsts();
    srrhi::PassConsts  pass  = srrhi::BasePassInputs::GetPassConsts();
    g_output[0] = asuint(frame.m_Time);
    g_output[1] = pass.m_PassIdx;
}
