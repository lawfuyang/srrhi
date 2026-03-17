# Shader Resource Render Hardware Interface (SRRHI)

SRRHI is a code-generation tool that reads `.sr` source files describing GPU shader resources and constant buffers, then emits matching **C++ headers** and **HLSL include files** that stay in sync automatically.

---

## Features

### `.sr` Source Language

`.sr` files use an HLSL-like syntax to declare the resource layout for a render pass.

**`cbuffer` declarations** — define constant buffer layouts using HLSL scalar, vector, and matrix types, as well as user-defined structs. Explicit padding fields are supported.

```hlsl
cbuffer SceneConstants
{
    float4x4 viewProj;
    float3   cameraPosWS;
    float    exposure;
    float4   fogColor;
    uint     frameNumber;
    float    deltaTime;
};
```

**`struct` declarations** — named structs that can be used as field types inside `cbuffer` blocks. Structs can be nested.

**`srinput` scopes** — group one or more `cbuffer` references together with SRV/UAV resources, samplers, and compile-time scalar constants into a single named binding set.

```hlsl
srinput ForwardPass
{
    SceneConstants m_Scene;          // cbuffer → b0

    Texture2D<float4>   m_Albedo;    // SRV     → t0
    RWTexture2D<float4> m_Output;    // UAV     → u0
    SamplerState        m_Linear;    // Sampler → s0
};
```

**`#include`** — `.sr` files can include other `.sr` files to share struct and cbuffer definitions across passes.

**`[push_constant]`** — annotate a cbuffer member inside an `srinput` to mark it as a push-constant / root constant.

```hlsl
srinput DrawPass
{
    [push_constant]
    PushPerDraw m_PerDraw;

    SceneConsts m_Scene;
};
```

**Scalar constants** — declare compile-time constants directly inside an `srinput` scope. All three qualifier forms are accepted: unqualified, `const`, and `static const`.

```hlsl
srinput RenderInputs
{
    FrameConsts m_Frame;

    static const uint  MaxLights   = 16;
    static const float Pi          = 3.14159;
    const        bool  Enabled     = true;
};
```

---

### Generated C++ Headers (`output/cpp/<name>.h`)

Each `.sr` file produces a C++ header inside `srrhi` namespace containing:

- **`cbuffer` classes** — `alignas(16)` C++ classes with **private** data members and public typed `Set*()` setters. Member names are stripped of common prefixes (`m_`, `g_`, `s_`) before the setter name is derived.
  - Scalar fields → `void SetFoo(float value)`
  - Vector/matrix fields → `void SetFoo(const DirectX::XMFLOAT4X4& value)`
  - `GetRawBytes()` — returns a `const uint8_t*` to the raw cbuffer memory for testing.

- **Compile-time offset validation** — a friend `Validator` struct with `static_assert(offsetof(...))` checks for every field, ensuring the C++ layout matches the HLSL cbuffer layout.

- **`srinput` structs** — plain structs that hold:
  - The cbuffer class instances as data members.
  - `static constexpr uint32_t` register index constants (`FrameRegisterIndex`, `NumCBuffers`, `NumSRVs`, `NumUAVs`, `NumSamplers`, `NumResources`, …).
  - Scalar constants as `static constexpr` members.
  - A flat `srrhi::ResourceEntry m_Resources[NumResources]` array with compile-time `slot` and `type` fields, ordered: CBuffers → SRVs → UAVs → Samplers.
  - Typed `Set*()` resource setters that write into `m_Resources`:
    - **Non-texture resources** (buffers, samplers, cbuffers, acceleration structures): `void SetFoo(void* pResource)`
    - **Texture SRVs** (non-array): simple `void SetFoo(void*)` + mip-range overload `void SetFoo(void*, int32_t baseMip, int32_t numMips)`
    - **Texture SRVs** (array): simple `void SetFoo(void*)` + full overload `void SetFoo(void*, int32_t baseMip, int32_t numMips, int32_t baseSlice, int32_t numSlices)`
    - **Texture UAVs**: `void SetFoo(void*, int32_t baseMipLevel)` — `numMipLevels` is always hardcoded to 1.

- **`static_assert` register checks** — file-scope assertions that verify all `NumCBuffers`, `NumSRVs`, `NumUAVs`, `NumSamplers`, `NumResources`, and every `*RegisterIndex` constant at compile time.

---

### Generated HLSL Includes (`output/hlsl/<name>.hlsli`)

Each `.sr` file also produces an `.hlsli` file (wrapped in an include guard) containing:

- HLSL `struct` definitions for each cbuffer layout.
- `cbuffer` declarations bound to their `register(b#)` slots.
- Resource declarations (`Texture2D`, `RWBuffer`, `SamplerState`, etc.) bound to their `register(t#)`, `register(u#)`, and `register(s#)` slots.
- A `namespace <SrInputName>` block with `Get*()` accessor functions for every binding, providing a clean namespaced API inside shaders.

---

### Supported Resource Types

| HLSL type | `ResourceType` | Register |
|---|---|---|
| `Texture1/2/3D`, `TextureCube` (+ Array variants, MS variants) | `Texture_SRV` | `t#` |
| `Buffer<T>` | `TypedBuffer_SRV` | `t#` |
| `StructuredBuffer<T>` | `StructuredBuffer_SRV` | `t#` |
| `ByteAddressBuffer` | `RawBuffer_SRV` | `t#` |
| `RaytracingAccelerationStructure` | `RayTracingAccelStruct` | `t#` |
| `RWTexture1/2/3D` (+ Array variants) | `Texture_UAV` | `u#` |
| `RWBuffer<T>` | `TypedBuffer_UAV` | `u#` |
| `RWStructuredBuffer<T>` | `StructuredBuffer_UAV` | `u#` |
| `RWByteAddressBuffer` | `RawBuffer_UAV` | `u#` |
| `cbuffer` reference | `ConstantBuffer` | `b#` |
| `SamplerState` / `SamplerComparisonState` | `Sampler` | `s#` |

---

### DXC Reflection Tests (`--test`)

When run with `--test`, srrhi compiles the generated `.hlsli` files with DXC and uses shader reflection to verify that every cbuffer field offset in the generated C++ header exactly matches what DXC reports. This catches any layout mismatch between the C++ struct and the HLSL cbuffer.

---

## Including `srrhi.h` in Your Project

Copy `srrhi.h` from the repository root into your project (or add the repository root to your include path). Then include it in any translation unit that uses the generated headers:

```cpp
#include "srrhi.h"
```

The generated headers (`output/cpp/<name>.h`) already contain `#include "srrhi.h"` and expect it to be resolvable from the same include path. No other dependencies are required beyond a C++17 compiler and DirectXMath (for `DirectX::XMFLOAT*` types used in cbuffer setters).

---

## `srrhi.exe` Usage

```
srrhi -i <input-dir> -o <output-dir> [-v] [--test [--gen-validation]]
```

| Argument | Description |
|---|---|
| `-i <dir>` | **Required.** Input folder. Scanned recursively for `.sr` files. |
| `-o <dir>` | **Required.** Output folder. `hlsl/` and `cpp/` subfolders are created automatically. |
| `-v` | Verbose mode. Prints the cbuffer layout visualizer output to stdout. |
| `--test` | After generation, verify cbuffer layouts via DXC reflection. Implies `-v`. |
| `--gen-validation` | Requires `--test`. Generates a minimal `validation_<name>.cpp` stub for each produced `.h` that includes the header and compiles it. |
| `-h` / `--help` | Print usage and exit. |

**Basic generation:**
```
srrhi -i shaders/sr -o generated
```
Produces `generated/hlsl/*.hlsli` and `generated/cpp/*.h` for every `.sr` file found under `shaders/sr/`.

**With layout verification:**
```
srrhi -i shaders/sr -o generated --test
```
Runs DXC reflection after generation and reports any cbuffer layout mismatches.

**With validation stubs:**
```
srrhi -i shaders/sr -o generated --test --gen-validation
```
Also writes `generated/cpp/validation_<name>.cpp` stubs that can be compiled as a quick smoke-test.

Register indices are assigned in declaration order within each `srinput` scope. CBuffers start at `b0`, SRVs at `t0`, UAVs at `u0`, and samplers at `s0` — each counter resets per `srinput` scope.
