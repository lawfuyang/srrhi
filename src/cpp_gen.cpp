#include "types.h"
#include <sstream>

// ---------------------------------------------------------------------------
// HLSL -> DirectXMath type mapping  (non-array, non-matrix use)
// ---------------------------------------------------------------------------

struct CppTypeInfo { std::string typeName; int arrayMult = 0; };

static CppTypeInfo mapToCppType(const HlslType& t)
{
    if (t.isScalar())
    {
        switch (t.scalar)
        {
            case ScalarKind::Bool:   return {"BOOL"};
            case ScalarKind::Half:   return {"DirectX::PackedVector::HALF"};
            case ScalarKind::Float:  return {"float"};
            case ScalarKind::Double: return {"double"};
            case ScalarKind::Int:    return {"int32_t"};
            case ScalarKind::Uint:   return {"uint32_t"};
            case ScalarKind::Uint16: return {"uint16_t"};
            case ScalarKind::Int16:  return {"int16_t"};
        }
    }
    if (t.isVector())
    {
        int n = t.cols;
        switch (t.scalar)
        {
            case ScalarKind::Float:
                if (n == 2) return {"DirectX::XMFLOAT2"};
                if (n == 3) return {"DirectX::XMFLOAT3"};
                if (n == 4) return {"DirectX::XMFLOAT4"};
                break;
            case ScalarKind::Int:
                if (n == 2) return {"DirectX::XMINT2"};
                if (n == 3) return {"DirectX::XMINT3"};
                if (n == 4) return {"DirectX::XMINT4"};
                break;
            case ScalarKind::Uint:
                if (n == 2) return {"DirectX::XMUINT2"};
                if (n == 3) return {"DirectX::XMUINT3"};
                if (n == 4) return {"DirectX::XMUINT4"};
                break;
            case ScalarKind::Half:
                if (n == 2) return {"DirectX::PackedVector::XMHALF2"};
                if (n == 4) return {"DirectX::PackedVector::XMHALF4"};
                break;
            default: break;
        }
        // Fallback: emit as scalar array
        CppTypeInfo fi = mapToCppType(HlslType{t.scalar, 1, 1, false});
        fi.arrayMult = n;
        return fi;
    }
    if (t.isMatrix())
    {
        if (t.scalar == ScalarKind::Float)
        {
            int r = t.rows, c = t.cols;
            if (r == 2 && c == 2) return {"DirectX::XMFLOAT2X2"};
            if (r == 3 && c == 3) return {"DirectX::XMFLOAT3X3"};
            if (r == 4 && c == 3) return {"DirectX::XMFLOAT4X3"};
            if (r == 4 && c == 4) return {"DirectX::XMFLOAT4X4"};
        }
        // Fallback: cols * (16/scalarBytes) scalars total
        CppTypeInfo fi = mapToCppType(HlslType{t.scalar, 1, 1, false});
        fi.arrayMult = t.cols * (16 / t.scalarBytes());
        return fi;
    }
    return {"float"};
}

// Helpers
// ---------------------------------------------------------------------------

// How many bytes the cursor advances after a single (non-array) primitive field.
// For matrices: (cols-1)*16 + rows*scalarBytes (last column has no trailing 16B slot padding).
// For others:   byteSize (= scalarBytes * cols for vectors).
static int primitiveFieldCursorAdvance(const HlslType& t)
{
    if (t.isMatrix())
        return (t.cols - 1) * 16 + t.rows * t.scalarBytes();
    return t.byteSize();
}

static void emitPadding(std::ostringstream& out, int padBytes, int& padCount,
                        const std::string& ind)
{
    if (padBytes <= 0) return;
    while (padBytes >= 4)
        out << ind << "uint32_t pad" << padCount++ << ";\n", padBytes -= 4;
    if (padBytes == 2)
        out << ind << "uint16_t pad" << padCount++ << ";\n";
    else if (padBytes != 0)
        out << ind << "// WARNING: " << padBytes << " unaccounted byte(s)\n";
}

// Returns true if this matrix type has a named DXMath type (sizeof matches exactly).
static bool matrixHasNamedType(const HlslType& t)
{
    if (!t.isMatrix() || t.scalar != ScalarKind::Float) return false;
    int r = t.rows, c = t.cols;
    return (r == 2 && c == 2) || (r == 3 && c == 3) ||
           (r == 4 && c == 3) || (r == 4 && c == 4);
}

// Expand a non-array unnamed matrix field into per-column members.
// Each column except the last occupies a full 16-byte slot.
// The last column only has rows*scalarBytes actual data — no trailing slot padding.
// Returns the offset committed after the last column's data.
static int expandMatrixColumns(std::ostringstream& out, const HlslType& ht,
                                const std::string& name, int baseOffset,
                                const std::string& ind, int& padCount)
{
    // Scalar type name for one component
    std::string scalarTypeName = mapToCppType(HlslType{ht.scalar, 1, 1, false}).typeName;
    int elemsPerSlot = 16 / ht.scalarBytes(); // floats per 16-byte slot
    int colDataElems = ht.rows;               // actual data elements in one column

    for (int c = 0; c < ht.cols; ++c)
    {
        std::string colName = name + "_c" + std::to_string(c);
        bool lastCol = (c == ht.cols - 1);
        if (lastCol)
        {
            // Last column: emit only the actual data elements (no slot padding)
            out << ind << scalarTypeName << " " << colName
                << "[" << colDataElems << "];\n";
        }
        else
        {
            // Non-last columns: emit slot-width array for proper 16-byte alignment
            out << ind << scalarTypeName << " " << colName
                << "[" << elemsPerSlot << "];\n";
        }
    }

    // Committed end after last column data
    return baseOffset + (ht.cols - 1) * 16 + colDataElems * ht.scalarBytes();
}

// Emit one non-array primitive field (no expansion).
// For unnamed matrix types (arrayMult>0), emit column-by-column expansion.
// Returns the committed end offset.
static int emitPrimitiveMember(std::ostringstream& out, const HlslType& ht,
                                const std::string& name, int fieldOffset,
                                const std::string& ind, int& padCount)
{
    auto cti = mapToCppType(ht);
    if (ht.isMatrix() && cti.arrayMult > 0 && !matrixHasNamedType(ht))
    {
        // Unnamed matrix: expand column by column for exact byte layout
        return expandMatrixColumns(out, ht, name, fieldOffset, ind, padCount);
    }
    out << ind << cti.typeName << " " << name;
    if (cti.arrayMult > 0)
        out << "[" << cti.arrayMult << "]";
    out << ";\n";
    return fieldOffset + primitiveFieldCursorAdvance(ht);
}

// Expand an array field into per-element members + inter-element stride padding.
// Last element gets no trailing stride padding (only consumes its data bytes).
// Returns committed end offset after last element's data.
static int expandArrayField(std::ostringstream& out, const Field& f, const std::string& ind,
                             int& padCount)
{
    int stride = f.sizeBytes + f.paddingBytes;
    int n = f.totalElements();
    int elemOffset = f.offsetBytes;

    for (int i = 0; i < n; ++i)
    {
        std::string ename = f.name + "_" + std::to_string(i);
        bool lastElem = (i == n - 1);

        if (f.isStruct)
        {
            out << ind << f.structType->name << " " << ename << ";\n";
        }
        else
        {
            emitPrimitiveMember(out, f.hlslType, ename, elemOffset, ind, padCount);
        }
        // Inter-element stride padding (skip after last element)
        if (!lastElem && f.paddingBytes > 0)
            emitPadding(out, f.paddingBytes, padCount, ind);

        elemOffset += stride;
    }

    // Committed end = last element's start + its sizeBytes (no trailing stride pad)
    return f.offsetBytes + (n - 1) * stride + f.sizeBytes;
}

// ---------------------------------------------------------------------------
// Struct emitters
// ---------------------------------------------------------------------------

// Used-struct definitions: structs referenced from cbuffers.
// We do NOT pad them to 16 bytes here — padding in the cbuffer struct is always explicit.
// However, when a struct is used as an array element, it needs its stride embedded.
// Solution: emit the struct with padding up to alignUp(sizeBytes, 16) so that
//           array elements naturally get 16-byte stride, and compensate in single-use
//           context by noting the struct is already padded.
// The simplest consistent choice: always pad struct to alignUp(sizeBytes, 16).
// Single-use cbuffer fields that need LESS trailing pad: handled by computing
//   f.paddingBytes = nextOffset - (offsetBytes + sizeBytes) in layout,
//   but the struct in C++ has alignUp(sizeBytes,16) size, so a single struct field
//   will be LARGER than expected.
//
// Better approach: emit struct as its exact sizeBytes (no trailing pad), then:
//   - Single use: emit trailing pad from f.paddingBytes
//   - Array use:  expand to per-element members with inter-element stride padding
//
// This means struct arrays cannot use C++ array syntax — we expand them.
// This is always correct and byte-accurate.
static void emitStructDef(std::ostringstream& out, const StructType& st, int& padCount)
{
    std::string fInd("    ");
    out << "struct " << st.name << "\n{\n";

    int offset = 0;
    for (const auto& f : st.fields)
    {
        if (f.offsetBytes > offset)
            emitPadding(out, f.offsetBytes - offset, padCount, fInd);

        if (f.arrayDims.empty())
        {
            if (f.isStruct)
            {
                out << fInd << f.structType->name << " " << f.name << ";\n";
                if (f.paddingBytes > 0)
                    emitPadding(out, f.paddingBytes, padCount, fInd);
                offset = f.offsetBytes + f.sizeBytes + f.paddingBytes;
            }
            else
            {
                int dataEnd = emitPrimitiveMember(out, f.hlslType, f.name, f.offsetBytes, fInd, padCount);
                if (f.paddingBytes > 0)
                    emitPadding(out, f.paddingBytes, padCount, fInd);
                offset = dataEnd + f.paddingBytes;
            }
        }
        else
        {
            // Array field inside a struct: expand element by element
            offset = expandArrayField(out, f, fInd, padCount);
        }
    }

    // No extra padding needed here — the struct definition exactly matches sizeBytes.
    // Consumers (array expansion) add inter-element stride padding explicitly.

    out << "};\n\n";
}

static void emitCBufferDef(std::ostringstream& out, const CBuffer& cb, int& padCount)
{
    std::string fInd("    ");
    out << "struct alignas(16) " << cb.name << "\n{\n";

    int offset = 0;
    for (const auto& f : cb.fields)
    {
        if (f.offsetBytes > offset)
            emitPadding(out, f.offsetBytes - offset, padCount, fInd);

        if (f.arrayDims.empty())
        {
            // Single (non-array) field
            if (f.isStruct)
            {
                out << fInd << f.structType->name << " " << f.name << ";\n";
                if (f.paddingBytes > 0)
                    emitPadding(out, f.paddingBytes, padCount, fInd);
                offset = f.offsetBytes + f.sizeBytes + f.paddingBytes;
            }
            else
            {
                int dataEnd = emitPrimitiveMember(out, f.hlslType, f.name, f.offsetBytes, fInd, padCount);
                if (f.paddingBytes > 0)
                    emitPadding(out, f.paddingBytes, padCount, fInd);
                offset = dataEnd + f.paddingBytes;
            }
        }
        else
        {
            // Array field: expand element by element with inter-element stride padding
            offset = expandArrayField(out, f, fInd, padCount);
        }
    }

    out << "};\n\n";
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string generateCpp(const ParseResult& pr, const std::vector<CBuffer>& layoutCbs,
                        int& padCount)
{
    logMsg("[cpp_gen] Generating C++ header...\n");
    
    std::ostringstream out;
    out << "// Auto-generated by srrhi. Do not edit.\n";
    out << "#pragma once\n";
    out << "#include <cstdint>\n";
    out << "#include <DirectXMath.h>\n";
    out << "#include <DirectXPackedVector.h>\n\n";

    logMsg("[cpp_gen]   Emitting %zu structs...\n", pr.structs.size());
    for (const auto& st : pr.structs)
        emitStructDef(out, st, padCount);

    logMsg("[cpp_gen]   Emitting %zu cbuffers...\n", layoutCbs.size());
    for (const auto& cb : layoutCbs)
        emitCBufferDef(out, cb, padCount);

    logMsg("[cpp_gen] C++ header generation complete (padCount=%d)\n", padCount);
    return out.str();
}