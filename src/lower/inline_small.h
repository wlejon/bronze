#pragma once

#include <cstdint>
#include <vector>

#include "il/il.h"
#include "lower/direct_method_table.h"

namespace bronze::lower {

// Returns true when the module contains no dynamic prototype mutation
// (`Object.setPrototypeOf`, `Reflect`, `Proxy`, `eval`, `with`, non-literal
// `__proto__` writes, or escaping `.prototype` references).
bool isModulePrototypePatchFree(const il::Module& module);

// Returns a bitmask indexed by `module.keyConstants` marking every property key
// that is assigned, defined, deleted, or installed as an accessor in any live
// function in the module.
std::vector<uint8_t> computeMutatedMethodKeyMask(const il::Module& module);

// For each key in `module.keyConstants`, returns the closed set of module
// function indices that can ever be stored under that key (via `MethodDef` or
// `PropSet`), or an empty vector if the key's function targets are not closed.
std::vector<std::vector<uint32_t>> computeClosedKeyTargets(const il::Module& module);

// Returns true when `inst` is a `MethodCall` with a resolved `directTarget`
// whose method key is never mutated and whose receiver class hierarchy has no
// subclass override in `directMethods`.
bool isSealedDirectMethodCall(const il::Instruction& inst, const il::Module& module,
                              const DirectMethodTable* directMethods, bool protoPatchFree,
                              const std::vector<uint8_t>& isMutatedMethodKey);

// Inlines small non-capturing helper functions (`Op::Call` and sealed
// `Op::MethodCall`) into functions containing loops.
bool inlineSmallFunctions(il::Module& module, const DirectMethodTable* directMethods = nullptr);

}  // namespace bronze::lower
