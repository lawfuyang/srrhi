#include "types.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cctype>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Static compile-time regex patterns for type parsing
// ---------------------------------------------------------------------------

// Pattern for HLSL scalar types with optional vector/matrix dimensions
// Matches: scalar, scalar#, scalar#x#
// E.g.: float, float3, float4x4, int2x2, half, double4, etc.
static const std::regex g_hlslScalarTypeRegex(
    R"(^(double|float|half|bool|uint|int)([1-4])?(x[1-4])?$)"
);

// ---------------------------------------------------------------------------
// Include tracking to prevent circular dependencies
// ---------------------------------------------------------------------------

static thread_local std::unordered_set<std::string> g_includedFiles;

// Resolve a file path relative to a base directory
static std::string resolveIncludePath(const std::string& baseDir, const std::string& includeFile)
{
    fs::path basePath(baseDir);
    fs::path baseDirPath = basePath.parent_path();
    fs::path fullPath = baseDirPath / includeFile;
    return fs::absolute(fullPath).string();
}

// ---------------------------------------------------------------------------
// Tokenizer
// ---------------------------------------------------------------------------

enum class TokKind
{
    Ident, Number, LBrace, RBrace, LBracket, RBracket, LAngle, RAngle,
    Semicolon, Comma, Star, Slash, Plus, Minus, Eq, Dot, Hash, String,
    Eof, Unknown
};

struct Token
{
    TokKind     kind;
    std::string text;
    int         line;
};

struct Lexer
{
    std::string src;
    size_t      pos  = 0;
    int         line = 1;

    explicit Lexer(const std::string& s) : src(s) {}

    void skipWhitespaceAndComments()
    {
        while (pos < src.size())
        {
            if (src[pos] == '\n') { ++line; ++pos; }
            else if (std::isspace((unsigned char)src[pos])) { ++pos; }
            else if (pos + 1 < src.size() && src[pos] == '/' && src[pos+1] == '/')
            {
                while (pos < src.size() && src[pos] != '\n') ++pos;
            }
            else if (pos + 1 < src.size() && src[pos] == '/' && src[pos+1] == '*')
            {
                pos += 2;
                while (pos + 1 < src.size() && !(src[pos] == '*' && src[pos+1] == '/'))
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
        if (pos >= src.size()) return {TokKind::Eof, "", line};

        int startLine = line;
        char c = src[pos];

        // Handle quoted strings for #include filenames
        if (c == '"')
        {
            ++pos;
            size_t start = pos;
            while (pos < src.size() && src[pos] != '"')
            {
                if (src[pos] == '\n') ++line;
                ++pos;
            }
            if (pos >= src.size())
            {
                throw std::runtime_error("Unterminated string literal at line " + std::to_string(startLine));
            }
            std::string str = src.substr(start, pos - start);
            ++pos;  // consume closing "
            return {TokKind::String, str, startLine};
        }

        if (std::isalpha((unsigned char)c) || c == '_')
        {
            size_t start = pos;
            while (pos < src.size() && (std::isalnum((unsigned char)src[pos]) || src[pos] == '_'))
                ++pos;
            return {TokKind::Ident, src.substr(start, pos - start), startLine};
        }
        if (std::isdigit((unsigned char)c) || (c == '-' && pos+1 < src.size() && std::isdigit((unsigned char)src[pos+1])))
        {
            size_t start = pos;
            if (c == '-') ++pos;
            while (pos < src.size() && std::isdigit((unsigned char)src[pos])) ++pos;
            return {TokKind::Number, src.substr(start, pos - start), startLine};
        }

        ++pos;
        switch (c)
        {
            case '{': return {TokKind::LBrace,    "{", startLine};
            case '}': return {TokKind::RBrace,    "}", startLine};
            case '[': return {TokKind::LBracket,  "[", startLine};
            case ']': return {TokKind::RBracket,  "]", startLine};
            case '<': return {TokKind::LAngle,    "<", startLine};
            case '>': return {TokKind::RAngle,    ">", startLine};
            case ';': return {TokKind::Semicolon, ";", startLine};
            case ',': return {TokKind::Comma,     ",", startLine};
            case '*': return {TokKind::Star,      "*", startLine};
            case '/': return {TokKind::Slash,     "/", startLine};
            case '+': return {TokKind::Plus,      "+", startLine};
            case '-': return {TokKind::Minus,     "-", startLine};
            case '=': return {TokKind::Eq,        "=", startLine};
            case '.': return {TokKind::Dot,       ".", startLine};
            case '#': return {TokKind::Hash,      "#", startLine};
            default:  return {TokKind::Unknown, std::string(1,c), startLine};
        }
    }

    Token peek()
    {
        size_t savedPos  = pos;
        int    savedLine = line;
        Token t = next();
        pos  = savedPos;
        line = savedLine;
        return t;
    }
};

// ---------------------------------------------------------------------------
// Parser context
// ---------------------------------------------------------------------------

// Forward declaration
static ParseResult parseFileInternal(const std::string& path);

struct Parser
{
    Lexer        lex;
    Token        cur;
    ParseResult& result;
    std::string  filePath;

    // map from struct name → index in result.structs
    std::unordered_map<std::string, size_t> structMap;

    Parser(const std::string& src, ParseResult& r, const std::string& path)
        : lex(src), result(r), filePath(path)
    {
        advance();
    }


    void advance() { cur = lex.next(); }

    Token expect(TokKind k, const std::string& what)
    {
        if (cur.kind != k)
        {
            throw std::runtime_error(
                filePath + ":" + std::to_string(cur.line) +
                ": expected " + what + ", got '" + cur.text + "'");
        }
        Token t = cur;
        advance();
        return t;
    }

    bool check(TokKind k) const { return cur.kind == k; }

    bool tryConsume(TokKind k)
    {
        if (cur.kind == k) { advance(); return true; }
        return false;
    }

    // -----------------------------------------------------------------------
    // Type parsing
    // -----------------------------------------------------------------------

    // Returns true if the identifier is a known HLSL scalar/vector/matrix type
    // or "matrix<...>" / "vector<...>"
    bool isHlslTypeName(const std::string& name) const
    {
        static const std::unordered_map<std::string,bool> known = {
            {"bool",1},{"bool1",1},{"bool2",1},{"bool3",1},{"bool4",1},
            {"half",1},{"half1",1},{"half2",1},{"half3",1},{"half4",1},
            {"float",1},{"float1",1},{"float2",1},{"float3",1},{"float4",1},
            {"double",1},{"double1",1},{"double2",1},{"double3",1},{"double4",1},
            {"int",1},{"int1",1},{"int2",1},{"int3",1},{"int4",1},
            {"uint",1},{"uint1",1},{"uint2",1},{"uint3",1},{"uint4",1},
            {"uint16_t",1},{"int16_t",1},
            {"float1x1",1},{"float1x2",1},{"float1x3",1},{"float1x4",1},
            {"float2x1",1},{"float2x2",1},{"float2x3",1},{"float2x4",1},
            {"float3x1",1},{"float3x2",1},{"float3x3",1},{"float3x4",1},
            {"float4x1",1},{"float4x2",1},{"float4x3",1},{"float4x4",1},
            {"int2x2",1},{"int3x3",1},{"int4x4",1},
            {"matrix",1},{"vector",1},
        };
        return known.count(name) > 0;
    }

    // Parse scalar kind from name
    ScalarKind scalarFromName(const std::string& name, int lineNo) const
    {
        if (name == "bool")   return ScalarKind::Bool;
        if (name == "half")   return ScalarKind::Half;
        if (name == "float")  return ScalarKind::Float;
        if (name == "double") return ScalarKind::Double;
        if (name == "int")    return ScalarKind::Int;
        if (name == "uint")   return ScalarKind::Uint;
        if (name == "uint16_t") return ScalarKind::Uint16;
        if (name == "int16_t")  return ScalarKind::Int16;
        throw std::runtime_error(
            filePath + ":" + std::to_string(lineNo) +
            ": unknown scalar type '" + name + "'");
    }

    // Parse an HLSL type like float, float3, float3x4, matrix<int,1,2>, vector<double,1>
    // Returns the HlslType. Caller already consumed the first identifier token.
    HlslType parseHlslTypeFromIdent(const std::string& ident, int lineNo)
    {
        HlslType t;

        // matrix<scalar, rows, cols>
        if (ident == "matrix")
        {
            expect(TokKind::LAngle, "<");
            std::string scalarName = expect(TokKind::Ident, "scalar type").text;
            t.scalar = scalarFromName(scalarName, lineNo);
            expect(TokKind::Comma, ",");
            int rows = std::stoi(expect(TokKind::Number, "rows").text);
            expect(TokKind::Comma, ",");
            int cols = std::stoi(expect(TokKind::Number, "cols").text);
            expect(TokKind::RAngle, ">");
            if (rows < 1 || rows > 4 || cols < 1 || cols > 4)
                throw std::runtime_error(filePath + ":" + std::to_string(lineNo) + ": matrix dimensions must be 1..4");
            t.rows = rows;
            t.cols = cols;
            t.declaredAsMatrix = true;
            return t;
        }

        // vector<scalar, N>
        if (ident == "vector")
        {
            expect(TokKind::LAngle, "<");
            std::string scalarName = expect(TokKind::Ident, "scalar type").text;
            t.scalar = scalarFromName(scalarName, lineNo);
            expect(TokKind::Comma, ",");
            int cols = std::stoi(expect(TokKind::Number, "cols").text);
            expect(TokKind::RAngle, ">");
            if (cols < 1 || cols > 4)
                throw std::runtime_error(filePath + ":" + std::to_string(lineNo) + ": vector size must be 1..4");
            t.rows = 1;
            t.cols = cols;
            return t;
        }

        // Special cases: uint16_t and int16_t
        if (ident == "uint16_t") { t.scalar = ScalarKind::Uint16; t.rows = 1; t.cols = 1; return t; }
        if (ident == "int16_t")  { t.scalar = ScalarKind::Int16;  t.rows = 1; t.cols = 1; return t; }

        // Standard HLSL types: scalar, scalar#, or scalar#x#
        // Pattern: (double|float|half|bool|uint|int)([1-4])?(x[1-4])?
        std::smatch matches;
        if (!std::regex_match(ident, matches, g_hlslScalarTypeRegex))
        {
            throw std::runtime_error(filePath+":"+std::to_string(lineNo)+": unrecognised HLSL type '"+ident+"'");
        }

        // Extract scalar type (group 1)
        std::string scalarStr = matches[1].str();
        t.scalar = scalarFromName(scalarStr, lineNo);

        // Extract row dimension (group 2) - only present if NxM format or N format
        std::string rowStr = matches[2].str();
        std::string colStr = matches[3].str();  // includes the 'x' prefix, needs trimming

        if (colStr.empty())
        {
            // Format: scalar or scalarN
            t.rows = 1;
            t.cols = rowStr.empty() ? 1 : std::stoi(rowStr);
        }
        else
        {
            // Format: scalarNxM
            // colStr is like "x4", need to remove the 'x'
            int cols = std::stoi(colStr.substr(1));
            t.rows = std::stoi(rowStr);
            t.cols = cols;
            t.declaredAsMatrix = true;
        }

        // Validate dimensions
        if (t.cols < 1 || t.cols > 4 || t.rows < 1 || t.rows > 4)
            throw std::runtime_error(filePath+":"+std::to_string(lineNo)+": invalid type dims in '"+ident+"'");

        return t;
    }

    // Parse array dimensions: zero or more [N] suffixes
    std::vector<ArrayDim> parseArrayDims()
    {
        std::vector<ArrayDim> dims;
        while (check(TokKind::LBracket))
        {
            advance(); // consume [
            int n = std::stoi(expect(TokKind::Number, "array size").text);
            expect(TokKind::RBracket, "]");
            dims.push_back({n});
        }
        return dims;
    }

    // -----------------------------------------------------------------------
    // Struct parsing
    // -----------------------------------------------------------------------

    // Parse a struct body (fields). cur is just past '{'.
    std::vector<Field> parseStructBody()
    {
        std::vector<Field> fields;

        while (!check(TokKind::RBrace) && !check(TokKind::Eof))
        {
            // Each statement: type name[dims][dims], name[dims]...; 
            // or: struct { ... } name[dims];
            // or: struct Name { ... } name[dims];

            if (cur.kind == TokKind::Ident && cur.text == "struct")
            {
                throw std::runtime_error(
                    filePath + ":" + std::to_string(cur.line) +
                    ": anonymous/inner struct definitions are not supported. "
                    "Define all structs at the top level.");
            }

            // Primitive type or named struct reference
            if (cur.kind != TokKind::Ident)
            {
                throw std::runtime_error(
                    filePath + ":" + std::to_string(cur.line) +
                    ": expected type name, got '" + cur.text + "'");
            }

            std::string typeName = cur.text;
            int         typeLine = cur.line;
            advance();

            // Validate that the type is either a native HLSL type or a declared struct
            bool isHlslType = isHlslTypeName(typeName);
            bool isStructRef = !isHlslType && structMap.count(typeName);
            
            if (!isHlslType && !isStructRef)
            {
                throw std::runtime_error(
                    filePath + ":" + std::to_string(typeLine) +
                    ": unrecognized type '" + typeName + "'; it must be either a native HLSL type "
                    "(bool, float, double, int, uint, half, int16_t, uint16_t, and their vector/matrix forms) "
                    "or a struct that has been declared in a visible scope");
            }
            
            // matrix<> or vector<> need angle-bracket parsing handled specially:
            // isHlslTypeName already marks "matrix"/"vector" as known

            if (isStructRef)
            {
                int stIdx = (int)structMap[typeName];
                // parse multiple declarators
                do {
                    std::string varName = expect(TokKind::Ident, "variable name").text;
                    auto dims = parseArrayDims();
                    Field f;
                    f.name          = varName;
                    f.isStruct      = true;
                    f.structTypeIdx = stIdx;
                    f.arrayDims     = dims;
                    fields.push_back(f);
                } while (tryConsume(TokKind::Comma));
                expect(TokKind::Semicolon, ";");
            }
            else
            {
                // HLSL primitive type
                HlslType ht = parseHlslTypeFromIdent(typeName, typeLine);

                // Multiple declarators: float a, b;
                do {
                    std::string varName = expect(TokKind::Ident, "variable name").text;
                    auto dims = parseArrayDims();
                    Field f;
                    f.name      = varName;
                    f.isStruct  = false;
                    f.hlslType  = ht;
                    f.arrayDims = dims;
                    fields.push_back(f);
                } while (tryConsume(TokKind::Comma));
                expect(TokKind::Semicolon, ";");
            }
        }
        return fields;
    }

    // -----------------------------------------------------------------------
    // Top-level parsing
    // -----------------------------------------------------------------------

    // Handle #include directive
    void parseInclude()
    {
        // cur is at Hash, next should be "include"
        int hashLine = cur.line;
        advance(); // consume #
        
        if (cur.kind != TokKind::Ident || cur.text != "include")
        {
            throw std::runtime_error(
                filePath + ":" + std::to_string(cur.line) + 
                ": expected 'include' after '#', got '" + cur.text + "'");
        }
        advance(); // consume "include"
        
        if (cur.kind != TokKind::String)
        {
            throw std::runtime_error(
                filePath + ":" + std::to_string(cur.line) + 
                ": expected filename string after '#include', got '" + cur.text + "'");
        }
        
        std::string includeFile = cur.text;
        advance(); // consume filename
        
        // Resolve the include path relative to the current file
        std::string resolvedPath = resolveIncludePath(filePath, includeFile);
        
        // Check for circular includes
        if (g_includedFiles.count(resolvedPath) > 0)
        {
            throw std::runtime_error(
                filePath + ":" + std::to_string(hashLine) + 
                ": circular include detected: " + resolvedPath);
        }
        
        // Mark this file as included
        g_includedFiles.insert(resolvedPath);
        
        // Parse the included file
        try
        {
            ParseResult includedResult = parseFileInternal(resolvedPath);
            
            // Merge structs from included file into result
            for (auto& st : includedResult.structs)
            {
                if (structMap.count(st.name) == 0)
                {
                    structMap[st.name] = result.structs.size();
                    result.structs.push_back(std::move(st));
                }
                else
                {
                    throw std::runtime_error(
                        filePath + ":" + std::to_string(hashLine) + 
                        ": struct '" + st.name + "' already defined");
                }
            }
            
            // Note: We don't merge cbuffers from includes, only structs
            // This matches standard C preprocessor behavior for includes
        }
        catch (const std::exception& e)
        {
            throw std::runtime_error(
                filePath + ":" + std::to_string(hashLine) + 
                ": error processing include '" + includeFile + "': " + e.what());
        }
    }

    void parse()
    {
        while (!check(TokKind::Eof))
        {
            // Handle #include directives
            if (cur.kind == TokKind::Hash)
            {
                parseInclude();
                continue;
            }

            if (cur.kind != TokKind::Ident)
            {
                // skip unknown top-level tokens
                advance();
                continue;
            }

            std::string kw = cur.text;

            if (kw == "struct")
            {
                advance();
                std::string name = expect(TokKind::Ident, "struct name").text;
                expect(TokKind::LBrace, "{");
                auto fields = parseStructBody();
                expect(TokKind::RBrace, "}");
                expect(TokKind::Semicolon, ";");

                StructType st;
                st.name   = name;
                st.fields = std::move(fields);
                result.structs.push_back(std::move(st));
                structMap[name] = result.structs.size() - 1;
                continue;
            }

            if (kw == "cbuffer")
            {
                advance();
                std::string name = expect(TokKind::Ident, "cbuffer name").text;
                expect(TokKind::LBrace, "{");
                auto fields = parseStructBody();
                expect(TokKind::RBrace, "}");
                expect(TokKind::Semicolon, ";");

                CBuffer cb;
                cb.name   = name;
                cb.fields = std::move(fields);
                result.cbuffers.push_back(std::move(cb));
                continue;
            }

            // Unknown keyword — skip to next semicolon or brace
            advance();
        }
    }
};

// ---------------------------------------------------------------------------
// ParseResult::resolvePointers — call after all parsing is done.
// Walks all fields in structs and cbuffers and fills structType* from index.
// ---------------------------------------------------------------------------

static void resolveFieldPointers(std::vector<Field>& fields, ParseResult& pr)
{
    for (auto& f : fields)
    {
        if (f.isStruct)
        {
            assert(f.structTypeIdx >= 0 && f.structTypeIdx < (int)pr.structs.size());
            f.structType = &pr.structs[f.structTypeIdx];
            // Recurse into the struct's own fields
            resolveFieldPointers(f.structType->fields, pr);
        }
    }
}

void ParseResult::resolvePointers()
{
    for (auto& st : structs)
        resolveFieldPointers(st.fields, *this);
    for (auto& cb : cbuffers)
        resolveFieldPointers(cb.fields, *this);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Internal parse function that doesn't clear the include set
static ParseResult parseFileInternal(const std::string& path)
{
    logMsg("[parser] Opening file: %s\n", path.c_str());
    
    std::ifstream f(path);
    if (!f.is_open())
    {
        logMsg("[parser] ERROR: Cannot open file: %s\n", path.c_str());
        throw std::runtime_error("Cannot open file: " + path);
    }

    std::ostringstream ss;
    ss << f.rdbuf();

    ParseResult result;
    result.sourceFile = path;

    logMsg("[parser] Parsing file...\n");
    Parser p(ss.str(), result, path);
    p.parse();

    // Now that the structs vector will not grow any more, resolve raw pointers
    result.resolvePointers();
    
    logMsg("[parser] Parse complete: %zu structs, %zu cbuffers\n", 
           result.structs.size(), result.cbuffers.size());

    return result;
}

ParseResult parseFile(const std::string& path)
{
    // Clear the include set for top-level parse
    bool wasEmpty = g_includedFiles.empty();
    if (wasEmpty)
    {
        g_includedFiles.clear();
    }

    try
    {
        // Get the absolute path for the main file
        std::string absPath = fs::absolute(path).string();
        g_includedFiles.insert(absPath);
        
        ParseResult result = parseFileInternal(path);
        
        // Clear includes on successful completion (only if this was a top-level call)
        if (wasEmpty)
        {
            g_includedFiles.clear();
        }
        
        return result;
    }
    catch (const std::exception& e)
    {
        // Clear includes on error (only if this was a top-level call)
        if (wasEmpty)
        {
            g_includedFiles.clear();
        }
        throw;
    }
}
