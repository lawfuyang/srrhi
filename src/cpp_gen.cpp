#include "types.h"
#include "common.h"
#include "flatten.h"
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <cctype>

// ---------------------------------------------------------------------------
// Map a ResourceKind to the corresponding srrhi::ResourceType enum string.
// Used to initialize ResourceEntry::type in the generated resources[] array.
// ---------------------------------------------------------------------------
static std::string ResourceKindToSrrResourceType(ResourceKind kind)
{
    switch (kind)
    {
        case ResourceKind::Texture1D:
        case ResourceKind::Texture1DArray:
        case ResourceKind::Texture2D:
        case ResourceKind::Texture2DArray:
        case ResourceKind::Texture2DMS:
        case ResourceKind::Texture2DMSArray:
        case ResourceKind::Texture3D:
        case ResourceKind::TextureCube:
        case ResourceKind::TextureCubeArray:        return "srrhi::ResourceType::Texture_SRV";
        case ResourceKind::Buffer:                  return "srrhi::ResourceType::TypedBuffer_SRV";
        case ResourceKind::StructuredBuffer:        return "srrhi::ResourceType::StructuredBuffer_SRV";
        case ResourceKind::ByteAddressBuffer:       return "srrhi::ResourceType::RawBuffer_SRV";
        case ResourceKind::RaytracingAccelerationStructure: return "srrhi::ResourceType::RayTracingAccelStruct";
        case ResourceKind::RWTexture1D:
        case ResourceKind::RWTexture1DArray:
        case ResourceKind::RWTexture2D:
        case ResourceKind::RWTexture2DArray:
        case ResourceKind::RWTexture3D:             return "srrhi::ResourceType::Texture_UAV";
        case ResourceKind::RWBuffer:                return "srrhi::ResourceType::TypedBuffer_UAV";
        case ResourceKind::RWStructuredBuffer:      return "srrhi::ResourceType::StructuredBuffer_UAV";
        case ResourceKind::RWByteAddressBuffer:     return "srrhi::ResourceType::RawBuffer_UAV";
    }
    return "srrhi::ResourceType::TypedBuffer_SRV";  // unreachable fallback
}

// ---------------------------------------------------------------------------
// Map a ResourceKind to the corresponding srrhi::TextureDimension enum string.
// Non-texture kinds (buffers, cbuffers, samplers) map to TextureDimension::None.
// ---------------------------------------------------------------------------
static std::string ResourceKindToTextureDimension(ResourceKind kind)
{
    switch (kind)
    {
        case ResourceKind::Texture1D:
        case ResourceKind::RWTexture1D:              return "srrhi::TextureDimension::Texture1D";
        case ResourceKind::Texture1DArray:
        case ResourceKind::RWTexture1DArray:         return "srrhi::TextureDimension::Texture1DArray";
        case ResourceKind::Texture2D:
        case ResourceKind::RWTexture2D:              return "srrhi::TextureDimension::Texture2D";
        case ResourceKind::Texture2DArray:
        case ResourceKind::RWTexture2DArray:         return "srrhi::TextureDimension::Texture2DArray";
        case ResourceKind::TextureCube:              return "srrhi::TextureDimension::TextureCube";
        case ResourceKind::TextureCubeArray:         return "srrhi::TextureDimension::TextureCubeArray";
        case ResourceKind::Texture2DMS:              return "srrhi::TextureDimension::Texture2DMS";
        case ResourceKind::Texture2DMSArray:         return "srrhi::TextureDimension::Texture2DMSArray";
        case ResourceKind::Texture3D:
        case ResourceKind::RWTexture3D:              return "srrhi::TextureDimension::Texture3D";
        default:                                     return "srrhi::TextureDimension::None";
    }
}

// Returns true if the ResourceKind is a texture type (SRV or UAV).
static bool IsTextureKind(ResourceKind kind)
{
    switch (kind)
    {
        case ResourceKind::Texture1D:
        case ResourceKind::Texture1DArray:
        case ResourceKind::Texture2D:
        case ResourceKind::Texture2DArray:
        case ResourceKind::Texture2DMS:
        case ResourceKind::Texture2DMSArray:
        case ResourceKind::Texture3D:
        case ResourceKind::TextureCube:
        case ResourceKind::TextureCubeArray:
        case ResourceKind::RWTexture1D:
        case ResourceKind::RWTexture1DArray:
        case ResourceKind::RWTexture2D:
        case ResourceKind::RWTexture2DArray:
        case ResourceKind::RWTexture3D:             return true;
        default:                                    return false;
    }
}

// Returns true if the ResourceKind is an array texture type.
// Array textures support baseArraySlice/numArraySlices parameters.
static bool IsArrayTextureKind(ResourceKind kind)
{
    return kind == ResourceKind::Texture1DArray
        || kind == ResourceKind::Texture2DArray
        || kind == ResourceKind::Texture2DMSArray
        || kind == ResourceKind::TextureCubeArray
        || kind == ResourceKind::RWTexture1DArray
        || kind == ResourceKind::RWTexture2DArray;
}

// ---------------------------------------------------------------------------
// C++ type mapping from BuiltinType
// ---------------------------------------------------------------------------
struct CppTypeInfo { std::string m_TypeName; int m_ArrayMult = 0; };

static CppTypeInfo MapBuiltinToCpp(const BuiltinType& bt)
{
    const std::string& sc = bt.m_ScalarName;
    int vs = bt.m_VectorSize;

    // Scalar
    if (vs == 1)
    {
        if (sc == "float" || sc == "float32_t")  return {"float"};
        if (sc == "float16_t")  return {"uint16_t"};   // closest 2-byte C type
        if (sc == "float64_t" || sc == "double") return {"double"};
        if (sc == "int"  || sc == "int32_t")  return {"int32_t"};
        if (sc == "uint" || sc == "uint32_t") return {"uint32_t"};
        if (sc == "int16_t")   return {"int16_t"};
        if (sc == "uint16_t")  return {"uint16_t"};
        if (sc == "int64_t")   return {"int64_t"};
        if (sc == "uint64_t")  return {"uint64_t"};
        if (sc == "bool")      return {"BOOL"};
        return {"float"};
    }

    // float vectors ? use DirectXMath when available
    if (sc == "float" || sc == "float32_t")
    {
        if (vs == 2) return {"DirectX::XMFLOAT2"};
        if (vs == 3) return {"DirectX::XMFLOAT3"};
        if (vs == 4) return {"DirectX::XMFLOAT4"};
    }
    if (sc == "int" || sc == "int32_t")
    {
        if (vs == 2) return {"DirectX::XMINT2"};
        if (vs == 3) return {"DirectX::XMINT3"};
        if (vs == 4) return {"DirectX::XMINT4"};
    }
    if (sc == "uint" || sc == "uint32_t")
    {
        if (vs == 2) return {"DirectX::XMUINT2"};
        if (vs == 3) return {"DirectX::XMUINT3"};
        if (vs == 4) return {"DirectX::XMUINT4"};
    }

    // Fallback: scalar array
    CppTypeInfo base = MapBuiltinToCpp(BuiltinType{bt.m_ScalarName, bt.m_ScalarName,
                                                    bt.m_ElementSize, bt.m_Alignment, 1});
    base.m_ArrayMult = vs;
    return base;
}

// ---------------------------------------------------------------------------
// Map float matrices to DirectX matrix types when applicable
// Returns empty string if size doesn't match (use byte array instead)
// Params: arr - the array node representing the matrix
//         computedSize - the size from the layout engine (0 = skip size check for named structs)
// ---------------------------------------------------------------------------
static std::string MapMatrixType(const ArrayNode& arr, int computedSize)
{
    // Check if this is a float4x4 (or similar standard DirectX matrix)
    if (!arr.m_bCreatedFromMatrix)
        return "";

    if (auto* elemBt = std::get_if<BuiltinType>(&arr.m_ElementType))
    {
        const std::string& sc = elemBt->m_ScalarName;
        int rows = elemBt->m_VectorSize;      // element type vector size = rows
        int cols = arr.m_ArraySize;            // array size = columns

        // Check for float-based matrices
        if ((sc == "float" || sc == "float32_t"))
        {
            // Define expected sizes for DirectX matrix types
            int expectedSize = 0;
            std::string matrixType;
            
            if (rows == 3 && cols == 3) { expectedSize = 36; matrixType = "DirectX::XMFLOAT3X3"; }
            else if (rows == 4 && cols == 3) { expectedSize = 48; matrixType = "DirectX::XMFLOAT4X3"; }
            else if (rows == 3 && cols == 4) { expectedSize = 48; matrixType = "DirectX::XMFLOAT3X4"; }
            else if (rows == 4 && cols == 4) { expectedSize = 64; matrixType = "DirectX::XMFLOAT4X4"; }
            
            // For cbuffer context (computedSize != 0): only use DirectX type if size matches
            // For named struct context (computedSize == 0): always use DirectX type if rows/cols match
            if (expectedSize > 0)
            {
                if (computedSize == 0 || computedSize == expectedSize)
                    return matrixType;
            }
        }
    }
    return "";
}



// Returns true if the C++ type should be passed by value (scalar types)
static bool IsCppScalarPassByValue(const std::string& typeName)
{
    static const std::unordered_set<std::string> scalars = {
        "float", "double", "int32_t", "uint32_t", "int16_t", "uint16_t",
        "int64_t", "uint64_t", "BOOL", "bool"
    };
    return scalars.count(typeName) > 0;
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Returns true if the layout member (a struct field) has any padding anywhere
// in its submember tree — either trailing padding on the struct itself, or
// any internal padding between its submembers.
// When a struct has no padding, its C++ layout matches the HLSL cbuffer layout
// exactly and we can emit it as the typed struct instead of a byte array.
// ---------------------------------------------------------------------------
static bool StructLayoutHasPadding(const LayoutMember& lm)
{
    // Trailing padding after the struct itself
    if (lm.m_Padding > 0)
        return true;
    // Internal padding: check gaps between consecutive submembers
    for (size_t i = 0; i < lm.m_Submembers.size(); ++i)
    {
        const LayoutMember& sub = lm.m_Submembers[i];
        if (sub.m_Padding > 0)
            return true;
        // Recurse into nested structs
        if (std::holds_alternative<StructType*>(sub.m_Type))
            if (StructLayoutHasPadding(sub))
                return true;
    }
    return false;
}

// SetterInfo: describes how to emit a public setter for one member
// ---------------------------------------------------------------------------
struct SetterInfo
{
    std::string m_CleanedName;    // private member name (capitalized, no prefix)
    std::string m_CppType;        // C++ param type  (empty when byte array)
    bool        m_bByValue        = false; // true → pass by value; false → const ref
    bool        m_bIsByteArray    = false; // true → member is uint8_t[N]
    int         m_ByteArraySize   = 0;
    std::string m_StructTypeName; // set when byte array originated from a struct field
};

static std::vector<SetterInfo> CollectSetterInfos(
    const std::vector<MemberVariable>& members,
    const std::vector<LayoutMember>& lms)
{
    std::vector<SetterInfo> result;
    for (size_t i = 0; i < members.size(); ++i)
    {
        const MemberVariable& mv = members[i];
        const LayoutMember*   lm = (i < lms.size()) ? &lms[i] : nullptr;
        if (!lm) continue;

        SetterInfo si;
        si.m_CleanedName = CleanMemberName(mv.m_Name);

        if (auto* sp = std::get_if<StructType*>(&mv.m_Type))
        {
            if (!StructLayoutHasPadding(*lm))
            {
                // No padding: use the typed struct directly (const ref setter)
                si.m_CppType  = "srrhi::" + (*sp)->m_Name;
                si.m_bByValue = false;
            }
            else
            {
                // Padding present: fall back to byte array + memcpy setter
                si.m_bIsByteArray   = true;
                si.m_ByteArraySize  = lm->m_Size;
                si.m_StructTypeName = (*sp)->m_Name;
            }
        }
        else if (std::holds_alternative<std::shared_ptr<ArrayNode>>(lm->m_Type))
        {
            const ArrayNode& arr = *std::get<std::shared_ptr<ArrayNode>>(lm->m_Type);
            if (arr.m_bCreatedFromMatrix)
            {
                std::string matType = MapMatrixType(arr, lm->m_Size);
                if (!matType.empty())
                {
                    si.m_CppType  = matType;
                    si.m_bByValue = false; // const ref for DirectX matrix types
                }
                else
                {
                    si.m_bIsByteArray  = true;
                    si.m_ByteArraySize = lm->m_Size;
                }
            }
            else
            {
                si.m_bIsByteArray  = true;
                si.m_ByteArraySize = lm->m_Size;
            }
        }
        else if (auto* bt = std::get_if<BuiltinType>(&lm->m_Type))
        {
            auto cti = MapBuiltinToCpp(*bt);
            if (cti.m_ArrayMult > 0)
            {
                // Unusual vector fallback → treat as byte array
                si.m_bIsByteArray  = true;
                si.m_ByteArraySize = lm->m_Size;
            }
            else
            {
                si.m_CppType  = cti.m_TypeName;
                si.m_bByValue = IsCppScalarPassByValue(cti.m_TypeName);
            }
        }

        result.push_back(std::move(si));
    }
    return result;
}

// ---------------------------------------------------------------------------
static void EmitPadding(std::ostringstream& out, int padBytes, int& padCount,
                        const std::string& ind)
{
    // Emit padding fields to maintain std140 cbuffer layout alignment.
    // std140 has specific alignment rules: scalars align to 4 bytes, vectors to 16 bytes,
    // arrays to their element's packed size with additional rounding, etc.
    // These padding fields ensure the struct layout matches the HLSL cbuffer exactly.
    if (padBytes <= 0) return;
    while (padBytes >= 4)
    {
        out << ind << "uint32_t pad" << padCount++ << ";  // std140 alignment padding\n";
        padBytes -= 4;
    }
    if (padBytes == 2)
        out << ind << "uint16_t pad" << padCount++ << ";  // std140 alignment padding\n";
    else if (padBytes != 0)
        out << ind << "// WARNING: " << padBytes << " unaccounted byte(s)\n";
}

// ---------------------------------------------------------------------------
// Emit struct / cbuffer members (C++)
//   - Walks members paired with their layout submembers.
//   - Array fields are EXPANDED to per-element members with stride padding.
//   - Matrix fields (m_bCreatedFromMatrix arrays) are emitted column-by-column.
//   - bCleanNames: strip prefixes and capitalize first letter for each member name.
// ---------------------------------------------------------------------------

static void EmitMembersCpp(std::ostringstream& out,
                            const std::vector<MemberVariable>& members,
                            const std::vector<LayoutMember>& lms,
                            int& padCount, const std::string& ind,
                            bool bCleanNames = false)
{
    int cursor = 0;

    for (size_t i = 0; i < members.size(); ++i)
    {
        const MemberVariable& mv = members[i];
        const LayoutMember* lm   = (i < lms.size()) ? &lms[i] : nullptr;

        // Determine field offset for pre-field padding
        int fieldOffset = lm ? lm->m_Offset : cursor;
        if (fieldOffset > cursor)
            EmitPadding(out, fieldOffset - cursor, padCount, ind);

        const std::string fieldName = bCleanNames ? CleanMemberName(mv.m_Name) : mv.m_Name;

        // === Struct field ===
        if (auto* sp = std::get_if<StructType*>(&mv.m_Type))
        {
            if (!lm) { cursor = fieldOffset; continue; }

            if (!StructLayoutHasPadding(*lm))
            {
                // No padding anywhere in this struct's layout: its C++ layout matches
                // the HLSL cbuffer layout exactly, so emit it as the typed struct.
                out << ind << "srrhi::" << (*sp)->m_Name << " " << fieldName << ";\n";
            }
            else
            {
                // Emit struct fields as byte arrays to match std140 cbuffer layout rules.
                // std140 layout adds implicit padding within or after this struct member;
                // the computed size includes this padding. Using a byte array guarantees
                // the layout matches the HLSL cbuffer without relying on C++ struct alignment.
                // To initialize: create a real instance and use std::memcpy() to copy bytes.
                out << ind << "uint8_t " << fieldName << "[" << lm->m_Size << "];";
                out << "  // byte array: std140 adds padding inside/after '" << (*sp)->m_Name << "'\n";
            }
            if (lm->m_Padding > 0)
                EmitPadding(out, lm->m_Padding, padCount, ind);
            cursor = lm->m_Offset + lm->m_Size + lm->m_Padding;
            continue;
        }

        // === BuiltinType or ArrayNode field ===
        if (!lm) { cursor = fieldOffset; continue; }

        bool bIsArray = std::holds_alternative<std::shared_ptr<ArrayNode>>(lm->m_Type);

        if (bIsArray)
        {
            const ArrayNode& arr = *std::get<std::shared_ptr<ArrayNode>>(lm->m_Type);

            if (arr.m_bCreatedFromMatrix)
            {
                // Try to map to DirectX matrix type
                std::string matrixType = MapMatrixType(arr, lm->m_Size);
                
                if (!matrixType.empty())
                {
                    // Use DirectX matrix type directly
                    out << ind << matrixType << " " << fieldName << ";\n";
                    if (lm->m_Padding > 0)
                        EmitPadding(out, lm->m_Padding, padCount, ind);
                    cursor = lm->m_Offset + lm->m_Size + lm->m_Padding;
                }
                else
                {
                    // Matrix: emit as byte array to match std140 layout.
                    // Non-standard matrix dimensions (e.g., 60-byte matrices) don't map to
                    // DirectX::XMFLOAT types. This byte array ensures the layout matches exactly.
                    // To initialize: create a real matrix type and use std::memcpy() to copy bytes.
                    out << ind << "uint8_t " << fieldName << "[" << lm->m_Size << "];  // std140 matrix padding\n";
                    if (lm->m_Padding > 0)
                        EmitPadding(out, lm->m_Padding, padCount, ind);
                    cursor = lm->m_Offset + lm->m_Size + lm->m_Padding;
                }
            }
            else
            {
                // Regular array: emit as byte array to match std140 layout.
                // In std140, array elements are padded as if each is a struct member.
                // For example, a float[4] takes 64 bytes (each element at 16-byte boundary),
                // not 16 bytes. Using a byte array preserves the exact computed layout.
                // To initialize: create a real array and use std::memcpy() to copy bytes.
                out << ind << "uint8_t " << fieldName << "[" << lm->m_Size << "];  // std140 array padding\n";
                if (lm->m_Padding > 0)
                    EmitPadding(out, lm->m_Padding, padCount, ind);
                cursor = lm->m_Offset + lm->m_Size + lm->m_Padding;
            }
        }
        else
        {
            // Single non-array BuiltinType field
            const BuiltinType* bt = std::get_if<BuiltinType>(&lm->m_Type);
            if (bt)
            {
                auto cti = MapBuiltinToCpp(*bt);
                out << ind << cti.m_TypeName << " " << fieldName;
                if (cti.m_ArrayMult > 0) out << "[" << cti.m_ArrayMult << "]";
                out << ";\n";
            }
            if (lm->m_Padding > 0)
                EmitPadding(out, lm->m_Padding, padCount, ind);
            cursor = lm->m_Offset + lm->m_Size + lm->m_Padding;
        }
    }
}

// ---------------------------------------------------------------------------
// Emit a cbuffer struct as a C++ class with private members + public setters
// ---------------------------------------------------------------------------
static void EmitClassCpp(std::ostringstream& out, const StructType& st,
                          const LayoutMember& layout, int& padCount,
                          bool bEmitValidation)
{
    // Collect setter infos before emitting (determines parameter types)
    auto setterInfos = CollectSetterInfos(st.m_Members, layout.m_Submembers);

    bool bNeedsMemcpy = false;
    for (const auto& si : setterInfos)
        if (si.m_bIsByteArray) { bNeedsMemcpy = true; break; }

    if (bEmitValidation)
        out << "class alignas(16) " << st.m_Name << "\n{\nfriend struct " << st.m_Name
            << "Validator;\nprivate:\n";
    else
        out << "class alignas(16) " << st.m_Name << "\n{\nprivate:\n";

    int localPadCount = padCount;
    EmitMembersCpp(out, st.m_Members, layout.m_Submembers, localPadCount, "    ",
                   /*bCleanNames=*/true);

    out << "\npublic:\n";

    for (const auto& si : setterInfos)
    {
        const std::string setterName = "Set" + si.m_CleanedName;

        if (si.m_bIsByteArray)
        {
            if (!si.m_StructTypeName.empty())
            {
                // Struct field → take const ref of the original struct type.
                // Use srrhi scope to correctly reference types in the namespace.
                out << "    void " << setterName << "(const srrhi::" << si.m_StructTypeName << "& value)"
                    << " { std::memcpy(" << si.m_CleanedName << ", &value, "
                    << si.m_ByteArraySize << "); }\n";
            }
            else
            {
                // Array/matrix byte array → raw pointer
                out << "    void " << setterName << "(const void* pData)"
                    << " { std::memcpy(" << si.m_CleanedName << ", pData, "
                    << si.m_ByteArraySize << "); }\n";
            }
        }
        else if (si.m_bByValue)
        {
            out << "    void " << setterName << "(" << si.m_CppType << " value)"
                << " { " << si.m_CleanedName << " = value; }\n";
        }
        else
        {
            out << "    void " << setterName << "(const " << si.m_CppType << "& value)"
                << " { " << si.m_CleanedName << " = value; }\n";
        }
    }

    if (bEmitValidation)
    {
        out << "\n    // Validation-only: returns raw cbuffer bytes for offset/value checks in tests.\n";
        out << "    const uint8_t* GetRawBytes() const { return reinterpret_cast<const uint8_t*>(this); }\n";
    }

    out << "};\n\n";
    if (bEmitValidation)
    {
        out << "// Friend validator struct for compile-time offset validation\n";
        out << "struct " << st.m_Name << "Validator {\n";
        for (const auto& localMem : layout.m_Submembers)
        {
            out << "    static_assert(offsetof(" << st.m_Name << ", " << CleanMemberName(localMem.m_Name)
                << ") == " << localMem.m_Offset << ", \""
                << st.m_Name << "::" << CleanMemberName(localMem.m_Name) << " offset check\");\n";
        }
        out << "};\n\n";
    }
    (void)bNeedsMemcpy; // <cstring> included unconditionally when classes exist
}

// ---------------------------------------------------------------------------
// Emit a named struct definition (for use in the C++ header)
// ---------------------------------------------------------------------------
static void EmitStructCpp(std::ostringstream& out, const StructType& st,
                           int& padCount)
{
    out << "struct " << st.m_Name << "\n{\n";
    // Structs without layout info: emit fields in order without padding
    // (padding is added in the cbuffer context)
    std::string fInd("    ");
    for (const auto& mv : st.m_Members)
    {
        if (auto* sp = std::get_if<StructType*>(&mv.m_Type))
            out << fInd << (*sp)->m_Name << " " << mv.m_Name << ";\n";
        else if (auto* bt = std::get_if<BuiltinType>(&mv.m_Type))
        {
            auto cti = MapBuiltinToCpp(*bt);
            out << fInd << cti.m_TypeName << " " << mv.m_Name;
            if (cti.m_ArrayMult > 0) out << "[" << cti.m_ArrayMult << "]";
            out << ";\n";
        }
        else if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&mv.m_Type))
        {
            const ArrayNode& arr = **ap;
            if (arr.m_bCreatedFromMatrix)
            {
                // Try to map to DirectX matrix type
                // Note: For named structs, we don't have computed layout sizes, so pass 0 (no size verification)
                std::string matrixType = MapMatrixType(arr, 0);
                
                if (!matrixType.empty())
                {
                    // Use DirectX matrix type directly
                    out << fInd << matrixType << " " << mv.m_Name << ";\n";
                }
                else
                {
                    // Matrix: emit as array of column vectors for non-standard matrices
                    if (auto* elemBt = std::get_if<BuiltinType>(&arr.m_ElementType))
                    {
                        auto cti = MapBuiltinToCpp(*elemBt);
                        out << fInd << cti.m_TypeName << " " << mv.m_Name << "[" << arr.m_ArraySize << "];\n";
                    }
                }
            }
            else
            {
                if (auto* elemBt = std::get_if<BuiltinType>(&arr.m_ElementType))
                {
                    auto cti = MapBuiltinToCpp(*elemBt);
                    out << fInd << cti.m_TypeName << " " << mv.m_Name << "[" << arr.m_ArraySize << "];\n";;
                }
                else if (auto* elemSp = std::get_if<StructType*>(&arr.m_ElementType))
                {
                    out << fInd << (*elemSp)->m_Name << " " << mv.m_Name << "[" << arr.m_ArraySize << "];\n";
                }
            }
        }
    }
    out << "};\n\n";
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::string GenerateCpp(const ParseResult& pr,
                         const std::vector<LayoutMember>& layouts,
                         int& padCount, bool bEmitValidation)
{
    VerboseMsg("[cpp_gen] Generating C++ header...\n");

    std::ostringstream out;
    out << "// Auto-generated by srrhi. Do not edit.\n";
    out << "#pragma once\n";
    out << "#include <cstdint>\n";

    // Include <cstring> for std::memcpy used in byte-array setters
    if (!pr.m_SrInputDefs.empty())
        out << "#include <cstring>\n";

    // Include srrhi.h for ResourceEntry and ResourceType (resource binding API).
    // Needed when any srinput (including via composition) has cbuffer refs, SRV/UAV resources, or samplers.
    {
        bool bNeedsResourceEntries = false;
        for (const auto& srInputDef : pr.m_SrInputDefs)
        {
            FlatSrInput flat = FlattenSrInput(srInputDef, pr.m_SrInputDefs);
            if (!flat.m_Members.empty() || !flat.m_Resources.empty() || !flat.m_Samplers.empty())
            {
                bNeedsResourceEntries = true;
                break;
            }
        }
        if (bNeedsResourceEntries)
            out << "#include \"srrhi.h\"\n";
    }

    // Emit #include directives for each directly included .sr file.
    // This avoids re-emitting struct definitions already present in those files.
    for (const auto& incFile : pr.m_DirectIncludes)
    {
        // Derive the .h filename: replace the .sr extension with .h
        std::string hName = incFile;
        const std::string srExt = ".sr";
        if (hName.size() > srExt.size() &&
            hName.substr(hName.size() - srExt.size()) == srExt)
        {
            hName = hName.substr(0, hName.size() - srExt.size()) + ".h";
        }
        out << "#include \"" << hName << "\"\n";
    }

    out << "\n";
    out << "namespace srrhi\n{\n\n";

    // Named struct definitions (remain as plain structs) — skip those from includes.
    for (const auto& st : pr.m_Structs)
    {
        if (pr.m_IncludedStructNames.count(st.m_Name)) continue;
        int localPadCount = 0;
        EmitStructCpp(out, st, localPadCount);
    }

    // Build a set of cbuffer names that are in srinput scopes (including transitive composition)
    std::unordered_set<std::string> cbuffersInSrInput;
    for (const auto& srInputDef : pr.m_SrInputDefs)
    {
        FlatSrInput flat = FlattenSrInput(srInputDef, pr.m_SrInputDefs);
        for (const auto& member : flat.m_Members)
            cbuffersInSrInput.insert(member.m_CBufferName);
    }

    // Emit cbuffer structs as classes with private members + public setters
    // (only for those referenced in srinput scopes)
    for (const auto& bufDef : pr.m_BufferDefs)
    {
        if (cbuffersInSrInput.count(bufDef.m_Name))
        {
            const LayoutMember* bufLayout = nullptr;
            for (const auto& lm : layouts)
            {
                if (lm.m_Name == bufDef.m_Name)
                {
                    bufLayout = &lm;
                    break;
                }
            }

            if (bufLayout)
            {
                int localPadCount = 0;
                EmitClassCpp(out, bufDef, *bufLayout, localPadCount, bEmitValidation);
            }
        }
    }

    // Emit per-srinput classes with NumCBuffers/NumSRVs/NumUAVs/NumSamplers + per-resource register index constants.
    // CBuffer, SRV, UAV, and sampler register numbers are all local to each srinput scope (reset per scope).
    // Composition: the flattened view is used so nested srinput resources are inlined with unique register indices.
    for (const auto& srInputDef : pr.m_SrInputDefs)
    {
        FlatSrInput flat = FlattenSrInput(srInputDef, pr.m_SrInputDefs);

        // Count SRVs and UAVs for this (flattened) srinput scope
        uint32_t numSRVs = 0;
        uint32_t numUAVs = 0;
        for (const auto& rm : flat.m_Resources)
        {
            if (IsUAV(rm.m_Kind)) ++numUAVs;
            else                  ++numSRVs;
        }
        uint32_t numSamplers = static_cast<uint32_t>(flat.m_Samplers.size());

        out << "struct " << srInputDef.m_Name << "\n{\n";

        // CBuffer counts and register indices (b# registers, local per scope).
        // All cbuffers (push constant or regular) use a sequential counter starting at 0.
        // Push constants must be declared first in the srinput, so they always land on b0.
        out << "    static constexpr uint32_t NumCBuffers = "
            << flat.m_Members.size() << ";\n";
        {
            uint32_t cbufReg = 0;
            for (const auto& member : flat.m_Members)
            {
                const std::string constName = CleanMemberName(member.m_MemberName) + "RegisterIndex";
                out << "    static constexpr uint32_t " << constName << " = "
                    << cbufReg << ";  // b" << cbufReg;
                if (member.m_bIsPushConstant) out << " (push constant)";
                out << "\n";
                ++cbufReg;
            }
        }

        // SRV counts and register indices (t# registers, local per scope)
        out << "    static constexpr uint32_t NumSRVs = " << numSRVs << ";\n";
        {
            uint32_t srvReg = 0;
            for (const auto& rm : flat.m_Resources)
            {
                if (!IsUAV(rm.m_Kind))
                {
                    const std::string constName = CleanMemberName(rm.m_MemberName) + "RegisterIndex";
                    out << "    static constexpr uint32_t " << constName << " = "
                        << srvReg++ << ";  // t" << (srvReg - 1) << "\n";
                }
            }
        }

        // UAV counts and register indices (u# registers, local per scope)
        out << "    static constexpr uint32_t NumUAVs = " << numUAVs << ";\n";
        {
            uint32_t uavReg = 0;
            for (const auto& rm : flat.m_Resources)
            {
                if (IsUAV(rm.m_Kind))
                {
                    const std::string constName = CleanMemberName(rm.m_MemberName) + "RegisterIndex";
                    out << "    static constexpr uint32_t " << constName << " = "
                        << uavReg++ << ";  // u" << (uavReg - 1) << "\n";
                }
            }
        }

        // Sampler counts and register indices (s# registers, local per scope)
        out << "    static constexpr uint32_t NumSamplers = " << numSamplers << ";\n";
        {
            uint32_t samplerReg = 0;
            for (const auto& sm : flat.m_Samplers)
            {
                const std::string constName = CleanMemberName(sm.m_MemberName) + "RegisterIndex";
                out << "    static constexpr uint32_t " << constName << " = "
                    << samplerReg++ << ";  // s" << (samplerReg - 1) << "\n";
            }
        }

        // Scalar constants declared in this srinput (including from nested srinputs via composition)
        for (const auto& sc : flat.m_ScalarConsts)
        {
            // Map HLSL scalar type to C++ type for constexpr
            std::string cppType = sc.m_TypeName;
            if (cppType == "float" || cppType == "float32_t") cppType = "float";
            else if (cppType == "double" || cppType == "float64_t") cppType = "double";
            else if (cppType == "float16_t") cppType = "uint16_t";  // closest 2-byte C type
            else if (cppType == "int" || cppType == "int32_t") cppType = "int32_t";
            else if (cppType == "uint" || cppType == "uint32_t") cppType = "uint32_t";
            else if (cppType == "int16_t") cppType = "int16_t";
            else if (cppType == "uint16_t") cppType = "uint16_t";
            else if (cppType == "int64_t") cppType = "int64_t";
            else if (cppType == "uint64_t") cppType = "uint64_t";
            else if (cppType == "bool") cppType = "bool";
            out << "    static constexpr " << cppType << " " << sc.m_Name
                << " = " << sc.m_Value << ";\n";
        }

        // Total resource count: CBuffers + SRVs + UAVs + Samplers
        const uint32_t numCBuffers = static_cast<uint32_t>(flat.m_Members.size());
        const uint32_t numResources = numCBuffers + numSRVs + numUAVs + numSamplers;
        out << "    static constexpr uint32_t NumResources = NumCBuffers + NumSRVs + NumUAVs + NumSamplers;\n";

        // Register space: always taken from the top-level srinput (not nested srinputs)
        out << "    static constexpr uint32_t RegisterSpace = " << std::max(0, srInputDef.m_RegisterSpace) << ";\n";

        // PushConstantBytes: size of the push constant struct (if any), or 0 if none
        {
            uint32_t pushConstantBytes = 0;
            for (const auto& member : flat.m_Members)
            {
                if (member.m_bIsPushConstant)
                {
                    // Find the layout for this cbuffer to get its size
                    for (const auto& lm : layouts)
                    {
                        if (lm.m_Name == member.m_CBufferName)
                        {
                            pushConstantBytes = lm.m_Size;
                            break;
                        }
                    }
                    break;  // Only one push constant allowed per srinput
                }
            }
            out << "    static constexpr uint32_t PushConstantBytes = " << pushConstantBytes << ";\n";
        }

        // Compose cbuffer structs directly as member variables (inlined from nested srinputs too)
        if (!flat.m_Members.empty())
        {
            out << "\n";
            for (const auto& member : flat.m_Members)
            {
                out << "    " << member.m_CBufferName << " " << member.m_MemberName << ";\n";
            }
        }

        // Resource entry array and per-resource setter functions.
        // Array ordering: CBuffers [0..N-1], SRVs [N..N+S-1], UAVs [N+S..N+S+U-1], Samplers [last].
        if (numResources > 0)
        {
            out << "\n";
            out << "    // Flat array of all resource binding entries for this srinput scope.\n";
            out << "    // 'slot' and 'type' are compile-time constants; 'pResource' is set at runtime.\n";
            out << "    srrhi::ResourceEntry m_Resources[NumResources] = {\n";

            // -- CBuffer entries --
            if (!flat.m_Members.empty())
            {
                out << "        // ConstantBuffers (b# registers)\n";
                for (const auto& member : flat.m_Members)
                {
                    const std::string constName = CleanMemberName(member.m_MemberName) + "RegisterIndex";
                    const std::string resType = member.m_bIsPushConstant
                        ? "srrhi::ResourceType::PushConstants"
                        : "srrhi::ResourceType::ConstantBuffer";
                    out << "        { nullptr, " << constName
                        << ", " << resType << ", srrhi::TextureDimension::None },\n";
                }
            }

            // -- SRV entries --
            bool firstSrv = true;
            for (const auto& rm : flat.m_Resources)
            {
                if (!IsUAV(rm.m_Kind))
                {
                    if (firstSrv) { out << "        // SRVs (t# registers)\n"; firstSrv = false; }
                    const std::string constName = CleanMemberName(rm.m_MemberName) + "RegisterIndex";
                    out << "        { nullptr, " << constName
                        << ", " << ResourceKindToSrrResourceType(rm.m_Kind)
                        << ", " << ResourceKindToTextureDimension(rm.m_Kind) << " },\n";
                }
            }

            // -- UAV entries --
            bool firstUav = true;
            for (const auto& rm : flat.m_Resources)
            {
                if (IsUAV(rm.m_Kind))
                {
                    if (firstUav) { out << "        // UAVs (u# registers)\n"; firstUav = false; }
                    const std::string constName = CleanMemberName(rm.m_MemberName) + "RegisterIndex";
                    out << "        { nullptr, " << constName
                        << ", " << ResourceKindToSrrResourceType(rm.m_Kind)
                        << ", " << ResourceKindToTextureDimension(rm.m_Kind) << " },\n";
                }
            }

            // -- Sampler entries --
            if (!flat.m_Samplers.empty())
            {
                out << "        // Samplers (s# registers)\n";
                for (const auto& sm : flat.m_Samplers)
                {
                    const std::string constName = CleanMemberName(sm.m_MemberName) + "RegisterIndex";
                    out << "        { nullptr, " << constName
                        << ", srrhi::ResourceType::Sampler, srrhi::TextureDimension::None },\n";
                }
            }

            out << "    };\n";

            // -- Per-resource setter functions --
            out << "\n";
            uint32_t resourceIdx = 0;

            // CBuffer setters: simple void* overload only
            for (const auto& member : flat.m_Members)
            {
                const std::string setterName = "Set" + CleanMemberName(member.m_MemberName);
                if (member.m_bIsPushConstant)
                {
                    // Push constant setter: stores a pointer to the push constant bytes.
                    // NOTE: This stores only a pointer — make sure the pointed-to data
                    // does not go out of scope before the GPU finishes using it.
                    out << "    void " << setterName << "(void* pushConstantsBytes)"
                        << " { m_Resources[" << resourceIdx << "].pResource = pushConstantsBytes; }\n";
                }
                else
                {
                    out << "    void " << setterName << "(void* pResource)"
                        << " { m_Resources[" << resourceIdx << "].pResource = pResource; }\n";
                }
                ++resourceIdx;
            }

            // SRV setters
            for (const auto& rm : flat.m_Resources)
            {
                if (!IsUAV(rm.m_Kind))
                {
                    const std::string setterName = "Set" + CleanMemberName(rm.m_MemberName);
                    if (IsTextureKind(rm.m_Kind))
                    {
                        // Texture_SRV: simple overload (uses default mip/slice values).
                        out << "    void " << setterName << "(void* pResource)"
                            << " { m_Resources[" << resourceIdx << "].pResource = pResource; }\n";

                        if (IsArrayTextureKind(rm.m_Kind))
                        {
                            // Array Texture_SRV: full 5-param overload for mip + array-slice control.
                            out << "    void " << setterName
                                << "(void* pResource, int32_t baseMipLevel, int32_t numMipLevels,"
                                   " int32_t baseArraySlice, int32_t numArraySlices)\n    {\n";
                            out << "        m_Resources[" << resourceIdx << "].pResource      = pResource;\n";
                            out << "        m_Resources[" << resourceIdx << "].baseMipLevel   = baseMipLevel;\n";
                            out << "        m_Resources[" << resourceIdx << "].numMipLevels   = numMipLevels;\n";
                            out << "        m_Resources[" << resourceIdx << "].baseArraySlice = baseArraySlice;\n";
                            out << "        m_Resources[" << resourceIdx << "].numArraySlices = numArraySlices;\n";
                            out << "    }\n";
                        }
                        else
                        {
                            // Non-array Texture_SRV: 3-param overload for mip-range control.
                            out << "    void " << setterName
                                << "(void* pResource, int32_t baseMipLevel, int32_t numMipLevels)\n    {\n";
                            out << "        m_Resources[" << resourceIdx << "].pResource    = pResource;\n";
                            out << "        m_Resources[" << resourceIdx << "].baseMipLevel = baseMipLevel;\n";
                            out << "        m_Resources[" << resourceIdx << "].numMipLevels = numMipLevels;\n";
                            out << "    }\n";
                        }
                    }
                    else
                    {
                        // Non-texture SRV (typed/structured/raw buffer, RTAS): simple void* overload only.
                        out << "    void " << setterName << "(void* pResource)"
                            << " { m_Resources[" << resourceIdx << "].pResource = pResource; }\n";
                    }
                    ++resourceIdx;
                }
            }

            // UAV setters
            for (const auto& rm : flat.m_Resources)
            {
                if (IsUAV(rm.m_Kind))
                {
                    const std::string setterName = "Set" + CleanMemberName(rm.m_MemberName);
                    if (IsTextureKind(rm.m_Kind))
                    {
                        if (IsArrayTextureKind(rm.m_Kind))
                        {
                            // Array Texture_UAV: 4-param overload (baseMip + array-slice control).
                            out << "    void " << setterName
                                << "(void* pResource, int32_t baseMipLevel,"
                                   " int32_t baseArraySlice, int32_t numArraySlices)\n    {\n";
                            out << "        m_Resources[" << resourceIdx << "].pResource      = pResource;\n";
                            out << "        m_Resources[" << resourceIdx << "].baseMipLevel   = baseMipLevel;\n";
                            out << "        m_Resources[" << resourceIdx << "].numMipLevels   = 1;\n";
                            out << "        m_Resources[" << resourceIdx << "].baseArraySlice = baseArraySlice;\n";
                            out << "        m_Resources[" << resourceIdx << "].numArraySlices = numArraySlices;\n";
                            out << "    }\n";
                        }
                        else
                        {
                            // Non-array Texture_UAV: 2-param overload (baseMip only).
                            out << "    void " << setterName
                                << "(void* pResource, int32_t baseMipLevel)\n    {\n";
                            out << "        m_Resources[" << resourceIdx << "].pResource    = pResource;\n";
                            out << "        m_Resources[" << resourceIdx << "].baseMipLevel = baseMipLevel;\n";
                            out << "        m_Resources[" << resourceIdx << "].numMipLevels = 1;\n";
                            out << "    }\n";
                        }
                    }
                    else
                    {
                        // Non-texture UAV (typed/structured/raw buffer): simple void* overload only.
                        out << "    void " << setterName << "(void* pResource)"
                            << " { m_Resources[" << resourceIdx << "].pResource = pResource; }\n";
                    }
                    ++resourceIdx;
                }
            }

            // Sampler setters: simple void* overload only
            for (const auto& sm : flat.m_Samplers)
            {
                const std::string setterName = "Set" + CleanMemberName(sm.m_MemberName);
                out << "    void " << setterName << "(void* pResource)"
                    << " { m_Resources[" << resourceIdx << "].pResource = pResource; }\n";
                ++resourceIdx;
            }
        }  // if (numResources > 0)

        out << "};\n\n";

        if (bEmitValidation)
        {
            // Emit static_assert checks so the values are verified at compile time.
            out << "// Compile-time register index checks for " << srInputDef.m_Name << "\n";

            out << "static_assert(" << srInputDef.m_Name << "::NumCBuffers == "
                << flat.m_Members.size() << "u);\n";
            {
                uint32_t cbufReg = 0;
                for (const auto& member : flat.m_Members)
                {
                    const std::string constName = CleanMemberName(member.m_MemberName) + "RegisterIndex";
                    out << "static_assert(" << srInputDef.m_Name << "::" << constName
                        << " == " << cbufReg++ << "u);";
                    if (member.m_bIsPushConstant) out << "  // push constant, always b0";
                    out << "\n";
                }
            }

            out << "static_assert(" << srInputDef.m_Name << "::NumSRVs == " << numSRVs << "u);\n";
            {
                uint32_t srvReg = 0;
                for (const auto& rm : flat.m_Resources)
                {
                    if (!IsUAV(rm.m_Kind))
                    {
                        const std::string constName = CleanMemberName(rm.m_MemberName) + "RegisterIndex";
                        out << "static_assert(" << srInputDef.m_Name << "::" << constName
                            << " == " << srvReg++ << "u);\n";
                    }
                }
            }

            out << "static_assert(" << srInputDef.m_Name << "::NumUAVs == " << numUAVs << "u);\n";
            {
                uint32_t uavReg = 0;
                for (const auto& rm : flat.m_Resources)
                {
                    if (IsUAV(rm.m_Kind))
                    {
                        const std::string constName = CleanMemberName(rm.m_MemberName) + "RegisterIndex";
                        out << "static_assert(" << srInputDef.m_Name << "::" << constName
                            << " == " << uavReg++ << "u);\n";
                    }
                }
            }

            out << "static_assert(" << srInputDef.m_Name << "::NumSamplers == " << numSamplers << "u);\n";
            {
                uint32_t samplerReg = 0;
                for (const auto& sm : flat.m_Samplers)
                {
                    const std::string constName = CleanMemberName(sm.m_MemberName) + "RegisterIndex";
                    out << "static_assert(" << srInputDef.m_Name << "::" << constName
                        << " == " << samplerReg++ << "u);\n";
                }
            }

            out << "static_assert(" << srInputDef.m_Name << "::NumResources == "
                << numResources << "u);\n";
            out << "static_assert(" << srInputDef.m_Name << "::RegisterSpace == "
                << std::max(0, srInputDef.m_RegisterSpace) << "u);\n";
            out << "\n";
        }
    }

    out << "\n}  // namespace srrhi\n";

    VerboseMsg("[cpp_gen] Done\n");
    return out.str();
}
