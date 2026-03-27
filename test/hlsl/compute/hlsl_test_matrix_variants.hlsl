// hlsl_test_matrix_variants.hlsl
// Runtime test: reads first rows from two float4x4 matrices (rowMat and colMat)
// in the StressMatrices cbuffer. Tests that row-major matrix rows are at the
// expected consecutive 16-byte-aligned positions.

#include "test_matrix_variants.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::StressMatrices d = srrhi::MainInputs::GetMatrices();
    g_output[0] = asuint(d.rowMat[0].x);
    g_output[1] = asuint(d.rowMat[0].y);
    g_output[2] = asuint(d.colMat[0].x);
    g_output[3] = asuint(d.colMat[0].y);
}
