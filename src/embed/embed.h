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
void setupIo();

// Run the compiled program: a GC root frame for the call, then
// `bronze_main()`. The host registers its globals and functions BEFORE this —
// the program's top level runs here, and a read of a host global it performs
// must find the value already registered.
void runMain();

// setupIo() then runMain(): the whole of the standalone `main`, for a host
// with no stdio opinions.
void runProgram();

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
void drainMicrotasks();

// Is there anything queued? For a host that wants to know whether a frame's
// work actually finished — and for a shutdown path that drains to quiescence
// before tearing the runtime down. Never necessary before drainMicrotasks(),
// which is a no-op on an empty queue.
bool microtasksPending();

// ---- host globals ----------------------------------------------------------

// Provide `name` as a global of the compiled program. The compile side is
// `--host-globals`: a name must be in the manifest the program was compiled
// with for its reads to reach the registry at all. Registering again replaces
// the value. The value is rooted for the life of the process.
void registerGlobal(std::string_view name, Value value);

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
class Persistent {
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
uint64_t toBits(Value v);
Value fromBits(uint64_t bits);

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
Value makeFunction(NativeFn fn, uint32_t arity = 0);

// Raise into the compiled program through the runtime's pending-exception
// cell, exactly as the builtins in src/runtime/builtin_*.cpp do. Each returns
// `undefined`, which the callback returns in turn — the caller's exception
// check does the rest.
Value throwValue(Value thrown);
Value throwError(const std::string& message);
Value throwTypeError(const std::string& message);
Value throwRangeError(const std::string& message);

// ---- object building (embed_object.cpp) ------------------------------------

// A plain `{}` with the shape every literal shares, so host-built objects sit
// on the same inline-cache paths as program-built ones. ALLOCATES.
Value createObject();

// Define an own data property (enumerable, like an assignment). ALLOCATES and
// may move `obj`: the return value is the object's post-call address, and any
// OTHER Value the host holds must be re-read from a Persistent.
Value setProperty(Value obj, std::string_view key, Value v);

// The same, under an integer key — `obj[3]` spelled from the host. A plain
// object stores it as the canonical numeric string, which is what enumeration
// order keys off. ALLOCATES; same return contract as setProperty.
Value setElement(Value obj, uint32_t index, Value v);

// `get key()` / `set key(v)` as one accessor property; pass undefined for a
// half the host does not provide. ALLOCATES; same return contract.
Value defineAccessor(Value obj, std::string_view key, Value getter, Value setter,
                     bool enumerable = true);

// Object.freeze, for a host handing the program a namespace it must not be
// able to redecorate. Returns the object (freezing does not move it, but the
// uniform shape keeps call sites chainable).
Value freeze(Value obj);

// ---- property reads (embed.cpp) --------------------------------------------

// `obj[key]` through the same generic element-get path a computed read in
// compiled code takes — prototype chain, accessors and all. A getter that
// throws is handled the way `call` handles a throw: the pending cell is
// cleared at the host boundary and the read answers undefined (a host
// accessor has no JS frame to propagate into). MAY ALLOCATE and MAY RUN USER
// CODE (a getter), so every other Value the host holds must be re-read from a
// Persistent afterwards.
Value getProperty(Value obj, std::string_view key);

// `obj[3]` spelled from the host — the numeric key takes the same path a
// program's `arr[i]` does, so it answers for real arrays, typed arrays and
// plain objects alike. Same allocation and throw contract as getProperty.
Value getElement(Value obj, uint32_t index);

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
Value makeHandle(void* data, HandleDestructor dtor);

// The pointer a handle carries, or nullptr for a value that is not one.
void* handleData(Value handle);

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
TypedArrayInfo typedArrayInfo(Value v);

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
ArrayBufferInfo arrayBufferInfo(Value v);

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
CallResult call(Value fn, Value thisValue, std::span<const Value> args);

// ---- value conversions (embed.cpp) -----------------------------------------

Value undefined();
Value null();
Value fromDouble(double d);
Value fromBool(bool b);
// UTF-8 in, heap string out. ALLOCATES — root the result before the next
// allocating call.
Value fromUtf8(std::string_view utf8);

// ToNumber over primitives (an object here is the hard error rt_convert.cpp
// documents — conversions that run user code are the program's business, not
// a host accessor's).
double toDouble(Value v);
// JS truthiness — the `if (v)` answer, never a strict-bool unbox.
bool toBool(Value v);
// The string's bytes as UTF-8 for a string value; ToString for the other
// primitives. An object is the same hard error toDouble's is. ALLOCATES for
// non-string inputs.
std::string toUtf8(Value v);

bool isUndefined(Value v);
bool isNull(Value v);
// A function object — callable through `call` above.
bool isFunction(Value v);
// Any Object-tagged heap value (functions and arrays included), which is the
// `typeof v === "object" || typeof v === "function"` envelope a host binding
// usually wants before reading properties.
bool isObject(Value v);

}  // namespace bronze::embed
