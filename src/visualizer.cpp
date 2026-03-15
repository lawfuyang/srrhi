#include "types.h"
#include <sstream>
#include <iomanip>

// ---------------------------------------------------------------------------
// Visualizer: walks the LayoutMember tree and produces a formatted table.
//
// Output format:
//   "                     offset |  size + pad"
//   "cbuffer Name {"
//   "    type name;              <off>  <size>[+ <pad>]"
//   "    struct T {"
//   "        member;"
//   "    } field;"
//   "};"
// ---------------------------------------------------------------------------

struct VisualizerState
{
    std::ostringstream out;
    int depth = 0;

    std::string indent() const { return std::string(4 + depth * 4, ' '); }

    // Emit a "struct T {" open line
    void emitStructOpen(const std::string& typeName)
    {
        out << indent() << typeName << " {\n";
        ++depth;
    }

    // Emit "} name;" close line with offset/size/padding
    void emitStructClose(const std::string& label, int offset, int size, int padding)
    {
        --depth;
        std::string ind = indent();
        std::string closeText = ind + "} " + label;

        // Pad to column 24
        int padTo = 24;
        if ((int)closeText.size() < padTo)
            closeText += std::string(padTo - closeText.size(), ' ');

        out << closeText;
        out << std::setw(5) << offset;
        out << std::setw(5) << size;
        if (padding > 0)
            out << " +" << std::setw(2) << padding;
        out << "\n";
    }

    // Emit a leaf data line: "    type name;  <off>  <size>[+ <pad>]"
    void emitLeaf(const std::string& typeName, const std::string& label,
                  int offset, int size, int padding)
    {
        std::string decl = indent() + typeName + " " + label;
        int padTo = 29;
        if ((int)decl.size() < padTo)
            decl += std::string(padTo - decl.size(), ' ');

        out << decl;
        out << std::setw(5) << offset;
        out << std::setw(5) << size;
        if (padding > 0)
            out << " +" << std::setw(2) << padding;
        out << "\n";
    }
};

// Forward declaration
static void visitMember(VisualizerState& vs, const LayoutMember& m,
                        bool bParentIsMatrix);

// Visit all submembers of a node (struct or array contents)
static void visitSubmembers(VisualizerState& vs, const LayoutMember& parent)
{
    bool bIsMatrix = false;
    if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&parent.m_Type))
        bIsMatrix = (*ap)->m_bCreatedFromMatrix;

    for (auto& sub : parent.m_Submembers)
        visitMember(vs, sub, bIsMatrix);
}

static void visitMember(VisualizerState& vs, const LayoutMember& m,
                        bool bParentIsMatrix)
{
    // --- BuiltinType leaf ---
    if (std::holds_alternative<BuiltinType>(m.m_Type))
    {
        const auto& bt = std::get<BuiltinType>(m.m_Type);
        // Matrix columns have no trailing ";"
        std::string label = m.m_Name + (bParentIsMatrix ? "" : ";");
        vs.emitLeaf(bt.m_Name, label, m.m_Offset, m.m_Size, m.m_Padding);
        return;
    }

    // --- ArrayNode (array or matrix) ---
    if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&m.m_Type))
    {
        // Recurse: each element is a submember rendered by visitMember
        visitSubmembers(vs, m);
        return;
    }

    // --- StructType* (struct) ---
    if (auto* sp = std::get_if<StructType*>(&m.m_Type))
    {
        const StructType* st = *sp;
        std::string typeName = "struct " + st->m_Name;
        vs.emitStructOpen(typeName);
        for (auto& sub : m.m_Submembers)
            visitMember(vs, sub, false);
        std::string label = m.m_Name + ";";
        vs.emitStructClose(label, m.m_Offset, m.m_Size, m.m_Padding);
        return;
    }
}

// ---------------------------------------------------------------------------
// Public API — human-readable ASCII table
// ---------------------------------------------------------------------------
std::string VisualizeLayouts(const std::vector<LayoutMember>& layouts)
{
    LogMsg("[visualizer] Generating visualization for %zu layout(s)...\n",
           layouts.size());

    VisualizerState vs;

    for (const auto& cbLayout : layouts)
    {
        LogMsg("[visualizer]   cbuffer: %s (%d bytes)\n",
               cbLayout.m_Name.c_str(), cbLayout.m_Size);

        vs.out << std::string(21, ' ') << "offset |  size + pad\n";
        vs.out << "cbuffer " << cbLayout.m_Name << " {\n";

        vs.depth = 0;
        visitSubmembers(vs, cbLayout);

        vs.out << "};\n\n";
    }

    LogMsg("[visualizer] Done\n");
    return vs.out.str();
}

// ---------------------------------------------------------------------------
// Machine-readable layout output
//
// Format (one cbuffer section per cbuffer, easy to parse line-by-line):
//
//   CBUFFER <name> SIZE <total_size_rounded_to_16>
//   MEMBER <name> OFFSET <offset>
//   MEMBER <name> OFFSET <offset>
//   CBUFFER_END
//
// Only top-level members of each cbuffer are emitted (no padding members,
// no sub-elements of arrays or matrices).  DXC reflection also exposes
// variables at this same granularity, making direct comparison trivial.
// ---------------------------------------------------------------------------
std::string VisualizeLayoutsMachineReadable(const std::vector<LayoutMember>& layouts)
{
    std::ostringstream out;

    for (const auto& cbLayout : layouts)
    {
        // Total size rounded to 16 — matches D3D12_SHADER_BUFFER_DESC::Size
        int totalSize = (cbLayout.m_Size + 15) & ~15;

        out << "CBUFFER " << cbLayout.m_Name << " SIZE " << totalSize << "\n";

        for (const auto& m : cbLayout.m_Submembers)
            out << "MEMBER " << m.m_Name << " OFFSET " << m.m_Offset << "\n";

        out << "CBUFFER_END\n\n";
    }

    return out.str();
}
