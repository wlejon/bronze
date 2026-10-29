#pragma once

#include <cstdint>
#include <vector>

#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze::lower {

// Whether a build compiles resolved method calls' bodies in place
// (inlineGuardedMethods). Only an image build: the JIT and the tiered engine
// may interpret a callee or replace its code, so the code word the guard
// compares is not a fixed fact there, and their frames are reported by the
// interpreters, which know nothing of inline frames.
struct InlineGuardedOptions {
    bool imageBuild = false;
};
bool inlineGuardedEnabled(const InlineGuardedOptions& options);

// Guarded method inlining (docs/il2mir.md, "Guarded method inlining"): each
// `method.call` in a loop (or anywhere in a function a loop enters by a real
// call, such as a method that walks a tree by calling itself on its children)
// whose site lowering resolved to one module function, and whose callee is
// small and self-contained, becomes
//
//     g = method.ic.is recv, <site>, @callee
//     br g, <the callee's finished body (its typed entry at a typed site),
//            its parameters bound to the call's operands>,
//           <the original method.call>
//
// joined after both. Copied instructions keep their spans and name an inline
// frame of the caller (il::Function::inlineFrames), so stack traces still
// show the callee. A failure of the IL verifier afterwards is a compiler bug
// and is reported to `diags`. Returns whether any call was inlined; the
// functions changed are appended to `changed` when it is given.
bool inlineGuardedMethods(il::Module& module, DiagnosticSink& diags,
                          std::vector<uint32_t>* changed = nullptr);

// The typed-entry fold (foldTypedEntry) over each function inlining changed.
// An inlined body reads its parameters as the call's operands, which at a
// numeric site are often `box.f64 %x`: the body's tag tests and checked
// unboxes of them answer themselves, as they do in a typed entry. A fold whose
// result fails the dominance check is dropped and the function kept as it
// was.
void foldInlinedCallers(il::Module& module, const std::vector<uint32_t>& changed);

}  // namespace bronze::lower
