// hlsl_test_define_resource_alias.hlsl
// Runtime test: validates that #define resource type aliases (MY_TEX, MY_RW_TEX)
// produce correct accessor functions in the generated hlsli.
//
// Uses test_define_resource_alias.hlsli (ResourceAliasInputs namespace, space1):
//   MY_TEX<float4>    colorTex  @ t0 space1
//   MY_TEX<float>     depthTex  @ t1 space1
//   MY_RW_TEX<float4> outputTex @ u0 space1
//
// g_output is at u0 space0 — no conflict with outputTex at u0 space1.
//
// Data layout (CPU-filled before dispatch):
//   colorTex[0,0] = {1.0f, 2.0f, 3.0f, 4.0f}   (Texture2D<float4>, 1x1)
//   depthTex[0,0] = {0.5f}                       (Texture2D<float>,  1x1)
//
// Outputs:
//   g_output[0] = asuint(colorTex.Load(0,0,0).x) = asuint(1.0f)
//   g_output[1] = asuint(colorTex.Load(0,0,0).y) = asuint(2.0f)
//   g_output[2] = asuint(colorTex.Load(0,0,0).z) = asuint(3.0f)
//   g_output[3] = asuint(colorTex.Load(0,0,0).w) = asuint(4.0f)
//   g_output[4] = asuint(depthTex.Load(0,0,0))   = asuint(0.5f)

#include "test_define_resource_alias.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    using namespace srrhi;

    MY_TEX<float4> color = ResourceAliasInputs::GetColorTex();
    float4 c = color.Load(int3(0, 0, 0));
    g_output[0] = asuint(c.x);
    g_output[1] = asuint(c.y);
    g_output[2] = asuint(c.z);
    g_output[3] = asuint(c.w);

    MY_TEX<float> depth = ResourceAliasInputs::GetDepthTex();
    g_output[4] = asuint(depth.Load(int3(0, 0, 0)));
}
