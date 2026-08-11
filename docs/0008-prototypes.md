# 0008 — Prototypes, `this`, and `new`

Status: designed and implemented 2026-08-10. Implements the prototype bullet of docs/0004
decision 2, which was accepted but unbuilt: `Shape::prototype` existed as a
field that nothing ever read, and there was no *surface* that could create
a prototype — `new Foo()` on a user function was
`error: unsupported constructor`, and `this` was not even a token.

Three.js is the bar (0001), and three.js is prototype code end to end:
`Vector3.prototype.add`, `Object.assign(Foo.prototype, {...})`, method
calls on instances in the inner loop. This is not an optional corner.

## What has to land together

A prototype is only observable through a chain of features, so shipping a
subset would mean shipping nothing testable:

| piece | why it is unavoidable |
| --- | --- |
| `this` | a constructor body and a method body both need the receiver |
| `new Foo(...)` | the only way to make an object whose shape has a prototype |
| `Foo.prototype` | the only way to put anything *on* a prototype |
| own-miss chain walk | the actual feature |
| proto-hit IC | the reason the feature is not slow |

`obj.method(args)` already lowers to a `prop.get` plus a dynamic call with
the receiver as `this`, so method dispatch needs no new mechanism — which
is exactly 0004's "method calls are property lookups".

## Decision 1 — the shape records the prototype, and only root shapes do

0004 requires the prototype on the shape rather than the object: a
proto-hit IC checks the *receiver's shape* and then trusts what it found
on the prototype, which is only sound if the shape determines the
prototype. Two objects with the same properties and different prototypes
are therefore different shapes, which falls out of giving each prototype
its own root shape.

Storing a `Value` (a movable object) in a shape (immortal, non-moving)
means the collector has to forward it. Rather than pay that per shape, only
the **root** shape carries the prototype; every transition child carries a
`Shape* root` back-pointer set at creation. So the number of prototype
slots the collector forwards equals the number of distinct prototypes, not
the number of shapes.

The collector reaches them through a new `Heap::add_root_source` — a
callback that yields `Value&`s at collection time. `add_permanent_root`
takes a fixed slot address and cannot describe a growable table; the shape
registry and (later) any other runtime-owned table need the callback form.

A side effect worth naming: plain `{}` literals now share one root shape
(prototype `undefined`) instead of each object literal minting its own
root. Before this, two `{x: 1}` literals had unrelated shapes, so any site
seeing both missed its IC every time.

## Decision 2 — the proto-hit IC caches a depth, not a holder

0004 says proto-hit ICs cache `{receiver shape, holder, slot}`. Taken
literally that puts a *movable object pointer* in the IC table, which
breaks 0004's own "IC words need no GC fixup" and turns every IC entry
into a GC root.

The refinement: cache `{receiver shape, proto_depth, slot}`. On a shape
match, follow `proto_depth` prototype links — each one a
`shape->root->prototype` load that decision 1 already keeps forwarded —
and read `slot` from the object found there. `proto_depth == 0` is an own
property, so this subsumes the existing own-property IC rather than
sitting beside it. Depth is 1 for the overwhelmingly common
`Foo.prototype.method` case, so the hit path is a pointer compare and one
chase.

The holder is thus *derived* rather than cached, and the IC stays three
plain words that no collection has to touch.

Property **writes** never walk the chain: assignment creates an own
property on the receiver (bronze has no setters), so `prop.set` ICs only
ever hit at depth 0.

## Decision 3 — `this` is a synthetic leading IL parameter

Like the environment (0007), `this` cannot be an SSA value computed inside
the function; it comes from the caller. Unlike the environment, it is not
tied to the closure's identity, so it does *not* need the dynamic calling
convention: a function whose body mentions `this` gets a synthetic leading
`__this` dynamic parameter, and every call site supplies it —

- a dynamic call / method call: the wrapper forwards its `this` argument;
- `new`: the freshly allocated object;
- a direct `call @f`: the constant `undefined`, which is what a plain
  `f()` means.

Parameter order is `[__env?, __this?, ...source params]`. Because a direct
call *can* supply `this`, there is no verifier restriction on `needsThis`
the way there is on `needsEnv` — a plain function that happens to use
`this` still gets direct typed calls.

`this` outside any function is a hard error naming itself, rather than
quietly becoming `undefined`: at module top level it is a module-system
question (0001 defers modules), and guessing would be a silent fallback.

## Decision 4 — `new` is a runtime helper, not open-coded

`new Foo(a, b)` lowers to one `construct` instruction over the callee
value and the arguments, and the helper does the whole ceremony:

1. the callee must be a function object, or a hard error naming what it is;
2. take (or lazily create) the function's `.prototype` object and the root
   shape for its instances;
3. allocate the instance with that shape;
4. call the function with `this` = the instance;
5. return the call's result if it is an object, otherwise the instance —
   JS's rule, and three.js relies on it nowhere but breaking it is free to
   avoid.

Open-coding this in the backend would put allocation, shape selection, and
the calling convention into codegen; it belongs behind one ABI line.

**Lazily**, in step 2, because a function that is never used as a
constructor should not pay for a prototype object — and closures are
created inside loops. The same lazy path serves a `Foo.prototype` read, so
the object a constructor uses and the object the program decorates are
necessarily the same one.

## Decision 5 — functions carry a prototype slot, not a shape

0004 says a JS function is an object with a shape, because three.js
attaches properties to functions. That is still the destination, but
`FunctionHeader` has no shape today and giving it one is a separate piece
of work. Here a function gets exactly two new fields — the lazily created
`.prototype` object and the root shape for its instances — and:

- reading a property a function does not have returns `undefined`, which is
  what JS says;
- **writing** any property other than `prototype` is a hard error naming
  itself, rather than being dropped on the floor.

**Amended 2026-08-11.** The first clause originally read "reading any *other*
property of a function returns `undefined` … `name` and `length` are not
implemented and are listed as such". That conflated two different things. A
property JS does not define reads `undefined` and is correct; `name`,
`length`, `call`, `apply` and `bind` are properties JS *does* define and
bronze has not built, and reading `undefined` for one of those is a claim
about the language that is false. Listing them in a doc is weaker than the
house rule, which is that an unimplemented construct is diagnosed by name —
so they are now `unsupported: Function.prototype.name is not implemented`.
The same correction applies to `Array.prototype`, `String.prototype`,
`%TypedArray%.prototype` and `ArrayBuffer.prototype`, whose real members
were all reading `undefined`; `[1].push` used to produce a call error naming
the bit pattern `fff6000000000000` rather than `push`. Plain objects are
deliberately left out of this: `getProp` cannot distinguish "absent" from
"present and holding `undefined`", so diagnosing `Object.prototype` members
there could hard-error on valid code, and a false error is worse than the
documented absence.

Assigning `Foo.prototype = {...}` is supported and resets the instance
root shape, so instances made after the assignment get the new prototype
and instances made before keep the old one — which is what the language
says.

## What shipped, and what is deliberately not here

Pinned as oracle cases, each also run under `oracle-gc-stress`:

- `prototypes` — constructor with `this`, a method and a data property on
  the prototype, two instances sharing them, an own property shadowing a
  prototype one, a miss returning `undefined`, and `Foo.prototype`
  reaching the same object every time it is named
- `proto_chain` — a two-link chain (`Derived.prototype = new Base()`), the
  same site hit a thousand times so the cache path is the one under test,
  one site seeing two receiver shapes, and a shared prototype method
  seeing different receivers

`proto_chain` is built so a plausible-looking cache bug cannot pass: the
instance's own slot 0 and the prototype's `kind` slot are the same slot
index, so a cache that remembers the slot and forgets the depth returns a
real string from the wrong object. That is not hypothetical — it caught
one. `bronze_prop_get`'s IC fast path read the cached slot straight off
the receiver, bypassing the chain walk entirely; the first version of this
test happened to hide it because both candidate strings were four
characters long.

Two deliberate breaks confirm the ratchets bite rather than merely pass:
zeroing the cached depth fails `oracle`, and dropping the shape-prototype
root source passes `oracle` and fails `oracle-gc-stress`.

Named hard errors, not silent fallbacks:

- `` `this` outside a function is unsupported `` (a compile-time
  diagnostic, since it is a module-system question — 0001 defers modules)
- `new on a value that is not a function`
- ``property writes on a function object other than `prototype` are
  unsupported until functions carry shapes`` (decision 5)
- ``assigning a non-object to a function's `prototype` is unsupported``
- `prototype chain too deep (a cycle?)`, so a cycle aborts rather than
  hangs
- `unsupported: Function.prototype.<name> is not implemented` (added
  2026-08-11 by the amendment to decision 5), and the same message for
  `Array.prototype`, `String.prototype`, `Float32Array.prototype` and
  `ArrayBuffer.prototype`

Not here, and named as such:

- **Functions do not carry shapes**, so they hold no properties but
  `prototype` (decision 5). `Foo.name` and `Foo.length` are diagnosed by
  name rather than read as `undefined` — see the amendment above.
- **No `Object.prototype`**, so a plain `{}` has prototype `undefined`
  rather than an object, and `({}).toString` is `undefined`. The shape
  machinery is ready for it — the plain-object root shape simply points at
  `undefined` today.
- **No `instanceof`, no `__proto__`, no `Object.create`,
  no `Object.getPrototypeOf`.** Reassigning `Foo.prototype` is the only
  way to change what a constructor's instances inherit.
