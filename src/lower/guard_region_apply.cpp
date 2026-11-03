// GUARDED REGIONS: the module driver (guard_region.h). Each function's loop
// regions under the growth budget, then its entry region, and the per-function
// trace and the aggregate report.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "lower/guard_region.h"

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

void guardRegionStatsReport(const GuardRegionStats& s) {
    const char* env = std::getenv("BRONZE_GUARDED_REGION_STATS");
    if (env == nullptr || std::strcmp(env, "1") != 0) return;
    const uint32_t refused = s.refusedHandler + s.refusedSingleEntry + s.refusedNonNumeric +
                             s.refusedTooFew + s.refusedGrowth + s.refusedPlacement +
                             s.refusedEntrySplit + s.refusedConcatSplit + s.refusedSsa +
                             s.refusedMachine + s.refusedCopyPred;
    std::fprintf(stderr,
                 "[guard] fns=%u regions=%u entry=%u dup=%u guards=%u points=%u runGuards=%u "
                 "numberClaims=%u numberSkipped=%u elidedBox=%u "
                 "elidedPin=%u unboxFolded=%u promoted=%u shapeTests=%u shapeGuarded=%u blocks=+%u pruned=%u "
                 "refused=%u(handler %u, singleEntry %u, nonNumeric %u, tooFew %u, growth %u, "
                 "placement %u, entrySplit %u, concatSplit %u, ssa %u, machine %u, copyPred %u)\n",
                 s.functions, s.regions, s.entryRegions, s.duplicated, s.guards, s.guardPoints,
                 s.runGuards, s.numberClaims, s.numberSkipped, s.elidedBox, s.elidedPin, s.unboxFolded, s.promoted,
                 s.shapeTests, s.shapeGuarded, s.blocksAdded, s.blocksPruned, refused, s.refusedHandler, s.refusedSingleEntry,
                 s.refusedNonNumeric, s.refusedTooFew, s.refusedGrowth, s.refusedPlacement,
                 s.refusedEntrySplit, s.refusedConcatSplit, s.refusedSsa, s.refusedMachine,
                 s.refusedCopyPred);
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
bool examineFunction(il::Function& fn, const std::vector<std::string>& keys,
                     GuardRegionStats& stats) {
    size_t originalInsts = 0;
    for (const auto& block : fn.blocks) originalInsts += block.instructions.size();
    size_t added = 0;
    std::vector<uint8_t> alreadyBuilt(fn.blocks.size(), 0);
    bool changed = false;

    for (int attempt = 0; attempt < kMaxRegionsPerFunction; ++attempt) {
        // Counted on the FIRST look at a function only. Every later look
        // re-derives the same answers for the regions it did not take, and
        // counting those again would report one loop as several.
        GuardRegionStats discard;
        std::vector<RegionPlan> plans =
            selectRegions(fn, keys, attempt == 0 ? stats : discard, alreadyBuilt);
        if (plans.empty()) break;

        const RegionPlan& plan = plans.front();
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
        if (!buildGuardedRegion(fn, plan, stats, alreadyBuilt)) break;
        added += estimate;
        if (fn.blocks.size() > blocksBefore) {
            stats.blocksAdded += static_cast<uint32_t>(fn.blocks.size() - blocksBefore);
        }
        changed = true;
    }

    if (changed) return true;
    // The entry region, on a copy with a preheader in front of it. There is no
    // growth budget to meet here and there cannot be: a whole-function copy
    // exceeds "adds no more than the function had" by construction. The region
    // caps in `analyzeRegion` are the size bound instead.
    il::Function prepped = withPreheader(fn);
    RegionPlan plan;
    if (!selectEntryRegion(prepped, keys, stats, plan)) return false;
    std::vector<uint8_t> entryBuilt(prepped.blocks.size(), 0);
    const size_t blocksBefore = prepped.blocks.size();
    if (!buildGuardedRegion(prepped, plan, stats, entryBuilt)) return false;
    if (prepped.blocks.size() > blocksBefore) {
        stats.blocksAdded += static_cast<uint32_t>(prepped.blocks.size() - blocksBefore);
    }
    fn = std::move(prepped);
    return true;
}

}  // namespace

bool applyGuardedRegions(il::Module& module, GuardRegionStats* statsOut) {
    GuardRegionStats local;
    GuardRegionStats& stats = statsOut != nullptr ? *statsOut : local;
    // A `--census` build is an INSTRUMENT: what it measures is where lowering
    // ran out of static answers, and a pass that changes which values reach an
    // operator changes the readings. It is off there for the same reason a
    // census run is counts and never times.
    if (!module.censusOutPath.empty()) return false;

    bool changed = false;
    for (il::Function& fn : module.functions) {
        ++stats.functions;
        const GuardRegionStats before = stats;
        if (examineFunction(fn, module.keyConstants, stats)) changed = true;
        if (!guardRegionTraceOn()) continue;
        const std::string delta = traceDelta(before, stats);
        if (!delta.empty()) {
            std::fprintf(stderr, "[guard-fn] %s %s\n", fn.name.c_str(), delta.c_str());
        }
    }
    return changed;
}

}  // namespace bronze::lower
