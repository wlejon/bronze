// Runtime tests for wide inline object capacity (BRONZE_ABI_OBJ_OVERFLOW_SLOT),
// 3-way polymorphic property write inline caches + harvest, SLOT method IC
// closure code-pointer harvesting, zero-allocation Function.prototype/ASCII
// member queries, and the GC-safe `bronze_instanceof` shape cache.

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "abi/bronze_feedback_site.h"
#include "runtime/builtin_object.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/fn_source.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/ic_harvest.h"
#include "runtime/ic_hot.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/type_feedback.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

uint64_t noopCtorBody(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromUndefined().rawBits();
}

uint64_t closureAlphaBody(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    const double x = argc > 0 ? Value(argv[0]).asNumber() : 0.0;
    return Value::fromDouble(x + 10.0).rawBits();
}

uint64_t closureBetaBody(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    const double x = argc > 0 ? Value(argv[0]).asNumber() : 0.0;
    return Value::fromDouble(x + 20.0).rawBits();
}

uint64_t customHasInstanceTrue(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromBool(true).rawBits();
}

uint64_t customHasInstanceFalse(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromBool(false).rawBits();
}

// Builds a valid IC identity record in memory with an empty path so harvesting
// can be armed in memory via `rtIcHarvestConfigure` and disarmed cleanly.
std::vector<uint8_t> makeIdentityRecord(uint64_t digest, uint32_t siteCount) {
    std::vector<uint8_t> buf(BRONZE_ABI_IC_IDENTITY_PATH_OFFSET + 8, 0);
    const uint32_t version = BRONZE_ABI_IC_IDENTITY_VERSION;
    const uint32_t flags = 0;
    const uint32_t pathLen = 0;
    const bronze_code_range* ranges = nullptr;
    const uint32_t rangeCount = 0;
    std::memcpy(buf.data() + 0, &version, 4);
    std::memcpy(buf.data() + BRONZE_ABI_IC_IDENTITY_FLAGS_OFFSET, &flags, 4);
    std::memcpy(buf.data() + BRONZE_ABI_IC_IDENTITY_SITES_OFFSET, &siteCount, 4);
    std::memcpy(buf.data() + BRONZE_ABI_IC_IDENTITY_PATHLEN_OFFSET, &pathLen, 4);
    std::memcpy(buf.data() + BRONZE_ABI_IC_IDENTITY_DIGEST_OFFSET, &digest, 8);
    std::memcpy(buf.data() + BRONZE_ABI_IC_IDENTITY_RANGES_OFFSET, &ranges, sizeof(ranges));
    std::memcpy(buf.data() + BRONZE_ABI_IC_IDENTITY_RANGE_COUNT_OFFSET, &rangeCount, 4);
    return buf;
}

Value makeConstructor(const char* name, uint32_t length = 0) {
    const uint32_t nameKey = bronze_register_key_string(name);
    return Value(bronze_create_function(noopCtorBody, length, length, nameKey,
                                        BRONZE_ABI_FN_FLAGS_ORDINARY,
                                        Value::fromUndefined().rawBits()));
}

}  // namespace

TEST_CASE("inline object capacity covers 62 fields inline before spilling to overflow") {
    NonMovingArena arena;
    Heap heap;
    ShadowStackFrame frame;

    CHECK(ObjectHeader::kOverflowSlot == 62);
    CHECK((ObjectHeader::kOverflowSlot & BRONZE_ABI_FN_INLINE_SLOTS_MASK) ==
          ObjectHeader::kOverflowSlot);

    Shape* root = Shape::createRoot(arena, Value::fromUndefined(), ObjectHeader::kOverflowSlot);
    CHECK(root->inline_slots == ObjectHeader::kOverflowSlot);
    Rooted<ObjectHeader*> obj(ObjectHeader::create(heap, arena, root));

    for (uint32_t i = 0; i < ObjectHeader::kOverflowSlot; ++i) {
        const std::string name = "f" + std::to_string(i);
        Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, name)));
        Rooted<Value> val(Value::fromDouble(static_cast<double>(i) * 1.25));
        obj = obj.get()->setProp(heap, arena, key, val);
    }
    CHECK(obj.get()->overflowCapacity() == 0);
    CHECK(obj.get()->shape->slot_index == ObjectHeader::kOverflowSlot - 1);

    // The 63rd field crosses kOverflowSlot and allocates the overflow block.
    {
        Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, "f_spill")));
        Rooted<Value> val(Value::fromDouble(999.5));
        obj = obj.get()->setProp(heap, arena, key, val);
    }
    CHECK(obj.get()->shape->slot_index == ObjectHeader::kOverflowSlot);
    CHECK(obj.get()->overflowCapacity() >= 1);

    for (uint32_t i = 0; i < ObjectHeader::kOverflowSlot; ++i) {
        const std::string name = "f" + std::to_string(i);
        Rooted<Value> key(Value::fromString(StringHeader::createFromUTF8(heap, name)));
        CHECK(obj.get()->getProp(heap, key).asNumber() == static_cast<double>(i) * 1.25);
    }
    Rooted<Value> spillKey(Value::fromString(StringHeader::createFromUTF8(heap, "f_spill")));
    CHECK(obj.get()->getProp(heap, spillKey).asNumber() == 999.5);
}

TEST_CASE("3-way property write IC populates ways 0..2, preserves way 3, and harvests all 3 shapes") {
    ShadowStackFrame frame;

    std::vector<InlineCacheSite> sites(1);
    std::vector<uint64_t> rows(2, 0);
    std::vector<uint8_t> idBytes = makeIdentityRecord(0x2B00000000000001ULL, 1);
    const auto* idWords = reinterpret_cast<const uint64_t*>(idBytes.data());
    uint64_t* siteWords = reinterpret_cast<uint64_t*>(sites.data());
    bronze_register_ic_hot(siteWords, rows.data(), 1, idWords);

    const uint32_t kA = bronze_register_key_string("w_a");
    const uint32_t kB = bronze_register_key_string("w_b");
    const uint32_t kC = bronze_register_key_string("w_c");
    const uint32_t kX = bronze_register_key_string("w_x");

    // Build 4 distinct shapes that all already own `w_x` so writes at `siteWords`
    // exercise overwrite latching across 1, 2, 3, and 4 shapes.
    Rooted<Value> o1{Value(bronze_create_object())};
    bronze_prop_set(o1.get().rawBits(), kX, Value::fromDouble(1.0).rawBits(), nullptr, true);

    Rooted<Value> o2{Value(bronze_create_object())};
    bronze_prop_set(o2.get().rawBits(), kA, Value::fromDouble(1.0).rawBits(), nullptr, true);
    bronze_prop_set(o2.get().rawBits(), kX, Value::fromDouble(2.0).rawBits(), nullptr, true);

    Rooted<Value> o3{Value(bronze_create_object())};
    bronze_prop_set(o3.get().rawBits(), kA, Value::fromDouble(1.0).rawBits(), nullptr, true);
    bronze_prop_set(o3.get().rawBits(), kB, Value::fromDouble(1.0).rawBits(), nullptr, true);
    bronze_prop_set(o3.get().rawBits(), kX, Value::fromDouble(3.0).rawBits(), nullptr, true);

    Rooted<Value> o4{Value(bronze_create_object())};
    bronze_prop_set(o4.get().rawBits(), kA, Value::fromDouble(1.0).rawBits(), nullptr, true);
    bronze_prop_set(o4.get().rawBits(), kB, Value::fromDouble(1.0).rawBits(), nullptr, true);
    bronze_prop_set(o4.get().rawBits(), kC, Value::fromDouble(1.0).rawBits(), nullptr, true);
    bronze_prop_set(o4.get().rawBits(), kX, Value::fromDouble(4.0).rawBits(), nullptr, true);

    const Shape* s1 = o1.get().asObject<ObjectHeader>()->shape;
    const Shape* s2 = o2.get().asObject<ObjectHeader>()->shape;
    const Shape* s3 = o3.get().asObject<ObjectHeader>()->shape;
    const Shape* s4 = o4.get().asObject<ObjectHeader>()->shape;

    bronze_prop_set(o1.get().rawBits(), kX, Value::fromDouble(11.0).rawBits(), siteWords, true);
    bronze_prop_set(o2.get().rawBits(), kX, Value::fromDouble(22.0).rawBits(), siteWords, true);
    bronze_prop_set(o3.get().rawBits(), kX, Value::fromDouble(33.0).rawBits(), siteWords, true);

    CHECK(sites[0].ways[0].cached_shape == s1);
    CHECK(sites[0].ways[1].cached_shape == s2);
    CHECK(sites[0].ways[2].cached_shape == s3);
    CHECK(sites[0].ways[3].cached_shape == nullptr);
    CHECK(siteWords[BRONZE_ABI_IC_SET_MEGA_COUNT_WORD] == 0);
    CHECK(rows[0] == reinterpret_cast<uint64_t>(s1));

    REQUIRE(rtIcHarvestConfigure(idBytes.data(), "/dev/null"));
    const std::string text3 = rtIcHarvestText(idBytes.data());
    CHECK(text3.find("ic\t0\tproperty\t-\n") != std::string::npos);
    CHECK(text3.find("way\t0\town\tObject{w_x}\n") != std::string::npos);
    CHECK(text3.find("way\t0\town\tObject{w_a,w_x}\n") != std::string::npos);
    CHECK(text3.find("way\t0\town\tObject{w_a,w_b,w_x}\n") != std::string::npos);

    // 4th shape rotates MRU into way 0 (`2 <- 1 <- 0`), keeps way 3 null,
    // resets the relatch counter on table miss, and updates the hot row to `s4`.
    siteWords[BRONZE_ABI_IC_SET_MEGA_COUNT_WORD] = 5;
    bronze_prop_set(o4.get().rawBits(), kX, Value::fromDouble(44.0).rawBits(), siteWords, true);
    CHECK(sites[0].ways[0].cached_shape == s4);
    CHECK(sites[0].ways[1].cached_shape == s1);
    CHECK(sites[0].ways[2].cached_shape == s2);
    CHECK(sites[0].ways[3].cached_shape == nullptr);
    CHECK(siteWords[BRONZE_ABI_IC_SET_MEGA_COUNT_WORD] == 0);
    CHECK(rows[0] == reinterpret_cast<uint64_t>(s4));

    // A subsequent write on evicted `s3` is answered from `mega_prop`, keeping
    // ways 0..2 intact and marking the site megamorphic via word 11.
    bronze_prop_set(o3.get().rawBits(), kX, Value::fromDouble(35.0).rawBits(), siteWords, true);
    CHECK(sites[0].ways[0].cached_shape == s4);
    CHECK(siteWords[BRONZE_ABI_IC_SET_MEGA_COUNT_WORD] == 1);

    const std::string text4 = rtIcHarvestText(idBytes.data());
    CHECK(text4.find("ic\t0\tproperty\tmega\n") != std::string::npos);
    CHECK(text4.find("way\t0\town\tObject{w_a,w_b,w_c,w_x}\n") != std::string::npos);

    rtIcHarvestConfigure(idBytes.data(), "");
    rtIcHotClearThread();
}

TEST_CASE("SLOT method IC records closure code pointer and harvests callee name") {
    ShadowStackFrame frame;

    static constexpr char kFakeSource[] = "function alpha(x){return x+10;}function beta(x){return x+20;}";
    const uint64_t entries[4] = {
        reinterpret_cast<uint64_t>(closureAlphaBody),
        (uint64_t{0} << 32) | uint64_t{31},
        reinterpret_cast<uint64_t>(closureBetaBody),
        (uint64_t{31} << 32) | uint64_t{30},
    };
    bronze_register_fn_sources(kFakeSource, static_cast<uint32_t>(sizeof(kFakeSource) - 1),
                               entries, 2);
    const std::string alphaName = typeFeedbackCodeName(reinterpret_cast<const void*>(closureAlphaBody));
    const std::string betaName = typeFeedbackCodeName(reinterpret_cast<const void*>(closureBetaBody));
    REQUIRE_FALSE(alphaName.empty());
    REQUIRE_FALSE(betaName.empty());

    std::vector<InlineCacheSite> sites(2);
    std::vector<uint64_t> rows(4, 0);
    std::vector<uint8_t> idBytes = makeIdentityRecord(0x2B00000000000002ULL, 2);
    const auto* idWords = reinterpret_cast<const uint64_t*>(idBytes.data());
    uint64_t* table = reinterpret_cast<uint64_t*>(sites.data());
    bronze_register_ic_hot(table, rows.data(), 2, idWords);

    const uint32_t runKey = bronze_register_key_string("runSlotFn");
    Rooted<Value> fnAlpha{Value(bronze_create_function(closureAlphaBody, 1, 1,
                                                       BRONZE_ABI_FN_NAME_NONE,
                                                       BRONZE_ABI_FN_FLAGS_ORDINARY,
                                                       Value::fromUndefined().rawBits()))};
    Rooted<Value> fnBeta{Value(bronze_create_function(closureBetaBody, 1, 1,
                                                      BRONZE_ABI_FN_NAME_NONE,
                                                      BRONZE_ABI_FN_FLAGS_ORDINARY,
                                                      Value::fromUndefined().rawBits()))};

    // Site 0: plain object own closure slot.
    Rooted<Value> recvObj{Value(bronze_create_object())};
    bronze_prop_set(recvObj.get().rawBits(), runKey, fnAlpha.get().rawBits(), nullptr, true);
    uint64_t* site0 = table;
    const uint64_t arg5 = Value::fromDouble(5.0).rawBits();
    CHECK(Value(bronze_call_method(recvObj.get().rawBits(), runKey, 1, &arg5, site0)).asNumber() == 15.0);
    CHECK((site0[BRONZE_ABI_METHOD_IC_ARITY_WORD] >> BRONZE_ABI_METHOD_IC_SLOT_SHIFT) != 0);
    CHECK(site0[BRONZE_ABI_METHOD_IC_CODE_WORD] == reinterpret_cast<uint64_t>(closureAlphaBody));

    // Site 1: function receiver statics-box closure slot.
    Rooted<Value> recvFn{makeConstructor("HostClass")};
    bronze_prop_set(recvFn.get().rawBits(), runKey, fnBeta.get().rawBits(), nullptr, true);
    uint64_t* site1 = table + (BRONZE_ABI_IC_SITE_SIZE / 8);
    CHECK(Value(bronze_call_method(recvFn.get().rawBits(), runKey, 1, &arg5, site1)).asNumber() == 25.0);
    CHECK((site1[BRONZE_ABI_METHOD_IC_ARITY_WORD] >> BRONZE_ABI_METHOD_IC_SLOT_SHIFT) != 0);
    CHECK(site1[BRONZE_ABI_METHOD_IC_CODE_WORD] == reinterpret_cast<uint64_t>(closureBetaBody));

    REQUIRE(rtIcHarvestConfigure(idBytes.data(), "/dev/null"));
    const std::vector<uint64_t> methodSites = {0, 1};
    rtIcHarvestFold(table, 2, idWords, &methodSites);
    // Drop from hot ranges before calling rtIcHarvestText so rtIcHotFoldLiveRanges
    // does not re-fold method sites as property sites without methodSites.
    rtIcHotClearThread();
    const std::string harvested = rtIcHarvestText(idBytes.data());
    CHECK(harvested.find("way\t0\tslot\tObject{runSlotFn}\t" + alphaName + "\n") != std::string::npos);
    CHECK(harvested.find("way\t1\tslot\tObject{runSlotFn}\t" + betaName + "\n") != std::string::npos);
    CHECK(harvested.find("*other*") == std::string::npos);

    rtIcHarvestConfigure(idBytes.data(), "");
}

TEST_CASE("StringHeader::equalsASCII and zero-alloc Function member reads") {
    Heap& heap = rtHeap();
    ShadowStackFrame frame;

    StringHeader* latin = StringHeader::createFromUTF8(heap, "prototype");
    CHECK(latin->equalsASCII("prototype"));
    CHECK_FALSE(latin->equalsASCII("prototyp"));
    CHECK_FALSE(latin->equalsASCII("prototypes"));

    const uint16_t utf16Proto[9] = {'p', 'r', 'o', 't', 'o', 't', 'y', 'p', 'e'};
    StringHeader* wideAscii = StringHeader::createUTF16(heap, utf16Proto, 9);
    CHECK(wideAscii->equalsASCII("prototype"));

    const uint16_t utf16High[9] = {0x0170, 'r', 'o', 't', 'o', 't', 'y', 'p', 'e'};
    StringHeader* wideNonAscii = StringHeader::createUTF16(heap, utf16High, 9);
    CHECK_FALSE(wideNonAscii->equalsASCII("prototype"));

    Rooted<Value> ctor{makeConstructor("Widget", 3)};
    const uint32_t protoKey = bronze_register_key_string("prototype");
    Rooted<Value> protoStr{Value::fromString(StringHeader::createFromUTF8(heap, "prototype"))};
    Rooted<Value> nameStr{Value::fromString(StringHeader::createFromUTF8(heap, "name"))};
    Rooted<Value> lenStr{Value::fromString(StringHeader::createFromUTF8(heap, "length"))};

    const uint64_t p1 = bronze_prop_get(ctor.get().rawBits(), protoKey, nullptr);
    const uint64_t p2 = bronze_elem_get(ctor.get().rawBits(), protoStr.get().rawBits());
    CHECK(Value(p1).isObject());
    CHECK(p1 == p2);
    CHECK(Value(bronze_elem_get(ctor.get().rawBits(), lenStr.get().rawBits())).asNumber() == 3.0);
    CHECK(Value(bronze_elem_get(ctor.get().rawBits(), nameStr.get().rawBits()))
              .asString<StringHeader>()
              ->equalsASCII("Widget"));

    // Arrow functions have no `.prototype` property and answer undefined on both paths.
    Rooted<Value> arrow{Value(bronze_create_function(noopCtorBody, 0, 0, BRONZE_ABI_FN_NAME_NONE,
                                                     /*flags=*/0,
                                                     Value::fromUndefined().rawBits()))};
    CHECK(Value(bronze_prop_get(arrow.get().rawBits(), protoKey, nullptr)).isUndefined());
    CHECK(Value(bronze_elem_get(arrow.get().rawBits(), protoStr.get().rawBits())).isUndefined());
}

TEST_CASE("bronze_instanceof shape cache handles hits, misses, prototype edits, and @@hasInstance") {
    ShadowStackFrame frame;

    Rooted<Value> ctorA{makeConstructor("Alpha")};
    Rooted<Value> ctorB{makeConstructor("Beta")};
    Rooted<Value> instA{Value(bronze_construct(ctorA.get().rawBits(), 0, nullptr))};
    Rooted<Value> plain{Value(bronze_create_object())};

    // Warm and hit both true, false, and primitive LHS arms.
    for (int i = 0; i < 4; ++i) {
        CHECK(bronze_instanceof(instA.get().rawBits(), ctorA.get().rawBits()));
        CHECK_FALSE(bronze_instanceof(instA.get().rawBits(), ctorB.get().rawBits()));
        CHECK_FALSE(bronze_instanceof(plain.get().rawBits(), ctorA.get().rawBits()));
        CHECK_FALSE(bronze_instanceof(Value::fromDouble(42.0).rawBits(), ctorA.get().rawBits()));
        CHECK_FALSE(bronze_instanceof(Value::fromNull().rawBits(), ctorA.get().rawBits()));
    }

    // Non-object and non-callable RHS still throw TypeError.
    Value thrown = Value::fromUndefined();
    CHECK(rtTryCatch(
        [&]() { bronze_instanceof(instA.get().rawBits(), Value::fromDouble(1.0).rawBits()); },
        thrown));
    CHECK(rtTryCatch(
        [&]() { bronze_instanceof(instA.get().rawBits(), plain.get().rawBits()); }, thrown));

    // Reassigning `ctorA.prototype = newProto` invalidates `instA instanceof ctorA`.
    const uint32_t protoKey = bronze_register_key_string("prototype");
    Rooted<Value> newProto1{Value(bronze_create_object())};
    bronze_prop_set(ctorA.get().rawBits(), protoKey, newProto1.get().rawBits(), nullptr, true);
    CHECK_FALSE(bronze_instanceof(instA.get().rawBits(), ctorA.get().rawBits()));
    Rooted<Value> instA2{Value(bronze_construct(ctorA.get().rawBits(), 0, nullptr))};
    CHECK(bronze_instanceof(instA2.get().rawBits(), ctorA.get().rawBits()));

    // Computed `ctorA["prototype"] = newProto2` also updates `instance_shape` and invalidates.
    Rooted<Value> protoStr{Value::fromString(StringHeader::createFromUTF8(rtHeap(), "prototype"))};
    Rooted<Value> newProto2{Value(bronze_create_object())};
    bronze_elem_set(ctorA.get().rawBits(), protoStr.get().rawBits(), newProto2.get().rawBits(),
                    true);
    CHECK_FALSE(bronze_instanceof(instA2.get().rawBits(), ctorA.get().rawBits()));

    // `Object.setPrototypeOf(plain, ctorB.prototype)` invalidates cached false for `plain`.
    const uint64_t bProtoBits = bronze_prop_get(ctorB.get().rawBits(), protoKey, nullptr);
    const uint64_t setArgs1[2] = {plain.get().rawBits(), bProtoBits};
    objectSetPrototypeOf(0, 0, 2, setArgs1);
    CHECK(bronze_instanceof(plain.get().rawBits(), ctorB.get().rawBits()));
    const uint64_t setArgs2[2] = {plain.get().rawBits(), Value::fromNull().rawBits()};
    objectSetPrototypeOf(0, 0, 2, setArgs2);
    CHECK_FALSE(bronze_instanceof(plain.get().rawBits(), ctorB.get().rawBits()));

    // Subclassing + custom `[Symbol.hasInstance]` invalidation on Base and on Ctor itself.
    Rooted<Value> baseCtor{makeConstructor("BaseCtor")};
    Rooted<Value> subCtor{makeConstructor("SubCtor")};
    bronze_class_extends(subCtor.get().rawBits(), baseCtor.get().rawBits());
    Rooted<Value> subInst{Value(bronze_construct(subCtor.get().rawBits(), 0, nullptr))};
    CHECK(bronze_instanceof(subInst.get().rawBits(), subCtor.get().rawBits()));
    CHECK(bronze_instanceof(subInst.get().rawBits(), baseCtor.get().rawBits()));
    CHECK_FALSE(bronze_instanceof(Value::fromDouble(7.0).rawBits(), subCtor.get().rawBits()));

    // Installing `BaseCtor[Symbol.hasInstance] = () => true` bumps `proto_epoch`
    // and overrides `SubCtor`'s cached false for primitive LHS!
    Rooted<Value> hasInstSym{Value::fromSymbol(rtSymbolHasInstance())};
    Rooted<Value> alwaysTrue{Value(bronze_create_function(customHasInstanceTrue, 1, 1,
                                                          BRONZE_ABI_FN_NAME_NONE,
                                                          BRONZE_ABI_FN_FLAGS_ORDINARY,
                                                          Value::fromUndefined().rawBits()))};
    bronze_elem_set(baseCtor.get().rawBits(), hasInstSym.get().rawBits(),
                    alwaysTrue.get().rawBits(), true);
    CHECK(bronze_instanceof(Value::fromDouble(7.0).rawBits(), subCtor.get().rawBits()));

    // Installing `ctorB[Symbol.hasInstance] = () => false` transitions `ctorB->properties`
    // and overrides `ctorB`'s cached true on its own instance!
    Rooted<Value> instB{Value(bronze_construct(ctorB.get().rawBits(), 0, nullptr))};
    CHECK(bronze_instanceof(instB.get().rawBits(), ctorB.get().rawBits()));
    Rooted<Value> alwaysFalse{Value(bronze_create_function(customHasInstanceFalse, 1, 1,
                                                           BRONZE_ABI_FN_NAME_NONE,
                                                           BRONZE_ABI_FN_FLAGS_ORDINARY,
                                                           Value::fromUndefined().rawBits()))};
    bronze_elem_set(ctorB.get().rawBits(), hasInstSym.get().rawBits(),
                    alwaysFalse.get().rawBits(), true);
    CHECK_FALSE(bronze_instanceof(instB.get().rawBits(), ctorB.get().rawBits()));
}
