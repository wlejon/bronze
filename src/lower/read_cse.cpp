#include "lower/read_cse.h"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "lower/guard_region_cfg.h"
#include "lower/loop_prop_hoist.h"

namespace bronze::lower {

namespace {

// A read whose value is in hand: a property read of `recv` at `key`, or a
// binding read at `index` of the environment `depth` levels above `recv`.
struct Read {
    bool env = false;
    il::ValueId recv = il::kNoValue;
    uint32_t key = 0;
    uint32_t depth = 0;
    il::ValueId value = il::kNoValue;

    bool sameRead(const Read& o) const {
        return env == o.env && recv == o.recv && key == o.key && depth == o.depth;
    }
};

bool typedOperand(const il::Instruction& inst, const std::vector<il::Type>& types) {
    for (il::ValueId v : inst.operands) {
        if (v >= types.size() || types[v] == il::Type::Dynamic) return false;
    }
    return true;
}

// What runs no code and writes nothing a read could see. Arithmetic and
// compares only over unboxed operands (over a boxed one they coerce, and
// ToPrimitive is a user `valueOf`); an unbox only where it is ToBoolean or a
// representation change (a checked number unbox is ToNumber).
bool transparent(const il::Instruction& inst, const std::vector<il::Type>& types) {
    switch (inst.op) {
        case il::Op::ConstF64: case il::Op::ConstI32: case il::Op::ConstBool:
        case il::Op::ConstUndefined: case il::Op::ConstNull:
        case il::Op::Box: case il::Op::StrictEq: case il::Op::IsNullish:
        case il::Op::IsNumber: case il::Op::IsDenseArray: case il::Op::IsTypedArray:
        case il::Op::IsNumberArray: case il::Op::IsShape: case il::Op::TypeOf:
        case il::Op::Jump: case il::Op::Branch: case il::Op::FunctionRef:
        case il::Op::KeepAlive: case il::Op::MethodIcIs: case il::Op::AccessorIcIs:
        case il::Op::ClosureIs: case il::Op::ClosureEnv:
            return true;
        case il::Op::Unbox:
            return inst.rawUnbox || inst.nullishUnbox || inst.type == il::Type::Bool;
        case il::Op::CmpLt: case il::Op::CmpGt: case il::Op::CmpLe: case il::Op::CmpGe:
        case il::Op::CmpEq: case il::Op::CmpNe: case il::Op::NumTruthy:
        case il::Op::Add: case il::Op::Sub: case il::Op::Mul: case il::Op::Div:
        case il::Op::Neg: case il::Op::MathUnary:
            return inst.type != il::Type::Dynamic && typedOperand(inst, types);
        default:
            return false;
    }
}

std::vector<il::Type> valueTypesOf(const il::Function& fn) {
    std::vector<il::Type> types(fn.valueCount, il::Type::Dynamic);
    for (size_t i = 0; i < fn.params.size() && i < types.size(); ++i) types[i] = fn.params[i].type;
    for (const il::Block& block : fn.blocks) {
        for (const il::BlockParam& p : block.params) {
            if (p.id < types.size()) types[p.id] = p.type;
        }
        for (const il::Instruction& inst : block.instructions) {
            if (inst.result != il::kNoValue && inst.result < types.size()) types[inst.result] = inst.type;
        }
    }
    return types;
}

class FunctionReads {
public:
    FunctionReads(il::Function& fn, const std::vector<uint8_t>& getter)
        : fn_(fn), getter_(getter), types_(valueTypesOf(fn)) {}

    bool run() {
        const Cfg cfg = buildCfg(fn_);
        std::vector<uint8_t> handler(fn_.blocks.size(), 0);
        for (const il::Block& block : fn_.blocks) {
            if (block.handler != il::kNoBlock && block.handler < handler.size()) handler[block.handler] = 1;
        }
        std::vector<std::vector<Read>> out(fn_.blocks.size());
        std::vector<uint8_t> done(fn_.blocks.size(), 0);
        std::vector<il::ValueId> alias(fn_.valueCount, il::kNoValue);
        bool any = false;
        for (il::BlockId b : cfg.rpo) {
            std::vector<Read> live;
            if (b != 0 && !handler[b] && !cfg.preds[b].empty()) live = joinOf(cfg.preds[b], out, done);
            il::Block& block = fn_.blocks[b];
            for (il::Instruction& inst : block.instructions) {
                Read r;
                if (readOf(inst, r)) {
                    // A receiver that is itself a folded read is the read it
                    // folded into; that one is never folded in turn.
                    if (r.recv < alias.size() && alias[r.recv] != il::kNoValue) r.recv = alias[r.recv];
                    const auto it = std::find_if(live.begin(), live.end(), [&](const Read& h) { return h.sameRead(r); });
                    if (it != live.end()) {
                        alias[inst.result] = it->value;
                        any = true;
                    } else {
                        r.value = inst.result;
                        live.push_back(r);
                    }
                    continue;
                }
                if (inst.op == il::Op::EnvSet) {
                    // Two environments can be one at different depths, so a
                    // binding write forgets every binding read.
                    live.erase(std::remove_if(live.begin(), live.end(), [](const Read& h) { return h.env; }),
                               live.end());
                    continue;
                }
                if (!transparent(inst, types_)) live.clear();
            }
            out[b] = std::move(live);
            done[b] = 1;
        }
        if (!any) return false;
        rewrite(alias);
        return true;
    }

private:
    // The reads every predecessor ends holding, with one value: a value all
    // of them hold is defined where it dominates each, so it dominates the
    // join. A predecessor not yet walked (a back edge) holds nothing.
    static std::vector<Read> joinOf(const std::vector<il::BlockId>& preds, const std::vector<std::vector<Read>>& out,
                                    const std::vector<uint8_t>& done) {
        for (il::BlockId p : preds) {
            if (!done[p]) return {};
        }
        std::vector<Read> live = out[preds[0]];
        for (size_t k = 1; k < preds.size(); ++k) {
            const std::vector<Read>& other = out[preds[k]];
            live.erase(std::remove_if(live.begin(), live.end(),
                                      [&](const Read& h) {
                                          return std::none_of(other.begin(), other.end(), [&](const Read& o) {
                                              return o.sameRead(h) && o.value == h.value;
                                          });
                                      }),
                       live.end());
        }
        return live;
    }

    bool readOf(const il::Instruction& inst, Read& r) const {
        if (inst.result == il::kNoValue || inst.type != il::Type::Dynamic) return false;
        if (inst.op == il::Op::EnvGet || inst.op == il::Op::EnvGetTdz) {
            if (inst.operands.size() != 1) return false;
            r.env = true;
            r.recv = inst.operands[0];
            r.depth = inst.envDepth;
            r.key = inst.envIndex;
            return true;
        }
        if (inst.op != il::Op::PropGet || inst.operands.size() != 1) return false;
        if (inst.directTarget != il::Instruction::kNoDirectTarget) return false;
        if (inst.keyIndex >= getter_.size() || getter_[inst.keyIndex]) return false;
        r.recv = inst.operands[0];
        r.key = inst.keyIndex;
        return true;
    }

    void rewrite(const std::vector<il::ValueId>& alias) {
        auto resolve = [&](il::ValueId v) {
            while (v < alias.size() && alias[v] != il::kNoValue) v = alias[v];
            return v;
        };
        for (il::Block& block : fn_.blocks) {
            std::vector<il::Instruction> kept;
            kept.reserve(block.instructions.size());
            for (il::Instruction& inst : block.instructions) {
                if (inst.result != il::kNoValue && inst.result < alias.size() && alias[inst.result] != il::kNoValue) {
                    continue;
                }
                for (il::ValueId& v : inst.operands) v = resolve(v);
                for (il::ValueId& v : inst.target.args) v = resolve(v);
                for (il::ValueId& v : inst.elseTarget.args) v = resolve(v);
                kept.push_back(std::move(inst));
            }
            block.instructions = std::move(kept);
        }
    }

    il::Function& fn_;
    const std::vector<uint8_t>& getter_;
    std::vector<il::Type> types_;
};

// The keys some getter may answer, or nothing when any key may be one. A
// getter is installed by an accessor in a class or literal (`accessor.def`),
// by a few names the runtime answers with one (`size`, `byteLength`, ...),
// and by a reflective define whose descriptor is not a literal of data
// fields: a literal one lowers to `define.own.attr`, which only ever writes a
// data property. A function making such a define may name its key with any
// string literal or literal field in it, so those all count; a define whose
// key or descriptor map is not one of those, a literal field there under a
// computed key, an accessor under a computed key, or a road to source text (`eval`, `Function`) leaves every key open.
// `freeze`, `seal` and `preventExtensions` install nothing, and a Proxy's
// trap is the loop hoist's case (read_cse.h).
std::optional<std::vector<uint8_t>> getterKeys(const il::Module& module) {
    const size_t nKeys = module.keyConstants.size();
    std::vector<uint8_t> getter(nKeys, 0);
    std::vector<uint8_t> reflective(nKeys, 0);
    uint32_t defineProperty = UINT32_MAX, defineProperties = UINT32_MAX, create = UINT32_MAX;
    uint32_t defineGetter = UINT32_MAX, objectKey = UINT32_MAX;
    std::vector<uint8_t> opaque(nKeys, 0);
    for (size_t k = 0; k < nKeys; ++k) {
        const std::string& name = module.keyConstants[k];
        const auto idx = static_cast<uint32_t>(k);
        if (name == "size" || name == "flags" || name == "source" || name == "buffer" || name == "byteLength" ||
            name == "byteOffset" || name == "__proto__" || name == "lastIndex") {
            getter[k] = 1;
        }
        if (name == "defineProperty") defineProperty = idx;
        if (name == "defineProperties") defineProperties = idx;
        if (name == "create") create = idx;
        if (name == "__defineGetter__" || name == "__defineSetter__") defineGetter = idx;
        if (name == "Object") objectKey = idx;
        if (name == "Reflect" || name == "eval" || name == "Function") opaque[k] = 1;
        if (name == "defineProperty" || name == "defineProperties" || name == "__defineGetter__" ||
            name == "__defineSetter__") {
            reflective[k] = 1;
        }
    }
    auto namesKey = [](const il::Instruction& inst) {
        switch (inst.op) {
            case il::Op::GlobalGet: case il::Op::PropGet: case il::Op::MethodCall: case il::Op::Call:
            case il::Op::DynamicCall: case il::Op::Construct: case il::Op::MethodCallSpread:
            case il::Op::DynamicCallSpread: case il::Op::ConstructSpread:
                return true;
            default:
                return false;
        }
    };
    for (const il::Function& fn : module.functions) {
        const std::vector<DefSite> defs = computeDefSites(fn);
        auto defOf = [&](il::ValueId v) -> const il::Instruction* {
            if (v >= defs.size() || defs[v].block == il::kNoBlock) return nullptr;
            return &fn.blocks[defs[v].block].instructions[defs[v].index];
        };
        auto isOp = [&](il::ValueId v, il::Op op) {
            const il::Instruction* d = defOf(v);
            return d != nullptr && d->op == op;
        };
        auto isStrLiteral = [&](il::ValueId v) {
            const il::Instruction* d = defOf(v);
            return d != nullptr && d->op == il::Op::Box && d->boxType == il::Type::Str && d->operands.empty();
        };
        bool installs = false;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.op == il::Op::AccessorDefComputed) return std::nullopt;
                if (inst.op == il::Op::AccessorDef && inst.keyIndex < nKeys) getter[inst.keyIndex] = 1;
                if (!namesKey(inst) || inst.keyIndex >= nKeys) continue;
                if (opaque[inst.keyIndex]) return std::nullopt;
                const bool objectCreate = inst.keyIndex == create && inst.op == il::Op::MethodCall &&
                                          inst.operands.size() >= 3 && isOp(inst.operands[0], il::Op::GlobalGet) &&
                                          defOf(inst.operands[0])->keyIndex == objectKey;
                if (!reflective[inst.keyIndex] && !objectCreate) continue;
                if (inst.op != il::Op::MethodCall) return std::nullopt;
                const std::vector<il::ValueId>& ops = inst.operands;
                if (inst.keyIndex == defineProperty) {
                    if (ops.size() < 3 || !isStrLiteral(ops[2])) return std::nullopt;
                } else if (inst.keyIndex == defineGetter) {
                    if (ops.size() < 2 || !isStrLiteral(ops[1])) return std::nullopt;
                } else if (inst.keyIndex == defineProperties || objectCreate) {
                    if (ops.size() < 3 || !isOp(ops[2], il::Op::CreateObject)) return std::nullopt;
                }
                installs = true;
            }
        }
        if (!installs) continue;
        std::unordered_set<il::ValueId> literals;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.op == il::Op::CreateObject) literals.insert(inst.result);
            }
        }
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                // A literal field under a computed or spread key names a key
                // no constant spells.
                const bool computedField =
                    (inst.op == il::Op::ElemSet || inst.op == il::Op::ObjectSpread ||
                     inst.op == il::Op::MethodDefComputed) &&
                    !inst.operands.empty() && literals.count(inst.operands[0]);
                if (computedField) return std::nullopt;
                if (inst.keyIndex >= nKeys) continue;
                const bool strLiteral = inst.op == il::Op::Box && inst.boxType == il::Type::Str && inst.operands.empty();
                const bool literalField =
                    inst.op == il::Op::PropSet && !inst.operands.empty() && literals.count(inst.operands[0]);
                if (strLiteral || literalField) getter[inst.keyIndex] = 1;
            }
        }
    }
    return getter;
}

}  // namespace

bool eliminateRepeatedReads(il::Module& module) {
    if (!module.censusOutPath.empty()) return false;
    const std::optional<std::vector<uint8_t>> getter = getterKeys(module);
    if (!getter) return false;
    const AccessorKeyFacts facts = computeAccessorKeyFacts(module);
    bool changed = false;
    for (size_t f = 0; f < module.functions.size(); ++f) {
        il::Function& fn = module.functions[f];
        if (fn.blocks.empty() || (f < facts.fnTouchesReflection.size() && facts.fnTouchesReflection[f])) continue;
        il::Function before = fn;
        if (!FunctionReads(fn, *getter).run()) continue;
        if (!dominanceHolds(fn)) {
            fn = std::move(before);
            continue;
        }
        changed = true;
    }
    return changed;
}

}  // namespace bronze::lower
