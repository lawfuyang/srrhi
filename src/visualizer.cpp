#include "types.h"
#include <sstream>
#include <iomanip>

std::string visualizeLayouts(const std::vector<FlatLayout>& layouts)
{
    logMsg("[visualizer] Generating visualization for %zu layouts...\n", layouts.size());
    
    std::ostringstream out;

    for (const auto& fl : layouts)
    {
        logMsg("[visualizer]   Visualizing cbuffer: %s (%d bytes)\n", 
               fl.cbufferName.c_str(), fl.totalSize);
        
        // Header
        out << std::string(21, ' ')
            << "offset |  size + pad\n";
        out << "cbuffer " << fl.cbufferName << " {\n";

        for (const auto& item : fl.items)
        {
            std::string lineInd(4 + item.depth * 4, ' ');

            if (item.isStructOpen)
            {
                out << lineInd << item.displayType << " {\n";
                continue;
            }

            if (item.isStructClose)
            {
                // indent is one less (we decremented depth)
                out << lineInd << "} " << item.displayName;
                // format offset and size + pad
                out << std::setw(std::max(1, 20 - (int)lineInd.size() - 2 - (int)item.displayName.size())) << " ";
                out << std::setw(5) << item.offsetBytes;
                out << std::setw(5) << item.sizeBytes;
                if (item.paddingBytes > 0)
                    out << " +" << std::setw(2) << item.paddingBytes;
                out << "\n";
                continue;
            }

            if (item.isLeaf)
            {
                // Build "    type name;"
                std::string decl = lineInd + item.displayType + " " + item.displayName;
                // pad to column 24
                int padTo = 29;
                if ((int)decl.size() < padTo)
                    decl += std::string(padTo - decl.size(), ' ');
                out << decl;
                out << std::setw(5) << item.offsetBytes;
                out << std::setw(5) << item.sizeBytes;
                if (item.paddingBytes > 0)
                    out << " +" << std::setw(2) << item.paddingBytes;
                out << "\n";
            }
        }

        out << "};\n\n";
    }

    logMsg("[visualizer] Visualization generation complete\n");
    return out.str();
}
