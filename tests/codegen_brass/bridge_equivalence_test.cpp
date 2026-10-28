#include <doctest/doctest.h>

#include <brass/mir/builder.hpp>
#include <brass/mir/module.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "abi/bronze_abi.h"
#include "codegen-brass/brass_backend.h"
#include "codegen-brass/brass_jit.h"
#include "codegen-brass/il2mir/il_abi.h"
#include "codegen-brass/il2mir/il_bridge_verifier.h"
#include "codegen-brass/il2mir/il_lowering_ops.h"
#include "codegen-brass/il2mir/il_translator.h"
#include "codegen-brass/il_to_brass_ast.h"
#include "eval/eval.h"
#include "il/il.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/value.h"
#include "support/diagnostics.h"

namespace {

double bitsToF64(uint64_t u) {
    double d = 0.0;
    std::memcpy(&d, &u, sizeof(d));
    return d;
}

std::vector<uint64_t> boundaryNanBoxValues() {
    std::vector<double> nums = {
        +0.0,
        -0.0,
        1.0,
        -1.0,
        0.5,
        -0.5,
        2.0,
        -2.0,
        31.0,
        32.0,
        33.0,
        -31.0,
        42.5,
        -42.5,
        2147483647.0,
        -2147483648.0,
        2147483648.0,
        -2147483649.0,
        4294967295.0,
        4294967296.0,
        9007199254740991.0,
        -9007199254740991.0,
        9223372036854775808.0,  // 2^63 (0x43E0000000000000)
        -9223372036854775808.0,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::min(),
        -std::numeric_limits<double>::min(),
        std::numeric_limits<double>::max(),
        -std::numeric_limits<double>::max(),
        std::numeric_limits<double>::denorm_min(),
        -std::numeric_limits<double>::denorm_min(),
    };
    std::vector<uint64_t> vals;
    for (double d : nums) {
        vals.push_back(bronze_box_f64(d));
    }
    vals.push_back(BRONZE_ABI_CANONICAL_NAN_BITS);
    vals.push_back(il2mir::kNullTag);
    vals.push_back(il2mir::kUndefinedTag);
    vals.push_back(il2mir::kBoolTag | 0ULL);
    vals.push_back(il2mir::kBoolTag | 1ULL);
    vals.push_back(il2mir::kInt32Tag | 0ULL);
    vals.push_back(il2mir::kInt32Tag | 42ULL);
    vals.push_back(il2mir::kInt32Tag | 0xFFFFFFFFULL); // -1
    vals.push_back(bronze_box_str("0"));
    vals.push_back(bronze_box_str("42"));
    vals.push_back(bronze_box_str("-7"));
    vals.push_back(bronze_box_str(""));
    vals.push_back(bronze_box_str("not_a_number"));
    return vals;
}

void addDummyMain(bronze::il::Module& m) {
    bronze::il::Function mainFn;
    mainFn.name = "main";
    mainFn.isEntryPoint = true;
    mainFn.returnType = bronze::il::Type::Void;
    bronze::il::Block b0;
    b0.id = 0;
    b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {}, 0, 0, 0});
    mainFn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(mainFn));
}

void addBinaryDynamicFn(bronze::il::Module& m, const std::string& name, bronze::il::Op op,
                        bronze::il::Type opType = bronze::il::Type::Dynamic) {
    bronze::il::Function fn;
    fn.name = name;
    fn.params = {{"a", bronze::il::Type::Dynamic}, {"b", bronze::il::Type::Dynamic}};
    fn.returnType = bronze::il::Type::Dynamic;
    fn.isExported = true;
    bronze::il::Block b0;
    b0.id = 0;
    b0.instructions.push_back({op, opType, 2, {0, 1}, 0, 0, 0});
    if (opType != bronze::il::Type::Dynamic) {
        bronze::il::Instruction boxInst{bronze::il::Op::Box, bronze::il::Type::Dynamic, 3, {2}, 0, 0, 0};
        boxInst.boxType = opType;
        b0.instructions.push_back(boxInst);
        b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {3}, 0, 0, 0});
        fn.valueCount = 4;
    } else {
        b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {2}, 0, 0, 0});
        fn.valueCount = 3;
    }
    fn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(fn));
}

void addUnaryDynamicFn(bronze::il::Module& m, const std::string& name, bronze::il::Op op,
                       bronze::il::Type paramType, bronze::il::Type opType) {
    bronze::il::Function fn;
    fn.name = name;
    fn.params = {{"a", paramType}};
    fn.returnType = bronze::il::Type::Dynamic;
    fn.isExported = true;
    bronze::il::Block b0;
    b0.id = 0;
    bronze::il::Instruction inst{op, opType, 1, {0}, 0, 0, 0};
    if (op == bronze::il::Op::Box) {
        inst.boxType = paramType;
    }
    b0.instructions.push_back(inst);
    if (opType != bronze::il::Type::Dynamic) {
        bronze::il::Instruction boxInst{bronze::il::Op::Box, bronze::il::Type::Dynamic, 2, {1}, 0, 0, 0};
        boxInst.boxType = opType;
        b0.instructions.push_back(boxInst);
        b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {2}, 0, 0, 0});
        fn.valueCount = 3;
    } else {
        b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {1}, 0, 0, 0});
        fn.valueCount = 2;
    }
    fn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(fn));
}

} // namespace

TEST_CASE("Bridge Verifier catches uncanonicalized NaN-box bitcast_i64_f64 and signed tag comparisons") {
    SUBCASE("rejects raw bitcast_i64_f64 escaping without NaN canonicalization") {
        brass::Module mod("bad_nan_box");
        brass::Builder b(mod);
        brass::Function* fn = mod.create_function("leak_neg_nan", brass::Type::i64(), {brass::Type::f64()});
        b.set_function(fn);
        brass::BasicBlock* entry = b.append_block("entry");
        b.position_at_end(entry);
        brass::Value* x = b.add_block_param(entry, brass::Type::f64());
        // Buggy lowering: bitcast f64 directly to i64 and return without canonicalizing NaNs!
        brass::Value* raw_bits = b.build_bitcast_i64_f64(x);
        b.build_ret(raw_bits);
        fn->rebuild_cfg_predecessors();

        auto res = il2mir::verify_bridge_function_detailed(*fn);
        CHECK_FALSE(res.ok);
        REQUIRE_FALSE(res.errors.empty());
        CHECK(res.errors[0].message.find("Uncanonicalized bitcast_i64_f64") != std::string::npos);
    }

    SUBCASE("accepts properly canonicalized bitcast_i64_f64") {
        brass::Module mod("good_nan_box");
        brass::Builder b(mod);
        brass::Function* fn = mod.create_function("safe_box_f64", brass::Type::i64(), {brass::Type::f64()});
        b.set_function(fn);
        brass::BasicBlock* entry = b.append_block("entry");
        b.position_at_end(entry);
        brass::Value* x = b.add_block_param(entry, brass::Type::f64());
        brass::Value* bits = b.build_bitcast_i64_f64(x);
        brass::Value* abs_bits = b.build_and(bits, b.build_iconst_i64(static_cast<int64_t>(0x7FFFFFFFFFFFFFFFULL)));
        brass::Value* is_nan = b.build_ugt(abs_bits, b.build_iconst_i64(static_cast<int64_t>(0x7FF0000000000000ULL)));
        brass::Value* boxed = b.build_select(
            is_nan, b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_CANONICAL_NAN_BITS)), bits);
        b.build_ret(boxed);
        fn->rebuild_cfg_predecessors();

        auto res = il2mir::verify_bridge_function_detailed(*fn);
        CHECK(res.ok);
        CHECK(res.errors.empty());
    }

    SUBCASE("accepts the f != f canonicalization box_f64_bits emits") {
        brass::Module mod("good_nan_box_fp");
        brass::Builder b(mod);
        brass::Function* fn = mod.create_function("safe_box_f64_fp", brass::Type::i64(), {brass::Type::f64()});
        b.set_function(fn);
        brass::BasicBlock* entry = b.append_block("entry");
        b.position_at_end(entry);
        brass::Value* x = b.add_block_param(entry, brass::Type::f64());
        brass::Value* boxed = il2mir::box_f64_bits(b, x);
        b.build_ret(boxed);
        fn->rebuild_cfg_predecessors();

        auto res = il2mir::verify_bridge_function_detailed(*fn);
        CHECK(res.ok);
        CHECK(res.errors.empty());
    }

    SUBCASE("rejects an f != f test of a different double than the bits") {
        brass::Module mod("bad_nan_box_fp");
        brass::Builder b(mod);
        brass::Function* fn =
            mod.create_function("wrong_nan_test", brass::Type::i64(), {brass::Type::f64(), brass::Type::f64()});
        b.set_function(fn);
        brass::BasicBlock* entry = b.append_block("entry");
        b.position_at_end(entry);
        brass::Value* x = b.add_block_param(entry, brass::Type::f64());
        brass::Value* y = b.add_block_param(entry, brass::Type::f64());
        // Buggy lowering: y's NaN test guards x's bits, so a NaN x escapes.
        brass::Value* bits = b.build_bitcast_i64_f64(x);
        brass::Value* boxed = b.build_select(
            b.build_ne(y, y), b.build_iconst_i64(static_cast<int64_t>(BRONZE_ABI_CANONICAL_NAN_BITS)), bits);
        b.build_ret(boxed);
        fn->rebuild_cfg_predecessors();

        auto res = il2mir::verify_bridge_function_detailed(*fn);
        CHECK_FALSE(res.ok);
        REQUIRE_FALSE(res.errors.empty());
        CHECK(res.errors[0].message.find("Uncanonicalized bitcast_i64_f64") != std::string::npos);
    }

    SUBCASE("rejects signed comparison against kBronzeNumberMaxBits") {
        brass::Module mod("bad_signed_tag");
        brass::Builder b(mod);
        brass::Function* fn = mod.create_function("signed_is_num", brass::Type::i32(), {brass::Type::i64()});
        b.set_function(fn);
        brass::BasicBlock* entry = b.append_block("entry");
        b.position_at_end(entry);
        brass::Value* x = b.add_block_param(entry, brass::Type::i64());
        // Buggy lowering: sle instead of ule against 0xFFF0000000000000!
        brass::Value* cmp = b.build_sle(
            x, b.build_iconst_i64(static_cast<int64_t>(il2mir::kBronzeNumberMaxBits)));
        b.build_ret(cmp);
        fn->rebuild_cfg_predecessors();

        auto res = il2mir::verify_bridge_function_detailed(*fn);
        CHECK_FALSE(res.ok);
        REQUIRE_FALSE(res.errors.empty());
        CHECK(res.errors[0].message.find("Signed comparison") != std::string::npos);
    }

    SUBCASE("rejects invalid shifted tag constant in tag-masked equality") {
        brass::Module mod("bad_shifted_tag");
        brass::Builder b(mod);
        brass::Function* fn = mod.create_function("bad_tag_mask", brass::Type::i32(), {brass::Type::i64()});
        b.set_function(fn);
        brass::BasicBlock* entry = b.append_block("entry");
        b.position_at_end(entry);
        brass::Value* x = b.add_block_param(entry, brass::Type::i64());
        brass::Value* masked = b.build_and(
            x, b.build_iconst_i64(static_cast<int64_t>(~uint64_t{BRONZE_ABI_VALUE_PAYLOAD_MASK})));
        // Unshifted tag 0xFFF5 instead of 0xFFF5000000000000ULL!
        brass::Value* cmp = b.build_eq(masked, b.build_iconst_i64(0xFFF5));
        b.build_ret(cmp);
        fn->rebuild_cfg_predecessors();

        auto res = il2mir::verify_bridge_function_detailed(*fn);
        CHECK_FALSE(res.ok);
        REQUIRE_FALSE(res.errors.empty());
        CHECK(res.errors[0].message.find("invalid shifted tag") != std::string::npos);
    }
}

TEST_CASE("nullishUnbox lowers to branchless inline MIR and matches bronze_unbox_f64 bit-for-bit") {
    bronze::il::Module m;
    m.name = "nullish_unbox_test";

    // unbox_nullish_fast(x: dynamic) -> f64 (nullishUnbox = true)
    {
        bronze::il::Function fn;
        fn.name = "unbox_nullish_fast";
        fn.params = {{"x", bronze::il::Type::Dynamic}};
        fn.returnType = bronze::il::Type::F64;
        fn.isExported = true;
        fn.valueCount = 2;
        bronze::il::Block b0;
        b0.id = 0;
        bronze::il::Instruction unboxInst{bronze::il::Op::Unbox, bronze::il::Type::F64, 1, {0}, 0, 0, 0};
        unboxInst.nullishUnbox = true;
        b0.instructions.push_back(unboxInst);
        b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {1}, 0, 0, 0});
        fn.blocks.push_back(std::move(b0));
        m.functions.push_back(std::move(fn));
    }

    // unbox_slow(x: dynamic) -> f64 (nullishUnbox = false)
    {
        bronze::il::Function fn;
        fn.name = "unbox_slow";
        fn.params = {{"x", bronze::il::Type::Dynamic}};
        fn.returnType = bronze::il::Type::F64;
        fn.isExported = true;
        fn.valueCount = 2;
        bronze::il::Block b0;
        b0.id = 0;
        bronze::il::Instruction unboxInst{bronze::il::Op::Unbox, bronze::il::Type::F64, 1, {0}, 0, 0, 0};
        b0.instructions.push_back(unboxInst);
        b0.instructions.push_back({bronze::il::Op::Ret, bronze::il::Type::Void, bronze::il::kNoValue, {1}, 0, 0, 0});
        fn.blocks.push_back(std::move(b0));
        m.functions.push_back(std::move(fn));
    }

    addDummyMain(m);

    bronze::BrassBackend backend;
    bronze::DiagnosticSink diags;

    auto mirMod = backend.buildMirModule(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(mirMod != nullptr);
    CHECK(il2mir::verify_bridge_module(*mirMod));

    // Verify that unbox_nullish_fast does NOT emit a call to bronze_unbox_f64.
    const brass::Function* fastMirFn = mirMod->get_function("unbox_nullish_fast");
    REQUIRE(fastMirFn != nullptr);
    bool callsRuntimeUnbox = false;
    for (const brass::BasicBlock* bb : fastMirFn->blocks()) {
        for (const brass::Instruction* inst = bb->head(); inst; inst = inst->next()) {
            if (inst->opcode() == brass::Opcode::call && inst->symbol() == "bronze_unbox_f64") {
                callsRuntimeUnbox = true;
            }
        }
    }
    CHECK_FALSE(callsRuntimeUnbox);

    auto program = backend.compileToJit(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(program != nullptr);

    bronze::ShadowStackFrame gcFrame;
    program->run();

    auto fastWrapper = reinterpret_cast<bronze_fn_code>(program->symbolAddress("__wrapper_unbox_nullish_fast"));
    auto slowWrapper = reinterpret_cast<bronze_fn_code>(program->symbolAddress("__wrapper_unbox_slow"));
    REQUIRE(fastWrapper != nullptr);
    REQUIRE(slowWrapper != nullptr);

    std::vector<uint64_t> inputs = {
        bronze_box_f64(+0.0),
        bronze_box_f64(-0.0),
        bronze_box_f64(1.0),
        bronze_box_f64(-1.0),
        bronze_box_f64(42.5),
        bronze_box_f64(std::numeric_limits<double>::infinity()),
        bronze_box_f64(-std::numeric_limits<double>::infinity()),
        bronze_box_f64(std::numeric_limits<double>::min()),
        bronze_box_f64(std::numeric_limits<double>::max()),
        bronze_box_f64(std::numeric_limits<double>::denorm_min()),
        BRONZE_ABI_CANONICAL_NAN_BITS,
        il2mir::kNullTag,
        il2mir::kUndefinedTag,
    };

    for (uint64_t v : inputs) {
        uint64_t fastBoxed = bronze::rtEnterJs(fastWrapper, 0, 0, 1, &v);
        uint64_t slowBoxed = bronze::rtEnterJs(slowWrapper, 0, 0, 1, &v);
        uint64_t runtimeBoxed = bronze_box_f64(bronze_unbox_f64(v));
        CHECK(fastBoxed == slowBoxed);
        CHECK(fastBoxed == runtimeBoxed);
    }
}

TEST_CASE("Fast-path MIR lowering matches slow-path C runtime helpers bit-for-bit across NaN-box boundaries") {
    bronze::ShadowStackFrame gcFrame;
    bronze::il::Module m;
    m.name = "bridge_equivalence_ops";

    addBinaryDynamicFn(m, "jit_add", bronze::il::Op::Add);
    addBinaryDynamicFn(m, "jit_sub", bronze::il::Op::Sub);
    addBinaryDynamicFn(m, "jit_mul", bronze::il::Op::Mul);
    addBinaryDynamicFn(m, "jit_div", bronze::il::Op::Div);
    addBinaryDynamicFn(m, "jit_mod", bronze::il::Op::Mod);

    addBinaryDynamicFn(m, "jit_bit_and", bronze::il::Op::BitAnd, bronze::il::Type::Dynamic);
    addBinaryDynamicFn(m, "jit_bit_or", bronze::il::Op::BitOr, bronze::il::Type::Dynamic);
    addBinaryDynamicFn(m, "jit_bit_xor", bronze::il::Op::BitXor, bronze::il::Type::Dynamic);
    addBinaryDynamicFn(m, "jit_shl", bronze::il::Op::Shl, bronze::il::Type::Dynamic);
    addBinaryDynamicFn(m, "jit_shr", bronze::il::Op::Shr, bronze::il::Type::Dynamic);
    addBinaryDynamicFn(m, "jit_ushr", bronze::il::Op::UShr, bronze::il::Type::Dynamic);

    addUnaryDynamicFn(m, "jit_bit_not", bronze::il::Op::BitNot, bronze::il::Type::Dynamic, bronze::il::Type::Dynamic);
    addUnaryDynamicFn(m, "jit_to_int32", bronze::il::Op::ToInt32, bronze::il::Type::Dynamic, bronze::il::Type::I32);
    addUnaryDynamicFn(m, "jit_is_number", bronze::il::Op::IsNumber, bronze::il::Type::Dynamic, bronze::il::Type::Bool);
    addUnaryDynamicFn(m, "jit_is_nullish", bronze::il::Op::IsNullish, bronze::il::Type::Dynamic, bronze::il::Type::Bool);
    addUnaryDynamicFn(m, "jit_num_truthy", bronze::il::Op::NumTruthy, bronze::il::Type::F64, bronze::il::Type::Bool);
    addUnaryDynamicFn(m, "jit_box_f64", bronze::il::Op::Box, bronze::il::Type::F64, bronze::il::Type::Dynamic);

    addBinaryDynamicFn(m, "jit_lt", bronze::il::Op::RelLt, bronze::il::Type::Bool);
    addBinaryDynamicFn(m, "jit_le", bronze::il::Op::RelLe, bronze::il::Type::Bool);
    addBinaryDynamicFn(m, "jit_gt", bronze::il::Op::RelGt, bronze::il::Type::Bool);
    addBinaryDynamicFn(m, "jit_ge", bronze::il::Op::RelGe, bronze::il::Type::Bool);
    addBinaryDynamicFn(m, "jit_strict_eq", bronze::il::Op::StrictEq, bronze::il::Type::Bool);
    addBinaryDynamicFn(m, "jit_loose_eq", bronze::il::Op::LooseEq, bronze::il::Type::Bool);

    addDummyMain(m);

    bronze::BrassBackend backend;
    bronze::DiagnosticSink diags;

    auto mirMod = backend.buildMirModule(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(mirMod != nullptr);
    CHECK(il2mir::verify_bridge_module(*mirMod));

    auto program = backend.compileToJit(m, diags);
    REQUIRE(!diags.hasErrors());
    REQUIRE(program != nullptr);
    program->run();

    auto getWrap = [&](const char* name) -> bronze_fn_code {
        void* sym = program->symbolAddress(std::string("__wrapper_") + name);
        REQUIRE(sym != nullptr);
        return reinterpret_cast<bronze_fn_code>(sym);
    };
    auto call1 = [](bronze_fn_code fn, uint64_t a) -> uint64_t {
        return bronze::rtEnterJs(fn, 0, 0, 1, &a);
    };
    auto call2 = [](bronze_fn_code fn, uint64_t a, uint64_t b) -> uint64_t {
        uint64_t args[2] = {a, b};
        return bronze::rtEnterJs(fn, 0, 0, 2, args);
    };

    auto wAdd = getWrap("jit_add");
    auto wSub = getWrap("jit_sub");
    auto wMul = getWrap("jit_mul");
    auto wDiv = getWrap("jit_div");
    auto wMod = getWrap("jit_mod");

    auto wBitAnd = getWrap("jit_bit_and");
    auto wBitOr = getWrap("jit_bit_or");
    auto wBitXor = getWrap("jit_bit_xor");
    auto wShl = getWrap("jit_shl");
    auto wShr = getWrap("jit_shr");
    auto wUshr = getWrap("jit_ushr");

    auto wBitNot = getWrap("jit_bit_not");
    auto wToInt32 = getWrap("jit_to_int32");
    auto wIsNumber = getWrap("jit_is_number");
    auto wIsNullish = getWrap("jit_is_nullish");
    auto wNumTruthy = getWrap("jit_num_truthy");
    auto jitBoxF64 = reinterpret_cast<uint64_t (*)(double)>(program->symbolAddress("jit_box_f64"));
    REQUIRE(jitBoxF64 != nullptr);

    auto wLt = getWrap("jit_lt");
    auto wLe = getWrap("jit_le");
    auto wGt = getWrap("jit_gt");
    auto wGe = getWrap("jit_ge");
    auto wStrictEq = getWrap("jit_strict_eq");
    auto wLooseEq = getWrap("jit_loose_eq");

    // Verify NaN canonicalization on Box(F64) with negative/signaling NaN bit patterns.
    std::vector<uint64_t> nanPatterns = {
        0x7FF8000000000000ULL, // canonical positive quiet NaN
        0xFFF8000000000000ULL, // negative quiet NaN (would overlap tag range if uncanonicalized!)
        0xFFF8000000001234ULL, // negative NaN with payload
        0x7FF0000000000001ULL, // signaling NaN
        0xFFFFFFFFFFFFFFFFULL, // all-ones NaN
    };
    for (uint64_t rawNan : nanPatterns) {
        double d = bitsToF64(rawNan);
        uint64_t boxed = jitBoxF64(d);
        CHECK(boxed == BRONZE_ABI_CANONICAL_NAN_BITS);
        CHECK(boxed == bronze_box_f64(d));
    }

    auto vals = boundaryNanBoxValues();
    bronze_register_value_cells(vals.data(), vals.size());

    auto checkDynEq = [](uint64_t rJit, uint64_t rRuntime) {
        if ((rJit >> BRONZE_ABI_VALUE_TAG_SHIFT) == BRONZE_ABI_TAG_STRING &&
            (rRuntime >> BRONZE_ABI_VALUE_TAG_SHIFT) == BRONZE_ABI_TAG_STRING) {
            bronze::Rooted<bronze::Value> rootJit(bronze::Value::fromRawBits(rJit));
            bronze::Rooted<bronze::Value> rootRt(bronze::Value::fromRawBits(rRuntime));
            CHECK(std::string_view(bronze_unbox_str(rootJit.get().rawBits())) ==
                  std::string_view(bronze_unbox_str(rootRt.get().rawBits())));
        } else {
            CHECK(rJit == rRuntime);
        }
    };

    for (size_t i = 0; i < vals.size(); ++i) {
        uint64_t a = vals[i];
        CHECK(call1(wToInt32, vals[i]) == bronze_box_i32(bronze_to_int32(vals[i])));
        CHECK(call1(wBitNot, vals[i]) == bronze_dynamic_bitnot(vals[i]));
        bool expectedIsNum = (vals[i] <= il2mir::kBronzeNumberMaxBits);
        CHECK(call1(wIsNumber, vals[i]) == bronze_box_bool(expectedIsNum));
        CHECK(call1(wIsNullish, vals[i]) == bronze_box_bool(bronze_is_nullish(vals[i])));

        if (expectedIsNum) {
            double d = bitsToF64(a);
            bool expectedTruthy = (d != 0.0) && !std::isnan(d);
            CHECK(call1(wNumTruthy, vals[i]) == bronze_box_bool(expectedTruthy));
            CHECK(jitBoxF64(d) == bronze_box_f64(d));
        }

        for (size_t j = 0; j < vals.size(); ++j) {
            uint64_t rAddJit = call2(wAdd, vals[i], vals[j]);
            bronze::Rooted<bronze::Value> rootAddJit(bronze::Value::fromRawBits(rAddJit));
            uint64_t rAddRt = bronze_dynamic_add(vals[i], vals[j]);
            checkDynEq(rootAddJit.get().rawBits(), rAddRt);

            CHECK(call2(wSub, vals[i], vals[j]) == bronze_dynamic_sub(vals[i], vals[j]));
            CHECK(call2(wMul, vals[i], vals[j]) == bronze_dynamic_mul(vals[i], vals[j]));
            CHECK(call2(wDiv, vals[i], vals[j]) == bronze_dynamic_div(vals[i], vals[j]));
            CHECK(call2(wMod, vals[i], vals[j]) == bronze_dynamic_mod(vals[i], vals[j]));

            CHECK(call2(wBitAnd, vals[i], vals[j]) == bronze_dynamic_bitand(vals[i], vals[j]));
            CHECK(call2(wBitOr, vals[i], vals[j]) == bronze_dynamic_bitor(vals[i], vals[j]));
            CHECK(call2(wBitXor, vals[i], vals[j]) == bronze_dynamic_bitxor(vals[i], vals[j]));
            CHECK(call2(wShl, vals[i], vals[j]) == bronze_dynamic_shl(vals[i], vals[j]));
            CHECK(call2(wShr, vals[i], vals[j]) == bronze_dynamic_shr(vals[i], vals[j]));
            CHECK(call2(wUshr, vals[i], vals[j]) == bronze_dynamic_ushr(vals[i], vals[j]));

            CHECK(call2(wLt, vals[i], vals[j]) == bronze_box_bool(bronze_rel_lt(vals[i], vals[j])));
            CHECK(call2(wLe, vals[i], vals[j]) == bronze_box_bool(bronze_rel_le(vals[i], vals[j])));
            CHECK(call2(wGt, vals[i], vals[j]) == bronze_box_bool(bronze_rel_gt(vals[i], vals[j])));
            CHECK(call2(wGe, vals[i], vals[j]) == bronze_box_bool(bronze_rel_ge(vals[i], vals[j])));
            CHECK(call2(wStrictEq, vals[i], vals[j]) == bronze_box_bool(bronze_strict_eq(vals[i], vals[j])));
            CHECK(call2(wLooseEq, vals[i], vals[j]) == bronze_box_bool(bronze_loose_eq(vals[i], vals[j])));
        }
    }
}

TEST_CASE("Property IC, Dense Array, and TypedArray fast paths match Tier-0 interpreter across boundary inputs") {
    const char* script = R"JS(
        function testPropIC() {
            function readX(o) { return o.x; }
            function writeX(o, v) { o.x = v; return o.x; }
            let sum = 0;
            // Monomorphic warmup + boundary values
            const mono = { x: 0 };
            for (let i = 0; i < 20; i++) {
                writeX(mono, i - 0.5);
                sum += readX(mono);
            }
            // Polymorphic shapes
            const shapes = [
                { x: 10, a: 1 },
                { a: 1, x: 20 },
                { a: 1, b: 2, x: 30 },
                { a: 1, b: 2, c: 3, x: -0.0 },
                { a: 1, b: 2, c: 3, d: 4, x: NaN }
            ];
            for (let s of shapes) {
                const v = readX(s);
                sum += Number.isNaN(v) ? 999 : (Object.is(v, -0) ? -777 : v);
            }
            return sum;
        }

        function testDenseAndHoleyArrays() {
            const dense = [10, -0.0, NaN, Infinity, -Infinity];
            const holey = new Array(4);
            holey[0] = 42;
            holey[2] = 100;
            let acc = [];
            for (let i = -1; i <= 5; i++) {
                const dv = dense[i];
                acc.push(dv === undefined ? "u" : (Number.isNaN(dv) ? "nan" : (Object.is(dv, -0) ? "-0" : String(dv))));
            }
            for (let i = 0; i < 4; i++) {
                acc.push(holey[i] === undefined ? "hole" : String(holey[i]));
            }
            return acc.join(",");
        }

        function testTypedArrays() {
            const edgeVals = [0, -0.0, 1, -1, 0.5, 1.5, 2.5, 127, 128, 255, 256, -128, -129, 32767, 32768, 65535, 65536, 2147483647, -2147483648, NaN, Infinity, -Infinity];
            const ctors = [
                Int8Array, Uint8Array, Uint8ClampedArray,
                Int16Array, Uint16Array, Int32Array, Uint32Array,
                Float32Array, Float64Array
            ];
            let out = [];
            for (let Ctor of ctors) {
                const ta = new Ctor(edgeVals.length);
                for (let i = 0; i < edgeVals.length; i++) {
                    ta[i] = edgeVals[i];
                }
                for (let i = 0; i < edgeVals.length; i++) {
                    const v = ta[i];
                    out.push(Number.isNaN(v) ? "nan" : (Object.is(v, -0) ? "-0" : String(v)));
                }
            }
            return out.join("|");
        }

        [testPropIC(), testDenseAndHoleyArrays(), testTypedArrays()].join("##");
    )JS";

    bronze::eval::EvalOptions interpOpts;
    interpOpts.tier = bronze::ExecutionTier::Tier0_Interpreter;
    auto interpRes = bronze::eval::evalScript(script, interpOpts);
    REQUIRE_FALSE(interpRes.thrown);

    bronze::eval::EvalOptions baseOpts;
    baseOpts.tier = bronze::ExecutionTier::Tier1_Baseline;
    auto baseRes = bronze::eval::evalScript(script, baseOpts);
    REQUIRE_FALSE(baseRes.thrown);

    bronze::eval::EvalOptions optOpts;
    optOpts.tier = bronze::ExecutionTier::Tier2_Optimized;
    auto optRes = bronze::eval::evalScript(script, optOpts);
    REQUIRE_FALSE(optRes.thrown);

    std::string sInterp = bronze_unbox_str(interpRes.value.rawBits());
    std::string sBase = bronze_unbox_str(baseRes.value.rawBits());
    std::string sOpt = bronze_unbox_str(optRes.value.rawBits());

    CHECK(sBase == sInterp);
    CHECK(sOpt == sInterp);
}
