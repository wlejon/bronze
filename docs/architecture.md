# Architecture

```
source.ts
   │  src/lex      — tokens (hand-written lexer, hard errors on unknown input)
   ▼
tokens
   │  src/parse    — recursive descent, consumes ALL input or errors
   ▼
AST (src/ast)      — plain structs + visitor; canonical s-expr dump
   │
   ├─▶ src/types   — inference over the AST: flow-sensitive types per
   │                 binding, shape classes per object site, and signatures
   │                 joined over the call graph. Produces a SIDE TABLE
   │                 (`types::InferenceResult`) keyed by AST node; mutates
   │                 nothing. Canonical dump: `bronze types <file>`
   │  src/lower    — AST + side table → IL. The only consumer of the table,
   │                 and the only place that knows it can be absent, which
   ▼                 is all `--no-infer` is
IL  (src/il)       — typed SSA, canonical text form. Native types where
   │                 inference PROVED them; `dynamic` everywhere else, which
   │                 is the sound fallback, not a failure. A TS annotation
   │                 never types anything on its own (docs/0010 decision 6)
   ▼
Backend (src/codegen interface)
   │  src/codegen-llvm  — LLVM, gated by BRONZE_WITH_LLVM
   ▼
object file → system linker → exe
```

Module rules:

- Each `src/<module>` is a static library `bronze::<module>` with its own
  doctest binary and ctest label. Test scope = module scope.
- Dependency edges point downward only (support ← lex ← parse; ast is a
  peer of lex; il depends only on support; types depends on ast and support
  and on nothing below it — it must never learn about the IL; lower depends
  on ast, il and types). The CLI is the composition root, and it is where
  inference is run and its result handed to lowering.
- Every stage owns a canonical, deterministic text form. Tests compare
  bytes. That discipline caught dozens of real bugs in the predecessor
  project and is non-negotiable here.
