// GUARDED NUMERIC REGIONS: THE READ-RUN MERGE (src/lower/guard_region.h).
//
// Coalescing only ever moves a guard EARLIER, so a kernel that reads and
// consumes one element at a time takes one guard point per read and the fast
// copy is one read per block — which is the shape the backend's run-arm planner
// cannot put on one arm. This is the rule that moves a point LATER instead, past
// a run of constant-index reads off one receiver, and the licence it spends:
// `is.dense_array`, tested in front of the run's FIRST read so that its failing
// edge enters the slow copy having read nothing.
//
// The cases here are the boundaries of "one run": what interrupts it, and what
// the merge must refuse rather than reorder. A read this rule merges past is
// re-run by the slow copy, and a read that can run a getter must never be.

#include <doctest/doctest.h>

#include <string>

#include "il/print.h"
#include "il/verifier.h"
#include "lower/guard_region.h"
#include "lower_fixture.h"

using namespace bronze;
using namespace bronze::lower;

namespace {

// The key pool the reads name. Indices 0..3 are canonical array indices and
// `k` is not, which is the whole difference the run rule turns on.
il::Module oneFunction(il::Function fn) {
    il::Module module;
    module.name = "test";
    module.icSiteCount = 64;
    module.keyConstants = {"0", "1", "2", "3", "k"};
    module.functions.push_back(std::move(fn));
    return module;
}

constexpr uint32_t kKeyNamed = 4;

il::Instruction add(il::ValueId result, il::ValueId a, il::ValueId b) {
    il::Instruction inst;
    inst.op = il::Op::Add;
    inst.type = il::Type::Dynamic;
    inst.result = result;
    inst.operands = {a, b};
    return inst;
}

il::Instruction propGet(il::ValueId result, il::ValueId receiver, uint32_t key, uint32_t ic) {
    il::Instruction inst;
    inst.op = il::Op::PropGet;
    inst.type = il::Type::Dynamic;
    inst.result = result;
    inst.operands = {receiver};
    inst.keyIndex = key;
    inst.icIndex = ic;
    return inst;
}

il::Instruction propSet(il::ValueId receiver, uint32_t key, il::ValueId value, uint32_t ic) {
    il::Instruction inst;
    inst.op = il::Op::PropSet;
    inst.type = il::Type::Void;
    inst.operands = {receiver, value};
    inst.keyIndex = key;
    inst.icIndex = ic;
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
    for (size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

bool seamIsOn() { return guardedRegionsDisabled() || regionRunGuardsDisabled(); }

// The `applyMatrix4` shape: read, consume, read, consume. Each read's first
// promoted use is the `add` right after it, so coalescing gives one point per
// read and has nothing to coalesce; two `add`s per read keep the region over the
// ratio floor that refuses a guard per operation.
//
// The middle read is the variable: `secondReceiver` and `secondKey` are what the
// two refusal cases change.
il::Function interleavedReads(uint32_t secondKey, il::ValueId secondReceiver) {
    il::Function fn;
    fn.name = "f";
    fn.params = {{"a", il::Type::Dynamic}, {"b", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 12;
    fn.blocks = {block(0, {propGet(2, 0, 0, 0), propGet(3, 0, 1, 1), add(4, 2, 3), add(5, 4, 2),
                           propGet(6, secondReceiver, secondKey, 2), add(7, 5, 6), add(8, 7, 6),
                           propGet(9, 0, 3, 3), add(10, 8, 9), add(11, 10, 9), ret(11)})};
    return fn;
}

}  // namespace

TEST_CASE("a run of constant-index reads off one receiver takes one guard point") {
    if (seamIsOn()) return;
    // Every read is on `%0` and every key is an index, so the whole block is one
    // run: one `is.dense_array` in front of the first read, the reads and their
    // arithmetic under it, and the four `is.number`s after the last read.
    il::Module module = oneFunction(interleavedReads(2, 0));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    CHECK(stats.guards == 4);
    CHECK(stats.guardPoints == 1);
    CHECK(stats.runGuards == 1);

    const std::string text = body(module);
    // The claim covers the largest index the run reads and no more.
    CHECK(countOf(text, "is.dense_array %0, 3") == 1);
    CHECK(countOf(text, "is.number") == 4);
    // The fast copy's four reads are in ONE block: no `br` between them, which
    // is the whole point — a span the run-arm planner can put on one arm.
    const size_t runAt = text.find("is.dense_array");
    const size_t firstNumber = text.find("is.number");
    REQUIRE(runAt != std::string::npos);
    REQUIRE(firstNumber != std::string::npos);
    const std::string between = text.substr(runAt, firstNumber - runAt);
    CHECK(countOf(between, "prop.get %0") == 4);
    CHECK(countOf(between, "br ") == 1);  // the dense test's own

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("a read off another receiver ends the run rather than joining it") {
    if (seamIsOn()) return;
    // `a[0] + a[1] + b[2] + a[3]`. One `is.dense_array` cannot answer for two
    // objects, and the reads after `b[2]` are not the reads the claim was made
    // about — so the run stops in front of it and every point stands.
    il::Module module = oneFunction(interleavedReads(2, 1));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    CHECK(stats.guards == 4);
    CHECK(stats.guardPoints == 3);
    CHECK(stats.runGuards == 0);
    CHECK(countOf(body(module), "is.dense_array") == 0);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("a NAMED read on the run's own receiver ends it") {
    if (seamIsOn()) return;
    // `a[0] + a[1] + a.k + a[3]`. `is.dense_array` says an INDEX is answered out
    // of the element block; it says nothing about a name, and `a.k` may be an
    // accessor. Merging past it would let the slow copy run that getter a second
    // time, so the run stops in front of it.
    il::Module module = oneFunction(interleavedReads(kKeyNamed, 0));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    CHECK(stats.guardPoints == 3);
    CHECK(stats.runGuards == 0);
    CHECK(countOf(body(module), "is.dense_array") == 0);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}

TEST_CASE("a store between two reads ends the run") {
    if (seamIsOn()) return;
    // `a[0], a[1], a[2]` merged into one run, then `a.k = t`, then `a[3]`.
    //
    // The store is the boundary and it is a soundness one, not a heuristic: the
    // slow copy performs it too, so a merged point whose failing edge entered
    // the slow copy in front of the first read would perform it twice. So the
    // run stops at the store, the three reads in front of it take one point
    // between them, and the read after it keeps its own.
    il::Function fn;
    fn.name = "f";
    fn.params = {{"a", il::Type::Dynamic}};
    fn.returnType = il::Type::Dynamic;
    fn.valueCount = 11;
    fn.blocks = {block(0, {propGet(1, 0, 0, 0), propGet(2, 0, 1, 1), add(3, 1, 2), add(4, 3, 1),
                           propGet(5, 0, 2, 2), add(6, 4, 5), add(7, 6, 5),
                           propSet(0, kKeyNamed, 7, 3), propGet(8, 0, 3, 4), add(9, 7, 8),
                           add(10, 9, 8), ret(10)})};

    il::Module module = oneFunction(std::move(fn));
    GuardRegionStats stats;
    REQUIRE(applyGuardedRegions(module, &stats));

    CHECK(stats.duplicated == 1);
    CHECK(stats.guards == 4);
    CHECK(stats.guardPoints == 2);
    CHECK(stats.runGuards == 1);

    const std::string text = body(module);
    CHECK(countOf(text, "is.dense_array %0, 2") == 1);
    // The store is NOT under the claim: it stands after the run's own
    // `is.number` chain, which is what makes the failing edge safe to take.
    // Searched forward from the claim, because the SLOW copy is printed first
    // and holds a store of its own.
    const size_t denseAt = text.find("is.dense_array");
    REQUIRE(denseAt != std::string::npos);
    const size_t storeAt = text.find("prop.set", denseAt);
    const size_t numberAt = text.find("is.number", denseAt);
    REQUIRE(storeAt != std::string::npos);
    REQUIRE(numberAt != std::string::npos);
    CHECK(numberAt < storeAt);

    DiagnosticSink diags;
    CHECK(il::verify(module, diags));
}
