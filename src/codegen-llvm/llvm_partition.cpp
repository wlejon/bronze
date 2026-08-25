#include "codegen-llvm/llvm_partition.h"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Error.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bronze::codegen_llvm {

namespace {

size_t instCountOf(const llvm::Function& f) {
    size_t n = 0;
    for (const llvm::BasicBlock& bb : f) n += bb.size();
    return n;
}

bool envIsOne(const char* name) {
    const char* env = std::getenv(name);
    return env != nullptr && std::strcmp(env, "1") == 0;
}

}  // namespace

unsigned crossPartitionInlineCap() {
    // The same 2048 the direct-edge inline budget uses (llvm_call.cpp,
    // markDirectMethodInlining), and for the same reason: a body the site's
    // budget will not inline is a body this has no reason to carry. Keeping
    // the two numbers equal is not an accident to be tidied away later — the
    // set this mechanism exists to protect is exactly the set that budget
    // admits, and a cap below it would silently un-protect part of it.
    //
    // Measured, not taken on faith: at 1024, `Matrix4.multiplyMatrices.inl`
    // (1782 instructions) falls outside the cap and `mat4_kernel` reads 18.31
    // ns/call — the E4 regression, unrepaired. At 2048 it reads 16.43. The cap
    // is not free: a kept body costs compile time in every partition that
    // keeps it, +16 % wall on a two-partition build and +22 % on a nine.
    // See the Stage E5 bench-log entry for the sweep.
    static const unsigned cap = [] {
        if (envIsOne("BRONZE_NO_XPART_INLINE")) return 0u;
        if (const char* env = std::getenv("BRONZE_XPART_INLINE_CAP")) {
            return static_cast<unsigned>(std::strtoul(env, nullptr, 10));
        }
        return 2048u;
    }();
    return cap;
}

bool crossPartitionKeepsEveryDirectCall() {
    // Which cross-bin edges are worth carrying a body for. `asked` keeps only
    // the callees of sites the compiler has already marked `alwaysinline` —
    // the direct method/closure edges stages 3.3 and E1 built, which is
    // exactly the set the split can silently un-inline. `all` keeps every
    // direct-call callee under the cap, which additionally lets LLVM's own
    // cost-based inliner see across the split.
    static const bool all = [] {
        const char* env = std::getenv("BRONZE_XPART_INLINE_MODE");
        return env != nullptr && std::strcmp(env, "all") == 0;
    }();
    return all;
}

unsigned crossPartitionInlineDepth() {
    static const unsigned depth = [] {
        if (const char* env = std::getenv("BRONZE_XPART_INLINE_DEPTH")) {
            return static_cast<unsigned>(std::strtoul(env, nullptr, 10));
        }
        return 2u;
    }();
    return depth;
}

PartitionPlan planPartitions(const llvm::Module& m, unsigned parts) {
    PartitionPlan plan;
    plan.keepBodies.resize(parts);
    if (parts == 0) return plan;

    // Partition assignment is bronze's own rather than llvm::SplitModule's:
    // SplitModule buckets by NAME HASH, which landed 2.4x the mean instruction
    // count in one bucket on the three.js bundle and made that bucket the
    // whole critical path. Greedy largest-first into the least-loaded bin is
    // within one function of optimal here, and the sort's name tie-break keeps
    // the assignment deterministic. The floor it cannot beat is the single
    // biggest function — the bundle's top level — which is the next lever, in
    // lowering, not here.
    struct Def {
        const llvm::Function* fn;
        size_t insts;
    };
    std::vector<Def> defs;
    for (const llvm::Function& f : m) {
        if (f.isDeclaration()) continue;
        defs.push_back({&f, instCountOf(f)});
    }
    std::stable_sort(defs.begin(), defs.end(), [](const Def& a, const Def& b) {
        if (a.insts != b.insts) return a.insts > b.insts;
        return a.fn->getName() < b.fn->getName();
    });

    llvm::DenseMap<const llvm::Function*, unsigned> binOfFn;
    llvm::DenseMap<const llvm::Function*, size_t> sizeOfFn;
    {
        std::vector<size_t> load(parts, 0);
        for (const Def& d : defs) {
            unsigned best = 0;
            for (unsigned i = 1; i < parts; ++i) {
                if (load[i] < load[best]) best = i;
            }
            binOfFn[d.fn] = best;
            sizeOfFn[d.fn] = d.insts;
            load[best] += d.insts;
            plan.binOf.emplace(d.fn->getName().str(), best);
        }
    }

    const unsigned cap = crossPartitionInlineCap();
    if (cap == 0 || parts < 2) return plan;
    const unsigned maxDepth = std::max(1u, crossPartitionInlineDepth());

    // The direct-call edges, once, in module order. A callee reached through a
    // function VALUE — a bitcast, an alias, a loaded pointer — is not here:
    // `getCalledFunction` returns null for it, and a callee LLVM cannot name
    // at the site is a callee it could not have inlined anyway.
    const bool keepEveryDirectCall = crossPartitionKeepsEveryDirectCall();
    llvm::DenseMap<const llvm::Function*, std::vector<const llvm::Function*>> calleesOf;
    calleesOf.reserve(static_cast<unsigned>(defs.size()));
    for (const Def& d : defs) {
        std::vector<const llvm::Function*> out;
        llvm::SmallPtrSet<const llvm::Function*, 16> seen;
        for (const llvm::BasicBlock& bb : *d.fn) {
            for (const llvm::Instruction& inst : bb) {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                if (call == nullptr) continue;
                // The site already asked to be inlined: markDirectMethodInlining
                // put `alwaysinline` here after checking the callee against the
                // direct-edge budget. That request is what the split would
                // otherwise silently drop on the floor.
                if (!keepEveryDirectCall && !call->hasFnAttr(llvm::Attribute::AlwaysInline)) {
                    continue;
                }
                const llvm::Function* callee = call->getCalledFunction();
                // A declaration is an ABI helper or an intrinsic: no body in
                // this module to keep. A self-call is not a cross-bin edge.
                if (callee == nullptr || callee->isDeclaration()) continue;
                if (callee == d.fn) continue;
                if (seen.insert(callee).second) out.push_back(callee);
            }
        }
        calleesOf[d.fn] = std::move(out);
    }

    for (unsigned bin = 0; bin < parts; ++bin) {
        // Every member of the bin is in the depth-0 frontier, so depth 1 is
        // "every direct callee of anything this bin owns" — a chain inside the
        // bin costs no depth. Depth only buys the hops through bodies that are
        // themselves kept.
        std::vector<const llvm::Function*> frontier;
        for (const Def& d : defs) {
            if (binOfFn.lookup(d.fn) == bin) frontier.push_back(d.fn);
        }
        llvm::SmallPtrSet<const llvm::Function*, 32> kept;
        for (unsigned depth = 0; depth < maxDepth && !frontier.empty(); ++depth) {
            std::vector<const llvm::Function*> next;
            for (const llvm::Function* f : frontier) {
                auto edges = calleesOf.find(f);
                if (edges == calleesOf.end()) continue;
                for (const llvm::Function* callee : edges->second) {
                    auto owner = binOfFn.find(callee);
                    if (owner == binOfFn.end() || owner->second == bin) continue;
                    if (sizeOfFn.lookup(callee) > cap) continue;
                    if (!kept.insert(callee).second) continue;
                    plan.keepBodies[bin].insert(callee->getName().str());
                    next.push_back(callee);
                }
            }
            frontier = std::move(next);
        }
    }
    return plan;
}

std::string applyPartition(llvm::Module& part, const PartitionPlan& plan, unsigned bin,
                           PartitionStats& stats) {
    const std::unordered_set<std::string>* keep =
        bin < plan.keepBodies.size() ? &plan.keepBodies[bin] : nullptr;

    for (llvm::Function& f : part) {
        if (f.isDeclaration()) continue;
        const std::string name = f.getName().str();
        auto owner = plan.binOf.find(name);
        const bool mine = owner != plan.binOf.end() && owner->second == bin;
        const bool borrowed = !mine && keep != nullptr && keep->count(name) != 0;

        if (mine || borrowed) {
            if (llvm::Error e = f.materialize()) {
                return "materialize failed: " + llvm::toString(std::move(e));
            }
        }
        if (mine) {
            stats.ownInsts += instCountOf(f);
            continue;
        }
        if (borrowed) {
            // `available_externally` is a definition the optimizer may read and
            // copy and the emitter must not emit — the pipeline's own
            // EliminateAvailableExternallyPass drops the body back to a
            // declaration after the inliner has had it, so the object file
            // this partition writes still defines only what it owns. The body
            // is byte-identical to the one the owning bin emits because both
            // came out of the same bitcode, after every per-module pass this
            // backend runs; nothing is rewritten per partition.
            f.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
            stats.keptInsts += instCountOf(f);
            ++stats.keptFns;
        } else {
            // Another bin's function, and nothing here calls it: a declaration.
            f.deleteBody();
        }
        // The export marking goes with the body, and an `available_externally`
        // definition is a declaration as far as the linker is concerned — the
        // verifier rejects dllexport on either.
        f.setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
    }
    return {};
}

}  // namespace bronze::codegen_llvm
