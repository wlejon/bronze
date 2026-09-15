#pragma once

#include <cstddef>
#include <cstdint>

#include "runtime/native_handle.h"
#include "runtime/value.h"

#ifndef BRONZE_EMBED_API
#  if defined(BRONZE_EMBED_SHARED_BUILD)
#    if defined(_WIN32)
#      define BRONZE_EMBED_API __declspec(dllexport)
#    else
#      define BRONZE_EMBED_API __attribute__((visibility("default")))
#    endif
#  elif defined(BRONZE_EMBED_SHARED) && defined(_WIN32)
#    define BRONZE_EMBED_API __declspec(dllimport)
#  else
#    define BRONZE_EMBED_API
#  endif
#endif

namespace bronze::embed {

using Value = bronze::Value;

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

// ---- local handle scopes (embed_handle.cpp) -------------------------------
//
// RAII management of short-lived GC roots on the C++ stack. Entering a
// HandleScope captures the stack top, and leaving it unwinds all local handles
// created within that scope in O(1) time without free-list overhead.
// Local<T> holds a slot index updated in place across moving collections.

BRONZE_EMBED_API uint32_t createLocalSlot(Value v);
BRONZE_EMBED_API Value getLocalValue(uint32_t index);
BRONZE_EMBED_API void setLocalValue(uint32_t index, Value v);

class HandleScope;
class EscapableHandleScope;

template <typename T = Value>
class Local {
public:
    Local() noexcept : index_(kNoIndex) {}
    explicit Local(uint32_t index) noexcept : index_(index) {}
    Local(HandleScope& scope, Value v);

    Local(const Local& other) noexcept = default;
    Local& operator=(const Local& other) noexcept = default;
    Local(Local&& other) noexcept : index_(other.index_) { other.index_ = kNoIndex; }
    Local& operator=(Local&& other) noexcept {
        if (this != &other) {
            index_ = other.index_;
            other.index_ = kNoIndex;
        }
        return *this;
    }

    bool isEmpty() const noexcept { return index_ == kNoIndex; }
    Value get() const { return index_ != kNoIndex ? getLocalValue(index_) : Value::fromUndefined(); }
    void set(Value v) {
        if (index_ != kNoIndex) setLocalValue(index_, v);
    }

    Value operator*() const { return get(); }
    operator Value() const { return get(); }

    uint32_t index() const noexcept { return index_; }

private:
    static constexpr uint32_t kNoIndex = UINT32_MAX;
    uint32_t index_{kNoIndex};
};

class BRONZE_EMBED_API HandleScope {
public:
    HandleScope();
    ~HandleScope();

    HandleScope(const HandleScope&) = delete;
    HandleScope& operator=(const HandleScope&) = delete;

    template <typename T = Value>
    Local<T> createLocal(Value v) {
        return Local<T>(createLocalSlot(v));
    }

    size_t numberOfHandles() const noexcept;

protected:
    friend class EscapableHandleScope;
    uint32_t prev_top_{0};
    HandleScope* prev_scope_{nullptr};
};

template <typename T>
inline Local<T>::Local(HandleScope& scope, Value v) : index_(scope.createLocal<T>(v).index()) {}

class BRONZE_EMBED_API EscapableHandleScope : public HandleScope {
public:
    EscapableHandleScope();
    ~EscapableHandleScope();

    template <typename T = Value>
    Local<T> escape(Local<T> val) {
        return Local<T>(escapeSlot(val.index()));
    }

private:
    uint32_t escapeSlot(uint32_t index);
    bool escaped_{false};
};

// The bits bridge, for a host that stores u64 (a component table, a script
// field). Raw bits are NOT a root — bits held across an allocation name a
// pre-collection address. Round-trip through a Persistent to keep them live.
BRONZE_EMBED_API uint64_t toBits(Value v);
BRONZE_EMBED_API Value fromBits(uint64_t bits);

// ---- opaque native handles (embed_handle.cpp) ------------------------------

// A heap cell owning a raw host pointer and a destructor: how a binding hangs
// a C++ object (an Engine*, a GL wrapper) off a bronze value. The destructor
// runs when the collector proves the cell dead — via the finalizer registry
// swept after each collection, because a moving collector never visits dead
// objects — and does NOT run at process exit for cells still alive then.
// ALLOCATES.
//
// The cell is a real plain object as far as the program is concerned: opaque
// by convention, not enforcement. A program that writes properties on one
// gets an ordinary object with properties; the payload stays invisible either
// way (internal slots have no property names). `Object.setPrototypeOf` on a
// handle is IN-CONTRACT: the payload and the brand live in internal slots the
// swap cannot reach, so handleData still answers afterwards — which is how a
// wrapper layer puts its methods on one shared prototype per class instead of
// closing over the handle per instance.
//
// The cell mechanism itself is the runtime's (runtime/native_handle.h),
// because generated code makes and unwraps handles too — a native class's
// constructor result and a native method's receiver (registerNative below)
// — and these two names are its types, spelled here for the host.
using HandleDestructor = runtime::HandleDestructor;

// WHEN the destructor runs, which decides what it may do:
//
//   InSweep   Inside the collection that proved the cell dead. The destructor
//             must not touch the bronze heap or call back into this API — the
//             collection is mid-flight; freeing host memory is its whole job.
//             The default, because it needs no pumping from the host.
//   Deferred  Queued at that same moment, run at the next drainFinalizers()
//             — which drainMicrotasks() performs, so a host already pumping
//             the checkpoint gets it free. By then the cell is long gone and
//             only the {data, dtor} pair survives, so there is nothing to
//             resurrect — which is what makes it safe for the destructor to
//             allocate, make any embed call, even call into compiled code: it
//             runs on a plain host stack like any other host code. For
//             teardown that must NOTIFY something (an observer list, a cache
//             invalidation in the program) rather than merely free memory.
using Finalize = runtime::Finalize;
BRONZE_EMBED_API Value makeHandle(void* data, HandleDestructor dtor,
                                  Finalize when = Finalize::InSweep);

// A handle BORN on `prototype` — an instance of a host class rather than a
// bare cell. This is the class story in three calls, all of them existing:
// read `prototype` off a makeFunction constructor with getProperty (the read
// mints the slot-backed object 10.2.4 describes; it is a plain object), put
// the shared methods on it with setProperty, and create each instance here.
// Instances inherit the methods (one copy per class, not a closure per
// instance), `x instanceof Ctor` answers true, and a method body unwraps its
// receiver with handleData(thisValue). Born-on, not swapped-on: instances
// share the memoized per-prototype root shape — the same one Object.create
// hands out — so property reads keep their inline caches, where a
// setPrototypeOf'd cell is in dictionary mode for the rest of its life.
// A constructor body may also return one of these to hand `new Ctor(...)`
// from COMPILED CODE a handle instance: bronze_construct's object-return rule
// delivers it in place of the ordinary instance. `prototype` must be a plain
// object; anything else is a named fatal. ALLOCATES.
BRONZE_EMBED_API Value makeHandle(void* data, HandleDestructor dtor,
                                  Finalize when, Value prototype);

// Run the destructors of Deferred handles whose cells a collection has since
// proved dead. RUNS HOST CODE that may allocate, collect, and thereby queue
// more — the drain keeps going until the queue is empty. A drain begun while
// one is already running on this thread returns immediately (the outer drain
// will pick up whatever was queued), so a destructor that reaches
// drainMicrotasks does not recurse. Safe to call with an empty queue.
BRONZE_EMBED_API void drainFinalizers();

// Anything queued? microtasksPending's twin, for the same shutdown-quiescence
// loop.
BRONZE_EMBED_API bool finalizersPending();

// The pointer a handle carries, or nullptr for a value that is not one.
BRONZE_EMBED_API void* handleData(Value handle);

}  // namespace bronze::embed
