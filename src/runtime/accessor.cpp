// Accessor properties: a property that is a PAIR OF FUNCTIONS rather than a
// value in a slot. Defining one is here; reading and writing through one is
// object.cpp's property path, which is the only thing that knows a receiver.

#include "runtime/accessor.h"

#include <string>

#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/object.h"

namespace bronze {

namespace {

// An accessor half is either `undefined` (the half was never written) or a
// function. Anything else means a slot pair was built from something that is
// not an accessor, which is a runtime bug and not a program error.
FunctionHeader* asAccessorFunction(Value v, const char* which) {
    if (v.isUndefined()) return nullptr;
    if (!v.isObject() || v.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        fatal((std::string("internal: a property's ") + which +
               " slot holds something that is not a function")
                  .c_str());
    }
    return v.asObject<FunctionHeader>();
}

}  // namespace

Value callGetter(Value getter, Rooted<Value>& receiver) {
    Rooted<Value> fnRoot{getter};
    if (!asAccessorFunction(getter, "getter")) return Value::fromUndefined();
    // Re-read through the root: nothing between here and the call allocates
    // today, but the callee is arbitrary user code and the pattern is what
    // keeps that true when something is inserted above.
    return fnRoot.get().asObject<FunctionHeader>()->call(receiver.get(), 0, nullptr);
}

void callSetter(Value setter, Rooted<Value>& receiver, Rooted<Value>& value,
                bool* noSetter) {
    Rooted<Value> fnRoot{setter};
    if (!asAccessorFunction(setter, "setter")) {
        // A get-only property written to: 10.1.9.2 step 5.c returns false. What
        // that MEANS is decided two frames up, by whether the reference was
        // strict — a sloppy write is the silent no-op `accessor_properties`
        // pins, a strict one is a TypeError — so this reports and does not
        // choose.
        if (noSetter) *noSetter = true;
        return;
    }
    // The argument buffer is the ROOT's slot, so the value stays live across
    // whatever the setter allocates before it copies its parameter out.
    fnRoot.get().asObject<FunctionHeader>()->call(receiver.get(), 1, value.slot_ptr());
}

// `get k() {}` and `set k(v) {}` define ONE property with two halves, so the
// second half of a name must find the first rather than create a rival
// property. Three ways in, in the order the cost rises:
//
//  - the name is already an accessor here: both halves have a home, and the
//    shape does not change. That is what makes `{ get x() {}, set x(v) {} }`
//    one property, and what lets two literals written the same way share a
//    hidden class;
//  - the object is a dictionary already: an entry, at the position it has;
//  - the name is an own DATA property: the transition tree cannot widen a
//    one-slot property into a two-slot one without renumbering slots it
//    shares with other objects, so this is exactly the case dictionary mode
// exists for.
void ObjectHeader::defineAccessor(Heap& heap, NonMovingArena& arena, Rooted<Value>& self,
                                  Rooted<Value>& key, Rooted<Value>& getter,
                                  Rooted<Value>& setter, bool enumerable) {
    const PropertyKey name = PropertyKey::fromValue(key.get());
    if (!name.valid()) {
        fatal("property name must be a string or a symbol");
    }
    auto* obj = self.get().asObject<ObjectHeader>();
    if (!obj->shape) {
        fatal("internal: an accessor defined on an object with no shape");
    }

    uint32_t slot = 0;
    PropertyInfo own;
    const bool hasOwn = obj->shape->lookupProperty(name, own);
    if (hasOwn && own.accessor) {
        slot = own.slot;
    } else if (obj->shape->isDictionary()) {
        obj = dictDefine(heap, arena, self, name, enumerable, /*accessor=*/true, slot);
    } else if (hasOwn) {
        toDictionary(arena, self);
        obj = dictDefine(heap, arena, self, name, enumerable, /*accessor=*/true, slot);
    } else {
        // An add on a shape-chain object, exactly like `setProp`'s: if this
        // object is somebody's prototype, the pair just defined shadows what
        // every depth > 0 entry below it points at.
        if (obj->shape->used_as_prototype) bumpProtoMutationEpoch();
        Shape* next = obj->shape->addProperty(arena, heap, key, slot, enumerable,
                                              /*is_accessor=*/true);
        obj = ensureSlots(heap, self, slot + 2);
        obj->shape = next;
        // A pair starts empty, so the half this call does not write reads as
        // absent rather than as whatever the slot last held.
        obj->setSlot(slot, Value::fromUndefined());
        obj->setSlot(slot + 1, Value::fromUndefined());
    }

    if (!getter.get().isUndefined()) obj->setSlot(slot, getter.get());
    if (!setter.get().isUndefined()) obj->setSlot(slot + 1, setter.get());
}

}  // namespace bronze
