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
    std::string m_Name;                       // display name, e.g. "float", "float3"
    std::string m_ScalarName;                 // base scalar, e.g. "float", "float16_t"
    int         m_ElementSize          = 4;   // bytes per scalar component
    int         m_Alignment            = 4;   // = elementsize (scalar alignment)
    int         m_VectorSize           = 1;   // number of components
    bool        m_bCreatedFromMatrix  = false;
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
    TypeRef     m_ElementType;
    int         m_ArraySize             = 0;
    std::string m_Name;                       // e.g. "float3[4]" or "float3x4"
    bool        m_bCreatedFromMatrix   = false;
    bool        m_bIsRowMajor          = false;
};

// ---------------------------------------------------------------------------
// Helpers on TypeRef
// ---------------------------------------------------------------------------
int         TypeAlignment(const TypeRef& t);    // scalar alignment for cbuffer layout
std::string TypeDisplayName(const TypeRef& t);  // display name for visualizer / codegen

// ---------------------------------------------------------------------------
// MemberVariable: a field within a struct or cbuffer
// ---------------------------------------------------------------------------
struct MemberVariable
{
    TypeRef     m_Type;
    std::string m_Name;
    bool        m_bIsCBuffer = false;
    bool        m_bIsSBuffer = false;
};

// ---------------------------------------------------------------------------
// StructType: named user-defined struct
// ---------------------------------------------------------------------------
struct StructType
{
    std::string                  m_Name;
    std::vector<MemberVariable>  m_Members;
};

// ---------------------------------------------------------------------------
// SrInputMember: a member within an srinput scope (cbuffer reference)
//   m_CBufferName – name of the cbuffer type being referenced
//   m_MemberName  – variable name for this member
// ---------------------------------------------------------------------------
struct SrInputMember
{
    std::string m_CBufferName;
    std::string m_MemberName;
};

// ---------------------------------------------------------------------------
// ResourceKind: the category of an HLSL resource object.
// ---------------------------------------------------------------------------
enum class ResourceKind
{
    // SRV (t# registers) -------------------------------------------------------
    Texture1D,
    Texture1DArray,
    Texture2D,
    Texture2DArray,
    Texture2DMS,
    Texture2DMSArray,
    Texture3D,
    TextureCube,
    TextureCubeArray,
    Buffer,
    StructuredBuffer,
    ByteAddressBuffer,
    RaytracingAccelerationStructure,

    // UAV (u# registers) -------------------------------------------------------
    RWTexture1D,
    RWTexture1DArray,
    RWTexture2D,
    RWTexture2DArray,
    RWTexture3D,
    RWBuffer,
    RWStructuredBuffer,
    RWByteAddressBuffer,
};

// Returns true if the resource kind is a UAV (u# register), false for SRV (t#).
inline bool IsUAV(ResourceKind k)
{
    return k == ResourceKind::RWTexture1D
        || k == ResourceKind::RWTexture1DArray
        || k == ResourceKind::RWTexture2D
        || k == ResourceKind::RWTexture2DArray
        || k == ResourceKind::RWTexture3D
        || k == ResourceKind::RWBuffer
        || k == ResourceKind::RWStructuredBuffer
        || k == ResourceKind::RWByteAddressBuffer;
}

// ---------------------------------------------------------------------------
// ResourceMember: a resource (SRV or UAV) declared inside an srinput block.
//   m_Kind        – resource type enum
//   m_TypeName    – full HLSL type string, e.g. "Texture2D<float4>"
//   m_TemplateArg – template argument string, e.g. "float4" (empty if none)
//   m_MemberName  – variable name declared in the srinput block
// ---------------------------------------------------------------------------
struct ResourceMember
{
    ResourceKind m_Kind;
    std::string  m_TypeName;    // e.g. "Texture2D<float4>" or "ByteAddressBuffer"
    std::string  m_TemplateArg; // e.g. "float4", "uint", "" for raw buffers
    std::string  m_MemberName;
};

// ---------------------------------------------------------------------------
// SrInputDef: srinput scope containing cbuffer references and resource members
//   m_Name      – name of the srinput scope
//   m_Members   – list of cbuffer references, order determines register assignment
//   m_Resources – list of SRV/UAV resource members, order determines t#/u# assignment
// ---------------------------------------------------------------------------
struct SrInputDef
{
    std::string                m_Name;
    std::vector<SrInputMember> m_Members;
    std::vector<ResourceMember> m_Resources;
};

// ---------------------------------------------------------------------------
// ParseResult: result of parsing a .sr file (and its transitive includes).
//   structs     – named struct definitions referenceable by field types
//   bufferDefs  – struct definitions for cbuffers / sbuffers (not referenceable)
//   buffers     – global buffer variables; each type points into bufferDefs
//   SrInputDefs – srinput scope definitions (group cbuffers with registers)
//
//   NOTE: structs and bufferDefs use std::deque so that StructType* pointers
//   stored in TypeRefs remain valid even as the containers grow during
//   include merging.
// ---------------------------------------------------------------------------
struct ParseResult
{
    std::deque<StructType>      m_Structs;
    std::deque<StructType>      m_BufferDefs;
    std::vector<MemberVariable> m_Buffers;
    std::vector<SrInputDef>     m_SrInputDefs;
    std::string                 m_SourceFile;
};

// ---------------------------------------------------------------------------
// LayoutMember: forms a tree produced by the layout engine.
//   padding on a member = bytes between end-of-data and start of next sibling.
//   For ArrayNode members, SetPadding propagates into the last array submember.
// ---------------------------------------------------------------------------
struct LayoutMember
{
    TypeRef                   m_Type;
    std::string               m_Name;
    int                       m_Offset   = 0;
    int                       m_Size     = 0;
    int                       m_Padding  = 0;
    bool                      m_bIsGlobal  = false;
    bool                      m_bIsCBuffer = false;
    bool                      m_bIsSBuffer = false;
    std::vector<LayoutMember> m_Submembers;

    // Sets self.padding and propagates into last array submember.
    void SetPadding(int p);

    // Computes trailing padding for the previous submember, then appends m.
    void PushSubmember(LayoutMember m);
};

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
void LogMsg(const char* fmt, ...);

// ---------------------------------------------------------------------------
// Visualizer declarations (implemented in visualizer.cpp)
// ---------------------------------------------------------------------------
std::string VisualizeLayouts(const std::vector<LayoutMember>& layouts);
std::string VisualizeLayoutsMachineReadable(const std::vector<LayoutMember>& layouts);
