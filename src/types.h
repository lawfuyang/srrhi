#pragma once
#include <string>
#include <vector>
#include <deque>
#include <variant>
#include <memory>
#include <cstdarg>
#include <unordered_set>

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
    std::string m_SizeExpr;                   // non-empty when size was specified via a scalar const
                                              // reference in the .sr file, e.g. "Config::MaxLights"
    bool        m_bCreatedFromMatrix   = false;
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
//   m_CBufferName    – name of the cbuffer type being referenced
//   m_MemberName     – variable name for this member
//   m_bIsPushConstant – true if this member was annotated with [push_constant]
// ---------------------------------------------------------------------------
struct SrInputMember
{
    std::string m_CBufferName;
    std::string m_MemberName;
    bool        m_bIsPushConstant = false;
};

// ---------------------------------------------------------------------------
// SrInputRef: a nested srinput reference within an srinput body.
//   m_SrInputName – the srinput type being composed in
//   m_VarName     – the syntactic variable name used in the parent scope
//                   (not emitted in generated code; used for diagnostics only)
// ---------------------------------------------------------------------------
struct SrInputRef
{
    std::string m_SrInputName;
    std::string m_VarName;
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
// SamplerKind: the category of an HLSL sampler object.
// ---------------------------------------------------------------------------
enum class SamplerKind
{
    SamplerState,
    SamplerComparisonState,
};

// ---------------------------------------------------------------------------
// SamplerMember: a sampler declared inside an srinput block.
//   m_Kind       – sampler type (SamplerState or SamplerComparisonState)
//   m_TypeName   – HLSL type string, e.g. "SamplerState"
//   m_MemberName – variable name declared in the srinput block
// ---------------------------------------------------------------------------
struct SamplerMember
{
    SamplerKind m_Kind;
    std::string m_TypeName;   // e.g. "SamplerState" or "SamplerComparisonState"
    std::string m_MemberName;
};

// ---------------------------------------------------------------------------
// ScalarConst: a compile-time scalar constant declared inside an srinput block.
//   Supports optional "static const" / "const" qualifiers (all treated the same).
//   m_TypeName  – HLSL scalar type name, e.g. "float", "uint", "int32_t"
//   m_Name      – variable name as written in the .sr file
//   m_Value     – literal value string as written in the .sr file
// ---------------------------------------------------------------------------
struct ScalarConst
{
    std::string m_TypeName;  // e.g. "float", "uint32_t", "bool"
    std::string m_Name;      // e.g. "MaxLights"
    std::string m_Value;     // e.g. "16", "3.14", "true"
};

// ---------------------------------------------------------------------------
// SrInputDef: srinput scope containing cbuffer references, resource members,
//             sampler members, scalar constants, optionally nested srinput refs,
//             and optionally a list of inherited base srinputs.
//   m_Name            – name of the srinput scope
//   m_RegisterSpace   – register space index from [space(N)] attribute; -1 = not specified
//   m_BaseInheritances– srinput names inherited from (in declaration order)
//                       Inherited content is flattened before this srinput's body items.
//   m_Members         – list of direct cbuffer references
//   m_Resources       – list of direct SRV/UAV resource members
//   m_Samplers        – list of direct sampler members
//   m_ScalarConsts    – list of direct scalar compile-time constants
//   m_NestedSrInputs  – list of nested srinput references (composition)
//   m_BodyOrder       – unified declaration order used for flattening
//                       kind: 0=CBuffer, 1=Resource, 2=Sampler, 3=ScalarConst, 4=NestedRef
//                       idx:  index into the corresponding array above
// ---------------------------------------------------------------------------
struct SrInputDef
{
    std::string                 m_Name;
    int                         m_RegisterSpace = -1;  // -1 = no [space(N)] attribute
    std::vector<std::string>    m_BaseInheritances;    // base srinput names, in declaration order
    std::vector<SrInputMember>  m_Members;
    std::vector<ResourceMember> m_Resources;
    std::vector<SamplerMember>  m_Samplers;
    std::vector<ScalarConst>    m_ScalarConsts;
    std::vector<SrInputRef>     m_NestedSrInputs;  // nested srinput references

    // Body item ordering: records declaration order for correct flattening.
    struct BodyItem { int kind; int idx; };
    std::vector<BodyItem>       m_BodyOrder;
};

// ---------------------------------------------------------------------------
// FlatSrInput: result of recursively flattening an srinput composition hierarchy.
//   Generated by FlattenSrInput() (defined in flatten.h).
//   All items from nested srinputs are expanded in declaration order (DFS).
//   Register space is always taken from the top-most parent srinput.
// ---------------------------------------------------------------------------
struct FlatSrInput
{
    std::vector<SrInputMember>  m_Members;
    std::vector<ResourceMember> m_Resources;
    std::vector<SamplerMember>  m_Samplers;
    std::vector<ScalarConst>    m_ScalarConsts;
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
    std::deque<StructType>           m_Structs;
    std::deque<StructType>           m_BufferDefs;
    std::vector<MemberVariable>      m_Buffers;
    std::vector<SrInputDef>          m_SrInputDefs;
    std::string                      m_SourceFile;

    // Include tracking: direct #include directives (in declaration order).
    // Each entry is the path string as written in the .sr file (e.g. "base.sr").
    std::vector<std::string>         m_DirectIncludes;

    // Names of ALL structs that originated from included files (transitively).
    // Generators use this to skip re-emitting struct definitions already
    // covered by the emitted #include directives.
    std::unordered_set<std::string>  m_IncludedStructNames;
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

// g_Verbose is defined in main.cpp; set by -v / --verbose / --test flags.
extern bool g_Verbose;

// VerboseMsg: like LogMsg but only emits output when g_Verbose is true.
// Use this for all diagnostic logging outside of main.cpp.
inline void VerboseMsg(const char* fmt, ...)
{
    if (!g_Verbose) return;
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// Visualizer declarations (implemented in visualizer.cpp)
// ---------------------------------------------------------------------------
std::string VisualizeLayouts(const std::vector<LayoutMember>& layouts);
std::string VisualizeLayoutsMachineReadable(const std::vector<LayoutMember>& layouts);
