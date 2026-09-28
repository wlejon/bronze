#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "il/il.h"

// The IL as bytes, for the on-disk code cache (src/eval/code_cache.h).
//
// A binary encoding of `Module` that decodes back to a module equal to the one
// encoded, field for field — except the per-file source TEXTS and line tables,
// which are never written: the cache re-reads every source file to validate it
// anyway, so a cached module carries only the file names and the reader rebuilds
// both from the texts it was handed (`attachSources`).
//
// The decoder is bounds-checked throughout and fails with a message rather than
// reading past its input: a truncated or corrupt file is a cache miss, never a
// crash. It is not a validator of the IL's own invariants — the cache checksums
// what it wrote, and a checksum-valid payload is one this encoder produced.
//
// Adding a field to `Instruction`, `Block`, `Function` or `Module` means adding
// it here and bumping `kFormatVersion`; the layout guards in serialize.cpp stop
// a build that forgets the first half.
namespace bronze::il {

inline constexpr uint32_t kFormatVersion = 1;

std::string serializeModule(const Module& module);

// False with `err` set on any malformed input; `out` is then unspecified.
bool deserializeModule(std::string_view bytes, Module& out, std::string& err);

// Fills `sourceTexts` (when `retainTexts`) and `lineTables` from the program's
// file texts, in `sourceFiles` order — what lowering built them from.
void attachSources(Module& module, const std::vector<std::string_view>& texts, bool retainTexts);

}  // namespace bronze::il
