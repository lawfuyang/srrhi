#include "types.h"

// ---------------------------------------------------------------------------
// CBuffer layout algorithm
//
// Rules (same as HLSL cbuffer packing):
//  - BuiltinType: align to type.alignment (scalar size), then check if the
//    FULL size crosses a 16-byte register boundary and, if so, re-align to 16.
//  - ArrayType:   align to 16, then each element starts at a new 16-byte boundary.
//  - StructType:  align to 16 (AlignOffsetTo16 before layout).
//  - Struct size  = actual cursor advance (NOT padded to multiple of 16).
// ---------------------------------------------------------------------------

class CBufferLayout
{
    int curOffset = 0;

    void alignTo16()
    {
        curOffset = (curOffset + 15) & ~15;
    }

    void alignTo(int align)
    {
        if (align > 0)
            curOffset = (curOffset + align - 1) & ~(align - 1);
    }

    void layoutBuiltin(const BuiltinType& bt, const std::string& name,
                       LayoutMember& parent)
    {
        alignTo(bt.alignment);
        int size = bt.elementsize * bt.vectorsize;
        // If this element would cross a 16-byte boundary, align to 16 first
        if ((curOffset + size - 1) / 16 > curOffset / 16)
            alignTo16();

        LayoutMember m;
        m.type   = bt;
        m.name   = name;
        m.offset = curOffset;
        m.size   = size;
        parent.pushSubmember(std::move(m));
        curOffset += size;
    }

    void layoutArray(const ArrayNode& arr, const std::string& name,
                     LayoutMember& parent)
    {
        alignTo16();
        int startOffset = curOffset;

        LayoutMember array;
        array.type   = std::make_shared<ArrayNode>(arr); // copy
        array.name   = name;
        array.offset = curOffset;

        for (int i = 0; i < arr.arraySize; ++i)
        {
            alignTo16();
            layoutType(arr.elementType,
                       name + "[" + std::to_string(i) + "]",
                       array);
        }

        array.size = curOffset - startOffset;
        parent.pushSubmember(std::move(array));
    }

    LayoutMember layoutStruct(const StructType& st, const std::string& name)
    {
        alignTo16();
        int startOffset = curOffset;

        LayoutMember layout;
        layout.type   = const_cast<StructType*>(&st);
        layout.name   = name;
        layout.offset = curOffset;

        for (auto& member : st.members)
            layoutType(member.type, member.name, layout);

        // NOTE: size is NOT rounded to multiple of 16
        layout.size = curOffset - startOffset;
        return layout;
    }

    void layoutType(const TypeRef& type, const std::string& name,
                    LayoutMember& parent)
    {
        if (auto* bt = std::get_if<BuiltinType>(&type))
        {
            layoutBuiltin(*bt, name, parent);
        }
        else if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&type))
        {
            layoutArray(**ap, name, parent);
        }
        else if (auto* sp = std::get_if<StructType*>(&type))
        {
            LayoutMember lm = layoutStruct(**sp, name);
            parent.pushSubmember(std::move(lm));
        }
    }

public:
    LayoutMember generate(const StructType& bufferStruct)
    {
        curOffset = 0;
        LayoutMember layout = layoutStruct(bufferStruct, "");
        layout.isCBuffer = true;
        layout.isGlobal  = true;
        return layout;
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::vector<LayoutMember> computeLayouts(ParseResult& pr)
{
    logMsg("[layout] Computing layouts for %zu buffer(s)...\n", pr.buffers.size());

    std::vector<LayoutMember> result;
    CBufferLayout algo;

    for (auto& bufMv : pr.buffers)
    {
        if (!bufMv.isCBuffer) continue; // skip sbuffers for now

        auto* st = std::get_if<StructType*>(&bufMv.type);
        if (!st || !*st)
        {
            logMsg("[layout]   WARNING: buffer has non-struct type, skipping\n");
            continue;
        }

        StructType& bufStruct = **st;
        logMsg("[layout]   Laying out: %s\n", bufStruct.name.c_str());

        LayoutMember lm = algo.generate(bufStruct);
        lm.name      = bufStruct.name;  // cbuffer name from the buffer struct
        lm.isCBuffer = true;
        lm.isGlobal  = true;

        logMsg("[layout]     Total size: %d bytes\n", lm.size);
        result.push_back(std::move(lm));
    }

    logMsg("[layout] Done\n");
    return result;
}