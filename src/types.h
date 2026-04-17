#pragma once
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <cstdarg>
#include <unordered_map>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
struct StructType;
struct MemberVariable;

// ---------------------------------------------------------------------------
// TypeRef: abstract base class for all type references.
//   Concrete subclasses:
//     BuiltinTypeRef  – scalar or vector
//     ArrayTypeRef    – array or matrix
//     StructTypeRef   – named struct reference (non-owning)
//     ExternTypeRef   – externally-defined type
//
//   Instances are always heap-allocated and owned via std::shared_ptr<TypeRef>.
//
//   The interface is designed so that callers never need to downcast.
//   Each virtual method has a safe default that is correct for types that
//   don't support that concept.
// ---------------------------------------------------------------------------
struct TypeRef
{
    virtual ~TypeRef() = default;

    // -----------------------------------------------------------------------
    // Core identity
    // -----------------------------------------------------------------------

    // Scalar alignment for cbuffer layout.
    virtual int         Alignment()   const = 0;

    // Display name for visualizer / codegen.
    virtual std::string DisplayName() const = 0;

    // Deep copy.
    virtual std::shared_ptr<TypeRef> Clone() const = 0;

    // -----------------------------------------------------------------------
    // Type checks — only the matching subclass returns true.
    // -----------------------------------------------------------------------
    virtual bool IsBuiltin() const { return false; }
    virtual bool IsArray()   const { return false; }
    virtual bool IsStruct()  const { return false; }
    virtual bool IsExtern()  const { return false; }

    // -----------------------------------------------------------------------
    // BuiltinTypeRef properties
    //   Meaningful only when IsBuiltin() == true; safe defaults otherwise.
    // -----------------------------------------------------------------------

    // Base scalar name, e.g. "float", "float16_t".
    virtual std::string ScalarName()         const { return ""; }

    // Number of vector components (1 for scalars, 2–4 for vecN types).
    virtual int         VectorSize()         const { return 1; }

    // Bytes per scalar component.
    virtual int         ElementSize()        const { return 4; }

    // -----------------------------------------------------------------------
    // ArrayTypeRef properties
    //   Meaningful only when IsArray() == true; safe defaults otherwise.
    // -----------------------------------------------------------------------

    // Number of array elements.
    virtual int         ArraySize()          const { return 0; }

    // Symbolic size expression (non-empty when size came from a scalar const ref).
    virtual std::string SizeExpr()           const { return ""; }

    // Element type (nullptr for non-array types).
    virtual const std::shared_ptr<TypeRef>& ElementType() const;

    // -----------------------------------------------------------------------
    // Shared BuiltinTypeRef / ArrayTypeRef property
    // -----------------------------------------------------------------------

    // True when this type was synthesised from matrix<T,R,C> syntax.
    virtual bool IsCreatedFromMatrix()       const { return false; }

    // -----------------------------------------------------------------------
    // StructTypeRef properties
    //   Meaningful only when IsStruct() == true; safe defaults otherwise.
    // -----------------------------------------------------------------------

    // Struct name without any "struct " prefix, e.g. "MyStruct".
    virtual std::string StructName()         const { return ""; }

    // Pointer to the struct's member list (nullptr for non-struct types).
    virtual const std::vector<MemberVariable>* Members() const { return nullptr; }

    // Remap the internal StructType* pointer after include merging.
    // Only StructTypeRef does anything; all others are no-ops.
    virtual void RemapStruct(const std::unordered_map<StructType*, StructType*>&) {}

    // -----------------------------------------------------------------------
    // ExternTypeRef properties
    //   Meaningful only when IsExtern() == true.
    //   The extern type name is returned by DisplayName().
    // -----------------------------------------------------------------------
    // (no additional properties beyond DisplayName())
};

// Null-safe static element-type sentinel (returned by default ElementType()).
inline const std::shared_ptr<TypeRef>& TypeRef::ElementType() const
{
    static const std::shared_ptr<TypeRef> s_null;
    return s_null;
}

// ---------------------------------------------------------------------------
// BuiltinTypeRef: scalar or vector.
//   alignment = elementsize  (scalar-based, NOT vector-size-based)
//   vectorsize = 1 for scalars, 2/3/4 for floatN / intN etc.
// ---------------------------------------------------------------------------
struct BuiltinTypeRef : TypeRef
{
    std::string m_Name;                       // display name, e.g. "float", "float3"
    std::string m_ScalarName;                 // base scalar, e.g. "float", "float16_t"
    int         m_ElementSize          = 4;   // bytes per scalar component
    int         m_Alignment_           = 4;   // = elementsize (scalar alignment)
    int         m_VectorSize           = 1;   // number of components
    bool        m_bCreatedFromMatrix   = false;

    int         Alignment()            const override { return m_Alignment_; }
    std::string DisplayName()          const override { return m_Name; }
    bool        IsBuiltin()            const override { return true; }
    std::string ScalarName()           const override { return m_ScalarName; }
    int         VectorSize()           const override { return m_VectorSize; }
    int         ElementSize()          const override { return m_ElementSize; }
    bool        IsCreatedFromMatrix()  const override { return m_bCreatedFromMatrix; }

    std::shared_ptr<TypeRef> Clone() const override
    {
        return std::make_shared<BuiltinTypeRef>(*this);
    }
};

// ---------------------------------------------------------------------------
// ArrayTypeRef: array of elementType (also used for column/row-major matrices).
// ---------------------------------------------------------------------------
struct ArrayTypeRef : TypeRef
{
    std::shared_ptr<TypeRef> m_ElementType;
    int         m_ArraySize             = 0;
    std::string m_Name;                       // e.g. "float3[4]" or "float3x4"
    std::string m_SizeExpr;                   // non-empty when size was specified via a scalar const
                                              // reference in the .sr file, e.g. "Config::MaxLights"
    bool        m_bCreatedFromMatrix    = false;

    int         Alignment()            const override
    {
        return m_ElementType ? m_ElementType->Alignment() : 4;
    }
    std::string DisplayName()          const override { return m_Name; }
    bool        IsArray()              const override { return true; }
    int         ArraySize()            const override { return m_ArraySize; }
    std::string SizeExpr()             const override { return m_SizeExpr; }
    bool        IsCreatedFromMatrix()  const override { return m_bCreatedFromMatrix; }

    const std::shared_ptr<TypeRef>& ElementType() const override { return m_ElementType; }

    std::shared_ptr<TypeRef> Clone() const override
    {
        auto copy = std::make_shared<ArrayTypeRef>();
        copy->m_ElementType        = m_ElementType ? m_ElementType->Clone() : nullptr;
        copy->m_ArraySize          = m_ArraySize;
        copy->m_Name               = m_Name;
        copy->m_SizeExpr           = m_SizeExpr;
        copy->m_bCreatedFromMatrix = m_bCreatedFromMatrix;
        return copy;
    }
};

// ---------------------------------------------------------------------------
// StructTypeRef: non-owning reference to a named struct (resolved post-parse).
// ---------------------------------------------------------------------------
struct StructTypeRef : TypeRef
{
    StructType* m_Struct = nullptr;  // non-owning; stable pointer into ParseResult::m_Structs

    int         Alignment()   const override { return 16; } // always 16-byte aligned in cbuffers
    std::string DisplayName() const override;                // defined after StructType
    bool        IsStruct()    const override { return true; }
    std::string StructName()  const override;                // defined after StructType
    const std::vector<MemberVariable>* Members() const override; // defined after MemberVariable

    void RemapStruct(const std::unordered_map<StructType*, StructType*>& remap) override
    {
        auto it = remap.find(m_Struct);
        if (it != remap.end()) m_Struct = it->second;
    }

    std::shared_ptr<TypeRef> Clone() const override
    {
        return std::make_shared<StructTypeRef>(*this);
    }
};

// ---------------------------------------------------------------------------
// ExternTypeRef: an externally-defined type declared with 'extern TypeName;'.
//   Not defined in any .sr file; assumed to be provided by the including code.
//   Valid as a struct member type or resource template argument.
//   Size/alignment are unknown — the layout engine skips such fields.
// ---------------------------------------------------------------------------
struct ExternTypeRef : TypeRef
{
    std::string m_Name;  // e.g. "MyExternalStruct"

    int         Alignment()   const override { return 4; } // unknown; default to 4 (skipped by layout engine)
    std::string DisplayName() const override { return m_Name; }
    bool        IsExtern()    const override { return true; }

    std::shared_ptr<TypeRef> Clone() const override
    {
        return std::make_shared<ExternTypeRef>(*this);
    }
};

// ---------------------------------------------------------------------------
// StructType: named user-defined struct
// ---------------------------------------------------------------------------
struct MemberVariable;  // forward-declared for StructType

struct StructType
{
    std::string                  m_Name;
    std::vector<MemberVariable>  m_Members;
};

// StructTypeRef methods that need StructType to be complete.
inline std::string StructTypeRef::DisplayName() const
{
    return m_Struct ? ("struct " + m_Struct->m_Name) : "struct <null>";
}
inline std::string StructTypeRef::StructName() const
{
    return m_Struct ? m_Struct->m_Name : "";
}
inline const std::vector<MemberVariable>* StructTypeRef::Members() const
{
    return m_Struct ? &m_Struct->m_Members : nullptr;
}

// ---------------------------------------------------------------------------
// Helpers on TypeRef (free functions kept for backward compatibility)
// ---------------------------------------------------------------------------
inline int TypeAlignment(const std::shared_ptr<TypeRef>& t)
{
    return t ? t->Alignment() : 4;
}
inline std::string TypeDisplayName(const std::shared_ptr<TypeRef>& t)
{
    return t ? t->DisplayName() : "???";
}

// ---------------------------------------------------------------------------
// MemberVariable: a field within a struct or cbuffer
// ---------------------------------------------------------------------------
struct MemberVariable
{
    std::shared_ptr<TypeRef> m_Type;
    std::string m_Name;
    std::string m_OriginalTypeName; // non-empty when the type came from a macro alias (#define / typedef / using)
    bool        m_bIsCBuffer = false;
    bool        m_bIsSBuffer = false;
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
// ---------------------------------------------------------------------------
struct ResourceMember
{
    ResourceKind m_Kind;
    std::string  m_TypeName;             // e.g. "Texture2D<float4>" (resolved) or "ByteAddressBuffer"
    std::string  m_TemplateArg;          // resolved template arg, e.g. "float4"
    std::string  m_OriginalTemplateArg;  // as written in .sr when it was a macro alias (e.g. "SPD_TYPE")
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
// ---------------------------------------------------------------------------
struct SamplerMember
{
    SamplerKind m_Kind;
    std::string m_TypeName;   // e.g. "SamplerState" or "SamplerComparisonState"
    std::string m_MemberName;
};

// ---------------------------------------------------------------------------
// ScalarConst: a compile-time scalar constant declared inside an srinput block.
// ---------------------------------------------------------------------------
struct ScalarConst
{
    std::string m_TypeName;  // e.g. "float", "uint32_t", "bool"
    std::string m_Name;      // e.g. "MaxLights"
    std::string m_Value;     // e.g. "16", "3.14", "true"
};

// ---------------------------------------------------------------------------
// SrInputDef
// ---------------------------------------------------------------------------
struct SrInputDef
{
    std::string                 m_Name;
    int                         m_RegisterSpace = -1;
    std::vector<std::string>    m_BaseInheritances;
    std::vector<SrInputMember>  m_Members;
    std::vector<ResourceMember> m_Resources;
    std::vector<SamplerMember>  m_Samplers;
    std::vector<ScalarConst>    m_ScalarConsts;
    std::vector<SrInputRef>     m_NestedSrInputs;

    struct BodyItem { int kind; int idx; };
    std::vector<BodyItem>       m_BodyOrder;
};

// ---------------------------------------------------------------------------
// FlatSrInput
// ---------------------------------------------------------------------------
struct FlatSrInput
{
    std::vector<SrInputMember>  m_Members;
    std::vector<ResourceMember> m_Resources;
    std::vector<SamplerMember>  m_Samplers;
    std::vector<ScalarConst>    m_ScalarConsts;
};

// ---------------------------------------------------------------------------
// ParseResult
// ---------------------------------------------------------------------------
struct ParseResult
{
    std::deque<StructType>           m_Structs;
    std::deque<StructType>           m_BufferDefs;
    std::vector<MemberVariable>      m_Buffers;
    std::vector<SrInputDef>          m_SrInputDefs;
    std::string                      m_SourceFile;

    std::vector<std::string>         m_DirectIncludes;
    std::unordered_set<std::string>  m_IncludedStructNames;
    std::unordered_set<std::string>  m_ExternTypeNames;

    enum class DeclKind { Struct, BufferDef, SrInput };
    struct DeclEntry { DeclKind kind; size_t idx; };
    std::vector<DeclEntry>           m_DeclOrder;

    std::vector<std::string>         m_PreprocPassthrough;
};

// ---------------------------------------------------------------------------
// LayoutMember: forms a tree produced by the layout engine.
// ---------------------------------------------------------------------------
struct LayoutMember
{
    std::shared_ptr<TypeRef>  m_Type;
    std::string               m_Name;
    int                       m_Offset   = 0;
    int                       m_Size     = 0;
    int                       m_Padding  = 0;
    bool                      m_bIsGlobal  = false;
    bool                      m_bIsCBuffer = false;
    bool                      m_bIsSBuffer = false;
    std::vector<LayoutMember> m_Submembers;

    void SetPadding(int p);
    void PushSubmember(LayoutMember m);
};

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
void LogMsg(const char* fmt, ...);

extern bool g_Verbose;

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
