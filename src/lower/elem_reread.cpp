// REPEATED ELEMENT READS (elem_reread.h).

#include "lower/elem_reread.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "il/il_pin.h"
#include "lower/guard_region_cfg.h"
#include "lower/truth_join_threading.h"

namespace bronze::lower {

namespace {

// One element: a receiver, the number its index is, and the read's flags
// (reads that differ in what they have proved are kept apart).
struct Element {
    il::ValueId receiver = il::kNoValue;
    il::ValueId index = il::kNoValue;
    int32_t flags = 0;
    uint32_t reads = 0;
    // A read of it to copy, and the `box.f64` that read's index is (`op` is
    // `Jump` where the index is no box).
    il::Instruction read{il::Op::Jump};
    il::Instruction box{il::Op::Jump};
};

bool arrayRead(const il::Instruction& inst) {
    return inst.op == il::Op::ElemGet && inst.operands.size() == 2 && inst.result != il::kNoValue &&
           (inst.immI32 & il::kElemFlagArrayReceiver) != 0;
}

// A named read whose claim an `is.shape` can ask in its place.
bool claimedRead(const il::Instruction& inst) {
    return inst.op == il::Op::PropGet && inst.operands.size() == 1 && inst.result != il::kNoValue &&
           inst.staticSlot != il::Instruction::kNoStaticSlot && !inst.shapeGuarded &&
           inst.directTarget == il::Instruction::kNoDirectTarget;
}

// Whether the instruction can neither store an Array's element nor run code.
// A flagged read of another element is a load (elem_reread.h says what a hole
// costs), a shape-guarded site a load or the store of a Number into a named
// slot, and the rest is what cannot collect: nothing that allocates nothing
// and calls nothing stores an element, but for the claim that rewrites an
// Int32 box in place and a typed store, which are left out by name.
bool leavesElements(const il::Instruction& inst) {
    if (arrayRead(inst)) return true;
    if ((inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) && inst.shapeGuarded) {
        return il::layoutFactOperand(inst) == il::kNoValue;
    }
    if (inst.op == il::Op::IsNumberArray || inst.op == il::Op::ElemSetTyped) return false;
    return !il::canCollect(inst);
}

struct Rewriter {
    il::Function fn;  // the copy being rewritten
    Cfg cfg;          // of the function as it was
    std::vector<Element> elements;
    std::unordered_map<il::ValueId, il::Instruction> boxOf;  // `box.f64` by result
    std::vector<uint8_t> reset;                    // by block: starts with nothing
    std::vector<std::vector<uint8_t>> wantedOut;   // by block, by element
    std::vector<std::vector<il::ValueId>> out;     // by block, by element: its name at the end
    std::vector<il::BlockId> tail;                 // by block: the block now holding its terminator
    std::unordered_map<il::ValueId, il::ValueId> rename;
    bool changed = false;

    explicit Rewriter(const il::Function& f) : fn(f), cfg(buildCfg(f)) {}

    il::ValueId indexOf(const il::Instruction& read) const {
        const auto it = boxOf.find(read.operands[1]);
        return it == boxOf.end() ? read.operands[1] : it->second.operands[0];
    }

    int elementOf(const il::Instruction& inst) const {
        if (!arrayRead(inst)) return -1;
        const il::ValueId index = indexOf(inst);
        for (size_t e = 0; e < elements.size(); ++e) {
            if (elements[e].receiver == inst.operands[0] && elements[e].index == index &&
                elements[e].flags == inst.immI32) {
                return static_cast<int>(e);
            }
        }
        return -1;
    }

    bool collect() {
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.op == il::Op::Box && inst.boxType == il::Type::F64 && inst.operands.size() == 1 &&
                    inst.result != il::kNoValue) {
                    boxOf.emplace(inst.result, inst);
                }
            }
        }
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (!arrayRead(inst)) continue;
                int e = elementOf(inst);
                if (e < 0) {
                    Element el;
                    el.receiver = inst.operands[0];
                    el.index = indexOf(inst);
                    el.flags = inst.immI32;
                    el.read = inst;
                    const auto box = boxOf.find(inst.operands[1]);
                    if (box != boxOf.end()) el.box = box->second;
                    elements.push_back(std::move(el));
                    e = static_cast<int>(elements.size()) - 1;
                }
                ++elements[e].reads;
            }
        }
        elements.erase(std::remove_if(elements.begin(), elements.end(), [](const Element& el) { return el.reads < 2; }),
                       elements.end());
        return !elements.empty();
    }

    // Which elements some path from a block's end reads before a block that
    // starts with nothing: one pass against the forward edges.
    void planWanted() {
        const size_t n = fn.blocks.size();
        reset.assign(n, 0);
        reset[0] = 1;
        for (size_t b = 0; b < n; ++b) {
            if (cfg.rpoIndex[b] == UINT32_MAX) reset[b] = 1;
            const il::BlockId h = fn.blocks[b].handler;
            if (h != il::kNoBlock && h < n) reset[h] = 1;
            for (il::BlockId p : cfg.preds[b]) {
                if (cfg.rpoIndex[p] == UINT32_MAX || cfg.rpoIndex[p] >= cfg.rpoIndex[b]) reset[b] = 1;
            }
        }
        wantedOut.assign(n, std::vector<uint8_t>(elements.size(), 0));
        std::vector<std::vector<uint8_t>> wantedIn = wantedOut;
        for (size_t r = cfg.rpo.size(); r-- > 0;) {
            const il::BlockId b = cfg.rpo[r];
            for (il::BlockId s : cfg.succs[b]) {
                if (reset[s]) continue;
                for (size_t e = 0; e < elements.size(); ++e) wantedOut[b][e] |= wantedIn[s][e];
            }
            wantedIn[b] = wantedOut[b];
            for (const il::Instruction& inst : fn.blocks[b].instructions) {
                const int e = elementOf(inst);
                if (e >= 0) wantedIn[b][e] = 1;
            }
        }
        wantedIn_ = std::move(wantedIn);
    }
    std::vector<std::vector<uint8_t>> wantedIn_;

    il::ValueId newValue() { return static_cast<il::ValueId>(fn.valueCount++); }

    il::BlockId newBlock(il::BlockId like) {
        il::Block block;
        block.id = static_cast<il::BlockId>(fn.blocks.size());
        block.handler = fn.blocks[like].handler;
        block.copyClass = fn.blocks[like].copyClass;
        block.copyRegion = fn.blocks[like].copyRegion;
        block.cold = fn.blocks[like].cold;
        fn.blocks.push_back(std::move(block));
        return fn.blocks.back().id;
    }

    // The element read again, appended to `into`; its new name.
    il::ValueId readAgain(std::vector<il::Instruction>& into, const Element& el) {
        il::Instruction read = el.read;
        if (el.box.op == il::Op::Box) {
            il::Instruction box = el.box;
            box.result = newValue();
            read.operands[1] = box.result;
            into.push_back(std::move(box));
        }
        // Not where the read it copies stood, so not behind that one's proof.
        read.immI32 &= ~il::kElemFlagProvenInBounds;
        read.result = newValue();
        const il::ValueId name = read.result;
        into.push_back(std::move(read));
        return name;
    }

    static il::Instruction jumpTo(il::BlockId block, std::vector<il::ValueId> args, const Span& span) {
        il::Instruction jump{il::Op::Jump};
        jump.type = il::Type::Void;
        jump.target = il::BlockTarget{block, std::move(args)};
        jump.span = span;
        return jump;
    }

    // Whether `cur` reads element `e` at or after instruction `from`.
    bool readsLater(il::BlockId cur, size_t from, size_t e) const {
        const auto& insts = fn.blocks[cur].instructions;
        for (size_t i = from; i < insts.size(); ++i) {
            if (elementOf(insts[i]) == static_cast<int>(e)) return true;
        }
        return false;
    }

    // Whether a static-slot site stands in `cur` at or after `from`. The
    // backend carries each receiver's shape from one such site to the next
    // within a block (il2mir il_shape_memo.cpp), and a split in front of one
    // would make it ask again.
    bool staticSiteLater(il::BlockId cur, size_t from) const {
        const auto& insts = fn.blocks[cur].instructions;
        for (size_t i = from; i < insts.size(); ++i) {
            if ((insts[i].op == il::Op::PropGet || insts[i].op == il::Op::PropSet) &&
                insts[i].staticSlot != il::Instruction::kNoStaticSlot) {
                return true;
            }
        }
        return false;
    }

    void run() {
        const size_t n = fn.blocks.size();
        out.assign(n, std::vector<il::ValueId>(elements.size(), il::kNoValue));
        tail.resize(n);
        for (size_t b = 0; b < n; ++b) tail[b] = static_cast<il::BlockId>(b);
        for (il::BlockId b : cfg.rpo) block(b);
        if (!changed) return;
        for (il::Block& blk : fn.blocks) {
            for (il::Instruction& inst : blk.instructions) {
                const auto fix = [&](il::ValueId& v) {
                    const auto it = rename.find(v);
                    if (it != rename.end()) v = it->second;
                };
                for (il::ValueId& v : inst.operands) fix(v);
                for (il::ValueId& v : inst.target.args) fix(v);
                for (il::ValueId& v : inst.elseTarget.args) fix(v);
            }
        }
    }

    void block(il::BlockId b) {
        std::vector<il::ValueId> state(elements.size(), il::kNoValue);
        if (!reset[b]) {
            for (size_t e = 0; e < elements.size(); ++e) {
                il::ValueId name = il::kNoValue;
                bool all = !cfg.preds[b].empty();
                bool same = true;
                for (il::BlockId p : cfg.preds[b]) {
                    const il::ValueId have = out[p][e];
                    if (have == il::kNoValue) all = false;
                    if (name == il::kNoValue) name = have;
                    same = same && have == name;
                }
                if (!all) continue;
                if (same) {
                    state[e] = name;
                    continue;
                }
                if (!wantedIn_[b][e]) continue;
                // Paths that hold different names: a parameter.
                const il::ValueId param = newValue();
                fn.blocks[b].params.push_back(il::BlockParam{param, il::Type::Dynamic});
                for (il::BlockId p : cfg.preds[b]) {
                    il::Instruction& term = fn.blocks[tail[p]].instructions.back();
                    if (term.target.block == b) term.target.args.push_back(out[p][e]);
                    if (term.op == il::Op::Branch && term.elseTarget.block == b) {
                        term.elseTarget.args.push_back(out[p][e]);
                    }
                }
                state[e] = param;
                changed = true;
            }
        }
        // Elements this block held and lost, not read again since.
        std::vector<uint8_t> lost(elements.size(), 0);
        il::BlockId cur = b;
        size_t i = 0;
        while (i < fn.blocks[cur].instructions.size()) {
            const il::Instruction inst = fn.blocks[cur].instructions[i];
            if (il::isTerminator(inst.op)) {
                std::vector<il::Instruction> again;
                for (size_t e = 0; e < elements.size(); ++e) {
                    if (lost[e] && wantedOut[b][e]) state[e] = readAgain(again, elements[e]);
                }
                if (!again.empty()) {
                    auto& insts = fn.blocks[cur].instructions;
                    insts.insert(insts.begin() + static_cast<ptrdiff_t>(i), again.begin(), again.end());
                    changed = true;
                }
                break;
            }
            const int e = elementOf(inst);
            if (e >= 0) {
                if (state[e] != il::kNoValue) {
                    rename[inst.result] = state[e];
                    auto& insts = fn.blocks[cur].instructions;
                    insts.erase(insts.begin() + static_cast<ptrdiff_t>(i));
                    changed = true;
                    continue;
                }
                state[e] = inst.result;
                lost[e] = 0;
                ++i;
                continue;
            }
            if (leavesElements(inst)) {
                ++i;
                continue;
            }
            if (claimedRead(inst) && !staticSiteLater(cur, i + 1)) {
                std::vector<size_t> kept;
                for (size_t k = 0; k < elements.size(); ++k) {
                    if (state[k] != il::kNoValue && (readsLater(cur, i + 1, k) || wantedOut[b][k])) kept.push_back(k);
                }
                if (!kept.empty()) {
                    cur = split(cur, i, kept, state);
                    tail[b] = cur;
                    i = 0;
                    changed = true;
                    continue;
                }
            }
            // A claimed read left as it is costs less than the read its
            // block's end would make for it; anything else is a call.
            const bool cheap = claimedRead(inst);
            for (size_t k = 0; k < elements.size(); ++k) {
                if (state[k] == il::kNoValue) continue;
                state[k] = il::kNoValue;
                lost[k] = cheap ? 0 : 1;
            }
            ++i;
        }
        out[b] = std::move(state);
    }

    // The claimed read at (cur, i) as a test, its hit and its miss
    // (elem_reread.h); `kept` are read again on the miss. The join's id.
    il::BlockId split(il::BlockId cur, size_t i, const std::vector<size_t>& kept, std::vector<il::ValueId>& state) {
        const il::Instruction site = fn.blocks[cur].instructions[i];
        const il::BlockId hit = newBlock(cur);
        const il::BlockId miss = newBlock(cur);
        const il::BlockId join = newBlock(cur);
        fn.blocks[miss].cold = true;

        il::Instruction test{il::Op::IsShape};
        test.type = il::Type::Bool;
        test.result = newValue();
        test.operands = {site.operands[0]};
        test.staticSlot = site.staticSlot;
        test.familyLo = site.familyLo;
        test.familySpan = site.familySpan;
        // A family claim has no cell; a cell claim is the miss's own.
        test.staticCellIndex = site.familyLo == il::Instruction::kNoFamily ? site.staticCellIndex : 0;
        test.span = site.span;
        il::Instruction br{il::Op::Branch};
        br.type = il::Type::Void;
        br.operands = {test.result};
        br.target = il::BlockTarget{hit, {}};
        br.elseTarget = il::BlockTarget{miss, {}};
        br.span = site.span;

        il::Instruction load = site;
        load.result = newValue();
        load.shapeGuarded = true;
        std::vector<il::ValueId> hitArgs{load.result};
        for (size_t k : kept) hitArgs.push_back(state[k]);
        fn.blocks[hit].instructions.push_back(std::move(load));
        fn.blocks[hit].instructions.push_back(jumpTo(join, std::move(hitArgs), site.span));

        il::Instruction ordinary = site;
        ordinary.result = newValue();
        std::vector<il::ValueId> missArgs{ordinary.result};
        fn.blocks[miss].instructions.push_back(std::move(ordinary));
        for (size_t k : kept) missArgs.push_back(readAgain(fn.blocks[miss].instructions, elements[k]));
        fn.blocks[miss].instructions.push_back(jumpTo(join, std::move(missArgs), site.span));

        fn.blocks[join].params.push_back(il::BlockParam{site.result, site.type});
        for (size_t k = 0; k < elements.size(); ++k) {
            if (std::find(kept.begin(), kept.end(), k) == kept.end()) {
                state[k] = il::kNoValue;
                continue;
            }
            state[k] = newValue();
            fn.blocks[join].params.push_back(il::BlockParam{state[k], il::Type::Dynamic});
        }
        auto& insts = fn.blocks[cur].instructions;
        fn.blocks[join].instructions.assign(insts.begin() + static_cast<ptrdiff_t>(i) + 1, insts.end());
        insts.resize(i, il::Instruction{il::Op::Jump});
        insts.push_back(std::move(test));
        insts.push_back(std::move(br));
        return join;
    }
};

}  // namespace

bool eliminateRepeatedElementReads(il::Module& module) {
    bool any = false;
    for (il::Function& fn : module.functions) {
        uint32_t reads = 0;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) reads += arrayRead(inst) ? 1 : 0;
        }
        if (reads < 2) continue;
        Rewriter rw(fn);
        if (!rw.collect()) continue;
        rw.planWanted();
        rw.run();
        if (!rw.changed) continue;
        if (!definitionsDominate(rw.fn)) continue;
        fn = std::move(rw.fn);
        any = true;
    }
    return any;
}

}  // namespace bronze::lower
