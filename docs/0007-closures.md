# 0007 — Closures and environment records

Status: designed and implemented 2026-08-10. Implements the "Functions and
closures" section of docs/0004, which accepted GC-allocated environment
records but left them unbuilt: `FunctionHeader::env_record` existed as an
unused `void*`, and a nested function referencing an enclosing variable was
`error: undefined variable`.

## Decision 1 — captured variables live in the environment, not in SSA

JS closures capture **variables, not values**: a closure sees later writes
by the enclosing function, and the enclosing function sees the closure's
writes. Copying a value into the closure at creation time is therefore
wrong for anything mutable, and `makeCounter` is not an exotic case.

So a variable that any nested function references stops being an SSA value
and becomes a slot in an environment record, for its whole lifetime:

- reads lower to `env.get`, writes to `env.set`, in the *declaring*
  function as much as in the closure;
- it takes no part in SSA joins — it is memory, so if/else and loop
  parameter lists skip it entirely.

Which variables those are comes from a pre-pass over the function body that
collects every name referenced by any nested function, transitively. The
set is an over-approximation (a same-named variable in an unrelated scope
is env-backed too); that costs a little speed and no correctness, and
inference's escape analysis is what eventually narrows it (0004).

## Decision 2 — one environment per *scope*, not per function

The unit is the block scope that declares the captured variable, not the
function activation:

```js
while (i < 3) {
  const k = i * 10;
  fns[i] = function () { return k; };   // three closures, three k's
  i = i + 1;
}
```

`const k` is a fresh binding on every iteration, so per-function
environments would give all three closures the same `k`. A scope that
declares at least one captured name creates its environment on entry —
which for a loop body means once per iteration, exactly matching the
language. Environments chain to the enclosing environment through a
`parent` slot, so a reference resolves to a statically known
`(depth, index)` pair.

Deferred, as a named hard error: capturing a `for (let i = ...)` **header**
binding. Its per-iteration copy semantics need the loop to thread the
environment across the back edge, which the block-scope rule does not give
for free. `while` loops with a block-scoped binding — the common shape —
work.

## Decision 3 — the environment reaches the callee through the calling convention

A closure's code is shared by every instance of it; only the environment
differs. Generated code therefore has to receive the environment at entry,
and the uniform dynamic calling convention (0004) had no room for it:

```c
/* before */ Value fn(Value thisArg, uint32_t argc, Value* argv);
/* after  */ Value fn(Value env, Value thisArg, uint32_t argc, Value* argv);
```

`bronze_dynamic_call` passes the callee's `FunctionHeader::env_record`,
which becomes a `Value` (it was a raw `void*`, invisible to the collector —
the generic payload scan would never have forwarded it).

- **Rejected: passing the callee function object** instead of the
  environment. It gives the callee its own identity too, which
  `arguments.callee`-style features would want, but bronze has none of
  them, and it costs a load per call to reach the environment.
- **Rejected: a per-call runtime global** holding the pending environment.
  No allocation, no ABI change — and it breaks the moment anything
  re-enters between the store and the load.

In the IL the environment is simply the function's first parameter when
`Function::needsEnv` is set, so it is an ordinary `dynamic` value: it gets
a GC root slot from 0006 like any other, with no special case.

A direct `call @f` never targets a function that needs an environment —
nested function declarations desugar to closure values bound to a name, so
every call to them is a dynamic call through the value. The verifier
enforces this rather than trusting it.

## Decision 4 — nested function declarations desugar to closures

`function inner() {...}` inside a function body becomes, in effect,
`const inner = function inner() {...}` hoisted to the top of the enclosing
body — so it is visible to statements before it, as JS requires, and so
there is exactly one code path for closures. Before this, a nested
declaration hit `error: unsupported AST node`, which named nothing and
violated the house rule about diagnosing constructs by name.

## Environment record layout

A GC object like any other, so the collector's generic payload scan
forwards it with no special case:

```
EnvHeader { HeapObjectHeader header; Value parent; Value slots[n]; }
```

`parent` is `undefined` at the outermost environment. `header.flags`
distinguishes it from objects/arrays/functions/views, so a mis-typed
`env.get` is caught rather than reinterpreted.

## What shipped, and what is deliberately not here

Pinned as oracle cases (each also runs under `oracle-gc-stress`, so every
environment record survives a collection at every allocation site):

- `closure_capture` — a closure reading an enclosing parameter
- `closure_counter` — two closures sharing one mutable binding
- `closure_nested` — two levels of nesting, reaching past the immediate parent
- `closure_loop` — a per-iteration block binding, captured once per iteration
- `closure_recursive` — a nested function calling itself, and two nested
  functions calling each other (the second one references the first before
  it is declared, which works because slots are allocated on scope entry)

Deferred, each a hard error naming itself rather than a silent miscompile:

- **`for (let i = ...)` header capture.** The loop *body*'s bindings are
  already per-iteration (decision 2); the loop *variable* is not, and
  getting it right means copying the header binding into a fresh
  environment per iteration. Diagnosed by name until then.
- **Narrowing the capture set.** `getCapturedNames` is an
  over-approximation: it includes a nested function's own locals, so an
  unrelated same-named variable in the enclosing scope is env-backed too.
  That costs speed, never correctness — narrowing it is escape analysis's
  job (docs/0004).
