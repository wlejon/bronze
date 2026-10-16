#pragma once

#include <string>
#include <vector>

#include <brass/object/macho_writer.hpp>
#include <brass/object/object_writer.hpp>
#include <brass/target/target.hpp>

#include "support/diagnostics.h"

// An object as a loadable module: brass's image writers (AotLinker) turn the
// backend's object into a DLL / .so / .dylib in process, importing every
// symbol the object leaves undefined from the SHARED runtime by name. No
// system linker is involved, which is what lets anything holding this library
// — the CLI, and a host compiling a script ahead of time through the embedded
// evaluator — write a module on a machine with no toolchain.
//
// Every undefined symbol the object references must be in the runtime's
// export surface — the registry in src/abi/bronze_abi.h plus the brass words
// cmake/bronze_abi_exports.cmake appends — or a C math function; one that is
// not is a diagnosed error naming it, never an unresolved import the loader
// discovers. The module exports the loadable-module contract that header
// states: the entry, its ABI stamp, its host-globals manifest and its native
// import table, and beside them the code-range table
// (`<entry>_code_ranges` / `<entry>_code_range_count`, or `bronze_object_*`
// for the default entry).

namespace bronze::codegen {

// What a module for `target` imports the runtime as (the DLL name on Windows,
// the soname on ELF, the install name on Mach-O), its file extension, and the
// search-path entry naming the module's own directory (none on Windows, where
// the loader looks beside the host).
struct ModuleTargetNames {
    const char* runtimeImport;
    const char* mathImport;
    const char* moduleExtension;
    const char* ownDirRpath;
};
ModuleTargetNames moduleTargetNames(const brass::Target& target);

// Object -> loadable module at `outputPath`. ELF and Mach-O modules search
// their own directory first, then each of `extraRpaths` in order; a loader
// that already has the runtime mapped binds that copy either way. A Mach-O
// module's LC_BUILD_VERSION is `machoVersion` (zero fields: brass's defaults).
bool writeModuleImage(const brass::object::ObjectFile& obj, const std::string& outputPath,
                      DiagnosticSink& diags, const std::string& entrySymbol,
                      const brass::object::MachOBuildVersion& machoVersion,
                      const std::vector<std::string>& extraRpaths);

}  // namespace bronze::codegen
