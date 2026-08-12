// `Object.prototype` (ECMA-262 20.1.3) — the intrinsic every plain object
// inherits from, and the four members bronze answers on it.
//
// It is a real object on the real chain, found by the ordinary prototype walk —
// not a table consulted beside it, which is what every other builtin receiver
// in bronze still is. The difference is the whole point: a method here can be
// held, compared, passed to `.call`, and replaced, and `Object.getPrototypeOf({})`
// has something true to answer.
//
// Its own translation unit, and the seam is the receiver. Every function in
// builtin_object.cpp takes its subject as an ARGUMENT — `Object.keys(o)` is a
// static that could have been a free function — while every function here takes
// it as `this`, off a chain a program can reach and modify. That is why the two
// disagree about what a bad receiver means: a static raises the TypeError its
// clause names, and a method here can only have been reached THROUGH a
// receiver, so a kind bronze cannot walk is a refusal rather than a throw.
//
// The two files still build one pair of intrinsics, because 20.1.2.1 and
// 20.1.3.1 make the namespace and the prototype each other's property — so
// `ensureObjectIntrinsics` stays in builtin_object.cpp and reaches the table
// below through `rtInstallObjectProtoMethods`, exactly as `String.prototype`'s
// members reach the object builtin_wrappers.cpp allocates.
//
// Every member is defined NON-ENUMERABLE, per 20.1.3. That is not tidiness:
// `for-in` walks the prototype chain, so an enumerable member here would appear
// in every for-in over every object in the program. `Object.keys`, spread and
// `JSON.stringify` ask for own enumerable keys and so cannot see it either,
// which is why this object could be introduced under a suite of pinned
// expectations without moving one of them.

#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isPlainObject(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN;
}

// The receiver of an Object.prototype method, which reaches one only through an
// ordinary call on a plain object or through `.call`. Three answers, and the
// third is the house rule: a receiver bronze cannot answer for is refused BY
// NAME rather than told that it has no own properties.
bool requireProtoReceiver(Value self, const char* method) {
    if (self.isNull() || self.isUndefined()) {
        rtThrowTypeError(std::string("Object.prototype.") + method +
                         " called on null or undefined");
        return false;
    }
    if (isPlainObject(self)) return true;
    fatal((std::string("unsupported: Object.prototype.") + method +
           " on a receiver that is not a plain object (an array, a function, a Map or a "
           "primitive reaches its members through the property path rather than through a "
           "prototype object, so bronze has no chain here to answer about)")
              .c_str());
}

uint64_t objectProtoHasOwnProperty(uint64_t, uint64_t thisBits, uint32_t argc,
                                   const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireProtoReceiver(self.get(), "hasOwnProperty")) {
        return Value::fromUndefined().rawBits();
    }
    return Value::fromBool(rtHasOwnPropertyNamed(self, args[0])).rawBits();
}

// 20.1.3.4. Own AND enumerable — a name that is only inherited answers false
// here where `in` answers true, and a class method (15.7.14 defines it
// non-enumerable) answers false where `hasOwnProperty` answers true.
uint64_t objectProtoPropertyIsEnumerable(uint64_t, uint64_t thisBits, uint32_t argc,
                                         const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireProtoReceiver(self.get(), "propertyIsEnumerable")) {
        return Value::fromUndefined().rawBits();
    }
    rtCheckStringExoticOwnKeys(self.get(), "testing");
    PropertyKey name = rtInternPropertyKey(args[0]);
    auto* obj = self.get().asObject<ObjectHeader>();
    PropertyInfo info;
    if (!obj->shape || !obj->shape->lookupProperty(name, info)) {
        return Value::fromBool(false).rawBits();
    }
    return Value::fromBool(info.enumerable).rawBits();
}

// 20.1.3.3. Walks the ARGUMENT's chain looking for the receiver, so it answers
// about ancestry rather than about identity: an object is not its own
// prototype, and the walk starts one link up for that reason.
uint64_t objectProtoIsPrototypeOf(uint64_t, uint64_t thisBits, uint32_t argc,
                                  const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireProtoReceiver(self.get(), "isPrototypeOf")) {
        return Value::fromUndefined().rawBits();
    }
    if (!isPlainObject(args[0])) return Value::fromBool(false).rawBits();
    // No allocation in the loop, so the raw pointers stay valid throughout.
    ObjectHeader* walker = args[0].asObject<ObjectHeader>();
    ObjectHeader* target = self.get().asObject<ObjectHeader>();
    for (uint32_t depth = 0; depth < ObjectHeader::kMaxPrototypeDepth; ++depth) {
        walker = walker->protoAncestor(1);
        if (!walker) return Value::fromBool(false).rawBits();
        if (walker == target) return Value::fromBool(true).rawBits();
    }
    fatal("prototype chain too deep (a cycle?)");
}

// 20.1.3.7 ToObject(this), which for an object is the object. It exists so that
// the name is not a hole in the chain; it is NOT what makes `{} + 1` work,
// because ToPrimitive is what calls valueOf and ToPrimitive is still unbuilt
// (rt_convert.cpp names it).
uint64_t objectProtoValueOf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!requireProtoReceiver(self, "valueOf")) return Value::fromUndefined().rawBits();
    return self.rawBits();
}

const NativeMethod kObjectProtoMethods[] = {
    {"hasOwnProperty", objectProtoHasOwnProperty, 1},
    {"isPrototypeOf", objectProtoIsPrototypeOf, 1},
    {"propertyIsEnumerable", objectProtoPropertyIsEnumerable, 1},
    {"valueOf", objectProtoValueOf, 0},
};

// 20.1.3 members bronze has not built, diagnosed by name on a plain object's
// full-chain miss.
//
// `toString` is deliberately here rather than answered with "[object Object]".
// 20.1.3.6 is a tag lookup — Array, Function, Error, Arguments, each of the
// wrapper kinds — and bronze cannot ask the question for all of them: an error
// object here is an ordinary plain object with no [[ErrorData]] to find, so a
// toString written today would answer "[object Object]" for one and be
// confidently wrong at exactly the place `Object.prototype.toString.call(x)` is
// used. `toLocaleString` is 20.1.3.5, which calls toString.
const char* const kObjectProtoUnimplemented[] = {
    "toLocaleString",
    "toString",
};

}  // namespace

void rtInstallObjectProtoMethods(Rooted<Value>& proto) {
    // `rtDefineMethods` is a DefineOwnProperty with `enumerable: false`, which
    // is what 20.1.3 says every one of these is — and not an assignment, so a
    // member here cannot be swallowed by a setter anything installed first.
    rtDefineMethods(proto, kObjectProtoMethods, std::size(kObjectProtoMethods));
}

void rtObjectProtoCheckMissingMember(const std::string& key) {
    rtCheckUnimplementedMember("Object.prototype", kObjectProtoUnimplemented,
                               std::size(kObjectProtoUnimplemented), key);
}

}  // namespace bronze::runtime
