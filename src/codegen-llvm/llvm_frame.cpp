#include "codegen-llvm/llvm_frame.h"


#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bronze::codegen_llvm {

// ---- GC root frame ---------------------------------------------
//
// Every Dynamic-typed value gets a slot in one contiguous array the collector
// walks: defs store into it, uses load out of it. The load is the point — a
// collection inside any helper call moves the object and updates the slot,
// while an SSA register would keep pointing into dead from-space. A function
// with no Dynamic values (proven-f64 code) gets no frame and pays nothing.
//
// Slots are REUSED once the value in them is dead, which is what keeps the
// frame proportional to how many values are live at once rather than to how
// many the function ever computes. Without it a 2000-statement function got
// 6002 slots — a 48 KB alloca, 6002 unrolled initialising stores, and 6002
// stack locations for the register allocator to colour — and that, not anything
// bronze does, was 93% of a three.js compile.
//
// A slot may be reused only where nothing can read the old value again, so
// the eligibility rule is deliberately narrow:
//
//  - A value used OUTSIDE its defining block keeps a slot to itself. So does
//    a block parameter and a function parameter. Deciding those needs real
//    liveness over the CFG — a loop header's parameter is live across the
//    back edge — and a wrong answer here is a use-after-move that only shows
//    up under GC stress, which is the most expensive bug this project has.
//  - Everything else is a temporary whose whole life is inside one block, and
//    a linear scan over that block is exact: the range is [def, last use] in
//    textual order, because within a block a def precedes every use of it.
//
// A freed slot is not cleared. It holds a dead-but-valid Value until the next
// def overwrites it, so a collection in between forwards one object that is
// no longer reachable from the program — the same one cycle of float the
// frame already had, not a new hazard.

FramePlan planFrame(const il::Function& func, bool moduleHasNewTarget) {
    FramePlan plan;
    plan.slotOf.assign(func.valueCount, kNoFrameSlot);
    const uint32_t n = func.valueCount;
    constexpr uint32_t kNoBlockIdx = UINT32_MAX;

    auto isRooted = [&](il::ValueId id, il::Type ty) {
        return id != il::kNoValue && id < n && ty == il::Type::Dynamic;
    };

    // Where each rooted value is defined, and whether any use is somewhere
    // its defining block's linear scan cannot see.
    std::vector<uint32_t> defBlock(n, kNoBlockIdx);
    std::vector<uint32_t> lastUse(n, 0);
    std::vector<bool> pinned(n, false);

    for (size_t p = 0; p < func.params.size(); ++p) {
        const auto id = static_cast<il::ValueId>(p);
        if (isRooted(id, func.params[p].type)) pinned[id] = true;
    }

    uint32_t maxArgc = 0;
    bool hasConstruct = false;
    for (uint32_t b = 0; b < func.blocks.size(); ++b) {
        const auto& block = func.blocks[b];
        for (const auto& param : block.params) {
            if (isRooted(param.id, param.type)) pinned[param.id] = true;
        }
        for (uint32_t i = 0; i < block.instructions.size(); ++i) {
            const auto& inst = block.instructions[i];
            if (isRooted(inst.result, inst.type)) {
                defBlock[inst.result] = b;
                lastUse[inst.result] = i;
            }
            // Every field on an instruction that names a value: `operands`,
            // and the two block-argument lists a terminator carries. The
            // argument lists are read HERE, at the branch, so they are
            // ordinary uses at index `i` rather than something wider — but
            // they are read out of `inst.target`, not `inst.operands`, and a
            // scan that forgot them would pool a value the branch still needs
            // and hand its slot to the next def.
            auto noteUse = [&](il::ValueId use) {
                if (use == il::kNoValue || use >= n) return;
                if (defBlock[use] != b) {
                    // Defined in another block, or not yet seen here at all
                    // (a value this block only reads). Either way its life is
                    // wider than this scan.
                    pinned[use] = true;
                } else {
                    lastUse[use] = i;
                }
            };
            for (il::ValueId use : inst.operands) noteUse(use);
            for (il::ValueId use : inst.target.args) noteUse(use);
            for (il::ValueId use : inst.elseTarget.args) noteUse(use);
            if (inst.op == il::Op::DynamicCall && inst.operands.size() >= 2) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size() - 2));
            }
            if (inst.op == il::Op::MethodCall && !inst.operands.empty()) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size() - 1));
            }
            if (inst.op == il::Op::SuperCall && inst.operands.size() >= 2) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size() - 2));
            }
            if (inst.op == il::Op::Construct && !inst.operands.empty()) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size() - 1));
                hasConstruct = true;
            }
            // console.log with more than one argument builds an argv too,
            // and console.warn/error take the same path to the other stream.
            if ((inst.op == il::Op::Print || inst.op == il::Op::PrintErr) &&
                inst.operands.size() > 1) {
                maxArgc = std::max(maxArgc, static_cast<uint32_t>(inst.operands.size()));
            }
        }
    }

    // The pinned values first, in the order they appear, so a frame's layout
    // stays a function of the IL and nothing else.
    uint32_t pinnedCount = 0;
    auto pin = [&](il::ValueId id, il::Type ty) {
        if (!isRooted(id, ty) || !pinned[id]) return;
        if (plan.slotOf[id] == kNoFrameSlot) plan.slotOf[id] = pinnedCount++;
    };
    for (size_t p = 0; p < func.params.size(); ++p) {
        pin(static_cast<il::ValueId>(p), func.params[p].type);
    }
    for (const auto& block : func.blocks) {
        for (const auto& param : block.params) pin(param.id, param.type);
        for (const auto& inst : block.instructions) pin(inst.result, inst.type);
    }

    // Then the block-local temporaries, out of a pool every block reuses:
    // two values in different blocks can never both be live, so the pool only
    // has to be as deep as the worst single block.
    uint32_t poolHighWater = 0;
    std::vector<uint32_t> freeSlots;
    std::vector<std::vector<il::ValueId>> expiringAt;
    for (const auto& block : func.blocks) {
        uint32_t poolSize = 0;
        freeSlots.clear();
        expiringAt.assign(block.instructions.size(), {});
        for (uint32_t i = 0; i < block.instructions.size(); ++i) {
            const auto& inst = block.instructions[i];
            // Release what died at the PREVIOUS instruction, never at this
            // one: this instruction's operands are loaded out of their slots
            // before its result is stored, and a slot handed to the result
            // here would be read after it had been overwritten.
            if (i > 0) {
                for (il::ValueId dead : expiringAt[i - 1]) freeSlots.push_back(plan.slotOf[dead]);
                expiringAt[i - 1].clear();
            }
            const il::ValueId res = inst.result;
            if (!isRooted(res, inst.type) || pinned[res]) continue;
            // Absolute from the start — the pool sits immediately above the
            // pinned block, and `pinnedCount` is already final here.
            if (freeSlots.empty()) {
                plan.slotOf[res] = pinnedCount + poolSize++;
            } else {
                plan.slotOf[res] = freeSlots.back();
                freeSlots.pop_back();
            }
            expiringAt[lastUse[res]].push_back(res);
        }
        poolHighWater = std::max(poolHighWater, poolSize);
    }

    // Call arguments live in the frame too: they are live across the callee,
    // and the widest call site's worth of slots is enough because IL is flat
    // SSA — an argument list is built immediately before its call and dead
    // immediately after, never nested.
    plan.argvBase = pinnedCount + poolHighWater;
    plan.ownSlots = plan.argvBase + maxArgc;

    // One slot above the argv region for the inline `new` fast path's fresh
    // instance. Shared by every construct site — a site's use of it ends at
    // its own merge, and IL construct sites never nest mid-flight for the
    // same reason argument lists never do.
    if (hasConstruct && !moduleHasNewTarget) {
        plan.constructSelfSlot = plan.ownSlots++;
    }

    return plan;
}

bool frameMergeDisabled() {
    static const bool off = [] {
        const char* env = std::getenv("BRONZE_NO_FRAME_MERGE");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return off;
}

namespace {

// Every direct edge in `caller` that asks to be inlined, in instruction order.
// These are exactly the two edge kinds that carry `bronze.direct_method`: the
// sibling-closure edge, whose callee is named outright, and the direct
// method-call edge, whose callee is a guarded guess with an out-of-line miss
// path. Both leave the callee's body in the caller when the ask is granted, so
// both are worth a region.
void inlineEdgesOf(const il::Function& caller, size_t functionCount,
                   std::vector<uint32_t>& out) {
    out.clear();
    for (const il::Block& block : caller.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            uint32_t callee = UINT32_MAX;
            if (inst.op == il::Op::Call && inst.callEnvHops != il::Instruction::kNoEnvHops) {
                callee = inst.calleeIndex;
            } else if ((inst.op == il::Op::MethodCall) &&
                       inst.directTarget != il::Instruction::kNoDirectTarget) {
                callee = inst.directTarget;
            }
            if (callee >= functionCount) continue;
            if (std::find(out.begin(), out.end(), callee) == out.end()) out.push_back(callee);
        }
    }
}

}  // namespace

RegionPlan planRegions(const il::Module& module, const std::vector<FramePlan>& plans) {
    const size_t n = module.functions.size();
    RegionPlan regions;
    regions.totalSlots.assign(n, 0);
    regions.isMergeTarget.assign(n, false);
    for (size_t i = 0; i < n; ++i) regions.totalSlots[i] = plans[i].ownSlots;
    if (frameMergeDisabled()) return regions;

    // A depth-first walk in function order, so the answer is a function of the
    // IL and nothing else. `open` marks the functions on the current path: an
    // edge back into one of them would make a region contain itself, and that
    // edge is refused rather than sized.
    enum class Mark : uint8_t { Unvisited, Open, Done };
    std::vector<Mark> mark(n, Mark::Unvisited);
    std::vector<uint32_t> region(n, 0);

    std::vector<uint32_t> edges;
    // Explicit stack: the three.js graph is 3000 functions deep in places the
    // native stack would rather not go.
    struct Frame {
        uint32_t fn;
        size_t next;
        std::vector<uint32_t> edges;
        uint32_t deepest;
    };
    for (uint32_t root = 0; root < n; ++root) {
        if (mark[root] != Mark::Unvisited) continue;
        std::vector<Frame> stack;
        inlineEdgesOf(module.functions[root], n, edges);
        stack.push_back({root, 0, edges, 0});
        mark[root] = Mark::Open;
        while (!stack.empty()) {
            Frame& top = stack.back();
            if (top.next < top.edges.size()) {
                const uint32_t callee = top.edges[top.next++];
                if (mark[callee] == Mark::Open) continue;  // refused: closes a cycle
                if (mark[callee] == Mark::Done) {
                    regions.merged.insert({top.fn, callee});
                    regions.isMergeTarget[callee] = true;
                    top.deepest = std::max(top.deepest, region[callee]);
                    continue;
                }
                inlineEdgesOf(module.functions[callee], n, edges);
                stack.push_back({callee, 0, edges, 0});
                mark[callee] = Mark::Open;
                continue;
            }
            const uint32_t fn = top.fn;
            const uint32_t deepest = top.deepest;
            region[fn] = plans[fn].ownSlots + deepest;
            regions.totalSlots[fn] = region[fn];
            mark[fn] = Mark::Done;
            stack.pop_back();
            if (!stack.empty()) {
                // The edge that pushed this frame is only settled now that the
                // callee's own region is known.
                Frame& parent = stack.back();
                regions.merged.insert({parent.fn, fn});
                regions.isMergeTarget[fn] = true;
                parent.deepest = std::max(parent.deepest, region[fn]);
            }
        }
    }
    return regions;
}

}  // namespace bronze::codegen_llvm
