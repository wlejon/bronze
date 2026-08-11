# 0005 — Control flow: blocks, branches, and SSA joins in the IL

Status: implemented. Pulls the control-flow item of 0001 phase 4 forward: it
gates real benchmarks, real libraries, and inference (docs/0010) alike. The
canonical text form below is a ratchet surface — settled here, never bent.

## Decision 1 — join representation: block arguments, not phi instructions

The IL becomes multi-block; SSA joins are expressed as **block
parameters** (Cranelift/MLIR/SIL style): a block declares typed
parameters, and every branch to it passes arguments.

- Rejected: LLVM-style phi instructions. Phis impose ordering rules
  ("phis before all other instructions"), reference predecessors from
  inside the successor, and complicate the verifier and the printer.
  Block arguments carry the same information on the edge where it
  belongs, and lower to LLVM phis mechanically.
- Rejected: mutable local slots + load/store, letting LLVM's mem2reg
  build SSA. It would work and is less frontend effort, but 0001
  decision 5 commits to an SSA IL for a reason: inference (bronze's
  entire point) wants one def per value. Building the memory form now
  means rebuilding the IL later.

## Decision 2 — SSA construction: structured joins over assigned-sets

JS source has structured control flow only, so lowering never needs
general SSA construction (no Braun/dominance-frontier machinery):

- Lowering keeps an environment `name -> current ValueId` in scope
  order.
- **if/else**: lower both arms from a snapshot; the join block takes one
  parameter per variable whose binding differs between (or was assigned
  in either of) the arms.
- **Loops**: the header block takes one parameter per variable assigned
  anywhere in the loop body (computed by a small AST pre-walk). The
  entry edge passes current values, the back edge passes updated ones.
  The exit block takes the same parameter set (break edges and the
  header's exit edge may disagree on values).
- **Parameter order is source declaration order of the variable** —
  never map iteration order (determinism rule, 0001 decision 10).

This is deliberately non-minimal SSA: a variable assigned to the same
value on every edge still becomes a parameter. Harmless — LLVM folds
redundant phis at -O2 — and it keeps the construction simple and the
output deterministic. Minimality is LLVM's job, not the frontend's.

## Decision 3 — canonical text form (the ratchet)

Blocks are named `b0..bN` in creation order; `b0` is the entry and has
no parameters (function parameters are `%0..%k-1` as today). Value
numbering stays function-wide; block parameters draw fresh ids from the
same counter. Empty parameter/argument lists omit the parentheses.

```
func count_to(%0: f64) -> f64 {
  b0:
    %1: f64 = const.f64 0
    jump b1(%1)
  b1(%2: f64):
    %3: bool = cmp.lt %2, %0
    br %3, b2, b3
  b2:
    %4: f64 = const.f64 1
    %5: f64 = add %2, %4
    jump b1(%5)
  b3:
    ret %2
}
```

Terminators (exactly one per block, always last):

- `jump bN(args...)`
- `br %cond, bThen(args...), bElse(args...)`
- `ret [%v]`

`br` with identical then/else targets is a verifier error: an LLVM phi
takes one incoming value per predecessor *block*, not per edge, so the
two argument lists could not both be honored. Lowering inserts an empty
forwarding block in the (rare) case it would otherwise need this.

## Decision 4 — truthiness

Condition contexts (`if`, `while`, `for`, `&&`, `||`, `?:`, `!`) need
JS ToBoolean, which is not `!= 0`:

- **f64 condition**: lower inline to `fcmp one x, 0.0` (ordered,
  not-equal) — false for +0, -0, and NaN in one instruction, no helper.
- **bool condition**: as-is.
- **dynamic condition**: new ABI helper `bronze_truthy(u64) -> bool`
  (one registry line in `src/abi/bronze_abi.h`): false for undefined,
  null, false, ±0, NaN, and the empty string; true otherwise — note
  `"0"` and empty objects/arrays are true.

`&&` / `||` / `??` are value-producing control flow (JS returns an
*operand*, not a boolean) and lower to a branch with a join-block
parameter. When both operands are provably f64 the join parameter is
f64; otherwise both edges box and the join is dynamic. `0 ?? x` must
return `0` — broc shipped the opposite (0000); the oracle case for it
is non-negotiable.

## In scope / explicitly deferred

In scope: `if`/`else`, `while`, `do-while`, three-part `for`, unlabeled
`break`/`continue`, `?:`, `&&`, `||`, `??`, `!`, plain assignment and
compound assignment (`+=` etc.) as expressions, `let`/`const` block
scoping, `var` function-scope hoisting.

Deferred, each a named hard error at lowering: `for-in`, `for-of`
(need enumeration order / iterator protocol), `switch`, labeled
break/continue, `throw`/`try` (exceptions are their own design), and
use of a `let`/`const` binding before initialization that lowering
cannot prove safe (TDZ is a runtime ReferenceError in node; bronze
diagnoses the construct by name until exceptions exist).

## Verifier (new, `src/il`)

Runs before printing and before codegen; failure is a hard error
carrying the IL dump. Checks: exactly one terminator per block,
nothing after it; branch targets in range; argument count and types
match target block parameters; every ValueId has exactly one definition;
within-block use-after-def. Full dominance-based use checking is
deferred — structured lowering cannot produce a violation the above
misses, and the check slots in cleanly later if a non-structured
producer ever appears.

## Codegen mapping (mechanical)

Two passes per function: create all `llvm::BasicBlock`s, then emit.
Block parameters become phis at the top of the block; each incoming
edge's arguments are added to the phis when the predecessor's
terminator is emitted. `jump`/`br` map to `CreateBr`/`CreateCondBr`.
`verifyModule` stays on.

## Order of work (oracle cases first, per 0003)

1. Oracle cases into `cases/blocked/`: loop counting/accumulation,
   nested loops, `break`/`continue`, `do-while`, if/else chains, the
   truthiness table (`0`, `NaN`, `""`, `"0"`, `undefined`, `null`,
   `{}`, `[]`), short-circuit *value* semantics (`0 || "x"`, `1 && 2`),
   `0 ?? 5`, ternary.
2. IL: `Block`, block parameters, `Jump`/`Branch` ops, printer for the
   canonical form above, verifier. Existing single-block functions
   become a lone `b0` (il/lower/codegen unit expectations updated once,
   in the same commit).
3. Lowering: scoped environment, assigned-set pre-walk, the constructs
   in scope; `bronze_truthy` registry line + implementation.
4. Codegen mapping; promote the blocked cases as they pass.
5. **Rewrite all three benchmarks as real loops** and re-log in 0002.
   The current numbers are straight-line unrolled code that LLVM can
   constant-fold, so they largely measure process startup; the loop
   numbers replace them as the honest baseline, whatever they say.
