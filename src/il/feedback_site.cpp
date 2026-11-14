#include "il/feedback_site.h"

#include "abi/bronze_abi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string_view>
#include <utility>

namespace bronze::il {

// The ops whose Dynamic operands are VALUE sites: the ones whose generic form
// is a runtime ladder a typed specialization would replace — arithmetic,
// relational and equality compares, truthiness, and ToNumber.
bool observesOperandTags(const Instruction& inst) {
    switch (inst.op) {
        case Op::Add: case Op::Sub: case Op::Mul: case Op::Div: case Op::Mod: case Op::Pow:
        case Op::Neg: case Op::BitAnd: case Op::BitOr: case Op::BitXor: case Op::Shl:
        case Op::Shr: case Op::UShr: case Op::BitNot: case Op::MathImul:
        case Op::ToNumeric: case Op::NumericStep: case Op::ToInt32:
        case Op::RelLt: case Op::RelGt: case Op::RelLe: case Op::RelGe:
        case Op::CmpLt: case Op::CmpGt: case Op::CmpLe: case Op::CmpGe: case Op::CmpEq:
        case Op::CmpNe: case Op::StrictEq: case Op::LooseEq:
            return true;
        // Truthiness (`unbox.bool`) and the checked ToNumber. A raw or nullish
        // unbox is already proven, and observing it would measure the proof.
        case Op::Unbox:
            return !inst.rawUnbox && !inst.nullishUnbox;
        default:
            return false;
    }
}

namespace {

bool observesResult(const Instruction& inst) {
    switch (inst.op) {
        case Op::Call: case Op::DynamicCall: case Op::DynamicCallSpread: case Op::MethodCall:
        case Op::MethodCallSpread: case Op::PropGet: case Op::ElemGet:
            return true;
        default:
            return false;
    }
}

bool namesCallee(Op op) {
    switch (op) {
        case Op::Call: case Op::CreateFunction: case Op::FunctionRef: case Op::CoroStart:
        case Op::MethodIcIs: case Op::AccessorIcIs: case Op::ClosureIs:
            return true;
        default:
            return false;
    }
}

// A transition run (lower/transition_runs.h) is `transitionRunLength`
// instructions the backend lowers as one stretch, counted from its first site.
// Nothing may be inserted strictly inside one, so a site whose record would
// land there is not a site. `interior[i]` says an insertion BEFORE instruction
// i would split a run.
std::vector<bool> runInteriors(const Block& block) {
    std::vector<bool> interior(block.instructions.size() + 1, false);
    for (size_t i = 0; i < block.instructions.size(); ++i) {
        const Instruction& inst = block.instructions[i];
        if (inst.op != Op::PropSet || inst.transitionRunCell == Instruction::kNoTransitionRun) continue;
        const size_t end = std::min(block.instructions.size(), i + inst.transitionRunLength);
        for (size_t j = i + 1; j < end; ++j) interior[j] = true;
    }
    return interior;
}

bool blockZeroHasPredecessor(const Function& fn) {
    const BlockId entry = fn.blocks.front().id;
    for (const Block& block : fn.blocks) {
        if (block.handler == entry) return true;
        for (const Instruction& inst : block.instructions) {
            if (inst.target.block == entry || inst.elseTarget.block == entry) return true;
        }
    }
    return false;
}

struct Fnv {
    uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    }
    void u64(uint64_t v) { bytes(&v, sizeof v); }
    void str(const std::string& s) {
        u64(s.size());
        bytes(s.data(), s.size());
    }
};

}  // namespace

const char* feedbackKindName(FeedbackKind kind) {
    static const char* const kNames[] = BRONZE_FEEDBACK_KIND_NAMES;
    const auto k = static_cast<uint32_t>(kind);
    return k < BRONZE_FEEDBACK_KIND_COUNT ? kNames[k] : "?";
}

std::vector<FeedbackSite> enumerateFeedbackSites(const Module& module, const Function& fn) {
    std::vector<FeedbackSite> sites;
    if (fn.isExternal || fn.blocks.empty()) return sites;
    (void)module;

    // Each value's type and whether it is held as raw double bits. A
    // `numberBits` read is Dynamic in name only — the backend keeps it as a
    // double until its guard chain — so it is never an operand of a record.
    std::vector<Type> types(fn.valueCount, Type::Void);
    std::vector<bool> doubleHeld(fn.valueCount, false);
    for (size_t p = 0; p < fn.params.size() && p < types.size(); ++p) types[p] = fn.params[p].type;
    for (const Block& block : fn.blocks) {
        for (const BlockParam& bp : block.params) {
            if (bp.id < types.size()) types[bp.id] = bp.type;
        }
        for (const Instruction& inst : block.instructions) {
            if (inst.result == kNoValue || inst.result >= types.size()) continue;
            types[inst.result] = inst.type;
            if (inst.numberBits) doubleHeld[inst.result] = true;
        }
    }
    auto observable = [&](ValueId v) {
        return v < types.size() && types[v] == Type::Dynamic && !doubleHeld[v];
    };

    auto add = [&](FeedbackKind kind, uint32_t block, uint32_t inst, ValueId value,
                   uint32_t keyIndex = 0) {
        FeedbackSite s;
        s.kind = kind;
        s.ordinal = static_cast<uint32_t>(sites.size());
        s.block = block;
        s.inst = inst;
        s.value = value;
        s.keyIndex = keyIndex;
        sites.push_back(s);
    };

    // Entry sites sit at the top of block 0, so they need block 0 to run once
    // per call: a loop back to it would count iterations as entries. The
    // module's own entry point is left alone too — it runs once, and its first
    // instruction is the native-table bind.
    if (!fn.isEntryPoint && !blockZeroHasPredecessor(fn)) {
        add(FeedbackKind::Entry, FeedbackSite::kEntrySite, 0, kNoValue);
        if (fn.needsThis) {
            const ValueId thisId = fn.needsEnv ? 1u : 0u;
            if (observable(thisId)) add(FeedbackKind::This, FeedbackSite::kEntrySite, 0, thisId);
        }
        const size_t first = fn.firstSourceParam();
        const size_t last = fn.params.size() - (fn.hasRestParam ? 1u : 0u);
        for (size_t p = first; p < last; ++p) {
            if (observable(static_cast<ValueId>(p))) {
                add(FeedbackKind::Param, FeedbackSite::kEntrySite, 0, static_cast<ValueId>(p));
            }
        }
    }

    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const Block& block = fn.blocks[b];
        const std::vector<bool> interior = runInteriors(block);
        const auto bi = static_cast<uint32_t>(b);
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const Instruction& inst = block.instructions[i];
            const auto ii = static_cast<uint32_t>(i);
            if (!interior[i]) {
                if (observesOperandTags(inst)) {
                    for (ValueId v : inst.operands) {
                        if (observable(v)) add(FeedbackKind::Value, bi, ii, v);
                    }
                }
                const bool icSite = inst.op == Op::PropGet || inst.op == Op::PropSet;
                if (icSite && !inst.operands.empty() && observable(inst.operands[0])) {
                    add(FeedbackKind::Shape, bi, ii, inst.operands[0]);
                }
                if ((inst.op == Op::DynamicCall || inst.op == Op::DynamicCallSpread) &&
                    !inst.operands.empty() && observable(inst.operands[0])) {
                    add(FeedbackKind::Target, bi, ii, inst.operands[0]);
                }
                if ((inst.op == Op::MethodCall || inst.op == Op::MethodCallSpread) &&
                    !inst.operands.empty() && observable(inst.operands[0])) {
                    add(FeedbackKind::Method, bi, ii, inst.operands[0], inst.keyIndex);
                }
            }
            // After instruction i is before instruction i + 1.
            if (!interior[i + 1] && observesResult(inst) && observable(inst.result)) {
                add(FeedbackKind::Result, bi, ii, inst.result);
            }
        }
    }
    return sites;
}

std::string stableFunctionName(const std::string& ilName) {
    std::string out;
    out.reserve(ilName.size());
    static const char kAnon[] = "__anon_fn_";
    const size_t anonLen = sizeof(kAnon) - 1;
    size_t i = 0;
    // `modN.` as the first component: the module linker's renaming, numbered
    // in graph order, so adding a module renumbers every later one.
    if (ilName.compare(0, 3, "mod") == 0) {
        size_t j = 3;
        while (j < ilName.size() && ilName[j] >= '0' && ilName[j] <= '9') ++j;
        if (j > 3 && j < ilName.size() && ilName[j] == '.') {
            out = "mod";
            i = j;
        }
    }
    while (i < ilName.size()) {
        if (ilName.compare(i, anonLen, kAnon) == 0) {
            out += kAnon;
            i += anonLen;
            while (i < ilName.size() && ilName[i] >= '0' && ilName[i] <= '9') ++i;
            continue;
        }
        out += ilName[i++];
    }
    return out;
}

uint64_t functionIlHash(const Module& module, const Function& fn) {
    Fnv h;
    auto fnName = [&](uint32_t idx) {
        return idx < module.functions.size() ? stableFunctionName(module.functions[idx].name)
                                             : std::string("?");
    };
    auto key = [&](uint32_t idx) {
        // Key 0 is whatever lowering interned first, and it is also every
        // key-less op's default: hashing its string would tie every function
        // to the program's first key. A real key 0 hashes as its index.
        if (idx == 0 || idx >= module.keyConstants.size()) {
            h.u64(idx);
            return;
        }
        h.str(module.keyConstants[idx]);
    };

    h.u64(fn.params.size());
    for (const Param& p : fn.params) {
        h.u64(static_cast<uint64_t>(p.type));
        h.u64(p.pinned ? 1u : 0u);
    }
    h.u64(static_cast<uint64_t>(fn.returnType));
    h.u64((fn.needsEnv ? 1u : 0u) | (fn.needsThis ? 2u : 0u) | (fn.needsArguments ? 4u : 0u) |
          (fn.isStrict ? 8u : 0u) | (fn.isGenerator ? 16u : 0u) | (fn.hasRestParam ? 32u : 0u));
    h.u64(static_cast<uint64_t>(static_cast<int64_t>(fn.coroKind)));
    // Not the inline-slot prediction (BRONZE_ABI_FN_INLINE_SLOTS_SHIFT): a
    // sizing hint that changes no site, so a profile outlives its retuning.
    h.u64(fn.fnFlags & ~(uint32_t{BRONZE_ABI_FN_INLINE_SLOTS_MASK} << BRONZE_ABI_FN_INLINE_SLOTS_SHIFT));
    if (fn.typedOrigin != Function::kNoTypedOrigin) h.str(fnName(fn.typedOrigin));
    h.u64(fn.blocks.size());
    for (const Block& block : fn.blocks) {
        h.u64(block.id);
        h.u64(block.handler);
        h.u64(static_cast<uint64_t>(block.copyClass));
        h.u64(block.params.size());
        for (const BlockParam& bp : block.params) {
            h.u64(bp.id);
            h.u64(static_cast<uint64_t>(bp.type));
        }
        h.u64(block.instructions.size());
        for (const Instruction& inst : block.instructions) {
            h.u64(static_cast<uint64_t>(inst.op));
            h.u64(static_cast<uint64_t>(inst.type));
            h.u64(inst.result);
            h.u64(inst.operands.size());
            for (ValueId v : inst.operands) h.u64(v);
            uint64_t f64bits = 0;
            std::memcpy(&f64bits, &inst.immF64, sizeof f64bits);
            h.u64(f64bits);
            // create.object's immI32 is its inline-slot prediction, as above.
            h.u64(inst.op == Op::CreateObject ? 0u : static_cast<uint32_t>(inst.immI32));
            h.u64(static_cast<uint64_t>(inst.boxType));
            key(inst.keyIndex);
            if (namesCallee(inst.op)) h.str(fnName(inst.calleeIndex));
            if (inst.directTarget != Instruction::kNoDirectTarget) h.str(fnName(inst.directTarget));
            if (inst.typedTarget != Instruction::kNoDirectTarget) h.str(fnName(inst.typedTarget));
            h.u64((inst.icMonomorphic ? 1u : 0u) | (inst.icPolymorphic ? 2u : 0u) |
                  (inst.icAbsent ? 4u : 0u) | (inst.icFnRecv ? 8u : 0u) | (inst.rawUnbox ? 16u : 0u) |
                  (inst.nullishUnbox ? 32u : 0u) | (inst.numberBits ? 64u : 0u) |
                  (inst.shapeGuarded ? 128u : 0u) | (inst.staticSlotGuessed ? 256u : 0u) |
                  (inst.envImmutable ? 512u : 0u) | (inst.rawBox ? 1024u : 0u));
            h.u64(inst.staticSlot);
            h.u64(inst.transitionRunCell == Instruction::kNoTransitionRun ? 0u
                                                                           : inst.transitionRunLength);
            h.u64(inst.envDepth);
            h.u64(inst.envIndex);
            h.u64(inst.callEnvHops);
            h.u64(inst.target.block);
            for (ValueId v : inst.target.args) h.u64(v);
            h.u64(inst.elseTarget.block);
            for (ValueId v : inst.elseTarget.args) h.u64(v);
        }
    }
    return h.h;
}

bool verifyFeedbackRecords(const Function& fn, DiagnosticSink& diags) {
    std::vector<Type> types(fn.valueCount, Type::Void);
    for (size_t p = 0; p < fn.params.size() && p < types.size(); ++p) types[p] = fn.params[p].type;
    for (const Block& block : fn.blocks) {
        for (const BlockParam& bp : block.params) {
            if (bp.id < types.size()) types[bp.id] = bp.type;
        }
        for (const Instruction& inst : block.instructions) {
            if (inst.result != kNoValue && inst.result < types.size()) types[inst.result] = inst.type;
        }
    }
    for (const Block& block : fn.blocks) {
        for (const Instruction& inst : block.instructions) {
            if (inst.op != Op::FeedbackRecord) continue;
            const uint32_t info = static_cast<uint32_t>(inst.immI32);
            const uint32_t kind = BRONZE_FEEDBACK_INFO_KIND(info);
            const size_t want = kind == BRONZE_FEEDBACK_KIND_ENTRY ? 0u : 1u;
            const bool ok = inst.type == Type::Void && inst.result == kNoValue &&
                            (info & BRONZE_FEEDBACK_FLAG) != 0 && kind != BRONZE_FEEDBACK_KIND_MARKER &&
                            kind < BRONZE_FEEDBACK_KIND_METHOD_NAME && inst.operands.size() == want &&
                            (want == 0 || (inst.operands[0] < types.size() &&
                                           types[inst.operands[0]] == Type::Dynamic));
            if (!ok) {
                diags.error(Span{}, "Function " + fn.name +
                                        ": feedback.record takes a site word and one dynamic operand "
                                        "(none for an entry site), and is void");
                return false;
            }
        }
    }
    return true;
}

std::vector<std::string> feedbackFunctionKeys(const Module& module) {
    std::vector<std::string> keys(module.functions.size());
    std::map<std::string, uint32_t> seen;
    for (size_t i = 0; i < module.functions.size(); ++i) {
        const Function& fn = module.functions[i];
        if (fn.isExternal) continue;
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx",
                      static_cast<unsigned long long>(functionIlHash(module, fn)));
        std::string k = stableFunctionName(fn.name) + "#" + hex;
        const uint32_t n = seen[k]++;
        if (n != 0) k += "~" + std::to_string(n);
        keys[i] = std::move(k);
    }
    return keys;
}

uint64_t icLayoutDigest(const Module& module) {
    uint64_t h = 1469598103934665603ull;
    auto mixByte = [&h](uint8_t b) {
        h ^= b;
        h *= 1099511628211ull;
    };
    auto mix64 = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) mixByte(static_cast<uint8_t>(v >> (i * 8)));
    };
    auto mixText = [&](std::string_view s) {
        mix64(s.size());
        for (unsigned char c : s) mixByte(c);
    };
    mix64(module.sourceTexts.size());
    for (const std::string& text : module.sourceTexts) mixText(text);
    mix64(module.icSiteCount);
    mix64(module.slowSiteCount);
    // Index -> (op, key): the first instruction met in function, block and
    // instruction order. Copies of a site (inlining, versioning, threading)
    // share its index and say the same thing about it.
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> named;
    for (const Function& fn : module.functions) {
        for (const Block& block : fn.blocks) {
            for (const Instruction& inst : block.instructions) {
                if (inst.op != Op::PropGet && inst.op != Op::PropSet && inst.op != Op::MethodCall &&
                    inst.op != Op::MethodCallSpread) {
                    continue;
                }
                named.emplace(inst.icIndex, std::make_pair(static_cast<uint32_t>(inst.op), inst.keyIndex));
            }
        }
    }
    for (const auto& [index, what] : named) {
        mix64(index);
        mix64(what.first);
        mixText(what.second < module.keyConstants.size() ? std::string_view(module.keyConstants[what.second])
                                                         : std::string_view("?"));
    }
    return h;
}

void numberSlowSites(Module& module) {
    uint32_t next = 0;
    for (Function& fn : module.functions) {
        for (Block& block : fn.blocks) {
            for (Instruction& inst : block.instructions) {
                switch (inst.op) {
                    case Op::ElemGet: case Op::ElemSet: case Op::PropGet: case Op::PropSet:
                    case Op::StrictEq: case Op::Unbox:
                    case Op::Add: case Op::Sub: case Op::Mul: case Op::Div:
                    case Op::Branch:
                        inst.slowSite = next;
                        next += kSlowSiteArms;
                        break;
                    default:
                        break;
                }
            }
        }
    }
    module.slowSiteCount = next;
}

}  // namespace bronze::il
