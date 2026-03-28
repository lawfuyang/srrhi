// hlsl_test_resource_srv_buffers.hlsl
// Runtime test: validates generated accessor functions for buffer SRV resources.
//
// Uses test_resource_all_srv.hlsli (AllSrvInputs namespace) which has ONLY
// SRV (t-register) resources — no UAV conflicts with g_output at u0.
//
// Resource bindings (AllSrvInputs namespace):
//   Buffer<float4>          TypedBuffer @ t9
//   StructuredBuffer<uint>  StructBuf   @ t10
//   ByteAddressBuffer       RawBuf      @ t11
//
// Data layout (CPU-filled before dispatch):
//   TypedBuffer[0] = {1.0f, 2.0f, 3.0f, 4.0f}  — Buffer<float4> SRV
//   StructBuf[0]   = 0xABCD1234                  — StructuredBuffer<uint> SRV
//   RawBuf [0..3]  = 0xDEAD5678                  — ByteAddressBuffer SRV
//
// Outputs:
//   g_output[0] = asuint(TypedBuffer[0].x) = asuint(1.0f)
//   g_output[1] = asuint(TypedBuffer[0].y) = asuint(2.0f)
//   g_output[2] = StructBuf[0]             = 0xABCD1234
//   g_output[3] = RawBuf.Load(0)           = 0xDEAD5678

#include "test_resource_all_srv.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    using namespace srrhi;

    // Read from typed Buffer<float4> SRV via generated accessor
    Buffer<float4> typedBuf = AllSrvInputs::GetTypedBuffer();
    float4 v = typedBuf[0];
    g_output[0] = asuint(v.x);
    g_output[1] = asuint(v.y);

    // Read from StructuredBuffer<uint> SRV via generated accessor
    StructuredBuffer<uint> structBuf = AllSrvInputs::GetStructBuf();
    g_output[2] = structBuf[0];

    // Read from ByteAddressBuffer SRV via generated accessor
    ByteAddressBuffer rawBuf = AllSrvInputs::GetRawBuf();
    g_output[3] = rawBuf.Load(0);
}
