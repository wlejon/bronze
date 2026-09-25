// Export/import collection, star export resolution, and rename map generation.

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast/queries.h"
#include "ast/query_walk.h"
#include "modules/graph.h"
#include "modules/link_internal.h"

namespace bronze::modules {

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

    // The loader's whole-tree walk (graph.cpp `DynamicImportFinder`), so that
    // every `import()` the loader followed gets its namespace local here, from
    // wherever in the file it stands.
    struct DynImportCollector : public ast::detail::IdentVisitor {
        ModuleFile& file;
        ModuleInfo& mi;
        size_t& counter;

        DynImportCollector(ModuleFile& f, ModuleInfo& m, size_t& c)
            : file(f), mi(m), counter(c) {}

        void scan(ast::Node* n) {
            if (n) n->accept(*this);
        }

        void mention(const std::string&) override {}
        using IdentVisitor::visit;
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
    } dynCollector{file, mi, syntheticCounter_};
    for (const auto& stmt : file.ast->body) {
        dynCollector.scan(stmt.get());
    }

    // The template-literal imports the loader globbed. Each one needs a
    // namespace local per file it matched — the same locals a written specifier
    // would get, and shared with one where a file is reached both ways — plus a
    // picker of its own, because two patterns in a file may glob different
    // directories and must not answer each other's strings.
    for (const auto& pattern : file.dynPatterns) {
        ModuleInfo::DynPicker picker;
        picker.local = "*dyn_pick_" + std::to_string(syntheticCounter_++) + "*";
        picker.head = pattern.head;
        picker.tail = pattern.tail;
        for (const std::string& specifier : pattern.specifiers) {
            auto dep = file.deps.find(specifier);
            if (dep == file.deps.end()) continue;
            const uint16_t target = dep->second;
            std::string local;
            for (const auto& ns : mi.namespaceLocals) {
                if (ns.second == target) {
                    local = ns.first;
                    break;
                }
            }
            if (local.empty()) {
                local = "*dyn_ns_" + std::to_string(syntheticCounter_++) + "*";
                mi.namespaceLocals.emplace_back(local, target);
            }
            picker.entries.emplace_back(specifier, local);
        }
        mi.dynPickers.push_back(std::move(picker));
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
    // The locals the linker invents for a bare `import('./x')` and for a
    // globbed pattern's table need no entry here: no source can spell them,
    // and the `import()` rewrite (Linker::rewriteDynamicImport) writes their
    // canonical names directly.
    return true;
}

}  // namespace bronze::modules
