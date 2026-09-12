# In-Memory Dynamic Evaluation & JIT in Bronze

Bronze supports native in-memory execution and dynamic script evaluation via the Brass JIT engine (`src/eval` and `src/codegen-brass/brass_jit.h`).

Unlike legacy setups that shell out to compilers and linkers to generate temporary `.so`/`.dll` files on disk, Bronze compiles JavaScript source code directly from memory into executable native machine code (`.text` segments mapped with `PROT_READ | PROT_EXEC`) in RAM, resolves runtime and host symbols, and executes immediately with **zero disk I/O and zero external dependencies**.

---

## 1. Architecture

```
JavaScript source string (eval, new Function, or host call)
   │
   ▼
Lexing & Parsing (src/lex, src/parse)
   │
   ▼
AST Transformation & Global Export (src/eval/eval.cpp)
   │
   ▼
Type & Shape Inference (src/types)
   │
   ▼
Lowering to Bronze SSA IL (src/lower, src/il)
   │
   ▼
Brass In-Memory JIT Compilation (src/codegen-brass/brass_jit.cpp)
   ├─ Translates Bronze IL to Brass IR
   ├─ Emits in-memory object image (brass::object::ObjectFile)
   ├─ Loads into executable memory via brass::codegen::JitExecutionEngine
   └─ Binds bronze runtime symbols (register_bronze_runtime_symbols)
   │
   ▼
Direct Native Execution in Process RAM
   ├─ Executes compiled entry point: void (*)() / uint64_t (*)()
   ├─ Synchronizes thrown exceptions via rtTls()->exception_cell
   ├─ Drains pending Promise microtasks (embed::drainMicrotasks())
   └─ Retains executable code image in memory (retainedPrograms())
```

---

## 2. Zero-Disk Design

Previous dynamic execution architectures required:
1. Writing source text to a temporary `.js` file on disk (e.g., `/tmp/bro_eval/eval_123.js`).
2. Invoking an AOT build pipeline (`bronze build` / `bronze::cli::runBuild`).
3. Calling an external C++ compiler or linker (`clang++`, `g++`, `lld`, `link.exe`) to produce a shared object (`.so` / `.dll`).
4. Invoking `dlopen()` / `LoadLibrary()` on the generated file.
5. Deleting temporary files from disk.

Bronze eliminates this entire overhead:
- **No temporary files:** Compilation happens purely in memory.
- **No external linkers:** Symbol resolution and relocation happen in-memory through `brass::codegen::JitExecutionEngine`.
- **Instant startup & execution:** Millisecond turnaround suitable for runtime UI evaluation, `new Function`, and hot evaluation paths.
- **Memory safety & code lifetime:** Following Bronze's "leak-the-image" design, compiled code buffers and exception tables remain mapped throughout the process lifetime so function pointers and closures never become dangling pointers.

---

## 3. Dynamic Built-in Hooks (`eval` and `new Function`)

In standard JavaScript, `eval` and `new Function` are builtin constructs. Bronze routes them through runtime host seams:
- `rtSetDynamicEvalHost` / `rtSetDefaultDynamicEvalHost`
- `rtSetDynamicFunctionHost` / `rtSetDefaultDynamicFunctionHost`

When `bronze::eval` is linked into a binary or host, it registers `installDefaultDynamicHooks()` via static initialization. Any script running inside Bronze can call:
```javascript
const result = eval("40 + 2"); // returns 42

const add = new Function("a", "b", "return a + b;");
console.log(add(10, 25)); // prints 35

const gen = new (function*(){}).constructor("yield 1; yield 2; return 3;");
const asyncFn = new (async function(){}).constructor("x", "return x * 2;");
```
All four kinds of dynamic functions (`Ordinary`, `Generator`, `Async`, `AsyncGenerator`) compile directly in memory.

---

## 4. Public C++ API (`src/eval/eval.h`)

Hosts (such as CLI tools, application runners, game engines, or browser environments like `bro`) can directly evaluate code using the public `bronze::eval` API:

```cpp
#include "eval/eval.h"

// Options controlling evaluation
struct EvalOptions {
    std::string filename = "<eval>";
    std::vector<std::string> hostGlobals = {};
    bool retainSource = true;
    std::vector<modules::ModuleRoot> moduleRoots = {};
    std::filesystem::path entryResolvesAs = {};
};

// Evaluates a script in memory and returns a CallResult (value or thrown error)
embed::CallResult evalScript(std::string_view source, const EvalOptions& options = {});

// Evaluates a file and its transitive module graph directly in memory via Brass JIT
embed::CallResult evalFile(const std::string& filePath, const EvalOptions& options = {});

// Direct evaluation returning Value (sets runtime exception cell on throw)
Value evalScriptDirect(std::string_view source, const EvalOptions& options = {});

// Compiles a dynamic Function object from parameters and body
Value evalFunction(runtime::DynamicFunctionKind kind, std::span<const Value> args);
Value evalFunction(std::span<const std::string> params, std::string_view body,
                   runtime::DynamicFunctionKind kind = runtime::DynamicFunctionKind::Ordinary);

// Installs default in-memory hooks for eval and Function
void installDefaultDynamicHooks();
```

---

## 5. CLI Commands

The `bronze` CLI provides direct subcommands for in-memory execution:

### `bronze run <file>`
Executes a JavaScript file directly in memory via JIT, resolving any relative module imports:
```bash
bronze run script.js
```

### `bronze eval <code>` (or `bronze -e <code>`)
Evaluates an arbitrary JavaScript expression or statement sequence in memory:
```bash
bronze eval "Math.max(10, 25) * 2"
bronze -e "const f = new Function('x', 'return x * 10'); f(4.2)"
```

---

## 6. Integration Guide for Hosts (e.g. `bro`)

For hosts embedding Bronze:
1. Link `bronze::eval` (or `bronze::cli` which re-exports it).
2. For evaluating scripts or UI components, call `bronze::eval::evalScript(source, options)` or `bronze::eval::evalFile(path, options)`.
3. If using custom host globals (like `document`, `window`, `WebGL2RenderingContext`), register them using `bronze::embed::registerHostGlobal`. Bronze's JIT evaluator automatically discovers all registered host globals from `runtime::rtHostGlobalEntries()`, making them accessible inside dynamic scripts without any manual configuration.
