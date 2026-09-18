// The `RegExp` object (ECMA-262 22.2): the constructor, `RegExp.prototype`
// with `exec`, `test`, `toString`, the flag accessors and the five
// symbol-keyed members, and the constructor's own `escape` and `@@species`.
// `exec` — which every other regular-expression operation in bronze is built
// from — and the compiled-pattern table under it are builtin_regexp_exec.cpp;
// `RegExp.escape` is builtin_regexp_escape.cpp; the symbol-keyed algorithms
// are builtin_regexp_symbols.cpp. builtin_regexp_internal.h is the seam.
//
// A RegExp is an ordinary object with internal slots (regexp.h): its
// [[Prototype]] lives on its shape, `RegExp.prototype` is a real object every
// instance chains to, and `class R extends RegExp` allocates one of these
// headers with the subclass's prototype. The one thing the property paths
// synthesise is `lastIndex` (22.2.4.1), the own data property the matcher
// keeps in the header rather than in a slot.

#include <cstring>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "regex/regex.h"
#include "runtime/builtin_regexp_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/native_base.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// `RegExp`, `RegExp.prototype`, the instance shape every `new RegExp` takes
// its [[Prototype]] from, and what the string members' fast path re-verifies
// (`rtRegExpChainPristine`): the prototype's shape as built, and the slots of
// the six DATA properties whose in-place overwrite bumps no epoch — `exec`
// and the five symbol-keyed algorithms.
struct RegExpIntrinsics {
    Value ctor = Value::fromUndefined();
    Value proto = Value::fromUndefined();
    Shape* instanceShape = nullptr;
    Shape* protoPristineShape = nullptr;
    static constexpr size_t kWitnessCount = 6;
    uint32_t witnessSlot[kWitnessCount] = {};
    Value witnessValue[kWitnessCount] = {Value::fromUndefined(), Value::fromUndefined(),
                                         Value::fromUndefined(), Value::fromUndefined(),
                                         Value::fromUndefined(), Value::fromUndefined()};
    uint64_t pristineEpoch = 0;
    bool pristine = false;
};

thread_local RegExpIntrinsics g_regexp;

void ensureRegExpIntrinsics();

bool isRegExp(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == RegExpHeader::kFlags;
}

// A RegExp whose constructor has RUN: a derived class reading `this` before
// `super()` holds an allocated header with no pattern yet, and 22.2.6's
// members refuse it as they refuse any other receiver without the slots.
bool isInitializedRegExp(Value v) {
    return isRegExp(v) && v.asObject<RegExpHeader>()->programIndex.isNumber();
}

bool requireRegExp(Value self, const char* member) {
    if (isInitializedRegExp(self)) return true;
    rtThrowTypeError(std::string("RegExp.prototype.") + member +
                     " called on an incompatible receiver");
    return false;
}

// 22.2.6.16: `test` is `exec` and a null check. Written as exactly that, so
// the two can never disagree about `lastIndex`.
uint64_t regexpTest(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    if (!requireRegExp(self.get(), "test")) return Value::fromUndefined().rawBits();
    Rooted<Value> input{rtValueToString(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Value result = rtRegExpExec(self, input);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return Value::fromBool(!result.isNull()).rawBits();
}

// 22.2.6.17: `/` + source + `/` + flags, both read as PROPERTIES so a
// subclass's accessor is honoured; the brand check is only that `this` is an
// object.
uint64_t regexpToString(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!self.get().isObject()) {
        return rtThrowTypeError("RegExp.prototype.toString called on an incompatible receiver")
            .rawBits();
    }
    Rooted<Value> sourceKey{rtMakeString("source")};
    Rooted<Value> source{rtValueToString(
        Value(bronze_elem_get(self.get().rawBits(), sourceKey.get().rawBits())))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> flagsKey{rtMakeString("flags")};
    Rooted<Value> flags{
        rtValueToString(Value(bronze_elem_get(self.get().rawBits(), flagsKey.get().rawBits())))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return rtMakeString("/" + rtUtf8Chars(source.get().asString<StringHeader>()) + "/" +
                        rtUtf8Chars(flags.get().asString<StringHeader>()))
        .rawBits();
}

// Annex B.2.4.1 `RegExp.prototype.compile`: defined so the name is a
// function, as node has it, but its body is a refusal — re-initialising a
// pattern in place is a legacy path nothing in the milestones takes.
uint64_t regexpCompileRefusal(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    fatal("unsupported: RegExp.prototype.compile (Annex B.2.4.1; build a new RegExp instead)");
}

// ---- the constructor (22.2.4.1) --------------------------------------------------

// 22.2.4.1 RegExp(pattern, flags). Reached two ways: through `new` — where
// the receiver is the header the running construction allocated from
// NewTarget's prototype, filled in place — and as a plain call, where step 2
// may hand the pattern itself back and the result otherwise takes the
// intrinsic prototype.
uint64_t regexpConstructor(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> receiver{Value(thisBits)};
    Rooted<Value> pattern{args[0]};
    Rooted<Value> flagsArg{args[1]};
    const bool constructing = rtIsNativeConstructReceiver(receiver.get()) && isRegExp(receiver.get());
    const bool patternIsRegExp = isRegExp(pattern.get());

    // Step 2: a plain call with a RegExp whose `constructor` is this very
    // function, and no flags, answers the pattern unchanged.
    if (!constructing && patternIsRegExp && flagsArg.get().isUndefined()) {
        Rooted<Value> ctorKey{rtMakeString("constructor")};
        const Value ctor = Value(bronze_elem_get(pattern.get().rawBits(), ctorKey.get().rawBits()));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (ctor.rawBits() == g_regexp.ctor.rawBits()) return pattern.get().rawBits();
    }

    // Steps 4-6: the source and the flags, from a RegExp's slots or from the
    // arguments. 22.2.4.1 step 5's absent pattern is the EMPTY pattern, not
    // the string "undefined"; what the empty pattern's source reads as is
    // rtInitializeRegExp's.
    Rooted<Value> source;
    std::string flagsText;
    if (patternIsRegExp) {
        source.set(pattern.get().asObject<RegExpHeader>()->source);
        if (flagsArg.get().isUndefined()) {
            flagsText = rtUtf8Chars(
                pattern.get().asObject<RegExpHeader>()->flagsText.asString<StringHeader>());
        }
    } else {
        source.set(pattern.get().isUndefined() ? rtMakeString("") : rtValueToString(pattern.get()));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }
    if (!flagsArg.get().isUndefined()) {
        flagsText = rtUtf8Chars(rtValueToString(flagsArg.get()).asString<StringHeader>());
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    }

    if (constructing) {
        if (!rtInitializeRegExp(receiver, source, flagsText)) return Value::fromUndefined().rawBits();
        return receiver.get().rawBits();
    }
    return rtMakeRegExp(source, flagsText).rawBits();
}

// ---- the flag accessors (22.2.6.4.1) ------------------------------------------------

// RegExpHasFlag steps 1-3: the flags of a RegExp receiver; null for
// `RegExp.prototype` itself (the getter answers `undefined`) and null with a
// TypeError pending for anything else.
const regex::Flags* flagsOfReceiver(Value self, const char* name) {
    if (isInitializedRegExp(self)) return &regex::patternFlags(rtRegExpPattern(self));
    if (self.isObject() && self.rawBits() == g_regexp.proto.rawBits()) return nullptr;
    rtThrowTypeError(std::string("RegExp.prototype.") + name +
                     " getter called on an incompatible receiver");
    return nullptr;
}

// One getter per flag, so each accessor has a code pointer of its own and
// nothing dispatches on a string.
#define BRONZE_REGEXP_FLAG_GETTER(fn, field, text)                                            \
    uint64_t fn(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {                     \
        const regex::Flags* flags = flagsOfReceiver(Value(thisBits), text);                   \
        if (!flags) return Value::fromUndefined().rawBits();                                  \
        return Value::fromBool(flags->field).rawBits();                                       \
    }
BRONZE_REGEXP_FLAG_GETTER(dotAllGetter, dotAll, "dotAll")
BRONZE_REGEXP_FLAG_GETTER(globalGetter, global, "global")
BRONZE_REGEXP_FLAG_GETTER(hasIndicesGetter, hasIndices, "hasIndices")
BRONZE_REGEXP_FLAG_GETTER(ignoreCaseGetter, ignoreCase, "ignoreCase")
BRONZE_REGEXP_FLAG_GETTER(multilineGetter, multiline, "multiline")
BRONZE_REGEXP_FLAG_GETTER(stickyGetter, sticky, "sticky")
BRONZE_REGEXP_FLAG_GETTER(unicodeGetter, unicode, "unicode")
BRONZE_REGEXP_FLAG_GETTER(unicodeSetsGetter, unicodeSets, "unicodeSets")
#undef BRONZE_REGEXP_FLAG_GETTER

// 22.2.6.13 `source`: the escaped pattern text (22.2.6.13.1, applied when the
// pattern was made); `RegExp.prototype` itself reads "(?:)".
uint64_t sourceGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    const Value self(thisBits);
    if (isInitializedRegExp(self)) return self.asObject<RegExpHeader>()->source.rawBits();
    if (self.isObject() && self.rawBits() == g_regexp.proto.rawBits()) {
        return rtMakeString("(?:)").rawBits();
    }
    return rtThrowTypeError("RegExp.prototype.source getter called on an incompatible receiver")
        .rawBits();
}

// 22.2.6.4 `flags`: eight property READS on the receiver, in the order
// "dgimsuvy", each ToBoolean'd — so a subclass's own `global` accessor shows
// up here, and any object at all can be asked. A pristine RegExp answers
// from its canonical flags text, which is the same eight answers.
uint64_t flagsGetter(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Rooted<Value> self{Value(thisBits)};
    if (!self.get().isObject()) {
        return rtThrowTypeError("RegExp.prototype.flags getter called on a non-object").rawBits();
    }
    if (isInitializedRegExp(self.get()) &&
        rtRegExpChainPristine(self.get().asObject<RegExpHeader>())) {
        return self.get().asObject<RegExpHeader>()->flagsText.rawBits();
    }
    struct FlagName {
        const char* name;
        char letter;
    };
    static const FlagName kOrder[] = {
        {"hasIndices", 'd'}, {"global", 'g'},  {"ignoreCase", 'i'},  {"multiline", 'm'},
        {"dotAll", 's'},     {"unicode", 'u'}, {"unicodeSets", 'v'}, {"sticky", 'y'},
    };
    std::string out;
    for (const FlagName& f : kOrder) {
        Rooted<Value> key{rtMakeString(f.name)};
        const Value v = Value(bronze_elem_get(self.get().rawBits(), key.get().rawBits()));
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (bronze_truthy(v.rawBits())) out.push_back(f.letter);
    }
    return rtMakeString(out).rawBits();
}

// ---- assembling the intrinsics ---------------------------------------------------

void defineGetter(Rooted<Value>& proto, const char* name, bronze_fn_code code,
                  const char* getterName) {
    Rooted<Value> key{rtMakeString(name)};
    Rooted<Value> getter{rtNativeFunction(code, 0, getterName, 0)};
    Rooted<Value> setter{Value::fromUndefined()};
    ObjectHeader::defineAccessor(rtHeap(), rtArena(), proto, key, getter, setter,
                                 /*enumerable=*/false);
}

uint32_t slotOf(Value obj, PropertyKey key) {
    PropertyInfo info;
    if (!obj.asObject<ObjectHeader>()->shape->lookupProperty(key, info)) {
        fatal("internal: a RegExp.prototype member was not installed");
    }
    return info.slot;
}

const NativeMethod kRegExpMethods[] = {
    {"exec", rtRegExpExecBody, 1, 1},
    {"test", regexpTest, 1, 1},
    {"toString", regexpToString, 0, 0},
    {"compile", regexpCompileRefusal, 0, 2},
};

void ensureRegExpIntrinsics() {
    if (g_regexp.proto.isObject()) return;

    Shape* protoShape = rtNewRootShape(rtObjectPrototype());
    protoShape->used_as_prototype = true;
    ObjectHeader* protoObj = ObjectHeader::create(rtHeap(), rtArena(), protoShape);
    protoObj->header.flags = HeapKind::Plain;
    Rooted<Value> proto{Value::fromObject(protoObj)};
    g_regexp.proto = proto.get();
    rtHeap().add_permanent_root(&g_regexp.proto);

    // Arity 2: 22.2.4's `length` is 2, and both arguments are read as absent-
    // or-undefined either way.
    Rooted<Value> ctor{rtNativeFunction(regexpConstructor, 2, "RegExp", 2)};
    g_regexp.ctor = ctor.get();
    rtHeap().add_permanent_root(&g_regexp.ctor);
    {
        // 22.2.6.2, a DEFINITION so it does not write through.
        Rooted<Value> key{rtMakeString("constructor")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
    rtDefineMethods(proto, kRegExpMethods, std::size(kRegExpMethods));
    // 22.2.6, in the clause's own order; the accessors have getters only.
    defineGetter(proto, "dotAll", dotAllGetter, "get dotAll");
    defineGetter(proto, "flags", flagsGetter, "get flags");
    defineGetter(proto, "global", globalGetter, "get global");
    defineGetter(proto, "hasIndices", hasIndicesGetter, "get hasIndices");
    defineGetter(proto, "ignoreCase", ignoreCaseGetter, "get ignoreCase");
    defineGetter(proto, "multiline", multilineGetter, "get multiline");
    defineGetter(proto, "source", sourceGetter, "get source");
    defineGetter(proto, "sticky", stickyGetter, "get sticky");
    defineGetter(proto, "unicode", unicodeGetter, "get unicode");
    defineGetter(proto, "unicodeSets", unicodeSetsGetter, "get unicodeSets");
    rtInstallRegExpSymbolMethods(proto);

    // The six data properties the string members' fast path must see
    // unchanged: `exec` and the five symbol-keyed algorithms. In-place
    // overwrites of a data property bump no epoch, so their VALUES are what
    // reverify compares, at the slots recorded here.
    {
        Rooted<Value> execKey{rtMakeString("exec")};
        g_regexp.witnessSlot[0] =
            slotOf(proto.get(), PropertyKey::forString(execKey.get().asString<StringHeader>()));
        SymbolHeader* (*const symbols[])() = {rtSymbolMatch, rtSymbolMatchAll, rtSymbolReplace,
                                              rtSymbolSearch, rtSymbolSplit};
        for (size_t i = 0; i < 5; ++i) {
            g_regexp.witnessSlot[i + 1] = slotOf(proto.get(), PropertyKey::forSymbol(symbols[i]()));
        }
        for (size_t i = 0; i < RegExpIntrinsics::kWitnessCount; ++i) {
            g_regexp.witnessValue[i] =
                proto.get().asObject<ObjectHeader>()->getSlot(g_regexp.witnessSlot[i]);
            rtHeap().add_permanent_root(&g_regexp.witnessValue[i]);
        }
    }

    // 22.2.5: `escape` and the `@@species` accessor, ordinary own properties
    // of the constructor's box, where a subclass reaches them by the chain
    // `extends` builds.
    rtEnsureFunctionProperties(ctor);
    {
        Rooted<Value> box{ctor.get().asObject<FunctionHeader>()->properties};
        const NativeMethod statics[] = {{"escape", rtRegExpEscapeBody, 1, 1}};
        rtDefineMethods(box, statics, std::size(statics));
        rtDefineSpeciesGetter(ctor);
    }

    FunctionHeader* fn = ctor.get().asObject<FunctionHeader>();
    fn->prototype = proto.get();
    // 22.2.5.1: non-writable, non-enumerable, non-configurable.
    fn->prototype_readonly = true;
    fn->native_base = NativeBase::RegExp;
    fn->instance_shape = rtNewRootShape(proto.get());
    g_regexp.instanceShape = fn->instance_shape;
    g_regexp.protoPristineShape = proto.get().asObject<ObjectHeader>()->shape;
    g_regexp.pristineEpoch = protoMutationEpoch();
    g_regexp.pristine = true;
}

// Re-verify the prototype's shape. Called only when the epoch moved, which
// any add, delete, redefinition or prototype swap on ANY object serving as a
// prototype does — so the common case is one compare against the latch.
bool reverifyPristine() {
    g_regexp.pristine =
        g_regexp.proto.asObject<ObjectHeader>()->shape == g_regexp.protoPristineShape;
    g_regexp.pristineEpoch = protoMutationEpoch();
    return g_regexp.pristine;
}

}  // namespace

Value rtRegExpConstructor(const std::string& name) {
    if (name != "RegExp") return Value::fromUndefined();
    ensureRegExpIntrinsics();
    return g_regexp.ctor;
}

Value rtRegExpPrototypeObject() {
    ensureRegExpIntrinsics();
    return g_regexp.proto;
}

Shape* rtRegExpInstanceShape() {
    ensureRegExpIntrinsics();
    return g_regexp.instanceShape;
}

bool rtRegExpChainPristine(const RegExpHeader* re) noexcept {
    if (re->object.shape != g_regexp.instanceShape) return false;
    if (g_regexp.pristineEpoch != protoMutationEpoch() && !reverifyPristine()) return false;
    if (!g_regexp.pristine) return false;
    // The shape being as built says the six slots ARE `exec` and the five
    // symbol keys; whether they still hold the intrinsics is a value compare
    // on every ask, because `RegExp.prototype.exec = f` moves no shape and
    // bumps no epoch.
    const auto* proto = g_regexp.proto.asObject<ObjectHeader>();
    for (size_t i = 0; i < RegExpIntrinsics::kWitnessCount; ++i) {
        if (proto->getSlot(g_regexp.witnessSlot[i]).rawBits() !=
            g_regexp.witnessValue[i].rawBits()) {
            return false;
        }
    }
    return true;
}

std::string rtRegExpText(Value re) {
    const auto* header = re.asObject<RegExpHeader>();
    return "/" + rtUtf8Chars(header->source.asString<StringHeader>()) + "/" +
           rtUtf8Chars(header->flagsText.asString<StringHeader>());
}

}  // namespace bronze::runtime
