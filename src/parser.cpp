#include "types.h"

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
                      bool bCreatedFromMatrix = false,
                      bool bIsRowMajor = false) const
    {
        auto node = std::make_shared<ArrayNode>();
        node->m_ElementType          = elemType;
        node->m_ArraySize            = arraySize;
        node->m_bCreatedFromMatrix  = bCreatedFromMatrix;
        node->m_bIsRowMajor         = bIsRowMajor;
        node->m_Name = TypeDisplayName(node->m_ElementType) +
                     "[" + std::to_string(arraySize) + "]";
        return node;
    }

    // -----------------------------------------------------------------------
    // matrix<T,r,c> or vector<T,n>
    // -----------------------------------------------------------------------
    TypeRef ParseTemplateType(const std::string& keyword, bool bIsRowMajor)
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
            if (bIsRowMajor) std::swap(vectorSize, arraySize);
            TypeRef elem = MakeBuiltin(*si, vectorSize, true);
            if (arraySize == 1) return elem;
            return MakeArray(std::move(elem), arraySize, true, bIsRowMajor);
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
            return ParseTemplateType(name, bIsRowMajor);

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
                return MakeArray(std::move(elem), arraySize, true, bIsRowMajor);
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

        return MakeArray(std::move(elemType), total, false, false);
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
        while (!Check(TokKind::RBrace) && !Check(TokKind::Eof))
            ParseMemberVariables(members);
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
                Expect(TokKind::LBrace, "{");

                SrInputDef srInputDef;
                srInputDef.m_Name = srInputName.m_Text;

                while (!Check(TokKind::RBrace) && !Check(TokKind::Eof))
                {
                    // Expect: typeName [<templateArg>] memberName;
                    if (m_Cur.m_Kind != TokKind::Ident)
                        throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                                 ": expected type name in srinput scope");
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

                    ResourceKind foundKind = ResourceKind::Texture2D; // placeholder
                    bool bIsResource = false;
                    for (const auto& [name, kind] : k_ResourceKinds)
                    {
                        if (typeName.m_Text == name) { foundKind = kind; bIsResource = true; break; }
                    }

                    if (bIsResource)
                    {
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

                        ResourceMember rm;
                        rm.m_Kind        = foundKind;
                        rm.m_TypeName    = fullTypeName;
                        rm.m_TemplateArg = templateArg;
                        rm.m_MemberName  = memberName.m_Text;
                        srInputDef.m_Resources.push_back(std::move(rm));
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

                        if (!bIsCBufferType)
                        {
                            throw std::runtime_error(m_FilePath + ":" + std::to_string(typeLine) +
                                                     ": srinput scope only allows cbuffer types or resource types; '" +
                                                     typeName.m_Text + "' is not a cbuffer or known resource type");
                        }

                        Token memberName = Expect(TokKind::Ident, "member name in srinput");
                        Expect(TokKind::Semicolon, ";");

                        SrInputMember member;
                        member.m_CBufferName = typeName.m_Text;
                        member.m_MemberName  = memberName.m_Text;
                        srInputDef.m_Members.push_back(std::move(member));
                    }
                }

                Expect(TokKind::RBrace, "}");
                Expect(TokKind::Semicolon, ";");

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