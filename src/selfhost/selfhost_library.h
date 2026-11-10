#pragma once

#include <cstdint>
#include <string_view>

#include "lower/lower.h"

namespace bronze::selfhost {

// The self-hosted builtins' library (README.md): builtins.js's text, compiled
// into the compiler, so the one source has one meaning wherever it is
// compiled: the prebuilt object (bronze_stage0, selfhost_build.cpp) and the
// copy an ahead-of-time program compiles in (lower.h SelfHostLibrary).
std::string_view librarySource();

// FNV-1a over the text: what a key of compiled output that may hold the
// library's code includes (eval/code_cache.cpp moduleKeyMaterial).
uint64_t libraryDigest();

// The library lowered for a program to compile in, once per process. Every
// caller of lower::lowerAheadOfTime passes it (AheadOfTimeOptions::
// selfHostLibrary). The text is the compiler's own, so a failure is a bug in
// the compiler and is fatal.
const lower::SelfHostLibrary* library();

}  // namespace bronze::selfhost
