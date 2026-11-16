// `object.literal`'s slow path (il_op.h; il2mir/il_lowering_literal.cpp):
// the object built the generic way, and the shape it ended at remembered for
// the literal's fast path when that path would make the same object.

#include "abi/bronze_abi.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"

namespace bronze::runtime {

namespace {

// Whether an object of `shape` is what the fast path makes: `keys` as
// writable, enumerable, configurable data properties, boxed or double, at
// slots 0..n-1, all inline, on `root` and nothing else. A double slot
// (slot_repr.h) only at a position of `numbers`, whose value the fast path
// stores only as a Number (il2mir/il_lowering_literal.cpp): any other position
// it fills with whatever the literal holds.
bool fastPathMakes(const Shape* shape, const Shape* root, const uint64_t* keys, uint32_t count, uint64_t numbers) {
    if (shape == nullptr || shape->isDictionary() || shape->root != root) return false;
    if (shape->slotEnd() != count || count > shape->inline_slots) return false;
    for (uint32_t i = 0; i < count; ++i) {
        StringHeader* key = rtKeyHeader(static_cast<uint32_t>(keys[i]));
        PropertyInfo info;
        if (key == nullptr || !shape->lookupProperty(PropertyKey::forString(key), info)) return false;
        if (info.slot != i || info.accessor || !info.writable || !info.enumerable || !info.configurable) {
            return false;
        }
        if (shape->slotIsObject(i)) return false;
        if (shape->slotIsDouble(i) && (i >= 64 || ((numbers >> i) & 1) == 0)) return false;
    }
    return true;
}

}  // namespace

extern "C" {

// `cache` is the literal's two words (il_abi.h kLiteralShapesSymbol), `argv`
// its `count` values and then their `count` runtime key ids, `numbers` the
// positions the fast path stores only Numbers into a double slot at.
uint64_t bronze_object_literal(uint64_t* cache, uint32_t inlineSlots, uint32_t count, const uint64_t* argv,
                               uint64_t numbers) {
    recordHelperCall("bronze_object_literal");
    Shape* root = inlineSlots == 0 ? rtPlainObjectShape() : rtPlainObjectShapeSized(inlineSlots);
    Rooted<Value> obj{Value(inlineSlots == 0 ? bronze_create_object() : bronze_create_object_sized(inlineSlots))};
    // argv is a scanned block: each value is read after the define before it,
    // which may have collected.
    for (uint32_t i = 0; i < count; ++i) {
        bronze_prop_define(obj.get().rawBits(), static_cast<uint32_t>(argv[count + i]), argv[i], nullptr);
    }
    const Shape* shape = obj.get().asObject<ObjectHeader>()->shape;
    if (cache != nullptr && obj.get().asObject<HeapObjectHeader>()->flags == HeapKind::Plain &&
        fastPathMakes(shape, root, argv + count, count, numbers)) {
        cache[0] = reinterpret_cast<uint64_t>(root);
        cache[1] = reinterpret_cast<uint64_t>(shape);
    }
    return obj.get().rawBits();
}

}  // extern "C"

}  // namespace bronze::runtime
