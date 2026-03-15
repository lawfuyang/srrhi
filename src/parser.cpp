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
static thread_local std::unordered_set<std::string> g_includedFiles;

static std::string resolveIncludePath(const std::string& baseFile,
                                      const std::string& includeFile)
{
    fs::path baseDirPath = fs::path(baseFile).parent_path();
    return fs::absolute(baseDirPath / includeFile).string();
}

// ---------------------------------------------------------------------------
// Built-in scalar type table
// Ordered longest-names-first so prefix matching works correctly.
// ---------------------------------------------------------------------------
struct ScalarInfo { std::string name; int elementsize; };

static const std::vector<std::pair<std::string, ScalarInfo>> g_scalars = {
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

struct Token { TokKind kind; std::string text; int line; };

struct Lexer
{
    std::string src;
    size_t pos  = 0;
    int    line = 1;

    explicit Lexer(const std::string& s) : src(s) {}

    void skipWhitespaceAndComments()
    {
        while (pos < src.size())
        {
            char c = src[pos];
            if (c == '\n') { ++line; ++pos; }
            else if (std::isspace((unsigned char)c)) { ++pos; }
            else if (pos + 1 < src.size() && c == '/' && src[pos + 1] == '/')
            {
                while (pos < src.size() && src[pos] != '\n') ++pos;
            }
            else if (pos + 1 < src.size() && c == '/' && src[pos + 1] == '*')
            {
                pos += 2;
                while (pos + 1 < src.size() &&
                       !(src[pos] == '*' && src[pos + 1] == '/'))
                {
                    if (src[pos] == '\n') ++line;
                    ++pos;
                }
                pos += 2;
            }
            else break;
        }
    }

    Token next()
    {
        skipWhitespaceAndComments();
        if (pos >= src.size()) return { TokKind::Eof, "", line };

        int startLine = line;
        char c = src[pos];

        if (c == '"')
        {
            ++pos;
            size_t start = pos;
            while (pos < src.size() && src[pos] != '"')
            {
                if (src[pos] == '\n') ++line;
                ++pos;
            }
            std::string str = src.substr(start, pos - start);
            if (pos < src.size()) ++pos;
            return { TokKind::String, str, startLine };
        }

        if (std::isalpha((unsigned char)c) || c == '_')
        {
            size_t start = pos;
            while (pos < src.size() &&
                   (std::isalnum((unsigned char)src[pos]) || src[pos] == '_'))
                ++pos;
            return { TokKind::Ident, src.substr(start, pos - start), startLine };
        }

        if (std::isdigit((unsigned char)c))
        {
            size_t start = pos;
            while (pos < src.size() && std::isdigit((unsigned char)src[pos]))
                ++pos;
            return { TokKind::Number, src.substr(start, pos - start), startLine };
        }

        ++pos;
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

    Token peek()
    {
        size_t s = pos; int l = line;
        Token t  = next();
        pos = s; line = l;
        return t;
    }
};

// ---------------------------------------------------------------------------
// Forward declaration
// ---------------------------------------------------------------------------
static ParseResult parseFileInternal(const std::string& path);

// ---------------------------------------------------------------------------
// Pointer fixup helpers for include merging
// When structs are moved from inc.structs → result.structs, any StructType*
// inside the moved structs must be remapped to the new addresses.
// ---------------------------------------------------------------------------
static void fixupTypeRef(TypeRef& t,
                         const std::unordered_map<StructType*, StructType*>& remap)
{
    if (auto* sp = std::get_if<StructType*>(&t))
    {
        auto it = remap.find(*sp);
        if (it != remap.end()) *sp = it->second;
    }
    else if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&t))
    {
        fixupTypeRef((*ap)->elementType, remap);
    }
}

static void fixupMembers(std::vector<MemberVariable>& members,
                         const std::unordered_map<StructType*, StructType*>& remap)
{
    for (auto& mv : members)
        fixupTypeRef(mv.type, remap);
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
struct Parser
{
    Lexer        lex;
    Token        cur;
    ParseResult& result;
    std::string  filePath;

    // Maps struct name to pointer into result.structs (stable due to deque)
    std::unordered_map<std::string, StructType*> structMap;

    Parser(const std::string& src, ParseResult& r, const std::string& path)
        : lex(src), result(r), filePath(path)
    {
        advance();
    }

    void advance() { cur = lex.next(); }

    Token expect(TokKind k, const std::string& what)
    {
        if (cur.kind != k)
            throw std::runtime_error(filePath + ":" + std::to_string(cur.line) +
                                     ": expected " + what + ", got '" + cur.text + "'");
        Token t = cur; advance(); return t;
    }

    bool check(TokKind k) const { return cur.kind == k; }
    bool tryConsume(TokKind k)
    {
        if (cur.kind != k) return false;
        advance(); return true;
    }

    int parseInteger()
    {
        Token t = expect(TokKind::Number, "integer");
        return std::stoi(t.text);
    }

    // -----------------------------------------------------------------------
    // Include handling
    // -----------------------------------------------------------------------
    void parseInclude()
    {
        int hashLine = cur.line;
        advance(); // '#'

        if (cur.kind != TokKind::Ident || cur.text != "include")
            throw std::runtime_error(filePath + ":" + std::to_string(cur.line) +
                                     ": expected 'include' after '#'");
        advance();

        if (cur.kind != TokKind::String)
            throw std::runtime_error(filePath + ":" + std::to_string(cur.line) +
                                     ": expected filename after '#include'");
        std::string includeFile = cur.text;
        advance();

        std::string resolvedPath = resolveIncludePath(filePath, includeFile);
        if (g_includedFiles.count(resolvedPath))
            throw std::runtime_error(filePath + ":" + std::to_string(hashLine) +
                                     ": circular include: " + resolvedPath);

        g_includedFiles.insert(resolvedPath);
        ParseResult inc = parseFileInternal(resolvedPath);

        // Collect old addresses and names BEFORE any moves (deque elements
        // are at fixed addresses, but their contents will be moved-from).
        std::vector<std::pair<StructType*, std::string>> entries;
        entries.reserve(inc.structs.size());
        for (auto& st : inc.structs)
            entries.push_back({ &st, st.name });

        // Move structs into result and build old→new remap.
        std::unordered_map<StructType*, StructType*> remap;
        remap.reserve(entries.size());
        for (auto& [oldPtr, name] : entries)
        {
            if (structMap.count(name))
                throw std::runtime_error(filePath + ":" + std::to_string(hashLine) +
                                         ": struct '" + name + "' already defined");
            result.structs.push_back(std::move(*oldPtr));
            StructType* newPtr = &result.structs.back();
            remap[oldPtr]   = newPtr;
            structMap[name] = newPtr;
        }

        // Fix up StructType* pointers inside the newly merged struct members.
        // They may have pointed to structs within `inc.structs` (now stale).
        size_t baseIdx = result.structs.size() - entries.size();
        for (size_t i = baseIdx; i < result.structs.size(); ++i)
            fixupMembers(result.structs[i].members, remap);
    }

    // -----------------------------------------------------------------------
    // Type building helpers
    // -----------------------------------------------------------------------
    TypeRef makeBuiltin(const ScalarInfo& si, int vectorsize,
                        bool created_from_matrix = false) const
    {
        BuiltinType bt;
        bt.scalarName         = si.name;
        bt.elementsize        = si.elementsize;
        bt.alignment          = si.elementsize;
        bt.vectorsize         = vectorsize;
        bt.created_from_matrix= created_from_matrix;
        bt.name = (vectorsize == 1) ? si.name : si.name + std::to_string(vectorsize);
        return bt;
    }

    TypeRef makeArray(TypeRef elemType, int arraySize,
                      bool created_from_matrix = false,
                      bool is_row_major = false) const
    {
        auto node = std::make_shared<ArrayNode>();
        node->elementType          = elemType;
        node->arraySize            = arraySize;
        node->created_from_matrix  = created_from_matrix;
        node->is_row_major         = is_row_major;
        node->name = typeDisplayName(node->elementType) +
                     "[" + std::to_string(arraySize) + "]";
        return node;
    }

    // -----------------------------------------------------------------------
    // matrix<T,r,c> or vector<T,n>
    // -----------------------------------------------------------------------
    TypeRef parseTemplateType(const std::string& keyword, bool is_row_major)
    {
        bool isMatrix = (keyword == "matrix");
        std::string scalarKey = "float";
        int vectorsize = 4, arraysize = 4;

        if (tryConsume(TokKind::LAngle))
        {
            Token st = expect(TokKind::Ident, "scalar type");
            bool found = false;
            for (auto& [key, info] : g_scalars)
                if (key == st.text) { scalarKey = key; found = true; break; }
            if (!found)
                throw std::runtime_error(filePath + ":" + std::to_string(st.line) +
                                         ": unknown scalar '" + st.text + "'");
            if (tryConsume(TokKind::Comma))
                vectorsize = parseInteger();
            if (isMatrix && tryConsume(TokKind::Comma))
                arraysize = parseInteger();
            expect(TokKind::RAngle, ">");
        }

        const ScalarInfo* si = nullptr;
        for (auto& [key, info] : g_scalars)
            if (key == scalarKey) { si = &info; break; }

        if (isMatrix)
        {
            if (is_row_major) std::swap(vectorsize, arraysize);
            TypeRef elem = makeBuiltin(*si, vectorsize, true);
            if (arraysize == 1) return elem;
            return makeArray(std::move(elem), arraysize, true, is_row_major);
        }
        return makeBuiltin(*si, vectorsize);
    }

    // -----------------------------------------------------------------------
    // NonStructType: optional row/column_major qualifier + type name
    // -----------------------------------------------------------------------
    TypeRef parseNonStructType()
    {
        bool is_row_major = false;
        if (cur.kind == TokKind::Ident &&
            (cur.text == "row_major" || cur.text == "column_major"))
        {
            is_row_major = (cur.text == "row_major");
            advance();
        }

        if (cur.kind != TokKind::Ident)
            throw std::runtime_error(filePath + ":" + std::to_string(cur.line) +
                                     ": expected type name");

        std::string name = cur.text;
        int nameLine     = cur.line;
        advance();

        if (name == "matrix" || name == "vector")
            return parseTemplateType(name, is_row_major);

        // Check scalar type table (longest-match, table is already ordered)
        for (auto& [key, info] : g_scalars)
        {
            if (name.rfind(key, 0) != 0) continue; // doesn't start with this key

            std::string suffix = name.substr(key.size());

            if (suffix.empty())
            {
                if (is_row_major)
                    throw std::runtime_error(filePath + ":" + std::to_string(nameLine) +
                                             ": row_major/column_major on non-matrix type");
                return makeBuiltin(info, 1);
            }

            if (suffix.size() == 1 && std::isdigit((unsigned char)suffix[0]))
            {
                int vs = suffix[0] - '0';
                if (vs < 1 || vs > 4)
                    throw std::runtime_error(filePath + ":" + std::to_string(nameLine) +
                                             ": invalid vector size");
                if (is_row_major)
                    throw std::runtime_error(filePath + ":" + std::to_string(nameLine) +
                                             ": row_major/column_major on non-matrix type");
                return makeBuiltin(info, vs);
            }

            if (suffix.size() == 3 && std::isdigit((unsigned char)suffix[0]) &&
                suffix[1] == 'x'  && std::isdigit((unsigned char)suffix[2]))
            {
                int rows = suffix[0] - '0';
                int cols = suffix[2] - '0';
                if (rows < 1 || rows > 4 || cols < 1 || cols > 4)
                    throw std::runtime_error(filePath + ":" + std::to_string(nameLine) +
                                             ": matrix dimension out of range");
                int vectorsize = is_row_major ? cols : rows;
                int arraysize  = is_row_major ? rows : cols;
                TypeRef elem = makeBuiltin(info, vectorsize, true);
                if (arraysize == 1) return elem; // NxM where M=1 is just a vector
                return makeArray(std::move(elem), arraysize, true, is_row_major);
            }

            // Starts with key but unrecognised suffix -> fall through to struct lookup
            break;
        }

        // Named struct reference
        auto it = structMap.find(name);
        if (it == structMap.end())
            throw std::runtime_error(filePath + ":" + std::to_string(nameLine) +
                                     ": unrecognized type '" + name + "'");
        return it->second; // StructType*
    }

    // -----------------------------------------------------------------------
    // Array dims: one or more [N], flattened to total count
    // -----------------------------------------------------------------------
    TypeRef parseArrayDims(TypeRef elemType)
    {
        int total = 1;
        do
        {
            int n = parseInteger();
            if (n < 1)
                throw std::runtime_error(filePath + ":" + std::to_string(cur.line) +
                                         ": array size must be >= 1");
            total *= n;
            expect(TokKind::RBracket, "]");
        }
        while (tryConsume(TokKind::LBracket));

        return makeArray(std::move(elemType), total, false, false);
    }

    // -----------------------------------------------------------------------
    // Member variable declaration(s): type name[dims], name[dims]; or ,
    // -----------------------------------------------------------------------
    void parseMemberVariables(std::vector<MemberVariable>& out)
    {
        // Reject anonymous/inner struct
        if (cur.kind == TokKind::Ident && cur.text == "struct")
            throw std::runtime_error(filePath + ":" + std::to_string(cur.line) +
                                     ": anonymous/inner struct definitions are not supported");

        TypeRef memberType = parseNonStructType();

        do
        {
            Token nameTok = expect(TokKind::Ident, "member name");
            TypeRef fieldType = memberType; // copy

            if (tryConsume(TokKind::LBracket))
                fieldType = parseArrayDims(std::move(fieldType));

            MemberVariable mv;
            mv.type = std::move(fieldType);
            mv.name = nameTok.text;
            out.push_back(std::move(mv));
        }
        while (tryConsume(TokKind::Comma));

        expect(TokKind::Semicolon, ";");
    }

    // -----------------------------------------------------------------------
    // Struct body: { member... }
    // -----------------------------------------------------------------------
    std::vector<MemberVariable> parseStructBody()
    {
        expect(TokKind::LBrace, "{");
        std::vector<MemberVariable> members;
        while (!check(TokKind::RBrace) && !check(TokKind::Eof))
            parseMemberVariables(members);
        expect(TokKind::RBrace, "}");
        return members;
    }

    // -----------------------------------------------------------------------
    // Optional register binding: ": register(rN)" - parse and discard
    // -----------------------------------------------------------------------
    void skipOptionalRegisterBinding()
    {
        if (!check(TokKind::Colon)) return;
        advance(); // ':'
        if (cur.kind != TokKind::Ident || cur.text != "register")
            throw std::runtime_error(filePath + ":" + std::to_string(cur.line) +
                                     ": expected 'register' after ':'");
        advance();
        // '(' is Unknown token since we don't have a dedicated Lparen
        if (cur.kind != TokKind::Unknown || cur.text != "(")
            throw std::runtime_error(filePath + ":" + std::to_string(cur.line) +
                                     ": expected '(' after 'register'");
        advance();
        while (!check(TokKind::Eof))
        {
            if (cur.kind == TokKind::Unknown && cur.text == ")")
            { advance(); break; }
            advance();
        }
    }

    // -----------------------------------------------------------------------
    // Top-level parse loop
    // -----------------------------------------------------------------------
    void parse()
    {
        while (!check(TokKind::Eof))
        {
            // #include
            if (check(TokKind::Hash)) { parseInclude(); continue; }

            if (cur.kind != TokKind::Ident) { advance(); continue; }

            std::string kw = cur.text;

            // ---- struct ----
            if (kw == "struct")
            {
                advance();
                Token nameTok = expect(TokKind::Ident, "struct name");
                if (structMap.count(nameTok.text))
                    throw std::runtime_error(filePath + ":" + std::to_string(nameTok.line) +
                                             ": struct '" + nameTok.text + "' already defined");

                auto members = parseStructBody();
                expect(TokKind::Semicolon, ";");

                result.structs.push_back({ nameTok.text, std::move(members) });
                structMap[nameTok.text] = &result.structs.back();
                continue;
            }

            // ---- cbuffer ----
            if (kw == "cbuffer")
            {
                advance();
                Token nameTok = expect(TokKind::Ident, "cbuffer name");
                skipOptionalRegisterBinding();
                auto members = parseStructBody();
                tryConsume(TokKind::Semicolon);

                StructType st{ nameTok.text, std::move(members) };
                result.bufferDefs.push_back(std::move(st));

                MemberVariable mv;
                mv.type      = &result.bufferDefs.back();
                mv.name      = "";
                mv.isCBuffer = true;
                result.buffers.push_back(std::move(mv));
                continue;
            }

            // ---- ConstantBuffer<T> name; ----
            if (kw == "ConstantBuffer")
            {
                advance();
                expect(TokKind::LAngle, "<");
                if (cur.kind != TokKind::Ident)
                    throw std::runtime_error(filePath + ":" + std::to_string(cur.line) +
                                             ": expected struct type in ConstantBuffer<>");
                Token innerTok = cur; advance();
                auto it = structMap.find(innerTok.text);
                if (it == structMap.end())
                    throw std::runtime_error(filePath + ":" + std::to_string(innerTok.line) +
                                             ": ConstantBuffer<> type '" + innerTok.text +
                                             "' not found");
                expect(TokKind::RAngle, ">");
                Token varTok = expect(TokKind::Ident, "variable name");
                while (tryConsume(TokKind::LBracket)) { parseInteger(); expect(TokKind::RBracket, "]"); }
                skipOptionalRegisterBinding();
                expect(TokKind::Semicolon, ";");

                // Wrap inner struct in outer struct
                StructType outer{ varTok.text, {} };
                MemberVariable innerMv; innerMv.type = it->second; innerMv.name = "";
                outer.members.push_back(std::move(innerMv));
                result.bufferDefs.push_back(std::move(outer));

                MemberVariable mv;
                mv.type      = &result.bufferDefs.back();
                mv.name      = varTok.text;
                mv.isCBuffer = true;
                result.buffers.push_back(std::move(mv));
                continue;
            }

            // ---- StructuredBuffer<T> name; ----
            if (kw == "StructuredBuffer")
            {
                advance();
                expect(TokKind::LAngle, "<");
                TypeRef templateType = parseNonStructType();
                expect(TokKind::RAngle, ">");
                Token varTok = expect(TokKind::Ident, "variable name");
                while (tryConsume(TokKind::LBracket)) { parseInteger(); expect(TokKind::RBracket, "]"); }
                skipOptionalRegisterBinding();
                expect(TokKind::Semicolon, ";");

                StructType outer{ varTok.text, {} };
                MemberVariable innerMv; innerMv.type = std::move(templateType); innerMv.name = "";
                outer.members.push_back(std::move(innerMv));
                result.bufferDefs.push_back(std::move(outer));

                MemberVariable mv;
                mv.type      = &result.bufferDefs.back();
                mv.name      = varTok.text;
                mv.isSBuffer = true;
                result.buffers.push_back(std::move(mv));
                continue;
            }

            // ---- typedef (skipped) ----
            if (kw == "typedef")
            {
                while (!check(TokKind::Semicolon) && !check(TokKind::Eof)) advance();
                tryConsume(TokKind::Semicolon);
                continue;
            }

            advance(); // unknown top-level keyword
        }
    }
};

// ---------------------------------------------------------------------------
// TypeRef helpers (also used in layout.cpp and codegen)
// ---------------------------------------------------------------------------
int typeAlignment(const TypeRef& t)
{
    if (auto* bt = std::get_if<BuiltinType>(&t))
        return bt->alignment;
    if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&t))
        return typeAlignment((*ap)->elementType);
    return 16; // StructType* -> always 16-byte aligned in cbuffers
}

std::string typeDisplayName(const TypeRef& t)
{
    if (auto* bt = std::get_if<BuiltinType>(&t))
        return bt->name;
    if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&t))
        return (*ap)->name;
    if (auto* sp = std::get_if<StructType*>(&t))
        return "struct " + (*sp)->name;
    return "???";
}

// ---------------------------------------------------------------------------
// LayoutMember helpers
// ---------------------------------------------------------------------------
void LayoutMember::setPadding(int p)
{
    padding = p;
    // Propagate into last array-element submember
    if (std::holds_alternative<std::shared_ptr<ArrayNode>>(type) && !submembers.empty())
        submembers.back().padding = p;
}

void LayoutMember::pushSubmember(LayoutMember m)
{
    if (!submembers.empty())
    {
        LayoutMember& last = submembers.back();
        int pad = m.offset - (last.offset + last.size);
        last.setPadding(pad);
    }
    submembers.push_back(std::move(m));
}

// ---------------------------------------------------------------------------
// Internal parse / public API
// ---------------------------------------------------------------------------
static ParseResult parseFileInternal(const std::string& path)
{
    logMsg("[parser] Opening: %s\n", path.c_str());

    std::ifstream f(path);
    if (!f.is_open())
        throw std::runtime_error("Cannot open file: " + path);

    std::ostringstream ss;
    ss << f.rdbuf();

    ParseResult result;
    result.sourceFile = path;

    Parser p(ss.str(), result, path);
    p.parse();

    logMsg("[parser] Done: %zu structs, %zu buffers\n",
           result.structs.size(), result.buffers.size());
    return result;
}

ParseResult parseFile(const std::string& path)
{
    g_includedFiles.clear();

    std::string absPath;
    try { absPath = fs::absolute(path).string(); }
    catch (...) { absPath = path; }

    g_includedFiles.insert(absPath);
    return parseFileInternal(absPath);
}