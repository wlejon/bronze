// Reading the graph: one file at a time, each parsed exactly once, following
// specifiers depth-first in source order. What comes out is a post-order —
// dependencies before dependents, entry last — which is the ES evaluation
// order, and which stays the answer inside a cycle: 16.2.1.5.3 evaluates each
// member once, on the way out of the walk.

#include <algorithm>
#include <fstream>
#include <sstream>

#include "lex/lexer.h"
#include "modules/graph.h"
#include "modules/modules.h"
#include "modules/resolve.h"
#include "parse/parser.h"

namespace bronze::modules {

// What makes a template-literal specifier globbable. The head is everything
// before the FIRST interpolation and the tail everything after it — later
// quasis and later interpolations are folded into the tail's refusal below,
// because a second `${}` is a second wildcard and one glob cannot say where the
// first ends.
//
// Two rules, both about reach rather than taste: the head must contain a `/`,
// so the pattern names a directory it can enumerate; and the tail may not,
// which together with the head's last `/` pins the interpolation inside a
// single FILENAME. `${x}` filling in `../../etc/passwd` therefore matches
// nothing — it would have to be the name of a file in that one directory.
// Backslashes are refused outright on both sides: a specifier is a URL path,
// and the one place a `\` could appear is a Windows path that leaked in.
bool dynamicImportPattern(const ast::TemplateLit& tpl, std::string& head, std::string& tail) {
    if (tpl.exprs.size() != 1 || tpl.quasis.size() != 2) return false;
    head = tpl.quasis[0];
    tail = tpl.quasis[1];
    if (head.find('/') == std::string::npos) return false;
    if (head.find('\\') != std::string::npos) return false;
    if (tail.find('/') != std::string::npos || tail.find('\\') != std::string::npos) return false;
    return true;
}

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
class DynamicImportFinder : public ast::Visitor {
public:
    std::vector<std::pair<std::string, Span>> list;
    // Template-literal specifiers, head and tail, in source order.
    std::vector<DynamicImportPattern> patterns;

    void scan(const ast::Node* n) {
        if (n) n->accept(*this);
    }

    void visit(const ast::NumberLit&) override {}
    void visit(const ast::BigIntLit&) override {}
    void visit(const ast::SpreadElement& s) override { scan(s.argument.get()); }
    void visit(const ast::StringLit&) override {}
    void visit(const ast::TemplateLit& t) override {
        for (const auto& e : t.exprs) scan(e.get());
    }
    void visit(const ast::TaggedTemplate& t) override {
        scan(t.tag.get());
        if (t.templateLit) scan(t.templateLit.get());
    }
    void visit(const ast::RegExpLit&) override {}
    void visit(const ast::BoolLit&) override {}
    void visit(const ast::NullLit&) override {}
    void visit(const ast::UndefinedLit&) override {}
    void visit(const ast::ThisExpr&) override {}
    void visit(const ast::Ident&) override {}
    void visit(const ast::Unary& u) override { scan(u.operand.get()); }
    void visit(const ast::Binary& b) override {
        scan(b.lhs.get());
        scan(b.rhs.get());
    }
    void visit(const ast::Ternary& t) override {
        scan(t.condition.get());
        scan(t.thenExpr.get());
        scan(t.elseExpr.get());
    }
    void visit(const ast::MemberAccess& m) override { scan(m.object.get()); }
    void visit(const ast::IndexAccess& i) override {
        scan(i.object.get());
        scan(i.index.get());
    }
    void visit(const ast::Call& c) override {
        scan(c.callee.get());
        for (const auto& a : c.args) scan(a.get());
    }
    void visit(const ast::NewExpr& n) override {
        scan(n.callee.get());
        for (const auto& a : n.args) scan(a.get());
    }
    void visit(const ast::NewTargetExpr&) override {}
    void visit(const ast::ImportMetaExpr&) override {}
    void visit(const ast::SuperCall& s) override {
        if (s.baseExpr) scan(s.baseExpr.get());
        for (const auto& a : s.args) scan(a.get());
    }
    void visit(const ast::SuperMember& s) override {
        if (s.baseExpr) scan(s.baseExpr.get());
    }
    void visit(const ast::YieldExpr& y) override { scan(y.argument.get()); }
    void visit(const ast::DynamicImportExpr& di) override {
        if (const auto* s = dynamic_cast<const ast::StringLit*>(di.specifier.get())) {
            list.emplace_back(s->value, di.span);
        } else if (const auto* t = dynamic_cast<const ast::TemplateLit*>(di.specifier.get())) {
            DynamicImportPattern p;
            if (dynamicImportPattern(*t, p.head, p.tail)) {
                p.span = di.span;
                patterns.push_back(std::move(p));
            }
        }
        scan(di.specifier.get());
    }
    void visit(const ast::DestructuringAssign& d) override { scan(d.value.get()); }
    void visit(const ast::ObjectLit& o) override {
        for (const auto& p : o.props) {
            scan(p.keyExpr.get());
            scan(p.value.get());
        }
    }
    void visit(const ast::ArrayLit& a) override {
        for (const auto& e : a.elements) scan(e.get());
    }
    void visit(const ast::FunctionExpr& f) override {
        for (const auto& s : f.body) scan(s.get());
    }
    void visit(const ast::ClassExpr& c) override {
        if (c.superClass) scan(c.superClass.get());
        for (const auto& m : c.methods) {
            scan(m.keyExpr.get());
            if (m.fn) scan(m.fn.get());
        }
    }
    void visit(const ast::BlockStmt& b) override {
        for (const auto& s : b.stmts) scan(s.get());
    }
    void visit(const ast::VarDecl& v) override { scan(v.init.get()); }
    void visit(const ast::ReturnStmt& r) override { scan(r.value.get()); }
    void visit(const ast::ExprStmt& e) override { scan(e.expr.get()); }
    void visit(const ast::IfStmt& i) override {
        scan(i.condition.get());
        for (const auto& s : i.thenBody) scan(s.get());
        for (const auto& s : i.elseBody) scan(s.get());
    }
    void visit(const ast::WhileStmt& w) override {
        scan(w.condition.get());
        for (const auto& s : w.body) scan(s.get());
    }
    void visit(const ast::DoWhileStmt& d) override {
        for (const auto& s : d.body) scan(s.get());
        scan(d.condition.get());
    }
    void visit(const ast::ForStmt& f) override {
        for (const auto& s : f.init) scan(s.get());
        scan(f.condition.get());
        scan(f.update.get());
        for (const auto& s : f.body) scan(s.get());
    }
    void visit(const ast::BreakStmt&) override {}
    void visit(const ast::ContinueStmt&) override {}
    void visit(const ast::SwitchStmt& s) override {
        scan(s.discriminant.get());
        for (const auto& c : s.cases) {
            scan(c.test.get());
            for (const auto& st : c.body) scan(st.get());
        }
    }
    void visit(const ast::ForInStmt& f) override {
        scan(f.object.get());
        for (const auto& s : f.body) scan(s.get());
    }
    void visit(const ast::LabeledStmt& l) override { scan(l.body.get()); }
    void visit(const ast::ForOfStmt& f) override {
        scan(f.iterable.get());
        for (const auto& s : f.body) scan(s.get());
    }
    void visit(const ast::TryStmt& t) override {
        for (const auto& s : t.body) scan(s.get());
        for (const auto& s : t.catchBody) scan(s.get());
        for (const auto& s : t.finallyBody) scan(s.get());
    }
    void visit(const ast::ThrowStmt& t) override { scan(t.value.get()); }
    void visit(const ast::ClassDecl& c) override {
        if (c.superClass) scan(c.superClass.get());
        for (const auto& m : c.methods) {
            scan(m.keyExpr.get());
            if (m.fn) scan(m.fn.get());
        }
    }
    void visit(const ast::FunctionDecl& f) override {
        for (const auto& s : f.body) scan(s.get());
    }
    void visit(const ast::Module& m) override {
        for (const auto& s : m.body) scan(s.get());
    }
    void visit(const ast::ImportDecl&) override {}
    void visit(const ast::ExportNamesDecl&) override {}
};

bool hasModuleDeclaration(const ast::Module& mod) {
    for (const auto& stmt : mod.body) {
        if (dynamic_cast<const ast::ImportDecl*>(stmt.get())) return true;
        if (dynamic_cast<const ast::ExportNamesDecl*>(stmt.get())) return true;
    }
    DynamicImportFinder finder;
    for (const auto& stmt : mod.body) {
        finder.scan(stmt.get());
    }
    return !finder.list.empty() || !finder.patterns.empty();
}

class Loader {
public:
    Loader(SourceSet& sources, DiagnosticSink& diags, Graph& graph, const ModuleOptions& options)
        : sources_(sources), diags_(diags), graph_(graph), options_(options) {}

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

        // ECMA-262 11.2.2: module code is always strict mode code. Every file
        // but the entry is module code by construction — it was REACHED through
        // a specifier, which is what makes it one — so it is parsed strict
        // without asking. The entry is the only ambiguous file: pointed at a
        // program with no `import` and no `export`, bronze compiles a Script,
        // and 121 pinned single-file cases are sloppy-by-default programs.
        //
        // So the entry is parsed, asked whether it turned out to be a module,
        // and parsed AGAIN when it did. A second parse and not a flag flipped
        // afterwards, because strictness decides EARLY ERRORS — a duplicate
        // parameter name, an octal literal, `delete x` — and those are raised
        // during the parse that has already happened. It costs one extra parse
        // of one file per build.
        auto parse = [&](bool forceStrict, DiagnosticSink& sink) {
            auto tokens = Lexer(buffer, sink).lex();
            if (sink.hasErrors()) return std::unique_ptr<ast::Module>();
            return Parser(std::move(tokens), sink, file->id)
                .parseModule(file->displayName, forceStrict);
        };

        const bool nonEntryFile = buffer.fileId() != 0;
        if (nonEntryFile) {
            file->ast = parse(/*forceStrict=*/true, diags_);
            if (!file->ast || diags_.hasErrors()) return false;
        } else {
            DiagnosticSink sloppySink;
            file->ast = parse(/*forceStrict=*/false, sloppySink);
            if (file->ast && !sloppySink.hasErrors()) {
                if (!file->ast->strict && hasModuleDeclaration(*file->ast)) {
                    file->ast = parse(/*forceStrict=*/true, diags_);
                    if (!file->ast || diags_.hasErrors()) return false;
                } else {
                    for (const auto& d : sloppySink.all()) {
                        if (d.severity == Severity::Error) diags_.error(d.span, d.message);
                        else if (d.severity == Severity::Warning) diags_.warning(d.span, d.message);
                    }
                }
            } else {
                // An entry file that fails its sloppy parse is retried strict
                // into a scratch sink before reporting: a module whose
                // strictness matters to an early error must be parsed in module mode.
                DiagnosticSink strictSink;
                auto strictAst = parse(/*forceStrict=*/true, strictSink);
                if (strictAst && !strictSink.hasErrors()) {
                    file->ast = std::move(strictAst);
                } else {
                    const auto& reportSink = strictSink.hasErrors() ? strictSink : sloppySink;
                    for (const auto& d : reportSink.all()) {
                        if (d.severity == Severity::Error) diags_.error(d.span, d.message);
                        else if (d.severity == Severity::Warning) diags_.warning(d.span, d.message);
                    }
                    return false;
                }
            }
        }

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
        DynamicImportFinder finder;
        for (const auto& stmt : file.ast->body) {
            finder.scan(stmt.get());
        }
        for (const auto& item : finder.list) {
            if (!follow(file, item.first, item.second)) return false;
        }
        for (auto& pattern : finder.patterns) {
            if (!followPattern(file, pattern)) return false;
        }
        return true;
    }

    // Glob one template-literal specifier and follow every file it matches.
    //
    // The directory is the head's, resolved against the importer exactly as a
    // written specifier would be — so an import map or a module root that
    // redirects the head redirects the whole pattern. A pattern that matches
    // nothing is not an error: the program may simply never take that path, and
    // a compile-time failure would be a stricter rule than the web's, where the
    // string is not even looked at until it is used.
    bool followPattern(ModuleFile& file, DynamicImportPattern& pattern) {
        const size_t slash = pattern.head.find_last_of('/');
        if (slash == std::string::npos) return true;  // no directory to enumerate
        const std::string dirSpec = pattern.head.substr(0, slash + 1);
        const std::string namePrefix = pattern.head.substr(slash + 1);

        std::filesystem::path dir;
        if (!resolveSpecifierDirectory(dirSpec, file.path, options_.moduleRoots, dir)) return true;

        std::error_code ec;
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (ec) break;
            std::error_code kindEc;
            if (!entry.is_regular_file(kindEc) || kindEc) continue;
            const std::string name = entry.path().filename().generic_string();
            if (name.size() <= namePrefix.size() + pattern.tail.size()) continue;
            if (name.compare(0, namePrefix.size(), namePrefix) != 0) continue;
            if (name.compare(name.size() - pattern.tail.size(), pattern.tail.size(),
                             pattern.tail) != 0) {
                continue;
            }
            names.push_back(name);
        }
        // Directory order is not defined across filesystems, and the specifier
        // list reaches the output (it is the load order of these modules), so
        // it is sorted rather than taken as read.
        std::sort(names.begin(), names.end());
        for (const std::string& name : names) {
            const std::string specifier = dirSpec + name;
            if (!follow(file, specifier, pattern.span)) return false;
            pattern.specifiers.push_back(specifier);
        }
        file.dynPatterns.push_back(std::move(pattern));
        return true;
    }

    bool follow(ModuleFile& file, const std::string& specifier, Span span) {
        if (file.deps.count(specifier)) return true;  // the same specifier twice is one edge
        std::filesystem::path target;
        if (!resolveSpecifier(specifier, file.path, span, diags_, target, options_.moduleRoots)) return false;
        uint16_t targetId = 0;
        if (!load(target, span, targetId)) return false;
        file.deps[specifier] = targetId;
        return true;
    }

    SourceSet& sources_;
    DiagnosticSink& diags_;
    Graph& graph_;
    const ModuleOptions& options_;
    std::map<std::string, uint16_t> byPath_;
};

}  // namespace

bool loadGraph(const std::string& entryPath, SourceSet& sources, DiagnosticSink& diags,
               Graph& out, const ModuleOptions& options) {
    ModuleOptions effectiveOptions = options;
    if (!effectiveOptions.importMapPath.empty()) {
        std::string err;
        if (!loadImportMap(effectiveOptions.importMapPath, effectiveOptions.moduleRoots, err)) {
            sources.add(effectiveOptions.importMapPath, "");
            diags.error(Span{}, err);
            return false;
        }
    }
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
    if (!Loader(sources, diags, out, effectiveOptions).load(entry, Span{}, entryId)) return false;
    return entryId == 0;
}

std::unique_ptr<ast::Module> loadProgram(const std::string& entryPath, SourceSet& sources,
                                         DiagnosticSink& diags,
                                         const ModuleOptions& options) {
    Graph graph;
    if (!loadGraph(entryPath, sources, diags, graph, options)) return nullptr;

    auto merged = std::make_unique<ast::Module>();
    merged->name = graph.modules[0]->displayName;
    if (!linkGraph(graph, sources, diags, *merged)) return nullptr;
    return merged;
}

}  // namespace bronze::modules
