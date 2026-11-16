#include "il/verifier.h"
#include "il/verifier_proofs.h"
#include "abi/bronze_abi.h"
#include "il/feedback_site.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bronze::il {

// The `+`-chain accumulator's non-observability rule (il.h, `Op::ConcatBegin`).
//
// A `concat.begin` or `concat.append` result is a BUILDER: an ordinary string
// carrying room past its text, which `bronze_concat_append` writes into in
// place. That is sound only while exactly ONE instruction holds it and that
// instruction is the next link of the same spine — a second holder would watch
// text appear under it, and any other consumer would be handed a string whose
// allocation is larger than its contents without knowing it. The lowerer builds
// the spine that way by construction, out of one AST node with one operand
// list; this is what says so rather than trusting it, and it is the reason the
// ops can be as cheap as they are.
//
// Naming a builder in a terminator's argument list counts as a use here, which
// is what rejects one threaded through a block parameter: past that edge the
// value has a name the spine did not give it, and nothing local can say who
// else holds it.
// A `to.string` carrying kToStrKeepNumber answers a String OR a Number, which
// only `concat.n` is written to take: every other instruction that reads it
// would be handed a Number where it was promised a String. A resume state (a
// generic function's own concat.n picks the value up again) and a block
// argument pass it along without reading it. A `concat.n` joins two pieces
// or more.
static bool verifyTemplatePieces(const Function& fn, DiagnosticSink& diags) {
    std::unordered_set<ValueId> keep;
    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            if (inst.op == Op::ToStr && (inst.immI32 & kToStrKeepNumber) != 0) keep.insert(inst.result);
            if (inst.op == Op::ConcatN && inst.operands.size() < 2) {
                diags.error(Span{}, "Function " + fn.name + ": concat.n takes two pieces or more");
                return false;
            }
        }
    }
    if (keep.empty()) return true;
    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            if (inst.op == Op::ConcatN || inst.op == Op::ResumePoint || inst.op == Op::Leave ||
                inst.op == Op::ResumeCall) {
                continue;
            }
            for (ValueId id : inst.operands) {
                if (keep.count(id) != 0) {
                    diags.error(Span{}, "Function " + fn.name + ": %" + std::to_string(id) +
                                            ", a to.string that lets a Number through, is read by " +
                                            opName(inst.op) + "; only concat.n may read it");
                    return false;
                }
            }
        }
    }
    return true;
}

static bool verifyConcatChain(const Function& fn, DiagnosticSink& diags) {
    auto fail = [&](const std::string& msg) {
        diags.error(Span{}, "Function " + fn.name + ": " + msg);
        return false;
    };

    struct Accumulator {
        size_t blockIdx = 0;
        size_t instIdx = 0;
        unsigned mentions = 0;
        unsigned links = 0;
    };
    std::unordered_map<ValueId, Accumulator> accs;
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto& insts = fn.blocks[b].instructions;
        for (size_t i = 0; i < insts.size(); ++i) {
            if (insts[i].op == Op::ConcatBegin || insts[i].op == Op::ConcatAppend) {
                accs[insts[i].result] = Accumulator{b, i, 0, 0};
            }
        }
    }
    if (accs.empty()) return true;

    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            for (ValueId id : inst.operands) {
                if (auto it = accs.find(id); it != accs.end()) ++it->second.mentions;
            }
        }
    }

    // Ordered walk, never an iteration over the map: a diagnostic is an output
    // path, and a hash order in one is a compiler that stops being a function
    // of its input.
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto& insts = fn.blocks[b].instructions;
        for (size_t i = 0; i < insts.size(); ++i) {
            const Instruction& inst = insts[i];
            if (inst.op != Op::ConcatAppend && inst.op != Op::ConcatEnd) continue;
            if (inst.operands.empty()) {
                return fail(std::string(opName(inst.op)) + " has no accumulator operand");
            }
            const ValueId acc = inst.operands[0];
            auto it = accs.find(acc);
            if (it == accs.end()) {
                return fail(std::string(opName(inst.op)) + " reads %" + std::to_string(acc) +
                            ", which no concat.begin or concat.append defines");
            }
            if (it->second.blockIdx != b || it->second.instIdx >= i) {
                return fail(std::string(opName(inst.op)) + " reads accumulator %" +
                            std::to_string(acc) + ", which is not defined earlier in the same "
                            "block");
            }
            ++it->second.links;
        }
    }

    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            if (inst.op != Op::ConcatBegin && inst.op != Op::ConcatAppend) continue;
            const Accumulator& acc = accs.at(inst.result);
            if (acc.mentions != 1 || acc.links != 1) {
                return fail("concat accumulator %" + std::to_string(inst.result) + " is named " +
                            std::to_string(acc.mentions) + " time(s) and consumed by a chain link " +
                            std::to_string(acc.links) +
                            " time(s); a builder must be used exactly once, by the next link");
            }
        }
    }
    return true;
}

// The fixed signatures of the speculation forms (il_op.h): operand types,
// result type, and whether `immI32` must be non-negative. `resume.point` and
// `leave` take any state and are checked on their own. Returns nullptr for
// every other op.
struct SpecSignature {
    uint8_t count;
    Type operands[3];
    Type result;
    bool nonNegativeImm;
};

static const SpecSignature* specSignature(Op op) {
    static const SpecSignature kLoadShape{1, {Type::Dynamic}, Type::Ptr, false};
    static const SpecSignature kShapeCell{0, {}, Type::Ptr, true};
    static const SpecSignature kPtrEq{2, {Type::Ptr, Type::Ptr}, Type::Bool, false};
    static const SpecSignature kShapeHasOwn{1, {Type::Ptr}, Type::Bool, true};
    static const SpecSignature kDynTest{1, {Type::Dynamic}, Type::Bool, false};
    static const SpecSignature kSlotLoad{1, {Type::Dynamic}, Type::Dynamic, true};
    static const SpecSignature kSlotStore{2, {Type::Dynamic, Type::Dynamic}, Type::Void, true};
    static const SpecSignature kUnboxI32{1, {Type::Dynamic}, Type::I32, false};
    static const SpecSignature kUnboxF64{1, {Type::Dynamic}, Type::F64, false};
    static const SpecSignature kI32ToF64{1, {Type::I32}, Type::F64, false};
    static const SpecSignature kI32Test{2, {Type::I32, Type::I32}, Type::Bool, false};
    static const SpecSignature kFnEnv{1, {Type::Dynamic}, Type::Dynamic, false};
    static const SpecSignature kArrayLength{1, {Type::Dynamic}, Type::I32, false};
    static const SpecSignature kElemLoad{2, {Type::Dynamic, Type::I32}, Type::Dynamic, false};
    static const SpecSignature kElemStore{3, {Type::Dynamic, Type::I32, Type::Dynamic}, Type::Void, false};
    static const SpecSignature kInstanceShape{1, {Type::Dynamic}, Type::Ptr, false};
    static const SpecSignature kCreateThis{2, {Type::Dynamic, Type::Ptr}, Type::Dynamic, false};
    static const SpecSignature kShapeStore{2, {Type::Dynamic, Type::Ptr}, Type::Void, false};
    static const SpecSignature kProtoCell{0, {}, Type::Bool, false};
    static const SpecSignature kProtoPublish{0, {}, Type::Void, false};
    static const SpecSignature kMemberCell{0, {}, Type::Ptr, false};
    static const SpecSignature kMemberPublish{1, {Type::Dynamic}, Type::Void, false};
    static const SpecSignature kConstructResult{2, {Type::Dynamic, Type::Dynamic}, Type::Dynamic, false};
    switch (op) {
        case Op::FnConstructs: return &kDynTest;
        case Op::FnInstanceShape: return &kInstanceShape;
        case Op::CreateThis: return &kCreateThis;
        case Op::ShapeStore: return &kShapeStore;
        case Op::ProtoCell: return &kProtoCell;
        case Op::ProtoPublish: return &kProtoPublish;
        case Op::MemberCell: return &kMemberCell;
        case Op::MemberPublish: return &kMemberPublish;
        case Op::ConstructResult:
        case Op::ArrayPush: return &kConstructResult;
        case Op::LoadShape: return &kLoadShape;
        case Op::ShapeCell: return &kShapeCell;
        case Op::PtrEq: return &kPtrEq;
        case Op::ShapeHasOwn: return &kShapeHasOwn;
        case Op::IsObject:
        case Op::TypeOfIs:
        case Op::IsI32:
        case Op::IsHole:
        case Op::IsUninit:
        case Op::FnCodeIs:
        case Op::IsArray:
        case Op::FnNativeIs:
            return &kDynTest;
        case Op::TypedLength: return &kArrayLength;
        case Op::SlotLoad: return &kSlotLoad;
        case Op::SlotStore: return &kSlotStore;
        case Op::UnboxI32: return &kUnboxI32;
        case Op::UnboxF64: return &kUnboxF64;
        case Op::I32ToF64: return &kI32ToF64;
        case Op::AddWouldOvf:
        case Op::SubWouldOvf:
        case Op::MulWouldOvf:
        case Op::InBounds:
            return &kI32Test;
        case Op::FnEnv: return &kFnEnv;
        case Op::ArrayLength: return &kArrayLength;
        case Op::ElemLoad: return &kElemLoad;
        case Op::ElemStore: return &kElemStore;
        default: return nullptr;
    }
}

bool verifyFunction(const Function& fn, DiagnosticSink& diags) {
    if (fn.blocks.empty()) {
        diags.error(Span{}, "Function " + fn.name + " has no blocks");
        return false;
    }

    if (fn.blocks[0].id != 0) {
        diags.error(Span{}, "Function " + fn.name + " entry block id is not 0");
        return false;
    }

    if (!fn.blocks[0].params.empty()) {
        diags.error(Span{}, "Function " + fn.name + " entry block b0 must not have block parameters");
        return false;
    }

    // Map of ValueId -> defined Type
    std::unordered_map<ValueId, Type> definedTypes;
    // Map of ValueId -> {block_index, instruction_index_in_block (or -1 if param)}
    struct DefLocation {
        size_t blockIdx;
        int instIdx;
    };
    std::unordered_map<ValueId, DefLocation> defLocations;

    auto recordDef = [&](ValueId id, Type type, size_t blockIdx, int instIdx) -> bool {
        // `kNoValue` is the "there is no value here" sentinel, never a name.
        // Letting it become a definition is what made every use of it legal:
        // one default-constructed BlockParam put UINT32_MAX into this map,
        // and from then on `jump b1(%4294967295)` type-checked and reached
        // codegen. Rejected here so the sentinel can never be looked up.
        if (id == kNoValue) {
            diags.error(Span{}, "Function " + fn.name +
                                    ": the no-value sentinel %" + std::to_string(kNoValue) +
                                    " is used as a definition");
            return false;
        }
        if (definedTypes.contains(id)) {
            diags.error(Span{}, "Function " + fn.name + ": duplicate definition of %" + std::to_string(id));
            return false;
        }
        definedTypes[id] = type;
        defLocations[id] = DefLocation{blockIdx, instIdx};
        return true;
    };

    // 1. Record function parameters (%0 .. %N-1)
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (!recordDef(static_cast<ValueId>(i), fn.params[i].type, 0, -1)) {
            return false;
        }
    }

    // 2. Record block parameters & instruction results
    for (size_t bIdx = 0; bIdx < fn.blocks.size(); ++bIdx) {
        const auto& block = fn.blocks[bIdx];
        if (block.id != static_cast<BlockId>(bIdx)) {
            diags.error(Span{}, "Function " + fn.name + ": block id mismatch at index " +
                                  std::to_string(bIdx) + " (expected b" + std::to_string(bIdx) +
                                  ", got b" + std::to_string(block.id) + ")");
            return false;
        }

        // A handler is a real edge the backend materializes, and it is entered
        // from the middle of a block — so it can carry no arguments, and there
        // is therefore nowhere for a parameter's incoming value to come from.
        // Both halves are checked here because lowering is the only producer
        // and a violation would surface as an LLVM phi with a missing incoming,
        // which names nothing.
        if (block.handler != kNoBlock) {
            if (block.handler >= fn.blocks.size()) {
                diags.error(Span{}, "Function " + fn.name + ": block b" + std::to_string(bIdx) +
                                        " names handler b" + std::to_string(block.handler) +
                                        ", which is out of range");
                return false;
            }
            if (!fn.blocks[block.handler].params.empty()) {
                diags.error(Span{}, "Function " + fn.name + ": handler b" +
                                        std::to_string(block.handler) +
                                        " has block parameters, which no edge into a handler can "
                                        "supply");
                return false;
            }
        }

        for (size_t pIdx = 0; pIdx < block.params.size(); ++pIdx) {
            const auto& param = block.params[pIdx];
            // A block parameter is an SSA definition, so it needs a real type
            // for the incoming arguments to be checked against. `Void` is the
            // default-constructed state and means the producer never filled it
            // in — every argument would then have to be Void too, which no
            // instruction produces.
            if (param.type == Type::Void) {
                diags.error(Span{}, "Function " + fn.name + ": block b" + std::to_string(bIdx) +
                                        " parameter " + std::to_string(pIdx) + " has type void");
                return false;
            }
            if (!recordDef(param.id, param.type, bIdx, -1)) {
                return false;
            }
        }

        for (size_t iIdx = 0; iIdx < block.instructions.size(); ++iIdx) {
            const auto& inst = block.instructions[iIdx];
            if (inst.result != kNoValue) {
                if (!recordDef(inst.result, inst.type, bIdx, static_cast<int>(iIdx))) {
                    return false;
                }
            }
        }
    }

    // Build CFG and compute dominators for cross-block dominance verification
    const size_t numBlocks = fn.blocks.size();
    std::vector<std::vector<BlockId>> succs(numBlocks);
    std::vector<std::vector<BlockId>> preds(numBlocks);

    auto addEdge = [&](BlockId from, BlockId to) {
        if (to == kNoBlock || to >= numBlocks) return;
        auto& s = succs[from];
        if (std::find(s.begin(), s.end(), to) == s.end()) s.push_back(to);
        auto& p = preds[to];
        if (std::find(p.begin(), p.end(), from) == p.end()) p.push_back(from);
    };

    for (size_t b = 0; b < numBlocks; ++b) {
        const auto& blk = fn.blocks[b];
        if (blk.handler != kNoBlock) {
            addEdge(static_cast<BlockId>(b), blk.handler);
        }
        if (!blk.instructions.empty()) {
            const auto& term = blk.instructions.back();
            if (term.op == Op::Jump) {
                addEdge(static_cast<BlockId>(b), term.target.block);
            } else if (term.op == Op::Branch) {
                addEdge(static_cast<BlockId>(b), term.target.block);
                addEdge(static_cast<BlockId>(b), term.elseTarget.block);
            }
        }
    }

    // Compute reverse post-order (RPO) from block 0
    std::vector<uint8_t> seen(numBlocks, 0);
    std::vector<BlockId> postorder;
    std::vector<std::pair<BlockId, size_t>> dfsStack;
    seen[0] = 1;
    dfsStack.push_back({0, 0});
    while (!dfsStack.empty()) {
        auto& [b, next] = dfsStack.back();
        if (next < succs[b].size()) {
            const BlockId s = succs[b][next++];
            if (!seen[s]) {
                seen[s] = 1;
                dfsStack.push_back({s, 0});
            }
            continue;
        }
        postorder.push_back(b);
        dfsStack.pop_back();
    }

    std::vector<BlockId> rpo(postorder.rbegin(), postorder.rend());
    std::vector<uint32_t> rpoIndex(numBlocks, UINT32_MAX);
    for (uint32_t i = 0; i < rpo.size(); ++i) {
        rpoIndex[rpo[i]] = i;
    }

    // Cooper-Harvey-Kennedy dominator tree computation
    std::vector<BlockId> idom(numBlocks, kNoBlock);
    idom[0] = 0;

    auto intersectIdom = [&](BlockId b1, BlockId b2) -> BlockId {
        while (b1 != b2) {
            while (rpoIndex[b1] > rpoIndex[b2]) {
                b1 = idom[b1];
                if (b1 == kNoBlock) return b2;
            }
            while (rpoIndex[b2] > rpoIndex[b1]) {
                b2 = idom[b2];
                if (b2 == kNoBlock) return b1;
            }
        }
        return b1;
    };

    bool domChanged = true;
    while (domChanged) {
        domChanged = false;
        for (BlockId b : rpo) {
            if (b == 0) continue;
            BlockId newIdom = kNoBlock;
            for (BlockId p : preds[b]) {
                if (rpoIndex[p] == UINT32_MAX || idom[p] == kNoBlock) continue;
                newIdom = (newIdom == kNoBlock) ? p : intersectIdom(newIdom, p);
            }
            if (newIdom != kNoBlock && idom[b] != newIdom) {
                idom[b] = newIdom;
                domChanged = true;
            }
        }
    }
    idom[0] = kNoBlock;

    auto dominates = [&](BlockId a, BlockId b) -> bool {
        if (a == b) return true;
        if (b >= idom.size()) return false;
        // A block no path from the entry reaches never runs, so every block
        // dominates it vacuously. Lowering leaves such blocks (the statements
        // after a `return` in a branch), using values of the blocks before
        // them; any pass that rebuilds the function's blocks keeps them.
        if (rpoIndex[b] == UINT32_MAX) return true;
        for (BlockId cur = idom[b]; cur != kNoBlock; cur = idom[cur]) {
            if (cur == a) return true;
        }
        return false;
    };

    // Whether a path enters `b` through the handler of `from` without passing
    // `from` again. The handler is entered from the middle of `from`, and the
    // backend makes no value an instruction of `from` defines available to
    // it (a helper call there is an invoke whose result exists only on its
    // normal edge), so such a value does not reach `b` on that path even
    // where `from` dominates `b`. Block parameters are defined on entry and
    // do. One set per `from`, made on first ask.
    std::vector<std::vector<uint8_t>> throwShadow(numBlocks);
    auto throwReaches = [&](BlockId from, BlockId b) -> bool {
        const BlockId handler = fn.blocks[from].handler;
        if (handler == kNoBlock || handler >= numBlocks) return false;
        std::vector<uint8_t>& shadow = throwShadow[from];
        if (shadow.empty()) {
            shadow.assign(numBlocks, 0);
            std::vector<BlockId> work;
            if (handler != from) {
                shadow[handler] = 1;
                work.push_back(handler);
            }
            while (!work.empty()) {
                const BlockId at = work.back();
                work.pop_back();
                for (BlockId next : succs[at]) {
                    if (next == from || shadow[next]) continue;
                    shadow[next] = 1;
                    work.push_back(next);
                }
            }
        }
        return shadow[b] != 0;
    };

    // Helper to check use of a value
    auto checkUse = [&](ValueId id, size_t useBlockIdx, size_t useInstIdx) -> bool {
        auto it = definedTypes.find(id);
        if (it == definedTypes.end()) {
            diags.error(Span{}, "Function " + fn.name + ": use of undefined value %" + std::to_string(id));
            return false;
        }
        const auto& loc = defLocations[id];
        if (loc.blockIdx == useBlockIdx && loc.instIdx >= static_cast<int>(useInstIdx)) {
            diags.error(Span{}, "Function " + fn.name + ": within-block use-after-def of %" + std::to_string(id));
            return false;
        }

        if (id >= static_cast<ValueId>(fn.params.size()) && loc.blockIdx != useBlockIdx) {
            if (!dominates(static_cast<BlockId>(loc.blockIdx), static_cast<BlockId>(useBlockIdx))) {
                diags.error(Span{}, "Function " + fn.name + ": definition of %" +
                                      std::to_string(id) + " in b" + std::to_string(loc.blockIdx) +
                                      " does not dominate use in b" + std::to_string(useBlockIdx));
                return false;
            }
            if (loc.instIdx >= 0 && throwReaches(static_cast<BlockId>(loc.blockIdx), static_cast<BlockId>(useBlockIdx))) {
                diags.error(Span{}, "Function " + fn.name + ": definition of %" + std::to_string(id) + " in b" +
                                      std::to_string(loc.blockIdx) + " is used in b" + std::to_string(useBlockIdx) +
                                      ", which a path reaches through b" + std::to_string(loc.blockIdx) +
                                      "'s handler");
                return false;
            }
        }
        return true;
    };

    // Block arguments are the SSA join, so they carry the same obligations as
    // any other use: one argument per target block parameter, each a value
    // defined somewhere, each of the parameter's type. An argument list that
    // satisfies none of that becomes an LLVM phi incoming value, which is why
    // this is a hard error rather than a lint.
    auto checkTarget = [&](const BlockTarget& target, size_t useBlockIdx, size_t useInstIdx) -> bool {
        if (target.block >= fn.blocks.size()) {
            diags.error(Span{}, "Function " + fn.name + ": branch target b" +
                                  std::to_string(target.block) + " out of range");
            return false;
        }
        const auto& tgtBlock = fn.blocks[target.block];
        if (target.args.size() != tgtBlock.params.size()) {
            diags.error(Span{}, "Function " + fn.name + ": target b" + std::to_string(target.block) +
                                  " expects " + std::to_string(tgtBlock.params.size()) +
                                  " arguments, got " + std::to_string(target.args.size()));
            return false;
        }
        for (size_t i = 0; i < target.args.size(); ++i) {
            ValueId argId = target.args[i];
            // Named separately from "undefined value": a `kNoValue` argument
            // is a producer that had no value to pass and passed the sentinel
            // anyway (an env-backed or uninitialised binding reaching an edge
            // it should never reach), not a typo'd id.
            if (argId == kNoValue) {
                diags.error(Span{}, "Function " + fn.name + ": argument " + std::to_string(i) +
                                        " to b" + std::to_string(target.block) +
                                        " is the no-value sentinel %" + std::to_string(kNoValue));
                return false;
            }
            if (!checkUse(argId, useBlockIdx, useInstIdx)) return false;
            if (definedTypes[argId] != tgtBlock.params[i].type) {
                diags.error(Span{}, "Function " + fn.name + ": type mismatch for argument " +
                                      std::to_string(i) + " passed to b" + std::to_string(target.block) +
                                      " (argument is " + typeName(definedTypes[argId]) +
                                      ", parameter is " + typeName(tgtBlock.params[i].type) + ")");
                return false;
            }
        }
        return true;
    };

    // Resume ids seen so far: each names one point in G, so a second would
    // make a `leave` ambiguous.
    std::unordered_set<int32_t> resumeIds;

    // 3. Verify instructions and terminators
    for (size_t bIdx = 0; bIdx < fn.blocks.size(); ++bIdx) {
        const auto& block = fn.blocks[bIdx];
        if (block.instructions.empty()) {
            diags.error(Span{}, "Function " + fn.name + ": block b" + std::to_string(bIdx) + " has no instructions");
            return false;
        }

        for (size_t iIdx = 0; iIdx < block.instructions.size(); ++iIdx) {
            const auto& inst = block.instructions[iIdx];
            bool isLast = (iIdx == block.instructions.size() - 1);
            bool term = isTerminator(inst.op);

            if (term && !isLast) {
                diags.error(Span{}, "Function " + fn.name + ": terminator inside block b" +
                                      std::to_string(bIdx) + " before last instruction");
                return false;
            }
            if (!term && isLast) {
                diags.error(Span{}, "Function " + fn.name + ": block b" + std::to_string(bIdx) +
                                      " does not end with a terminator");
                return false;
            }

            for (ValueId opId : inst.operands) {
                if (!checkUse(opId, bIdx, iIdx)) return false;
            }
            // The speculation forms have fixed signatures; what they assume
            // about their operands beyond the type (a dominating test) is not
            // checked here, by design (il_op.h).
            if (const SpecSignature* sig = specSignature(inst.op)) {
                bool ok = inst.operands.size() == sig->count && inst.type == sig->result &&
                          (inst.result != kNoValue) == (sig->result != Type::Void) &&
                          (!sig->nonNegativeImm || inst.immI32 >= 0);
                for (size_t i = 0; ok && i < sig->count; ++i) {
                    ok = definedTypes[inst.operands[i]] == sig->operands[i];
                }
                if (!ok) {
                    std::string want = "(";
                    for (size_t i = 0; i < sig->count; ++i) {
                        want += (i ? ", " : "") + std::string(typeName(sig->operands[i]));
                    }
                    want += ") -> " + std::string(typeName(sig->result));
                    diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) + " in b" +
                                            std::to_string(bIdx) + " does not have the form " + want +
                                            (sig->nonNegativeImm ? " with a non-negative immediate" : ""));
                    return false;
                }
            }
            if (inst.op == Op::ShapeIn) {
                bool ok = inst.operands.size() >= 3 && inst.type == Type::Bool && inst.result != kNoValue;
                for (size_t i = 0; ok && i < inst.operands.size(); ++i) {
                    ok = definedTypes[inst.operands[i]] == Type::Ptr;
                }
                if (!ok) {
                    diags.error(Span{}, "Function " + fn.name + ": shape.in in b" + std::to_string(bIdx) +
                                            " does not have the form (ptr, ptr, ptr...) -> bool");
                    return false;
                }
            }
            if (inst.op == Op::ResumePoint) {
                if (fn.isSpeculated()) {
                    diags.error(Span{}, "Function " + fn.name +
                                            ": resume.point in a speculated function; only the generic "
                                            "function it leaves to has resume points");
                    return false;
                }
                if (inst.result != kNoValue || inst.immI32 < 0 ||
                    !resumeIds.insert(inst.immI32).second) {
                    diags.error(Span{}, "Function " + fn.name + ": resume.point " +
                                            std::to_string(inst.immI32) +
                                            " is negative, has a result or repeats an id");
                    return false;
                }
                for (ValueId v : inst.operands) {
                    if (definedTypes[v] == Type::Void) {
                        diags.error(Span{}, "Function " + fn.name + ": resume.point " +
                                                std::to_string(inst.immI32) + " carries a void value");
                        return false;
                    }
                }
            }
            if ((inst.op == Op::Leave || inst.op == Op::ResumeCall) && !fn.isSpeculated()) {
                diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                        " in a function that was not speculated from another");
                return false;
            }
            // The point after the generic call (il.h `resumeAfter`) is a
            // direct call's, and only a speculated function's: its id names
            // a point of the function's G, checked with the module.
            if (inst.resumeAfter != Instruction::kNoResumeAfter &&
                (inst.op != Op::Call || !fn.isSpeculated())) {
                diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                        " carries a resume-after id; only a direct call in a "
                                        "speculated function may");
                return false;
            }
            if (inst.op == Op::IsPlainArray && (inst.operands.size() != 1 || inst.type != Type::Bool)) {
                diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) + " takes one operand and is a bool");
                return false;
            }
            // The kind is compared against the view header's kind word, so it
            // must be one of the Number kinds.
            const int32_t taKind = inst.immI32 & ~kTypedArrayPristineLength;
            if (inst.op == Op::IsTypedArray &&
                (inst.operands.size() != 1 || inst.type != Type::Bool || inst.result == kNoValue ||
                 definedTypes[inst.operands[0]] != Type::Dynamic ||
                 taKind < 0 || taKind > BRONZE_ABI_TA_KIND_FLOAT64)) {
                diags.error(Span{}, "Function " + fn.name +
                                        ": is.typed_array takes one dynamic operand and a Number element kind, "
                                        "and is a bool");
                return false;
            }

            // The fixed-operand call forms end at sixteen arguments — the
            // last helper of the super-call ladder is `bronze_super_call_16`
            // (abi/bronze_abi.h), and every form is bounded alike.
            // The lowerer hands a longer list to the `*.spread` form as one
            // array (`Lowerer::argsTakeArrayPath`); this is what says so.
            {
                size_t fixedPrefix = 0;
                bool fixedForm = true;
                switch (inst.op) {
                    case Op::DynamicCall: fixedPrefix = 2; break;  // callee, this
                    case Op::SuperCall: fixedPrefix = 2; break;    // base, this
                    case Op::MethodCall: fixedPrefix = 1; break;   // receiver
                    case Op::Construct: fixedPrefix = 1; break;    // constructor
                    default: fixedForm = false; break;
                }
                if (fixedForm && inst.operands.size() > fixedPrefix + 16) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                            " passes " +
                                            std::to_string(inst.operands.size() - fixedPrefix) +
                                            " arguments; past 16 the spread form carries them");
                    return false;
                }
            }

            // A suspension and the mode it resumed with exist only in a
            // coroutine body: the backend reads both off the frame that only a
            // body has.
            if ((inst.op == Op::CoroSuspend || inst.op == Op::CoroMode) && !fn.isCoroutineBody()) {
                diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                        " outside a coroutine body");
                return false;
            }
            if (inst.op == Op::CoroSuspend &&
                (inst.operands.size() != 1 || definedTypes[inst.operands[0]] != Type::Dynamic ||
                 inst.type != Type::Dynamic)) {
                diags.error(Span{}, "Function " + fn.name +
                                        ": coro.suspend takes one dynamic operand and answers dynamic");
                return false;
            }
            if (inst.op == Op::CoroMode && inst.type != Type::I32) {
                diags.error(Span{}, "Function " + fn.name + ": coro.mode answers i32");
                return false;
            }

            if (inst.op == Op::Throw) {
                if (inst.operands.size() != 1) {
                    diags.error(Span{}, "Function " + fn.name +
                                            ": throw takes exactly one operand");
                    return false;
                }
                if (definedTypes[inst.operands[0]] != Type::Dynamic) {
                    diags.error(Span{}, "Function " + fn.name + ": throw operand %" +
                                            std::to_string(inst.operands[0]) + " is " +
                                            typeName(definedTypes[inst.operands[0]]) +
                                            ", not dynamic (any value can be thrown)");
                    return false;
                }
            }

            // `is.number`: both halves of its shape are load-bearing: the operand must be BOXED, because
            // the test is a compare against the NaN-box's number range and an
            // f64 operand would be comparing a double's bits to a tag bound,
            // and the result must be `bool`, because the only thing that may
            // consume it is a `br`.
            if (inst.op == Op::IsNumber) {
                if (inst.operands.size() != 1 || inst.result == kNoValue) {
                    diags.error(Span{}, "Function " + fn.name +
                                            ": is.number takes exactly one operand and has a "
                                            "result");
                    return false;
                }
                if (definedTypes[inst.operands[0]] != Type::Dynamic) {
                    diags.error(Span{}, "Function " + fn.name + ": is.number operand %" +
                                            std::to_string(inst.operands[0]) + " is " +
                                            typeName(definedTypes[inst.operands[0]]) +
                                            ", not dynamic");
                    return false;
                }
                if (inst.type != Type::Bool) {
                    diags.error(Span{}, "Function " + fn.name + ": is.number result %" +
                                            std::to_string(inst.result) + " is " +
                                            typeName(inst.type) + ", not bool");
                    return false;
                }
            }

            // `is.dense_array` owes the same two halves plus one: a receiver
            // whose bits are a tagged value, a `bool` for a `br` to consume,
            // and a NON-NEGATIVE index — `immI32` is compared against a
            // length, and a negative claim would widen to an enormous unsigned
            // bound.
            if (inst.op == Op::IsDenseArray) {
                const std::string opn = opName(inst.op);
                if (inst.operands.size() != 1 || inst.result == kNoValue) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opn +
                                            " takes exactly one operand and has a "
                                            "result");
                    return false;
                }
                if (definedTypes[inst.operands[0]] != Type::Dynamic) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opn + " operand %" +
                                            std::to_string(inst.operands[0]) + " is " +
                                            typeName(definedTypes[inst.operands[0]]) +
                                            ", not dynamic");
                    return false;
                }
                if (inst.type != Type::Bool) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opn + " result %" +
                                            std::to_string(inst.result) + " is " +
                                            typeName(inst.type) + ", not bool");
                    return false;
                }
                if (inst.immI32 < 0) {
                    diags.error(Span{}, "Function " + fn.name +
                                            ": " + opn + " index " +
                                            std::to_string(inst.immI32) + " is negative");
                    return false;
                }
            }

            if (inst.op == Op::Jump) {
                if (!checkTarget(inst.target, bIdx, iIdx)) return false;
            } else if (inst.op == Op::Branch) {
                if (inst.operands.empty()) {
                    diags.error(Span{}, "Function " + fn.name + ": br missing condition operand");
                    return false;
                }
                ValueId condId = inst.operands[0];
                if (definedTypes[condId] != Type::Bool) {
                    diags.error(Span{}, "Function " + fn.name + ": br condition %" +
                                          std::to_string(condId) + " is not bool");
                    return false;
                }
                if (inst.target.block == inst.elseTarget.block) {
                    diags.error(Span{}, "Function " + fn.name + ": br has identical then and else target b" +
                                          std::to_string(inst.target.block));
                    return false;
                }
                if (!checkTarget(inst.target, bIdx, iIdx)) return false;
                if (!checkTarget(inst.elseTarget, bIdx, iIdx)) return false;
            }
        }
    }

    if (!verifyConcatChain(fn, diags)) return false;
    if (!verifyTemplatePieces(fn, diags)) return false;
    return verifyFunctionProofs(fn, nullptr, diags);
}

}  // namespace bronze::il
