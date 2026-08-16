#pragma once

#include <filesystem>
#include <string>

#include "modules/modules.h"
#include "support/diagnostics.h"
#include "support/source.h"

// The two halves of specifier resolution, shared between `resolve.cpp` (which
// decides what KIND of specifier it is holding) and `package.cpp` (which knows
// what a `node_modules` directory and a `package.json` mean). Not part of
// `modules.h`: nothing outside this module resolves a specifier itself.
namespace bronze::modules {

// The one file rule every resolved path obeys, wherever the path came from: it
// must name an existing REGULAR FILE exactly as written. bronze appends no
// extension and looks for no directory index, because both are a way to pick a
// file the program did not name — and picking wrong there is not a compile
// error, it is a different program.
//
// So that the refusal is still useful, the paths a guesser WOULD have reached
// are looked at and named, without being taken: one of them is a "you meant
// this, write it" and two or more is the ambiguity itself, spelled out rather
// than silently broken by a precedence rule.
//
// `what` names the thing being resolved, for the message ("module specifier
// \"./x\"", "\"main\" of package \"lib\"").
bool requireExistingFile(const std::filesystem::path& path, const std::string& what, Span span,
                         DiagnosticSink& diags);

// A BARE specifier — `lib`, `lib/sub.js`, `@scope/pkg` — to a file, by walking
// `node_modules` upward from the importing file and reading the package's
// `package.json`. False on a diagnosed error, and every step that could pick a
// different file than the program meant is one of those errors rather than a
// guess.
bool resolvePackageSpecifier(const std::string& specifier,
                             const std::filesystem::path& importerPath, Span span,
                             DiagnosticSink& diags, std::filesystem::path& out);

}  // namespace bronze::modules
