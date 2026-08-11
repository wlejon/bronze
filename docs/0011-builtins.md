# 0011 — Builtins: the globals and the prototype methods

Status: designed and landing 2026-08-11. This is the first half of phase 4
of docs/0001 ("language growth"), and it is the reason three.js cannot begin
today: `Math.sqrt(9)` is `undefined variable: Math`, and every real member
of `Array.prototype` and `String.prototype` is a hard error naming itself.

docs/0003 already wrote the spec for this work. When phases 2 and 3 emptied
`cases/blocked/`, it was re-seeded on 2026-08-11 with exactly three cases —
`math_builtin`, `array_methods`, `string_methods` — each with a committed
`.expected` derived from ECMA-262. Those three files are what this doc has
to make pass, and the promote-on-pass ratchet is what says when it did.

## Decision 1 — a provided global is resolved by NAME, at compile time

bronze has no global object, and docs/0009 decision 2 already made the call
for `Object`: the name is recognized by lowering rather than looked up.
`Math` follows it, with one difference — `Object.keys` is recognized at the
CALL, so `Object` cannot be read as a value, while `Math` is a real object
that a program may hold (`const m = Math`, `arr.map(Math.sqrt)`).

So the resolution happens where the free identifier is read: lowering keeps
a closed list (`Lowerer::isProvidedGlobal`) and emits `global.get "<name>"`
for a name on it. Everything else keeps the compile error it has always
had. That is the whole point of the list being closed and checked at
compile time — a global bronze does not provide must never become a runtime
`undefined` that a program feature-tests its way around.

Shadowing falls out of where the check sits: the global path is the LAST
resort in identifier lowering, after locals, after the environment chain,
after module functions. `const Math = {...}` therefore wins, with no
special case (`activeVarMap_` is not consulted a second time — a second copy
of that rule is a copy that can drift, docs/0010's `export` lesson).

`bronze_global_get(keyIndex)` is one new ABI entry. It caches per key index
behind a root source, because every mention of `Math` in the source is one
of these calls — including one inside a loop — and a string compare per
reference is not a thing to leave in a hot path.

## Decision 2 — a builtin is an ordinary function object

Every `Math` member is a `FunctionHeader` over a native code pointer, the
same representation `charCodeAt` already used, deduplicated by
`bronze_function_singleton`. Nothing about the call path is special-cased:
`Math.min` is read with the ordinary property machinery (its own root shape,
so its sites do not fight `{}` literals for cache entries) and called
through the ordinary dynamic convention.

The one subtlety is arity. `FunctionHeader::arity` is the count a short call
is PADDED to with `undefined`, so a variadic builtin must declare 0 — with
arity 2, `Math.min()` would reach the builtin as two undefineds and answer
`NaN` where the language says `Infinity`. Pinned by the oracle case.

## Decision 3 — a member bronze has not built stays LOUD

rt_helpers.cpp already carries a table per prototype of names that ECMA-262
says exist and bronze has not implemented; reading one is a hard error
naming it, never `undefined`. A namespace object is an ordinary object, so
its misses need the same treatment, and `Math` gets its own table
(`Math.random`, `Math.fround`, `Math.imul`, the hyperbolics, …). The check
runs only on the miss, so the hit path is untouched.

`Math.random` is on that list deliberately and not for want of a call to a
PRNG: bronze has no decision about seeding, and a nondeterministic global
wants one before it can be defended (docs/0001 decision 10).

As a member lands, its name leaves the table. Membership is always the
ECMA-262 question "does this exist?", never "have we got round to it?".

## Decision 4 — the numeric fast path is inference's, and it is not here yet

`Math.sqrt(x)` today is: resolve the global (cached), read a property (IC
hit), then a dynamic call into a native trampoline that does `ToNumber` and
boxes a double. NaN-boxing makes the boxing itself free, but the call is
not, and V8 turns `Math.sqrt` into one instruction.

The fix has the shape docs/0010 decision 7 already used for property reads:
where inference PROVES the arguments are numbers, lowering emits the
operation inline instead of a call, and where it proves nothing, the call
stays. `sqrt`, `abs`, `floor`, `ceil`, `trunc` and `pow` map exactly onto
LLVM intrinsics; `round`, `sign`, `min`, `max` and `hypot` deliberately do
not (JS rounds half towards +Infinity, and JS min/max order -0 below +0 and
propagate NaN, which `llvm.minnum` does not) — those keep the call until
each is written out longhand and pinned.

**Not built in this step, and named as such.** The two paths must agree on
every edge case, which is exactly what the oracle's with-inference and
`--no-infer` runs would compare; until the fast path exists, both runs take
the same route and the comparison proves nothing about it. A bench case
(`bench/math_loop.js`) and a docs/0002 log entry land with it, not before.

## Order of work

1. **`Math`** — decisions 1–3, `math_builtin` promoted. *(done)*
2. **`Array.prototype`** — the members `array_methods` names, plus the
   callback-taking ones (`map`, `filter`, `reduce`, `forEach`), which reach
   user code back through `bronze_dynamic_call`.
3. **`String.prototype`** — the members `string_methods` names. Strings are
   immutable (docs/0004), so every one of them allocates a fresh string and
   none can work in place.

## Named diagnostics

- `undefined variable: <name>` — unchanged, and now the answer for any free
  name that is not on the provided-globals list.
- `unsupported: Math.<name> is not implemented` — decision 3.
- `internal: no global named <name>` — a `global.get` for a name the runtime
  cannot resolve. Lowering's list is what prevents it, so reaching it is a
  drift between the two lists, not a program error.
