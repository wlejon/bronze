// Reading the graph: one file at a time, each parsed exactly once, following
// specifiers depth-first in source order. What comes out is a post-order —
// dependencies before dependents, entry last — which is the ES evaluation
// order once cycles are out of the picture (docs/0023 decision 2).

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

// The depth-first walk. `stack` is the path of modules currently being
// loaded, which is what makes a cycle a back edge and nothing subtler:
// meeting an id that is on the stack means the specifier just followed closes
// a loop, and the loop is exactly the tail of the stack.
class Loader {
public:
    Loader(SourceSet& sources, DiagnosticSink& diags, Graph& graph)
        : sources_(sources), diags_(diags), graph_(graph) {}

    bool load(const std::filesystem::path& path, Span importSpan, uint16_t& outId) {
        const std::string key = path.generic_string();
        auto known = byPath_.find(key);
        if (known != byPath_.end()) {
            const uint16_t id = known->second;
            for (size_t i = 0; i < stack_.size(); ++i) {
                if (stack_[i] != id) continue;
                reportCycle(i, importSpan);
                return false;
            }
            // Already loaded and not on the stack: a diamond. One module,
            // parsed once, evaluated once — which is what "a file imported
            // twice is instantiated once" means.
            outId = id;
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
        byPath_[key] = id;
        if (graph_.modules.size() <= id) graph_.modules.resize(id + 1);
        graph_.modules[id] = std::move(file);
        stack_.push_back(id);

        if (!loadDependencies(*graph_.modules[id])) return false;

        stack_.pop_back();
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

    // The cycle is the stack from the module that was met again, plus the
    // module whose specifier closed it. Naming the whole path is the
    // difference between a message a reader can act on and "there is a cycle
    // somewhere".
    void reportCycle(size_t firstOnStack, Span importSpan) {
        std::string path;
        for (size_t i = firstOnStack; i < stack_.size(); ++i) {
            path += graph_.modules[stack_[i]]->displayName;
            path += " -> ";
        }
        path += graph_.modules[stack_[firstOnStack]]->displayName;
        diags_.error(importSpan,
                     "cyclic module dependency: " + path +
                         " (bronze has no temporal dead zone, so a binding read before its "
                         "initialiser has run would answer undefined instead of raising a "
                         "ReferenceError)");
    }

    SourceSet& sources_;
    DiagnosticSink& diags_;
    Graph& graph_;
    std::map<std::string, uint16_t> byPath_;
    std::vector<uint16_t> stack_;
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
