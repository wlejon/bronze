#include "opt/redundancy/expr_table.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace bronze::opt::redundancy {

namespace {

using il::Op;

// Ops whose two operands may be swapped without changing the value. Typed
// forms only: a dynamic `add` is concatenation for strings (and never reaches
// the table anyway, since it calls unknown code).
bool isCommutative(const il::Instruction& inst) {
    if (inst.operands.size() != 2) return false;
    switch (inst.op) {
        case Op::Add:
        case Op::Mul:
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
            return inst.type != il::Type::Dynamic;
        case Op::CmpEq:
        case Op::CmpNe:
        case Op::StrictEq:
        case Op::PtrEq:
        case Op::AddWouldOvf:
        case Op::MulWouldOvf:
            return true;
        default:
            return false;
    }
}

inline void mix(size_t& h, uint64_t v) {
    h ^= std::hash<uint64_t>{}(v) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
}

}  // namespace

size_t ExprKeyHash::operator()(const ExprKey& key) const {
    size_t h = static_cast<size_t>(key.op);
    mix(h, static_cast<uint64_t>(key.type));
    mix(h, static_cast<uint64_t>(key.boxType));
    mix(h, static_cast<uint32_t>(key.immI32));
    mix(h, key.immBits);
    mix(h, key.calleeIndex);
    mix(h, key.keyIndex);
    mix(h, (static_cast<uint64_t>(key.envDepth) << 32) | key.envIndex);
    mix(h, (static_cast<uint64_t>(key.callEnvHops) << 32) | key.directTarget);
    for (ValueId v : key.operands) mix(h, v);
    return h;
}

ExprKey exprKeyOf(const il::Instruction& inst) {
    ExprKey key;
    key.op = inst.op;
    key.type = inst.type;
    key.boxType = inst.boxType;
    key.immI32 = inst.immI32;
    std::memcpy(&key.immBits, &inst.immF64, sizeof key.immBits);
    key.calleeIndex = inst.calleeIndex;
    key.keyIndex = inst.keyIndex;
    key.envDepth = inst.envDepth;
    key.envIndex = inst.envIndex;
    key.callEnvHops = inst.callEnvHops;
    key.directTarget = inst.directTarget;
    key.operands = inst.operands;
    if (isCommutative(inst)) std::sort(key.operands.begin(), key.operands.end());
    // The checked read carries the binding's name for its diagnostic only.
    if (inst.op == Op::EnvGet || inst.op == Op::EnvGetTdz) key.keyIndex = 0;
    return key;
}

ExprKey loadKey(il::Op loadOp, il::Type type, const Location& where) {
    il::Instruction inst;
    inst.op = loadOp;
    inst.type = type;
    switch (where.place) {
        case Place::Slot:
            inst.operands = {where.base};
            inst.immI32 = static_cast<int32_t>(where.a);
            break;
        case Place::Elem:
            inst.operands = {where.base, where.index};
            break;
        case Place::Shape:
            inst.operands = {where.base};
            break;
        case Place::Env:
            inst.operands = {where.base};
            inst.envDepth = static_cast<uint32_t>(where.a);
            inst.envIndex = static_cast<uint32_t>(where.b);
            break;
        case Place::ModuleEnv:
        case Place::None:
            break;
    }
    return exprKeyOf(inst);
}

void ExprTable::push() { scopes_.push_back(log_.size()); }

void ExprTable::pop() {
    assert(!scopes_.empty());
    if (scopes_.empty()) return;
    const size_t mark = scopes_.back();
    scopes_.pop_back();
    // Newest first. Additions are LIFO across scopes, so an added entry is the
    // last one stored when its log entry comes off.
    while (log_.size() > mark) {
        const LogEntry entry = log_.back();
        log_.pop_back();
        switch (entry.kind) {
            case LogKind::Killed:
                entries_[entry.index].alive = true;
                break;
            case LogKind::Hid:
                floor_ = entry.index;
                break;
            case LogKind::Added: {
                assert(entry.index + 1 == entries_.size());
                auto it = index_.find(entries_[entry.index].key);
                assert(it != index_.end() && !it->second.empty() && it->second.back() == entry.index);
                it->second.pop_back();
                if (it->second.empty()) index_.erase(it);
                if (!memEntries_.empty() && memEntries_.back() == entry.index) memEntries_.pop_back();
                entries_.pop_back();
                break;
            }
        }
    }
}

Available ExprTable::lookup(const ExprKey& key) const {
    const auto it = index_.find(key);
    if (it == index_.end()) return {};
    for (auto r = it->second.rbegin(); r != it->second.rend(); ++r) {
        if (*r < floor_) break;  // older entries are hidden too
        const Entry& e = entries_[*r];
        if (e.alive) return Available{e.value, e.forwarded};
    }
    return {};
}

void ExprTable::add(const ExprKey& key, ValueId value, const Access& read, bool forwarded) {
    const uint32_t i = static_cast<uint32_t>(entries_.size());
    entries_.push_back(Entry{key, value, read, forwarded, true});
    index_[key].push_back(i);
    if (!read.empty()) memEntries_.push_back(i);
    log_.push_back(LogEntry{LogKind::Added, i});
}

void ExprTable::killEntry(uint32_t i) {
    entries_[i].alive = false;
    log_.push_back(LogEntry{LogKind::Killed, i});
}

void ExprTable::kill(const Access& write, const AliasInfo& alias) {
    if (write.empty()) return;
    for (uint32_t i : memEntries_) {
        if (entries_[i].alive && writeKillsRead(write, entries_[i].read, alias)) killEntry(i);
    }
}

void ExprTable::killAll(const WriteSet& writes, const AliasInfo& alias) {
    if (writes.empty()) return;
    for (uint32_t i : memEntries_) {
        if (entries_[i].alive && writes.kills(entries_[i].read, alias)) killEntry(i);
    }
}

void ExprTable::hideAll() {
    log_.push_back(LogEntry{LogKind::Hid, floor_});
    floor_ = static_cast<uint32_t>(entries_.size());
}

}  // namespace bronze::opt::redundancy
