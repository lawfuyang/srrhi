#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

#include "types.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
ParseResult ParseFile(const std::string& path);
std::vector<LayoutMember> ComputeLayouts(ParseResult& pr);
std::string GenerateHlsl(const ParseResult& pr, const std::vector<LayoutMember>& layouts, int& padCount);
std::string GenerateCpp(const ParseResult& pr, const std::vector<LayoutMember>& layouts, int& padCount);
std::string VisualizeLayouts(const std::vector<LayoutMember>& layouts);
std::string VisualizeLayoutsMachineReadable(const std::vector<LayoutMember>& layouts);
int RunReflectionTests(const fs::path& testInputDir);

// ---------------------------------------------------------------------------
// Custom deleter for FILE*
// ---------------------------------------------------------------------------
struct FileDeleter
{
    void operator()(FILE* f) const { if (f) fclose(f); }
};

// ---------------------------------------------------------------------------
// Global output handles
// ---------------------------------------------------------------------------
static std::unique_ptr<FILE, FileDeleter> g_Vis; // visualizer_results.txt

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
// Derive HLSL include-guard name from the output stem.
// e.g. stem "sub/example" -> "EXAMPLE_HLSLI"
// ---------------------------------------------------------------------------
static std::string MakeHlslGuard(const fs::path& stem)
{
    std::string name = stem.filename().string() + "_HLSLI";
    for (char& c : name)
        c = std::isalnum((unsigned char)c) ? (char)std::toupper((unsigned char)c) : '_';
    return name;
}

// ---------------------------------------------------------------------------
// Process one .sr file — throws on any unrecoverable error
// ---------------------------------------------------------------------------
static void ProcessFile(const fs::path& srFile,
                        const fs::path& inputRoot,
                        const fs::path& outputRoot,
                        int& globalPadCount)
{
    LogMsg("[srrhi] Processing: %s\n", srFile.string().c_str());

    // --- Parse ---
    ParseResult pr;
    try
    {
        pr = ParseFile(srFile.string());
        long cbufferCount = 0;
        for (auto& b : pr.m_Buffers) if (b.m_bIsCBuffer) ++cbufferCount;
        LogMsg("  Parse OK: %ld cbuffer(s), %zu struct(s)\n",
               cbufferCount, pr.m_Structs.size());
    }
    catch (const std::exception& e)
    {
        LogMsg("  ERROR (parse): %s\n", e.what());
        throw std::runtime_error(
            std::string("Parse failed for '") + srFile.string() + "': " + e.what());
    }

    bool bHasCBuffers = false;
    for (auto& b : pr.m_Buffers) if (b.m_bIsCBuffer) { bHasCBuffers = true; break; }
    if (!bHasCBuffers)
    {
        LogMsg("  No cbuffers found in '%s', skipping.\n", srFile.string().c_str());
        return;
    }

    // --- Compute layouts ---
    std::vector<LayoutMember> layouts;
    try
    {
        layouts = ComputeLayouts(pr);
        LogMsg("  Layout OK: %zu layout(s) computed\n", layouts.size());
    }
    catch (const std::exception& e)
    {
        LogMsg("  ERROR (layout): %s\n", e.what());
        throw std::runtime_error(
            std::string("Layout computation failed for '") + srFile.string() + "': " + e.what());
    }

    // --- Visualizer ---
    try
    {
        std::string vis      = VisualizeLayouts(layouts);
        std::string visMR    = VisualizeLayoutsMachineReadable(layouts);
        if (g_Vis)
        {
            fprintf(g_Vis.get(), "=== %s ===\n", srFile.string().c_str());
            fwrite(vis.c_str(), 1, vis.size(), g_Vis.get());
            // Machine-readable section, clearly delimited
            fprintf(g_Vis.get(), "--- machine-readable ---\n");
            fwrite(visMR.c_str(), 1, visMR.size(), g_Vis.get());
            fprintf(g_Vis.get(), "\n");
            fflush(g_Vis.get());
        }
        LogMsg("  Visualizer: written %zu bytes for %s\n",
               vis.size(), srFile.filename().string().c_str());
    }
    catch (const std::exception& e)
    {
        LogMsg("  ERROR (visualizer): %s\n", e.what());
        throw std::runtime_error(
            std::string("Visualizer failed for '") + srFile.string() + "': " + e.what());
    }

    // Relative stem: e.g. "sub/example" for use in output paths and guard names
    fs::path relPath = fs::relative(srFile, inputRoot);
    fs::path stem    = relPath.parent_path() / srFile.stem();

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
        WriteFile(hlslOut, hlslContent);
        LogMsg("  HLSL  -> %s\n", hlslOut.string().c_str());
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
        std::string cppContent = GenerateCpp(pr, layouts, padCount);

        fs::path cppOut = outputRoot / "cpp" / stem;
        cppOut += ".h";
        WriteFile(cppOut, cppContent);
        LogMsg("  C++   -> %s\n", cppOut.string().c_str());
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
    // Derive bin directory from argv[0] to place visualizer_results.txt beside the exe
    fs::path binDir;
    try
    {
        binDir = fs::absolute(fs::path(argv[0])).parent_path();
    }
    catch (...)
    {
        binDir = fs::current_path();
    }

    // Open visualizer results file (visualizer_results.txt in bin/)
    {
        fs::path visPath = binDir / "visualizer_results.txt";
        g_Vis.reset(fopen(visPath.string().c_str(), "w"));
        if (!g_Vis)
        {
            LogMsg("[srrhi] WARNING: Cannot open visualizer results file: %s — visualizer output will be skipped\n",
                   (binDir / "visualizer_results.txt").string().c_str());
        }
        else
        {
            LogMsg("[srrhi] Visualizer results file: %s\n", visPath.string().c_str());
        }
    }

    LogMsg("[srrhi] Started.\n");

    // --- Parse arguments ---
    std::string inputDir;
    std::string outputDir;
    bool runTests = false;

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
        else if (arg == "--test")
        {
            runTests = true;
            LogMsg("[srrhi] Arg --test: reflection test mode enabled\n");
        }
        else if (arg == "--help" || arg == "-h")
        {
            LogMsg("Usage: srrhi -i <input-dir> -o <output-dir> [--test]\n"
                   "  -i <dir>   Input folder (recursively scanned for .sr files)\n"
                   "  -o <dir>   Output folder (hlsl/ and cpp/ subfolders created)\n"
                   "  --test     After generation, verify cbuffer layouts via DXC reflection\n");
            return 0;
        }
        else
        {
            LogMsg("[srrhi] WARNING: Unknown argument ignored: %s\n", arg.c_str());
        }
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

    for (const auto& f : srFiles)
    {
        try
        {
            ProcessFile(f, inputRoot, outputRoot, globalPadCount);
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

