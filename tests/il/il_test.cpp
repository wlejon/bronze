#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "il/print.h"

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
    add.body.push_back({Op::ConstF64, Type::F64, 1, {}, 1.0, 0, 0});
    add.body.push_back({Op::Add, Type::F64, 2, {0, 1}, 0, 0, 0});
    add.body.push_back({Op::Ret, Type::Void, kNoValue, {2}, 0, 0, 0});
    m.functions.push_back(std::move(add));

    Function main;
    main.name = "main";
    main.returnType = Type::F64;
    main.valueCount = 2;
    main.body.push_back({Op::ConstF64, Type::F64, 0, {}, 41.0, 0, 0});
    main.body.push_back({Op::Call, Type::F64, 1, {0}, 0, 0, 0});
    main.body.push_back({Op::Ret, Type::Void, kNoValue, {1}, 0, 0, 0});
    m.functions.push_back(std::move(main));

    CHECK(print(m) ==
          "module demo\n"
          "\n"
          "func addOne(%0: f64) -> f64 export {\n"
          "  %1: f64 = const.f64 1\n"
          "  %2: f64 = add %0, %1\n"
          "  ret %2\n"
          "}\n"
          "\n"
          "func main() -> f64 {\n"
          "  %0: f64 = const.f64 41\n"
          "  %1: f64 = call @addOne(%0)\n"
          "  ret %1\n"
          "}\n");
}

TEST_CASE("float printing is shortest round-trippable, locale-free") {
    Module m;
    m.name = "f";
    Function fn;
    fn.name = "c";
    fn.returnType = Type::F64;
    fn.valueCount = 1;
    fn.body.push_back({Op::ConstF64, Type::F64, 0, {}, 0.1, 0, 0});
    fn.body.push_back({Op::Ret, Type::Void, kNoValue, {0}, 0, 0, 0});
    m.functions.push_back(std::move(fn));
    CHECK(print(m).find("const.f64 0.1\n") != std::string::npos);
}

TEST_CASE("canonical print of Box, Unbox, PropGet, PropSet, DynamicCall") {
    Module m;
    m.name = "dynamic_ops";

    Function fn;
    fn.name = "run";
    fn.params = {{"obj", Type::Dynamic}};
    fn.returnType = Type::Dynamic;
    fn.valueCount = 7;

    // %1: f64 = const.f64 42
    fn.body.push_back({Op::ConstF64, Type::F64, 1, {}, 42.0, 0, 0});
    // %2: dynamic = box.f64 %1
    fn.body.push_back({Op::Box, Type::Dynamic, 2, {1}, 0.0, 0, 0, Type::F64, 0, 0});
    // prop.set %0, 0, %2, 1
    fn.body.push_back({Op::PropSet, Type::Void, kNoValue, {0, 2}, 0.0, 0, 0, Type::Void, 0, 1});
    // %3: dynamic = prop.get %0, 0, 2
    fn.body.push_back({Op::PropGet, Type::Dynamic, 3, {0}, 0.0, 0, 0, Type::Void, 0, 2});
    // %4: f64 = unbox.f64 %3
    fn.body.push_back({Op::Unbox, Type::F64, 4, {3}, 0.0, 0, 0, Type::Void, 0, 0});
    // %5: dynamic = call.dynamic %3, %0, 1, %2
    fn.body.push_back({Op::DynamicCall, Type::Dynamic, 5, {3, 0, 2}, 0.0, 0, 0, Type::Void, 0, 0});
    // ret %5
    fn.body.push_back({Op::Ret, Type::Dynamic, kNoValue, {5}, 0.0, 0, 0});

    m.functions.push_back(std::move(fn));

    CHECK(print(m) ==
          "module dynamic_ops\n"
          "\n"
          "func run(%0: dynamic) -> dynamic {\n"
          "  %1: f64 = const.f64 42\n"
          "  %2: dynamic = box.f64 %1\n"
          "  prop.set %0, 0, %2, 1\n"
          "  %3: dynamic = prop.get %0, 0, 2\n"
          "  %4: f64 = unbox.f64 %3\n"
          "  %5: dynamic = call.dynamic %3, %0, 1, %2\n"
          "  ret %5\n"
          "}\n");
}
