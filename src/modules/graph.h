#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "support/diagnostics.h"
#include "support/source.h"

// The loaded graph, shared between the loader that builds it and the linker
// that reads it. Not part of `modules.h`: nothing outside this module has any
// business knowing that a build read more than one file.
namespace bronze::modules {

struct ModuleFile {
    uint16_t id = 0;
    std::filesystem::path path;
    std::string displayName;  // what a diagnostic calls it
    std::unique_ptr<ast::Module> ast;
    // The top-level import and export nodes, in source order, borrowed from
    // `ast`. Source order is what makes the load deterministic: the DFS
    // follows the specifiers in the order they were written (docs/0001
    // decision 10 — a graph is a new place for an iteration order to reach an
    // output path).
    std::vector<const ast::ImportDecl*> imports;
    std::vector<const ast::ExportNamesDecl*> exports;
    // Specifier text as written -> the module it resolved to. An ordered map:
    // it is only ever looked up, but a graph is exactly the kind of thing
    // that grows an iteration over it later.
    std::map<std::string, uint16_t> deps;
};

struct Graph {
    std::vector<std::unique_ptr<ModuleFile>> modules;  // indexed by id
    // Post-order of the depth-first walk: a module's dependencies come before
    // it, and the entry is last. Cycles are refused (docs/0023 decision 2),
    // so this order is total and evaluating in it is ES semantics.
    std::vector<uint16_t> evaluationOrder;
};

// Reads, lexes and parses the entry and everything it reaches. False on a
// diagnosed error, which includes an unresolvable specifier and a cycle.
bool loadGraph(const std::string& entryPath, SourceSet& sources, DiagnosticSink& diags,
               Graph& out);

// Links the loaded graph into `out`: export tables, import bindings, the
// renaming, the namespace objects, and the concatenation in evaluation order.
// False on a diagnosed error.
bool linkGraph(Graph& graph, SourceSet& sources, DiagnosticSink& diags, ast::Module& out);

// Renames every reference that resolves to the file's MODULE scope, per
// `renames`, and stamps `fileId` onto every span the walk reaches.
//
// `namespaceLocals` are the local names bound to an imported module
// namespace; a property write through one is refused by name, because the
// object bronze synthesizes for a namespace has getters and no setters and
// would swallow the write (docs/0023 decision 4).
//
// A node kind the walk does not know is an internal error, not a subtree left
// alone: leaving one alone is a reference that keeps a name the linker has
// moved, and the two failure modes are a wrong binding and a wrong shadow —
// both silent.
bool renameModuleScope(std::vector<ast::StmtPtr>& stmts,
                       const std::map<std::string, std::string>& renames, uint16_t fileId,
                       const std::map<std::string, std::string>& namespaceLocals,
                       DiagnosticSink& diags);

}  // namespace bronze::modules
