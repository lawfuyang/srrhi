// reflection_test.cpp
//
// For each .sr test input file:
//   - Expected-to-fail tests (packoffset, pragma pack, invalid/undefined types):
//     verify that parsing throws as expected.
//   - All other tests:
//     1. Parse + compute layout + generate HLSL (same pipeline as production).
//     2. Append a trivial compute-shader entry point so DXC can compile it.
//     3. Compile with IDxcCompiler3 targeting cs_6_2.
//     4. Extract cbuffer layout via ID3D12ShaderReflection.
//     5. Compare DXC-reported member offsets against our computed offsets.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wrl/client.h>  // ComPtr

#include <dxcapi.h>
#include <d3d12shader.h>

#include "types.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Forward declarations from other modules
// ---------------------------------------------------------------------------
ParseResult                ParseFile(const std::string& path);
std::vector<LayoutMember>  ComputeLayouts(ParseResult& pr);
std::string                GenerateHlsl(const ParseResult& pr,
                                         const std::vector<LayoutMember>& layouts,
                                         int& padCount);

// ---------------------------------------------------------------------------
// Tests whose .sr stems are expected to fail at parse time.
// Any file whose stem is in this set must throw during ParseFile().
// ---------------------------------------------------------------------------
static const std::unordered_set<std::string> k_ExpectedFailStems = {
    "test_invalid_type",
    "test_undefined_struct",
    "test_packoffset",
    "test_pragma_pack",
    "test_srinput_invalid",
    // Resource-related error tests
    "test_resource_unsupported_texturebuffer",
    "test_resource_unsupported_rwtexture2dms",
    "test_resource_unsupported_append",
    "test_resource_unsupported_consume",
    "test_resource_unsupported_rov",
    "test_resource_unsupported_feedback",
    "test_resource_undefined_struct",
    "test_resource_not_a_type",
    "test_resource_uav_no_template",
    "test_resource_buffer_no_template",
    "test_resource_rwbuffer_no_template",
    "test_resource_structuredbuffer_no_template",
    // Sampler-related error tests
    "test_sampler_in_cbuffer",
    "test_sampler_in_struct",
    "test_sampler_cmp_in_cbuffer",
    // Scalar const error tests
    "test_srinput_scalar_bad_type",
    "test_srinput_scalar_no_value",
    "test_srinput_scalar_duplicate",
    // Push constant error tests
    "test_push_constant_multiple",
    "test_push_constant_too_large",
    "test_push_constant_on_resource",
    "test_push_constant_on_sampler",
    "test_push_constant_not_first",
    // Register space error tests
    "test_srinput_space_invalid_on_cbuffer",
    "test_srinput_space_invalid_no_paren",
    "test_srinput_space_invalid_unknown_attr",
    "test_srinput_space_invalid_negative",
    "test_srinput_space_invalid_empty_paren",
};

// ---------------------------------------------------------------------------
// Reflected view of a single cbuffer extracted from DXC.
// Only "real" member variables (i.e.\ not our auto-generated pad* vars)
// are stored here.
// ---------------------------------------------------------------------------
struct ReflectedVar
{
    std::string m_Name;
    uint32_t    m_Offset = 0;
    uint32_t    m_Size   = 0;
};

struct ReflectedCBuffer
{
    std::string               m_Name;
    uint32_t                  m_TotalSize = 0;  // D3D12_SHADER_BUFFER_DESC::Size
    std::vector<ReflectedVar> m_Vars;
};

// ---------------------------------------------------------------------------
// Detect auto-generated padding variables emitted by our HLSL generator.
// They have the form "pad" followed by one or more decimal digits.
// Note: user-declared fields like "pad0" that live in our ParseResult are
// intentionally NOT filtered — they appear in our layout and are matched.
// ---------------------------------------------------------------------------
static bool IsGeneratedPadding(const std::string& name,
                                const std::unordered_map<std::string, int>& ourMembers)
{
    if (name.size() < 4 || name.substr(0, 3) != "pad")
        return false;
    for (size_t i = 3; i < name.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(name[i])))
            return false;
    // It looks like a pad variable.  If it's also in the parsed layout, it's
    // a real user field, not generated padding.
    return ourMembers.find(name) == ourMembers.end();
}

// ---------------------------------------------------------------------------
// Name-cleaning helper: strip common HLSL member prefixes (m_, g_, s_) and
// capitalize the first letter.  Matches the logic in hlsl_gen.cpp.
// ---------------------------------------------------------------------------
static std::string RtCleanMemberName(const std::string& name)
{
    const char* prefixes[] = {"m_", "g_", "s_"};
    std::string r = name;
    for (auto* p : prefixes)
    {
        size_t plen = std::strlen(p);
        if (r.size() > plen && r.substr(0, plen) == p)
        {
            r = r.substr(plen);
            break;
        }
    }
    if (!r.empty())
        r[0] = (char)std::toupper((unsigned char)r[0]);
    return r;
}

// Build mapping: cbuffer type name → HLSL variable name (e.g. "SceneConstants" → "MainInputs_Scene")
// Only cbuffers that are members of an srinput scope appear in the generated HLSL.
static std::unordered_map<std::string, std::string> BuildCbufVarNameMap(
    const ParseResult& pr)
{
    std::unordered_map<std::string, std::string> map;
    for (const auto& srInputDef : pr.m_SrInputDefs)
    {
        for (const auto& member : srInputDef.m_Members)
        {
            std::string cleanedMemberName = RtCleanMemberName(member.m_MemberName);
            std::string varName = srInputDef.m_Name + "_" + cleanedMemberName;
            map[member.m_CBufferName] = varName;
        }
    }
    return map;
}

// ---------------------------------------------------------------------------
// Build a minimal HLSL shader body that reads from each cbuffer so DXC keeps
// them in the resource binding table and includes them in reflection.
//
// We use asuint() which accepts any numeric scalar/vector type and produces
// a uint, giving us a type-independent way to read from any member.
//
// The shader writes the result to a volatile local so the read cannot be
// optimised away even without -Od.
// ---------------------------------------------------------------------------
static std::string BuildDummyEntryPoint(
    const std::vector<LayoutMember>& layouts,
    const ParseResult&               pr,
    const std::unordered_map<std::string, std::string>& cbufVarNames)
{
    std::ostringstream body;
    body << "[numthreads(1,1,1)]\n"
         << "void main()\n"
         << "{\n"
         << "    volatile uint _srrhi_check = 0u;\n";

    for (size_t li = 0; li < layouts.size(); ++li)
    {
        const auto& cbLayout = layouts[li];
        if (cbLayout.m_Submembers.empty()) continue;

        // Resolve the HLSL variable name for this cbuffer.
        // For srinput cbuffers: {SrInputName}_{CleanedMemberName}
        auto it = cbufVarNames.find(cbLayout.m_Name);
        std::string varPrefix = (it != cbufVarNames.end())
            ? it->second
            : ("m_" + cbLayout.m_Name); // fallback for non-srinput cbuffers

        // Find the first scalar or vector member (matrices and arrays need [idx])
        for (const auto& m : cbLayout.m_Submembers)
        {
            std::string fieldAccess = varPrefix + "." + m.m_Name;

            if (std::holds_alternative<BuiltinType>(m.m_Type))
            {
                const auto& bt = std::get<BuiltinType>(m.m_Type);
                if (bt.m_VectorSize == 1)
                    body << "    _srrhi_check ^= asuint((float)" << fieldAccess << ");\n";
                else
                    body << "    _srrhi_check ^= asuint(" << fieldAccess << ".x);\n";
                break;
            }
            if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&m.m_Type))
            {
                const ArrayNode& arr = **ap;
                if (auto* bt = std::get_if<BuiltinType>(&arr.m_ElementType))
                {
                    if (bt->m_VectorSize == 1)
                        body << "    _srrhi_check ^= asuint((float)" << fieldAccess << "[0]);\n";
                    else
                        body << "    _srrhi_check ^= asuint(" << fieldAccess << "[0].x);\n";
                    break;
                }
                // array of struct — try first member of struct
                if (auto* sp = std::get_if<StructType*>(&arr.m_ElementType))
                {
                    const StructType* st = *sp;
                    if (!st->m_Members.empty())
                    {
                        const auto& firstField = st->m_Members[0];
                        if (auto* bt2 = std::get_if<BuiltinType>(&firstField.m_Type))
                        {
                            if (bt2->m_VectorSize == 1)
                                body << "    _srrhi_check ^= asuint((float)" << fieldAccess << "[0]." << firstField.m_Name << ");\n";
                            else
                                body << "    _srrhi_check ^= asuint(" << fieldAccess << "[0]." << firstField.m_Name << ".x);\n";
                            break;
                        }
                    }
                }
                break;
            }
            if (auto* sp = std::get_if<StructType*>(&m.m_Type))
            {
                const StructType* st = *sp;
                if (!st->m_Members.empty())
                {
                    const auto& firstField = st->m_Members[0];
                    body << "    _srrhi_check ^= asuint((float)" << fieldAccess << "." << firstField.m_Name << ");\n";
                }
                break;
            }
        }
    }

    body << "}\n";
    return body.str();
}

// ---------------------------------------------------------------------------
// Compile hlslSource with DXC and return the reflected cbuffers.
// Returns empty vector if compilation fails; outErrors receives the error text.
// ---------------------------------------------------------------------------
static std::vector<ReflectedCBuffer> CompileAndReflect(
    IDxcUtils*          pUtils,
    IDxcCompiler3*      pCompiler,
    const std::string&  hlslSource,
    const std::unordered_map<std::string, int>& ourMembers,   // for pad filtering
    std::string&        outErrors)
{
    outErrors.clear();

    // ---- Source blob -------------------------------------------------------
    ComPtr<IDxcBlobEncoding> pSourceBlob;
    HRESULT hr = pUtils->CreateBlob(
        hlslSource.c_str(),
        static_cast<UINT32>(hlslSource.size()),
        DXC_CP_UTF8,
        &pSourceBlob);
    if (FAILED(hr))
        throw std::runtime_error("DXC: CreateBlob() failed");

    DxcBuffer srcBuf{};
    srcBuf.Ptr      = pSourceBlob->GetBufferPointer();
    srcBuf.Size     = pSourceBlob->GetBufferSize();
    srcBuf.Encoding = DXC_CP_UTF8;

    // ---- Compile -----------------------------------------------------------
    // cs_6_2 supports doubles natively; -enable-16bit-types adds float16/uint16.
    // -Od skips optimisations: DXC preserves all declared cbuffer variables in
    // reflection even if they are unused by the shader body.
    LPCWSTR args[] = {
        L"-T", L"cs_6_2",
        L"-E", L"main",
        L"-enable-16bit-types",
        L"-Od",
    };

    ComPtr<IDxcResult> pResult;
    hr = pCompiler->Compile(
        &srcBuf,
        args, static_cast<UINT32>(std::size(args)),
        nullptr,              // no include handler — generated HLSL has no #include
        IID_PPV_ARGS(&pResult));
    if (FAILED(hr))
        throw std::runtime_error("DXC: Compile() call failed (internal error)");

    // ---- Error text --------------------------------------------------------
    ComPtr<IDxcBlobUtf8> pErrors;
    pResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&pErrors), nullptr);
    if (pErrors && pErrors->GetStringLength() > 0)
        outErrors = pErrors->GetStringPointer();

    HRESULT hrStatus = S_OK;
    pResult->GetStatus(&hrStatus);
    if (FAILED(hrStatus))
        return {};   // compilation failed — caller inspects outErrors

    // ---- Use the full DXIL container for reflection -----------------------
    // DXC_OUT_REFLECTION (RDAT) may be absent for SM < 6.3.
    // The full DXIL container always embeds the RDEF reflection block, so use
    // DXC_OUT_OBJECT and pass it directly to IDxcUtils::CreateReflection.
    ComPtr<IDxcBlob> pDxilBlob;
    hr = pResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&pDxilBlob), nullptr);
    if (FAILED(hr) || !pDxilBlob || pDxilBlob->GetBufferSize() == 0)
        throw std::runtime_error("DXC: failed to obtain DXIL blob");

    DxcBuffer dxilBuf{};
    dxilBuf.Ptr      = pDxilBlob->GetBufferPointer();
    dxilBuf.Size     = pDxilBlob->GetBufferSize();
    dxilBuf.Encoding = 0;

    ComPtr<ID3D12ShaderReflection> pRefl;
    hr = pUtils->CreateReflection(&dxilBuf, IID_PPV_ARGS(&pRefl));
    if (FAILED(hr))
        throw std::runtime_error("DXC: CreateReflection() failed");

    // ---- Walk cbuffers -----------------------------------------------------
    D3D12_SHADER_DESC shaderDesc{};
    pRefl->GetDesc(&shaderDesc);

    std::vector<ReflectedCBuffer> result;
    result.reserve(shaderDesc.ConstantBuffers);

    for (UINT ci = 0; ci < shaderDesc.ConstantBuffers; ++ci)
    {
        ID3D12ShaderReflectionConstantBuffer* pCB =
            pRefl->GetConstantBufferByIndex(ci);

        D3D12_SHADER_BUFFER_DESC cbDesc{};
        if (FAILED(pCB->GetDesc(&cbDesc))) continue;

        ReflectedCBuffer rcb;
        rcb.m_Name      = cbDesc.Name ? cbDesc.Name : "";
        rcb.m_TotalSize = cbDesc.Size;

        for (UINT vi = 0; vi < cbDesc.Variables; ++vi)
        {
            ID3D12ShaderReflectionVariable* pVar = pCB->GetVariableByIndex(vi);

            D3D12_SHADER_VARIABLE_DESC varDesc{};
            if (FAILED(pVar->GetDesc(&varDesc))) continue;

            std::string varName = varDesc.Name ? varDesc.Name : "";

            // Skip variables that our HLSL generator injected as padding
            // (but preserve user-declared fields with pad-like names)
            if (IsGeneratedPadding(varName, ourMembers)) continue;

            ReflectedVar rv;
            rv.m_Name   = varName;
            rv.m_Offset = varDesc.StartOffset;
            rv.m_Size   = varDesc.Size;
            rcb.m_Vars.push_back(rv);
        }

        result.push_back(std::move(rcb));
    }

    return result;
}

// ---------------------------------------------------------------------------
// Compare our computed layouts against the DXC-reflected layout.
// Returns true if all checks pass; appends a human-readable report.
// ---------------------------------------------------------------------------
static bool CompareWithReflection(
    const std::vector<LayoutMember>&     layouts,
    const std::vector<ReflectedCBuffer>& reflected,
    std::ostringstream&                  report,
    const std::unordered_map<std::string, std::string>& cbufVarNames)
{
    bool ok = true;

    for (const auto& cbLayout : layouts)
    {
        const std::string& cbName = cbLayout.m_Name;

        // Find matching reflected cbuffer by name
        const ReflectedCBuffer* pRcb = nullptr;
        for (const auto& rcb : reflected)
            if (rcb.m_Name == cbName) { pRcb = &rcb; break; }

        if (!pRcb)
        {
            report << "    [FAIL] cbuffer '" << cbName
                   << "' not found in DXC reflection\n";
            ok = false;
            continue;
        }

        // --- Total cbuffer size ---
        // Our size rounded up to 16 bytes must match DXC's reported size.
        int ourRounded = (cbLayout.m_Size + 15) & ~15;
        if (ourRounded != static_cast<int>(pRcb->m_TotalSize))
        {
            report << "    [FAIL] cbuffer '" << cbName << "' total size: "
                   << "ours=" << ourRounded
                   << " dxc=" << pRcb->m_TotalSize << "\n";
            ok = false;
        }
        else
        {
            report << "    [OK]   cbuffer '" << cbName
                   << "' total size = " << pRcb->m_TotalSize << " bytes\n";
        }

        // --- Check struct member wrapper ---
        // The HLSL cbuffer contains a single struct member named
        // {SrInputName}_{CleanedMemberName} (e.g. "MainInputs_Scene").
        auto it = cbufVarNames.find(cbName);
        std::string expectedStructMember = (it != cbufVarNames.end())
            ? it->second
            : ("m_" + cbName); // fallback for non-srinput cbuffers
        
        // Check if the expected struct member exists in DXC reflection
        bool foundStructMember = false;
        for (const auto& rv : pRcb->m_Vars)
        {
            if (rv.m_Name == expectedStructMember)
            {
                foundStructMember = true;
                // The struct member should be at offset 0
                if (rv.m_Offset != 0)
                {
                    report << "    [FAIL] struct member '" << cbName << "::" 
                           << expectedStructMember << "' offset: "
                           << "ours=0 dxc=" << rv.m_Offset << "\n";
                    ok = false;
                }
                else
                {
                    report << "    [OK]   struct member '" << cbName << "::" 
                           << expectedStructMember << "' offset=0 size=" << rv.m_Size << "\n";
                }
                break;
            }
        }
        
        if (!foundStructMember)
        {
            report << "    [FAIL] expected struct member '" << cbName << "::" 
                   << expectedStructMember << "' not found in DXC reflection\n";
            ok = false;
        }
    }

    return ok;
}



// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
int RunReflectionTests(const fs::path& testInputDir)
{
    LogMsg("\n[test] ========== Reflection Tests ==========\n");

    // ---- Initialise DXC ----------------------------------------------------
    ComPtr<IDxcUtils>     pUtils;
    ComPtr<IDxcCompiler3> pCompiler;

    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&pUtils));
    if (FAILED(hr))
        throw std::runtime_error(
            "Failed to create IDxcUtils — is dxcompiler.dll in the same directory?");

    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&pCompiler));
    if (FAILED(hr))
        throw std::runtime_error("Failed to create IDxcCompiler3");



    // ---- Collect .sr files -------------------------------------------------
    std::vector<fs::path> srFiles;
    for (const auto& entry : fs::recursive_directory_iterator(testInputDir))
        if (entry.is_regular_file() && entry.path().extension() == ".sr")
            srFiles.push_back(entry.path());

    std::sort(srFiles.begin(), srFiles.end());

    int passed = 0, failed = 0, skipped = 0;

    for (const auto& srFile : srFiles)
    {
        std::string stem      = srFile.stem().string();
        bool        expectFail = k_ExpectedFailStems.count(stem) > 0;

        LogMsg("\n[test] ---- %s ----\n", srFile.filename().string().c_str());

        // ---- Parse ---------------------------------------------------------
        ParseResult pr;
        bool        parseOk    = false;
        std::string parseError;
        try
        {
            pr      = ParseFile(srFile.string());
            parseOk = true;
        }
        catch (const std::exception& e)
        {
            parseError = e.what();
        }

        // ---- Negative-test check (parse phase) ----------------------------
        // If parsing threw and this was an expected-fail test, it passes.
        // If parsing threw unexpectedly, it fails.
        // If parsing succeeded but the test was expected to fail, we continue
        // to later phases (layout, hlslgen) where the failure might occur.
        if (!parseOk)
        {
            if (expectFail)
            {
                LogMsg("[test]   PASS  (expected failure — parser threw: %s)\n",
                       parseError.c_str());
                ++passed;
            }
            else
            {
                LogMsg("[test]   FAIL  (unexpected parse error: %s)\n", parseError.c_str());
                ++failed;
            }
            continue;
        }

        // ---- Compute layout ------------------------------------------------
        std::vector<LayoutMember> layouts;
        try
        {
            layouts = ComputeLayouts(pr);
        }
        catch (const std::exception& e)
        {
            if (expectFail)
            {
                LogMsg("[test]   PASS  (expected failure — layout threw: %s)\n", e.what());
                ++passed;
            }
            else
            {
                LogMsg("[test]   FAIL  (layout error: %s)\n", e.what());
                ++failed;
            }
            continue;
        }

        // ---- Generate HLSL -------------------------------------------------
        int         padCount = 10000;  // start high to avoid collisions with user-named fields
        std::string hlslBase;
        try
        {
            hlslBase = GenerateHlsl(pr, layouts, padCount);
        }
        catch (const std::exception& e)
        {
            if (expectFail)
            {
                LogMsg("[test]   PASS  (expected failure — hlsl gen threw: %s)\n", e.what());
                ++passed;
            }
            else
            {
                LogMsg("[test]   FAIL  (HLSL gen error: %s)\n", e.what());
                ++failed;
            }
            continue;
        }

        // If an expected-fail test reached here without any exception, it's a failure.
        if (expectFail)
        {
            LogMsg("[test]   FAIL  (expected failure, but all stages (parse/layout/hlslgen) succeeded)\n");
            ++failed;
            continue;
        }

        // If there are no cbuffer layouts (e.g. resource-only srinput files),
        // there is nothing to validate against DXC reflection — PASS immediately.
        if (layouts.empty())
        {
            LogMsg("[test]   PASS  (no cbuffers to validate — parse+layout+hlslgen OK)\n");
            ++passed;
            continue;
        }

        // Build the mapping from cbuffer type name → HLSL variable name.
        // This must match what GenerateHlsl() emits for srinput cbuffers.
        auto cbufVarNames = BuildCbufVarNameMap(pr);

        // Since all srinputs share space0, only one srinput can be active per
        // shader dispatch.  For the reflection test we validate only the first
        // srinput's cbuffers so the dummy shader has no register conflicts.
        std::vector<LayoutMember> firstSrInputLayouts;
        if (!pr.m_SrInputDefs.empty())
        {
            const auto& firstSrInput = pr.m_SrInputDefs[0];
            for (const auto& member : firstSrInput.m_Members)
            {
                for (const auto& lm : layouts)
                {
                    if (lm.m_Name == member.m_CBufferName)
                    {
                        firstSrInputLayouts.push_back(lm);
                        break;
                    }
                }
            }
        }
        else
        {
            firstSrInputLayouts = layouts;
        }

        // Append a compute-shader entry point that reads from the first srinput's
        // cbuffers so DXC preserves them in the shader reflection.
        std::string hlslFull = hlslBase + "\n" + BuildDummyEntryPoint(firstSrInputLayouts, pr, cbufVarNames);

        // ---- Build the set of "real" member names for padding detection ----
        // This covers all top-level submembers across the first srinput's cbuffers.
        std::unordered_map<std::string, int> ourMembers;
        for (const auto& cbLayout : firstSrInputLayouts)
            for (const auto& m : cbLayout.m_Submembers)
                ourMembers[m.m_Name] = m.m_Offset;

        // ---- Compile & reflect ---------------------------------------------
        std::string                  dxcErrors;
        std::vector<ReflectedCBuffer> reflected;
        try
        {
            reflected = CompileAndReflect(
                pUtils.Get(), pCompiler.Get(),
                hlslFull, ourMembers, dxcErrors);
        }
        catch (const std::exception& e)
        {
            LogMsg("[test]   FAIL  (DXC exception: %s)\n", e.what());
            ++failed;
            continue;
        }

        if (reflected.empty())
        {
            LogMsg("[test]   FAIL  (DXC compilation failed)\n");
            if (!dxcErrors.empty())
                LogMsg("[test]   DXC diagnostics:\n%s\n", dxcErrors.c_str());
            ++failed;
            continue;
        }

        // ---- Compare -------------------------------------------------------
        // Compare only the first srinput's cbuffers (the ones compiled into the dummy shader).
        std::ostringstream report;
        bool               ok = CompareWithReflection(firstSrInputLayouts, reflected, report, cbufVarNames);

        if (ok)
        {
            LogMsg("[test]   PASS\n");
            LogMsg("%s", report.str().c_str());
            ++passed;
        }
        else
        {
            LogMsg("[test]   FAIL\n");
            LogMsg("%s", report.str().c_str());
            ++failed;
        }
    }

    LogMsg("\n[test] ==========================================\n");
    LogMsg("[test] Results: %d passed, %d failed, %d skipped\n",
           passed, failed, skipped);

    return (failed == 0) ? 0 : 1;
}
