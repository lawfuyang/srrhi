#pragma once

#include <string>
#include <cctype>
#include <cstring>

// ---------------------------------------------------------------------------
// Name-cleaning helpers: strip common prefixes (m_, g_, s_) and capitalize
// ---------------------------------------------------------------------------

inline std::string StripCommonPrefixes(const std::string& name)
{
    const char* prefixes[] = {"m_", "g_", "s_"};
    for (auto* p : prefixes)
    {
        size_t plen = std::strlen(p);
        if (name.size() > plen && name.substr(0, plen) == p)
            return name.substr(plen);
    }
    return name;
}

inline std::string CapitalizeFirst(const std::string& s)
{
    if (s.empty()) return s;
    std::string r = s;
    r[0] = (char)std::toupper((unsigned char)r[0]);
    return r;
}

inline std::string CleanMemberName(const std::string& name)
{
    return CapitalizeFirst(StripCommonPrefixes(name));
}
