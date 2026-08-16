// Subclassing a native constructor: which exotic object a `new` allocates,
// where the subclass's prototype lives on one, and how a read finds it.
// runtime/native_base.h carries the design; this file is its implementation and
// the single list of which natives are subclassable.

#include "runtime/native_base.h"

#include <string>

#include "runtime/array.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/promise.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"

namespace bronze::runtime {

namespace {

bool isFunctionObject(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == HeapKind::Function;
}

// The exotic kinds whose ordinary-object half is a `properties` box. Both
// headers put it at the same conceptual place and neither carries a shape, so
// the two are one question everywhere below.
bool hasPropertyBox(Value v) {
    if (!v.isObject()) return false;
    const uint16_t kind = v.asObject<HeapObjectHeader>()->flags;
    return kind == HeapKind::Array || kind == MapHeader::kMapFlags ||
           kind == MapHeader::kSetFlags || kind == MapHeader::kWeakMapFlags ||
           kind == MapHeader::kWeakSetFlags;
}

// Give the instance's ordinary half the [[Prototype]] 10.1.14 derived, which
// for these kinds means BUILDING the box with that prototype behind it rather
// than the null one an ordinary expando store leaves it with.
//
// The box is built here rather than through `ensureProperties` and
// `setPrototype`, and it is not a shortcut: `setPrototype` puts an object into
// DICTIONARY mode by design (a prototype lives on a shape's root, so swapping
// one needs a shape of this object's own), and a dictionary per subclass
// instance would cost every one of them its shape sharing and its inline
// caches. Creating the box already pointed at the right prototype gives every
// instance of a class ONE memoized root shape, which is what an ordinary class
// instance gets.
//
// Skipped entirely when `proto` is not a subclass's: `new Array()` reaches here
// with %Array.prototype% and `new Map()` with nothing at all, and neither may
// be given a box — an array with one is an array that has left the
// method-cache fast path (rt_prop.cpp fills the array-method IC only for an
// array with no box, which is precisely the guard that keeps a subclass
// instance off it).
void attachSubclassPrototype(Rooted<Value>& self, Rooted<Value>& proto) {
    if (!proto.get().isObject()) return;
    if (rtIsArrayPrototypeObject(proto.get())) return;
    if (ObjectHeader* protoObj = proto.get().asObject<ObjectHeader>(); protoObj->shape) {
        protoObj->shape->used_as_prototype = true;
    }
    Shape* boxShape = rtRootShapeForPrototype(proto.get());
    ObjectHeader* box = ObjectHeader::create(rtHeap(), rtArena(), boxShape);
    box->header.flags = HeapKind::Plain;
    // Re-derived through the root: `create` above may have moved the instance.
    const Value boxVal = Value::fromObject(box);
    if (self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
        self.get().asObject<ArrayHeader>()->properties = boxVal;
    } else {
        self.get().asObject<MapHeader>()->properties = boxVal;
    }
}

}  // namespace

uint8_t rtNativeBaseOf(Value fn) {
    if (!isFunctionObject(fn)) return NativeBase::None;
    if (const uint8_t recorded = fn.asObject<FunctionHeader>()->native_base;
        recorded != NativeBase::None) {
        return recorded;
    }
    // Every probe below can materialise the intrinsic it compares against, so
    // the argument is re-read from a root between them rather than compared as
    // the address it had on entry.
    Rooted<Value> f{fn};
    if (rtIsArrayConstructor(f.get())) return NativeBase::Array;
    if (rtIsPromiseConstructor(f.get())) return NativeBase::Promise;
    if (const char* name = rtMapConstructorName(f.get())) {
        return std::string(name) == "Map" ? NativeBase::Map : NativeBase::Set;
    }
    return NativeBase::None;
}

void rtInheritNativeBase(Rooted<Value>& derived, Rooted<Value>& base) {
    if (!isFunctionObject(derived.get())) return;
    // The answer FIRST, then the store: reading it can allocate, and a
    // `derived.get()` evaluated before that allocation would be written through
    // a pointer the collector has already retired.
    const uint8_t inherited = rtNativeBaseOf(base.get());
    derived.get().asObject<FunctionHeader>()->native_base = inherited;
}

void rtRealizeNativeStatics(Rooted<Value>& ctor) {
    // Two families answer statics beside the value, and each realizes them off
    // the very table its read path uses. Neither is asked which constructor
    // this is: both answer false for a function that is not theirs, so the day
    // a third family joins, this is one more line and no new decision.
    if (rtInstallGlobalConstructorStatics(ctor)) return;
    rtInstallMapStatics(ctor);
}

Value rtAllocateNativeBaseInstance(uint8_t kind, Rooted<Value>& ctor) {
    // 10.1.14 GetPrototypeFromConstructor is `Get(constructor, "prototype")`,
    // and for a class that is the object `extends` built. For the intrinsic
    // itself it is %Array.prototype% (a real object) or, for a Map and a Set,
    // nothing at all — bronze builds no prototype OBJECT for those two, and the
    // instance keeps the beside-the-value chain it has always had.
    rtEnsureFunctionPrototype(ctor);
    Rooted<Value> proto{ctor.get().asObject<FunctionHeader>()->prototype};
    Rooted<Value> self{Value::fromUndefined()};
    switch (kind) {
        case NativeBase::Array:
            self.set(Value::fromObject(ArrayHeader::create(rtHeap())));
            break;
        case NativeBase::Map:
            self.set(Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kMapFlags)));
            break;
        case NativeBase::Set:
            self.set(Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kSetFlags)));
            break;
        case NativeBase::Promise:
            // A promise is an ordinary object with internal slots, so its
            // [[Prototype]] is its SHAPE's — and the constructor already holds
            // the memoized root shape for its own prototype, which is exactly
            // what 10.1.13 asks for and costs no shape per instance.
            return rtNewPromiseWithShape(ctor.get().asObject<FunctionHeader>()->instance_shape);
        default:
            fatal("internal: unknown native base kind at construction");
    }
    attachSubclassPrototype(self, proto);
    return self.get();
}

namespace {
thread_local NativeReceiverScope* g_topNativeReceiver = nullptr;
}  // namespace

NativeReceiverScope::NativeReceiverScope(Value receiver)
    : receiver_(receiver), prev_(g_topNativeReceiver) {
    g_topNativeReceiver = this;
}

NativeReceiverScope::~NativeReceiverScope() { g_topNativeReceiver = prev_; }

bool rtIsNativeConstructReceiver(Value v) {
    if (!v.isObject() || !g_topNativeReceiver) return false;
    return g_topNativeReceiver->receiver_.get().rawBits() == v.rawBits();
}

void rtCheckNativeBaseExtends(Rooted<Value>& base) {
    if (rtNativeBaseOf(base.get()) != NativeBase::None) return;

    // Everything below is a native whose instances are built by the runtime
    // and which this file has no allocation for. Each is refused BY NAME
    // rather than left to produce a derived instance that is an ordinary plain
    // object: `new (class extends Date)()` would answer TypeError to every one
    // of 21.4.4's members, which is a wrong answer where this is a missing one.
    //
    // The message names the SLOT the subclass would be missing, because that is
    // what a reader has to know to judge the workaround (composition holds a
    // real one; `extends` cannot).
    if (const char* name = rtIntrinsicConstructorName(base.get())) {
        // `String`, `Boolean`, `Number` and `Proxy`. `Array` never reaches
        // here — `rtNativeBaseOf` answered for it above — which is why this
        // list is a refusal and not a table of everything in that file.
        //
        // A Proxy is not a wrapper and gets its own sentence: 28.2.1.1 makes
        // its [[ProxyTarget]] and [[ProxyHandler]] the object, not slots ON an
        // ordinary object, and every internal method it has is the handler's.
        if (std::string(name) == "Proxy") {
            fatal("extending `Proxy` is unsupported (a Proxy IS its target and handler pair "
                  "rather than an object carrying them, so a subclass would inherit no "
                  "internal method at all)");
        }
        fatal((std::string("extending the native constructor `") + name +
               "` is unsupported (its instances are the primitive WRAPPER its body builds from "
               "the argument, not an object created from NewTarget, so a subclass's would "
               "carry no wrapped value)")
                  .c_str());
    }
    if (rtIsDateConstructor(base.get())) {
        fatal("extending `Date` is unsupported (a Date's [[DateValue]] is created by the "
              "runtime's own constructor, so a subclass's instances would not carry one)");
    }
    if (const char* name = rtWeakCollectionConstructorName(base.get())) {
        fatal((std::string("extending `") + name +
               "` is unsupported (its instances carry [[WeakMapData]]/[[WeakSetData]], which "
               "bronze allocates only for the intrinsic, so a subclass's would carry none)")
                  .c_str());
    }
    if (rtIsArrayBufferConstructor(base.get())) {
        fatal("extending `ArrayBuffer` is unsupported (its instances carry [[ArrayBufferData]] "
              "— the raw block itself, not a Value on an ordinary object — which bronze "
              "allocates only for the intrinsic, so a subclass's would carry none)");
    }
    if (const char* name = rtTypedArrayConstructorName(base.get())) {
        fatal((std::string("extending `") + name +
               "` is unsupported (its instances carry [[ViewedArrayBuffer]] and the length "
               "and offset beside it, which bronze allocates only for the intrinsic, so a "
               "subclass's would carry none)")
                  .c_str());
    }
    if (const char* name = rtDataViewConstructorName(base.get())) {
        fatal((std::string("extending `") + name +
               "` is unsupported (its instances carry [[ViewedArrayBuffer]], which bronze "
               "allocates only for the intrinsic, so a subclass's would carry none)")
                  .c_str());
    }
    if (rtIsRegExpConstructor(base.get())) {
        fatal("extending `RegExp` is unsupported (a RegExp's [[RegExpMatcher]] and the "
              "compiled pattern beside it are created by the runtime's own constructor, so "
              "a subclass's instances would carry none and every member of 22.2.6 would "
              "read `undefined` on them)");
    }
}

Value rtExoticPropertyBox(Value obj) {
    if (!hasPropertyBox(obj)) return Value::fromUndefined();
    // Both headers keep the box in a Value field; the two are read through
    // their own types because the field sits at a different offset in each.
    if (obj.asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
        return obj.asObject<ArrayHeader>()->properties;
    }
    return obj.asObject<MapHeader>()->properties;
}

Value rtExoticSubclassPrototype(Value obj) {
    const Value box = rtExoticPropertyBox(obj);
    if (!box.isObject()) return Value::fromUndefined();
    ObjectHeader* proto = box.asObject<ObjectHeader>()->protoAncestor(1);
    return proto ? Value::fromObject(proto) : Value::fromUndefined();
}

bool rtExoticNamedRead(Rooted<Value>& recv, StringHeader* keyHeader, Value& out) {
    if (!keyHeader) return false;
    // THE FAST PATH, and the reason this is one call rather than a chain walk
    // spliced into five member functions: an array or a collection a program
    // has neither subclassed nor written a named property on has no box, and
    // answers here with one load and one tag test.
    Rooted<Value> box{rtExoticPropertyBox(recv.get())};
    if (!box.get().isObject()) return false;

    const PropertyKey name = PropertyKey::forString(keyHeader);
    Rooted<Value> holder{box.get()};
    for (uint32_t depth = 0; depth < ObjectHeader::kMaxPrototypeDepth; ++depth) {
        ObjectHeader* obj = holder.get().asObject<ObjectHeader>();
        PropertyInfo info;
        if (obj->shape && obj->shape->lookupProperty(name, info)) {
            Rooted<Value> key{Value::fromString(keyHeader)};
            // The RECEIVER is the exotic object, so an accessor defined on a
            // subclass prototype runs against the Map the program read from
            // and not against the box its own properties live in.
            out = holder.get().asObject<ObjectHeader>()->getProp(rtHeap(), key, /*ic=*/nullptr,
                                                                 recv.slot_ptr());
            return true;
        }
        ObjectHeader* next = holder.get().asObject<ObjectHeader>()->protoAncestor(1);
        if (!next) return false;
        holder.set(Value::fromObject(next));
    }
    fatal("prototype chain of a native subclass instance exceeds the bounded walk");
}

}  // namespace bronze::runtime
