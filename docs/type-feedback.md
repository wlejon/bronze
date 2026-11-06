# Type feedback (`bronze build --feedback-out <file.tfb>`)

Most values in a real library stay `Dynamic` in the IL, because bronze types a
value only where a proof does. Type feedback is the measurement that says what
those values actually are on a representative run, so a later build can
specialize the hot ones behind a guard. This document covers the recording,
the file, the reader, and the build that spends a profile (`--feedback-in`).

```sh
bronze build app.js -o app.fb --feedback-out app.tfb   # 1. instrument
./app.fb                                               # 2. a representative run
bronze build app.js -o app --feedback-in app.tfb       # 3. the build that reads it
python3 bench/tools/feedback_report.py app.tfb         # what the run saw
```

A feedback build is an instrument, never measured and never shipped. Without
either flag a build is byte-identical to one made before the feature existed;
`--feedback-in` with a profile that names none of the build's functions
changes no byte either.

Implementation:

| piece | file |
|---|---|
| site words, kinds, tags (shared by compiler and runtime) | `src/abi/bronze_feedback_site.h` |
| site enumeration, function keys, IL hash | `src/il/feedback_site.{h,cpp}` |
| the `feedback.record` op | `src/il/il_op.h` |
| instrumentation pass | `src/lower/lower_feedback.{h,cpp}` |
| runtime recorder and writer | `src/runtime/type_feedback.{h,cpp}` |
| reader and query API | `src/opt/spec/profile.{h,cpp}` |
| plan (profile read against the probe) | `src/opt/spec/feedback_plan.{h,cpp}` |
| claims, direct targets, hints | `src/opt/spec/feedback_apply.cpp` |
| probe + where each consumer runs | `src/lower/lower_final_passes.cpp` |
| closure split for profiled `call.dynamic` | `src/lower/own_callee_calls.{h,cpp}` (`applyClosureCalls`) |
| hinted backend lowering | `src/codegen-brass/il2mir/il_lowering_dynamic.{h,cpp}` |
| adversarial-profile suite | `tests/oracle/oracle_feedback_test.cpp` |
| report | `bench/tools/feedback_report.py` |

## Sites

`il::enumerateFeedbackSites(module, fn)` is the single definition of what a
site is. It runs on the IL as the last lowering pass leaves it, and the
instrumentation pass runs after everything else in `lowerModule`, so the sites
are the final build's own and no pass ever sees a record. A reading build calls
the same function at the same point and gets the same sites without
instrumenting anything.

Per function, in this order (the position is the site's **ordinal**):

| kind | where | observes |
|---|---|---|
| `entry` | top of block 0 | that the function ran (entry count) |
| `this` | top of block 0 | the `this` parameter, if Dynamic |
| `param` | top of block 0 | each Dynamic source parameter (not rest) |
| `value` | before the instruction | each Dynamic operand of arithmetic, bitwise, `to.numeric`, `numeric.step`, `to.int32`, `rel.*`, `cmp.*`, `strict.eq`, `loose.eq`, and a checked `unbox` (truthiness or ToNumber) |
| `shape` | before | the receiver of `prop.get` / `prop.set` (an inline-cache site) |
| `target` | before | the callee of `call.dynamic[.spread]` |
| `method` | before | the receiver of `method.call[.spread]`: its shape, and the method its key resolves to |
| `result` | after | the result of `call`, `call.dynamic`, `method.call`, `prop.get`, `elem.get` |

Entry, this and param sites are skipped for the module entry point and for a
function whose block 0 is a branch target (a record there would count loop
iterations). A value held as raw double bits (`numberBits`) is never an operand
of a record, and nothing is inserted strictly inside a transition run.

## Keys

A site's key is **(function key, ordinal)**. The function key is

```
<stable name>#<16 hex digits of an FNV-1a hash of the function's IL>[~N]
```

The stable name is the IL name with renumbered counters stripped:
`__anon_fn_12` becomes `__anon_fn_`, a leading `mod3.` becomes `mod.`. The hash
covers the signature and every instruction (op, types, value ids, immediates,
flags, block targets), with key indices resolved to their strings and function
indices to stable names. It leaves out spans, inline-cache indices and static
cells, which move when other code changes. `~N` disambiguates the Nth further
function that would otherwise share a key.

So a changed function loses its feedback (its hash moves), and an unchanged
one keeps it across edits elsewhere. One instability is known: an environment
slot index is part of the hash, so adding a captured binding ahead of the ones
a function reads changes that function's key.

## Recording

`feedback.record [%v], <kind> #<ordinal>, "<function key>"` lowers to the pin
census's runtime call. The site word's top bit routes it to the type-feedback
recorder, so the generated-code ABI and its fingerprint are unchanged. A
feedback build registers its output path through the census table: a marker
row, plus a (method site, method name) row pair per `method` site, since a
record carries only one key. `--census` and `--feedback-out` are refused
together.

Per site the recorder keeps:

- **tag counts**: `f64`, `i32` (an Int32 box, or an integral double in int32
  range that is not -0), `undefined`, `null`, `bool`, `string`, `object` (plain),
  `array`, `typedarray`, `function`, `other`;
- **shapes** (shape and method sites): by stable name,
  `<prototype>{k1,k2,...}`, where the prototype is named by its own
  `constructor`'s `name` (`^` per level climbed when it has none), and the
  keys are the own keys in insertion order (32 shown, then `+N`). A dictionary
  is `<prototype>{dict}`. A receiver that is not a plain object is named by
  kind (`array`, `function`, `number`, ...). Shapes that differ only in
  attributes or slot representation share a name;
- **targets** (target and method sites): `<name>@<8 hex digits>`, the
  function's `name` and an FNV-1a hash of its source text (what a reading
  build can compute for an IL function from `sourceBegin..sourceEnd`), or
  `native:<name>` for a function with no source. A method is resolved without
  running anything: own and prototype data slots by shape (cached per
  receiver shape, checked against the prototype-mutation epoch), `accessor.<m>`
  for an accessor, `<kind>.<m>` for a non-plain receiver (`array.push`).

At most 32 distinct names per site are tracked; further ones are counted in
`*other*`. The recorder takes one mutex per record (the census's approach)
and allocates nothing on the JS heap.

The profile is written at process exit (atexit), and on demand by
`__bronze_feedback_flush()`, a builtin that answers `true` when it wrote. Call
it from a host that leaves through `_exit` (bro-headless does).
`BRONZE_FEEDBACK_OUT` overrides the baked-in path, which the CLI makes
absolute at build time.

## File format (version 1)

UTF-8 text, one record per line, fields separated by tabs. In names, `\`, tab,
newline and CR are escaped as `\\`, `\t`, `\n`, `\r`. Lines starting with `#`
are comments.

```
bronze-type-feedback 1
fn      <function key>  <entries>
site    <ordinal>  <kind>  <total>  <tag>=<count> <tag>=<count> ...
shape   <ordinal>  <count>  <shape name>
target  <ordinal>  <count>  <callee name>
```

Functions are sorted by key and sites by ordinal; a site's tags are in tag
order, and the names are largest count first (ties by name), at most 8, then
`*other*` for the rest. `shape` and `target` rows follow their `site` row. An
`entry` site has no tags; its total is the entry count, which is also the
`fn` row's number. A site that never ran has no row, so "absent" means "not
reached on this run", "changed", or "not a site".

The reader (`spec::TypeProfile::parse`) is strict: a wrong version, an unknown
row, a malformed count or a row outside a function is an error naming the
line. `serialize()` reproduces the runtime's bytes, and the CLI test checks
that round trip on a real run.

## Query API (`src/opt/spec/profile.h`)

```cpp
auto profile = spec::TypeProfile::load(path, &err);
std::vector<std::string> keys = il::feedbackFunctionKeys(module);   // by function index
for (const il::FeedbackSite& s : il::enumerateFeedbackSites(module, fn)) {
    const spec::SiteFeedback* fb = profile->site(keys[fnIndex], s.ordinal);
    // fb->tags, fb->tagMask(), fb->monomorphic(), fb->soleShape(), fb->soleTarget()
}
profile->entryCount(keys[fnIndex]);  profile->isHot(keys[fnIndex], minEntries);
```

## Consumers (`--feedback-in`)

A profile's keys name sites of the final IL of a build without a profile. The
build that reads it changes that IL (a direct target planned before inlining
moves its callers' hashes), so it cannot look its own sites up by key.
Lowering instead runs its final IL passes twice
(`Lowerer::finishIlPasses`): first on a copy of the module, the PROBE, whose
final IL is the recorded build's, where `planFeedback` reads the profile and
refiles every observation under an identity the real run shares:

- a property or method site by its inline-cache index (every copy of an
  instruction keeps it: inlining, versioning, guarded regions);
- a `call.dynamic` by its span, and a value operand by (span, op, operand
  position): an inlined copy keeps its callee's span.

Copies' observations are summed. Then the passes run on the module, spending
the plan where each claim has to be made:

- **B, direct targets** (right after `predictDirectTargets`, before the passes
  that copy bodies, so typed entries and inlining see them): a `method.call`
  with one profiled callee and no planned target gets that callee as its
  direct target, on the terms of a whole-program one; the site's method cache
  is the guard (its direct entry names the code only for a receiver whose
  lookup finds it). A `call.dynamic` with one profiled callee is split on
  `closure.is` (`applyClosureCalls`): the direct entry on a match, the generic
  call on the other side. From the receivers a method site saw: when their
  key lists hold the key (an own slot, the method cache's SLOT form, which a
  direct target's guard never matches), one callee becomes an own-slot callee
  (`applyOwnCalleeCalls`) and several get `kMethodOwnCallee`, the SLOT form
  dispatched at the site; two or more shapes set `kMethodPolyShapes`
  (`poly-recv` in `bronze il`), so a way-0 miss tests the cache's way 1 at the
  site before the method stub. A predicted direct target at a site whose run
  called a native (a name with no `@hash`, such as `array.push`) is dropped:
  its guard would miss every time. The callee may declare fewer parameters
  than the call passes: when it reads neither `arguments` nor a rest
  parameter, the direct edge drops the extra arguments (three.js's empty
  `onBeforeRender(renderer, scene, camera, ...)`), and a short call pads
  with `undefined`. A callee that never reads `this` is called without it.
- **A, static-slot claims** (after `wpo::optimizeForBuild`, before the layout
  facts spend claims): a `prop.get`/`prop.set` the whole-program pass left
  unclaimed, whose receivers had one dominant shape, claims the key's slot in
  that shape: a family claim when the shape's key list matches class families
  that all have the key at that slot, else an identity claim with a static
  cell of its own. Both are guarded as wpo's are: the runtime publishes a cell
  only after checking an own data slot there, and refuses a store's cell on a
  shape used as a prototype. An identity cell latches the first receiver's
  shape, and that may be an object still being built (three.js's
  `materialProperties`, whose `uniforms` site first meets the record before
  its last keys). Where that can happen, a miss on a published cell tests,
  out of line, whether the receiver's shape grew from the cell's shape one or
  two keys on (its parent or grandparent is the cell); if so the site clears
  the cell and publishes again for the receiver (`emit_static_regrow`,
  `il_static_slot.cpp`). The runtime's publish checks are unchanged, and any
  other miss keeps the cell. The test is emitted only for cells marked
  `staticRegrow` (`@N regrow` in `bronze il`): an identity claim whose profile
  also saw a shape the claimed one grew from by one or two keys (same
  prototype, its key list a prefix), and wpo's identity claims on one
  allocation site whose shape it could not fix; every other cell's miss
  tests only for an empty cell.
- **C, number hints**: an arithmetic, compare, truthiness or unbox op whose
  every observed Dynamic operand was a Number gets `numberHint` (`!num` in
  `bronze il`). The backend then lays the Number test first and moves the
  generic path to cold blocks (sunk to the function's end); it adds
  Number-first arms for truthiness, `==` and unary `-`. The value's
  representation does not change. A truthiness `unbox` whose every observed
  operand was a Boolean, an object, undefined or null gets `truthHint`, the
  set of those kinds it saw (`!truth(boun)` in `bronze il`): the backend
  tests exactly those kinds first, each with its answer (a Boolean's low
  bit, an object true, undefined and null false), and lays the rest of
  ToBoolean out cold. This covers presence tests (`if (material.map)`),
  flag fields, and the joins of `a && b`, whose profile is usually
  {Boolean, undefined}.
- **E, inline way chains** (with A, after `wpo::optimizeForBuild`): a
  `prop.get` of a named key whose receivers were one to four plain shapes
  (each at least 1%, no `*other*`, no dictionary) gets `icChain`
  (`chain N[+proto]` in `bronze il`): the backend compares the receiver's
  shape with that many of the site's cache ways inline, in fill order, and
  answers from the way that matches, an own data slot, a negative entry at
  the current prototype epoch (`undefined`), or with `+proto` (some receiver
  lacked the key and the read returned something else) a data slot one
  prototype link up at the current epoch. The ways are laid out by what the
  profile saw answer: when more reads met a receiver lacking the key than one
  holding it, the past-the-own-slots path is the fall-through (`notown-first`),
  and when the prototype answered most of them, rather than `undefined`, the
  one-link-up read comes before the negative entry (`proto-first`). Only the
  order changes; every test still runs. Everything else is the property
  stub, laid out cold (`il_prop_get_chain.cpp`), through its move-to-front
  entry: a mega-table answer at a full site goes into way 0, so a site whose
  ways warm-up shapes filled comes to hold the shapes it meets now. A site
  of one shape that A already claimed is left to its claim.
- **D, profile-guided regions** (after inlining and the own-slot calls,
  before transition runs, define-add cells and the claims;
  `src/lower/guard_region.h`, "PROFILE-GUIDED REGIONS"): the guarded-region
  pass runs again with the profile. A region is seeded only by ops every
  observed operand of which was a Number (arithmetic, checked unboxes, and
  relational / `==` / `===` compares), its seeds must have run
  `kFeedbackHotExecutions` (256) times, and an operand seen as anything else
  poisons its value. The fast copy carries the values as doubles: arithmetic
  is f64, a compare of two such values is `cmp.*` with a Bool result, loop
  phis are f64, and nothing on those paths calls a helper or boxes. A guard
  that fails jumps into the slow copy (the original IL) after the last op
  that ran, so nothing runs twice: the re-execution licence is the static
  pass's. Under the profile, loops whose split lands below a join get their
  slow header's parameters repaired, caps are doubled, and the slow copies
  and trampolines are marked `cold` (`il::Block::cold`, laid out after the
  fast path by brass). Regions the static pass built get the same `cold`
  mark when the profile shows their slow copy idle: no non-Number seen in
  it, and its busiest site ran at most 1/16th as often as the function was
  entered (`FeedbackPlan::entries`). `BRONZE_GUARDED_REGION_STATS=1` prints a
  `[guard-fb]` line; `BRONZE_GUARDED_REGION_TRACE=1` per-function
  `[guard-fb-fn]` lines.

Thresholds (`feedback_plan.h`): a site needs `kMinExecutions` (32)
executions; a shape or callee needs `kDominantPercent` (99%) of them.
`--wpo-stats` prints `feedback:` lines: sites matched, and per consumer the
sites taken and the profiled executions they cover.

A wrong profile costs speed, never an answer. The `oracle-feedback` ctest
(label `feedback`) builds each oracle case with `--feedback-out`, then on its
own profile and on an ADVERSARIAL one (every value site numbers only, every
shape's keys rotated so each slot guess is wrong, every callee another
site's, each with 1000 executions), and requires the pinned bytes from all
three. `BRONZE_ORACLE_FEEDBACK_CASES=<substr>[,...]` runs a subset.

## Report

`bench/tools/feedback_report.py <file.tfb> [--top N] [--json]` prints, as
shares of executions: dynamic-op operands with one tag (and with numbers
only), inline-cache executions with one shape, and call executions with one
target, overall and for the hottest functions by entry count.
