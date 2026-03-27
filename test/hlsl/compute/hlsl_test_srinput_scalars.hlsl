// hlsl_test_srinput_scalars.hlsl
// Runtime test: reads static const namespace values from the ScalarTestInputs
// namespace. These are compile-time constants baked into the shader — no cbuffer
// data is uploaded. Validates that the generated static const declarations are
// correct and accessible via the srinput namespace.

#include "test_srinput_scalars.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    g_output[0] = asuint(srrhi::ScalarTestInputs::MaxDistance);  // 1000.0f
    g_output[1] = srrhi::ScalarTestInputs::MaxLights;            // 16u
    g_output[2] = asuint(srrhi::ScalarTestInputs::Pi);           // 3.14159f
    g_output[3] = asuint((float)srrhi::ScalarTestInputs::NegOne); // -1 as int → float
    g_output[4] = srrhi::ScalarTestInputs::Version;              // 2u
}
