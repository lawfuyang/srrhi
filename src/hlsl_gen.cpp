#include "types.h"
#include <sstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// HLSL type name reconstruction from TypeRef
// ---------------------------------------------------------------------------
static std::string HlslTypeName(const TypeRef& t)
{
    if (auto* bt = std::get_if<BuiltinType>(&t))
        return bt->m_Name;

    if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&t))
    {
        const ArrayNode& arr = **ap;
        if (arr.m_bCreatedFromMatrix)
        {
            // Reconstruct "scalarNxM" (or "row_major scalarNxM")
            // Column-major (default): element.m_VectorSize = rows, arraySize = cols
            // Row-major:              element.m_VectorSize = cols, arraySize = rows
            const BuiltinType* elem = std::get_if<BuiltinType>(&arr.m_ElementType);
            if (!elem)
                throw std::runtime_error("matrix element is not a BuiltinType");

            int vs = elem->m_VectorSize;  // rows for col-major, cols for row-major
            int as = arr.m_ArraySize;     // cols for col-major, rows for row-major

            std::string base;
            if (!arr.m_bIsRowMajor)
                base = elem->m_ScalarName + std::to_string(vs) + "x" + std::to_string(as);
            else
                base = "row_major " + elem->m_ScalarName +
                       std::to_string(as) + "x" + std::to_string(vs);
            return base;
        }
        // Regular array: recurse for element type name + [size]
        return HlslTypeName(arr.m_ElementType) + "[" + std::to_string(arr.m_ArraySize) + "]";
    }

    if (auto* sp = std::get_if<StructType*>(&t))
        return (*sp)->m_Name;

    return "???";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void EmitPadding(std::ostringstream& out, int padBytes, int& padCount,
                        const std::string& ind)
{
    if (padBytes <= 0) return;
    while (padBytes >= 4)
    {
        out << ind << "uint pad" << padCount++ << ";\n";
        padBytes -= 4;
    }
    if (padBytes == 2)
        out << ind << "uint16_t pad" << padCount++ << ";\n";
    else if (padBytes != 0)
        out << ind << "// WARNING: " << padBytes << " unaccounted byte(s)\n";
}

// ---------------------------------------------------------------------------
// Emit struct definition (HLSL)
// Fields are emitted using the parsed type info; padding is inserted based on
// layout offsets derived from the LayoutMember tree.
// ---------------------------------------------------------------------------

// Forward declaration
static void EmitStructHlsl(std::ostringstream& out, const StructType& st,
                            int& padCount, int indent,
                            const LayoutMember* lm = nullptr);

static void EmitStructBodyHlsl(std::ostringstream& out,
                                const std::vector<MemberVariable>& members,
                                const std::vector<LayoutMember>& layoutSubmembers,
                                int& padCount, int indent)
{
    std::string fInd(indent * 4, ' ');
    int cursor = 0;

    // Walk parsed members alongside their layout info
    // layoutSubmembers has one entry per parsed member (same order)
    for (size_t i = 0; i < members.size(); ++i)
    {
        const MemberVariable& mv = members[i];
        const LayoutMember*   lm = (i < layoutSubmembers.size()) ? &layoutSubmembers[i] : nullptr;

        int fieldOffset = lm ? lm->m_Offset : cursor;
        int fieldSize   = lm ? lm->m_Size : 0;
        int fieldPad    = lm ? lm->m_Padding : 0;

        // Padding before this field
        if (fieldOffset > cursor)
            EmitPadding(out, fieldOffset - cursor, padCount, fInd);

        // Emit the field declaration
        if (auto* structP = std::get_if<StructType*>(&mv.m_Type))
        {
            // Struct field
            out << fInd << (*structP)->m_Name << " " << mv.m_Name << ";\n";
        }
        else if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&mv.m_Type))
        {
            const ArrayNode& arr = **ap;
            if (arr.m_bCreatedFromMatrix)
            {
                // Matrix: type name already includes NxM, no array suffix
                out << fInd << HlslTypeName(mv.m_Type) << " " << mv.m_Name << ";\n";
            }
            else
            {
                // Regular array: emit "elementType name[size];"
                out << fInd << HlslTypeName(arr.m_ElementType)
                    << " " << mv.m_Name
                    << "[" << arr.m_ArraySize << "];\n";
            }
        }
        else
        {
            out << fInd << HlslTypeName(mv.m_Type) << " " << mv.m_Name << ";\n";
        }

        // Trailing padding after this field
        if (fieldPad > 0)
            EmitPadding(out, fieldPad, padCount, fInd);

        // Advance cursor: end of field data + trailing pad
        cursor = fieldOffset + fieldSize + fieldPad;
    }
}

static void EmitStructHlsl(std::ostringstream& out, const StructType& st,
                            int& padCount, int indent,
                            const LayoutMember* lm)
{
    std::string ind(indent * 4, ' ');
    out << ind << "struct " << st.m_Name << "\n" << ind << "{\n";

    const std::vector<LayoutMember>* subs = lm ? &lm->m_Submembers : nullptr;
    std::vector<LayoutMember> empty;
    EmitStructBodyHlsl(out, st.m_Members,
                       subs ? *subs : empty,
                       padCount, indent + 1);

    out << ind << "};\n\n";
}

// ---------------------------------------------------------------------------
// Emit cbuffer (HLSL)
// ---------------------------------------------------------------------------
static void EmitCBufferHlsl(std::ostringstream& out,
                             const StructType& bufferStruct,
                             const LayoutMember& layout,
                             int& padCount)
{
    out << "cbuffer " << bufferStruct.m_Name << "\n{\n";

    EmitStructBodyHlsl(out, bufferStruct.m_Members, layout.m_Submembers,
                       padCount, 1);

    out << "};\n\n";
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::string GenerateHlsl(const ParseResult& pr,
                          const std::vector<LayoutMember>& layouts,
                          int& padCount)
{
    LogMsg("[hlsl_gen] Generating HLSL header...\n");

    std::ostringstream out;
    out << "// Auto-generated by srrhi. Do not edit.\n\n";

    // Emit named struct definitions
    LogMsg("[hlsl_gen]   Emitting %zu struct(s)...\n", pr.m_Structs.size());
    for (const auto& st : pr.m_Structs)
        EmitStructHlsl(out, st, padCount, 0, nullptr);

    // Emit cbuffers (using layout info for padding)
    LogMsg("[hlsl_gen]   Emitting %zu cbuffer(s)...\n", layouts.size());

    size_t layoutIdx = 0;
    for (const auto& bufMv : pr.m_Buffers)
    {
        if (!bufMv.m_bIsCBuffer) continue;
        auto* sp = std::get_if<StructType*>(&bufMv.m_Type);
        if (!sp || !*sp) continue;

        if (layoutIdx < layouts.size())
        {
            EmitCBufferHlsl(out, **sp, layouts[layoutIdx], padCount);
            ++layoutIdx;
        }
    }

    LogMsg("[hlsl_gen] Done (padCount=%d)\n", padCount);
    return out.str();
}
