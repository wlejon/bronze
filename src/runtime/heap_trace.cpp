// How brass's collector finds the references in a bronze object: the object
// layouts bronze registers (heap.h, GcLayout) and their trace functions.
//
// Every bronze Value is NaN-boxed, and the heap's reference tags are exactly
// the pointer tags (Object, String, Symbol, BigInt), so a trace can visit any
// word that holds a Value: a number, a boolean, a raw host pointer (top 16
// bits zero) or a packed small-integer pair is not a reference and the
// collector leaves it alone. What a trace must NOT visit is a word that is not
// a Value and could carry a pointer tag: an ArrayBuffer's bytes, which are
// arbitrary. The shape pointer is skipped because the prototype behind it is
// traced separately.

#include "runtime/heap_trace.h"

#include <algorithm>

#include "runtime/object.h"
#include "runtime/shape.h"

namespace bronze::gc_detail {

namespace {

using brass::gc::Tracer;

// Words of the ordinary-object prefix: the header, the shape, the overflow
// reference and the inline property slots.
constexpr size_t kPrefixWords = sizeof(ObjectHeader) / sizeof(Value) + ObjectHeader::kInlineSlots;
// The first word after the shape pointer.
constexpr size_t kFirstValueWord = 2;
static_assert(offsetof(ObjectHeader, shape) == 8 && offsetof(ObjectHeader, overflow) == 16);

// An object's shape decides its [[Prototype]], which lives on the ROOT shape
// in the non-moving arena. A full collection keeps a prototype alive for as
// long as an object built on it is, by tracing it from every such object.
// A minor collection visits every root shape's prototype from rt_state.cpp's
// root source instead (an old instance is not rescanned by one), and
// verification checks only heap slots.
void traceShapePrototype(const uint64_t* words, Tracer& t) {
    if (t.purpose() != Tracer::Purpose::Full) return;
    auto* shape = reinterpret_cast<Shape*>(static_cast<uintptr_t>(words[1]));
    if (!shape || !shape->root) return;
    t.visit(reinterpret_cast<uint64_t*>(&shape->root->prototype));
}

// The words of the object that hold Values, as [first, end).
struct ValueWords {
    size_t first;
    size_t end;
    bool shaped;
};

ValueWords valueWordsOf(uintptr_t payload, size_t payload_bytes) {
    const auto* h = reinterpret_cast<const HeapObjectHeader*>(payload);
    const size_t words = std::min<size_t>(h->size, payload_bytes) / sizeof(Value);
    if (h->tag == static_cast<uint16_t>(Tag::RawBytes)) {
        // An ArrayBuffer's ordinary-object prefix, and nothing after it: its
        // lengths, flags, external pointer and bytes are not Values.
        if (h->flags != HeapKind::ArrayBuffer) return {0, 0, false};
        return {kFirstValueWord, std::min(words, kPrefixWords), true};
    }
    if (h->tag != static_cast<uint16_t>(Tag::Object)) return {0, 0, false};
    // A kind that carries a shape begins with an ObjectHeader; every word
    // after the shape pointer is a Value (a typed array's packed length words
    // are small integers, far below any pointer tag).
    if (HeapKind::carriesShape(h->flags)) return {kFirstValueWord, words, true};
    // Everything else — a function, an array, an environment, an iterator
    // record, a proxy, a namespace, a slot block, a value block — is a Value
    // in every word after the header.
    return {1, words, false};
}

void traceCell(uintptr_t payload, size_t payload_bytes, Tracer& t) {
    auto* w = reinterpret_cast<uint64_t*>(payload);
    const ValueWords v = valueWordsOf(payload, payload_bytes);
    for (size_t i = v.first; i < v.end; ++i) t.visit(w + i);
    if (v.shaped) traceShapePrototype(w, t);
}

// A WeakRef: an ordinary object whose last internal slot is its target.
void traceWeakLast(uintptr_t payload, size_t payload_bytes, Tracer& t) {
    auto* w = reinterpret_cast<uint64_t*>(payload);
    const ValueWords v = valueWordsOf(payload, payload_bytes);
    if (v.end <= v.first) return;
    for (size_t i = v.first; i + 1 < v.end; ++i) t.visit(w + i);
    t.visit_weak(w + v.end - 1, Value::fromUndefined().rawBits());
    if (v.shaped) traceShapePrototype(w, t);
}

// A WeakMap's or WeakSet's entry table: (key, value) pairs after the header.
// A tombstone's key is the Hole, which is not a reference and so counts as
// alive; its value is undefined. A pair whose key dies becomes one.
void traceEphemerons(uintptr_t payload, size_t payload_bytes, Tracer& t) {
    auto* w = reinterpret_cast<uint64_t*>(payload);
    const auto* h = reinterpret_cast<const HeapObjectHeader*>(payload);
    const size_t words = std::min<size_t>(h->size, payload_bytes) / sizeof(Value);
    const uint64_t hole = Value::fromHole().rawBits();
    const uint64_t undefined = Value::fromUndefined().rawBits();
    for (size_t i = 1; i + 1 < words; i += 2) {
        if (w[i] == hole) continue;
        t.visit_ephemeron(w + i, w + i + 1, hole, undefined);
    }
}

brass::gc::LayoutId registerCustom(brass::gc::TraceFn fn, const char* name) {
    brass::gc::LayoutDescriptor d;
    d.kind = brass::gc::LayoutKind::Custom;
    d.trace = fn;
    d.type_tag = 0xB50E;
    d.name = name;
    return brass::gc::register_layout(d);
}

}  // namespace

const LayoutIds& layoutIds() {
    static const LayoutIds ids = [] {
        LayoutIds out;
        out.cell = registerCustom(&traceCell, "bronze.cell");
        out.weakLast = registerCustom(&traceWeakLast, "bronze.weak_last");
        out.ephemerons = registerCustom(&traceEphemerons, "bronze.ephemerons");
        brass::gc::LayoutDescriptor leaf;
        leaf.kind = brass::gc::LayoutKind::Leaf;
        leaf.type_tag = 0xB50E;
        leaf.name = "bronze.leaf";
        out.leaf = brass::gc::register_layout(leaf);
        return out;
    }();
    return ids;
}

bool isBronzeLayout(brass::gc::LayoutId id) {
    const LayoutIds& ids = layoutIds();
    return id == ids.cell || id == ids.leaf || id == ids.weakLast || id == ids.ephemerons;
}

}  // namespace bronze::gc_detail
