# Scoped escape analysis over environment records (stage R3)

An environment record is a heap object. Every captured variable a closure reads
is a load out of it and every write is a store into it, and both are cheap —
stage E2 got them down to one instruction each. What no stage before this one
could do is make them *disappear*: the record is heap-addressable, every call in
a loop might reach it, and so nothing survives a backedge in a register.

Stage E4 measured exactly what that costs. `bench/env_slot_kernel.js` and
`bench/env_slot_kernel_registers.js` are the same arithmetic and the same
checksum, one with the hot state in a factory closure's record and one with it
in bindings nothing captures: **29 loads, 47 stores and 22 calls per iteration
against 3, 7 and 5**, and 11.03 ns/iter against 4.27. The gap IS the record.

Stage R3 promotes a record's slot to an SSA value over a **region** — a span of
code in which nothing but the accesses the promotion rewrites can observe that
slot — with a load at the region's entry and a write-back at every one of its
exits.

Implementation: `src/codegen-llvm/llvm_env_reach.{h,cpp}` (what can see a slot),
`src/codegen-llvm/llvm_env_promote.{h,cpp}` (regions and the rewrite), wired at
`registerOptimizerEarlyEPCallback` in `src/codegen-llvm/llvm_backend.cpp`. The
`!bronze.env.nonptr` half is emitted by `emitEnvSet` in
`src/codegen-llvm/llvm_env.cpp`. Tests:
`tests/codegen_llvm/env_promotion_test.cpp` and the six
`tests/oracle/cases/env_promotion_*.js`.

## Where the analysis runs, and why that is the whole stage

**It runs on optimized LLVM IR, after the inliner.** Asked of the IL, this
analysis proves nothing at all on the shape it was built for. `env_slot_kernel`'s
loop is four calls to sibling closures over the same record; every one of them
writes the slots the loop reads, so at IL level every region ends at the first
call and not one slot is promoted. Asked of the IR after stage E1's direct edges
have been inlined, those calls are gone and their accesses are ordinary loads
and stores in the caller's own loop, which the promotion rewrites along with the
caller's.

That also settles how inlining composes, without a second argument. A callee
that inlined has its accesses *in* the region. A callee that did not is a call,
and a call the analysis cannot see through ends the region. The nightmare case
— a promoted register live while inlined code writes the heap slot — cannot
arise, because both halves are read out of the same IR.

The extension point is `registerOptimizerEarlyEPCallback`, which is after the
module SIMPLIFICATION pipeline (inlining, SROA, EarlyCSE, GVN, LoopSimplify,
LICM) and before the function optimization pipeline. Late enough that CSE has
already made a callee's `env` argument and the caller's record the same SSA
value; early enough that the phis it creates are there for unrolling and
vectorization to read.

## What a key is

A promoted slot is named by a pair: the **i64 Value bits** an access resolved its
record from, and the **byte offset** of the slot inside that record.

The record is an SSA value and not a symbolic `(scope, depth)` pair on purpose.
Two accesses that came from different source functions name the same slot here
exactly when the inliner and CSE made them share a value — which is the only
condition under which promoting them together is correct.

An access is recognized by the shape `llvm_env.cpp` emits and nothing else: an
8-byte load or store carrying the `EnvRecordSlots` alias scope, through
constant-offset GEPs off `inttoptr (and %bits, PAYLOAD_MASK)`, at an 8-aligned
offset at or past `BRONZE_ABI_ENV_SLOTS_OFFSET`. An access to the record HEADER
— the brand, the size, the parent link — is not a slot and is never a key.

## What can see a slot — the enumeration, and every refusal in it

**An environment record cannot be named by the accepted language.** `with` is a
hard parse error in both modes (`src/parse/parser_stmt.cpp`), `eval` runs with
indirect semantics and says so once per module (`src/lower/lower_call.cpp`), and
no helper in the ABI registry but the five `bronze_env_*` entries touches a slot.
That is what makes an enumeration possible at all rather than merely optimistic.

| what | verdict | why |
|---|---|---|
| another access to the same key | not an observer | it IS the region |
| an env access at a different offset | not an observer | two 8-byte accesses at different 8-aligned offsets are disjoint bytes inside one record, and are two different records otherwise |
| an env access at the same offset through another record value | **ends the region** | the two values might be one record |
| a `noreturn` call | not an observer | control does not come back, and bronze's `noreturn` helpers are fatals |
| a `memory(none)` call | not an observer | it cannot touch memory |
| an env-blind call | not an observer | see below |
| any other call | **ends the region** | including every indirect call |
| a load or store whose underlying object is a global or an `alloca` | not an observer | a record is allocated in the collector's heap and lives nowhere else, so those bytes are never a slot's |
| any other load or store alias analysis cannot separate from the slot | **ends the region** | |
| a `yield` / `await` | not reachable | a suspension is a return out of the resume function; there is no region across one |
| a `throw` | not an observer | in this runtime a `throw` is a store into `bronze_exception_cell` and a branch to the handler edge — no call, no unwind, no `invoke`, no landing pad. It is the row above that admits it |

**Env-blindness** is a greatest fixpoint over the module's call graph
(`EnvReach`). A defined function is blind unless its body contains an env-scoped
memory access, an indirect call, or a call to a function that is not blind. A
declaration is blind only if it is `memory(none)`, or if it is on the named list
in `llvm_env_reach.cpp` — where each entry is an explicit soundness claim about
one ABI helper, and the five `bronze_env_*` entries are deliberately absent.

The fixpoint starts optimistic and lowers, which is what makes a recursive pair
of leaf helpers blind rather than mutually suspicious. It is the same shape
`planRepr` solves for representations.

> There are no guards in this stage and no optimism. Every other campaign
> mechanism that claims something about a program can be wrong under a guard
> that catches it; a wrong region here is a silent wrong answer, so where the
> call graph is incomplete — an escaped closure, a dynamic call, a call through
> a value — the region ends. That is the whole soundness argument, and the
> region-end histogram below is what says how often it is paid.

## The regions

Two shapes, and both are entered once and left once.

**A loop**, entered through its preheader and left through its exit blocks. This
is the shape the stage exists for: N accesses per ITERATION become one load per
loop ENTRY and one store on the exit that is taken. Loops are tried
outermost-first, because a clean outer loop subsumes every loop inside it.

**A whole function body** that contains no observer at all, entered at the
record's definition and left at every `ret`. This is the leaf closure — a getter,
a small accessor — where the accesses are not in a loop. It is taken only when
the record is an argument or is defined in the entry block, so that the entry
load dominates every write-back, and only when it removes more memory operations
than it adds.

### Write-back placement

**Every edge that leaves a region carries the write-back**, and the enumeration
is short because of how bronze compiles control flow.

| exit | where the write-back goes |
|---|---|
| falling out of the loop | the dedicated exit block |
| `break`, `continue` crossing the region boundary, a labelled jump | the same — they are exit edges of the natural loop |
| `return` from inside a loop | an exit edge, because a `return` block cannot reach the latch |
| `return` in a function region | before the `ret` |
| `throw` | wherever the pending-flag branch leaves the region, which is one of the above |
| a block terminated by `unreachable` | **nowhere** |

The `unreachable` exception is deliberate and is two arguments, not one. Those
blocks are the access guards' failure edges and the fatal helpers they call, so
the process does not survive one and there is no later reader to be handed a
stale slot — and the pointer a write-back would store through is precisely the
one the guard has just rejected.

A region that stores nothing into its slot takes no write-back at all: the heap
slot stays exactly right, and hoisting the load is the whole of the win.

**Dedicated exits are formed rather than required.** An exit block shared with
code outside the loop has nowhere to put a write-back — an ordinary path into it
would run one — so the loop's edges into it are split into a block of their own
(`llvm::formDedicatedExitBlocks`, which is what LICM does for the same reason).
This is done AFTER the region is known clean, so a refused key never costs the
module a basic block. It is not an optional refinement: in `__wrapper_render` the
block a pending exception returns through is reached from the loop AND from the
epilogue, so the kernel this stage exists for has no dedicated exit until one is
made.

## The collector

A promoted register is invisible to the collector, and the heap slot the
collector DOES scan goes stale the moment the register is written. So a key is
promoted only when one of three holds:

- the region stores nothing into the slot — the heap stays exactly right; or
- nothing in the region can collect; or
- every value the region stores into the slot is provably not a heap address.

The value a region STARTS with needs no clause. Until the first store, the heap
slot still holds it and the record is what the collector scans; and a heap slot
left holding a value nothing reads any more is over-rooting, which is safe.

The third clause is **stage R2 speaking**. `emitEnvSet` attaches
`!bronze.env.nonptr` to a slot store whose IL value the representation plan calls
`NeverPointer` (`docs/slot-representation.md`), and nothing new is being believed
here: R2 already spends the same fact on giving that value no GC root slot at
all, which is strictly the stronger consequence. A structural fallback covers a
store the optimizer rebuilt without the metadata — a double-typed value from
arithmetic, a `bitcast` of one, a constant whose NaN-box tag is not one of the
three heap tags, or a `select`/`phi` of those. Absence of the metadata means
"not proven", never "proven false", so losing it to an optimization is a lost
region and not a wrong one.

So the answer to "are promoted heap pointers still rooted per R2's rules" is:
**they are excluded from promotion**, and the histogram counts each exclusion as
`heap-value-store`.

## The census, and why an instrumented build cannot report a different program

The obvious hazard for a stage that hides a slot's value in a register is a
diagnostic surface that reads records at arbitrary points — it would report the
heap, which is stale, and the numbers a census produced under promotion would
not be the numbers the same program produces without it.

There is no such surface, and that is a fact about where bronze observes rather
than a choice made here. Both observers of a captured binding — the `--pins`
barrier and the census's `EnvSlot` site — are emitted in `lower_scope.cpp`'s
`emitEnvSet` **on the value being stored, before the store**, and keyed on the
logical slot rather than on the record. They see the value the program computed;
promotion changes where that value is written and not what it is. And in an
instrumented build they are calls (`bronze_census_record`,
`bronze_pin_violation`) that no allowlist admits, so a slot carrying either one
has no region at all.

So promotion is left ON in the instrumented build, because the two cannot
disagree: an observed slot is not promoted, and an unobserved slot has nothing
to report.

## The one thing that changes besides speed

The entry load is **speculative**: it reads the slot on a path that might not
have accessed it, and it reads it without the access guards. Both are the static
plan's claim — the same one `BRONZE_ELIDE_ENV_GUARDS` rests on, and
`llvm_env.cpp` says at length what licenses it.

What it costs is tripwire ORDER. If a lowering bug ever produced a record of the
wrong kind, a promoted function faults at the entry load instead of reaching
`bronze_env_access_failed`'s fatal. The guard itself is untouched and still armed
at every access site that survives the region, and under
`BRONZE_ELIDE_ENV_GUARDS=1` there was no tripwire on that path to begin with.
This is recorded rather than fixed: placing the entry load behind a guard of its
own would put the guard back in the preheader, which is where LICM was already
hoisting it.

## Env vars

| var | effect |
|---|---|
| `BRONZE_NO_ENV_PROMOTION=1` | **the seam.** The analysis does not run; the emitted code is what the stage-R2 compiler produced. Read by the COMPILER, once per invocation, because what it isolates is the emitted code — a run-time flag could not change it. Swept as a `CSEAMS` entry in `tests/oracle/pin_matrix.sh` for the same reason. |
| `BRONZE_ENV_PROMOTION_STATS=1` | prints the static counts and the region-end histogram to stderr after the last partition's pipeline has joined |
| `BRONZE_DUMP_LLVM_IR=<prefix>` | `<prefix>.post.ll` is where a region can be read: the promoted slots are the `env.promote.in` loads in a preheader and the `env.promote.out` stores in the exits |

## Reading the counters

```
$ BRONZE_ENV_PROMOTION_STATS=1 bronze build bench/env_slot_kernel.js -o es.exe \
      --pins bench/pins/env-slot-kernel.pins
[envpromote] functions=7 keys=35 regions=8 (loop=7 fn=1) slots=8 loadsElided=10 \
  storesElided=12 entryLoads=8 writeBacks=15
[envpromote] region-end unknown-call=15 env-helper=0 indirect-call=0 \
  aliasing-memory=0 aliasing-env-slot=0 record-not-invariant=0 loop-shape=0 \
  heap-value-store=0 no-benefit=7
```

The counter exists for the reason `llvm_repr.h` gives for its own: every arm is
conditional on a proof, so a stage that proves nothing emits exactly the code it
replaced and the whole suite still passes. The site count is the only thing
separating "the fast arm is correct" from "the fast arm is ever taken".

`loadsElided` and `storesElided` are counted where the pass runs, which is
before unswitching and unrolling duplicate a loop body — so they are smaller than
the per-iteration counts a `.post.ll` shows, and the two are not the same
measurement. The histogram counts REFUSALS, not regions: one key can be refused
at function scope and then promoted at loop scope, and both are recorded.

## What R3 does not build, and hands on

- **A callee summary that is finer than "touches a record".** A call to a defined
  function that touches SOME env record ends every region, even for a key that
  function provably cannot reach. Making that finer needs record identity across
  a frame boundary, which is a different analysis.
- **Argument promotion.** A region that ends at a direct call to a callee which
  touches exactly one slot could pass that slot in and out by value instead of
  ending. That is the mechanism that would make a NOT-inlined direct edge as good
  as an inlined one.
- **A region that spans a barrier.** Today a function whose body contains one
  opaque call gets no region at all, even with ten accesses on either side of
  it. The general form is one alloca shadow per key over the whole function with
  a flush before every barrier and a reload after it, which is beneficial when
  `1 + 2·barriers + returns < accesses` — weighted by block frequency, because a
  barrier inside a loop costs per iteration and the accesses it separates may
  not. That is the mechanism that turns this stage from "the loop backedge" into
  "the whole function", and it is deliberately not built here: it can make code
  WORSE where barriers are dense, and nothing in this stage's measurements says
  which way a real corpus goes.

The region-end histogram over a real corpus is the input to all of these:
whichever bucket dominates is the analysis worth building next.
