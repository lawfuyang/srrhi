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
                        bool parentIsMatrix);

// Visit all submembers of a node (struct or array contents)
static void visitSubmembers(VisualizerState& vs, const LayoutMember& parent)
{
    bool isMatrix = false;
    if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&parent.type))
        isMatrix = (*ap)->created_from_matrix;

    for (auto& sub : parent.submembers)
        visitMember(vs, sub, isMatrix);
}

static void visitMember(VisualizerState& vs, const LayoutMember& m,
                        bool parentIsMatrix)
{
    // --- BuiltinType leaf ---
    if (std::holds_alternative<BuiltinType>(m.type))
    {
        const auto& bt = std::get<BuiltinType>(m.type);
        // Matrix columns have no trailing ";"
        std::string label = m.name + (parentIsMatrix ? "" : ";");
        vs.emitLeaf(bt.name, label, m.offset, m.size, m.padding);
        return;
    }

    // --- ArrayNode (array or matrix) ---
    if (auto* ap = std::get_if<std::shared_ptr<ArrayNode>>(&m.type))
    {
        // Recurse: each element is a submember rendered by visitMember
        visitSubmembers(vs, m);
        return;
    }

    // --- StructType* (struct) ---
    if (auto* sp = std::get_if<StructType*>(&m.type))
    {
        const StructType* st = *sp;
        std::string typeName = "struct " + st->name;
        vs.emitStructOpen(typeName);
        for (auto& sub : m.submembers)
            visitMember(vs, sub, false);
        std::string label = m.name + ";";
        vs.emitStructClose(label, m.offset, m.size, m.padding);
        return;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
std::string visualizeLayouts(const std::vector<LayoutMember>& layouts)
{
    logMsg("[visualizer] Generating visualization for %zu layout(s)...\n",
           layouts.size());

    VisualizerState vs;

    for (const auto& cbLayout : layouts)
    {
        logMsg("[visualizer]   cbuffer: %s (%d bytes)\n",
               cbLayout.name.c_str(), cbLayout.size);

        vs.out << std::string(21, ' ') << "offset |  size + pad\n";
        vs.out << "cbuffer " << cbLayout.name << " {\n";

        vs.depth = 0;
        visitSubmembers(vs, cbLayout);

        vs.out << "};\n\n";
    }

    logMsg("[visualizer] Done\n");
    return vs.out.str();
}