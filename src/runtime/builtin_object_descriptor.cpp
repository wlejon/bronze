// The property descriptor as a reified object: the round trip between it and
// bronze's internal form.
//
// 6.2.6.4 FromPropertyDescriptor turns what an object actually stores — a shape
// slot, and the `writable` and `configurable` bits a dictionary entry carries —
// into an ordinary object a program can hold; 6.2.6.5 ToPropertyDescriptor
// reads one back. `getOwnPropertyDescriptor` and `getOwnPropertyDescriptors`
// are the first direction and live here with the decode; `defineProperty` and
// `defineProperties` are the second, and the APPLY they are defined over —
// 10.1.6.3 and 10.4.2.1 over the decoded form — is builtin_object_define.cpp,
// reached through builtin_object_descriptor_internal.h. That round trip is what
// makes them one subject rather than four members that happened to be next to
// each other.
//
// The FIELD ORDER of the object built here is the specification's and not a
// convenience: `Object.keys(descriptor)` prints it, so it is pinned bytes.
#include "runtime/builtin_object.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_object_descriptor_internal.h"
#include "runtime/dictionary.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/map.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// ---- reading a descriptor's six fields --------------------------------------
//
// 6.2.6.5 ToPropertyDescriptor asks HasProperty and then Get, once for each of
// six field names. A constructor decodes one descriptor per object it makes —
// three.js gives every `Object3D` six through `Object.defineProperties` and a
// seventh through `defineProperty` — so the decode is not a cold path, and
// spelled as the two generic calls it was mostly string work: `rtMakeString`
// built the field's name, `bronze_has_property` converted it back to text and
// built a SECOND heap string from that, walked, and `getProp` walked again.
// Two heap strings, a `std::string` and two chain walks per field, before the
// descriptor's own contents were looked at.
//
// The names are six constants. They go through the key registry, which interns
// by text and hands back the immortal arena string it already holds — so the
// decode allocates nothing, and the two questions are answered in ONE walk,
// because for an ordinary descriptor both answers are the slot the walk finds.
// A shape key is matched by content (`PropertyKey::matches`), so what the walk
// spends per link is a length test and a memcmp of at most twelve bytes.

// The six fields, in the order 6.2.6.5 reads them. That order is OBSERVABLE —
// a descriptor may spell a field as a getter, and one getter may add or change
// a field the decode has not reached yet — so it is the specification's and not
// a convenience. `kDescFieldNames` is indexed by this enum.
enum class DescField : uint8_t { Enumerable, Configurable, Value, Writable, Get, Set };
constexpr size_t kDescFieldCount = 6;
constexpr const char* kDescFieldNames[kDescFieldCount] = {"enumerable", "configurable", "value",
                                                          "writable",   "get",          "set"};

// BRONZE_NO_DESC_FIELDS=1 puts the decode back on a freshly built name and the
// pair of generic calls, so one binary A/Bs the whole of the above.
bool descFieldFastPath() {
    static const bool enabled = []() {
        const char* env = std::getenv("BRONZE_NO_DESC_FIELDS");
        return !(env && std::strcmp(env, "1") == 0);
    }();
    return enabled;
}

// The field's name as the immortal arena string the key registry holds for it.
// The registry is per-thread and so is this memo of it; an arena string never
// moves and is never freed, so holding the header is the same promise
// `rtKeyHeader` already makes to the property path.
PropertyKey descFieldKey(DescField field) {
    static thread_local StringHeader* memo[kDescFieldCount] = {};
    const size_t i = static_cast<size_t>(field);
    if (memo[i] == nullptr) {
        memo[i] = rtKeyHeader(bronze_register_key_string(kDescFieldNames[i]));
    }
    return PropertyKey::forString(memo[i]);
}

// What one walk of the descriptor's chain could answer.
enum class FieldFound : uint8_t { Present, Absent, Generic };

// HasProperty and Get over `desc`'s prototype chain, in one walk.
//
// The walk is `plainObjectHas`'s (rt_operator.cpp) step for step, because that
// is the walk `bronze_has_property` reaches for a plain receiver and the
// presence answer has to be the same answer. Where the walk cannot also produce
// what Get would return — an ACCESSOR, whose Get runs user code, or a holder
// that is not an ordinary object, whose value does not come out of a slot — it
// answers `Generic` and the caller runs the two generic calls as before.
FieldFound lookupField(Value descVal, PropertyKey name, Value& out) {
    auto* hdr = descVal.asObject<HeapObjectHeader>();
    for (uint32_t depth = 0; depth <= 1000; ++depth) {
        auto* holder = reinterpret_cast<ObjectHeader*>(hdr);
        PropertyInfo info;
        if (holder->shape != nullptr && holder->shape->lookupProperty(name, info)) {
            if (info.accessor || !HeapKind::carriesShape(hdr->flags)) return FieldFound::Generic;
            out = holder->getSlot(info.slot);
            return FieldFound::Present;
        }
        ObjectHeader* next = holder->protoAncestor(1);
        if (next == nullptr) return FieldFound::Absent;
        hdr = reinterpret_cast<HeapObjectHeader*>(next);
    }
    fatal("prototype chain too deep (a cycle?)");
}

// `ordinary` is false for a String wrapper, which answers `length` and its
// indices beside its shape and so is not fully described by the walk above.
// None of the six names is such a key, but that is a fact about the RECEIVER
// and is asked once per descriptor rather than assumed six times.
Value readField(Rooted<Value>& desc, DescField field, bool ordinary, bool& present) {
    // The key is taken BEFORE the descriptor is read: interning one the first
    // time a thread asks for it allocates, and an argument list gives no order
    // to evaluate the two in — so a raw `desc.get()` beside the call could be
    // the address the collector had just moved away from.
    const PropertyKey name = descFieldKey(field);
    if (ordinary && descFieldFastPath()) {
        Value out;
        switch (lookupField(desc.get(), name, out)) {
            case FieldFound::Present:
                present = true;
                return out;
            case FieldFound::Absent:
                present = false;
                return Value::fromUndefined();
            case FieldFound::Generic:
                break;
        }
    }
    Rooted<Value> key{descFieldFastPath()
                          ? name.toValue()
                          : rtMakeString(kDescFieldNames[static_cast<size_t>(field)])};
    present = bronze_has_property(key.get().rawBits(), desc.get().rawBits());
    if (!present) return Value::fromUndefined();
    return desc.get().asObject<ObjectHeader>()->getProp(rtHeap(), key);
}

}  // namespace

namespace descriptor_internal {

void putField(Rooted<Value>& obj, const char* name, Rooted<Value>& val) {
    Rooted<Value> key{rtMakeString(name)};
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
}

void putField(Rooted<Value>& obj, Rooted<Value>& key, Rooted<Value>& val) {
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val);
}

bool decodeDescriptor(Rooted<Value>& desc, DecodedDescriptor& d, Rooted<Value>& value,
                      Rooted<Value>& getter, Rooted<Value>& setter) {
    if (!rtObjectIsPlain(desc.get())) {
        rtThrowTypeError("Property description must be an object");
        return false;
    }
    Value wrapped;
    const bool ordinary = !rtStringWrapperData(desc.get(), wrapped);
    // 6.2.6.5 steps 3 through 8, in the order it states them. The order is
    // observable whenever a field is spelled as a getter: one such getter can
    // see which fields have already been asked for, and can add a field the
    // decode has not reached yet, so reading `value` before `enumerable` is a
    // different program than reading it after.
    //
    // Every one of the six is spelled `? HasProperty(...)` / `? Get(...)`. A
    // field getter that throws — or a Proxy descriptor's `has` trap — is an
    // ABRUPT COMPLETION: 6.2.6.5 returns it, so the remaining fields are never
    // read (their getters must not run) and 10.1.6.3 never runs at all. The
    // throw leaves `readField` and this function with it, which is exactly
    // that.
    Rooted<Value> enumerableV{readField(desc, DescField::Enumerable, ordinary, d.hasEnumerable)};
    Rooted<Value> configurableV{
        readField(desc, DescField::Configurable, ordinary, d.hasConfigurable)};
    value.set(readField(desc, DescField::Value, ordinary, d.hasValue));
    Rooted<Value> writableV{readField(desc, DescField::Writable, ordinary, d.hasWritable)};
    getter.set(readField(desc, DescField::Get, ordinary, d.hasGet));
    setter.set(readField(desc, DescField::Set, ordinary, d.hasSet));

    // 6.2.6.5 steps 7.c and 8.c: a `get` or `set` that is PRESENT and is
    // neither callable nor `undefined` does not describe an accessor, so the
    // descriptor is rejected before anything is defined. This is an error of
    // the DECODE and not a refusal — `Reflect.defineProperty` raises it too.
    if (d.hasGet && !getter.get().isUndefined() && !rtIsCallableValue(getter.get())) {
        rtThrowTypeError("Getter must be a function");
        return false;
    }
    if (d.hasSet && !setter.get().isUndefined() && !rtIsCallableValue(setter.get())) {
        rtThrowTypeError("Setter must be a function");
        return false;
    }

    if ((d.hasGet || d.hasSet) && (d.hasValue || d.hasWritable)) {
        rtThrowTypeError(
            "Invalid property descriptor. Cannot both specify accessors and a value or "
            "writable attribute");
        return false;
    }
    d.wantWritable = d.hasWritable && bronze_truthy(writableV.get().rawBits());
    d.wantEnumerable = d.hasEnumerable && bronze_truthy(enumerableV.get().rawBits());
    d.wantConfigurable = d.hasConfigurable && bronze_truthy(configurableV.get().rawBits());
    return true;
}

// Are the six descriptor fields of a fresh object literal exactly the ones the
// literal WROTE?
//
// 6.2.6.5 reads each field with HasProperty and Get, and both walk the
// descriptor's prototype chain — so `{ value: v }` describes a non-enumerable
// property only while nothing on `Object.prototype` answers `enumerable`. A
// program that puts one there has changed what every descriptor literal in it
// means, and a lowering that read the literal's own text would not have
// noticed.
//
// The answer is memoized on the prototype's SHAPE, which is what makes the
// question affordable: a shape is immutable and installing a property
// transitions to a different one, so a pointer compare is a proof that nothing
// was added since the walk. Two conditions narrow where that proof holds — a
// dictionary shape is mutated in place, and the memo answers for the whole
// chain rather than for one link — and outside them the six lookups are
// simply repeated.
bool literalDescriptorFieldsAreOwnOnly() {
    // The keys first: interning one allocates the first time a thread asks for
    // it, and an allocation moves the object the walk below holds a pointer to.
    PropertyKey fields[kDescFieldCount];
    for (size_t f = 0; f < kDescFieldCount; ++f) {
        fields[f] = descFieldKey(static_cast<DescField>(f));
    }

    const Value protoVal = rtObjectPrototype();
    if (!protoVal.isObject()) return true;
    auto* proto = protoVal.asObject<ObjectHeader>();

    static thread_local Shape* verified = nullptr;
    const bool memoizable = proto->shape != nullptr && !proto->shape->isDictionary() &&
                            proto->protoAncestor(1) == nullptr;
    if (memoizable && proto->shape == verified) return true;

    ObjectHeader* link = proto;
    for (uint32_t depth = 0; link != nullptr && depth <= 1000; ++depth) {
        if (link->shape != nullptr) {
            PropertyInfo info;
            for (const PropertyKey& key : fields) {
                if (link->shape->lookupProperty(key, info)) return false;
            }
        }
        link = link->protoAncestor(1);
    }
    if (memoizable) verified = proto->shape;
    return true;
}

}  // namespace descriptor_internal

using descriptor_internal::putField;

// 6.2.6.4 FromPropertyDescriptor over the attributes 10.4.3 fixes for a String
// exotic object's CHARACTER keys: non-writable and non-configurable for both an
// index and `length`, enumerable for an index alone. Its own function because
// the arm that builds it now has an early exit around it.
static Value stringCharDescriptor(const StringOwnProperty& own) {
    // Rooted first — building the result allocates.
    Rooted<Value> value{own.value};
    Rooted<Value> out{Value(bronze_create_object())};
    putField(out, "value", value);
    Rooted<Value> w{Value::fromBool(false)};
    putField(out, "writable", w);
    Rooted<Value> e{Value::fromBool(own.enumerable)};
    putField(out, "enumerable", e);
    Rooted<Value> c{Value::fromBool(false)};
    putField(out, "configurable", c);
    return out.get();
}

// 6.2.6.4 FromPropertyDescriptor. The FIELD ORDER is the specification's, not
// a convenience — `Object.keys(descriptor)` prints it, so it is pinned bytes.
uint64_t rtObjectGetOwnPropertyDescriptor(uint64_t, uint64_t, uint32_t argc,
                                          const uint64_t* argv) {
    RootedArgs args(argc, argv);
    switch (rtObjectOwnKeysOf(args[0], "getOwnPropertyDescriptor")) {
        case ObjectOwnKeys::Threw:
            return Value::fromUndefined().rawBits();
        case ObjectOwnKeys::None:
            // The box has no own property, so every key misses — which is
            // `undefined`, the same answer a plain object gives for a name it
            // does not carry.
            return Value::fromUndefined().rawBits();
        case ObjectOwnKeys::Proxy:
            // 10.5.5 [[GetOwnProperty]]: the `getOwnPropertyDescriptor` trap's
            // descriptor, completed, or the target's own (proxy_reflect.cpp).
            return rtProxyGetOwnPropertyDescriptor(args[0], args[1]).rawBits();
        case ObjectOwnKeys::StringChars: {
            // A String exotic OBJECT has the characters AND an ordinary shape
            // (10.4.3.1 step 3 defers to OrdinaryGetOwnProperty), and
            // `String.prototype` is exactly that object: its [[StringData]] is
            // "" and every string method is a property of its shape. So a key
            // the characters do not answer — a symbol, or any name that is not
            // an index or `length` — falls through to the ordinary walk below,
            // the way the array and typed-array arms already do. Only a
            // PRIMITIVE string stops here, because it has no shape to walk.
            const bool isWrapper = args[0].isObject();
            if (!args[1].isSymbol()) {
                const std::string key = rtObjectKeyTextOf(args[1]);
                Value data = args[0];
                if (!data.isString()) rtStringWrapperData(args[0], data);
                StringOwnProperty own;
                if (rtStringDataOwnProperty(data, key, own)) {
                    return stringCharDescriptor(own).rawBits();
                }
            }
            if (!isWrapper) return Value::fromUndefined().rawBits();
            break;
        }
        case ObjectOwnKeys::Namespace: {
            // 10.4.6.1 gives a namespace one own SYMBOL-keyed property —
            // `@@toStringTag`, the string "Module" — and it is the one own key
            // of one that is not an export, so it is answered before the export
            // table is consulted.
            if (Value tag; rtModuleNamespaceOwnSymbol(args[0], args[1], tag)) {
                Rooted<Value> value{tag};
                Rooted<Value> out{Value(bronze_create_object())};
                putField(out, "value", value);
                // All three false: 10.4.6.1 defines it { [[Writable]]: false,
                // [[Enumerable]]: false, [[Configurable]]: false }.
                Rooted<Value> w{Value::fromBool(false)};
                putField(out, "writable", w);
                Rooted<Value> e{Value::fromBool(false)};
                putField(out, "enumerable", e);
                Rooted<Value> c{Value::fromBool(false)};
                putField(out, "configurable", c);
                return out.get().rawBits();
            }
            Value found;
            // False is 10.4.6.5's `undefined` — a name the module does not
            // export has no descriptor at all, which is not the same as a
            // descriptor of `undefined`.
            if (!rtModuleNamespaceOwnProperty(args[0], args[1], found)) {
                return Value::fromUndefined().rawBits();
            }
            Rooted<Value> value{found};
            Rooted<Value> out{Value(bronze_create_object())};
            putField(out, "value", value);
            // `writable: true` is 10.4.6.5's own answer and is not a slip: the
            // EXPORTING module may still assign to the binding, and 6.1.7.3
            // forbids a non-writable non-configurable property whose value
            // changes. What refuses `ns.x = 1` is [[Set]] (10.4.6.9), which
            // returns false whatever this descriptor says — the two are
            // different internal methods and only one of them is an attribute.
            Rooted<Value> w{Value::fromBool(true)};
            putField(out, "writable", w);
            Rooted<Value> e{Value::fromBool(true)};
            putField(out, "enumerable", e);
            Rooted<Value> c{Value::fromBool(false)};
            putField(out, "configurable", c);
            return out.get().rawBits();
        }
        case ObjectOwnKeys::Function: {
            // The three own properties that live in the HEADER rather than in
            // the statics object, and so are invisible to the forward below:
            // `prototype` (10.2.4) and the `length`/`name` pair (10.2.10,
            // 10.2.9). Answered first, because the statics object is where a
            // `static` of the same name would be and there can be none — the
            // write path refuses all three.
            if (!args[1].isSymbol()) {
                const std::string key = rtObjectKeyTextOf(args[1]);
                const bool isProto = key == "prototype";
                const bool isPair = (key == "length" || key == "name") &&
                                    args[0].asObject<FunctionHeader>()->name != nullptr;
                if (isProto || isPair) {
                    Rooted<Value> self{args[0]};
                    Rooted<Value> value{Value::fromUndefined()};
                    if (isProto) {
                        rtEnsureFunctionPrototype(self);
                        value.set(self.get().asObject<FunctionHeader>()->prototype);
                    } else if (key == "length") {
                        value.set(
                            Value::fromDouble(self.get().asObject<FunctionHeader>()->length));
                    } else {
                        value.set(rtKeyAsValue(self.get().asObject<FunctionHeader>()->name));
                    }
                    // 10.2.4 makes `prototype` non-enumerable and
                    // non-configurable, writable unless the function is one
                    // whose prototype never was; 10.2.9 and 10.2.10 make the
                    // pair non-writable, non-enumerable and CONFIGURABLE.
                    const bool writable =
                        isProto && rtFunctionPrototypeWritable(self.get());
                    const bool configurable = !isProto;
                    Rooted<Value> out{Value(bronze_create_object())};
                    putField(out, "value", value);
                    Rooted<Value> w{Value::fromBool(writable)};
                    putField(out, "writable", w);
                    Rooted<Value> e{Value::fromBool(false)};
                    putField(out, "enumerable", e);
                    Rooted<Value> c{Value::fromBool(configurable)};
                    putField(out, "configurable", c);
                    return out.get().rawBits();
                }
            }
            Value props = args[0].asObject<FunctionHeader>()->properties;
            if (props.isUndefined() || !props.isObject()) {
                return Value::fromUndefined().rawBits();
            }
            const uint64_t call[2] = {props.rawBits(), args[1].rawBits()};
            return rtObjectGetOwnPropertyDescriptor(0, 0, 2, call);
        }
        case ObjectOwnKeys::Array: {
            // Two own properties live in the header rather than in the side
            // object: `length` (10.4.2.2: writable until `Object.freeze`,
            // never enumerable or configurable) and each ELEMENT (defined by
            // CreateDataProperty, so all three attributes true until
            // `Object.seal` or `Object.freeze` takes the last two away — the
            // same two questions an element write and delete ask, integrity.h).
            // A hole is not an own property at all.
            if (!args[1].isSymbol()) {
                const std::string key = rtObjectKeyTextOf(args[1]);
                Rooted<Value> self{args[0]};
                uint32_t index = 0;
                const bool isLength = key == "length";
                const bool isIndex = !isLength && rtIsIntegerLikeKey(key, index);
                if (isLength || isIndex) {
                    auto* arr = self.get().asObject<ArrayHeader>();
                    if (isIndex && !arr->hasElem(index)) return Value::fromUndefined().rawBits();
                    Rooted<Value> value{isLength ? Value::fromDouble(arr->length)
                                                 : arr->getElem(index)};
                    const bool writable =
                        isLength ? rtIntegrityLevel(self.get()) != IntegrityLevel::Frozen
                                 : rtArrayElementWriteRefusal(self.get(), index) ==
                                       SetRefusal::None;
                    const bool configurable =
                        isIndex && rtArrayElementsConfigurable(self.get());
                    Rooted<Value> out{Value(bronze_create_object())};
                    putField(out, "value", value);
                    Rooted<Value> w{Value::fromBool(writable)};
                    putField(out, "writable", w);
                    Rooted<Value> e{Value::fromBool(isIndex)};
                    putField(out, "enumerable", e);
                    Rooted<Value> c{Value::fromBool(configurable)};
                    putField(out, "configurable", c);
                    return out.get().rawBits();
                }
            }
            // Everything else is in the side object, an ordinary shape that
            // answers with the ordinary walk below — through the same forward
            // the function arm makes for its statics.
            Value props = args[0].asObject<ArrayHeader>()->properties;
            if (!props.isObject()) return Value::fromUndefined().rawBits();
            const uint64_t call[2] = {props.rawBits(), args[1].rawBits()};
            return rtObjectGetOwnPropertyDescriptor(0, 0, 2, call);
        }
        case ObjectOwnKeys::TypedArray: {
            // 10.4.5.1 [[GetOwnProperty]]: a numeric key is an element —
            // `{ value, writable: true, enumerable: true, configurable: true }`
            // within the length, absent outside it — and never the shape's;
            // any other key is the ordinary walk below over the view's shape.
            if (!args[1].isSymbol()) {
                const std::string key = rtObjectKeyTextOf(args[1]);
                uint32_t index = 0;
                if (rtIsIntegerLikeKey(key, index)) {
                    if (index >= args[0].asObject<TypedArrayHeader>()->length) {
                        return Value::fromUndefined().rawBits();
                    }
                    Rooted<Value> value{rtTypedArrayElement(args[0], index)};
                    Rooted<Value> out{Value(bronze_create_object())};
                    putField(out, "value", value);
                    Rooted<Value> t{Value::fromBool(true)};
                    putField(out, "writable", t);
                    putField(out, "enumerable", t);
                    putField(out, "configurable", t);
                    return out.get().rawBits();
                }
                if (rtIsCanonicalNumericString(key)) return Value::fromUndefined().rawBits();
            }
            break;
        }
        case ObjectOwnKeys::RegExp: {
            // 22.2.3.2: `lastIndex` is { value, writable: true (until frozen),
            // enumerable: false, configurable: false }, held in the header;
            // any other key is the ordinary walk below over the shape.
            if (!args[1].isSymbol()) {
                const std::string key = rtObjectKeyTextOf(args[1]);
                if (key == "lastIndex") {
                    Rooted<Value> value{rtRegExpLastIndexValue(args[0])};
                    const bool writable = rtIntegrityLevel(args[0]) != IntegrityLevel::Frozen;
                    Rooted<Value> out{Value(bronze_create_object())};
                    putField(out, "value", value);
                    Rooted<Value> w{Value::fromBool(writable)};
                    putField(out, "writable", w);
                    Rooted<Value> f{Value::fromBool(false)};
                    putField(out, "enumerable", f);
                    putField(out, "configurable", f);
                    return out.get().rawBits();
                }
            }
            break;
        }
        case ObjectOwnKeys::Shape:
            break;
    }
    Rooted<Value> self{args[0]};
    rtCheckStringExoticOwnKeys(self.get(), "describing");
    PropertyKey name = rtInternPropertyKey(args[1]);

    PropertyInfo info;
    auto* obj = self.get().asObject<ObjectHeader>();
    if (!obj->shape || !obj->shape->lookupProperty(name, info)) {
        return Value::fromUndefined().rawBits();
    }
    // Read the slots BEFORE building the result object: creating it allocates,
    // and these are raw reads off a pointer a collection would move.
    Rooted<Value> a{obj->getSlot(info.slot)};
    Rooted<Value> b{info.accessor ? obj->getSlot(info.slot + 1) : Value::fromUndefined()};

    Rooted<Value> out{Value(bronze_create_object())};
    if (info.accessor) {
        putField(out, "get", a);
        putField(out, "set", b);
    } else {
        putField(out, "value", a);
        Rooted<Value> w{Value::fromBool(info.writable)};
        putField(out, "writable", w);
    }
    Rooted<Value> e{Value::fromBool(info.enumerable)};
    putField(out, "enumerable", e);
    Rooted<Value> c{Value::fromBool(info.configurable)};
    putField(out, "configurable", c);
    return out.get().rawBits();
}

// 20.1.2.9 Object.getOwnPropertyDescriptors: one entry per own property, each
// the object `getOwnPropertyDescriptor` builds. Defined in terms of it (step 4
// calls it per key), so it calls it, rather than growing a second copy of the
// descriptor shape that could drift from the first.
uint64_t rtObjectGetOwnPropertyDescriptors(uint64_t, uint64_t, uint32_t argc,
                                           const uint64_t* argv) {
    RootedArgs args(argc, argv);
    // The receiver classification happens here only to raise (or refuse) before
    // any work; every source below is then handled by the two members this is
    // defined in terms of, which is the point of defining it in terms of them.
    if (rtObjectOwnKeysOf(args[0], "getOwnPropertyDescriptors") == ObjectOwnKeys::Threw) {
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> self{args[0]};
    Rooted<Value> out{Value(bronze_create_object())};
    // ALL own keys, not just the enumerable ones (20.1.2.9 step 2 is
    // OwnPropertyKeys), which is where this differs from `Object.keys`.
    const uint64_t ownCall[1] = {self.get().rawBits()};
    Rooted<Value> names{Value(rtObjectGetOwnPropertyNames(0, 0, 1, ownCall))};
    const uint32_t count = names.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{names.get().asObject<ArrayHeader>()->getElem(i)};
        const uint64_t call[2] = {self.get().rawBits(), key.get().rawBits()};
        Rooted<Value> desc{Value(rtObjectGetOwnPropertyDescriptor(0, 0, 2, call))};
        putField(out, key, desc);
    }
    return out.get().rawBits();
}

}  // namespace bronze::runtime
