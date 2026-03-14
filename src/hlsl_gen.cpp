#include "types.h"
#include <sstream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string hlslScalarName(ScalarKind sk)
{
    switch (sk)
    {
        case ScalarKind::Bool:   return "bool";
        case ScalarKind::Half:   return "half";
        case ScalarKind::Float:  return "float";
        case ScalarKind::Double: return "double";
        case ScalarKind::Int:    return "int";
        case ScalarKind::Uint:   return "uint";
        case ScalarKind::Uint16: return "uint16_t";
        case ScalarKind::Int16:  return "int16_t";
    }
    return "float";
}

// Type name as it should appear in HLSL source
static std::string hlslTypeName(const HlslType& t)
{
    std::string base = hlslScalarName(t.scalar);
    if (t.isScalar()) return base;
    if (t.isVector()) return base + std::to_string(t.cols);
    // Matrix
    if (t.declaredAsMatrix && t.rows == 1)
        return "matrix<" + base + "," + std::to_string(t.rows) + "," + std::to_string(t.cols) + ">";
    return base + std::to_string(t.rows) + "x" + std::to_string(t.cols);
}

static std::string arrayDimsStr(const std::vector<ArrayDim>& dims)
{
    std::string s;
    for (auto& d : dims)
        s += "[" + std::to_string(d.size) + "]";
    return s;
}

// Validate HLSL compliance
static void validateHlslType(const HlslType& t, const std::string& fieldName,
                              const std::string& src)
{
    if (t.rows < 1 || t.rows > 4)
        throw std::runtime_error("Field '" + fieldName + "': invalid matrix rows " +
                                 std::to_string(t.rows) + " in " + src);
    if (t.cols < 1 || t.cols > 4)
        throw std::runtime_error("Field '" + fieldName + "': invalid vector/matrix cols " +
                                 std::to_string(t.cols) + " in " + src);
}

// Emit padding variables to fill `padBytes` bytes.
// Handles both 2-byte and 4-byte units.
static void emitPadding(std::ostringstream& out, int padBytes, int& padCount,
                        const std::string& ind)
{
    if (padBytes <= 0) return;
    // Use uint16_t for 2-byte remainder, uint for 4-byte units
    while (padBytes >= 4)
    {
        out << ind << "uint pad" << padCount++ << ";\n";
        padBytes -= 4;
    }
    if (padBytes == 2)
    {
        out << ind << "uint16_t pad" << padCount++ << ";\n";
    }
    else if (padBytes != 0)
    {
        // Odd bytes: shouldn't normally happen in valid HLSL cbuffer
        // Emit a warning comment
        out << ind << "// WARNING: " << padBytes << " byte(s) unaccounted padding\n";
    }
}

// Effective "cursor end" for a field (offset + data bytes written)
static int fieldCursorEnd(const Field& f)
{
    if (f.isStruct)
    {
        if (f.arrayDims.empty())
            return f.offsetBytes + f.sizeBytes;
        int stride = f.sizeBytes + f.paddingBytes;
        return f.offsetBytes + (f.totalElements() - 1) * stride + f.sizeBytes;
    }
    const HlslType& ht = f.hlslType;
    if (f.arrayDims.empty())
    {
        if (ht.isMatrix())
        {
            int colDataSize = ht.rows * ht.scalarBytes();
            return f.offsetBytes + (ht.cols - 1) * 16 + colDataSize;
        }
        return f.offsetBytes + f.sizeBytes;
    }
    int stride  = f.sizeBytes + f.paddingBytes;
    int dataEnd = ht.isMatrix()
                ? (ht.rows * ht.scalarBytes())  // colDataSize
                : f.sizeBytes;
    return f.offsetBytes + (f.totalElements() - 1) * stride + dataEnd;
}

// ---------------------------------------------------------------------------
// Emit a struct definition (HLSL)
// ---------------------------------------------------------------------------

static void emitStructHlsl(std::ostringstream& out,
                            const StructType& st,
                            int& padCount,
                            int indent)
{
    std::string ind(indent * 4, ' ');
    std::string fInd((indent + 1) * 4, ' ');
    out << ind << "struct " << st.name << "\n" << ind << "{\n";

    int offset = 0;

    for (const auto& f : st.fields)
    {
        // Padding before this field
        if (f.offsetBytes > offset)
            emitPadding(out, f.offsetBytes - offset, padCount, fInd);

        if (f.isStruct)
        {
            out << fInd << f.structType->name << " " << f.name << arrayDimsStr(f.arrayDims) << ";\n";
        }
        else
        {
            validateHlslType(f.hlslType, f.name, st.name);
            out << fInd << hlslTypeName(f.hlslType) << " " << f.name << arrayDimsStr(f.arrayDims) << ";\n";
        }

        int cursorEnd = fieldCursorEnd(f);

        // Trailing padding (only for non-array single fields — arrays handle their own stride)
        if (f.arrayDims.empty() && f.paddingBytes > 0)
            emitPadding(out, f.paddingBytes, padCount, fInd);

        offset = cursorEnd + (f.paddingBytes > 0 && f.arrayDims.empty() ? f.paddingBytes : 0);
    }

    // Trailing struct padding to reach sizeBytes
    if (offset < st.sizeBytes)
        emitPadding(out, st.sizeBytes - offset, padCount, fInd);

    out << ind << "};\n\n";
}

// ---------------------------------------------------------------------------
// Emit a cbuffer (HLSL)
// ---------------------------------------------------------------------------

static void emitCBufferHlsl(std::ostringstream& out,
                             const CBuffer& cb,
                             int& padCount)
{
    out << "cbuffer " << cb.name << "\n{\n";

    int offset = 0;
    std::string fInd("    ");

    for (const auto& f : cb.fields)
    {
        if (f.offsetBytes > offset)
            emitPadding(out, f.offsetBytes - offset, padCount, fInd);

        if (f.isStruct)
        {
            out << fInd << f.structType->name << " " << f.name << arrayDimsStr(f.arrayDims) << ";\n";
        }
        else
        {
            validateHlslType(f.hlslType, f.name, cb.name);
            out << fInd << hlslTypeName(f.hlslType) << " " << f.name << arrayDimsStr(f.arrayDims) << ";\n";
        }

        int cursorEnd = fieldCursorEnd(f);

        if (f.arrayDims.empty() && f.paddingBytes > 0)
            emitPadding(out, f.paddingBytes, padCount, fInd);

        offset = cursorEnd + (f.paddingBytes > 0 && f.arrayDims.empty() ? f.paddingBytes : 0);
    }

    out << "};\n\n";
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string generateHlsl(const ParseResult& pr, const std::vector<CBuffer>& layoutCbs, int& padCount)
{
    logMsg("[hlsl_gen] Generating HLSL header...\n");
    
    std::ostringstream out;
    out << "// Auto-generated by srrhi. Do not edit.\n\n";

    logMsg("[hlsl_gen]   Emitting %zu structs...\n", pr.structs.size());
    for (const auto& st : pr.structs)
        emitStructHlsl(out, st, padCount, 0);

    logMsg("[hlsl_gen]   Emitting %zu cbuffers...\n", layoutCbs.size());
    for (const auto& cb : layoutCbs)
        emitCBufferHlsl(out, cb, padCount);

    logMsg("[hlsl_gen] HLSL header generation complete (padCount=%d)\n", padCount);
    return out.str();
}