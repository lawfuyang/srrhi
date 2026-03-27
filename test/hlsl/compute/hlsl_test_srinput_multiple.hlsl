// hlsl_test_srinput_multiple.hlsl
// Runtime test: reads from the LightingInputs namespace which binds two cbuffers
// (CameraConsts at b0, LightConsts at b1). Tests that multi-cbuffer, multi-scope
// generated files correctly bind separate namespaces to separate registers.
//
// NOTE: MaterialInputs also declares a cbuffer at b0 in the same file. This shader
// only accesses LightingInputs, so only b0 (CameraConsts) and b1 (LightConsts) are read.

#include "test_srinput_multiple.hlsli"

RWStructuredBuffer<uint> g_output : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    srrhi::CameraConsts camera = srrhi::LightingInputs::GetCamera();
    srrhi::LightConsts  light  = srrhi::LightingInputs::GetLight();
    g_output[0] = asuint(camera.FOV);
    g_output[1] = asuint(light.Intensity);
}
