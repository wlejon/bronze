#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "runtime/value.h"

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
// Threading: single-threaded by design, like the runtime itself (bronze has no
// threads and no design for them). The host owns the frame loop and calls in
// from one thread.
//
// THE GC CONTRACT, which every function below is written against: the heap is
// a moving semispace collector, so any allocation may relocate every heap
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
// one: use only (1) — dlopen the module, resolve the three symbols
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
// runtime/typed_array.h owns the definition; this is the opaque redeclaration,
// so typedArrayInfo below can name the kind without pulling the heap headers
// into every host translation unit.
enum class ElementKind : uint32_t;
}  // namespace bronze

namespace bronze::embed {

using Value = bronze::Value;

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

// Collect now. A host that has just released a large graph — a level torn
// down, a frame's scratch objects dropped — knows something the heap's own
// growth heuristic does not, and this is how it says so. Everything the host
// holds in a Persistent survives and is updated in place; every raw pointer
// the host obtained from typedArrayInfo or arrayBufferInfo is DEAD after this,
// by the pointer contract those functions carry.
BRONZE_EMBED_API void collectGarbage();

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

// ---- persistent handles (embed_handle.cpp) ---------------------------------

// A heap-safe root the host may hold across frames and collections: the
// collector updates the slot in place, so `get()` always answers the value's
// CURRENT address. Backed by a global slot registry with one root source
// (the g_rootShapes pattern in rt_state.cpp).
//
// COPYABLE, and a copy is an independent root over the same value — chosen
// over move-only because a host stores these in containers and callback
// captures, where "copying a handle" reading as "one more root" costs a slot
// and surprises nobody, while a deleted copy constructor turns every capture
// into a std::move audit.
class BRONZE_EMBED_API Persistent {
public:
    Persistent();  // holds undefined
    explicit Persistent(Value v);
    ~Persistent();

    Persistent(const Persistent& other);
    Persistent& operator=(const Persistent& other);
    Persistent(Persistent&& other) noexcept;
    Persistent& operator=(Persistent&& other) noexcept;

    Value get() const;
    void set(Value v);

private:
    static constexpr uint32_t kNoSlot = UINT32_MAX;
    uint32_t slot_{kNoSlot};  // kNoSlot only in the moved-from state
};

// The bits bridge, for a host that stores u64 (a component table, a script
// field). Raw bits are NOT a root — bits held across an allocation name a
// pre-collection address. Round-trip through a Persistent to keep them live.
BRONZE_EMBED_API uint64_t toBits(Value v);
BRONZE_EMBED_API Value fromBits(uint64_t bits);

// ---- native functions (embed_function.cpp) ---------------------------------

// A host callable, invoked when compiled JS calls the wrapping function
// object. `args` points at GC-ROOTED slots the collector updates in place, so
// the span stays current across anything the callback does; `thisValue` is a
// plain copy, current at entry — re-root it before allocating.
//
// To throw into JS, call one of the throw helpers below and return its result;
// do NOT let a C++ exception escape — the caller may be generated code, whose
// frames carry no unwind metadata (fatal.h says why that boundary is hard).
using NativeFn = std::function<Value(Value thisValue, std::span<const Value> args)>;

// Wrap `fn` into a bronze function object callable from compiled JS. The
// closure state rides in the function's environment slot as a native handle
// (see below), so it survives collections and is destroyed when the function
// object dies. `arity` is the count short calls are padded to (undefined
// fill), not the JS `length`. ALLOCATES.
BRONZE_EMBED_API Value makeFunction(NativeFn fn, uint32_t arity = 0);

// Raise into the compiled program through the runtime's pending-exception
// cell, exactly as the builtins in src/runtime/builtin_*.cpp do. Each returns
// `undefined`, which the callback returns in turn — the caller's exception
// check does the rest.
BRONZE_EMBED_API Value throwValue(Value thrown);
BRONZE_EMBED_API Value throwError(const std::string& message);
BRONZE_EMBED_API Value throwTypeError(const std::string& message);
BRONZE_EMBED_API Value throwRangeError(const std::string& message);

// ---- object building (embed_object.cpp) ------------------------------------

// A plain `{}` with the shape every literal shares, so host-built objects sit
// on the same inline-cache paths as program-built ones. ALLOCATES.
BRONZE_EMBED_API Value createObject();

// Define an own data property (enumerable, like an assignment). ALLOCATES and
// may move `obj`: the return value is the object's post-call address, and any
// OTHER Value the host holds must be re-read from a Persistent.
BRONZE_EMBED_API Value setProperty(Value obj, std::string_view key, Value v);

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
// way getProperty handles a throwing getter: the pending cell is cleared at
// the host boundary and the write is dropped. ALLOCATES; same return contract
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
// throws is handled the way `call` handles a throw: the pending cell is
// cleared at the host boundary and the read answers undefined (a host
// accessor has no JS frame to propagate into). MAY ALLOCATE and MAY RUN USER
// CODE (a getter), so every other Value the host holds must be re-read from a
// Persistent afterwards.
BRONZE_EMBED_API Value getProperty(Value obj, std::string_view key);

// `obj[3]` spelled from the host — the numeric key takes the same path a
// program's `arr[i]` does, so it answers for real arrays, typed arrays and
// plain objects alike. Same allocation and throw contract as getProperty.
BRONZE_EMBED_API Value getElement(Value obj, uint32_t index);

// ---- opaque native handles (embed_handle.cpp) ------------------------------

// A heap cell owning a raw host pointer and a destructor: how a binding hangs
// a C++ object (an Engine*, a GL wrapper) off a bronze value. The destructor
// runs when the collector proves the cell dead — via the finalizer registry
// swept after each collection, because a moving collector never visits dead
// objects — and does NOT run at process exit for cells still alive then. It
// runs MID-COLLECTION, so it must not touch the bronze heap or call back into
// this API; freeing host memory is its whole job. ALLOCATES.
//
// The cell is a real plain object as far as the program is concerned: opaque
// by convention, not enforcement. A program that writes properties on one
// gets an ordinary object with properties; the payload stays invisible either
// way (internal slots have no property names).
using HandleDestructor = void (*)(void* data);
BRONZE_EMBED_API Value makeHandle(void* data, HandleDestructor dtor);

// The pointer a handle carries, or nullptr for a value that is not one.
BRONZE_EMBED_API void* handleData(Value handle);

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
// the RangeError the constructor raises, reported through the pending cell.
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
// as a thrown result. The pending-exception cell is checked and CLEARED here:
// the host boundary is where propagation ends, the way `main`'s uncaught
// handler ends it for a standalone program. ALLOCATES (roots the arguments).
BRONZE_EMBED_API CallResult call(Value fn, Value thisValue, std::span<const Value> args);

// Parse a UTF-8 JSON string into bronze heap values (objects, arrays, primitives).
// Returns the parsed Value, or thrown=true with an Error instance on syntax error.
// ALLOCATES.
BRONZE_EMBED_API CallResult parseJson(std::string_view jsonUtf8);

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
BRONZE_EMBED_API bool isPromise(Value v);
BRONZE_EMBED_API bool isArrayBuffer(Value v);
BRONZE_EMBED_API bool isTypedArray(Value v);

}  // namespace bronze::embed
