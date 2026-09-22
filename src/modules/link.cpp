// Linking: namespace and picker synthesis, module graph flattening, and linkGraph entry.

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "lex/lexer.h"
#include "modules/graph.h"
#include "modules/link_internal.h"
#include "parse/parser.h"

namespace bronze::modules {

namespace {

// A JS string literal's body. The paths and export names that reach this are
// the compiler's own, but a name is whatever the source wrote, so the two
// characters that could end the literal are escaped rather than assumed absent.
std::string quoteForJs(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

// The local a synthetic declaration owns. `#` cannot occur in a JavaScript
// identifier, so `mod3.#ns` is a name no source can spell and no rename of a
// user binding can collide with — the same argument the `.` in `canonicalName`
// makes, one level down.
std::string syntheticLocal(const char* what) { return std::string("#") + what; }

}  // namespace

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

// The function a template-literal `import()` becomes: a Map from the specifiers
// the glob found to the namespace objects behind them, and a lookup that
// resolves a hit and REJECTS a miss.
//
// A Map and not an object literal, because the key is a string the program
// computed and an object would answer `"constructor"` out of its prototype. It
// rejects rather than throws because that is what `import()` does with a
// specifier it cannot resolve — the caller is awaiting a promise, and a synchronous
// throw from a callee it is not calling directly would land somewhere else.
//
// Generated as source and parsed, for the reason synthesizeNamespace gives:
// hand-built AST is a second answer to "what does this shape look like", and
// the two drift.
bool Linker::synthesizePicker(uint16_t owner, const ModuleInfo::DynPicker& picker,
                              std::vector<ast::StmtPtr>& out) {
    std::map<std::string, std::string> renames;
    const std::string fn = "bz_pick_" + std::to_string(syntheticCounter_++);
    renames[fn] = canonicalName(owner, picker.local);

    std::string src = "const " + fn + "_t = new Map([";
    size_t slot = 0;
    for (const auto& entry : picker.entries) {
        const std::string placeholder = fn + "_v" + std::to_string(slot);
        renames[placeholder] = canonicalName(owner, entry.second);
        std::string quoted;
        for (char c : entry.first) {
            if (c == '\\' || c == '\"') quoted += '\\';
            quoted += c;
        }
        if (slot++ > 0) src += ", ";
        src += "[\"" + quoted + "\", " + placeholder + "]";
    }
    src += "]);\n";
    // `String(s)` and not `s`: 16.2.1.8 ToString()s the specifier before it
    // resolves anything, so `import({ toString() { return './a.js'; } })` finds
    // the module a browser would.
    src += "function " + fn + "(s) {\n";
    src += "    const k = String(s);\n";
    src += "    if (" + fn + "_t.has(k)) return Promise.resolve(" + fn + "_t.get(k));\n";
    src += "    return Promise.reject(new TypeError(\"Cannot resolve module \" + k));\n";
    src += "}\n";
    renames[fn + "_t"] = canonicalName(owner, picker.local + "_t");

    const SourceBuffer& buffer = sources_.add(
        graph_.modules[owner]->displayName + " (dynamic import table)", src);
    auto tokens = Lexer(buffer, diags_).lex();
    if (diags_.hasErrors()) return false;
    auto parsed = Parser(std::move(tokens), diags_, buffer.fileId()).parseModule("<dynimport>");
    if (!parsed || parsed->body.size() != 2) {
        diags_.error(Span{}, "internal error: synthesized dynamic-import table did not parse");
        return false;
    }
    if (!renameModuleScope(parsed->body, renames, buffer.fileId(), {}, diags_)) return false;
    for (auto& stmt : parsed->body) out.push_back(std::move(stmt));
    return true;
}

bool Linker::emitSynthesized(const std::string& label, const std::string& src,
                             const std::map<std::string, std::string>& renames,
                             std::vector<ast::StmtPtr>& out) {
    const SourceBuffer& buffer = sources_.add(label, src);
    auto tokens = Lexer(buffer, diags_).lex();
    if (diags_.hasErrors()) return false;
    auto parsed = Parser(std::move(tokens), diags_, buffer.fileId()).parseModule("<synthetic>");
    if (!parsed) {
        diags_.error(Span{}, "internal error: synthesized " + label + " did not parse");
        return false;
    }
    if (!renameModuleScope(parsed->body, renames, buffer.fileId(), {}, diags_)) return false;
    for (auto& stmt : parsed->body) out.push_back(std::move(stmt));
    return true;
}

// An EXTERNAL module contributes no statements. What it contributes instead is
// one binding per export, read out of the namespace an earlier unit published:
//
//     const mod3.#ext = __bronze_module_lookup("<path>");
//     const mod3.counter = mod3.#ext["counter"];
//
// Every importer of this module was renamed to `mod3.counter` by `buildRenames`
// exactly as if the module had been evaluated here, so nothing else in the
// linker has to know the difference.
//
// Only the names this module DECLARES are bound. A name it re-exports from
// somewhere else resolves to that module's canonical binding, which is either
// bound by that module's own external arm or declared by its statements — so
// binding it here would be a duplicate declaration of the same slot.
bool Linker::synthesizeExternalBindings(uint16_t id, std::vector<ast::StmtPtr>& out) {
    const ModuleInfo& mi = info_[id];
    std::map<std::string, std::string> renames;
    const std::string nsPlaceholder = "bz_ext_" + std::to_string(syntheticCounter_++);
    renames[nsPlaceholder] = canonicalName(id, syntheticLocal("ext"));

    std::string src = "const " + nsPlaceholder + " = __bronze_module_lookup(\"" +
                      quoteForJs(graph_.modules[id]->displayName) + "\");\n";
    size_t slot = 0;
    for (const auto& name : mi.exportOrder) {
        uint16_t defModule = 0;
        std::string defLocal;
        if (!resolveExport(id, name, Span{}, &diags_, defModule, defLocal)) return false;
        if (defModule != id) continue;
        const std::string placeholder = nsPlaceholder + "_v" + std::to_string(slot++);
        renames[placeholder] = canonicalName(defModule, defLocal);
        src += "const " + placeholder + " = " + nsPlaceholder + "[\"" + quoteForJs(name) +
               "\"];\n";
    }
    return emitSynthesized(graph_.modules[id]->displayName + " (external module bindings)", src,
                           renames, out);
}

// The publishing half: a namespace object for this module, left in the realm's
// registry under the module's canonical path.
//
//     const mod3.#ns = { get "counter"() { return mod3.counter; } };
//     __bronze_module_publish("<path>", mod3.#ns);
//
// The namespace is the same 10.4.6 exotic `import * as` builds, and it is built
// by the same function — so an export read through the registry is a read of
// this module's own binding, not of a copy taken when the module finished.
bool Linker::synthesizePublish(uint16_t id, std::vector<ast::StmtPtr>& out) {
    const std::string local = syntheticLocal("ns");
    auto decl = synthesizeNamespace(id, local, id);
    if (!decl) return false;
    out.push_back(std::move(decl));

    const std::string placeholder = "bz_pub_" + std::to_string(syntheticCounter_++);
    std::map<std::string, std::string> renames{{placeholder, canonicalName(id, local)}};
    const std::string src = "__bronze_module_publish(\"" +
                            quoteForJs(graph_.modules[id]->displayName) + "\", " + placeholder +
                            ");\n";
    return emitSynthesized(graph_.modules[id]->displayName + " (module registry publish)", src,
                           renames, out);
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
                if (c->superClass) visitExpr(c->superClass);
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
                // A TEMPLATE specifier the loader globbed: the call becomes a
                // lookup in that pattern's own table. Matched by head and tail
                // rather than by position, so that this walk and the loader's
                // need not visit the file in the same order — and two spellings
                // of the same pattern in one file share one table.
                if (auto* tpl = dynamic_cast<ast::TemplateLit*>(di->specifier.get())) {
                    std::string head, tail;
                    if (dynamicImportPattern(*tpl, head, tail)) {
                        for (const auto& picker : mi.dynPickers) {
                            if (picker.head != head || picker.tail != tail) continue;
                            auto call = std::make_unique<ast::Call>();
                            call->span = di->span;
                            auto callee = std::make_unique<ast::Ident>();
                            callee->span = di->span;
                            callee->name = picker.local;
                            call->callee = std::move(callee);
                            call->args.push_back(std::move(di->specifier));
                            // The interpolation is ordinary code of this
                            // module and still has to be walked — it is now an
                            // argument, and may hold an `import()` of its own.
                            visitExpr(call->args[0]);
                            ep = std::move(call);
                            return;
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
                if (s->baseExpr) visitExpr(s->baseExpr);
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
                if (cl->superClass) visitExpr(cl->superClass);
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
        // An EXTERNAL module's instance already exists: its exports are bound
        // from the realm's registry and its statements are not merged, so the
        // program that imports it observes the one instance rather than making
        // a second. Its namespaces and pickers are not synthesized either —
        // those hold the modules IT imports, whose bindings this unit never
        // evaluated. `import * as ns` from an external module still works: the
        // namespace the IMPORTER declares is built from the canonical names
        // bound just below.
        if (file.isExternal) {
            if (!synthesizeExternalBindings(id, out.body)) return false;
            continue;
        }
        for (const auto& ns : info_[id].namespaceLocals) {
            auto decl = synthesizeNamespace(id, ns.first, ns.second);
            if (!decl) return false;
            out.body.push_back(std::move(decl));
        }
        // After the namespaces, because a picker's table holds them.
        for (const auto& picker : info_[id].dynPickers) {
            if (!synthesizePicker(id, picker, out.body)) return false;
        }
        for (auto& stmt : file.ast->body) {
            if (dynamic_cast<const ast::ImportDecl*>(stmt.get())) continue;
            if (dynamic_cast<const ast::ExportNamesDecl*>(stmt.get())) continue;
            out.body.push_back(std::move(stmt));
        }
        // AFTER the statements, because what is published is the module's
        // state once its top level has run. Never the entry (id 0): the entry
        // is the program being run, not an instance another unit imports, and
        // publishing it would make a second run of the same file a no-op.
        if (graph_.publishModules && id != 0) {
            if (!synthesizePublish(id, out.body)) return false;
        }
    }
    return !diags_.hasErrors();
}

bool linkGraph(Graph& graph, SourceSet& sources, DiagnosticSink& diags, ast::Module& out) {
    return Linker(graph, sources, diags).run(out);
}

}  // namespace bronze::modules
