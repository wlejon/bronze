// MEMBER CELLS (il.h Module::memberCells): reconciling a compile-time guess
// with a run-time prototype chain.
//
// A speculated method call, getter read or setter write whose harvest found
// the member above the receiver (opt/speculate/member_call.cpp) compiles to
// shape tests against two-word module CELLS, each followed by a direct call
// of — or the inlined body of — the function the cell is for; a read of a
// data property above it (member_data.cpp), by the walk to the holder and
// the load of its slot. The compiler
// can only GUESS: a receiver may carry an own property of the same name, a
// subclass may override the member, and any of the prototypes may be
// patched. This is the one function that decides, for one shape, that the
// guess is what [[Get]] or [[Set]] (and the call after the [[Get]]) would
// actually do.
//
// The cell is (shape, epoch), and the guard is
//
//     receiver is plain  &&  receiver.shape == cell[0]
//
// one compare, because a cell that names a shape always speaks for the
// current epoch: every published cell is on the thread's list, and a bump of
// the epoch empties each one before anything else runs
// (`rtForgetPublishedCells`, called by `bumpProtoMutationEpoch`). The epoch
// word is the slow path's alone: it says whether the miss should ask again.
//
// The SHAPE stands for everything about the receiver itself: a non-dictionary
// shape fixes the object's own keys (so the key is still not own) and its
// [[Prototype]] (which lives on the shape's root). The EPOCH stands for
// everything above it: every change that could make the chain answer
// differently — an add to a prototype (which may shadow), a delete or a
// dictionary demotion on one, an accessor redefined in place, a prototype
// swap — bumps the thread's prototype-mutation epoch (object.cpp,
// accessor.cpp, dictionary.cpp), which is the same contract the inline
// caches' depth > 0 entries already lean on. `chainIsCacheable` is the check
// that makes it hold for the chain in front of us: every link is marked
// `used_as_prototype`, so a later add to any of them does bump.
//
// A data write to a prototype that leaves no shape behind does not bump, and
// does not need to: overwriting a slot that held a FUNCTION does bump
// (`notePrototypeSlotOverwrite`), and an accessor's pair is never written by
// an ordinary [[Set]] (that calls the setter). `Matrix4.prototype.isMatrix4 =
// true` in every construction therefore leaves `Euler`'s cells alone.
//
// The cell's states:
//
//   [0, 0]               never tried; the guard misses (no live shape is 0).
//   [kRefused, epoch]    tried at this epoch and the chain did not answer the
//                        guess for that receiver; the guard misses.
//   [shape, epoch]       this shape resolves the key to the guessed member.
//
// A cell is re-published when the epoch moves: setup bumps it often (every
// class body adds methods to a prototype), and a cell pinned once would go
// stale for good. The site's exit asks again on every miss, and leaves for
// the generic code either way.
//
// The list is also emptied, cells and all, before a compiled-in-process
// module's memory can go (its cells live there) and when the thread's
// runtime state is torn down (a later thread's shape may take a dead one's
// address): rt_state.cpp. A cell emptied that way reads as never tried.

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/realm.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

#include <vector>

namespace bronze::runtime {

namespace {

// Not a legal shape address (shapes come from the arena 8-aligned), so the
// guard needs no extra test to treat a refusal as a miss.
constexpr uint64_t kRefused = 1;

// The cells that name a shape now. A pointer, deleted when they are
// forgotten: the thread's teardown forgets them from another thread_local's
// destructor, after a vector with its own destructor may be gone.
thread_local std::vector<uint64_t*>* t_published = nullptr;

void publish(uint64_t* cell, uint64_t word, uint64_t epoch) {
    cell[0] = word;
    cell[1] = epoch;
    if (word != kRefused) {
        if (t_published == nullptr) t_published = new std::vector<uint64_t*>;
        t_published->push_back(cell);
    }
}

// The function the key resolves to on `obj`'s prototype chain — an accessor's
// half, or a data property's value for a method — or null when the chain
// answers anything else: the key own on the receiver, the other kind of
// property above it, a link the epoch does not watch, or no such key at all.
enum class Member { Getter, Setter, Method };

const FunctionHeader* resolvedMember(ObjectHeader* obj, PropertyKey key, Member want) {
    PropertyInfo info;
    // An own property — data or accessor — is what [[Get]] finds first.
    if (obj->shape->lookupProperty(key, info)) return nullptr;
    if (!obj->chainIsCacheable()) return nullptr;
    ObjectHeader* cur = obj;
    for (uint32_t depth = 1; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        const Value proto = cur->shape->prototypeValue();
        if (!proto.isObject()) return nullptr;
        auto* hdr = proto.asObject<HeapObjectHeader>();
        // A link that is not a plain shape-chain object answers through
        // machinery of its own, and a dictionary (even a frozen one, which
        // `chainIsCacheable` admits) is a holder whose slots no shape names.
        if (hdr->flags != HeapKind::Plain) return nullptr;
        cur = reinterpret_cast<ObjectHeader*>(hdr);
        if (cur->shape == nullptr || cur->shape->isDictionary()) return nullptr;
        if (!cur->shape->lookupProperty(key, info)) continue;
        if (info.accessor != (want != Member::Method)) return nullptr;
        // The setter is the slot after the getter (accessor.cpp).
        const Value half = cur->getSlot(info.slot + (want == Member::Setter ? 1u : 0u));
        if (!half.isObject() || half.asObject<HeapObjectHeader>()->flags != HeapKind::Function) return nullptr;
        return half.asObject<FunctionHeader>();
    }
    return nullptr;
}

// Whether [[Get]] of `key` on `obj` finds nothing at all: not own, and on no
// link of a chain the epoch watches, ending at null. Every link must be a
// plain object (an exotic one answers through machinery of its own) whose
// shape a read may name: a transition-tree node, or a frozen dictionary (the
// intrinsics after lockdown), which nothing can be added to.
bool resolvesAbsent(ObjectHeader* obj, PropertyKey key) {
    PropertyInfo info;
    if (obj->shape->lookupProperty(key, info)) return false;
    if (!obj->chainIsCacheable()) return false;
    ObjectHeader* cur = obj;
    for (uint32_t depth = 1; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        const Value proto = cur->shape->prototypeValue();
        if (proto.isNull()) return true;
        if (!proto.isObject()) return false;
        auto* hdr = proto.asObject<HeapObjectHeader>();
        if (hdr->flags != HeapKind::Plain) return false;
        cur = reinterpret_cast<ObjectHeader*>(hdr);
        if (cur->shape == nullptr || !cur->shape->readCacheable()) return false;
        if (cur->shape->lookupProperty(key, info)) return false;
    }
    return false;
}

// Whether [[Get]] of `key` on `obj` finds a data property exactly `depth`
// links above it, at `slot` of that holder: not own, on no link below the
// holder, and every link up to the holder a plain object at a transition-tree
// shape (a dictionary's slots are no shape's, and an exotic link answers
// through machinery of its own) on a chain the epoch watches. Compiled code
// then walks the same links from the receiver's shape (`shape.proto`, which
// reads the root's prototype word, so it reaches the holder where a
// collection has moved it) and loads the slot: a write of the value moves no
// shape and needs no bump, as the load reads the slot each time, and either
// representation of the slot reads as the Value it holds (slot_repr.h).
bool resolvesData(ObjectHeader* obj, PropertyKey key, uint32_t depth, uint32_t slot) {
    PropertyInfo info;
    if (depth == 0 || depth > ObjectHeader::kMaxPrototypeDepth) return false;
    if (obj->shape->lookupProperty(key, info)) return false;
    if (!obj->chainIsCacheable()) return false;
    ObjectHeader* cur = obj;
    for (uint32_t d = 1; d <= depth; ++d) {
        const Value proto = cur->shape->prototypeValue();
        if (!proto.isObject()) return false;
        auto* hdr = proto.asObject<HeapObjectHeader>();
        if (hdr->flags != HeapKind::Plain) return false;
        cur = reinterpret_cast<ObjectHeader*>(hdr);
        if (cur->shape == nullptr || cur->shape->isDictionary()) return false;
        if (!cur->shape->lookupProperty(key, info)) continue;
        return d == depth && !info.accessor && info.slot == slot;
    }
    return false;
}

// The absent-cache entry (bronze_abi_layout.h BRONZE_ABI_SLOTKEY_LACKS_ENTRIES)
// a `shape.lacks` fill of `keyIndex` writes, `lacks` being the cache's first
// word. A key has two buckets, its home and its alternate, and generated code
// tests both, so the few keys one shape is asked about stay cached together
// rather than evicting each other from a shared bucket on every ask (three.js
// asks a material's shape for half a dozen `isXMaterial` flags a draw). The
// key's own entry is reused, else a bucket naming no key at this epoch; when
// both buckets hold other live keys, the home's occupant moves to its own
// other bucket if that one is free, and failing that is overwritten.
uint64_t* lacksEntryFor(uint64_t* lacks, uint32_t keyIndex) {
    const uint64_t epoch = protoMutationEpoch();
    auto entry = [&](uint32_t bucket) { return lacks + 2 * bucket; };
    auto holds = [](const uint64_t* e, uint32_t k) {
        return (e[0] & ~BRONZE_ABI_SLOTKEY_LACKS_FOUND) == uint64_t{k} + 1;
    };
    auto vacant = [&](const uint64_t* e) { return e[0] == 0 || e[1] != epoch; };
    uint64_t* home = entry(BRONZE_ABI_SLOTKEY_LACKS_HOME(keyIndex));
    uint64_t* alt = entry(BRONZE_ABI_SLOTKEY_LACKS_ALT(keyIndex));
    if (holds(home, keyIndex)) return home;
    if (holds(alt, keyIndex)) return alt;
    if (vacant(home)) return home;
    if (vacant(alt)) return alt;
    for (uint64_t* e : {home, alt}) {
        const auto occupant = static_cast<uint32_t>((e[0] & ~BRONZE_ABI_SLOTKEY_LACKS_FOUND) - 1);
        uint64_t* other = entry(BRONZE_ABI_SLOTKEY_LACKS_HOME(occupant));
        if (other == e) other = entry(BRONZE_ABI_SLOTKEY_LACKS_ALT(occupant));
        if (vacant(other)) {
            other[0] = e[0];
            other[1] = e[1];
            return e;
        }
    }
    return home;
}

}  // namespace

void rtForgetPublishedCells() noexcept {
    if (t_published == nullptr) return;
    for (uint64_t* cell : *t_published) {
        cell[0] = 0;
        cell[1] = 0;
    }
    delete t_published;
    t_published = nullptr;
}

extern "C" {

// `code` is the cell's function's code word: what its function object
// carries. The fast path calls that function's body with no function object
// in hand, so each condition below is one it will not re-check:
//
//   - a plain, non-dictionary receiver (the shape word stands for its keys
//     and its prototype only then);
//   - the chain resolves the key, at this epoch, above the receiver, to a
//     member of `kind` (il.h Module::MemberKind: 0 a method in a data
//     property, 1 a getter, 2 a setter) whose function has exactly that code,
//     for kind 3 (absent) finds the key nowhere (`code` unused), or for kind
//     4 (data) finds a data property at the place `code` packs (il.h
//     Module::memberDataPlace: the links to the holder, its slot there);
//   - a function compiled code may enter without the runtime (no class
//     constructor for a method, which throws when called; an accessor that
//     needs no scope pushed), made in the realm running now.
//
// The cells of one site (`group`, `groupSize` of them) are tested in order by
// the fast path, so a shape goes into one of them only: a site whose
// receivers of several shapes all reach one function gets one cell per shape.
void bronze_member_cell_publish(uint64_t objBits, uint32_t keyIndex, uint64_t* cell, bronze_fn_code code,
                                uint32_t kind, uint64_t* group, uint32_t groupSize) {
    recordHelperCall("bronze_member_cell_publish");
    if (cell == nullptr || kind > 4) return;
    const uint64_t epoch = protoMutationEpoch();
    // A shape at this epoch stays; a refusal is asked again, as another
    // receiver may resolve differently.
    if (cell[1] == epoch && cell[0] != kRefused && cell[0] != 0) return;
    Value objVal(objBits);
    StringHeader* key = rtKeyHeader(keyIndex);
    if (key == nullptr || !objVal.isObject() || objVal.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) return;
    auto* obj = objVal.asObject<ObjectHeader>();
    if (obj->shape == nullptr || obj->shape->isDictionary()) return;
    const uint64_t shapeWord = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(obj->shape));
    for (uint32_t i = 0; group != nullptr && i < groupSize; ++i) {
        if (group[2 * i] == shapeWord && group[2 * i + 1] == epoch) return;
    }
    uint64_t published = kRefused;
    if (kind == 3) {
        if (resolvesAbsent(obj, PropertyKey::forString(key))) published = shapeWord;
        publish(cell, published, epoch);
        return;
    }
    if (kind == 4) {
        const auto place = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(code));
        const uint32_t slotMask = (1u << BRONZE_ABI_MEMBER_DATA_SLOT_BITS) - 1;
        if (resolvesData(obj, PropertyKey::forString(key), place >> BRONZE_ABI_MEMBER_DATA_SLOT_BITS,
                         place & slotMask)) {
            published = shapeWord;
        }
        publish(cell, published, epoch);
        return;
    }
    const Member want = kind == 0 ? Member::Method : kind == 1 ? Member::Getter : Member::Setter;
    const FunctionHeader* fn = resolvedMember(obj, PropertyKey::forString(key), want);
    if (fn != nullptr && fn->code == code && (fn->realmSlot() == 0 || fn->realmSlot() == rtCurrentRealmSlot()) &&
        (want == Member::Method ? !(fn->function_flags & BRONZE_ABI_FN_FLAG_CLASS_CTOR) : rtEntersWithoutScope(fn))) {
        published = shapeWord;
    }
    publish(cell, published, epoch);
}

// A `shape.lacks` test's miss (il_op.h ShapeLacks): whether [[Get]] of the
// key on `obj` finds nothing now, recorded in the absent cache of the
// object's shape (bronze_abi_layout.h BRONZE_ABI_SLOTKEY_LACKS_ENTRIES) under
// the current epoch. Only a plain, non-dictionary receiver is answered and
// recorded, for the reason a member cell names a shape only for one (above):
// its shape is then all there is to know about its own keys and its
// [[Prototype]], and the epoch stands for the rest of the chain. An entry is
// read only beside the epoch word it was filled with, so a bump makes every
// entry a miss without anything here having to forget it.
uint64_t bronze_shape_lacks(uint64_t objBits, uint32_t keyIndex) {
    recordPropCall("bronze_shape_lacks", keyIndex, nullptr);
    Value objVal(objBits);
    StringHeader* key = rtKeyHeader(keyIndex);
    if (key == nullptr || !objVal.isObject() || objVal.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
        return 0;
    }
    auto* obj = objVal.asObject<ObjectHeader>();
    if (obj->shape == nullptr || obj->shape->isDictionary()) return 0;
    // Either answer holds for every object of the shape until a prototype
    // changes (the epoch), so both are kept; a found key's entry sends the
    // next receiver of the shape straight to the generic read.
    const bool absent = resolvesAbsent(obj, PropertyKey::forString(key));
    const auto shapeBits = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(obj->shape));
    auto* table = reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(bronze_shape_slot_keys(shapeBits)));
    uint64_t* entry = lacksEntryFor(table - BRONZE_ABI_SLOTKEY_LACKS_BYTES / sizeof(uint64_t), keyIndex);
    entry[0] = (uint64_t{keyIndex} + 1) | (absent ? 0 : BRONZE_ABI_SLOTKEY_LACKS_FOUND);
    entry[1] = protoMutationEpoch();
    return absent ? 1 : 0;
}

}  // extern "C"

}  // namespace bronze::runtime
