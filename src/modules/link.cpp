// Linking: what each file exports, what each file's imported names actually
// name, and the one flat namespace all of it is renamed into.
//
// The central claim this file has to make true is that an import binding is a
// LIVE VIEW and not a copy. It makes it true by not creating a binding at
// all: `import { n } from './c.js'` puts `n -> mod1.n` in this file's rename
// map, so every reference to `n` here is a reference to that file's binding,
// resolved by the one name resolver bronze already has. There is one slot,
// and a later write through the exporting module is seen for the same reason
// two lines of one file see each other.

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "lex/lexer.h"
#include "modules/graph.h"
#include "parse/parser.h"

namespace bronze::modules {

namespace {

// A name in the one flat namespace. The `.` is what makes a collision
// impossible: it cannot occur in a JavaScript identifier, so a renamed name
// can never be a name the source wrote. The ENTRY file is not renamed, so a
// single-file build produces exactly the IL it produced before modules
// existed — which is what 121 pinned oracle cases and every pinned IL dump
// depend on.
std::string canonicalName(uint16_t moduleId, const std::string& local) {
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
    std::map<std::string, std::string> renames;
};

class Linker {
public:
    Linker(Graph& graph, SourceSet& sources, DiagnosticSink& diags)
        : graph_(graph), sources_(sources), diags_(diags), info_(graph.modules.size()) {}

    bool run(ast::Module& out);

private:
    bool collectLocals(ModuleFile& file);
    bool collectImports(ModuleFile& file);
    bool collectExports(ModuleFile& file);
    // `changed` is set when a name is added, which is what lets the caller run
    // this to a fixpoint — see `run`.
    bool expandStarExports(ModuleFile& file, bool& changed);
    bool buildRenames(ModuleFile& file);
    // Follows indirect entries to the module that DECLARES the binding.
    bool resolveExport(uint16_t moduleId, const std::string& name, Span span,
                       DiagnosticSink* report, uint16_t& outModule, std::string& outLocal);
    bool addExport(ModuleFile& file, const std::string& exported, ExportEntry entry);
    // `const <ns> = { get "a"() { return <canonical a>; },... };` built by
    // generating source and parsing it.
    ast::StmtPtr synthesizeNamespace(uint16_t owner, const std::string& local, uint16_t target);

    Graph& graph_;
    SourceSet& sources_;
    DiagnosticSink& diags_;
    std::vector<ModuleInfo> info_;
    size_t syntheticCounter_ = 0;
};

bool Linker::addExport(ModuleFile& file, const std::string& exported, ExportEntry entry) {
    ModuleInfo& mi = info_[file.id];
    auto existing = mi.exports.find(exported);
    if (existing != mi.exports.end()) {
        diags_.error(entry.span,
                     "duplicate export '" + exported + "' in " + file.displayName);
        return false;
    }
    mi.exportOrder.push_back(exported);
    mi.exports.emplace(exported, std::move(entry));
    return true;
}

bool Linker::collectLocals(ModuleFile& file) {
    ModuleInfo& mi = info_[file.id];
    for (const auto& name : ast::getScopeDeclarations(file.ast->body)) mi.localNames.insert(name);
    // A `var` written inside a top-level block is a module-level binding
    // wherever it is spelled, so it is renamed with the others; leaving it
    // alone would make it collide with another file's `var` of the same name.
    for (const auto& name : ast::getHoistedVarDeclarations(file.ast->body)) {
        mi.localNames.insert(name);
    }
    return true;
}

bool Linker::collectImports(ModuleFile& file) {
    ModuleInfo& mi = info_[file.id];
    for (const auto* imp : file.imports) {
        const uint16_t target = file.deps.at(imp->specifier);
        for (const auto& spec : imp->specifiers) {
            if (mi.imports.count(spec.local)) {
                diags_.error(spec.span, "duplicate import binding '" + spec.local + "' in " +
                                            file.displayName);
                return false;
            }
            if (mi.localNames.count(spec.local)) {
                diags_.error(spec.span, "'" + spec.local + "' is imported and also declared in " +
                                            file.displayName);
                return false;
            }
            ImportBinding binding;
            binding.module = target;
            binding.exportName = spec.imported;
            binding.isNamespace = spec.isNamespace;
            binding.span = spec.span;
            mi.imports.emplace(spec.local, binding);
            if (spec.isNamespace) mi.namespaceLocals.emplace_back(spec.local, target);
        }
    }

    struct DynImportCollector : public ast::Visitor {
        ModuleFile& file;
        ModuleInfo& mi;
        size_t& counter;

        DynImportCollector(ModuleFile& f, ModuleInfo& m, size_t& c)
            : file(f), mi(m), counter(c) {}

        void scan(ast::Node* n) {
            if (n) n->accept(*this);
        }

        void visit(const ast::NumberLit&) override {}
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
        void visit(const ast::SuperCall& s) override {
            for (const auto& a : s.args) scan(a.get());
        }
        void visit(const ast::SuperMember&) override {}
        void visit(const ast::YieldExpr& y) override { scan(y.argument.get()); }
        void visit(const ast::DynamicImportExpr& di) override {
            if (const auto* str = dynamic_cast<const ast::StringLit*>(di.specifier.get())) {
                auto it = file.deps.find(str->value);
                if (it != file.deps.end()) {
                    const uint16_t target = it->second;
                    bool exists = false;
                    for (const auto& ns : mi.namespaceLocals) {
                        if (ns.second == target) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        const std::string local = "*dyn_ns_" + std::to_string(counter++) + "*";
                        mi.namespaceLocals.emplace_back(local, target);
                    }
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
    } dynCollector{file, mi, syntheticCounter_};
    for (const auto& stmt : file.ast->body) {
        dynCollector.scan(stmt.get());
    }
    return true;
}

bool Linker::collectExports(ModuleFile& file) {
    ModuleInfo& mi = info_[file.id];
    for (const auto* exp : file.exports) {
        if (exp->isStar && exp->starAlias.empty()) continue;  // pass 2

        if (exp->isStar) {
            // `export * as ns from './x'`: one export, bound to a namespace
            // object this file owns. Its local name is deliberately
            // unspellable — no source can name it, and it exists only so that
            // the namespace has somewhere to live.
            const uint16_t target = file.deps.at(exp->fromSpecifier);
            const std::string local = "*ns" + std::to_string(syntheticCounter_++) + "*";
            mi.namespaceLocals.emplace_back(local, target);
            ExportEntry entry;
            entry.localName = local;
            entry.span = exp->span;
            if (!addExport(file, exp->starAlias, std::move(entry))) return false;
            continue;
        }

        if (exp->hasFrom) {
            const uint16_t target = file.deps.at(exp->fromSpecifier);
            for (const auto& spec : exp->specifiers) {
                ExportEntry entry;
                entry.indirect = true;
                entry.targetModule = target;
                entry.targetName = spec.local;
                entry.span = spec.span;
                if (!addExport(file, spec.exported, std::move(entry))) return false;
            }
            continue;
        }

        for (const auto& spec : exp->specifiers) {
            ExportEntry entry;
            entry.span = spec.span;
            auto imported = mi.imports.find(spec.local);
            if (imported != mi.imports.end()) {
                // Re-exporting a name this file imported. A namespace binding
                // is a real local const, so it exports as one; anything else
                // is the other module's binding and stays indirect, which is
                // what keeps it live through two hops.
                if (imported->second.isNamespace) {
                    entry.localName = spec.local;
                } else {
                    entry.indirect = true;
                    entry.targetModule = imported->second.module;
                    entry.targetName = imported->second.exportName;
                }
            } else if (mi.localNames.count(spec.local)) {
                entry.localName = spec.local;
            } else {
                diags_.error(spec.span, "export '" + spec.local + "' names nothing declared in " +
                                            file.displayName);
                return false;
            }
            if (!addExport(file, spec.exported, std::move(entry))) return false;
        }
    }
    return true;
}

bool Linker::expandStarExports(ModuleFile& file, bool& changed) {
    ModuleInfo& mi = info_[file.id];
    for (const auto* exp : file.exports) {
        if (!exp->isStar || !exp->starAlias.empty()) continue;
        const uint16_t target = file.deps.at(exp->fromSpecifier);
        // 16.2.3.7: `export *` does not re-export `default`. The target's
        // table may still be GROWING — a cycle can put a star edge between two
        // modules that each star from the other — so the caller runs this to a
        // fixpoint and a pass that copies a name into place is enough.
        //
        // Iterating `exportOrder` by index: expanding into a module that stars
        // back can append to the very vector being walked.
        for (size_t i = 0; i < info_[target].exportOrder.size(); ++i) {
            const std::string name = info_[target].exportOrder[i];
            if (name == "default") continue;
            auto existing = mi.exports.find(name);
            if (existing != mi.exports.end()) {
                if (!existing->second.fromStar) continue;  // an explicit export wins
                // Two stars offering one name. ECMA-262 makes the name
                // ambiguous and therefore absent, which is a silent hole; two
                // stars naming the SAME definition is legal and harmless.
                uint16_t aMod = 0, bMod = 0;
                std::string aLocal, bLocal;
                const bool a = resolveExport(existing->second.targetModule,
                                             existing->second.targetName, exp->span, nullptr, aMod,
                                             aLocal);
                const bool b = resolveExport(target, name, exp->span, nullptr, bMod, bLocal);
                if (a && b && aMod == bMod && aLocal == bLocal) continue;
                diags_.error(exp->span, "ambiguous export '" + name + "' in " + file.displayName +
                                            ": two 'export * from' sources provide it");
                return false;
            }
            ExportEntry entry;
            entry.indirect = true;
            entry.targetModule = target;
            entry.targetName = name;
            entry.fromStar = true;
            entry.span = exp->span;
            if (!addExport(file, name, std::move(entry))) return false;
            changed = true;
        }
    }
    return true;
}

bool Linker::resolveExport(uint16_t moduleId, const std::string& name, Span span,
                           DiagnosticSink* report, uint16_t& outModule, std::string& outLocal) {
    uint16_t current = moduleId;
    std::string want = name;
    for (int hop = 0; hop < 64; ++hop) {
        const ModuleInfo& mi = info_[current];
        auto it = mi.exports.find(want);
        if (it == mi.exports.end()) {
            if (report) {
                report->error(span, "module " + graph_.modules[current]->displayName +
                                        " has no export named '" + want + "'");
            }
            return false;
        }
        if (!it->second.indirect) {
            outModule = current;
            outLocal = it->second.localName;
            return true;
        }
        current = it->second.targetModule;
        want = it->second.targetName;
    }
    if (report) report->error(span, "export chain for '" + name + "' is too deep to resolve");
    return false;
}

bool Linker::buildRenames(ModuleFile& file) {
    ModuleInfo& mi = info_[file.id];
    for (const auto& name : mi.localNames) mi.renames[name] = canonicalName(file.id, name);
    for (const auto& entry : mi.imports) {
        const std::string& local = entry.first;
        const ImportBinding& binding = entry.second;
        if (binding.isNamespace) {
            // The namespace object is this file's own const, so it is renamed
            // like one of its own declarations.
            mi.renames[local] = canonicalName(file.id, local);
            continue;
        }
        uint16_t defModule = 0;
        std::string defLocal;
        if (!resolveExport(binding.module, binding.exportName, binding.span, &diags_, defModule,
                           defLocal)) {
            return false;
        }
        mi.renames[local] = canonicalName(defModule, defLocal);
    }
    return true;
}

// A generated snippet, parsed by the real parser. Hand-building the AST would
// be a second answer to "what does an object literal with a getter look
// like", and the two would drift the first time the parser changed a field.
// The placeholders are legal identifiers because the canonical names are not
// (they contain dots), so the snippet is renamed rather than interpolated.
ast::StmtPtr Linker::synthesizeNamespace(uint16_t owner, const std::string& local,
                                         uint16_t target) {
    const ModuleInfo& targetInfo = info_[target];
    std::map<std::string, std::string> renames;
    const std::string nsPlaceholder = "bz_ns_" + std::to_string(syntheticCounter_++);
    renames[nsPlaceholder] = canonicalName(owner, local);

    std::string src = "const " + nsPlaceholder + " = {";
    size_t slot = 0;
    for (const auto& name : targetInfo.exportOrder) {
        uint16_t defModule = 0;
        std::string defLocal;
        if (!resolveExport(target, name, Span{}, &diags_, defModule, defLocal)) return nullptr;
        const std::string placeholder = nsPlaceholder + "_v" + std::to_string(slot++);
        renames[placeholder] = canonicalName(defModule, defLocal);
        // The key is a string literal, not an identifier: an export name may
        // be a reserved word, and `get default()` does not parse.
        std::string quoted;
        for (char c : name) {
            if (c == '\\' || c == '"') quoted += '\\';
            quoted += c;
        }
        if (slot > 1) src += ",";
        src += " get \"" + quoted + "\"() { return " + placeholder + "; }";
    }
    src += " };\n";

    const SourceBuffer& buffer =
        sources_.add(graph_.modules[owner]->displayName + " (namespace of " +
                         graph_.modules[target]->displayName + ")",
                     src);
    auto tokens = Lexer(buffer, diags_).lex();
    if (diags_.hasErrors()) return nullptr;
    auto parsed = Parser(std::move(tokens), diags_, buffer.fileId()).parseModule("<namespace>");
    if (!parsed || parsed->body.size() != 1) {
        diags_.error(Span{}, "internal error: synthesized module namespace did not parse");
        return nullptr;
    }
    if (!renameModuleScope(parsed->body, renames, buffer.fileId(), {}, diags_)) return nullptr;
    // The literal is 10.4.6's exotic object, not an object with getters. The
    // flag is set on the parsed node rather than spelled in the generated
    // source because no source syntax can say it — which is the point: nothing
    // a program writes can forge a module namespace.
    auto* decl = dynamic_cast<ast::VarDecl*>(parsed->body[0].get());
    auto* literal = decl ? dynamic_cast<ast::ObjectLit*>(decl->init.get()) : nullptr;
    if (!literal) {
        diags_.error(Span{}, "internal error: synthesized module namespace is not a literal");
        return nullptr;
    }
    literal->isModuleNamespace = true;
    return std::move(parsed->body[0]);
}

bool Linker::run(ast::Module& out) {
    // Tables first, in evaluation order, so that a module's dependencies are
    // linked before it and `resolveExport` never has to look at a half-built
    // table. Star expansion is a second pass over the same order for the same
    // reason, plus one of its own: an explicit export wins over a starred one
    // however they are written.
    for (const uint16_t id : graph_.evaluationOrder) {
        ModuleFile& file = *graph_.modules[id];
        if (!collectLocals(file)) return false;
        if (!collectImports(file)) return false;
        if (!collectExports(file)) return false;
    }
    // To a fixpoint rather than once. `export * from` copies one module's table
    // into another's, and in a cycle both tables are still growing while it
    // does — so a single pass in evaluation order can leave a name behind. The
    // tables only ever grow and the graph is finite, so this terminates.
    for (bool changed = true; changed;) {
        changed = false;
        for (const uint16_t id : graph_.evaluationOrder) {
            if (!expandStarExports(*graph_.modules[id], changed)) return false;
        }
    }
    for (const uint16_t id : graph_.evaluationOrder) {
        if (!buildRenames(*graph_.modules[id])) return false;
    }

    // `export function f() {}` sets `FunctionDecl::isExported` in the parser,
    // but `function f() {} export { f };` is the same fact written elsewhere.
    // Inference reads the flag to decide that a function escapes
    // (`src/types/escape.cpp`), so leaving the second spelling unmarked would
    // let it prove a signature from call sites it can see, for a function
    // whose exportedness says it may have others.
    for (const uint16_t id : graph_.evaluationOrder) {
        ModuleFile& file = *graph_.modules[id];
        for (const auto& entry : info_[id].exports) {
            if (entry.second.indirect) continue;
            for (auto& stmt : file.ast->body) {
                auto* fn = dynamic_cast<ast::FunctionDecl*>(stmt.get());
                if (fn && fn->name == entry.second.localName) fn->isExported = true;
            }
        }
    }

    struct DynImportRewriter {
        ModuleFile& file;
        ModuleInfo& mi;

        void visitStmt(ast::StmtPtr& s) {
            if (!s) return;
            if (auto* b = dynamic_cast<ast::BlockStmt*>(s.get())) {
                for (auto& st : b->stmts) visitStmt(st);
            } else if (auto* v = dynamic_cast<ast::VarDecl*>(s.get())) {
                visitExpr(v->init);
            } else if (auto* r = dynamic_cast<ast::ReturnStmt*>(s.get())) {
                visitExpr(r->value);
            } else if (auto* e = dynamic_cast<ast::ExprStmt*>(s.get())) {
                visitExpr(e->expr);
            } else if (auto* i = dynamic_cast<ast::IfStmt*>(s.get())) {
                visitExpr(i->condition);
                for (auto& st : i->thenBody) visitStmt(st);
                for (auto& st : i->elseBody) visitStmt(st);
            } else if (auto* w = dynamic_cast<ast::WhileStmt*>(s.get())) {
                visitExpr(w->condition);
                for (auto& st : w->body) visitStmt(st);
            } else if (auto* d = dynamic_cast<ast::DoWhileStmt*>(s.get())) {
                for (auto& st : d->body) visitStmt(st);
                visitExpr(d->condition);
            } else if (auto* f = dynamic_cast<ast::ForStmt*>(s.get())) {
                for (auto& init : f->init) visitStmt(init);
                visitExpr(f->condition);
                visitExpr(f->update);
                for (auto& st : f->body) visitStmt(st);
            } else if (auto* sw = dynamic_cast<ast::SwitchStmt*>(s.get())) {
                visitExpr(sw->discriminant);
                for (auto& c : sw->cases) {
                    visitExpr(c.test);
                    for (auto& st : c.body) visitStmt(st);
                }
            } else if (auto* fi = dynamic_cast<ast::ForInStmt*>(s.get())) {
                visitExpr(fi->object);
                for (auto& st : fi->body) visitStmt(st);
            } else if (auto* fo = dynamic_cast<ast::ForOfStmt*>(s.get())) {
                visitExpr(fo->iterable);
                for (auto& st : fo->body) visitStmt(st);
            } else if (auto* l = dynamic_cast<ast::LabeledStmt*>(s.get())) {
                visitStmt(l->body);
            } else if (auto* t = dynamic_cast<ast::TryStmt*>(s.get())) {
                for (auto& st : t->body) visitStmt(st);
                for (auto& st : t->catchBody) visitStmt(st);
                for (auto& st : t->finallyBody) visitStmt(st);
            } else if (auto* th = dynamic_cast<ast::ThrowStmt*>(s.get())) {
                visitExpr(th->value);
            } else if (auto* c = dynamic_cast<ast::ClassDecl*>(s.get())) {
                for (auto& meth : c->methods) {
                    visitExpr(meth.keyExpr);
                    if (meth.fn) {
                        for (auto& st : meth.fn->body) visitStmt(st);
                    }
                }
            } else if (auto* fn = dynamic_cast<ast::FunctionDecl*>(s.get())) {
                for (auto& st : fn->body) visitStmt(st);
            }
        }

        void visitExpr(ast::ExprPtr& ep) {
            if (!ep) return;
            if (auto* di = dynamic_cast<ast::DynamicImportExpr*>(ep.get())) {
                if (const auto* str = dynamic_cast<const ast::StringLit*>(di->specifier.get())) {
                    auto it = file.deps.find(str->value);
                    if (it != file.deps.end()) {
                        uint16_t target = it->second;
                        for (const auto& ns : mi.namespaceLocals) {
                            if (ns.second == target) {
                                auto call = std::make_unique<ast::Call>();
                                call->span = di->span;
                                auto mem = std::make_unique<ast::MemberAccess>();
                                mem->span = di->span;
                                auto pObj = std::make_unique<ast::Ident>();
                                pObj->span = di->span;
                                pObj->name = "Promise";
                                mem->object = std::move(pObj);
                                mem->property = "resolve";
                                call->callee = std::move(mem);
                                auto arg = std::make_unique<ast::Ident>();
                                arg->span = di->span;
                                arg->name = ns.first;
                                call->args.push_back(std::move(arg));
                                ep = std::move(call);
                                return;
                            }
                        }
                    }
                }
                visitExpr(di->specifier);
            } else if (auto* u = dynamic_cast<ast::Unary*>(ep.get())) {
                visitExpr(u->operand);
            } else if (auto* b = dynamic_cast<ast::Binary*>(ep.get())) {
                visitExpr(b->lhs);
                visitExpr(b->rhs);
            } else if (auto* t = dynamic_cast<ast::Ternary*>(ep.get())) {
                visitExpr(t->condition);
                visitExpr(t->thenExpr);
                visitExpr(t->elseExpr);
            } else if (auto* m = dynamic_cast<ast::MemberAccess*>(ep.get())) {
                visitExpr(m->object);
            } else if (auto* i = dynamic_cast<ast::IndexAccess*>(ep.get())) {
                visitExpr(i->object);
                visitExpr(i->index);
            } else if (auto* c = dynamic_cast<ast::Call*>(ep.get())) {
                visitExpr(c->callee);
                for (auto& a : c->args) visitExpr(a);
            } else if (auto* n = dynamic_cast<ast::NewExpr*>(ep.get())) {
                visitExpr(n->callee);
                for (auto& a : n->args) visitExpr(a);
            } else if (auto* s = dynamic_cast<ast::SuperCall*>(ep.get())) {
                for (auto& a : s->args) visitExpr(a);
            } else if (auto* y = dynamic_cast<ast::YieldExpr*>(ep.get())) {
                visitExpr(y->argument);
            } else if (auto* da = dynamic_cast<ast::DestructuringAssign*>(ep.get())) {
                visitExpr(da->value);
            } else if (auto* o = dynamic_cast<ast::ObjectLit*>(ep.get())) {
                for (auto& p : o->props) {
                    visitExpr(p.keyExpr);
                    visitExpr(p.value);
                }
            } else if (auto* a = dynamic_cast<ast::ArrayLit*>(ep.get())) {
                for (auto& e : a->elements) visitExpr(e);
            } else if (auto* f = dynamic_cast<ast::FunctionExpr*>(ep.get())) {
                for (auto& st : f->body) visitStmt(st);
            } else if (auto* cl = dynamic_cast<ast::ClassExpr*>(ep.get())) {
                for (auto& meth : cl->methods) {
                    visitExpr(meth.keyExpr);
                    if (meth.fn) {
                        for (auto& st : meth.fn->body) visitStmt(st);
                    }
                }
            } else if (auto* tpl = dynamic_cast<ast::TemplateLit*>(ep.get())) {
                for (auto& e : tpl->exprs) visitExpr(e);
            } else if (auto* tt = dynamic_cast<ast::TaggedTemplate*>(ep.get())) {
                visitExpr(tt->tag);
            } else if (auto* sp = dynamic_cast<ast::SpreadElement*>(ep.get())) {
                visitExpr(sp->argument);
            }
        }
    };

    for (const uint16_t id : graph_.evaluationOrder) {
        ModuleFile& file = *graph_.modules[id];
        ModuleInfo& mi = info_[id];
        DynImportRewriter rewriter{file, mi};
        for (auto& stmt : file.ast->body) {
            rewriter.visitStmt(stmt);
        }
    }

    for (const uint16_t id : graph_.evaluationOrder) {
        ModuleFile& file = *graph_.modules[id];
        ModuleInfo& mi = info_[id];
        // Every import binding this file has, namespace locals included: each
        // is an immutable view of another module's slot, and the rename has
        // just made it literally that slot, so an assignment through one is
        // refused rather than silently stored (graph.h says why, and why a
        // property write through a namespace is NOT the same question).
        std::map<std::string, std::string> importedBindings;
        for (const auto& entry : mi.imports) {
            importedBindings[entry.first] = graph_.modules[entry.second.module]->displayName;
        }
        if (!renameModuleScope(file.ast->body, mi.renames, id, importedBindings, diags_)) {
            return false;
        }
    }

    // The merge below produces exactly one top level: N files' statements
    // become one `main`, which has one mode. That mode is the entry's, and for
    // a graph of more than one file it is always STRICT — 11.2.2 makes module
    // code strict, and the loader has already parsed every file that way (a
    // non-entry file because being reached through a specifier is what makes it
    // module code, the entry because it turned out to hold an `import` or an
    // `export`). A single-file entry with neither is a Script and keeps
    // whatever its own Directive Prologue asked for.
    //
    // So a disagreement here is no longer something a program can write; it is
    // the loader and this file having drifted apart about which files are
    // module code, and a merged program compiled half in the wrong mode
    // discards a top-level write where it should throw. A tripwire, not a
    // diagnosis of the source.
    out.strict = graph_.modules[0]->ast->strict;
    for (const uint16_t id : graph_.evaluationOrder) {
        ModuleFile& file = *graph_.modules[id];
        if (file.ast->strict == out.strict) continue;
        diags_.error(Span{},
                     "internal error: '" + file.displayName + "' was parsed " +
                         (file.ast->strict ? "strict" : "sloppy") + " and the entry '" +
                         graph_.modules[0]->displayName + "' was parsed " +
                         (out.strict ? "strict" : "sloppy") +
                         ", but every file of a module graph is strict (ECMA-262 11.2.2)");
        return false;
    }

    // The merge. Evaluation order is the post-order of the load, so a module's
    // statements run after everything it imports — which is ES semantics for
    // module bodies, and graph-wide function hoisting comes free, because
    // `lower()` lifts every top-level FunctionDecl out of the statement list
    // before it lowers any body.
    for (const uint16_t id : graph_.evaluationOrder) {
        ModuleFile& file = *graph_.modules[id];
        for (const auto& ns : info_[id].namespaceLocals) {
            auto decl = synthesizeNamespace(id, ns.first, ns.second);
            if (!decl) return false;
            out.body.push_back(std::move(decl));
        }
        for (auto& stmt : file.ast->body) {
            if (dynamic_cast<const ast::ImportDecl*>(stmt.get())) continue;
            if (dynamic_cast<const ast::ExportNamesDecl*>(stmt.get())) continue;
            out.body.push_back(std::move(stmt));
        }
    }
    return !diags_.hasErrors();
}

}  // namespace

bool linkGraph(Graph& graph, SourceSet& sources, DiagnosticSink& diags, ast::Module& out) {
    return Linker(graph, sources, diags).run(out);
}

}  // namespace bronze::modules
