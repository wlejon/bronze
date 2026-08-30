// What the affinity packer promises, on modules small enough to state it
// exactly (src/codegen-llvm/llvm_partition.h).
//
// Two properties, and the second is the one this file exists for:
//
//   - an inlinable call edge does not straddle a bin, because the packer
//     unions the pair into one cluster before it balances anything;
//   - the assignment of every OTHER function does not depend on how big the
//     dominant function is. The legacy packer picks the least-loaded bin at
//     every step, so on the three.js math graph a 250-instruction change to the
//     module top level re-seated 479 of 556 definitions and moved the
//     benchmarks several percent with no code change anywhere. That is a
//     measurement hazard for every codegen chunk, and it is what a prefix cut
//     over clusters removes.
//
// Stated as two MODULES differing only in the dominant function's size rather
// than as the `BRONZE_XPART_PAD` seam, because the seam is a process-wide
// static: a test cannot hold two values of it at once, and the modules say the
// same thing without the indirection.

#include <doctest/doctest.h>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "codegen-llvm/llvm_call.h"
#include "codegen-llvm/llvm_partition.h"

using namespace bronze;

namespace {

// A function of exactly `insts` instructions: `insts - 1` adds and the `ret`.
llvm::Function* sizedFn(llvm::Module& m, llvm::StringRef name, unsigned insts) {
    llvm::LLVMContext& ctx = m.getContext();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    auto* fn = llvm::Function::Create(llvm::FunctionType::get(i64Ty, {i64Ty}, false),
                                      llvm::GlobalValue::ExternalLinkage, name, m);
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(ctx, "entry", fn));
    llvm::Value* v = fn->getArg(0);
    for (unsigned i = 1; i < insts; ++i) v = b.CreateAdd(v, llvm::ConstantInt::get(i64Ty, 1));
    b.CreateRet(v);
    return fn;
}

// A direct method edge as the backend leaves one: the metadata
// markDirectMethodInlining reads and the `alwaysinline` it spends.
void directCall(llvm::Function* caller, llvm::Function* callee) {
    llvm::BasicBlock& entry = caller->getEntryBlock();
    llvm::IRBuilder<> b(&entry, entry.begin());
    auto* call = b.CreateCall(callee, {caller->getArg(0)});
    call->setMetadata(codegen_llvm::kDirectMethodMD,
                      llvm::MDNode::get(caller->getContext(), {}));
    call->addFnAttr(llvm::Attribute::AlwaysInline);
}

unsigned binOfName(const codegen_llvm::PartitionPlan& plan, llvm::StringRef name) {
    auto it = plan.binOf.find(name.str());
    REQUIRE(it != plan.binOf.end());
    return it->second;
}

// One dominant definition — the shape a bundle's module top level makes — and a
// spread of ordinary ones around it. `dominant` is the only thing that varies
// between the two modules the stability case builds.
struct Graph {
    std::unique_ptr<llvm::Module> m;
    std::vector<std::string> names;
};

// Proportions taken from the real thing rather than invented: on the three.js
// math graph the module top level is ~76k of ~750k instructions across 556
// definitions, so the dominant function is a tenth of the module and sits well
// under one bin's share. A dominant function BIGGER than a bin's share would
// make the test trivially stable — it would be alone in bin 0, and nothing
// downstream could feel it — and would prove nothing about the shape that
// actually occurs.
Graph buildGraph(llvm::LLVMContext& ctx, unsigned dominant) {
    Graph g;
    g.m = std::make_unique<llvm::Module>("xpart_stability", ctx);
    sizedFn(*g.m, "top_level", dominant);
    for (unsigned i = 0; i < 120; ++i) {
        // Distinct sizes, so nothing here is placed on a name tie-break and the
        // ordering the packer sees is the ordering the sizes give it.
        char name[16];
        std::snprintf(name, sizeof(name), "fn_%03u", i);
        sizedFn(*g.m, name, 60 + i * 7);
        g.names.emplace_back(name);
    }
    return g;
}

}  // namespace

TEST_CASE("an inlinable edge does not straddle a bin") {
    llvm::LLVMContext ctx;
    llvm::Module m("xpart_affinity", ctx);
    // Under the legacy packer these sizes force the pair apart: 400 -> b0,
    // 400 -> b1, caller(100) -> b0, callee(50) -> b1.
    sizedFn(m, "filler_a", 400);
    sizedFn(m, "filler_b", 400);
    llvm::Function* caller = sizedFn(m, "caller", 100);
    llvm::Function* callee = sizedFn(m, "callee", 50);
    directCall(caller, callee);

    const codegen_llvm::PartitionPlan plan = codegen_llvm::planPartitions(m, 2);
    if (codegen_llvm::partitionUsesLegacyPacker()) return;  // the A/B seam column
    if (codegen_llvm::crossPartitionInlineCap() == 0) return;

    const unsigned home = binOfName(plan, "caller");
    CHECK(binOfName(plan, "callee") == home);
    // And because the edge never crossed, no bin has to carry the body: the
    // clustering did the keep mechanism's job for this edge instead of paying
    // for it with a duplicated body in every bin that calls it.
    for (const auto& kept : plan.keepBodies) CHECK(kept.count("callee") == 0);
}

TEST_CASE("a cluster may not grow past one bin's share") {
    llvm::LLVMContext ctx;
    llvm::Module m("xpart_cluster_cap", ctx);
    // A chain of inlinable edges whose total is most of the module. Unioning
    // all of it would leave one cluster no bin could hold, so the packer has to
    // refuse an edge — the alternative is a partition that cannot be balanced
    // at all, which is worse than an un-inlined call.
    llvm::Function* a = sizedFn(m, "chain_a", 400);
    llvm::Function* b = sizedFn(m, "chain_b", 300);
    llvm::Function* c = sizedFn(m, "chain_c", 200);
    directCall(a, b);
    directCall(b, c);
    sizedFn(m, "other", 100);

    const codegen_llvm::PartitionPlan plan = codegen_llvm::planPartitions(m, 4);
    if (codegen_llvm::partitionUsesLegacyPacker()) return;  // the A/B seam column
    // The chain did not collapse into one cluster: the union that would have
    // done it was refused, and the pair it refused went back to the keep
    // mechanism, which carries a body instead of constraining the packer.
    CHECK(plan.clusters >= 3);
    const unsigned homeA = binOfName(plan, "chain_a");
    CHECK(binOfName(plan, "chain_b") != homeA);
    if (codegen_llvm::crossPartitionInlineCap() != 0) {
        CHECK(plan.keepBodies[homeA].count("chain_b") == 1);
    }
    // Every bin is non-empty: a packer that empties a bin has thrown away the
    // thread the split exists to use.
    std::vector<unsigned> used(4, 0);
    for (const auto& entry : plan.binOf) ++used[entry.second];
    for (const unsigned n : used) CHECK(n > 0);
}

TEST_CASE("growing the dominant function does not re-seat the rest of the module") {
    llvm::LLVMContext ctx;
    const Graph base = buildGraph(ctx, 4000);
    const Graph grown = buildGraph(ctx, 4300);

    const codegen_llvm::PartitionPlan planBase = codegen_llvm::planPartitions(*base.m, 4);
    const codegen_llvm::PartitionPlan planGrown = codegen_llvm::planPartitions(*grown.m, 4);
    if (codegen_llvm::partitionUsesLegacyPacker()) return;  // the A/B seam column

    unsigned moved = 0;
    for (const std::string& name : base.names) {
        if (binOfName(planBase, name) != binOfName(planGrown, name)) ++moved;
    }
    // The bound the design gives, and the reason it is a bound rather than
    // zero: a prefix cut has `parts - 1` boundaries, and a size change can only
    // move a cluster that sits within that change of one of them. Nothing else
    // can feel it. The legacy packer's least-loaded pick has no such bound — on
    // the three.js math graph it re-seated 479 of 556 for the same perturbation.
    CHECK(moved <= 3);

    // The stability is not bought with a lopsided split: no bin is more than a
    // fifth off the mean.
    std::vector<size_t> load(4, 0);
    size_t total = 0;
    for (const llvm::Function& f : *base.m) {
        if (f.isDeclaration()) continue;
        size_t insts = 0;
        for (const llvm::BasicBlock& bb : f) insts += bb.size();
        load[binOfName(planBase, f.getName())] += insts;
        total += insts;
    }
    for (const size_t l : load) {
        CHECK(l * 4 * 5 >= total * 4);      // >= 0.8 x mean
        CHECK(l * 4 * 5 <= total * 6);      // <= 1.2 x mean
    }
}

TEST_CASE("the partition trace is stable and sorted by name") {
    llvm::LLVMContext ctx;
    const Graph g = buildGraph(ctx, 4000);

    const codegen_llvm::PartitionPlan first = codegen_llvm::planPartitions(*g.m, 4);
    const codegen_llvm::PartitionPlan second = codegen_llvm::planPartitions(*g.m, 4);
    const std::string a = codegen_llvm::describePartition(*g.m, first);
    const std::string b = codegen_llvm::describePartition(*g.m, second);
    CHECK(a == b);

    // Header lines, then one `bin<TAB>insts<TAB>name` line per definition, in
    // name order — which is what makes "how many functions moved" a line diff
    // rather than a set comparison.
    std::vector<std::string> lines;
    for (size_t at = 0; at < a.size();) {
        const size_t nl = a.find('\n', at);
        REQUIRE(nl != std::string::npos);
        lines.push_back(a.substr(at, nl - at));
        at = nl + 1;
    }
    REQUIRE(lines.size() == 1 + 4 + 121);
    CHECK(lines[0].rfind("xpart: packer=", 0) == 0);
    std::string prev;
    for (size_t i = 5; i < lines.size(); ++i) {
        const size_t tab = lines[i].rfind('\t');
        REQUIRE(tab != std::string::npos);
        const std::string name = lines[i].substr(tab + 1);
        CHECK(name > prev);
        prev = name;
    }
}
