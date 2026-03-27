// hlsl_test_arrays.hlsl
// Runtime test: reads from ExampleArrays cbuffer via MainInputs::GetArrays().
// Validates that float[2] array members inside a cbuffer-bound struct are
// correctly indexed with the expected 16-byte-per-element stride, and that
// the surrounding float2 fields read correctly.

#include "test_arrays.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::ExampleArrays d = srrhi::MainInputs::GetArrays();
    g_output[0] = asuint(d.before.x);
    g_output[1] = asuint(d.before.y);
    g_output[2] = asuint(d.array[0]);
    g_output[3] = asuint(d.array[1]);
    g_output[4] = asuint(d.after.x);
    g_output[5] = asuint(d.after.y);
}
