// The solver's driver: the instruction worklist and the transfer of every
// instruction that is not a property access or a call (those are
// solver_props.cpp and solver_calls.cpp).

#include "opt/wpo/solver.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "opt/wpo/solver_tags.h"

namespace bronze::wpo {

namespace {

uint16_t primsForType(il::Type type) {
    switch (type) {
        case il::Type::Bool: return kBool;
        case il::Type::I32: return kI32;
        case il::Type::F64: return kNumber;
        case il::Type::Str: return kStr;
        default: return 0;
    }
}

const AVal kEmpty;

// `mod<digits>.<name>`: the linker's rename of a binding of an imported module
// (lower_util.cpp, sourceSpelling). An identifier cannot hold a `.`.
bool linkerRenamed(const std::string& name) {
    if (name.size() < 5 || name.compare(0, 3, "mod") != 0) return false;
    size_t i = 3;
    while (i < name.size() && name[i] >= '0' && name[i] <= '9') ++i;
    return i > 3 && i < name.size() && name[i] == '.';
}

// The one boolean a comparison of two values gives, when only one is
// possible: what turns `typeof X !== 'undefined'` on a known type, and a
// test of a constant flag, into a branch with one live edge.
uint16_t compareConst(il::Op op, const AVal& a, const AVal& b) {
    if (a.empty() || b.empty()) return kBool;
    auto single = [](const AVal& v, uint16_t bit) { return v.prims == bit && !v.hasObjects(); };
    bool eq = false;
    if (a.isStrConst() && b.isStrConst()) {
        eq = a.strKey == b.strKey;
    } else if ((single(a, kTrue) || single(a, kFalse)) && (single(b, kTrue) || single(b, kFalse))) {
        eq = a.prims == b.prims;
    } else if (single(a, kUndef) && single(b, kUndef)) {
        eq = true;
    } else if (op == il::Op::LooseEq) {
        const uint16_t nullish = kUndef | kNull;
        if ((a.prims & ~nullish) != 0 || (b.prims & ~nullish) != 0 || a.hasObjects() || b.hasObjects()) return kBool;
        eq = true;
    } else {
        return kBool;
    }
    if (op == il::Op::CmpNe) eq = !eq;
    return eq ? kTrue : kFalse;
}

}  // namespace

AVal Solver::typeOfValue(const AVal& v) {
    // The one string `typeof` gives, when every value of the set gives it.
    if (v.empty()) return AVal{};
    const char* name = nullptr;
    if (!v.hasObjects()) {
        if (v.prims == kUndef) name = "undefined";
        else if ((v.prims & ~kNumber) == 0) name = "number";
        else if ((v.prims & ~(kStr | kStrOther)) == 0) name = "string";
        else if ((v.prims & ~kBool) == 0) name = "boolean";
    }
    if (name == nullptr) return AVal::prim(kStr);
    return AVal::str(keys_.intern(name));
}

Solver::Solver(const il::Module& mod, SolverOptions opts)
    : mod_(mod), opts_(std::move(opts)), catalog_(builtinCatalog()), insts_(mod), keys_(mod) {
    fns_.resize(mod_.functions.size());
    for (const auto& name : opts_.hostGlobals) hostGlobals_.insert(name);
    // `console` and `performance` are the host's, and still known.
    hostGlobals_.erase("console");
    hostGlobals_.erase("performance");
    for (const char* name : {"valueOf", "toString", "toJSON", "then", "handleEvent"}) {
        implicitKeys_.push_back(keys_.intern(name));
    }
    std::sort(implicitKeys_.begin(), implicitKeys_.end());
    prepass();
}

uint32_t Solver::internReason(const std::string& text) {
    auto it = reasonIndex_.find(text);
    if (it != reasonIndex_.end()) return it->second;
    const uint32_t index = static_cast<uint32_t>(reasons_.size());
    reasons_.push_back(text);
    reasonIndex_.emplace(text, index);
    return index;
}

// What can be decided about a creation site by looking at the instructions
// right after it, before anything could observe the object: which keys a class
// prototype is given before its class is reachable, and whether the
// prototype link is set by a `class.extend` / `object.proto` rather than being
// the default one.
void Solver::prepass() {
    const uint32_t protoKey = keys_.intern("prototype");
    for (uint32_t f = 0; f < mod_.functions.size(); ++f) {
        const auto& fn = mod_.functions[f];
        for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
            const auto& insts = fn.blocks[b].instructions;
            for (uint32_t i = 0; i < insts.size(); ++i) {
                const auto& inst = insts[i];
                if (inst.op == il::Op::MethodCall || inst.op == il::Op::MethodCallSpread) {
                    methodKeyByIc_.emplace(inst.icIndex, inst.keyIndex);
                }
                if (inst.op == il::Op::CreateObject) {
                    // A literal's `__proto__:` anywhere after it in this block.
                    for (uint32_t j = i + 1; j < insts.size(); ++j) {
                        if (insts[j].op == il::Op::ObjectLitProto && !insts[j].operands.empty() &&
                            insts[j].operands[0] == inst.result) {
                            siteInfo_[insts_.id(f, b, i)].literalProto = true;
                            break;
                        }
                    }
                    continue;
                }
                if (inst.op != il::Op::CreateFunction) continue;
                SiteInfo info;
                std::vector<il::ValueId> protoVals;
                auto isProto = [&](il::ValueId v) {
                    return std::find(protoVals.begin(), protoVals.end(), v) != protoVals.end();
                };
                for (uint32_t j = i + 1; j < insts.size(); ++j) {
                    const auto& next = insts[j];
                    const il::ValueId recv = next.operands.empty() ? il::kNoValue : next.operands[0];
                    if (next.op == il::Op::PropGet && recv == inst.result && next.keyIndex == protoKey) {
                        protoVals.push_back(next.result);
                        continue;
                    }
                    if ((next.op == il::Op::MethodDef || next.op == il::Op::AccessorDef) && isProto(recv)) {
                        info.definiteProtoKeys.push_back(next.keyIndex);
                        continue;
                    }
                    if (next.op == il::Op::ClassExtend && recv == inst.result) {
                        info.extendFollows = true;
                        continue;
                    }
                    if ((next.op == il::Op::MethodDef || next.op == il::Op::AccessorDef ||
                         next.op == il::Op::MethodDefComputed) &&
                        (recv == inst.result || isProto(recv))) {
                        continue;
                    }
                    if (il::isTerminator(next.op) || il::canThrow(next)) break;
                }
                std::sort(info.definiteProtoKeys.begin(), info.definiteProtoKeys.end());
                info.definiteProtoKeys.erase(
                    std::unique(info.definiteProtoKeys.begin(), info.definiteProtoKeys.end()),
                    info.definiteProtoKeys.end());
                siteInfo_[insts_.id(f, b, i)] = std::move(info);
            }
        }
    }
}

ObjId Solver::alloc(uint32_t site, uint32_t tag, ObjKind kind) {
    const uint64_t k = (static_cast<uint64_t>(site) << 32) | tag;
    auto it = sites_.find(k);
    if (it != sites_.end()) return it->second;
    const ObjId id = static_cast<ObjId>(objs_.size());
    objs_.emplace_back();
    objs_.back().kind = kind;
    objs_.back().site = site;
    objs_.back().tag = tag;
    sites_.emplace(k, id);
    return id;
}

void Solver::markReachable(uint32_t f) {
    FnState& st = fns_[f];
    if (st.reachable) return;
    const auto& fn = mod_.functions[f];
    st.reachable = true;
    st.values.resize(fn.valueCount);
    // A typed parameter holds that type's values whatever is passed.
    for (size_t p = 0; p < fn.params.size() && p < st.values.size(); ++p) {
        st.values[p].addPrims(primsForType(fn.params[p].type));
    }
    if (fn.isExternal) return;
    st.live.assign(fn.blocks.size(), 0);
    st.users.assign(fn.valueCount, {});
    for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
        const auto& insts = fn.blocks[b].instructions;
        for (uint32_t i = 0; i < insts.size(); ++i) {
            const uint32_t site = insts_.id(f, b, i);
            auto use = [&](il::ValueId v) {
                if (v >= st.users.size()) return;
                auto& u = st.users[v];
                if (u.empty() || u.back() != site) u.push_back(site);
            };
            for (il::ValueId v : insts[i].operands) use(v);
            for (il::ValueId v : insts[i].target.args) use(v);
            for (il::ValueId v : insts[i].elseTarget.args) use(v);
        }
    }
    if (!fn.blocks.empty()) markLive(f, 0);
}

void Solver::markLive(uint32_t f, uint32_t b) {
    FnState& st = fns_[f];
    const auto& fn = mod_.functions[f];
    // A block runs once control can reach it: from the entry, along an edge
    // a branch can take, or by a throw out of a live block into its handler.
    while (b < st.live.size() && !st.live[b]) {
        st.live[b] = 1;
        for (uint32_t i = 0; i < fn.blocks[b].instructions.size(); ++i) enqueueInst(insts_.id(f, b, i));
        b = fn.blocks[b].handler;
    }
}

void Solver::enqueueInst(uint32_t site) {
    if (queued_[site]) return;
    queued_[site] = 1;
    worklist_.push_back(site);
}

void Solver::enqueueUsers(uint32_t f, il::ValueId v) {
    const FnState& st = fns_[f];
    if (!st.reachable || v >= st.users.size()) return;
    for (uint32_t site : st.users[v]) enqueueInst(site);
}

void Solver::enqueueCallers(uint32_t f) {
    // A caller may be a read that ran a getter: it reads every receiver
    // again to take the new return.
    for (uint32_t site : fns_[f].callers) {
        rereadAll_[site] = 1;
        enqueueInst(site);
    }
}

ObjSet Solver::unreadReceivers(const ObjSet& recv, const ObjSet& seen) {
    // The receivers not read yet, and the read ones whose own heap facts
    // changed. A change anywhere else (a prototype, a getter's return) reads
    // them all again.
    auto dirty = dirtyObjs_.find(curSite_);
    bool all = rereadAll_[curSite_] != 0;
    rereadAll_[curSite_] = 0;
    ObjSet out;
    if (dirty != dirtyObjs_.end()) {
        for (ObjId d : dirty->second) {
            if (contains(seen, d)) out.push_back(d);
            else if (!contains(recv, d)) all = true;
        }
        dirtyObjs_.erase(dirty);
    }
    if (all) return recv;
    std::set_difference(recv.begin(), recv.end(), seen.begin(), seen.end(), std::back_inserter(out));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

void Solver::run() {
    const auto start = std::chrono::steady_clock::now();
    recording_ = true;
    queued_.assign(insts_.size(), 0);
    rereadAll_.assign(insts_.size(), 0);
    for (uint32_t f = 0; f < mod_.functions.size(); ++f) {
        const auto& fn = mod_.functions[f];
        if (fn.isEntryPoint) markReachable(f);
    }
    // A function the entry module exports is called by whoever loads the
    // program. One an imported module exports is reached through the imports
    // the program spells, which the IL shows as it shows any other binding.
    for (uint32_t f = 0; f < mod_.functions.size(); ++f) {
        const auto& fn = mod_.functions[f];
        if (fn.isExported && !fn.isEntryPoint && !fn.isExternal && !linkerRenamed(fn.name)) bindTop(f, kNoObj);
    }
    for (;;) {
        while (!worklist_.empty()) {
            const uint32_t site = worklist_.front();
            worklist_.pop_front();
            queued_[site] = 0;
            visit(site);
        }
        if (escapedSeen_ == escapedList_.size()) break;
        escapedSeen_ = escapedList_.size();
        for (uint32_t site : topCallSites_) enqueueInst(site);
    }
    recording_ = false;
    curFn_ = UINT32_MAX;
    stats_.millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void Solver::visit(uint32_t site) {
    ++stats_.transfers;
    const auto loc = insts_.locate(site);
    curFn_ = loc.fn;
    curSite_ = site;
    transfer(loc.fn, loc.block, loc.inst, mod_.functions[loc.fn].blocks[loc.block].instructions[loc.inst]);
    curFn_ = UINT32_MAX;
    curSite_ = UINT32_MAX;
}

const AVal& Solver::op(const il::Instruction& inst, size_t i) const {
    if (i >= inst.operands.size()) return kEmpty;
    const auto& values = fns_[curFn_].values;
    const il::ValueId v = inst.operands[i];
    return v < values.size() ? values[v] : kEmpty;
}

void Solver::widen(AVal& v) {
    if (v.objs.size() <= opts_.setCap) return;
    ++stats_.widenings;
    const ObjSet objs = std::move(v.objs);
    v.objs.clear();
    v.topObj = true;
    AVal gone;
    gone.objs = objs;
    escape(gone, internReason("widened: a points-to set grew past the cap"));
}

bool Solver::joinTracked(AVal& into, const AVal& from) {
    // A prediction keeps every object it knows, escaped or not.
    if (opts_.predict) return into.join(from);
    // An escaped object is one of the objects `topObj` stands for: a value
    // that takes it on holds `topObj` instead, so the sets that flow stay the
    // objects the program alone still knows about. Its methods are reached
    // from an unknown receiver by dispatch over the escaped objects
    // (methodCall).
    const bool any = std::any_of(from.objs.begin(), from.objs.end(), [&](ObjId o) { return objs_[o].escaped; });
    if (!any) return into.join(from);
    AVal kept;
    kept.prims = from.prims;
    kept.strKey = from.strKey;
    kept.topObj = true;
    kept.viaEscape = true;
    for (ObjId o : from.objs) {
        if (!objs_[o].escaped) kept.objs.push_back(o);
    }
    return into.join(kept);
}

bool Solver::mergeInto(AVal& into, const AVal& from, il::Type type, bool keepEscaped) {
    if (type == il::Type::Str && from.empty()) return false;
    if (type == il::Type::Str && from.onlyEnumKeys()) {
        // A typed copy of enumerated key names is still only those names.
        AVal keys = AVal::enumKeys();
        keys.strKey = from.strKey;
        return into.join(keys);
    }
    if (type == il::Type::Bool && !from.hasObjects() && (from.prims & ~kBool) == 0) {
        // A boolean keeps which booleans it can be.
        return into.addPrims(from.prims);
    }
    if (type != il::Type::Dynamic && type != il::Type::Void) {
        return into.addPrims(primsForType(type));
    }
    if (!(keepEscaped ? into.join(from) : joinTracked(into, from))) return false;
    if (into.objs.size() > opts_.setCap) widen(into);
    return true;
}

bool Solver::setResult(const il::Instruction& inst, const AVal& v, bool keepEscaped) {
    if (inst.result == il::kNoValue || inst.type == il::Type::Void) return false;
    auto& values = fns_[curFn_].values;
    if (inst.result >= values.size()) return false;
    if (!mergeInto(values[inst.result], v, inst.type, keepEscaped)) return false;
    enqueueUsers(curFn_, inst.result);
    return true;
}

void Solver::flowTarget(const il::BlockTarget& target, bool& changed) {
    const auto& fn = mod_.functions[curFn_];
    if (target.block >= fn.blocks.size()) return;
    markLive(curFn_, target.block);
    const auto& params = fn.blocks[target.block].params;
    auto& values = fns_[curFn_].values;
    for (size_t i = 0; i < params.size() && i < target.args.size(); ++i) {
        const il::ValueId from = target.args[i];
        if (from >= values.size() || params[i].id >= values.size()) continue;
        const AVal copy = values[from];
        if (mergeInto(values[params[i].id], copy, params[i].type)) {
            changed = true;
            enqueueUsers(curFn_, params[i].id);
        }
    }
}

AVal Solver::createFunction(const il::Instruction& inst, uint32_t site) {
    const uint32_t f = inst.calleeIndex;
    const ObjId o = alloc(site, kTagMain, ObjKind::Function);
    objs_[o].fn = f;
    if (!inst.operands.empty()) {
        if (objs_[o].env.join(op(inst, 0))) notify(o, kKeyEnv);
    }
    const auto& callee = mod_.functions[f];
    auto infoIt = siteInfo_.find(site);
    const bool extend = infoIt != siteInfo_.end() && infoIt->second.extendFollows;
    if (!extend && objs_[o].proto.empty()) {
        objs_[o].proto.addObj(builtinObj("Function.prototype"));
    }
    if ((callee.fnFlags & BRONZE_ABI_FN_FLAG_PROTOTYPE) != 0) {
        const ObjId p = alloc(site, kTagProto, ObjKind::Proto);
        objs_[p].fn = f;
        if (objs_[p].definite.empty()) {
            const uint32_t ctorKey = keys_.intern("constructor");
            std::vector<uint32_t> definite{ctorKey};
            if (infoIt != siteInfo_.end()) {
                for (uint32_t k : infoIt->second.definiteProtoKeys) definite.push_back(k);
            }
            std::sort(definite.begin(), definite.end());
            definite.erase(std::unique(definite.begin(), definite.end()), definite.end());
            objs_[p].definite = std::move(definite);
            if (!extend) objs_[p].proto.addObj(builtinObj("Object.prototype"));
        }
        // `prototype` is non-configurable: a read of it ends at the function.
        if (objs_[o].definite.empty()) objs_[o].definite.push_back(keys_.intern("prototype"));
        writeField(p, keys_.intern("constructor"), AVal::obj(o));
        writeField(o, keys_.intern("prototype"), AVal::obj(p));
    }
    return AVal::obj(o);
}

bool Solver::transfer(uint32_t f, uint32_t, uint32_t, const il::Instruction& inst) {
    using il::Op;
    (void)f;
    bool changed = false;
    auto result = [&](const AVal& v) {
        if (setResult(inst, v)) changed = true;
    };
    auto primResult = [&](uint16_t bits) { result(AVal::prim(bits)); };
    auto args = [&](size_t from) {
        CallArgs a;
        for (size_t i = from; i < inst.operands.size(); ++i) a.args.push_back(op(inst, i));
        return a;
    };
    switch (inst.op) {
        case Op::ConstF64: {
            const double d = inst.immF64;
            const bool i32 = std::isfinite(d) && d == std::trunc(d) && d >= -2147483648.0 &&
                             d <= 2147483647.0 && !(d == 0 && std::signbit(d));
            primResult(i32 ? kI32 : kF64);
            break;
        }
        case Op::ConstI32: primResult(kI32); break;
        case Op::ConstBool: primResult(inst.immI32 != 0 ? kTrue : kFalse); break;
        case Op::ConstUndefined: primResult(kUndef); break;
        case Op::ConstNull: primResult(kNull); break;
        case Op::ConstBigInt: primResult(kBigInt); break;
        case Op::Add: primResult(kNumber | kStr | kBigInt); break;
        case Op::ConcatBegin: case Op::ConcatAppend: case Op::ConcatEnd: case Op::ToStr:
            primResult(kStr);
            break;
        case Op::TypeOf: result(typeOfValue(op(inst, 0))); break;
        case Op::StrictEq: case Op::LooseEq: case Op::CmpEq: case Op::CmpNe:
            primResult(compareConst(inst.op, op(inst, 0), op(inst, 1)));
            break;
        case Op::Sub: case Op::Neg: case Op::Mul: case Op::Div: case Op::Mod: case Op::Pow:
        case Op::ToNumeric: case Op::NumericStep: case Op::ToInt32: case Op::BitAnd:
        case Op::BitOr: case Op::BitXor: case Op::MathImul: case Op::Shl: case Op::Shr:
        case Op::UShr: case Op::BitNot:
            primResult(kNumber | kBigInt);
            break;
        case Op::CmpLt: case Op::CmpGt: case Op::CmpLe: case Op::CmpGe:
        case Op::NumTruthy: case Op::RelLt:
        case Op::RelGt: case Op::RelLe: case Op::RelGe: case Op::IsNullish: case Op::IsNumber:
        case Op::IsDenseArray: case Op::InstanceOf: case Op::In: case Op::IterStep:
        case Op::MethodIcIs: case Op::AccessorIcIs: case Op::IsTypedArray: case Op::IsNumberArray: case Op::IsArray:
        case Op::IsPlainArray:
        case Op::ClosureIs: case Op::PrivateHas: case Op::IsShape:
            primResult(kBool);
            break;
        case Op::MathUnary: case Op::MathRandom: case Op::ElemGetTyped: case Op::LayoutEpoch:
            primResult(kNumber);
            break;
        case Op::CoroMode: primResult(kI32); break;
        case Op::Ret: {
            FnState& st = fns_[curFn_];
            const bool givesInstance = inst.operands.empty() || inst.operands[0] == thisParam(curFn_) ||
                                       st.superForeign.count(inst.operands[0]) != 0 || op(inst, 0).prims != 0;
            if (givesInstance && !st.newGivesInstance) {
                st.newGivesInstance = true;
                enqueueCallers(curFn_);
            }
            if (inst.operands.empty()) break;
            const il::Type retType = mod_.functions[curFn_].returnType == il::Type::Void
                                         ? il::Type::Dynamic
                                         : mod_.functions[curFn_].returnType;
            const il::ValueId rv = inst.operands[0];
            bool foreignChanged = false;
            if (rv != thisParam(curFn_)) {
                auto sup = st.superForeign.find(rv);
                foreignChanged = mergeInto(st.foreignRet, sup != st.superForeign.end() ? sup->second : op(inst, 0),
                                           retType);
            }
            const bool retChanged = mergeInto(st.ret, op(inst, 0), retType);
            if (retChanged || foreignChanged) enqueueCallers(curFn_);
            if (retChanged && st.retEscapes) escape(st.ret, internReason("returned to unknown code"));
            break;
        }
        case Op::Throw:
            escape(op(inst, 0), internReason("thrown"));
            break;
        case Op::ExcTake: result(AVal::top()); break;
        case Op::Jump: flowTarget(inst.target, changed); break;
        case Op::Branch: {
            // Only the edges the condition allows. A condition with no value
            // yet takes both: an empty set here is a value the lattice has not
            // modelled (a call that has not returned), not dead code.
            const AVal& cond = op(inst, 0);
            if (cond.empty() || cond.mayBeTruthy()) flowTarget(inst.target, changed);
            if (cond.empty() || cond.mayBeFalsy()) flowTarget(inst.elseTarget, changed);
            break;
        }
        case Op::Call: result(directCall(inst)); break;
        case Op::Box:
            if (inst.operands.empty()) result(AVal::str(inst.keyIndex));
            else result(op(inst, 0));
            break;
        case Op::Unbox: {
            // A boolean unboxed is the booleans it was.
            const AVal& v = op(inst, 0);
            const bool bools = inst.type == il::Type::Bool && !v.hasObjects() && (v.prims & ~kBool) == 0;
            if (bools && v.prims != 0) primResult(v.prims);
            else if (!bools || !v.empty()) primResult(primsForType(inst.type));
            break;
        }
        case Op::PropGet: {
            // Only the receivers this site has not read yet, unless the heap
            // under the ones it has read changed: the result only grows.
            const AVal& recv = op(inst, 0);
            ObjSet& seen = readRecv_[curSite_];
            AVal fresh = recv;
            fresh.objs = unreadReceivers(recv.objs, seen);
            seen = recv.objs;
            result(readProp(fresh, inst.keyIndex));
            break;
        }
        case Op::SuperGet: {
            AVal out;
            const AVal& self = op(inst, 1);
            const AVal& proto = op(inst, 0);
            if (proto.topObj) out.join(AVal::top());
            for (ObjId o : proto.objs) readKey(o, inst.keyIndex, self, out, 0);
            result(out);
            break;
        }
        case Op::SuperSet: {
            const AVal& proto = op(inst, 0);
            for (ObjId o : proto.objs) callSetters(o, inst.keyIndex, op(inst, 1), op(inst, 2), 0);
            writeProp(op(inst, 1), inst.keyIndex, op(inst, 2), true);
            break;
        }
        case Op::PropSet:
            writeProp(op(inst, 0), inst.keyIndex, op(inst, 1), il::isDefineWrite(inst));
            break;
        case Op::ElemGet: {
            AVal out;
            readElem(op(inst, 0), op(inst, 1), out);
            result(out);
            break;
        }
        case Op::ElemSet: writeElem(op(inst, 0), op(inst, 1), op(inst, 2)); break;
        case Op::ElemSetTyped:
            for (ObjId o : op(inst, 0).objs) writeElems(o, AVal::prim(kNumber));
            break;
        case Op::DynamicCall: {
            CallArgs a = args(2);
            result(callValue(op(inst, 0), op(inst, 1), a));
            break;
        }
        case Op::DynamicCallSpread: {
            const AVal spread = elemsOf(op(inst, 2));
            CallArgs a;
            a.spread = &spread;
            result(callValue(op(inst, 0), op(inst, 1), a));
            break;
        }
        case Op::Construct: {
            CallArgs a = args(1);
            result(construct(op(inst, 0), a));
            break;
        }
        case Op::ConstructSpread: {
            const AVal spread = elemsOf(op(inst, 1));
            CallArgs a;
            a.spread = &spread;
            result(construct(op(inst, 0), a));
            break;
        }
        case Op::MethodCall: {
            CallArgs a = args(1);
            result(methodCall(inst, op(inst, 0), a));
            break;
        }
        case Op::MethodCallSpread: {
            const AVal spread = elemsOf(op(inst, 1));
            CallArgs a;
            a.spread = &spread;
            result(methodCall(inst, op(inst, 0), a));
            break;
        }
        case Op::SuperCall: case Op::SuperCallSpread: {
            CallArgs a;
            AVal spread;
            if (inst.op == Op::SuperCall) {
                a = args(2);
            } else {
                spread = elemsOf(op(inst, 2));
                a.spread = &spread;
            }
            AVal foreign;
            // The result is the receiver from here on, which keeps its
            // escaped objects as the `this` parameter does.
            setResult(inst, superCall(op(inst, 0), op(inst, 1), a, foreign), true);
            if (inst.operands[1] == thisParam(curFn_) && fns_[curFn_].superForeign[inst.result].join(foreign)) {
                enqueueUsers(curFn_, inst.result);
            }
            break;
        }
        case Op::CoroStart: result(coroStart(inst)); break;
        case Op::CoroSuspend:
            escape(op(inst, 0), internReason("yielded or awaited"));
            result(AVal::top());
            break;
        case Op::CreateObject: {
            const ObjId o = alloc(curSite_, kTagMain, ObjKind::Plain);
            auto it = siteInfo_.find(curSite_);
            if ((it == siteInfo_.end() || !it->second.literalProto) && objs_[o].proto.empty()) {
                objs_[o].proto.addObj(builtinObj("Object.prototype"));
            }
            result(AVal::obj(o));
            break;
        }
        case Op::CreateArray: {
            const ObjId o = alloc(curSite_, kTagMain, ObjKind::Array);
            if (objs_[o].proto.empty()) objs_[o].proto.addObj(builtinObj("Array.prototype"));
            result(AVal::obj(o));
            break;
        }
        case Op::TemplateCached: case Op::TemplateObject: {
            AVal v = newArray(kTagMain, AVal::prim(kStr));
            if (inst.op == Op::TemplateCached) v.addPrims(kUndef);
            result(v);
            break;
        }
        case Op::CreateFunction: result(createFunction(inst, curSite_)); break;
        case Op::FunctionRef: {
            auto it = fnSingletons_.find(inst.calleeIndex);
            ObjId o;
            if (it == fnSingletons_.end()) {
                o = alloc(curSite_, kTagMain, ObjKind::Function);
                objs_[o].fn = inst.calleeIndex;
                objs_[o].proto.addObj(builtinObj("Function.prototype"));
                fnSingletons_.emplace(inst.calleeIndex, o);
            } else {
                o = it->second;
            }
            result(AVal::obj(o));
            break;
        }
        case Op::EnvCreate: {
            const ObjId o = alloc(curSite_, kTagMain, ObjKind::Env);
            if (!inst.operands.empty() && objs_[o].proto.join(op(inst, 0))) notify(o, kKeyProto);
            result(AVal::obj(o));
            break;
        }
        case Op::EnvGet: case Op::EnvGetTdz:
            result(envGet(op(inst, 0), inst.envDepth, inst.envIndex));
            break;
        case Op::EnvSet:
            envSet(op(inst, 0), inst.envDepth, inst.envIndex, op(inst, 1));
            break;
        case Op::EnvInitTdz: break;
        case Op::ModuleEnvSet:
            if (moduleEnv_.join(op(inst, 0))) {
                for (uint32_t r : moduleEnvReaders_) enqueueInst(r);
            }
            break;
        case Op::ModuleEnvGet:
            if (std::find(moduleEnvReaders_.begin(), moduleEnvReaders_.end(), curSite_) ==
                moduleEnvReaders_.end()) {
                moduleEnvReaders_.push_back(curSite_);
            }
            result(moduleEnv_);
            break;
        case Op::ModuleNamespace:
            // `import * as ns`: the namespace is the object lowering built
            // with one getter per export, given the exotic namespace
            // behaviour (no writes, no prototype). Reads through it are its
            // getters, so it is that object.
            result(op(inst, 0));
            break;
        case Op::ObjectKeys: case Op::ForInKeys:
            result(newArray(kTagMain, AVal::enumKeys()));
            break;
        case Op::MethodDef:
            writeProp(op(inst, 0), inst.keyIndex, op(inst, 1), true);
            break;
        case Op::MethodDefComputed: writeElem(op(inst, 0), op(inst, 1), op(inst, 2)); break;
        case Op::AccessorDef:
            defineAccessor(op(inst, 0), inst.keyIndex, op(inst, 1), op(inst, 2));
            break;
        case Op::AccessorDefComputed: {
            const AVal& key = op(inst, 1);
            if (key.isStrConst()) {
                defineAccessor(op(inst, 0), key.strKey, op(inst, 2), op(inst, 3));
            } else {
                const uint32_t why = internReason("accessor under a computed key");
                escape(op(inst, 2), why);
                escape(op(inst, 3), why);
                for (ObjId o : op(inst, 0).objs) markFlag(o, &AObj::anyKey);
            }
            break;
        }
        case Op::DefineOwnAttr:
            if ((static_cast<uint32_t>(inst.immI32) & BRONZE_ABI_DESC_HAS_VALUE) != 0) {
                writeProp(op(inst, 0), inst.keyIndex, op(inst, 1), true);
            }
            break;
        case Op::ClassExtend: {
            const AVal& derived = op(inst, 0);
            const AVal& base = op(inst, 1);
            AVal ctorProto = base;
            if ((base.prims & kNull) != 0) ctorProto.addObj(builtinObj("Function.prototype"));
            ctorProto.prims = 0;
            const AVal baseProto = readProp(base, keys_.intern("prototype"));
            AVal instProto = baseProto;
            instProto.prims = 0;
            const uint32_t protoKey = keys_.intern("prototype");
            for (ObjId d : derived.objs) {
                if (objs_[d].proto.join(ctorProto)) notify(d, kKeyProto);
                auto it = objs_[d].fields.find(protoKey);
                if (it == objs_[d].fields.end()) continue;
                const ObjSet protos = it->second.objs;
                for (ObjId p : protos) {
                    if (objs_[p].proto.join(instProto)) notify(p, kKeyProto);
                }
            }
            break;
        }
        case Op::ObjectLitProto: {
            const AVal& v = op(inst, 1);
            AVal proto;
            proto.objs = v.objs;
            proto.topObj = v.topObj;
            if ((v.prims & ~kNull) != 0) proto.addObj(builtinObj("Object.prototype"));
            for (ObjId o : op(inst, 0).objs) {
                if (objs_[o].proto.join(proto)) notify(o, kKeyProto);
            }
            break;
        }
        case Op::GetNewTarget: result(AVal::top()); break;
        case Op::ImportMeta: result(AVal::obj(alloc(curSite_, kTagMain, ObjKind::Other))); break;
        case Op::PropDelete: case Op::ElemDelete:
            for (ObjId o : op(inst, 0).objs) markFlag(o, &AObj::deleted);
            primResult(kBool);
            break;
        case Op::GlobalGet: result(globalValue(inst.keyIndex)); break;
        case Op::ResolveName:
            // A name no binding, provided global or host global declares
            // (lower_unresolved.cpp). It is a property of the global object
            // only if something the program does not show made it one, which
            // a prediction assumes nothing did: `typeof __THREE_DEVTOOLS__`
            // is then "undefined" and the code it guards does not run.
            result(opts_.predict ? AVal::prim(kUndef) : globalValue(inst.keyIndex));
            break;
        case Op::ImmutableAssign: case Op::PrivateMisuse: primResult(kUndef); break;
        case Op::PinGuard: case Op::CensusRecord: case Op::FeedbackRecord: case Op::IterClose: case Op::KeepAlive:
        case Op::Print: case Op::PrintErr: case Op::PrintSpread: case Op::PrintSpreadErr:
            break;
        case Op::PrivateNew: result(AVal::obj(alloc(curSite_, kTagMain, ObjKind::Other))); break;
        case Op::PrivateGet: {
            AVal out;
            const AVal& table = op(inst, 0);
            if (table.topObj) out.join(AVal::top());
            for (ObjId t : table.objs) {
                depend(t, kKeyElems);
                out.join(objs_[t].elems);
            }
            result(out);
            break;
        }
        case Op::PrivateAdd: case Op::PrivateSet:
            for (ObjId t : op(inst, 0).objs) writeElems(t, op(inst, 2));
            if (op(inst, 0).topObj) escape(op(inst, 2), internReason("stored in an unknown private table"));
            break;
        case Op::IterOpen: {
            const ObjId it = alloc(curSite_, kTagIter, ObjKind::Iterator);
            writeElems(it, iterValues(op(inst, 0)));
            result(AVal::obj(it));
            break;
        }
        case Op::IterValue: result(elemsOf(op(inst, 0))); break;
        case Op::IterRest: result(newArray(kTagMain, elemsOf(op(inst, 0)))); break;
        case Op::PatternCheck: result(op(inst, 0)); break;
        case Op::ArrayAppend:
            for (ObjId o : op(inst, 0).objs) writeElems(o, op(inst, 1));
            break;
        case Op::ArrayAppendHole:
            for (ObjId o : op(inst, 0).objs) writeElems(o, AVal::prim(kUndef));
            break;
        case Op::ArraySpread: {
            const AVal vals = iterValues(op(inst, 1));
            for (ObjId o : op(inst, 0).objs) writeElems(o, vals);
            break;
        }
        case Op::ObjectSpread: {
            AVal vals;
            const AVal& src = op(inst, 1);
            if (src.topObj) vals.join(AVal::top());
            readAll(src.objs, vals);
            for (ObjId o : op(inst, 0).objs) {
                writeElems(o, vals);
                markFlag(o, &AObj::anyKey);
            }
            break;
        }
        case Op::ObjectRest: {
            AVal vals;
            const AVal& src = op(inst, 0);
            if (src.topObj) vals.join(AVal::top());
            readAll(src.objs, vals);
            const ObjId o = alloc(curSite_, kTagMain, ObjKind::Plain);
            if (objs_[o].proto.empty()) objs_[o].proto.addObj(builtinObj("Object.prototype"));
            writeElems(o, vals);
            markFlag(o, &AObj::anyKey);
            result(AVal::obj(o));
            break;
        }
        case Op::MethodSlotFn: {
            AVal v = readProp(op(inst, 0), methodKeyOfIc(inst.icIndex));
            v.addPrims(kUndef);
            result(v);
            break;
        }
        case Op::ClosureEnv: {
            AVal out;
            const AVal& fv = op(inst, 0);
            if (fv.topObj) out.join(AVal::top());
            for (ObjId o : fv.objs) {
                depend(o, kKeyEnv);
                out.join(objs_[o].env);
            }
            result(out);
            break;
        }
        default: {
            // The open world for anything not modelled: every operand escapes
            // and the result is unknown.
            const uint32_t why = internReason(std::string("operand of unmodelled ") + il::opName(inst.op));
            for (size_t i = 0; i < inst.operands.size(); ++i) escape(op(inst, i), why);
            result(AVal::top());
            break;
        }
    }
    return changed;
}

uint32_t Solver::methodKeyOfIc(uint32_t ic) const {
    auto it = methodKeyByIc_.find(ic);
    return it == methodKeyByIc_.end() ? 0 : it->second;
}

std::string Solver::describe(ObjId id) const {
    const AObj& o = objs_[id];
    auto fnName = [&](uint32_t f) {
        return f < mod_.functions.size() ? mod_.functions[f].name : std::string("?");
    };
    auto at = [&]() {
        if (o.site == UINT32_MAX) return std::string();
        const auto loc = insts_.locate(o.site);
        return "@" + fnName(loc.fn) + ":b" + std::to_string(loc.block);
    };
    switch (o.kind) {
        case ObjKind::Function: return "fn " + fnName(o.fn);
        case ObjKind::Proto: return fnName(o.fn) + ".prototype";
        case ObjKind::Instance: return "new " + fnName(o.fn);
        case ObjKind::Plain: return "{}" + at();
        case ObjKind::Array: return "[]" + at();
        case ObjKind::Env: return o.site == UINT32_MAX ? std::string("env") : "env" + at();
        case ObjKind::Builtin: return o.builtin < catalog_.size() ? catalog_.def(o.builtin).path : "builtin";
        case ObjKind::Bound: return "bound" + at();
        case ObjKind::Iterator: return "iter" + at();
        default: {
            const std::string name =
                o.builtin < catalog_.size() ? catalog_.def(o.builtin).path : objKindName(o.kind);
            return "new " + name + at();
        }
    }
}

}  // namespace bronze::wpo
