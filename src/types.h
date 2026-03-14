#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// HLSL scalar / vector / matrix type info
// ---------------------------------------------------------------------------

enum class ScalarKind
{
    Bool,
    Half,
    Float,
    Double,
    Int,
    Uint,
    Uint16,  // uint16_t
    Int16,   // int16_t
};

struct HlslType
{
    ScalarKind scalar = ScalarKind::Float;
    int         rows   = 1;   // 1 for scalars/vectors
    int         cols   = 1;   // 1 for scalars, N for vectors, NxM for matrices
    bool        declaredAsMatrix = false; // true when declared via matrix<> keyword

    // Convenience
    bool isScalar()  const { return !declaredAsMatrix && rows == 1 && cols == 1; }
    bool isVector()  const { return !declaredAsMatrix && rows == 1 && cols >  1; }
    bool isMatrix()  const { return  declaredAsMatrix || rows >  1; }

    // Byte size of the base element (one component)
    int scalarBytes() const
    {
        switch (scalar)
        {
            case ScalarKind::Bool:   return 4; // HLSL bool = 32-bit
            case ScalarKind::Half:   return 2;
            case ScalarKind::Float:  return 4;
            case ScalarKind::Double: return 8;
            case ScalarKind::Int:    return 4;
            case ScalarKind::Uint:   return 4;
            case ScalarKind::Uint16: return 2;
            case ScalarKind::Int16:  return 2;
        }
        return 4;
    }

    // Total bytes of the type (no array, no struct)
    int byteSize() const { return scalarBytes() * rows * cols; }
};

// ---------------------------------------------------------------------------
// Struct / cbuffer field definitions (parsed from .sr)
// ---------------------------------------------------------------------------

struct ArrayDim
{
    int size; // e.g. 3 for [3]
};

// Forward declaration
struct StructType;

// A field inside a struct or cbuffer
struct Field
{
    std::string name;

    // Either a primitive HlslType or a named struct type
    bool isStruct = false;
    HlslType  hlslType;                            // valid when !isStruct
    int       structTypeIdx = -1;                  // index into ParseResult::structs (valid when isStruct)

    // Resolved after all parsing is done (call ParseResult::resolvePointers())
    StructType* structType = nullptr;

    std::vector<ArrayDim> arrayDims;               // outermost first, e.g. [3][2]

    // Filled during layout phase
    int offsetBytes  = 0;
    int sizeBytes    = 0;   // size of one element (no array multiplier)
    int paddingBytes = 0;   // trailing padding for this array element slot

    int totalElements() const
    {
        int n = 1;
        for (auto& d : arrayDims) n *= d.size;
        return n;
    }
};

// A named struct
struct StructType
{
    std::string        name;
    std::vector<Field> fields;

    // Filled during layout phase (size of the struct itself, before any padding to array stride)
    int sizeBytes      = 0;
    int alignmentBytes = 0;
};

// A top-level cbuffer declaration
struct CBuffer
{
    std::string        name;
    std::vector<Field> fields;
};

// Top-level parse result for one .sr file
struct ParseResult
{
    std::vector<StructType> structs;  // named struct definitions (order matters for lookup)
    std::vector<CBuffer>    cbuffers;
    std::string             sourceFile;

    // Call once after all parsing to resolve structType pointers in all fields.
    void resolvePointers();
};

// ---------------------------------------------------------------------------
// Flat layout items produced by the layout engine
// ---------------------------------------------------------------------------

struct LayoutItem
{
    std::string displayType;   // e.g. "float3", "struct Test"
    std::string displayName;   // e.g. "mat[2]", "test[0]", "s"
    int         offsetBytes;
    int         sizeBytes;
    int         paddingBytes;  // trailing pad for this slot
    int         depth;         // indentation depth
    bool        isStructOpen;  // print "struct X {" line
    bool        isStructClose; // print "} name[i];" line
    bool        isLeaf;        // actual data line
};

struct FlatLayout
{
    std::string           cbufferName;
    std::vector<LayoutItem> items;
    int                   totalSize;
};

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

// Log message function (available across all translation units)
void logMsg(const char* fmt, ...);
