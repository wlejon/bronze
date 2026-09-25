#pragma once

// Internal linker structures and Linker class declaration shared between
// link.cpp and link_exports.cpp.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "modules/graph.h"
#include "support/diagnostics.h"
#include "support/source.h"

namespace bronze::modules {

// A name in the one flat namespace. The `.` is what makes a collision
// impossible: it cannot occur in a JavaScript identifier, so a renamed name
// can never be a name the source wrote. The ENTRY file is not renamed, so a
// single-file build produces exactly the IL it produced before modules
// existed — which is what 121 pinned oracle cases and every pinned IL dump
// depend on.
inline std::string canonicalName(uint16_t moduleId, const std::string& local) {
    if (moduleId == 0) return local;
    return "mod" + std::to_string(moduleId) + "." + local;
}

struct ExportEntry {
    // An export of a name this module declares.
    std::string localName;
    // An export that names another module's export (`export { x } from`, and
    // every name a bare `export * from` contributes).
    bool indirect = false;
    uint16_t targetModule = 0;
    std::string targetName;
    bool fromStar = false;
    Span span;
};

struct ImportBinding {
    uint16_t module = 0;
    std::string exportName;
    bool isNamespace = false;
    Span span;
};

struct ModuleInfo {
    // Export names in SOURCE order. The namespace object's property order is
    // this order, and a `.expected` file is a byte comparison, so it has to be
    // a function of the source and not of a hash table.
    std::vector<std::string> exportOrder;
    std::map<std::string, ExportEntry> exports;
    std::map<std::string, ImportBinding> imports;  // local binding -> what it names
    std::set<std::string> localNames;
    // Local bindings holding a module namespace object this file must
    // synthesize: (local name, the module it mirrors). A `import * as ns`
    // local is written by the source; an `export * as ns from` local is not,
    // and gets a name no source can spell.
    std::vector<std::pair<std::string, uint16_t>> namespaceLocals;
    // One entry per template-literal `import()` the file writes: the name of
    // the function that turns the runtime string into a promise, and the
    // (specifier, namespace local) pairs it can answer with. The specifiers are
    // what the loader's glob found, so the table holds exactly the files that
    // were on disk at compile time — and a string outside it rejects, which is
    // what a browser does with a 404.
    struct DynPicker {
        std::string local;
        std::string head;
        std::string tail;
        std::vector<std::pair<std::string, std::string>> entries;  // specifier -> ns local
    };
    std::vector<DynPicker> dynPickers;
    std::map<std::string, std::string> renames;
};

class Linker {
public:
    Linker(Graph& graph, SourceSet& sources, DiagnosticSink& diags)
        : graph_(graph), sources_(sources), diags_(diags), info_(graph.modules.size()) {}

    bool run(ast::Module& out);

    // Export/import collection, star export resolution, and renames (link_exports.cpp)
    bool collectLocals(ModuleFile& file);
    bool collectImports(ModuleFile& file);
    bool collectExports(ModuleFile& file);
    bool expandStarExports(ModuleFile& file, bool& changed);
    bool buildRenames(ModuleFile& file);
    bool resolveExport(uint16_t moduleId, const std::string& name, Span span,
                       DiagnosticSink* report, uint16_t& outModule, std::string& outLocal);
    bool addExport(ModuleFile& file, const std::string& exported, ExportEntry entry);

    // Synthesis and merging (link.cpp)
    ast::StmtPtr synthesizeNamespace(uint16_t owner, const std::string& local, uint16_t target);
    bool synthesizePicker(uint16_t owner, const ModuleInfo::DynPicker& picker,
                          std::vector<ast::StmtPtr>& out);
    bool synthesizeExternalBindings(uint16_t id, std::vector<ast::StmtPtr>& out);
    bool synthesizePublish(uint16_t id, std::vector<ast::StmtPtr>& out);
    bool emitSynthesized(const std::string& label, const std::string& src,
                         const std::map<std::string, std::string>& renames,
                         std::vector<ast::StmtPtr>& out);

    Graph& graph_;
    SourceSet& sources_;
    DiagnosticSink& diags_;
    std::vector<ModuleInfo> info_;
    // Every binding an EXTERNAL module declares and exports, by canonical
    // name: a read of one is a read through the published namespace
    // (graph.h `ExternalRead`). Built once the renames exist.
    std::map<std::string, ExternalRead> liveReads_;
    size_t syntheticCounter_ = 0;
};

}  // namespace bronze::modules
