# 0020 — Exceptions: the abrupt completion, and what it costs every call

Status: implemented. Part of phase 4 of docs/0001.

`throw`, `try`/`catch`/`finally`, and the `Error` constructors. Every chunk
before this one added a construct; this one adds an *edge* — a way for a
callee to leave a frame its caller has to finish unwinding — and every call
in generated code now sits on it. docs/0006 named the obligation and deferred
it:

> Unwinding. There are no exceptions (0005 defers `throw`/`try`), so a frame
> is popped by exactly one `ret` path. When exceptions land they must pop
> frames on the throw path, and that is that design's problem — named here so
> it cannot be forgotten.

That is decision 2 below. Getting it wrong does not produce a wrong answer;
it produces a heap that is quietly corrupt several statements later, which is
worse, because it is unattributable.

Before this chunk `try`, `catch`, `finally` and `throw` parsed into two
*empty* AST nodes — `TryStmt` and `ThrowStmt` carried no children at all, the
parser skipped past their blocks, every walker in the tree treated them as
opaque, and lowering rejected both by name. `cases/blocked/try_catch_throw.js`
held the hand-derived expectations; it is promoted here, with five new cases
pinning what it does not reach.

## Decision 1 — a pending-exception cell, not `invoke`/`landingpad`

Two mechanisms were sized. This chunk takes the second.

**Real unwinding.** LLVM `invoke` + `landingpad`, the Windows SEH personality
(`__CxxFrameHandler3` or `__C_specific_handler`), unwind tables in `.pdata` /
`.xdata`, and a `_CxxThrowException` at the throw site. Correct, and *free* on
the non-throwing path, which is the whole argument for it.

The cost is not the LLVM IR, which is a day's work. It is that the contract
stops being expressible in the ABI. `src/abi/bronze_abi.h` is pure C by
construction — `abi_check.c` compiles it as C to enforce that — and CLAUDE.md
forbids a C++ type in any signature generated code touches, because MSVC
returns a class with a user-defined constructor through a hidden `sret`
pointer and silently shifts every argument register (the 2026-08-10
dynamic-call crash). A thrown value under SEH is a C++ exception object with a
throw-info descriptor, a destructor and a copy constructor; a landing pad has
to name its type in a `__CxxFrameHandler3` funclet. None of that is
representable as `u64 in / u64 out`. It would not extend the ABI, it would
introduce a *second* ABI, one whose two halves (the runtime's `throw` and
generated code's `catch`) agree by hand rather than by the X-macro registry
that makes drift structurally impossible today. That registry is the reason
bronze has had exactly one calling-convention crash, and it is not something
to buy a fast path with.

Two smaller strikes, recorded because they were part of the sizing and not
the decision: funclet-based EH interacts badly with the inline GC root frame
(docs/0006 decision 1) — a funclet runs on the same stack but the frame chain
head has to be restored by *someone*, and under SEH that someone is a
cleanup pad LLVM generates and bronze does not control; and `oracle-gc-stress`
would be proving a mechanism whose Windows-specific half is the least
exercised path in the LLVM COFF backend.

**A pending-exception cell.** One `uint64_t` in the runtime, holding the
thrown value or the Hole singleton when nothing is pending. Generated code
tests it after every call that can throw and propagates by returning; the
returned value is garbage the caller never reads, because the caller's own
test fires first. Costs a global load, a compare and a not-taken branch per
throwing call site on the fast path.

It is taken for three reasons, in increasing order of weight:

- **It changes no signature.** `bronze_fn_code` is what it was. The ABI grows
  one data symbol and two functions. Nothing generated code touches acquires
  a C++ type.
- **It is self-evidently correct with respect to root frames.** The unwind
  path is a `ret`, and a `ret` already pops the frame — the code that does it
  is the code decision 2 of docs/0006 shipped, unchanged. There is no second
  way out of a function, so there is no second place the frame chain can be
  left wrong. With `invoke` there would be.
- **It is one mechanism on every platform.** The oracle suite proves it on
  Windows and the design does not have a second, unproven half waiting for
  the first Linux build.

The price is real and is stated here so it can be measured later: a global
load, a compare and a not-taken branch per throwing call site. It is NOT
measured yet — there is no benchmark harness in the tree, so putting a number
here would be inventing one. What can be said without measuring is that the
fast path is untouched for proven-f64 code, which has no helper calls to check
after: the same property that makes docs/0006's frames free for `fib`. The
first thing a benchmark chunk should do is put a number against this.

The cell is `bronze_exception_cell`, an ABI **global**, and the Hole tag
(`0xFFF7`, docs/0004 decision 1: "internal: array holes / TDZ; never
user-visible") is what "nothing pending" means. A separate boolean flag was
rejected: two words can disagree, and the singleton Hole payload is 0, so
"pending?" is a single 64-bit compare against a constant rather than a mask
and a shift.

The cell is registered with `Heap::add_permanent_root`. A thrown object is
live for exactly as long as it is pending, across an arbitrary number of
frames, and nothing else roots it.

That registration happens on **first use**, not at static initialization.
`rtHeap()` returns a namespace-scope object in another translation unit, and
registering into it from this one's static initializer is the initialization-
order fiasco: written that way first, the registration landed in a heap whose
constructor had not run, the constructor then default-constructed the root
tables over it, and the roots were silently gone. It survived every ordinary
run and crashed under `BRONZE_GC_STRESS=1` — which is the argument for that
mode existing. Every root a runtime file owns is registered from a function-
local static for this reason.

## Decision 2 — the unwind path is a `ret`, so frames pop themselves

Generated code, after any instruction that can throw:

```
%c = load i64 @bronze_exception_cell
%p = icmp ne i64 %c, 0xFFF7000000000000
br i1 %p, label %unwind_or_handler, label %continue
```

`%unwind_or_handler` is the enclosing `try`'s handler block if there is one
in this function, and otherwise a single per-function **unwind block** that
stores `frame->prev` back to `bronze_gc_frame_top` and returns. That store is
byte-for-byte the one `emitTerminator` already emits before every `ret`; the
unwind block is a `ret` like any other, and the collector cannot tell the two
apart. This is the property that made the mechanism worth its branch.

Two obligations fall out and are written down because nothing enforces them:

- **A helper that sets the cell must return the `undefined` bit pattern.**
  The caller stores its result into a GC root slot before it tests the cell,
  and the collector reads every slot of a linked frame. A helper that threw
  and returned a half-built pointer would put a value the collector cannot
  parse into a live root. `rtThrow` returns `Value::fromUndefined()` and every
  throwing helper is written `return rtThrowTypeError(...)` for that reason,
  not for tidiness.
- **The check follows the result store, not the call.** Same reason, from the
  other side: the slot must be valid before anything can branch away from it.

`main` is the one function with no caller to propagate to, so its unwind block
calls `bronze_uncaught_exception` and does not return. This is a property of
the FUNCTION — `il::Function::isEntryPoint`, set by lowering — and not a block
of IL, which is the second thing tried and the wrong one. Giving `main` a real
handler block holding an `uncaught.throw` terminator put that block in the
dump of **every program**, including the ones with no `try` and no `throw`
anywhere, and shifted every subsequent block id in `main` by one. Two pinned
lowering tests caught it immediately. A program that cannot throw should not
pay a line of IL for exceptions, and the flag costs nothing: `unwindTargetFor`
already had to choose between a handler block and the per-function unwind
block, and this only changes what that block contains.

node prints an uncaught error to **stderr** and exits non-zero; bronze does
the same, so a program's stdout holds exactly what it printed before it died.

## Decision 3 — the handler is a property of the block, and takes no parameters

The IL grows two operations and one field:

- `throw %v` — a terminator. Stores `%v` into the cell and goes to the
  handler.
- `%e = exc.take` — reads the cell and clears it. The first instruction of
  every handler block.
- `il::Block::handler` — the block to branch to when the cell turns out to be
  set inside this block, or `kNoBlock` for "leave the function".

Putting the handler on the *block* rather than on each call is what keeps the
check out of the IL entirely. Lowering never emits one; `createBlock` stamps
the current handler onto every block it makes, and codegen decides where the
tests go from `il::canThrow(inst)`. The alternative — lowering emitting an
explicit `br` after every call — would split a block per call, and every split
in bronze's block-argument SSA (docs/0005) costs one block parameter per
variable the region assigns. A ten-call `try` body would have carried ten
copies of the whole live set.

**The handler block has no parameters, and that is a hard rule, not a
convenience.** It is entered from an arbitrary point in the middle of the
protected region — the point where some call happened to fail — and nothing
at IL-construction time knows what any binding held there. There is no
argument list to write. Which forces decision 4.

## Decision 4 — a binding assigned inside a `try` lives in memory, not in SSA

```js
let x = 1;
try { x = 2; f(); x = 3; } catch (e) { }
console.log(x);
```

`x` is 1, 2 or 3 depending on where `f` threw. In SSA that is a join
parameter whose incoming edges cannot be enumerated. So `x` may not be in
SSA.

bronze already has exactly one mechanism for a binding that is not in SSA:
the environment record of docs/0007, which is what a *captured* binding uses,
for a structurally identical reason — something outside the linear flow can
read it at a moment lowering cannot name. `capturedNames_` becomes one input
to a wider set, `memoryNames_`, whose second input is
`ast::getTryAssignedNames`: every name assigned anywhere inside any `try`
statement of the function, nested functions excluded. `enterScope`,
`enterFunctionEnv` and `planModuleEnv` allocate slots from the union;
`getActiveVarsInDeclOrder` already skips env-backed bindings, so such a name
takes part in no join at all, and the handler block and the post-`try` join
both end up with zero parameters. The mechanism did not have to be built; it
had to be pointed at a second question.

Three details are load-bearing.

**The two sets stay separate where they are asked different questions.**
`lowerForStmt`'s per-iteration-binding diagnostic (docs/0007 decision 2) is a
*hard error*, and it must keep asking `capturedNames_`. Pointing it at the
union would make `try { for (let i = 0; i < n; i++) ... } catch {}` illegal,
which is absurd — nothing closes over `i`, so per-iteration freshness is
unobservable for it. Same shape as the split `planModuleEnv` already
documents.

**The over-approximation is safe in one direction only, and it is the right
one.** `getTryAssignedNames` is name-based and does not track shadowing, so a
binding *declared inside* the try that happens to share a name with one
outside is promoted too. That costs an environment slot and a load; it can
never lose a value. Narrowing it needs the same escape analysis docs/0004
defers for captures.

**A `for` header binding is the one name the promotion cannot reach**, since
`lowerForStmt` opens its scope with the no-argument `enterScope()` and no
record is created. That is sound rather than lucky: a header binding is
declared inside the `try`, so it is out of scope in the `catch`, in the
`finally`, and after the statement. There is no reader for the value the
promotion would have preserved.

**The catch parameter is env-backed too**, and it is the one name in the union
that does not strictly need to be: it is bound at the top of the handler block,
before any user code, so it could have stayed in SSA. It is left in memory
because 14.15.2 gives the catch clause its own declarative environment and
because a closure made in the catch body captures it — the promoted oracle
case does exactly that — so the record would usually be built anyway. The cost
is one environment record per handler ENTRY, which is the slow path by
construction.

Inference reads the same union. `queries.h` already states why — "two
hand-maintained copies of these rules would eventually disagree, and a
disagreement is a silent miscompile" — and `types/flow.cpp` seeds
`scope_.captured` from `getCapturedNames` ∪ `getTryAssignedNames` for exactly
that reason. An env-backed cell is one value joined over the whole function,
which is the sound reading of "a handler may observe any of the writes".

## Decision 5 — `finally` is duplicated per exit path, not dispatched to

`finally` runs on every way out of the `try`: normal completion, `throw`,
`return`, `break`, `continue`. The two implementations are a **completion
record** — store what the try was trying to do, run the finally once, switch
on the record — or **duplication**: lower the finally body again in front of
each exit.

Duplication wins here, and the reason is specific to bronze rather than
general. The completion record needs somewhere to put the record, and bronze
IL has no stack slots: the only writable memory lowering can address is an
environment record, whose `(depth, index)` addressing is relative to
`currentEnvValue_`. Introducing a synthetic scope for a compiler temporary
would shift the depth of every capture inside the `try` — a change to the
meaning of every `env.get` in the region, to hold two words. The dispatch
switch then has to re-enter block-argument joins for `break L` at arbitrary
depth, which is the machinery decision 3 exists to avoid needing.

Duplication needs none of it, and it gets the hard cases right by
construction rather than by rule:

- **`try { return 1 } finally { return 2 }` is 2.** The return value is
  computed, the finally is lowered inline in front of the `ret`, and the
  finally's own `ret` terminates the block first. The outer `ret` is
  unreachable code, which docs/0014's statement-list rule already drops.
- **`try { throw x } finally { return 2 }` is 2, and `x` is discarded.** The
  handler takes the pending value with `exc.take` — which *clears* the cell —
  lowers the finally, and only re-raises with `throw %e` if the finally
  completed normally. A `return` inside it terminates the block before the
  re-raise is reached, so the discard is the absence of code rather than a
  rule about precedence.
- **A labelled `break` crossing two `finally`s runs both, innermost first**,
  because crossing is a loop over a stack: `finallyStack_` entries each record
  the `jumpStack_` depth they were pushed at, and `break` to the target at
  index `i` runs every entry whose depth exceeds `i`, from the top down. The
  same loop, run to the bottom, is what `return` does.

The cost is code size: the finally body is lowered once for normal
completion, once for the exception path, and once per `return` / `break` /
`continue` that crosses it. Bounded by the source, and paid in a construct
that is rare in hot code. It is also why the finally body is lowered from the
AST each time rather than cloned — a re-lowering is a fresh scope with fresh
blocks and fresh SSA values, and nothing in lowering is stateful across it
except the IL function list, where a named function declaration written
inside a `finally` yields one IL function per copy. They are identical, they
are reached as closure values, and only the last is in `functionIndices_`;
the waste is recorded rather than diagnosed because refusing a legal program
to save an object-file section would be the wrong trade.

`try { A } catch { C } finally { F }` is lowered as `try { try { A } catch
{ C } } finally { F }`, which is what 14.15.3 says it is: the finally covers
the catch clause, so a `throw` from inside `C` still runs `F`.

## Decision 6 — which runtime errors become catchable, and which stay fatal

Every hard error in the runtime aborted the process before this chunk. Not
all of them should become throws, and the line is **what ECMA-262 defines**,
not what happens to be reachable:

- A `TypeError` / `RangeError` / `ReferenceError` the spec names becomes a
  real thrown Error object. Reading a property of `null` (docs/0018 decision
  9), `delete` of the same, calling a non-function, `new` on a non-function,
  `instanceof` with a non-object right operand, an `Array.prototype` method
  on a non-array or with a non-function callback, `reduce` of an empty array
  with no initial value, a `String.prototype` method on a non-string,
  `repeat` with a negative count.
- **An unimplemented construct stays `fatal`.** `unsupported: $-substitution
  in String.prototype.replace` is not a JS error, it is bronze declining, and
  a program must not be able to `catch` it and carry on as though the feature
  existed. That is the "hard errors over silent fallbacks" rule applied to a
  new escape hatch that would otherwise dissolve it.
- **An internal invariant stays `fatal`.** `internal: a shape transition
  attempted on a dictionary-mode object` describes a compiler bug. Catching
  it would make the bug a program's problem.

A helper that calls back into JS has one further obligation: it must test the
cell after the callback and stop. `[1,2,3].forEach(f)` where `f` throws must
visit one element, not three, and the loop is inside the runtime where no
generated check exists. Every callback loop in `builtin_array.cpp` and both
accessor calls in `accessor.cpp` carry that test.

## Decision 7 — `Error` is a native constructor, and printing one has no stack

`Error`, `TypeError` and `RangeError` are provided globals (docs/0011
decision 1) whose values are ordinary function objects over a native
`bronze_fn_code`. Nothing new: a `FunctionHeader` holds a C function pointer,
`bronze_construct` already builds the instance from `instance_shape` and runs
the code, and the three prototypes chain to `Error.prototype` through the
same `rtNewRootShape` that `class D extends B` uses. So `err instanceof
TypeError`, `err instanceof Error`, `err.name`, `err.message` and the
prototype walk all work through machinery that was already there, and a
`catch` block cannot tell a runtime-raised TypeError from a hand-written
`throw new TypeError(...)`.

Each of the three needs its **own** `bronze_fn_code`, which is not obvious and
is the source of the worst bug in this chunk. Native function objects are
interned by code pointer (`bronze_function_singleton`), so three classes
sharing one constructor body became one function object: the last class built
won every `.prototype`, and `new Error("x").name` answered `"RangeError"`.
Every `instanceof` still said true, because there was only one class. Three
one-line trampolines onto a shared implementation is the fix, and
`exception_test.cpp` pins that the three objects are distinct.

`Error("x")` builds the same thing as `new Error("x")`: 20.5.1.1 opens with
"if NewTarget is undefined, let newTarget be the active function object", so
the constructor makes its own instance when it is reached as a plain call.
`message` is installed NON-enumerable (20.5.1.1 step 4 is
CreateNonEnumerableDataPropertyOrThrow), which is what keeps
`Object.keys(err)` empty.

`console.log` of an error prints `Name: message`, or `Name` when the message
is empty. **This is a deliberate divergence from node**, which prints a stack
trace, and it is deliberate in the docs/0013 sense: bronze has no stack to
print. An error is recognised by walking its prototype chain to
`Error.prototype` rather than by a header flag, because a flag other than 0
would take error instances off the inline property fast path
(`BRONZE_ABI_OBJ_FLAGS_PLAIN`) for no gain.

Rendering it **must not allocate**. `inspect.cpp` holds raw `ArrayHeader*` and
`ObjectHeader*` across every element it formats and says so in a comment;
reading `name` and `message` through the ordinary property path allocated the
key strings, and one collection inside that loop moved the container out from
under it. `rtErrorText` therefore reads the two arena-interned key headers it
captured when the classes were built, walks the prototype chain by hand, and
answers **false** for anything it cannot render that way — an accessor `name`,
a non-string `message`, a dictionary-mode instance. Such a value prints as the
object it is. Inventing a rendering would have been the silent wrong answer;
this is the boundary drawn where it can be seen.

`err.stack` is not defined, and reads as `undefined` rather than as a named
error, because 20.5.6.1 does not define it either — it is a de-facto
extension, and `undefined` is the answer for a property JS does not have.
`Error.prototype.toString` (20.5.3.4) is likewise absent: bronze has no
ToPrimitive for objects at all (`"" + err` is a hard error today), so
providing the method alone would make `err.toString()` work and leave every
implicit conversion around it saying something else.

## What this chunk deliberately leaves out

Three things were deferred *into* this chunk by earlier ones. One is closed,
one cannot be closed here, and one is a mechanism of its own. Each of the two
that stay open is re-seeded as a blocked case, with expectations derived by
hand and a header saying what blocks it.

- **Closed: a nullish property access and the `Array.prototype` TypeErrors
  are now catchable.** They were process-fatal (docs/0018 decision 9). See
  decision 6.
- **Not closed, and not closable here: docs/0019 decision 6**, the
  strict-mode TypeError for writing a getter-only property. `throw` now
  exists, so the stated blocker is gone — but the pinned
  `accessor_properties.expected` records the *sloppy-mode* answer, a silent
  no-op, and a committed expectation is never edited to match a change in
  bronze (docs/0003). Raising the TypeError therefore is not a bug fix; it is
  a decision that bronze's source language is strict-mode JavaScript, which
  would also change `delete` of a non-configurable property, assignment to an
  undeclared name, duplicate parameter names, and `this` in a plain function
  call. That is a language-level decision with its own case list, and it does
  not belong in the chunk that happens to have built the raising mechanism.
  `cases/blocked/strict_mode.js` holds it.
- **Not here: the temporal dead zone**, and therefore `let` directly in a
  `switch` case (docs/0018 decision 6, which expected `throw` to lift it). A
  `ReferenceError` for reading a `let` before its declaration is not a use of
  `throw`; it is a second mechanism — every let-binding's storage has to start
  as an "uninitialized" marker and every *read* has to test for it, which is a
  check on the hottest operation in the language and needs inference to remove
  it in the ordinary case. The Hole sentinel is not available for it: decision
  1 above spent it on the empty cell. `ReferenceError` is not one of the three
  constructors either. Lowering's existing static `isInitialized` diagnostic
  covers the straight-line reading and is not weakened.
  `cases/blocked/temporal_dead_zone.js` holds the expectations, and
  `cases/blocked/switch_case_lexical.js` names it as the prerequisite for the
  switch-case restriction.

Also absent, with no promises: `error.stack`, `error.cause`,
`Error.captureStackTrace`, `AggregateError`, `SyntaxError`/`URIError` (nothing
raises them), `try` around a `for-of` calling the iterator's `return()`
(bronze's for-of is an index walk, docs/0012 decision 2, and has no iterator
to close), and `catch` of an error raised inside a getter reached from
`console.log` (inspect deliberately does not run getters — docs/0019 decision
4).

## Divergences from node

- **An error prints without a stack.** Decision 7.
- **An uncaught exception's stderr text is bronze's, not node's.** It names
  the value and exits 1; there is no stack trace and no `^` source excerpt.
- **`throw` of a non-Error still works and is not warned about.** `throw
  "negative"` is what the promoted oracle case does, and it is legal JS.

## Named diagnostics this chunk introduces

Parse:

- `'{' to open a try block`
- `'{' to open a catch block`
- `'{' to open a finally block`
- `a catch parameter name`
- `')' after a catch parameter`

There is deliberately no diagnostic for a default or a rest element inside a
catch parameter. The first draft refused both; `CatchParameter` is a
`BindingPattern` (14.15) and its elements admit an `Initializer` and a
`BindingRestElement` like any other, so `catch ([a = 1, ...r])` is legal and
`exception_catch_binding.js` pins it.

Lower: none added. `unsupported construct: try/catch/throw` is retired.

Runtime (all catchable, all real Error objects). The texts are bronze's, not
the standard's — ECMA-262 fixes which constructor, never the message — so they
are pinned in `tests/runtime/exception_test.cpp` and never in an oracle
expectation:

- `Cannot read properties of null (reading 'k')` / `... of undefined ...`
- `Cannot set properties of null (setting 'k')` / `... of undefined ...`
- `Cannot convert null to object (deleting 'k')` / `... undefined ...`
- `a <type> is not a function` / `a <type> is not a constructor`
- `Right-hand side of 'instanceof' is not callable`
- `Cannot use 'in' operator: the right-hand side is not an object`
- `Array.prototype.<m> called on a value that is not an array`
- `Array.prototype.<m> needs a function argument`
- `Reduce of empty array with no initial value`
- `String.prototype.<m> called on a value that is not a string`
- `Invalid count value: <n>` (`repeat`)
- `array destructuring of null or undefined` / `object destructuring of ...`
- `<construct> of a value that is not an array, string or typed array`

Retired: the `fatal` wordings of every runtime error listed above.

## Files this chunk added, and the seams it cut

- `src/lower/lower_try.cpp` — decisions 3 to 5, kept out of
  `lower_control.cpp` (555 lines) because the seam is a completion kind rather
  than a block shape, and because `lower_control.cpp` is one of the two files
  CLAUDE.md's 1000-line rule was going to bite next.
- `src/runtime/exception.{h,cpp}` — the cell, the `Error` family and the raise
  helpers, kept out of `rt_prop.cpp` (523 lines) for the same reason. There is
  no separate `rt_error.cpp`: `bronze_uncaught_exception` is the only ABI
  entry point exceptions add, and it belongs next to the cell it reads.
- `src/ast/assigned.{h,cpp}` — moved out of `src/lower/`, because decision 4
  made "which names does this statement write" a question inference asks as
  well as lowering, and `src/types` may not depend on `src/lower`. The move
  renamed `lower::getAssignedVariables` to `ast::getAssignedNames` and added
  `ast::getTryAssignedNames` beside it, one walk with a flag rather than two
  copies of "which expression forms write a name".
- `src/types/flow_analyzer.h` + `src/types/flow_expr.cpp` — `flow.cpp` reached
  974 lines with decision 4's changes in it. The cut is statements from
  expressions: a statement rule is about the ENVIRONMENT (what survives a
  merge, what a loop header converges to) and an expression rule is about a
  VALUE (what an operator produces, which callee a site widens). They share a
  walker only because the environment is threaded through the expressions.
- Tests: `tests/lower/lower_try_test.cpp` pins the IL shape (two of this
  chunk's three self-inflicted bugs were invisible in program output and
  obvious in the dump), and `tests/runtime/exception_test.cpp` pins the cell,
  the class identities and the message texts.
