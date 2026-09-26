// Prototype operations on Object: getPrototypeOf, setPrototypeOf, and Object.create.

#include "runtime/builtin_object.h"

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/integrity.h"
#include "runtime/namespace.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

inline bool isPlainObject(Value v) { return rtObjectIsPlain(v); }

// 20.1.2.21 step 4, in one place: every false [[SetPrototypeOf]] answers —
// a non-extensible object (10.1.2.1 step 4), a chain that would loop (step
// 8.c.ii), a proxy trap that refused (10.5.2 step 9) — reach it, and none of
// them is reported differently for the storage the receiver keeps its
// prototype in.
uint64_t refuseInextensiblePrototype() {
    return rtThrowTypeError("Cannot set the prototype: the object is not extensible, the "
                            "chain would be cyclic, or a proxy trap refused")
        .rawBits();
}

}  // namespace

// The [[Prototype]] of a receiver that keeps no shape — an array and a
// function, whose prototypes are FIXED by 23.1.6.1 and 20.2.3 and so need no
// storage to be known. False for every other such kind, whose prototype is an
// intrinsic bronze builds no object for.
//
// One function because two operations must agree about it: `getPrototypeOf`
// answers with it, and `setPrototypeOf` compares against it to decide whether a
// write changes anything (10.1.2 step 2).
bool rtShapelessPrototypeOf(Value obj, Value& out) {
    if (!obj.isObject()) return false;
    const uint16_t kind = obj.asObject<HeapObjectHeader>()->flags;
    // A SUBCLASS instance stores its [[Prototype]] after all — on the box
    // holding its ordinary half (runtime/native_base.h) — so it is asked
    // FIRST, ahead of the fixed intrinsic answer below. Without this line
    // `Object.getPrototypeOf(new (class extends Array)())` would report
    // %Array.prototype% and skip the subclass's, which is the one link the
    // program can see.
    if (Value proto = rtExoticSubclassPrototype(obj); proto.isObject() || proto.isNull()) {
        out = proto;
        return true;
    }
    if (kind == HeapKind::Array) {
        out = rtArrayPrototypeObject();
        return true;
    }
    if (kind == HeapKind::RegExp) {
        out = rtRegExpPrototypeObject();
        return true;
    }
    if (kind != HeapKind::Function) return false;
    // A DERIVED CLASS stores its own [[Prototype]] — the base constructor,
    // 15.7.14 step 6 — so it is asked first, on the same terms as the exotic
    // subclass above: a stored link is the one the program can see, and the
    // fixed intrinsic below is only what a function that never got one has.
    if (Value stored = obj.asObject<FunctionHeader>()->parent; !stored.isUndefined()) {
        out = stored;
        return true;
    }
    // A generator, an async function and an async generator do not inherit
    // from %Function.prototype% at all: 27.3.4, 27.7.4 and 27.4.4 put each
    // one's own kind prototype in between. Asked before the ordinary answer
    // below, and never for an ordinary function, which answers `undefined`
    // here and falls through.
    if (Value kindProto = rtFunctionKindPrototype(obj); !kindProto.isUndefined()) {
        out = kindProto;
        return true;
    }
    // %Function.prototype% is itself a function object (20.2.3), and its own
    // [[Prototype]] is Object.prototype. Without this line the general rule
    // below would answer with the object being asked about, and a chain walk
    // over it would never terminate.
    out = bronze_strict_eq(obj.rawBits(), rtFunctionPrototypeObject().rawBits())
              ? rtObjectPrototype()
              : rtFunctionPrototypeObject();
    return true;
}

// Step 1 of 20.1.2.12 is ToObject, which is the whole reason a PRIMITIVE has an
// answer here at all: `Object.getPrototypeOf("x")` is String.prototype, and
// only `null` and `undefined` are the TypeError (7.1.18). All four primitive
// kinds with an intrinsic now answer from it, and the wrapper ToObject would
// build is still not built — [[GetPrototypeOf]] of one is the intrinsic
// whatever it wraps, so building it would allocate an object to read a constant
// off it.
uint64_t objectGetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!isPlainObject(args[0])) {
        if (args[0].isNull() || args[0].isUndefined()) {
            return rtThrowTypeError("Object.getPrototypeOf called on " +
                                    std::string(args[0].isNull() ? "null" : "undefined"))
                .rawBits();
        }
        if (args[0].isString()) return rtStringPrototype().rawBits();
        if (args[0].isBool()) return rtBooleanPrototype().rawBits();
        if (args[0].isNumber()) return rtNumberPrototype().rawBits();
        if (args[0].isSymbol()) return rtSymbolPrototype().rawBits();
        // 10.4.6.1 fixes a module namespace's [[Prototype]] at null, and it is
        // immutable — so this is the language's own answer and not the "no
        // prototype object exists" that an array's `null` would have been.
        if (rtIsModuleNamespace(args[0])) return Value::fromNull().rawBits();
        // An array's and a function's [[Prototype]] is not stored anywhere —
        // neither header carries a shape — but it is not unknown either: it is
        // the intrinsic, fixed by 23.1.6.1 and 20.2.3, and bronze builds both
        // objects. So the answer is the intrinsic, and what an array's chain
        // does NOT do (walk through it — an array's members are answered beside
        // the value) is a separate fact, already refused by name where a
        // program could act on it: decorating Array.prototype.
        if (Value proto; rtShapelessPrototypeOf(args[0], proto)) return proto.rawBits();
        if (args[0].isObject() &&
            args[0].asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
            // 10.5.1 [[GetPrototypeOf]]: the `getPrototypeOf` trap, or the
            // target's prototype. A revoked proxy throws here like everywhere.
            return rtProxyGetPrototypeOf(args[0]).rawBits();
        }
        // What is left is a kind whose prototype IS an intrinsic bronze has
        // never built as an object: `RegExp.prototype` is answered by the
        // property path from a C table beside the value, so there is no object
        // to hand back and `null` would be a lie about a chain that works.
        if (args[0].isObject() &&
            args[0].asObject<HeapObjectHeader>()->flags == HeapKind::RegExp) {
            return rtRegExpPrototypeObject().rawBits();
        }
        return rtThrowTypeError(std::string("unsupported: Object.getPrototypeOf of ") +
                                rtObjectKindName(args[0]))
            .rawBits();
    }
    Shape* shape = args[0].asObject<ObjectHeader>()->shape;
    const Value proto = shape ? shape->prototypeValue() : Value::fromUndefined();
    if (proto.isObject() || proto.isNull()) return proto.rawBits();
    // A plain object whose root shape carries `undefined` rather than a
    // prototype: every route to one now names either an object or null, so this
    // is a bronze bug and not a program's doing.
    fatal("internal: a plain object whose root shape names no prototype");
}

// Does `proto` name the prototype this receiver already has? Asked only of the
// kinds that carry no shape, so that a [[SetPrototypeOf]] which changes nothing
// can succeed the way 10.1.2 step 2 says it does, without anything having to be
// stored. False for a kind whose prototype bronze cannot name at all — the
// caller refuses those, which is the honest answer for "I cannot tell".
bool rtSamePrototypeAsCurrent(Value obj, Value proto) {
    Value current;
    if (!rtShapelessPrototypeOf(obj, current)) return false;
    return bronze_strict_eq(proto.rawBits(), current.rawBits());
}

bool rtObjectSetPrototypeOfOrdinary(Rooted<Value>& self, Rooted<Value>& proto) {
    if (!isPlainObject(self.get())) {
        if (self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) {
            // 10.5.2 [[SetPrototypeOf]]: the `setPrototypeOf` trap, or the
            // target's own, checked against the target's extensibility.
            return rtProxySetPrototypeOf(self.get(), proto.get());
        }
        // 10.1.2 OrdinarySetPrototypeOf step 2: a write of the prototype the
        // object ALREADY has succeeds and changes nothing. That is the whole
        // of `Object.setPrototypeOf(arr, Array.prototype)`, a defensive idiom
        // real code writes, and answering it needs no storage — only the
        // getter above, which now knows what an array's and a function's
        // prototype is.
        if (rtSamePrototypeAsCurrent(self.get(), proto.get())) return true;
        if (!rtIsExtensible(self.get())) return false;

        if (self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
            // Check for cycle (10.1.2 step 8)
            Value link = proto.get();
            for (uint32_t depth = 0; link.isObject() && depth <= 1000; ++depth) {
                if (link.rawBits() == self.get().rawBits()) return false;
                if (link.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
                    Value next = link.asObject<FunctionHeader>()->parent;
                    link = next.isUndefined() ? rtFunctionPrototypeObject() : next;
                } else if (isPlainObject(link)) {
                    Shape* shape = link.asObject<ObjectHeader>()->shape;
                    link = shape ? shape->prototypeValue() : Value::fromNull();
                } else {
                    break;
                }
            }

            self.get().asObject<FunctionHeader>()->parent = proto.get();

            // Link statics/properties prototype chain so derived constructor inherits base statics
            rtEnsureFunctionProperties(self);
            Rooted<Value> selfProps{self.get().asObject<FunctionHeader>()->properties};

            if (proto.get().isObject() &&
                proto.get().asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
                rtEnsureFunctionProperties(proto);
                Rooted<Value> protoProps{proto.get().asObject<FunctionHeader>()->properties};
                Shape* newRoot = rtRootShapeForPrototype(protoProps.get());
                ObjectHeader::setPrototype(rtArena(), selfProps, newRoot);
            } else if (proto.get().isObject() || proto.get().isNull()) {
                Shape* newRoot = rtRootShapeForPrototype(proto.get());
                ObjectHeader::setPrototype(rtArena(), selfProps, newRoot);
            }
            return true;
        }

        if (self.get().asObject<HeapObjectHeader>()->flags == HeapKind::Array) {
            // Check for cycle (10.1.2 step 8)
            Value link = proto.get();
            for (uint32_t depth = 0; link.isObject() && depth <= 1000; ++depth) {
                if (link.rawBits() == self.get().rawBits()) return false;
                Value next;
                if (rtShapelessPrototypeOf(link, next)) {
                    link = next;
                } else if (isPlainObject(link)) {
                    Shape* shape = link.asObject<ObjectHeader>()->shape;
                    link = shape ? shape->prototypeValue() : Value::fromNull();
                } else {
                    break;
                }
            }

            if (proto.get().isObject()) {
                if (ObjectHeader* protoObj = proto.get().asObject<ObjectHeader>(); protoObj->shape) {
                    protoObj->shape->used_as_prototype = true;
                }
            }

            Shape* newRoot = rtRootShapeForPrototype(proto.get());
            if (!self.get().asObject<ArrayHeader>()->properties.isObject()) {
                ObjectHeader* box = ObjectHeader::create(rtHeap(), rtArena(), newRoot);
                box->header.flags = HeapKind::Plain;
                self.get().asObject<ArrayHeader>()->properties = Value::fromObject(box);
            } else {
                Rooted<Value> props{self.get().asObject<ArrayHeader>()->properties};
                ObjectHeader::setPrototype(rtArena(), props, newRoot);
            }
            self.get().asObject<ArrayHeader>()->reserved |= ArrayHeader::kHasCustomPrototype;
            return true;
        }

        return false;
    }
    // 10.1.2.1 OrdinarySetPrototypeOf steps 2 to 4, in that order. Step 2's
    // SameValue comes FIRST, so a write of the prototype the object already has
    // succeeds on a non-extensible object too — it stores nothing, and there is
    // nothing for [[Extensible]] to forbid. Any other write needs the object to
    // be extensible, and step 4's `false` is what 20.1.2.21 step 4 turns into a
    // TypeError. Without this a `preventExtensions`'d object's chain could be
    // replaced outright: the write landed, and `Object.isExtensible` went on
    // reporting false about an object whose prototype had just moved.
    {
        const uint64_t call[1] = {self.get().rawBits()};
        const Value current(objectGetPrototypeOf(0, 0, 1, call));
        if (bronze_strict_eq(proto.get().rawBits(), current.rawBits())) return true;
    }
    if (!rtIsExtensible(self.get())) return false;
    // Step 8: a chain that would loop back to the object is refused, walked
    // over ordinary links only — step 8.c.i stops the walk at a link whose
    // [[GetPrototypeOf]] is not the ordinary one, a proxy being the case.
    {
        Value link = proto.get();
        for (uint32_t depth = 0; link.isObject() && depth <= 1000; ++depth) {
            if (link.rawBits() == self.get().rawBits()) return false;
            if (!isPlainObject(link)) break;
            Shape* shape = link.asObject<ObjectHeader>()->shape;
            link = shape ? shape->prototypeValue() : Value::fromNull();
        }
    }
    Shape* newRoot = rtRootShapeForPrototype(proto.get());
    ObjectHeader::setPrototype(rtArena(), self, newRoot);
    return true;
}

// 20.1.2.21 Object.setPrototypeOf. Returns the object, so it composes.
uint64_t objectSetPrototypeOf(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!args[1].isObject() && !args[1].isNull()) {
        return rtThrowTypeError("Object prototype may only be an Object or null").rawBits();
    }
    if (args[0].isNull() || args[0].isUndefined()) {
        return rtThrowTypeError("Object.setPrototypeOf called on null or undefined").rawBits();
    }
    if (!args[0].isObject()) return args[0].rawBits();  // 20.1.2.21 step 3
    Rooted<Value> self{args[0]};
    Rooted<Value> proto{args[1]};
    const bool ok = rtObjectSetPrototypeOfOrdinary(self, proto);
    if (!ok) return refuseInextensiblePrototype();
    return self.get().rawBits();
}

// 20.1.2.2 Object.create. `null` really means no prototype, and every walk
// over a prototype chain already stops at a shape whose prototype is not an
// object — so an object with no prototype needs no special case anywhere,
// which is the point of putting the prototype on the shape.
uint64_t objectCreate(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!args[0].isObject() && !args[0].isNull()) {
        return rtThrowTypeError("Object prototype may only be an Object or null").rawBits();
    }
    // A PROXY may be the prototype: every walk that misses stops at the link
    // and hands the rest of the operation to the proxy's internal method with
    // the receiver it had (object.cpp's read and write walks, rt_operator.cpp's
    // `in`, rt_enumerate.cpp's for-in), which is what 10.1.8.1 step 3 and its
    // siblings say happens at a link whose [[Get]] is not the ordinary one.
    if (args[0].isObject() && !isPlainObject(args[0]) &&
        args[0].asObject<HeapObjectHeader>()->flags != HeapKind::Proxy) {
        return rtThrowTypeError(
                   std::string("Object.create with ") + rtObjectKindName(args[0]) +
                   " as the prototype is unsupported (a prototype is walked by every read "
                   "that misses, and this kind answers its members from a table beside the "
                   "value rather than from a shape a walk can step through; only a plain "
                   "object may be one)")
            .rawBits();
    }
    Rooted<Value> proto{args[0]};
    Rooted<Value> out{Value::fromObject(
        ObjectHeader::create(rtHeap(), rtArena(), rtRootShapeForPrototype(proto.get())))};
    out.get().asObject<ObjectHeader>()->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
    if (!args[1].isUndefined()) {
        Rooted<Value> descriptors{args[1]};
        rtObjectDefineFromDescriptors(out, descriptors);
    }
    return out.get().rawBits();
}

}  // namespace bronze::runtime

extern "C" uint64_t bronze_get_prototype_of(uint64_t objBits) {
    uint64_t argv[1] = {objBits};
    return bronze::runtime::objectGetPrototypeOf(0, 0, 1, argv);
}
