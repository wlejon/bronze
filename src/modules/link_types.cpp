// TypeScript type names across modules. A type name resolves the way a value
// name does: a module sees its own declarations, what it imports, and nothing
// else. So each module gets a table of the types it exports — its own
// exported declarations, the types it re-exports, the types `export *`
// passes through — and each import, type-only or ordinary, binds the
// declaration the exporting module's table holds into the importer's root
// type scope. Two modules' private `type Id` never meet.
//
// Types bind nothing at run time, so none of this changes the program the
// value linker builds. It runs first only because the value tables need to
// know which names are types: `import { I }` of an interface is legal and
// binds no value.

#include <utility>
#include <vector>

#include "modules/graph.h"
#include "modules/link_internal.h"

namespace bronze::modules {

namespace {

// The module a specifier names from `file`: a value edge, or a type-only
// specifier the loader resolved to a file the graph holds. -1 when neither.
int targetOf(const ModuleFile& file, const std::string& specifier) {
    auto dep = file.deps.find(specifier);
    if (dep != file.deps.end()) return dep->second;
    auto typeDep = file.typeDeps.find(specifier);
    if (typeDep != file.typeDeps.end()) return typeDep->second;
    return -1;
}

// What a type import or export from a module the graph does not hold binds:
// a declaration of no kind, which hints nothing. It still makes the name a
// type the module knows, so that an ordinary `import { T }` of a re-exported
// type stays the legal import it is.
ast::TypeDeclPtr opaqueType(const std::string& name) {
    auto decl = std::make_shared<ast::TypeDecl>();
    decl->name = name;
    return decl;
}

}  // namespace

bool Linker::exportsType(uint16_t moduleId, const std::string& name) const {
    return moduleId < typeExports_.size() && typeExports_[moduleId] &&
           typeExports_[moduleId]->count(name) != 0;
}

ModuleFile& Linker::typeFile(uint16_t moduleId) const {
    const size_t modules = graph_.modules.size();
    return moduleId < modules ? *graph_.modules[moduleId] : *graph_.typeModules[moduleId - modules];
}

ast::TypeDeclPtr Linker::moduleTypeNamespace(uint16_t moduleId) {
    if (!typeNamespaces_[moduleId]) {
        auto decl = std::make_shared<ast::TypeDecl>();
        decl->name = typeFile(moduleId).displayName;
        decl->kinds = ast::kTypeDeclModuleNamespace;
        decl->members = typeExports_[moduleId];
        typeNamespaces_[moduleId] = std::move(decl);
    }
    return typeNamespaces_[moduleId];
}

// To a fixpoint, like the value tables' star expansion and for the same
// reason: a re-export reads another module's table, which in a cycle may
// still be growing. Every step only ever adds a name that was absent, and
// the names are finite, so it terminates.
void Linker::linkTypes() {
    // The program's modules, then the files read for their types alone.
    std::vector<uint16_t> order = graph_.evaluationOrder;
    for (const auto& file : graph_.typeModules) order.push_back(file->id);
    const size_t count = graph_.modules.size() + graph_.typeModules.size();
    typeExports_.assign(count, nullptr);
    typeNamespaces_.assign(count, nullptr);
    for (const uint16_t id : order) {
        typeExports_[id] = std::make_shared<ast::TypeExportTable>();
    }

    bool changed = true;
    auto put = [&](ast::TypeExportTable& table, const std::string& name, ast::TypeDeclPtr decl) {
        if (name.empty() || !decl) return;
        ast::TypeDeclPtr& slot = table[name];
        if (slot) return;
        slot = std::move(decl);
        changed = true;
    };
    // An import never displaces a declaration of the module's own: that is a
    // conflict TypeScript reports, and the local one is what the module's
    // other references were written against.
    auto bind = [&](ast::TypeScope& root, const std::string& local, ast::TypeDeclPtr decl) {
        if (local.empty() || !decl || root.decls.count(local) != 0) return;
        root.decls.emplace(local, std::move(decl));
        changed = true;
    };
    // The table entry `name` of `target`, or an opaque one when the target is
    // not in the graph. Null when the target is in the graph and has no such
    // type (yet: a later round may add it).
    auto lookup = [&](int target, const std::string& name) -> ast::TypeDeclPtr {
        if (target < 0) return opaqueType(name);
        const ast::TypeExportTable& table = *typeExports_[target];
        auto it = table.find(name);
        return it == table.end() ? nullptr : it->second;
    };
    auto namespaceOf = [&](int target, const std::string& name) -> ast::TypeDeclPtr {
        return target < 0 ? opaqueType(name) : moduleTypeNamespace(static_cast<uint16_t>(target));
    };
    // `export * from`: every name but `default`, copied out first because a
    // module may star from itself.
    auto starFrom = [&](ast::TypeExportTable& table, int target) {
        if (target < 0) return;
        std::vector<std::pair<std::string, ast::TypeDeclPtr>> entries(
            typeExports_[target]->begin(), typeExports_[target]->end());
        for (auto& entry : entries) {
            if (entry.first != "default") put(table, entry.first, std::move(entry.second));
        }
    };

    for (int round = 0; changed && round < 256; ++round) {
        changed = false;
        for (const uint16_t id : order) {
            ModuleFile& file = typeFile(id);
            ast::TypeScope& root = *file.ast->rootTypeScope();
            ast::TypeExportTable& table = *typeExports_[id];

            // Imports: an ordinary import binds the types its names also are
            // (a class is a value and a type), a type-only one binds nothing
            // else.
            for (const auto* imp : file.imports) {
                const int target = targetOf(file, imp->specifier);
                if (target < 0) continue;
                for (const auto& spec : imp->specifiers) {
                    if (spec.isNamespace) {
                        bind(root, spec.local, namespaceOf(target, spec.local));
                    } else {
                        bind(root, spec.local, lookup(target, spec.imported));
                    }
                }
            }
            for (const auto& imp : file.ast->typeImports) {
                const int target = targetOf(file, imp.specifier);
                bind(root, imp.local,
                     imp.isNamespace ? namespaceOf(target, imp.local) : lookup(target, imp.imported));
            }

            // Exports. An ordinary export of a name that is (also) a type
            // exports the type under the same name.
            for (const auto* exp : file.exports) {
                if (exp->isStar) {
                    const int target = targetOf(file, exp->fromSpecifier);
                    if (exp->starAlias.empty()) {
                        starFrom(table, target);
                    } else {
                        put(table, exp->starAlias, namespaceOf(target, exp->starAlias));
                    }
                    continue;
                }
                const int target = exp->hasFrom ? targetOf(file, exp->fromSpecifier) : -1;
                for (const auto& spec : exp->specifiers) {
                    if (exp->hasFrom) {
                        if (target >= 0) put(table, spec.exported, lookup(target, spec.local));
                        continue;
                    }
                    auto local = root.decls.find(spec.local);
                    if (local != root.decls.end()) put(table, spec.exported, local->second);
                }
            }
            for (const auto& exp : file.ast->typeExports) {
                if (exp.isStar) {
                    const int target = targetOf(file, exp.fromSpecifier);
                    if (exp.exported.empty()) {
                        starFrom(table, target);
                    } else {
                        put(table, exp.exported, namespaceOf(target, exp.exported));
                    }
                    continue;
                }
                if (exp.hasFrom) {
                    put(table, exp.exported, lookup(targetOf(file, exp.fromSpecifier), exp.local));
                    continue;
                }
                auto local = root.decls.find(exp.local);
                if (local != root.decls.end()) put(table, exp.exported, local->second);
            }
        }
    }
}

}  // namespace bronze::modules
