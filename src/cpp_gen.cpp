#include "types.h"
#include <sstream>
#include <stdexcept>

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
// ---------------------------------------------------------------------------
static std::string MapMatrixType(const ArrayNode& arr)
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
            // Map to DirectX types
            if (rows == 3 && cols == 3) return "DirectX::XMFLOAT3X3";
            if (rows == 4 && cols == 3) return "DirectX::XMFLOAT4X3";
            if (rows == 3 && cols == 4) return "DirectX::XMFLOAT3X4";
            if (rows == 4 && cols == 4) return "DirectX::XMFLOAT4X4";
        }
    }
    return "";
}

static void EmitPadding(std::ostringstream& out, int padBytes, int& padCount,
                        const std::string& ind)
{
    if (padBytes <= 0) return;
    while (padBytes >= 4)
    {
        out << ind << "uint32_t pad" << padCount++ << ";\n";
        padBytes -= 4;
    }
    if (padBytes == 2)
        out << ind << "uint16_t pad" << padCount++ << ";\n";
    else if (padBytes != 0)
        out << ind << "// WARNING: " << padBytes << " unaccounted byte(s)\n";
}

// ---------------------------------------------------------------------------
// Emit struct / cbuffer members (C++)
//   - Walks members paired with their layout submembers.
//   - Array fields are EXPANDED to per-element members with stride padding.
//   - Matrix fields (m_bCreatedFromMatrix arrays) are emitted column-by-column.
// ---------------------------------------------------------------------------

static void EmitMembersCpp(std::ostringstream& out,
                            const std::vector<MemberVariable>& members,
                            const std::vector<LayoutMember>& lms,
                            int& padCount, const std::string& ind)
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

        // === Struct field ===
        if (auto* sp = std::get_if<StructType*>(&mv.m_Type))
        {
            if (!lm) { cursor = fieldOffset; continue; }

            // Check if this is an array of structs (layout node is an array)
            bool bIsArray = std::holds_alternative<std::shared_ptr<ArrayNode>>(lm->m_Type);
            if (bIsArray)
            {
                // Expand each array element as a separate struct member
                for (size_t e = 0; e < lm->m_Submembers.size(); ++e)
                {
                    const LayoutMember& elem = lm->m_Submembers[e];
                    std::string eName = mv.m_Name + "_" + std::to_string(e);
                    out << ind << (*sp)->m_Name << " " << eName << ";\n";
                    if (elem.m_Padding > 0)
                        EmitPadding(out, elem.m_Padding, padCount, ind);
                    cursor = elem.m_Offset + elem.m_Size + elem.m_Padding;
                }
            }
            else
            {
                // Single struct member
                out << ind << (*sp)->m_Name << " " << mv.m_Name << ";\n";
                if (lm->m_Padding > 0)
                    EmitPadding(out, lm->m_Padding, padCount, ind);
                cursor = lm->m_Offset + lm->m_Size + lm->m_Padding;
            }
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
                std::string matrixType = MapMatrixType(arr);
                
                if (!matrixType.empty())
                {
                    // Use DirectX matrix type directly
                    out << ind << matrixType << " " << mv.m_Name << ";\n";
                    if (lm->m_Padding > 0)
                        EmitPadding(out, lm->m_Padding, padCount, ind);
                    cursor = lm->m_Offset + lm->m_Size + lm->m_Padding;
                }
                else
                {
                    // Matrix: emit column-by-column (for non-standard matrices)
                    const BuiltinType* elemBt = std::get_if<BuiltinType>(&arr.m_ElementType);
                    if (elemBt)
                    {
                        auto cti = MapBuiltinToCpp(*elemBt);
                        int colDataElems = elemBt->m_VectorSize;
                        int elemsPerSlot = 16 / elemBt->m_ElementSize;

                        for (size_t c = 0; c < lm->m_Submembers.size(); ++c)
                        {
                            const LayoutMember& col = lm->m_Submembers[c];
                            bool bLastCol = (c == lm->m_Submembers.size() - 1);
                            std::string colName = mv.m_Name + "_c" + std::to_string(c);

                            if (bLastCol)
                                out << ind << cti.m_TypeName << " " << colName << "[" << colDataElems << "];\n";
                            else
                                out << ind << cti.m_TypeName << " " << colName << "[" << elemsPerSlot << "];\n";

                            if (!bLastCol && col.m_Padding > 0)
                                EmitPadding(out, col.m_Padding, padCount, ind);
                            cursor = col.m_Offset + col.m_Size + (bLastCol ? lm->m_Padding : col.m_Padding);
                        }
                        if (lm->m_Padding > 0)
                            EmitPadding(out, lm->m_Padding, padCount, ind);
                        cursor = lm->m_Offset + lm->m_Size + lm->m_Padding;
                    }
                }
            }
            else
            {
                // Regular array: expand per element
                for (size_t e = 0; e < lm->m_Submembers.size(); ++e)
                {
                    const LayoutMember& elem = lm->m_Submembers[e];
                    bool bLastElem = (e == lm->m_Submembers.size() - 1);

                    if (auto* elemBt = std::get_if<BuiltinType>(&elem.m_Type))
                    {
                        auto cti = MapBuiltinToCpp(*elemBt);
                        std::string eName = mv.m_Name + "_" + std::to_string(e);
                        out << ind << cti.m_TypeName << " " << eName;
                        if (cti.m_ArrayMult > 0)
                            out << "[" << cti.m_ArrayMult << "]";
                        out << ";\n";
                    }
                    else if (auto* elemSp = std::get_if<StructType*>(&elem.m_Type))
                    {
                        std::string eName = mv.m_Name + "_" + std::to_string(e);
                        out << ind << (*elemSp)->m_Name << " " << eName << ";\n";
                    }

                    if (!bLastElem && elem.m_Padding > 0)
                        EmitPadding(out, elem.m_Padding, padCount, ind);
                    cursor = elem.m_Offset + elem.m_Size + (bLastElem ? 0 : elem.m_Padding);
                }
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
                out << ind << cti.m_TypeName << " " << mv.m_Name;
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
                std::string matrixType = MapMatrixType(arr);
                
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
                         int& padCount)
{
    LogMsg("[cpp_gen] Generating C++ header...\n");

    std::ostringstream out;
    out << "// Auto-generated by srrhi. Do not edit.\n";
    out << "#pragma once\n";
    out << "#include <cstdint>\n\n";

    // Named struct definitions
    for (const auto& st : pr.m_Structs)
        EmitStructCpp(out, st, padCount);

    // cbuffer as alignas(16) structs with full byte-exact layout
    size_t layoutIdx = 0;
    for (const auto& bufMv : pr.m_Buffers)
    {
        if (!bufMv.m_bIsCBuffer) continue;
        auto* sp = std::get_if<StructType*>(&bufMv.m_Type);
        if (!sp || !*sp) continue;

        if (layoutIdx >= layouts.size()) continue;
        const LayoutMember& lm = layouts[layoutIdx++];
        const StructType& st   = **sp;

        out << "struct alignas(16) " << st.m_Name << "\n{\n";
        EmitMembersCpp(out, st.m_Members, lm.m_Submembers, padCount, "    ");
        out << "};\n\n";
    }

    LogMsg("[cpp_gen] Done (padCount=%d)\n", padCount);
    return out.str();
}
