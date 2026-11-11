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
| inline-cache harvest: identity digest | `src/il/feedback_site.{h,cpp}` (`icLayoutDigest`) |
| inline-cache harvest: identity record | `src/abi/bronze_abi_layout.h`, `src/codegen-brass/brass_backend_sections.cpp` (`emitIcIdentity`) |
| inline-cache harvest: reader of the caches, writer | `src/runtime/ic_harvest.{h,cpp}`, `src/runtime/ic_hot.cpp` |
| inline-cache harvest: plan | `src/opt/spec/feedback_plan.cpp` (`planHarvest`) |
| adversarial-profile and harvest suites | `tests/oracle/oracle_feedback_test.cpp` |
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
  keys are the own keys in insertion order, every one of them (version 1
  showed 32, then `+N`). A dictionary
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

## File format (version 2)

UTF-8 text, one record per line, fields separated by tabs. In names, `\`, tab,
newline and CR are escaped as `\\`, `\t`, `\n`, `\r`. Lines starting with `#`
are comments. Version 2 differs from version 1 in two ways: a shape name lists
every key, and a file may carry a harvest (below). The reader takes both
versions.

```
bronze-type-feedback 2
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
- **F, accessor targets** (right after lowering's own accessor guess,
  `resolveAccessors`, before guarded inlining, so the inliner copies the
  getter or setter in): a `prop.get` / `prop.set` of a named key with no
  slot claim and no direct target, whose receivers had one dominant shape
  whose key list lacks the key, gets the accessor half that shape's class
  resolves the key to as its `directTarget`, with an accessor cell pair of
  its own (il.h). A list a version-1 recorder cut short (`,+N`: it named 32
  keys) counts as lacking a key it does not name; a receiver holding the key
  among the rest is the guard's to refuse. The class is the one the shape's
  prototype is named by (its constructor's `name`): every class declared under that
  name must resolve the key, at or above it, to the same getter (or setter)
  that an accessor site's hit can call (`accessorOfShownClass`,
  `direct_method_table.cpp`); a name no class has, a `^Name` prototype, or
  classes that disagree get nothing. This is what lowering stamps when
  inference knows the receiver's class, for receivers only the run saw
  (parameters, array elements, `this` of methods unknown code calls). The
  guard is the cell's: the runtime publishes (shape, prototype epoch) only
  after resolving the key on a plain receiver's chain to an accessor whose
  half has this function's code (`runtime/accessor_cell.cpp`), so a
  receiver with the key as its own, a subclass that overrides it, an
  accessor redefined or a prototype replaced (both move the epoch), or a
  receiver that is no plain object takes the site's ordinary access.
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

## Harvest (`bronze build --harvest-out <file.tfb>`)

An ordinary build's inline caches already hold what its property and method
sites met. A HARVEST reads them back when the run ends and writes them as a
profile `--feedback-in` reads. No site is instrumented and nothing is counted
on a hot path, so the harvested build is the build that ships.

```sh
bronze build app.js -o app --harvest-out app.tfb      # 1. an ordinary build
./app                                                 # 2. a run; app.tfb at exit
bronze build app.js -o app --feedback-in app.tfb      # 3. the build that reads it
```

**What is read.** A module's IC table (`__bronze_ic_table`, 12 words a site)
is registered per (thread, realm) at module init by `bronze_register_ic_hot`,
and its method sites by `bronze_register_method_ic_rows`. Both registrations
already existed. The harvest folds a table when its instance is dropped (a
realm released, a reload resetting the image) and at the end of the run. A
property site's four ways give `<form> <receiver>` per way:

- `own`, `proto`, `absent` or `accessor`, from the depth word's flags, or
  `store` for a write way a store filled;
- `array`, from the array-method sentinel.

A method site's two ways give `<form> <receiver> <callee>`:

- `direct`: a receiver shape and the code it calls. The callee is named
  `~<source offset>@<fnv32 of its source>`, which the plan matches against an
  IL function's `sourceBegin`;
- `slot`: an own-slot entry, whose callee is `*other*`;
- `native`: an exotic receiver, named by kind.

Two flags record that a site met more receivers than its cache holds:

- `full` marks a read whose four ways are all in use;
- `mega` marks a write or call whose stub's mega table answered.

Receivers are named as the recorder names them, with every key. Writing at
exit reads only what was already folded and never walks the heap.

**Identity.** A harvest names sites by IC index, so it is valid only for the
build whose indices it names. Lowering computes `il::icLayoutDigest` before
the final threading pass. The digest is FNV-1a over:

- the source texts;
- the IC site count;
- for each index, the op and key of the first instruction that carries it.

The build bakes the digest into an IC IDENTITY record, `<entry>_ic_identity`
(`bronze_abi_layout.h`, version 2): version, flags, site count, digest, the
module's code-range table and its length (for the samples, below), and the
baked harvest path. The record is passed to `bronze_register_ic_hot` as its fourth
argument. The file's `harvest` row carries the digest and site count.

A `--feedback-in` build compares them with its probe's. The probe runs the
same passes as a build without a profile, so the indices are the same. A
build from other source, another compiler, or other options gets a different
digest. Its harvest is then reported as `ignored` and changes nothing.

A build that spent a profile has the `PROFILED` flag set and is never
harvested. Its inlined copies number their sites differently, and its claimed
sites never fill their caches. Harvesting it again would also be a loop.

**How sites map onto the thresholds** (`planHarvest`). A cache records which
receivers a site met, not how often. Each harvested site therefore counts as
`kHarvestExecutions` (1024) executions, which is past every threshold, split
evenly over the receivers it held.

When the cache says the site met more receivers than it holds, half of the
executions go to `*other*`. No consumer claims `*other*`. This applies to:

- a full read;
- a write whose mega counter is non-zero;
- a mega method site.

A write cache holds the shape after a transition. A write way filled by the
add that made its key (form `own`, the key the shape's last) may have met only
receivers one key short, so half of that receiver's share also goes to
`*other*`. A way filled by a store to a key the receiver had is harvested as
`store` (`InlineCache::fillForStore` marks it in the epoch word) and keeps its
share. A write that met a shape and the one it grew from by one or two keys
counts as the grown shape's, the other keeping one execution, which is what
gives the claim its regrow. The result:

Before any claim, `addProfiledFamilies` (`feedback_apply.cpp`) reads layout
families off the plan: where a property site met several shapes that all begin
with the keys of the shortest (a base class's method run on the base and its
subclasses, whose constructors the compiler could not model), that key list
joins `classFamilies`, the runtime stamps every shape that begins with it, and
the site is claimed by one range test (consumer A's family form) in place of a
chain. A list related to a family the lowering proved is left out.

- a single held receiver is dominant (consumers A and F);
- two or three receivers are a chain (E). Four receivers fill the cache, so
  that site is marked `full`;
- two receivers at a method site get poly receivers or a direct target (B).

**What it cannot see**, and what that costs:

- **Value sites**, so no number hints (C) and no profile-guided regions (D).
  The table below measures this cost.
- **Entry and execution counts.** A site that ran once looks as hot as one
  that ran a million times, so the plan may spend code size on cold sites.
  (The samples, below, say which functions the time went to; only function
  placement reads them.)
- **`call.dynamic` callees.** There is no call cache, so there is no closure
  split.
- **Sites that never fill a cache.** These are sites already answered by a
  static claim, a layout fact or a typed path, and dictionary receivers.
- **Own-slot callees.** These appear as `*other*`.
- **Callees without source.** The callee is named `*other*` in a build that
  registers no function source.

**Samples** (`runtime/harvest_sampler.{h,cpp}`). While a harvest is armed, a
sampler thread arms the JS thread's interrupt every 250 us, and the next
interrupt poll the thread reaches (a prologue's stack check or a loop's back
edge, both in every compiled function already) notes the compiled function it
polled from; at a prologue the first JS function among its callers is noted
too, since the time before the call was the caller's. A poll in a stub's loop
(a sort's merge) passes the sample on to the next poll, the comparator's
entry. Nothing is added to the code. The writer files each count under the
module whose code-range table holds the function, by its descriptor's
`name_hash` (FNV-1a of the IL name), and `planHarvest` matches the hashes to
the probe's functions (`FeedbackPlan::samples`). A stale harvest gives no
samples.

The sampler wakes only while JS runs: after arming it sleeps with no timeout
until a poll takes the sample, so an app that is idle, blocked or in the
background causes no wakeups. It exits after 10,000 samples, when no harvest
is armed, and once the harvest is written. The interrupt it rides is the
stack-limit word every target's compiled code checks, so it samples on every
platform; MSVC builds credit only the polling function (no frame-pointer
walk to the caller).

**Configuration.** Harvesting is off unless a path is given. A path can be
given in three ways:

- `bronze build --harvest-out <file>` bakes the absolute path into the
  identity record. The program writes the file at exit (atexit, and from
  `bronze_host`'s main after the entry).
- An embedding host sets `eval::EvalOptions::harvestOutPath` for
  `eval::runModule`. That call passes the loaded module's identity to
  `embed::harvestInlineCaches(identity, path)`, which returns false for a
  profiled module.
- A host calls `embed::harvestInlineCaches` itself.

`embed::writeInlineCacheHarvests()` folds the live instances on the calling
thread and writes every file now. Call it before leaving through `_exit`,
when a page is hidden, or before a reload. The reverse direction is
`eval::EvalOptions::feedbackInPath`: `buildModule` reads that profile or
harvest, and the module's key includes the file's text. The module's result
global is named from the key *without* the profile, so a build on a harvest
has the same property keys, and therefore the same digest, as the build the
harvest came from. `ModuleBuildResult::feedbackApplied` says whether the
build used the profile.

**Format.** A harvest is a version 2 file with these rows (any `fn` rows
come first):

```
harvest <16 hex digest>  <IC site count>
ic      <index>  property|method  -|full|mega|full,mega
way     <index>  <form>  <receiver>[  <callee>]
sample  <8 hex name hash>  <count>
```

**Tests.** The `oracle-harvest` ctest (label `feedback`) builds each oracle
case three ways and requires the pinned bytes and exit code from each run:

1. plain with `--harvest-out`, then run;
2. on that harvest;
3. on an adversarial harvest, in which every site holds one way (so it reads
   as monomorphic with every threshold met), every receiver's keys are
   rotated, and every method site calls another site's callee.

`BRONZE_ORACLE_FEEDBACK_CASES` selects the cases. `aot-module` runs the
evaluator path, and `spec` covers parsing and round trips.

## Function placement (`src/lower/function_placement.{h,cpp}`)

Where each function sits in the module's text is decided from the profile,
not left to module order (brass `FunctionPlacement`, set by
`brass_backend_placement.cpp`). Laid out in module order, a program's hot
functions are scattered over megabytes; which of their lines share L1
instruction-cache sets and op-cache lines is a draw that every edit anywhere
in the module redraws (a few percent of cycles at equal instructions on the
realworld scenes, from padding alone). The text is laid out as:

1. the IC stubs every module calls (`__bronze_prop_get_ic` and its kin);
2. with a profile: the most sampled functions to 99% of the samples
   (samples plus a recorded profile's entry counts), in C3 call chains, then
   the functions they call that ran but were not sampled hot, in module
   order; each function is followed by its call wrapper;
3. the slow-path stubs (`__bronze_prop_get_slow`, `__bronze_sort_merge`);
4. everything else, in module order;
5. last: top-level code (it runs once), and with a profile the functions
   whose cache sites could have filled and none did.

Without a usable profile (none, a stale harvest, or a run too short to be
sampled) there is no placement at all: module order, as before. Moving only
the stubs and the top level measured +1.5% and +2.8% on pixi_sprites and
nothing better elsewhere. Placement never changes an instruction. The hot functions keep the
target's alignment (32 on x64): 64 bought nothing measurable, 16 was 7%
slower on pixi_sprites (loop heads are aligned from the function's start).

What it cannot fix: a hot function's own cold code. On the realworld scenes
the lines that take 99% of a frame's cycles are 30 KB (pixi_sprites) to
140 KB (three_skinned), inside 1.4 to 3.5 MB of hot functions, because a
compiled function carries its slow paths inline. Where those lines fit the
L1 instruction cache (pixi_sprites) placement takes 5% off the frame and
holds it under edits elsewhere; where they do not, the order of whole
functions moves the frame by a couple of percent either way, and splitting
the slow paths out of the hot functions is the remaining lever.

## Report

`bench/tools/feedback_report.py <file.tfb> [--top N] [--json]` prints, as
shares of executions: dynamic-op operands with one tag (and with numbers
only), inline-cache executions with one shape, and call executions with one
target, overall and for the hottest functions by entry count.
