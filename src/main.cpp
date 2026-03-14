#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "types.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
ParseResult parseFile(const std::string& path);
std::vector<FlatLayout> computeLayouts(ParseResult& pr);
std::string generateHlsl(const ParseResult& pr, const std::vector<CBuffer>& layoutCbs, int& padCount);
std::string generateCpp(const ParseResult& pr, const std::vector<CBuffer>& layoutCbs, int& padCount);
std::string visualizeLayouts(const std::vector<FlatLayout>& layouts);

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
static std::unique_ptr<FILE, FileDeleter> g_log; // output.txt
static std::unique_ptr<FILE, FileDeleter> g_vis; // visualizer_results.txt

void logMsg(const char* fmt, ...)
{
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log.get(), fmt, args);
    va_end(args);
    fflush(g_log.get());
}

// ---------------------------------------------------------------------------
// Utility: write a string to a file, creating parent dirs as needed
// ---------------------------------------------------------------------------
static void writeFile(const fs::path& path, const std::string& content)
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

    bool ok = (fwrite(content.c_str(), 1, content.size(), f) == content.size());
    int closeErr = fclose(f);
    if (!ok)
        throw std::runtime_error("Write error for file: " + path.string());
    if (closeErr != 0)
        throw std::runtime_error("Close error for file: " + path.string());
}

// ---------------------------------------------------------------------------
// Derive HLSL include-guard name from the output stem.
// e.g. stem "sub/example" -> "EXAMPLE_HLSLI"
// ---------------------------------------------------------------------------
static std::string makeHlslGuard(const fs::path& stem)
{
    std::string name = stem.filename().string() + "_HLSLI";
    for (char& c : name)
        c = std::isalnum((unsigned char)c) ? (char)std::toupper((unsigned char)c) : '_';
    return name;
}

// ---------------------------------------------------------------------------
// Process one .sr file — throws on any unrecoverable error
// ---------------------------------------------------------------------------
static void processFile(const fs::path& srFile,
                        const fs::path& inputRoot,
                        const fs::path& outputRoot,
                        int& globalPadCount)
{
    logMsg("[srrhi] Processing: %s\n", srFile.string().c_str());

    // --- Parse ---
    ParseResult pr;
    try
    {
        pr = parseFile(srFile.string());
        logMsg("  Parse OK: %zu cbuffer(s), %zu struct(s)\n",
               pr.cbuffers.size(), pr.structs.size());
    }
    catch (const std::exception& e)
    {
        logMsg("  ERROR (parse): %s\n", e.what());
        throw std::runtime_error(
            std::string("Parse failed for '") + srFile.string() + "': " + e.what());
    }

    if (pr.cbuffers.empty())
    {
        logMsg("  No cbuffers found in '%s', skipping.\n", srFile.string().c_str());
        return;
    }

    // --- Compute layouts ---
    std::vector<FlatLayout> layouts;
    try
    {
        layouts = computeLayouts(pr);
        logMsg("  Layout OK: %zu layout(s) computed\n", layouts.size());
    }
    catch (const std::exception& e)
    {
        logMsg("  ERROR (layout): %s\n", e.what());
        throw std::runtime_error(
            std::string("Layout computation failed for '") + srFile.string() + "': " + e.what());
    }

    // --- Visualizer ---
    try
    {
        std::string vis = visualizeLayouts(layouts);
        if (g_vis)
        {
            fprintf(g_vis.get(), "=== %s ===\n", srFile.string().c_str());
            fwrite(vis.c_str(), 1, vis.size(), g_vis.get());
            fflush(g_vis.get());
        }
        logMsg("  Visualizer: written %zu bytes for %s\n",
               vis.size(), srFile.filename().string().c_str());
    }
    catch (const std::exception& e)
    {
        logMsg("  ERROR (visualizer): %s\n", e.what());
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
        std::string hlslContent = generateHlsl(pr, pr.cbuffers, padCount);

        // Wrap with old-school include guard
        std::string guard = makeHlslGuard(stem);
        hlslContent = "#ifndef " + guard + "\n"
                    + "#define " + guard + "\n"
                    + "\n"
                    + hlslContent
                    + "#endif // " + guard + "\n";

        fs::path hlslOut = outputRoot / "hlsl" / stem;
        hlslOut += ".hlsli";
        writeFile(hlslOut, hlslContent);
        logMsg("  HLSL  -> %s\n", hlslOut.string().c_str());
        globalPadCount = padCount;
    }
    catch (const std::exception& e)
    {
        logMsg("  ERROR (hlsl gen): %s\n", e.what());
        throw std::runtime_error(
            std::string("HLSL generation failed for '") + srFile.string() + "': " + e.what());
    }

    // --- Generate C++ ---
    try
    {
        int padCount = globalPadCount;
        std::string cppContent = generateCpp(pr, pr.cbuffers, padCount);

        fs::path cppOut = outputRoot / "cpp" / stem;
        cppOut += ".h";
        writeFile(cppOut, cppContent);
        logMsg("  C++   -> %s\n", cppOut.string().c_str());
        globalPadCount = padCount;
    }
    catch (const std::exception& e)
    {
        logMsg("  ERROR (cpp gen): %s\n", e.what());
        throw std::runtime_error(
            std::string("C++ generation failed for '") + srFile.string() + "': " + e.what());
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    // Derive bin directory from argv[0] to place output.txt beside the exe
    fs::path binDir;
    try
    {
        binDir = fs::absolute(fs::path(argv[0])).parent_path();
    }
    catch (...)
    {
        binDir = fs::current_path();
    }

    // Open log file (output.txt in bin/)
    {
        fs::path logPath = binDir / "output.txt";
        g_log.reset(fopen(logPath.string().c_str(), "w"));
        if (!g_log)
        {
            // Last resort: print to stderr since logging is unavailable
            fprintf(stderr, "[srrhi] FATAL: Cannot open log file: %s\n",
                    logPath.string().c_str());
            return 1;
        }
        logMsg("[srrhi] Log opened: %s\n", logPath.string().c_str());
    }

    // Open visualizer results file (visualizer_results.txt in bin/)
    {
        fs::path visPath = binDir / "visualizer_results.txt";
        g_vis.reset(fopen(visPath.string().c_str(), "w"));
        if (!g_vis)
        {
            logMsg("[srrhi] WARNING: Cannot open visualizer results file: %s — visualizer output will be skipped\n",
                   (binDir / "visualizer_results.txt").string().c_str());
        }
        else
        {
            logMsg("[srrhi] Visualizer results file: %s\n", visPath.string().c_str());
        }
    }

    logMsg("[srrhi] Started.\n");

    // --- Parse arguments ---
    std::string inputDir;
    std::string outputDir;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-i" && i + 1 < argc)
        {
            inputDir = argv[++i];
            logMsg("[srrhi] Arg -i: %s\n", inputDir.c_str());
        }
        else if (arg == "-o" && i + 1 < argc)
        {
            outputDir = argv[++i];
            logMsg("[srrhi] Arg -o: %s\n", outputDir.c_str());
        }
        else if (arg == "--help" || arg == "-h")
        {
            logMsg("Usage: srrhi -i <input-dir> -o <output-dir>\n"
                   "  -i <dir>   Input folder (recursively scanned for .sr files)\n"
                   "  -o <dir>   Output folder (hlsl/ and cpp/ subfolders created)\n");
            return 0;
        }
        else
        {
            logMsg("[srrhi] WARNING: Unknown argument ignored: %s\n", arg.c_str());
        }
    }

    if (inputDir.empty())
    {
        logMsg("[srrhi] ERROR: -i <input-dir> is required.\n"
               "Usage: srrhi -i <input-dir> -o <output-dir>\n");
        return 1;
    }
    if (outputDir.empty())
    {
        logMsg("[srrhi] ERROR: -o <output-dir> is required.\n"
               "Usage: srrhi -i <input-dir> -o <output-dir>\n");
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
        logMsg("[srrhi] ERROR: Invalid path argument: %s\n", e.what());
        return 1;
    }

    logMsg("[srrhi] Input  dir: %s\n", inputRoot.string().c_str());
    logMsg("[srrhi] Output dir: %s\n", outputRoot.string().c_str());

    if (!fs::exists(inputRoot) || !fs::is_directory(inputRoot))
    {
        logMsg("[srrhi] ERROR: Input directory does not exist: %s\n",
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
                logMsg("[srrhi] Found: %s\n", entry.path().string().c_str());
            }
        }
    }
    catch (const std::exception& e)
    {
        logMsg("[srrhi] ERROR: Failed to scan input directory '%s': %s\n",
               inputRoot.string().c_str(), e.what());
        return 1;
    }

    if (srFiles.empty())
    {
        logMsg("[srrhi] No .sr files found in: %s\n", inputRoot.string().c_str());
        return 0;
    }

    logMsg("[srrhi] Total: %zu .sr file(s) to process.\n\n", srFiles.size());

    // --- Process each file ---
    int exitCode = 0;
    int globalPadCount = 0;

    for (const auto& f : srFiles)
    {
        try
        {
            processFile(f, inputRoot, outputRoot, globalPadCount);
        }
        catch (const std::exception& e)
        {
            logMsg("[srrhi] FATAL ERROR processing '%s': %s\n",
                   f.string().c_str(), e.what());
            exitCode = 1;
            // Continue processing remaining files so all errors are logged
        }
        catch (...)
        {
            logMsg("[srrhi] FATAL unknown error processing '%s'\n", f.string().c_str());
            exitCode = 1;
        }
    }

    if (exitCode == 0)
        logMsg("\n[srrhi] Done. All files processed successfully.\n");
    else
        logMsg("\n[srrhi] Done with errors (exit code %d).\n", exitCode);

    return exitCode;
}

