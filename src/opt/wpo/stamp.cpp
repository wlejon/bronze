#include "opt/wpo/stamp.h"

#include <algorithm>
#include <set>
#include <tuple>

#include "abi/bronze_abi_layout.h"
#include "il/key.h"

namespace bronze::wpo {

namespace {

// One slot of a predicted key order: the key, or no data key at all for
// either word of an accessor.
constexpr uint32_t kAccessorSlot = UINT32_MAX;

std::vector<uint32_t> slotsOf(const ObjShape& sh) {
    std::vector<uint32_t> out;
    for (const SeqEntry& e : sh.seq) {
        if (e.accessor) {
            out.push_back(kAccessorSlot);
            out.push_back(kAccessorSlot);
        } else {
            out.push_back(e.key);
        }
    }
    return out;
}

// The families the runtime may stamp a shape with, as class_family.cpp picks
// one: the most fields among those whose whole field list begins the shape,
// every one of a tie. A shape the prediction knows only a prefix of may go
// on to match a longer family, which is then a possible stamp too. Empty
// when no family begins the predicted keys.
std::vector<uint32_t> possibleStamps(const il::Module& mod, const std::vector<uint32_t>& slots, bool exact) {
    auto begins = [&](const std::vector<il::Module::ClassFamilyField>& fields) {
        const size_t n = std::min(fields.size(), slots.size());
        for (size_t i = 0; i < n; ++i) {
            if (fields[i].keyIndex != slots[i]) return false;
        }
        return true;
    };
    size_t best = 0;
    bool any = false;
    for (const auto& fam : mod.classFamilies) {
        if (fam.fields.size() <= slots.size() && begins(fam.fields)) {
            best = any ? std::max(best, fam.fields.size()) : fam.fields.size();
            any = true;
        }
    }
    std::vector<uint32_t> out;
    if (!any) return out;
    for (uint32_t c = 0; c < mod.classFamilies.size(); ++c) {
        const auto& fields = mod.classFamilies[c].fields;
        if (!begins(fields)) continue;
        if (fields.size() == best || (!exact && fields.size() > slots.size())) out.push_back(c);
    }
    return out;
}

// Instructions a transition run covers (lower/transition_runs.h): their
// writes add keys, and the run's own guard answers them.
std::vector<std::vector<std::vector<uint8_t>>> runMembers(const il::Module& mod) {
    std::vector<std::vector<std::vector<uint8_t>>> out(mod.functions.size());
    for (size_t f = 0; f < mod.functions.size(); ++f) {
        const auto& fn = mod.functions[f];
        out[f].resize(fn.blocks.size());
        for (size_t b = 0; b < fn.blocks.size(); ++b) {
            const auto& insts = fn.blocks[b].instructions;
            out[f][b].assign(insts.size(), 0);
            for (size_t i = 0; i < insts.size(); ++i) {
                if (insts[i].transitionRunCell == il::Instruction::kNoTransitionRun) continue;
                for (size_t k = i; k < insts.size() && k < i + insts[i].transitionRunLength; ++k) out[f][b][k] = 1;
            }
        }
    }
    return out;
}

// A direct target for a method call the prediction gives one callee, on the
// terms `DirectMethodTable::resolve` sets one (direct_method_table.cpp): a
// callee whose parameters a fixed operand list can fill.
//
// The method cache's direct entry stands for "this shape's lookup of the key
// gives this code", which holds for a method a prototype holds and not for a
// function held in the receiver's own slot: two objects of one shape can hold
// different functions there. So the target must be a function the program
// stores only on prototypes, and the receivers must hold no own property of
// the key; anything else is an own-slot call (own_callee_calls.h).
struct OwnHeld {
    std::vector<uint8_t> fn;    // an IL function some non-prototype object holds
};

OwnHeld ownHeld(const Solver& s) {
    const il::Module& mod = s.module();
    OwnHeld out;
    out.fn.assign(mod.functions.size(), 0);
    auto hold = [&](const AVal& v) {
        for (ObjId o : v.objs) {
            const AObj& f = s.object(o);
            if (f.kind == ObjKind::Function && f.fn < out.fn.size()) out.fn[f.fn] = 1;
        }
    };
    for (ObjId id = 0; id < s.objectCount(); ++id) {
        const AObj& o = s.object(id);
        if (o.kind == ObjKind::Proto) continue;
        for (const auto& [k, v] : o.fields) hold(v);
        hold(o.elems);
    }
    return out;
}

void planDirect(const Solver& s, const OwnHeld& held, const SiteFact& fact, StampPlan& plan) {
    // An unknown target beside one known callee (an escaped receiver whose
    // prototype the prediction still sees) is planned like a single one: the
    // direct edge's guard is the site's method cache, which names the callee
    // only for a receiver whose lookup really finds it.
    const bool single = fact.call == CallClass::Single ||
                        (fact.call == CallClass::UnknownTarget && s.options().predict);
    if (!single || fact.targets.size() != 1) return;
    const il::Module& mod = s.module();
    const il::Instruction& inst = mod.functions[fact.fn].blocks[fact.block].instructions[fact.inst];
    if (inst.op != il::Op::MethodCall) return;
    const uint32_t target = fact.targets[0];
    if (inst.directTarget != il::Instruction::kNoDirectTarget) {
        // Lowering's guess names the method its receiver's CLASS has under
        // the key; the prediction saw the receivers, and their callee is
        // another function. A guard that every one of them fails is a compare
        // and a dead inlined body at the site, in front of the dispatch it
        // falls back to anyway.
        if (inst.directTarget == target) return;
        plan.withdrawn.push_back(PolyEntry{fact.fn, fact.block, fact.inst});
        ++plan.callWithdrawn;
    }
    ++plan.callCandidates;
    // Whether a receiver may hold the callee in an own slot. A named write of
    // the key elsewhere (`this.setValue = setter` in one class) says nothing
    // about receivers the prediction saw without one (another class's
    // prototype `setValue`); and a callee no non-prototype object holds can
    // never be the closure an own slot answers, so the own-slot split would
    // miss every call. Either way the DIRECT edge stays sound: its guard is
    // the method cache's DIRECT entry, which the runtime latches only for a
    // method found on the chain (rt_method_call.cpp, latchMethodIc).
    bool ownSlot = held.fn[target] != 0;
    for (ObjId r : s.value(fact.fn, inst.operands[0]).objs) {
        const AObj& o = s.object(r);
        if (o.fields.count(inst.keyIndex) != 0 || o.accessors.count(inst.keyIndex) != 0 || o.anyKey) ownSlot = true;
    }
    if (ownSlot) {
        ++plan.callOwnHeld;
        // The receiver's own slot holds the callee's closure: a call the
        // closure's code word may enter directly (own_callee_calls.h).
        plan.ownCalls.push_back(DirectEntry{fact.fn, fact.block, fact.inst, target});
        return;
    }
    const il::Function& callee = mod.functions[target];
    const size_t argc = inst.operands.empty() ? 0 : inst.operands.size() - 1;
    // Extra arguments are dropped by the direct edge, which only a callee
    // with an `arguments` object or a rest parameter could observe.
    bool fits = !callee.needsEnv && !callee.needsArguments && !callee.hasRestParam && !callee.isExternal;
    const size_t base = callee.firstSourceParam();
    for (size_t i = argc; fits && i < callee.callerParamCount(); ++i) {
        if (callee.params[i + base].type != il::Type::Dynamic) fits = false;
    }
    if (!fits) {
        ++plan.callRefused;
        return;
    }
    plan.directs.push_back(DirectEntry{fact.fn, fact.block, fact.inst, target});
}

// A read the static claims left alone: marked polymorphic when every
// predicted receiver holds the key as an own data property and the cache's
// ways can hold one shape per receiver. A read inference already calls
// monomorphic keeps its way-0 form unless more than one receiver reaches it.
void planPoly(const il::Module& mod, const SiteFact& fact, StampPlan& plan) {
    if (fact.op != il::Op::PropGet) return;
    if (fact.ownOrAbsent) {
        const il::Instruction& inst = mod.functions[fact.fn].blocks[fact.block].instructions[fact.inst];
        if (inst.staticSlot == il::Instruction::kNoStaticSlot && inst.directTarget == il::Instruction::kNoDirectTarget &&
            !inst.operands.empty() && !il::parseIndexKey(mod.keyConstants[inst.keyIndex])) {
            plan.absents.push_back(PolyEntry{fact.fn, fact.block, fact.inst});
        }
        return;
    }
    const bool ownEverywhere = fact.prop == PropClass::SlotDiffers || fact.prop == PropClass::NotInPrefix ||
                               fact.prop == PropClass::SameSlot || fact.prop == PropClass::SameSlotEscaped;
    if (!ownEverywhere) return;
    const il::Instruction& inst = mod.functions[fact.fn].blocks[fact.block].instructions[fact.inst];
    if (inst.staticSlot != il::Instruction::kNoStaticSlot || inst.directTarget != il::Instruction::kNoDirectTarget ||
        inst.icPolymorphic || inst.operands.empty() || il::parseIndexKey(mod.keyConstants[inst.keyIndex])) {
        return;
    }
    if (inst.icMonomorphic && fact.receivers < 2) return;
    ++plan.polyCandidates;
    if (fact.receivers > BRONZE_ABI_IC_WAYS) {
        ++plan.polyTooMany;
        return;
    }
    plan.polys.push_back(PolyEntry{fact.fn, fact.block, fact.inst});
}

}  // namespace

StampPlan planStamps(const Solver& predicted, const ShapeAnalysis& shapes, const std::vector<SiteFact>& facts) {
    StampPlan plan;
    const il::Module& mod = predicted.module();
    const auto runs = runMembers(mod);
    const OwnHeld held = ownHeld(predicted);
    // A store into an object other objects inherit from has to tell the
    // method caches (the runtime's prototype epoch), which the static store
    // does not: those stay with the generic set.
    std::vector<uint8_t> inherited(predicted.objectCount(), 0);
    for (ObjId id = 0; id < predicted.objectCount(); ++id) {
        const AObj& o = predicted.object(id);
        if (o.kind == ObjKind::Proto) inherited[id] = 1;
        for (ObjId p : o.proto.objs) inherited[p] = 1;
    }
    for (const SiteFact& fact : facts) {
        if (fact.isCall) {
            planDirect(predicted, held, fact, plan);
            continue;
        }
        if (fact.slot < 0) continue;
        if (fact.prop != PropClass::SingleShape && fact.prop != PropClass::SameSlot &&
            fact.prop != PropClass::SameSlotEscaped) {
            continue;
        }
        const il::Instruction& inst = mod.functions[fact.fn].blocks[fact.block].instructions[fact.inst];
        if (inst.staticSlot != il::Instruction::kNoStaticSlot ||
            inst.directTarget != il::Instruction::kNoDirectTarget || il::isDefineWrite(inst) ||
            inst.operands.empty() || runs[fact.fn][fact.block][fact.inst] != 0 ||
            il::parseIndexKey(mod.keyConstants[inst.keyIndex])) {
            continue;
        }
        const AVal& recv = predicted.value(fact.fn, inst.operands[0]);
        if (inst.op == il::Op::PropSet &&
            std::any_of(recv.objs.begin(), recv.objs.end(), [&](ObjId o) { return inherited[o] != 0; })) {
            continue;
        }
        ++plan.candidates;
        StampEntry e;
        e.fn = fact.fn;
        e.block = fact.block;
        e.inst = fact.inst;
        e.slot = static_cast<uint32_t>(fact.slot);
        if (fact.prop == PropClass::SingleShape) {
            e.form = StampForm::Identity;
            plan.entries.push_back(e);
            continue;
        }
        if (mod.classFamilies.empty()) {
            if (recv.objs.size() == 1) {
                e.form = StampForm::Identity;
                plan.entries.push_back(e);
                ++plan.identityInexact;
                continue;
            }
            ++plan.noFamilies;
            continue;
        }
        // Every family a receiver may be stamped with has to sit inside the
        // site's range, and every family in the range has to declare the key
        // at the slot: the guard admits the whole range.
        uint32_t lo = UINT32_MAX, hi = 0;
        bool stampless = false;
        for (ObjId o : recv.objs) {
            const ObjShape& sh = shapes.shape(o);
            const auto stamps = possibleStamps(mod, slotsOf(sh), sh.exact && sh.extras.empty());
            if (stamps.empty()) {
                stampless = true;
                break;
            }
            lo = std::min(lo, stamps.front());
            hi = std::max(hi, stamps.back());
        }
        if (stampless || lo == UINT32_MAX) {
            // One allocation site whose shape is not certain (a literal
            // filled in after it was exposed): its objects still most likely
            // share one shape, which a cell can hold.
            if (recv.objs.size() == 1) {
                e.form = StampForm::Identity;
                plan.entries.push_back(e);
                ++plan.identityInexact;
                continue;
            }
            ++plan.noStamp;
            continue;
        }
        const bool write = inst.op == il::Op::PropSet;
        bool agree = true;
        for (uint32_t c = lo; c <= hi && agree; ++c) {
            const auto& fields = mod.classFamilies[c].fields;
            agree = e.slot < fields.size() && fields[e.slot].keyIndex == inst.keyIndex &&
                    (!write || fields[e.slot].writable);
        }
        if (!agree) {
            ++plan.rangeMismatch;
            continue;
        }
        e.form = StampForm::Family;
        e.familyLo = lo;
        e.familySpan = hi - lo;
        plan.entries.push_back(e);
    }
    std::set<std::tuple<uint32_t, uint32_t, uint32_t>> stamped;
    for (const StampEntry& e : plan.entries) stamped.insert({e.fn, e.block, e.inst});
    for (const SiteFact& fact : facts) {
        if (!fact.isCall && stamped.count({fact.fn, fact.block, fact.inst}) == 0) planPoly(mod, fact, plan);
    }
    return plan;
}

bool familyClaimFor(const il::Module& mod, const std::vector<uint32_t>& slots, uint32_t slot, uint32_t keyIndex,
                    bool write, uint32_t* lo, uint32_t* span) {
    if (mod.classFamilies.empty()) return false;
    const auto stamps = possibleStamps(mod, slots, /*exact=*/true);
    if (stamps.empty()) return false;
    const uint32_t first = stamps.front(), last = stamps.back();
    for (uint32_t c = first; c <= last; ++c) {
        const auto& fields = mod.classFamilies[c].fields;
        if (slot >= fields.size() || fields[slot].keyIndex != keyIndex || (write && !fields[slot].writable)) {
            return false;
        }
    }
    *lo = first;
    *span = last - first;
    return true;
}

void applyDirects(il::Module& mod, const StampPlan& plan) {
    for (const PolyEntry& w : plan.withdrawn) {
        il::Instruction& inst = mod.functions[w.fn].blocks[w.block].instructions[w.inst];
        inst.directTarget = il::Instruction::kNoDirectTarget;
        inst.typedTarget = il::Instruction::kNoDirectTarget;
    }
    for (const DirectEntry& d : plan.directs) {
        mod.functions[d.fn].blocks[d.block].instructions[d.inst].directTarget = d.target;
    }
}

void applyStamps(il::Module& mod, const StampPlan& plan) {
    applyDirects(mod, plan);
    for (const PolyEntry& p : plan.polys) {
        mod.functions[p.fn].blocks[p.block].instructions[p.inst].icPolymorphic = true;
    }
    for (const PolyEntry& p : plan.absents) {
        mod.functions[p.fn].blocks[p.block].instructions[p.inst].icAbsent = true;
    }
    for (const StampEntry& e : plan.entries) {
        il::Instruction& inst = mod.functions[e.fn].blocks[e.block].instructions[e.inst];
        inst.staticSlot = e.slot;
        inst.staticSlotGuessed = true;
        if (e.form == StampForm::Identity) {
            inst.staticCellIndex = mod.staticSiteCount++;
            inst.familyLo = il::Instruction::kNoFamily;
            inst.familySpan = 0;
        } else {
            inst.staticCellIndex = 0;
            inst.familyLo = e.familyLo;
            inst.familySpan = e.familySpan;
        }
    }
}

}  // namespace bronze::wpo
