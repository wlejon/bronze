#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "il/il.h"
#include "support/diagnostics.h"
#include "types/pins.h"
#include "types/result.h"

namespace bronze::lower {

// The AST -> IL pass. `inference` is the side table inference produces; it is
// nullable, and passing null reproduces the pre-inference calling convention
// exactly — every value `Dynamic`, every user function reached the uniform
// dynamic way. That is the `--no-infer` seam and the bisection tool for any
// miscompile inference is suspected of causing.
//
// It reproduces the pre-inference *convention*, not the pre-inference bugs: an
// unproven answer is the dynamic path in BOTH modes, so the seam compares two
// correct programs. Two such sites: `s += "b"` on a local, and every TS
// annotation — which types nothing without a proof, so with no inference result
// it types nothing at all. The warnings that normally name a discarded
// annotation are silent here: with nothing provable they would fire on every
// annotation in the file and report only which switch is on. Unreadable
// annotation text is still the hard error it is in the other mode — this is a
// bisection seam, not a laxer compiler.
// `hostGlobals` is the `--host-globals` manifest, already read and validated
// by the CLI: identifiers an EMBEDDING HOST promises to register with the
// runtime before the program runs. Each joins the provided-globals set, so a
// read lowers to the same `global.get` a builtin does instead of the
// unresolved-name warning and runtime ReferenceError. Nullable like
// `inference`, and independent of it — which set of names resolves is a
// lowering-level fact, so `--no-infer` changes nothing about it.
// `assumeNoBigInt` is the embedder's promise, made with --assume-no-bigint,
// that no BigInt will reach an arithmetic operator in this program — including
// through channels the compiler cannot see: an exported function a host calls,
// or a host global's return value. It is the same KIND of claim
// `--host-globals` makes (the host asserts a boundary the compiler cannot
// inspect), and like that one it is a promise, not a proof.
//
// What it buys: `*`, `-`, `/` and `%` over a boxed operand may produce an f64
// instead of a boxed value, because with no BigInt in reach ToNumeric IS
// ToNumber. An f64 result is not a GC root, and `planRootFrame` roots Dynamic
// values and only those — so a chain of arithmetic keeps its intermediates in
// registers instead of storing and reloading every one of them around every
// instruction.
//
// The promise is checked as far as it can be: the flag only takes effect if
// `bigIntMayReach` also finds nothing in the program's own text, so an
// embedder that passes it over a program full of BigInts gets the safe
// lowering rather than a miscompile.
class InferStatsCollector;

// `pins` is the `--pins` manifest, or null. Lowering reads only its ENV-SLOT
// entries: a captured binding has no name inference can key a side table on —
// it has a (function, binding) pair and a record layout this pass invents — so
// that half of the manifest is consumed here rather than there. The field
// entries reach lowering already spent, as `provenFieldReads` and
// `nullishNumberFieldReads` on the inference result.
std::optional<il::Module> lowerModule(const ast::Module& astModule, DiagnosticSink& diags,
                                      const types::InferenceResult* inference = nullptr,
                                      const std::vector<std::string>* hostGlobals = nullptr,
                                      const SourceSet* sources = nullptr,
                                      InferStatsCollector* stats = nullptr,
                                      bool assumeNoBigInt = false,
                                      const types::PinManifest* pins = nullptr);

}  // namespace bronze::lower
