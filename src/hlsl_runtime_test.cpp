// hlsl_runtime_test.cpp
//
// Runtime HLSL shader validation using D3D12 (WARP software adapter) + DXC.
//
// For each test case:
//   1. Compile a compute shader that includes a generated .hlsli and reads from
//      cbuffer(s) via srinput namespace accessor functions.
//   2. Upload known float values into one or more cbuffer upload heaps.
//   3. Dispatch the shader on the WARP adapter (no real GPU required).
//   4. Read back the UAV output buffer and compare against expected bit-exact
//      uint32 values.
//
// This validates end-to-end that:
//   - The generated HLSL struct layouts are correct (right field offsets).
//   - The accessor functions (GetX()) return proper data from the cbuffer.
//   - The cbuffer register bindings (b0..bN, spaceS) are correctly generated.
//
// Usage:
//   hlsl_runtime_validation.exe <hlsl-dir> <hlsli-dir>
//
//   hlsl-dir  : directory containing hlsl_test_*.hlsl compute shaders
//               (e.g., ..\test\hlsl\compute)
//   hlsli-dir : directory containing the generated .hlsli files
//               (e.g., ..\test\output\hlsl)
//
// Both paths are derived automatically from argv[0] if not supplied.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxcapi.h>
#include <d3d12shader.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

// ============================================================================
// Constants
// ============================================================================

static constexpr uint32_t k_OutputElements = 16;   // max UAV output slots (uint32)
static constexpr uint32_t k_CbufSlots      = 128;  // 512 bytes per cbuffer (uint32 slots)
static constexpr uint32_t k_CbufBytes      = k_CbufSlots * sizeof(uint32_t);

// ============================================================================
// Utility helpers
// ============================================================================

static uint32_t FloatBits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

static void ThrowIfFailed(HRESULT hr, const char* msg)
{
    if (FAILED(hr))
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (HRESULT 0x%08X)", msg, (unsigned)hr);
        throw std::runtime_error(buf);
    }
}

// ============================================================================
// DXC file-based include handler
// Resolves #include "foo.hlsli" by reading the file from a specified directory.
// ============================================================================

class FsIncludeHandler : public IDxcIncludeHandler
{
    IDxcUtils* m_pUtils;
    fs::path   m_dir;
    ULONG      m_refCount = 1;

public:
    FsIncludeHandler(IDxcUtils* pUtils, fs::path dir)
        : m_pUtils(pUtils), m_dir(std::move(dir))
    {}

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename,
                                         IDxcBlob** ppIncludeSource) override
    {
        if (!pFilename || !ppIncludeSource) return E_INVALIDARG;
        *ppIncludeSource = nullptr;

        // Strip any path prefix — we only know the bare filename.
        std::wstring wPath(pFilename);
        auto sep = wPath.rfind(L'\\');
        auto sep2 = wPath.rfind(L'/');
        std::wstring::size_type lastSep = std::wstring::npos;
        if (sep  != std::wstring::npos) lastSep = sep;
        if (sep2 != std::wstring::npos) lastSep = (lastSep == std::wstring::npos) ? sep2 : std::max(lastSep, sep2);

        std::wstring wBase = (lastSep != std::wstring::npos) ? wPath.substr(lastSep + 1) : wPath;
        // Convert wide filename to narrow (ASCII-safe for .hlsli filenames)
        std::string name;
        name.reserve(wBase.size());
        for (wchar_t wc : wBase)
            name.push_back(static_cast<char>(wc));

        fs::path fullPath = m_dir / name;
        std::ifstream f(fullPath, std::ios::binary);
        if (!f.is_open())
        {
            fprintf(stderr, "[hlsli] Include not found: %s\n", fullPath.string().c_str());
            return E_FAIL;
        }

        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        ComPtr<IDxcBlobEncoding> pBlob;
        HRESULT hr = m_pUtils->CreateBlob(
            content.c_str(), static_cast<UINT32>(content.size()),
            DXC_CP_UTF8, &pBlob);
        if (FAILED(hr)) return hr;
        *ppIncludeSource = pBlob.Detach();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef()  override { return ++m_refCount; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDxcIncludeHandler))
        {
            *ppv = static_cast<IDxcIncludeHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
};

// ============================================================================
// D3D12 runtime context (WARP device + command infrastructure)
// ============================================================================

struct D3D12Ctx
{
    ComPtr<IDXGIFactory4>            factory;
    ComPtr<IDXGIAdapter>             warpAdapter;
    ComPtr<ID3D12Device>             device;
    ComPtr<ID3D12CommandQueue>       queue;
    ComPtr<ID3D12CommandAllocator>   allocator;
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence>              fence;
    HANDLE                           fenceEvent = nullptr;
    uint64_t                         fenceValue = 0;

    // Descriptor heap for UAV (one slot)
    ComPtr<ID3D12DescriptorHeap> uavHeap;
    UINT                         rtvDescSize = 0; // unused, but kept for completeness

    // Persistent UAV buffer (DEFAULT heap, 64 bytes)
    ComPtr<ID3D12Resource> uavBuf;
    // Readback buffer (READBACK heap, 64 bytes)
    ComPtr<ID3D12Resource> readbackBuf;
    // Upload (cbuffer) buffers — up to 3, each 512 bytes
    // Indices correspond to cbuffer slot order in each test.
    static constexpr int k_MaxCbufs = 3;
    ComPtr<ID3D12Resource> cbufUpload[k_MaxCbufs];

    void Init()
    {
        // Debug layer (optional; skip silently if unavailable)
        {
            ComPtr<ID3D12Debug> dbg;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
                dbg->EnableDebugLayer();
        }

        ThrowIfFailed(
            CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)),
            "CreateDXGIFactory2");

        ThrowIfFailed(
            factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)),
            "EnumWarpAdapter");

        ThrowIfFailed(
            D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_12_0,
                              IID_PPV_ARGS(&device)),
            "D3D12CreateDevice(WARP)");

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        ThrowIfFailed(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)),
                      "CreateCommandQueue");

        ThrowIfFailed(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&allocator)),
            "CreateCommandAllocator");

        ThrowIfFailed(
            device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&cmdList)),
            "CreateCommandList");
        cmdList->Close(); // start in closed state

        ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                          IID_PPV_ARGS(&fence)),
                      "CreateFence");
        fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) throw std::runtime_error("CreateEvent failed");

        // UAV descriptor heap (1 descriptor)
        {
            D3D12_DESCRIPTOR_HEAP_DESC dhd = {};
            dhd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            dhd.NumDescriptors = 1;
            dhd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            ThrowIfFailed(device->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&uavHeap)),
                          "CreateDescriptorHeap(UAV)");
        }

        auto MakeBuffer = [&](UINT64 size, D3D12_HEAP_TYPE heapType,
                               D3D12_RESOURCE_FLAGS flags,
                               D3D12_RESOURCE_STATES initialState)
            -> ComPtr<ID3D12Resource>
        {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = heapType;

            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width            = size;
            rd.Height           = 1;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            rd.Flags            = flags;

            ComPtr<ID3D12Resource> res;
            ThrowIfFailed(
                device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                initialState, nullptr,
                                                IID_PPV_ARGS(&res)),
                "CreateCommittedResource");
            return res;
        };

        // UAV output buffer (DEFAULT, 64 bytes = 16 × uint32)
        uavBuf = MakeBuffer(k_OutputElements * sizeof(uint32_t),
                            D3D12_HEAP_TYPE_DEFAULT,
                            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Readback buffer (READBACK, 64 bytes)
        readbackBuf = MakeBuffer(k_OutputElements * sizeof(uint32_t),
                                 D3D12_HEAP_TYPE_READBACK,
                                 D3D12_RESOURCE_FLAG_NONE,
                                 D3D12_RESOURCE_STATE_COPY_DEST);

        // cbuffer upload buffers (UPLOAD, 512 bytes each)
        for (int i = 0; i < k_MaxCbufs; ++i)
            cbufUpload[i] = MakeBuffer(k_CbufBytes,
                                       D3D12_HEAP_TYPE_UPLOAD,
                                       D3D12_RESOURCE_FLAG_NONE,
                                       D3D12_RESOURCE_STATE_GENERIC_READ);

        // UAV descriptor for the output buffer
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavd = {};
        uavd.ViewDimension               = D3D12_UAV_DIMENSION_BUFFER;
        uavd.Format                      = DXGI_FORMAT_UNKNOWN;
        uavd.Buffer.NumElements          = k_OutputElements;
        uavd.Buffer.StructureByteStride  = sizeof(uint32_t);
        uavd.Buffer.FirstElement         = 0;
        uavd.Buffer.Flags                = D3D12_BUFFER_UAV_FLAG_NONE;
        device->CreateUnorderedAccessView(
            uavBuf.Get(), nullptr, &uavd,
            uavHeap->GetCPUDescriptorHandleForHeapStart());
    }

    void WaitForGpu()
    {
        const uint64_t signalValue = ++fenceValue;
        ThrowIfFailed(queue->Signal(fence.Get(), signalValue), "queue->Signal");
        if (fence->GetCompletedValue() < signalValue)
        {
            ThrowIfFailed(fence->SetEventOnCompletion(signalValue, fenceEvent),
                          "SetEventOnCompletion");
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }

    void Shutdown()
    {
        if (fenceEvent) { CloseHandle(fenceEvent); fenceEvent = nullptr; }
    }
};

// ============================================================================
// Shader compilation (DXC)
// ============================================================================

static ComPtr<IDxcBlob> CompileShader(
    IDxcUtils*          pUtils,
    IDxcCompiler3*      pCompiler,
    const std::string&  hlslSrc,
    IDxcIncludeHandler* pInclude)
{
    ComPtr<IDxcBlobEncoding> pSrc;
    ThrowIfFailed(
        pUtils->CreateBlob(hlslSrc.c_str(),
                           static_cast<UINT32>(hlslSrc.size()),
                           DXC_CP_UTF8, &pSrc),
        "DXC: CreateBlob");

    DxcBuffer srcBuf{};
    srcBuf.Ptr      = pSrc->GetBufferPointer();
    srcBuf.Size     = pSrc->GetBufferSize();
    srcBuf.Encoding = DXC_CP_UTF8;

    // cs_6_2: supports doubles; -enable-16bit-types: float16/uint16_t etc.
    LPCWSTR args[] = {
        L"-T", L"cs_6_2",
        L"-E", L"main",
        L"-enable-16bit-types",
    };

    ComPtr<IDxcResult> pResult;
    ThrowIfFailed(
        pCompiler->Compile(&srcBuf, args, static_cast<UINT32>(std::size(args)),
                           pInclude, IID_PPV_ARGS(&pResult)),
        "DXC: Compile");

    HRESULT status = S_OK;
    pResult->GetStatus(&status);

    ComPtr<IDxcBlobUtf8> pErrors;
    pResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr);

    if (FAILED(status))
    {
        std::string errText = (pErrors && pErrors->GetStringLength() > 0)
                            ? pErrors->GetStringPointer()
                            : "(no error text)";
        throw std::runtime_error("DXC compile error:\n" + errText);
    }

    if (pErrors && pErrors->GetStringLength() > 0)
        fprintf(stderr, "[dxc] warnings:\n%s\n", pErrors->GetStringPointer());

    ComPtr<IDxcBlob> pDxil;
    ThrowIfFailed(
        pResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&pDxil), nullptr),
        "DXC: GetOutput(OBJECT)");
    return pDxil;
}

// ============================================================================
// Root signature builder
// Creates a root signature with:
//   [0] = UAV descriptor table (u0, space0)
//   [1..N] = inline CBV root descriptors for each cbuffer spec
// ============================================================================

struct CbufSpec
{
    uint32_t shaderRegister; // b0, b1, b2, ...
    uint32_t registerSpace;  // 0, 1, ...
};

static ComPtr<ID3D12RootSignature> BuildRootSignature(
    ID3D12Device*                  device,
    const std::vector<CbufSpec>&   cbuffers)
{
    const UINT numCbufs = static_cast<UINT>(cbuffers.size());
    const UINT numParams = 1 + numCbufs;

    std::vector<D3D12_ROOT_PARAMETER> params(numParams);

    // [0]: UAV descriptor table for g_output at u0, space0
    D3D12_DESCRIPTOR_RANGE uavRange = {};
    uavRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors                    = 1;
    uavRange.BaseShaderRegister                = 0;
    uavRange.RegisterSpace                     = 0;
    uavRange.OffsetInDescriptorsFromTableStart = 0;

    params[0].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges   = &uavRange;
    params[0].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    // [1..N]: inline CBV root descriptors
    for (UINT i = 0; i < numCbufs; ++i)
    {
        params[1 + i].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1 + i].Descriptor.ShaderRegister = cbuffers[i].shaderRegister;
        params[1 + i].Descriptor.RegisterSpace  = cbuffers[i].registerSpace;
        params[1 + i].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters     = numParams;
    rsd.pParameters       = params.data();
    rsd.NumStaticSamplers = 0;
    rsd.pStaticSamplers   = nullptr;
    rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> pSig, pErr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &pSig, &pErr);
    if (FAILED(hr))
    {
        std::string msg = "D3D12SerializeRootSignature failed";
        if (pErr) msg += std::string(": ") + (char*)pErr->GetBufferPointer();
        throw std::runtime_error(msg);
    }

    ComPtr<ID3D12RootSignature> rootSig;
    ThrowIfFailed(
        device->CreateRootSignature(0, pSig->GetBufferPointer(),
                                    pSig->GetBufferSize(),
                                    IID_PPV_ARGS(&rootSig)),
        "CreateRootSignature");
    return rootSig;
}

// ============================================================================
// Test case definition
// ============================================================================

using CbufData = std::array<uint32_t, k_CbufSlots>; // 512-byte cbuffer as uint32 slots

struct HlslRtTestCase
{
    const char*              name;
    const char*              shaderFile;        // filename under test/hlsl/compute/
    std::vector<CbufSpec>    cbuffers;           // register(bN, spaceS) for each cbuffer
    std::vector<CbufData>    cbufData;           // uploaded data per cbuffer
    std::vector<uint32_t>    expected;           // expected UAV output (uint32 bit-exact)
};

// ============================================================================
// Test case definitions
// ============================================================================

static std::vector<HlslRtTestCase> MakeTestCases()
{
    std::vector<HlslRtTestCase> tests;
    auto MkBuf = []() { CbufData d{}; return d; };

    // ------------------------------------------------------------------
    // 1. test_minimal
    //    cb { float4x4 g_mvp [slots 0-15]; float g_time [slot 16]; }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_minimal";
        tc.shaderFile = "hlsl_test_minimal.hlsl";
        tc.cbuffers   = {{0, 0}};  // b0 space0

        auto buf = MkBuf();
        buf[0]  = FloatBits(1.0f);  // g_mvp row0.x
        buf[1]  = FloatBits(2.0f);  // g_mvp row0.y
        buf[2]  = FloatBits(3.0f);  // g_mvp row0.z
        buf[3]  = FloatBits(4.0f);  // g_mvp row0.w
        buf[16] = FloatBits(5.0f);  // g_time  (offset 64 = slot 16)
        tc.cbufData.push_back(buf);

        tc.expected = {
            FloatBits(1.0f), FloatBits(2.0f), FloatBits(3.0f), FloatBits(4.0f),
            FloatBits(5.0f)
        };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 2. test_lighting
    //    LightingConstants {
    //      float4 LightData[8];           [slots 0-31, stride 4 slots]
    //      float4 ShadowCascadeSplits;    [slots 32-35]
    //      int    ActiveLightCount;       [slot 36]
    //      uint   FrameIndex;             [slot 37]
    //    }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_lighting";
        tc.shaderFile = "hlsl_test_lighting.hlsl";
        tc.cbuffers   = {{0, 0}};

        auto buf = MkBuf();
        buf[0]  = FloatBits(10.0f);  // LightData[0].x
        buf[30] = FloatBits(20.0f);  // LightData[7].z  (offset 120 = slot 30)
        buf[33] = FloatBits(30.0f);  // ShadowCascadeSplits.y (offset 132 = slot 33)
        buf[36] = 8;                 // ActiveLightCount = 8 (int bit pattern same as uint)
        buf[37] = 42;                // FrameIndex = 42
        tc.cbufData.push_back(buf);

        tc.expected = {
            FloatBits(10.0f), FloatBits(20.0f), FloatBits(30.0f),
            FloatBits(8.0f),  // asuint((float)8)
            42u
        };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 3. test_perframe
    //    PerFrame {
    //      float4x4 WorldViewProjection; [slots  0-15]
    //      float4x4 View;                [slots 16-31]
    //      float4x4 Projection;          [slots 32-47]
    //      float3   CameraPosition;      [slots 48-50]
    //      float    Time;                [slot  51]     offset 204
    //      float4   FogColorAndDensity;  [slots 52-55]
    //      float    NearPlane;           [slot  56]     offset 224
    //      float    FarPlane;            [slot  57]     offset 228
    //    }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_perframe";
        tc.shaderFile = "hlsl_test_perframe.hlsl";
        tc.cbuffers   = {{0, 0}};

        auto buf = MkBuf();
        buf[49] = FloatBits(7.5f);    // CameraPosition.y  (offset 196 = slot 49)
        buf[51] = FloatBits(1.25f);   // Time              (offset 204 = slot 51)
        buf[56] = FloatBits(0.1f);    // NearPlane         (offset 224 = slot 56)
        buf[57] = FloatBits(1000.0f); // FarPlane          (offset 228 = slot 57)
        tc.cbufData.push_back(buf);

        tc.expected = {
            FloatBits(1.25f), FloatBits(7.5f), FloatBits(0.1f), FloatBits(1000.0f)
        };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 4. test_srinput_basic  (two cbuffers)
    //    FrameConsts @ b0 { float4x4 m_Matrix [0-15]; float m_Time [16]; }
    //    PassConsts  @ b1 { uint m_PassIdx [0]; }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_srinput_basic";
        tc.shaderFile = "hlsl_test_srinput_basic.hlsl";
        tc.cbuffers   = {{0, 0}, {1, 0}};  // b0 and b1, space0

        auto buf0 = MkBuf();
        buf0[16] = FloatBits(3.75f);  // m_Time (offset 64 = slot 16)

        auto buf1 = MkBuf();
        buf1[0] = 99u;  // m_PassIdx

        tc.cbufData = {buf0, buf1};
        tc.expected = { FloatBits(3.75f), 99u };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 5. test_padding_trap
    //    Test { float3 position [0-2]; float radius [3]; float4 color [4-7]; }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_padding_trap";
        tc.shaderFile = "hlsl_test_padding_trap.hlsl";
        tc.cbuffers   = {{0, 0}};

        auto buf = MkBuf();
        buf[0] = FloatBits(1.0f);  // position.x
        buf[1] = FloatBits(2.0f);  // position.y
        buf[2] = FloatBits(3.0f);  // position.z
        buf[3] = FloatBits(4.0f);  // radius
        buf[4] = FloatBits(5.0f);  // color.x  (offset 16 = slot 4)
        buf[5] = FloatBits(6.0f);  // color.y
        tc.cbufData.push_back(buf);

        tc.expected = {
            FloatBits(1.0f), FloatBits(2.0f), FloatBits(3.0f),
            FloatBits(4.0f), FloatBits(5.0f), FloatBits(6.0f)
        };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 6. test_srinput_multiple — LightingInputs namespace only
    //    CameraConsts @ b0 { float3 Position [0-2]; float FOV [3]; }
    //    LightConsts  @ b1 { float3 Direction [0-2]; float Intensity [3]; }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_srinput_multiple";
        tc.shaderFile = "hlsl_test_srinput_multiple.hlsl";
        tc.cbuffers   = {{0, 0}, {1, 0}};

        auto buf0 = MkBuf();
        buf0[3] = FloatBits(60.0f);  // Camera.FOV (offset 12 = slot 3)

        auto buf1 = MkBuf();
        buf1[3] = FloatBits(0.75f);  // Light.Intensity (offset 12 = slot 3)

        tc.cbufData = {buf0, buf1};
        tc.expected = { FloatBits(60.0f), FloatBits(0.75f) };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 7. test_srinput_scalars — static const namespace values, no cbuffer binding
    //    MaxDistance = 1000.0f, MaxLights = 16, Pi = 3.14159f,
    //    NegOne = -1, Version = 2
    //    NOTE: FrameConsts cbuffer IS declared but not accessed; DXC optimizes
    //    it away from the resource binding table. We nonetheless bind a dummy
    //    zero-filled cbuffer to satisfy any debug validation.
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_srinput_scalars";
        tc.shaderFile = "hlsl_test_srinput_scalars.hlsl";
        tc.cbuffers   = {{0, 0}};  // FrameConsts @ b0 — dummy bind

        tc.cbufData.push_back(MkBuf());  // all zeros

        const float piVal = 3.14159f;
        tc.expected = {
            FloatBits(1000.0f),       // MaxDistance
            16u,                      // MaxLights
            FloatBits(piVal),         // Pi
            FloatBits(-1.0f),         // (float)NegOne
            2u                        // Version
        };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 8. test_arrays
    //    ExampleArrays {
    //      float2 before  [slots 0-1];
    //      uint   pad0    [slot 2];
    //      uint   pad1    [slot 3];
    //      float  array[2]; — each element is placed at a 16-byte boundary,
    //                         so array[0] @ slot 4 (offset 16),
    //                            array[1] @ slot 8 (offset 32).
    //                         However, float2 after follows the last element's
    //                         4-byte physical data, NOT the end of its 16-byte
    //                         register. So after.x @ slot 9 (offset 36).
    //      float2 after   [slots 9-10] (offset 36)
    //    }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_arrays";
        tc.shaderFile = "hlsl_test_arrays.hlsl";
        tc.cbuffers   = {{0, 0}};

        auto buf = MkBuf();
        buf[0]  = FloatBits(1.5f);  // before.x
        buf[1]  = FloatBits(2.5f);  // before.y
        buf[4]  = FloatBits(3.0f);  // array[0]  (offset 16 = slot 4)
        buf[8]  = FloatBits(4.0f);  // array[1]  (offset 32 = slot 8)
        // after.x/y: float array[2] has 16-byte-aligned elements inside the struct
        // but the NEXT non-array member picks up right after the last element's
        // 4 physical bytes (offset 36 = slot 9), not after the padded 16-byte slot.
        buf[9]  = FloatBits(5.0f);  // after.x   (offset 36 = slot 9)
        buf[10] = FloatBits(6.0f);  // after.y   (offset 40 = slot 10)
        tc.cbufData.push_back(buf);

        tc.expected = {
            FloatBits(1.5f), FloatBits(2.5f),
            FloatBits(3.0f), FloatBits(4.0f),
            FloatBits(5.0f), FloatBits(6.0f)
        };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 9. test_scene  (large cbuffer, > 256 bytes)
    //    SceneConstants {
    //      4 × float4x4 (256 bytes total, slots  0-63)
    //      float3 cameraPosWS    [slots 64-66]  offset 256
    //      float  exposure       [slot  67]     offset 268
    //      float4 fogParams      [slots 68-71]  offset 272
    //      float4 fogColor       [slots 72-75]  offset 288
    //      uint   frameNumber    [slot  76]     offset 304
    //      float  deltaTime      [slot  77]     offset 308
    //    }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_scene";
        tc.shaderFile = "hlsl_test_scene.hlsl";
        tc.cbuffers   = {{0, 0}};

        auto buf = MkBuf();
        buf[67] = FloatBits(2.2f);   // exposure     (offset 268 = slot 67)
        buf[76] = 64u;               // frameNumber  (offset 304 = slot 76)
        buf[77] = FloatBits(0.016f); // deltaTime    (offset 308 = slot 77)
        tc.cbufData.push_back(buf);

        tc.expected = {
            FloatBits(2.2f), 64u, FloatBits(0.016f)
        };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 10. test_matrix_variants
    //     StressMatrices { float4x4 rowMat [slots 0-15]; float4x4 colMat [slots 16-31]; ... }
    //     Tests first rows of the two float4x4 matrices.
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_matrix_variants";
        tc.shaderFile = "hlsl_test_matrix_variants.hlsl";
        tc.cbuffers   = {{0, 0}};

        auto buf = MkBuf();
        buf[0]  = FloatBits(10.0f);  // rowMat[0].x
        buf[1]  = FloatBits(20.0f);  // rowMat[0].y
        buf[16] = FloatBits(30.0f);  // colMat[0].x  (offset 64 = slot 16)
        buf[17] = FloatBits(40.0f);  // colMat[0].y
        tc.cbufData.push_back(buf);

        tc.expected = {
            FloatBits(10.0f), FloatBits(20.0f),
            FloatBits(30.0f), FloatBits(40.0f)
        };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 11. test_srinput_space_basic  (register space1)
    //     FrameConsts @ b0 space1 { float4x4 m_ViewProj [0-15]; float m_Time [16]; }
    //     PassConsts  @ b1 space1 { uint m_PassIdx [0]; }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_srinput_space_basic";
        tc.shaderFile = "hlsl_test_srinput_space_basic.hlsl";
        tc.cbuffers   = {{0, 1}, {1, 1}};  // b0 space1, b1 space1

        auto buf0 = MkBuf();
        buf0[16] = FloatBits(2.5f);  // m_Time (offset 64 = slot 16)

        auto buf1 = MkBuf();
        buf1[0] = 55u;  // m_PassIdx

        tc.cbufData = {buf0, buf1};
        tc.expected = { FloatBits(2.5f), 55u };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 12. test_srinput_compose_basic — ExtendedPass namespace
    //     FrameConsts @ b0 space0 { float4x4 m_ViewProj [0-15]; float m_Time [16]; }
    //     PassConsts  @ b1 space0 { uint m_PassIdx [0]; }
    //     SceneConsts @ b2 space0 { float3 m_SunDir [0-2]; float m_Intensity [3]; }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_srinput_compose_basic";
        tc.shaderFile = "hlsl_test_srinput_compose_basic.hlsl";
        tc.cbuffers   = {{0, 0}, {1, 0}, {2, 0}};  // b0, b1, b2 — space0

        auto buf0 = MkBuf();
        buf0[16] = FloatBits(4.0f);  // FrameConsts.m_Time (@ b0 slot 16)

        auto buf1 = MkBuf();  // PassConsts (unused in shader, bound for correctness)

        auto buf2 = MkBuf();
        buf2[3] = FloatBits(0.8f);  // SceneConsts.m_Intensity (@ b2 slot 3)

        tc.cbufData = {buf0, buf1, buf2};
        tc.expected = { FloatBits(4.0f), FloatBits(0.8f) };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 13. test_srinput_inherit_basic — Derived namespace
    //     FrameConsts @ b0 { float4x4 m_ViewProj [0-15]; float m_Time [16]; }
    //     SceneConsts @ b1 { float3 m_SunDir [0-2]; float m_Intensity [3]; }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_srinput_inherit_basic";
        tc.shaderFile = "hlsl_test_srinput_inherit_basic.hlsl";
        tc.cbuffers   = {{0, 0}, {1, 0}};

        auto buf0 = MkBuf();
        buf0[16] = FloatBits(6.0f);  // m_Time (offset 64 = slot 16)

        auto buf1 = MkBuf();
        buf1[3] = FloatBits(0.5f);  // m_Intensity (offset 12 = slot 3)

        tc.cbufData = {buf0, buf1};
        tc.expected = { FloatBits(6.0f), FloatBits(0.5f) };
        tests.push_back(std::move(tc));
    }

    // ------------------------------------------------------------------
    // 14. test_valid_struct
    //     valid_cb {
    //       ValidStruct data          [slots 0-2, NOT padded since not in array]
    //       float       extra         [slot 3]       offset 12
    //       ValidStruct array[5]      each element at 16-byte stride:
    //                                 array[0] @ slot 4  (offset 16)
    //                                 array[4] @ slot 20 (offset 80)
    //     }
    //     ValidStruct = { float x [0]; float y [1]; int count [2]; }
    // ------------------------------------------------------------------
    {
        HlslRtTestCase tc;
        tc.name       = "test_valid_struct";
        tc.shaderFile = "hlsl_test_valid_struct.hlsl";
        tc.cbuffers   = {{0, 0}};

        auto buf = MkBuf();
        buf[0]  = FloatBits(1.0f);  // data.x           (offset 0  = slot 0)
        buf[3]  = FloatBits(2.0f);  // extra             (offset 12 = slot 3)
        buf[4]  = FloatBits(3.0f);  // array[0].x        (offset 16 = slot 4)
        buf[20] = FloatBits(4.0f);  // array[4].x        (offset 80 = slot 20)
        tc.cbufData.push_back(buf);

        tc.expected = {
            FloatBits(1.0f), FloatBits(2.0f),
            FloatBits(3.0f), FloatBits(4.0f)
        };
        tests.push_back(std::move(tc));
    }

    return tests;
}

// ============================================================================
// Run one test case
// ============================================================================

static bool RunTest(
    const HlslRtTestCase& tc,
    D3D12Ctx&             ctx,
    IDxcUtils*            pUtils,
    IDxcCompiler3*        pCompiler,
    IDxcIncludeHandler*   pInclude,
    const fs::path&       shaderDir)
{
    printf("[test] %-45s ... ", tc.name);
    fflush(stdout);

    try
    {
        // ---- Read shader source ----------------------------------------
        fs::path shaderPath = shaderDir / tc.shaderFile;
        std::ifstream shaderFile(shaderPath, std::ios::binary);
        if (!shaderFile.is_open())
            throw std::runtime_error("Cannot open shader: " + shaderPath.string());
        std::string hlslSrc((std::istreambuf_iterator<char>(shaderFile)),
                             std::istreambuf_iterator<char>());

        // ---- Compile shader --------------------------------------------
        ComPtr<IDxcBlob> pDxil = CompileShader(pUtils, pCompiler, hlslSrc, pInclude);

        // ---- Build root signature and PSO ------------------------------
        ComPtr<ID3D12RootSignature> rootSig =
            BuildRootSignature(ctx.device.Get(), tc.cbuffers);

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature    = rootSig.Get();
        psoDesc.CS.pShaderBytecode = pDxil->GetBufferPointer();
        psoDesc.CS.BytecodeLength  = pDxil->GetBufferSize();

        ComPtr<ID3D12PipelineState> pso;
        ThrowIfFailed(
            ctx.device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)),
            "CreateComputePipelineState");

        // ---- Upload cbuffer data ----------------------------------------
        const int numCbufs = static_cast<int>(tc.cbuffers.size());
        assert(numCbufs <= D3D12Ctx::k_MaxCbufs);

        for (int i = 0; i < numCbufs; ++i)
        {
            void* pMapped = nullptr;
            ThrowIfFailed(
                ctx.cbufUpload[i]->Map(0, nullptr, &pMapped),
                "cbufUpload Map");
            std::memcpy(pMapped, tc.cbufData[i].data(), k_CbufBytes);
            ctx.cbufUpload[i]->Unmap(0, nullptr);
        }

        // ---- Record and execute commands --------------------------------
        ThrowIfFailed(ctx.allocator->Reset(),    "CmdAllocator Reset");
        ThrowIfFailed(ctx.cmdList->Reset(ctx.allocator.Get(), nullptr), "CmdList Reset");

        ID3D12DescriptorHeap* heaps[] = { ctx.uavHeap.Get() };
        ctx.cmdList->SetDescriptorHeaps(1, heaps);
        ctx.cmdList->SetComputeRootSignature(rootSig.Get());

        // Bind UAV descriptor table
        ctx.cmdList->SetComputeRootDescriptorTable(
            0, ctx.uavHeap->GetGPUDescriptorHandleForHeapStart());

        // Bind cbuffers as inline root descriptors
        for (int i = 0; i < numCbufs; ++i)
            ctx.cmdList->SetComputeRootConstantBufferView(
                static_cast<UINT>(1 + i),
                ctx.cbufUpload[i]->GetGPUVirtualAddress());

        ctx.cmdList->SetPipelineState(pso.Get());
        ctx.cmdList->Dispatch(1, 1, 1);

        // UAV barrier then copy to readback
        D3D12_RESOURCE_BARRIER uavBarrier = {};
        uavBarrier.Type                    = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uavBarrier.UAV.pResource           = ctx.uavBuf.Get();
        ctx.cmdList->ResourceBarrier(1, &uavBarrier);

        // Transition UAV buf: UNORDERED_ACCESS → COPY_SOURCE
        D3D12_RESOURCE_BARRIER toSrc = {};
        toSrc.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toSrc.Transition.pResource   = ctx.uavBuf.Get();
        toSrc.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toSrc.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toSrc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ctx.cmdList->ResourceBarrier(1, &toSrc);

        ctx.cmdList->CopyResource(ctx.readbackBuf.Get(), ctx.uavBuf.Get());

        // Transition back: COPY_SOURCE → UNORDERED_ACCESS
        D3D12_RESOURCE_BARRIER toUav = {};
        toUav.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toUav.Transition.pResource   = ctx.uavBuf.Get();
        toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toUav.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toUav.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ctx.cmdList->ResourceBarrier(1, &toUav);

        ThrowIfFailed(ctx.cmdList->Close(), "CmdList Close");

        ID3D12CommandList* lists[] = { ctx.cmdList.Get() };
        ctx.queue->ExecuteCommandLists(1, lists);
        ctx.WaitForGpu();

        // ---- Read back and compare -------------------------------------
        D3D12_RANGE readRange = { 0, k_OutputElements * sizeof(uint32_t) };
        void* pReadback = nullptr;
        ThrowIfFailed(
            ctx.readbackBuf->Map(0, &readRange, &pReadback),
            "readbackBuf Map");

        const uint32_t* pOut = static_cast<const uint32_t*>(pReadback);
        bool ok = true;

        for (size_t i = 0; i < tc.expected.size(); ++i)
        {
            if (pOut[i] != tc.expected[i])
            {
                if (ok) printf("FAIL\n");
                ok = false;
                float gotF, expF;
                std::memcpy(&gotF, &pOut[i],         4);
                std::memcpy(&expF, &tc.expected[i],  4);
                printf("  [%zu] got 0x%08X (%.6g)  expected 0x%08X (%.6g)\n",
                       i, pOut[i], gotF, tc.expected[i], expF);
            }
        }

        D3D12_RANGE noWrite = { 0, 0 };
        ctx.readbackBuf->Unmap(0, &noWrite);

        if (ok) printf("PASS\n");
        return ok;
    }
    catch (const std::exception& e)
    {
        printf("ERROR\n  %s\n", e.what());
        return false;
    }
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[])
{
    // Derive workspace root from executable path: bin/../ = workspace root
    fs::path exeDir;
    try
    {
        exeDir = fs::canonical(argv[0]).parent_path();
    }
    catch (...)
    {
        exeDir = fs::current_path();
    }

    fs::path workspaceRoot = exeDir.parent_path();
    fs::path shaderDir  = workspaceRoot / "test" / "hlsl" / "compute";
    fs::path hlsliDir   = workspaceRoot / "test" / "output" / "hlsl";

    // Optional overrides via command line
    if (argc >= 2) shaderDir = argv[1];
    if (argc >= 3) hlsliDir  = argv[2];

    printf("HLSL Runtime Validation\n");
    printf("  Shader dir : %s\n", shaderDir.string().c_str());
    printf("  HLSLI  dir : %s\n", hlsliDir.string().c_str());
    printf("\n");

    // ---- Init DXC -------------------------------------------------------
    ComPtr<IDxcUtils>     pUtils;
    ComPtr<IDxcCompiler3> pCompiler;
    try
    {
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils,     IID_PPV_ARGS(&pUtils)),
                      "DxcCreateInstance(Utils) — is dxcompiler.dll in bin/?");
        ThrowIfFailed(DxcCreateInstance(CLSID_DxcCompiler,  IID_PPV_ARGS(&pCompiler)),
                      "DxcCreateInstance(Compiler)");
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "[fatal] DXC init failed: %s\n", e.what());
        return 1;
    }

    // File-based include handler: reads .hlsli from hlsliDir
    FsIncludeHandler* rawInclude = new FsIncludeHandler(pUtils.Get(), hlsliDir);
    ComPtr<IDxcIncludeHandler> pInclude;
    pInclude.Attach(rawInclude);

    // ---- Init D3D12 (WARP) ----------------------------------------------
    D3D12Ctx ctx;
    try
    {
        ctx.Init();
        printf("D3D12 WARP device initialized.\n\n");
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "[fatal] D3D12 init failed: %s\n", e.what());
        return 1;
    }

    // ---- Run all tests --------------------------------------------------
    auto testCases = MakeTestCases();
    int passed = 0, failed = 0;

    printf("Running %zu tests...\n\n", testCases.size());

    for (const auto& tc : testCases)
    {
        bool ok = RunTest(tc, ctx, pUtils.Get(), pCompiler.Get(),
                          pInclude.Get(), shaderDir);
        if (ok) ++passed; else ++failed;
    }

    printf("\n=== Results: %d/%d passed ===\n",
           passed, (int)testCases.size());

    ctx.Shutdown();
    return (failed == 0) ? 0 : 1;
}
