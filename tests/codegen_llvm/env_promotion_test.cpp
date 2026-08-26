// Stage R3: which regions form, which do not, and what the rewrite leaves.
//
// Checked against hand-written LLVM IR rather than against a compiled fixture,
// for the reason `repr_test.cpp` gives for checking a plan against hand-built
// IL: a region-end cause read off eleven instructions is exact, where the same
// cause read off a compiled program is arithmetic on a count. The other half of
// this stage's coverage is behavioural and lives in `tests/oracle/cases` —
// those cases assert what a program PRINTS, which is the only test that would
// catch a region that spans an observer.

#include <doctest/doctest.h>

#include <memory>
#include <string>

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Transforms/Utils/LoopSimplify.h>

#include "codegen-llvm/llvm_env_promote.h"
#include "codegen-llvm/llvm_env_reach.h"

using namespace bronze;
using codegen_llvm::RegionEnd;

namespace {

// The alias metadata every fixture ends with: the EnvRecordSlots scope exactly
// as `getScopedAliasInfo` builds it, plus one other heap family so a fixture
// can spell a store the analysis must NOT read as an environment access.
constexpr const char* kMetadata = R"(
!0 = !{!1}
!1 = !{!"EnvRecordSlots", !2}
!2 = !{!"BronzeAliasDomain"}
!3 = !{!4}
!4 = !{!"ObjectPropertySlots", !2}
!5 = !{}
)";

struct Promoted {
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module> module;
    codegen_llvm::EnvPromotionStats stats;
};

// Parses `ir`, brings its loops into the form the pass requires (which the O3
// pipeline guarantees at the extension point the pass really runs at), then
// runs the pass and verifies the result.
Promoted promote(const std::string& ir) {
    Promoted out;
    out.ctx = std::make_unique<llvm::LLVMContext>();
    llvm::SMDiagnostic err;
    out.module = llvm::parseAssemblyString(ir + kMetadata, err, *out.ctx);
    REQUIRE_MESSAGE(out.module != nullptr, err.getMessage().str());

    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb;
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);

    llvm::ModulePassManager mpm;
    mpm.addPass(llvm::createModuleToFunctionPassAdaptor(llvm::LoopSimplifyPass()));
    mpm.addPass(codegen_llvm::EnvPromotionPass(&out.stats));
    mpm.run(*out.module, mam);

    std::string verifyError;
    llvm::raw_string_ostream os(verifyError);
    REQUIRE_MESSAGE(!llvm::verifyModule(*out.module, &os), verifyError);
    return out;
}

// Recognized environment-slot accesses in one named block, by kind.
struct SlotCounts {
    unsigned loads = 0;
    unsigned stores = 0;
};

SlotCounts slotAccessesIn(llvm::Module& module, const char* fnName, const char* blockName) {
    SlotCounts counts;
    llvm::Function* fn = module.getFunction(fnName);
    REQUIRE(fn != nullptr);
    for (llvm::BasicBlock& block : *fn) {
        if (block.getName() != blockName) continue;
        for (llvm::Instruction& inst : block) {
            if (!codegen_llvm::matchEnvSlotAccess(inst, module.getDataLayout()).has_value()) {
                continue;
            }
            if (llvm::isa<llvm::StoreInst>(inst)) {
                ++counts.stores;
            } else {
                ++counts.loads;
            }
        }
    }
    return counts;
}

SlotCounts slotAccessesInFunction(llvm::Module& module, const char* fnName) {
    SlotCounts counts;
    llvm::Function* fn = module.getFunction(fnName);
    REQUIRE(fn != nullptr);
    for (llvm::BasicBlock& block : *fn) {
        for (llvm::Instruction& inst : block) {
            if (!codegen_llvm::matchEnvSlotAccess(inst, module.getDataLayout()).has_value()) {
                continue;
            }
            if (llvm::isa<llvm::StoreInst>(inst)) {
                ++counts.stores;
            } else {
                ++counts.loads;
            }
        }
    }
    return counts;
}

unsigned endCount(const codegen_llvm::EnvPromotionStats& stats, RegionEnd cause) {
    return stats.ends[static_cast<size_t>(cause)];
}

// The canonical fixture: a counting loop over slot 0 of the record the function
// is handed. `preamble` goes in the entry block (which is where every case puts
// its opaque call, so the whole-function region is refused and the LOOP region
// is what is under test); `body` goes in the loop, between the load and the
// store.
std::string loopKernel(const std::string& declarations, const std::string& preamble,
                       const std::string& body) {
    return declarations + R"(
define i64 @kernel(i64 %env, i64 %n) {
entry:
  %payload = and i64 %env, 281474976710655
  %hdr = inttoptr i64 %payload to ptr
  %slot = getelementptr inbounds i8, ptr %hdr, i64 16
)" + preamble + R"(
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %v = load i64, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %vd = bitcast i64 %v to double
  %sd = fadd double %vd, 1.000000e+00
  %sum = bitcast double %sd to i64
)" + body + R"(
  store i64 %sum, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  %final = load i64, ptr %slot, align 8, !alias.scope !0, !noalias !3
  ret i64 %final
}
)";
}

// Every case is a statement about the mechanism, so the seam column asserts
// nothing rather than asserting the opposite.
bool stageIsOff() { return codegen_llvm::envPromotionDisabled(); }

constexpr const char* kOpaque = "declare void @opaque()\n";

}  // namespace

TEST_CASE("a loop no call can see keeps its slot in a register across the backedge") {
    if (stageIsOff()) return;
    Promoted p = promote(loopKernel(kOpaque, "  call void @opaque()", ""));

    CHECK(p.stats.slotsPromoted == 1);
    CHECK(p.stats.loopRegions == 1);
    CHECK(p.stats.loadsElided == 1);
    CHECK(p.stats.storesElided == 1);

    // The loop body is what the stage is for: no slot traffic at all.
    const SlotCounts inLoop = slotAccessesIn(*p.module, "kernel", "loop");
    CHECK(inLoop.loads == 0);
    CHECK(inLoop.stores == 0);

    // One load at the region's entry, one write-back on the exit that is taken,
    // and the read after the loop still reads the heap — which is exactly what
    // makes the write-back load-bearing.
    CHECK(p.stats.entryLoads == 1);
    CHECK(p.stats.writeBacks == 1);
    const SlotCounts whole = slotAccessesInFunction(*p.module, "kernel");
    CHECK(whole.loads == 2);
    CHECK(whole.stores == 1);
}

TEST_CASE("a call the analysis cannot see through ends the region") {
    if (stageIsOff()) return;
    Promoted p = promote(loopKernel(kOpaque, "", "  call void @opaque()"));

    CHECK(p.stats.slotsPromoted == 0);
    CHECK(endCount(p.stats, RegionEnd::UnknownCall) >= 1);
    const SlotCounts inLoop = slotAccessesIn(*p.module, "kernel", "loop");
    CHECK(inLoop.loads == 1);
    CHECK(inLoop.stores == 1);
}

TEST_CASE("a call through a value ends the region, and says so") {
    if (stageIsOff()) return;
    Promoted p = promote(loopKernel(kOpaque, "  call void @opaque()",
                                    "  %fp = inttoptr i64 %v to ptr\n"
                                    "  call void %fp()"));
    CHECK(p.stats.slotsPromoted == 0);
    CHECK(endCount(p.stats, RegionEnd::IndirectCall) >= 1);
}

TEST_CASE("an environment helper ends the region, and is counted apart from a user call") {
    if (stageIsOff()) return;
    // No opaque call in the preamble: this case is about which BUCKET the
    // refusal lands in, so the env helper must be the only observer there is.
    Promoted p = promote(loopKernel("declare i64 @bronze_env_get(i64, i32, i32)\n", "",
                                    "  %g = call i64 @bronze_env_get(i64 %env, i32 0, i32 1)"));
    CHECK(p.stats.slotsPromoted == 0);
    CHECK(endCount(p.stats, RegionEnd::EnvHelperCall) >= 1);
    CHECK(endCount(p.stats, RegionEnd::UnknownCall) == 0);
}

TEST_CASE("a call that touches no memory does not end a region") {
    if (stageIsOff()) return;
    Promoted p =
        promote(loopKernel(std::string(kOpaque) + "declare i64 @pure(i64) memory(none)\n",
                           "  call void @opaque()", "  %q = call i64 @pure(i64 %v)"));
    CHECK(p.stats.slotsPromoted == 1);
    const SlotCounts inLoop = slotAccessesIn(*p.module, "kernel", "loop");
    CHECK(inLoop.loads == 0);
    CHECK(inLoop.stores == 0);
}

TEST_CASE("a defined callee that touches no record does not end a region") {
    if (stageIsOff()) return;
    const char* decls = R"(
declare void @opaque()
define internal i64 @leaf(i64 %x) noinline {
  %r = add i64 %x, 7
  ret i64 %r
}
)";
    Promoted p = promote(loopKernel(decls, "  call void @opaque()",
                                    "  %q = call i64 @leaf(i64 %v)"));
    CHECK(p.stats.slotsPromoted == 1);
}

TEST_CASE("a defined callee that touches a record DOES end a region") {
    if (stageIsOff()) return;
    const char* decls = R"(
declare void @opaque()
define internal i64 @peeks(i64 %e) noinline {
  %p = and i64 %e, 281474976710655
  %h = inttoptr i64 %p to ptr
  %s = getelementptr inbounds i8, ptr %h, i64 16
  %v = load i64, ptr %s, align 8, !alias.scope !0, !noalias !3
  ret i64 %v
}
)";
    Promoted p = promote(loopKernel(decls, "  call void @opaque()",
                                    "  %q = call i64 @peeks(i64 %env)"));
    CHECK(p.stats.slotsPromoted == 0);
    CHECK(endCount(p.stats, RegionEnd::UnknownCall) >= 1);
}

TEST_CASE("a callee that only reaches a record through a callee still ends the region") {
    if (stageIsOff()) return;
    const char* decls = R"(
declare void @opaque()
define internal i64 @peeks(i64 %e) noinline {
  %p = and i64 %e, 281474976710655
  %h = inttoptr i64 %p to ptr
  %s = getelementptr inbounds i8, ptr %h, i64 16
  %v = load i64, ptr %s, align 8, !alias.scope !0, !noalias !3
  ret i64 %v
}
define internal i64 @wraps(i64 %e) noinline {
  %r = call i64 @peeks(i64 %e)
  ret i64 %r
}
)";
    Promoted p = promote(loopKernel(decls, "  call void @opaque()",
                                    "  %q = call i64 @wraps(i64 %env)"));
    CHECK(p.stats.slotsPromoted == 0);
}

TEST_CASE("a store to another slot of the same record is disjoint bytes, not an observer") {
    if (stageIsOff()) return;
    Promoted p = promote(loopKernel(
        kOpaque, "  %other = getelementptr inbounds i8, ptr %hdr, i64 24\n  call void @opaque()",
        "  store i64 3, ptr %other, align 8, !alias.scope !0, !noalias !3"));
    // Both slots promote: neither can be the other's bytes.
    CHECK(p.stats.slotsPromoted == 2);
}

TEST_CASE("the same slot reached through a second record value ends the region") {
    if (stageIsOff()) return;
    Promoted p = promote(loopKernel(
        kOpaque,
        "  %p2 = and i64 %n, 281474976710655\n"
        "  %h2 = inttoptr i64 %p2 to ptr\n"
        "  %s2 = getelementptr inbounds i8, ptr %h2, i64 16\n"
        "  call void @opaque()",
        "  store i64 5, ptr %s2, align 8, !alias.scope !0, !noalias !3"));
    CHECK(endCount(p.stats, RegionEnd::AliasingEnvSlot) >= 1);
    CHECK(p.stats.slotsPromoted == 0);
}

TEST_CASE("a block that ends in unreachable takes no write-back") {
    if (stageIsOff()) return;
    const std::string ir = std::string(kOpaque) + R"(
declare void @bronze_env_access_failed(i64, i32, i32) noreturn cold

define i64 @kernel(i64 %env, i64 %n) {
entry:
  %payload = and i64 %env, 281474976710655
  %hdr = inttoptr i64 %payload to ptr
  %slot = getelementptr inbounds i8, ptr %hdr, i64 16
  call void @opaque()
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
  %v = load i64, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %ok = icmp ult i64 %v, 100
  br i1 %ok, label %latch, label %trap

trap:
  call void @bronze_env_access_failed(i64 %env, i32 0, i32 0)
  unreachable

latch:
  %sum = add i64 %v, 1
  store i64 %sum, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i64 0
}
)";
    Promoted p = promote(ir);
    CHECK(p.stats.slotsPromoted == 1);
    // One exit takes the write-back; the `unreachable` one does not.
    CHECK(p.stats.writeBacks == 1);
    CHECK(slotAccessesIn(*p.module, "kernel", "trap").stores == 0);
}

TEST_CASE("a heap-valued store in a region that can collect refuses the slot") {
    if (stageIsOff()) return;
    // -3940649673818112 is 0xFFF2_0000_0002_0000: the String tag over a payload.
    Promoted p = promote(loopKernel(
        std::string(kOpaque) + "declare i64 @bronze_env_create(i64, i32)\n",
        "  call void @opaque()",
        "  %fresh = call i64 @bronze_env_create(i64 %env, i32 4)\n"
        "  store i64 -3940649673818112, ptr %slot, align 8, !alias.scope !0, !noalias !3"));
    CHECK(endCount(p.stats, RegionEnd::HeapValueStore) >= 1);
    CHECK(p.stats.slotsPromoted == 0);
}

TEST_CASE("the same region promotes once the stored value is proven not a pointer") {
    if (stageIsOff()) return;
    Promoted p = promote(loopKernel(
        std::string(kOpaque) + "declare i64 @bronze_env_create(i64, i32)\n",
        "  call void @opaque()",
        "  %fresh = call i64 @bronze_env_create(i64 %env, i32 4)\n"
        "  store i64 -3940649673818112, ptr %slot, align 8, !alias.scope !0, !noalias !3, "
        "!bronze.env.nonptr !5"));
    CHECK(p.stats.slotsPromoted == 1);
}

TEST_CASE("a region that stores nothing needs no write-back at all") {
    if (stageIsOff()) return;
    const std::string ir = std::string(kOpaque) + R"(
declare i64 @sink(i64)

define i64 @kernel(i64 %env, i64 %n) {
entry:
  %payload = and i64 %env, 281474976710655
  %hdr = inttoptr i64 %payload to ptr
  %slot = getelementptr inbounds i8, ptr %hdr, i64 16
  call void @opaque()
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %acc = phi i64 [ 0, %entry ], [ %next, %loop ]
  %v = load i64, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %next = add i64 %acc, %v
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i64 %next
}
)";
    Promoted p = promote(ir);
    CHECK(p.stats.slotsPromoted == 1);
    CHECK(p.stats.loadsElided == 1);
    CHECK(p.stats.storesElided == 0);
    CHECK(p.stats.writeBacks == 0);
}

TEST_CASE("a whole function no call can see is one region, written back at every return") {
    if (stageIsOff()) return;
    const char* ir = R"(
define i64 @leafclosure(i64 %env, i64 %flag) {
entry:
  %payload = and i64 %env, 281474976710655
  %hdr = inttoptr i64 %payload to ptr
  %slot = getelementptr inbounds i8, ptr %hdr, i64 16
  %v = load i64, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %c = icmp eq i64 %flag, 0
  br i1 %c, label %yes, label %no

yes:
  store i64 11, ptr %slot, align 8, !alias.scope !0, !noalias !3
  ret i64 %v

no:
  %w = load i64, ptr %slot, align 8, !alias.scope !0, !noalias !3
  ret i64 %w
}
)";
    Promoted p = promote(ir);
    CHECK(p.stats.functionRegions == 1);
    CHECK(p.stats.loopRegions == 0);
    CHECK(p.stats.loadsElided == 2);
    CHECK(p.stats.storesElided == 1);
    // Both `ret`s carry the write-back: an exit the analysis forgot is a stale
    // slot read by someone with a legitimate view.
    CHECK(p.stats.writeBacks == 2);
}

TEST_CASE("a record defined inside the loop is not a key the loop can hold") {
    if (stageIsOff()) return;
    const std::string ir = std::string(kOpaque) + R"(
declare i64 @next_record(i64)

define void @kernel(i64 %env, i64 %n) {
entry:
  call void @opaque()
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %rec = call i64 @next_record(i64 %i)
  %payload = and i64 %rec, 281474976710655
  %hdr = inttoptr i64 %payload to ptr
  %slot = getelementptr inbounds i8, ptr %hdr, i64 16
  %v = load i64, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %sum = add i64 %v, 1
  store i64 %sum, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %exit, label %loop

exit:
  ret void
}
)";
    Promoted p = promote(ir);
    CHECK(p.stats.slotsPromoted == 0);
}

TEST_CASE("an unrecognized store alias analysis cannot separate ends the region") {
    if (stageIsOff()) return;
    // A store through a pointer with no family at all: `inttoptr` is where the
    // provenance walk stops, so nothing can say it is not the record.
    Promoted p = promote(loopKernel(kOpaque, "  call void @opaque()",
                                    "  %anyp = inttoptr i64 %n to ptr\n"
                                    "  store i64 9, ptr %anyp, align 8"));
    CHECK(p.stats.slotsPromoted == 0);
    CHECK(endCount(p.stats, RegionEnd::AliasingMemory) >= 1);
}

TEST_CASE("a store to a different storage family does not end the region") {
    if (stageIsOff()) return;
    Promoted p = promote(loopKernel(kOpaque, "  call void @opaque()",
                                    "  %anyp = inttoptr i64 %n to ptr\n"
                                    "  store i64 9, ptr %anyp, align 8, !alias.scope !3, "
                                    "!noalias !0"));
    CHECK(p.stats.slotsPromoted == 1);
}

TEST_CASE("a store into a global is not an observer, which is what makes a throw one too") {
    if (stageIsOff()) return;
    // `throw` in this runtime is exactly this: a store into the exception cell
    // and a branch to the handler edge (llvm_func.cpp). No call, no unwind.
    const std::string ir = std::string(kOpaque) + R"(
@bronze_exception_cell = external global i64

define i64 @kernel(i64 %env, i64 %n) {
entry:
  %payload = and i64 %env, 281474976710655
  %hdr = inttoptr i64 %payload to ptr
  %slot = getelementptr inbounds i8, ptr %hdr, i64 16
  call void @opaque()
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %latch ]
  %v = load i64, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %vd = bitcast i64 %v to double
  %sd = fadd double %vd, 1.000000e+00
  %sum = bitcast double %sd to i64
  store i64 %sum, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %hot = icmp ugt i64 %sum, 99
  br i1 %hot, label %raise, label %latch

raise:
  store i64 7, ptr @bronze_exception_cell, align 8
  br label %handler

latch:
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %handler, label %loop

handler:
  %final = load i64, ptr %slot, align 8, !alias.scope !0, !noalias !3
  ret i64 %final
}
)";
    Promoted p = promote(ir);
    CHECK(p.stats.slotsPromoted == 1);
    CHECK(slotAccessesIn(*p.module, "kernel", "loop").stores == 0);
    // Both edges out of the loop reach the handler, and after the split each
    // carries a write-back of its own.
    CHECK(p.stats.writeBacks == 2);
}

TEST_CASE("an exit block shared with code outside the loop is split, not refused") {
    if (stageIsOff()) return;
    const std::string ir = std::string(kOpaque) + R"(
define i64 @kernel(i64 %env, i64 %n, i1 %skip) {
entry:
  %payload = and i64 %env, 281474976710655
  %hdr = inttoptr i64 %payload to ptr
  %slot = getelementptr inbounds i8, ptr %hdr, i64 16
  call void @opaque()
  br i1 %skip, label %after, label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %i.next, %loop ]
  %v = load i64, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %vd = bitcast i64 %v to double
  %sd = fadd double %vd, 1.000000e+00
  %sum = bitcast double %sd to i64
  store i64 %sum, ptr %slot, align 8, !alias.scope !0, !noalias !3
  %i.next = add i64 %i, 1
  %done = icmp eq i64 %i.next, %n
  br i1 %done, label %after, label %loop

after:
  %final = load i64, ptr %slot, align 8, !alias.scope !0, !noalias !3
  ret i64 %final
}
)";
    Promoted p = promote(ir);
    CHECK(p.stats.slotsPromoted == 1);
    CHECK(p.stats.writeBacks == 1);
    // The write-back is NOT in `after`: an ordinary path into it would have run
    // one. It is in the block the loop's own edge was split into.
    CHECK(slotAccessesIn(*p.module, "kernel", "after").stores == 0);
    CHECK(slotAccessesIn(*p.module, "kernel", "after").loads == 1);
}

TEST_CASE("an access to the record header is not a slot and is never promoted") {
    if (stageIsOff()) return;
    const char* ir = R"(
define i64 @kernel(i64 %env) {
entry:
  %payload = and i64 %env, 281474976710655
  %hdr = inttoptr i64 %payload to ptr
  %parent = getelementptr inbounds i8, ptr %hdr, i64 8
  %a = load i64, ptr %parent, align 8, !alias.scope !0, !noalias !3
  %b = load i64, ptr %parent, align 8, !alias.scope !0, !noalias !3
  %s = add i64 %a, %b
  ret i64 %s
}
)";
    Promoted p = promote(ir);
    CHECK(p.stats.keys == 0);
    CHECK(p.stats.slotsPromoted == 0);
}
