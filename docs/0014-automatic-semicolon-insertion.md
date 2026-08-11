# 0014 — Automatic semicolon insertion, and what it uncovered

Status: implemented. Part of phase 4 of docs/0001.

bronze required a semicolon after every statement. That is not a subset of
JavaScript, it is a different language: `function f() { return 1 }` did not
compile, and neither did any of the several million lines of published JS
written without terminators. ASI is not a style accommodation — ECMA-262
12.10 is part of the grammar, and a `return` alone on its line means
something specific that a program can depend on.

The bar is three.js, which is written with semicolons throughout. That is
exactly why this went unnoticed for so long, and it is not a reason to skip
it: the restricted productions below change what semicolon-styled code
*means*, so an implementation without them can read correct source and
produce a wrong answer rather than an error.

## Decision 1 — the newline is a lexer fact; the insertion is a parser rule

Trivia is discarded in `skipTrivia`, so by the time the parser runs, the
only thing that can still answer "was there a line break here?" is a fact
the lexer recorded. `Token::newlineBefore` is that fact and the whole of it:
one bool per token, set from the trivia consumed before it. The grammar is
otherwise whitespace-insensitive and stays that way.

The insertion itself cannot live in the lexer, and the reason is decision 2.

## Decision 2 — insertion happens at the *offending token*, and nowhere else

`Parser::consumeSemicolon` supplies a missing terminator when the token it
is looking at is on a later line, is the `}` closing the enclosing block, or
is the end of input. Those are the spec's three conditions and there is no
fourth: `let a = 1 let b = 2` on one line is the error it always was.

What makes this a parser rule is that it runs *after* the expression grammar
has taken everything it can:

```js
const c = 1
+ 2
```

is one addition, because `parseExpr` consumed the `+ 2` before anything
asked for a semicolon. A lexer inserting semicolons at line ends would have
to decide this without knowing it was inside an expression, and would get it
wrong in whichever direction it guessed. The same rule is why `"abc"` on one
line and `.toUpperCase()` on the next is a single method call.

The corollary is that the famous ASI hazards are reproduced rather than
fixed. `const x = foo` followed by a line starting `(` is a call, because
that is what the language says it is. bronze is not the place to improve on
that.

## Decision 3 — the restricted productions are where this changes meaning

Five productions forbid a line terminator at a specific point, and each one
turns a line break into a different program rather than into an error:

- `return` — `return\n  value` returns **undefined**, and the value below is
  a separate, unreachable statement. This is the single most consequential
  rule in ASI and the reason it cannot be treated as cosmetic.
- `break` and `continue` — the identifier on the next line is the next
  statement, not the label.
- postfix `++` and `--` — the operator on the next line belongs to the
  following statement as a **prefix** operator. `let e = d` then `++d` on
  the next line leaves `e` at `d`'s old value and increments `d`; folding
  the two lines together would be a silent wrong answer, which docs/0000
  names as the failure mode worth the most vigilance.
- `throw` — the one restricted production with no fallback reading, because
  there would be nothing to throw. The spec makes it a syntax error, so
  bronze diagnoses it by name rather than inserting.

Pinning the postfix rule required a change in the AST dump: `d++` and `++d`
both printed as `(unary ++ ...)`, so the two constructs that this rule
exists to separate were indistinguishable in the one artefact the parser's
tests compare. They now print `++post` and `++pre` — the same rule docs/0012
decision 3 applied to arrows, that two constructs which lower differently
must not dump identically.

## Decision 4 — a `for` header's semicolons are not statement terminators

`for (let i = 0; i < n; i++)` contains two semicolons that are punctuation
of the `for` production. ASI does not apply to them, so `parseVarDecl` takes
a flag saying which position it is in, and the header keeps `expect`. The
statement terminators all route through `consumeSemicolon` and the
production's own punctuation never does; there is one call site each way and
no third rule that could drift from either.

## Decision 5 — unreachable code is dropped, and that is the language's answer

Making `return\n  1` parse correctly immediately produced a program bronze
could not compile: the `1` is a real statement in a block whose terminator
has already been emitted, so lowering appended instructions after a `ret`
and the IL verifier rejected the function. The bug was never about ASI —
`function f() { return 3; g(); }` failed the same way and always had,
reporting the verifier's internal wording to the user.

`lowerStmtList` now stops at the terminator. This is not a silent fallback
in the sense docs/0001 decision 8 forbids: unreachable code has no effect,
so dropping it is what the language specifies, and every engine does the
same. What a dead region *can* still contribute is hoisting, and it still
does — the hoisting pass runs over the whole list before the reachability
loop begins, so a function declared below a `return` is bound before any
statement runs and a call above it resolves. The two being separate passes
is what makes that true, and `unreachable_code` pins it.

## Decision 6 — a function that returns no value evaluates to undefined

The same case exposed a second live bug with the same shape. A function with
no returned value lowers to a void IL function, and lowering handed the void
call's *absent* result back as the expression's value — so `console.log(f())`
reached the verifier as a box of the no-value sentinel, and any use of such
a call as a value failed to compile.

The IL return type stays void, because the call genuinely returns nothing
and paying for a boxed undefined on every call would be a real cost. The
undefined is materialized at the call site instead, where the only thing
that can see it is the expression the call sits in. A void call used as a
*statement* is the common case and still emits nothing extra worth naming —
the constant is dead and does not survive the backend.

## Named diagnostics

- `a line terminator is not allowed between 'throw' and its expression` —
  decision 3, the only restricted production that is an error rather than a
  different program.
- `expected ';' after <what>` — unchanged in wording, and now reached only
  when none of decision 2's three conditions holds.
- `unsupported construct: import declaration (bronze has no modules yet)` —
  not ASI, but found in the same sweep and the same kind of hole. `import`
  has lexed as a keyword since the beginning and never had a production, so
  it fell through to the expression parser and reported "expected
  expression", naming nothing. It was the one unimplemented construct in
  bronze that was not diagnosed by name (docs/0001 decision 8). Modules stay
  phase-4 backlog; the error is now honest about which one it is.
