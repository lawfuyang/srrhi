#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "types.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
ParseResult ParseFile(const std::string& path);
std::vector<LayoutMember> ComputeLayouts(ParseResult& pr);
std::string GenerateHlsl(const ParseResult& pr, const std::vector<LayoutMember>& layouts, int& padCount);
std::string GenerateCpp(const ParseResult& pr, const std::vector<LayoutMember>& layouts, int& padCount, bool bEmitValidation);
std::string VisualizeLayouts(const std::vector<LayoutMember>& layouts);
std::string VisualizeLayoutsMachineReadable(const std::vector<LayoutMember>& layouts);
int RunReflectionTests(const fs::path& testInputDir);

// ---------------------------------------------------------------------------
// Global flags
// ---------------------------------------------------------------------------
bool g_Verbose = false; // -v or --test enables visualizer output to stdout

void LogMsg(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

// ---------------------------------------------------------------------------
// Utility: write a string to a file, creating parent dirs as needed
// ---------------------------------------------------------------------------
static void WriteFile(const fs::path& path, const std::string& content)
{
    try
    {
        fs::create_directories(path.parent_path());
    }
    catch (const std::exception& e)
    {
        throw std::runtime_error("Cannot create directories for '" +
                                 path.parent_path().string() + "': " + e.what());
    }

    FILE* f = fopen(path.string().c_str(), "w");
    if (!f)
        throw std::runtime_error("Cannot open file for writing: " + path.string());

    bool bOk = (fwrite(content.c_str(), 1, content.size(), f) == content.size());
    int closeErr = fclose(f);
    if (!bOk)
        throw std::runtime_error("Write error for file: " + path.string());
    if (closeErr != 0)
        throw std::runtime_error("Close error for file: " + path.string());
}

// ---------------------------------------------------------------------------
// Generate minimal validation .cpp stubs for each generated .h file.
// Each stub simply includes the header and compiles it.  All static_assert
// register-index checks are emitted directly by the C++ code generator into
// the header itself, so no extra logic is needed here.
// ---------------------------------------------------------------------------
static int GenerateValidationStubs(const fs::path& headerDir)
{
    LogMsg("[srrhi] Generating validation stubs in: %s\n", headerDir.string().c_str());

    if (!fs::exists(headerDir) || !fs::is_directory(headerDir))
    {
        LogMsg("[srrhi] ERROR: Header directory does not exist: %s\n",
               headerDir.string().c_str());
        return 1;
    }

    int count = 0;
    for (const auto& entry : fs::directory_iterator(headerDir))
    {
        if (!entry.is_regular_file()) continue;
        const fs::path& hFile = entry.path();
        if (hFile.extension() != ".h") continue;

        std::string headerName = hFile.filename().string();
        std::string stem       = headerName.substr(0, headerName.size() - 2); // strip ".h"
        std::string stubName   = "validation_" + stem + ".cpp";
        fs::path    stubPath   = headerDir / stubName;

        std::string code =
            "// Auto-generated validation stub for " + headerName + "\n"
            "// Compile-time layout and register-index validation is done via\n"
            "// static_assert statements emitted directly into the header.\n"
            "\n"
            "#include <windows.h>\n"
            "#include <DirectXMath.h>\n"
            "\n"
            "#include \"" + headerName + "\"\n"
            "\n"
            "int main() { return 0; }\n";

        try
        {
            WriteFile(stubPath, code);
            LogMsg("[srrhi]   -> %s\n", stubPath.string().c_str());
            ++count;
        }
        catch (const std::exception& e)
        {
            LogMsg("[srrhi] ERROR writing validation stub '%s': %s\n",
                   stubPath.string().c_str(), e.what());
            return 1;
        }
    }

    LogMsg("[srrhi] Generated %d validation stub(s).\n", count);
    return 0;
}

// ---------------------------------------------------------------------------
// Derive HLSL include-guard name from the output stem.
// e.g. stem "sub/example" -> "SSRHI_EXAMPLE_HLSLI"
// ---------------------------------------------------------------------------
static std::string MakeHlslGuard(const fs::path& stem)
{
    std::string name = "SSRHI_" + stem.filename().string() + "_HLSLI";
    for (char& c : name)
        c = std::isalnum((unsigned char)c) ? (char)std::toupper((unsigned char)c) : '_';
    return name;
}

// ---------------------------------------------------------------------------
// Resolve include path relative to base file
// ---------------------------------------------------------------------------
static std::string ResolveIncludePath(const std::string& baseFile,
                                      const std::string& includeFile)
{
    fs::path baseDirPath = fs::path(baseFile).parent_path();
    return fs::absolute(baseDirPath / includeFile).string();
}

// ---------------------------------------------------------------------------
// Recursively collect all files in the .sr file include hierarchy
// ---------------------------------------------------------------------------
static void CollectIncludedFiles(const std::string& srFilePath,
                                 std::unordered_set<std::string>& visited,
                                 std::vector<std::string>& allFiles)
{
    // Avoid infinite loops from circular includes
    if (visited.count(srFilePath))
        return;
    visited.insert(srFilePath);
    allFiles.push_back(srFilePath);

    // Try to read the file and extract #include directives
    std::ifstream f(srFilePath);
    if (!f.is_open())
        return; // If we can't read, just skip

    std::string line;
    while (std::getline(f, line))
    {
        // Look for #include directives
        // Pattern: #include "filename"
        size_t hashPos = line.find('#');
        if (hashPos == std::string::npos)
            continue;

        size_t includePos = line.find("include", hashPos);
        if (includePos == std::string::npos)
            continue;

        size_t quotePos = line.find('"', includePos);
        if (quotePos == std::string::npos)
            continue;

        size_t endQuotePos = line.find('"', quotePos + 1);
        if (endQuotePos == std::string::npos)
            continue;

        std::string includeFile = line.substr(quotePos + 1, endQuotePos - quotePos - 1);

        // Resolve the path (relative to the current file's directory)
        std::string resolvedPath = ResolveIncludePath(srFilePath, includeFile);

        // Recursively collect
        CollectIncludedFiles(resolvedPath, visited, allFiles);
    }
}

// ---------------------------------------------------------------------------
// Process one .sr file — throws on any unrecoverable error
// ---------------------------------------------------------------------------
static void ProcessFile(const fs::path& srFile,
                        const fs::path& inputRoot,
                        const fs::path& outputRoot,
                        int& globalPadCount,
                        bool bEmitValidation,
                        const fs::path& exePath)
{
    LogMsg("[srrhi] Processing: %s\n", srFile.string().c_str());

    // --- Parse ---
    ParseResult pr;
    try
    {
        pr = ParseFile(srFile.string());
        long cbufferCount = 0;
        for (auto& b : pr.m_Buffers) if (b.m_bIsCBuffer) ++cbufferCount;
        VerboseMsg("  Parse OK: %ld cbuffer(s), %zu struct(s)\n",
               cbufferCount, pr.m_Structs.size());
    }
    catch (const std::exception& e)
    {
        LogMsg("  ERROR (parse): %s\n", e.what());
        throw std::runtime_error(
            std::string("Parse failed for '") + srFile.string() + "': " + e.what());
    }

    // --- Compute layouts ---
    std::vector<LayoutMember> layouts;
    try
    {
        layouts = ComputeLayouts(pr);
        VerboseMsg("  Layout OK: %zu layout(s) computed\n", layouts.size());
    }
    catch (const std::exception& e)
    {
        LogMsg("  ERROR (layout): %s\n", e.what());
        throw std::runtime_error(
            std::string("Layout computation failed for '") + srFile.string() + "': " + e.what());
    }

    // --- Visualizer ---
    if (g_Verbose)
    {
        try
        {
            std::string vis = VisualizeLayouts(layouts);
            std::string visMR = VisualizeLayoutsMachineReadable(layouts);

            printf("=== %s ===\n", srFile.string().c_str());
            fwrite(vis.c_str(), 1, vis.size(), stdout);
            // Machine-readable section, clearly delimited
            printf("--- machine-readable ---\n");
            fwrite(visMR.c_str(), 1, visMR.size(), stdout);
            printf("\n");
            fflush(stdout);

            VerboseMsg("  Visualizer: %zu bytes for %s\n",
                vis.size(), srFile.filename().string().c_str());
        }
        catch (const std::exception& e)
        {
            LogMsg("  ERROR (visualizer): %s\n", e.what());
            throw std::runtime_error(
                std::string("Visualizer failed for '") + srFile.string() + "': " + e.what());
        }
    }

    // Relative stem: e.g. "sub/example" for use in output paths and guard names
    fs::path relPath = fs::relative(srFile, inputRoot);
    fs::path stem    = relPath.parent_path() / srFile.stem();

    // Collect all files in the include hierarchy
    std::unordered_set<std::string> visited;
    std::vector<std::string> allIncludedFiles;
    CollectIncludedFiles(srFile.string(), visited, allIncludedFiles);

    // Returns true if outPath needs to be (re)written:
    // either it doesn't exist yet, or its timestamp is older than the source .sr file or any included file,
    // or if the current executable is newer than outPath.
    auto NeedsWrite = [&](const fs::path& outPath) -> bool
    {
        if (bEmitValidation) return true; // In validation generation mode, always write stubs
        if (!fs::exists(outPath)) return true;
        
        auto outTime = fs::last_write_time(outPath);

        // Check if outPath is older than any file in the include hierarchy
        for (const auto& includedFile : allIncludedFiles)
        {
            if (fs::exists(includedFile))
            {
                if (outTime < fs::last_write_time(includedFile))
                    return true;
            }
        }
        
        // Check if the current executable is newer than outPath
        if (outTime < fs::last_write_time(exePath))
            return true;

        return false;
    };

    // --- Generate HLSL ---
    try
    {
        int padCount = globalPadCount;
        std::string hlslContent = GenerateHlsl(pr, layouts, padCount);

        // Wrap with old-school include guard
        std::string guard = MakeHlslGuard(stem);
        hlslContent = "#ifndef " + guard + "\n"
                    + "#define " + guard + "\n"
                    + "\n"
                    + hlslContent
                    + "#endif // " + guard + "\n";

        fs::path hlslOut = outputRoot / "hlsl" / stem;
        hlslOut += ".hlsli";
        if (NeedsWrite(hlslOut))
        {
            WriteFile(hlslOut, hlslContent);
            LogMsg("  HLSL  -> %s\n", hlslOut.string().c_str());
        }
        else
        {
            LogMsg("  HLSL  up-to-date, skipped: %s\n", hlslOut.string().c_str());
        }
        globalPadCount = padCount;
    }
    catch (const std::exception& e)
    {
        LogMsg("  ERROR (hlsl gen): %s\n", e.what());
        throw std::runtime_error(
            std::string("HLSL generation failed for '") + srFile.string() + "': " + e.what());
    }

    // --- Generate C++ ---
    try
    {
        int padCount = globalPadCount;
        std::string cppContent = GenerateCpp(pr, layouts, padCount, bEmitValidation);

        fs::path cppOut = outputRoot / "cpp" / stem;
        cppOut += ".h";
        if (NeedsWrite(cppOut))
        {
            WriteFile(cppOut, cppContent);
            LogMsg("  C++   -> %s\n", cppOut.string().c_str());
        }
        else
        {
            LogMsg("  C++   up-to-date, skipped: %s\n", cppOut.string().c_str());
        }
        globalPadCount = padCount;
    }
    catch (const std::exception& e)
    {
        LogMsg("  ERROR (cpp gen): %s\n", e.what());
        throw std::runtime_error(
            std::string("C++ generation failed for '") + srFile.string() + "': " + e.what());
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    LogMsg("[srrhi] Started.\n");

    // --- Parse arguments ---
    std::string inputDir;
    std::string outputDir;
    bool runTests        = false;
    bool genValidation   = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc)
        {
            inputDir = argv[++i];
            LogMsg("[srrhi] Arg -i: %s\n", inputDir.c_str());
        }
        else if (arg == "-o" && i + 1 < argc)
        {
            outputDir = argv[++i];
            LogMsg("[srrhi] Arg -o: %s\n", outputDir.c_str());
        }
        else if (arg == "-v" || arg == "--verbose")
        {
            g_Verbose = true;
            LogMsg("[srrhi] Arg %s: verbose mode enabled\n", arg.c_str());
        }
        else if (arg == "--test")
        {
            runTests = true;
            g_Verbose = true;
            LogMsg("[srrhi] Arg --test: reflection test mode enabled\n");
        }
        else if (arg == "--gen-validation")
        {
            genValidation = true;
            LogMsg("[srrhi] Arg --gen-validation: validation stub generation enabled\n");
        }
        else if (arg == "--help" || arg == "-h")
        {
            LogMsg("Usage: srrhi -i <input-dir> -o <output-dir> [-v] [--test [--gen-validation]]\n"
                   "  -i <dir>           Input folder (recursively scanned for .sr files)\n"
                   "  -o <dir>           Output folder (hlsl/ and cpp/ subfolders created)\n"
                   "  -v                 Verbose: print visualizer output to stdout\n"
                   "  --test             After generation, verify cbuffer layouts via DXC reflection (implies -v)\n"
                   "  --gen-validation   (requires --test) Generate validation .cpp stubs for each generated .h\n");
            return 0;
        }
        else
        {
            LogMsg("[srrhi] WARNING: Unknown argument ignored: %s\n", arg.c_str());
        }
    }

    if (genValidation && !runTests)
    {
        LogMsg("[srrhi] WARNING: --gen-validation has no effect without --test.\n");
    }

    if (inputDir.empty())
    {
        LogMsg("[srrhi] ERROR: -i <input-dir> is required.\n"
               "Usage: srrhi -i <input-dir> -o <output-dir> [--test]\n");
        return 1;
    }
    if (outputDir.empty())
    {
        LogMsg("[srrhi] ERROR: -o <output-dir> is required.\n"
               "Usage: srrhi -i <input-dir> -o <output-dir> [--test]\n");
        return 1;
    }

    // Resolve paths
    fs::path inputRoot;
    fs::path outputRoot;
    try
    {
        inputRoot  = fs::absolute(inputDir);
        outputRoot = fs::absolute(outputDir);
    }
    catch (const std::exception& e)
    {
        LogMsg("[srrhi] ERROR: Invalid path argument: %s\n", e.what());
        return 1;
    }

    LogMsg("[srrhi] Input  dir: %s\n", inputRoot.string().c_str());
    LogMsg("[srrhi] Output dir: %s\n", outputRoot.string().c_str());

    if (!fs::exists(inputRoot) || !fs::is_directory(inputRoot))
    {
        LogMsg("[srrhi] ERROR: Input directory does not exist: %s\n",
               inputRoot.string().c_str());
        return 1;
    }

    // --- Collect .sr files ---
    std::vector<fs::path> srFiles;
    try
    {
        for (const auto& entry : fs::recursive_directory_iterator(inputRoot))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".sr")
            {
                srFiles.push_back(entry.path());
                LogMsg("[srrhi] Found: %s\n", entry.path().string().c_str());
            }
        }
    }
    catch (const std::exception& e)
    {
        LogMsg("[srrhi] ERROR: Failed to scan input directory '%s': %s\n",
               inputRoot.string().c_str(), e.what());
        return 1;
    }

    if (srFiles.empty())
    {
        LogMsg("[srrhi] No .sr files found in: %s\n", inputRoot.string().c_str());
        return 0;
    }

    LogMsg("[srrhi] Total: %zu .sr file(s) to process.\n\n", srFiles.size());

    // --- Process each file ---
    int exitCode = 0;
    int globalPadCount = 0;
    fs::path currentExe = fs::absolute(argv[0]);

    for (const auto& f : srFiles)
    {
        try
        {
            ProcessFile(f, inputRoot, outputRoot, globalPadCount, runTests, currentExe);
        }
        catch (const std::exception& e)
        {
            LogMsg("[srrhi] FATAL ERROR processing '%s': %s\n",
                   f.string().c_str(), e.what());
            exitCode = 1;
            // Continue processing remaining files so all errors are logged
        }
        catch (...)
        {
            LogMsg("[srrhi] FATAL unknown error processing '%s'\n", f.string().c_str());
            exitCode = 1;
        }
    }

    if (exitCode == 0)
        LogMsg("\n[srrhi] Done. All files processed successfully.\n");
    else
        LogMsg("\n[srrhi] Done with errors (exit code %d).\n", exitCode);

    if (runTests)
    {
        // --gen-validation: generate validation .cpp stubs for each produced .h
        if (genValidation)
        {
            LogMsg("\n[srrhi] Generating validation stubs...\n");
            fs::path headerDir = outputRoot / "cpp";
            int genResult = GenerateValidationStubs(headerDir);
            if (genResult != 0)
            {
                LogMsg("[srrhi] Validation stub generation FAILED.\n");
                return genResult;
            }
            LogMsg("[srrhi] Validation stub generation PASSED.\n");
        }

        LogMsg("\n[srrhi] Running reflection tests against DXC...\n");
        int testResult = 1;
        try
        {
            testResult = RunReflectionTests(inputRoot);
        }
        catch (const std::exception& e)
        {
            LogMsg("[srrhi] ERROR running reflection tests: %s\n", e.what());
            testResult = 1;
        }
        if (testResult != 0)
            LogMsg("[srrhi] Reflection tests FAILED.\n");
        else
            LogMsg("[srrhi] Reflection tests PASSED.\n");

        // In --test mode the final exit code reflects the reflection test result;
        // generation errors on intentionally-broken test files are expected and do
        // not constitute a "test failure" in their own right.
        return testResult;
    }

    return exitCode;
}

