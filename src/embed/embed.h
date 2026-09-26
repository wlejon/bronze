#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "abi/bronze_native_type.h"  // bronze_native_buffer: a `T[]` return's out-descriptor
#include "runtime/host_globals.h"
#include "runtime/interpreted_frames.h"
#include "runtime/native_handle.h"
#include "runtime/value.h"
#include "embed/embed_handle.h"

// The host-facing embedding API: what a C++ application links to run a
// bronze-compiled program inside its own process — register globals, hand the
// program native functions and objects, call back into compiled code, and hold
// heap values across frames.
//
// This is NOT the generated-code ABI. bronze_abi.h stays pure C and lists only
// the symbols GENERATED code links against; nothing compiled code calls is
// declared here, and nothing here may ever move there. This header is C++ for
// a C++ host, and the boundary it sits on is host <-> runtime, not
// codegen <-> runtime.
//
// ---- THE THREADING CONTRACT ------------------------------------------------
//
// The runtime is PER-THREAD, and a thread needs no ceremony to get one. Every
// piece of mutable runtime state — heap, arena, the interning tables, the
// intrinsics, the microtask queue, the Persistent slots and finalizer
// registries below — is thread_local and built lazily on first touch, so a
// worker that simply calls in owns a fresh, fully independent runtime from
// that first call. There is no attach step, no init ordering between threads,
// and no cross-thread locking, because there is nothing shared to lock.
//
// What that buys, and what it imposes:
//
//  * A Value is THREAD-BOUND. It points into the heap of the thread that made
//    it, and so does everything reachable from it — a Persistent included,
//    which is a slot in its creating thread's registry. Handing either to
//    another thread is not a data race to synchronize away, it is nonsense:
//    the other thread's runtime has never heard of the value, and the
//    collector that owns it moves it without telling anyone else. Cross-thread
//    data travel is the HOST's job — serialize on one side, re-create on the
//    other.
//
//  * A compiled MODULE is NOT thread-bound: one image may run on any number
//    of threads at once. Everything it writes — inline caches, global cache,
//    template cells, environment cell, native import table — lives in a run
//    of its data that is per THREAD: the first thread to run the entry uses
//    the image's own bytes, every later thread's entry makes that thread a
//    copy of them as they were before anything wrote to them
//    (bronze_abi.h, bronze_module_instance), and compiled code reaches the
//    calling thread's copy through its TLS block. The shared remainder is
//    immutable once filled (the key remap holds process-wide interned ids).
//    The one rule: a thread runs the module's entry before it calls any of
//    the module's code — which it must anyway, since the entry is what makes
//    the functions a thread can reach. The values each thread's run
//    produces are that thread's, like every other Value.
//
//  * There is NO TEARDOWN. A thread's runtime lives until its thread exits and
//    its memory until process exit — deliberately leaked, because proving no
//    reference escaped would cost more than the memory does. Pool and reuse
//    worker threads rather than churning them.
//
// tests/threaded_modules is this contract in executable form: two compiled
// modules on two threads, concurrently, each against its own runtime — and
// one module image entered on two threads at once.
//
// THE GC CONTRACT, which every function below is written against: the heap is
// a moving generational collector, so any allocation may relocate any heap
// value in sight. A `Value` held in a plain C++ variable is current only until
// the next allocating call; a value that must survive one — or survive between
// frames — lives in a `Persistent`. Functions here that allocate say so, and
// the ones that take a receiver and allocate return its post-call address.
//
// THE TRAP IN THAT RULE, because "the next allocating call" is easy to read as
// "the next statement": ARGUMENTS TO ONE CALL ARE EVALUATED IN UNSPECIFIED
// ORDER. So
//
//     setElement(obj.get(), 3, fromUtf8("x"));   // WRONG
//
// is already broken, in one statement: a compiler is free to read `obj.get()`
// first and then run the allocation that moves `obj`, and the receiver the
// call gets is a pre-collection address. MSVC happens to evaluate right to
// left and clang left to right, so this is a bug that passes every test on one
// platform and faults on another — and only once the heap is full enough that
// the dead address stops landing in mapped memory. Build the allocating
// argument in its OWN statement, into a Persistent, and pass slot reads only:
//
//     Persistent v{fromUtf8("x")};
//     setElement(obj.get(), 3, v.get());        // right
//
// Nothing on this side of the boundary can rescue a caller who gets this
// wrong: by the time a function here roots its receiver, it is rooting bits
// that already name freed memory.
//
// ---- THE TWO BOUNDARIES ----------------------------------------------------
//
// A host and a bronze runtime meet along two seams with very different rules,
// and confusing them is how a process ends up with two heaps.
//
//  1. The GENERATED-CODE ABI (src/abi/bronze_abi.h). Pure C, primitives only,
//     u64 in and u64 out. It is FINGERPRINT-CHECKED: every compiled object
//     carries the hash of the header it was built against, and the runtime
//     refuses to run a module whose stamp is not its own. That check is what
//     makes it safe to load a module built by a different bronze, on a
//     different day, with a different compiler — the check either passes or
//     names both versions and stops.
//
//  2. THIS header, the host↔runtime C++ boundary. It carries C++ types —
//     std::string, std::span, std::function, a class with a destructor — and
//     C++ has no stable ABI. There is NO fingerprint here and there cannot be
//     one, because the failures are not versioned facts about bronze: they are
//     facts about the two compilations. The host and the runtime must be built
//     BY THE SAME COMPILER, at the same major version, against the SAME C
//     RUNTIME (on MSVC: the same /MD or /MT, the same debug/release CRT). A
//     std::string crossing between two CRTs is freed by an allocator that
//     never allocated it, and a Persistent destroyed against a different
//     runtime's slot registry frees a root that is not there.
//
// A host that cannot guarantee (2) has one supported option and it is a good
// one: use only (1) — dlopen the module, resolve the four symbols
// bronze_abi.h documents, and drive it through the C ABI.
//
// BRONZE_EMBED_API is what makes (2) reachable across a shared runtime at all,
// and this header is THE ONE PLACE in bronze that may carry such an
// annotation. The C ABI's export list is generated from the registry
// (cmake/bronze_abi_exports.cmake) precisely so no runtime source ever grows
// one; the exception is here because this boundary has no registry to generate
// from — the declarations below ARE the list.
//
//   (neither defined)          the static path: expands to nothing, unchanged.
//   BRONZE_EMBED_SHARED_BUILD  building the shared runtime: export.
//   BRONZE_EMBED_SHARED        a host linking the shared runtime: import.
#if defined(BRONZE_EMBED_SHARED_BUILD)
#  if defined(_WIN32)
#    define BRONZE_EMBED_API __declspec(dllexport)
#  else
#    define BRONZE_EMBED_API __attribute__((visibility("default")))
#  endif
#elif defined(BRONZE_EMBED_SHARED) && defined(_WIN32)
#  define BRONZE_EMBED_API __declspec(dllimport)
#else
#  define BRONZE_EMBED_API
#endif

namespace bronze {
class Realm;
// runtime/typed_array.h owns the definition; this is the opaque redeclaration,
// so typedArrayInfo below can name the kind without pulling the heap headers
// into every host translation unit.
enum class ElementKind : uint32_t;
}  // namespace bronze

namespace bronze::embed {

using Value = bronze::Value;
using Realm = bronze::Realm;

// ---- program entry ---------------------------------------------------------

// What src/rt/rt.cpp's `main` does, minus `main`: stdio setup, then the
// compiled program. Split so a host that already configured its stdio (an
// engine with its own console handling) is not forced back through it.

// Binary stdout and crash dialogs routed to stderr — the setup a standalone
// bronze executable performs before anything can fail. Idempotent.
BRONZE_EMBED_API void setupIo();

// Run the compiled program: a GC root frame for the call, then
// `bronze_main()`. The host registers its globals and functions BEFORE this —
// the program's top level runs here, and a read of a host global it performs
// must find the value already registered.
BRONZE_EMBED_API void runMain();

// setupIo() then runMain(): the whole of the standalone `main`, for a host
// with no stdio opinions.
//
// runMain and runProgram are NOT in the shared runtime. They name `bronze_main`
// at link time, and a shared runtime's modules arrive at RUN time under
// whatever names --entry-symbol gave them, so the symbol could never be
// resolved in that library. runEntry below is the same sequence with the entry
// passed in — which is what a host that loaded its module has.
BRONZE_EMBED_API void runProgram();

// ---- loadable modules (embed_module.cpp) -----------------------------------
//
// runMain() names `bronze_main` at LINK time, which is precisely what a host
// that loads its modules cannot do: it has a function POINTER, resolved from a
// module it opened, and a process may hold several. The two below are the same
// sequence with the entry as an argument.

// This runtime's ABI fingerprint — the hash of the bronze_abi.h it was built
// against. A loader compares it against the module's own `<entry>_abi_fingerprint`
// (bronze_abi.h documents that symbol) BEFORE calling the entry, and refuses
// the module naming both values when they differ. The comparison must read
// this at RUN time rather than compile the constant into the host: a host that
// baked in its own build's value would be checking the module against itself
// and would sail straight into the drift the stamp exists to catch.
BRONZE_EMBED_API uint32_t abiFingerprint();

// A compiled module's entry: `void(void)`, the shape `--entry-symbol` names.
using ModuleEntry = void (*)();

// Run one module's top level: a GC root frame for the call, then `entry()`,
// then the microtask checkpoint — exactly what runMain() does for the linked
// `bronze_main`. The host registers its globals BEFORE this, and checks the
// module's fingerprint before this, for the reasons both are stated above.
//
// Called once per module, in the host's chosen order. Everything a module
// needs at run time it registers itself at entry (the key remap, its own root
// spans), so ordering is the host's to decide and nothing here is per-process
// setup in disguise.
BRONZE_EMBED_API void runEntry(ModuleEntry entry);

// Install (or, with nullptr, remove) the calling thread's C++ -> JS entry hook
// (runtime/fn.h rtSetEnterJsHook): the tiered engine uses it to route calls
// into functions it is still interpreting. It lives on this surface because the
// hook is runtime state, and the compiler backend that installs it must reach
// the ONE runtime in the process — the shared image's, in a host that loads
// modules — rather than link a static copy of its own.
// `code` is bronze_abi.h's bronze_fn_code, spelled out so this header does not
// pull in the whole ABI registry.
using EnterJsHook = bool (*)(uint64_t (*code)(uint64_t, uint64_t, uint32_t, const uint64_t*),
                             uint64_t env_bits, uint64_t this_bits, uint32_t argc,
                             const uint64_t* argv, uint64_t* out_result);
BRONZE_EMBED_API void setEnterJsHook(EnterJsHook hook);

// Install (or, with nullptr, remove) the process's interpreted-frame walker
// (runtime/interpreted_frames.h): how a stack trace sees the JS frames the
// tiered engine is interpreting, which have no native code to find. On this
// surface for the reason setEnterJsHook is: it must reach the ONE runtime.
BRONZE_EMBED_API void setInterpretedFrameWalker(runtime::InterpretedFrameWalker walker);

// Install (or, with nullptr, remove) the calling thread's promise-rejection
// hook: HostPromiseRejectionTracker as an embedder with an event loop wants it
// (HTML "notify about rejected promises").
//
//  * Unhandled — at the end of every microtask drain, once per promise that
//    was rejected with no handler and still has none, in rejection order.
//    With a hook installed this REPLACES the stderr "Unhandled promise
//    rejection" line; with none, bronze prints it as before.
//  * Handled — a promise the hook was told about as Unhandled has just been
//    given its first handler (a `then`, `catch` or `await`). Called
//    synchronously from inside that subscription, so it runs in the middle of
//    user code: record it and act later (HTML queues a task), never call back
//    into JS from here.
//
// `promise` and `reason` are raw Values, current only until the next
// allocation: a hook that keeps them puts them in Persistents first.
enum class PromiseRejectionOperation : uint32_t { Unhandled = 0, Handled = 1 };
using PromiseRejectionHook = void (*)(PromiseRejectionOperation op, Value promise, Value reason);
BRONZE_EMBED_API void setPromiseRejectionHook(PromiseRejectionHook hook);

// ---- module unload / hot swap ----------------------------------------------
//
// A module's entry registers the module's own root spans (its global cache,
// its module-environment cell, its function-singleton slots) with the
// collector. The pair below is how a host takes them back out, which is the
// runtime's whole share of hot swap:
//
//     ModuleHandle h = beginModuleLoad();
//     runEntry(entry);            // spans registered here carry the handle
//     endModuleLoad(h);           // spans registered from here on are nobody's
//     ...
//     unloadModule(h);            // the spans stop being roots
//
// beginModuleLoad is called immediately BEFORE the entry it brackets and
// nothing else runs a module entry in between: the bracket is what ties the
// registrations to the handle. A host that never brackets (or a linked
// program) gets permanent registrations, exactly as before.
//
// endModuleLoad CLOSES the bracket. Without it the handle stays current, and
// every span registered on the thread afterwards — by a program the host
// compiles and runs later, by a module whose first read of a provided global
// happens in a callback — would carry this module's handle and die with it.
// That is wrong in both directions: a later program is not this module, and
// unloading this module must not unroot it. After endModuleLoad such spans
// are epoch 0, permanent, which is the safe error: a stale cache cell keeps a
// global alive, a dropped one keeps a running program's cells untraced.
// Ending a handle that is not current is a no-op, so a host may end
// unconditionally.
//
// WHAT UNLOAD MEANS — AND WHAT IT DOES NOT:
//
//  * The module's .data stops being a ROOT SOURCE. Heap values the module
//    created live exactly as long as something else references them: a host
//    Persistent, another module, a property on globalThis. Nothing is freed
//    eagerly; the next collection simply stops tracing through the module's
//    own tables. In particular, functions the module assigned onto globalThis
//    are still referenced BY globalThis — the new version overwriting those
//    names is what actually lets the old module's environment die.
//
//  * THE HOST MUST NEVER FreeLibrary/dlclose THE IMAGE. Function objects and
//    closures hold raw code pointers into the module's .text, and proving no
//    such reference survives would be a whole-heap analysis. The image is
//    deliberately leaked — a few MB per swap is the price of soundness, and a
//    dev-loop host swaps a bounded number of times per run. Load the NEW
//    version from a distinct path (or copy) so the loader maps a fresh image
//    rather than handing back the old one.
//
//  * Interned property keys and hidden-class shapes the module minted are
//    immortal by design and are not reclaimed.
//
// Calling unloadModule while a call into the module's code is on the stack is
// a host error, exactly as freeing any object mid-use is.
using ModuleHandle = uint64_t;
BRONZE_EMBED_API ModuleHandle beginModuleLoad();
BRONZE_EMBED_API void endModuleLoad(ModuleHandle module);
BRONZE_EMBED_API void unloadModule(ModuleHandle module);

// Collect now. A host that has just released a large graph — a level torn
// down, a frame's scratch objects dropped — knows something the heap's own
// growth heuristic does not, and this is how it says so. Everything the host
// holds in a Persistent survives and is updated in place; every raw pointer
// the host obtained from typedArrayInfo or arrayBufferInfo is DEAD after this,
// by the pointer contract those functions carry.
BRONZE_EMBED_API void collectGarbage();

// The collector's relocation counter: advanced by every collection that moves
// an object, so it changes when addresses change and not merely when
// collections complete. This
// is the primitive an identity map is built on — a host table keyed on raw
// value bits (toBits) records this number when it builds its index and
// rebuilds when the number has moved on, and such a cache cannot go stale,
// because bits only change when this does. The bronze Map keyed on objects
// uses the same discipline internally (heap.h relocation_epoch).
BRONZE_EMBED_API uint64_t relocationEpoch();

// Creates the calling thread's heap if it has none. The heap is brass's
// collector (brass::gc::Heap), and creating it binds it as the thread's
// current brass heap, so every brass interpreter built on the thread from
// then on registers its frames with it. Anything that touches the runtime
// does this implicitly; the compiler backend calls it before it builds an
// interpreter. On this surface for the reason setEnterJsHook is: it must reach
// the ONE runtime.
BRONZE_EMBED_API void bindThreadHeap();

// ---- callee naming bridge & profile report (embed_module.cpp) --------------
using ProfileCalleeNamer = bool (*)(uint64_t calleeBits, void* code, char* out, size_t outSize);
BRONZE_EMBED_API void setProfileCalleeNamer(ProfileCalleeNamer namer);
BRONZE_EMBED_API void dumpProfileReport();

// ---- runtime telemetry (embed_module.cpp) ----------------------------------
struct RuntimeTelemetry {
    size_t heapUsedBytes{0};
    size_t heapCommittedBytes{0};
    size_t heapReservedBytes{0};
    uint64_t gcCollections{0};
    uint64_t gcPauseNs{0};
    uint64_t shapeTransitions{0};
};
BRONZE_EMBED_API RuntimeTelemetry getRuntimeTelemetry();

// ---- the microtask checkpoint (embed_run.cpp) ------------------------------
//
// Promise reactions and async resumptions run as JOBS, and a job runs only
// when something drains the queue. `runMain` drains once after the program's
// top level, which is all a batch program needs; a host with a FRAME LOOP owns
// the rest. The pair below is what it pumps with — typically
// `drainMicrotasks()` once per frame, after the frame's calls into compiled
// code and before presenting.
//
// Draining RUNS USER CODE and ALLOCATES, so every Value the host holds across
// one must live in a Persistent. It is safe to call with an empty queue, and
// safe to call from a stack with no bronze frame on it.
BRONZE_EMBED_API void drainMicrotasks();

// Is there anything queued? For a host that wants to know whether a frame's
// work actually finished — and for a shutdown path that drains to quiescence
// before tearing the runtime down. Never necessary before drainMicrotasks(),
// which is a no-op on an empty queue.
BRONZE_EMBED_API bool microtasksPending();

// ---- host globals ----------------------------------------------------------

// Provide `name` as a global of the compiled program. The compile side is
// `--host-globals`: a name must be in the manifest the program was compiled
// with for its reads to reach the registry at all. Registering again replaces
// the value. The value is rooted for the life of the process.
BRONZE_EMBED_API void registerGlobal(std::string_view name, Value value);

// Is `name` in the host-global registry? The load-time half of a manifest
// check: a module's `<entry>_host_globals` manifest names every global the
// program was compiled to expect from its host, and a manifest name nobody
// registered is a fatal at first READ, mid-run. A loader that walks the
// manifest and probes here can refuse the module at load time with a message
// naming the gap instead.
BRONZE_EMBED_API bool hasHostGlobal(std::string_view name);

// Every name in the host-global registry, in registration order, with a
// replaced name keeping its first position. This is the compile side of
// `--host-globals` read back off the run-time side: a host that has finished
// registering can hand the result to `EvalOptions::hostGlobals`, or print it
// as a manifest for an ahead-of-time `bronze build`, and the two lists cannot
// drift because there is only one. Per-thread like the registry itself (the
// threading contract above): a worker asking on its own thread sees what that
// thread registered. Does not allocate on the JS heap.
BRONZE_EMBED_API std::vector<std::string> hostGlobalNames();

// The value a free read of `name` in compiled code would see: the builtin
// ladder first (Math, the constructors — a host cannot shadow those, exactly
// as the compiled path cannot), then the host-global registry, then own
// properties of globalThis (a program that assigned `globalThis.foo = ...`
// created the global its next free `foo` reads). `found` is false where a
// compiled read would fatal — the probe-and-degrade answer a host binding
// wants. MAY ALLOCATE (the builtin namespaces build lazily), so the usual
// Persistent rules apply to the result.
struct GlobalValue {
    Value value;  // undefined when !found
    bool found{false};
};
BRONZE_EMBED_API GlobalValue globalValue(std::string_view name);

// ---- host natives (embed_native.cpp) ---------------------------------------
//
// A native is a C function the host binds under a JS path and the compiler
// lowers a call to DIRECTLY: `bro.mesh.box(w, h)` becomes a machine call with
// two doubles in registers, no dynamic dispatch, no boxing. The host globals
// above are the dynamic half of the same boundary — a value the program
// reads and calls through the ordinary runtime paths — and the two are
// disjoint by name: a bare native path and a host global cannot share a
// name (whichever registration comes second is refused, loudly). A DOTTED
// native path under a host global's root is the normal arrangement, though:
// bro registers `bro` as an object the program can walk, and the natives are
// the paths under it the compiler short-circuits. Every OTHER access under
// that root — `let f = bro.mesh.box`, `x instanceof bro.ai.AIAgent` — goes
// through the host global as before, so a host that wants those to work
// provides them there too.
//
// Bound at LOAD time, by name, never at link time: a compiled module carries
// an import table (`<entry>_native_imports`, bronze_abi.h's loadable-module
// section) of every native it calls, the runtime fills it from this
// registry when the module is bound, and a native the module was compiled
// against that nobody registered is a refusal naming it — before the first
// call, not a crash inside one.
//
// Two ways the compiler learns the registry:
//   JIT (evalScript)      reads this thread's registry directly; register
//                         before evaluating, and the program is bound before
//                         it runs.
//   AOT (`bronze build`)  reads a manifest the registry prints:
//                         writeNativeManifest, then
//                         `bronze build --native-manifest <file>`. The
//                         module's table is bound when it is loaded
//                         (bindNativeImports), or by its entry's own first
//                         call if the loader did not.
//
// Per-thread, like the host globals, and so is a module's import table: the
// image's own table is its HOME thread's (the first to run the entry), and
// every other thread's entry binds its own copy from that thread's registry.
// bindNativeImports on the image's table therefore checks and binds only the
// home thread's; a worker's entry binds, or refuses fatally, for itself.
//
// The type vocabulary is abi/bronze_native_type.h, and it is closed: a type
// spelling not in it and not a registered class is a refused registration,
// never `dynamic`.
//
//   "void"        return only
//   "f64" "i32" "bool" "str" "dynamic"
//                 double, int32_t (ToInt32), bool, const char* (UTF-8, valid
//                 for the call only), raw Value bits. A missing scalar
//                 argument arrives as 0 / false / "".
//   "f32[]" "f64[]" "i32[]" "u8[]" "u16[]" "u32[]" "i8[]" "i16[]"
//                 a typed array of exactly that element kind, passed as TWO
//                 C parameters (T* data, uint32_t length). Wrong kind,
//                 detached buffer or missing argument: TypeError. The
//                 pointer is valid for the call only; the native must not
//                 allocate through this API while it holds it. As a RETURN
//                 type the C function returns void and takes one extra
//                 trailing `bronze_native_buffer* out`
//                 (abi/bronze_native_type.h) it fills: `release` null means
//                 the runtime COPIES `length` elements into a fresh JS-owned
//                 array, non-null means the array is a zero-copy view over
//                 `data` and `release(ctx)` runs when the buffer is collected
//                 (Deferred; at drainFinalizers). The runtime decides
//                 nothing else — no count-then-fill, no second call.
//   "<class>"     a handle made by that class's constructor, passed as the
//                 void* the constructor returned. Any other value (another
//                 class, a plain object, undefined) is a TypeError naming
//                 the class. As a return type the native returns void* and
//                 the runtime wraps it into a handle of the class (null →
//                 null), owing the class's destructor.
enum class NativeKind : uint8_t {
    Function,     // `path(args)`; params are the C parameters in order
    Method,       // `obj.member(args)`; the C function takes (void* self, args...)
    Constructor,  // `new path(args)`; returns void*; registers the class
    Getter,       // `obj.member` / `ns.prop`; C takes (void* self) / () and returns the type
    Setter,       // `obj.member = v` / `ns.prop = v`; C takes (void* self, v) / (v), returns void
};

struct NativeSignature {
    std::string returnType;               // a vocabulary spelling or a class path;
                                          // a constructor's is its own path (or empty)
    std::vector<std::string> paramTypes;  // EXCLUDING the receiver of a member kind
    NativeKind kind = NativeKind::Function;
    std::string className;                // member kinds: the class path; a
                                          // getter/setter with none is a namespace property
    std::string returnClass;              // returnType "dynamic" only: the class the value
                                          // is a handle of (the compiler tags it; the
                                          // runtime still checks at every use)
    runtime::HandleDestructor destructor = nullptr;  // Constructor only; null = the class owns nothing
    runtime::Finalize finalize = runtime::Finalize::InSweep;  // Constructor only
};

// Bind `fn` under `jsPath` (`bro.mesh.box`, `bro.ai.AIAgent.move`,
// `bro.ai.AIAgent` for a constructor, `bro.time.scale` for a namespace
// property). `fn` is called with the C signature the vocabulary above gives
// for `sig`. Returns false with `*error` set (or a fatal when `error` is
// null) for every refusal: a path that is not a JS path, an unknown type
// spelling, a class name nothing registered, a member of no class, a
// duplicate, a bare name that is already a host global. Register a class's
// constructor before anything that names the class. Does not allocate on
// the JS heap except for a constructor, which mints the class's prototype.
BRONZE_EMBED_API bool registerNative(std::string_view jsPath, void* fn, const NativeSignature& sig,
                                     std::string* error = nullptr);

// Remove one registration. A module already bound keeps calling what it
// bound; a later bind refuses the name. Unregistering a constructor unlinks
// the class: instances already made keep their tag and still answer to
// methods bound before, and no later registration can reuse that tag.
BRONZE_EMBED_API bool unregisterNative(std::string_view jsPath, NativeKind kind);

struct NativeEntry {
    std::string path;
    NativeSignature signature;
    void* fn{nullptr};
    std::string signatureText;  // the canonical "ret(params)" the bind step compares
};
// Every registration on this thread, in registration order.
BRONZE_EMBED_API std::vector<NativeEntry> hostNatives();
// The paths only, one per registration (a path registered under two kinds —
// a getter and a setter — appears twice).
BRONZE_EMBED_API std::vector<std::string> hostNativeNames();

// The prototype every instance of a registered class is born on: a plain
// object the registry roots, minted when the constructor was registered.
// This is where a host puts the JS-visible methods of the class (the
// compiled direct calls never consult it, but a dynamic `obj.move()` on a
// handle that reached the program through an untyped path does), and what
// it sets as `.prototype` of a constructor function it exposes, which is
// what makes `x instanceof bro.ai.AIAgent` answer true. `found` false for a
// name that is not a registered class.
BRONZE_EMBED_API GlobalValue nativeClassPrototype(std::string_view className);

// Wrap a host pointer as a tagged instance of a registered native class.
// The handle is born on the class's prototype and carries the class's tag,
// so native methods accept it as receiver. Returns null if data is null or
// className is not a registered native class.
BRONZE_EMBED_API Value wrapNative(void* data, std::string_view className);

// The registry as JSON — the manifest an ahead-of-time `bronze build
// --native-manifest <file>` compiles against. Deterministic for one
// registration order. writeNativeManifest is the same text to a file
// (false with `*error` set, or a fatal when `error` is null, if the file
// cannot be written).
BRONZE_EMBED_API std::string nativeManifestJson();
BRONZE_EMBED_API bool writeNativeManifest(const std::string& path, std::string* error = nullptr);

// Fill a loaded module's import table (the `<entry>_native_imports` symbol,
// resolved by the loader beside the entry) from this thread's registry.
// True when every slot was filled; otherwise `*missing` receives one line
// per slot it could not fill — "<kind> <path> (not registered)", or the
// signature mismatch — and the loader refuses the module by name. The
// module's entry calls the same bind itself and is FATAL on a gap, so this
// is the soft form for a loader that wants the refusal before entry.
// Idempotent. A JIT program (evalScript) is bound by the evaluator.
BRONZE_EMBED_API bool bindNativeImports(const void* importTable,
                                        std::vector<std::string>* missing = nullptr);

// ---- CreateDynamicFunction, answered by the host ---------------------------
//
// `new Function(src)`, and the generator/async/async-generator forms, refuse by
// name in a standalone bronze program: compiling a string at run time is the
// one thing an ahead-of-time compiler cannot do. A host that ALREADY runs an
// interpreter is a different situation — bro embeds QuickJS beside the compiled
// code — and this is how it says so. Install a hook and the four constructors
// ask it; install nothing and the refusal is unchanged, which is the default
// and the right answer for every host that has no interpreter to offer.
//
// What bronze does with the result: nothing but return it. The hook is handed
// 27.3.1.1's raw arguments (parameter lists then body, unconverted — ToString
// is the host's, so it can refuse a bad argument in its own words) and gives
// back a callable. bronze never parses the source and never inspects what came
// back, so the function may be a bronze function object the host built, or a
// wrapper around something living entirely in the host's own engine.
//
// `args` points at ROOTED slots the collector updates in place, so the span
// stays current across anything the hook allocates. Throwing works the way it
// does from a NativeFn: call a throw helper and return its result.
using DynamicFunctionKind = runtime::DynamicFunctionKind;
using DynamicFunctionHook = runtime::DynamicFunctionHost;
BRONZE_EMBED_API void setDynamicFunctionHook(DynamicFunctionHook hook);

// ---- eval, answered by the host ---------------------------------------------
//
// `Function`'s sibling seam. `eval` is a real provided global — `typeof eval`
// is "function", `const e = eval` is a value, and both the direct and the
// indirect spelling call the same object — whose body hands SOURCE TEXT to
// this hook, or throws a catchable TypeError when none is installed. The
// runtime performs 19.2.1 step 2 itself, so the hook only ever sees a string
// value; what comes back is returned to the program uninspected.
//
// The one thing the hook is NOT given is the caller's scope: an AOT frame has
// no environment record to reify, so both spellings carry the indirect
// (global-environment) semantics, and the lowering warns at syntactically
// direct call sites. `source` is a ROOTED slot, current across anything the
// hook allocates; throwing works the way it does from a NativeFn.
using DynamicEvalHook = runtime::DynamicEvalHost;
BRONZE_EMBED_API void setDynamicEvalHook(DynamicEvalHook hook);

// ---- import(), answered by the host -----------------------------------------
//
// The third seam. bronze follows every `import()` it can read at compile time
// — a string literal, or a template literal with a static head and a tail
// ending in a module extension, which it globs — and those calls resolve to
// compiled namespaces without ever reaching the runtime. A specifier it could
// not read (a variable; `../../${path}`) is warned about at compile time and
// arrives here at run time, where a standalone program rejects it with a
// TypeError. A host that can load modules itself installs a hook and is
// handed the specifier value (a ROOTED slot, unconverted) and the importing
// module's URL — its `import.meta.url` text, the base a relative specifier is
// resolved against. The hook's answer is returned uninspected; the program is
// awaiting it, so return a promise. Throwing works the way it does from a
// NativeFn.
using DynamicImportHook = runtime::DynamicImportHost;
BRONZE_EMBED_API void setDynamicImportHook(DynamicImportHook hook);

// ---- native functions (embed_function.cpp) ---------------------------------

// A host callable, invoked when compiled JS calls the wrapping function
// object. `args` points at GC-ROOTED slots the collector updates in place, so
// the span stays current across anything the callback does; `thisValue` is a
// plain copy, current at entry — re-root it before allocating.
//
// To throw into JS, call one of the throw helpers below. Their exception is
// the only one that may leave a callback: generated code unwinds it to the
// program's `catch`. No other C++ exception may escape a callback: JS
// `catch` cannot see it, and nothing between the callback and the host's
// own entry point is prepared for it.
using NativeFn = std::function<Value(Value thisValue, std::span<const Value> args)>;

// Wrap `fn` into a bronze function object callable from compiled JS. The
// closure state rides in the function's environment slot as a native handle
// (see below), so it survives collections and is destroyed when the function
// object dies. `arity` is the count short calls are padded to (undefined
// fill), not the JS `length`. ALLOCATES.
BRONZE_EMBED_API Value makeFunction(NativeFn fn, uint32_t arity = 0);

// The same, NAMED. `f.name` on an unnamed host function is a hard refusal
// (rt_state.cpp says why an absent name is not an empty one) and `setProperty`
// refuses the key outright, so this is the only way in — and it is needed by a
// host that stands in for a language feature which names what it builds:
// CreateDynamicFunction calls every function it makes `anonymous`, and library
// code reads that. `arity` becomes `length` too, since 10.2.9 and 10.2.10 fill
// the pair together. ALLOCATES.
BRONZE_EMBED_API Value makeFunction(NativeFn fn, uint32_t arity, std::string_view name);

// Raise into the compiled program, exactly as the builtins in
// src/runtime/builtin_*.cpp do: each throws the runtime's one exception type
// (brass::runtime::BrassException carrying the thrown value) and never
// returns. The Value return type lets a callback spell `return throwX(...)`.
// The raise unwinds the callback's own C++ frames, so hold resources in RAII.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4646)  // noreturn with a non-void type, on purpose
#endif
[[noreturn]] BRONZE_EMBED_API Value throwValue(Value thrown);
[[noreturn]] BRONZE_EMBED_API Value throwError(const std::string& message);
[[noreturn]] BRONZE_EMBED_API Value throwTypeError(const std::string& message);
[[noreturn]] BRONZE_EMBED_API Value throwRangeError(const std::string& message);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// ---- object building (embed_object.cpp) ------------------------------------

BRONZE_EMBED_API Value createObject();

// Allocate a fresh JavaScript Array (`[]`), inheriting Array.prototype. ALLOCATES.
BRONZE_EMBED_API Value makeArray(uint32_t length = 0);

// Define an own data property (enumerable, like an assignment). The receiver
// may be a plain object or a FUNCTION: `URL.createObjectURL` is a property on
// a function value, and a host shipping such a namespace has to be able to
// spell it. On a function the definition lands where a class `static` member's
// would; `name`, `length` and `prototype` are slot-backed or non-writable
// (10.2.9, 10.2.10, 10.2.4) and are refused by name, loudly — hard errors
// over a property the reads would then lie about. ALLOCATES and may move
// `obj`: the return value is the receiver's post-call address, and any OTHER
// Value the host holds must be re-read from a Persistent.
BRONZE_EMBED_API Value setProperty(Value obj, std::string_view key, Value v);

// Delete an own property from `obj`. Returns true if deletion succeeded or property was absent.
BRONZE_EMBED_API bool deleteProperty(Value obj, std::string_view key);

// `obj[3] = v` spelled from the host, for any receiver the program could
// write through: an Array grows and renumbers, a typed array converts and
// stores (or drops the write, out of range, exactly as JS does), a plain
// object takes the canonical numeric string, which is what enumeration order
// keys off.
//
// The two halves have different SEMANTICS and that is deliberate. A plain
// object is DEFINED onto, like setProperty and for the same reason: a host
// building an object must not run an inherited setter. Everything else goes
// through the generic element-set path a compiled `arr[i] = v` takes, because
// an array's length bookkeeping and a typed array's narrowing conversion are
// that path's, and reimplementing either here would be a second, drifting
// answer to a question the runtime already answers.
//
// A throw from that path (a frozen array, a detached buffer) is handled the
// way getProperty handles a throwing getter: the throw is caught at the host
// boundary and the write is dropped. ALLOCATES; same return contract
// as setProperty.
BRONZE_EMBED_API Value setElement(Value obj, uint32_t index, Value v);

// `get key()` / `set key(v)` as one accessor property; pass undefined for a
// half the host does not provide. ALLOCATES; same return contract.
BRONZE_EMBED_API Value defineAccessor(Value obj, std::string_view key, Value getter, Value setter,
                     bool enumerable = true);

// Object.freeze, for a host handing the program a namespace it must not be
// able to redecorate. Returns the object (freezing does not move it, but the
// uniform shape keeps call sites chainable).
BRONZE_EMBED_API Value freeze(Value obj);

// ---- property reads (embed.cpp) --------------------------------------------

// `obj[key]` through the same generic element-get path a computed read in
// compiled code takes — prototype chain, accessors and all. A getter that
// throws is handled the way `call` handles a throw: it is caught at the host
// boundary and the read answers undefined (a host
// accessor has no JS frame to propagate into). MAY ALLOCATE and MAY RUN USER
// CODE (a getter), so every other Value the host holds must be re-read from a
// Persistent afterwards.
BRONZE_EMBED_API Value getProperty(Value obj, std::string_view key);

// `obj[3]` spelled from the host — the numeric key takes the same path a
// program's `arr[i]` does, so it answers for real arrays, typed arrays and
// plain objects alike. Same allocation and throw contract as getProperty.
BRONZE_EMBED_API Value getElement(Value obj, uint32_t index);

// ---- realms (embed_realm.cpp) -----------------------------------------------

// A Realm encapsulates its own global object (globalThis), builtins, and
// environment. Entering a realm pushes it onto the thread's active realm stack;
// all subsequent accesses to globalThis and global variable lookups delegate to
// the active realm's global object. Exiting a realm pops it from the stack.
// ALLOCATES (the global object).
BRONZE_EMBED_API Realm* createRealm(Value customGlobal = Value::fromUndefined());
BRONZE_EMBED_API void destroyRealm(Realm* realm);
BRONZE_EMBED_API void enterRealm(Realm* realm);
BRONZE_EMBED_API void exitRealm();
BRONZE_EMBED_API Value getRealmGlobal(Realm* realm = nullptr);
BRONZE_EMBED_API Realm* currentRealm();

// RAII helper for entering and exiting a realm.
class RealmScope {
public:
    explicit RealmScope(Realm* realm) : realm_(realm) {
        enterRealm(realm_);
    }
    ~RealmScope() {
        exitRealm();
    }
    RealmScope(const RealmScope&) = delete;
    RealmScope& operator=(const RealmScope&) = delete;

private:
    Realm* realm_;
};

// ---- typed-array access (embed_typed_array.cpp) ----------------------------

// Raw views over the program's binary data, for a host that consumes it in
// place — a GL buffer upload, a texture image, an audio block.
//
// THE POINTER CONTRACT, stated as loudly as it deserves: `data` points INTO
// THE MOVING BRONZE HEAP and is valid only until the next allocation on it —
// any embed call marked ALLOCATES, any call into compiled code, any native
// function a callback re-enters. A host either consumes the bytes
// synchronously (hand them to a GL call that copies them into the driver) or
// memcpy's them out before doing anything else. It never stores the pointer,
// not even alongside a Persistent — the Persistent keeps the VALUE alive and
// current, but this pointer is a snapshot of an address the collector is free
// to abandon.

struct TypedArrayInfo {
    uint8_t* data{nullptr};  // nullptr: the value was not a typed array
    uint32_t byteLength{0};
    uint32_t elementCount{0};
    uint32_t bytesPerElement{0};
    ElementKind elementKind{};  // meaningful only when data != nullptr
    explicit operator bool() const { return data != nullptr; }
};

// The view's window over its buffer — offset already applied, so `data` is
// element 0. Answers a null-data result for anything that is not a typed
// array (an ArrayBuffer and a DataView included: each has its own accessor
// or deliberately none, below).
BRONZE_EMBED_API TypedArrayInfo typedArrayInfo(Value v);

struct ArrayBufferInfo {
    uint8_t* data{nullptr};  // nullptr: the value was not an ArrayBuffer
    uint32_t byteLength{0};
    explicit operator bool() const { return data != nullptr; }
};

// The whole byte store of an ArrayBuffer value. bronze models the buffer and
// its views as separate heap kinds (runtime/typed_array.h), so a host handed
// a Float32Array must go through typedArrayInfo — this answers null for a
// view, exactly as typedArrayInfo answers null for a bare buffer. A DataView
// has no accessor here on purpose: nothing a host binding consumes arrives as
// one, and exposing it would be surface without a caller.
BRONZE_EMBED_API ArrayBufferInfo arrayBufferInfo(Value v);

// Allocate a fresh zero-filled ArrayBuffer of `byteLength` bytes. ALLOCATES.
BRONZE_EMBED_API Value createArrayBuffer(size_t byteLength);

// Allocate an ArrayBuffer initialized with a copy of `bytes`. ALLOCATES.
BRONZE_EMBED_API Value createArrayBuffer(std::span<const uint8_t> bytes);

// ---- typed-array construction (embed_typed_array.cpp) ----------------------
//
// The write half of the seam above: a host that PRODUCES binary data for the
// program — a decoded image, a mesh the engine built, a block of audio — makes
// the view itself and hands it over, rather than asking the program to
// allocate one and filling it afterwards.
//
// The element kind is the same enumeration typedArrayInfo reports, spelled
// here so a host that includes only this header can name one without pulling
// in runtime/typed_array.h. The values are pinned against that header by
// static_asserts in embed_typed_array.cpp — the enum's numbering is stored
// data (an ElementKind lives in every view's header), so it could not move
// even if these constants did not exist.
namespace elements {
inline constexpr ElementKind Int8 = static_cast<ElementKind>(0);
inline constexpr ElementKind Uint8 = static_cast<ElementKind>(1);
inline constexpr ElementKind Uint8Clamped = static_cast<ElementKind>(2);
inline constexpr ElementKind Int16 = static_cast<ElementKind>(3);
inline constexpr ElementKind Uint16 = static_cast<ElementKind>(4);
inline constexpr ElementKind Int32 = static_cast<ElementKind>(5);
inline constexpr ElementKind Uint32 = static_cast<ElementKind>(6);
inline constexpr ElementKind Float32 = static_cast<ElementKind>(7);
inline constexpr ElementKind Float64 = static_cast<ElementKind>(8);
// Appended in the runtime's enumeration order and not 23.2's, because the
// numbers are ABI for generated code and could not be renumbered. A host that
// creates one of the last two gets a view whose ELEMENTS are BigInts; the
// byte-level `fillTypedArray` works on it like any other, and there is no
// double-based host accessor for a 64-bit integer element by design.
inline constexpr ElementKind Float16 = static_cast<ElementKind>(9);
inline constexpr ElementKind BigInt64 = static_cast<ElementKind>(10);
inline constexpr ElementKind BigUint64 = static_cast<ElementKind>(11);
}  // namespace elements

// A view of `length` elements over a fresh zero-filled buffer of its own —
// `new Float32Array(n)` spelled from the host, and the same object the program
// would have got from that expression: same prototype, same element paths, and
// indistinguishable to the compiled code that receives it.
//
// A length whose byte size exceeds what bronze will allocate for one buffer is
// the RangeError the constructor raises, thrown as throwRangeError throws.
// ALLOCATES.
BRONZE_EMBED_API Value createTypedArray(ElementKind kind, uint32_t length);

// Fill from raw bytes: `bytes` is copied into the view's storage starting at
// element 0, in the HOST's byte order and layout — a memcpy, not a conversion,
// so the caller's buffer must already hold the element type's bit patterns
// (float for Float32, int32_t for Int32). This is the fast path a decoder or a
// mesh builder wants; per-element conversion from a JS number is setElement's
// job.
//
// Refuses, writing nothing, if `view` is not a typed array or if `bytes` does
// not fit — a partial fill would leave the program holding half a texture with
// nothing to distinguish it from a whole one. Does NOT allocate, and therefore
// cannot move anything: the copy is the whole of it.
BRONZE_EMBED_API bool fillTypedArray(Value view, std::span<const uint8_t> bytes);

// ---- external buffer storage (embed_typed_array.cpp) ------------------------
//
// The exception to the pointer contract above, bought deliberately: a buffer
// whose bytes live OUTSIDE the moving heap, in a refcounted host block that
// never moves. This is what lets a second engine in the same process — bro's
// QuickJS realm — hold a view over the SAME bytes a compiled program's
// Float32Array reads, instead of a copy that diverges on the first write.
// The runtime never creates one on its own; a buffer becomes external only
// through the two calls below, and every element path (interpreted helper and
// inline generated code alike) selects on the buffer's external word, which
// is the entire runtime cost.
//
// LIFETIME. The store is refcounted, and the count is the only lifetime
// authority — deliberately independent of BOTH collectors, which is what
// makes it safe where a cross-heap reference cannot be. The bronze buffer
// object holds one reference, dropped through a Deferred finalizer (so the
// release runs on a plain host stack at the drainFinalizers checkpoint, where
// a host deleter may do anything); every ExternalBytes handed out below is
// one more, released by the host with releaseExternalStore. The bytes pointer
// stays valid until the LAST reference drops, wherever that happens.

struct ExternalBytes {
    uint8_t* data{nullptr};   // nullptr: the value was not an externalizable buffer
    uint32_t byteLength{0};
    void* store{nullptr};     // opaque refcount handle; owed one releaseExternalStore
    explicit operator bool() const { return data != nullptr; }
};

// Make `bufferOrView`'s storage external, migrating the bytes out of the
// moving heap on the first call (a resizable buffer migrates its whole
// reservation, so `resize` keeps working) — idempotent after that. Accepts an
// ArrayBuffer or any typed-array view (the view's BUFFER is what
// externalizes; the answered window is the view's own). Answers null-data for
// a detached buffer or a non-buffer. The result carries a RETAINED store
// reference the caller must eventually release. ALLOCATES nothing on the
// bronze heap; the returned pointer is NOT subject to the moving-heap
// contract and survives every collection.
BRONZE_EMBED_API ExternalBytes externalizeArrayBuffer(Value bufferOrView);

// A fresh ArrayBuffer whose storage IS `bytes` — host memory bronze never
// copies, for the reverse crossing: an interpreter's buffer read in place by
// compiled code. `deleter(user, bytes)` runs when the last reference drops,
// possibly from the deferred-finalizer drain (a plain host stack; any call is
// legal there, including into another engine). bronze holds the one initial
// reference; the caller keeps none unless it retains. `bytes` must stay valid
// and fixed until the deleter runs, and byteLength is capped like any
// buffer's. If `bytes` already backs a live external store, the new buffer
// SHARES that store and `deleter` runs immediately — the existing
// registration governs the block's lifetime. ALLOCATES (the header cell).
BRONZE_EMBED_API Value createExternalArrayBuffer(uint8_t* bytes, uint32_t byteLength,
                                                 void (*deleter)(void* user, uint8_t* bytes),
                                                 void* user);

// One more / one fewer reference on a store from the two calls above.
BRONZE_EMBED_API void retainExternalStore(void* store);
BRONZE_EMBED_API void releaseExternalStore(void* store);

// A view over an EXISTING buffer — `new Float32Array(buffer, byteOffset, n)`
// spelled from the host, and the way a host hands compiled code a window onto
// an external buffer it just created. `byteOffset` and `length` are validated
// against the buffer (the constructor's RangeError, thrown as throwRangeError throws,
// on a miss); `buffer` must be an ArrayBuffer value. ALLOCATES.
BRONZE_EMBED_API Value createTypedArrayView(ElementKind kind, Value buffer,
                                            uint32_t byteOffset, uint32_t length);

// The buffer behind a typed-array view (undefined for anything else), and the
// view's byte offset into it. A bridge needs the buffer's IDENTITY — two
// views over one buffer must cross as two windows on one store, not two
// stores — and typedArrayInfo deliberately answers only the window's bytes.
BRONZE_EMBED_API Value typedArrayBuffer(Value view);
BRONZE_EMBED_API uint32_t typedArrayByteOffset(Value view);
BRONZE_EMBED_API void detachArrayBuffer(Value v);
BRONZE_EMBED_API bool isDetachedArrayBuffer(Value v);

// ---- promises (embed_promise.cpp) ------------------------------------------

// A fresh pending intrinsic promise. ALLOCATES.
BRONZE_EMBED_API Value createPromise();

// Resolve/reject a promise with `value`/`reason`. Settling schedules reaction
// jobs into the microtask queue (the same queue drainMicrotasks() drains).
// First settle wins (the [[AlreadyResolved]] latch). ALLOCATES (may run user
// thenable getters on resolve).
BRONZE_EMBED_API void resolvePromise(Value promise, Value value);
BRONZE_EMBED_API void rejectPromise(Value promise, Value reason);

// ---- calling into compiled code (embed.cpp) --------------------------------

// One call's outcome: the returned value, or the thrown one. `thrown` false
// with `value` set is the only success shape — there is no third state.
struct CallResult {
    Value value;
    bool thrown{false};
};

// Call a JS function value with `thisValue` and `args`, through the same
// dynamic-call machinery compiled call sites use (bronze_dynamic_call). A
// non-callable `fn` is the TypeError that machinery already raises, reported
// as a thrown result. Any throw is caught here: the host boundary is where
// propagation ends, the way `main`'s uncaught report ends it for a
// standalone program. ALLOCATES (roots the arguments).
BRONZE_EMBED_API CallResult call(Value fn, Value thisValue, std::span<const Value> args);

// `new fn(...args)` through the same machinery a compiled `new` uses
// (bronze_construct): bound functions unwrap, a Proxy takes its `construct`
// trap, and a non-constructor is the TypeError that path already raises,
// reported as a thrown result. With globalValue above, this is how a host
// reaches anything only a constructor can make — the program's own classes
// off globalThis, or `new Proxy(target, traps)` for a dataset/style-shaped
// binding. Same boundary contract as `call`. ALLOCATES.
BRONZE_EMBED_API CallResult construct(Value fn, std::span<const Value> args);

// Parse a UTF-8 JSON string into bronze heap values (objects, arrays, primitives).
// Returns the parsed Value, or thrown=true with an Error instance on syntax error.
// ALLOCATES.
BRONZE_EMBED_API CallResult parseJson(std::string_view jsonUtf8);

// Runs `body` and reports a JS throw out of it as a thrown result, as `call`
// reports one: for host code that runs embed operations (a throw helper, or
// an entry point that propagates) outside any call from JS, where nothing
// above it would catch. `body`'s returned Value is the result otherwise.
BRONZE_EMBED_API CallResult catchThrow(const std::function<Value()>& body);

// ---- value conversions (embed.cpp) -----------------------------------------

BRONZE_EMBED_API Value undefined();
BRONZE_EMBED_API Value null();
BRONZE_EMBED_API Value fromDouble(double d);
BRONZE_EMBED_API Value fromBool(bool b);
// UTF-8 in, heap string out. ALLOCATES — root the result before the next
// allocating call.
BRONZE_EMBED_API Value fromUtf8(std::string_view utf8);

// ToNumber over primitives (an object here is the hard error rt_convert.cpp
// documents — conversions that run user code are the program's business, not
// a host accessor's).
BRONZE_EMBED_API double toDouble(Value v);
BRONZE_EMBED_API uint64_t toUint64(Value v);
BRONZE_EMBED_API int64_t toInt64(Value v);
// JS truthiness — the `if (v)` answer, never a strict-bool unbox.
BRONZE_EMBED_API bool toBool(Value v);
// The string's bytes as UTF-8 for a string value; ToString for the other
// primitives. An object is the same hard error toDouble's is. ALLOCATES for
// non-string inputs.
BRONZE_EMBED_API std::string toUtf8(Value v);

BRONZE_EMBED_API bool isUndefined(Value v);
BRONZE_EMBED_API bool isNull(Value v);
// A function object — callable through `call` above.
BRONZE_EMBED_API bool isFunction(Value v);
// Any Object-tagged heap value (functions and arrays included), which is the
// `typeof v === "object" || typeof v === "function"` envelope a host binding
// usually wants before reading properties.
BRONZE_EMBED_API bool isObject(Value v);
// A symbol primitive — the value a host must NOT hand to toUtf8/toDouble
// (7.1.17 makes that a TypeError in JS; here it is the same hard error other
// objects get), and must pass through call/getProperty untouched instead.
BRONZE_EMBED_API bool isSymbol(Value v);
// The three remaining primitive tags, by name. A host that CONVERTS values —
// one bridging them into another engine, rather than reading a property it
// already knows the type of — has to ask which primitive it is holding, and
// the coercions above cannot answer: toDouble of "5" and of 5 agree, and
// toUtf8 of 5 and of "5" agree, so a bridge built on them turns one into the
// other silently. These are the tag itself, not a coercion.
BRONZE_EMBED_API bool isNumber(Value v);
BRONZE_EMBED_API bool isBigInt(Value v);
BRONZE_EMBED_API bool isString(Value v);
BRONZE_EMBED_API bool isBool(Value v);
BRONZE_EMBED_API bool isPromise(Value v);
BRONZE_EMBED_API bool isArrayBuffer(Value v);
BRONZE_EMBED_API bool isTypedArray(Value v);

}  // namespace bronze::embed
