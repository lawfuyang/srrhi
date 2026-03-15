#include "types.h"
#include <sstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// C++ type mapping from BuiltinType
// ---------------------------------------------------------------------------
struct CppTypeInfo { std::string typeName; int arrayMult = 0; };

static CppTypeInfo mapBuiltinToCpp(const BuiltinType& bt)
{
    const std::string& sc = bt.scalarName;
    int vs = bt.vectorsize;

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

    // float vectors — use DirectXMath when available
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
    CppTypeInfo base = mapBuiltinToCpp(BuiltinType{bt.scalarName, bt.scalarName,
                                                    bt.elementsize, bt.alignment, 1});
    base.arrayMult = vs;
    return base;
}

static void emitPadding(std::ostringstream& out, int padBytes, int& padCount,
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
//   - Matrix fields (created_from_matrix arrays) are emitted column-by-column.
// ---------------------------------------------------------------------------

static void emitMembersCpp(std::ostringstream& out,
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
        int fieldOffset = lm ? lm->offset : cursor;
        if (fieldOffset > cursor)
            emitPadding(out, fieldOffset - cursor, padCount, ind);

        // === Struct field ===
        if (auto* sp = std::get_if<StructType*>(&mv.type))
        {
            if (!lm) { cursor = fieldOffset; continue; }

            // Check if this is an array of structs (layout node is an array)
            bool isArray = std::holds_alternative<std::shared_ptr<ArrayNode>>(lm->type);
            if (isArray)
            {
                // Expand each array element as a separate struct member
                for (size_t e = 0; e < lm->submembers.size(); ++e)
                {
                    const LayoutMember& elem = lm->submembers[e];
                    std::string eName = mv.name + "_" + std::to_string(e);
                    out << ind << (*sp)->name << " " << eName << ";\n";
                    if (elem.padding > 0)
                        emitPadding(out, elem.padding, padCount, ind);
                    cursor = elem.offset + elem.size + elem.padding;
                }
            }
            else
            {
                // Single struct member
                out << ind << (*sp)->name << " " << mv.name << ";\n";
                if (lm->padding > 0)
                    emitPadding(out, lm->padding, padCount, ind);
                cursor = lm->offset + lm->size + lm->padding;
            }
            continue;
        }

        // === BuiltinType or ArrayNode field ===
        if (!lm) { cursor = fieldOffset; continue; }

        bool isArray = std::holds_alternative<std::shared_ptr<ArrayNode>>(lm->type);

        if (isArray)
        {
            const ArrayNode& arr = *std::get<std::shared_ptr<ArrayNode>>(lm->type);

            if (arr.created_from_matrix)
            {
                // Matrix: emit column-by-column
                const BuiltinType* elemBt = std::get_if<BuiltinType>(&arr.elementType);
                if (elemBt)
                {
                    auto cti = mapBuiltinToCpp(*elemBt);
                    int colDataElems = elemBt->vectorsize;
                    int elemsPerSlot = 16 / elemBt->elementsize;

                    for (size_t c = 0; c < lm->submembers.size(); ++c)
                    {
                        const LayoutMember& col = lm->submembers[c];
                        bool lastCol = (c == lm->submembers.size() - 1);
                        std::string colName = mv.name + "_c" + std::to_string(c);

                        if (lastCol)
                            out << ind << cti.typeName << " " << colName << "[" << colDataElems << "];\n";
                        else
                            out << ind << cti.typeName << " " << colName << "[" << elemsPerSlot << "];\n";

                        if (!lastCol && col.padding > 0)
                            emitPadding(out, col.padding, padCount, ind);
                        cursor = col.offset + col.size + (lastCol ? lm->padding : col.padding);
                    }
                    if (lm->padding > 0)
                        emitPadding(out, lm->padding, padCount, ind);
                    cursor = lm->offset + lm->size + lm->padding;
                }
            }
            else
            {
                // Regular array: expand per element
                for (size_t e = 0; e < lm->submembers.size(); ++e)
                {
                    const LayoutMember& elem = lm->submembers[e];
                    bool lastElem = (e == lm->submembers.size() - 1);

                    if (auto* elemBt = std::get_if<BuiltinType>(&elem.type))
                    {
                        auto cti = mapBuiltinToCpp(*elemBt);
                        std::string eName = mv.name + "_" + std::to_string(e);
                        out << ind << cti.typeName << " " << eName;
                        if (cti.arrayMult > 0)
                            out << "[" << cti.arrayMult << "]";
                        out << ";\n";
                    }
                    else if (auto* elemSp = std::get_if<StructType*>(&elem.type))
                    {
                        std::string eName = mv.name + "_" + std::to_string(e);
                        out << ind << (*elemSp)->name << " " << eName << ";\n";
                    }

                    if (!lastElem && elem.padding > 0)
                        emitPadding(out, elem.padding, padCount, ind);
                    cursor = elem.offset + elem.size + (lastElem ? 0 : elem.padding);
                }
                cursor = lm->offset + lm->size + lm->padding;
            }
        }
        else
        {
            // Single non-array BuiltinType field
            const BuiltinType* bt = std::get_if<BuiltinType>(&lm->type);
            if (bt)
            {
                auto cti = mapBuiltinToCpp(*bt);
                out << ind << cti.typeName << " " << mv.name;
                if (cti.arrayMult > 0) out << "[" << cti.arrayMult << "]";
                out << ";\n";
            }
            if (lm->padding > 0)
                emitPadding(out, lm->padding, padCount, ind);
            cursor = lm->offset + lm->size + lm->padding;
        }
    }
}

// ---------------------------------------------------------------------------
// Emit a named struct definition (for use in the C++ header)
// ---------------------------------------------------------------------------
static void emitStructCpp(std::ostringstream& out, const StructType& st,
                           int& padCount)
{
    out << "struct " << st.name << "\n{\n";
    // Structs without layout info: emit fields in order without padding
    // (padding is added in the cbuffer context)
    std::string fInd("    ");
    for (const auto& mv : st.members)
    {
        if (auto* sp = std::get_if<StructType*>(&mv.type))
            out << fInd << (*sp)->name << " " << mv.name << ";\n";
        else if (auto* bt = std::get_if<BuiltinType>(&mv.type))
        {
            auto cti = mapBuiltinToCpp(*bt);
            out << fInd << cti.typeName << " " << mv.name;
            if (cti.arrayMult > 0) out << "[" << cti.arrayMult << "]";
            out << ";\n";
        }
        else if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&mv.type))
        {
            const ArrayNode& arr = **ap;
            if (arr.created_from_matrix)
            {
                // Matrix: emit as array of column vectors, e.g. float4x4 → XMFLOAT4[4]
                if (auto* elemBt = std::get_if<BuiltinType>(&arr.elementType))
                {
                    auto cti = mapBuiltinToCpp(*elemBt);
                    out << fInd << cti.typeName << " " << mv.name << "[" << arr.arraySize << "];\n";
                }
            }
            else
            {
                if (auto* elemBt = std::get_if<BuiltinType>(&arr.elementType))
                {
                    auto cti = mapBuiltinToCpp(*elemBt);
                    out << fInd << cti.typeName << " " << mv.name << "[" << arr.arraySize << "];\n";
                }
                else if (auto* elemSp = std::get_if<StructType*>(&arr.elementType))
                {
                    out << fInd << (*elemSp)->name << " " << mv.name << "[" << arr.arraySize << "];\n";
                }
            }
        }
    }
    out << "};\n\n";
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::string generateCpp(const ParseResult& pr,
                         const std::vector<LayoutMember>& layouts,
                         int& padCount)
{
    logMsg("[cpp_gen] Generating C++ header...\n");

    std::ostringstream out;
    out << "// Auto-generated by srrhi. Do not edit.\n";
    out << "#pragma once\n";
    out << "#include <cstdint>\n\n";

    // Named struct definitions
    for (const auto& st : pr.structs)
        emitStructCpp(out, st, padCount);

    // cbuffer as alignas(16) structs with full byte-exact layout
    size_t layoutIdx = 0;
    for (const auto& bufMv : pr.buffers)
    {
        if (!bufMv.isCBuffer) continue;
        auto* sp = std::get_if<StructType*>(&bufMv.type);
        if (!sp || !*sp) continue;

        if (layoutIdx >= layouts.size()) continue;
        const LayoutMember& lm = layouts[layoutIdx++];
        const StructType& st   = **sp;

        out << "struct alignas(16) " << st.name << "\n{\n";
        emitMembersCpp(out, st.members, lm.submembers, padCount, "    ");
        out << "};\n\n";
    }

    logMsg("[cpp_gen] Done (padCount=%d)\n", padCount);
    return out.str();
}