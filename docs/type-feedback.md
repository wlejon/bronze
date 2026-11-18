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
| harvest suite | `tests/oracle/run.py` (modes `profile`, `switch`, `adversarial`) |
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
  kind (`array`, `function`, `number`, ...). A key is spelled `!k` when it
  is a data property that is not writable and `&k` when it is an accessor
  pair, so a reader knows which slots it may store into and where the slot
  rule (one slot per data property) holds. Shapes that differ only in
  enumerability, configurability or slot representation share a name;
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

## File format (version 9)

UTF-8 text, one record per line, fields separated by tabs. In names, `\`, tab,
newline and CR are escaped as `\\`, `\t`, `\n`, `\r`. Lines starting with `#`
are comments. Version 2 differs from version 1 in two ways: a shape name lists
every key, and a file may carry a harvest (below). Version 3 adds the
harvest's `other` row; a version 2 harvest has none, and a build on one claims
as it did before the row existed. Version 4 adds the harvest's `slowsites` and
`slow` rows ("Slow paths", below); an older harvest has none, and a build on
one keeps every fallback in line. Version 5 adds the shape turns among the
harvest's `slow` rows and its `last` rows ("Phases", below); a build on an
older harvest reads no site as a phase. Version 6 adds the `callee` row (a
call's or a `new`'s last directly entered callee, below); a build on an older
harvest plans a plain call from a recorded profile only. Version 7 adds the
getter and setter names to a harvest's `accessor` ways (below); a build on an
older harvest reads every accessor way as naming neither, and leaves those
sites generic. Version 8 adds the holder's depth and slot to a harvest's
`proto` ways (below); a build on an older harvest leaves every read with a
`proto` way generic. Version 9 drops the harvest's `sample` rows (a timer
sampler's counts, now each function's loop count among the `slow` rows,
"Which functions are hot" below); the reader skips them in an older file. The
reader takes all nine versions.

```
bronze-type-feedback 9
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
- **A, static-slot claims** (after `wpo::optimizeForBuild`, before the
  regions of inlined loops and the layout facts, which both spend claims): a
  `prop.get`/`prop.set` the whole-program pass left unclaimed (a site it
  predicted absent included), whose receivers had one dominant shape, claims
  the key's slot in that shape: a family claim when the shape's key list matches class families
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
  tests only for an empty cell. The pass also hands back the claimed sites
  the run saw miss (`missedClaims`): a claim, its own or wpo's, whose profile
  has an `*other*` row or a shape it cannot read, a second receiver name
  under an identity claim, more cached ways than receiver names
  (`FeedbackPlan::shapeVariantIcs`: two layouts of one key list, of which an
  identity cell holds one), the key at another slot, or a shape outside the
  claimed family range. The region pass gives every other claimed site of a
  re-entered loop a shape test that leaves the fast copy
  (`src/lower/guard_region.h`, "SHAPE GUARDS"), and leaves these in line.
- **A2, layout cells** (in `finalPasses`, after the regions and before
  `foldGuardedLoads`; `applyFeedbackLayoutCells`): in a function the harvest
  calls hot, the claimed sites and `is.shape` tests on one receiver under one
  claim whose profiles all hold the same single shape, with its whole key
  list, move to that shape's layout cell (`il::Module::layoutCells`, one per
  shape name), which the runtime publishes for the listed (key, slot, written)
  pairs together. A site that is a missed claim, a shape variant, a direct
  target, a define or a profile's own regrow keeps its claim, and so does a
  group one of whose sites does. A site that had its slot before the profile
  was read (a class family's range, a whole-program stamp's cell, regrow or
  not) is a member like any other where the harvested build ran it through
  its cache (`--harvest-out`, "Phases"). In a harvest of a build that kept
  the stamps' own guards (`eval::runModule`'s), such a site's cache holds
  only what missed its guard, so its group takes the shape the receiver's
  profiled sites met instead, where they all met one, each of the group's
  keys is at its slot in it, and one of them is in the site's block or one
  that dominates it. So a receiver read through a family's range, a stamp's
  cell and a profile's claim is tested once. Report line `feedback-layout`.
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

A wrong profile costs speed, never an answer: a guess the run does not bear
out leaves to the generic code. `tests/oracle/run.py --modes adversarial`
holds every oracle case to it: it derives three wrong harvests from the
case's own (receivers' key lists rotated or borrowed from other sites, way
forms exchanged, proto ways' depth and slot moved, slot representations
flipped, sites and calls that ran made to look unrun; every site one way,
callees exchanged across sites and arities; slow-site counts toggled, whole
sites' rows exchanged; every function's entry count made hot, and some
loop counts, in all three), builds on
each and requires the pinned output and the harvest run's exit status.

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
  `store` for a write way a store filled. An `accessor` way also names the
  getter and the setter of the accessor it found above the receiver,
  `accessor <receiver> <getter> <setter>`, named as a method way's callee
  (below): the harvest walks the way's depth from its receiver shape to the
  holder as the chain stands at the end of the run and reads the pair at the
  way's slot. `*other*` for a half that is no function, an accessor own on
  the receiver, a function receiver's statics, or a holder whose shape has no
  accessor at that slot (the chain changed since the way was filled). A
  `proto` way names where it found the data property, `proto <receiver>
  <depth> <slot>`: the links from the receiver to the holder and the
  holder's slot (neither for a function receiver's statics chain);
- `array`, from the array-method sentinel.

A method site's two ways give `<form> <receiver> <callee>`:

- `direct`: a receiver shape and the code it calls. The callee is named
  `~<source offset>@<fnv32 of its source>`, which the plan matches against an
  IL function's `sourceBegin`;
- `slot`: an own-slot entry, whose callee is `*other*`, or
  `intrinsic:Math.<name>` when the code is a Math function's native
  (`rtMathNativeName`), which `planIntrinsic` reads as a `math.*` call;
- `native`: an exotic receiver, named by kind.

Either form names a collection's `get` or `has` native `intrinsic:Map.get`,
`intrinsic:Map.has`, `intrinsic:Set.has`, `intrinsic:WeakMap.get` or
`intrinsic:Weak.has` (one native for WeakMap and WeakSet;
`rtCollectionNativeName`). A method site of one `direct` way from a class's
instance shape (`Map{}`, `Set{}`, `WeakMap{}`, `WeakSet{}`) whose callee is the
native the site's key names there becomes `is.collection` + `collection.op`
(`opt/speculate/collection_call.cpp`): the receiver's shape is compared with
the class's instance shape (the TLS block's `collection_shapes`), and the
call is the native's, made by `bronze_collection_op` without the method
read or the argument block. Lockdown froze the prototypes, so at that shape
the key can only read the native; a receiver at any other shape leaves in
front of the site.

A read site all of whose ways are `accessor` with named getters, a write site
whose ways name setters, and a method site whose ways are all `direct` become
MEMBER CELL sites in speculation (`opt/speculate/member_call.cpp`,
`evidence_member.cpp`): one cell per way (il.h `Module::memberCells`, two
words a cell in `__bronze_member_cells`), a shape test per cell, and a direct
call to the way's function, which the inliner can copy in. A cell holds a
receiver shape only while the runtime's check stands
(`bronze_member_cell_publish`, `runtime/accessor_cell.cpp`): a plain receiver
of that shape lacks the key as its own and its chain resolves the key, at the
prototype epoch, to a function (or an accessor half) with the cell's code; a
bump of the epoch empties it. A receiver no cell holds leaves in front of the
site, after asking the runtime to fill the site's cells for it, and G does the
generic access. A function that reads its environment record is not called
this way (the cell has no function object to give it one).

A read site with `proto` ways that name their holders (and any `own` or
`absent` ways beside them) is a DATA member site (`opt/speculate/member_data.cpp`):
an `own` way is a shape cell's test and the receiver's slot, an `absent` way
an absent cell's test and `undefined`, and a `proto` way a data cell's test,
then the walk of its depth from the receiver's shape (`shape.proto`, the root
shape's prototype word) and the holder's slot. A data cell holds a receiver
shape while its chain finds the key, at the prototype epoch, as a data
property of a plain holder at the way's depth and slot. A write of the value
bumps nothing and needs nothing: the slot is loaded each time. A `full` read
is planned too, with a generic arm for the receivers its ways do not name,
unless it has `own` ways, whose receivers the slot-keyed form answers better.

Two flags record that a site met more receivers than its cache holds:

- `full` marks a read whose four ways are all in use;
- `mega` marks a write whose stub's mega table answered, or a call both of
  whose ways missed at least `BRONZE_ABI_METHOD_IC_MEGA_RELATCH` (64) times:
  a call site that met a third receiver a few times while starting up, or
  whose entries a prototype change made stale once, is still the two
  receivers it holds (three.js's `resolvedProperty.fromArray(...)`, a
  Quaternion or a Vector3).

Receivers are named as the recorder names them, with every key. Writing at
exit reads only what was already folded and never walks the heap.

**Receivers no way can hold.** A way holds a plain object's shape (or, for a
read, the array-method sentinel). A receiver that is anything else never fills
one: an Array read for a name that is not an array method, a primitive, a
function, a typed array, a RegExp, a dictionary-mode object, or a method
receiver the call latched nothing for. A site that met one plain object once
and Arrays ever after would read as that one shape, and a claim's guard (which
passes plain objects of the claimed shape only) would send the function to its
resume twin on nearly every call, and to its generic entry for good
(eventemitter3's `listeners.fn`, an `EE` with one listener and an Array with
more). So while a harvest is armed, the read, write and method-call helpers
count, per site, the calls whose receiver the site's ways did not come to hold
(`rtIcHarvestNoteUncached`, `runtime/ic_harvest.h`): a read whose receiver is
not a plain object a way names after the read (a read whose four ways are all
in use is `full` already), a write whose receiver is not a plain object of a
shape-tree shape, a method call that latched nothing. Only sites of a
registered IC table count (not a helper's stack scratch site); the counts are
thread-local and folded with the site's table. Inline-cache hits never reach
the helpers, so a receiver the cache holds is never counted, and the run's
shapes and caches are a plain build's. A site that met such receivers gets an
`other <index> <count>` row after its ways (a site with no ways gets none:
nothing is claimed there anyway). In `planHarvest` a count of at least
`kHarvestUncachedMin` (64) makes the site one that met more than its ways
hold, as `full` does: half its executions go to `*other*`, so it is neither
claimed nor chained and keeps the plain build's inline cache. Fewer is a
receiver met now and then (a start-up path), whose exits a twin absorbs.

A read whose receiver expression names a class (`icFnRecv`,
`Klass.defaultOptions`) is never claimed from a harvest: its ways name the
class's statics box, the fn-recv arm reads it inline (so the helper sees it
once), and a claim's guard would refuse the function receiver on every call.

**Identity.** A harvest names sites by IC index, so it is valid only for the
build whose indices it names. Lowering computes `il::icLayoutDigest` before
the final threading pass. The digest is FNV-1a over:

- the source texts;
- the IC site count;
- for each index, the op and key of the first instruction that carries it.

The build bakes the digest into an IC IDENTITY record, `<entry>_ic_identity`
(`bronze_abi_layout.h`, version 4): version, flags, site count, digest, its
slow sites' count (below) and the baked harvest path. The record is passed to `bronze_register_ic_hot` as its fourth
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
- a mega method site;
- a site whose `other` row counts at least 64 receivers no way could hold.

A write cache holds the shape after a transition. A write way filled by the
add that made its key (form `own`, the key the shape's last) may have met only
receivers one key short, so half of that receiver's share also goes to
`*other*`. A way filled by a store to a key the receiver had is harvested as
`store` (`InlineCache::fillForStore` marks it in the epoch word) and keeps its
share. A write that met a shape and the one it grew from by one or two keys
counts as the grown shape's, the other keeping one execution, which is what
gives the claim its regrow. The result:

**Phases.** A build made to be harvested (`--harvest-out`) differs from a
plain one in two ways. Every named read and write goes through its cache: a
site with a slot of its own (a class family's, a whole-program stamp's) is
lowered as if it had none (`il_to_brass_ast.cpp`, `cacheEvidence`), because
its guard's hits never touch the cache and the harvest would hold only what
missed the guard. And in front of each such site the build keeps the SHAPE
TURNS of its receivers in two of the site's slow-site words
(`il/feedback_site.h` `kLastShapeArm`, `kShapeTurnArm`;
`PropertyLoweringHelper::count_shape_turn`): the last receiver's shape, with
bit 63 set so the harvest leaves the word alone, and the count of receivers
whose shape was not that one, a few instructions and no branch. The harvest
names the first word's shape in a `last` row, by the word's slow-site index
(`*other*` for a receiver that was no plain object). The order of a cache's
ways does not say which shape came last: a site behind a recursion meets a
child first, then its parent once, then children for good.

A `call.dynamic` or a `new` keeps CALLEE TURNS in the same two words, at its
own slow site: the code pointer of the callee its code entered directly, with
bits 63 and 62 set (`kLastCalleeBit`), and the count of entries whose callee
was not the last one's. The harvest names the callee in a `callee` row
(`*other*` for a native); a call the helper made counts the slow arm. A
harvest `planCall` reads as one callee when the site turned once and its
helper never ran. The same build marks a dynamic numeric op's operand kinds
as arms (il2mir `il_lowering_numeric.cpp`: arithmetic, `%`, the bitwise ops
and shifts, unary `-` and `~`, the relational compares, `==`, `to.numeric`
and the loop step; `kArithArmNumbers`, then per operand a Number that was no
int32, then `kArithArmResultDouble` for a Number result that was no int32),
which `planArith` reads when no recorded value site exists. It also splits an
element access's typed-array arm by element kind (`kElemArmView` for
Float32, `kElemArmViewF64`, `kElemArmViewI32`, `kElemArmViewOther`) and marks
`kElemArmMiss` for a view index past the length. An Array read whose array
holds anything but Numbers (its NUMBERS bit clear and not claimable,
runtime/array.h) marks `kElemArmNoNumbers`; a read that never did is planned
on the bit (`is.dense_array` with `kDenseNumbers`, no hole or number test
after the load). Every build that counts
slow sites marks an element access's append (`kElemArmGrow`, which keeps
`planElem` off a write that grew its Array) and an Array read that found no
element (`kElemArmMiss`), and a `.length` read's header answers
(`kLengthArmArray`, `kLengthArmString`, `kLengthArmView`): `planLength` reads
an Array's alone as `array.length`. An element access and an index-named
`o[0]` (a `prop.get` / `prop.set` of an index key) plan alike.

`planHarvest` reads a site that held several shapes, none of them through
`*other*`, as a PHASE when its turns are at most `kPhaseTurns` (16; a site
that ran counts its first receiver, and copies of a site that inlining made
share the count) and its `last` row names one of them: the shapes came one
after another, and the site is the last one's alone. pixi's `Container._onUpdate` runs for every sprite while its
constructor's `b.x = ...` sets a position, before `vx` and `vy` are added, and
then on every frame for finished sprites; its sites held both shapes and
turned twice. Read as the finished shape's, they are members of that shape's
layout cell with every other site on the receiver. The claim's guard leaves
for an unfinished object, so the functions that build the objects leave their
speculated code for their generic entries (il_speculation.h, "FAILURE
POLICY") and the ones that run after keep one compare. A site whose shapes
alternate for good turns each time and stays a chain. The runtime weighs an
unfinished receiver at a layout cell's publish (`static_shape.cpp`, fewer keys
than the cell's count) a 64th of a wrong one, since every object of the run
may pass a member site before the first finished one does.

Before any claim, `addProfiledFamilies` (`feedback_apply.cpp`) reads layout
families off the plan: where a property site met several shapes that all begin
with the keys of the shortest (a base class's method run on the base and its
subclasses, whose constructors the compiler could not model), that key list
joins `classFamilies`, the runtime stamps every shape that begins with it, and
the site is claimed by one range test (consumer A's family form). A list related to a family the lowering proved is left out.

- a single held receiver is dominant (consumers A and F);
- four receivers fill the cache, so that site is marked `full`;
- two receivers at a method site get poly receivers or a direct target (B).

**What it cannot see**, and what that costs:

- **Value sites**, so no number hints (C) and no profile-guided regions (D).
  The table below measures this cost.
- **Execution counts of property sites.** A property site that ran once looks
  as hot as one that ran a million times, so the plan may spend code size on
  cold sites. Calls, function entries and loop iterations are counted (CALL
  COUNTS, LOOP COUNTS, below), and those say which functions the work went
  to.
- **`call.dynamic` callees.** There is no call cache, so there is no closure
  split.
- **Sites that never fill a cache.** These are sites already answered by a
  static claim, a layout fact or a typed path. (Receivers a cache cannot
  hold are counted, above, but only at sites some way filled.)
- **Own-slot callees.** These appear as `*other*`.
- **Callees without source.** The callee is named `*other*` in a build that
  registers no function source.

**Call counts.** Every `call`, `call.dynamic`, `method.call` and construct
has a slow site (`il::numberSlowSites`), and a build made to be harvested adds
one to its `kCallCountArm` word (`il/feedback_site.h`) as the call starts: a
load, an add and a store (three_skinned's harvest run: +0.8% instructions).
Each function with a body also has one index of its own, its ENTRY COUNT
(`Function::entrySlowSite`), bumped in its prologue. Both come back as `slow`
rows like any other counter. A direct call the speculation step makes from a
generic call keeps that call's slow site (`opt/speculate/direct_call.cpp`), so
the inliner (`opt/inline/driver.cpp`) knows how often each call it may take
ran, and shares a polymorphic site's count over its arms by their callees'
entry counts. A call made on fewer than 1 in 16 of its caller's entries
(`kRareShare`) is RARE and takes a small callee only: inlined, a first-use
path (`cache[k] || init(o)`) saves a call now and then and is paid for in
the caller's size, which decides whether the caller's own callers take it.
This is what makes speculating a function no worse than leaving it generic
when it runs only on such a path: pixi_sprites' `Sprite.visualBounds` runs only while a sprite's
batch record is made, and its S copy (with `updateQuadBounds` inlined) grew
`_getGpuSprite`'s rare path until `RenderGroup.updateRenderable` passed the
hot-callee limit and stayed a call in the per-sprite loop (16.07M
instructions a frame; 15.12M with the rule, as with `visualBounds` not
speculated). A method call's slot way (a function an own slot held) records
its callee turns too, as a `call.dynamic` does.

**Loop counts.** Each function with a body has a second index of its own
after its entry count (`il::kLoopCountOffset`), its LOOP COUNT: a build made
to be harvested adds one to it at each of the function's loop headers (the
targets of the back edges of a depth-first walk from its entry, il2mir
`lowering_order`), so it counts every iteration and every start of a loop,
inlined loops included. A coroutine body's entry count is bumped at every
resume as well, each stretch of it that ran. Entries and loop runs together
are a function's WORK, and every counted function's summed is the run's.

**Which functions are hot** (`Evidence::isHot`, `opt/speculate/evidence.h`)
is decided from counts alone, so every harvest of a program that runs the
same way decides the same (a timer sampler did this until version 9 of the
format: which functions near its threshold were hot changed from one harvest
to the next, and three_skinned's builds ranged 33.3M to 33.8M instructions a
frame over them). A recorded profile's function is hot at `kMinEntries` (64)
entries or a site that ran `kMinSiteRunsForHot` (4096) times. A harvested
function is hot when the build counted it and:

- the run entered it `kMinEntries` times or more; or
- it was entered (fewer times) and its loops ran at least
  `kMinSiteRunsForHot` times and at least 1 / `kLoopShareDen` (1%) of the
  run's work: a loop the run spent a share of its time in, not a step of its
  start-up (three.js's `WebGLProgram`, 6 entries, whose 1.4 MB of S, S+ and
  twin code a share rule keeps out); or
- it was entered and one of its call sites ran `kMinSiteRunsForHot` times: a
  driver loop whose time is in what it calls (without it the mat4_f32 kernel
  goes from 7.2 to 8.6 ms).

A function the run never entered, or one the build did not count, is never
hot.

The run's addresses decide nothing either. The megamorphic property cache
(`runtime/rt_prop_mega.h`) hashes a shape by `Shape::mega_hash`, the shape's
number among its thread's shapes; hashed by address, which pairs collided
changed with where the arena's chunks landed, and with it which full sites
answered from the table, which shapes their ways held, and the fallback counts
the build reads (`Object3D.updateMatrix` became an S+ clone in some
three_skinned harvests and not others). Two harvests of three_skinned now
differ only in a few allocation and tenure counts, and build the same IL.

**Heat** (`il::Function::heat`, set for every function by the speculation
step from `Evidence::heat`, kept by every copy and serialized): `Never` for a
counted function the harvested run never entered, `Entered` for one entered
fewer than `kMinEntries` times and not hot, `Hot` for the rest (entered
often or hot, speculated or not), `Unknown` without a harvest or for a
function the harvested build did not count. A `Never` or `Entered` function is
COLD: its property, element, method, call and `new` sites lower to their
runtime helpers alone, with no inline-cache test in front
(`PropertyLoweringHelper::cold_code`); the helpers read and fill the same
sites, so a later hot caller's cache is warm all the same. A plain build has
no heat and compiles everything as before. A wrong profile costs speed, never
an answer (the oracle's adversarial mode builds every case on a harvest that
says no function ran).

**Cold functions.** An image built ahead of time on a harvest does not
compile its cold functions: it INTERPRETS them (`codegen-brass/cold_companion.h`).
Each keeps its wrapper and descriptor, and in place of its body has a STUB of
the body's signature (`il2mir/il_lowering_cold.cpp`) that calls through the
function's CELL in `__bronze_cold_cells`, a word the image shares across
threads. The image carries the cold functions' IL (the module with every other
body dropped, and the tables numbered over all bodies that the IL alone cannot
number again: global-read keys, element-cell sites, method sites, descriptor
flags, the retirable speculated functions, the line tables) and a LINK TABLE
of every image symbol their code refers to: compiled functions, descriptors,
data tables, module stubs, thunks. The build lowers that IL once as the run
will, which is how it knows the table and that the IL lowers at all; it does
so on a thread of its own while brass compiles the image
(`codegen::planColdCompanion`), whose largest functions leave most threads
idle at the end, and writes the table once the image's symbols exist. The first
call through an empty cell calls `bronze_cold_resolve`, which builds the
image's COMPANION once per process (the IL lowered to MIR as a program of the
tiered pipeline, its every reference bound to the link table, attached for
stack traces and the profiler as an in-process program is:
`TieredProgramImage::attach`) and fills that cell with its function's address
in that pipeline; each other cell is filled on its own first call. A cold
function then runs in Tier 0 and tiers up by its own invocation and loop
counts (a call from native code does not request a compile:
`TieringConfig::native_call_requests_tier1`); a cold function calls a
compiled one at the image's own code, and the image calls a cold one through
its stub. A speculated callee is called directly, not through its entry cell:
companion code is compiled in process, out of `call [rip + cell]`'s reach of
the image, and the callee's guards leave to its generic function as the
retired entry would. The stub is no frame of its own (its code range names no
descriptor). Kept compiled, as brass's cold functions (no MIR optimization
passes, light codegen: `mir::Function::set_cold`): coroutine bodies and
functions that start or suspend one (brass lowers coroutines over a whole
module before it runs), speculated functions and those holding resume points
or leaves (a resume twin is cut from its function's body), functions of more
parameters than a native-to-Tier-0 bridge takes, and the program's top-level
code (`BRONZE_FN_DESC_TOPLEVEL`: run once, at start, where lowering it for
the interpreter costs more than its code); every function the harvested run
entered at all (`Function::heat` Entered: start-up code, run again at every
launch, where the interpreter's first call costs more than native cold code;
with those interpreted too the first frame of three_meshes came at 100 ms
where it comes at 26 ms); and every cold function of an image for another
target, or built in process. On three_meshes 2429 never-entered functions are
interpreted; bench.so goes from 23.4 MB to 21.6 MB (the IL is 1.71 MB, the
link table 0.12 MB), pixi_text's from 31.3 to 28.6 MB (3737 interpreted).
Never-entered functions cost a profiled build little wall time beyond
inference (which must read them: a never-entered function can still run, and
what it does to properties and shapes is part of every closed-world fact): on
three_meshes the redundancy pass spends 0.18 s of CPU on its 2482
never-entered functions, 0.03 s of wall time with the build's threads, and
leaving their IL as lowered would grow the companion's IL by 0.15 MB, so they
are optimized as before. The build's time was in steps superlinear in a
function's size, run on the largest hot functions (three.js's
`setProgram$spec`, 1930 IL blocks after inlining), and in steps run on one
thread: the redundancy pass bypassed one forwarding block per CFG snapshot
(0.8 s on that function alone, and again in the inliner's sizing copies), the
IL verifier and the resume twins' SROA ran one function at a time, and the
companion's lowering waited for the image. With those gone a profiled build
of three_meshes takes 6.5 s where it took 9.7 s, pixi_text's 6.6 s where it
took 9.0 s, the modules byte for byte the same. What is left: inference, one
thread (1.8 s and 2.3 s), and brass's passes and register allocation on the
largest function, about 2.4 s of the 3.6 s codegen of three_meshes.
Frames and start-up are unchanged (first output of three_meshes at 26 ms,
of pixi_text at 45 ms): a run like the harvested one calls no interpreted
function. The first call of one builds the companion, and the companion
reads each body only when its function is first lowered: the blob holds the
module with every body dropped, then per interpreted function its
CreateFunction callees and parameter counts (all that lowering reads of every
body up front: closure names and arities) and its body's size, then the
bodies one after another (`il::serializeBlocks`). The companion decodes the
headers and leaves each body a view into the image (`decodeCompanion`'s
`deferBodies`), marks its brass AST function `body_deferred`, and on the
function's first lowering reads the IL body and converts it
(`TranslatorOptions::deferred_body`, `lowerFunctionToBrassAst`). The
companion does not check the IL's proof certificates again
(`il::verifyModuleProofs`): its bodies are the image's own IL, which the
image's build proof-checked with the whole module, and they ship inside that
image, whose native code is trusted on the same grounds. On smallworlds'
app.js (8268 interpreted functions, an 8.3 MB blob) the first cold call
builds the companion in 17 ms where it took 82 ms, and that call's own body
in 30 µs: the IL headers and the unique names (2 ms each), the module's
declarations and per-function metadata for all 10306 functions (8 ms with
lowering's setup), the tiered pipeline (2.6 ms) and the link table (1 ms);
the module is 33 KB smaller. A wrong profile (every function never
entered) costs what it did with the native cold tier: three_meshes runs
18.1 ms a frame on it either way, against 2.8 ms on its own harvest, until
bro's cold-entry count drops the profile.

**Configuration.** Harvesting is off unless a path is given. A path can be
given in three ways:

- `bronze build --harvest-out <file>` bakes the absolute path into the
  identity record. The program writes the file at exit (atexit, and from
  `bronze_host`'s main after the entry).
- An embedding host sets `eval::EvalOptions::harvestOutPath` for
  `eval::runModule`. That call passes the loaded module's identity to
  `embed::harvestInlineCaches(identity, path)`, which returns false for a
  profiled module.
- An embedding host sets `eval::EvalOptions::harvestOutPath` for
  `eval::compileScript` / `compileFile`: the program's FIRST RUN, compiled in
  process to be harvested, with no module built on nothing (bro's installed
  apps, bro docs/apps.md). It is lowered as `buildModule` lowers it (the
  ahead-of-time front end, `lower::lowerAheadOfTime`, and the module's result
  name, `detail::moduleResName`), so `numberSlowSites` and `icLayoutDigest`
  give the IL of the build that reads the harvest: the same sites and the
  same digest. The pipeline that build then runs (`opt::optimizeModule`)
  renumbers nothing. The tiered engine compiles it with harvest evidence
  (`TieredEngineConfig::harvestEvidence`, `BrassBackend::setHarvestEvidence`
  in process): the counters, shape and callee turns, arms and allocation
  mementos are MIR, so the interpreter, baseline and optimized tiers and OSR
  entries all run them, and a function keeps counting when it gets hot. A
  body lowered lazily arms its sites when it is built, into the loaded
  image's own bitmap (`il2mir::SlowArmedBits::attach`); every function's
  entry and loop counts are armed up front, so a function the run never
  entered reads as never entered (`Evidence::counted`), as in a module. The
  compile is not served from or stored in the code cache, and
  `runCompiledScript` arms the harvest (`CompiledScript::harvestIdentity`).
  On ai-arena (6k lines, headless) the counting run starts and runs frames
  as the plain JIT does, within noise.
- A host calls `embed::harvestInlineCaches` itself.

`embed::writeInlineCacheHarvests()` folds the live instances on the calling
thread and writes every file now. Call it before leaving through `_exit`,
when a page is hidden, or before a reload. The reverse direction is
`eval::EvalOptions::feedbackInPath`: `buildModule` reads that profile or
harvest, and the module's key includes the file's text. The module's result
global is named from the key *without* the profile, so a build on a harvest
has the same property keys, and therefore the same digest, as the build the
harvest came from. `ModuleBuildResult::feedbackApplied` says whether the
file is this module's (a harvest from a build of the same text, even one
in which nothing ran often enough to specialize on).
A file that is not (another text's, options' or compiler's harvest) stops
the build before code generation: `ModuleBuildResult::profileStale`, and
nothing is written.

**Cold entries.** A build on a harvest that made functions cold
(`Function::heat` Never or Entered) gives the image one word per function,
`__bronze_cold_entries` (exported behind its word count as
`<entry>_cold_entries`, `eval::LoadedModule::coldEntries`), and each cold
function adds one to its word on entry, in whichever tier runs it (an
interpreted one's body counts in Tier 0 and in the code it tiers up to).
Cold functions are entered seldom by
the harvest's own measure, so this costs nothing that shows; a host reads it
after a run to see whether the run entered often what the harvest saw seldom
or never: the harvest was too thin a run, and the host harvests again (bro
docs/apps.md).

**Format.** A harvest is a version 9 file with these rows (any `fn` rows
come first):

```
harvest <16 hex digest>  <IC site count>
slowsites <slow-site count>  <armed bitmap: per 64 sites, 16 hex digits>
slow    <slow-site index>  <times its fallback ran>
last    <slow-site index>  <shape of the named site's last receiver>
ic      <index>  property|method  -|full|mega|full,mega
way     <index>  <form>  <receiver>[  <callee>]  (accessor: <getter> <setter>; proto: <depth> <slot>)
other   <index>  <calls whose receiver no way could hold>
doubles <receiver>  <key positions, ascending, comma-separated>
mixed   <key>
objects <receiver>  <key positions, ascending, comma-separated>
nonobject <key>
```

**Slot representation.** The harvest also says which slots held only
Numbers, without changing a shape of the run it measures (no key is made
eligible for a double slot while it runs): the runtime notes, per shape node,
whether every property add that made it held a Number
(`rtIcHarvestNoteBirth`, `runtime/ic_harvest.h`), and at the write the heap
is walked for the objects of the shapes the caches named. A `doubles` row
names, for a receiver the caches held, the positions in its key list that were
born Numbers and that every object found of every shape of that name held a
Number in; a `mixed` row names a key some object was found holding anything
else under at a position not born so (born a Number, or its birth unseen):
the key a run with it eligible would have made a double slot of and then
generalized. A position born something else does not make its key mixed
(eligible, that add gets a boxed node). A build on the harvest makes the keys
at a `doubles` position that are not mixed eligible
(`bronze_register_slot_repr` at module init) and spells their positions `%k`
in the receiver names its shape cells resolve
(`opt/speculate/number_slots.h`), so reads behind those cells are Numbers.
`objects` and `nonobject` rows say the same of plain objects (not arrays,
functions or null); no build reads them today. See
docs/slot-representation.md, "From a harvest".

**Never-entered blocks.** The same counters say which blocks of a function
the harvested run never entered: an armed word still zero is code that never
ran, and a call's count, a named access's shape-turn count and every word of
an element access or numeric op are marked on each execution of the
instruction. A block holding such an instruction that never ran, which calls
out and whose code rejoins the rest, is one the speculation step's OPTIMISTIC
copy leaves at the top of (`src/opt/speculate/never_entered.cpp`,
`optimistic.h`); the conservative copy keeps it, and the optimistic one
retires to it after 100 such exits (docs/deopt-stats.md, "Optimistic
copies"). Every block holding an instruction that never ran, with the
blocks it dominates, is also marked cold in the generic function before
the copies are made (`unenteredBlocks`): a layout hint that puts it after
the main line in every copy, and the inliner skips its calls; what the
block does is unchanged.

**Slow paths.** An inline fast path with a generic fallback (an element read
or write, a constant-index read or write and the runs of them il2mir lowers
under one guard, a strict compare's contents call, a checked unbox and a
truthiness test's helper) joins its fallback again, and the fallback calls
the runtime. In a loop that one cold call is a write to all memory, so brass
reloads every header word and slot behind it each iteration. A harvest says
which fallbacks never ran, and a build on it makes those exits to the
function's resume twin (il2mir/il_speculation.h, "SLOW ARMS"): no call, no
join, and the loads after it are the loads before it.

`il::numberSlowSites` (`il/feedback_site.h`) numbers the instructions that
have such a fallback (`ElemGet`, `ElemSet`, `PropGet`, `PropSet`, `StrictEq`,
`Unbox`) first thing in the final passes, so the probe of a build on the
harvest numbers them as the harvested build did; inlined copies keep their
original's number and share its counter. The dynamic `+ - * /` are numbered
too: their helper's arm is a fallback, the result past an arm that left is a
Number with nothing to ask (its number flag), and the shape memo crosses the
join as it crosses a static site's. An instruction takes `kSlowSiteArms`
(8) indices: the first is its fallback's, the rest are its **arms**, the
alternatives of its inline path (a store into an Array or a view of one
element kind, a truthiness test's flag / object / Number / String answers,
a `===` of equal or different bits of Numbers or of anything else). Each
arm stores a 1 in its counter where it begins, and a build on the harvest
asks only for the kind the harvested build saw and leaves for the whole
ladder otherwise: one test for a truthiness site that saw one kind, the
doubles' compare for a `===` that saw only Numbers, the bits' for one that
saw only objects (il_speculation.h, "ARMS"). An arm's answer is computed in
the arm's block: in front of the test it would be the exit's state, kept
alive on the fast path for the twin alone. A build that may be harvested (not
profiled) gives the module that many u64 counters, ending where its IC table
starts (`__bronze_slow_counts`, the IC identity's `slowSites`, version 3),
and every fallback bumps its instruction's counter first thing (alias class
`kSlowCountAliasClass`, a load, an add and a store, all on the cold path).
Lowering also notes each site it gave a counting fallback, and the backend
lays that ARMED bitmap out right before the counters. A profiled build, whose
code counts nothing and which is never harvested, has neither, and its IC
identity's `slowSites` is 0. A run's fallback bumps
the counter of every numbered instruction in it: the build on the harvest
need not form the runs the harvested build did. A constant-index run's
fallback never leaves: its exit holds every element it loaded in its state,
and three.js's `Matrix4.multiplyMatrices` ran 28% more instructions so. The harvest folds and clears
the counters with the IC table and writes a `slowsites` row (the count and
the armed bitmap) and a `slow` row per counter that ran. A build on it whose
probe's count matches leaves from the COLD fallbacks of a twinned function,
those armed and never run (`FeedbackPlan::slowCold`); a fallback the harvested
build did not arm (it lowered the instruction another way: a cache call, a
proven representation, no hint) stays a branch, and a stale or older harvest
leaves none.

**Loop index facts.** A counted loop over an Array speculates to an int32
header parameter, its `cmp.lt i, n`, and per iteration `in.bounds i, n` in
front of the element read and `add.would_ovf i, 1` in front of the
increment. The redundancy pass's value walk answers both from the compare
(`opt/redundancy/index_facts.h`): an i32 value is NON-NEGATIVE when every
definition of it makes one from non-negative operands (a constant >= 0, an
`add` that cannot wrap, a block parameter every edge passes such a value),
the greatest fixed point of that, which is the induction over the loop's
iterations; and the arm of an integer compare that says x < n, with x
non-negative, proves `in.bounds x, n` and (x + 1 <= n <= INT32_MAX) that
x + 1 is an int32 (FactEnv::sumFits). Nothing of the profile is used, so a
wrong one cannot make it wrong. Instructions a frame: three_skinned 32.30M
to 32.16M, three_meshes 51.77M to 51.54M, pixi_text 23.63M to 23.58M,
pixi_graphics 308.7M to 307.8M, pixi_sprites 14.87M to 14.82M, the other
two unchanged; the same guard exits, three_meshes' module 21.62 MB to
21.58 MB.

Before this, facts lost at rare joins were measured, and were not worth
building: re-checking on the rare edge of a join what the frequent edge
proves, so the tests after the join go. With every write in a block that
holds a call the harvest made on under 1/16 of the function's entries and
loop runs (and every write of a never-entered block) dropped from the
pass's kills, an unsound upper bound, pixi_text ran 23.57M instructions a
frame against 23.63M, and three_skinned's run failed: rare paths there do
change the shapes the facts are about, so the re-checks would leave.
Treating `instanceof`, generic element and property reads, and every
direct call's Shapes, Protos and ElemKinds as written by nothing, all
unsound too, gave 23.49-23.59M and 32.21M against 32.30M: the guards the
census found repeated behind a call are repeated at the machine level, in
the inline paths whose fallback rejoins, not in the IL.

**Guards hoisted out of inlined loops.** The redundancy pass moves a
loop-invariant guard to the preheader only where a leave from there has a
resume point for entering the loop (opt/speculate loops.cpp). A loop the
inliner copied into a caller has fresh value ids, which name no block of the
caller's G, so its guards stayed in the loop: removeListener's loop in
pixi's `getTexture$opt` tested two per iteration. The splice now records,
per copied loop whose callee G has a point at the header's top, the exit a
leave from its preheader takes (il.h `Function::inlinedLoopEntries`): the
leave to the callee's loop-entry point finished by the rule every inlined
leave is, `resume.call @G_f, h, entry state` then `leave @G_c, a,
callerState(r)`; an entry the callee carries from its own inlining is
renamed and finished once more, so any depth works (opt/inline splice.cpp).
`loopEntryResume` answers such a loop from it, with the header's entry
parameters replaced by the preheader jump's arguments and every value held
to the own-loop tests (available at the preheader's end, of the type the
named G's point gives it, the point still there); the exit block gets the
`resume.call`s and conversions with fresh ids. Exact for the reason an
inlined leave is: the hoisted guard runs where S has done what G_f would
have done up to entering its loop, G_f's twin finishes the call from the
loop's first iteration, and G_c continues after the call. Hoisted guards
(static, before -> after): three_meshes 90 -> 277, three_skinned 172 -> 496,
three_instanced 168 -> 320, three_lights 82 -> 225, pixi_sprites 69 -> 109,
pixi_graphics 140 -> 280, pixi_text 86 -> 148. On one harvest each,
instructions a frame: three_meshes 51.52M to 51.29M, three_skinned 32.16M to
31.66M, three_lights 2.766M to 2.757M,
pixi_text 23.58M to 23.42M, three_instanced and pixi_sprites unchanged;
pixi_graphics, on fresh harvests, 307.85M to 302.34M.
pixi_text leaves 1249 times against 1231 (selfhost `findIndex`'s callback
test, hoisted, fails on entry 9 times).

**Keyed reads of dictionaries.** What remains of getTexture is keyed reads
and writes of dictionary-mode objects, and the keys are built for each read:
`text.styleKey` is a getter returning a template of three parts, so
`_activeTextures[textKey]` hashes a new string and probes the table every
time (with the template, about 12% of pixi_text's cycles under its JS:
`Dictionary::find`, `StringHeader::hash`, the computed-read stubs). Nothing
the harvest could record makes that cheaper: the string is new, and node
pays the same hash. Letting facts survive the generic read was measured
before (above): 0.1-0.3%. A Number or constant-string key of a dictionary
(`_gpuData[uid]`) is answered by the computed-read table's dictionary form
(runtime/elem_ic.h `elemCacheFillDict`), which the module's stub reads while
the table's generation holds: pixi_text 23.62M to 23.52M instructions a
frame on one build, the others unchanged; a heap string's key is never
kept (its entry would answer only that string object, and interning one per
fresh key cost pixi_text 0.4%).

Every fallback that can leave must count in the harvested build, or an exit
the harvest never saw runs often and gives the function up for its generic
entry (il_speculation.h, "FAILURE POLICY"): a constant-index read on a typed
array whose fallback did not count sent pixi's `Hasher.num` there. So a
fallback the harvested build reaches another way stays a branch: a hinted
truthiness test's ladder (the harvested build had no hint, so no ladder to
count), `ToInt32` (a harvested double has no fallback to count), and an
inlined method's cell pair, which holds one shape where the site's cache
holds two (leaving on its miss sent pixi's `updateTransformAndChildren` to
its generic entry).

**Pretenuring.** Every object literal and construct is an ALLOCATION SITE
with a slow site (`il::numberSlowSites`; a construct the speculation step
turns into `create.this` gives it the construct's, `opt/speculate/construct.cpp`).
A build made to be harvested (`--harvest-out`: `cache_evidence`; a memento is
two words more of eden per object, which a build that may only be harvested
does not pay) adds one to its `kAllocCountArm` word for each object its inline
path makes (`IlLowering::alloc_memento`) and bumps an ALLOCATION MEMENTO right
behind the object: a one-word leaf of the runtime's memento layout
(`gc_detail::layoutIds().memento`, its header the TLS block's
`gc_memento_header`) holding the address of the site's `kAllocTenuredArm`
word. Arming a harvest tells brass the layout (`brass::gc::set_memento_layout`,
brass docs/gc_contract.md 2.14), and from then on a minor collection carries
the memento along with a surviving object and, when it promotes the object,
adds one to that word. Both are the image's own words, not a thread's or a
realm's copy: the home instance, which the harvest folds and which lives as
long as the image, where another copy may be freed while a memento names it.
The two come back as `slow` rows. The build on the harvest
(`opt/pipeline/pipeline.cpp`, `planPretenure`) pretenures each site that made
`kPretenureMinObjects` (256) or more, four fifths or more of them promoted,
and at least a hundredth of what the module's allocation sites made
(`Module::pretenureSites`, carried by the IL file). Promoted decides, not
surviving one collection: pixi_graphics' per-frame `{path,style}` survived
its first collection at 0.94 but only 0.12 of those were promoted, and
allocated old they would be garbage only a full collection takes back. The
share decides too: an object allocated old saves its copies, but every store
of a young value into it is a barrier's call and a dirty card, which a site of
few objects filled at length after its birth (pixi_graphics' Graphics, one a
frame) pays without the saving. A pretenured site's inline path calls
`bronze_alloc_pretenured` (`runtime/rt_object.cpp`, over brass's
`Heap::try_allocate_pretenured`) where it would bump eden, passing the site's
record (`__bronze_pretenure_sites`, one brass `PretenureSite` per pretenured
site in the image's own data, `IlLowering::pretenure_site`); the call never
collects and answers 0, taken to the site's ordinary slow path, when neither
generation has room without a collection. The object is old with its card
dirty (grey in a marking cycle), or young, so an object literal's own field
stores keep no barrier, and the next minor collection promotes what an old
one was filled with (brass docs/gc_contract.md 2.14). pixi_text pretenures
`measureText`'s metrics object and `LRU.set`'s node: 26.3M to 25.1M
instructions a frame and 93 minor collections (52 ms) to 34 (27 ms).

A claim can be wrong for the run at hand: the program's inputs differ from
the harvested run's. A site allocated old whose objects die young is garbage
until a full collection, and every minor collection meanwhile promotes what
its dead objects' dirty cards name. So the collector checks the claim as the
program runs: one object in 32 of a pretenured site (at most 64 between two
minor collections) is made young, a probe, and once more than half of a
site's probes die young the site makes young objects for the rest of the run
(brass `src/gc/heap_pretenure.cpp`). The probes made before the first minor
collection, when everything is young, are judged by it, so a site wrong
from the start never allocates old; one whose objects turn short-lived is
refused within about four minor collections. A kernel whose training run
keeps a literal's 300,000 objects and whose real run drops 12M of them: 506
ms of minor pauses in 97 collections (5.3 ms each) and 48 full ones without
the check, 1.7 ms in 143 with it, as with no pretenuring (1.7 ms); one that
keeps 300,000 before dropping 12M: 531 ms and 50 full collections without the
check, 18.5 ms with it, 28.3 ms with no pretenuring. `adversarial`'s
`p_lifetimes` builds on both kinds of wrong claim, and the oracle case
`pretenure_wrong_claim` switches a literal's and a construct's objects from
kept to dropped.

**Tests.** `tests/oracle/run.py`'s `profile` mode builds each oracle case
with `--harvest-out`, runs it, rebuilds on that harvest and requires the
pinned bytes and exit code; `switch` covers a case whose run leaves the arm
it trained on, and `adversarial` (above) builds each case on wrong harvests.
`aot-module` runs the evaluator path, and `spec` covers parsing and round
trips.

## Report

`bench/tools/feedback_report.py <file.tfb> [--top N] [--json]` prints, as
shares of executions: dynamic-op operands with one tag (and with numbers
only), inline-cache executions with one shape, and call executions with one
target, overall and for the hottest functions by entry count.
