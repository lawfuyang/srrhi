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

**`extern TypeName;`** — declare externally-defined user types that are not defined in any `.sr` file. These can be used in structs, cbuffers, and resource template arguments (for structured buffers).

Both simple names and **namespace/nested-class qualified names** (using `::`) are supported:

```hlsl
extern GPUSceneBlock;                        // simple name
extern nvrhi::rt::IndirectInstanceDesc;      // namespace-qualified name
extern engine::render::LightData;            // multi-level namespace

cbuffer FrameData
{
    GPUSceneBlock scene;
};

struct RayTracingData
{
    nvrhi::rt::IndirectInstanceDesc m_Instance;
    float4                          m_Color;
};

srinput FramePass
{
    StructuredBuffer<GPUSceneBlock>                   m_SceneHistory;
    StructuredBuffer<nvrhi::rt::IndirectInstanceDesc> m_Instances;
    FrameData m_Frame;
};
```

`extern` declarations are idempotent — re-declaring the same type (at global scope, inside a `struct`, or inside an `srinput`) is allowed and has no effect. For qualified names, the conflict checks against native types, local structs, type aliases, and srinput names are skipped, since namespaced types cannot collide with local names.

**Type aliases** — aliases are supported via all three forms: `typedef`, `using`, and `#define` type aliases.

```hlsl
typedef float4 Color4;
using Position3 = float3;
#define NORMAL_T float3

struct VertexData
{
    Position3 position;
    NORMAL_T  normal;
    Color4    color;
};
```

**Preprocessor conditionals** — file-scope directives are supported (`#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`, `defined(...)`, logical operators), and are preserved in generated HLSL where applicable.

```hlsl
#define USE_MSAA 1

#if USE_MSAA
Texture2DMS<float4> m_Color;
#else
Texture2D<float4> m_Color;
#endif
```

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

Scalar constants can also drive array sizes in later declarations via `SrInputName::ConstName`:

```hlsl
srinput Config
{
    static const uint MaxLights = 64;
};

struct LightCluster
{
    float4 lightData[Config::MaxLights];
};
```

**`[space(N)]` register space attribute** — optionally specify an explicit HLSL register space for an `srinput` scope. By default, generated HLSL omits `space` qualifiers (`space0` is implicit). This is useful when:

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

In generated C++, `RegisterSpace` is always emitted. If `[space(N)]` is not specified, it defaults to `0`.

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
    - **`RegisterSpace`** — always present. Defaults to `0` when no `[space(N)]` attribute is set.
    - **`PushConstantBytes`** — byte size of the push-constant cbuffer in that flattened srinput (`0` when none is present).
  - Scalar constants as `static constexpr` members.
  - A flat `srrhi::ResourceEntry m_Resources[NumResources]` array with compile-time `slot` and `type` fields, ordered: CBuffers → SRVs → UAVs → Samplers.
  - Typed `Set*()` resource setters that write into `m_Resources`:
    - **Non-texture resources** (buffers, samplers, cbuffers, acceleration structures): `void SetFoo(void* pResource)`
    - **Texture SRVs** (non-array): simple `void SetFoo(void*)` + mip-range overload `void SetFoo(void*, int32_t baseMip, int32_t numMips)`
    - **Texture SRVs** (array): simple `void SetFoo(void*)` + full overload `void SetFoo(void*, int32_t baseMip, int32_t numMips, int32_t baseSlice, int32_t numSlices)`
        - **Texture UAVs** (non-array): `void SetFoo(void*, int32_t baseMipLevel)` — `numMipLevels` is hardcoded to `1`.
        - **Texture UAVs** (array): `void SetFoo(void*, int32_t baseMipLevel, int32_t baseSlice, int32_t numSlices)` — `numMipLevels` is hardcoded to `1`.

- **Extern-type validation checks** — when `extern` types are used **inside `cbuffer` definitions** (directly or transitively through nested structs), generated headers emit compile-time checks requiring:
    - `sizeof(T) % 16 == 0`
    - `alignof(T) >= 16`
    - `std::is_trivially_copyable_v<T>`

  Extern types used **only** as `StructuredBuffer<T>` / `RWStructuredBuffer<T>` template arguments are exempt — no static_asserts are generated for them, since structured buffers have no cbuffer packing constraints.

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
| `[push_constant]` cbuffer reference | `PushConstants` | `b#` |
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

If you don't specify `[space(N)]`, all srinputs remain in the default register space (`space0`):

```hlsl
// Both use space0 implicitly; register numbers are local to each srinput scope.
srinput Pass1
{
    Consts1 m_C1;                // b0
    Texture2D<float4> m_Tex1;    // t0
};

srinput Pass2
{
    Consts2 m_C2;                // b0 (new scope, counter resets)
    Texture2D<float4> m_Tex2;    // t0 (new scope, counter resets)
};
```

This is the right choice when:
- Your shader only uses a single active srinput at a time
- You're binding all resources to the default descriptor heap
- You don't need independent resource groups with colliding register numbers

Generated C++ still emits:

```cpp
static constexpr uint32_t RegisterSpace = 0;
```

### Srinput Inheritance

You can define srinput inheritance hierarchies where a derived srinput inherits all cbuffers, resources, samplers, and scalar constants from one or more base srinputs.

**Syntax:**
```hlsl
srinput Derived : Base1, Base2, Base3 { ... }
```

Inheritance is useful for:
- Building progressively more feature-complete srinputs from simpler base configurations
- Sharing common bindings across multiple passes without duplication
- Organizing large shaders with increasingly specialized requirements

**Key behaviors:**
- Derived srinputs inherit **all** members and resources from bases (in declaration order)
- Flattening order: base items are prepended, then derived body items follow
- Register indices continue sequentially: base cbuffers at b0, b1, then derived body at b2, b3, etc.
- Register space (`[space(N)]`) is inherited from the top-level parent
- Name clash detection: bases and body cannot have duplicate member names

**Basic inheritance example:**

`effects.sr`:
```hlsl
cbuffer FrameConsts
{
    float4x4 m_ViewProj;
    float    m_Time;
};

cbuffer SceneConsts
{
    float3 m_SunDir;
    float  m_Intensity;
};

srinput Base
{
    FrameConsts m_Frame;
};

srinput Derived : Base
{
    SceneConsts m_Scene;
};
```

Generated C++ hierarchy:
```cpp
struct FrameConsts { /* ... */ };
struct SceneConsts { /* ... */ };

struct Base
{
    static constexpr uint32_t NumCBuffers = 1;
    static constexpr uint32_t FrameRegisterIndex = 0; // b0
    FrameConsts m_Frame;
    srrhi::ResourceEntry m_Resources[1];
};

struct Derived
{
    static constexpr uint32_t NumCBuffers = 2;
    static constexpr uint32_t FrameRegisterIndex = 0;  // b0 (inherited)
    static constexpr uint32_t SceneRegisterIndex = 1;  // b1 (new)
    FrameConsts m_Frame;     // inherited
    SceneConsts m_Scene;     // own member
    srrhi::ResourceEntry m_Resources[2];
};
```

**HLSL usage:**
```hlsl
#include "effects.hlsli"

// Derived automatically has both cbuffers available
FrameConsts g_FrameConsts = Derived::GetFrameConsts();
SceneConsts g_SceneConsts = Derived::GetSceneConsts();

float4 GetLightContribution(float3 pos)
{
    float3 toLight = normalize(g_SceneConsts.m_SunDir);
    return float4(g_SceneConsts.m_Intensity * dot(normalize(pos), toLight), 1.0);
}
```

**Multiple inheritance example:**

```hlsl
cbuffer CameraConsts
{
    float3 m_Position;
    float  m_FOV;
};

cbuffer LightConsts
{
    float3 m_Color;
    float  m_Intensity;
};

cbuffer PostConsts
{
    float m_Exposure;
    float m_Gamma;
};

srinput CameraPass
{
    CameraConsts m_Camera;
    Texture2D<float4> m_AlbedoA;
};

srinput LightPass
{
    LightConsts  m_Light;
    Texture2D<float4> m_AlbedoB;
};

// Inherits from both CameraPass and LightPass
srinput CombinedPass : CameraPass, LightPass
{
    PostConsts m_Post;
    RWTexture2D<float4> m_Output;
};
```

The generated `CombinedPass` flattens in order: first `CameraPass` members (CameraConsts at b0, AlbedoA at t0), then `LightPass` members (LightConsts at b1, AlbedoB at t1), then its own body (PostConsts at b2, Output at u0).

---

### Srinput Composition

Srinput composition allows you to nest one srinput inside another as a member, automatically flattening all nested resources into the parent. This is distinct from inheritance: you explicitly declare a nested srinput member, and it gets inlined during code generation.

**Syntax:**
```hlsl
srinput ParentInput
{
    ChildInput m_Child;    // nest another srinput here
    // ... parent's own members ...
};
```

Composition is useful for:
- Organizing large shaders by grouping related bindings into named units
- Reusing predefined input sets without duplicating definitions
- Creating flexible modular binding combinations
- Layering resources: base pass inputs, then lighting inputs, then post-processing inputs

**Key behaviors:**
- A nested srinput member is expanded inline during flattening (DFS order)
- Register indices continue sequentially through the nesting hierarchy
- Each level's nested members appear in declaration order before that level's own members
- Register space (`[space(N)]`) is determined by the top-level parent only
- Name clash detection: nested and own members cannot have collision

**Basic composition example:**

`render_inputs.sr`:
```hlsl
cbuffer FrameConsts
{
    float4x4 m_ViewProj;
    float    m_Time;
};

cbuffer PassConsts
{
    uint m_PassIdx;
};

cbuffer SceneConsts
{
    float3 m_SunDir;
    float  m_Intensity;
};

srinput BaseInputs
{
    FrameConsts m_Frame;
    PassConsts  m_Pass;
};

srinput ExtendedPass
{
    BaseInputs  m_Base;        // composition: nests BaseInputs
    SceneConsts m_Scene;       // own member
};
```

Generated register layout:
- b0: FrameConsts (from nested BaseInputs)
- b1: PassConsts (from nested BaseInputs)
- b2: SceneConsts (from ExtendedPass)

Generated C++:
```cpp
struct FrameConsts { /* ... */ };
struct PassConsts { /* ... */ };
struct SceneConsts { /* ... */ };

struct BaseInputs
{
    FrameConsts m_Frame;
    PassConsts  m_Pass;
    srrhi::ResourceEntry m_Resources[2];
    // ...
};

struct ExtendedPass
{
    static constexpr uint32_t NumCBuffers = 3;
    static constexpr uint32_t FrameRegisterIndex = 0;   // b0 (from nested)
    static constexpr uint32_t PassRegisterIndex = 1;    // b1 (from nested)
    static constexpr uint32_t SceneRegisterIndex = 2;   // b2 (own)
    
    FrameConsts m_Frame;  // flattened from m_Base
    PassConsts  m_Pass;   // flattened from m_Base
    SceneConsts m_Scene;  // own member
    srrhi::ResourceEntry m_Resources[3];
};
```

**Deep composition (multi-level nesting):**

```hlsl
cbuffer AConsts   { float m_A; };
cbuffer BConsts   { float m_B; };
cbuffer CConsts   { float m_C; };

srinput LevelA
{
    AConsts m_AConsts;
    Texture2D<float> m_ATex;
};

srinput LevelB
{
    LevelA m_A;              // nests LevelA
    BConsts m_BConsts;
    Texture2D<float> m_BTex;
};

srinput LevelC
{
    LevelB m_B;              // nests LevelB (which nests LevelA)
    CConsts m_CConsts;
    Texture2D<float> m_CTex;
};
```

Generated register bindings for LevelC:
- b0: AConsts, t0: ATex (from LevelA via LevelB)
- b1: BConsts, t1: BTex (from LevelB)
- b2: CConsts, t2: CTex (from LevelC)

**Composition with resources example:**

```hlsl
cbuffer CameraData { float4x4 viewProj; };
cbuffer LightData   { float3 lightDir;  };

srinput ShadowInputs
{
    Texture2D<float> m_NormalMap;
    Texture2D<float> m_DepthRT;
    SamplerState m_LinearSampler;
};

srinput GBufferInputs
{
    CameraData m_Camera;
    Texture2D<float4> m_PositionGBuffer;
    Texture2D<float4> m_NormalGBuffer;
};

srinput BaseLightPass
{
    LightData m_Light;
    Texture2D<float4> m_Lighting;
};

srinput FullLightPass
{
    ShadowInputs    m_Shadows;      // b0, t0-t1, s0
    GBufferInputs   m_GBuffer;      // b1, t2-t3
    BaseLightPass   m_Light;        // b2, t4
    RWTexture2D<float4> m_Output;   // u0
};
```

All nested resources are flattened and register-numbered sequentially in the output.

---

### Combining Inheritance and Composition

You can use both inheritance and composition in the same srinput. The flattening order is:
1. All inherited base members (in inheritance declaration order)
2. This srinput's own members (including composition nesting)

**Combined example:**

```hlsl
cbuffer SharedFrame { float4x4 viewProj; };
cbuffer SharedScene { float3 sunDir; };

cbuffer LightData   { float3 lightColor; };
cbuffer ShadowData  { float bias; };

srinput BasePass
{
    SharedFrame m_Frame;
    SharedScene m_Scene;
};

srinput LightPass
{
    LightData m_Light;
};

srinput ShadowPass
{
    ShadowData m_Shadow;
};

srinput CombinedPass : BasePass
{
    LightPass m_LightComp;    // composition
    ShadowPass m_ShadowComp;  // composition
    Texture2D<float4> m_Output;
};
```

Register layout for `CombinedPass`:
- b0: SharedFrame (inherited from BasePass)
- b1: SharedScene (inherited from BasePass)
- b2: LightData (from composed LightPass)
- b3: ShadowData (from composed ShadowPass)
- t0: Output

---

### Extern Types in Generated C++

When `.sr` uses `extern TypeName;` or `extern ns::TypeName;`, generated C++ headers expect the type to be visible before including the generated header.

```cpp
// project_types.h
struct alignas(16) GPUSceneBlock
{
    float data[16];
};

namespace nvrhi { namespace rt {
struct alignas(16) IndirectInstanceDesc
{
    // ... real definition ...
};
} }

// render.cpp
#include "project_types.h"
#include "generated/cpp/frame_pass.h"
```

Generated headers perform `static_assert` checks to enforce cbuffer packing assumptions, but **only for extern types that are actually used inside a `cbuffer` definition** (directly as a field, or transitively through a nested struct):
- `sizeof(T) % 16 == 0`
- `alignof(T) >= 16`
- `std::is_trivially_copyable_v<T>`

Extern types used **only** as `StructuredBuffer<T>` / `RWStructuredBuffer<T>` template arguments are **exempt** — no static_asserts are generated for them. Structured buffers have no cbuffer packing constraints, so the 16-byte size/alignment/trivially-copyable requirements do not apply.

When `--gen-validation` is used, the generated `validation_<name>.cpp` stub emits stub struct definitions inside the correct namespace blocks for qualified names, so the stub compiles without requiring the real type headers.

### Preprocessor and Alias Patterns

SRRHI supports HLSL-style feature toggles and aliases in `.sr` files. A practical pattern is to define type aliases once, then select resources by build flags:

```hlsl
#define HIST_T float4
using NormalT = float3;

#if defined(USE_HISTORY) && USE_HISTORY
Texture2D<HIST_T> m_History;
#endif

struct GBufferData
{
    NormalT normal;
};
```

### Push Constant Usage Pattern

Only one push constant is allowed per flattened srinput, and it must be the first cbuffer (`b0`).

```hlsl
cbuffer DrawPC
{
    uint drawId;
    uint materialId;
};

cbuffer FrameCB
{
    float4x4 viewProj;
};

srinput DrawInputs
{
    [push_constant]
    DrawPC m_Draw;
    FrameCB m_Frame;
};
```

Generated C++ exposes `PushConstantBytes` and marks that resource entry as `srrhi::ResourceType::PushConstants`.
