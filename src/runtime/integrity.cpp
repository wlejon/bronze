// SetIntegrityLevel and TestIntegrityLevel (ECMA-262 7.3.14, 7.3.15), and the
// six `Object` members that are them.
//
// The operation is one algorithm over "the object's own properties", and every
// receiver kind bronze has stores those differently — a plain object in its
// shape and slots, an array in its element block, a function in a side object
// and a `prototype` slot. So the file is organised by that question and not by
// the six entry points: `targetOf` decides which storage is involved,
// `setIntegrity` stamps it, `testIntegrity` reads it back, and the six builtins
// are three pairs of thin wrappers over those.
//
// The house rule that shapes all of it: a kind whose level bronze cannot record
// is refused BY NAME. It is never quietly left alone, and — the part that
// matters more — the predicates are never allowed to report a state the object
// is not in. `Object.isFrozen` answering true for an array `Object.freeze` had
// silently skipped was a self-consistent lie: a program that checks before it
// writes was told the object was protected and the write still landed.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/dictionary.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/integrity.h"
#include "runtime/iterator.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/namespace.h"
#include "runtime/regexp.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// Which of the three storage stories a receiver tells. `Refused` is a real
// answer and not an error case: it is the set of kinds that have nowhere to
// keep a level, and naming it here is what keeps every one of the six entry
// points from inventing its own opinion about a Map.
enum class Target {
    NotAnObject,  // 20.1.2.6 step 1 / 7.3.15 step 1: nothing to do, vacuously frozen
    Plain,
    Array,
    Function,
    // A module namespace: the one kind with no storage that is not refused,
    // because it needs none. 10.4.6.3 [[IsExtensible]] returns false for every
    // namespace and 10.4.6.5 makes every export non-configurable, so its
    // integrity state is a fact about the KIND and there is nothing a
    // dictionary would have to record.
    ModuleNamespace,
    Refused,
};

Target targetOf(Value v) {
    if (!v.isObject()) return Target::NotAnObject;
    switch (v.asObject<HeapObjectHeader>()->flags) {
        case BRONZE_ABI_OBJ_FLAGS_PLAIN: return Target::Plain;
        case HeapKind::Array: return Target::Array;
        case HeapKind::Function: return Target::Function;
        case ModuleNamespaceHeader::kFlags: return Target::ModuleNamespace;
        default: return Target::Refused;
    }
}

// How many names a namespace exports, which is the only thing about one that
// varies — and it decides exactly one answer, `isFrozen`.
uint32_t namespaceExportCount(Value v) { return v.asObject<ModuleNamespaceHeader>()->count; }

// The receiver's kind for a diagnostic. The three targets above never reach
// this, so the shared namer's answers for them are unused here — what this adds
// is the one kind that is not a JS value at all.
const char* refusedKindName(Value v) {
    if (v.asObject<HeapObjectHeader>()->flags == IterRecordHeader::kFlags) {
        // Nothing hands a program one, so reaching this is a lowering bug
        // rather than something a program did.
        fatal("internal: an integrity operation on an iteration record");
    }
    return rtObjectKindName(v);
}

// What every refused kind has in common, said once: it has no side object, so
// there is nowhere for [[Extensible]] to go, and a bit invented for it would
// have to be read back by every write path that kind has.
[[noreturn]] void refuseKind(Value v, const char* operation) {
    fatal((std::string("unsupported: Object.") + operation + " on " + refusedKindName(v) +
           " (it keeps no property table, so bronze has nowhere to record "
           "[[Extensible]] — and a level nothing could read back would be a "
           "no-op reported as a success)")
              .c_str());
}

// The plain object a receiver keeps its own NAMED properties in, created on
// demand and moved to dictionary mode, which is where the level is recorded.
// Only called for the three targets that have one.
ObjectHeader* integrityTableOwner(Rooted<Value>& self, Target target) {
    switch (target) {
        case Target::Plain:
            ObjectHeader::toDictionary(rtArena(), self);
            return self.get().asObject<ObjectHeader>();
        case Target::Array: {
            ArrayHeader::ensureProperties(rtHeap(), rtArena(), self);
            Rooted<Value> props{self.get().asObject<ArrayHeader>()->properties};
            ObjectHeader::toDictionary(rtArena(), props);
            return props.get().asObject<ObjectHeader>();
        }
        case Target::Function: {
            rtEnsureFunctionProperties(self);
            Rooted<Value> props{self.get().asObject<FunctionHeader>()->properties};
            ObjectHeader::toDictionary(rtArena(), props);
            return props.get().asObject<ObjectHeader>();
        }
        default:
            fatal("internal: an integrity table asked of a receiver that has none");
    }
}

// Does this array still have an own index property? A hole is not one, so an
// array of nothing but holes has only `length` — and `length` is
// non-configurable from birth, which is why an empty array is SEALED the moment
// it stops being extensible (10.4.2 ArrayCreate).
bool hasAnyElement(const ArrayHeader* arr) {
    for (uint32_t i = 0; i < arr->length; ++i) {
        if (arr->hasElem(i)) return true;
    }
    return false;
}

// 7.3.14 steps 4 and 5 over the properties a dictionary DOES list. An accessor
// keeps its halves — there is no `writable` on one — and only stops being
// configurable, which is 10.1.6.3 read literally rather than "make everything
// read-only".
void stampEntries(Dictionary& d, bool frozen) {
    for (DictEntry& e : d.entries) {
        if (frozen && !e.accessor) e.writable = false;
        e.configurable = false;
    }
}

// 20.1.2.6 / 20.1.2.20 / 20.1.2.19, once. `want` is the level the operation
// asks for; the recorded level only ever RISES, because seal after freeze must
// not unfreeze and every one of these three is defined to remove capabilities.
uint64_t setIntegrity(Value receiver, IntegrityLevel want, const char* operation) {
    const Target target = targetOf(receiver);
    // 20.1.2.6 step 1 returns a non-object unchanged rather than throwing.
    if (target == Target::NotAnObject) return receiver.rawBits();
    if (target == Target::ModuleNamespace) {
        // 7.3.14 on a namespace, and every step of it has an answer here.
        // [[PreventExtensions]] (10.4.6.4) returns true with nothing to do,
        // because [[IsExtensible]] was already the constant false. Then step 5
        // defines each own key — `configurable: false` for `seal`, and
        // `configurable: false, writable: false` for `freeze`. 10.4.6.6
        // [[DefineOwnProperty]] accepts a descriptor only when it MATCHES what
        // 10.4.6.5 reports, and that says `writable: true`.
        //
        // So `seal` and `preventExtensions` succeed as no-ops, and `freeze`
        // fails — as a catchable TypeError from step 5.b.i's
        // DefinePropertyOrThrow, which is the language's own answer and not
        // bronze's "nowhere to record a level". A namespace with no exports has
        // no key for that step to reach, so it freezes vacuously.
        if (want == IntegrityLevel::Frozen && namespaceExportCount(receiver) > 0) {
            return rtThrowTypeError(
                       std::string("Cannot ") + operation +
                       " a module namespace object that has exports: 10.4.6.5 reports every "
                       "export as writable, and 10.4.6.6 accepts no descriptor that "
                       "disagrees")
                .rawBits();
        }
        return receiver.rawBits();
    }
    if (target == Target::Refused) {
        // A typed array is the one refused kind with a SPECIFIED answer rather
        // than a bronze gap: 10.4.5.3 [[DefineOwnProperty]] refuses any
        // descriptor that asks an integer-indexed element to stop being
        // configurable or writable, so 7.3.14's DefinePropertyOrThrow throws
        // for both `seal` and `freeze` the moment the view has one element.
        // That is the language's answer, and bronze can give it exactly.
        if (want != IntegrityLevel::Open &&
            receiver.asObject<HeapObjectHeader>()->flags == TypedArrayHeader::kFlags &&
            receiver.asObject<TypedArrayHeader>()->length > 0) {
            return rtThrowTypeError(
                       std::string("Cannot ") + operation +
                       " a typed array that has elements: an integer-indexed property cannot "
                       "be made non-configurable or non-writable")
                .rawBits();
        }
        refuseKind(receiver, operation);
    }

    Rooted<Value> self{receiver};
    ObjectHeader* owner = integrityTableOwner(self, target);
    Dictionary& d = *owner->shape->dict;
    // `preventExtensions` is [[PreventExtensions]] alone: 7.3.14's steps 4 and
    // 5 belong to `seal` and `freeze`, and running them here would take
    // `configurable` off every existing property — which is precisely the
    // difference `cases/object_prototype_statics` pins by deleting one
    // afterwards.
    if (want != IntegrityLevel::Open) stampEntries(d, want == IntegrityLevel::Frozen);
    d.extensible = false;
    if (static_cast<uint8_t>(want) > static_cast<uint8_t>(d.level)) d.level = want;
    return self.get().rawBits();
}

// 7.3.15 TestIntegrityLevel. `frozen` selects between the two questions; the
// walk is one because the sealed test is the frozen one minus the writability
// half, and writing it twice is how the two would come to disagree about an
// accessor.
bool testIntegrity(Value receiver, bool frozen) {
    const Target target = targetOf(receiver);
    if (target == Target::NotAnObject) return true;  // 7.3.15 step 1: vacuously
    if (target == Target::ModuleNamespace) {
        // 7.3.15 over 10.4.6, and nothing here is read off the object. Step 3's
        // [[IsExtensible]] is the constant false (10.4.6.3) and every own
        // property is `configurable: false` (10.4.6.5) — so a namespace is
        // SEALED from birth, with nothing having been done to it. FROZEN wants
        // every data property non-writable as well, and 10.4.6.5 says
        // `writable: true`, so the only frozen namespace is one with no export
        // to be writable. That is the same split `Object.freeze` above makes,
        // and it is why the two cannot be written independently.
        return !frozen || namespaceExportCount(receiver) == 0;
    }
    // Nothing can have made one of these non-extensible — every route is the
    // hard error above — so `false` here is the correct answer and not a guess.
    if (target == Target::Refused) return false;

    const Dictionary* d = rtIntegrityTable(receiver);
    if (!d || d->extensible) return false;  // step 3

    if (target == Target::Array) {
        const auto* arr = receiver.asObject<ArrayHeader>();
        // `length` is an own property of every array: non-configurable from
        // birth and writable until `freeze` (10.4.2), so it alone decides the
        // frozen answer and never affects the sealed one.
        if (frozen && d->level != IntegrityLevel::Frozen) return false;
        // The elements. Non-configurable from `Sealed` up, so an array that is
        // merely non-extensible fails the sealed test as soon as it has one.
        if (!frozen && d->level == IntegrityLevel::Open && hasAnyElement(arr)) return false;
    } else if (target == Target::Function) {
        // `prototype` is the function's `length`: non-configurable and writable
        // (10.2.4), so `freeze` is the only thing that settles it. bronze
        // materialises the object lazily, which is invisible here — the
        // PROPERTY is there either way, and its attributes are what this asks.
        if (frozen && d->level != IntegrityLevel::Frozen) return false;
    }

    for (const DictEntry& e : d->entries) {
        if (e.configurable) return false;
        if (frozen && !e.accessor && e.writable) return false;
    }
    return true;
}

}  // namespace

// Every heap kind, named as a program would say it. It lives here because this
// file already had the switch and every include it needs; what made it worth
// exporting is that a diagnostic which REFUSES a receiver has to say what the
// receiver IS, and more than one file had grown its own partial answer to that.
const char* rtObjectKindName(Value v) {
    if (!v.isObject()) fatal("internal: asked the heap kind of a value that is not an object");
    switch (v.asObject<HeapObjectHeader>()->flags) {
        case BRONZE_ABI_OBJ_FLAGS_PLAIN: return "a plain object";
        case HeapKind::Array: return "an array";
        case HeapKind::Function: return "a function";
        case MapHeader::kMapFlags: return "a Map";
        case MapHeader::kSetFlags: return "a Set";
        case TypedArrayHeader::kFlags: return "a typed array";
        case ArrayBufferHeader::kFlags: return "an ArrayBuffer";
        case DataViewHeader::kFlags: return "a DataView";
        case RegExpHeader::kFlags: return "a RegExp";
        case ModuleNamespaceHeader::kFlags: return "a module namespace object";
        default: return "this object";
    }
}

Dictionary* rtIntegrityTable(Value obj) {
    ObjectHeader* owner = nullptr;
    switch (targetOf(obj)) {
        case Target::Plain:
            owner = obj.asObject<ObjectHeader>();
            break;
        case Target::Array: {
            Value props = obj.asObject<ArrayHeader>()->properties;
            if (props.isObject()) owner = props.asObject<ObjectHeader>();
            break;
        }
        case Target::Function: {
            Value props = obj.asObject<FunctionHeader>()->properties;
            if (props.isObject()) owner = props.asObject<ObjectHeader>();
            break;
        }
        default:
            return nullptr;
    }
    if (!owner || !owner->shape || !owner->shape->isDictionary()) return nullptr;
    return owner->shape->dict;
}

SetRefusal rtArrayElementWriteRefusalSlow(Value arrVal, uint32_t index) {
    const Dictionary* d = rtIntegrityTable(arrVal);
    if (!d) return SetRefusal::None;  // the ordinary array: open and extensible
    const bool creating = !arrVal.asObject<ArrayHeader>()->hasElem(index);
    if (creating) {
        return d->extensible ? SetRefusal::None : SetRefusal::NotExtensible;
    }
    return d->level == IntegrityLevel::Frozen ? SetRefusal::NotWritable : SetRefusal::None;
}

bool rtArrayElementsConfigurable(Value arrVal) {
    return rtIntegrityLevel(arrVal) == IntegrityLevel::Open;
}

bool rtFunctionPrototypeWritable(Value fnVal) {
    return rtIntegrityLevel(fnVal) != IntegrityLevel::Frozen;
}

uint64_t rtObjectFreeze(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return setIntegrity(args[0], IntegrityLevel::Frozen, "freeze");
}

uint64_t rtObjectSeal(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return setIntegrity(args[0], IntegrityLevel::Sealed, "seal");
}

// 20.1.2.19. `preventExtensions` asks for the extensibility bit alone, so the
// level it wants is `Open`: a sealed object that is prevented again must stay
// sealed, which the monotone rule in setIntegrity already says.
uint64_t rtObjectPreventExtensions(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return setIntegrity(args[0], IntegrityLevel::Open, "preventExtensions");
}

uint64_t rtObjectIsFrozen(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return Value::fromBool(testIntegrity(args[0], /*frozen=*/true)).rawBits();
}

uint64_t rtObjectIsSealed(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return Value::fromBool(testIntegrity(args[0], /*frozen=*/false)).rawBits();
}

// 20.1.2.16. Step 1 answers `false` for a non-object — the only one of the six
// whose primitive case is not the vacuous `true`, because a primitive cannot
// have properties added to it.
uint64_t rtObjectIsExtensible(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    const Target target = targetOf(args[0]);
    if (target == Target::NotAnObject) return Value::fromBool(false).rawBits();
    // 10.4.6.3 [[IsExtensible]] "return false" — no steps, no slot, no
    // condition. A namespace is the one object whose answer is fixed by its
    // kind, which is exactly why it needs no place to record one.
    if (target == Target::ModuleNamespace) return Value::fromBool(false).rawBits();
    if (target == Target::Refused) return Value::fromBool(true).rawBits();
    return Value::fromBool(rtIsExtensible(args[0])).rawBits();
}

}  // namespace bronze::runtime
