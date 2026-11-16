# Deopt accounting (`BRONZE_DEOPT_STATS=1`)

A speculated function S leaves for its generic function G when a guard fails
(docs/il2mir.md; `leave` / `resume.call` in `src/il/il_op.h`). Every such exit
is counted, always, and a program run with `BRONZE_DEOPT_STATS=1` prints the
counts at exit. The variable only selects the report: the counting and what it
drives (retirement, below) are the same with or without it.

```
BRONZE_DEOPT_STATS=1 ./app
deopt: 894 exits at 58 resume points
       exits  id    guards -> resumes at                    where                 function
         720  1     fn.code_is/is.object/ptr.eq -> method.call  three.module.js:19304  mod4.WebGLUniforms.setValue
          46  2     is.object/proto.cell/ptr.eq -> prop.set     three.module.js:3903   mod4.Vector3
...
deopt: retired mod4.InstancedMesh.setMatrixAt (1000 exits)
deopt: retired addListener's optimistic copy to its speculated copy (100 optimistic exits)
```

One row per resume point of a generic function that was reached at least
once, by count:

- `exits`: how many times G's resume twin was entered there: every `leave` of
  S and every `resume.call` of an inlined copy of S that resumes at the point.
- `id`: the point's id within G (`resume.point <id>` in `bronze il`).
- `guards -> resumes at`: the ops that define the conditions of the branches
  into S's exit blocks for the point (`ptr.eq` a shape or cell compare,
  `is.object`, `is.number`, `fn.code_is` a callee identity, `proto.cell` a
  transition cell, `inlined f` an exit of an inlined body of f, `jump` an exit
  reached without a branch), then the IL op G re-executes there, and
  `(loop entry)` for a point at the top of a loop header (a hoisted guard).
  `optimistic` first marks an OPTIMISTIC point, which only an optimistic copy
  leaves for (below).
- `where`: the source position of that op.
- `function`: G's name, as the backend names it.

A function retired during the run is listed after the table, and so is an
optimistic copy retired to its speculated copy.

## How it is counted

`src/codegen-brass/deopt_sites.{h,cpp}` numbers every resume point of the
module (by function, then id) from the IL the optimiser left and writes one
line of text per point. `__bronze_deopt_counts` (per-thread module data) holds
one RETIRE word per generic function with points, then one counter per point,
then one OPTIMISTIC word per generic function.
The backend builds each G's resume twin with brass's `ResumeTwinOptions::
on_exit` hook (`il_lowering_resume.cpp`): the entry of the twin for point `id`
adds one to the point's counter and to G's retire word (its optimistic word,
for an optimistic point), after it has read the state. Nothing of it is in S: a fast path pays nothing. The module's entry
registers the table with `bronze_register_deopt_counts`
(`src/runtime/deopt_stats.cpp`), which prints the report at exit when asked.
Only the registering thread's copy is printed.

## Retirement

AOT code cannot re-profile, so a speculation that keeps failing would fail for
the rest of the run. Each speculated function S whose G has points has an
ENTRY CELL in the image's shared data (`__bronze_entry_<S>`), a word holding
S's address that every direct call of S and S's wrapper call through
(`call @S via @cell`: one `call [rip + cell]`, as the direct call was). Once
G's retire word has reached 1000 exits (`TranslatorOptions::deopt_retire_after`),
G's twin stores S's retired entry (`__retired_<S>`, S's signature, calls G)
into the cell, so every call is G's from then on and S's own code tests
nothing (`build_resume_twins`, `il_lowering_resume.cpp`). Copies of S inlined
into other speculated functions are not covered and keep exiting through
their `resume.call`s (which keep counting). A loop that left S runs to its end in the twin; there is no way
back into S at a loop header.

## Optimistic copies

A function where an assumption pays only if its miss leaves has a second
speculated copy, the OPTIMISTIC copy S+ (`<G>$opt`; il.h `Function::fallback`
names S), entered before S: blocks of G the harvested run never entered, and
the generic arms of polymorphic own-slot reads and writes and of stores into
double slots, leave S+ where S runs them in place (`src/opt/speculate/
optimistic.h` has the design). Those exits leave for optimistic points of G
and count into G's optimistic word. Once it reaches 100
(`TranslatorOptions::deopt_optimistic_after`), G's twin copies S's entry cell
into S+'s, so S+'s calls run S (or G, if S has retired). An ordinary exit of
S+ counts toward G's retire word as S's do, and retiring G stores the retired
entry into both cells. So data the harvest never showed costs at most 100
leaves per thread before the run is S's, and no function retires to G that
would not have without S+.

