// What a module-graph load depended on besides its files' texts, and the
// re-check that it still holds (modules.h, DependencyLog).

#include <algorithm>

#include "modules/modules.h"
#include "modules/resolve.h"

namespace bronze::modules {

std::vector<std::string> listGlobMatches(const std::filesystem::path& dir, const std::string& namePrefix,
                                         const std::string& tail) {
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        std::error_code kindEc;
        if (!entry.is_regular_file(kindEc) || kindEc) continue;
        const std::string name = entry.path().filename().generic_string();
        if (name.size() <= namePrefix.size() + tail.size()) continue;
        if (name.compare(0, namePrefix.size(), namePrefix) != 0) continue;
        if (name.compare(name.size() - tail.size(), tail.size(), tail) != 0) continue;
        names.push_back(name);
    }
    // Directory order is not defined across filesystems, and the specifier
    // list reaches the output (it is the load order of these modules), so it
    // is sorted rather than taken as read.
    std::sort(names.begin(), names.end());
    return names;
}

bool dependenciesUnchanged(const DependencyLog& log, const std::vector<ModuleRoot>& moduleRoots) {
    for (const auto& glob : log.globs) {
        std::filesystem::path dir;
        const std::filesystem::path importer(glob.importer);
        const bool haveDir = resolveSpecifierDirectory(glob.dirSpecifier, importer, moduleRoots, dir);
        const std::vector<std::string> names =
            haveDir ? listGlobMatches(dir, glob.namePrefix, glob.tail) : std::vector<std::string>{};
        if (names != glob.names) return false;
    }
    for (const auto& edge : log.edges) {
        // A resolution that now fails is a change as much as one that lands on
        // another file; its diagnostic is the fresh load's to report.
        DiagnosticSink quiet;
        std::filesystem::path target;
        if (!resolveSpecifier(edge.specifier, std::filesystem::path(edge.importer), Span{}, quiet, target,
                              moduleRoots)) {
            return false;
        }
        if (target.generic_string() != edge.target) return false;
    }
    return true;
}

}  // namespace bronze::modules
