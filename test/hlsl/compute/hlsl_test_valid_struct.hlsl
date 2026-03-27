// hlsl_test_valid_struct.hlsl
// Runtime test: reads from valid_cb cbuffer which contains a nested struct
// (ValidStruct data), a float scalar (extra), and an array of nested structs
// (ValidStruct array[5]). Tests that nested struct fields and struct array
// element indexing work correctly via the MainInputs::GetValidCb() accessor.

#include "test_valid_struct.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::valid_cb d = srrhi::MainInputs::GetValidCb();
    g_output[0] = asuint(d.data.x);
    g_output[1] = asuint(d.extra);
    g_output[2] = asuint(d.array[0].x);
    g_output[3] = asuint(d.array[4].x);
}
