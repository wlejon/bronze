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
    // With the live reads: a getter over an EXTERNAL module's binding (a
    // re-export of one) reads the instance's current value, not the snapshot.
    if (!renameModuleScope(parsed->body, renames, buffer.fileId(), {}, diags_, &liveReads_)) {
        return nullptr;
    }
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
// exactly as if the module had been evaluated here. Those consts are a
// SNAPSHOT, though, taken when this unit starts, and an import is a live view:
// so every expression that reads one is rewritten, by the rename, to read
// `mod3.#ext["counter"]` instead (`liveReads_`). The consts remain for the
// few references that are names rather than expressions (`class C extends
// Imported`), which read the binding once, where they stand.
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
    // One local exported under two names (`export { y, y as alsoY }`) is one
    // binding, declared once.
    std::set<std::string> bound;
    for (const auto& name : mi.exportOrder) {
        uint16_t defModule = 0;
        std::string defLocal;
        if (!resolveExport(id, name, Span{}, &diags_, defModule, defLocal)) return false;
        if (defModule != id) continue;
        if (!bound.insert(defLocal).second) continue;
        const std::string placeholder = nsPlaceholder + "_v" + std::to_string(slot++);
        renames[placeholder] = canonicalName(defModule, defLocal);
        src += "const " + placeholder + " = " + nsPlaceholder + "[\"" + quoteForJs(name) +
               "\"];\n";
    }
    return emitSynthesized(graph_.modules[id]->displayName + " (external module bindings)", src,
                           renames, out);
}

// The one namespace object of a module this unit evaluates, as a binding of
// that module's own scope. Not `#ns`: the ENTRY's names are not prefixed
// (canonicalName), and a bare `#ns` is a private name to lowering. `*` is the
// spelling the linker's other unprefixable synthetic locals use.
std::string Linker::moduleNamespaceName(uint16_t target) const {
    return canonicalName(target, "*namespace*");
}

// Declares `target`'s namespace object here unless an earlier point of the
// merge already did. Every `import * as`, `export * as`, `import()` and
// registry publish of one module reaches it through this, so they all hold the
// same object (16.2.1.6.2 GetModuleNamespace step 3: created once, then
// returned from [[Namespace]]). The first asker is never later than any other
// in the merged program, so no asker sees the declaration in its TDZ that
// would not have seen its own.
bool Linker::ensureModuleNamespace(uint16_t target, std::vector<ast::StmtPtr>& out) {
    if (namespaceDeclared_.count(target)) return true;
    auto decl = synthesizeNamespace(target, "*namespace*", target);
    if (!decl) return false;
    out.push_back(std::move(decl));
    namespaceDeclared_.insert(target);
    return true;
}

// The publishing half: this module's namespace object, left in the realm's
// registry under the module's canonical path.
//
//     const mod3.*namespace* = { get "counter"() { return mod3.counter; } };
//     __bronze_module_publish("<path>", mod3.*namespace*);
//
// The namespace is the one every `import * as` of this module in this unit
// holds — so an export read through the registry is a read of this module's
// own binding, not of a copy taken when the module finished, and a later
// unit's `import * as` of it is the same object as this unit's.
bool Linker::synthesizePublish(uint16_t id, std::vector<ast::StmtPtr>& out) {
    if (!ensureModuleNamespace(id, out)) return false;

    const std::string placeholder = "bz_pub_" + std::to_string(syntheticCounter_++);
    std::map<std::string, std::string> renames{{placeholder, moduleNamespaceName(id)}};
    const std::string src = "__bronze_module_publish(\"" +
                            quoteForJs(graph_.modules[id]->displayName) + "\", " + placeholder +
                            ");\n";
    return emitSynthesized(graph_.modules[id]->displayName + " (module registry publish)", src,
                           renames, out);
}

// An `import()` of a module the graph holds, as the rename meets it (graph.h
// `DynamicImportRewrite`). What it returns is already in canonical names: the
// rename has walked the specifier and will not walk the replacement.
//
// A string specifier resolves to the module's namespace object at once. A
// TEMPLATE specifier the loader globbed becomes a lookup in that pattern's own
// table, matched by head and tail rather than by position, so that this walk
// and the loader's need not visit the file in the same order — and two
// spellings of the same pattern in one file share one table.
ast::ExprPtr Linker::rewriteDynamicImport(uint16_t id, ast::DynamicImportExpr& di) {
    const ModuleFile& file = *graph_.modules[id];
    const ModuleInfo& mi = info_[id];
    auto ident = [&di](const std::string& name) {
        auto n = std::make_unique<ast::Ident>();
        n->span = di.span;
        n->name = name;
        return n;
    };
    if (const auto* str = dynamic_cast<const ast::StringLit*>(di.specifier.get())) {
        auto it = file.deps.find(str->value);
        if (it == file.deps.end()) return nullptr;
        for (const auto& ns : mi.namespaceLocals) {
            if (ns.second != it->second) continue;
            auto mem = std::make_unique<ast::MemberAccess>();
            mem->span = di.span;
            mem->object = ident("Promise");
            mem->property = "resolve";
            auto call = std::make_unique<ast::Call>();
            call->span = di.span;
            call->callee = std::move(mem);
            call->args.push_back(ident(canonicalName(id, ns.first)));
            return call;
        }
        return nullptr;
    }
    auto* tpl = dynamic_cast<ast::TemplateLit*>(di.specifier.get());
    std::string head, tail;
    if (!tpl || !dynamicImportPattern(*tpl, head, tail)) return nullptr;
    for (const auto& picker : mi.dynPickers) {
        if (picker.head != head || picker.tail != tail) continue;
        auto call = std::make_unique<ast::Call>();
        call->span = di.span;
        call->callee = ident(canonicalName(id, picker.local));
        call->args.push_back(std::move(di.specifier));
        return call;
    }
    return nullptr;
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

    for (const uint16_t id : graph_.evaluationOrder) {
        if (!graph_.modules[id]->isExternal) continue;
        const std::string ns = canonicalName(id, syntheticLocal("ext"));
        for (const auto& name : info_[id].exportOrder) {
            uint16_t defModule = 0;
            std::string defLocal;
            if (!resolveExport(id, name, Span{}, &diags_, defModule, defLocal)) return false;
            if (defModule != id) continue;
            // One local exported under two names is one binding; either name
            // reads it.
            liveReads_.emplace(canonicalName(id, defLocal), ExternalRead{ns, name});
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
        const DynamicImportRewrite dynImports = [this, id](ast::DynamicImportExpr& di) {
            return rewriteDynamicImport(id, di);
        };
        if (!renameModuleScope(file.ast->body, mi.renames, id, importedBindings, diags_,
                               &liveReads_, &dynImports)) {
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
            // A namespace OF an external module is the one it published —
            // the same object, not a second one with the same getters, so
            // `ns === pageNs` holds across the seam as it does inside a unit.
            if (graph_.modules[ns.second]->isExternal) {
                const std::string placeholder = "bz_nsx_" + std::to_string(syntheticCounter_++);
                std::map<std::string, std::string> renames{
                    {placeholder, canonicalName(id, ns.first)},
                    {placeholder + "_src", canonicalName(ns.second, syntheticLocal("ext"))}};
                if (!emitSynthesized(graph_.modules[id]->displayName + " (namespace of " +
                                         graph_.modules[ns.second]->displayName + ")",
                                     "const " + placeholder + " = " + placeholder + "_src;\n",
                                     renames, out.body)) {
                    return false;
                }
                continue;
            }
            // One module has one namespace object, however many files ask for
            // it and in how many spellings: this file's local is a second name
            // for it.
            if (!ensureModuleNamespace(ns.second, out.body)) return false;
            const std::string placeholder = "bz_nsa_" + std::to_string(syntheticCounter_++);
            std::map<std::string, std::string> renames{
                {placeholder, canonicalName(id, ns.first)},
                {placeholder + "_src", moduleNamespaceName(ns.second)}};
            if (!emitSynthesized(graph_.modules[id]->displayName + " (namespace of " +
                                     graph_.modules[ns.second]->displayName + ")",
                                 "const " + placeholder + " = " + placeholder + "_src;\n",
                                 renames, out.body)) {
                return false;
            }
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
        // state once its top level has run. The entry (id 0) only when the
        // host said it is a module file (`publishEntry`): otherwise it is the
        // program being run, not an instance another unit imports.
        if (graph_.publishModules && (id != 0 || graph_.publishEntry)) {
            if (!synthesizePublish(id, out.body)) return false;
        }
    }
    return !diags_.hasErrors();
}

bool linkGraph(Graph& graph, SourceSet& sources, DiagnosticSink& diags, ast::Module& out) {
    return Linker(graph, sources, diags).run(out);
}

}  // namespace bronze::modules
