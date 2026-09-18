// The JS surface of the typed-array family (ECMA-262 23.2): the abstract
// `%TypedArray%` (23.2.1) with its statics (23.2.2) and `%TypedArray%.prototype`
// (23.2.3), the twelve view constructors (23.2.6) and their prototypes
// (23.2.7), and the two questions the property path still answers from the
// KIND rather than by a walk. Construction is builtin_typed_array_construct.cpp,
// the method bodies builtin_typed_array_methods.cpp and
// builtin_typed_array_iteration.cpp, `ArrayBuffer` builtin_array_buffer.cpp,
// and the representation typed_array.{h,cpp}.
//
// A typed array is an ORDINARY OBJECT with a real prototype chain, built here
// exactly as 23.2 lays it out: `Uint8Array.prototype` holds `constructor` and
// `BYTES_PER_ELEMENT` and nothing else, its [[Prototype]] is
// `%TypedArray%.prototype` where every method and the four accessors live, and
// `Uint8Array`'s own [[Prototype]] is `%TypedArray%`, from which it inherits
// `from`, `of` and `@@species`. A NAMED read of a view is the ordinary shape
// walk, cached at the site like any prototype method; what stays exotic is the
// integer index (10.4.5) and, on a pristine chain, the four accessors — both
// answered from the header in rt_prop.cpp with the kind as the guard, because
// `v[i]` and `v.length` are what a typed array is measured on.

#include <cstring>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/bigint.h"
#include "runtime/builtin_typed_array_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/proxy.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
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

constexpr size_t kKindCount = static_cast<size_t>(ElementKind::Count);

// One record per view kind. Every Value is a permanent root: the first use of
// any member of the family builds all of it, and every later use reads it.
// The PRISTINE fields are the witnesses the two fast paths compare against —
// the shape each object was left with when the family was built, and the slot
// its `constructor` sits in — so a program that redefines any of it is seen.
struct ViewIntrinsics {
    Value ctor = Value::fromUndefined();
    Value proto = Value::fromUndefined();
    Shape* instanceShape = nullptr;
    Shape* protoPristineShape = nullptr;
    Shape* ctorBoxPristineShape = nullptr;
    uint32_t constructorSlot = 0;
};

// `%TypedArray%` and its prototype, with the four accessor getters and the
// slots they occupy — what `rtTypedArrayChainPristine` re-verifies.
struct FamilyIntrinsics {
    Value ctor = Value::fromUndefined();
    Value proto = Value::fromUndefined();
    Shape* protoPristineShape = nullptr;
    Shape* ctorBoxPristineShape = nullptr;
    static constexpr size_t kAccessorCount = 4;
    uint32_t accessorSlot[kAccessorCount] = {};
    Value accessorGetter[kAccessorCount] = {Value::fromUndefined(), Value::fromUndefined(),
                                            Value::fromUndefined(), Value::fromUndefined()};
    // `[Symbol.iterator]`'s slot and the `values` it holds when untouched. A
    // DATA property, so a program overwrites it in place — no shape change,
    // no epoch bump — and the value has to be compared on every ask rather
    // than latched with the accessors above.
    uint32_t iteratorSlot = 0;
    Value iteratorValues = Value::fromUndefined();
    // The epoch the chain was last verified at, and the verdict. A changed
    // epoch means SOME prototype somewhere changed; the thirteen shapes say
    // whether it was one of ours.
    uint64_t pristineEpoch = 0;
    bool pristine = false;
    // `ArrayBuffer`'s instance shape, immortal and never replaced (25.1.5.2
    // makes the prototype non-writable), kept here so `new Float32Array(n)`
    // does not cross into builtin_array_buffer.cpp's `ensure` for it.
    Shape* bufferInstanceShape = nullptr;
};

thread_local ViewIntrinsics g_views[kKindCount];
thread_local FamilyIntrinsics g_family;

void ensureTypedArrayIntrinsics();

// ---- the constructors --------------------------------------------------------

template <ElementKind K>
uint64_t viewCtor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> receiver{Value(thisBits)};
    return rtTypedArrayConstructBody(K, receiver, argc, argv).rawBits();
}

// 23.2.1.1 %TypedArray%(): a TypeError whether called or constructed — it
// exists to be the [[Prototype]] of the twelve and to hold what they share.
uint64_t typedArrayAbstractCtor(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return rtThrowTypeError("Abstract class TypedArray not directly constructable").rawBits();
}

struct CtorEntry {
    ElementKind kind;
    bronze_fn_code code;
};

const CtorEntry kCtors[] = {
    {ElementKind::Int8, viewCtor<ElementKind::Int8>},
    {ElementKind::Uint8, viewCtor<ElementKind::Uint8>},
    {ElementKind::Uint8Clamped, viewCtor<ElementKind::Uint8Clamped>},
    {ElementKind::Int16, viewCtor<ElementKind::Int16>},
    {ElementKind::Uint16, viewCtor<ElementKind::Uint16>},
    {ElementKind::Int32, viewCtor<ElementKind::Int32>},
    {ElementKind::Uint32, viewCtor<ElementKind::Uint32>},
    {ElementKind::Float32, viewCtor<ElementKind::Float32>},
    {ElementKind::Float64, viewCtor<ElementKind::Float64>},
    {ElementKind::Float16, viewCtor<ElementKind::Float16>},
    {ElementKind::BigInt64, viewCtor<ElementKind::BigInt64>},
    {ElementKind::BigUint64, viewCtor<ElementKind::BigUint64>},
};

static_assert(std::size(kCtors) == kKindCount,
              "the constructor table has drifted from the ElementKind enum");

// ---- the accessors (23.2.3.1–.3, 23.2.3.18, 23.2.3.35) ----------------------

// 23.2.3's four getters open with RequireInternalSlot: a foreign receiver is a
// TypeError, never a lookup. A view whose constructor has not run yet (a
// derived class reading `this` before `super()`) has no buffer to answer
// from and is refused on the same terms.
bool requireViewReceiver(Value self, const char* getter) {
    if (!isTypedArray(self) || !self.asObject<TypedArrayHeader>()->buffer.isObject()) {
        rtThrowTypeError(std::string("get %TypedArray%.prototype.") + getter +
                         " called on incompatible receiver");
        return false;
    }
    return true;
}

// 23.2.3.1: the identity outlives the window — a detached buffer is still the
// answer.
uint64_t bufferGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!requireViewReceiver(self, "buffer")) return Value::fromUndefined().rawBits();
    return self.asObject<TypedArrayHeader>()->buffer.rawBits();
}

// 23.2.3.2: 0 for a view its buffer left behind, through the maintained
// window alone (typed_array.h, `length`).
uint64_t byteLengthGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!requireViewReceiver(self, "byteLength")) return Value::fromUndefined().rawBits();
    return Value::fromDouble(self.asObject<TypedArrayHeader>()->byteLength()).rawBits();
}

// 23.2.3.3: +0 out of bounds — the one length-family answer the maintained
// window cannot carry, because the stored offset survives the closing.
uint64_t byteOffsetGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!requireViewReceiver(self, "byteOffset")) return Value::fromUndefined().rawBits();
    const auto* view = self.asObject<TypedArrayHeader>();
    return Value::fromDouble(view->isOutOfBounds() ? 0.0 : view->byteOffset).rawBits();
}

// 23.2.3.18.
uint64_t lengthGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!requireViewReceiver(self, "length")) return Value::fromUndefined().rawBits();
    return Value::fromDouble(self.asObject<TypedArrayHeader>()->length).rawBits();
}

// 23.2.3.35 get %TypedArray%.prototype[@@toStringTag]: [[TypedArrayName]] for
// a view, `undefined` — NOT a throw — for anything else, which is what keeps
// `Object.prototype.toString.call(Uint8Array.prototype)` "[object Object]".
uint64_t toStringTagGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self{Value(thisBits)};
    if (!isTypedArray(self)) return Value::fromUndefined().rawBits();
    return rtMakeString(self.asObject<TypedArrayHeader>()->kindName()).rawBits();
}

// 23.2.3.31 toLocaleString, refused by name rather than aliased to `join`:
// its element format is `Number.prototype.toLocaleString`'s, which bronze
// refuses for want of locale data, so the member is a function that says so
// instead of a `undefined` a program would feature-test away. The same
// arrangement as `BigInt.prototype.toLocaleString`.
uint64_t toLocaleStringRefusal(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    fatal("unsupported: %TypedArray%.prototype.toLocaleString is not implemented (its "
          "elements format through Number.prototype.toLocaleString, which needs locale data "
          "bronze does not carry)");
}

// ---- assembling the family ----------------------------------------------------

// A DEFINITION of a non-enumerable data property — the terms `rtDefineMethods`
// uses — with the attributes spelled out, because 23.2.7.1's
// `BYTES_PER_ELEMENT` is the rare property that is neither writable nor
// configurable.
void defineData(Rooted<Value>& obj, Rooted<Value>& key, Rooted<Value>& val, bool writable,
                bool configurable) {
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, /*ic=*/nullptr,
                                                /*enumerable=*/false, /*defineOwn=*/true,
                                                /*receiver=*/nullptr, /*refused=*/nullptr,
                                                writable, configurable);
}

void defineGetter(Rooted<Value>& proto, Rooted<Value>& key, bronze_fn_code code,
                  const char* name) {
    // 10.2.9 step 5: an accessor's getter is named with the "get " prefix.
    Rooted<Value> getter{rtNativeFunction(code, 0, name, 0)};
    Rooted<Value> setter{Value::fromUndefined()};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), proto, key, getter, setter,
                                 /*enumerable=*/false);
}

uint32_t slotOf(Value obj, PropertyKey key) {
    PropertyInfo info;
    if (!obj.asObject<ObjectHeader>()->shape->lookupProperty(key, info)) {
        fatal("internal: a typed-array intrinsic lost a property it just defined");
    }
    return info.slot;
}

ObjectHeader* newPlainObject(Value proto) {
    Shape* shape = rtNewRootShape(proto);
    shape->used_as_prototype = true;
    ObjectHeader* obj = ObjectHeader::create(rtHeap(), rtArena(), shape);
    obj->header.flags = HeapKind::Plain;
    return obj;
}

// %TypedArray% and %TypedArray%.prototype, in the order 23.2.3 lists them
// where the order is observable. Publishes each object into the record the
// moment it exists, because every install below allocates.
void buildFamily() {
    Rooted<Value> proto{Value::fromObject(newPlainObject(rtObjectPrototype()))};
    g_family.proto = proto.get();
    rtHeap().add_permanent_root(&g_family.proto);

    Rooted<Value> ctor{rtNativeFunction(typedArrayAbstractCtor, 0, "TypedArray", 0)};
    g_family.ctor = ctor.get();
    rtHeap().add_permanent_root(&g_family.ctor);
    {
        // 23.2.3.5, a DEFINITION so it does not write through.
        Rooted<Value> key{rtMakeString("constructor")};
        defineData(proto, key, ctor, /*writable=*/true, /*configurable=*/true);
    }

    size_t methodCount = 0;
    const NativeMethod* methods = rtTypedArrayMethodTable(methodCount);
    rtDefineMethods(proto, methods, methodCount);
    {
        Rooted<Value> key{rtMakeString("toLocaleString")};
        Rooted<Value> fn{rtNativeFunction(toLocaleStringRefusal, 0, "toLocaleString", 0)};
        defineData(proto, key, fn, /*writable=*/true, /*configurable=*/true);
    }
    {
        // Before `[Symbol.iterator]`, because 23.2.3 fixes no order for the two
        // symbol keys and node reports them in this one.
        Rooted<Value> key{Value::fromSymbol(rtSymbolToStringTag())};
        defineGetter(proto, key, toStringTagGetter, "get [Symbol.toStringTag]");
    }
    {
        // 23.2.3.34: `[Symbol.iterator]` IS `values` — one function object
        // under two keys, which the interning on the code pointer gives.
        Rooted<Value> key{Value::fromSymbol(rtSymbolIterator())};
        Rooted<Value> values{rtNativeFunction(taValues, 0, "values", 0)};
        defineData(proto, key, values, /*writable=*/true, /*configurable=*/true);
        g_family.iteratorValues = values.get();
        rtHeap().add_permanent_root(&g_family.iteratorValues);
        g_family.iteratorSlot =
            slotOf(proto.get(), PropertyKey::forSymbol(key.get().asSymbol<SymbolHeader>()));
    }

    struct Accessor {
        const char* name;
        bronze_fn_code code;
        const char* getterName;
    };
    const Accessor accessors[FamilyIntrinsics::kAccessorCount] = {
        {"buffer", bufferGetter, "get buffer"},
        {"byteLength", byteLengthGetter, "get byteLength"},
        {"byteOffset", byteOffsetGetter, "get byteOffset"},
        {"length", lengthGetter, "get length"},
    };
    for (size_t i = 0; i < FamilyIntrinsics::kAccessorCount; ++i) {
        Rooted<Value> key{rtMakeString(accessors[i].name)};
        defineGetter(proto, key, accessors[i].code, accessors[i].getterName);
        g_family.accessorGetter[i] = rtNativeFunction(accessors[i].code, 0);
        rtHeap().add_permanent_root(&g_family.accessorGetter[i]);
        g_family.accessorSlot[i] =
            slotOf(proto.get(), PropertyKey::forString(key.get().asString<StringHeader>()));
    }

    // 23.2.2: the statics, ordinary own properties of the constructor's box —
    // where a view constructor reaches them by the chain `extends` builds
    // between the boxes, exactly as a class reaches an inherited static.
    // 23.2.2.4's `@@species` is a real accessor there too, so a subclass
    // inherits it and `Object.getOwnPropertyDescriptor` can describe it.
    rtEnsureFunctionProperties(ctor);
    {
        Rooted<Value> box{ctor.get().asObject<FunctionHeader>()->properties};
        const NativeMethod statics[] = {
            {"from", rtTypedArrayFromBody, 1, 1},
            {"of", rtTypedArrayOfBody, 0, 0},
        };
        rtDefineMethods(box, statics, std::size(statics));
        rtDefineSpeciesGetter(ctor);
    }

    FunctionHeader* fn = ctor.get().asObject<FunctionHeader>();
    fn->prototype = proto.get();
    // 23.2.2.3: `{ [[Writable]]: false, [[Enumerable]]: false,
    // [[Configurable]]: false }`.
    fn->prototype_readonly = true;
    fn->instance_shape = rtNewRootShape(proto.get());
    g_family.protoPristineShape = proto.get().asObject<ObjectHeader>()->shape;
}

// One view: its constructor, its prototype (23.2.7: `constructor` and
// `BYTES_PER_ELEMENT`, nothing else), the chain links to the family, and the
// instance shape every `new` allocates from.
void buildView(ElementKind kind, bronze_fn_code code) {
    ViewIntrinsics& out = g_views[static_cast<size_t>(kind)];
    const ElementKindInfo& info = elementKindInfo(kind);

    Rooted<Value> proto{Value::fromObject(newPlainObject(g_family.proto))};
    out.proto = proto.get();
    rtHeap().add_permanent_root(&out.proto);

    // Arity 0: a variadic native must not be padded, or `new Float64Array(buf)`
    // would arrive with two extra `undefined`s and take the explicit-offset
    // branch. `length` is 23.2.6's 3.
    Rooted<Value> ctor{rtNativeFunction(code, 0, info.name, 3)};
    out.ctor = ctor.get();
    rtHeap().add_permanent_root(&out.ctor);
    {
        Rooted<Value> key{rtMakeString("constructor")};
        defineData(proto, key, ctor, /*writable=*/true, /*configurable=*/true);
        out.constructorSlot =
            slotOf(proto.get(), PropertyKey::forString(key.get().asString<StringHeader>()));
    }
    Rooted<Value> bpeKey{rtMakeString("BYTES_PER_ELEMENT")};
    Rooted<Value> bpe{Value::fromDouble(info.bytesPerElement)};
    // 23.2.7.1 and 23.2.6.1: non-writable, non-enumerable, non-configurable
    // on the prototype and on the constructor alike.
    defineData(proto, bpeKey, bpe, /*writable=*/false, /*configurable=*/false);

    // The statics box, chained to `%TypedArray%`'s so `Uint8Array.from` is
    // found by the walk `MyArr.of` is found by (runtime/native_base.h).
    Rooted<Value> familyBox{g_family.ctor.asObject<FunctionHeader>()->properties};
    ObjectHeader* box = ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(familyBox.get()));
    box->header.flags = HeapKind::Plain;
    Rooted<Value> boxRoot{Value::fromObject(box)};
    defineData(boxRoot, bpeKey, bpe, /*writable=*/false, /*configurable=*/false);
    // Marked as a prototype's shape would be, though nothing inherits from it
    // yet: the mark is what makes a mutation of the box — a `@@species` a
    // program defines on `Uint8Array` itself — bump the epoch that
    // reverifyPristine latches on, so the species fast path can trust the
    // latch instead of re-reading this shape on every `slice`.
    boxRoot.get().asObject<ObjectHeader>()->shape->used_as_prototype = true;

    FunctionHeader* fn = ctor.get().asObject<FunctionHeader>();
    fn->prototype = proto.get();
    // 23.2.6.2: non-writable, non-enumerable, non-configurable.
    fn->prototype_readonly = true;
    fn->properties = boxRoot.get();
    // 23.2.6: the constructor's own [[Prototype]] is %TypedArray%.
    fn->parent = g_family.ctor;
    // Which native object `new` allocates — recorded on the intrinsic itself
    // and copied down every `extends` link from it.
    fn->native_base = static_cast<uint8_t>(NativeBase::TypedArrayFirst + static_cast<uint8_t>(kind));
    // Last: an instance cannot exist before there is a shape to build one
    // from, and `rtAllocateNativeBaseInstance` reads this slot for every
    // construction — the intrinsic's own and a subclass's alike.
    fn->instance_shape = rtNewRootShape(proto.get());
    out.instanceShape = fn->instance_shape;
    out.protoPristineShape = proto.get().asObject<ObjectHeader>()->shape;
    out.ctorBoxPristineShape = boxRoot.get().asObject<ObjectHeader>()->shape;
}

void ensureTypedArrayIntrinsics() {
    if (g_family.proto.isObject()) return;
    buildFamily();
    for (const CtorEntry& entry : kCtors) buildView(entry.kind, entry.code);
    // The family box's shape is captured LAST: each view's box chains to it
    // (rtNewRootShape marks the link, and can move a shared shape to a
    // dedicated one when it does), so the pristine shape is whatever the
    // twelve links left it.
    g_family.ctorBoxPristineShape =
        g_family.ctor.asObject<FunctionHeader>()->properties.asObject<ObjectHeader>()->shape;
    g_family.bufferInstanceShape = rtArrayBufferInstanceShape();
    g_family.pristineEpoch = protoMutationEpoch();
    g_family.pristine = true;
}

// Re-verify the thirteen prototype shapes, the four getter slots, the
// thirteen statics-box shapes and the twelve `extends` links. Called only
// when the epoch moved, which any add, delete, redefinition or prototype swap
// on ANY object serving as a prototype does — the boxes are marked as such
// in buildView exactly so that a `@@species` defined on one moves it — so the
// common case is one compare against the latched epoch.
bool reverifyPristine() {
    const auto* proto = g_family.proto.asObject<ObjectHeader>();
    bool ok = proto->shape == g_family.protoPristineShape;
    for (size_t i = 0; ok && i < FamilyIntrinsics::kAccessorCount; ++i) {
        ok = proto->getSlot(g_family.accessorSlot[i]).rawBits() ==
             g_family.accessorGetter[i].rawBits();
    }
    ok = ok && g_family.ctor.asObject<FunctionHeader>()->properties.asObject<ObjectHeader>()->shape ==
                   g_family.ctorBoxPristineShape;
    for (size_t k = 0; ok && k < kKindCount; ++k) {
        const ViewIntrinsics& kind = g_views[k];
        const auto* ctor = kind.ctor.asObject<FunctionHeader>();
        ok = kind.proto.asObject<ObjectHeader>()->shape == kind.protoPristineShape &&
             ctor->parent.rawBits() == g_family.ctor.rawBits() &&
             ctor->properties.asObject<ObjectHeader>()->shape == kind.ctorBoxPristineShape;
    }
    g_family.pristine = ok;
    g_family.pristineEpoch = protoMutationEpoch();
    return ok;
}

// 7.3.22 SpeciesConstructor(O, defaultConstructor).
Value speciesConstructor(Rooted<Value>& exemplar, Value defaultCtor) {
    Rooted<Value> fallback{defaultCtor};
    Rooted<Value> ctorKey{rtMakeString("constructor")};
    Rooted<Value> ctor{Value(bronze_elem_get(exemplar.get().rawBits(), ctorKey.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined();
    if (ctor.get().isUndefined()) return fallback.get();
    if (!ctor.get().isObject()) {
        return rtThrowTypeError("the constructor of this typed array is not an object");
    }
    Rooted<Value> speciesKey{Value::fromSymbol(rtSymbolSpecies())};
    Rooted<Value> species{
        Value(bronze_elem_get(ctor.get().rawBits(), speciesKey.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined();
    if (species.get().isUndefined() || species.get().isNull()) return fallback.get();
    if (!rtIsConstructorValue(species.get())) {
        return rtThrowTypeError("[Symbol.species] of this typed array is not a constructor");
    }
    return species.get();
}

// Is the exemplar's species its own kind's intrinsic, with no `Get`
// performed? Its shape is the intrinsic instance shape (no own `constructor`
// and the intrinsic prototype), the chain and the statics boxes are as built
// (the epoch latch), and that prototype's `constructor` slot still holds the
// intrinsic — a DATA property a program overwrites in place, no shape change
// and no epoch bump, so it is the one value compared on every ask.
bool speciesPristine(const TypedArrayHeader* view) {
    if (!rtTypedArrayChainPristine(view)) return false;
    const ViewIntrinsics& kind = g_views[view->kind];
    return kind.proto.asObject<ObjectHeader>()->getSlot(kind.constructorSlot).rawBits() ==
           kind.ctor.rawBits();
}

// 23.2.4.1 step 3: the species may not change the content type.
bool sameContentType(Value made, ElementKind kind) {
    if (isBigIntElementKind(kindOf(made)) == isBigIntElementKind(kind)) return true;
    rtThrowTypeError("the species constructor built a typed array of the other content type "
                     "(BigInt where Number was expected, or the reverse)");
    return false;
}

}  // namespace

// ---- the intrinsics, by name and by identity ----------------------------------

Value rtTypedArrayConstructor(const std::string& name) {
    if (name == "ArrayBuffer") return rtArrayBufferConstructor(name);
    for (const CtorEntry& entry : kCtors) {
        if (name == elementKindInfo(entry.kind).name) return rtTypedArrayConstructorFor(entry.kind);
    }
    return Value::fromUndefined();
}

Value rtTypedArrayConstructorFor(ElementKind kind) {
    ensureTypedArrayIntrinsics();
    return g_views[static_cast<size_t>(kind)].ctor;
}

// By CODE POINTER and never by interning a constructor and comparing bits:
// identifying an intrinsic must not build one, because this is asked from
// paths where an unexpected allocation retires a pointer mid-lookup.
bool rtTypedArrayConstructorKind(Value fn, ElementKind& out) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return false;
    }
    const bronze_fn_code code = fn.asObject<FunctionHeader>()->code;
    for (const CtorEntry& entry : kCtors) {
        if (entry.code != code) continue;
        out = entry.kind;
        return true;
    }
    return false;
}

const char* rtTypedArrayConstructorName(Value fn) {
    ElementKind kind = ElementKind::Int8;
    return rtTypedArrayConstructorKind(fn, kind) ? elementKindInfo(kind).name : nullptr;
}

bool rtIsTypedArrayIntrinsic(Value fn) {
    return fn.isObject() && fn.asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
           fn.asObject<FunctionHeader>()->code == typedArrayAbstractCtor;
}

bool rtIsIntrinsicTypedArrayIterator(Value fn) {
    return fn.isObject() && fn.asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
           fn.asObject<FunctionHeader>()->code == taValues;
}

Shape* rtTypedArrayInstanceShape(ElementKind kind) {
    ensureTypedArrayIntrinsics();
    return g_views[static_cast<size_t>(kind)].instanceShape;
}

// ---- allocation ---------------------------------------------------------------

// One `ensure` and the shapes read straight off the records: these two sit
// under every allocating method, and each `rt*InstanceShape` getter would
// re-ask its own `ensure` on the way to the same immortal pointer.
Value rtNewTypedArray(ElementKind kind, uint32_t length) {
    ensureTypedArrayIntrinsics();
    return Value::fromObject(TypedArrayHeader::create(rtHeap(),
                                                      g_views[static_cast<size_t>(kind)].instanceShape,
                                                      g_family.bufferInstanceShape, kind, length));
}

Value rtNewTypedArrayOverBuffer(ElementKind kind, Rooted<Value>& buffer, uint32_t byteOffset,
                                uint32_t length, bool tracking) {
    ensureTypedArrayIntrinsics();
    return Value::fromObject(TypedArrayHeader::createOverBuffer(
        rtHeap(), g_views[static_cast<size_t>(kind)].instanceShape, kind, buffer, byteOffset,
        length, tracking));
}

Value rtTypedArraySpeciesCreate(Rooted<Value>& exemplar, uint32_t length) {
    const ElementKind kind = kindOf(exemplar.get());
    if (!checkAllocatable(length * elementKindInfo(kind).bytesPerElement)) {
        return Value::fromUndefined();
    }
    if (speciesPristine(exemplar.get().asObject<TypedArrayHeader>())) {
        return rtNewTypedArray(kind, length);
    }
    Rooted<Value> ctor{speciesConstructor(exemplar, g_views[static_cast<size_t>(kind)].ctor)};
    if (rtExceptionPending()) return Value::fromUndefined();
    Rooted<Value> made{rtTypedArrayCreateFromConstructor(ctor, length)};
    if (rtExceptionPending()) return Value::fromUndefined();
    if (!sameContentType(made.get(), kind)) return Value::fromUndefined();
    return made.get();
}

Value rtTypedArraySpeciesCreateOverBuffer(Rooted<Value>& exemplar, Rooted<Value>& buffer,
                                          uint32_t byteOffset, uint32_t length, bool tracking) {
    const ElementKind kind = kindOf(exemplar.get());
    if (speciesPristine(exemplar.get().asObject<TypedArrayHeader>())) {
        return rtNewTypedArrayOverBuffer(kind, buffer, byteOffset, length, tracking);
    }
    Rooted<Value> ctor{speciesConstructor(exemplar, g_views[static_cast<size_t>(kind)].ctor)};
    if (rtExceptionPending()) return Value::fromUndefined();
    // 23.2.3.30 step 13-14: « buffer, beginByteOffset » for a tracking result,
    // « buffer, beginByteOffset, newLength » otherwise.
    RootedBlock block(tracking ? 2 : 3);
    block.set(0, buffer.get());
    block.set(1, Value::fromDouble(static_cast<double>(byteOffset)));
    if (!tracking) block.set(2, Value::fromDouble(static_cast<double>(length)));
    Rooted<Value> made{Value(bronze_construct(ctor.get().rawBits(), block.count(), block.data()))};
    if (rtExceptionPending()) return Value::fromUndefined();
    // 23.2.4.4 ValidateTypedArray on what came back.
    if (!requireTypedArray(made.get(), "subarray")) return Value::fromUndefined();
    if (!sameContentType(made.get(), kind)) return Value::fromUndefined();
    return made.get();
}

// ---- the two kind-answered questions ----------------------------------------------

bool rtTypedArrayChainPristine(const TypedArrayHeader* view) noexcept {
    if (view->object.shape != g_views[view->kind].instanceShape) return false;
    if (g_family.pristineEpoch == protoMutationEpoch()) return g_family.pristine;
    return reverifyPristine();
}

bool rtTypedArrayIteratorPristine(const TypedArrayHeader* view) noexcept {
    if (!rtTypedArrayChainPristine(view)) return false;
    // The chain's shapes are the intrinsic ones, so the slot IS
    // `[Symbol.iterator]`'s; whether it still holds `values` is the value
    // compare a data property needs (an in-place overwrite moves nothing else).
    return g_family.proto.asObject<ObjectHeader>()->getSlot(g_family.iteratorSlot).rawBits() ==
           g_family.iteratorValues.rawBits();
}

bool rtIsCanonicalNumericString(const std::string& key) {
    if (key.empty()) return false;
    // The first character filters every ordinary name — `set`, `subarray`,
    // `length` — before a conversion is attempted: a canonical numeric string
    // starts with a digit, a sign, or the first letter of NaN / Infinity.
    const char c = key[0];
    if (!((c >= '0' && c <= '9') || c == '-' || c == 'N' || c == 'I')) return false;
    if (key == "-0") return true;
    Rooted<Value> str{rtMakeString(key)};
    const double n = rtToNumber(str.get());
    char buf[32];
    const size_t len = formatJsNumber(n, buf);
    return key.size() == len && std::memcmp(key.data(), buf, len) == 0;
}

// ---- the element funnel ------------------------------------------------------------

// One element as a JS VALUE, which is where the two BigInt views stop being
// "two more widths": ten kinds answer a Number and these two answer a BigInt, so
// every read path in the runtime funnels through here rather than each one
// keeping its own opinion about `view->get`.
//
// ALLOCATES for a BigInt kind, and the bytes are therefore read BEFORE the
// allocation — the raw `view` pointer is dead from that line on. Out of range is
// `undefined`, which 10.4.5.4 makes an absence rather than an error.
Value rtTypedArrayElement(Value viewVal, uint32_t index) {
    auto* view = viewVal.asObject<TypedArrayHeader>();
    if (index >= view->length) return Value::fromUndefined();
    const ElementKind kind = view->elementKind();
    if (!isBigIntElementKind(kind)) return Value::fromDouble(view->get(index));
    const uint64_t bits = view->rawBits64(index);
    return rtBigIntFromRawBits64(bits, kind == ElementKind::BigInt64);
}

// 10.4.5.16 IntegerIndexedElementSet. The conversion is ToNumber for the ten
// numeric kinds and ToBigInt for the two 64-bit integer ones — 23.2.5.13's
// split, and the one place in the language where a typed-array write THROWS
// (7.1.13 has no Number row) instead of truncating.
//
// Either conversion can run user code, so the view is re-derived through the
// root afterwards and its length re-read — the conversion itself may have
// transferred the buffer away, which zeroes the window. An index that is out
// of range by then is a discarded write, not an error; the
// conversion-before-validity order is 10.4.5.16's own.
void rtTypedArraySetElement(Rooted<Value>& view, uint32_t index, Value value) {
    const ElementKind kind = view.get().asObject<TypedArrayHeader>()->elementKind();
    Rooted<Value> val{value};
    if (isBigIntElementKind(kind)) {
        uint64_t bits = 0;
        if (!rtBigIntToRawBits64(val.get(), bits)) return;
        auto* live = view.get().asObject<TypedArrayHeader>();
        if (index < live->length) live->setRawBits64(index, bits);
        return;
    }
    const double num = rtToNumber(val.get());
    if (rtExceptionPending()) return;
    auto* live = view.get().asObject<TypedArrayHeader>();
    if (index < live->length) live->set(index, num);
}

}  // namespace bronze::runtime
