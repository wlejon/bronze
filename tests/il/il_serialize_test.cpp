// The IL's binary encoding (il/serialize.h): a module decodes to the module
// encoded, and a damaged encoding is refused rather than read past its end.
#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

#include "il/print.h"
#include "il/serialize.h"

using namespace bronze::il;

namespace {

// Every field of every struct set to something other than its default, so a
// field the encoder dropped shows up as a difference.
Module fullModule() {
    Module m;
    m.name = "full";
    m.keyConstants = {"x", "y", "hello world", ""};
    m.icSiteCount = 7;
    m.templateSiteCount = 2;
    m.staticSiteCount = 3;
    m.classFamilies.push_back({"Point", {{0, true}, {1, false}}});
    m.slotReprFields = {0, 1};
    m.censusSites.push_back({2, 5});
    m.censusOutPath = "census.json";
    m.nativeImports.push_back({"fn a.b", "(f64)->f64", 1, 3});
    m.sourceFiles = {"D:/a.js", "D:/b.js"};

    Function fn;
    fn.name = "f";
    fn.params = {{"__env", Type::Dynamic, false, 0}, {"x", Type::F64, true, 3}};
    fn.returnType = Type::F64;
    fn.returnPinned = true;
    fn.returnPinKeyIndex = 2;
    fn.isExported = true;
    fn.isEntryPoint = true;
    fn.needsEnv = true;
    fn.needsThis = true;
    fn.needsArguments = true;
    fn.isStrict = true;
    fn.isGenerator = true;
    fn.coroKind = 2;
    fn.fnFlags = 5;
    fn.hasRestParam = true;
    fn.requiredArgs = 1;
    fn.nameKeyIndex = 0;
    fn.sourceFile = 1;
    fn.sourceBegin = 10;
    fn.sourceEnd = 40;
    fn.displayName = "f";
    fn.descFlags = 9;
    fn.valueCount = 12;

    Block b;
    b.id = 3;
    b.params = {{4, Type::I32}, {5, Type::Str}};
    b.copyClass = CopyClass::Fast;
    b.copyRegion = 1;
    b.handler = 2;
    Instruction inst{Op::PropGet, Type::Dynamic, 6, {1, 2, 3}, -0.0, -17, 4};
    inst.boxType = Type::F64;
    inst.keyIndex = 1;
    inst.icIndex = 6;
    inst.icMonomorphic = true;
    inst.icFnRecv = true;
    inst.rawUnbox = true;
    inst.nullishUnbox = true;
    inst.staticSlot = 2;
    inst.staticCellIndex = 1;
    inst.familyLo = 0;
    inst.familySpan = 3;
    inst.directTarget = 0;
    inst.envDepth = 2;
    inst.envIndex = 7;
    inst.envImmutable = true;
    inst.callEnvHops = 1;
    inst.span = {100, 90, 1};  // end before begin still round-trips
    inst.target = {4, {1, 2}};
    inst.elseTarget = {5, {}};
    b.instructions.push_back(inst);
    b.instructions.push_back({Op::ConstF64, Type::F64, 7, {}, 3.25, 0, 0});
    b.instructions.push_back({Op::Ret, Type::Void, kNoValue, {7}, 0, 0, 0});
    fn.blocks.push_back(b);
    fn.blocks.push_back(Block{});
    m.functions.push_back(fn);

    Function ext;
    ext.name = "__bronze_native_0";
    ext.isExternal = true;
    m.functions.push_back(ext);
    return m;
}

}  // namespace

TEST_CASE("a module round-trips through its binary encoding") {
    const Module m = fullModule();
    const std::string bytes = serializeModule(m);
    Module back;
    std::string err;
    REQUIRE(deserializeModule(bytes, back, err));
    CHECK(serializeModule(back) == bytes);
    CHECK(print(back) == print(m));

    const Instruction& a = m.functions[0].blocks[0].instructions[0];
    const Instruction& b = back.functions[0].blocks[0].instructions[0];
    CHECK(std::signbit(b.immF64));
    CHECK(b.immI32 == a.immI32);
    CHECK(b.span.begin == a.span.begin);
    CHECK(b.span.end == a.span.end);
    CHECK(b.span.file == a.span.file);
    CHECK(b.target.args == a.target.args);
    CHECK(b.callEnvHops == a.callEnvHops);
    CHECK(b.nullishUnbox);
    CHECK(back.functions[0].blocks[1].handler == kNoBlock);
    CHECK(back.functions[0].blocks[0].instructions[2].result == kNoValue);
    CHECK(back.functions[0].coroKind == 2);
    CHECK(back.functions[1].isExternal);
    CHECK(back.nativeImports[0].bufferReturnKind == 3);
    CHECK(back.sourceFiles == m.sourceFiles);
}

TEST_CASE("source texts and line tables are attached, not encoded") {
    Module m = fullModule();
    std::vector<std::string_view> texts = {"a\nb\n", "c"};
    attachSources(m, texts, /*retainTexts=*/true);
    const std::string bytes = serializeModule(m);
    Module back;
    std::string err;
    REQUIRE(deserializeModule(bytes, back, err));
    CHECK(back.sourceTexts.empty());
    attachSources(back, texts, /*retainTexts=*/false);
    CHECK(back.sourceTexts.empty());
    REQUIRE(back.lineTables.size() == 2);
    CHECK(back.lineTables[0].lineCol(2).line == 2);
    attachSources(back, texts, /*retainTexts=*/true);
    CHECK(back.sourceTexts == std::vector<std::string>{"a\nb\n", "c"});
}

TEST_CASE("every truncation of an encoding is refused") {
    const std::string bytes = serializeModule(fullModule());
    for (size_t n = 0; n < bytes.size(); ++n) {
        Module back;
        std::string err;
        CHECK_FALSE(deserializeModule(std::string_view(bytes.data(), n), back, err));
        CHECK(!err.empty());
    }
}

TEST_CASE("damaged bytes decode or fail, never read out of bounds") {
    const std::string bytes = serializeModule(fullModule());
    for (size_t i = 0; i < bytes.size(); ++i) {
        for (unsigned flip : {0x01u, 0x80u, 0xFFu}) {
            std::string damaged = bytes;
            damaged[i] = static_cast<char>(static_cast<unsigned char>(damaged[i]) ^ flip);
            Module back;
            std::string err;
            (void)deserializeModule(damaged, back, err);
        }
    }
    std::string trailing = bytes + "x";
    Module back;
    std::string err;
    CHECK_FALSE(deserializeModule(trailing, back, err));
}
