#pragma once

#include <string>

#include <brass/object/macho_writer.hpp>
#include <brass/object/object_writer.hpp>

#include "support/diagnostics.h"

// The link step: everything between "the backend has an object" and "there is
// a program on disk". No system linker is involved anywhere below — brass's
// own image writers (AotLinker) turn the object into a DLL/.so/.dylib in
// process, and the runtime is never linked INTO anything: every symbol the
// object leaves undefined is imported from the shared runtime by name.
//
// That makes one kind of artefact rather than two. A loadable module is the
// object as an image importing the runtime, and a `bronze build` executable
// is that same module beside a copy of a prebuilt host (src/host) that opens
// the module named after itself, plus the runtime library the pair loads.

namespace bronze::cli {

// Object -> loadable module (DLL / .so / .dylib) importing the SHARED runtime.
//
// Every undefined symbol the object references must be in the runtime's
// export surface — the registry in src/abi/bronze_abi.h plus the three brass
// words cmake/bronze_abi_exports.cmake appends — and one that is not is a
// diagnosed error naming it, never an unresolved import the loader discovers.
// The module exports the loadable-module contract that header states: the
// entry, its ABI stamp, its host-globals manifest and its native import
// table, and beside them the code-range table a host registers for stack
// walks (`<entry>_code_ranges` / `<entry>_code_range_count`, or
// `bronze_object_*` for the default entry).
//
// ELF and Mach-O modules carry a run-time search path: their own directory
// first, then the directory the shared runtime was found in (see
// findSharedRuntimeDir), so a host beside the runtime or with it already
// loaded resolves the same library either way. A Mach-O module's
// LC_BUILD_VERSION is `machoVersion` (zero fields: brass's defaults).
bool linkSharedModule(const brass::object::ObjectFile& obj, const std::string& outputPath,
                      DiagnosticSink& diags, const std::string& entrySymbol = "bronze_main",
                      const brass::object::MachOBuildVersion& machoVersion = {});

// Object -> native program at `outputPath`: the module written beside it with
// the platform's library extension in place of the output's, the prebuilt
// host copied to `outputPath` itself, and the shared runtime library copied
// into the same directory if it is not already there and current. The host
// finds the module by its own basename at run time (src/host/host_main.cpp).
//
// The host and the runtime come from the directory the shared runtime lives
// in; a tree built without the shared runtime has neither, and the error says
// so by name.
bool linkExecutable(const brass::object::ObjectFile& obj, const std::string& outputPath,
                    DiagnosticSink& diags);

}  // namespace bronze::cli
