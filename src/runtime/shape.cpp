#include "runtime/spread_plain.h"
#include "runtime/shape.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "runtime/elem_ic.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"
#include "runtime/shape_leave.h"
#include "runtime/slot_repr.h"

namespace bronze {

std::atomic<uint64_t> g_shapeTransitions{0};

Shape* Shape::createRoot(NonMovingArena& arena, Value proto, uint32_t inlineSlots) {
    Shape* root = arena.create<Shape>();
    root->prototype = proto;
    root->setInlineSlots(inlineSlots);
    // This is the ONE moment an object becomes a prototype — every route to one
    // (a class, `Object.create`, `Object.setPrototypeOf`) ends in a root shape
    // carrying it — so it is where the mark is applied rather than at each of
    // those callers.
    //
    // Only a shape-carrying object is marked: `cachedProtoHolder` refuses to
    // walk through anything else, so an array or a function used as a
    // prototype already misses for a reason that has nothing to do with the
    // epoch.
    if (proto.isObject()) {
        auto* hdr = proto.asObject<HeapObjectHeader>();
        if (HeapKind::carriesShape(hdr->flags)) {
            markObjectAsPrototype(arena, reinterpret_cast<ObjectHeader*>(hdr));
        } else if (hdr->flags == HeapKind::Function) {
            // A function answers a key from its statics object, so that is
            // what is marked: its index keys are scanned now and its adds trip
            // the hazard from here on (object.h). %Function.prototype% is the
            // prototype of intrinsics, which is why this is not a trip. A
            // function with no statics object yet holds no key now, and one
            // it is given later is not watched: the one gap, for a program
            // that makes a function some array's prototype and then gives the
            // function an index-named property.
            const Value props = reinterpret_cast<FunctionHeader*>(hdr)->properties;
            if (props.isObject()) markObjectAsPrototype(arena, props.asObject<ObjectHeader>());
        } else {
            // An array or a proxy on a chain answers indices through no shape
            // the add trip could watch (object.h).
            tripArrayIndexHazard("a prototype that is no shape-carrying object");
        }
    }
    return root;
}

// A marked copy of `chain` (root-first) hung off `root` WITHOUT entering the
// root's transition table, so no other object can ever take a transition onto
// it: every node is private to the object that moves there, and every add it
// makes later transitions from a private node. `boxIndex`, when not
// UINT32_MAX, is a slot whose node is rebuilt boxed (the generalization step).
static Shape* privateMarkedChain(NonMovingArena& arena, Shape* root, const std::vector<Shape*>& chain,
                                 uint32_t boxIndex) {
    Shape* cur = root;
    for (Shape* node : chain) {
        SlotRepr repr = node->repr;
        if (node->slot_index == boxIndex) repr = SlotRepr::Boxed;
        Shape* next = arena.create<Shape>(cur, node->key, node->slot_index, root, node->enumerable,
                                          node->accessor, node->writable, node->configurable, repr);
        next->used_as_prototype = true;
        cur = next;
    }
    return cur;
}

static std::vector<Shape*> chainOf(Shape* shape) {
    std::vector<Shape*> chain;
    for (Shape* cur = shape; cur != nullptr && cur->key.valid(); cur = cur->parent) {
        chain.push_back(cur);
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

void Shape::markObjectAsPrototype(NonMovingArena& arena, ObjectHeader* obj) {
    Shape* protoShape = obj ? obj->shape : nullptr;
    if (!protoShape || protoShape->used_as_prototype) return;
    // From here every add to the object trips the hazard itself, so what it
    // holds already is what has to be asked about now.
    noteObjectBecomingPrototype(obj);
    // A dictionary shape is already private to the object, and no set-site
    // entry is ever filled with one — marking it in place is the private copy.
    if (protoShape->isDictionary()) {
        protoShape->used_as_prototype = true;
        return;
    }

    const std::vector<Shape*> chain = chainOf(protoShape);
    if (!chain.empty()) {
        // The same root, so the prototype, any brand a root carries (a Date's)
        // and the root's collector registration are all unchanged.
        Shape* const marked = privateMarkedChain(arena, protoShape->root, chain, UINT32_MAX);
        runtime::noteShapeLeave(protoShape, marked, ShapeLeave::BecomePrototype);
        obj->shape = marked;
        return;
    }

    // The object sits AT a root every object built on that prototype shares,
    // and the next property it gains would transition from there onto shared
    // nodes. It gets a root of its own — which the collector has to forward,
    // and can only forward when the prototype is on this thread's heap (a
    // test's private heap is not); otherwise it keeps the shared root, marked.
    // The new root also lives in `arena`, and the registry outlives a test's
    // private arena, so it is only registered when that arena is the thread's.
    const Value proto = protoShape->prototypeValue();
    const bool threadOwned = &arena == &runtime::rtArena() && runtime::rtHeap().contains(obj);
    if (!threadOwned ||
        (proto.isObject() && !runtime::rtHeap().contains(proto.asObject<HeapObjectHeader>()))) {
        protoShape->used_as_prototype = true;
        return;
    }
    Shape* fresh = arena.create<Shape>();
    fresh->prototype = proto;
    // The object keeps its storage, so its new root keeps the capacity.
    fresh->setInlineSlots(protoShape->inline_slots);
    fresh->used_as_prototype = true;
    runtime::rtRegisterRootShape(fresh);
    runtime::noteShapeLeave(protoShape, fresh, ShapeLeave::BecomePrototype);
    obj->shape = fresh;
}

namespace {

// The transition count at which a node's edges get a hash index. Below it a
// scan of the vector is as cheap as hashing the key.
constexpr size_t kTransitionIndexAt = 16;

// A string key by its content hash, so a heap string finds the edge an arena
// copy of it made; a symbol by its address, since a symbol is its identity.
// Equal keys hash equal; the scan still asks `matches`, so a collision costs a
// compare and nothing else.
uint32_t transitionKeyHash(PropertyKey key) {
    if (key.isString()) return key.string()->hash();
    const auto bits = reinterpret_cast<uintptr_t>(key.identity());
    return static_cast<uint32_t>(bits ^ (bits >> 32));
}

void indexTransition(Shape& shape, uint32_t position) {
    shape.transition_index->operator[](transitionKeyHash(shape.transitions[position].key))
        .push_back(position);
}

// Every edge whose key may be `key`, in insertion order — all of them on a
// short list, the hash bucket's on a long one.
template <typename F>
void forEachTransitionFor(const Shape& shape, PropertyKey key, F&& visit) {
    if (!shape.transition_index) {
        for (const ShapeTransition& trans : shape.transitions) visit(trans);
        return;
    }
    auto found = shape.transition_index->find(transitionKeyHash(key));
    if (found == shape.transition_index->end()) return;
    for (uint32_t position : found->second) visit(shape.transitions[position]);
}

}  // namespace

namespace {
// The thread's computed-key nodes so far (Shape::kComputedTransitionBudget).
thread_local uint32_t t_computedTransitions = 0;
}  // namespace

bool Shape::refusesNewTransition(PropertyKey added) const noexcept {
    if (!added.isString() || added.immortal()) return false;
    if (computed_transitions < kComputedTransitionLimit &&
        t_computedTransitions < kComputedTransitionBudget) {
        return false;
    }
    bool exists = false;
    forEachTransitionFor(*this, added, [&](const ShapeTransition& trans) {
        if (!exists && trans.key.matches(added)) exists = true;
    });
    return !exists;
}

Shape* Shape::addProperty(NonMovingArena& arena, Heap& heap, Rooted<Value>& name,
                          uint32_t& out_slot, bool is_enumerable, bool is_accessor,
                          bool is_writable, bool is_configurable, SlotRepr desired) {
    (void)heap;
    PropertyKey incoming = PropertyKey::fromValue(name.get());
    if (!incoming.valid()) {
        fatal("property name must be a string or a symbol");
    }
    // Shapes are immortal and non-moving, so they must never point into the
    // movable heap. A STRING key is copied into the arena, which is sound
    // exactly because a string key is matched by content — the copy is the
    // same key. A SYMBOL is already in the arena and must NOT be copied: a
    // symbol is matched by identity, so a copy would be a different key
    // that nothing could ever look up again (runtime/symbol.h).
    //
    // Done BEFORE the transition scan and not only on the create path, so the
    // key handed to `addPropertyKey` is always an arena key — which is what
    // lets that entry be the one place a node is minted.
    // An edge already taken for this key holds an arena copy of it, and a
    // string key is matched by content, so that copy is the key: the common
    // case (the hundredth object built the same way) then skips the intern
    // table's hash and probe.
    PropertyKey stored = incoming;
    if (!incoming.isSymbol()) {
        stored = PropertyKey{};
        forEachTransitionFor(*this, incoming, [&](const ShapeTransition& trans) {
            if (!stored.valid() && trans.key.matches(incoming)) stored = trans.key;
        });
        if (!stored.valid()) stored = PropertyKey::forString(StringHeader::internToArena(arena, incoming.string()));
    }
    const size_t edgesBefore = transitions.size();
    Shape* next = addPropertyKey(arena, stored, out_slot, is_enumerable, is_accessor, is_writable,
                                 is_configurable, desired);
    // A minted edge for a computed key counts against the bound (shape.h).
    if (transitions.size() != edgesBefore && incoming.isString() && !incoming.immortal()) {
        ++computed_transitions;
        ++t_computedTransitions;
    }
    return next;
}

Shape* Shape::addPropertyKey(NonMovingArena& arena, PropertyKey stored, uint32_t& out_slot,
                             bool is_enumerable, bool is_accessor, bool is_writable,
                             bool is_configurable, SlotRepr desired) {
    if (!stored.valid()) {
        fatal("property name must be a string or a symbol");
    }
    if (isDictionary()) {
        fatal("internal: a shape transition attempted on a dictionary-mode object");
    }
    // An accessor pair is two slots holding two function references; there is
    // no double representation to give it, and the width arithmetic below
    // assumes none exists.
    if (is_accessor) desired = SlotRepr::Boxed;

    // Two passes, because the representation is part of the key and the edge
    // this store WANTS may not be the one it gets. A double edge that has
    // already generalized once is a demotion this key has earned, and taking it
    // again would split the tree a second time for a field that has proven it
    // turns over — so the sticky bit sends the store to the boxed edge instead.
    Shape* next_shape = nullptr;
    Shape* boxed_edge = nullptr;
    forEachTransitionFor(*this, stored, [&](const ShapeTransition& trans) {
        if (trans.enumerable != is_enumerable || trans.accessor != is_accessor ||
            trans.writable != is_writable || trans.configurable != is_configurable ||
            !trans.key.matches(stored)) {
            return;
        }
        if (trans.repr == SlotRepr::Boxed) boxed_edge = trans.next_shape;
        if (trans.repr != desired) return;
        next_shape = trans.next_shape;
    });
    if (next_shape != nullptr && desired == SlotRepr::Double && next_shape->repr_generalized) {
        desired = SlotRepr::Boxed;
        next_shape = boxed_edge;
    }
    if (next_shape != nullptr) {
        out_slot = next_shape->slot_index;
    } else {
        uint32_t next_slot = nextSlotIndex(is_accessor ? 2u : 1u);

        if (slotsBefore(next_slot, inline_slots) + (is_accessor ? 1u : 0u) >= kDictionaryThreshold) {
            // The add paths (setProp, defineAccessor) convert the object to
            // dictionary mode before they get here, so this is a tripwire
            // for a caller that minted a transition without asking.
            fatal("internal: a shape transition past the dictionary threshold; "
                  "the caller should have moved the object to dictionary mode");
        }
        if (next_slot >= kSlotReprLimit) desired = SlotRepr::Boxed;

        out_slot = next_slot;
        next_shape =
            arena.create<Shape>(this, stored, next_slot, root, is_enumerable, is_accessor,
                                is_writable, is_configurable, desired);
        transitions.push_back(ShapeTransition{stored, next_shape, is_enumerable, is_accessor,
                                              is_writable, is_configurable, desired});
        if (transition_index) {
            indexTransition(*this, static_cast<uint32_t>(transitions.size() - 1));
        } else if (transitions.size() >= kTransitionIndexAt) {
            transition_index =
                std::make_unique<std::unordered_map<uint32_t, std::vector<uint32_t>>>();
            for (size_t i = 0; i < transitions.size(); ++i) {
                indexTransition(*this, static_cast<uint32_t>(i));
            }
        }
        // A stable layout's child is watched (shape.h `watched`). Only a NEW
        // node needs it: an existing child of a watched node was marked by
        // `Shape::watch` with the rest of the subtree.
        next_shape->watched = watched;
        if (desired == SlotRepr::Double) {
            ++runtime::slotReprMutableCounters().double_nodes;
        } else {
            ++runtime::slotReprMutableCounters().boxed_nodes;
        }
    }

    // An object that was a prototype before this add is still one after it, so
    // the mark has to reach the shape it lands on or the NEXT add would not
    // bump the epoch. On the reuse path as much as the create path: a shape is
    // marked when some object first becomes a prototype, which can be long
    // after its transitions were built by unrelated objects.
    if (used_as_prototype) next_shape->used_as_prototype = true;
    // Every add to a shape-mode object passes here, so this is where an index
    // key reaching a prototype is seen (dictionary adds: dictDefine).
    noteKeyAddedToPrototype(used_as_prototype, stored);
    // A telemetry count (embed's HostTelemetry), bumped without a locked
    // read-modify-write: every runtime property add passes here, and the lock
    // was most of a cached transition's cost. Two threads adding at once may
    // lose an increment, which a count read for reporting can afford.
    g_shapeTransitions.store(g_shapeTransitions.load(std::memory_order_relaxed) + 1,
                             std::memory_order_relaxed);
    return next_shape;
}

namespace runtime {
// A watched layout broke (shape_leave.h): the layout epoch, which is the
// prototype epoch, moves, so every compiled assumption that compared it —
// and every prototype-hit entry and transition cell, which is the price of
// sharing the word — is asked again. Steady-state frames break none
// (docs/aot-specialization.md, "S0").
void layoutBroken(const Shape* from, ShapeLeave why) {
    (void)from;
    (void)why;
    bumpProtoMutationEpoch();
}
}  // namespace runtime

void Shape::watch(Shape* shape) {
    if (shape == nullptr || shape->watched || shape->isDictionary()) return;
    std::vector<Shape*> todo{shape};
    while (!todo.empty()) {
        Shape* node = todo.back();
        todo.pop_back();
        if (node->watched) continue;
        node->watched = true;
        for (const ShapeTransition& t : node->transitions) {
            if (t.next_shape != nullptr && !t.next_shape->watched) todo.push_back(t.next_shape);
        }
    }
}

Shape* Shape::withSlotBoxed(NonMovingArena& arena, Shape* shape, uint32_t index) {
    if (shape == nullptr || shape->isDictionary() || !shape->slotIsDouble(index)) return shape;

    // Root-first order, because a transition tree is only walkable downward
    // from its root and the chain links the other way.
    std::vector<Shape*> chain;
    for (Shape* cur = shape; cur != nullptr && cur->key.valid(); cur = cur->parent) {
        chain.push_back(cur);
    }
    std::reverse(chain.begin(), chain.end());

    // A prototype's chain is private to it (markObjectAsPrototype), and the
    // rebuild keeps it so: through the shared transitions it would land on
    // nodes other objects reach, marked in place.
    if (shape->used_as_prototype) {
        for (Shape* node : chain) {
            if (node->slot_index == index && node->repr == SlotRepr::Double && !node->repr_generalized) {
                node->repr_generalized = true;
                ++runtime::slotReprMutableCounters().generalized_nodes;
            }
        }
        ++runtime::slotReprMutableCounters().generalizations;
        return privateMarkedChain(arena, shape->root, chain, index);
    }

    Shape* rebuilt = shape->root;
    for (Shape* node : chain) {
        SlotRepr repr = node->repr;
        if (node->slot_index == index && repr == SlotRepr::Double) {
            // The demotion, and the sticky mark that keeps the next object to
            // install this key from taking the same edge back.
            if (!node->repr_generalized) {
                node->repr_generalized = true;
                ++runtime::slotReprMutableCounters().generalized_nodes;
            }
            repr = SlotRepr::Boxed;
        }
        uint32_t slot = 0;
        rebuilt = rebuilt->addPropertyKey(arena, node->key, slot, node->enumerable, node->accessor,
                                          node->writable, node->configurable, repr);
        if (slot != node->slot_index) {
            // The rebuild walked the same names with the same widths in the
            // same order, so it must have handed out the same numbers. If it
            // did not, an object is about to be told its properties moved.
            fatal("internal: slot renumbered while generalizing a double slot");
        }
    }
    if (shape->used_as_prototype) rebuilt->used_as_prototype = true;
    ++runtime::slotReprMutableCounters().generalizations;
    return rebuilt;
}

PropertyKeyList Shape::ownKeysInInsertionOrder(bool enumerableOnly) const {
    PropertyKeyList keys;
    if (isDictionary()) {
        for (const DictEntry& e : dict->entries) {
            if (!e.live()) continue;
            if (enumerableOnly && !e.enumerable) continue;
            keys.push_back(e.key);
        }
        return keys;
    }
    for (const Shape* curr = this; curr != nullptr; curr = curr->parent) {
        if (!curr->key.valid()) continue;
        if (enumerableOnly && !curr->enumerable) continue;
        keys.push_back(curr->key);
    }
    // Collected newest-first walking toward the root; insertion order is
    // the reverse.
    std::reverse(keys.begin(), keys.end());
    return keys;
}

// THE LOOKUP MEMO. A transition-tree shape's answer for a key never changes:
// a node's key, slot and four attributes are fixed when the node is made (a
// representation change or a prototype's private chain builds NEW nodes,
// withSlotBoxed above), and nodes live in the arena for the thread's life. So
// (shape, key) -> answer is a pure function, and a lookup that misses every IC
// — the runtime's own property paths, `in`, hasOwnProperty, a computed store
// adding a key — need not walk a chain as long as the object's key count each
// time it asks. The memo is direct-mapped and per thread (shapes are), and it
// remembers absences as well as hits: an add first asks whether the key is
// there.
//
// The key half is compared by ADDRESS, so only an IMMORTAL key is memoized
// (PropertyKey::immortal: a symbol, or an arena string). A heap string's
// address can be reused by another string after a collection, and a memo
// keyed on it could answer for the wrong name; such a lookup walks, as before.
// A dictionary shape is mutable and is never memoized. The arena's teardown
// (rtTeardownThreadState) empties the memo, since its addresses go with it.
namespace {
struct LookupMemo {
    const Shape* shape;
    const void* key;
    PropertyInfo info;
    bool found;
};
constexpr uint32_t kLookupMemoBits = 13;
thread_local LookupMemo g_lookupMemo[1u << kLookupMemoBits];

inline uint64_t mixLookupHash(uint64_t h) noexcept {
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return h;
}

inline LookupMemo& lookupMemoFor(const Shape* shape, const void* key) noexcept {
    const uint64_t s = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(shape)) >> 4;
    const uint64_t k = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(key)) >> 3;
    const uint64_t h = mixLookupHash(s * 0x9E3779B97F4A7C15ULL ^ k * 0xC2B2AE3D27D4EB4FULL);
    return g_lookupMemo[h >> (64 - kLookupMemoBits)];
}

// Memo for non-immortal heap strings (`!name.immortal()`): keyed by
// `(shape, name->hash())`. Hits validate content equality against the matched
// shape node's immortal arena `StringHeader*`; short Latin-1 misses store the
// bytes inline so negative walks across deep shapes hit without retaining any
// movable heap address across collections.
struct StringLookupMemo {
    static constexpr uint32_t kMaxInlineMissLen = 30;
    const Shape* shape;
    const StringHeader* arenaKey;
    uint32_t keyHash;
    PropertyInfo info;
    char missChars[kMaxInlineMissLen];
    uint8_t missLen;
    bool isMiss;
};
constexpr uint32_t kStringLookupMemoBits = 12;
thread_local StringLookupMemo g_stringLookupMemo[1u << kStringLookupMemoBits];

inline StringLookupMemo& stringLookupMemoFor(const Shape* shape, uint32_t keyHash) noexcept {
    const uint64_t s = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(shape)) >> 4;
    const uint64_t h = mixLookupHash(s * 0x9E3779B97F4A7C15ULL ^
                                     static_cast<uint64_t>(keyHash) * 0xC2B2AE3D27D4EB4FULL);
    return g_stringLookupMemo[h >> (64 - kStringLookupMemoBits)];
}

// THE CHAIN INDEX. A lookup the memos miss walks the chain, newest property
// first, and a map the program keeps growing misses them constantly: each new
// key gives the object a new shape, which no memo entry is about, so the next
// touch of every older key walks down to it (pixi's `ids[source.uid] = n`
// over a few hundred uids: 500 walks a frame, 150 links each). So a walk that
// has gone `kIndexAfter` links without an answer stops at the next CHECKPOINT
// — a node whose slot is a multiple of `kIndexEvery` — and asks that node's
// index: every key from the checkpoint up, hashed, built the first time it is
// asked for. A walk is then at most kIndexAfter + 2 * kIndexEvery links and
// one probe however long the chain.
//
// An entry is the node that defines the key (the newest, as the walk would
// find it), and the answer is read off the node at the hit, so an attribute
// a node changes in place (its slot's representation) is never stale. Nodes
// are immortal and their parents fixed, so an index never goes stale either;
// it is dropped with the memos (`forgetLookupMemo`) because a later runtime
// on this thread may put another shape at a checkpoint's address.
constexpr uint32_t kIndexAfter = 16;
constexpr uint32_t kIndexEvery = 32;

uint32_t chainKeyHash(PropertyKey key) noexcept {
    if (const StringHeader* s = key.string()) return s->hash();
    return static_cast<uint32_t>(mixLookupHash(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(key.symbol()))));
}

// A pointer, made at the first long walk and deleted with the memos: the
// thread's teardown runs from another thread_local's destructor, and a map
// with a destructor of its own may already be gone by then (bro-headless
// aborted in free() at start-up on exactly that).
using ChainIndex = std::unordered_map<const Shape*, std::vector<const Shape*>>;
thread_local ChainIndex* t_chainIndex = nullptr;

const Shape* chainIndexFind(const std::vector<const Shape*>& cells, PropertyKey name) noexcept {
    const uint32_t mask = static_cast<uint32_t>(cells.size()) - 1;
    for (uint32_t i = chainKeyHash(name) & mask;; i = (i + 1) & mask) {
        const Shape* node = cells[i];
        if (node == nullptr || node->key.matches(name)) return node;
    }
}

const std::vector<const Shape*>& chainIndexFor(const Shape* checkpoint) {
    if (t_chainIndex == nullptr) t_chainIndex = new ChainIndex;
    auto [it, fresh] = t_chainIndex->try_emplace(checkpoint);
    std::vector<const Shape*>& cells = it->second;
    if (!fresh) return cells;
    uint32_t count = 0;
    for (const Shape* s = checkpoint; s != nullptr && s->key.valid(); s = s->parent) ++count;
    uint32_t cap = 8;
    while (cap < count * 2) cap *= 2;
    cells.assign(cap, nullptr);
    const uint32_t mask = cap - 1;
    for (const Shape* s = checkpoint; s != nullptr && s->key.valid(); s = s->parent) {
        uint32_t i = chainKeyHash(s->key) & mask;
        // Newest first, so a key already in is the one the walk would find.
        while (cells[i] != nullptr && !cells[i]->key.matches(s->key)) i = (i + 1) & mask;
        if (cells[i] == nullptr) cells[i] = s;
    }
    return cells;
}
}  // namespace

void Shape::forgetLookupMemo() noexcept {
    delete t_chainIndex;
    t_chainIndex = nullptr;
    for (LookupMemo& m : g_lookupMemo) m.shape = nullptr;
    for (StringLookupMemo& m : g_stringLookupMemo) {
        m.shape = nullptr;
        m.arenaKey = nullptr;
        m.isMiss = false;
    }
    runtime::rtForgetElemSetFastCache();
    runtime::rtForgetSpreadShapeHints();
    bumpProtoMutationEpoch();
}

bool Shape::lookupProperty(PropertyKey name, PropertyInfo& out) const noexcept {
    if (!name.valid()) return false;

    if (!isDictionary()) {
        if (!key.valid()) return false;
        const bool memoize = name.immortal();
        LookupMemo* memo = memoize ? &lookupMemoFor(this, name.identity()) : nullptr;
        if (memo && memo->shape == this && memo->key == name.identity()) {
            if (memo->found) out = memo->info;
            return memo->found;
        }
        StringLookupMemo* smemo = nullptr;
        uint32_t strHash = 0;
        if (!memoize && name.isString()) {
            const StringHeader* s = name.string();
            strHash = s->hash();
            smemo = &stringLookupMemoFor(this, strHash);
            if (smemo->shape == this && smemo->keyHash == strHash) {
                if (smemo->arenaKey != nullptr) {
                    if (smemo->arenaKey->equals(*s)) {
                        out = smemo->info;
                        return true;
                    }
                } else if (smemo->isMiss && s->isLatin1() && s->length == smemo->missLen &&
                           (smemo->missLen == 0 ||
                            std::memcmp(smemo->missChars, s->latin1Data(), smemo->missLen) == 0)) {
                    return false;
                }
            }
        }
        bool found = false;
        PropertyInfo info;
        const StringHeader* hitArenaKey = nullptr;
        const auto take = [&](const Shape* node) {
            info.slot = node->slot_index;
            info.enumerable = node->enumerable;
            info.accessor = node->accessor;
            info.writable = node->writable;
            info.configurable = node->configurable;
            info.repr = node->repr;
            hitArenaKey = node->key.string();
            found = true;
        };
        uint32_t walked = 0;
        for (const Shape* curr = this; curr != nullptr && curr->key.valid(); curr = curr->parent) {
            // The chain index (above), for the rest of a long walk.
            if (++walked > kIndexAfter && curr->slot_index % kIndexEvery == 0) {
                if (const Shape* node = chainIndexFind(chainIndexFor(curr), name)) take(node);
                break;
            }
            if (curr->key.matches(name)) {
                take(curr);
                break;
            }
        }
        if (memo) {
            *memo = LookupMemo{this, name.identity(), info, found};
        } else if (smemo) {
            if (found && hitArenaKey && hitArenaKey->isArena()) {
                smemo->shape = this;
                smemo->arenaKey = hitArenaKey;
                smemo->keyHash = strHash;
                smemo->info = info;
                smemo->isMiss = false;
            } else if (!found) {
                const StringHeader* s = name.string();
                if (s->isLatin1() && s->length <= StringLookupMemo::kMaxInlineMissLen) {
                    smemo->shape = this;
                    smemo->arenaKey = nullptr;
                    smemo->keyHash = strHash;
                    if (s->length > 0) {
                        std::memcpy(smemo->missChars, s->latin1Data(), s->length);
                    }
                    smemo->missLen = static_cast<uint8_t>(s->length);
                    smemo->isMiss = true;
                }
            }
        }
        if (found) out = info;
        return found;
    }

    const DictEntry* e = dict->find(name);
    if (!e) return false;
    out.slot = e->slot;
    out.enumerable = e->enumerable;
    out.accessor = e->accessor;
    out.writable = e->writable;
    out.configurable = e->configurable;
    out.repr = SlotRepr::Boxed;
    return true;
}

bool Shape::lookupProperty(PropertyKey name, uint32_t& out_slot) const noexcept {
    PropertyInfo info;
    if (!lookupProperty(name, info)) return false;
    out_slot = info.slot;
    return true;
}

}  // namespace bronze
