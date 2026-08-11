# 0023 — ES modules: the graph, the flat namespace, and the cycle refusal

Status: implemented. Phase 4 of docs/0001; the last of the "still open" list
in that doc's phase 4 paragraph.

Until this chunk a bronze build took exactly one file, and `import` was
`unsupported construct: import declaration (bronze has no modules yet)`
(docs/0014). It now takes a graph: an entry file, everything it reaches
through relative specifiers, parsed once each, ordered, linked, and lowered
to one IL module with one `main`.

The first two decisions were made before any code, because both of them have a
wrong answer that compiles and produces plausible output; the rest are what
building it turned up.

## Decision 1 — one module scope, and the linker renames into it

docs/0016 decision 1 rests on a fact that a graph appears to destroy: **the
module scope is entered exactly once**, so its environment record does not
have to be threaded through calls — `main` creates it, publishes it to
`g_moduleEnv`, and any module function loads it at entry. That is what keeps
top-level `function` declarations directly callable, which docs/0002
attributes the 11x on `fib` to.

With N files there are N module scopes. Two ways to keep the fact true:

- **N records.** Each file gets its own environment record and its own
  global; `module.env.get` grows an operand saying which. Every module
  function has to know which file it came from, `envDepthOf` has to count
  hops through a chain that now forks, and — the part that decides it —
  `src/types` reasons about *one* module scope today (docs/0010): its
  side table is keyed on names in a single top-level scope, and giving it N
  would be a second chunk inside this one.
- **One record, and the linker makes the names not collide.** The graph is
  flattened into a single `ast::Module` before inference or lowering sees
  anything, with every module-level binding of every non-entry file renamed
  to `mod<fileId>.<name>`.

bronze takes the second. Nothing downstream of the linker learns that
modules exist: inference still has one scope, closures still count the same
hops (docs/0007), `planModuleEnv` still allocates one layout, and the IL is
what it always was. The whole feature lives in `src/modules` and stops at
its boundary.

`mod<fileId>.` is chosen because a `.` cannot occur in a JavaScript
identifier, so a renamed name can never collide with a written one — the same
reason docs/0022 decision 1 put dots in `obj.<n>.<name>`. The **entry file is
file 0 and is not renamed at all**, so a single-file program produces exactly
the IL it produced before this chunk, byte for byte. That is not a
convenience: every one of the 120 single-file oracle cases and every pinned IL
dump in `tests/lower` and `tests/il` is a single file, and a linker that
renamed them would have made this chunk indistinguishable from a regression in
all of them.

### What this buys, and it is the reason to prefer it

**An import binding is not a copy — it is the same name.** `import { n }
from './counter.js'` does not create a binding `n`; it puts `n → mod1.n` in
the importing file's rename map, so every reference to `n` in that file
*is* a reference to the exporting file's binding, resolved by the one
resolver bronze already has. A live view cannot be got wrong here because
there is nothing to get wrong: there is one slot, and

```js
// counter.js          // main.js
export let n = 0;      import { n, bump } from './counter.js';
export function bump() { n += 1; }
```

sees `1` after `bump()` for the same reason a single file would. The chunk
brief flagged item 4 as the one that might want its own chunk; under this
decision it is not a feature at all, it is the absence of one.

### What it costs

A **scope-aware renamer** — `src/modules/rename.cpp`. A reference is renamed
only when it resolves to the file's module scope, so the walk carries a stack
of shadowing sets (a function's parameters plus its lexical declarations plus
its hoisted `var`s; a block's lexical declarations; a `for` header; a `catch`
parameter; a named function expression's own name). Getting that wrong in the
direction of shadowing too little is a wrong binding, which is silent — so the
walk is written to fail loudly instead: a node kind it does not recognise is
`internal error: the module linker cannot rename inside a <kind> node`, not a
subtree left alone. Every miss is therefore a build failure that names the
node, and one really was: `continue`, on the first run of the oracle suite.

Two names that are *not* identifiers still have to move with a renamed
binding, because the parser built them out of one:

- a class method's IL symbol is `<class>.<member>` (`parser_func.cpp`), so
  renaming `Foo` to `mod1.Foo` rewrites the `Foo.` prefix of each method's
  symbol with it;
- an object-literal method's symbol is `obj.<n>.<key>` with `n` an ordinal
  *per parser*, which is per file — two files each with an object method
  would have produced the same symbol. The ordinal is now qualified by the
  file id for every file but the entry, which keeps file 0's symbols exactly
  as they were.

`Span` grew a `file` field (defaulted to 0, so every span built before this
chunk is a span into the entry file and renders as it did), and diagnostics
render against a `SourceSet` rather than one buffer. Without it, an error in
`lib/geom.js` would have been reported at whatever text happened to be at that
byte offset of `main.js` — a plausible, wrong, and very confusing location.

## Decision 2 — a cycle in the module graph is a named hard error

ES modules hoist function declarations across the whole graph and evaluate
bodies in post-order, so in a cycle a module body can run while a binding of a
module *above* it in the cycle is declared but not yet initialised. Reading it
is a `ReferenceError` — the temporal dead zone, which bronze does not have:
there is no uninitialised-binding state and no `ReferenceError` for one, which
is exactly what `cases/blocked/temporal_dead_zone.js` has pinned since
docs/0016.

The three available answers were:

1. **Read `undefined`.** This is the answer that costs nothing and is a
   silent wrong answer of the worst kind — a real program that happens to be
   cyclic prints a number that is not the one it computed. docs/0000 puts this
   at the top of the list of things bronze exists to not do.
2. **Allow the cycle when every binding crossing a back edge is a hoisted
   `function` declaration.** This is sound for the crossing itself, and it is
   the relaxation real code needs, because the overwhelmingly common cycle in
   a library is two files exchanging functions. But it is only *locally*
   sound: `main.js` may call the imported function during its own evaluation,
   and that function may read a `let` in a file whose body has not run. The
   analysis that rules that out is a whole-graph reachability question over
   module-evaluation-time calls, and it is not this chunk.
3. **Refuse the cycle by name.**

bronze takes 3. `cyclic module dependency: a.js -> b.js -> a.js (bronze has
no temporal dead zone, so a binding read before its initialiser has run would
answer undefined instead of raising a ReferenceError)`. The message names the
edge that closed the cycle and the reason, so it is not "unsupported" with no
route forward.

The detection is exact — the graph load is a depth-first walk and a cycle is
a back edge, nothing statistical about it — which is what makes the refusal
honest: bronze never *silently* takes the undefined branch, because it never
reaches it. `cases/blocked/module_cycle/` holds the expectation for what a
function-only cycle must print when option 2 lands, so the case list says what
comes next (docs/0003's rule for `blocked/`).

Everything about the ordering follows from having refused cycles: the
evaluation order is the post-order of the DFS, it is total, and it is the
order the merged module's statements are concatenated in. Top-level `function`
declarations are hoisted across the whole graph for free, because `lower()`
already lifts every top-level `FunctionDecl` out of the statement list into an
IL module function before it lowers any body (docs/0016 decision 1) — so
graph-wide function hoisting is not something this chunk implements, it is
something it inherits.

## Decision 3 — relative specifiers only; a bare specifier is named, not guessed

`./x.js` and `../lib/y.js` resolve against the importing file's directory,
and that is the whole algorithm. `import x from "three"` is

    unsupported module specifier "three": bronze resolves relative
    specifiers only ('./x.js', '../lib/y.js'); a bare specifier needs a
    package resolution algorithm bronze does not have

A node_modules walk is a real algorithm with `exports` maps, conditions and
extension guessing in it, and every one of those is a place to guess wrong
about *which file* a program means. Guessing wrong there is not a compile
error, it is a different program. It gets its own chunk when it is worth one.

An extension is **not** guessed either: `./x` does not become `./x.js`, and a
directory does not become its `index.js`. The specifier names a file and the
file either exists or it is `cannot resolve module specifier "./x" from
<file>: no such file <path> (bronze does not guess an extension or a directory
index)`. An absolute specifier takes the same message as a bare one, because
it is the same missing thing: a base to resolve against that is not the
importing file.

A file reached twice through different spellings is one module: the loader
keys on `std::filesystem::weakly_canonical`, so `./a.js` and `./sub/../a.js`
are the same entry, parsed once, evaluated once. The key is an ordered
`std::map`, and the load is a depth-first walk of each file's specifiers *in
source order*, so the module numbering, the evaluation order and therefore the
IL text are functions of the source alone (docs/0001 decision 10 — a module
graph is a new place for a set's iteration order to reach an output path).

## Decision 4 — the namespace object is a literal of getters, and its limits are written down

`import * as ns from './x'` needs an object whose properties are *live views*
of `x`'s bindings. bronze has accessor properties (docs/0019 decision 4), so
the linker synthesizes

```js
const <ns> = { get a() { return mod1.a; }, get b() { return mod1.b; } };
```

which reads live, in the export order of the target module, and costs the
linker no new runtime concept at all. It is synthesized by *generating JS
text and parsing it with the real parser*, then renaming the placeholders —
not by hand-building AST nodes. A hand-built literal is a second answer to
"what does an object literal with a getter look like", and the two would drift
the first time the parser changed one field.

Where it is not a module namespace exotic object (ECMA-262 10.4.6), and these
are pinned as divergences rather than left to be discovered:

- A write, `ns.a = 1`, is silently ignored where the spec's `[[Set]]` returns
  false and module code is strict, so it must throw a `TypeError`. bronze has
  no strict mode either (`cases/blocked/strict_mode.js`), so a write is
  refused where the linker can *see* it — `ns.a = 1` written against a
  namespace binding is a named error — and undetectable once `ns` is passed
  to a function.
- `Object.keys(ns)` answers source order, where 10.4.6.2 sorts the names.
- `ns.missing` is `undefined` rather than a compile-time error, because the
  literal has no property of that name and bronze has no way to mark an object
  closed.

## Decision 5 — the oracle harness learns directories, and the single-file cases do not move

docs/0003's harness finds `cases/*.js` and pairs each with
`cases/<name>.expected`. A multi-file case needs several files with one
expectation, and the constraint is that nothing about how the existing cases
are found or compared may change.

A multi-file case is a **directory**: `cases/<name>/main.js` is the entry, the
rest of the directory is whatever it imports, and the expectation is
`cases/<name>/main.expected` — which means the pairing rule ("the entry's path
with the extension replaced") is the same rule, applied to a path one level
deeper. The harness gained one function that lists `<dir>/*/main.js` and skips
`blocked/`; everything after that — the determinism grep, the missing-`.expected`
hard failure, building twice and requiring the same bytes from inference and
`--no-infer` — is the code path the single-file cases already take, unchanged.
`cases/blocked/<name>/main.js` is the same convention for a blocked case.

The case *name* reported by doctest is `<dir>/main.js`, and the temporary
executable is named for the directory rather than for `main`, so two
multi-file cases cannot collide on `main_oracle.exe`.

## Decision 6 — an export clause is not a statement, anywhere downstream

`export { a as b };` names bindings and evaluates nothing (ECMA-262 16.2.3).
The linker deletes both module-item kinds before inference or lowering sees
the merged module, so in a real build the question never arises — but
`tests/types` and `tests/lower` parse one file and analyse it directly, and
they met the node the moment `export function f` started producing one.

The two passes therefore skip it, and *where* they skip it is the part that
had a wrong answer:

- inference skips it **before taking the statement index**, because the index
  is what its dump numbers statements by. Skipping it after would renumber
  every statement following an export in the dump — the artefact the whole
  types suite compares.
- `lower()` skips it when it builds the top-level statement list, not when it
  lowers one. A module whose entire content is exported function declarations
  has no top-level statements and therefore no `main`; counting the export
  clause as one gave it an empty `main` it never had, and the IL of every such
  file would have changed with nothing running in it.

An `ImportDecl` reaching lowering is the opposite case and is a hard error
naming the broken pipeline: an import BINDS a name, so one arriving unresolved
means the linker did not run, and lowering it as nothing would leave the name
reading `undefined`.

## Decision 7 — `isExported` is a fact about the module, so the linker sets it

`export function f() {}` sets `FunctionDecl::isExported` in the parser, and
`function f() {} export { f };` is the same fact spelled elsewhere.
`src/types/escape.cpp` reads the flag to decide a function escapes, so leaving
the second spelling unmarked would let inference prove a signature from the
call sites it can see for a function whose exportedness says there may be
others. The linker sets the flag from the export table, which is the only
place both spellings have been reduced to one answer.

## What a module is, mechanically

`src/modules` is four seams and four files, because the graph, the paths, the
export tables and the renaming fail in four different ways:

| File | What it owns |
|---|---|
| `resolve.cpp` | a specifier and an importing file → a canonical path, or a named error |
| `graph.cpp` | read, lex, parse each file once; the DFS; the cycle refusal; post-order |
| `link.cpp` | export tables, `export * from` expansion, import binding resolution, the namespace literal, the merge |
| `rename.cpp` | the scope-aware alpha-renaming walk |

The CLI calls `modules::loadProgram(entry, diags)` and gets back one
`ast::Module` and the `SourceSet` its spans point into. `driver.cpp` gained no
knowledge of specifiers, paths or graphs; it still lexes-parses-infers-lowers,
except that the first two steps now happen inside the loader.

## What this cost the pinned tests

One pinned assertion changed, and it is the only one: `tests/parse`'s
`function with typed params, if/else, calls` dumps an `(export (name max as
max))` node beside the `export function` it always had. That is a new true
fact about the tree rather than a weakened expectation — `export { a as b }`
and `export ... from` cannot be spelled as a flag on a declaration, so every
export form reduces to an export-entry node and the declaration form does too.
The alternative, suppressing the node for the one form that has a flag, is two
mechanisms for one question and the drift between them would be an export
bronze forgets it has.

`tests/parse`'s ``import` is diagnosed by name rather than as a missing
expression` is replaced rather than deleted, by three cases that pin what the
production now accepts and the three forms it still refuses. No `.expected`
file was touched, and no case was renamed or repurposed.

## Errors this chunk added

- `unsupported module specifier "<s>": bronze resolves relative specifiers
  only ('./x.js', '../lib/y.js'); a bare specifier needs a package resolution
  algorithm bronze does not have`
- `cannot resolve module specifier "<s>" from <file>: no such file <path>
  (bronze does not guess an extension or a directory index)`
- `cannot read module <path>`, and `module graph exceeds 4000 files; bronze
  numbers files in 16 bits`
- `cyclic module dependency: <a> -> <b> -> <a> (…)` — decision 2, with the
  whole loop named rather than "there is a cycle somewhere".
- `module <file> has no export named '<n>'`
- `duplicate export '<n>' in <file>`, and `ambiguous export '<n>' in <file>:
  two 'export * from' sources provide it` — where ECMA-262 makes the name
  ambiguous and therefore silently absent, which is a hole rather than an
  answer.
- `duplicate import binding '<n>' in <file>`
- `'<n>' is imported and also declared in <file>`
- `export '<n>' names nothing declared in <file>`
- `unsupported construct: dynamic import() (bronze has no promises)` and
  `unsupported construct: import.meta` — both diagnosed from the EXPRESSION
  grammar as well as the statement one, because neither ever reaches the
  statement production and "expected expression" names nothing.
- `an import or export declaration may only appear at the top level of a
  module` — a ModuleItem is not a Statement (16.2).
- `a reserved word imported by name needs 'as': write 'import { default as
  d }'`
- `cannot assign to '<ns>.<k>': it is a binding of the imported module
  namespace <file>, which is read-only` — decision 4.
- `unsupported construct: 'export ='` and `unsupported construct: 'import x =
  require(...)'` — the TypeScript export/import assignment forms, named
  rather than read as an expression.
- `internal error: the module linker cannot rename inside a <kind> node`, and
  `internal error: an import declaration reached lowering; the module linker
  did not run` — neither is reachable from any source text, and both exist so
  that a gap in this chunk's own walk is a build failure with a name on it
  rather than a subtree quietly left un-renamed. The first one fired during
  development, on `continue`.

## What this chunk stopped short of

- **Cycles**, per decision 2. `cases/blocked/module_cycle/`.
- **Bare specifiers and `node_modules`**, per decision 3.
  `cases/blocked/module_bare_specifier/`.
- **The namespace exotic object**, per decision 4.
  `cases/blocked/module_namespace_object/`.
- **`import()`** — dynamic import is a promise-returning form and bronze has
  no promises; it is a named error.
- **`import()`** — dynamic import returns a promise and bronze has none.
- **A pre-existing hole this chunk did not open and did not close**: a named
  function expression cannot refer to itself (`const f = function rec(n) {
  ... rec(n - 1) ... }` is `undefined variable: rec`). It was found while
  probing the renamer's shadowing, reproduces on a single file with no
  modules in sight, and is left where it was found rather than fixed inside a
  chunk about modules.

## What is right by construction rather than by care

Three things this chunk did not have to build, and it is worth writing down
which they are so that a later change does not assume they were designed:

- **Graph-wide function hoisting.** `lower()` lifts every top-level
  `FunctionDecl` out of the statement list into an IL module function before
  it lowers any body (docs/0016 decision 1), and the merge concatenates
  statement lists — so every module's function declarations are hoisted above
  every module's body, which is what 16.2.1.6.4 asks for.
- **A `var` at a non-entry module's top level.** It is renamed and put in the
  one module record like any other module-level binding, and a same-named
  `var` in another file is a different name, because module scopes really are
  separate in ES too.
- **`--no-infer` producing identical bytes across a graph.** There is one
  merged module and one pipeline behind it; the switch never sees a graph.
</content>
