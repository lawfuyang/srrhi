#include "types.h"
#include <stdexcept>
#include <string>

// ---------------------------------------------------------------------------
// CBuffer layout algorithm
//
// Rules (same as HLSL cbuffer packing):
//  - BuiltinType: align to type.m_Alignment (scalar size), then check if the
//    FULL size crosses a 16-byte register boundary and, if so, re-align to 16.
//  - ArrayType:   align to 16, then each element starts at a new 16-byte boundary.
//  - StructType:  align to 16 (AlignOffsetTo16 before layout).
//  - Struct size  = actual cursor advance (NOT padded to multiple of 16).
// ---------------------------------------------------------------------------

class CBufferLayout
{
    int m_CurOffset = 0;

    void AlignTo16()
    {
        m_CurOffset = (m_CurOffset + 15) & ~15;
    }

    void AlignTo(int align)
    {
        if (align > 0)
            m_CurOffset = (m_CurOffset + align - 1) & ~(align - 1);
    }

    void LayoutBuiltin(const BuiltinTypeRef& bt, const std::string& name,
                       LayoutMember& parent)
    {
        AlignTo(bt.Alignment());
        int size = bt.ElementSize() * bt.VectorSize();
        // If this element would cross a 16-byte boundary, align to 16 first
        if ((m_CurOffset + size - 1) / 16 > m_CurOffset / 16)
            AlignTo16();

        LayoutMember m;
        m.m_Type   = bt.Clone();
        m.m_Name   = name;
        m.m_Offset = m_CurOffset;
        m.m_Size   = size;
        parent.PushSubmember(std::move(m));
        m_CurOffset += size;
    }

    void LayoutArray(const ArrayTypeRef& arr, const std::string& name,
                     LayoutMember& parent)
    {
        AlignTo16();
        int startOffset = m_CurOffset;

        LayoutMember array;
        array.m_Type   = arr.Clone();
        array.m_Name   = name;
        array.m_Offset = m_CurOffset;

        for (int i = 0; i < arr.ArraySize(); ++i)
        {
            AlignTo16();
            LayoutType(arr.ElementType(),
                       name + "[" + std::to_string(i) + "]",
                       array);
        }

        array.m_Size = m_CurOffset - startOffset;
        parent.PushSubmember(std::move(array));
    }

    LayoutMember LayoutStruct(const StructType& st, const std::string& name)
    {
        AlignTo16();
        int startOffset = m_CurOffset;

        LayoutMember layout;
        auto srRef = std::make_shared<StructTypeRef>();
        srRef->m_Struct = const_cast<StructType*>(&st);
        layout.m_Type   = std::move(srRef);
        layout.m_Name   = name;
        layout.m_Offset = m_CurOffset;

        for (auto& member : st.m_Members)
            LayoutType(member.m_Type, member.m_Name, layout);

        // NOTE: size is NOT rounded to multiple of 16
        layout.m_Size = m_CurOffset - startOffset;
        return layout;
    }

    void LayoutType(const std::shared_ptr<TypeRef>& type, const std::string& name,
                    LayoutMember& parent)
    {
        if (!type) return;
        if (type->IsBuiltin())
        {
            LayoutBuiltin(static_cast<const BuiltinTypeRef&>(*type), name, parent);
        }
        else if (type->IsArray())
        {
            LayoutArray(static_cast<const ArrayTypeRef&>(*type), name, parent);
        }
        else if (type->IsStruct())
        {
            LayoutMember lm = LayoutStruct(*static_cast<const StructTypeRef&>(*type).m_Struct, name);
            parent.PushSubmember(std::move(lm));
        }
        else if (type->IsExtern())
        {
            // ExternType: the actual sizeof is not known at generation time.
            // srrhi assumes the type satisfies both:
            //   - sizeof(T) % 16 == 0  (size is a multiple of one HLSL register)
            //   - alignof(T) >= 16     (16-byte aligned, matching cbuffer packing rules)
            // These constraints are enforced by static_asserts in the generated C++ header.
            // We align to 16 before placement to reflect the alignment assumption.
            // m_CurOffset is NOT advanced after placement because sizeof is unknown;
            // offsets for any fields that follow an extern member are therefore approximate.
            AlignTo16();
            LayoutMember lm;
            lm.m_Type   = type->Clone();
            lm.m_Name   = name;
            lm.m_Offset = m_CurOffset;
            lm.m_Size   = 0;
            parent.PushSubmember(std::move(lm));
        }
    }

public:
    LayoutMember Generate(const StructType& bufferStruct)
    {
        m_CurOffset = 0;
        LayoutMember layout = LayoutStruct(bufferStruct, "");
        layout.m_bIsCBuffer = true;
        layout.m_bIsGlobal  = true;
        return layout;
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::vector<LayoutMember> ComputeLayouts(ParseResult& pr)
{
    VerboseMsg("[layout] Computing layouts for %zu buffer(s)...\n", pr.m_Buffers.size());

    std::vector<LayoutMember> result;
    CBufferLayout algo;

    for (auto& bufMv : pr.m_Buffers)
    {
        if (!bufMv.m_bIsCBuffer) continue; // skip sbuffers for now

        auto* st = bufMv.m_Type && bufMv.m_Type->IsStruct()
            ? static_cast<StructTypeRef*>(bufMv.m_Type.get()) : nullptr;
        if (!st || !st->m_Struct)
        {
            VerboseMsg("[layout]   WARNING: buffer has non-struct type, skipping\n");
            continue;
        }

        StructType& bufStruct = *st->m_Struct;
        VerboseMsg("[layout]   Laying out: %s\n", bufStruct.m_Name.c_str());

        LayoutMember lm = algo.Generate(bufStruct);
        lm.m_Name      = bufStruct.m_Name;  // cbuffer name from the buffer struct
        lm.m_bIsCBuffer = true;
        lm.m_bIsGlobal  = true;

        VerboseMsg("[layout]     Total size: %d bytes\n", lm.m_Size);
        result.push_back(std::move(lm));
    }

    VerboseMsg("[layout] Done\n");

    // Validate push constant sizes after all layouts are computed.
    // D3D12 push constants (root constants) must be < 256 bytes.
    for (const auto& srInputDef : pr.m_SrInputDefs)
    {
        for (const auto& member : srInputDef.m_Members)
        {
            if (!member.m_bIsPushConstant) continue;
            for (const auto& lm : result)
            {
                if (lm.m_Name == member.m_CBufferName)
                {
                    if (lm.m_Size >= 256)
                    {
                        LogMsg("[layout] ERROR: push constant '%s' in srinput '%s' is %d bytes"
                               " (padded); must be < 256 bytes\n",
                               member.m_CBufferName.c_str(),
                               srInputDef.m_Name.c_str(), lm.m_Size);
                        throw std::runtime_error(
                            "push constant '" + member.m_CBufferName +
                            "' in srinput '" + srInputDef.m_Name +
                            "' has padded byte size " + std::to_string(lm.m_Size) +
                            " which must be < 256 bytes");
                    }
                    break;
                }
            }
        }
    }

    return result;
}
