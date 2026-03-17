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

    void LayoutBuiltin(const BuiltinType& bt, const std::string& name,
                       LayoutMember& parent)
    {
        AlignTo(bt.m_Alignment);
        int size = bt.m_ElementSize * bt.m_VectorSize;
        // If this element would cross a 16-byte boundary, align to 16 first
        if ((m_CurOffset + size - 1) / 16 > m_CurOffset / 16)
            AlignTo16();

        LayoutMember m;
        m.m_Type   = bt;
        m.m_Name   = name;
        m.m_Offset = m_CurOffset;
        m.m_Size   = size;
        parent.PushSubmember(std::move(m));
        m_CurOffset += size;
    }

    void LayoutArray(const ArrayNode& arr, const std::string& name,
                     LayoutMember& parent)
    {
        AlignTo16();
        int startOffset = m_CurOffset;

        LayoutMember array;
        array.m_Type   = std::make_shared<ArrayNode>(arr); // copy
        array.m_Name   = name;
        array.m_Offset = m_CurOffset;

        for (int i = 0; i < arr.m_ArraySize; ++i)
        {
            AlignTo16();
            LayoutType(arr.m_ElementType,
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
        layout.m_Type   = const_cast<StructType*>(&st);
        layout.m_Name   = name;
        layout.m_Offset = m_CurOffset;

        for (auto& member : st.m_Members)
            LayoutType(member.m_Type, member.m_Name, layout);

        // NOTE: size is NOT rounded to multiple of 16
        layout.m_Size = m_CurOffset - startOffset;
        return layout;
    }

    void LayoutType(const TypeRef& type, const std::string& name,
                    LayoutMember& parent)
    {
        if (auto* bt = std::get_if<BuiltinType>(&type))
        {
            LayoutBuiltin(*bt, name, parent);
        }
        else if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&type))
        {
            LayoutArray(**ap, name, parent);
        }
        else if (auto* sp = std::get_if<StructType*>(&type))
        {
            LayoutMember lm = LayoutStruct(**sp, name);
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
    LogMsg("[layout] Computing layouts for %zu buffer(s)...\n", pr.m_Buffers.size());

    std::vector<LayoutMember> result;
    CBufferLayout algo;

    for (auto& bufMv : pr.m_Buffers)
    {
        if (!bufMv.m_bIsCBuffer) continue; // skip sbuffers for now

        auto* st = std::get_if<StructType*>(&bufMv.m_Type);
        if (!st || !*st)
        {
            LogMsg("[layout]   WARNING: buffer has non-struct type, skipping\n");
            continue;
        }

        StructType& bufStruct = **st;
        LogMsg("[layout]   Laying out: %s\n", bufStruct.m_Name.c_str());

        LayoutMember lm = algo.Generate(bufStruct);
        lm.m_Name      = bufStruct.m_Name;  // cbuffer name from the buffer struct
        lm.m_bIsCBuffer = true;
        lm.m_bIsGlobal  = true;

        LogMsg("[layout]     Total size: %d bytes\n", lm.m_Size);
        result.push_back(std::move(lm));
    }

    LogMsg("[layout] Done\n");

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
