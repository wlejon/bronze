#include "runtime/shape.h"

#include <algorithm>

#include "runtime/fatal.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"

namespace bronze {

Shape* Shape::createRoot(NonMovingArena& arena, Value proto) {
    Shape* root = arena.create<Shape>();
    root->prototype = proto;
    // This is the ONE moment an object becomes a prototype — every route to one
    // (a class, `Object.create`, `Object.setPrototypeOf`) ends in a root shape
    // carrying it — so it is where the mark is applied rather than at each of
    // those callers.
    //
    // Only a plain object is marked: `cachedProtoHolder` refuses to walk
    // through anything else, so an array or a function used as a prototype
    // already misses for a reason that has nothing to do with the epoch.
    if (proto.isObject()) {
        auto* hdr = proto.asObject<HeapObjectHeader>();
        if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
            auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
            if (Shape* protoShape = obj->shape) {
                Shape* plainShape = runtime::rtCurrentPlainObjectShape();
                if (plainShape && protoShape == plainShape) {
                    Shape* dedicated = arena.create<Shape>();
                    dedicated->prototype = protoShape->prototype;
                    dedicated->used_as_prototype = true;
                    obj->shape = dedicated;
                    runtime::rtRegisterRootShape(dedicated);
                } else {
                    protoShape->used_as_prototype = true;
                }
            }
        }
    }
    return root;
}

Shape* Shape::addProperty(NonMovingArena& arena, Heap& heap, Rooted<Value>& name,
                          uint32_t& out_slot, bool is_enumerable, bool is_accessor,
                          bool is_writable, bool is_configurable) {
    (void)heap;
    PropertyKey incoming = PropertyKey::fromValue(name.get());
    if (!incoming.valid()) {
        fatal("property name must be a string or a symbol");
    }
    if (isDictionary()) {
        fatal("internal: a shape transition attempted on a dictionary-mode object");
    }

    Shape* next_shape = nullptr;
    for (const auto& trans : transitions) {
        if (trans.enumerable == is_enumerable && trans.accessor == is_accessor &&
            trans.writable == is_writable && trans.configurable == is_configurable &&
            trans.key.matches(incoming)) {
            out_slot = trans.next_shape->slot_index;
            next_shape = trans.next_shape;
            break;
        }
    }

    if (!next_shape) {
        uint32_t next_slot = nextSlotIndex();

        if (next_slot + (is_accessor ? 1u : 0u) >= kDictionaryThreshold) {
            fatal("object has too many properties for the shape transition tree; "
                  "dictionary mode is not implemented");
        }

        out_slot = next_slot;
        // Shapes are immortal and non-moving, so they must never point into the
        // movable heap. A STRING key is copied into the arena, which is sound
        // exactly because a string key is matched by content — the copy is the
        // same key. A SYMBOL is already in the arena and must NOT be copied: a
        // symbol is matched by identity, so a copy would be a different key
        // that nothing could ever look up again (runtime/symbol.h).
        PropertyKey stored =
            incoming.isSymbol()
                ? incoming
                : PropertyKey::forString(StringHeader::internToArena(arena, incoming.string()));
        next_shape =
            arena.create<Shape>(this, stored, next_slot, root, is_enumerable, is_accessor,
                                is_writable, is_configurable);
        transitions.push_back(ShapeTransition{stored, next_shape, is_enumerable, is_accessor,
                                             is_writable, is_configurable});
    }

    // An object that was a prototype before this add is still one after it, so
    // the mark has to reach the shape it lands on or the NEXT add would not
    // bump the epoch. On the reuse path as much as the create path: a shape is
    // marked when some object first becomes a prototype, which can be long
    // after its transitions were built by unrelated objects.
    if (used_as_prototype) next_shape->used_as_prototype = true;
    return next_shape;
}

std::vector<PropertyKey> Shape::ownKeysInInsertionOrder(bool enumerableOnly) const {
    std::vector<PropertyKey> keys;
    if (isDictionary()) {
        for (const DictEntry& e : dict->entries) {
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

bool Shape::lookupProperty(PropertyKey name, PropertyInfo& out) const noexcept {
    if (!name.valid()) return false;

    if (isDictionary()) {
        const DictEntry* e = dict->find(name);
        if (!e) return false;
        out.slot = e->slot;
        out.enumerable = e->enumerable;
        out.accessor = e->accessor;
        out.writable = e->writable;
        out.configurable = e->configurable;
        return true;
    }

    const Shape* curr = this;
    while (curr != nullptr) {
        if (curr->key.matches(name)) {
            out.slot = curr->slot_index;
            out.enumerable = curr->enumerable;
            out.accessor = curr->accessor;
            out.writable = curr->writable;
            out.configurable = curr->configurable;
            return true;
        }
        curr = curr->parent;
    }
    return false;
}

bool Shape::lookupProperty(PropertyKey name, uint32_t& out_slot) const noexcept {
    PropertyInfo info;
    if (!lookupProperty(name, info)) return false;
    out_slot = info.slot;
    return true;
}

}  // namespace bronze
