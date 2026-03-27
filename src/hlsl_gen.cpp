#include "types.h"
#include "common.h"
#include "flatten.h"
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <cstring>



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
            // Reconstruct as row_major matrix
            // Column-major (default): element.m_VectorSize = rows, arraySize = cols
            // Row-major:              element.m_VectorSize = cols, arraySize = rows
            const BuiltinType* elem = std::get_if<BuiltinType>(&arr.m_ElementType);
            if (!elem)
                throw std::runtime_error("matrix element is not a BuiltinType");

            int vs = elem->m_VectorSize;  // rows for col-major, cols for row-major
            int as = arr.m_ArraySize;     // cols for col-major, rows for row-major

            // Always emit as row_major
            std::string base = "row_major " + elem->m_ScalarName +
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
// ---------------------------------------------------------------------------
// Emit wrapped cbuffer for srinput member (HLSL)
// varName = "{SrInputName}_{CleanedMemberName}", e.g. "MainInputs_Scene"
// registerSpace = -1 means no space qualifier is emitted
// ---------------------------------------------------------------------------
static void EmitWrappedCBufferHlsl(std::ostringstream& out,
                                   const StructType& cbufferDef,
                                   const LayoutMember& layout,
                                   int registerNum,
                                   const std::string& varName,
                                   int registerSpace,
                                   int& padCount)
{
    out << "cbuffer " << cbufferDef.m_Name << " : register(b" << registerNum;
    if (registerSpace >= 0)
        out << ", space" << registerSpace;
    out << ")\n{\n";

    // Variable name: {SrInputName}_{CleanedMemberName}
    out << "    " << cbufferDef.m_Name << " " << varName << ";\n";

    out << "};\n\n";
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::string GenerateHlsl(const ParseResult& pr,
                          const std::vector<LayoutMember>& layouts,
                          int& padCount)
{
    VerboseMsg("[hlsl_gen] Generating HLSL header...\n");

    std::ostringstream out;
    out << "// Auto-generated by srrhi. Do not edit.\n\n";

    // Emit #include directives for each directly included .sr file.
    // This avoids re-emitting struct definitions already present in those files.
    for (const auto& incFile : pr.m_DirectIncludes)
    {
        // Derive the .hlsli filename: replace the .sr extension with .hlsli
        std::string hlsliName = incFile;
        const std::string srExt = ".sr";
        if (hlsliName.size() > srExt.size() &&
            hlsliName.substr(hlsliName.size() - srExt.size()) == srExt)
        {
            hlsliName = hlsliName.substr(0, hlsliName.size() - srExt.size()) + ".hlsli";
        }
        out << "#include \"" << hlsliName << "\"\n";
    }
    if (!pr.m_DirectIncludes.empty())
        out << "\n";

    out << "namespace srrhi\n{\n\n";

    // Emit named struct definitions — skip those that came from included files.
    VerboseMsg("[hlsl_gen]   Emitting %zu struct(s)...\n", pr.m_Structs.size());
    for (const auto& st : pr.m_Structs)
    {
        if (pr.m_IncludedStructNames.count(st.m_Name)) continue;
        int localPadCount = 0;
        EmitStructHlsl(out, st, localPadCount, 0, nullptr);
    }

    // Build a set of cbuffer names that are in srinput scopes (including transitive nested srinputs)
    std::unordered_set<std::string> cbuffersInSrInput;
    for (const auto& srInputDef : pr.m_SrInputDefs)
    {
        FlatSrInput flat = FlattenSrInput(srInputDef, pr.m_SrInputDefs);
        for (const auto& member : flat.m_Members)
            cbuffersInSrInput.insert(member.m_CBufferName);
    }

    // Emit cbuffer definitions as structs (only for those in srinput scopes)
    VerboseMsg("[hlsl_gen]   Emitting %zu cbuffer definition(s) as struct(s)...\n",
               cbuffersInSrInput.size());
    for (const auto& bufDef : pr.m_BufferDefs)
    {
        if (cbuffersInSrInput.count(bufDef.m_Name))
        {
            const LayoutMember* bufLayout = nullptr;
            for (const auto& lm : layouts)
            {
                if (lm.m_Name == bufDef.m_Name) { bufLayout = &lm; break; }
            }
            int localPadCount = 0;
            EmitStructHlsl(out, bufDef, localPadCount, 0, bufLayout);
        }
    }

    // Emit cbuffers that are in srinput scopes.
    // Register numbers are local to each srinput scope (reset per srinput).
    // Composition: flatten the srinput hierarchy so registers are assigned uniquely.
    // Space is always taken from the top-level parent srinput.
    VerboseMsg("[hlsl_gen]   Emitting cbuffers with register bindings...\n");
    for (const auto& srInputDef : pr.m_SrInputDefs)
    {
        FlatSrInput flat = FlattenSrInput(srInputDef, pr.m_SrInputDefs);
        int regNum = 0;  // b# counter: local to this srinput scope, resets per srinput
        for (const auto& member : flat.m_Members)
        {
            const std::string cleanedMemberName = CleanMemberName(member.m_MemberName);
            const std::string varName = srInputDef.m_Name + "_" + cleanedMemberName;

            // Find the cbuffer definition
            StructType* bufDef = nullptr;
            for (const auto& bd : pr.m_BufferDefs)
            {
                if (bd.m_Name == member.m_CBufferName)
                {
                    bufDef = const_cast<StructType*>(&bd);
                    break;
                }
            }

            if (bufDef)
            {
                const LayoutMember* correspondingLayout = nullptr;
                for (const auto& lm : layouts)
                {
                    if (lm.m_Name == member.m_CBufferName)
                    {
                        correspondingLayout = &lm;
                        break;
                    }
                }

                if (correspondingLayout)
                {
                    int localPadCount = 0;
                    // Space is always from the top-level parent srinput (not the nested one)
                    EmitWrappedCBufferHlsl(out, *bufDef, *correspondingLayout,
                                          regNum, varName, srInputDef.m_RegisterSpace, localPadCount);
                }
            }

            // Always advance — push constants occupy b0 and advance to b1, etc.
            ++regNum;
        }
    }

    // Emit per-srinput resource declarations (SRV/UAV globals with register bindings).
    // SRV and UAV register counters are local to each srinput scope (reset per scope).
    // Composition: use flattened view; space is always from the top-level parent.
    VerboseMsg("[hlsl_gen]   Emitting resource declarations...\n");
    for (const auto& srInputDef : pr.m_SrInputDefs)
    {
        FlatSrInput flat = FlattenSrInput(srInputDef, pr.m_SrInputDefs);
        int srvReg = 0;
        int uavReg = 0;
        for (const auto& rm : flat.m_Resources)
        {
            const std::string cleanedName = CleanMemberName(rm.m_MemberName);
            const std::string globalVarName = srInputDef.m_Name + "_" + cleanedName;
            bool bUAV = IsUAV(rm.m_Kind);
            int regNum = bUAV ? uavReg++ : srvReg++;
            out << rm.m_TypeName << " " << globalVarName
                << " : register(" << (bUAV ? "u" : "t") << regNum;
            if (srInputDef.m_RegisterSpace >= 0)
                out << ", space" << srInputDef.m_RegisterSpace;
            out << ");\n";
        }
        if (!flat.m_Resources.empty())
            out << "\n";
    }

    // Emit per-srinput sampler declarations (s# registers, local per scope).
    VerboseMsg("[hlsl_gen]   Emitting sampler declarations...\n");
    for (const auto& srInputDef : pr.m_SrInputDefs)
    {
        FlatSrInput flat = FlattenSrInput(srInputDef, pr.m_SrInputDefs);
        int samplerReg = 0;
        for (const auto& sm : flat.m_Samplers)
        {
            const std::string cleanedName = CleanMemberName(sm.m_MemberName);
            const std::string globalVarName = srInputDef.m_Name + "_" + cleanedName;
            out << sm.m_TypeName << " " << globalVarName
                << " : register(s" << samplerReg++;
            if (srInputDef.m_RegisterSpace >= 0)
                out << ", space" << srInputDef.m_RegisterSpace;
            out << ");\n";
        }
        if (!flat.m_Samplers.empty())
            out << "\n";
    }

    // Emit per-srinput namespaces with getter functions.
    // For composed srinputs, flattened members/resources/samplers/consts all land
    // in the top-level parent's namespace under the parent's global variable names.
    for (const auto& srInputDef : pr.m_SrInputDefs)
    {
        FlatSrInput flat = FlattenSrInput(srInputDef, pr.m_SrInputDefs);
        out << "namespace " << srInputDef.m_Name << "\n{\n";

        // Cbuffer getters
        for (const auto& member : flat.m_Members)
        {
            const std::string cleanedName = CleanMemberName(member.m_MemberName);
            const std::string varName = srInputDef.m_Name + "_" + cleanedName;
            out << "    " << member.m_CBufferName << " Get" << cleanedName
                << "() { return " << varName << "; }\n";
        }

        // Resource getter functions
        for (const auto& rm : flat.m_Resources)
        {
            const std::string cleanedName = CleanMemberName(rm.m_MemberName);
            const std::string globalVarName = srInputDef.m_Name + "_" + cleanedName;
            out << "    " << rm.m_TypeName << " Get" << cleanedName
                << "() { return " << globalVarName << "; }\n";
        }

        // Sampler getter functions
        for (const auto& sm : flat.m_Samplers)
        {
            const std::string cleanedName = CleanMemberName(sm.m_MemberName);
            const std::string globalVarName = srInputDef.m_Name + "_" + cleanedName;
            out << "    " << sm.m_TypeName << " Get" << cleanedName
                << "() { return " << globalVarName << "; }\n";
        }

        // Scalar constants as static const declarations
        for (const auto& sc : flat.m_ScalarConsts)
        {
            // Map C-style type aliases to canonical HLSL type names
            std::string hlslType = sc.m_TypeName;
            if (hlslType == "int32_t")        hlslType = "int";
            else if (hlslType == "uint32_t")  hlslType = "uint";
            else if (hlslType == "float32_t") hlslType = "float";
            else if (hlslType == "float64_t") hlslType = "double";
            out << "    static const " << hlslType << " " << sc.m_Name
                << " = " << sc.m_Value << ";\n";
        }

        out << "}\n\n";
    }

    out << "\n}  // namespace srrhi\n";

    VerboseMsg("[hlsl_gen] Done (padCount=%d)\n", padCount);
    return out.str();
}
