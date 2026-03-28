// hlsl_test_resource_srv_textures.hlsl
// Runtime test: validates generated accessor functions for texture SRV resources
// across all supported dimensions.
//
// Uses test_texture_dimension_srv.hlsli (AllSrvTextureDimensions namespace):
//   Texture1D<float4>       Tex1D      @ t0
//   Texture1DArray<float4>  Tex1DArray @ t1
//   Texture2D<float4>       Tex2D      @ t2
//   Texture2DArray<float4>  Tex2DArray @ t3
//   TextureCube<float4>     TexCube    @ t4
//   TextureCubeArray<float4>TexCubeArray@ t5
//   Texture2DMS<float4>     Tex2DMS    @ t6
//   Texture2DMSArray<float4>Tex2DMSArray@ t7
//   Texture3D<float4>       Tex3D      @ t8
//
// All are SRV-only (t-registers) — no UAV conflict with g_output at u0.
// Uses Load() for texel access (no sampler required).
//
// Data layout (CPU-filled before dispatch):
//   Tex1D[0].x       = 1.0f   (Texture1D, 1×1, mip0)
//   Tex2D[0,0].x     = 2.0f   (Texture2D, 1×1, mip0)
//   Tex3D[0,0,0].x   = 3.0f   (Texture3D, 1×1×1, mip0)
//   Tex2DArray[0,0,0].x = 4.0f (Texture2DArray slice 0)
//   Tex1DArray[0,0].x   = 5.0f (Texture1DArray slice 0)
//
// Outputs:
//   g_output[0] = asuint(Tex1D.Load(int2(0,0)).x)       = asuint(1.0f)
//   g_output[1] = asuint(Tex2D.Load(int3(0,0,0)).x)     = asuint(2.0f)
//   g_output[2] = asuint(Tex3D.Load(int4(0,0,0,0)).x)   = asuint(3.0f)
//   g_output[3] = asuint(Tex2DArray.Load(int4(0,0,0,0)).x) = asuint(4.0f)
//   g_output[4] = asuint(Tex1DArray.Load(int3(0,0,0)).x)   = asuint(5.0f)

#include "test_texture_dimension_srv.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    using namespace srrhi;

    // Texture1D<float4> — Load(int2(texel, mip))
    Texture1D<float4> tex1D = AllSrvTextureDimensions::GetTex1D();
    g_output[0] = asuint(tex1D.Load(int2(0, 0)).x);

    // Texture2D<float4> — Load(int3(u, v, mip))
    Texture2D<float4> tex2D = AllSrvTextureDimensions::GetTex2D();
    g_output[1] = asuint(tex2D.Load(int3(0, 0, 0)).x);

    // Texture3D<float4> — Load(int4(u, v, w, mip))
    Texture3D<float4> tex3D = AllSrvTextureDimensions::GetTex3D();
    g_output[2] = asuint(tex3D.Load(int4(0, 0, 0, 0)).x);

    // Texture2DArray<float4> — Load(int4(u, v, slice, mip))
    Texture2DArray<float4> tex2DArray = AllSrvTextureDimensions::GetTex2DArray();
    g_output[3] = asuint(tex2DArray.Load(int4(0, 0, 0, 0)).x);

    // Texture1DArray<float4> — Load(int3(texel, slice, mip))
    Texture1DArray<float4> tex1DArray = AllSrvTextureDimensions::GetTex1DArray();
    g_output[4] = asuint(tex1DArray.Load(int3(0, 0, 0)).x);
}
