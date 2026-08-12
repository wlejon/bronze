# 0032 — The add an inline cache could not see

Status: implemented.

`cases/blocked/proto_chain_invalidation` was the one entry in that directory
blocked on a BUG rather than on a missing feature: bronze compiled it, ran it,
and printed the wrong answer, identically with inference and with
`--no-infer`. Two reads of the same property in the same program disagreed —
`read(leaf)` answered `"top"` where `leaf.p`, one line later, answered
`"mid"`. This is the mechanism that closes it, the measurement that decided
its shape, and the two other copies of the same decision the fix had to
delete.

## 1. What the receiver's shape cannot see

An entry is `(shape, slot, depth)` and a hit is taken when the RECEIVER's
shape word matches. docs/0008 decision 2 makes that sound at depth 0 and at
depth 1, and the argument is worth restating because it is exactly half an
argument:

- At **depth 0** the entry names an own property. An own property that would
  shadow it changes the receiver's own shape, so the compare misses.
- At **depth 1** the holder is the receiver's immediate prototype. There is
  nothing between the two to shadow from, and the prototype lives on the
  shape (docs/0008 decision 1), so a prototype swap changes the shape too.
- At **depth 2 or more** there is something in between, and adding a property
  to it changes only THAT object's shape. The receiver's is untouched, the
  entry still hits, the walk still follows `depth` links, and the read still
  lands on the property the add shadowed.

docs/0019 decision 5 had already closed two of the three ways a deeper chain
can move. A **delete** renumbers slots and a **`setPrototypeOf`** replaces the
holder; both put the object they touch into dictionary mode, and
`cachedProtoHolder` refuses a walk that crosses a dictionary anywhere,
including at the holder. What was left is the **add** — and an add is the one
operation the transition tree is designed to make free, so it leaves no
dictionary and no other trace on the walk.

The two questions are genuinely different and are answered separately. "Is
this walk safe to take" is about the chain as it is now, and
`cachedProtoHolder` can see it. "Is this entry still about the same chain" is
about the chain as it WAS when the entry was filled, and nothing on the walk
can see that. It needs something recorded at fill time.

## 2. A counter, and the measurement that decided what counts

The entry grows a fourth word: the value of a global **prototype-mutation
epoch** at the moment it was filled. A depth > 0 hit is taken only when that
word still matches. Depth 0 never consults it — the receiver's own shape is
already the whole answer there — which is why the inline fast path in
generated code is unchanged.

The design question is not the counter, it is what bumps it. The first
implementation bumped on **every property add anywhere**, because identifying
an add that lands on a prototype needs per-object state and counting
everything needs none. It is sound — an unnecessary bump can only cause a
MISS — and it was written first on purpose, so that the precise version would
be justified by a number rather than by a prediction.

The number said build it. Three benchmarks, 3M iterations each, best of five:

| | every add bumps | only prototype adds bump |
| --- | --- | --- |
| depth-3 read, no adds in the loop | 233 ms | 233 ms |
| depth-3 read + `new Pt(i)` per iteration | 2739 ms | 1960 ms |
| depth-0 read + `new Pt(i)` per iteration | 1882 ms | 1880 ms |

The middle row is the whole argument. Object construction is property adds,
so under the coarse rule a loop that constructs anything invalidates every
proto cache in the program on every iteration — 857 ms of chain walking over
the depth-0 control, against 80 ms once the rule is precise. That 80 ms is
the cost of the three pointer loads a depth-3 cached hit costs over a depth-0
one, and it is not the epoch's.

three.js allocates inside its update paths, so the coarse rule would have
been a real regression on the one workload that matters, invisible in every
benchmark bronze had. Both proto benchmarks are now in `bench/` for that
reason: `property_access` is two OWN properties, which generated code inlines
and the epoch never touches, so nothing in the suite could previously have
noticed proto caching being switched off entirely.

## 3. The mark lives on the shape, and where that is imprecise

An add bumps when the object's shape carries `used_as_prototype`. The mark is
applied in `Shape::createRoot`, which is the one moment an object becomes a
prototype — every route to one, a class, `Object.create`, or
`Object.setPrototypeOf`, ends in a root shape carrying it — so no caller has
to remember to do it.

Two things make the mark correct rather than merely plausible:

- **It propagates through `addProperty`, on the reuse path as well as the
  create path.** An object that was a prototype before an add is still one
  after it, so the shape the add lands on must be marked or the NEXT add
  would not bump. The reuse path matters because a shape is marked when some
  object first becomes a prototype, which can be long after unrelated objects
  built its transitions.
- **Only a plain object is marked.** `cachedProtoHolder` refuses to walk
  through anything else, so an array or a function used as a prototype
  already misses for a reason that predates this doc.

The imprecision is that shapes are SHARED: an object that merely has the same
layout as a prototype is marked too, and its adds bump. That direction only
ever costs an extra miss. The degenerate case is an EMPTY object becoming a
prototype — `Foo.prototype = {}` — which marks the shape every `{}` literal
starts at and, through the propagation above, gradually marks the transition
tree below it. The program is then back to the coarse rule's performance and
still correct. A class prototype does not do this: it is created with a root
shape of its own, not the shared `{}` root. `Foo.prototype = { m() {} }` does
not either, because a literal with properties has a shape of its own.

Closing that last case wants a per-object bit, and the only places to put one
are `HeapObjectHeader::flags` — which is the kind field the generated fast
path discriminates on, so a bit there costs a mask on every property read —
or eight more bytes on every `ObjectHeader`. Neither is worth paying for a
shape that no measured workload marks.

## 4. Three copies of one decision, and the write path that was one check short

The hit condition existed in **two** places — `bronze_prop_get`'s own fast
path and `ObjectHeader::getProp`'s — and the first fix only patched the
second, so the case still printed the wrong answer after the epoch was
already working. That is docs/0031 decision 4 happening again, four commits
later, in the same file.

So the fix is not a third copy. `InlineCache::describes(shape)` is the one
answer to "is this entry still live", and both read paths ask it. It
deliberately does NOT cover whether the walk is safe to take, because
`cachedProtoHolder` owns that question and a caller needs both — two
mechanisms, two names, one caller combining them, rather than one condition
written twice.

Reading across for the same shape found a third: `bronze_prop_set`'s fast
path checked the cached shape and wrote the slot **without checking
`cached_depth == 0`**, where `ObjectHeader::setProp` two files away checked
it. A depth > 0 entry there would write an ancestor's slot index onto the
receiver — a real own property, silently overwritten. It is unreachable
today, because the compiler never gives a get site and a set site the same
table entry and only the get path ever fills at depth > 0. But that is a fact
about site numbering being relied on inside the runtime, unstated, and it is
the same shape as the premise docs/0031 decision 7 found written down three
lines from the code that contradicted it. `describesOwn` now says it, in the
runtime, where the assumption is being made.

## 5. What the promoted case pins

`cases/proto_chain_invalidation` was promoted with its expectation
byte-for-byte as it was written from ECMA-262 10.1.8.1 — the point of a
blocked case being that the bytes were derived before the implementation
existed. Its header was rewritten on promotion (docs/0003) to describe the
three mechanisms it now covers rather than the bug it used to hold:

1. An add to an intermediate prototype — the epoch.
2. A delete, and a `setPrototypeOf` — dictionary mode, refused by
   `cachedProtoHolder`.
3. An own add and an own delete — the receiver's own shape word.

The line ordering is load-bearing and is called out in the file. Shadowing at
the NEAREST prototype used to pass only because the delete on the line before
had left a dictionary on the walk; the case keeps it in that position so it
still covers the depth-1 add, and the epoch is what answers it now rather
than the accident.

## Named diagnostics

None. Every path this doc touches was already reachable and already answered;
what changed is that two of the answers were wrong and one was unguarded.
