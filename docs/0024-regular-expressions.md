# 0024 — Regular expressions: the literal, the pattern, the matcher, and the object

Status: implemented, with named gaps. Phase 4 of docs/0001, alongside the
builtins of docs/0011.

Until this chunk `/` was always division and `str.match` was
`unsupported: String.prototype.match is not implemented`. It is now a lexical
form, a grammar and a matcher of its own in `src/regex`, a heap object kind in
the runtime, and six `String.prototype` methods.

The layering is deliberate: **`src/regex` knows nothing about Values, and the
runtime knows nothing about the pattern grammar.** The module takes
`std::u16string_view` in and gives capture *offsets* out; everything that is a
string, an array, a property or a GC root lives above it. That is what lets the
pattern compiler run inside the *parser* — a malformed pattern is a compile
error at the literal, not a surprise the first time the line runs — while the
same compiled pattern is what the runtime executes.

## Decision 1 — the `/` ambiguity is decided from the previous token

`a /b/ c` is three divisions or one regular expression, and nothing in the
character stream distinguishes them. The lexer keeps the token it last emitted
and asks whether a *division* could legally follow it: after an identifier, a
number, a string, a template tail, `)`, `]`, `++`, `--`, `this`, `super`,
`true`, `false`, `null` or `undefined`, a `/` divides; after anything else —
every operator, every keyword like `return` and `typeof`, `(`, `[`, `,`, `;`
and `{` — it starts a regular expression.

Two entries in that list are judgement calls, and both are the ones every
implementation gets asked about:

- **`)` divides.** `(a + b) / 2` is common; `if (x) /re/.test(s)` is not. The
  correct rule needs the *parser* to say whether the `)` closed an `if`/`for`/
  `while` head or an expression, which is a coupling this lexer does not have.
- **`}` starts a regular expression.** `}` ends a block far more often than it
  ends an object literal, and after a block a `/` is a regular expression.
  `({}) / 2` needs the parenthesis it already has to be sensible.

Both are pinned in `tests/lex`, because getting this wrong is not a diagnostic
— it is a silent mis-parse that compiles to something else entirely. The other
half of the same risk is the literal's *extent*: `/` inside a character class
does not end it (`/[/]/`), a `/` behind a backslash does not end it, and a
literal may not cross a line terminator, so `/[a/;` is an unterminated literal
rather than a pattern that swallows the rest of the file.

The literal's body is taken **verbatim**. It is not string-escape-decoded: `\d`
is two characters that the pattern grammar reads, and decoding it the way a
string literal is decoded would leave the matcher a `d`.

## Decision 2 — the pattern is compiled at the literal, and shared by source

A regular expression literal lowers to a *construction* of the `RegExp`
intrinsic, not to a constant: 22.2.4.1 makes a fresh object with its own
`lastIndex` every time the literal is evaluated, so a literal inside a loop
that is used as a cursor must not be shared. That would make a literal inside a
loop *compile* its pattern per iteration, which is why the compiled pattern is
not in the object.

Instead the runtime owns a table of compiled patterns, and the object holds an
**index** into it as a plain number. Two regular expressions with the same
source and the same flags share one compilation; the memo is keyed on the
canonical flag text followed by the source, so `/a/gi` and `/a/ig` are one
entry and `/a/g` and `/a/gi` are two (`i` compiles different nodes, not a
different mode). The table is a `std::vector`, the memo an ordered `std::map`
— never a hash map, because the index is observable through nothing but
identity today and determinism is cheaper to keep than to restore.

The index-not-pointer part is not an optimisation. A compiled pattern is a tree
of `unique_ptr`s; the collector scans every object payload as an array of
Values and relocates what it finds. A pointer to C++ memory in a heap object is
a crash waiting for the first collection.

## Decision 3 — a backtracking matcher, with continuations, and budgets

22.2.2 defines matching as continuation-passing over a backtracking search, and
that is what `src/regex/matcher.cpp` is: a `Cont` chain describing what remains
after the current node, and a recursive `matchNode` that calls it. No
compilation to bytecode, no JIT, no automaton. The spec's own structure is
worth following literally here, because the parts of it that look like
implementation detail are observable — in particular 22.2.2.5.1's rule that a
repeated Atom's own captures are **cleared at the start of every turn** (so
`/(?:(a)|b)*/.exec("ab")` leaves group 1 `undefined`, not `"a"`), and its guard
that an iteration matching the empty string ends the repetition rather than
looping forever.

Two departures, both about not falling over:

- **A counted fast path for simple atoms.** A `Repeat` whose atom is one unit
  wide and captures nothing (`.*`, `[a-z]+`, `a{3,}`) is scanned with a loop
  and then backtracked by decrementing a count, instead of recursing once per
  input unit. Without it `/.*b/` over a long string overflows the C++ stack,
  which is a crash rather than an answer. Pinned with a 50 000-unit input.
- **A step budget and a depth cap.** `/(a+)+b/` against a long string of `a`s
  is exponential by construction; the matcher gives up after a fixed number of
  steps or 2 000 nested frames and reports an error, which the runtime turns
  into a fatal naming catastrophic backtracking. Both limits are constants
  rather than clocks, so a pattern that fails fails identically everywhere —
  a wall-clock timeout would make the same program pass on one machine and
  fail on another.

## Decision 4 — case-insensitivity is ASCII and Latin-1, and everything else is refused

22.2.2.9 Canonicalize is defined by the Unicode Default Case Conversion table.
bronze carries the part of that table it can write down and check by hand: the
ASCII letters, the Latin-1 supplement, and its two escapes from itself (U+00B5
uppercases *out* of Latin-1 to U+039C, U+00FF to U+0178, and U+00DF stays
because its uppercase is two units).

For every other code point the fold is *unknown*, and there are only two honest
options: guess, or refuse. It refuses — by name, at compile time, naming the
code point:

```
unsupported: case-insensitive matching of U+03A9 (bronze carries no Unicode
case tables; only ASCII and Latin-1 fold under the `i` flag)
```

The refusal is over *blocks* that contain cased characters (Greek, Cyrillic,
Armenian, Georgian, Cherokee, the Latin extensions, the letterlike and
enclosed-alphanumeric blocks, fullwidth forms), not over individual characters,
because a block boundary is checkable and a character list is a place for a
silent hole. The cost is deliberately narrow: it applies only under `i`, only
to characters written in the *pattern*, so `/日本/i` and `/😀/i` still compile,
and only case-insensitive **backreferences** can reach it at run time (where it
is a fatal, since the pattern is legal and the input decided it).

The reason for refusing rather than approximating is docs/0000's rule in its
purest form: `/Ω/i` quietly not matching a lowercase omega is a wrong
answer that no test catches unless someone already suspected it. A named error
is caught by the first person who writes the pattern.

`classMatches` asks the question 22.2.2.7.1 actually asks — does the *set*
contain a member that canonicalizes to what the input canonicalizes to — rather
than the easier one, because that is what makes `/[α-ω]/i` match a capital
gamma once the tables exist, and it is what makes `/[a-z]/i` match `ABC` today.

## Decision 5 — what the pattern grammar refuses, and why each one is separate

Everything below is a named error at the literal, never a silent skip:

| Construct | Why not |
| --- | --- |
| `(?<=` / `(?<!` lookbehind | 22.2.2.6 matches it *backwards*: the terms of an Alternative in reverse, every quantifier against a decreasing index. The matcher threads a forward continuation through every node, so direction is not a parameter it has. |
| the `u` and `v` flags | They change the alphabet from the code unit to the code point: `.` and classes take a surrogate pair as one character, `\u{...}` becomes spellable, ranges span above U+FFFF, and the Annex B leniencies switch off. |
| `\p{...}` / `\P{...}` | Legal only in UnicodeMode, and each one needs real UAX #44 category and script tables. |
| the `d` flag (`hasIndices`) | Match indices are a second array of pairs on the match object; nothing else needs them. |
| legacy octal escapes | Annex B only, and `\1` is already a backreference; guessing which one `\01` means is exactly the ambiguity to leave shut. |
| `\q` and other alphanumeric identity escapes | Reserved by 22.2.1 for future extensions; accepting them as literal letters is how a future `\q` silently changes meaning. |

The first three are seeded as failing cases in `tests/oracle/cases/blocked/`,
with the behaviour a correct implementation would print already derived, so
that the day one of them lands the harness's promote-on-pass rule notices. They
are one chunk between them: `u` and `\p{...}` need the same generated Unicode
tables that decision 4's case folding needs.

Annex B *is* implemented where it is unambiguous: a `{` that does not begin a
valid quantifier is an ordinary character (`/a{/`), and `[]` is the empty class
that matches nothing while `[^]` matches anything — a rule that reads backwards
to everyone who learned POSIX.

## Decision 6 — the match array is an array with named properties

22.2.7.2 makes `exec`'s result an Array that also carries `index`, `input` and
`groups`. bronze arrays are a dense `Value` vector with a length and no
property table, and named writes on one are a hard error.

Two ways in: a new heap object kind for match arrays, or a property table on
the array. A new kind loses — `flags == 1` is checked by `Array.isArray`, every
array method, spread, `for-of` and `JSON.stringify`, and a second array-like
kind means finding and correcting all of them, with a silent wrong answer at
every site that is missed.

So `ArrayHeader` grew one field: `Value properties`, `undefined` until
something needs it, and then a plain object. Only the runtime ever creates it —
`o.x = 1` on an array from *JavaScript* is still the hard error it was, because
the case for supporting it has not been made and weakening a refusal is not a
side effect this chunk gets to have. Reads consult it before the array-method
table, `Object.keys` appends its keys after the indices, and `console.log`
prints them after the elements, which is where docs/0013's format puts them.

The three properties are created in the order 22.2.7.2 creates them, because
docs/0009's shape chain makes creation order enumeration order, and enumeration
order is printed output.

## Decision 7 — no `RegExp.prototype`, so the symbol protocols are direct

22.1.3.19 defines `String.prototype.replace` as *looking up* `@@replace` on its
argument and calling it. bronze has no intrinsic prototype objects at all
(docs/0011 decision 2), and this chunk did not add one: `re.exec` is handed out
by the property path, `RegExp.prototype` is not a value a program can hold, and
`re instanceof RegExp` does not work — `instanceof` against a non-callable
right-hand side is a TypeError today, for `Object` as much as for `RegExp`.

The consequence is that `"s".replace(re, r)` reads `re`'s flags and calls the
matcher, rather than dispatching through a method a program could have
replaced. Monkey-patching `RegExp.prototype[Symbol.replace]` is therefore not
observable — it is not even expressible. That is the same trade docs/0011 made
for `Math` and `Array.prototype`, and it lands here for the same reason: the
prototype objects are one coherent chunk of work, and half of one is worse than
none.

Members that 22.2.6 defines and bronze has not built (`unicode`, `hasIndices`,
`compile`, the `@@` methods) are named errors rather than `undefined`, so that
reaching for one is a diagnostic and not a `TypeError: undefined is not a
function` three lines later.

## Decision 8 — a bad pattern throws, a broken match is fatal

Two failure kinds, deliberately different:

- **A pattern or flag string that does not parse is a catchable `SyntaxError`**
  when it comes from `new RegExp(...)`, because 22.2.3.1 says so and because
  the string may have come from user input at run time. This chunk added
  `SyntaxError` to the error family of docs/0020 for it. When the same failure
  comes from a *literal*, it is a compile error instead — there is no run time
  in which to catch it.
- **A match that gives up is a fatal.** Catastrophic backtracking and a
  case-insensitive backreference over an unfoldable code point are not
  conditions ECMA-262 has, so there is no `Error` subclass they belong to and
  no `catch` that would be meaning anything by catching them. They stop the
  program with a message naming the pattern's problem.

## Decision 9 — `source` is always a pattern that could have been written down

22.2.6.10 EscapeRegExpPattern requires `source` to be text that would parse
between two slashes. Only a pattern built from a *string* can violate that, and
it can in three ways: an empty pattern (`//` is a line comment), a bare `/`,
and a line terminator. All three are escaped when the object is made — before
the compile, so the pattern that the source describes is the pattern that runs
— giving `new RegExp("").source === "(?:)"`, `new RegExp("a/b").source ===
"a\\/b"`, and a `console.log` of either that prints a literal you could paste
back.

## What is not built, beyond the table in decision 5

- `RegExp.prototype.compile`, `RegExp.$1`, and the other legacy statics.
- `lastIndex`'s non-writability on a non-global pattern (assigning to it is
  allowed and then ignored, which is what the flags already imply).
- `String.prototype.replace` does not update `lastIndex` on a non-global sticky
  pattern; 22.2.6.11 leaves it alone for non-global patterns generally, and the
  sticky-only corner it opens has no test that distinguishes it.
- The `Symbol.species` protocol in `split`, which needs the `constructor`
  lookup that needs prototypes. The limit, the capture splicing and the
  empty-string rules of 22.2.6.14 are all there; only the species is not.
