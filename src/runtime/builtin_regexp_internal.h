#pragma once

#include <cstdint>
#include <string>

#include "runtime/fn.h"
#include "runtime/rt_builtins.h"
#include "runtime/value.h"

// What the RegExp translation units share and nothing else may see:
// builtin_regexp.cpp (the constructor, the member tables and the flag
// accessors), builtin_regexp_exec.cpp (the compiled-pattern table, `exec`,
// the match array and the `lastIndex` protocol) and builtin_regexp_escape.cpp
// (`RegExp.escape`). The symbol-keyed members are builtin_regexp_symbols.cpp,
// which reaches the matcher through the public `rtRegExpPattern` alone.

namespace bronze::runtime {

// 22.2.3.1 step 5 onward: the source and the flags, compiled, with `lastIndex`
// at zero. `undefined` with the exception cell set when the pattern does not
// compile. `sourceStr` is REWRITTEN in place when 22.2.6.10 has to escape it.
Value rtMakeRegExp(Rooted<Value>& sourceStr, const std::string& flagsText);

// The native bodies the tables in builtin_regexp.cpp name and the other two
// files own.
uint64_t rtRegExpExecBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtRegExpEscapeBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

}  // namespace bronze::runtime
