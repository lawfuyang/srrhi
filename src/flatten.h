#pragma once
#include "types.h"
#include <vector>
#include <unordered_set>

// ---------------------------------------------------------------------------
// HasPushConstantInHierarchy
//   Returns true if `srInput` or any of its transitively nested srinputs
//   declares a [push_constant] cbuffer member.
// ---------------------------------------------------------------------------
inline bool HasPushConstantInHierarchy(const SrInputDef& srInput,
                                        const std::vector<SrInputDef>& allSrInputDefs)
{
    for (const auto& m : srInput.m_Members)
        if (m.m_bIsPushConstant) return true;

    for (const auto& ref : srInput.m_NestedSrInputs)
    {
        for (const auto& other : allSrInputDefs)
        {
            if (other.m_Name == ref.m_SrInputName)
            {
                if (HasPushConstantInHierarchy(other, allSrInputDefs))
                    return true;
                break;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// FlattenSrInput
//   Recursively flattens the srinput composition hierarchy of `srInput` into
//   a FlatSrInput, expanding nested srinput refs in declaration order (DFS).
//
//   Body item kinds (matching SrInputDef::BodyItem::kind):
//     0 = CBuffer, 1 = Resource, 2 = Sampler, 3 = ScalarConst, 4 = NestedRef
//
//   Register space: the caller (generator) always uses the top-level parent's
//   space — FlatSrInput does not carry a space; generators read it from the
//   original top-level SrInputDef.m_RegisterSpace.
// ---------------------------------------------------------------------------
inline FlatSrInput FlattenSrInput(const SrInputDef& srInput,
                                   const std::vector<SrInputDef>& allSrInputDefs)
{
    FlatSrInput result;

    // 1. Flatten inherited bases first, in declaration order.
    //    Inherited content is prepended before this srinput's own body items.
    for (const auto& baseName : srInput.m_BaseInheritances)
    {
        for (const auto& other : allSrInputDefs)
        {
            if (other.m_Name == baseName)
            {
                FlatSrInput baseFlat = FlattenSrInput(other, allSrInputDefs);
                for (auto& m  : baseFlat.m_Members)      result.m_Members.push_back(std::move(m));
                for (auto& r  : baseFlat.m_Resources)    result.m_Resources.push_back(std::move(r));
                for (auto& s  : baseFlat.m_Samplers)     result.m_Samplers.push_back(std::move(s));
                for (auto& sc : baseFlat.m_ScalarConsts) result.m_ScalarConsts.push_back(std::move(sc));
                break;
            }
        }
    }

    // 2. Then process this srinput's own body items in declaration order.
    for (const auto& item : srInput.m_BodyOrder)
    {
        switch (item.kind)
        {
            case 0: // CBuffer
                result.m_Members.push_back(srInput.m_Members[item.idx]);
                break;

            case 1: // Resource
                result.m_Resources.push_back(srInput.m_Resources[item.idx]);
                break;

            case 2: // Sampler
                result.m_Samplers.push_back(srInput.m_Samplers[item.idx]);
                break;

            case 3: // ScalarConst
                result.m_ScalarConsts.push_back(srInput.m_ScalarConsts[item.idx]);
                break;

            case 4: // NestedRef — expand the referenced srinput in-place
            {
                const SrInputRef& ref = srInput.m_NestedSrInputs[item.idx];
                for (const auto& other : allSrInputDefs)
                {
                    if (other.m_Name == ref.m_SrInputName)
                    {
                        FlatSrInput nested = FlattenSrInput(other, allSrInputDefs);
                        for (auto& m  : nested.m_Members)      result.m_Members.push_back(std::move(m));
                        for (auto& r  : nested.m_Resources)    result.m_Resources.push_back(std::move(r));
                        for (auto& s  : nested.m_Samplers)     result.m_Samplers.push_back(std::move(s));
                        for (auto& sc : nested.m_ScalarConsts) result.m_ScalarConsts.push_back(std::move(sc));
                        break;
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    return result;
}
