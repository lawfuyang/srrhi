// hlsl_test_resource_basic.hlsl
// Runtime test: validates generated accessor functions for a basic SRV resource layout.
//
// Uses test_resource_basic.hlsli (BasePassInputs namespace):
//   Texture2D<float>         DepthTexture @ t0
//   Texture2D                GBufferAlbedo@ t1  (typeless, not used in this test)
//   RWStructuredBuffer<uint> DrawCount    @ u0  (SKIPPED: conflicts with g_output @ u0)
//
// Since Texture2D<float> is at t0 and g_output is at u0, there is no register conflict
// for the SRV path. The DrawCount UAV at u0 is declared in the hlsli but NOT
// accessed in this shader, so DXC does not include it in the root signature and
// the runtime binding only needs the SRV descriptor.
//
// Wait — DXC will reject two resources sharing register u0 even if one is unused.
// Therefore this shader must not include test_resource_basic.hlsli.
// Instead it uses test_resource_basic.hlsli indirectly via a compatible hlsli:
//
// Uses test_texture_dimension_srv.hlsli (AllSrvTextureDimensions namespace):
//   Texture2D<float4>       Tex2D      @ t2
//   StructuredBuffer<uint>  (not in this hlsli — use test_resource_all_srv.hlsli)
//
// This file exercises the Texture2D SRV read path. For buffer SRV testing
// see hlsl_test_resource_srv_buffers.hlsl.
//
// Resource binding:
//   Texture2D<float4>   Tex2D  @ t2  (AllSrvTextureDimensions namespace)
//
// Data layout (CPU-filled before dispatch):
//   Tex2D[0,0] = {1.0f, 2.0f, 3.0f, 4.0f}   (Texture2D, 1x1)
//
// Outputs:
//   g_output[0] = asuint(Tex2D.Load(int3(0,0,0)).x) = asuint(1.0f)
//   g_output[1] = asuint(Tex2D.Load(int3(0,0,0)).y) = asuint(2.0f)
//   g_output[2] = asuint(Tex2D.Load(int3(0,0,0)).z) = asuint(3.0f)
//   g_output[3] = asuint(Tex2D.Load(int3(0,0,0)).w) = asuint(4.0f)

#include "test_texture_dimension_srv.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    using namespace srrhi;

    // Texture2D<float4> @ t2 via AllSrvTextureDimensions::GetTex2D()
    Texture2D<float4> tex2D = AllSrvTextureDimensions::GetTex2D();
    float4 texel = tex2D.Load(int3(0, 0, 0));
    g_output[0] = asuint(texel.x);
    g_output[1] = asuint(texel.y);
    g_output[2] = asuint(texel.z);
    g_output[3] = asuint(texel.w);
}
