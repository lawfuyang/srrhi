#include "types.h"
#include "flatten.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Include tracking to prevent circular dependencies
// ---------------------------------------------------------------------------
static thread_local std::unordered_set<std::string> g_IncludedFiles;

static std::string ResolveIncludePath(const std::string& baseFile,
                                      const std::string& includeFile)
{
    fs::path baseDirPath = fs::path(baseFile).parent_path();
    return fs::absolute(baseDirPath / includeFile).string();
}

// ---------------------------------------------------------------------------
// Built-in scalar type table
// Ordered longest-names-first so prefix matching works correctly.
// ---------------------------------------------------------------------------
struct ScalarInfo { std::string m_Name; int m_ElementSize; };

static const std::vector<std::pair<std::string, ScalarInfo>> g_Scalars = {
    { "float16_t", { "float16_t", 2 } },
    { "float32_t", { "float32_t", 4 } },
    { "float64_t", { "float64_t", 8 } },
    { "int16_t",   { "int16_t",   2 } },
    { "uint16_t",  { "uint16_t",  2 } },
    { "int32_t",   { "int32_t",   4 } },
    { "uint32_t",  { "uint32_t",  4 } },
    { "int64_t",   { "int64_t",   8 } },
    { "uint64_t",  { "uint64_t",  8 } },
    { "double",    { "double",    8 } },
    { "float",     { "float",     4 } },
    { "bool",      { "bool",      4 } },
    { "int",       { "int",       4 } },
    { "uint",      { "uint",      4 } },
};

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------
enum class TokKind
{
    Ident, Number,
    LBrace, RBrace, LBracket, RBracket, LAngle, RAngle,
    Semicolon, Comma, Colon, Hash, String,
    Unknown, Eof
};

struct Token { TokKind m_Kind; std::string m_Text; int m_Line; };

struct Lexer
{
    std::string m_Src;
    size_t m_Pos  = 0;
    int    m_Line = 1;

    explicit Lexer(const std::string& s) : m_Src(s) {}

    void SkipWhitespaceAndComments()
    {
        while (m_Pos < m_Src.size())
        {
            char c = m_Src[m_Pos];
            if (c == '\n') { ++m_Line; ++m_Pos; }
            else if (std::isspace((unsigned char)c)) { ++m_Pos; }
            else if (m_Pos + 1 < m_Src.size() && c == '/' && m_Src[m_Pos + 1] == '/')
            {
                while (m_Pos < m_Src.size() && m_Src[m_Pos] != '\n') ++m_Pos;
            }
            else if (m_Pos + 1 < m_Src.size() && c == '/' && m_Src[m_Pos + 1] == '*')
            {
                m_Pos += 2;
                while (m_Pos + 1 < m_Src.size() &&
                       !(m_Src[m_Pos] == '*' && m_Src[m_Pos + 1] == '/'))
                {
                    if (m_Src[m_Pos] == '\n') ++m_Line;
                    ++m_Pos;
                }
                m_Pos += 2;
            }
            else break;
        }
    }

    Token Next()
    {
        SkipWhitespaceAndComments();
        if (m_Pos >= m_Src.size()) return { TokKind::Eof, "", m_Line };

        int startLine = m_Line;
        char c = m_Src[m_Pos];

        if (c == '"')
        {
            ++m_Pos;
            size_t start = m_Pos;
            while (m_Pos < m_Src.size() && m_Src[m_Pos] != '"')
            {
                if (m_Src[m_Pos] == '\n') ++m_Line;
                ++m_Pos;
            }
            std::string str = m_Src.substr(start, m_Pos - start);
            if (m_Pos < m_Src.size()) ++m_Pos;
            return { TokKind::String, str, startLine };
        }

        if (std::isalpha((unsigned char)c) || c == '_')
        {
            size_t start = m_Pos;
            while (m_Pos < m_Src.size() &&
                   (std::isalnum((unsigned char)m_Src[m_Pos]) || m_Src[m_Pos] == '_'))
                ++m_Pos;
            return { TokKind::Ident, m_Src.substr(start, m_Pos - start), startLine };
        }

        if (std::isdigit((unsigned char)c))
        {
            size_t start = m_Pos;
            while (m_Pos < m_Src.size() && std::isdigit((unsigned char)m_Src[m_Pos]))
                ++m_Pos;
            return { TokKind::Number, m_Src.substr(start, m_Pos - start), startLine };
        }

        ++m_Pos;
        switch (c)
        {
            case '{': return { TokKind::LBrace,    "{", startLine };
            case '}': return { TokKind::RBrace,    "}", startLine };
            case '[': return { TokKind::LBracket,  "[", startLine };
            case ']': return { TokKind::RBracket,  "]", startLine };
            case '<': return { TokKind::LAngle,    "<", startLine };
            case '>': return { TokKind::RAngle,    ">", startLine };
            case ';': return { TokKind::Semicolon, ";", startLine };
            case ',': return { TokKind::Comma,     ",", startLine };
            case ':': return { TokKind::Colon,     ":", startLine };
            case '#': return { TokKind::Hash,      "#", startLine };
            default:  return { TokKind::Unknown, std::string(1, c), startLine };
        }
    }

    Token Peek()
    {
        size_t s = m_Pos; int l = m_Line;
        Token t  = Next();
        m_Pos = s; m_Line = l;
        return t;
    }
};

// ---------------------------------------------------------------------------
// Forward declaration
// ---------------------------------------------------------------------------
static ParseResult ParseFileInternal(const std::string& path);

// ---------------------------------------------------------------------------
// Pointer fixup helpers for include merging
// When structs are moved from inc.m_Structs → result.m_Structs, any StructType*
// inside the moved structs must be remapped to the new addresses.
// ---------------------------------------------------------------------------
static void FixupTypeRef(TypeRef& t,
                         const std::unordered_map<StructType*, StructType*>& remap)
{
    if (auto* sp = std::get_if<StructType*>(&t))
    {
        auto it = remap.find(*sp);
        if (it != remap.end()) *sp = it->second;
    }
    else if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&t))
    {
        FixupTypeRef((*ap)->m_ElementType, remap);
    }
}

static void FixupMembers(std::vector<MemberVariable>& members, const std::unordered_map<StructType*, StructType*>& remap)
{
    for (auto& mv : members)
        FixupTypeRef(mv.m_Type, remap);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
struct Parser
{
    Lexer        m_Lex;
    Token        m_Cur;
    ParseResult& m_Result;
    std::string  m_FilePath;

    // Maps struct name to pointer into m_Result.m_Structs (stable due to deque)
    std::unordered_map<std::string, StructType*> m_StructMap;

    // Maps srinput name to index in m_Result.m_SrInputDefs (indices are stable)
    std::unordered_map<std::string, size_t> m_SrInputMap;

    Parser(const std::string& src, ParseResult& r, const std::string& path)
        : m_Lex(src), m_Result(r), m_FilePath(path)
    {
        Advance();
    }

    void Advance() { m_Cur = m_Lex.Next(); }

    Token Expect(TokKind k, const std::string& what)
    {
        if (m_Cur.m_Kind != k)
            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                     ": expected " + what + ", got '" + m_Cur.m_Text + "'");
        Token t = m_Cur; Advance(); return t;
    }

    bool Check(TokKind k) const { return m_Cur.m_Kind == k; }
    bool TryConsume(TokKind k)
    {
        if (m_Cur.m_Kind != k) return false;
        Advance(); return true;
    }

    int ParseInteger()
    {
        Token t = Expect(TokKind::Number, "integer");
        return std::stoi(t.m_Text);
    }

    // -----------------------------------------------------------------------
    // Include handling
    // -----------------------------------------------------------------------
    // Called with m_Cur already pointing at the "include" identifier token.
    // hashLine is the source line of the leading '#' (for error messages).
    void ParseInclude(int hashLine)
    {
        // m_Cur == TokKind::Ident("include")  — already verified by caller
        Advance(); // consume "include"; m_Cur is now the filename string

        if (m_Cur.m_Kind != TokKind::String)
            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                     ": expected filename after '#include'");
        std::string includeFile = m_Cur.m_Text;
        Advance();

        std::string resolvedPath = ResolveIncludePath(m_FilePath, includeFile);
        if (g_IncludedFiles.count(resolvedPath))
            throw std::runtime_error(m_FilePath + ":" + std::to_string(hashLine) +
                                     ": circular include: " + resolvedPath);

        g_IncludedFiles.insert(resolvedPath);
        ParseResult inc = ParseFileInternal(resolvedPath);

        // Collect old addresses and names BEFORE any moves (deque elements
        // are at fixed addresses, but their contents will be moved-from).
        std::vector<std::pair<StructType*, std::string>> entries;
        entries.reserve(inc.m_Structs.size());
        for (auto& st : inc.m_Structs)
            entries.push_back({ &st, st.m_Name });

        // Move structs into result and build old→new remap.
        std::unordered_map<StructType*, StructType*> remap;
        remap.reserve(entries.size());
        for (auto& [oldPtr, name] : entries)
        {
            if (m_StructMap.count(name))
                throw std::runtime_error(m_FilePath + ":" + std::to_string(hashLine) +
                                         ": struct '" + name + "' already defined");
            m_Result.m_Structs.push_back(std::move(*oldPtr));
            StructType* newPtr = &m_Result.m_Structs.back();
            remap[oldPtr]   = newPtr;
            m_StructMap[name] = newPtr;
        }

        // Fix up StructType* pointers inside the newly merged struct members.
        // They may have pointed to structs within `inc.m_Structs` (now stale).
        size_t baseIdx = m_Result.m_Structs.size() - entries.size();
        for (size_t i = baseIdx; i < m_Result.m_Structs.size(); ++i)
            FixupMembers(m_Result.m_Structs[i].m_Members, remap);

        // Track include for code-gen: record the direct include and all struct
        // names from this include (including its own transitive includes) so
        // generators can emit a #include directive instead of re-emitting defs.
        m_Result.m_DirectIncludes.push_back(includeFile);
        for (auto& [oldPtr, name] : entries)
            m_Result.m_IncludedStructNames.insert(name);
        for (const auto& name : inc.m_IncludedStructNames)
            m_Result.m_IncludedStructNames.insert(name);
    }

    // -----------------------------------------------------------------------
    // Type building helpers
    // -----------------------------------------------------------------------
    TypeRef MakeBuiltin(const ScalarInfo& si, int vectorSize,
                        bool bCreatedFromMatrix = false) const
    {
        BuiltinType bt;
        bt.m_ScalarName         = si.m_Name;
        bt.m_ElementSize        = si.m_ElementSize;
        bt.m_Alignment          = si.m_ElementSize;
        bt.m_VectorSize         = vectorSize;
        bt.m_bCreatedFromMatrix= bCreatedFromMatrix;
        bt.m_Name = (vectorSize == 1) ? si.m_Name : si.m_Name + std::to_string(vectorSize);
        return bt;
    }

    TypeRef MakeArray(TypeRef elemType, int arraySize,
                      bool bCreatedFromMatrix = false) const
    {
        auto node = std::make_shared<ArrayNode>();
        node->m_ElementType          = elemType;
        node->m_ArraySize            = arraySize;
        node->m_bCreatedFromMatrix  = bCreatedFromMatrix;
        node->m_Name = TypeDisplayName(node->m_ElementType) +
                     "[" + std::to_string(arraySize) + "]";
        return node;
    }

    // -----------------------------------------------------------------------
    // matrix<T,r,c> or vector<T,n>
    // -----------------------------------------------------------------------
    TypeRef ParseTemplateType(const std::string& keyword)
    {
        bool bIsMatrix = (keyword == "matrix");
        std::string scalarKey = "float";
        int vectorSize = 4, arraySize = 4;

        if (TryConsume(TokKind::LAngle))
        {
            Token st = Expect(TokKind::Ident, "scalar type");
            bool bFound = false;
            for (auto& [key, info] : g_Scalars)
                if (key == st.m_Text) { scalarKey = key; bFound = true; break; }
            if (!bFound)
                throw std::runtime_error(m_FilePath + ":" + std::to_string(st.m_Line) +
                                         ": unknown scalar '" + st.m_Text + "'");
            if (TryConsume(TokKind::Comma))
                vectorSize = ParseInteger();
            if (bIsMatrix && TryConsume(TokKind::Comma))
                arraySize = ParseInteger();
            Expect(TokKind::RAngle, ">");
        }

        const ScalarInfo* si = nullptr;
        for (auto& [key, info] : g_Scalars)
            if (key == scalarKey) { si = &info; break; }

        if (bIsMatrix)
        {
            TypeRef elem = MakeBuiltin(*si, vectorSize, true);
            if (arraySize == 1) return elem;
            return MakeArray(std::move(elem), arraySize);
        }
        return MakeBuiltin(*si, vectorSize);
    }

    // -----------------------------------------------------------------------
    // NonStructType: optional row/column_major qualifier + type name
    // -----------------------------------------------------------------------
    TypeRef ParseNonStructType()
    {
        bool bIsRowMajor = false;
        if (m_Cur.m_Kind == TokKind::Ident &&
            (m_Cur.m_Text == "row_major" || m_Cur.m_Text == "column_major"))
        {
            bIsRowMajor = (m_Cur.m_Text == "row_major");
            Advance();
        }

        if (m_Cur.m_Kind != TokKind::Ident)
            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                     ": expected type name");

        std::string name = m_Cur.m_Text;
        int nameLine     = m_Cur.m_Line;
        Advance();

        if (name == "matrix" || name == "vector")
            return ParseTemplateType(name);

        // Check scalar type table (longest-match, table is already ordered)
        for (auto& [key, info] : g_Scalars)
        {
            if (name.rfind(key, 0) != 0) continue; // doesn't start with this key

            std::string suffix = name.substr(key.size());

            if (suffix.empty())
            {
                if (bIsRowMajor)
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                             ": row_major/column_major on non-matrix type");
                return MakeBuiltin(info, 1);
            }

            if (suffix.size() == 1 && std::isdigit((unsigned char)suffix[0]))
            {
                int vs = suffix[0] - '0';
                if (vs < 1 || vs > 4)
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                             ": invalid vector size");
                if (bIsRowMajor)
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                             ": row_major/column_major on non-matrix type");
                return MakeBuiltin(info, vs);
            }

            if (suffix.size() == 3 && std::isdigit((unsigned char)suffix[0]) &&
                suffix[1] == 'x'  && std::isdigit((unsigned char)suffix[2]))
            {
                int rows = suffix[0] - '0';
                int cols = suffix[2] - '0';
                if (rows < 1 || rows > 4 || cols < 1 || cols > 4)
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                             ": matrix dimension out of range");
                // Reject row_major/column_major on float4x4
                if (key == "float" && rows == 4 && cols == 4 && bIsRowMajor)
                {
                    LogMsg("[parser] ERROR: row_major/column_major on float4x4 is not allowed at line %d\n", nameLine);
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                             ": row_major/column_major on float4x4 is not allowed");
                }
                int vectorSize = bIsRowMajor ? cols : rows;
                int arraySize  = bIsRowMajor ? rows : cols;
                TypeRef elem = MakeBuiltin(info, vectorSize, true);
                if (arraySize == 1) return elem; // NxM where M=1 is just a vector
                return MakeArray(std::move(elem), arraySize);
            }

            // Starts with key but unrecognised suffix -> fall through to struct lookup
            break;
        }

        // Named struct reference
        auto it = m_StructMap.find(name);
        if (it == m_StructMap.end())
            throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                     ": unrecognized type '" + name + "'");
        return it->second; // StructType*
    }

    // -----------------------------------------------------------------------
    // Array dims: one or more [N], flattened to total count
    // -----------------------------------------------------------------------
    TypeRef ParseArrayDims(TypeRef elemType)
    {
        int total = 1;
        do
        {
            int n = ParseInteger();
            if (n < 1)
                throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                         ": array size must be >= 1");
            total *= n;
            Expect(TokKind::RBracket, "]");
        }
        while (TryConsume(TokKind::LBracket));

        return MakeArray(std::move(elemType), total);
    }

    // -----------------------------------------------------------------------
    // Member variable declaration(s): type name[dims], name[dims]; or ,
    // -----------------------------------------------------------------------
    void ParseMemberVariables(std::vector<MemberVariable>& out)
    {
        // Reject anonymous/inner struct
        if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Text == "struct")
            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                     ": anonymous/inner struct definitions are not supported");

        // Reject sampler types in cbuffer/struct context
        if (m_Cur.m_Kind == TokKind::Ident &&
            (m_Cur.m_Text == "SamplerState" || m_Cur.m_Text == "SamplerComparisonState"))
        {
            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                     ": sampler type '" + m_Cur.m_Text +
                                     "' is not allowed in cbuffer or struct; use it inside an srinput block");
        }

        TypeRef memberType = ParseNonStructType();

        do
        {
            Token nameTok = Expect(TokKind::Ident, "member name");
            TypeRef fieldType = memberType; // copy

            if (TryConsume(TokKind::LBracket))
                fieldType = ParseArrayDims(std::move(fieldType));

            // Check for and reject semantic annotations (e.g., packoffset)
            if (Check(TokKind::Colon))
            {
                Advance(); // consume ':'
                if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Text == "packoffset")
                {
                    int lineNum = m_Cur.m_Line;
                    LogMsg("[parser] ERROR: cbuffer member '%s' uses forbidden 'packoffset' semantic at line %d\n",
                           nameTok.m_Text.c_str(), lineNum);
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(lineNum) +
                                             ": cbuffer members cannot use 'packoffset' semantic");
                }
                // Skip other semantic annotations (like 'register')
                while (!Check(TokKind::Semicolon) && !Check(TokKind::Comma) && !Check(TokKind::Eof))
                    Advance();
            }

            MemberVariable mv;
            mv.m_Type = std::move(fieldType);
            mv.m_Name = nameTok.m_Text;
            
            // Skip explicit padding members (ones named pad*) during parsing
            // The layout engine will auto-generate padding as needed
            if (mv.m_Name.size() < 3 || mv.m_Name.substr(0, 3) != "pad")
            {
                out.push_back(std::move(mv));
            }
        }
        while (TryConsume(TokKind::Comma));

        Expect(TokKind::Semicolon, ";");
    }

    // -----------------------------------------------------------------------
    // Struct body: { member... }
    // -----------------------------------------------------------------------
    std::vector<MemberVariable> ParseStructBody()
    {
        Expect(TokKind::LBrace, "{");
        std::vector<MemberVariable> members;
        std::unordered_set<std::string> memberNames;
        while (!Check(TokKind::RBrace) && !Check(TokKind::Eof))
        {
            size_t prevCount = members.size();
            ParseMemberVariables(members);
            // Check newly added members for duplicate names
            for (size_t i = prevCount; i < members.size(); ++i)
            {
                const std::string& name = members[i].m_Name;
                if (!memberNames.insert(name).second)
                {
                    LogMsg("[parser] ERROR: duplicate member name '%s' in struct at line %d\n",
                           name.c_str(), m_Cur.m_Line);
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                             ": duplicate member name '" + name + "' in struct");
                }
            }
        }
        Expect(TokKind::RBrace, "}");
        return members;
    }

    // -----------------------------------------------------------------------
    // Optional register binding: ": register(rN)" - parse and discard
    // -----------------------------------------------------------------------
    void SkipOptionalRegisterBinding()
    {
        if (!Check(TokKind::Colon)) return;
        Advance(); // ':'
        if (m_Cur.m_Kind != TokKind::Ident || m_Cur.m_Text != "register")
            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                     ": expected 'register' after ':'");
        Advance();
        // '(' is Unknown token since we don't have a dedicated Lparen
        if (m_Cur.m_Kind != TokKind::Unknown || m_Cur.m_Text != "(")
            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                     ": expected '(' after 'register'");
        Advance();
        while (!Check(TokKind::Eof))
        {
            if (m_Cur.m_Kind == TokKind::Unknown && m_Cur.m_Text == ")")
            { Advance(); break; }
            Advance();
        }
    }

    // -----------------------------------------------------------------------
    // Top-level parse loop
    // -----------------------------------------------------------------------
    void Parse()
    {
        while (!Check(TokKind::Eof))
        {
            // #include or other #pragma directives
            if (Check(TokKind::Hash))
            {
                int hashLine = m_Cur.m_Line;
                Advance(); // consume '#'; m_Cur is now the directive keyword

                // Handle #include
                if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Text == "include")
                {
                    // m_Cur is at "include" — ParseInclude picks up from here
                    ParseInclude(hashLine);
                    continue;
                }

                // Reject #pragma pack and #pragma pack_matrix directives
                if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Text == "pragma")
                {
                    Advance(); // consume 'pragma'
                    if (m_Cur.m_Kind == TokKind::Ident && (m_Cur.m_Text == "pack" || m_Cur.m_Text == "pack_matrix"))
                    {
                        std::string pragmaType = m_Cur.m_Text;
                        LogMsg("[parser] ERROR: forbidden '#pragma %s' directive at line %d\n", pragmaType.c_str(), hashLine);
                        throw std::runtime_error(m_FilePath + ":" + std::to_string(hashLine) +
                                                 ": '#pragma " + pragmaType + "' is not allowed in this file format");
                    }
                    // Skip unknown pragmas (advance until next '#' or EOF)
                    while (!Check(TokKind::Eof) && !Check(TokKind::Hash))
                        Advance();
                    continue;
                }

                // Skip unknown hash directives
                while (!Check(TokKind::Eof) && !Check(TokKind::Hash))
                    Advance();
                continue;
            }

            // ---- Optional [space(N)] attribute before 'srinput' ----
            int pendingRegisterSpace = -1;
            if (Check(TokKind::LBracket))
            {
                int attrLine = m_Cur.m_Line;
                Advance(); // consume '['
                if (m_Cur.m_Kind != TokKind::Ident || m_Cur.m_Text != "space")
                {
                    LogMsg("[parser] ERROR: unknown top-level attribute '%s' at line %d\n",
                           m_Cur.m_Text.c_str(), attrLine);
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(attrLine) +
                                             ": unknown top-level attribute '" + m_Cur.m_Text +
                                             "'; only '[space(N)]' is supported before 'srinput'");
                }
                Advance(); // consume 'space'
                if (m_Cur.m_Kind != TokKind::Unknown || m_Cur.m_Text != "(")
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(attrLine) +
                                             ": expected '(' after 'space' in '[space(N)]' attribute");
                Advance(); // consume '('
                pendingRegisterSpace = ParseInteger();
                if (m_Cur.m_Kind != TokKind::Unknown || m_Cur.m_Text != ")")
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(attrLine) +
                                             ": expected ')' in '[space(N)]' attribute");
                Advance(); // consume ')'
                Expect(TokKind::RBracket, "]");
                if (m_Cur.m_Kind != TokKind::Ident || m_Cur.m_Text != "srinput")
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(attrLine) +
                                             ": '[space(N)]' attribute can only be applied to 'srinput' declarations");
            }

            if (m_Cur.m_Kind != TokKind::Ident) { Advance(); continue; }

            std::string kw = m_Cur.m_Text;

            // ---- struct ----
            if (kw == "struct")
            {
                Advance();
                Token nameTok = Expect(TokKind::Ident, "struct name");
                if (m_StructMap.count(nameTok.m_Text))
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(nameTok.m_Line) +
                                             ": struct '" + nameTok.m_Text + "' already defined");

                auto members = ParseStructBody();
                Expect(TokKind::Semicolon, ";");

                m_Result.m_Structs.push_back({ nameTok.m_Text, std::move(members) });
                m_StructMap[nameTok.m_Text] = &m_Result.m_Structs.back();
                continue;
            }

            // ---- cbuffer ----
            if (kw == "cbuffer")
            {
                Advance();
                Token nameTok = Expect(TokKind::Ident, "cbuffer name");
                SkipOptionalRegisterBinding();
                auto members = ParseStructBody();
                TryConsume(TokKind::Semicolon);

                StructType st{ nameTok.m_Text, std::move(members) };
                m_Result.m_BufferDefs.push_back(std::move(st));

                MemberVariable mv;
                mv.m_Type      = &m_Result.m_BufferDefs.back();
                mv.m_Name      = "";
                mv.m_bIsCBuffer = true;
                m_Result.m_Buffers.push_back(std::move(mv));
                continue;
            }

            // ---- ConstantBuffer<T> name; ----
            if (kw == "ConstantBuffer")
            {
                Advance();
                Expect(TokKind::LAngle, "<");
                if (m_Cur.m_Kind != TokKind::Ident)
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                             ": expected struct type in ConstantBuffer<>");
                Token innerTok = m_Cur; Advance();
                auto it = m_StructMap.find(innerTok.m_Text);
                if (it == m_StructMap.end())
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(innerTok.m_Line) +
                                             ": ConstantBuffer<> type '" + innerTok.m_Text +
                                             "' not found");
                Expect(TokKind::RAngle, ">");
                Token varTok = Expect(TokKind::Ident, "variable name");
                while (TryConsume(TokKind::LBracket)) { ParseInteger(); Expect(TokKind::RBracket, "]"); }
                SkipOptionalRegisterBinding();
                Expect(TokKind::Semicolon, ";");

                // Wrap inner struct in outer struct
                StructType outer{ varTok.m_Text, {} };
                MemberVariable innerMv; innerMv.m_Type = it->second; innerMv.m_Name = "";
                outer.m_Members.push_back(std::move(innerMv));
                m_Result.m_BufferDefs.push_back(std::move(outer));

                MemberVariable mv;
                mv.m_Type      = &m_Result.m_BufferDefs.back();
                mv.m_Name      = varTok.m_Text;
                mv.m_bIsCBuffer = true;
                m_Result.m_Buffers.push_back(std::move(mv));
                continue;
            }

            // ---- StructuredBuffer<T> name; ----
            if (kw == "StructuredBuffer")
            {
                Advance();
                Expect(TokKind::LAngle, "<");
                TypeRef templateType = ParseNonStructType();
                Expect(TokKind::RAngle, ">");
                Token varTok = Expect(TokKind::Ident, "variable name");
                while (TryConsume(TokKind::LBracket)) { ParseInteger(); Expect(TokKind::RBracket, "]"); }
                SkipOptionalRegisterBinding();
                Expect(TokKind::Semicolon, ";");

                StructType outer{ varTok.m_Text, {} };
                MemberVariable innerMv; innerMv.m_Type = std::move(templateType); innerMv.m_Name = "";
                outer.m_Members.push_back(std::move(innerMv));
                m_Result.m_BufferDefs.push_back(std::move(outer));

                MemberVariable mv;
                mv.m_Type      = &m_Result.m_BufferDefs.back();
                mv.m_Name      = varTok.m_Text;
                mv.m_bIsSBuffer = true;
                m_Result.m_Buffers.push_back(std::move(mv));
                continue;
            }

            // ---- srinput Name { cbuffers / resources }; ----
            if (kw == "srinput")
            {
                Advance();
                Token srInputName = Expect(TokKind::Ident, "srinput name");

                // ---- Optional inheritance list: : [public|protected|private] Base1, Base2, ... ----
                std::vector<std::string> baseInheritances;
                if (Check(TokKind::Colon))
                {
                    int colonLine = m_Cur.m_Line;
                    Advance(); // consume ':'

                    do
                    {
                        // Skip optional access specifiers (public/protected/private)
                        if (m_Cur.m_Kind == TokKind::Ident &&
                            (m_Cur.m_Text == "public" || m_Cur.m_Text == "protected" || m_Cur.m_Text == "private"))
                        {
                            Advance();
                        }

                        if (m_Cur.m_Kind != TokKind::Ident)
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                                     ": expected base srinput name after ':' in srinput '" +
                                                     srInputName.m_Text + "'");

                        Token baseTok = m_Cur;
                        Advance();

                        // Self-inheritance check
                        if (baseTok.m_Text == srInputName.m_Text)
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(baseTok.m_Line) +
                                                     ": srinput '" + srInputName.m_Text +
                                                     "' cannot inherit from itself");

                        // Forward reference check: base must be previously declared
                        auto baseIt = m_SrInputMap.find(baseTok.m_Text);
                        if (baseIt == m_SrInputMap.end())
                        {
                            LogMsg("[parser] ERROR: srinput '%s' inherits unknown base '%s' at line %d\n",
                                   srInputName.m_Text.c_str(), baseTok.m_Text.c_str(), baseTok.m_Line);
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(baseTok.m_Line) +
                                                     ": srinput '" + srInputName.m_Text +
                                                     "' inherits from '" + baseTok.m_Text +
                                                     "' which has not been declared yet; srinput inheritance requires forward declarations to be declared first");
                        }

                        // Duplicate base check
                        for (const auto& existing : baseInheritances)
                        {
                            if (existing == baseTok.m_Text)
                            {
                                LogMsg("[parser] ERROR: srinput '%s' lists base '%s' more than once at line %d\n",
                                       srInputName.m_Text.c_str(), baseTok.m_Text.c_str(), baseTok.m_Line);
                                throw std::runtime_error(m_FilePath + ":" + std::to_string(baseTok.m_Line) +
                                                         ": srinput '" + srInputName.m_Text +
                                                         "' inherits from '" + baseTok.m_Text +
                                                         "' more than once; duplicate base srinputs are not allowed");
                            }
                        }

                        baseInheritances.push_back(baseTok.m_Text);
                    }
                    while (TryConsume(TokKind::Comma));
                }

                Expect(TokKind::LBrace, "{");

                SrInputDef srInputDef;
                srInputDef.m_Name             = srInputName.m_Text;
                srInputDef.m_RegisterSpace    = pendingRegisterSpace;
                srInputDef.m_BaseInheritances = std::move(baseInheritances);
                std::unordered_set<std::string> srInputMemberNames;
                int pushConstantCount = 0; // enforce max one [push_constant] per srinput body

                // Pre-populate name set with all names from flattened bases,
                // so that body items can be checked for clashes against inherited names.
                for (const auto& baseName : srInputDef.m_BaseInheritances)
                {
                    for (const auto& other : m_Result.m_SrInputDefs)
                    {
                        if (other.m_Name == baseName)
                        {
                            FlatSrInput baseFlat = FlattenSrInput(other, m_Result.m_SrInputDefs);
                            auto checkInheritedName = [&](const std::string& name, int line)
                            {
                                if (!srInputMemberNames.insert(name).second)
                                {
                                    LogMsg("[parser] ERROR: inherited name '%s' from base '%s' clashes with another inherited name in srinput '%s'\n",
                                           name.c_str(), baseName.c_str(), srInputDef.m_Name.c_str());
                                    throw std::runtime_error(m_FilePath + ":" + std::to_string(line) +
                                                             ": inherited name '" + name +
                                                             "' from base '" + baseName +
                                                             "' clashes with a name already inherited by '" +
                                                             srInputDef.m_Name + "'");
                                }
                            };
                            for (const auto& m  : baseFlat.m_Members)
                                checkInheritedName(m.m_MemberName, srInputName.m_Line);
                            for (const auto& r  : baseFlat.m_Resources)
                                checkInheritedName(r.m_MemberName, srInputName.m_Line);
                            for (const auto& s  : baseFlat.m_Samplers)
                                checkInheritedName(s.m_MemberName, srInputName.m_Line);
                            for (const auto& sc : baseFlat.m_ScalarConsts)
                                checkInheritedName(sc.m_Name, srInputName.m_Line);
                            break;
                        }
                    }
                }

                while (!Check(TokKind::RBrace) && !Check(TokKind::Eof))
                {
                    // ---- Attribute annotation: [push_constant] ----
                    bool bNextIsPushConstant = false;
                    if (Check(TokKind::LBracket))
                    {
                        int attrLine = m_Cur.m_Line;
                        Advance(); // consume '['
                        if (m_Cur.m_Kind != TokKind::Ident || m_Cur.m_Text != "push_constant")
                        {
                            LogMsg("[parser] ERROR: unknown attribute '%s' in srinput at line %d\n",
                                   m_Cur.m_Text.c_str(), attrLine);
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(attrLine) +
                                                     ": unknown attribute '" + m_Cur.m_Text +
                                                     "'; only '[push_constant]' is supported in srinput scope");
                        }
                        Advance(); // consume 'push_constant'
                        Expect(TokKind::RBracket, "]");

                        if (++pushConstantCount > 1)
                        {
                            LogMsg("[parser] ERROR: multiple [push_constant] members in srinput '%s' at line %d\n",
                                   srInputDef.m_Name.c_str(), attrLine);
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(attrLine) +
                                                     ": srinput '" + srInputDef.m_Name +
                                                     "' has more than one [push_constant] member; only one is allowed");
                        }
                        bNextIsPushConstant = true;
                    }

                    // Expect: typeName [<templateArg>] memberName;
                    if (m_Cur.m_Kind != TokKind::Ident)
                        throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                                 ": expected type name in srinput scope");

                    // ---- Scalar constant: [static] [const] scalarType name = value; ----
                    // Consume optional "static" and/or "const" qualifiers.
                    bool bHasStatic = false;
                    bool bHasConst  = false;
                    if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Text == "static")
                    {
                        bHasStatic = true;
                        Advance();
                        if (m_Cur.m_Kind != TokKind::Ident)
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                                     ": expected 'const' or type name after 'static' in srinput scope");
                    }
                    if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Text == "const")
                    {
                        bHasConst = true;
                        Advance();
                        if (m_Cur.m_Kind != TokKind::Ident)
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                                     ": expected type name after 'const' in srinput scope");
                    }

                    // If we consumed static/const, the next token must be a scalar type.
                    // Peek ahead: if the token after the type name is '=' it's a scalar const.
                    // We detect this by checking if the type name is in g_Scalars (scalar only,
                    // not vector/matrix) and the token after the member name is '='.
                    bool bIsScalarConst = false;
                    if (bHasStatic || bHasConst)
                    {
                        // After static/const we MUST have a scalar type followed by name = value
                        bIsScalarConst = true;
                    }
                    else
                    {
                        // No qualifiers: peek to see if this is "scalarType name = value;"
                        // We do this by checking if the current ident is a scalar type name
                        // and the token two positions ahead is '='.
                        if (m_Cur.m_Kind == TokKind::Ident)
                        {
                            bool bIsScalar = false;
                            for (auto& [key, info] : g_Scalars)
                            {
                                if (m_Cur.m_Text == key) { bIsScalar = true; break; }
                            }
                            if (bIsScalar)
                            {
                                // Save state, advance past type name and member name, check for '='
                                size_t savedPos  = m_Lex.m_Pos;
                                int    savedLine = m_Lex.m_Line;
                                Token  savedCur  = m_Cur;
                                Advance(); // consume type name
                                if (m_Cur.m_Kind == TokKind::Ident)
                                {
                                    Advance(); // consume member name
                                    if (m_Cur.m_Kind == TokKind::Unknown && m_Cur.m_Text == "=")
                                        bIsScalarConst = true;
                                }
                                // Restore state
                                m_Lex.m_Pos  = savedPos;
                                m_Lex.m_Line = savedLine;
                                m_Cur        = savedCur;
                            }
                        }
                    }

                    if (bIsScalarConst)
                    {
                        // Parse: scalarType name = value;
                        if (m_Cur.m_Kind != TokKind::Ident)
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                                     ": expected scalar type name in srinput scalar constant");
                        Token scalarTypeTok = m_Cur;
                        int   scalarLine    = scalarTypeTok.m_Line;
                        Advance();

                        // Validate: must be a plain scalar (no vector/matrix suffix)
                        bool bValidScalar = false;
                        for (auto& [key, info] : g_Scalars)
                        {
                            if (scalarTypeTok.m_Text == key) { bValidScalar = true; break; }
                        }
                        if (!bValidScalar)
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(scalarLine) +
                                                     ": '" + scalarTypeTok.m_Text +
                                                     "' is not a valid scalar type for srinput scalar constant");

                        Token nameTok = Expect(TokKind::Ident, "scalar constant name");

                        // Expect '='
                        if (m_Cur.m_Kind != TokKind::Unknown || m_Cur.m_Text != "=")
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                                     ": expected '=' after scalar constant name");
                        Advance(); // consume '='

                        // Parse value: collect tokens until ';', concatenating without spaces
                        // so that "1000.0" (tokenized as "1000", ".", "0") and "-1"
                        // (tokenized as "-", "1") are reconstructed correctly.
                        std::string valueStr;
                        while (!Check(TokKind::Semicolon) && !Check(TokKind::Eof))
                        {
                            valueStr += m_Cur.m_Text;
                            Advance();
                        }
                        Expect(TokKind::Semicolon, ";");

                        if (valueStr.empty())
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(nameTok.m_Line) +
                                                     ": scalar constant '" + nameTok.m_Text + "' has no value");

                        if (!srInputMemberNames.insert(nameTok.m_Text).second)
                        {
                            LogMsg("[parser] ERROR: duplicate member name '%s' in srinput '%s' at line %d\n",
                                   nameTok.m_Text.c_str(), srInputDef.m_Name.c_str(), nameTok.m_Line);
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(nameTok.m_Line) +
                                                     ": duplicate member name '" + nameTok.m_Text +
                                                     "' in srinput '" + srInputDef.m_Name + "'");
                        }

                        ScalarConst sc;
                        sc.m_TypeName = scalarTypeTok.m_Text;
                        sc.m_Name     = nameTok.m_Text;
                        sc.m_Value    = valueStr;
                        int scIdx = static_cast<int>(srInputDef.m_ScalarConsts.size());
                        srInputDef.m_ScalarConsts.push_back(std::move(sc));
                        srInputDef.m_BodyOrder.push_back({3, scIdx}); // ScalarConst
                        continue;
                    }

                    Token typeName = m_Cur; Advance();
                    int typeLine = typeName.m_Line;

                    // ---- Check for unsupported resource types (throw immediately) ----
                    static const std::vector<std::string> k_UnsupportedResources = {
                        "TextureBuffer",
                        "RWTexture2DMS", "RWTexture2DMSArray",
                        "RWTextureCube", "RWTextureCubeArray",
                        "AppendStructuredBuffer",
                        "ConsumeStructuredBuffer",
                        "RasterizerOrderedBuffer",
                        "RasterizerOrderedByteAddressBuffer",
                        "RasterizerOrderedStructuredBuffer",
                        "RasterizerOrderedTexture1D",
                        "RasterizerOrderedTexture1DArray",
                        "RasterizerOrderedTexture2D",
                        "RasterizerOrderedTexture2DArray",
                        "RasterizerOrderedTexture3D",
                        "FeedbackTexture2D",
                        "FeedbackTexture2DArray",
                    };
                    for (const auto& unsup : k_UnsupportedResources)
                    {
                        if (typeName.m_Text == unsup)
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(typeLine) +
                                                     ": resource type '" + unsup + "' is not supported");
                    }

                    // ---- Map keyword to ResourceKind (if it is a resource type) ----
                    static const std::vector<std::pair<std::string, ResourceKind>> k_ResourceKinds = {
                        { "Texture1D",                       ResourceKind::Texture1D },
                        { "Texture1DArray",                  ResourceKind::Texture1DArray },
                        { "Texture2DMS",                     ResourceKind::Texture2DMS },
                        { "Texture2DMSArray",                ResourceKind::Texture2DMSArray },
                        { "Texture2DArray",                  ResourceKind::Texture2DArray },
                        { "Texture2D",                       ResourceKind::Texture2D },
                        { "Texture3D",                       ResourceKind::Texture3D },
                        { "TextureCubeArray",                ResourceKind::TextureCubeArray },
                        { "TextureCube",                     ResourceKind::TextureCube },
                        { "Buffer",                          ResourceKind::Buffer },
                        { "StructuredBuffer",                ResourceKind::StructuredBuffer },
                        { "ByteAddressBuffer",               ResourceKind::ByteAddressBuffer },
                        { "RaytracingAccelerationStructure", ResourceKind::RaytracingAccelerationStructure },
                        { "RWTexture1DArray",                ResourceKind::RWTexture1DArray },
                        { "RWTexture1D",                     ResourceKind::RWTexture1D },
                        { "RWTexture2DArray",                ResourceKind::RWTexture2DArray },
                        { "RWTexture2D",                     ResourceKind::RWTexture2D },
                        { "RWTexture3D",                     ResourceKind::RWTexture3D },
                        { "RWBuffer",                        ResourceKind::RWBuffer },
                        { "RWStructuredBuffer",              ResourceKind::RWStructuredBuffer },
                        { "RWByteAddressBuffer",             ResourceKind::RWByteAddressBuffer },
                    };

                    // ---- Check for sampler types ----
                    static const std::vector<std::pair<std::string, SamplerKind>> k_SamplerKinds = {
                        { "SamplerComparisonState", SamplerKind::SamplerComparisonState },
                        { "SamplerState",           SamplerKind::SamplerState },
                    };

                    SamplerKind foundSamplerKind = SamplerKind::SamplerState; // placeholder
                    bool bIsSampler = false;
                    for (const auto& [name, kind] : k_SamplerKinds)
                    {
                        if (typeName.m_Text == name) { foundSamplerKind = kind; bIsSampler = true; break; }
                    }

                    if (bIsSampler)
                    {
                        if (bNextIsPushConstant)
                        {
                            LogMsg("[parser] ERROR: [push_constant] cannot be applied to sampler member in srinput '%s' at line %d\n",
                                   srInputDef.m_Name.c_str(), typeName.m_Line);
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(typeName.m_Line) +
                                                     ": [push_constant] attribute cannot be applied to sampler member '" +
                                                     typeName.m_Text + "'; it is only valid on cbuffer members");
                        }

                        // ---- Sampler member ----
                        Token memberName = Expect(TokKind::Ident, "member name in srinput");
                        Expect(TokKind::Semicolon, ";");

                        if (!srInputMemberNames.insert(memberName.m_Text).second)
                        {
                            LogMsg("[parser] ERROR: duplicate member name '%s' in srinput '%s' at line %d\n",
                                   memberName.m_Text.c_str(), srInputDef.m_Name.c_str(), memberName.m_Line);
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(memberName.m_Line) +
                                                     ": duplicate member name '" + memberName.m_Text +
                                                     "' in srinput '" + srInputDef.m_Name + "'");
                        }

                        SamplerMember sm;
                        sm.m_Kind       = foundSamplerKind;
                        sm.m_TypeName   = typeName.m_Text;
                        sm.m_MemberName = memberName.m_Text;
                        int smIdx = static_cast<int>(srInputDef.m_Samplers.size());
                        srInputDef.m_Samplers.push_back(std::move(sm));
                        srInputDef.m_BodyOrder.push_back({2, smIdx}); // Sampler
                        continue;
                    }

                    ResourceKind foundKind = ResourceKind::Texture2D; // placeholder
                    bool bIsResource = false;
                    for (const auto& [name, kind] : k_ResourceKinds)
                    {
                        if (typeName.m_Text == name) { foundKind = kind; bIsResource = true; break; }
                    }

                    if (bIsResource)
                    {
                        if (bNextIsPushConstant)
                        {
                            LogMsg("[parser] ERROR: [push_constant] cannot be applied to resource member in srinput '%s' at line %d\n",
                                   srInputDef.m_Name.c_str(), typeName.m_Line);
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(typeName.m_Line) +
                                                     ": [push_constant] attribute cannot be applied to resource member '" +
                                                     typeName.m_Text + "'; it is only valid on cbuffer members");
                        }

                        // ---- Resource member ----
                        // Types that never take a template arg:
                        bool bNoTemplateArg =
                            foundKind == ResourceKind::ByteAddressBuffer ||
                            foundKind == ResourceKind::RWByteAddressBuffer ||
                            foundKind == ResourceKind::RaytracingAccelerationStructure;

                        // UAV types and typed buffer SRVs MUST have a template arg.
                        // SRV texture types (Texture1D/2D/3D/Cube and their array/MS variants)
                        // may omit <T> (DXC defaults to float4).
                        bool bTemplateRequired =
                            !bNoTemplateArg && (
                                IsUAV(foundKind) ||
                                foundKind == ResourceKind::Buffer ||
                                foundKind == ResourceKind::StructuredBuffer
                            );

                        std::string templateArg;
                        std::string fullTypeName = typeName.m_Text;

                        if (!bNoTemplateArg && Check(TokKind::LAngle))
                        {
                            // Parse <templateArg> — may be a builtin type or a struct name.
                            Advance(); // consume '<'
                            if (m_Cur.m_Kind != TokKind::Ident)
                                throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                                         ": expected type argument inside '<>'");
                            Token argTok = m_Cur; Advance();
                            templateArg = argTok.m_Text;

                            // Validate: if it looks like a struct name, it must be visible.
                            // First check if it's a known builtin scalar/vector.
                            bool bIsBuiltin = false;
                            for (auto& [key, info] : g_Scalars)
                                if (templateArg.rfind(key, 0) == 0) { bIsBuiltin = true; break; }

                            if (!bIsBuiltin)
                            {
                                // Must be a user-defined struct visible at this scope.
                                if (m_StructMap.find(templateArg) == m_StructMap.end())
                                    throw std::runtime_error(m_FilePath + ":" + std::to_string(argTok.m_Line) +
                                                             ": resource template argument '" + templateArg +
                                                             "' is not a visible struct or builtin type");
                            }

                            Expect(TokKind::RAngle, ">");
                            fullTypeName = typeName.m_Text + "<" + templateArg + ">";
                        }
                        else if (bTemplateRequired)
                        {
                            // UAV / Buffer / StructuredBuffer without <T> — DXC compile error.
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(typeLine) +
                                                     ": '" + typeName.m_Text +
                                                     "' requires a template type argument, e.g. <float4>");
                        }

                        Token memberName = Expect(TokKind::Ident, "member name in srinput");
                        Expect(TokKind::Semicolon, ";");

                        if (!srInputMemberNames.insert(memberName.m_Text).second)
                        {
                            LogMsg("[parser] ERROR: duplicate member name '%s' in srinput '%s' at line %d\n",
                                   memberName.m_Text.c_str(), srInputDef.m_Name.c_str(), memberName.m_Line);
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(memberName.m_Line) +
                                                     ": duplicate member name '" + memberName.m_Text +
                                                     "' in srinput '" + srInputDef.m_Name + "'");
                        }

                        ResourceMember rm;
                        rm.m_Kind        = foundKind;
                        rm.m_TypeName    = fullTypeName;
                        rm.m_TemplateArg = templateArg;
                        rm.m_MemberName  = memberName.m_Text;
                        int rmIdx = static_cast<int>(srInputDef.m_Resources.size());
                        srInputDef.m_Resources.push_back(std::move(rm));
                        srInputDef.m_BodyOrder.push_back({1, rmIdx}); // Resource
                    }
                    else
                    {
                        // ---- cbuffer reference ----
                        bool bIsCBufferType = false;
                        for (const auto& bufDef : m_Result.m_BufferDefs)
                        {
                            if (bufDef.m_Name == typeName.m_Text)
                            {
                                bIsCBufferType = true;
                                break;
                            }
                        }

                        if (bIsCBufferType)
                        {
                            Token memberName = Expect(TokKind::Ident, "member name in srinput");
                            Expect(TokKind::Semicolon, ";");

                            if (!srInputMemberNames.insert(memberName.m_Text).second)
                            {
                                LogMsg("[parser] ERROR: duplicate member name '%s' in srinput '%s' at line %d\n",
                                       memberName.m_Text.c_str(), srInputDef.m_Name.c_str(), memberName.m_Line);
                                throw std::runtime_error(m_FilePath + ":" + std::to_string(memberName.m_Line) +
                                                         ": duplicate member name '" + memberName.m_Text +
                                                         "' in srinput '" + srInputDef.m_Name + "'");
                            }

                            if (bNextIsPushConstant && !srInputDef.m_Members.empty())
                            {
                                LogMsg("[parser] ERROR: [push_constant] member '%s' is not the first cbuffer in srinput '%s' at line %d\n",
                                       memberName.m_Text.c_str(), srInputDef.m_Name.c_str(), memberName.m_Line);
                                throw std::runtime_error(m_FilePath + ":" + std::to_string(memberName.m_Line) +
                                                         ": [push_constant] member '" + memberName.m_Text +
                                                         "' must be the first cbuffer declared in srinput '" +
                                                         srInputDef.m_Name + "' (it always occupies register b0)");
                            }

                            SrInputMember member;
                            member.m_CBufferName    = typeName.m_Text;
                            member.m_MemberName     = memberName.m_Text;
                            member.m_bIsPushConstant = bNextIsPushConstant;
                            int mbIdx = static_cast<int>(srInputDef.m_Members.size());
                            srInputDef.m_Members.push_back(std::move(member));
                            srInputDef.m_BodyOrder.push_back({0, mbIdx}); // CBuffer
                        }
                        else
                        {
                            // ---- Check if it is a known srinput name (composition) ----
                            auto srIt = m_SrInputMap.find(typeName.m_Text);
                            if (srIt != m_SrInputMap.end())
                            {
                                // ---- Nested srinput reference ----
                                if (bNextIsPushConstant)
                                {
                                    LogMsg("[parser] ERROR: [push_constant] cannot be applied to nested srinput '%s' in srinput '%s' at line %d\n",
                                           typeName.m_Text.c_str(), srInputDef.m_Name.c_str(), typeName.m_Line);
                                    throw std::runtime_error(m_FilePath + ":" + std::to_string(typeName.m_Line) +
                                                             ": [push_constant] attribute cannot be applied to nested srinput '" +
                                                             typeName.m_Text + "'; it is only valid on cbuffer members");
                                }

                                const SrInputDef& nestedDef = m_Result.m_SrInputDefs[srIt->second];

                                // Parse the local variable name for this nested ref
                                Token memberName = Expect(TokKind::Ident, "member name for nested srinput");
                                Expect(TokKind::Semicolon, ";");

                                // Flatten nested srinput and check for name clashes across all inner names
                                FlatSrInput flat = FlattenSrInput(nestedDef, m_Result.m_SrInputDefs);
                                auto checkFlatName = [&](const std::string& name)
                                {
                                    if (!srInputMemberNames.insert(name).second)
                                    {
                                        LogMsg("[parser] ERROR: name '%s' from nested srinput '%s' clashes with existing member in srinput '%s' at line %d\n",
                                               name.c_str(), typeName.m_Text.c_str(), srInputDef.m_Name.c_str(), memberName.m_Line);
                                        throw std::runtime_error(m_FilePath + ":" + std::to_string(memberName.m_Line) +
                                                                 ": name '" + name + "' from nested srinput '" +
                                                                 typeName.m_Text + "' clashes with an existing member name in '" +
                                                                 srInputDef.m_Name + "'");
                                    }
                                };
                                for (const auto& m  : flat.m_Members)      checkFlatName(m.m_MemberName);
                                for (const auto& r  : flat.m_Resources)    checkFlatName(r.m_MemberName);
                                for (const auto& s  : flat.m_Samplers)     checkFlatName(s.m_MemberName);
                                for (const auto& sc : flat.m_ScalarConsts) checkFlatName(sc.m_Name);

                                // Register the nested ref
                                SrInputRef ref;
                                ref.m_SrInputName = typeName.m_Text;
                                ref.m_VarName     = memberName.m_Text;
                                int refIdx = static_cast<int>(srInputDef.m_NestedSrInputs.size());
                                srInputDef.m_NestedSrInputs.push_back(std::move(ref));
                                srInputDef.m_BodyOrder.push_back({4, refIdx}); // NestedRef
                            }
                            else
                            {
                                throw std::runtime_error(m_FilePath + ":" + std::to_string(typeLine) +
                                                         ": srinput scope only allows cbuffer types, resource types, or nested srinput names; '" +
                                                         typeName.m_Text + "' is not a cbuffer, known resource type, or previously-declared srinput");
                            }
                        }
                    }
                }

                Expect(TokKind::RBrace, "}");
                Expect(TokKind::Semicolon, ";");

                // After full parse, validate that the flattened srinput has at most 1 push constant
                {
                    FlatSrInput flat = FlattenSrInput(srInputDef, m_Result.m_SrInputDefs);
                    int pushConstCount = 0;
                    for (const auto& member : flat.m_Members)
                        if (member.m_bIsPushConstant) ++pushConstCount;
                    if (pushConstCount > 1)
                    {
                        LogMsg("[parser] ERROR: srinput '%s' has %d push constants in its flattened view; at most 1 is allowed\n",
                               srInputDef.m_Name.c_str(), pushConstCount);
                        throw std::runtime_error(m_FilePath + ": srinput '" + srInputDef.m_Name +
                                                 "' has " + std::to_string(pushConstCount) +
                                                 " push constants in its flattened view; at most 1 is allowed");
                    }
                }

                m_SrInputMap[srInputDef.m_Name] = m_Result.m_SrInputDefs.size();
                m_Result.m_SrInputDefs.push_back(std::move(srInputDef));
                continue;
            }

            // ---- typedef (skipped) ----
            if (kw == "typedef")
            {
                while (!Check(TokKind::Semicolon) && !Check(TokKind::Eof)) Advance();
                TryConsume(TokKind::Semicolon);
                continue;
            }

            Advance(); // unknown top-level keyword
        }
    }
};

// ---------------------------------------------------------------------------
// TypeRef helpers (also used in layout.cpp and codegen)
// ---------------------------------------------------------------------------
int TypeAlignment(const TypeRef& t)
{
    if (auto* bt = std::get_if<BuiltinType>(&t))
        return bt->m_Alignment;
    if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&t))
        return TypeAlignment((*ap)->m_ElementType);
    return 16; // StructType* -> always 16-byte aligned in cbuffers
}

std::string TypeDisplayName(const TypeRef& t)
{
    if (auto* bt = std::get_if<BuiltinType>(&t))
        return bt->m_Name;
    if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&t))
        return (*ap)->m_Name;
    if (auto* sp = std::get_if<StructType*>(&t))
        return "struct " + (*sp)->m_Name;
    return "???";
}

// ---------------------------------------------------------------------------
// LayoutMember helpers
// ---------------------------------------------------------------------------
void LayoutMember::SetPadding(int p)
{
    m_Padding = p;
    // Propagate into last array-element submember
    if (std::holds_alternative<std::shared_ptr<ArrayNode>>(m_Type) && !m_Submembers.empty())
        m_Submembers.back().m_Padding = p;
}

void LayoutMember::PushSubmember(LayoutMember m)
{
    if (!m_Submembers.empty())
    {
        LayoutMember& last = m_Submembers.back();
        int pad = m.m_Offset - (last.m_Offset + last.m_Size);
        last.SetPadding(pad);
    }
    m_Submembers.push_back(std::move(m));
}

// ---------------------------------------------------------------------------
// Internal parse / public API
// ---------------------------------------------------------------------------
static ParseResult ParseFileInternal(const std::string& path)
{
    LogMsg("[parser] Opening: %s\n", path.c_str());

    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open file: " + path);

    std::ostringstream ss;
    ss << f.rdbuf();

    ParseResult result;
    result.m_SourceFile = path;

    Parser p(ss.str(), result, path);
    p.Parse();

    LogMsg("[parser] Done: %zu structs, %zu buffers, %zu srinput(s)\n",
           result.m_Structs.size(), result.m_Buffers.size(), result.m_SrInputDefs.size());
    return result;
}

ParseResult ParseFile(const std::string& path)
{
    g_IncludedFiles.clear();

    std::string absPath;
    try { absPath = fs::absolute(path).string(); }
    catch (...) { absPath = path; }

    g_IncludedFiles.insert(absPath);
    return ParseFileInternal(absPath);
}