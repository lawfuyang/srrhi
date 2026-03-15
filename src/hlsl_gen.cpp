#include "types.h"
#include <sstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// HLSL type name reconstruction from TypeRef
// ---------------------------------------------------------------------------
static std::string hlslTypeName(const TypeRef& t)
{
    if (auto* bt = std::get_if<BuiltinType>(&t))
        return bt->name;

    if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&t))
    {
        const ArrayNode& arr = **ap;
        if (arr.created_from_matrix)
        {
            // Reconstruct "scalarNxM" (or "row_major scalarNxM")
            // Column-major (default): element.vectorsize = rows, arraySize = cols
            // Row-major:              element.vectorsize = cols, arraySize = rows
            const BuiltinType* elem = std::get_if<BuiltinType>(&arr.elementType);
            if (!elem)
                throw std::runtime_error("matrix element is not a BuiltinType");

            int vs = elem->vectorsize;  // rows for col-major, cols for row-major
            int as = arr.arraySize;     // cols for col-major, rows for row-major

            std::string base;
            if (!arr.is_row_major)
                base = elem->scalarName + std::to_string(vs) + "x" + std::to_string(as);
            else
                base = "row_major " + elem->scalarName +
                       std::to_string(as) + "x" + std::to_string(vs);
            return base;
        }
        // Regular array: recurse for element type name + [size]
        return hlslTypeName(arr.elementType) + "[" + std::to_string(arr.arraySize) + "]";
    }

    if (auto* sp = std::get_if<StructType*>(&t))
        return (*sp)->name;

    return "???";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void emitPadding(std::ostringstream& out, int padBytes, int& padCount,
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
static void emitStructHlsl(std::ostringstream& out, const StructType& st,
                            int& padCount, int indent,
                            const LayoutMember* lm = nullptr);

static void emitStructBodyHlsl(std::ostringstream& out,
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

        int fieldOffset = lm ? lm->offset : cursor;
        int fieldSize   = lm ? lm->size : 0;
        int fieldPad    = lm ? lm->padding : 0;

        // Padding before this field
        if (fieldOffset > cursor)
            emitPadding(out, fieldOffset - cursor, padCount, fInd);

        // Emit the field declaration
        if (auto* structP = std::get_if<StructType*>(&mv.type))
        {
            // Struct field
            out << fInd << (*structP)->name << " " << mv.name << ";\n";
        }
        else if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&mv.type))
        {
            const ArrayNode& arr = **ap;
            if (arr.created_from_matrix)
            {
                // Matrix: type name already includes NxM, no array suffix
                out << fInd << hlslTypeName(mv.type) << " " << mv.name << ";\n";
            }
            else
            {
                // Regular array: emit "elementType name[size];"
                out << fInd << hlslTypeName(arr.elementType)
                    << " " << mv.name
                    << "[" << arr.arraySize << "];\n";
            }
        }
        else
        {
            out << fInd << hlslTypeName(mv.type) << " " << mv.name << ";\n";
        }

        // Trailing padding after this field
        if (fieldPad > 0)
            emitPadding(out, fieldPad, padCount, fInd);

        // Advance cursor: end of field data + trailing pad
        cursor = fieldOffset + fieldSize + fieldPad;
    }
}

static void emitStructHlsl(std::ostringstream& out, const StructType& st,
                            int& padCount, int indent,
                            const LayoutMember* lm)
{
    std::string ind(indent * 4, ' ');
    out << ind << "struct " << st.name << "\n" << ind << "{\n";

    const std::vector<LayoutMember>* subs = lm ? &lm->submembers : nullptr;
    std::vector<LayoutMember> empty;
    emitStructBodyHlsl(out, st.members,
                       subs ? *subs : empty,
                       padCount, indent + 1);

    out << ind << "};\n\n";
}

// ---------------------------------------------------------------------------
// Emit cbuffer (HLSL)
// ---------------------------------------------------------------------------
static void emitCBufferHlsl(std::ostringstream& out,
                             const StructType& bufferStruct,
                             const LayoutMember& layout,
                             int& padCount)
{
    out << "cbuffer " << bufferStruct.name << "\n{\n";

    emitStructBodyHlsl(out, bufferStruct.members, layout.submembers,
                       padCount, 1);

    out << "};\n\n";
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::string generateHlsl(const ParseResult& pr,
                          const std::vector<LayoutMember>& layouts,
                          int& padCount)
{
    logMsg("[hlsl_gen] Generating HLSL header...\n");

    std::ostringstream out;
    out << "// Auto-generated by srrhi. Do not edit.\n\n";

    // Emit named struct definitions
    logMsg("[hlsl_gen]   Emitting %zu struct(s)...\n", pr.structs.size());
    for (const auto& st : pr.structs)
        emitStructHlsl(out, st, padCount, 0, nullptr);

    // Emit cbuffers (using layout info for padding)
    logMsg("[hlsl_gen]   Emitting %zu cbuffer(s)...\n", layouts.size());

    size_t layoutIdx = 0;
    for (const auto& bufMv : pr.buffers)
    {
        if (!bufMv.isCBuffer) continue;
        auto* sp = std::get_if<StructType*>(&bufMv.type);
        if (!sp || !*sp) continue;

        if (layoutIdx < layouts.size())
        {
            emitCBufferHlsl(out, **sp, layouts[layoutIdx], padCount);
            ++layoutIdx;
        }
    }

    logMsg("[hlsl_gen] Done (padCount=%d)\n", padCount);
    return out.str();
}