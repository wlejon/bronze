// GUARDED REGIONS: SHAPE GUARDS (src/lower/guard_region.h, "SHAPE GUARDS").
//
// A static-slot site guards its receiver's shape itself, and its miss rejoins.
// In the fast copy a point's `is.shape` tests ask each receiver's guard once and
// fail into the slow copy instead, and the sites behind them are bare loads and
// stores (`shapeGuarded`). The cases here are what a point covers and what ends
// that cover: anything that may move a shape, an ordinary site among them.

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "il/verifier.h"
#include "lower/guard_region.h"
#include "lower_fixture.h"

using namespace bronze;
using namespace bronze::lower;

namespace {

il::Module oneFunction(il::Function fn) {
    il::Module module;
    module.name = "test";
    module.icSiteCount = 64;
    module.staticSiteCount = 64;
    module.keyConstants = {"x", "y", "z", "f"};
    module.classFamilies.resize(4);
    module.functions.push_back(std::move(fn));
    return module;
}

il::Instruction arith(il::Op op, il::ValueId result, il::ValueId a, il::ValueId b) {
    il::Instruction inst;
    inst.op = op;
    inst.type = il::Type::Dynamic;
    inst.result = result;
    inst.operands = {a, b};
    return inst;
}

// `recv.<key>` at slot `key`, through the identity cell `cell` or, when `family`
// is set, the family range 3..3.
il::Instruction slotGet(il::ValueId result, il::ValueId recv, uint32_t key, uint32_t cell, bool family = false) {
    il::Instruction inst;
    inst.op = il::Op::PropGet;
    inst.type = il::Type::Dynamic;
    inst.result = result;
    inst.operands = {recv};
    inst.keyIndex = key;
    inst.icIndex = cell;
    inst.staticSlot = key;
    if (family) {
        inst.familyLo = 3;
        inst.familySpan = 0;
    } else {
        inst.staticCellIndex = cell;
    }
    return inst;
}

il::Instruction slotSet(il::ValueId recv, uint32_t key, il::ValueId value, uint32_t cell) {
    il::Instruction inst;
    inst.op = il::Op::PropSet;
    inst.type = il::Type::Void;
    inst.operands = {recv, value};
    inst.keyIndex = key;
    inst.icIndex = cell;
    inst.immI32 = 1;
    inst.staticSlot = key;
    inst.staticCellIndex = cell;
    return inst;
}

il::Instruction callDynamic(il::ValueId result, il::ValueId callee) {
    il::Instruction inst;
    inst.op = il::Op::DynamicCall;
    inst.type = il::Type::Dynamic;
    inst.result = result;
    inst.operands = {callee, callee};
    return inst;
}

il::Instruction ret(il::ValueId value) {
    il::Instruction inst;
    inst.op = il::Op::Ret;
    inst.operands = {value};
    return inst;
}

il::Block block(il::BlockId id, std::vector<il::Instruction> insts) {
    il::Block b;
    b.id = id;
    b.instructions = std::move(insts);
    return b;
}

std::string body(const il::Module& module) {
    const std::string text = il::print(module);
    const size_t at = text.find("func ");
    return at == std::string::npos ? text : text.substr(at);
}

size_t countOf(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

// `this.x * v.x + this.y * v.y + this.z * v.z`: `this` (%0) through identity
// cells, `v` (%1) through a family.
il::Function dot() {
    il::Function fn;
    fn.name = "dot";
    fn.params = {{"this", il::Type::Dynamic}, {"v", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 13;
    fn.blocks = {block(0, {slotGet(2, 0, 0, 10), slotGet(3, 1, 0, 0, true), arith(il::Op::Mul, 4, 2, 3),
                           slotGet(5, 0, 1, 11), slotGet(6, 1, 1, 0, true), arith(il::Op::Mul, 7, 5, 6),
                           arith(il::Op::Add, 8, 4, 7), slotGet(9, 0, 2, 12), slotGet(10, 1, 2, 0, true),
                           arith(il::Op::Mul, 11, 9, 10), arith(il::Op::Add, 12, 8, 11), ret(12)})};
    return fn;
}

}  // namespace

TEST_CASE("a dot product takes one point: its shapes tested once, every read a load") {
    il::Module module = oneFunction(dot());
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    CHECK(stats.guardPoints == 1);
    CHECK(stats.runGuards == 1);
    // Three cells for `this`, one family range for `v` however many slots.
    CHECK(stats.shapeTests == 4);
    CHECK(stats.shapeGuarded == 6);

    const std::string text = body(module);
    CHECK(countOf(text, "is.shape %0") == 3);
    CHECK(countOf(text, "is.shape %1") == 1);
    CHECK(countOf(text, "shape-guarded") == 6);
    // The shape tests stand in front of the reads and the number tests after
    // them, so a failing value test enters the slow copy having read nothing
    // the slow copy cannot read again.
    const size_t lastShape = text.rfind("is.shape");
    const size_t firstGuarded = text.find("shape-guarded");
    const size_t firstNumber = text.find("is.number");
    REQUIRE(lastShape != std::string::npos);
    CHECK(lastShape < firstGuarded);
    CHECK(firstGuarded < firstNumber);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("a call ends a point's cover and the reads after it are guarded again") {
    // `a = this.x + this.y; f(); b = this.z * this.x + this.y`. The call may
    // change any shape, so the reads after it cannot lean on the tests before
    // it: they take a point of their own, one family test for all three.
    il::Function fn;
    fn.name = "f";
    fn.params = {{"this", il::Type::Dynamic}, {"g", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 13;
    fn.blocks = {block(0, {slotGet(2, 0, 0, 10, true), slotGet(3, 0, 1, 11, true), arith(il::Op::Add, 4, 2, 3),
                           arith(il::Op::Mul, 5, 4, 2), callDynamic(6, 1), slotGet(7, 0, 2, 12, true),
                           slotGet(8, 0, 0, 13, true), arith(il::Op::Mul, 9, 7, 8), slotGet(10, 0, 1, 14, true),
                           arith(il::Op::Add, 11, 9, 10), arith(il::Op::Add, 12, 11, 5), ret(12)})};
    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    CHECK(stats.shapeGuarded == 5);
    const std::string text = body(module);
    const size_t call = text.find("call.dynamic", text.find("is.shape"));
    REQUIRE(call != std::string::npos);
    // Tests on both sides of the fast copy's call.
    CHECK(text.find("is.shape %0, slot 0 family 3..3") < call);
    CHECK(text.find("is.shape %0, slot 2 family 3..3") > call);
    CHECK(countOf(text, "is.shape %0") == 2);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("a store of a promoted number is a bare store; a store of anything else stays a site") {
    // `this.x = this.x * this.y; this.z = g; this.y = this.y + this.x`. The
    // first store's value is promoted arithmetic, a Number the fast copy boxes,
    // so it is covered. `g` is a parameter nothing tested, and a slot that holds
    // doubles would refuse it: that store keeps its own guard, its miss may run
    // a setter, and the reads after it are guarded again.
    il::Function fn;
    fn.name = "f";
    fn.params = {{"this", il::Type::Dynamic}, {"g", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 10;
    fn.blocks = {block(0, {slotGet(2, 0, 0, 10), slotGet(3, 0, 1, 11), arith(il::Op::Mul, 4, 2, 3),
                           arith(il::Op::Mul, 5, 4, 3), slotSet(0, 0, 5, 12), slotSet(0, 2, 1, 13),
                           slotGet(6, 0, 1, 14, true), slotGet(7, 0, 0, 15, true), arith(il::Op::Add, 8, 6, 7),
                           arith(il::Op::Mul, 9, 8, 6), ret(9)})};
    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    const std::string text = body(module);
    CHECK(countOf(text, "prop.set %0, 0, ") == 2);  // one per copy
    CHECK(countOf(text, "slot 0@12, shape-guarded") == 1);
    CHECK(countOf(text, "slot 2@13, shape-guarded") == 0);
    // The reads after the ordinary store are covered by a test after it.
    const size_t plainStore = text.find("slot 2@13", text.find("is.shape"));
    REQUIRE(plainStore != std::string::npos);
    CHECK(text.find("is.shape %0, slot 1 family 3..3", plainStore) != std::string::npos);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("a read with no slot claim ends a shape run") {
    // `this.x * this.f() ...` reduced to its reads: `this.f` has no static
    // slot, so it is an ordinary access that may run a getter, and no read
    // after it may be merged under the tests in front of it.
    il::Instruction plain;
    plain.op = il::Op::PropGet;
    plain.type = il::Type::Dynamic;
    plain.result = 4;
    plain.operands = {0};
    plain.keyIndex = 3;
    plain.icIndex = 20;
    il::Function fn;
    fn.name = "f";
    fn.params = {{"this", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 10;
    fn.blocks = {block(0, {slotGet(1, 0, 0, 10, true), slotGet(2, 0, 1, 11, true), arith(il::Op::Mul, 3, 1, 2), plain,
                           arith(il::Op::Mul, 5, 3, 4), slotGet(6, 0, 2, 12, true), slotGet(7, 0, 0, 13, true),
                           arith(il::Op::Mul, 8, 6, 7), arith(il::Op::Add, 9, 8, 5), ret(9)})};
    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    const std::string text = body(module);
    const size_t fastPlain = text.find("prop.get %0, 3, 20", text.find("is.shape"));
    REQUIRE(fastPlain != std::string::npos);
    // The reads after the plain one are guarded by tests after it.
    CHECK(text.find("is.shape %0, slot 2 family 3..3", fastPlain) != std::string::npos);
    CHECK(countOf(text.substr(0, fastPlain), "slot 2 family 3..3, shape-guarded") == 0);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("a repeated read inside a shape run is the first read in the fast copy") {
    // `this.x * this.x + this.y * this.y`: each key read twice, through two
    // sites with two cells. Behind the run's shape tests both reads of a key
    // are one load, so the fast copy keeps the first and tests it alone.
    il::Function fn;
    fn.name = "lengthSq";
    fn.params = {{"this", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 8;
    fn.blocks = {block(0, {slotGet(1, 0, 0, 10), slotGet(2, 0, 0, 11), arith(il::Op::Mul, 3, 1, 2),
                           slotGet(4, 0, 1, 12), slotGet(5, 0, 1, 13), arith(il::Op::Mul, 6, 4, 5),
                           arith(il::Op::Add, 7, 3, 6), ret(7)})};
    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    CHECK(stats.guards == 2);
    CHECK(stats.shapeTests == 2);
    const std::string text = body(module);
    CHECK(countOf(text, "shape-guarded") == 2);
    CHECK(countOf(text, "slot 0@11") == 1);  // the slow copy's alone
    CHECK(countOf(text, "slot 1@13") == 1);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}
