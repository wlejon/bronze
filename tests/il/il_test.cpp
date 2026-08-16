#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "il/print.h"
#include "il/verifier.h"

using namespace bronze::il;

TEST_CASE("canonical print of a two-function module") {
    Module m;
    m.name = "demo";

    Function add;
    add.name = "addOne";
    add.params = {{"x", Type::F64}};
    add.returnType = Type::F64;
    add.isExported = true;
    add.valueCount = 3;
    Block b0;
    b0.id = 0;
    b0.instructions.push_back({Op::ConstF64, Type::F64, 1, {}, 1.0, 0, 0});
    b0.instructions.push_back({Op::Add, Type::F64, 2, {0, 1}, 0, 0, 0});
    b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {2}, 0, 0, 0});
    add.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(add));

    Function main;
    main.name = "main";
    main.returnType = Type::F64;
    main.valueCount = 2;
    Block mb0;
    mb0.id = 0;
    mb0.instructions.push_back({Op::ConstF64, Type::F64, 0, {}, 41.0, 0, 0});
    mb0.instructions.push_back({Op::Call, Type::F64, 1, {0}, 0, 0, 0});
    mb0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {1}, 0, 0, 0});
    main.blocks.push_back(std::move(mb0));
    m.functions.push_back(std::move(main));

    CHECK(print(m) ==
          "module demo\n"
          "\n"
          "func addOne(%0: f64) -> f64 export {\n"
          "  b0:\n"
          "    %1: f64 = const.f64 1\n"
          "    %2: f64 = add %0, %1\n"
          "    ret %2\n"
          "}\n"
          "\n"
          "func main() -> f64 {\n"
          "  b0:\n"
          "    %0: f64 = const.f64 41\n"
          "    %1: f64 = call @addOne(%0)\n"
          "    ret %1\n"
          "}\n");
}

TEST_CASE("float printing is shortest round-trippable, locale-free") {
    Module m;
    m.name = "f";
    Function fn;
    fn.name = "c";
    fn.returnType = Type::F64;
    fn.valueCount = 1;
    Block b0;
    b0.id = 0;
    b0.instructions.push_back({Op::ConstF64, Type::F64, 0, {}, 0.1, 0, 0});
    b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {0}, 0, 0, 0});
    fn.blocks.push_back(std::move(b0));
    m.functions.push_back(std::move(fn));
    CHECK(print(m).find("const.f64 0.1\n") != std::string::npos);
}

TEST_CASE("canonical print of multi-block count_to example from 0005 spec") {
    Module m;
    m.name = "count";

    Function fn;
    fn.name = "count_to";
    fn.params = {{"n", Type::F64}};
    fn.returnType = Type::F64;
    fn.valueCount = 6;

    // b0: %1 = const.f64 0; jump b1(%1)
    Block b0{.id = 0};
    b0.instructions.push_back({Op::ConstF64, Type::F64, 1, {}, 0.0});
    Instruction j1;
    j1.op = Op::Jump;
    j1.target = BlockTarget{.block = 1, .args = {1}};
    b0.instructions.push_back(j1);
    fn.blocks.push_back(std::move(b0));

    // b1(%2: f64): %3 = cmp.lt %2, %0; br %3, b2, b3
    Block b1{.id = 1, .params = {{2, Type::F64}}};
    b1.instructions.push_back({Op::CmpLt, Type::Bool, 3, {2, 0}});
    Instruction br1;
    br1.op = Op::Branch;
    br1.operands = {3};
    br1.target = BlockTarget{.block = 2, .args = {}};
    br1.elseTarget = BlockTarget{.block = 3, .args = {}};
    b1.instructions.push_back(br1);
    fn.blocks.push_back(std::move(b1));

    // b2: %4 = const.f64 1; %5 = add %2, %4; jump b1(%5)
    Block b2{.id = 2};
    b2.instructions.push_back({Op::ConstF64, Type::F64, 4, {}, 1.0});
    b2.instructions.push_back({Op::Add, Type::F64, 5, {2, 4}});
    Instruction j2;
    j2.op = Op::Jump;
    j2.target = BlockTarget{.block = 1, .args = {5}};
    b2.instructions.push_back(j2);
    fn.blocks.push_back(std::move(b2));

    // b3: ret %2
    Block b3{.id = 3};
    b3.instructions.push_back({Op::Ret, Type::Void, kNoValue, {2}});
    fn.blocks.push_back(std::move(b3));

    m.functions.push_back(std::move(fn));

    bronze::DiagnosticSink diags;
    CHECK(verify(m, diags));
    CHECK_FALSE(diags.hasErrors());

    std::string expected =
        "module count\n"
        "\n"
        "func count_to(%0: f64) -> f64 {\n"
        "  b0:\n"
        "    %1: f64 = const.f64 0\n"
        "    jump b1(%1)\n"
        "  b1(%2: f64):\n"
        "    %3: bool = cmp.lt %2, %0\n"
        "    br %3, b2, b3\n"
        "  b2:\n"
        "    %4: f64 = const.f64 1\n"
        "    %5: f64 = add %2, %4\n"
        "    jump b1(%5)\n"
        "  b3:\n"
        "    ret %2\n"
        "}\n";

    CHECK(print(m) == expected);
}

TEST_CASE("IL Verifier catches errors") {
    SUBCASE("br with identical then and else targets") {
        Module m;
        m.name = "bad_br";
        Function fn;
        fn.name = "test";
        fn.params = {{"cond", Type::Bool}};
        fn.valueCount = 1;

        Block b0{.id = 0};
        Instruction br;
        br.op = Op::Branch;
        br.operands = {0};
        br.target = BlockTarget{.block = 1, .args = {}};
        br.elseTarget = BlockTarget{.block = 1, .args = {}};
        b0.instructions.push_back(br);
        fn.blocks.push_back(std::move(b0));

        Block b1{.id = 1};
        b1.instructions.push_back({Op::Ret, Type::Void});
        fn.blocks.push_back(std::move(b1));

        m.functions.push_back(std::move(fn));

        bronze::DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
        CHECK(diags.hasErrors());
    }

    SUBCASE("within-block use-after-def") {
        Module m;
        m.name = "bad_use";
        Function fn;
        fn.name = "test";
        fn.valueCount = 2;

        Block b0{.id = 0};
        b0.instructions.push_back({Op::Add, Type::F64, 0, {1, 1}});
        b0.instructions.push_back({Op::ConstF64, Type::F64, 1, {}, 5.0});
        b0.instructions.push_back({Op::Ret, Type::Void, kNoValue, {0}});
        fn.blocks.push_back(std::move(b0));

        m.functions.push_back(std::move(fn));

        bronze::DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
        CHECK(diags.hasErrors());
    }

    SUBCASE("missing block terminator") {
        Module m;
        m.name = "missing_term";
        Function fn;
        fn.name = "test";
        fn.valueCount = 1;

        Block b0{.id = 0};
        b0.instructions.push_back({Op::ConstF64, Type::F64, 0, {}, 1.0});
        fn.blocks.push_back(std::move(b0));

        m.functions.push_back(std::move(fn));

        bronze::DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
        CHECK(diags.hasErrors());
    }
}

// "Argument count and types match target block parameters" is one of the
// verifier's jobs. Block arguments are the SSA join, so a malformed one becomes
// an LLVM phi incoming value; every shape below has to be rejected before
// codegen ever sees it.
TEST_CASE("IL Verifier checks block arguments") {
    // b0: jump b1(<args>)   b1(<params>): ret
    auto twoBlockModule = [](std::vector<BlockParam> params, std::vector<ValueId> args,
                             std::vector<Instruction> b0Prefix, uint32_t valueCount) {
        Module m;
        m.name = "blockargs";
        Function fn;
        fn.name = "test";
        fn.valueCount = valueCount;

        Block b0{.id = 0};
        for (auto& inst : b0Prefix) b0.instructions.push_back(inst);
        Instruction jump;
        jump.op = Op::Jump;
        jump.target = BlockTarget{.block = 1, .args = std::move(args)};
        b0.instructions.push_back(jump);
        fn.blocks.push_back(std::move(b0));

        Block b1{.id = 1};
        b1.params = std::move(params);
        b1.instructions.push_back({Op::Ret, Type::Void});
        fn.blocks.push_back(std::move(b1));

        m.functions.push_back(std::move(fn));
        return m;
    };

    Instruction constF64;
    constF64.op = Op::ConstF64;
    constF64.type = Type::F64;
    constF64.result = 0;
    constF64.immF64 = 1.0;

    SUBCASE("a well-formed edge verifies") {
        Module m = twoBlockModule({BlockParam{1, Type::F64}}, {0}, {constF64}, 2);
        bronze::DiagnosticSink diags;
        CHECK(verify(m, diags));
        CHECK_FALSE(diags.hasErrors());
    }

    SUBCASE("kNoValue as a block parameter id is not a definition") {
        // The hole this closes: recording kNoValue as a definition made every
        // later use of it legal, so `jump b1(%4294967295)` type-checked and
        // reached codegen.
        Module m = twoBlockModule({BlockParam{kNoValue, Type::F64}}, {kNoValue}, {}, 1);
        bronze::DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
        CHECK(diags.hasErrors());
        bronze::SourceBuffer buf("il", "");
        CHECK(diags.render(buf).find("no-value sentinel") != std::string::npos);
    }

    SUBCASE("kNoValue as a block argument is named") {
        Module m = twoBlockModule({BlockParam{1, Type::F64}}, {kNoValue}, {constF64}, 2);
        bronze::DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
        bronze::SourceBuffer buf("il", "");
        CHECK(diags.render(buf).find("argument 0 to b1 is the no-value sentinel") !=
              std::string::npos);
    }

    SUBCASE("an undefined block argument is rejected") {
        Module m = twoBlockModule({BlockParam{1, Type::F64}}, {7}, {constF64}, 2);
        bronze::DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
        bronze::SourceBuffer buf("il", "");
        CHECK(diags.render(buf).find("use of undefined value %7") != std::string::npos);
    }

    SUBCASE("too few block arguments is rejected") {
        Module m = twoBlockModule({BlockParam{1, Type::F64}}, {}, {constF64}, 2);
        bronze::DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
        bronze::SourceBuffer buf("il", "");
        CHECK(diags.render(buf).find("expects 1 arguments, got 0") != std::string::npos);
    }

    SUBCASE("a block argument of the wrong type is rejected") {
        Module m = twoBlockModule({BlockParam{1, Type::Dynamic}}, {0}, {constF64}, 2);
        bronze::DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
        bronze::SourceBuffer buf("il", "");
        CHECK(diags.render(buf).find("type mismatch for argument 0 passed to b1 "
                                     "(argument is f64, parameter is dynamic)") !=
              std::string::npos);
    }

    SUBCASE("a void block parameter is rejected") {
        // The default-constructed BlockParam: no id and no type. Either half
        // alone is enough to reject it.
        Module m = twoBlockModule({BlockParam{1, Type::Void}}, {0}, {constF64}, 2);
        bronze::DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
        bronze::SourceBuffer buf("il", "");
        CHECK(diags.render(buf).find("block b1 parameter 0 has type void") != std::string::npos);
    }

    SUBCASE("br checks both edges") {
        Module m;
        m.name = "blockargs_br";
        Function fn;
        fn.name = "test";
        fn.params = {{"cond", Type::Bool}};
        fn.valueCount = 3;

        Block b0{.id = 0};
        Instruction c;
        c.op = Op::ConstF64;
        c.type = Type::F64;
        c.result = 1;
        c.immF64 = 1.0;
        b0.instructions.push_back(c);
        Instruction br;
        br.op = Op::Branch;
        br.operands = {0};
        br.target = BlockTarget{.block = 1, .args = {1}};
        br.elseTarget = BlockTarget{.block = 2, .args = {kNoValue}};
        b0.instructions.push_back(br);
        fn.blocks.push_back(std::move(b0));

        Block b1{.id = 1};
        b1.params.push_back(BlockParam{2, Type::F64});
        b1.instructions.push_back({Op::Ret, Type::Void});
        fn.blocks.push_back(std::move(b1));

        Block b2{.id = 2};
        b2.params.push_back(BlockParam{3, Type::F64});
        b2.instructions.push_back({Op::Ret, Type::Void});
        fn.blocks.push_back(std::move(b2));

        m.functions.push_back(std::move(fn));

        bronze::DiagnosticSink diags;
        CHECK_FALSE(verify(m, diags));
        bronze::SourceBuffer buf("il", "");
        CHECK(diags.render(buf).find("argument 0 to b2 is the no-value sentinel") !=
              std::string::npos);
    }
}

// `canThrow` decides where the backend puts an exception test, and the list is
// written the safe way round — the cases that answer FALSE are the ones that
// provably cannot raise. Two of them turn on a field rather than on the op,
// because the op is two different operations depending on it.
TEST_CASE("the numeric coercions can throw exactly when they call ToNumber") {
    // `unbox.f64` is 7.1.4 ToNumber, which runs ToPrimitive on an object and is
    // a TypeError for a Symbol. `unbox.bool` is ToBoolean, which is total, and
    // `unbox.i32` reads a number it has already tested for.
    Instruction toNumber{Op::Unbox, Type::F64};
    CHECK(canThrow(toNumber));

    Instruction toBool{Op::Unbox, Type::Bool};
    CHECK_FALSE(canThrow(toBool));

    Instruction toI32{Op::Unbox, Type::I32};
    CHECK_FALSE(canThrow(toI32));

    // 7.1.6 ToInt32 step 1 is that same ToNumber, but only a BOXED operand
    // reaches it — an f64 or i32 operand is a machine conversion with no call
    // in it, which is what keeps a proven-numeric bitwise chain branch-free.
    Instruction boxedBits{Op::ToInt32, Type::I32};
    boxedBits.boxType = Type::Dynamic;
    CHECK(canThrow(boxedBits));

    Instruction numericBits{Op::ToInt32, Type::I32};
    numericBits.boxType = Type::F64;
    CHECK_FALSE(canThrow(numericBits));

    // `add` is the same shape and was already written this way: dynamic `+` is
    // ToPrimitive, and f64 `+` is an instruction.
    Instruction dynamicAdd{Op::Add, Type::Dynamic};
    CHECK(canThrow(dynamicAdd));
    Instruction numericAdd{Op::Add, Type::F64};
    CHECK_FALSE(canThrow(numericAdd));
}
