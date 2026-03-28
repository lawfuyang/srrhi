// hlsl_test_resource_mixed_space.hlsl
// Runtime test: validates generated accessor functions for a mixed resource layout
// where all resources (cbuffer, textures, UAV) are in register space3.
//
// Uses test_srinput_space_resources.hlsli (MaterialPass namespace):
//   MaterialConsts cbuffer      @ b0 space3
//     - float3 Albedo    [slots 0-2]
//     - float  Metallic  [slot 3]
//     - float  Roughness [slot 4]
//   Texture2D<float4>    AlbedoMap     @ t0 space3
//   Texture2D<float>     RoughnessMap  @ t1 space3
//   RWStructuredBuffer<uint> DrawCounter @ u0 space3
//   SamplerState         LinearSampler @ s0 space3
//
// Since all resources are in space3, they do NOT conflict with:
//   g_output @ u0 space0  (output UAV — used by the test harness)
//
// Data layout (CPU-filled before dispatch):
//   cbuffer MaterialConsts:
//     Albedo.x   = 0.8f  (slot 0)
//     Metallic   = 0.5f  (slot 3)
//     Roughness  = 0.3f  (slot 4)
//   AlbedoMap[0,0].x    = 0.9f  (Texture2D<float4> @ t0 space3)
//   RoughnessMap[0,0].x = 0.25f (Texture2D<float>  @ t1 space3)
//   DrawCounter[0]      = 7u    (RWStructuredBuffer<uint> @ u0 space3)
//
// Outputs:
//   g_output[0] = asuint(MaterialConsts.Albedo.x)      = asuint(0.8f)
//   g_output[1] = asuint(MaterialConsts.Metallic)      = asuint(0.5f)
//   g_output[2] = asuint(AlbedoMap.Load(int3(0,0,0)).x) = asuint(0.9f)
//   g_output[3] = asuint(RoughnessMap.Load(int3(0,0,0))) = asuint(0.25f)
//   g_output[4] = DrawCounter[0]                       = 7u

#include "test_srinput_space_resources.hlsli"

// g_output in space0 (default). ALL resources in the hlsli are in space3,
// so there is no register conflict.
RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    using namespace srrhi;

    // Read cbuffer via generated accessor
    MaterialConsts mat = MaterialPass::GetMaterial();
    g_output[0] = asuint(mat.Albedo.x);
    g_output[1] = asuint(mat.Metallic);

    // Read from Texture2D<float4> SRV @ t0 space3 via generated accessor
    Texture2D<float4> albedoMap = MaterialPass::GetAlbedoMap();
    g_output[2] = asuint(albedoMap.Load(int3(0, 0, 0)).x);

    // Read from Texture2D<float> SRV @ t1 space3 via generated accessor
    Texture2D<float> roughnessMap = MaterialPass::GetRoughnessMap();
    g_output[3] = asuint(roughnessMap.Load(int3(0, 0, 0)));

    // Read from RWStructuredBuffer<uint> UAV @ u0 space3 via generated accessor
    RWStructuredBuffer<uint> drawCounter = MaterialPass::GetDrawCounter();
    g_output[4] = drawCounter[0];
}
