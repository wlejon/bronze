# Type feedback (`bronze build --feedback-out <file.tfb>`)

Most values in a real library stay `Dynamic` in the IL, because bronze types a
value only where a proof does. Type feedback is the measurement that says what
those values actually are on a representative run, so a later build can
specialize the hot ones behind a guard. This document covers the recording,
the file, and the reader. Nothing consumes a profile for codegen yet.

```sh
bronze build app.js -o app.fb --feedback-out app.tfb   # 1. instrument
./app.fb                                               # 2. a representative run
bronze build app.js -o app --feedback-in app.tfb       # 3. the build that reads it
python3 bench/tools/feedback_report.py app.tfb         # what the run saw
```

A feedback build is an instrument, never measured and never shipped. Without
either flag a build is byte-identical to one made before the feature existed;
`--feedback-in` alone changes no byte either (the profile is only held, at
`Lowerer::feedbackProfile()`).

Implementation:

| piece | file |
|---|---|
| site words, kinds, tags (shared by compiler and runtime) | `src/abi/bronze_feedback_site.h` |
| site enumeration, function keys, IL hash | `src/il/feedback_site.{h,cpp}` |
| the `feedback.record` op | `src/il/il_op.h` |
| instrumentation pass | `src/lower/lower_feedback.{h,cpp}` |
| runtime recorder and writer | `src/runtime/type_feedback.{h,cpp}` |
| reader and query API | `src/opt/spec/profile.{h,cpp}` |
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

## Consumers (later chunks)

The intended consumer is a specialization pass that runs where the
instrumentation runs, after every other lowering pass, so the keys it computes
are the ones the profile was recorded under:

- a `value` site that saw only `f64`/`i32` gets an `is.number` guard and the
  typed arithmetic, with the generic op on the cold side;
- a `shape` site that saw one shape gets a shape-guarded fast path;
- a `target` / `method` site with one callee gets a guarded direct call;
- `entryCount` decides which functions are worth it.

The guard is what makes a specialization sound; the profile only decides where
a guard pays.

## Report

`bench/tools/feedback_report.py <file.tfb> [--top N] [--json]` prints, as
shares of executions: dynamic-op operands with one tag (and with numbers
only), inline-cache executions with one shape, and call executions with one
target, overall and for the hottest functions by entry count.
