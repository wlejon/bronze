# Self-hosted builtins

Builtins written in JavaScript and compiled by bronze itself, in place of C++
in `src/runtime`. `builtins.js` is the library; today it holds
`Array.prototype.{forEach, map, filter, some, every, find, findIndex,
reduce}`. There is one implementation of each: the C++ bodies they replace are
deleted, and nothing selects between the two. A builtin moves here only when
its JavaScript is at least as fast as the C++ it replaces: `indexOf` (a dense
scan with `===` per element) and `Object.assign` (a keyed store per key) were
moved and measured slower, 33% and 27%, and are C++ again.

## How it is built

`bronze_stage0` (this directory) is bronze's front end and brass backend
linked against a runtime without the library, the one image that cannot have
it. The build runs it over `builtins.js` (`selfhost_build.cpp`):

    loadProgram -> inferModule -> lowerAheadOfTime(selfHost) -> brass --emit-obj

with the entry named `bronze_selfhost_main`, no function source kept, and
direct JS-to-JS calls, as an image build has. The object is the static
library `bronze_selfhost`, which `bronze_runtime`'s link interface adds to
every executable and shared library that links the runtime (never to a static
library in between, never to `bronze_stage0`), and a source of the shared
runtime (`cmake/bronze_shared_runtime.cmake`). So `bronze run` and every JIT
tier, `eval::buildModule` (an installed app's scripts) and `bronze build`
(whose executables load the shared runtime) all call the same machine code.

## The dialect

`src/lower/lower_selfhost.cpp`. Ordinary strict JavaScript, plus:

- **No global reads.** Checked over the finished IL: any `GlobalGet` or
  `ResolveName` is an error, so a builtin cannot see a program's replacement
  of `Array`, `TypeError`, `Function.prototype.call`, ... `NaN` and `Infinity`
  are globals; write `0 / 0` and `1 / 0`.
- **Intrinsics**, `$Name(...)`, recognised in this mode only (anywhere else
  `$Call` is an ordinary unresolved name, so no program can reach one). Each is
  an IL instruction or a call to one runtime helper by symbol
  (`src/runtime/selfhost.cpp`), never a property read:

  | Intrinsic | Lowers to | Operation |
  |---|---|---|
  | `$Call(f, thisArg, ...args)` | `DynamicCall` | 7.3.14 Call |
  | `$ToObject(v, message)` | `bronze_sh_to_object` | 7.1.18 ToObject; `message` is the TypeError's text for null/undefined ("" = generic) |
  | `$ThrowNotCallable(v)` | `bronze_sh_throw_not_callable` | TypeError "`<v>` is not a function" in V8's wording |
  | `$ThrowTypeError(msg)` / `$ThrowRangeError(msg)` | `bronze_sh_throw_*` | |
  | `$ArraySpeciesCreate(O, len)` | `bronze_sh_array_species_create` | 10.4.2.3 |
  | `$CreateDataPropertyOrThrow(A, k, v)` | `elem.set` define (il.h `kWriteDefine`), inline for an open Array | 7.3.7 |
  | `$LengthOfArrayLike(O)` | `bronze_sh_length_of_array_like` | 7.3.18 (an Array's header, else ToLength(Get)) |
  | `$Builtin(fn, key, display, name, length)` | `bronze_sh_register(key, fn)` | registers a builtin |

  Everything else is plain JavaScript: `typeof f === "function"` is
  IsCallable, `k in O` HasProperty, `O[k]` Get, `+v` ToNumber, a strict
  assignment Set(…, true).

- **`$Builtin` rows** at the top level name the builtins: the registry key the
  runtime asks for (`"Array.prototype.map"`), the frame's name in Error.stack
  (`"Array.map"`, V8's), the function's `name` and `length`. The compile makes
  each such function NATIVE (`toString` is the NativeFunction form; no
  [[Construct]], no `prototype`; realm 0, the thread's intrinsics) with a
  BUILTIN descriptor. A builtin may not capture, read `arguments`, or be a
  generator or async function (a rest parameter is fine: `reduce`'s
  "was an initial value passed" is `initial.length`). Every other function of
  the library is a HIDDEN frame (`BRONZE_FN_DESC_HIDDEN`), which Error.stack
  leaves out as it leaves out a C++ builtin's own calls.

## At run time

`runtime/selfhost.{h,cpp}`. The intrinsic tables ask for a builtin by id
(`NativeMethod::selfHosted`, `rtMethodFunction`); the first ask on a thread
runs `bronze_selfhost_main` once (`rtRunAsLazyInit`, so its roots are
permanent whatever program's read forced it, and `rtRunEntryForEveryRealm`, so
one module instance on the thread serves every realm — the functions belong
to no realm and the library has no top-level state a realm could own). Its
`$Builtin` rows register the function objects (rooted per thread) and their
code addresses (process-wide).

A self-hosted builtin's object is NATIVE but its code is compiled code, which
needs the pinned TLS register. The three runtime paths that call a native's
code as a C function — `bronze_dynamic_call`'s native shortcut,
`Function.prototype.call`, and the method cache's EXOTIC hit — ask
`rtIsSelfHostedCode` and take `rtEnterJs` instead. Generated code calls the
code directly either way.

## Adding a builtin

1. Write it in `builtins.js` against its clause, with a `$Builtin` row.
2. Add its id to `SelfHosted` (`runtime/selfhost.h`) and its key to `kKeys`
   (`runtime/selfhost.cpp`).
3. Point its table row at the id (`{"name", nullptr, arity, length, nullptr,
   SelfHosted::Id}`) and delete the C++ body.
4. A new intrinsic is a row of `kHelpers` (`lower_selfhost.cpp`) and an
   `extern "C"` helper in `runtime/selfhost.cpp`.
