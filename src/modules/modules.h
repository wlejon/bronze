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

// What a load's OUTPUT depends on beyond the text of the files it read (which
// the SourceSet already holds): the directory listings behind a template-literal
// `import()` glob, and every non-relative specifier's resolution, which reads
// module roots, `node_modules` and `package.json` files along the way. A
// relative specifier needs no entry: it is joined, never searched for
// (resolve.cpp), so only its target's text can change it.
//
// Filled by a load when `ModuleOptions::dependencyLog` is set, and re-checked by
// `dependenciesUnchanged` — the on-disk code cache's proof that a program it
// stored would still be the program a fresh load builds.
struct DependencyLog {
    struct Glob {
        std::string dirSpecifier;  // the head up to its last '/', as written
        std::string importer;      // the path it resolved from
        std::string namePrefix;
        std::string tail;
        std::vector<std::string> names;  // the sorted matches found
    };
    struct Edge {
        std::string specifier;
        std::string importer;
        std::string target;  // generic_string of the resolved path
    };
    std::vector<Glob> globs;
    std::vector<Edge> edges;
    // The SourceSet names of the buffers read from disk. Every other buffer
    // (the in-memory entry, text the linker synthesizes) is not a file.
    std::vector<std::string> filesRead;
};

// True when every glob in `log` still lists the same names and every edge still
// resolves to the same file. The file TEXTS are the caller's to compare.
bool dependenciesUnchanged(const DependencyLog& log, const std::vector<ModuleRoot>& moduleRoots);

struct ModuleOptions {
    std::vector<ModuleRoot> moduleRoots;
    std::string importMapPath;
    // Where the ENTRY file's own specifiers resolve from, when the file on disk
    // is not where the program lives: a host that compiles a document's inline
    // script writes it to a temp file, and `./x.js` in that script means the
    // file beside the document, not one beside the temp file. Empty means the
    // entry resolves from its own path like every other module. Only the entry
    // is affected — a module it imports is a real file and resolves from where
    // it is.
    std::filesystem::path entryResolvesAs;
    // The REALM's module registry, when the host wants this unit to share
    // module instances with the units compiled beside it
    // (runtime/module_registry.h says why that is a thing at all).
    //
    // `publishModules` makes every file this unit evaluates leave a namespace
    // object behind under its canonical path. `externalModules` is the set of
    // canonical paths some earlier unit already left one for: such a file is
    // still parsed, for its export names, but contributes no statements —
    // its exports are bound from the registry instead, so the importing code
    // sees the instance that exists rather than a second one.
    //
    // Off by default, because a standalone program is one unit and a registry
    // it never reads is a namespace object per module for nothing.
    bool publishModules = false;
    std::vector<std::string> externalModules;
    // Publish the ENTRY as well (with `publishModules`). The entry is normally
    // the program being run, not an instance anyone imports — a driver script
    // run twice must run twice. A host's `<script type="module" src=x.js>` is
    // different: it IS the module instance for x.js, and a later unit's
    // `import "x.js"` must bind it rather than run the page's boot again. The
    // entry is then keyed by its canonical path, the key an import resolves to.
    // Only for an entry that is a real file; inline script text has no URL a
    // module could be imported by.
    bool publishEntry = false;
    // Filled with what the load depended on (see DependencyLog). Null: nothing
    // is recorded. Last, so positional initializers of the fields above keep
    // their meaning.
    DependencyLog* dependencyLog = nullptr;
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

// Reads, parses and links the graph rooted at in-memory entry `code`.
//
// Null on a diagnosed error.
std::unique_ptr<ast::Module> loadProgramSource(const std::string& code,
                                               const std::string& entryPath,
                                               SourceSet& sources,
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
