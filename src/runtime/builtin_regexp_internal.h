#pragma once

#include <cstdint>
#include <string>

#include "runtime/fn.h"
#include "runtime/rt_builtins.h"
#include "runtime/value.h"

// What the RegExp translation units share and nothing else may see:
// builtin_regexp.cpp (the constructor, `RegExp.prototype` and the flag
// accessors), builtin_regexp_exec.cpp (the compiled-pattern table, `exec`,
// the match array and the `lastIndex` protocol), builtin_regexp_escape.cpp
// (`RegExp.escape`) and builtin_regexp_symbols.cpp (the five symbol-keyed
// members, which reach the matcher through the public `rtRegExpPattern`).

namespace bronze::runtime {

// 22.2.3.3 RegExpInitialize on a header `rtAllocateNativeBaseInstance` made:
// the source and the flags, compiled, with `lastIndex` at zero. False with
// the exception cell set when the pattern does not compile. `sourceStr` is
// REWRITTEN in place when 22.2.6.13.1 has to escape it.
bool rtInitializeRegExp(Rooted<Value>& re, Rooted<Value>& sourceStr, const std::string& flagsText);

// 22.2.3.1 + 22.2.3.3 with the intrinsic prototype: what a literal and a
// plain `RegExp(...)` call build. `undefined` with the exception cell set
// when the pattern does not compile.
Value rtMakeRegExp(Rooted<Value>& sourceStr, const std::string& flagsText);

// The native bodies builtin_regexp.cpp installs and the other files own.
uint64_t rtRegExpExecBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t rtRegExpEscapeBody(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
// 22.2.6.8, .9, .11, .12, .14 as own data properties of `RegExp.prototype`,
// in the clause's order.
void rtInstallRegExpSymbolMethods(Rooted<Value>& proto);

// Cache inspection for testing.
size_t rtRegExpCacheSize();

}  // namespace bronze::runtime
