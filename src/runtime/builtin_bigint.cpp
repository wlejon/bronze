// The JS surface of BigInt (ECMA-262 21.2): the `BigInt` function, its two
// statics, and `BigInt.prototype`.
//
// `BigInt.prototype` is a REAL object on the real chain, the way
// `Number.prototype` is (builtin_wrappers.cpp) — a primitive BigInt reaches it
// by the ordinary prototype walk, so `(255n).toString(16)` and
// `BigInt.prototype.toString.call(255n, 16)` are one function found in one
// place. What it is NOT is a wrapper: 21.2.3 makes it an ORDINARY object with
// no [[BigIntData]] slot, because 21.2.1.1 makes `new BigInt()` a TypeError and
// there is therefore no wrapper object for a slot to live on.

#include <cstdint>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/bigint.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/number_format.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

thread_local Value g_bigIntPrototype = Value::fromUndefined();
thread_local Value g_bigIntFunction = Value::fromUndefined();

// 7.1.13 ToBigInt, for a value that is ALREADY primitive. The Number case is
// its caller's: 21.2.1.1 routes a Number through NumberToBigInt (which accepts
// an integer and refuses a fraction) where 7.1.13 refuses every Number outright,
// and the two are different answers for `BigInt(1)`.
bool toBigIntPrimitive(Value prim, BigNum& out) {
    if (prim.isBigInt()) {
        out = rtBigIntValue(prim);
        return true;
    }
    if (prim.isBool()) {
        out = BigNum::fromInt64(prim.asBool() ? 1 : 0);
        return true;
    }
    if (prim.isString()) {
        // 7.1.13 step 1's String row: StringToBigInt, and *undefined* — a
        // string that is not a StringIntegerLiteral — is the SyntaxError the
        // table names, not a TypeError and not NaN.
        if (!rtStringToBigInt(rtAsciiChars(prim.asString<StringHeader>()), out)) {
            rtThrowError(ErrorKind::SyntaxError, "Cannot convert this string to a BigInt");
            return false;
        }
        return true;
    }
    if (prim.isUndefined() || prim.isNull()) {
        rtThrowTypeError(std::string("Cannot convert ") +
                         (prim.isNull() ? "null" : "undefined") + " to a BigInt");
        return false;
    }
    if (prim.isSymbol()) {
        rtThrowTypeError("Cannot convert a Symbol value to a BigInt");
        return false;
    }
    rtThrowTypeError("Cannot convert this value to a BigInt");
    return false;
}

// 21.2.1.1 BigInt(value), the `new`-less form — the only form there is, since
// `bronze_construct` refuses the other by name.
uint64_t bigIntConstructorBody(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> input{args[0]};
    // Step 2 is ToPrimitive with hint NUMBER, so an object with a `valueOf`
    // converts through it — and that call can collect, which is why the
    // primitive is taken through a root before anything below reads it.
    Rooted<Value> prim{rtToPrimitive(input, ToPrimitiveHint::Number)};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();

    BigNum value;
    if (prim.get().isNumber() || prim.get().isInt32()) {
        // Step 3: NumberToBigInt (21.2.1.1.1), which is a RangeError for
        // anything that is not an integer — including NaN and both infinities.
        // Deliberately not a truncation: `BigInt(1.5)` losing the .5 silently
        // is exactly the class of wrong answer this type exists to prevent.
        const double n = prim.get().isInt32()
                             ? static_cast<double>(static_cast<int32_t>(prim.get().payload()))
                             : prim.get().asNumber();
        if (!BigNum::fromDoubleExact(n, value)) {
            // The JS spelling of the number, not C's: `std::to_string` writes
            // 1.5 as "1.500000" and 1e21 as a 22-digit integer, and the
            // deterministic-output rule owns every number bronze prints.
            char buf[64];
            const size_t len = formatJsNumber(n, buf);
            return rtThrowRangeError("The number " + std::string(buf, len) +
                                     " cannot be converted to a BigInt because it is not an "
                                     "integer")
                .rawBits();
        }
    } else if (!toBigIntPrimitive(prim.get(), value)) {
        return Value::fromUndefined().rawBits();
    }
    return rtMakeBigInt(value).rawBits();
}

}  // namespace

bool rtToBigInt(Value v, BigNum& out) {
    Rooted<Value> input{v};
    Rooted<Value> prim{rtToPrimitive(input, ToPrimitiveHint::Number)};
    if (rtExceptionPending()) return false;
    if (prim.get().isNumber() || prim.get().isInt32()) {
        // 7.1.13's Number row is a TypeError with no conversion at all. That is
        // what makes `view.setBigInt64(0, 1)` and `BigInt.asIntN(8, 255)` throw
        // where the same calls with `1n` and `255n` work.
        rtThrowTypeError("Cannot convert a Number to a BigInt");
        return false;
    }
    return toBigIntPrimitive(prim.get(), out);
}

namespace {

// The `bits` argument of 21.2.2.1 and 21.2.2.2: ToIndex, which is a RangeError
// for a negative or fractional count rather than a truncation.
bool toBits(Value v, uint64_t& out) {
    const double n = rtToNumber(v);
    if (rtExceptionPending()) return false;
    const double integer = std::isnan(n) ? 0.0 : std::trunc(n);
    if (integer < 0.0 || integer > 9007199254740991.0) {
        rtThrowRangeError("Invalid value: not a valid number of bits");
        return false;
    }
    out = static_cast<uint64_t>(integer);
    return true;
}

// The shared body of asIntN and asUintN: both take (bits, bigint) and differ
// only in how the window is read back.
uint64_t asNBody(uint32_t argc, const uint64_t* argv, bool signedResult) {
    RootedArgs args(argc, argv);
    uint64_t bits = 0;
    if (!toBits(args[0], bits)) return Value::fromUndefined().rawBits();
    // Step 2 is ToBigInt, so a Number argument is refused rather than
    // converted: `BigInt.asIntN(8, 255)` is a TypeError, not -1n.
    BigNum value;
    if (!rtToBigInt(args[1], value)) return Value::fromUndefined().rawBits();

    BigNumError err = BigNumError::None;
    const BigNum wrapped = signedResult ? BigNum::asIntN(bits, value, err)
                                        : BigNum::asUintN(bits, value, err);
    if (err != BigNumError::None) {
        return rtThrowRangeError("Maximum BigInt size exceeded").rawBits();
    }
    return rtMakeBigInt(wrapped).rawBits();
}

uint64_t bigIntAsIntN(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return asNBody(argc, argv, /*signedResult=*/true);
}

uint64_t bigIntAsUintN(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    return asNBody(argc, argv, /*signedResult=*/false);
}

// 21.2.3.4 thisBigIntValue. There is no wrapper object, so the receiver is
// either the primitive or an incompatible one — which is the whole of the
// check.
bool thisBigInt(Value self, const char* method, Value& out) {
    if (!self.isBigInt()) {
        rtThrowTypeError(std::string("BigInt.prototype.") + method +
                         " called on a value that is not a BigInt");
        return false;
    }
    out = self;
    return true;
}

uint64_t bigIntProtoToString(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Value self;
    if (!thisBigInt(Value(thisBits), "toString", self)) {
        return Value::fromUndefined().rawBits();
    }
    int radix = 10;
    if (!args[0].isUndefined()) {
        const double r = rtToNumber(args[0]);
        if (rtExceptionPending()) return Value::fromUndefined().rawBits();
        if (!(r >= 2.0 && r <= 36.0)) {
            return rtThrowRangeError("toString() radix must be between 2 and 36").rawBits();
        }
        radix = static_cast<int>(r);
    }
    return rtMakeString(rtBigIntToString(self, radix)).rawBits();
}

uint64_t bigIntProtoValueOf(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self;
    if (!thisBigInt(Value(thisBits), "valueOf", self)) {
        return Value::fromUndefined().rawBits();
    }
    return self.rawBits();
}

// 21.2.3.2 toLocaleString, refused by name rather than aliased to `toString`.
// Its answer is a LOCALE's — digit grouping and the digit set both — and
// bronze's output is deterministic by rule, so an implementation of it would
// have to be a lie about which locale it implemented.
uint64_t bigIntProtoToLocaleString(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    fatal("unsupported: BigInt.prototype.toLocaleString (its result is locale-dependent, and "
          "bronze's output is deterministic by rule — use toString())");
}

const NativeMethod kBigIntProtoMethods[] = {
    {"toString", bigIntProtoToString, 0},
    {"toLocaleString", bigIntProtoToLocaleString, 0},
    {"valueOf", bigIntProtoValueOf, 0},
};

struct BigIntStatic {
    const char* name;
    bronze_fn_code code;
    uint32_t arity;
};

const BigIntStatic kBigIntStatics[] = {
    {"asIntN", bigIntAsIntN, 2},
    {"asUintN", bigIntAsUintN, 2},
};

// The two prototype members 21.2.3 defines that bronze does not answer from
// the object above. `@@toStringTag` is one of them and it IS installed, so the
// list is empty today — kept as the seam every other intrinsic has, so that a
// member added to the clause is refused by name here rather than read as
// `undefined`.
const char* const kBigIntProtoUnimplemented[] = {nullptr};

void ensureBigIntIntrinsics() {
    if (g_bigIntPrototype.isObject()) return;

    Rooted<Value> objectProto{rtObjectPrototype()};
    Shape* protoShape = rtNewRootShape(objectProto.get());
    ObjectHeader* proto = ObjectHeader::create(rtHeap(), rtArena(), protoShape);
    proto->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
    Rooted<Value> protoRoot{Value::fromObject(proto)};

    // Published before anything is installed, so that `constructor` below can
    // ask for the function object — which asks for this prototype — without
    // the two building each other forever. The same half-built publication
    // `ensureWrapperIntrinsics` uses.
    g_bigIntPrototype = protoRoot.get();
    rtHeap().add_permanent_root(&g_bigIntPrototype);

    rtDefineMethods(protoRoot, kBigIntProtoMethods, std::size(kBigIntProtoMethods));
    // 21.2.3.5: the tag is "BigInt", which is what makes
    // `Object.prototype.toString.call(1n)` be "[object BigInt]".
    rtDefineToStringTag(protoRoot, "BigInt");

    Rooted<Value> key{rtMakeString("constructor")};
    Rooted<Value> ctor{rtBigIntConstructorObject()};
    protoRoot.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, ctor, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
}

}  // namespace

Value rtBigIntPrototype() {
    ensureBigIntIntrinsics();
    return g_bigIntPrototype;
}

Value rtBigIntConstructorObject() {
    if (g_bigIntFunction.isObject()) return g_bigIntFunction;
    // Arity 0, like every other native constructor here: a variadic native
    // must not be padded, or `BigInt()` would arrive with an `undefined` the
    // language says it was not given.
    Rooted<Value> fn{rtNativeFunction(bigIntConstructorBody, 0)};
    g_bigIntFunction = fn.get();
    rtHeap().add_permanent_root(&g_bigIntFunction);

    {
        Rooted<Value> proto{rtBigIntPrototype()};
        FunctionHeader* live = fn.get().asObject<FunctionHeader>();
        live->prototype = proto.get();
        // Set with the prototype because `rtEnsureFunctionPrototype`'s guard
        // tests both; nothing ever builds an instance from the shape, since
        // `new BigInt()` is refused.
        live->instance_shape = rtRootShapeForPrototype(proto.get());
    }

    rtEnsureFunctionProperties(fn);
    Rooted<Value> props{fn.get().asObject<FunctionHeader>()->properties};
    for (const BigIntStatic& s : kBigIntStatics) {
        Rooted<Value> key{rtMakeString(s.name)};
        Rooted<Value> val{rtNativeFunction(s.code, s.arity)};
        props.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, nullptr,
                                                      /*enumerable=*/false, /*defineOwn=*/true);
    }
    return g_bigIntFunction;
}

Value rtBigIntConstructor(const std::string& name) {
    if (name != "BigInt") return Value::fromUndefined();
    return rtBigIntConstructorObject();
}

bool rtIsBigIntConstructor(Value fn) {
    if (!fn.isObject() || fn.asObject<HeapObjectHeader>()->flags != HeapKind::Function) {
        return false;
    }
    return fn.asObject<FunctionHeader>()->code == bigIntConstructorBody;
}

void rtCheckBigIntProtoMember(const std::string& key) {
    for (const char* name : kBigIntProtoUnimplemented) {
        if (name && key == name) {
            fatal((std::string("unsupported: BigInt.prototype.") + key + " is not implemented")
                      .c_str());
        }
    }
}

}  // namespace bronze::runtime
