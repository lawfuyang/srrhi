// hlsl_test_resource_uav_textures.hlsl
// Runtime test: validates generated accessor functions for UAV texture resources
// across supported dimensions.
//
// Uses test_resource_all_uav.hlsli (AllUavInputs namespace):
//   RWTexture1D<float4>      RwTex1d       @ u0
//   RWTexture1DArray<float4> RwTex1dArray  @ u1
//   RWTexture2D<float4>      RwTex2d       @ u2
//   RWTexture2DArray<float4> RwTex2dArray  @ u3
//   RWTexture3D<float4>      RwTex3d       @ u4
//   RWBuffer<float4>         RwTypedBuffer @ u5
//   RWStructuredBuffer<uint> RwStructBuf   @ u6
//   RWByteAddressBuffer      RwRawBuf      @ u7
//
// To avoid the register conflict between u0 (RwTex1d in the hlsli) and the
// test harness output UAV, g_output is placed at u0, space1.
// The AllUavInputs resources occupy u0..u7 in space0 — no overlap.
//
// Data layout (CPU-filled before dispatch):
//   RwTex1d[0].x       = 10.0f  (RWTexture1D at u0 space0)
//   RwTex2d[0,0].x     = 20.0f  (RWTexture2D at u2 space0)
//   RwTex3d[0,0,0].x   = 30.0f  (RWTexture3D at u4 space0)
//   RwStructBuf[0]     = 0xBEEF0042  (RWStructuredBuffer at u6 space0)
//
// Outputs:
//   g_output[0] = asuint(RwTex1d[int(0)].x)       = asuint(10.0f)
//   g_output[1] = asuint(RwTex2d[int2(0,0)].x)    = asuint(20.0f)
//   g_output[2] = asuint(RwTex3d[int3(0,0,0)].x)  = asuint(30.0f)
//   g_output[3] = RwStructBuf[0]                   = 0xBEEF0042

#include "test_resource_all_uav.hlsli"

// g_output in space1 to avoid conflict with AllUavInputs UAVs in space0
RWStructuredBuffer<uint> g_output : register(u0, space1);

[numthreads(1, 1, 1)]
void main()
{
    using namespace srrhi;

    // Read from RWTexture1D<float4> @ u0 space0 via generated accessor
    RWTexture1D<float4> rwTex1d = AllUavInputs::GetRwTex1d();
    g_output[0] = asuint(rwTex1d[int(0)].x);

    // Read from RWTexture2D<float4> @ u2 space0 via generated accessor
    RWTexture2D<float4> rwTex2d = AllUavInputs::GetRwTex2d();
    g_output[1] = asuint(rwTex2d[int2(0, 0)].x);

    // Read from RWTexture3D<float4> @ u4 space0 via generated accessor
    RWTexture3D<float4> rwTex3d = AllUavInputs::GetRwTex3d();
    g_output[2] = asuint(rwTex3d[int3(0, 0, 0)].x);

    // Read from RWStructuredBuffer<uint> @ u6 space0 via generated accessor
    RWStructuredBuffer<uint> rwStructBuf = AllUavInputs::GetRwStructBuf();
    g_output[3] = rwStructBuf[0];
}
