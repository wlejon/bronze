# Architecture

```
source.ts
   │  src/lex      — tokens (hand-written lexer, hard errors on unknown input)
   ▼
tokens
   │  src/parse    — recursive descent, consumes ALL input or errors
   ▼
AST (src/ast)      — plain structs + visitor; canonical s-expr dump
   │  types        — (phase 3) TS annotations/inference → typed layouts
   ▼
IL  (src/il)       — typed SSA, canonical text form; static layouts default,
   │                 `dynamic` type only at declared boundaries
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
  peer of lex; il depends only on support). The CLI is the composition
  root.
- Every stage owns a canonical, deterministic text form. Tests compare
  bytes. That discipline caught dozens of real bugs in the predecessor
  project and is non-negotiable here.
