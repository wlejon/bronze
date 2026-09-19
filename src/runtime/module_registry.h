#pragma once

#include <string>
#include <vector>

#include "runtime/value.h"

// The realm's MODULE REGISTRY: the module namespace object each already-
// evaluated module graph left behind, keyed by the module file's canonical
// path.
//
// It exists because a bronze compilation unit is a whole flattened graph. The
// linker merges every file it reached into one program and renames each file's
// module-level bindings into one namespace (`modules/link.cpp`), so "a module"
// has no runtime existence at all — its top level is ordinary top-level
// declarations of the image it was compiled into. Compile the same file into a
// SECOND image and its statements run a second time, in slots the first image
// cannot see. That is right for two independent programs and wrong for two
// compilations that share a realm: a host that compiles a page and then
// compiles a test script against it means one instance of the page's modules,
// the way one module map per context meant it.
//
// So a unit PUBLISHES a namespace for each file it evaluated, and a later unit
// compiled in the same realm treats a path already published as EXTERNAL: it
// parses the file for its export names, emits no statements for it, and binds
// each export from the registry instead (`ModuleOptions::moduleRegistry`).
//
// Per REALM and not per process, because a realm is the boundary a module map
// has in the language: an iframe's document is a different set of module
// instances from its parent's, and the two must not answer each other's
// imports.
//
// What the registry holds is the namespace EXOTIC object (10.4.6), which is
// what makes an export read through it a read of the exporting module's own
// binding rather than of a copy — the namespace's members are getters over the
// canonical slots.

namespace bronze::runtime {

// Publish `ns` under `path` in the current realm, replacing any earlier entry.
// Replacement rather than a refusal: a host that reloads an app compiles the
// page again, and the new instance is the one the next unit must see.
void rtModulePublish(const std::string& path, Value ns);

// The namespace published for `path` in the current realm, or false when the
// realm has none. False is not "an empty module" — it is the answer that makes
// the caller compile the file instead.
bool rtModuleLookup(const std::string& path, Value& out);

// Every published path in the current realm, in publication order. This is
// what a compiler consults to decide which specifiers resolve to an instance
// that already exists, so it is a list of keys and never of values.
std::vector<std::string> rtModuleRegistryPaths();

// The two intrinsics the linker's generated source calls. Named with the
// `__bronze_` prefix every synthetic identifier uses, resolved by the builtin
// ladder (`rtResolveBuiltinGlobal`) and admitted by lowering
// (`Lowerer::isProvidedGlobal`) so that a generated call lowers to `global.get`
// like any other builtin.
//
// `publish(path, ns)` returns undefined; `lookup(path)` throws a catchable
// TypeError for a path the realm never published, which is a compiler bug
// rather than a program's doing and so is reported rather than fatal.
Value rtModuleRegistryIntrinsic(const std::string& name);

}  // namespace bronze::runtime
