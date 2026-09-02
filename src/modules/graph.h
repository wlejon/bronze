#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "modules/modules.h"
#include "support/diagnostics.h"
#include "support/source.h"

// The loaded graph, shared between the loader that builds it and the linker
// that reads it. Not part of `modules.h`: nothing outside this module has any
// business knowing that a build read more than one file.
namespace bronze::modules {

// A dynamic `import()` whose specifier is a TEMPLATE LITERAL with a static
// head and tail — `import(`./Sidebar.Geometry.${type}.js`)`. A compiler that
// waits for the runtime string has nothing to compile; what it CAN do is what
// the directory says, which is the same thing a browser would find when the
// string finally arrives. So the head and tail become a glob, every file in
// that directory matching it joins the graph, and the call becomes a lookup in
// a table of exactly those specifiers. A string the table does not hold
// rejects, which is what a browser does with a 404.
//
// Only the FIRST interpolation is used as the wildcard and the tail must
// contain no path separator, so the pattern can never reach outside the one
// directory the head names.
struct DynamicImportPattern {
    std::string head;                   // specifier text before the first `${`
    std::string tail;                   // specifier text after it
    Span span;
    // Every concrete specifier the glob found, in directory order. These are
    // keys of `ModuleFile::deps`, so the linker reaches each target through
    // the same table an ordinary specifier uses.
    std::vector<std::string> specifiers;
};

struct ModuleFile {
    uint16_t id = 0;
    std::filesystem::path path;
    std::string displayName;  // what a diagnostic calls it
    std::unique_ptr<ast::Module> ast;
    // The top-level import and export nodes, in source order, borrowed from
    // `ast`. Source order is what makes the load deterministic: the DFS follows
    // the specifiers in the order they were written (a graph is a new place for
    // an iteration order to reach an output path).
    std::vector<const ast::ImportDecl*> imports;
    std::vector<const ast::ExportNamesDecl*> exports;
    // Specifier text as written -> the module it resolved to. An ordered map:
    // it is only ever looked up, but a graph is exactly the kind of thing
    // that grows an iteration over it later.
    std::map<std::string, uint16_t> deps;
    // The template-literal dynamic imports this file writes, in source order,
    // and what each one's directory held at compile time. See
    // `dynamicImportPattern` below for what makes a template one of these.
    std::vector<DynamicImportPattern> dynPatterns;
};

struct Graph {
    std::vector<std::unique_ptr<ModuleFile>> modules;  // indexed by id
    // Post-order of the depth-first walk: a module's dependencies come before
    // it, and the entry is last. A CYCLE has no such order — that is what a
    // cycle is — and the post-order is the answer 16.2.1.5.3 gives anyway: the
    // member of the cycle the walk left first runs first, and a binding a
    // later member has not initialized yet is in its temporal dead zone rather
    // than absent.
    std::vector<uint16_t> evaluationOrder;
};

// Reads, lexes and parses the entry and everything it reaches. False on a
// diagnosed error, which includes an unresolvable specifier.
bool loadGraph(const std::string& entryPath, SourceSet& sources, DiagnosticSink& diags,
               Graph& out, const ModuleOptions& options = {});

// The head/tail of a template-literal specifier, or false when the template is
// not one this compiler can turn into a glob: no interpolation at all (then it
// is just a string), a head that names no directory, a tail carrying a path
// separator, or a tail that does not end in a module extension (then the
// interpolation is bounded by nothing, and the call is the runtime's). Shared
// by the loader, which globs with it, and the linker, which builds the lookup
// table from the same reading.
bool dynamicImportPattern(const ast::TemplateLit& tpl, std::string& head, std::string& tail);

// Links the loaded graph into `out`: export tables, import bindings, the
// renaming, the namespace objects, and the concatenation in evaluation order.
// False on a diagnosed error.
bool linkGraph(Graph& graph, SourceSet& sources, DiagnosticSink& diags, ast::Module& out);

// Renames every reference that resolves to the file's MODULE scope, per
// `renames`, and stamps `fileId` onto every span the walk reaches.
//
// `importedBindings` are this file's import bindings — every `import { x }`,
// `import d`, `import * as ns` local — mapped to the module each names. An
// ASSIGNMENT to one is refused here, because 16.2.1.6.1 CreateImportBinding
// makes an import an immutable indirect binding: the local is a view of another
// module's slot, and linking has renamed it to that slot, so a write bronze let
// through would silently store into the exporting module's binding.
//
// A property write THROUGH a namespace local — `ns.z = 5` — is deliberately not
// refused here, and the two are not the same operation. That one assigns to a
// property of an ordinary object VALUE; ECMA-262 10.4.6.9 makes it a [[Set]]
// that returns false, which is a runtime TypeError in strict code and nothing
// the compiler can decide. Refusing it here is what kept a whole correct
// program from building (`tests/oracle/cases/module_namespace_object`).
//
// A node kind the walk does not know is an internal error, not a subtree left
// alone: leaving one alone is a reference that keeps a name the linker has
// moved, and the two failure modes are a wrong binding and a wrong shadow —
// both silent.
bool renameModuleScope(std::vector<ast::StmtPtr>& stmts,
                       const std::map<std::string, std::string>& renames, uint16_t fileId,
                       const std::map<std::string, std::string>& importedBindings,
                       DiagnosticSink& diags);

}  // namespace bronze::modules
