#include <doctest/doctest.h>

#include "il/il_pin.h"
#include "il/verifier.h"
#include "il/verifier_proofs.h"

using namespace bronze;
using namespace bronze::il;

namespace {

Module makeSingleFnModule(Function fn) {
    Module m;
    m.name = "proof_test";
    m.keyConstants = {"x", "arr", "fn.x: number"};
    m.icSiteCount = 4;
    m.functions.push_back(std::move(fn));
    return m;
}

}  // namespace

TEST_CASE("Proof verifier accepts valid rawUnbox, nullishUnbox, PinGuard, and ElemGetTyped certificates") {
    Function fn;
    fn.name = "validKernel";
    fn.params = {{"obj", Type::Dynamic}, {"pinX", Type::F64, true, 2}};
    fn.returnType = Type::F64;
    fn.returnPinned = true;
    fn.returnPinKeyIndex = 2;
    fn.valueCount = 10;

    Block b0;
    b0.id = 0;

    // %2 = prop.get %0, "x"
    Instruction propGet{Op::PropGet, Type::Dynamic, 2, {0}, 0.0, 0, 0};
    propGet.keyIndex = 0;
    propGet.icIndex = 0;
    b0.instructions.push_back(propGet);

    // %3 = unbox %2, raw
    Instruction rawUnbox{Op::Unbox, Type::F64, 3, {2}, 0.0, 0, 0};
    rawUnbox.rawUnbox = true;
    b0.instructions.push_back(rawUnbox);

    // %4 = prop.get %0, "x" -> %5 = unbox %4, nullish
    Instruction propGet2{Op::PropGet, Type::Dynamic, 4, {0}, 0.0, 0, 0};
    propGet2.keyIndex = 0;
    propGet2.icIndex = 1;
    b0.instructions.push_back(propGet2);

    Instruction nullishUnbox{Op::Unbox, Type::F64, 5, {4}, 0.0, 0, 0};
    nullishUnbox.nullishUnbox = true;
    b0.instructions.push_back(nullishUnbox);

    // pin.guard %0, Number, key=2
    Instruction pin{Op::PinGuard, Type::Void, kNoValue, {0}, 0.0,
                    static_cast<int32_t>(PinBarrier::Number), 0};
    pin.keyIndex = 2;
    b0.instructions.push_back(pin);

    // %6 = const.f64 0
    b0.instructions.push_back({Op::ConstF64, Type::F64, 6, {}, 0.0, 0, 0});

    // %7 = elem.get.typed %0, %6, kElemKindPlainArrayF64
    Instruction elemGet{Op::ElemGetTyped, Type::F64, 7, {0, 6}, 0.0,
                        kElemKindPlainArrayF64, 0};
    b0.instructions.push_back(elemGet);

    // %8 = add %3, %7
    b0.instructions.push_back({Op::Add, Type::F64, 8, {3, 7}, 0.0, 0, 0});
    b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {8}, 0.0, 0, 0});

    fn.blocks.push_back(std::move(b0));
    Module m = makeSingleFnModule(std::move(fn));
    DiagnosticSink diags;
    CHECK(verify(m, diags));
    CHECK(!diags.hasErrors());
}

TEST_CASE("Proof verifier rejects rawUnbox + nullishUnbox set simultaneously") {
    Function fn;
    fn.name = "bothFlags";
    fn.params = {{"x", Type::Dynamic}};
    fn.returnType = Type::F64;
    fn.valueCount = 2;

    Block b0;
    b0.id = 0;
    Instruction unbox{Op::Unbox, Type::F64, 1, {0}, 0.0, 0, 0};
    unbox.rawUnbox = true;
    unbox.nullishUnbox = true;
    b0.instructions.push_back(unbox);
    b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {1}, 0.0, 0, 0});
    fn.blocks.push_back(std::move(b0));

    DiagnosticSink diags;
    CHECK_FALSE(verifyFunction(fn, diags));
    CHECK(diags.hasErrors());
}

TEST_CASE("Proof verifier rejects rawUnbox of known non-Number producers") {
    auto checkProducerRejected = [](Instruction producer) {
        Function fn;
        fn.name = "badRawUnbox";
        fn.returnType = Type::F64;
        fn.valueCount = 3;

        Block b0;
        b0.id = 0;
        producer.result = 0;
        producer.type = Type::Dynamic;
        b0.instructions.push_back(producer);

        Instruction unbox{Op::Unbox, Type::F64, 1, {0}, 0.0, 0, 0};
        unbox.rawUnbox = true;
        b0.instructions.push_back(unbox);
        b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {1}, 0.0, 0, 0});
        fn.blocks.push_back(std::move(b0));

        DiagnosticSink diags;
        CHECK_FALSE(verifyFunction(fn, diags));
        CHECK(diags.hasErrors());
    };

    checkProducerRejected({Op::CreateObject, Type::Dynamic, 0, {}, 0.0, 0, 0});
    checkProducerRejected({Op::CreateArray, Type::Dynamic, 0, {}, 0.0, 0, 0});
    checkProducerRejected({Op::ConstNull, Type::Dynamic, 0, {}, 0.0, 0, 0});
    checkProducerRejected({Op::ConstUndefined, Type::Dynamic, 0, {}, 0.0, 0, 0});

    // Also test Box(Str)
    {
        Function fn;
        fn.name = "boxStrRawUnbox";
        fn.returnType = Type::F64;
        fn.valueCount = 2;
        Block b0;
        b0.id = 0;
        Instruction boxStr{Op::Box, Type::Dynamic, 0, {}, 0.0, 0, 0};
        boxStr.boxType = Type::Str;
        boxStr.keyIndex = 0;
        b0.instructions.push_back(boxStr);
        Instruction unbox{Op::Unbox, Type::F64, 1, {0}, 0.0, 0, 0};
        unbox.rawUnbox = true;
        b0.instructions.push_back(unbox);
        b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {1}, 0.0, 0, 0});
        fn.blocks.push_back(std::move(b0));

        DiagnosticSink diags;
        CHECK_FALSE(verifyFunction(fn, diags));
        CHECK(diags.hasErrors());
    }
}

TEST_CASE("Proof verifier rejects rawUnbox on the false branch of IsNumber") {
    Function fn;
    fn.name = "falseBranchUnbox";
    fn.params = {{"x", Type::Dynamic}};
    fn.returnType = Type::F64;
    fn.valueCount = 4;

    Block b0;
    b0.id = 0;
    b0.instructions.push_back({Op::IsNumber, Type::Bool, 1, {0}, 0.0, 0, 0});
    Instruction br{Op::Branch, Type::Void, kNoValue, {1}, 0.0, 0, 0};
    br.target.block = 1;
    br.elseTarget.block = 2;
    b0.instructions.push_back(br);

    Block b1;
    b1.id = 1;
    b1.instructions.push_back({Op::ConstF64, Type::F64, 2, {}, 1.0, 0, 0});
    b1.instructions.push_back({Op::Ret, Type::Void, kNoValue, {2}, 0.0, 0, 0});

    // b2 is the FALSE target of `is.number %0` — rawUnbox of %0 here is unsound!
    Block b2;
    b2.id = 2;
    Instruction badUnbox{Op::Unbox, Type::F64, 3, {0}, 0.0, 0, 0};
    badUnbox.rawUnbox = true;
    b2.instructions.push_back(badUnbox);
    b2.instructions.push_back({Op::Ret, Type::Void, kNoValue, {3}, 0.0, 0, 0});

    fn.blocks.push_back(std::move(b0));
    fn.blocks.push_back(std::move(b1));
    fn.blocks.push_back(std::move(b2));

    DiagnosticSink diags;
    CHECK_FALSE(verifyFunction(fn, diags));
    CHECK(diags.hasErrors());
}

TEST_CASE("Proof verifier rejects rawUnbox on EnvGet when EnvSet stores a non-Number") {
    Function fn;
    fn.name = "contradictedEnvSlot";
    fn.params = {{"__env", Type::Dynamic}};
    fn.returnType = Type::F64;
    fn.valueCount = 4;

    Block b0;
    b0.id = 0;
    // %1 = create.object
    b0.instructions.push_back({Op::CreateObject, Type::Dynamic, 1, {}, 0.0, 0, 0});
    // env.set %0, 0, 1, %1
    Instruction envSet{Op::EnvSet, Type::Void, kNoValue, {0, 1}, 0.0, 0, 0};
    envSet.envDepth = 0;
    envSet.envIndex = 1;
    b0.instructions.push_back(envSet);

    // %2 = env.get %0, 0, 1
    Instruction envGet{Op::EnvGet, Type::Dynamic, 2, {0}, 0.0, 0, 0};
    envGet.envDepth = 0;
    envGet.envIndex = 1;
    b0.instructions.push_back(envGet);

    // %3 = unbox %2, raw  (contradicted by env.set of CreateObject!)
    Instruction unbox{Op::Unbox, Type::F64, 3, {2}, 0.0, 0, 0};
    unbox.rawUnbox = true;
    b0.instructions.push_back(unbox);
    b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {3}, 0.0, 0, 0});
    fn.blocks.push_back(std::move(b0));

    DiagnosticSink diags;
    CHECK_FALSE(verifyFunction(fn, diags));
    CHECK(diags.hasErrors());
}

TEST_CASE("Proof verifier rejects invalid PinGuard, Param::pinned, and Function::returnPinned") {
    // 1. Out-of-range PinBarrier enum
    {
        Function fn;
        fn.name = "badPinBarrier";
        fn.params = {{"x", Type::Dynamic}};
        fn.returnType = Type::Void;
        fn.valueCount = 1;
        Block b0;
        b0.id = 0;
        Instruction pin{Op::PinGuard, Type::Void, kNoValue, {0}, 0.0, 99, 0};
        pin.keyIndex = 0;
        b0.instructions.push_back(pin);
        b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {}, 0.0, 0, 0});
        fn.blocks.push_back(std::move(b0));

        Module m = makeSingleFnModule(std::move(fn));
        DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
    }

    // 2. Out-of-range PinGuard keyIndex
    {
        Function fn;
        fn.name = "badPinKey";
        fn.params = {{"x", Type::Dynamic}};
        fn.returnType = Type::Void;
        fn.valueCount = 1;
        Block b0;
        b0.id = 0;
        Instruction pin{Op::PinGuard, Type::Void, kNoValue, {0}, 0.0,
                        static_cast<int32_t>(PinBarrier::Number), 0};
        pin.keyIndex = 999;
        b0.instructions.push_back(pin);
        b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {}, 0.0, 0, 0});
        fn.blocks.push_back(std::move(b0));

        Module m = makeSingleFnModule(std::move(fn));
        DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
    }

    // 3. Pinned parameter with Dynamic type instead of F64
    {
        Function fn;
        fn.name = "badPinnedParam";
        fn.params = {{"x", Type::Dynamic, true, 0}};
        fn.returnType = Type::Void;
        fn.valueCount = 1;
        Block b0;
        b0.id = 0;
        b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {}, 0.0, 0, 0});
        fn.blocks.push_back(std::move(b0));

        Module m = makeSingleFnModule(std::move(fn));
        DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
    }

    // 4. returnPinned with out-of-range returnPinKeyIndex
    {
        Function fn;
        fn.name = "badReturnPinned";
        fn.returnType = Type::F64;
        fn.returnPinned = true;
        fn.returnPinKeyIndex = 999;
        fn.valueCount = 1;
        Block b0;
        b0.id = 0;
        b0.instructions.push_back({Op::ConstF64, Type::F64, 0, {}, 1.0, 0, 0});
        b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {0}, 0.0, 0, 0});
        fn.blocks.push_back(std::move(b0));

        Module m = makeSingleFnModule(std::move(fn));
        DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
    }
}

TEST_CASE("Proof verifier rejects ElemGetTyped/ElemSetTyped on CreateObject receiver or invalid element kind") {
    // 1. ElemGetTyped on CreateObject
    {
        Function fn;
        fn.name = "elemGetOnObj";
        fn.returnType = Type::F64;
        fn.valueCount = 3;
        Block b0;
        b0.id = 0;
        b0.instructions.push_back({Op::CreateObject, Type::Dynamic, 0, {}, 0.0, 0, 0});
        b0.instructions.push_back({Op::ConstF64, Type::F64, 1, {}, 0.0, 0, 0});
        Instruction getInst{Op::ElemGetTyped, Type::F64, 2, {0, 1}, 0.0,
                            kElemKindPlainArrayF64, 0};
        b0.instructions.push_back(getInst);
        b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {2}, 0.0, 0, 0});
        fn.blocks.push_back(std::move(b0));

        DiagnosticSink diags;
        CHECK_FALSE(verifyFunction(fn, diags));
    }

    // 2. ElemSetTyped with invalid element kind
    {
        Function fn;
        fn.name = "elemSetBadKind";
        fn.params = {{"arr", Type::Dynamic}};
        fn.returnType = Type::Void;
        fn.valueCount = 3;
        Block b0;
        b0.id = 0;
        b0.instructions.push_back({Op::ConstF64, Type::F64, 1, {}, 0.0, 0, 0});
        b0.instructions.push_back({Op::ConstF64, Type::F64, 2, {}, 42.0, 0, 0});
        Instruction setInst{Op::ElemSetTyped, Type::Void, kNoValue, {0, 1, 2}, 0.0, 777, 0};
        b0.instructions.push_back(setInst);
        b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {}, 0.0, 0, 0});
        fn.blocks.push_back(std::move(b0));

        DiagnosticSink diags;
        CHECK_FALSE(verifyFunction(fn, diags));
    }
}
