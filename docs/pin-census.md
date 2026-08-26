# The pin census (`bronze build --census <out.pins>`)

A `--pins` manifest is a promise a build makes about a program
(`src/types/pins.h`). Until stage C1 somebody had to write that promise by
hand, class by class, against the source. The census is the tool that writes
it.

Implementation: `src/runtime/pin_census.{h,cpp}` (the recorder and the manifest
writer), `src/lower/lower_census.cpp` (which sites exist), `src/il/il.h`
(`Op::CensusRecord`, `il::CensusSite`, `Module::censusSites`).

It is **offline**, and there is no JIT anywhere in it. Two compiles, one
artefact:

```sh
bronze build app.js -o census.exe --census app.pins   # 1. instrument
./census.exe                                          # 2. a representative run
bronze build app.js -o app.exe --pins app.pins        # 3. the real build
```

Step 2 writes `app.pins` when the process exits. `BRONZE_PIN_CENSUS_OUT`
overrides the baked-in path at run time, which is how one census binary can
produce a manifest per workload.

A census build is an **instrument**. It is never benchmarked, never shipped and
never linked into anything that is: it carries one plain call per observed
site, and the only thing that has to be true of it is that a representative run
completes.

## What a site is

A census site exists **only where lowering ran out of static answers**. This is
not an efficiency: it is the design. The closure parameter proof
(`planClosureParamNumbers`), the env-slot fixpoint (`planEnvSlotNumberTypes`)
and the signature join all run *before* any site is created, and every one of
them removes sites. A parameter the proof typed `f64` has no manifest line to
write, so it gets no site — the compiler already knows.

| site | where | the entry it would support |
|---|---|---|
| env slot | every store to a captured `let`/`const` with the binding structure an env-slot pin needs that the fixpoint could not type | `function <fn>.<binding>: number` |
| parameter | the CALLEE'S ENTRY, for a source parameter still typed `Dynamic` with no default, pattern or rest | `param <owner>(<p>): number` |
| return | the `return` statement, for a function whose return the convention left `Dynamic` | `return <owner>: number` |
| field | every store path that reaches a field on a receiver inference can name — the same six paths B1's write barrier sits on | `<Class>.<field>: number` / `number-or-nullish` / `numeric-elements` |
| opaque store | a store to a field name through a receiver inference types `dynamic` | *nothing* — see **the two strengths** below |

**Parameters are recorded at the callee's entry and not at the call sites, and
that is the whole reason this instrument exists.** Stage E4's proof enumerates
the call sites of a nested declaration whose name never leaves callee position,
and the closure a factory *hands out* has call sites that enumeration provably
cannot reach. The entry sees every call there is, escaped or not, which is
exactly the join a `param` entry claims.

## Aggregation is a join, never a vote

Sites that name the same entry — every store to one field, every call position
of one escaped closure — join. All of them Number is a pin; **one** of them
anything else is no pin, and no threshold or weighting changes that: a pin is
spent *unchecked* at the read, so "almost always a number" is not a weaker
version of the claim, it is a different and false one.

Four further refusals, each of which is a real shape the census met:

* A return whose body can **fall off its end** is refused by the site table,
  with no observation needed. Every return it executes may be a Number and the
  entry is still wrong, because falling off yields `undefined` and there is no
  `undefined` in an f64. Reachability is a property of the program and no run
  can be asked about it.
* An owner spelling that would govern **two different IL functions** is refused
  the same way. A `--pins` entry matches an IL name by suffix, so a bare
  `param clamp(x)` written for a module function would also govern a
  `Bar.clamp` elsewhere — possibly one whose `x` has a default, which is a
  build failure rather than a wrong number.
* A field observed **only ever nullish** is refused rather than widened to
  `number-or-nullish`. The pin buys nothing (what it licenses is the coercing
  position on the *number* arm) and risks everything: "not assigned yet" is what
  an optional field looks like on a short run and an object is what it holds on
  a long one. three.js's `Material.clippingPlanes` is exactly that shape.
* A target **no manifest line can spell** is refused before it is ever observed.
  An accessor lowers to an IL function named `Euler.set x` — a space in the
  middle — and `param Euler.set x(value): number` is not a line the parser
  accepts. Getters, computed method names and quoted field keys (`o["a b"] = 1`)
  arrive the same way. This is the census's worst failure mode and the only one
  it can have: not a wrong claim, which B1 turns into a `TypeError`, but a file
  the next build refuses to read. Nine of the three.js oracle's proposals were
  of this kind.

The manifest carries the refusals as comments, with their tallies, because a
census whose output is only its hits tells a reader nothing about the claims it
declined — and the declines are where the interesting shapes are.

## Enforced by default, and the two strengths

Stage B1 made a violated pin a **catchable `TypeError` naming the manifest
line**, at the violating write. That is what makes an inferred entry shippable
at all: the census only has to be right about the hot path, not right in the
"no false positives, ever" sense a proof would need. A wrong entry on a path the
census run did not take is a diagnostic, not a pointer read as a double.

There is exactly one place that is not true, and the census marks it. A store
through a receiver inference types `dynamic` gets **no barrier**, while a
class-known read elsewhere still spends the claim (`src/types/pins.h`, and
stage B1's negative 1). So the census emits two strengths:

```
Vector3.z: number              # every store to it is from a site the compiler can type
Vector3.x: number @observed    # some store to a field named 'x' is not
```

`@observed` is **refused by a default build**, by name, with the line quoted.
`--pins-allow-observed` accepts it, and accepting it is the deliberate act of
taking back B1's guarantee — for one entry, named in a file.

The marker is per **field name**, not per class, because that is the granularity
the manifest itself has: an entry matches a class on its last dotted component,
and the compiler cannot say which class an untypeable store's receiver belongs
to. An array index (`te[0] = x`) is filtered out before this: a manifest cannot
spell `<Class>.0`, so a row for one could only ever mark real entries
`@observed` on evidence that means nothing.

## Reading the file

```
# 101 entries, 20 marked @observed, 1718 candidate sites.

# --- fields (`<Class>.<field>`) ---------------------------------------------
Matrix4.elements: numeric-elements  # 21 obs / 1 site
Quaternion._x: number  # 131 obs / 28 sites
Vector3.x: number @observed  # 112 obs / 44 sites; a store to 'x' goes through a receiver the compiler cannot type
# refused Euler._order (field): polymorphic: 30001 string
# refused Quaternion.isQuaternion (field): polymorphic: 3 boolean
# refused param Euler.set x(value) (param): the compiler refuses this form here
```

`obs` is observations, `sites` is how many distinct stores or call positions
stand behind them. One site with a million observations is a hot loop; forty
sites with seven observations each is a library the run barely touched, and a
reader deciding whether a run was representative wants to know which.

## What it cannot see

* **The dynamic-receiver gap itself.** The census reports it; it cannot close
  it. Closing it needs a runtime-visible pin table keyed by shape, so an
  unknown-receiver store can ask.
* **A path the run did not take.** Everything except the static refusals is an
  observation. A manifest from a thin run is a promise about a thin run, and B1
  is what makes the difference a diagnostic instead of a corruption.
* **The in-bounds half of `numeric-elements`.** In-bounds is a claim about a
  read, so there is no write to hold it at (`src/types/pins.h`). The census
  checks that every observed value of the field was a dense all-number array,
  which is strictly more than a hand author checks, and it still does not make
  the index claim.
