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

**`[space(N)]` register space attribute** — optionally specify an explicit HLSL register space for an `srinput` scope. By default, register spaces are not emitted in HLSL (`space0` is implicit). This is useful when:

- Binding different shader passes that share resource layouts but need to operate independently within a single shader (e.g., deferred rendering multiple passes in one shader).
- Organizing large shaders into logical groups that have separate descriptor heaps or descriptor tables.
- Preventing register number collisions when multiple independent `srinput` scopes are active.

Without `[space(N)]`, all srinputs share `space0` (HLSL default) and register numbers reset per scope. With `[space(N)]`, that specific srinput and all its resources are bound to the given space.

```hlsl
cbuffer MainSceneConsts
{
    float4x4 viewProj;
    float3   cameraPos;
};

cbuffer PostSceneConsts
{
    float exposure;
    float contrast;
};

// Space 1: resources for main pass
[space(1)]
srinput MainPass
{
    MainSceneConsts m_Main;      // register(b0, space1)
    Texture2D<float4> m_Albedo;  // register(t0, space1)
};

// Space 2: resources for post-processing pass
[space(2)]
srinput PostPass
{
    PostSceneConsts m_Post;      // register(b0, space2)
    Texture2D<float4> m_Result;  // register(t0, space2)
};
```

In the generated C++ header, the `RegisterSpace` constant is only emitted when `[space(N)]` is specified:

```cpp
struct MainPass
{
    static constexpr uint32_t RegisterSpace = 1;
    // ... register index constants, resource array, setters ...
};

struct PostPass
{
    static constexpr uint32_t RegisterSpace = 2;
    // ... register index constants, resource array, setters ...
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
  - **`RegisterSpace`** — emitted only if the `srinput` was decorated with `[space(N)]`. Contains the register space index as a compile-time constant.
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

---

## Examples & Patterns

### Multi-Pass Shader with Register Spaces

A common pattern is to implement multiple rendering passes in a single shader, each with its own set of constant buffers and resources. Using `[space(N)]` prevents register number collisions:

**`render_passes.sr`:**
```hlsl
// Shared structures
struct SceneData {
    float4x4 viewProj;
    float3 cameraPos;
    float padding;
};

struct MaterialData {
    float3 albedo;
    float roughness;
    float3 normal;
    float metallic;
};

struct PostProcessData {
    float exposure;
    float saturation;
    float contrast;
};

cbuffer SceneConsts { SceneData scene; };
cbuffer MaterialConsts { MaterialData material; };
cbuffer PostConsts { PostProcessData post; };

// ==== Space 0: Forward Rendering Pass ====
srinput ForwardPass
{
    SceneConsts       m_Scene;       // b0
    MaterialConsts    m_Material;    // b1
    Texture2D<float4> m_Albedo;      // t0
    Texture2D<float4> m_Normal;      // t1
    SamplerState      m_Linear;      // s0
};

// ==== Space 1: Deferred G-Buffer Write ====
[space(1)]
srinput GBufferPass
{
    SceneConsts         m_Scene;     // b0, space1
    RWTexture2D<float4> m_Albedo;    // u0, space1
    RWTexture2D<float2> m_Normal;    // u1, space1
};

// ==== Space 2: Post-Processing ====
[space(2)]
srinput PostPass
{
    PostConsts        m_Post;        // b0, space2
    Texture2D<float4> m_Input;       // t0, space2
    RWTexture2D<float4> m_Output;    // u0, space2
    SamplerState      m_Point;       // s0, space2
};
```

**C++ usage:**
```cpp
#include "render_passes.h"

using namespace srrhi;

// Instantiate each pass's resources
ForwardPass fwd;
GBufferPass gbuffer;
PostPass post;

// Set ForwardPass resources (space 0)
fwd.m_Scene.SetViewProj(/* insert view proj matrix here */);
fwd.SetMaterial(&materialBuffer);
fwd.SetAlbedo(&albedoTexture);
fwd.SetNormal(&normalTexture);
fwd.SetLinear(&linearSampler);

...
```

**HLSL shader:**
```hlsl
#include "render_passes.hlsli"

// Use GetXXX() accessors from appropriate namespaces
SceneConsts g_SceneConst = ForwardPass::GetSceneConsts();
Texture2D<float4> g_Albedo = ForwardPass::GetAlbedo();

...
```

### Organizing Large Shaders with Multiple Srinputs

When a single shader composes several independent rendering tasks (e.g., a full deferred pipeline with multiple lighting passes), use register spaces to organize resources into logical groups:

```hlsl
// Lighting pass 1: Point lights
[space(3)]
srinput PointLightPass
{
    PointLightConsts m_Lights;      // b0, space3
    Texture2D<float4> m_Position;   // t0, space3 (GBuffer)
    Texture2D<float4> m_Normal;     // t1, space3 (GBuffer)
    RWTexture2D<float4> m_Lighting; // u0, space3
};

// Lighting pass 2: Directional light with shadows
[space(4)]
srinput DirectionalLightPass
{
    DirectionalLightConsts m_Light; // b0, space4
    Texture2D<float> m_ShadowMap;   // t0, space4
    Texture2D<float4> m_Position;   // t1, space4 (GBuffer)
    RWTexture2D<float4> m_Lighting; // u0, space4
    SamplerComparisonState m_ShadowSampler; // s0, space4
};

// Composition pass
[space(5)]
srinput CompositionPass
{
    CompositionConsts m_Consts;     // b0, space5
    Texture2D<float4> m_Lighting;   // t0, space5
    Texture2D<float4> m_Albedo;     // t1, space5
    RWTexture2D<float4> m_Output;   // u0, space5
};
```

This pattern makes it easy to:
- Clearly separate resource dependencies per pass
- Verify at compile-time (via `static_assert`) that each pass has the expected register space
- Organize descriptor heap / descriptor table construction in C++ by iterating over spaces
- Understand shader logic by seeing which `srinput` namespace is being used

### No Register Space (Default Behavior)

If you don't specify `[space(N)]`, all srinputs remain in the default register space (`space0`), and no `RegisterSpace` constant is emitted:

```hlsl
// Both use space0 implicitly, but different b# and t# within space0
srinput Pass1
{
    Consts1 m_C1;                // b0
    Texture2D<float4> m_Tex1;    // t0
};

srinput Pass2
{
    Consts2 m_C2;                // b1 (resets after Pass1)
    Texture2D<float4> m_Tex2;    // t1 (resets after Pass1)
};
```

This is the right choice when:
- Your shader only uses a single active srinput at a time
- You're binding all resources to the default descriptor heap
- You don't need independent resource groups with colliding register numbers

