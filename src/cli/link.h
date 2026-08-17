#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "support/diagnostics.h"

// The link step family: everything between "bronze has written an object" and
// "there is an artefact on disk". Split out of driver.cpp because it is a
// self-contained concern with its own shape — a search for the runtime
// libraries, a per-platform fan-out of linker command lines, and a cache of
// the one that worked — and because that shape repeats once per KIND of
// artefact. There are two kinds now.

namespace bronze::cli {

// Object -> native executable: bronze's runtime linked in whole, `main` from
// src/rt/rt.cpp. Tries the toolchains it knows in order and remembers the
// first that worked, because a build compiles hundreds of programs and the
// misses are process launches.
// Both linkers take a LIST of objects because a large module is emitted as
// several partition objects in parallel (llvm_backend.cpp, writeObjectFile);
// an ordinary program's list has one entry.
bool linkExecutable(const std::vector<std::string>& objPaths, const std::string& outputPath,
                    DiagnosticSink& diags);

// Object -> loadable module (DLL / .so / .dylib), against the SHARED runtime.
//
// Same fan-out, three differences that are the whole point: `/DLL` or
// `-shared` instead of an executable; the shared runtime's import library or
// `.so` instead of the static archives, so the loaded module and the host it
// is loaded into share one heap; and no fallback to compiling src/rt/rt.cpp,
// which exists to supply `main` and a module has none.
//
// A missing shared runtime is a diagnosed error naming the library and the
// override, never a silent fall through to the static path — a module linked
// against a static runtime would load, run, and quietly allocate out of a
// second heap.
bool linkSharedModule(const std::vector<std::string>& objPaths, const std::string& outputPath,
                      DiagnosticSink& diags);

// A temp object path unique per process and per call, for the two commands
// that emit an object only to hand it straight to a linker.
std::filesystem::path uniqueTempObjPath(const std::string& sourcePath);

}  // namespace bronze::cli
