# 0010 — Inference: proving types and layouts

Status: designed 2026-08-10. This is phase 3 of docs/0001, and the reason
bronze exists. Everything shipped through docs/0009 built the `dynamic`
fallback well; this doc is where it stops being the substrate.

## The hole this closes

0001 decision 4 commits to "shape/type INFERENCE produces struct layouts
and typed IL ops; TS annotations are untrusted hints". `architecture.md`
draws a `types` box between AST and IL. Neither exists. What `src/lower`
does today is *syntactic typing*: an annotation is mapped straight to an
IL type and believed, an unannotated parameter is `Dynamic`, and a binary
`+` unboxes both sides to f64 and reboxes the result. There is no
analysis, so:

- every user function's parameters are `Dynamic`, so every call boxes;
- every property access is a call into `bronze_prop_get`, whose IC hit
  path is cheap but whose *call* is not;
- an annotation is **trusted**, which 0001 explicitly forbids —
  `function f(x: number)` reached with a string unboxes a string pointer
  as a double today. That is the one live unsoundness in the compiler.

The 0002 log has pointed at this doc four times: `property_access` at
~0.15µs/iteration is two helper calls and two boxes that inference is
supposed to delete.

## Decision 1 — inference runs on the AST, before lowering

A new module `src/types` consumes the AST and produces a **side table**
that lowering reads. It does not mutate the AST and it does not rewrite
IL.

Rejected: inferring on the IL after lowering everything to `Dynamic`.
SSA is the friendlier substrate for dataflow, but it would mean lowering
emits a boxed program and a second pass rewrites it — two lowering paths
to keep honest, and the IL rewrite has to reconstruct source-level facts
(which object literal, which annotation) that the AST still has. The
architecture diagram already puts types between AST and IL; this keeps
one lowering path, which is the one that has to stay correct.

The cost is that dataflow over loops needs explicit fixpoint iteration
rather than falling out of SSA. JS control flow is structured (0005), so
this is a worklist over a tree, not dominance-frontier machinery.
`src/lower/assigned_set.cpp` is the precedent.

## Decision 2 — the lattice, and no union types in v1

```
                    Dynamic  (⊤ — anything, the designed fallback)
   Number  Bool  String  Undefined  Null  Object(shape?)  Function(idx?)
                    Never    (⊥ — no value reaches here yet)
```

Join rules: `Never ⊔ t = t`; `t ⊔ t = t`; anything else is `Dynamic`.

**Unions are deliberately not modelled.** `number | undefined` — which a
`let` without an initialiser produces immediately — collapses to
`Dynamic`. Modelling unions means every consumer grows a case analysis
and every specialization grows a multi-way guard, for a win that is
unmeasured. The narrow common case (a `let` assigned exactly once before
any use) is handled by the flow analysis of decision 3 rather than by the
type, which is where it belongs.

`Object` carries an optional **shape class** (decision 4).
`Function` carries an optional module function index, which is what makes
direct typed calls possible (decision 5).

Falling back to `Dynamic` is always sound and is never a diagnostic —
it is the designed fallback, not a silent lie (contrast the house rule on
silent fallbacks, which is about paths that change *semantics*). What is
diagnosed is a *contradiction*: decision 6.

## Decision 3 — flow-sensitive within a function, per binding

The analysis walks statements in order carrying an environment
`name -> Type`, exactly mirroring lowering's own scope walk:

- **if/else**: analyse both arms from a snapshot, join at the merge.
- **loops**: analyse the body to fixpoint — seed the header with the
  entry environment, re-analyse until the environment stops changing.
  Bounded by lattice height (3), so at most two extra passes.
- **env-backed variables** (docs/0007 captures) are analysed as a single
  cell over the whole function, joined across every write anywhere,
  because a closure can write them at any time. Flow-sensitivity would
  be unsound; this is the conservative answer and it is what escape
  analysis later sharpens.

A binding's type is therefore per-program-point, not per-name, and
lowering asks about a *use site*, not a name.

## Decision 4 — shape classes: compile-time object identity

Every object-creating site (an object literal, a `new F()`) gets a
**shape class**: an ordered list of property names, plus the constructor
identity for `new`. Two sites with the same ordered list and same
prototype source share a class, mirroring the runtime's transition tree
(docs/0004 decision 2), so a literal written twice does not produce two
classes — the same reasoning that gave every `{}` one root shape in
docs/0008 decision 1.

A property access whose receiver has a known shape class, and whose
property is at a known index in that class, is **monomorphic by proof**.
That is the entire input decision 7 needs.

A receiver whose class is unknown, or which joins two different classes,
is `Object` with no class — the access stays a helper call. Polymorphic
sites are not specialized in v1 and this is a named non-goal, not an
oversight: 0004 says ICs are the midpoint and warns against gold-plating
them.

## Decision 5 — interprocedural: signature specialization over the call graph

A module-level function whose name is **only ever called directly** —
never read as a value, never passed as an argument, never stored on an
object, never closed over — has no unknown callers. For those, and only
those, parameter and return types are inferred by joining across all call
sites, to fixpoint over the call graph.

The escape test is a single pre-walk over the AST looking for the name in
any position but callee-of-a-call. It is deliberately blunt: one escaping
reference and the function keeps the uniform dynamic convention. This is
what deletes `fib`'s boxing — every `fib` call site passes a number, so
the parameter and the return become `f64`, and the recursive call becomes
a direct typed call.

Functions that need an environment (`needsEnv`, docs/0007) are excluded
by construction: 0007 already records that they are never direct-call
targets.

Recursion is handled by the fixpoint: a function's signature starts at
`Never` and only widens, so a self-call reads the current estimate and
the iteration converges.

## Decision 6 — annotations are hints, and a contradiction is diagnosed

This is the hint-trust/verify policy 0001 promised.

An annotation **seeds** the lattice at the declaration and constrains
nothing. Inference proceeds independently. Then:

- inference proves the same type → the annotation was free information,
  and the typed path is taken because the *proof* allows it;
- inference proves something else, or cannot prove anything → **the
  annotation is discarded**, the value stays `Dynamic`, and a warning
  names the annotation and what was actually seen.

The annotation therefore never widens what bronze believes; it can only
agree with a proof or be thrown away. This closes the live unsoundness:
`function f(x: number)` called with a string is now a warning plus a
correct dynamic path, not an unboxed string pointer.

Warnings, not errors: wild JS with wrong annotations must still compile
(0001 decision 4). A future `--strict-hints` can promote them.

## Decision 7 — the IC table moves into generated code

Today `bronze_prop_get(obj, keyIndex, icIndex)` indexes a runtime
`std::vector<InlineCache>`. A vector can reallocate, so generated code
cannot hold a pointer into it, so the IC check can only happen *inside*
the helper — and the call is most of the cost.

The table becomes a **global array in the generated object file**, sized
at compile time from the site count the lowering already assigns. Then:

- generated code gets a stable address per site and inlines the check:
  load the receiver's shape word, compare against `ic->cached_shape`, and
  on a hit load the slot directly. On a miss, or a non-object receiver,
  call the helper, which fills the entry as it does today.
- the helper's third parameter becomes the entry pointer rather than an
  index (`BRONZE_ABI_PU64`), which also deletes the bounds check and the
  lazy `resize` from the hot path.

Inference's contribution is knowing *when to emit the inline form at
all*: a site proven monomorphic (decision 4) gets it; an unproven site
keeps the plain call, so the inline path never becomes a polymorphic
guard chain in generated code.

The guard stays even when the shape class is proven, because the proof is
over *this* compilation's source and the shape word is the runtime's
authority. Deleting the guard needs escape analysis proving the object
never reaches unknown code, which is not in this phase and is named as
such below.

## Decision 8 — canonical dump and a disable switch

`bronze types <file>` prints the inferred types in a deterministic text
form — per function: signature, whether it is direct-callable, then per
statement the bindings whose type changed. Unit tests pin the bytes, like
every other stage (0001's carried-over discipline).

`--no-infer` forces every inferred type to `Dynamic`, reproducing today's
behaviour exactly. It is the bisection seam for any miscompile that
inference is suspected of causing, and the oracle suite must pass with it
both on and off — which makes it a ratchet, not a comfort blanket.

## Order of work

Each step builds, passes its own module's tests, and is committed before
the next starts.

1. **`src/types`** — lattice, flow analysis, shape classes, call graph,
   the dump, `bronze types`, unit tests. Consumed by nothing yet.
2. **Decompose `src/lower/lower.cpp`** (2,780 lines, over the 2k limit)
   into files under 1k. Pure refactor: IL output byte-identical.
3. **Numeric + direct calls** — lowering consumes decisions 3 and 5.
4. **Property access** — decisions 4 and 7, including the ABI change.
5. **Hints, `--no-infer`, oracle cases, bench log** — decisions 6 and 8.

## Named hard errors

Inference itself has none: every failure to prove is a sound fallback to
`Dynamic`. The diagnostics it adds are:

- `warning: annotation '<t>' on '<name>' is not provable; ignoring
  (inferred: <t2>)` — decision 6.
- `warning: annotation '<t>' on '<name>' contradicts inferred <t2>` —
  decision 6, the contradiction case.

## Not here, and named as such

- **Escape analysis**, and therefore guard-free property access and
  stack-allocated environments. Named in 0004 and 0007 as inference's
  job; it is the step after this one.
- **Union types** (decision 2), and therefore polymorphic-site
  specialization (decision 4).
- **Int32 specialization.** 0004 reserved the tag and said the fast path
  lands when a benchmark demands it. No benchmark demands it yet.
- **Cross-module inference.** bronze has no modules (0001 phase 4).
- **Typed array element access lowering to raw loads**, which 0004 calls
  a headline perf target. It needs the view's element type proven, which
  this doc's lattice can carry, but the lowering is its own work.
- **Deoptimization.** There is none and there must be none: every
  specialization here is guarded or proven, never speculative.
