// Reading the graph: one file at a time, each parsed exactly once, following
// specifiers depth-first in source order. What comes out is a post-order —
// dependencies before dependents, entry last — which is the ES evaluation
// order, and which stays the answer inside a cycle: 16.2.1.5.3 evaluates each
// member once, on the way out of the walk.

#include <fstream>
#include <sstream>

#include "lex/lexer.h"
#include "modules/graph.h"
#include "modules/modules.h"
#include "parse/parser.h"

namespace bronze::modules {

namespace {

bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// The depth-first walk. Meeting an id that is already known is the same answer
// whether it is a diamond or a cycle: the module is loaded, and the edge needs
// nothing from this walk but its id. What separates the two is the POST-order
// the walk records — a diamond's target has already been appended to the
// evaluation order, a cycle's has not and will be appended when its own load
// finishes, which is 16.2.1.5.3's "a module in a cycle is evaluated once, on
// the way out".
class Loader {
public:
    Loader(SourceSet& sources, DiagnosticSink& diags, Graph& graph)
        : sources_(sources), diags_(diags), graph_(graph) {}

    bool load(const std::filesystem::path& path, Span importSpan, uint16_t& outId) {
        const std::string key = path.generic_string();
        auto known = byPath_.find(key);
        if (known != byPath_.end()) {
            // One module, parsed once, evaluated once — which is what "a file
            // imported twice is instantiated once" means, and it is also all a
            // cycle needs: following the back edge again would not read a
            // second copy of anything.
            outId = known->second;
            return true;
        }

        std::string text;
        const bool read = readFile(path, text);
        const SourceBuffer& buffer = sources_.add(path.generic_string(), read ? text : "");
        if (!read) {
            diags_.error(importSpan, "cannot read module " + path.generic_string());
            return false;
        }
        if (sources_.size() > kMaxModules) {
            diags_.error(importSpan, "module graph exceeds " + std::to_string(kMaxModules) +
                                         " files; bronze numbers files in 16 bits");
            return false;
        }

        auto file = std::make_unique<ModuleFile>();
        file->id = buffer.fileId();
        file->path = path;
        file->displayName = path.generic_string();

        auto tokens = Lexer(buffer, diags_).lex();
        if (diags_.hasErrors()) return false;
        file->ast = Parser(std::move(tokens), diags_, file->id).parseModule(file->displayName);
        if (!file->ast || diags_.hasErrors()) return false;

        for (const auto& stmt : file->ast->body) {
            if (const auto* imp = dynamic_cast<const ast::ImportDecl*>(stmt.get())) {
                file->imports.push_back(imp);
            } else if (const auto* exp = dynamic_cast<const ast::ExportNamesDecl*>(stmt.get())) {
                file->exports.push_back(exp);
            }
        }

        const uint16_t id = file->id;
        // Registered BEFORE its dependencies are followed, which is the whole
        // of cycle support: a specifier that comes back round to this file
        // finds it here and stops.
        byPath_[key] = id;
        if (graph_.modules.size() <= id) graph_.modules.resize(id + 1);
        graph_.modules[id] = std::move(file);

        if (!loadDependencies(*graph_.modules[id])) return false;

        graph_.evaluationOrder.push_back(id);
        outId = id;
        return true;
    }

private:
    static constexpr size_t kMaxModules = 4000;

    bool loadDependencies(ModuleFile& file) {
        // Imports first, then `export ... from`, each in source order. Both
        // are edges: a re-export needs the target instantiated exactly as an
        // import does.
        for (const auto* imp : file.imports) {
            if (!follow(file, imp->specifier, imp->specifierSpan)) return false;
        }
        for (const auto* exp : file.exports) {
            if (!exp->hasFrom) continue;
            if (!follow(file, exp->fromSpecifier, exp->fromSpan)) return false;
        }
        return true;
    }

    bool follow(ModuleFile& file, const std::string& specifier, Span span) {
        if (file.deps.count(specifier)) return true;  // the same specifier twice is one edge
        std::filesystem::path target;
        if (!resolveSpecifier(specifier, file.path, span, diags_, target)) return false;
        uint16_t targetId = 0;
        if (!load(target, span, targetId)) return false;
        file.deps[specifier] = targetId;
        return true;
    }

    SourceSet& sources_;
    DiagnosticSink& diags_;
    Graph& graph_;
    std::map<std::string, uint16_t> byPath_;
};

}  // namespace

bool loadGraph(const std::string& entryPath, SourceSet& sources, DiagnosticSink& diags,
               Graph& out) {
    std::error_code ec;
    std::filesystem::path entry = std::filesystem::weakly_canonical(entryPath, ec);
    if (ec) entry = entryPath;
    if (!std::filesystem::exists(entry, ec)) {
        // No buffer has been added yet, so give the sink something to render
        // against: an empty buffer named for the file the user asked for.
        sources.add(entryPath, "");
        diags.error(Span{}, "cannot read " + entryPath);
        return false;
    }
    uint16_t entryId = 0;
    if (!Loader(sources, diags, out).load(entry, Span{}, entryId)) return false;
    return entryId == 0;
}

std::unique_ptr<ast::Module> loadProgram(const std::string& entryPath, SourceSet& sources,
                                         DiagnosticSink& diags) {
    Graph graph;
    if (!loadGraph(entryPath, sources, diags, graph)) return nullptr;

    auto merged = std::make_unique<ast::Module>();
    merged->name = graph.modules[0]->displayName;
    if (!linkGraph(graph, sources, diags, *merged)) return nullptr;
    return merged;
}

}  // namespace bronze::modules
