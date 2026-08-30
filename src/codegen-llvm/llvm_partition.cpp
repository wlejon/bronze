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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

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

// One definition, with two sizes: `insts` is the truth — what the inline cap
// and the stats are measured against — and `packInsts` is what the packer
// reads, which the pad seam moves. Keeping them apart is what makes the seam an
// instrument: padding must change WHERE a body lands and nothing else about it.
struct Def {
    const llvm::Function* fn;
    size_t insts;
    size_t packInsts;
};

// Definitions largest first, name tie-break. Both packers place in this order,
// so the pad seam perturbs the same function under either one.
std::vector<Def> sortedDefs(const llvm::Module& m, size_t& padApplied) {
    std::vector<Def> defs;
    size_t total = 0;
    for (const llvm::Function& f : m) {
        if (f.isDeclaration()) continue;
        const size_t n = instCountOf(f);
        total += n;
        defs.push_back({&f, n, n});
    }
    std::stable_sort(defs.begin(), defs.end(), [](const Def& a, const Def& b) {
        if (a.insts != b.insts) return a.insts > b.insts;
        return a.fn->getName() < b.fn->getName();
    });
    // A perturbation is only a perturbation if it is SMALL. The seam asks what
    // a few hundred instructions out of hundreds of thousands do to the
    // assignment; the same few hundred against a thousand-instruction module is
    // not a nudge, it is a different module, and the answer it gives is about
    // nothing. One percent is the line, and `padApplied` carries what was
    // really done so the trace never claims a pad it did not take.
    padApplied = 0;
    if (const unsigned pad = partitionPadInsts();
        pad != 0 && !defs.empty() && static_cast<size_t>(pad) * 100 <= total) {
        defs[0].packInsts += pad;
        padApplied = pad;
    }
    return defs;
}

// The callee sets the keep fixpoint walks and the clustering unions — the same
// edges for both, deliberately: a cluster exists to spare the keep mechanism an
// edge it would otherwise have to carry a whole body across.
//
// A callee reached through a function VALUE — a bitcast, an alias, a loaded
// pointer — is not here: `getCalledFunction` returns null for it, and a callee
// LLVM cannot name at the site is a callee it could not have inlined anyway.
llvm::DenseMap<const llvm::Function*, std::vector<const llvm::Function*>> directCallEdges(
    const std::vector<Def>& defs, bool keepEveryDirectCall) {
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
    return calleesOf;
}

// Union-find over definitions, indexed by position in the `defs` order.
struct Clustering {
    std::vector<unsigned> parent;
    std::vector<size_t> weight;  // valid at a root: the cluster's total insts

    explicit Clustering(const std::vector<Def>& defs) {
        parent.resize(defs.size());
        weight.resize(defs.size());
        for (unsigned i = 0; i < defs.size(); ++i) {
            parent[i] = i;
            weight[i] = defs[i].packInsts;
        }
    }

    unsigned find(unsigned i) {
        while (parent[i] != i) {
            parent[i] = parent[parent[i]];
            i = parent[i];
        }
        return i;
    }

    // Merges only if the result still fits a bin. A cluster bigger than a bin
    // cannot be placed at all, and one that is merely close to a bin's size
    // hands the packer a lump it must build a whole bin around — so the cap is
    // what keeps "keep inlinable pairs together" from collapsing the call graph
    // into a single component, which on a bundle it otherwise does.
    bool unite(unsigned a, unsigned b, size_t cap) {
        a = find(a);
        b = find(b);
        if (a == b) return true;
        if (weight[a] + weight[b] > cap) return false;
        // Lower index wins the root, so the merged cluster's identity does not
        // depend on which edge happened to be walked first.
        if (b < a) std::swap(a, b);
        parent[b] = a;
        weight[a] += weight[b];
        return true;
    }
};

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

bool partitionUsesLegacyPacker() {
    static const bool legacy = envIsOne("BRONZE_XPART_LEGACY");
    return legacy;
}

unsigned partitionPadInsts() {
    static const unsigned pad = [] {
        if (const char* env = std::getenv("BRONZE_XPART_PAD")) {
            return static_cast<unsigned>(std::strtoul(env, nullptr, 10));
        }
        return 0u;
    }();
    return pad;
}

namespace {

// Greedy largest-first into the least-loaded bin — what shipped before the
// affinity packer, kept whole behind `BRONZE_XPART_LEGACY=1` so one binary
// answers both. Within one function of optimal on balance, and that is the
// whole of its case: the least-loaded pick reads every previous placement, so
// it turns a small size change anywhere into a different assignment everywhere.
void packGreedy(const std::vector<Def>& defs, unsigned parts,
                llvm::DenseMap<const llvm::Function*, unsigned>& binOfFn) {
    std::vector<size_t> load(parts, 0);
    for (const Def& d : defs) {
        unsigned best = 0;
        for (unsigned i = 1; i < parts; ++i) {
            if (load[i] < load[best]) best = i;
        }
        binOfFn[d.fn] = best;
        load[best] += d.packInsts;
    }
}

// Clusters of definitions the split must not come between, cut into bins by a
// prefix sum. See llvm_partition.h for why a prefix cut is the stable shape and
// the least-loaded pick is not.
void packAffinity(const std::vector<Def>& defs, unsigned parts,
                  const llvm::DenseMap<const llvm::Function*, std::vector<const llvm::Function*>>&
                      calleesOf,
                  unsigned inlineCap, PartitionPlan& plan,
                  llvm::DenseMap<const llvm::Function*, unsigned>& binOfFn) {
    llvm::DenseMap<const llvm::Function*, unsigned> indexOf;
    size_t total = 0;
    for (unsigned i = 0; i < defs.size(); ++i) {
        indexOf[defs[i].fn] = i;
        total += defs[i].packInsts;
    }

    Clustering clusters(defs);
    // A cluster may not exceed one bin's share. Bigger than that and it is not
    // placeable; at exactly that the packer has no room left to balance with.
    const size_t clusterCap = std::max<size_t>(1, total / parts);

    // Union order is by CALLER NAME, not by size — the one place where reading
    // sizes would leak the pad seam (and any real size change) into the cluster
    // shape itself, which is exactly the cascade being fixed. Names do not move.
    std::vector<unsigned> byName(defs.size());
    for (unsigned i = 0; i < defs.size(); ++i) byName[i] = i;
    std::stable_sort(byName.begin(), byName.end(), [&](unsigned a, unsigned b) {
        return defs[a].fn->getName() < defs[b].fn->getName();
    });
    for (const unsigned i : byName) {
        auto edges = calleesOf.find(defs[i].fn);
        if (edges == calleesOf.end()) continue;
        for (const llvm::Function* callee : edges->second) {
            // A body over the cap is one the keep mechanism would refuse to
            // carry and the inliner would refuse to copy, so binding the pair
            // buys nothing and costs the packer a constraint.
            auto at = indexOf.find(callee);
            if (at == indexOf.end() || defs[at->second].insts > inlineCap) continue;
            clusters.unite(i, at->second, clusterCap);
        }
    }

    // One entry per cluster: total size, and the smallest member index in the
    // largest-first order, which is both a stable identity and the right thing
    // to order by, since the biggest cluster is the one whose placement matters.
    struct Bundle {
        unsigned root;
        size_t insts;
    };
    std::vector<Bundle> bundles;
    std::vector<unsigned> bundleAt(defs.size(), 0);
    for (unsigned i = 0; i < defs.size(); ++i) {
        if (clusters.find(i) != i) continue;
        bundleAt[i] = static_cast<unsigned>(bundles.size());
        bundles.push_back({i, clusters.weight[i]});
    }
    std::stable_sort(bundles.begin(), bundles.end(), [&](const Bundle& a, const Bundle& b) {
        if (a.insts != b.insts) return a.insts > b.insts;
        return a.root < b.root;  // already the largest-first, name-tie-broken order
    });

    plan.clusters = bundles.size();
    plan.largestCluster = bundles.empty() ? 0 : bundles.front().insts;

    std::vector<std::vector<unsigned>> members(bundles.size());
    for (unsigned i = 0; i < defs.size(); ++i) {
        members[bundleAt[clusters.find(i)]].push_back(i);
    }
    // bundleAt was filled in root order; re-point it at the sorted positions.
    std::vector<std::vector<unsigned>> sortedMembers(bundles.size());
    for (unsigned b = 0; b < bundles.size(); ++b) {
        sortedMembers[b] = std::move(members[bundleAt[bundles[b].root]]);
    }

    // The cut. Each bin takes a contiguous run of the ordered clusters, closing
    // when one more would land further from the bin's share than stopping does.
    // The share is recomputed from what is LEFT, so a bin that overshot is paid
    // for by the bins after it rather than by the last one.
    size_t remaining = total;
    unsigned binsLeft = parts;
    unsigned next = 0;
    for (unsigned bin = 0; bin < parts; ++bin) {
        const bool last = bin + 1 == parts;
        const size_t share = last ? remaining : remaining / binsLeft;
        size_t acc = 0;
        while (next < bundles.size()) {
            // Every later bin needs a cluster of its own; a packer that empties
            // a bin has thrown away a thread.
            if (!last && bundles.size() - next <= binsLeft - 1) break;
            const size_t take = bundles[next].insts;
            // A bin closes at the cluster that would leave it further from its
            // share than stopping does. The `acc >= share` test is not
            // redundant with that: once a bin has accepted an overshoot there
            // is no undershoot left to weigh against, and the subtraction that
            // would weigh it is unsigned.
            if (!last && acc != 0) {
                if (acc >= share) break;
                const size_t over = acc + take > share ? acc + take - share : 0;
                if (over > share - acc) break;
            }
            for (const unsigned member : sortedMembers[next]) binOfFn[defs[member].fn] = bin;
            acc += take;
            ++next;
        }
        remaining -= acc;
        --binsLeft;
    }
}

}  // namespace

PartitionPlan planPartitions(const llvm::Module& m, unsigned parts) {
    PartitionPlan plan;
    plan.keepBodies.resize(parts);
    if (parts == 0) return plan;

    // Partition assignment is bronze's own rather than llvm::SplitModule's:
    // SplitModule buckets by NAME HASH, which landed 2.4x the mean instruction
    // count in one bucket on the three.js bundle and made that bucket the
    // whole critical path.
    const std::vector<Def> defs = sortedDefs(m, plan.padApplied);

    const unsigned cap = crossPartitionInlineCap();
    const bool keepEveryDirectCall = crossPartitionKeepsEveryDirectCall();
    // Built before the packing, not after it: the affinity packer clusters on
    // exactly the edges the keep fixpoint below walks.
    const llvm::DenseMap<const llvm::Function*, std::vector<const llvm::Function*>> calleesOf =
        directCallEdges(defs, keepEveryDirectCall);

    llvm::DenseMap<const llvm::Function*, unsigned> binOfFn;
    llvm::DenseMap<const llvm::Function*, size_t> sizeOfFn;
    for (const Def& d : defs) sizeOfFn[d.fn] = d.insts;
    if (partitionUsesLegacyPacker() || parts < 2) {
        packGreedy(defs, parts, binOfFn);
    } else {
        // With the mechanism off there is nothing to cluster FOR, and an
        // affinity cluster would silently re-couple what the A/B seam was set
        // to decouple; the prefix cut still runs, so the stability is kept.
        packAffinity(defs, parts, calleesOf, cap, plan, binOfFn);
    }
    for (const Def& d : defs) plan.binOf.emplace(d.fn->getName().str(), binOfFn.lookup(d.fn));

    if (cap == 0 || parts < 2) {
        if (envIsOne("BRONZE_XPART_TRACE")) {
            const std::string text = describePartition(m, plan);
            std::fwrite(text.data(), 1, text.size(), stderr);
        }
        return plan;
    }
    const unsigned maxDepth = std::max(1u, crossPartitionInlineDepth());

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
    if (envIsOne("BRONZE_XPART_TRACE")) {
        const std::string text = describePartition(m, plan);
        std::fwrite(text.data(), 1, text.size(), stderr);
    }
    return plan;
}

std::string describePartition(const llvm::Module& m, const PartitionPlan& plan) {
    // Sizes as the module has them, not as the pad seam reports them: a trace
    // that echoed the pad back would hide the very thing it is read for.
    std::vector<std::pair<std::string, size_t>> rows;
    for (const llvm::Function& f : m) {
        if (f.isDeclaration()) continue;
        rows.emplace_back(f.getName().str(), instCountOf(f));
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    const unsigned parts = static_cast<unsigned>(plan.keepBodies.size());
    std::vector<size_t> load(parts, 0);
    std::vector<size_t> count(parts, 0);
    for (const auto& [name, insts] : rows) {
        auto owner = plan.binOf.find(name);
        if (owner == plan.binOf.end() || owner->second >= parts) continue;
        load[owner->second] += insts;
        ++count[owner->second];
    }

    std::string out = "xpart: packer=";
    out += partitionUsesLegacyPacker() ? "legacy" : "affinity";
    out += " parts=" + std::to_string(parts) + " fns=" + std::to_string(rows.size()) +
           " pad=" + std::to_string(plan.padApplied) +
           " clusters=" + std::to_string(plan.clusters) +
           " largest_cluster=" + std::to_string(plan.largestCluster) + "\n";
    for (unsigned b = 0; b < parts; ++b) {
        out += "xpart: bin " + std::to_string(b) + " insts=" + std::to_string(load[b]) +
               " fns=" + std::to_string(count[b]) +
               " keep=" + std::to_string(plan.keepBodies[b].size()) + "\n";
    }
    for (const auto& [name, insts] : rows) {
        auto owner = plan.binOf.find(name);
        const std::string bin =
            owner == plan.binOf.end() ? std::string("-") : std::to_string(owner->second);
        out += bin + "\t" + std::to_string(insts) + "\t" + name + "\n";
    }
    return out;
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
