# In-Memory Dynamic Evaluation & JIT in Bronze

Bronze runs JavaScript in process, from memory: `src/eval` compiles source text or a module graph to brass MIR and runs it through the tiered engine (`src/codegen-brass/brass_tiered_engine.h`). Nothing is written to disk and no external compiler or linker is involved; symbol resolution and relocation happen in memory.

---

## 1. Architecture

```
JavaScript source (a host call, eval, new Function)
   │
   ▼
Lexing, parsing, module graph (src/lex, src/parse, src/modules)
   │
   ▼
AST transformation & global export (src/eval/eval.cpp)
   │
   ▼
Type & shape inference (src/types)  →  lowering to bronze IL (src/lower, src/il)
   │
   ▼
BrassTieredEngine::compile (src/codegen-brass/brass_tiered_engine.cpp)
   ├─ IL → brass MIR (il2mir), one MIR module per program
   ├─ the program's data image (BrassBackend::buildDataImage): the tables,
   │  descriptors and data cells an object file would carry, loaded into
   │  memory and registered with the program's pipeline
   └─ the program's own MultiTierPipeline (a FunctionDispatchTable per program)
   │
   ▼
Execution on the calling thread (BrassTieredProgram::run)
   ├─ starts in brass's fast interpreter
   ├─ hot functions are baseline-compiled on the mutator, then optimized by
   │  the program's background compiler and installed while it runs
   ├─ thrown exceptions surface through rtTls()->exception_cell
   ├─ pending Promise microtasks drain (embed::drainMicrotasks())
   └─ the program is retained for the process lifetime (retainProgram)
```

### Tiers

`EvalOptions::tier` picks how a program runs; unset means the process default (`setDefaultTier`, initially `Auto`), which also governs every `eval()` and `new Function()` the program makes. `bronze run --tier=` sets that default.

| Tier | What runs |
|------|-----------|
| `Auto` (default) | The tiered pipeline: every function starts interpreted; one that crosses the invocation threshold is baseline-compiled, and one that stays hot is queued for the optimizing compiler, which builds it on the program's background worker and installs it over the baseline code. The worker starts on the first queued compile. |
| `Tier0_Interpreter` | The pipeline, never compiling: every function interpreted, including the ones the runtime calls natively (through the function's stub and a native-to-interpreter bridge). |
| `Tier1_Baseline` | The pipeline, every function baseline-compiled before the program runs; none is optimized further. |
| `Tier2_Optimized` | The whole program optimized ahead of running (`BrassBackend::compileToJit`: ModuleCompiler → object → `JitExecutionEngine`), outside the pipeline. |

A function's pointer — what a closure holds, what descriptors and source tables point at — is its lazy stub in every pipeline tier (`MultiTierPipeline::function_address`), so pointers compare equal whichever tier made them, and the stub reaches whatever code the function currently has.

### Stack traces and function source

`Error.prototype.stack` walks the native stack and maps return addresses through the code ranges the runtime has registered (`runtime/stack_trace.cpp`). Pipeline code is registered as it is installed: the pipeline tells the program's image of every baseline and optimized function it installs (`MultiTierPipeline::set_code_install_observer`), with the compiler's line table, and the image registers a code range with a pc table built from it (`brass_tiered_image.cpp`). Interpreted frames have no native code; the process's interpreted-frame walker (`runtime/interpreted_frames.h`, installed through `embed::setInterpretedFrameWalker`) reports them with the stack address of their frame record, and the walk merges them among the native frames by that address. Both kinds are attributed to the function descriptors in the program's data image, so a trace reads the same at every tier.

`Function.prototype.toString` reads the source slices the program's entry registers from its data image (`bronze_register_fn_sources`), keyed by the functions' stub addresses.

---

## 2. Dynamic Built-in Hooks (`eval` and `new Function`)

`eval` and `new Function` route through runtime host seams:
- `rtSetDynamicEvalHost` / `rtSetDefaultDynamicEvalHost`
- `rtSetDynamicFunctionHost` / `rtSetDefaultDynamicFunctionHost`

`installDefaultDynamicHooks()` installs bronze's in-memory evaluator into them; the CLI and the eval tests call it, and a host that wants dynamic code asks for it the same way (a `bronze build` executable does not, so its `Function("...")` stays the refusal the AOT contract promises). Any script can then call:
```javascript
const result = eval("40 + 2"); // returns 42

const add = new Function("a", "b", "return a + b;");
console.log(add(10, 25)); // prints 35

const gen = new (function*(){}).constructor("yield 1; yield 2; return 3;");
const asyncFn = new (async function(){}).constructor("x", "return x * 2;");
```
All four kinds of dynamic functions (`Ordinary`, `Generator`, `Async`, `AsyncGenerator`) compile in memory, each as its own retained program.

---

## 3. Public C++ API (`src/eval/eval.h`)

```cpp
#include "eval/eval.h"

// Compile on any thread; run on the thread that owns the realm.
std::unique_ptr<CompiledScript> compileScript(std::string_view source, const EvalOptions& options = {});
std::unique_ptr<CompiledScript> compileFile(const std::string& filePath, const EvalOptions& options = {});
embed::CallResult runCompiledScript(std::unique_ptr<CompiledScript> script, const EvalOptions& options = {});

// Compile and run in one call.
embed::CallResult evalScript(std::string_view source, const EvalOptions& options = {});
embed::CallResult evalFile(const std::string& filePath, const EvalOptions& options = {});
Value evalScriptDirect(std::string_view source, const EvalOptions& options = {});

// new Function(...)
Value evalFunction(runtime::DynamicFunctionKind kind, std::span<const Value> args);
Value evalFunction(std::span<const std::string> params, std::string_view body,
                   runtime::DynamicFunctionKind kind = runtime::DynamicFunctionKind::Ordinary);

// The tier a program runs at when its options name none.
void setDefaultTier(ExecutionTier tier);
ExecutionTier defaultTier();

void installDefaultDynamicHooks();
```

`EvalOptions` (eval.h documents each field) carries the file name, host globals, module roots, the module registry switches, `moduleHandleOut` for a host that unloads a program's registrations later (`embed::unloadModule`), `optimize`, and `tier`.

A program is retained for the process lifetime once it runs (`retainProgram`): closures hold its function pointers. At exit the retained programs' background compiles are stopped before the process tears down.

---

## 4. CLI Commands

### `bronze run <file>`
Runs a file and its module graph in process. `--tier=0|1|2|auto` (or `--interp` for 0) pins the tier; the default is `auto`.
```bash
bronze run script.js
bronze run --tier=0 script.js
```

### `bronze eval <code>` (or `bronze -e <code>`)
Evaluates an expression or statement sequence:
```bash
bronze eval "Math.max(10, 25) * 2"
bronze -e "const f = new Function('x', 'return x * 10'); f(4.2)"
```

---

## 5. Integration Guide for Hosts

1. Link `bronze::eval` (or `bronze::cli`, which carries it).
2. Compile with `compileScript` / `compileFile` (any thread) and run with `runCompiledScript` on the realm's thread, or call `evalScript` / `evalFile`.
3. Register host globals with `bronze::embed::registerHostGlobal`; the evaluator discovers them from `runtime::rtHostGlobalEntries()`.
