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
static void FixupTypeRef(std::shared_ptr<TypeRef>& t,
                         const std::unordered_map<StructType*, StructType*>& remap)
{
    if (!t) return;
    t->RemapStruct(remap);
    if (t->IsArray())
    {
        // ElementType() returns a const ref; we need to recurse into the mutable
        // element type stored inside the ArrayTypeRef.
        auto* arr = static_cast<ArrayTypeRef*>(t.get());
        FixupTypeRef(arr->m_ElementType, remap);
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

    // Extern type declarations: type names declared with 'extern TypeName;'.
    // These are external types with no definition in any .sr file.
    std::unordered_set<std::string> m_ExternMap;

    // -------------------------------------------------------------------------
    // Preprocessor state
    // -------------------------------------------------------------------------
    // Macro definitions: name → value string ("" for flag macros and type aliases).
    std::unordered_map<std::string, std::string> m_Defines;
    // Type aliases from #define, typedef, or using — name → resolved TypeRef.
    std::unordered_map<std::string, std::shared_ptr<TypeRef>> m_TypeAliases;

    // Conditional compilation stack (#if / #ifdef / #else / #endif).
    struct CondFrame
    {
        bool parentActive;    // outer context was active when this #if was entered
        bool anyBranchTaken;  // at least one branch condition evaluated to true so far
        bool currentlyActive; // current branch is live
        bool inElse;          // have already processed an #else for this frame
        int  openHashLine = 0;// line of the '#' that opened this block (#if/#ifdef/#ifndef)
    };
    std::vector<CondFrame> m_CondStack;

    // True when code in the current context should be compiled.
    bool IsActive() const
    {
        if (m_CondStack.empty()) return true;
        return m_CondStack.back().currentlyActive;
    }

    // -----------------------------------------------------------------------
    // Raw-text capture helpers for preprocessor passthrough.
    // -----------------------------------------------------------------------

    // Line-start position table: entry [i] = byte offset of line (i+1) in m_Lex.m_Src.
    std::vector<size_t> m_LineStartPositions;

    // Set by ParseNonStructType() when the type identifier resolved via a macro alias.
    // Cleared at the start of each ParseNonStructType() call.
    std::string m_LastResolvedAliasName;

    // Tracks nesting depth inside struct/cbuffer/srinput bodies.
    // Passthrough blocks are captured only at depth 0 (true file scope).
    int m_BodyNestingDepth = 0;

    // Returns verbatim source text for lines startLine..endLine (1-based, inclusive),
    // including the trailing newline of endLine.
    std::string GetRawLines(int startLine, int endLine) const
    {
        if (startLine < 1 || endLine < startLine ||
            startLine > (int)m_LineStartPositions.size())
            return "";
        size_t startPos = m_LineStartPositions[startLine - 1];
        size_t endPos   = (endLine < (int)m_LineStartPositions.size())
                        ? m_LineStartPositions[endLine]
                        : m_Lex.m_Src.size();
        return m_Lex.m_Src.substr(startPos, endPos - startPos);
    }

    Parser(const std::string& src, ParseResult& r, const std::string& path)
        : m_Lex(src), m_Result(r), m_FilePath(path)
    {
        // Build line-start position table for raw preprocessor capture.
        m_LineStartPositions.push_back(0);
        for (size_t i = 0; i < m_Lex.m_Src.size(); ++i)
            if (m_Lex.m_Src[i] == '\n')
                m_LineStartPositions.push_back(i + 1);
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
    // Skip all tokens whose line number equals 'line'.
    // Used to consume the remainder of a preprocessor directive line.
    // -----------------------------------------------------------------------
    void SkipRestOfLine(int line)
    {
        while (m_Cur.m_Kind != TokKind::Eof && m_Cur.m_Line == line)
            Advance();
    }

    // -----------------------------------------------------------------------
    // Returns true if the given identifier is a builtin/native type name
    // (scalar, vector, matrix, resource, or sampler keyword).
    // Used to validate 'extern' declarations.
    // -----------------------------------------------------------------------
    static bool IsNativeTypeName(const std::string& name)
    {
        // Scalar base types (and their sized variants)
        static const std::vector<std::string> k_ScalarPrefixes = {
            "float16_t", "float32_t", "float64_t",
            "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t",
            "double", "float", "bool", "int", "uint",
        };
        for (const auto& sc : k_ScalarPrefixes)
        {
            if (name == sc) return true;
            // vector: floatN, intN, uintN etc.
            if (name.size() == sc.size() + 1 && name.rfind(sc, 0) == 0 &&
                std::isdigit((unsigned char)name.back()))
                return true;
            // matrix: floatNxM
            if (name.size() == sc.size() + 3 && name.rfind(sc, 0) == 0 &&
                std::isdigit((unsigned char)name[sc.size()]) &&
                name[sc.size() + 1] == 'x' &&
                std::isdigit((unsigned char)name[sc.size() + 2]))
                return true;
        }
        // matrix / vector template types
        if (name == "matrix" || name == "vector") return true;
        // Resource types
        static const std::vector<std::string> k_ResourceNames = {
            "Texture1D", "Texture1DArray", "Texture2D", "Texture2DArray",
            "Texture2DMS", "Texture2DMSArray", "Texture3D",
            "TextureCube", "TextureCubeArray", "TextureBuffer",
            "Buffer", "StructuredBuffer", "ByteAddressBuffer",
            "RaytracingAccelerationStructure", "ConstantBuffer",
            "RWTexture1D", "RWTexture1DArray", "RWTexture2D", "RWTexture2DArray",
            "RWTexture3D", "RWBuffer", "RWStructuredBuffer", "RWByteAddressBuffer",
            "AppendStructuredBuffer", "ConsumeStructuredBuffer",
        };
        for (const auto& r : k_ResourceNames)
            if (name == r) return true;
        // Sampler types
        if (name == "SamplerState" || name == "SamplerComparisonState") return true;
        return false;
    }

    // -----------------------------------------------------------------------
    // Handle 'extern TypeName;' declaration (at global, struct, or srinput scope).
    // m_Cur is positioned on the type-name token after 'extern'.
    // Supports simple names ("MyType") and namespace/nested-class qualified names
    // ("nvrhi::rt::IndirectInstanceDesc").  The full qualified name is stored as-is.
    // Registers the type name in m_ExternMap and m_Result.m_ExternTypeNames.
    // -----------------------------------------------------------------------
    void HandleExternDecl(int externLine)
    {
        if (m_Cur.m_Kind != TokKind::Ident)
            throw std::runtime_error(m_FilePath + ":" + std::to_string(externLine) +
                                     ": expected type name after 'extern'");

        // Collect the (possibly qualified) type name: Ident (:: Ident)*
        std::string typeName = m_Cur.m_Text;
        int nameLine = m_Cur.m_Line;
        Advance(); // consume first identifier

        while (m_Cur.m_Kind == TokKind::Colon)
        {
            // Peek ahead: we need exactly "::" (two consecutive Colon tokens)
            int colonLine = m_Cur.m_Line;
            Advance(); // consume first ':'
            if (m_Cur.m_Kind != TokKind::Colon || m_Cur.m_Line != colonLine)
                throw std::runtime_error(m_FilePath + ":" + std::to_string(colonLine) +
                                         ": expected '::' in qualified type name after '" + typeName + "'");
            Advance(); // consume second ':'

            if (m_Cur.m_Kind != TokKind::Ident)
                throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                         ": expected identifier after '::' in qualified type name");
            typeName += "::" + m_Cur.m_Text;
            Advance(); // consume next identifier component
        }

        // For qualified names (containing "::"), skip the simple-name conflict checks —
        // namespaced types cannot conflict with local struct/alias/srinput names.
        // For simple (unqualified) names, apply all the usual validation rules.
        bool bIsQualified = (typeName.find("::") != std::string::npos);

        if (!bIsQualified)
        {
            // Block native/builtin types
            if (IsNativeTypeName(typeName))
                throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                         ": 'extern' cannot be used with native type '" + typeName +
                                         "'; only externally-defined user types may be declared extern");

            // Block already-defined struct names
            if (m_StructMap.count(typeName))
                throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                         ": 'extern " + typeName + "' conflicts with an already-defined struct");

            // Block type aliases (typedef / using / #define)
            if (m_TypeAliases.count(typeName))
                throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                         ": 'extern " + typeName + "' conflicts with an existing type alias");

            // Block srinput names
            if (m_SrInputMap.count(typeName))
                throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                         ": 'extern " + typeName + "' conflicts with an srinput name");
        }

        Expect(TokKind::Semicolon, ";");

        // Idempotent: re-declaring the same extern type is allowed
        m_ExternMap.insert(typeName);
        m_Result.m_ExternTypeNames.insert(typeName);
    }

    // -----------------------------------------------------------------------
    // Preprocessor condition evaluators for #if / #elif.
    // All functions only consume tokens whose m_Line == condLine.
    // Unknown/undefined macros silently evaluate to 0 (false).
    // -----------------------------------------------------------------------
    bool EvalPrimary(int condLine)
    {
        if (m_Cur.m_Kind == TokKind::Eof || m_Cur.m_Line != condLine)
            return false;

        // Parenthesised sub-expression: ( expr )
        if (m_Cur.m_Kind == TokKind::Unknown && m_Cur.m_Text == "(")
        {
            Advance();
            bool val = EvalOrExpr(condLine);
            if (m_Cur.m_Line == condLine &&
                m_Cur.m_Kind == TokKind::Unknown && m_Cur.m_Text == ")")
                Advance();
            return val;
        }

        // defined(MACRO) or defined MACRO
        if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Text == "defined")
        {
            Advance();
            bool hasParen = (m_Cur.m_Line == condLine &&
                             m_Cur.m_Kind == TokKind::Unknown && m_Cur.m_Text == "(");
            if (hasParen) Advance();
            std::string macroName;
            if (m_Cur.m_Line == condLine && m_Cur.m_Kind == TokKind::Ident)
            { macroName = m_Cur.m_Text; Advance(); }
            if (hasParen && m_Cur.m_Line == condLine &&
                m_Cur.m_Kind == TokKind::Unknown && m_Cur.m_Text == ")")
                Advance();
            return m_Defines.count(macroName) > 0 || m_TypeAliases.count(macroName) > 0;
        }

        // Identifier: macro reference with optional comparison operator
        if (m_Cur.m_Kind == TokKind::Ident)
        {
            std::string macroName = m_Cur.m_Text;
            Advance();

            if (m_Cur.m_Kind != TokKind::Eof && m_Cur.m_Line == condLine)
            {
                std::string opStr;
                if (m_Cur.m_Kind == TokKind::LAngle)          // '<' or '<='
                {
                    opStr = "<"; Advance();
                    if (m_Cur.m_Line == condLine && m_Cur.m_Kind == TokKind::Unknown &&
                        m_Cur.m_Text == "=")
                    { opStr = "<="; Advance(); }
                }
                else if (m_Cur.m_Kind == TokKind::RAngle)     // '>' or '>='
                {
                    opStr = ">"; Advance();
                    if (m_Cur.m_Line == condLine && m_Cur.m_Kind == TokKind::Unknown &&
                        m_Cur.m_Text == "=")
                    { opStr = ">="; Advance(); }
                }
                else if (m_Cur.m_Kind == TokKind::Unknown)
                {
                    char ch = m_Cur.m_Text[0];
                    if (ch == '=')
                    {
                        opStr = "="; Advance();
                        if (m_Cur.m_Line == condLine && m_Cur.m_Kind == TokKind::Unknown &&
                            m_Cur.m_Text == "=")
                        { opStr = "=="; Advance(); }
                    }
                    else if (ch == '!')
                    {
                        // Peek ahead: '!' followed by '=' is the '!=' operator.
                        Token pk = m_Lex.Peek();
                        if (pk.m_Kind == TokKind::Unknown && pk.m_Text == "=")
                        { opStr = "!="; Advance(); Advance(); }
                        // else: standalone '!' belongs to the outer not_expr; leave it.
                    }
                }

                if (!opStr.empty() && opStr != "=") // bare single "=" is a syntax error; ignore
                {
                    std::string rhs;
                    if (m_Cur.m_Line == condLine)
                    {
                        if (m_Cur.m_Kind == TokKind::Number)
                        { rhs = m_Cur.m_Text; Advance(); }
                        else if (m_Cur.m_Kind == TokKind::Ident)
                        { rhs = m_Cur.m_Text; Advance(); }
                    }
                    std::string lhs = "0";
                    auto it = m_Defines.find(macroName);
                    if (it != m_Defines.end())
                        lhs = it->second.empty() ? "1" : it->second;

                    if (opStr == "==") return lhs == rhs;
                    if (opStr == "!=") return lhs != rhs;
                    try
                    {
                        int l = std::stoi(lhs), r = std::stoi(rhs);
                        if (opStr == "<")  return l <  r;
                        if (opStr == "<=") return l <= r;
                        if (opStr == ">")  return l >  r;
                        if (opStr == ">=") return l >= r;
                    }
                    catch (...) {}
                    return false;
                }
            }

            // No operator: truthy if macro is defined and non-zero
            auto it = m_Defines.find(macroName);
            if (it != m_Defines.end())
            {
                if (it->second.empty()) return true; // flag macro
                try { return std::stoi(it->second) != 0; } catch (...) { return true; }
            }
            if (m_TypeAliases.count(macroName)) return true; // type alias counts as defined
            return false; // undefined = 0
        }

        // Numeric literal
        if (m_Cur.m_Kind == TokKind::Number)
        {
            int val = 0;
            try { val = std::stoi(m_Cur.m_Text); } catch (...) {}
            Advance();
            return val != 0;
        }

        Advance(); // unknown token: skip and return false
        return false;
    }

    bool EvalNotExpr(int condLine)
    {
        if (m_Cur.m_Kind != TokKind::Eof && m_Cur.m_Line == condLine &&
            m_Cur.m_Kind == TokKind::Unknown && m_Cur.m_Text == "!")
        {
            Advance(); // consume '!'  (always logical NOT here; '!=' is handled in EvalPrimary)
            return !EvalNotExpr(condLine);
        }
        return EvalPrimary(condLine);
    }

    bool EvalAndExpr(int condLine)
    {
        bool val = EvalNotExpr(condLine);
        while (m_Cur.m_Kind != TokKind::Eof && m_Cur.m_Line == condLine &&
               m_Cur.m_Kind == TokKind::Unknown && m_Cur.m_Text == "&")
        {
            Advance(); // first '&'
            if (m_Cur.m_Kind == TokKind::Unknown && m_Cur.m_Text == "&") Advance(); // second '&'
            bool rhs = EvalNotExpr(condLine);
            val = val && rhs;
        }
        return val;
    }

    bool EvalOrExpr(int condLine)
    {
        bool val = EvalAndExpr(condLine);
        while (m_Cur.m_Kind != TokKind::Eof && m_Cur.m_Line == condLine &&
               m_Cur.m_Kind == TokKind::Unknown && m_Cur.m_Text == "|")
        {
            Advance(); // first '|'
            if (m_Cur.m_Kind == TokKind::Unknown && m_Cur.m_Text == "|") Advance(); // second '|'
            bool rhs = EvalAndExpr(condLine);
            val = val || rhs;
        }
        return val;
    }

    // Evaluate a full condition expression on 'condLine', consume all tokens on
    // that line, and return the boolean result.
    bool EvalConditionLine(int condLine)
    {
        bool result = EvalOrExpr(condLine);
        SkipRestOfLine(condLine); // discard any remaining tokens on the condition line
        return result;
    }

    // -----------------------------------------------------------------------
    // Central handler for all preprocessor directives.
    // Called after '#' has already been consumed; m_Cur is at the keyword.
    // -----------------------------------------------------------------------
    void HandlePreprocessorDirective(int hashLine)
    {
        if (m_Cur.m_Kind != TokKind::Ident)
        { SkipRestOfLine(hashLine); return; }

        // #include — ParseInclude expects m_Cur still pointing at "include"
        if (m_Cur.m_Text == "include")
        {
            if (!IsActive()) { SkipRestOfLine(hashLine); return; }
            ParseInclude(hashLine);
            return;
        }

        std::string directive = m_Cur.m_Text;
        Advance(); // consume directive keyword

        // ---- #pragma ----
        if (directive == "pragma")
        {
            if (!IsActive()) { SkipRestOfLine(hashLine); return; }
            if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Line == hashLine &&
                (m_Cur.m_Text == "pack" || m_Cur.m_Text == "pack_matrix"))
            {
                std::string pt = m_Cur.m_Text;
                LogMsg("[parser] ERROR: forbidden '#pragma %s' directive at line %d\n",
                       pt.c_str(), hashLine);
                throw std::runtime_error(m_FilePath + ":" + std::to_string(hashLine) +
                                         ": '#pragma " + pt + "' is not allowed in this file format");
            }
            SkipRestOfLine(hashLine);
            return;
        }

        // ---- #define ----
        if (directive == "define")
        {
            if (!IsActive()) { SkipRestOfLine(hashLine); return; }
            if (m_Cur.m_Kind != TokKind::Ident || m_Cur.m_Line != hashLine)
            { SkipRestOfLine(hashLine); return; }
            Token macroName = m_Cur;
            Advance();
            if (m_Cur.m_Kind == TokKind::Eof || m_Cur.m_Line != hashLine)
            {
                // Flag macro: #define FOO
                m_Defines[macroName.m_Text] = "";
            }
            else if (m_Cur.m_Kind == TokKind::Number && m_Cur.m_Line == hashLine)
            {
                // Numeric macro: #define FOO 3
                m_Defines[macroName.m_Text] = m_Cur.m_Text;
                Advance();
                SkipRestOfLine(hashLine);
            }
            else if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Line == hashLine)
            {
                // Type-alias macro: #define FOO float3
                // Parse and validate the type using the standard type parser.
                try
                {
                    auto aliasType = ParseNonStructType();
                    m_TypeAliases[macroName.m_Text] = std::move(aliasType);
                    m_Defines[macroName.m_Text] = ""; // also mark defined for defined() checks
                }
                catch (const std::exception& e)
                {
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(hashLine) +
                                             ": '#define " + macroName.m_Text +
                                             "' value is not a valid HLSL type: " + e.what());
                }
                SkipRestOfLine(hashLine);
            }
            else
            {
                throw std::runtime_error(m_FilePath + ":" + std::to_string(hashLine) +
                                         ": '#define " + macroName.m_Text +
                                         "': value must be a valid HLSL type name or an integer literal");
            }
            // Passthrough: capture this #define line verbatim when at file scope
            // (not inside a struct/cbuffer/srinput body, not inside a conditional block).
            // Defines inside a conditional are captured as part of the whole block at #endif time.
            if (m_CondStack.empty() && m_BodyNestingDepth == 0)
            {
                std::string rawLine = GetRawLines(hashLine, hashLine);
                if (!rawLine.empty())
                    m_Result.m_PreprocPassthrough.push_back(std::move(rawLine));
            }
            return;
        }

        // ---- #undef ----
        if (directive == "undef")
        {
            if (!IsActive()) { SkipRestOfLine(hashLine); return; }
            if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Line == hashLine)
            {
                m_Defines.erase(m_Cur.m_Text);
                m_TypeAliases.erase(m_Cur.m_Text);
                Advance();
            }
            SkipRestOfLine(hashLine);
            return;
        }

        // ---- #ifdef ----
        if (directive == "ifdef")
        {
            bool parentActive = IsActive();
            bool cond = false;
            if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Line == hashLine)
            {
                std::string n = m_Cur.m_Text; Advance();
                cond = m_Defines.count(n) > 0 || m_TypeAliases.count(n) > 0;
            }
            SkipRestOfLine(hashLine);
            bool curr = parentActive && cond;
            m_CondStack.push_back({parentActive, curr, curr, false, hashLine});
            return;
        }

        // ---- #ifndef ----
        if (directive == "ifndef")
        {
            bool parentActive = IsActive();
            bool cond = false;
            if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Line == hashLine)
            {
                std::string n = m_Cur.m_Text; Advance();
                cond = m_Defines.count(n) == 0 && m_TypeAliases.count(n) == 0;
            }
            SkipRestOfLine(hashLine);
            bool curr = parentActive && cond;
            m_CondStack.push_back({parentActive, curr, curr, false, hashLine});
            return;
        }

        // ---- #if ----
        if (directive == "if")
        {
            bool parentActive = IsActive();
            bool cond = parentActive ? EvalConditionLine(hashLine)
                                     : (SkipRestOfLine(hashLine), false);
            bool curr = parentActive && cond;
            m_CondStack.push_back({parentActive, curr, curr, false, hashLine});
            return;
        }

        // ---- #elif ----
        if (directive == "elif")
        {
            if (m_CondStack.empty())
                throw std::runtime_error(m_FilePath + ":" + std::to_string(hashLine) +
                                         ": '#elif' without matching '#if'");
            CondFrame& top = m_CondStack.back();
            if (top.inElse)
                throw std::runtime_error(m_FilePath + ":" + std::to_string(hashLine) +
                                         ": '#elif' after '#else'");
            bool canEval = top.parentActive && !top.anyBranchTaken;
            bool cond    = canEval ? EvalConditionLine(hashLine)
                                   : (SkipRestOfLine(hashLine), false);
            bool newlyActive    = canEval && cond;
            top.anyBranchTaken  = top.anyBranchTaken || newlyActive;
            top.currentlyActive = newlyActive;
            return;
        }

        // ---- #else ----
        if (directive == "else")
        {
            if (m_CondStack.empty())
                throw std::runtime_error(m_FilePath + ":" + std::to_string(hashLine) +
                                         ": '#else' without matching '#if'");
            CondFrame& top = m_CondStack.back();
            if (top.inElse)
                throw std::runtime_error(m_FilePath + ":" + std::to_string(hashLine) +
                                         ": '#else' after '#else'");
            SkipRestOfLine(hashLine);
            top.currentlyActive = top.parentActive && !top.anyBranchTaken;
            top.anyBranchTaken  = true;
            top.inElse          = true;
            return;
        }

        // ---- #endif ----
        if (directive == "endif")
        {
            if (m_CondStack.empty())
                throw std::runtime_error(m_FilePath + ":" + std::to_string(hashLine) +
                                         ": '#endif' without matching '#if'");
            // Capture the outermost conditional block verbatim for HLSL passthrough.
            // Inner (nested) blocks are part of their enclosing block's raw text.
            if (m_CondStack.size() == 1 && m_BodyNestingDepth == 0)
            {
                int openLine = m_CondStack.back().openHashLine;
                std::string block = GetRawLines(openLine, hashLine);
                if (!block.empty())
                    m_Result.m_PreprocPassthrough.push_back(std::move(block));
            }
            m_CondStack.pop_back();
            SkipRestOfLine(hashLine);
            return;
        }

        // Unknown directive: skip the rest of the line silently.
        SkipRestOfLine(hashLine);
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

        // Propagate extern type declarations from the included file.
        for (const auto& extName : inc.m_ExternTypeNames)
        {
            m_ExternMap.insert(extName);
            m_Result.m_ExternTypeNames.insert(extName);
        }
    }

    // -----------------------------------------------------------------------
    // Type building helpers
    // -----------------------------------------------------------------------
    std::shared_ptr<TypeRef> MakeBuiltin(const ScalarInfo& si, int vectorSize,
                                         bool bCreatedFromMatrix = false) const
    {
        auto bt = std::make_shared<BuiltinTypeRef>();
        bt->m_ScalarName          = si.m_Name;
        bt->m_ElementSize         = si.m_ElementSize;
        bt->m_Alignment_          = si.m_ElementSize;
        bt->m_VectorSize          = vectorSize;
        bt->m_bCreatedFromMatrix  = bCreatedFromMatrix;
        bt->m_Name = (vectorSize == 1) ? si.m_Name : si.m_Name + std::to_string(vectorSize);
        return bt;
    }

    std::shared_ptr<TypeRef> MakeArray(std::shared_ptr<TypeRef> elemType, int arraySize,
                                       bool bCreatedFromMatrix = false,
                                       const std::string& sizeExpr = "") const
    {
        auto node = std::make_shared<ArrayTypeRef>();
        node->m_ElementType         = elemType;
        node->m_ArraySize           = arraySize;
        node->m_bCreatedFromMatrix  = bCreatedFromMatrix;
        node->m_SizeExpr            = sizeExpr;
        node->m_Name = TypeDisplayName(node->m_ElementType) +
                       "[" + (sizeExpr.empty() ? std::to_string(arraySize) : sizeExpr) + "]";
        return node;
    }

    // -----------------------------------------------------------------------
    // matrix<T,r,c> or vector<T,n>
    // -----------------------------------------------------------------------
    std::shared_ptr<TypeRef> ParseTemplateType(const std::string& keyword)
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
            auto elem = MakeBuiltin(*si, vectorSize, true);
            if (arraySize == 1) return elem;
            return MakeArray(std::move(elem), arraySize, true);
        }
        return MakeBuiltin(*si, vectorSize);
    }

    // -----------------------------------------------------------------------
    // NonStructType: optional row/column_major qualifier + type name
    // -----------------------------------------------------------------------
    std::shared_ptr<TypeRef> ParseNonStructType()
    {
        m_LastResolvedAliasName.clear();
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
                auto elem = MakeBuiltin(info, vectorSize, true);
                if (arraySize == 1) return elem; // NxM where M=1 is just a vector
                return MakeArray(std::move(elem), arraySize, true);
            }

            // Starts with key but unrecognised suffix -> fall through to struct lookup
            break;
        }

        // Accumulate optional namespace/class qualifiers: Ident (:: Ident)*
        // This allows qualified extern types like "nvrhi::rt::IndirectInstanceDesc".
        // We only consume "::" tokens when the resulting qualified name is in m_ExternMap,
        // so we use a speculative approach: peek ahead and build the full name first.
        {
            // Save lexer state so we can backtrack if the qualified name is not an extern type.
            size_t savedPos  = m_Lex.m_Pos;
            int    savedLine = m_Lex.m_Line;
            Token  savedCur  = m_Cur;

            std::string qualName = name;
            bool bBuiltQualified = false;

            while (m_Cur.m_Kind == TokKind::Colon)
            {
                int colonLine = m_Cur.m_Line;
                // Save state before consuming the first ':'
                size_t sp2 = m_Lex.m_Pos; int sl2 = m_Lex.m_Line; Token sc2 = m_Cur;
                Advance(); // consume first ':'
                if (m_Cur.m_Kind != TokKind::Colon || m_Cur.m_Line != colonLine)
                {
                    // Not "::" — restore and stop
                    m_Lex.m_Pos = sp2; m_Lex.m_Line = sl2; m_Cur = sc2;
                    break;
                }
                Advance(); // consume second ':'
                if (m_Cur.m_Kind != TokKind::Ident)
                {
                    // Malformed qualified name — restore and stop
                    m_Lex.m_Pos = sp2; m_Lex.m_Line = sl2; m_Cur = sc2;
                    break;
                }
                qualName += "::" + m_Cur.m_Text;
                Advance();
                bBuiltQualified = true;
            }

            if (bBuiltQualified)
            {
                // Check if the fully-qualified name is a declared extern type.
                if (m_ExternMap.count(qualName))
                {
                    if (bIsRowMajor)
                        throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                                 ": row_major/column_major qualifier cannot be applied"
                                                 " to extern type '" + qualName + "'");
                    auto ext = std::make_shared<ExternTypeRef>();
                    ext->m_Name = qualName;
                    return ext;
                }
                // Qualified name not found — restore to just after the first identifier
                // so the simple-name lookups below can still run.
                m_Lex.m_Pos = savedPos; m_Lex.m_Line = savedLine; m_Cur = savedCur;
            }
        }

        // Named struct reference
        auto structIt = m_StructMap.find(name);
        if (structIt != m_StructMap.end())
        {
            auto sr = std::make_shared<StructTypeRef>();
            sr->m_Struct = structIt->second;
            return sr;
        }

        // Type alias defined via #define, typedef, or using
        auto aliasIt = m_TypeAliases.find(name);
        if (aliasIt != m_TypeAliases.end())
        {
            if (bIsRowMajor)
                throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                         ": row_major/column_major qualifier cannot be applied"
                                         " to type alias '" + name + "'");
            m_LastResolvedAliasName = name; // remember original name for HLSL output
            return aliasIt->second->Clone(); // return a fresh copy of the aliased type
        }

        // Externally-defined type declared with 'extern TypeName;' (simple unqualified name)
        if (m_ExternMap.count(name))
        {
            if (bIsRowMajor)
                throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                         ": row_major/column_major qualifier cannot be applied"
                                         " to extern type '" + name + "'");
            auto ext = std::make_shared<ExternTypeRef>();
            ext->m_Name = name;
            return ext;
        }

        throw std::runtime_error(m_FilePath + ":" + std::to_string(nameLine) +
                                 ": unrecognized type '" + name + "'");
    }

    // -----------------------------------------------------------------------
    // Returns true if the scalar type name is an integral type suitable for
    // use as an array size (int, uint, and their fixed-width aliases).
    // -----------------------------------------------------------------------
    static bool IsIntegralScalarType(const std::string& typeName)
    {
        return typeName == "int"      || typeName == "uint"
            || typeName == "int32_t"  || typeName == "uint32_t"
            || typeName == "int16_t"  || typeName == "uint16_t"
            || typeName == "int64_t"  || typeName == "uint64_t";
    }

    // -----------------------------------------------------------------------
    // Array dims: one or more [N], flattened to total count.
    // Also supports a single [SrInputName::ConstName] referencing a scalar
    // const of integral type from a previously-declared srinput block.
    // -----------------------------------------------------------------------
    std::shared_ptr<TypeRef> ParseArrayDims(std::shared_ptr<TypeRef> elemType)
    {
        int total = 1;
        std::string sizeExpr; // symbolic expression when size came from a scalar const ref
        int iterCount = 0;

        do
        {
            ++iterCount;

            if (m_Cur.m_Kind == TokKind::Ident)
            {
                // Possibly SrInputName::ConstName
                int tokenLine = m_Cur.m_Line;
                std::string srInputName = m_Cur.m_Text;
                Advance(); // consume the identifier

                // Expect first ':'
                if (m_Cur.m_Kind != TokKind::Colon)
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(tokenLine) +
                                             ": expected '::' after '" + srInputName +
                                             "' in array size; use SrInputName::ConstName syntax");
                Advance(); // consume first ':'

                // Expect second ':'
                if (m_Cur.m_Kind != TokKind::Colon)
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(tokenLine) +
                                             ": expected second ':' in '::' for array size"
                                             " (SrInputName::ConstName)");
                Advance(); // consume second ':'

                Token constNameTok = Expect(TokKind::Ident, "scalar constant name after '::'");

                // Symbolic size must be the only dimension (no multi-dim chaining)
                if (iterCount > 1 || total > 1)
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(tokenLine) +
                                             ": scalar const reference '"
                                             + srInputName + "::" + constNameTok.m_Text
                                             + "' cannot be combined with other array dimensions");

                // Look up srinput
                auto srIt = m_SrInputMap.find(srInputName);
                if (srIt == m_SrInputMap.end())
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(tokenLine) +
                                             ": unknown srinput '" + srInputName
                                             + "' referenced in array size"
                                             + "; srinput must be declared before this struct/cbuffer");

                const SrInputDef& srInputDef = m_Result.m_SrInputDefs[srIt->second];

                // Find the scalar const by name
                const ScalarConst* found = nullptr;
                for (const auto& sc : srInputDef.m_ScalarConsts)
                {
                    if (sc.m_Name == constNameTok.m_Text)
                    {
                        found = &sc;
                        break;
                    }
                }
                if (!found)
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(tokenLine) +
                                             ": srinput '" + srInputName + "' has no scalar const named '"
                                             + constNameTok.m_Text + "'");

                // Validate integral type
                if (!IsIntegralScalarType(found->m_TypeName))
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(tokenLine) +
                                             ": scalar const '" + srInputName + "::" + constNameTok.m_Text
                                             + "' has type '" + found->m_TypeName
                                             + "', which cannot be used as an array size;"
                                             " only integral types (int, uint, int32_t, uint32_t, etc.) are allowed");

                // Parse the value as an integer
                int n = 0;
                try { n = std::stoi(found->m_Value); }
                catch (...)
                {
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(tokenLine) +
                                             ": scalar const '" + srInputName + "::" + constNameTok.m_Text
                                             + "' value '" + found->m_Value
                                             + "' cannot be parsed as an integer for use as array size");
                }
                if (n < 1)
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(tokenLine) +
                                             ": scalar const '" + srInputName + "::" + constNameTok.m_Text
                                             + "' value " + std::to_string(n)
                                             + " is not a valid array size (must be >= 1)");

                sizeExpr = srInputName + "::" + constNameTok.m_Text;
                total = n;
            }
            else
            {
                // Literal integer dimension
                if (!sizeExpr.empty())
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                             ": cannot add more dimensions after symbolic array size '"
                                             + sizeExpr + "'");
                int n = ParseInteger();
                if (n < 1)
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                             ": array size must be >= 1");
                total *= n;
            }

            Expect(TokKind::RBracket, "]");
        }
        while (TryConsume(TokKind::LBracket));

        return MakeArray(std::move(elemType), total, false, sizeExpr);
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

        auto memberType = ParseNonStructType();
        std::string memberAliasName = m_LastResolvedAliasName; // non-empty if type was a macro alias

        do
        {
            Token nameTok = Expect(TokKind::Ident, "member name");
            auto fieldType = memberType->Clone(); // copy

            if (TryConsume(TokKind::LBracket))
                fieldType = ParseArrayDims(fieldType);

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
            mv.m_Type             = std::move(fieldType);
            mv.m_Name             = nameTok.m_Text;
            mv.m_OriginalTypeName = memberAliasName; // preserve macro name for HLSL output
            
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
        ++m_BodyNestingDepth; // entering struct/cbuffer body
        std::vector<MemberVariable> members;
        std::unordered_set<std::string> memberNames;
        while (!Check(TokKind::RBrace) && !Check(TokKind::Eof))
        {
            // Handle preprocessor directives (#if/#ifdef/#define/etc.) inside struct/cbuffer body.
            if (Check(TokKind::Hash))
            {
                int hashLine = m_Cur.m_Line;
                Advance();
                HandlePreprocessorDirective(hashLine);
                continue;
            }
            // Skip member declarations in inactive preprocessor branches.
            if (!IsActive()) { Advance(); continue; }

            // Handle 'extern TypeName;' declarations inside struct/cbuffer body.
            if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Text == "extern")
            {
                int externLine = m_Cur.m_Line;
                Advance(); // consume 'extern'
                HandleExternDecl(externLine);
                continue;
            }

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
        --m_BodyNestingDepth; // leaving struct/cbuffer body
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
            // Preprocessor directives (#include, #define, #ifdef, #if, #else, #endif, …)
            if (Check(TokKind::Hash))
            {
                int hashLine = m_Cur.m_Line;
                Advance(); // consume '#'; m_Cur is now the directive keyword
                HandlePreprocessorDirective(hashLine);
                continue;
            }

            // Skip top-level declarations that fall inside an inactive preprocessor branch.
            if (!IsActive()) { Advance(); continue; }

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
                m_Result.m_DeclOrder.push_back({ParseResult::DeclKind::Struct, m_Result.m_Structs.size() - 1});
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
                m_Result.m_DeclOrder.push_back({ParseResult::DeclKind::BufferDef, m_Result.m_BufferDefs.size() - 1});

                MemberVariable mv;
                {
                    auto srRef = std::make_shared<StructTypeRef>();
                    srRef->m_Struct = &m_Result.m_BufferDefs.back();
                    mv.m_Type = std::move(srRef);
                }
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
                MemberVariable innerMv;
                {
                    auto innerRef = std::make_shared<StructTypeRef>();
                    innerRef->m_Struct = it->second;
                    innerMv.m_Type = std::move(innerRef);
                }
                innerMv.m_Name = "";
                outer.m_Members.push_back(std::move(innerMv));
                m_Result.m_BufferDefs.push_back(std::move(outer));
                m_Result.m_DeclOrder.push_back({ParseResult::DeclKind::BufferDef, m_Result.m_BufferDefs.size() - 1});

                MemberVariable mv;
                {
                    auto srRef = std::make_shared<StructTypeRef>();
                    srRef->m_Struct = &m_Result.m_BufferDefs.back();
                    mv.m_Type = std::move(srRef);
                }
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
                auto templateType = ParseNonStructType();
                Expect(TokKind::RAngle, ">");
                Token varTok = Expect(TokKind::Ident, "variable name");
                while (TryConsume(TokKind::LBracket)) { ParseInteger(); Expect(TokKind::RBracket, "]"); }
                SkipOptionalRegisterBinding();
                Expect(TokKind::Semicolon, ";");

                StructType outer{ varTok.m_Text, {} };
                MemberVariable innerMv; innerMv.m_Type = std::move(templateType); innerMv.m_Name = "";
                outer.m_Members.push_back(std::move(innerMv));
                m_Result.m_BufferDefs.push_back(std::move(outer));
                m_Result.m_DeclOrder.push_back({ParseResult::DeclKind::BufferDef, m_Result.m_BufferDefs.size() - 1});

                MemberVariable mv;
                auto srRef = std::make_shared<StructTypeRef>();
                srRef->m_Struct = &m_Result.m_BufferDefs.back();
                mv.m_Type      = std::move(srRef);
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
                ++m_BodyNestingDepth; // suppress passthrough capture inside srinput body

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
                    // Handle preprocessor directives inside srinput body.
                    if (Check(TokKind::Hash))
                    {
                        int hashLine = m_Cur.m_Line;
                        Advance();
                        HandlePreprocessorDirective(hashLine);
                        continue;
                    }
                    // Skip declarations in inactive preprocessor branches.
                    if (!IsActive()) { Advance(); continue; }

                    // Handle 'extern TypeName;' declarations inside srinput body.
                    if (m_Cur.m_Kind == TokKind::Ident && m_Cur.m_Text == "extern")
                    {
                        int externLine = m_Cur.m_Line;
                        Advance(); // consume 'extern'
                        HandleExternDecl(externLine);
                        continue;
                    }

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
                        std::string fullTypeName    = typeName.m_Text;
                        std::string originalTemplateArg; // raw identifier from .sr (may be an external macro)

                        if (!bNoTemplateArg && Check(TokKind::LAngle))
                        {
                            // Parse <templateArg> — may be a builtin type, struct name, type alias,
                            // or an externally-defined macro name (validated by DXC, not by the parser).
                            // Also supports namespace/class qualified names like "nvrhi::rt::Foo".
                            Advance(); // consume '<'
                            if (m_Cur.m_Kind != TokKind::Ident)
                                throw std::runtime_error(m_FilePath + ":" + std::to_string(m_Cur.m_Line) +
                                                         ": expected type argument inside '<>'");
                            Token argTok = m_Cur; Advance();
                            originalTemplateArg = argTok.m_Text; // preserve raw name before alias resolution
                            templateArg         = argTok.m_Text;

                            // Accumulate optional namespace/class qualifiers: Ident (:: Ident)*
                            // This allows qualified extern types like "nvrhi::rt::IndirectInstanceDesc".
                            while (m_Cur.m_Kind == TokKind::Colon)
                            {
                                int colonLine = m_Cur.m_Line;
                                size_t sp2 = m_Lex.m_Pos; int sl2 = m_Lex.m_Line; Token sc2 = m_Cur;
                                Advance(); // consume first ':'
                                if (m_Cur.m_Kind != TokKind::Colon || m_Cur.m_Line != colonLine)
                                {
                                    m_Lex.m_Pos = sp2; m_Lex.m_Line = sl2; m_Cur = sc2;
                                    break;
                                }
                                Advance(); // consume second ':'
                                if (m_Cur.m_Kind != TokKind::Ident)
                                {
                                    m_Lex.m_Pos = sp2; m_Lex.m_Line = sl2; m_Cur = sc2;
                                    break;
                                }
                                templateArg         += "::" + m_Cur.m_Text;
                                originalTemplateArg += "::" + m_Cur.m_Text;
                                Advance();
                            }

                            // Resolve type alias if the token is a known macro / typedef / using.
                            auto aliasIt = m_TypeAliases.find(templateArg);
                            if (aliasIt != m_TypeAliases.end())
                                templateArg = TypeDisplayName(aliasIt->second);

                            // Validate the resolved type: must be a known builtin, a known struct, or
                            // an unresolved identifier treated as an externally-defined macro.
                            // Exception: StructuredBuffer/RWStructuredBuffer require a named struct.
                            bool bIsBuiltin = false;
                            for (auto& [key, info] : g_Scalars)
                                if (templateArg.rfind(key, 0) == 0) { bIsBuiltin = true; break; }

                            bool bRequiresStruct = (foundKind == ResourceKind::StructuredBuffer ||
                                                    foundKind == ResourceKind::RWStructuredBuffer);

                            if (!bIsBuiltin && m_StructMap.find(templateArg) == m_StructMap.end())
                            {
                                if (bRequiresStruct)
                                {
                                    // Allow extern types as StructuredBuffer/RWStructuredBuffer template args.
                                    if (m_ExternMap.count(templateArg) == 0)
                                    {
                                        // Struct-typed resources must reference a visible struct definition
                                        // or a declared extern type.
                                        throw std::runtime_error(m_FilePath + ":" + std::to_string(argTok.m_Line) +
                                                                 ": resource template argument '" + originalTemplateArg +
                                                                 "' is not a defined struct or extern type");
                                    }
                                    // extern type used as StructuredBuffer template arg — valid
                                    templateArg = originalTemplateArg; // use the raw name (no srrhi:: prefix)
                                }
                                else
                                {
                                    // Other resource types: treat as externally-defined macro (e.g. SPD_TYPE).
                                    // DXC will validate at compile time.
                                    templateArg = originalTemplateArg;
                                }
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
                        rm.m_Kind                = foundKind;
                        rm.m_TypeName            = fullTypeName;
                        rm.m_TemplateArg         = templateArg;
                        rm.m_OriginalTemplateArg = (originalTemplateArg != templateArg) ? originalTemplateArg : "";
                        rm.m_MemberName          = memberName.m_Text;
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

                --m_BodyNestingDepth;
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
                m_Result.m_DeclOrder.push_back({ParseResult::DeclKind::SrInput, m_Result.m_SrInputDefs.size() - 1});
                continue;
            }

            // ---- extern TypeName; ----
            if (kw == "extern")
            {
                Advance(); // consume 'extern'
                HandleExternDecl(m_Cur.m_Line);
                continue;
            }

            // ---- typedef BaseType AliasName; ----
            if (kw == "typedef")
            {
                Advance(); // consume 'typedef'
                auto baseType = ParseNonStructType();
                Token aliasName  = Expect(TokKind::Ident, "alias name after base type in typedef");
                Expect(TokKind::Semicolon, ";");
                // Reconstruct a HLSL-compatible typedef line for passthrough (file-scope only).
                if (m_CondStack.empty())
                {
                    std::string hlslLine = "typedef " + TypeDisplayName(baseType) + " " + aliasName.m_Text + ";\n";
                    m_Result.m_PreprocPassthrough.push_back(std::move(hlslLine));
                }
                m_TypeAliases[aliasName.m_Text] = std::move(baseType);
                continue;
            }

            // ---- using AliasName = BaseType; ----
            if (kw == "using")
            {
                Advance(); // consume 'using'
                Token aliasName = Expect(TokKind::Ident, "alias name after 'using'");
                if (m_Cur.m_Kind != TokKind::Unknown || m_Cur.m_Text != "=")
                    throw std::runtime_error(m_FilePath + ":" + std::to_string(aliasName.m_Line) +
                                             ": expected '=' after alias name in 'using' declaration");
                Advance(); // consume '='
                auto baseType = ParseNonStructType();
                Expect(TokKind::Semicolon, ";");
                // Reconstruct a HLSL-compatible typedef line for passthrough (file-scope only).
                if (m_CondStack.empty())
                {
                    std::string hlslLine = "typedef " + TypeDisplayName(baseType) + " " + aliasName.m_Text + ";\n";
                    m_Result.m_PreprocPassthrough.push_back(std::move(hlslLine));
                }
                m_TypeAliases[aliasName.m_Text] = std::move(baseType);
                continue;
            }

            Advance(); // unknown top-level keyword
        }

        // Check that every #if / #ifdef has a matching #endif.
        if (!m_CondStack.empty())
            throw std::runtime_error(m_FilePath +
                                     ": unclosed '#if' or '#ifdef' directive; missing '#endif'");
    }
};

// ---------------------------------------------------------------------------
// TypeRef helpers (also used in layout.cpp and codegen)
// These free functions delegate to the virtual methods on the TypeRef hierarchy.
// ---------------------------------------------------------------------------
// (TypeAlignment and TypeDisplayName are inline in types.h; definitions here
//  are kept as thin wrappers for any translation unit that only sees the header.)

// ---------------------------------------------------------------------------
// LayoutMember helpers
// ---------------------------------------------------------------------------
void LayoutMember::SetPadding(int p)
{
    m_Padding = p;
    // Propagate into last array-element submember
    if (m_Type && m_Type->IsArray() && !m_Submembers.empty())
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
    VerboseMsg("[parser] Opening: %s\n", path.c_str());

    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open file: " + path);

    std::ostringstream ss;
    ss << f.rdbuf();

    ParseResult result;
    result.m_SourceFile = path;

    Parser p(ss.str(), result, path);
    p.Parse();

    VerboseMsg("[parser] Done: %zu structs, %zu buffers, %zu srinput(s)\n",
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