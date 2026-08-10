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
