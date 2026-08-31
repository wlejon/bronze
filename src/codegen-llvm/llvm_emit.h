#pragma once

#include <string>
#include <vector>

#include "support/diagnostics.h"

namespace llvm {
class Module;
}

namespace bronze::codegen_llvm {

// A finished LLVM module → object file(s) on disk: the O3 pipeline, the host
// target machine, and — above a size threshold — the parallel partition
// emission that llvm_partition.h plans. Everything before this point builds
// IR; nothing after it does.
//
// `pic` asks for position-independent code, and it is the one thing about this
// path that `--emit-shared` changes. bronze's ordinary output is NOT
// position-independent — which is why the driver links executables `-no-pie` on
// Linux and why tests/two_module links its host the same way — and on ELF
// x86-64 a non-PIC object simply cannot go into a shared object: GNU ld refuses
// the R_X86_64_32S relocations with "can not be used when making a shared
// object; recompile with -fPIC". A loadable module IS a shared object, so it is
// compiled as one. Mach-O is position-independent throughout and COFF has no
// such concept, so this is a no-op on both; asking for it uniformly under the
// shared flag is still right, because what it expresses is "this object is
// going into a library", which is true on all three.
//
// `emittedPaths` records what was actually written: one path on the
// single-object path, the partition objects in link order above the threshold.
// Null means the caller can only link one object, which also forbids the split.
bool writeObjectFile(llvm::Module& llvmModule, const std::string& outputPath, bool pic,
                     const std::string& entrySymbol,
                     std::vector<std::string>* emittedPaths, DiagnosticSink& diags);

// `BRONZE_XALIGN=<n>` parsed: the byte alignment every emitted function is
// given, or 0 for "leave every function at the target's default", which is what
// an unset variable means and what makes the unset build byte-identical.
//
// A measurement instrument, like `BRONZE_XPART_PAD`, and for a neighbouring
// reason. The same objects relinked in a different order move the compute
// region of a bundle benchmark by a tenth, and that spread is not noise — it is
// which hot bodies happen to collide in a set-associative instruction cache.
// Alignment QUANTIZES where a body can start, so it is the one lever that can
// change the VARIANCE of that placement rather than the mean, and a
// low-variance configuration is worth having even if its centre is worse: it is
// the bar an A/B is adjudicated against.
//
// Rejects anything that is not a power of two in [1, 4096] rather than rounding
// or ignoring it, because a seam that silently measured something other than
// what it was asked for is worse than no seam.
bool parseFunctionAlign(const char* value, unsigned& bytes, std::string& errOut);

// Give every definition in `m` `bytes`-byte alignment. A no-op at 0.
void alignEmittedFunctions(llvm::Module& m, unsigned bytes);

}  // namespace bronze::codegen_llvm
