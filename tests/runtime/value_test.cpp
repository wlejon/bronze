#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/value.h"

using namespace bronze;

TEST_CASE("tag values pin ratchet") {
    CHECK(static_cast<uint16_t>(Tag::Object) == 0xFFF1);
    CHECK(static_cast<uint16_t>(Tag::String) == 0xFFF2);
    CHECK(static_cast<uint16_t>(Tag::Int32) == 0xFFF3);
    CHECK(static_cast<uint16_t>(Tag::Bool) == 0xFFF4);
    CHECK(static_cast<uint16_t>(Tag::Null) == 0xFFF5);
    CHECK(static_cast<uint16_t>(Tag::Undefined) == 0xFFF6);
    CHECK(static_cast<uint16_t>(Tag::Hole) == 0xFFF7);
    CHECK(static_cast<uint16_t>(Tag::Symbol) == 0xFFF8);
}

TEST_CASE("singletons encoding and queries") {
    auto nullVal = Value::fromNull();
    CHECK(nullVal.isNull());
    CHECK_FALSE(nullVal.isUndefined());
    CHECK_FALSE(nullVal.isNumber());
    CHECK(nullVal.tag() == 0xFFF5);
    CHECK(nullVal.payload() == 0);
    CHECK(nullVal.rawBits() == 0xFFF5000000000000ULL);

    auto undefinedVal = Value::fromUndefined();
    CHECK(undefinedVal.isUndefined());
    CHECK_FALSE(undefinedVal.isNull());
    CHECK_FALSE(undefinedVal.isNumber());
    CHECK(undefinedVal.tag() == 0xFFF6);
    CHECK(undefinedVal.payload() == 0);
    CHECK(undefinedVal.rawBits() == 0xFFF6000000000000ULL);

    Value defaultVal;
    CHECK(defaultVal.isUndefined());
    CHECK(defaultVal == undefinedVal);

    auto holeVal = Value::fromHole();
    CHECK(holeVal.isHole());
    CHECK_FALSE(holeVal.isUndefined());
    CHECK_FALSE(holeVal.isNumber());
    CHECK(holeVal.tag() == 0xFFF7);
    CHECK(holeVal.payload() == 0);
    CHECK(holeVal.rawBits() == 0xFFF7000000000000ULL);

    auto trueVal = Value::fromBool(true);
    CHECK(trueVal.isBool());
    CHECK(trueVal.asBool() == true);
    CHECK(trueVal.tag() == 0xFFF4);
    CHECK(trueVal.payload() == 1);
    CHECK(trueVal.rawBits() == 0xFFF4000000000001ULL);

    auto falseVal = Value::fromBool(false);
    CHECK(falseVal.isBool());
    CHECK(falseVal.asBool() == false);
    CHECK(falseVal.tag() == 0xFFF4);
    CHECK(falseVal.payload() == 0);
    CHECK(falseVal.rawBits() == 0xFFF4000000000000ULL);
}

TEST_CASE("double numbers encoding canonicalization and special values") {
    auto vZero = Value::fromDouble(0.0);
    CHECK(vZero.isNumber());
    CHECK(vZero.asNumber() == 0.0);
    CHECK(vZero.rawBits() == 0x0000000000000000ULL);

    auto vNegZero = Value::fromDouble(-0.0);
    CHECK(vNegZero.isNumber());
    CHECK(vNegZero.asNumber() == 0.0);
    CHECK(std::signbit(vNegZero.asNumber()));
    CHECK(vNegZero.rawBits() == 0x8000000000000000ULL);

    auto v42 = Value::fromDouble(42.5);
    CHECK(v42.isNumber());
    CHECK(v42.asNumber() == 42.5);

    double inf = std::numeric_limits<double>::infinity();
    auto vInf = Value::fromDouble(inf);
    CHECK(vInf.isNumber());
    CHECK(vInf.asNumber() == inf);
    CHECK(vInf.rawBits() == 0x7FF0000000000000ULL);

    auto vNegInf = Value::fromDouble(-inf);
    CHECK(vNegInf.isNumber());
    CHECK(vNegInf.asNumber() == -inf);
    CHECK(vNegInf.rawBits() == 0xFFF0000000000000ULL);

    double nan1 = std::numeric_limits<double>::quiet_NaN();
    auto vNan1 = Value::fromDouble(nan1);
    CHECK(vNan1.isNumber());
    CHECK(std::isnan(vNan1.asNumber()));
    CHECK(vNan1.rawBits() == kCanonicalNaNBits);

    double nanPayload = std::bit_cast<double>(0x7FF8123456789ABCULL);
    auto vNanPayload = Value::fromDouble(nanPayload);
    CHECK(vNanPayload.isNumber());
    CHECK(std::isnan(vNanPayload.asNumber()));
    CHECK(vNanPayload.rawBits() == kCanonicalNaNBits);

    double nanTagAlias = std::bit_cast<double>(0xFFF8000000000000ULL);
    auto vNanTagAlias = Value::fromDouble(nanTagAlias);
    CHECK(vNanTagAlias.isNumber());
    CHECK_FALSE(vNanTagAlias.isSymbol());
    CHECK(vNanTagAlias.rawBits() == kCanonicalNaNBits);
}

TEST_CASE("pointer encoding and decoding for 48-bit address range") {
    uintptr_t addresses[] = {
        0x0000000000000000ULL,
        0x0000000000000004ULL,
        0x00007FFF12345678ULL,
        0x0000FFFFFFFFFFFFULL
    };

    for (auto addr : addresses) {
        const void* ptr = reinterpret_cast<const void*>(addr);

        auto objVal = Value::fromObject(ptr);
        CHECK(objVal.isObject());
        CHECK(objVal.isPointer());
        CHECK_FALSE(objVal.isNumber());
        CHECK(objVal.tag() == 0xFFF1);
        CHECK(objVal.payload() == addr);
        CHECK(objVal.asObject<const void>() == ptr);

        auto strVal = Value::fromString(ptr);
        CHECK(strVal.isString());
        CHECK(strVal.isPointer());
        CHECK_FALSE(strVal.isNumber());
        CHECK(strVal.tag() == 0xFFF2);
        CHECK(strVal.payload() == addr);
        CHECK(strVal.asString<const void>() == ptr);

        auto symVal = Value::fromSymbol(ptr);
        CHECK(symVal.isSymbol());
        CHECK(symVal.isPointer());
        CHECK_FALSE(symVal.isNumber());
        CHECK(symVal.tag() == 0xFFF8);
        CHECK(symVal.payload() == addr);
        CHECK(symVal.asSymbol<const void>() == ptr);
    }
}

TEST_CASE("bronze_truthy tests") {
    CHECK_FALSE(bronze_truthy(Value::fromUndefined().rawBits()));
    CHECK_FALSE(bronze_truthy(Value::fromNull().rawBits()));
    CHECK_FALSE(bronze_truthy(Value::fromBool(false).rawBits()));
    CHECK(bronze_truthy(Value::fromBool(true).rawBits()));
    CHECK_FALSE(bronze_truthy(Value::fromDouble(0.0).rawBits()));
    CHECK_FALSE(bronze_truthy(Value::fromDouble(-0.0).rawBits()));
    CHECK_FALSE(bronze_truthy(Value::fromDouble(std::numeric_limits<double>::quiet_NaN()).rawBits()));
    CHECK(bronze_truthy(Value::fromDouble(1.0).rawBits()));
    CHECK(bronze_truthy(Value::fromDouble(-5.5).rawBits()));
    uint64_t emptyObj = bronze_create_object();
    CHECK(bronze_truthy(emptyObj));
    uint64_t emptyStr = bronze_box_str("");
    CHECK_FALSE(bronze_truthy(emptyStr));
    uint64_t zeroStr = bronze_box_str("0");
    CHECK(bronze_truthy(zeroStr));
}


TEST_CASE("bronze_strict_eq follows JS === semantics") {
    // numbers: value equality, NaN never equal, +0 === -0
    CHECK(bronze_strict_eq(Value::fromDouble(5.0).rawBits(), Value::fromDouble(5.0).rawBits()));
    CHECK_FALSE(bronze_strict_eq(Value::fromDouble(5.0).rawBits(), Value::fromDouble(6.0).rawBits()));
    const uint64_t nanBits = Value::fromDouble(std::numeric_limits<double>::quiet_NaN()).rawBits();
    CHECK_FALSE(bronze_strict_eq(nanBits, nanBits));
    CHECK(bronze_strict_eq(Value::fromDouble(0.0).rawBits(), Value::fromDouble(-0.0).rawBits()));

    // strings: content equality, not identity. Every value held across a
    // subsequent allocation must be rooted — under BRONZE_GC_STRESS each
    // allocation collects, and an unrooted pointer's address gets recycled.
    ShadowStackFrame frame;
    Rooted<Value> a1(Value::fromRawBits(bronze_box_str("abc")));
    Rooted<Value> a2(Value::fromRawBits(bronze_box_str("abc")));
    Rooted<Value> b(Value::fromRawBits(bronze_box_str("abd")));
    CHECK(bronze_strict_eq(a1.get().rawBits(), a2.get().rawBits()));
    CHECK_FALSE(bronze_strict_eq(a1.get().rawBits(), b.get().rawBits()));

    // mixed tags are never strictly equal
    CHECK_FALSE(bronze_strict_eq(Value::fromDouble(0.0).rawBits(), Value::fromBool(false).rawBits()));
    CHECK_FALSE(bronze_strict_eq(Value::fromNull().rawBits(), Value::fromUndefined().rawBits()));
    CHECK_FALSE(bronze_strict_eq(bronze_box_str("5"), Value::fromDouble(5.0).rawBits()));

    // identity cases
    CHECK(bronze_strict_eq(Value::fromNull().rawBits(), Value::fromNull().rawBits()));
    CHECK(bronze_strict_eq(Value::fromUndefined().rawBits(), Value::fromUndefined().rawBits()));
    Rooted<Value> obj(Value::fromRawBits(bronze_create_object()));
    CHECK(bronze_strict_eq(obj.get().rawBits(), obj.get().rawBits()));
    CHECK_FALSE(bronze_strict_eq(obj.get().rawBits(), bronze_create_object()));
}
