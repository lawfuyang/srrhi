#pragma once
#include <string>
#include <vector>
#include <deque>
#include <variant>
#include <memory>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
struct ArrayNode;
struct StructType;

// ---------------------------------------------------------------------------
// BuiltinType: scalar or vector.
//   alignment = elementsize  (scalar-based, NOT vector-size-based)
//   vectorsize = 1 for scalars, 2/3/4 for floatN / intN etc.
// ---------------------------------------------------------------------------
struct BuiltinType
{
    std::string name;                       // display name, e.g. "float", "float3"
    std::string scalarName;                 // base scalar, e.g. "float", "float16_t"
    int         elementsize          = 4;   // bytes per scalar component
    int         alignment            = 4;   // = elementsize (scalar alignment)
    int         vectorsize           = 1;   // number of components
    bool        created_from_matrix  = false;
};

// ---------------------------------------------------------------------------
// TypeRef: tagged reference to any of the three type kinds.
//   BuiltinType                  – scalar or vector
//   shared_ptr<ArrayNode>        – array or matrix (heap for recursive nesting)
//   StructType*                  – named struct reference (non-owning; resolved post-parse)
// ---------------------------------------------------------------------------
using TypeRef = std::variant<BuiltinType, std::shared_ptr<ArrayNode>, StructType*>;

// ---------------------------------------------------------------------------
// ArrayNode: array of elementType (also used for column/row-major matrices).
// ---------------------------------------------------------------------------
struct ArrayNode
{
    TypeRef     elementType;
    int         arraySize             = 0;
    std::string name;                       // e.g. "float3[4]" or "float3x4"
    bool        created_from_matrix   = false;
    bool        is_row_major          = false;
};

// ---------------------------------------------------------------------------
// Helpers on TypeRef
// ---------------------------------------------------------------------------
int         typeAlignment(const TypeRef& t);    // scalar alignment for cbuffer layout
std::string typeDisplayName(const TypeRef& t);  // display name for visualizer / codegen

// ---------------------------------------------------------------------------
// MemberVariable: a field within a struct or cbuffer
// ---------------------------------------------------------------------------
struct MemberVariable
{
    TypeRef     type;
    std::string name;
    bool        isCBuffer = false;
    bool        isSBuffer = false;
};

// ---------------------------------------------------------------------------
// StructType: named user-defined struct
// ---------------------------------------------------------------------------
struct StructType
{
    std::string                  name;
    std::vector<MemberVariable>  members;
};

// ---------------------------------------------------------------------------
// ParseResult: result of parsing a .sr file (and its transitive includes).
//   structs     – named struct definitions referenceable by field types
//   bufferDefs  – struct definitions for cbuffers / sbuffers (not referenceable)
//   buffers     – global buffer variables; each type points into bufferDefs
//
//   NOTE: structs and bufferDefs use std::deque so that StructType* pointers
//   stored in TypeRefs remain valid even as the containers grow during
//   include merging.
// ---------------------------------------------------------------------------
struct ParseResult
{
    std::deque<StructType>      structs;
    std::deque<StructType>      bufferDefs;
    std::vector<MemberVariable> buffers;
    std::string                 sourceFile;
};

// ---------------------------------------------------------------------------
// LayoutMember: forms a tree produced by the layout engine.
//   padding on a member = bytes between end-of-data and start of next sibling.
//   For ArrayNode members, SetPadding propagates into the last array submember.
// ---------------------------------------------------------------------------
struct LayoutMember
{
    TypeRef                   type;
    std::string               name;
    int                       offset   = 0;
    int                       size     = 0;
    int                       padding  = 0;
    bool                      isGlobal  = false;
    bool                      isCBuffer = false;
    bool                      isSBuffer = false;
    std::vector<LayoutMember> submembers;

    // Sets self.padding and propagates into last array submember.
    void setPadding(int p);

    // Computes trailing padding for the previous submember, then appends m.
    void pushSubmember(LayoutMember m);
};

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
void logMsg(const char* fmt, ...);
