# 0013 — console.log of a container

Status: decided and landed 2026-08-11.

Every oracle case since docs/0003 has routed its results through `join`,
`length` or a scalar, with the same sentence in each header: *console.log of
a container has no pinned format yet, and a case must not decide that in an
`.expected`*. Arrays, objects, classes and prototypes have all landed since,
and `console.log([1, 2])` still printed `[object]`. That is now the thing
standing between bronze and a case that reads like the program someone would
actually write, so this doc decides it.

## Decision 1 — the format is node's `util.inspect`, single line

`console.log` of an object is not `String(obj)` — the language's ToString
would say `[object Object]`, which nobody debugging wants. Every JS runtime
prints a structural inspection instead, and node's is the one bronze is
measured against (docs/0003), so bronze's is node's:

```
[ 1, 2, 3 ]         []          { a: 1, b: 'x' }        {}
[ 'a', "it's" ]     [ true, null, undefined, -0 ]
Float32Array(3) [ 0, 0, 0 ]
```

- one space inside non-empty brackets and braces, none in empty ones
- strings are quoted INSIDE a container and raw at the top level, exactly as
  node does it: `console.log("hi")` is `hi`, `console.log(["hi"])` is
  `[ 'hi' ]`. Single quotes, unless the string contains one and no double
  quote (then `"`), or contains both (then a backtick) — node's rule.
- control characters inside a quoted string are escaped (`\n`, `\t`, `\xNN`)
- an identifier-like key is bare, anything else is quoted: `{ a: 1 }` but
  `{ 'a-b': 1 }` and `{ '2': 'two' }`
- own keys come out in the language's order — integer-like ascending, then
  insertion order (docs/0009). It is the same question `Object.keys` answers,
  so it is the same code path, and the two cannot drift.
- `-0` prints as `-0`, here and at the top level. It is inspect formatting,
  not ToString(Number), and the sign of zero is observable.

## Decision 2 — depth 2, and cycles are marked, not followed

node's default `depth: 2`: a container nested deeper prints as `[Array]` or
`[Object]` rather than being expanded. bronze does the same, so
`[[1,[2,[3,[4]]]]]` is `[ [ 1, [ 2, [Array] ] ] ]`.

A reference back to an object already on the path prints `[Circular *1]`,
and the whole output is prefixed `<ref *1> `. Without this, printing a cycle
is not a formatting difference, it is a hang — and three.js objects have
parent pointers.

## Decision 3 — the divergences from node, named

These are deliberate, and each is here so it is not mistaken for a bug:

1. **One line, always.** node breaks a long container across lines at a
   terminal-width heuristic, and groups numeric arrays into columns. That
   heuristic is node-version-specific formatting, not semantics; matching it
   byte-for-byte would pin bronze to a node version, and the harness never
   runs node to check (docs/0003). Every oracle case keeps its containers
   short, where the two agree.
2. **No names on functions or instances.** A function object carries no
   name — nothing in the runtime has ever needed one — so a function prints
   `[Function]` where node prints `[Function: sum]`. Printing
   `[Function (anonymous)]`, node's text for a function that really has no
   name, would be a lie about a named one; `[Function]` says what bronze
   knows. The same gap makes a class instance print `{ x: 1, y: 2 }` where
   node prints `Point { x: 1, y: 2 }`: the constructor name would have to
   reach the prototype object, and nothing carries it there.

   The fix is a name on `FunctionHeader`, which changes how every function
   object is created, so it is its own step. When it lands,
   `print_containers.expected` GAINS those names — an expectation moving
   towards node, which is the ratchet turning, not being weakened.
3. **One circular marker.** node numbers each circularly-referenced object
   (`*1`, `*2`, …). bronze marks every back-reference `*1`. For the common
   case — a cycle back to the object being printed — they agree exactly.
4. **`ArrayBuffer` is a hard error, not `<00 00 …>`.** Its inspect form is a
   byte dump bronze has no case for; naming it is better than inventing one.

## Where it lives

`src/runtime/inspect.cpp`, not rt_helpers.cpp: it is a recursive walk with
its own rules and no business in the print helper. Nothing in it allocates a
JS value, which is what makes the raw pointers it walks safe — the collector
cannot run while the string is being built (docs/0006).

## What this unblocks

`print_containers` is the first oracle case that prints a container, and
future cases no longer have to route results through `join`. The `join`
spelling stays where the case is ABOUT the method, not about printing.
