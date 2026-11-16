#include "opt/redundancy/location.h"

#include <cassert>

namespace bronze::opt::redundancy {

namespace {

using il::Op;

// Whether operand `slot` of `inst` uses its value WITHOUT capturing it: the
// object operand of a keyed access, or the subject of a test. Nothing reached
// this way can store the object, return it or hand it to code that could.
// Every other use — an edge argument, a call, a stored VALUE, a leave's state —
// lets the object be named by another value.
bool nonCapturingUse(const il::Instruction& inst, size_t slot) {
    switch (inst.op) {
        case Op::SlotLoad:
        case Op::SlotStore:     // operand 1 is the stored value: that captures
        case Op::LoadShape:
        case Op::ShapeStore:    // operand 1 is a shape, not a value
        case Op::ElemLoad:
        case Op::ElemStore:     // operand 2 is the stored value
        case Op::ArrayLength:
        case Op::TypedLength:
        case Op::TypedLoad:
        case Op::TypedStore:
        case Op::ArrayPush:     // operand 1 is the pushed value: that captures
            return slot == 0;
        case Op::IsObject:
        case Op::IsI32:
        case Op::IsNumber:
        case Op::IsNullish:
        case Op::IsHole:
        case Op::IsUninit:
        case Op::ShapeLacks:    // operand 1 is a shape
        case Op::IsDenseArray:
        case Op::IsPlainArray:
        case Op::IsTypedArray:
        case Op::IsArray:
        case Op::FnCodeIs:
        case Op::FnNativeIs:
        // A way out of S: no instruction of S runs after it (location.h).
        case Op::Leave:
        case Op::ResumeCall:
            return true;
        default:
            return false;
    }
}

bool isAllocation(Op op) {
    return op == Op::CreateObject || op == Op::CreateArray || op == Op::ObjectLiteral || op == Op::ArrayLiteral ||
           op == Op::CreateThis;
}

// Whether a place names memory of one object, its `base`.
bool objectPlace(Place place) {
    return place == Place::Slot || place == Place::Elem || place == Place::Shape || place == Place::Array;
}

// The location class a place lives in. A place is only attached to an access
// whose LocSet includes it, so the refinement never narrows what the effect
// table says beyond that one class.
il::Loc naturalLoc(Place place) {
    switch (place) {
        case Place::Slot: return il::Loc::Slots;
        case Place::Elem: return il::Loc::Elements;
        case Place::Env: return il::Loc::EnvSlots;
        case Place::ModuleEnv: return il::Loc::ModuleEnv;
        case Place::Shape: return il::Loc::Shapes;
        case Place::Array: return il::Loc::ArrayLen;
        case Place::None: break;
    }
    return il::Loc::Heap;
}

// The place of a keyed access, or Place::None. Shared by reads and writes:
// the object, array or record is operand 0 throughout.
Location placeOf(const il::Instruction& inst) {
    Location at;
    switch (inst.op) {
        case Op::SlotLoad:
        case Op::SlotStore:
            if (inst.operands.empty()) return at;
            at.place = Place::Slot;
            at.base = inst.operands[0];
            at.a = inst.immI32;
            return at;
        case Op::ElemLoad:
        case Op::ElemStore:
            if (inst.operands.size() < 2) return at;
            at.place = Place::Elem;
            at.base = inst.operands[0];
            at.index = inst.operands[1];
            return at;
        case Op::EnvGet:
        case Op::EnvGetTdz:
        case Op::EnvSet:
        case Op::EnvInitTdz:
            if (inst.operands.empty()) return at;
            at.place = Place::Env;
            at.base = inst.operands[0];
            at.a = inst.envDepth;
            at.b = inst.envIndex;
            return at;
        case Op::ModuleEnvGet:
        case Op::ModuleEnvSet:
            at.place = Place::ModuleEnv;
            return at;
        case Op::LoadShape:
        case Op::ShapeStore:
            if (inst.operands.empty()) return at;
            at.place = Place::Shape;
            at.base = inst.operands[0];
            return at;
        case Op::IsDenseArray:
        case Op::ArrayLength:
        // Its length and its elements: one array's memory (il_op.h).
        case Op::ArrayPush:
            if (inst.operands.empty()) return at;
            at.place = Place::Array;
            at.base = inst.operands[0];
            return at;
        // The new object's shape word, written as it is made.
        case Op::CreateThis:
            at.place = Place::Shape;
            at.base = inst.result;
            return at;
        default:
            return at;
    }
}

Access accessOf(const il::Instruction& inst, il::LocSet locs, bool callsUnknown) {
    Access access;
    access.locs = callsUnknown ? il::LocSet::all() : locs;
    if (access.locs.empty() || callsUnknown) return access;
    const Location at = placeOf(inst);
    if (at.place != Place::None && access.locs.has(naturalLoc(at.place))) access.where = at;
    return access;
}

// Rules 3-5 of the invalidation rule (location.h): whether two places can be
// the same memory.
bool placesMayMeet(const Location& x, const Location& y, const AliasInfo& alias) {
    if (x.place == Place::None || y.place == Place::None) return true;
    if (x.place != y.place) return !(objectPlace(x.place) && objectPlace(y.place)) || alias.mayAlias(x.base, y.base);
    switch (x.place) {
        case Place::Slot:
            return x.a == y.a && alias.mayAlias(x.base, y.base);
        case Place::Elem: {
            if (!alias.mayAlias(x.base, y.base)) return false;
            if (x.index == y.index) return true;
            const std::optional<int32_t> cx = alias.constI32(x.index);
            const std::optional<int32_t> cy = alias.constI32(y.index);
            return !(cx && cy && *cx != *cy);
        }
        case Place::Env:
            // Different slot numbers are different memory in any record; one
            // record value at two depths names two records of one chain.
            if (x.b != y.b) return false;
            return !(x.base == y.base && x.a != y.a);
        case Place::ModuleEnv:
            return true;
        case Place::Shape:
        case Place::Array:
            return alias.mayAlias(x.base, y.base);
        case Place::None:
            break;
    }
    return true;
}

}  // namespace

AliasInfo::AliasInfo(const il::Function& fn, const DomTree* dom) : dom_(dom) {
    const size_t n = fn.valueCount;
    fresh_.assign(n, 0);
    alloc_.assign(n, 0);
    defBlock_.assign(n, kNoBlock);
    defPos_.assign(n, -1);
    isConst_.assign(n, 0);
    constValue_.assign(n, 0);
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        const il::Block& block = fn.blocks[b];
        for (const il::BlockParam& p : block.params) {
            if (p.id < n) defBlock_[p.id] = static_cast<BlockId>(b);
        }
        for (size_t i = 0; i < block.instructions.size(); ++i) {
            const il::Instruction& inst = block.instructions[i];
            if (inst.result == kNoValue || inst.result >= n) continue;
            defBlock_[inst.result] = static_cast<BlockId>(b);
            defPos_[inst.result] = static_cast<int32_t>(i);
            if (isAllocation(inst.op)) {
                fresh_[inst.result] = 1;
                alloc_[inst.result] = 1;
            }
            if (inst.op == Op::ConstI32 && inst.type == il::Type::I32) {
                isConst_[inst.result] = 1;
                constValue_[inst.result] = inst.immI32;
            }
        }
    }
    // Any capturing use disqualifies an allocation, wherever it is.
    auto escape = [&](ValueId v) {
        if (v < n) fresh_[v] = 0;
    };
    for (const il::Block& block : fn.blocks) {
        for (const il::Instruction& inst : block.instructions) {
            for (size_t i = 0; i < inst.operands.size(); ++i) {
                if (!nonCapturingUse(inst, i)) escape(inst.operands[i]);
            }
            for (ValueId v : inst.target.args) escape(v);
            for (ValueId v : inst.elseTarget.args) escape(v);
        }
    }
}

bool AliasInfo::definedBefore(ValueId v, ValueId a) const {
    if (dom_ == nullptr || a >= alloc_.size() || !alloc_[a] || v >= defBlock_.size()) return false;
    const BlockId ab = defBlock_[a];
    const BlockId vb = defBlock_[v];
    if (ab == kNoBlock) return false;
    if (vb == kNoBlock) return true;  // a function parameter
    if (vb == ab) return defPos_[v] < defPos_[a];
    return dom_->strictlyDominates(vb, ab);
}

bool AliasInfo::mayAlias(ValueId a, ValueId b) const {
    if (a == b) return true;
    if (isFreshUnescaped(a) || isFreshUnescaped(b)) return false;
    return !definedBefore(a, b) && !definedBefore(b, a);
}

std::optional<int32_t> AliasInfo::constI32(ValueId v) const {
    if (v >= isConst_.size() || !isConst_[v]) return std::nullopt;
    return constValue_[v];
}

Access readOf(const il::Instruction& inst, const il::Effects& effects) {
    Access read = accessOf(inst, effects.reads, effects.callsUnknown);
    // Only the plain read of an initialized immutable binding: the checked
    // form exists because its slot may not be initialized yet.
    read.immutable = inst.op == Op::EnvGet && inst.envImmutable && read.where.place == Place::Env;
    return read;
}

Access writeOf(const il::Instruction& inst, const il::Effects& effects) {
    return accessOf(inst, effects.writes, effects.callsUnknown);
}

bool writeKillsRead(const Access& write, const Access& read, const AliasInfo& alias) {
    if (read.immutable) return false;
    return mayOverlap(write, read, alias);
}

bool mayOverlap(const Access& x, const Access& y, const AliasInfo& alias) {
    if (!x.locs.intersects(y.locs)) return false;
    return placesMayMeet(x.where, y.where, alias);
}

bool sameLocation(const Location& x, const Location& y, const AliasInfo& alias) {
    if (x.place == Place::None || x.place != y.place) return false;
    switch (x.place) {
        case Place::Slot:
            return x.base == y.base && x.a == y.a;
        case Place::Elem: {
            if (x.base != y.base) return false;
            if (x.index == y.index) return true;
            const std::optional<int32_t> cx = alias.constI32(x.index);
            const std::optional<int32_t> cy = alias.constI32(y.index);
            return cx && cy && *cx == *cy;
        }
        case Place::Env:
            return x.base == y.base && x.a == y.a && x.b == y.b;
        case Place::ModuleEnv:
            return true;
        case Place::Shape:
        case Place::Array:
            return x.base == y.base;
        case Place::None:
            break;
    }
    return false;
}

ValueId storedValue(const il::Instruction& inst) {
    switch (inst.op) {
        case Op::SlotStore:
        case Op::EnvSet:
            return inst.operands.size() == 2 ? inst.operands[1] : kNoValue;
        case Op::ElemStore:
            return inst.operands.size() == 3 ? inst.operands[2] : kNoValue;
        case Op::ModuleEnvSet:
            return inst.operands.size() == 1 ? inst.operands[0] : kNoValue;
        case Op::ShapeStore:
        case Op::CreateThis:
            return inst.operands.size() == 2 ? inst.operands[1] : kNoValue;
        default:
            return kNoValue;
    }
}

void WriteSet::add(const Access& write) {
    if (write.locs.empty()) return;
    if (write.where.place == Place::None || precise_.size() >= kMaxPrecise) {
        coarse_ |= write.locs;
        return;
    }
    for (const Access& w : precise_) {
        if (w.locs == write.locs && w.where == write.where) return;
    }
    precise_.push_back(write);
}

void WriteSet::addAll(const WriteSet& other) {
    coarse_ |= other.coarse_;
    for (const Access& w : other.precise_) add(w);
}

bool WriteSet::kills(const Access& read, const AliasInfo& alias) const {
    if (read.immutable) return false;
    if (coarse_.intersects(read.locs)) return true;
    for (const Access& w : precise_) {
        if (writeKillsRead(w, read, alias)) return true;
    }
    return false;
}

il::LocSet WriteSet::allLocs() const {
    il::LocSet out = coarse_;
    for (const Access& w : precise_) out |= w.locs;
    return out;
}

std::vector<WriteSet> blockWriteSets(const il::Function& fn) {
    std::vector<WriteSet> out(fn.blocks.size());
    for (size_t b = 0; b < fn.blocks.size(); ++b) {
        for (const il::Instruction& inst : fn.blocks[b].instructions) {
            out[b].add(writeOf(inst, il::effectsOf(inst)));
        }
    }
    return out;
}

WriteSet regionWrites(const Cfg& cfg, const std::vector<WriteSet>& writes, BlockId from, BlockId to,
                      const std::function<bool(BlockId, BlockId)>* deadEdge) {
    // opt::writesBetween's walk: backwards from `to`'s predecessors, stopping
    // at `from`. Every block met is dominated by `from`.
    assert(from != kNoBlock);
    WriteSet out;
    std::vector<uint8_t> seen(cfg.size(), 0);
    std::vector<BlockId> work;
    auto predsOf = [&](BlockId b) {
        for (BlockId p : cfg.preds(b)) {
            if (deadEdge == nullptr || !(*deadEdge)(p, b)) work.push_back(p);
        }
    };
    predsOf(to);
    while (!work.empty()) {
        const BlockId b = work.back();
        work.pop_back();
        if (b == from || seen[b] || !cfg.reachable(b)) continue;
        seen[b] = 1;
        out.addAll(writes[b]);
        predsOf(b);
    }
    return out;
}

}  // namespace bronze::opt::redundancy
