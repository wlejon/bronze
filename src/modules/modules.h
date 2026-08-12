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
namespace bronze::modules {

// Reads, parses and links the graph rooted at `entryPath`. Every file read is
// appended to `sources` (the entry first, so it is file 0), which the caller
// owns because diagnostics have to render against it even when this fails.
//
// Null on a diagnosed error.
std::unique_ptr<ast::Module> loadProgram(const std::string& entryPath, SourceSet& sources,
                                         DiagnosticSink& diags);

// A specifier as written, and the file it was written in, to the file it names.
// Relative only; everything else is a named error. False on a diagnosed error.
bool resolveSpecifier(const std::string& specifier, const std::filesystem::path& importerPath,
                      Span span, DiagnosticSink& diags, std::filesystem::path& out);

}  // namespace bronze::modules
