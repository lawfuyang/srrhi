// =============================================================================
// srrhi.h
// Public shared header for the srrhi (Shader Resource Render Hardware Interface)
// resource binding abstraction layer.
//
// This file defines the ResourceType enum and ResourceEntry struct, which are
// used by all srrhi-generated headers to provide a uniform interface for
// binding GPU resources (SRVs, UAVs, CBuffers, Samplers) to shader pipelines.
//
// Usage:
//   - This header is included automatically by any srrhi-generated header that
//     contains resource bindings (SRVs, UAVs, CBuffers, or Samplers).
//   - Each generated srinput struct exposes a 'resources[]' array of ResourceEntry
//     objects, one per binding slot, with typed Set*() helper functions.
//   - Call the Set*() setters to assign resource pointers at runtime before
//     submitting draw/dispatch calls.
//
// Example:
//   MyPassInputs inputs;
//   inputs.SetDiffuseTex(myTextureSRV, 0, -1, 0, -1);  // Texture_SRV: full mip/slice overload
//   inputs.SetOutputTex(myTextureUAV, 0, 0, -1);        // Texture_UAV: 4-param overload (numMipLevels hardcoded to 1)
//   inputs.SetTypedBuf(myBufferSRV);                     // non-texture: simple void* overload
//   BindResources(inputs.m_Resources, inputs.NumResources);
// =============================================================================

#pragma once

namespace srrhi
{

// =============================================================================
// ResourceType
//
// Compile-time classification of a bindable GPU resource.
//
// These values are assigned at code-generation time and stored in the 'type'
// field of each ResourceEntry in a generated srinput struct's resources[] array.
// The ResourceType determines how the resource is interpreted by the backend
// graphics API layer (D3D12, Vulkan, etc.).
//
// Texture types (Texture_SRV, Texture_UAV) support sub-resource selection via
// the baseMipLevel, numMipLevels, baseArraySlice, and numArraySlices fields of
// ResourceEntry. All other types ignore those fields.
//
// Note: PushConstants is reserved for future use and is not yet referenced by
// any generated code.
// =============================================================================
enum class ResourceType : uint32_t
{
    Texture_SRV,            ///< Texture read-only SRV                    (t# register)
    Texture_UAV,            ///< Texture read-write UAV                   (u# register)
    TypedBuffer_SRV,        ///< Typed  Buffer<T>   SRV                   (t# register)
    TypedBuffer_UAV,        ///< Typed  RWBuffer<T> UAV                   (u# register)
    StructuredBuffer_SRV,   ///< StructuredBuffer<T>   SRV                (t# register)
    StructuredBuffer_UAV,   ///< RWStructuredBuffer<T> UAV                (u# register)
    RawBuffer_SRV,          ///< ByteAddressBuffer   SRV                  (t# register)
    RawBuffer_UAV,          ///< RWByteAddressBuffer UAV                  (u# register)
    ConstantBuffer,         ///< Constant buffer                          (b# register)
    Sampler,                ///< SamplerState / SamplerComparisonState    (s# register)
    RayTracingAccelStruct,  ///< RaytracingAccelerationStructure SRV      (t# register)
    PushConstants,          ///< Push constants — reserved for future use (b# register)
};

// =============================================================================
// ResourceEntry
//
// Describes a single bindable GPU resource slot inside a generated srinput struct.
//
// Each generated srinput struct maintains a C-style array of ResourceEntry objects:
//
//   srrhi::ResourceEntry m_Resources[NumResources];
//
// where NumResources = NumCBuffers + NumSRVs + NumUAVs + NumSamplers.
// Array layout: CBuffers first, then SRVs, then UAVs, then Samplers.
//
// The 'slot' and 'type' fields are const and initialized directly from the
// compile-time register index constants in the generated srinput struct.
// They are fixed for the lifetime of the entry and cannot be changed at runtime.
//
// The 'pResource' field starts as nullptr and is updated at runtime via the
// typed Set*() setter functions generated alongside the struct.
//
// Fields:
//   pResource      — Pointer to the API resource object (e.g. D3D12 descriptor
//                    handle wrapper, Vulkan image view pointer, etc.).
//                    Defaults to nullptr. Updated via Set*() setters.
//
//   slot           — Compile-time register index (e.g. 0 for t0, 2 for u2).
//                    Initialized from the generated RegisterIndex constant.
//
//   type           — Compile-time ResourceType classification, derived from the
//                    HLSL resource variable type declared in the .sr source file.
//
//   baseMipLevel   — First mip level to bind. Relevant for texture types only.
//                    Default: 0.
//
//   numMipLevels   — Number of mip levels to bind. Relevant for texture types only.
//                    -1 means "all mip levels starting from baseMipLevel".
//                    For Texture_UAV resources, this must always be 1 — a UAV
//                    can only bind a single mip level at a time.
//                    Default: -1.
//
//   baseArraySlice — First array slice to bind. Relevant for array texture types.
//                    Default: 0.
//
//   numArraySlices — Number of array slices to bind. Relevant for array textures.
//                    -1 means "all slices starting from baseArraySlice".
//                    Default: -1.
// =============================================================================
struct ResourceEntry
{
    void*              pResource      = nullptr; ///< Runtime resource pointer (set via Set*() helpers).
    const uint32_t     slot;                     ///< Compile-time register slot index (t#/u#/b#/s#).
    const ResourceType type;                     ///< Compile-time resource type classification.
    int32_t            baseMipLevel   =  0;      ///< Base mip level  (textures only; default 0).
    int32_t            numMipLevels   = -1;      ///< Mip level count (textures only; -1 = all mips).
    int32_t            baseArraySlice =  0;      ///< Base array slice (array textures only; default 0).
    int32_t            numArraySlices = -1;      ///< Array slice count (array textures only; -1 = all slices).
};

}  // namespace srrhi
