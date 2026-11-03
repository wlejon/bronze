// Classifying every property and method site by what the analysis knows
// about its receivers (census.h). Printing is census_format.cpp.

#include "opt/wpo/census.h"

#include <algorithm>
#include <map>

namespace bronze::wpo {

const char* propClassName(PropClass c) {
    switch (c) {
        case PropClass::Unreached: return "unreached";
        case PropClass::Unknown: return "unknown-recv";
        case PropClass::Primitive: return "primitive";
        case PropClass::SingleShape: return "single-shape";
        case PropClass::SameSlot: return "same-slot";
        case PropClass::SameSlotEscaped: return "same-slot-escaped";
        case PropClass::SlotDiffers: return "slot-differs";
        case PropClass::NotInPrefix: return "not-in-prefix";
        case PropClass::Inherited: return "inherited";
        case PropClass::Accessor: return "accessor";
        case PropClass::BuiltinRecv: return "builtin-recv";
        case PropClass::FnStatic: return "fn-static";
        case PropClass::Absent: return "absent";
        case PropClass::Mixed: return "mixed";
        case PropClass::IndexArray: return "index-array";
        case PropClass::IndexTyped: return "index-typed";
        case PropClass::IndexOther: return "index-other";
    }
    return "?";
}

const char* callClassName(CallClass c) {
    switch (c) {
        case CallClass::Unreached: return "unreached";
        case CallClass::Unknown: return "unknown-recv";
        case CallClass::UnknownTarget: return "unknown-target";
        case CallClass::Single: return "single";
        case CallClass::Multi: return "multi";
        case CallClass::Builtin: return "builtin";
        case CallClass::Mixed: return "mixed";
        case CallClass::None: return "none";
    }
    return "?";
}

namespace {

// Where one receiver object holds a key.
enum class Where : uint8_t { OwnSlot, OwnUnknown, Inherited, Accessor, Builtin, FnStatic, Absent, UnknownProto };

struct Resolution {
    Where where = Where::Absent;
    int slot = -1;
    ObjId holder = kNoObj;
};

class Classifier {
public:
    Classifier(Solver& solver, const ShapeAnalysis& shapes) : s_(solver), shapes_(shapes) {}

    std::vector<SiteFact> run() {
        std::vector<SiteFact> facts;
        const auto& mod = s_.module();
        for (uint32_t f = 0; f < mod.functions.size(); ++f) {
            const auto& fn = mod.functions[f];
            if (fn.isExternal) continue;
            defs_.clear();
            for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
                const auto& insts = fn.blocks[b].instructions;
                for (uint32_t i = 0; i < insts.size(); ++i) {
                    if (insts[i].result != il::kNoValue) defs_[insts[i].result] = {b, i};
                }
            }
            for (uint32_t b = 0; b < fn.blocks.size(); ++b) {
                const auto& insts = fn.blocks[b].instructions;
                for (uint32_t i = 0; i < insts.size(); ++i) {
                    const auto& inst = insts[i];
                    const bool prop = inst.op == il::Op::PropGet || inst.op == il::Op::PropSet;
                    const bool call = inst.op == il::Op::MethodCall || inst.op == il::Op::MethodCallSpread;
                    if (!prop && !call) continue;
                    SiteFact fact;
                    fact.fn = f;
                    fact.block = b;
                    fact.inst = i;
                    fact.op = inst.op;
                    fact.key = inst.keyIndex;
                    fact.isCall = call;
                    fact.stamped = inst.staticSlot != il::Instruction::kNoStaticSlot;
                    fact.stampSlot = fact.stamped ? static_cast<int>(inst.staticSlot) : -1;
                    fact.guessed = inst.staticSlotGuessed;
                    fact.direct = inst.directTarget != il::Instruction::kNoDirectTarget;
                    fact.directTarget = inst.directTarget;
                    fact.inlined = inst.inlineFrame != 0;
                    fact.mono = inst.icMonomorphic;
                    if (s_.reachable(f) && s_.blockLive(f, b)) {
                        if (prop) classifyProp(inst, fact);
                        else classifyCall(inst, fact);
                    }
                    facts.push_back(std::move(fact));
                }
            }
        }
        return facts;
    }

private:
    const AVal& recvOf(const il::Instruction& inst) {
        static const AVal kEmpty;
        if (inst.operands.empty()) return kEmpty;
        return s_.value(curFn_, inst.operands[0]);
    }

    // Where `key` lives for a read on `id`, following the chain.
    Resolution resolve(ObjId id, uint32_t key, bool forWrite) {
        Resolution r;
        const AObj& o = s_.object(id);
        switch (o.kind) {
            case ObjKind::Builtin: case ObjKind::Array: case ObjKind::TypedArray:
            case ObjKind::Collection: case ObjKind::Iterator: case ObjKind::Other:
            case ObjKind::Bound: case ObjKind::Env:
                r.where = Where::Builtin;
                return r;
            case ObjKind::Function:
                r.where = Where::FnStatic;
                return r;
            default:
                break;
        }
        const ObjShape& sh = shapes_.shape(id);
        if (o.accessors.count(key) != 0) {
            r.where = Where::Accessor;
            r.holder = id;
            return r;
        }
        const int slot = sh.slotOf(key);
        if (slot >= 0 && shapes_.slotsHold(id)) {
            r.where = Where::OwnSlot;
            r.slot = slot;
            r.holder = id;
            return r;
        }
        // An escaped object may have been given any key by code the program
        // does not show.
        const bool escaped = o.escaped && !s_.options().predict;
        if (o.fields.count(key) != 0 || slot >= 0 || o.anyKey || escaped || forWrite) {
            r.where = Where::OwnUnknown;
            r.holder = id;
            return r;
        }
        // Up the chain: the first object that has the key.
        std::vector<ObjId> frontier(o.proto.objs.begin(), o.proto.objs.end());
        if (o.proto.topObj) {
            r.where = Where::UnknownProto;
            r.holder = id;
            return r;
        }
        std::vector<ObjId> seen;
        Resolution found;
        found.where = Where::Absent;
        while (!frontier.empty()) {
            const ObjId p = frontier.back();
            frontier.pop_back();
            if (std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
            seen.push_back(p);
            const AObj& po = s_.object(p);
            if (po.accessors.count(key) != 0) {
                found.where = Where::Accessor;
                found.holder = p;
                return found;
            }
            bool has = po.fields.count(key) != 0;
            if (!has && po.kind == ObjKind::Builtin) {
                const AVal v = s_.queryProp(AVal::obj(p), key);
                has = !(v.prims == kUndef && !v.hasObjects());
            }
            if (has) {
                if (found.where == Where::Inherited && found.holder != p) found.holder = kNoObj;
                else if (found.where == Where::Absent) found.holder = p;
                found.where = Where::Inherited;
                continue;
            }
            for (ObjId q : po.proto.objs) frontier.push_back(q);
        }
        return found;
    }

    std::string causeOf(il::ValueId v, int depth = 0) {
        const auto& fn = s_.module().functions[curFn_];
        if (v < fn.params.size()) {
            std::string c = "param " + fn.params[v].name;
            if (s_.calledWithUnknown(curFn_)) c += " of a function unknown code calls";
            return c;
        }
        auto it = defs_.find(v);
        if (it == defs_.end()) return "block param";
        const auto& inst = fn.blocks[it->second.first].instructions[it->second.second];
        const std::string keyName = s_.keys().name(inst.keyIndex);
        switch (inst.op) {
            case il::Op::PropGet: {
                const AVal& recv = recvOf(inst);
                for (ObjId o : recv.objs) {
                    const AObj& a = s_.object(o);
                    if (a.escaped) {
                        return "field ." + keyName + " of escaped " + s_.describe(o) + " (" +
                               (a.escapeReason == UINT32_MAX ? std::string("?") : s_.reason(a.escapeReason)) + ")";
                    }
                }
                if (recv.topObj) {
                    if (depth < 2) return "field ." + keyName + " of [" + causeOf(inst.operands[0], depth + 1) + "]";
                    return "field ." + keyName + " of unknown";
                }
                for (ObjId o : recv.objs) {
                    const AObj& a = s_.object(o);
                    auto f = a.fields.find(inst.keyIndex);
                    if (f != a.fields.end() && f->second.topObj) {
                        return "field ." + keyName + " of " + s_.describe(o) + " holds unknown";
                    }
                    if (a.proto.topObj) return "field ." + keyName + ": unknown prototype of " + s_.describe(o);
                }
                return "field ." + keyName + " (via accessor or chain)";
            }
            case il::Op::MethodCall: return "result of ." + keyName + "()";
            case il::Op::DynamicCall: return "result of a dynamic call";
            case il::Op::Call: return "result of " + s_.module().functions[inst.calleeIndex].name + "()";
            case il::Op::Construct: return "result of new";
            case il::Op::GlobalGet: case il::Op::ResolveName: return "global " + keyName;
            case il::Op::ElemGet: return "element read";
            case il::Op::EnvGet: case il::Op::EnvGetTdz: return "environment slot";
            case il::Op::ExcTake: return "caught exception";
            case il::Op::IterValue: return "iterated value";
            case il::Op::CoroSuspend: return "await / yield result";
            default: return std::string("result of ") + il::opName(inst.op);
        }
    }

    std::string escapeWhyOf(const AVal& recv) {
        for (ObjId o : recv.objs) {
            const AObj& a = s_.object(o);
            if (a.escaped) {
                return s_.describe(o) + ": " +
                       (a.escapeReason == UINT32_MAX ? std::string("?") : s_.reason(a.escapeReason));
            }
        }
        return {};
    }

    void summarize(const AVal& recv, SiteFact& fact) {
        std::string d;
        size_t n = 0;
        for (ObjId o : recv.objs) {
            if (n++ == 4) {
                d += ", +" + std::to_string(recv.objs.size() - 4);
                break;
            }
            if (!d.empty()) d += ", ";
            d += s_.describe(o);
        }
        if (recv.topObj) d += d.empty() ? "unknown" : ", unknown";
        fact.detail = d;
    }

    void classifyProp(const il::Instruction& inst, SiteFact& fact) {
        curFn_ = fact.fn;
        const AVal& recv = recvOf(inst);
        fact.receivers = recv.objs.size();
        summarize(recv, fact);
        // A prediction is about the receivers the program shows; the unknown
        // rest is what the guard in front of the fast path is for.
        fact.partial = recv.topObj && !recv.objs.empty();
        if (recv.topObj && !(s_.options().predict && fact.partial)) {
            fact.prop = PropClass::Unknown;
            fact.cause = (recv.viaEscape ? "escaped objects, " : "") + causeOf(inst.operands[0]);
            return;
        }
        if (recv.objs.empty()) {
            fact.prop = (recv.prims & ~(kUndef | kNull)) != 0 ? PropClass::Primitive : PropClass::Unreached;
            return;
        }
        if (s_.keys().isIndex(inst.keyIndex)) {
            bool allArray = true, allTyped = true;
            for (ObjId o : recv.objs) {
                const ObjKind k = s_.object(o).kind;
                if (k != ObjKind::Array) allArray = false;
                if (k != ObjKind::TypedArray) allTyped = false;
            }
            fact.prop = allArray ? PropClass::IndexArray : allTyped ? PropClass::IndexTyped : PropClass::IndexOther;
            return;
        }
        const bool write = inst.op == il::Op::PropSet;
        std::vector<Resolution> rs;
        for (ObjId o : recv.objs) rs.push_back(resolve(o, inst.keyIndex, write));
        auto all = [&](Where w) {
            return std::all_of(rs.begin(), rs.end(), [&](const Resolution& r) { return r.where == w; });
        };
        auto any = [&](Where w) {
            return std::any_of(rs.begin(), rs.end(), [&](const Resolution& r) { return r.where == w; });
        };
        fact.ownOrAbsent = any(Where::Absent) && std::all_of(rs.begin(), rs.end(), [](const Resolution& r) {
            return r.where == Where::Absent || r.where == Where::OwnSlot || r.where == Where::OwnUnknown;
        });
        if (all(Where::OwnSlot)) {
            const int slot = rs[0].slot;
            const bool same = std::all_of(rs.begin(), rs.end(), [&](const Resolution& r) { return r.slot == slot; });
            if (!same) {
                fact.prop = PropClass::SlotDiffers;
                return;
            }
            fact.slot = slot;
            bool single = true, escaped = false;
            const ObjShape& first = shapes_.shape(recv.objs[0]);
            const AVal& firstProto = s_.object(recv.objs[0]).proto;
            for (ObjId o : recv.objs) {
                if (s_.object(o).escaped) escaped = true;
                const ObjShape& sh = shapes_.shape(o);
                if (!shapes_.singleShape(o) || !(sh.seq == first.seq) || !(s_.object(o).proto == firstProto)) {
                    single = false;
                }
            }
            if (single) fact.prop = PropClass::SingleShape;
            else if (escaped) {
                fact.prop = PropClass::SameSlotEscaped;
                fact.escapeWhy = escapeWhyOf(recv);
            } else {
                fact.prop = PropClass::SameSlot;
            }
            return;
        }
        if (all(Where::Builtin)) { fact.prop = PropClass::BuiltinRecv; return; }
        if (all(Where::FnStatic)) { fact.prop = PropClass::FnStatic; return; }
        if (any(Where::Accessor)) { fact.prop = PropClass::Accessor; return; }
        if (all(Where::Absent)) { fact.prop = PropClass::Absent; return; }
        if (std::all_of(rs.begin(), rs.end(), [](const Resolution& r) {
                return r.where == Where::OwnSlot || r.where == Where::OwnUnknown;
            })) {
            fact.prop = PropClass::NotInPrefix;
            for (size_t k = 0; k < rs.size(); ++k) {
                if (rs[k].where == Where::OwnUnknown) {
                    const ObjShape& sh = shapes_.shape(recv.objs[k]);
                    const AObj& ro = s_.object(recv.objs[k]);
                    std::string why = sh.why.empty() ? "added after construction" : sh.why;
                    if (ro.escaped && sh.slotOf(inst.keyIndex) < 0 && ro.fields.count(inst.keyIndex) == 0) {
                        why = "escaped, " + (ro.escapeReason == UINT32_MAX ? std::string("?") : s_.reason(ro.escapeReason));
                    }
                    fact.cause = s_.describe(recv.objs[k]) + ": " + why;
                    break;
                }
            }
            return;
        }
        if (std::all_of(rs.begin(), rs.end(), [](const Resolution& r) {
                return r.where == Where::Inherited || r.where == Where::Absent;
            })) {
            fact.prop = PropClass::Inherited;
            std::vector<ObjId> holders;
            for (const auto& r : rs) {
                if (r.where == Where::Inherited &&
                    std::find(holders.begin(), holders.end(), r.holder) == holders.end()) {
                    holders.push_back(r.holder);
                }
            }
            fact.holders = holders.size();
            return;
        }
        fact.prop = PropClass::Mixed;
        for (const auto& r : rs) {
            if (r.where == Where::UnknownProto) {
                fact.cause = "unknown prototype of " + s_.describe(r.holder);
                break;
            }
        }
    }

    void classifyCall(const il::Instruction& inst, SiteFact& fact) {
        curFn_ = fact.fn;
        const AVal& recv = recvOf(inst);
        fact.receivers = recv.objs.size();
        summarize(recv, fact);
        if (recv.empty()) {
            fact.call = CallClass::Unreached;
            return;
        }
        fact.partial = recv.topObj && !recv.objs.empty();
        if (recv.topObj && !(s_.options().predict && fact.partial)) {
            fact.call = CallClass::Unknown;
            fact.cause = (recv.viaEscape ? "escaped objects, " : "") + causeOf(inst.operands[0]);
            return;
        }
        AVal known = recv;
        known.topObj = false;
        const AVal fnv = s_.queryProp(known, inst.keyIndex);
        const auto c = s_.callees(fnv);
        fact.targets = c.fns;
        if (c.unknown) {
            fact.call = CallClass::UnknownTarget;
            fact.escapeWhy = escapeWhyOf(recv);
            for (ObjId o : recv.objs) {
                if (!s_.object(o).escaped && s_.object(o).proto.topObj) {
                    fact.cause = "unknown prototype of " + s_.describe(o);
                    break;
                }
            }
            if (fact.cause.empty() && fact.escapeWhy.empty()) {
                fact.cause = "method value holds unknown (a field or builtin member)";
            }
            return;
        }
        if (c.fns.empty()) {
            fact.call = c.builtin ? CallClass::Builtin : CallClass::None;
            return;
        }
        if (c.builtin) {
            fact.call = CallClass::Mixed;
            return;
        }
        fact.call = c.fns.size() == 1 ? CallClass::Single : CallClass::Multi;
    }

    Solver& s_;
    const ShapeAnalysis& shapes_;
    uint32_t curFn_ = 0;
    std::map<il::ValueId, std::pair<uint32_t, uint32_t>> defs_;
};

}  // namespace

std::vector<SiteFact> classifySites(Solver& solver, const ShapeAnalysis& shapes) {
    return Classifier(solver, shapes).run();
}

}  // namespace bronze::wpo
