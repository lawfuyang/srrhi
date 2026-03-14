#include "types.h"
#include <stdexcept>
#include <cassert>
#include <algorithm>

// ---------------------------------------------------------------------------
// HLSL cbuffer packing rules
// Reference: https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-packing-rules
//
// Summary:
//  - Variables are packed into 4-component (16-byte) registers.
//  - A variable cannot straddle a register boundary.
//  - Scalars: aligned to their own size (bool/int/uint/float=4B, half=2B, double=8B)
//  - Vectors: float2 aligns to 8B, float3/float4 align to 16B
//  - Matrices: floatNxM stored column-major: cols many columns, each column = floatN in 16B slot
//              (float3x4: N=3,M=4 → 4 columns of float3 → display float3[0..3], total=4*16=64B)
//  - Structs: align to 16B, size padded to multiple of 16B.
//  - Arrays: each element starts on a 16B boundary (inter-element stride >= 16).
//            Next field after array starts at: lastElemBaseOffset + elemDataSize (NOT +stride).
// ---------------------------------------------------------------------------

static inline int alignUp(int value, int align)
{
    assert(align > 0);
    return (value + align - 1) & ~(align - 1);
}

// Base alignment of a single (non-array) HlslType
static int baseAlignment(const HlslType& t)
{
    int elem = t.scalarBytes();
    if (t.isScalar()) return elem;
    if (t.isVector())
    {
        int n = t.cols;
        if (n <= 2) return elem * 2;
        return elem * 4; // vec3 & vec4 → 16B for 4B scalars
    }
    // Matrices: single-column matrices align like vectors, multi-column like arrays
    if (t.cols == 1)
    {
        int n = t.rows;
        if (n <= 2) return elem * 2;
        return elem * 4; // single-column as a vector type
    }
    return 16; // multi-column matrices
}

// Raw data size (bytes) of a single HlslType element (no trailing padding)
static int typeSize(const HlslType& t)
{
    int elem = t.scalarBytes();
    if (t.isScalar()) return elem;
    if (t.isVector())  return elem * t.cols;
    // Matrix floatNxM: stored as M columns of floatN, each column in a 16B slot.
    // typeSize = M * 16 (N <= 4, so floatN fits in a 16B slot, with 16-N*elem trailing bytes)
    return t.cols * 16;
}

static int arrayElemAlignment(const HlslType& t)
{
    return std::max(16, baseAlignment(t));
}

static int arrayElemStride(const HlslType& t)
{
    return alignUp(typeSize(t), 16);
}

// ---------------------------------------------------------------------------
// Recursive struct layout
// ---------------------------------------------------------------------------

static void computeStructLayout(StructType& st, const std::string& srcPath);

static void computeStructLayoutIfNeeded(StructType& st, const std::string& srcPath)
{
    if (st.sizeBytes == 0)
        computeStructLayout(st, srcPath);
}

// Cursor advance past a single (non-array) primitive field.
// For matrices: last column has no trailing 16B padding, so cursor = baseOffset + (cols-1)*16 + colDataSize
// For scalars/vectors: cursor = baseOffset + typeSize
static int typeCursorAdvance(const HlslType& t)
{
    if (t.isMatrix())
    {
        int colDataSize = t.rows * t.scalarBytes();
        return (t.cols - 1) * 16 + colDataSize;
    }
    return typeSize(t);
}

static int placePrimitive(Field& field, int offset)
{
    const HlslType& ht = field.hlslType;
    int align  = baseAlignment(ht);
    int size   = typeSize(ht);
    int aligned = alignUp(offset, align);

    // Cannot straddle a 16-byte boundary
    if (!ht.isMatrix() && size > 0 && (aligned / 16) != ((aligned + size - 1) / 16))
    {
        if ((aligned % 16) != 0)
            aligned = alignUp(aligned, 16);
    }

    field.offsetBytes  = aligned;
    field.sizeBytes    = size;
    field.paddingBytes = 0;
    // Return cursor = aligned + advance past last element's actual data
    return aligned + typeCursorAdvance(ht);
}

static void computeStructLayout(StructType& st, const std::string& srcPath)
{
    int structAlign = 16;
    int offset      = 0;
    bool prevWasArray = false;        // Track arrays separately
    bool prevWasMatrix = false;       // Track non-array matrices separately

    for (size_t i = 0; i < st.fields.size(); ++i)
    {
        auto& field = st.fields[i];
        if (field.isStruct)
        {
            assert(field.structType);
            computeStructLayoutIfNeeded(*field.structType, srcPath);
            StructType& inner = *field.structType;

            int elemAlign  = std::max(inner.alignmentBytes, 16);
            int elemStride = alignUp(inner.sizeBytes, 16);

            offset = alignUp(offset, elemAlign);
            field.offsetBytes = offset;
            field.sizeBytes   = inner.sizeBytes;

            int totalElems = field.arrayDims.empty() ? 1 : field.totalElements();
            // Inter-element padding (padding shown between elements)
            field.paddingBytes = elemStride - inner.sizeBytes;

            if (totalElems > 1)
            {
                // After last element: cursor = lastElemOffset + dataSize
                offset = field.offsetBytes + (totalElems - 1) * elemStride + inner.sizeBytes;
                prevWasArray = true;
                prevWasMatrix = false;
            }
            else
            {
                // Single non-array struct: cursor advances by sizeBytes only.
                // The next field aligns normally from there.
                // paddingBytes will be recomputed in second pass.
                field.paddingBytes = 0;
                offset = field.offsetBytes + inner.sizeBytes;
                prevWasArray = false;
                prevWasMatrix = false;
            }

            structAlign = std::max(structAlign, elemAlign);
        }
        else
        {
            const HlslType& ht = field.hlslType;
            bool isArray = !field.arrayDims.empty();

            if (isArray)
            {
                int elemAlign  = arrayElemAlignment(ht);
                int size       = typeSize(ht);
                int stride     = arrayElemStride(ht);
                int totalElems = field.totalElements();

                offset = alignUp(offset, elemAlign);
                field.offsetBytes  = offset;
                field.sizeBytes    = size;
                field.paddingBytes = stride - size;

                if (totalElems > 1)
                    offset = field.offsetBytes + (totalElems - 1) * stride + typeCursorAdvance(ht);
                else
                    offset = field.offsetBytes + typeCursorAdvance(ht);

                prevWasArray = true;
                prevWasMatrix = false;
                structAlign = std::max(structAlign, elemAlign);
            }
            else
            {
                // Non-array primitive/matrix: 
                // If previous field was an ARRAY, place directly at cursor without alignment.
                // Matrices still need 16-byte alignment.
                // Fields after matrices still get their own alignment.
                if (prevWasArray && !ht.isMatrix())
                {
                    // Previous was array, current is non-matrix primitive: skip alignment
                    int size = typeSize(ht);
                    field.offsetBytes = offset;
                    field.sizeBytes   = size;
                    field.paddingBytes = 0;
                    offset = offset + typeCursorAdvance(ht);
                    prevWasArray = false;
                    prevWasMatrix = false;
                }
                else if (ht.isMatrix() && ht.cols > 1)
                {
                    // Multi-column matrices need 16-byte alignment
                    offset = alignUp(offset, 16);
                    int size = typeSize(ht);
                    field.offsetBytes = offset;
                    field.sizeBytes   = size;
                    field.paddingBytes = 0;
                    offset = offset + typeCursorAdvance(ht);
                    prevWasArray = false;
                    prevWasMatrix = true;
                }
                else
                {
                    // Regular scalar/vector: use normal alignment
                    offset = placePrimitive(field, offset);
                    prevWasArray = false;
                    prevWasMatrix = false;
                }
                structAlign = std::max(structAlign, baseAlignment(ht));
            }
        }
    }

    st.alignmentBytes = structAlign;
    st.sizeBytes      = offset; // actual data size, NOT padded to 16

    // Second pass: paddingBytes for non-array primitives AND non-array single structs
    for (size_t i = 0; i < st.fields.size(); ++i)
    {
        auto& f = st.fields[i];
        bool isNonArrayPrimitive = !f.isStruct && f.arrayDims.empty();
        bool isNonArrayStruct    =  f.isStruct && f.arrayDims.empty();
        if (isNonArrayPrimitive || isNonArrayStruct)
        {
            int nextOffset = (i + 1 < st.fields.size())
                           ? st.fields[i+1].offsetBytes
                           : st.sizeBytes;
            // For matrices, the "end" of the field data is at offsetBytes + typeCursorAdvance(ht)
            // not at offsetBytes + typeSize(ht)
            int fieldDataEnd = f.offsetBytes + f.sizeBytes;
            if (isNonArrayPrimitive && f.hlslType.isMatrix())
                fieldDataEnd = f.offsetBytes + typeCursorAdvance(f.hlslType);
            f.paddingBytes = nextOffset - fieldDataEnd;
        }
    }
}

// ---------------------------------------------------------------------------
// Flat layout builder
// ---------------------------------------------------------------------------

struct LayoutBuilder
{
    std::vector<LayoutItem> items;
    int depth = 0;

    static std::string hlslTypeName(const HlslType& t)
    {
        static const char* scalarNames[] = {
            "bool","half","float","double","int","uint","uint16_t","int16_t"
        };
        std::string base = scalarNames[(int)t.scalar];
        if (t.isScalar()) return base;
        if (t.isVector()) return base + std::to_string(t.cols);
        return base + std::to_string(t.rows) + "x" + std::to_string(t.cols);
    }

    void addOpen(const std::string& typeName)
    {
        LayoutItem it{};
        it.displayType  = typeName;
        it.depth        = depth;
        it.isStructOpen = true;
        items.push_back(it);
        ++depth;
    }

    void addClose(const std::string& label, int offset, int size, int pad)
    {
        --depth;
        LayoutItem it{};
        it.displayName   = label;
        it.offsetBytes   = offset;
        it.sizeBytes     = size;
        it.paddingBytes  = pad;
        it.depth         = depth;
        it.isStructClose = true;
        items.push_back(it);
    }

    void addLeaf(const std::string& typeName, const std::string& label,
                 int offset, int size, int pad)
    {
        LayoutItem it{};
        it.displayType  = typeName;
        it.displayName  = label;
        it.offsetBytes  = offset;
        it.sizeBytes    = size;
        it.paddingBytes = pad;
        it.depth        = depth;
        it.isLeaf       = true;
        items.push_back(it);
    }

    // Expand a matrix field into column-vector entries for display.
    // floatNxM → M columns of floatN, each at 16-byte intervals.
    // Single-column matrices (N×1) are shown as a single vector, not as [0].
    void expandMatrix(const HlslType& ht, const std::string& prefix,
                      int baseOffset, int trailingPad)
    {
        int numCols = ht.cols;
        HlslType colType;
        colType.scalar = ht.scalar;
        colType.rows   = 1;
        colType.cols   = ht.rows; // each column has N components
        std::string colName = hlslTypeName(colType);
        int colDataSize = typeSize(colType);

        // Single-column matrices are displayed as a regular vector, not as [0]
        if (numCols == 1)
        {
            addLeaf(colName, prefix + ";", baseOffset, colDataSize, trailingPad);
            return;
        }

        for (int c = 0; c < numCols; ++c)
        {
            bool lastCol = (c == numCols - 1);
            int offset = baseOffset + c * 16;
            int pad    = lastCol ? trailingPad : (16 - colDataSize);
            addLeaf(colName, prefix + "[" + std::to_string(c) + "]",
                    offset, colDataSize, pad);
        }
    }

    void flattenFields(const std::vector<Field>& fields, int baseOffset)
    {
        for (const auto& f : fields)
        {
            if (f.isStruct)
            {
                const StructType& st = *f.structType;
                std::string typeName = "struct " + st.name;
                int totalElems = f.arrayDims.empty() ? 1 : f.totalElements();
                int elemStride = alignUp(st.sizeBytes, 16);
                int interPad   = elemStride - st.sizeBytes;

                for (int i = 0; i < totalElems; ++i)
                {
                    bool lastElem = (i == totalElems - 1);
                    int elemOff = f.offsetBytes + baseOffset;
                    if (totalElems > 1)
                        elemOff += i * elemStride;

                    addOpen(typeName);
                    flattenFields(st.fields, elemOff);

                    // Close label
                    std::string label = f.name;
                    if (!f.arrayDims.empty())
                        label += "[" + std::to_string(i) + "]";
                    label += ";";

                    // Padding shown on close:
                    // Non-last array element → inter-element pad
                    // Last or non-array single → no array pad (0)
                    // But for a non-array single struct, the struct itself is padded
                    // to 16B boundary and we show that as the padding.
                    int closePad = 0;
                    if (!lastElem)
                        closePad = interPad;
                    else if (f.arrayDims.empty())
                        closePad = f.paddingBytes;

                    addClose(label, elemOff, st.sizeBytes, closePad);
                }
            }
            else
            {
                const HlslType& ht = f.hlslType;
                std::string typeName = hlslTypeName(ht);
                int totalElems = f.arrayDims.empty() ? 1 : f.totalElements();
                int stride = arrayElemStride(ht);

                if (ht.isMatrix())
                {
                    for (int i = 0; i < totalElems; ++i)
                    {
                        bool lastElem = (i == totalElems - 1);
                        int elemOff = f.offsetBytes + baseOffset;
                        if (totalElems > 1)
                            elemOff += i * stride;

                        // trailingPad: padding after the last column of this matrix instance
                        int trailingPad = 0;
                        if (!f.arrayDims.empty() && !lastElem)
                            trailingPad = stride - typeSize(ht);
                        else if (f.arrayDims.empty())
                            trailingPad = f.paddingBytes;

                        std::string prefix = f.name;
                        if (!f.arrayDims.empty())
                            prefix += "[" + std::to_string(i) + "]";

                        expandMatrix(ht, prefix, elemOff, trailingPad);
                    }
                }
                else
                {
                    for (int i = 0; i < totalElems; ++i)
                    {
                        bool lastElem = (i == totalElems - 1);
                        int elemOff = f.offsetBytes + baseOffset;
                        if (totalElems > 1)
                            elemOff += i * stride;

                        int pad = f.arrayDims.empty()
                                ? f.paddingBytes
                                : (lastElem ? 0 : f.paddingBytes);

                        std::string label = f.name;
                        if (!f.arrayDims.empty())
                            label += "[" + std::to_string(i) + "]";
                        label += ";";

                        addLeaf(typeName, label, elemOff, f.sizeBytes, pad);
                    }
                }
            }
        }
    }
};

// ---------------------------------------------------------------------------
// CBuffer layout
// ---------------------------------------------------------------------------

static FlatLayout layoutCBuffer(CBuffer& cb, ParseResult& pr)
{
    for (auto& st : pr.structs)
        computeStructLayoutIfNeeded(st, pr.sourceFile);

    StructType tmp;
    tmp.name   = cb.name;
    tmp.fields = cb.fields;
    computeStructLayout(tmp, pr.sourceFile);
    cb.fields = tmp.fields;

    LayoutBuilder builder;
    builder.flattenFields(cb.fields, 0);

    FlatLayout fl;
    fl.cbufferName = cb.name;
    fl.items       = std::move(builder.items);
    fl.totalSize   = tmp.sizeBytes;
    return fl;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<FlatLayout> computeLayouts(ParseResult& pr)
{
    logMsg("[layout] Computing layouts for %zu cbuffers...\n", pr.cbuffers.size());
    
    std::vector<FlatLayout> result;
    for (auto& cb : pr.cbuffers)
    {
        logMsg("[layout]   Computing layout for: %s\n", cb.name.c_str());
        result.push_back(layoutCBuffer(cb, pr));
        logMsg("[layout]     Total size: %d bytes\n", result.back().totalSize);
    }
    
    logMsg("[layout] Layout computation complete\n");
    return result;
}