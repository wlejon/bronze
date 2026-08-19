#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "ast/ast.h"
#include "support/diagnostics.h"
#include "support/source.h"

// The module graph. A bronze build used to be one file; it is now an entry file
// plus everything it reaches through relative `import` and `export... from`
// specifiers.
//
// Everything modules mean stops at this boundary. What comes out is ONE
// `ast::Module` — the graph flattened in evaluation order, with every non-entry
// file's module-level bindings renamed into a single namespace — so inference,
// lowering, the IL and the backend see the single-file program they saw before
// this existed.
//
// A CYCLE survives that flattening because the flattening keeps the two things
// that make one well defined: every module's function declarations are hoisted
// before any body runs (`lower()` lifts them all out of the statement list),
// and every module's lexical bindings hold the uninitialized marker until
// their own declaration is reached. So a cycle crossed by function
// declarations works and a cycle that reads a `let` too early is 9.1.1.1.6's
// ReferenceError — which is what ECMA-262 says about each.
namespace bronze::modules {

struct ModuleRoot {
    std::string prefix;
    std::filesystem::path target;
};

struct ModuleOptions {
    std::vector<ModuleRoot> moduleRoots;
    std::string importMapPath;
};

// Loads an import map from a JSON file, resolving relative target paths relative
// to the directory containing the import map JSON file, and appending the resulting
// ModuleRoot entries to `outRoots`.
//
// Returns true on success; on error returns false and sets `err`.
bool loadImportMap(const std::filesystem::path& path, std::vector<ModuleRoot>& outRoots,
                   std::string& err);

// Reads, parses and links the graph rooted at `entryPath`. Every file read is
// appended to `sources` (the entry first, so it is file 0), which the caller
// owns because diagnostics have to render against it even when this fails.
//
// Null on a diagnosed error.
std::unique_ptr<ast::Module> loadProgram(const std::string& entryPath, SourceSet& sources,
                                         DiagnosticSink& diags,
                                         const ModuleOptions& options = {});

// A specifier as written, and the file it was written in, to the file it names.
// Relative (`./x.js`) or BARE (`lib`, `@scope/pkg/sub.js`, resolved by walking
// `node_modules` upward and reading the package's `package.json`); an absolute
// path, a URL and a `#` import are each a named error, as is every step of
// package resolution that has more than one answer — see `resolve.h`. False on
// a diagnosed error.
bool resolveSpecifier(const std::string& specifier, const std::filesystem::path& importerPath,
                      Span span, DiagnosticSink& diags, std::filesystem::path& out,
                      const std::vector<ModuleRoot>& moduleRoots = {});

// The DIRECTORY a specifier prefix names — `./panels/`, `three/addons/loaders/`
// — honouring module roots and the import map exactly as a full specifier
// would. False when the head names no directory, which is deliberately not a
// diagnosed error: its only caller is the template-literal glob, where "no
// such directory" and "the glob matched nothing" are the same answer and
// neither is a reason to fail a build.
bool resolveSpecifierDirectory(const std::string& dirSpecifier,
                               const std::filesystem::path& importerPath,
                               const std::vector<ModuleRoot>& moduleRoots,
                               std::filesystem::path& out);

}  // namespace bronze::modules
