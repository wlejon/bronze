// GUARDED REGIONS: the module driver (guard_region.h). Each function's loop
// regions under the growth budget, then its entry region, and the per-function
// trace and the aggregate report.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lower/guard_region.h"
#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

// The per-function growth budget is "no more than the function's own size", and
// this is the floor under it. Without one the budget refuses exactly the case
// the pass exists for: in `sumProps` the loop IS the function, so a copy of it
// plus two guards and three trampolines is more instructions than the whole
// function had. Doubling something this small is not a code-size problem, and
// the cap that matters — a region of 400 instructions — is the one above.
constexpr size_t kGrowthFloor = 64;
// A function is rewritten at most this many times. Each successful rewrite
// makes its own region unselectable — the fast copy's arithmetic is `f64` and
// the slow copy has gained edges into blocks that are not its header — so this
// is a belt on a loop that already terminates.
constexpr int kMaxRegionsPerFunction = 8;

// The aggregate counters say how often a reason fired; they cannot say WHERE,
// and "where" is what decides whether a reason is worth a rule — fifty refusals
// spread over cold constructors are not the same fact as five over the
// functions a benchmark spends its time in. This is the per-function view, off
// unless asked for, and deterministic because it walks the module's functions
// in order and prints one line each.
bool guardRegionTraceOn() {
    static const bool on = [] {
        const char* env = std::getenv("BRONZE_GUARDED_REGION_TRACE");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return on;
}

// The counters that moved while one function was examined, as `reason=n` pairs.
// Empty when nothing moved, which is the common case and the one worth not
// printing.
std::string traceDelta(const GuardRegionStats& before, const GuardRegionStats& after) {
    const std::pair<const char*, std::pair<uint32_t, uint32_t>> fields[] = {
        {"dup", {before.duplicated, after.duplicated}},
        {"guards", {before.guards, after.guards}},
        {"points", {before.guardPoints, after.guardPoints}},
        {"runGuards", {before.runGuards, after.runGuards}},
        {"numberClaims", {before.numberClaims, after.numberClaims}},
        {"numberSkipped", {before.numberSkipped, after.numberSkipped}},
        {"unboxFolded", {before.unboxFolded, after.unboxFolded}},
        {"shapeTests", {before.shapeTests, after.shapeTests}},
        {"shapeGuarded", {before.shapeGuarded, after.shapeGuarded}},
        {"icExits", {before.icExits, after.icExits}},
        {"icExitsDropped", {before.icExitsDropped, after.icExitsDropped}},
        {"handler", {before.refusedHandler, after.refusedHandler}},
        {"singleEntry", {before.refusedSingleEntry, after.refusedSingleEntry}},
        {"nonNumeric", {before.refusedNonNumeric, after.refusedNonNumeric}},
        {"tooFew", {before.refusedTooFew, after.refusedTooFew}},
        {"growth", {before.refusedGrowth, after.refusedGrowth}},
        {"placement", {before.refusedPlacement, after.refusedPlacement}},
        {"entrySplit", {before.refusedEntrySplit, after.refusedEntrySplit}},
        {"repaired", {before.repairedNames, after.repairedNames}},
        {"concatSplit", {before.refusedConcatSplit, after.refusedConcatSplit}},
        {"ssa", {before.refusedSsa, after.refusedSsa}},
        {"machine", {before.refusedMachine, after.refusedMachine}},
        {"copyPred", {before.refusedCopyPred, after.refusedCopyPred}},
        {"cold", {before.refusedCold, after.refusedCold}},
        {"compares", {before.comparesPromoted, after.comparesPromoted}},
        {"slowCold", {before.slowCopiesCold, after.slowCopiesCold}},
    };
    std::string out;
    for (const auto& [label, counts] : fields) {
        if (counts.second == counts.first) continue;
        if (!out.empty()) out += ' ';
        out += label;
        out += '=';
        out += std::to_string(counts.second - counts.first);
    }
    return out;
}

}  // namespace

void guardRegionStatsReport(const GuardRegionStats& s, const char* label) {
    const char* env = std::getenv("BRONZE_GUARDED_REGION_STATS");
    if (env == nullptr || std::strcmp(env, "1") != 0) return;
    const uint32_t refused = s.refusedHandler + s.refusedSingleEntry + s.refusedNonNumeric +
                             s.refusedTooFew + s.refusedGrowth + s.refusedPlacement +
                             s.refusedEntrySplit + s.refusedConcatSplit + s.refusedSsa +
                             s.refusedMachine + s.refusedCopyPred + s.refusedCold;
    std::fprintf(stderr,
                 "[%s] fns=%u regions=%u entry=%u dup=%u guards=%u points=%u runGuards=%u "
                 "numberClaims=%u numberSkipped=%u elidedBox=%u "
                 "elidedPin=%u unboxFolded=%u promoted=%u shapeTests=%u shapeGuarded=%u icExits=%u icExitsDropped=%u "
                 "blocks=+%u pruned=%u "
                 "compares=%u slowCold=%u refused=%u(handler %u, singleEntry %u, nonNumeric %u, tooFew %u, growth %u, "
                 "placement %u, entrySplit %u, concatSplit %u, ssa %u, machine %u, copyPred %u, cold %u)\n",
                 label, s.functions, s.regions, s.entryRegions, s.duplicated, s.guards, s.guardPoints,
                 s.runGuards, s.numberClaims, s.numberSkipped, s.elidedBox, s.elidedPin, s.unboxFolded, s.promoted,
                 s.shapeTests, s.shapeGuarded, s.icExits, s.icExitsDropped, s.blocksAdded, s.blocksPruned, s.comparesPromoted, s.slowCopiesCold, refused, s.refusedHandler, s.refusedSingleEntry,
                 s.refusedNonNumeric, s.refusedTooFew, s.refusedGrowth, s.refusedPlacement,
                 s.refusedEntrySplit, s.refusedConcatSplit, s.refusedSsa, s.refusedMachine,
                 s.refusedCopyPred, s.refusedCold);
}

il::Function withPreheader(const il::Function& fn) {
    il::Function out = fn;
    for (auto& block : out.blocks) {
        block.id += 1;
        if (block.handler != il::kNoBlock) block.handler += 1;
        for (auto& inst : block.instructions) {
            for (il::BlockTarget* t : {&inst.target, &inst.elseTarget}) {
                if (t->block != il::kNoBlock) t->block += 1;
            }
        }
    }
    il::Block pre;
    pre.id = 0;
    il::Instruction jump;
    jump.op = il::Op::Jump;
    jump.type = il::Type::Void;
    jump.target.block = 1;
    pre.instructions.push_back(std::move(jump));
    out.blocks.insert(out.blocks.begin(), std::move(pre));
    return out;
}

namespace {

// One function's whole examination: its loop regions until the growth budget or
// the attempt limit stops it, and the entry region when none of them was taken.
// A function of its own so that the module loop above it can bracket it — the
// per-function trace is the difference between the counters before and after
// this call, which a body full of `continue` could not report.
// Whether the loop is the one typed_loop_version.cpp kept beside its typed
// copies: entered from the last of their entry tests, on the test's failing
// edge, so it runs only for a receiver no copy takes. Read off the blocks and
// not off that pass's block list, which the inliner in between renumbers.
bool fallbackOfTypedCopies(const il::Function& fn, const RegionPlan& plan) {
    if (plan.entryPred >= fn.blocks.size()) return false;
    const auto& insts = fn.blocks[plan.entryPred].instructions;
    if (insts.size() < 2) return false;
    const il::Instruction& test = insts[insts.size() - 2];
    const il::Instruction& br = insts.back();
    if (test.op != il::Op::IsArray && test.op != il::Op::IsTypedArray && test.op != il::Op::IsNumber) return false;
    return br.op == il::Op::Branch && br.operands.size() == 1 && br.operands[0] == test.result &&
           br.elseTarget.block == plan.header && br.target.block != plan.header;
}

bool examineFunction(il::Function& fn, const il::Module& module,
                     GuardRegionStats& stats, const RegionFeedback* feedback, bool afterInlining,
                     const std::unordered_set<uint32_t>* missedClaims) {
    const std::vector<std::string>& keys = module.keyConstants;
    size_t originalInsts = 0;
    for (const auto& block : fn.blocks) originalInsts += block.instructions.size();
    size_t added = 0;
    // The profile-guided run comes after the static one and after inlining,
    // which copies bodies with their regions: a block that is already one copy
    // of a region is the earlier rewrite's output, never a new opportunity.
    std::vector<uint8_t> alreadyBuilt(fn.blocks.size(), 0);
    bool anyCopy = false;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        if (fn.blocks[b].copyClass == il::CopyClass::Shared) continue;
        alreadyBuilt[b] = 1;
        anyCopy = true;
    }
    bool changed = false;
    if (feedback != nullptr && anyCopy) {
        const uint32_t marked = markConfirmedSlowCopiesCold(fn, *feedback);
        stats.slowCopiesCold += marked;
        changed = marked != 0;
    }

    for (int attempt = 0; attempt < kMaxRegionsPerFunction; ++attempt) {
        // Counted on the FIRST look at a function only. Every later look
        // re-derives the same answers for the regions it did not take, and
        // counting those again would report one loop as several.
        GuardRegionStats discard;
        std::vector<RegionPlan> plans =
            selectRegions(fn, keys, module.slotReprObjectFields, attempt == 0 ? stats : discard, alreadyBuilt, feedback, afterInlining,
                          /*icExits=*/true, missedClaims);
        if (plans.empty()) break;
        // The budget below is one for the function. Taken in block order it
        // went to a versioned loop's original, which comes first, and the
        // typed copy that runs in its place was left without a region.
        std::stable_partition(plans.begin(), plans.end(),
                              [&](const RegionPlan& p) { return !fallbackOfTypedCopies(fn, p); });

        RegionPlan plan = std::move(plans.front());
        size_t regionInsts = 0;
        for (il::BlockId b : plan.blocks) regionInsts += fn.blocks[b].instructions.size();
        // The per-FUNCTION budget: the copies may not add more than was
        // there, so no function can be more than doubled however many loops
        // it has — with the small-function floor above under it. The bound is
        // on the copied instructions; the guards and trampolines each copy
        // brings are counted into `added` afterwards, against the next copy.
        // Counting them here refused every function whose loop is nearly all
        // of it (a per-frame `step(t)` over its instances): such a copy plus
        // its guards is always a little over the function's size.
        const size_t estimate = regionInsts + 4 * plan.guardCount + 2 * plan.blocks.size();
        if (added + regionInsts > std::max(originalInsts, kGrowthFloor)) {
            if (attempt == 0) ++stats.refusedGrowth;
            break;
        }

        const size_t blocksBefore = fn.blocks.size();
        if (!buildGuardedRegion(fn, plan, stats, alreadyBuilt)) {
            // A rewrite refused with IC exits in it is tried again as the
            // region is without them, when that region is still worth taking.
            if (plan.icExitCount == 0) break;
            const il::BlockId header = plan.header;
            plans = selectRegions(fn, keys, module.slotReprObjectFields, discard, alreadyBuilt, feedback, afterInlining, /*icExits=*/false,
                                  missedClaims);
            const auto again = std::find_if(plans.begin(), plans.end(),
                                            [&](const RegionPlan& p) { return p.header == header; });
            if (again == plans.end() || !buildGuardedRegion(fn, *again, stats, alreadyBuilt)) break;
            ++stats.icExitsDropped;
        }
        added += estimate;
        if (fn.blocks.size() > blocksBefore) {
            stats.blocksAdded += static_cast<uint32_t>(fn.blocks.size() - blocksBefore);
        }
        changed = true;
    }

    if (changed) return true;
    // A whole-function copy of a function holding copies would nest one
    // region's pair inside another's; not attempted.
    if (anyCopy) return false;
    // The entry region, on a copy with a preheader in front of it. There is no
    // growth budget to meet here and there cannot be: a whole-function copy
    // exceeds "adds no more than the function had" by construction. The region
    // caps in `analyzeRegion` are the size bound instead.
    il::Function prepped = withPreheader(fn);
    RegionPlan plan;
    if (!selectEntryRegion(prepped, keys, stats, plan, feedback)) return false;
    std::vector<uint8_t> entryBuilt(prepped.blocks.size(), 0);
    const size_t blocksBefore = prepped.blocks.size();
    if (!buildGuardedRegion(prepped, plan, stats, entryBuilt)) {
        // As for a loop region: once more without the IC exits.
        GuardRegionStats discard;
        if (plan.icExitCount == 0 ||
            !selectEntryRegion(prepped, keys, discard, plan, feedback, /*icExits=*/false) ||
            !buildGuardedRegion(prepped, plan, stats, entryBuilt)) {
            return false;
        }
        ++stats.icExitsDropped;
    }
    if (prepped.blocks.size() > blocksBefore) {
        stats.blocksAdded += static_cast<uint32_t>(prepped.blocks.size() - blocksBefore);
    }
    fn = std::move(prepped);
    return true;
}

}  // namespace

bool applyGuardedRegions(il::Module& module, GuardRegionStats* statsOut, const RegionFeedback* feedback,
                         const std::vector<uint32_t>* only, const std::unordered_set<uint32_t>* missedClaims) {
    GuardRegionStats local;
    GuardRegionStats& stats = statsOut != nullptr ? *statsOut : local;
    // A `--census` build is an INSTRUMENT: what it measures is where lowering
    // ran out of static answers, and a pass that changes which values reach an
    // operator changes the readings. It is off there for the same reason a
    // census run is counts and never times.
    if (!module.censusOutPath.empty()) return false;

    std::vector<uint8_t> selected;
    if (only != nullptr) {
        selected.assign(module.functions.size(), 0);
        for (uint32_t f : *only) {
            if (f < selected.size()) selected[f] = 1;
        }
    }
    bool changed = false;
    for (size_t f = 0; f < module.functions.size(); ++f) {
        if (only != nullptr && selected[f] == 0) continue;
        il::Function& fn = module.functions[f];
        // An inlined body's loop exits through forwarding blocks
        // (guard_region_cfg.h); threaded, its exit edges carry its values.
        if (only != nullptr && threadForwardingBlocks(fn) != 0) changed = true;
        ++stats.functions;
        const GuardRegionStats before = stats;
        if (examineFunction(fn, module, stats, feedback, only != nullptr, missedClaims)) changed = true;
        if (!guardRegionTraceOn()) continue;
        const std::string delta = traceDelta(before, stats);
        if (!delta.empty()) {
            std::fprintf(stderr, "[%s] %s %s\n", feedback != nullptr ? "guard-fb-fn" : "guard-fn", fn.name.c_str(),
                         delta.c_str());
        }
    }
    return changed;
}

}  // namespace bronze::lower
