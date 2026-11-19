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
#include <brass/runtime/coroutine.hpp>

#include "runtime/array.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/rt_state.h"
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
static_assert(offsetof(ArrayHeader, overflow) == 16 && offsetof(ArrayHeader, elements) == 40);

// An object's shape decides its [[Prototype]], which lives on the ROOT shape
// in the non-moving arena. A full collection keeps a prototype alive for as
// long as an object built on it is, by tracing it from every such object.
// A minor collection visits every root shape's prototype from rt_state.cpp's
// root source instead (an old instance is not rescanned by one), and
// verification checks only heap slots.
//
// A DICTIONARY object's keys are reached the same way: the table hangs off the
// object's private shape, off the heap, and a string key there is an old one
// (ObjectHeader::dictKeyToStore), alive for as long as its entry is. Old keys
// are never moved and a minor collection decides nothing old, so only a full
// collection visits them; the table belongs to this one object, so no other
// trace reads it at the same time. A key stored while a marking cycle is open
// is shaded where it is stored.
void traceShapePrototype(const uint64_t* words, Tracer& t) {
    if (t.purpose() != Tracer::Purpose::Full) return;
    auto* shape = reinterpret_cast<Shape*>(static_cast<uintptr_t>(words[1]));
    if (!shape || !shape->root) return;
    t.visit(reinterpret_cast<uint64_t*>(&shape->root->prototype));
    if (Dictionary* d = shape->dict) {
        for (DictEntry& e : d->entries) t.visit_address(e.key.traceSlot());
    }
}

void traceDynamicModuleFromWord(uint64_t bits, Tracer& t) {
    const Value v(bits);
    if (!v.isObject()) return;
    const auto target = reinterpret_cast<uintptr_t>(v.asObject<void>());
    if (!target || isBronzeLayout(brass::gc::header_of(target)->layout)) return;
    const auto* frame = reinterpret_cast<const brass::runtime::BrassCoroFrame*>(target);
    if (frame->is_done) return;
    const void* token = nullptr;
    if (const auto* body = brass::runtime::coro_body(frame)) {
        token = body->table.load(std::memory_order_acquire);
    } else {
        token = frame->fn_ptr;
    }
    if (token) runtime::rtTraceDynamicModuleToken(token, t);
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
    // The one string that is a Cell is a view (string.h): its buffer follows
    // the length and flags.
    if (h->tag == static_cast<uint16_t>(Tag::String)) return {2, words, false};
    if (h->tag != static_cast<uint16_t>(Tag::Object)) return {0, 0, false};
    if (h->flags == HeapKind::NumberBlock) return {0, 0, false};
    // Every object value begins with the shape word and the overflow block
    // (heap.h carriesShape); every word after the shape pointer is a Value or
    // a word no pointer tag can spell — a typed array's or an array's packed
    // length words are small integers, a function's code, instance shape and
    // name pointers are raw addresses with a zero top half.
    if (HeapKind::carriesShape(h->flags)) return {kFirstValueWord, words, true};
    // Everything else — an environment, an iterator record, a slot block, a
    // value block — is a Value in every word after the header.
    return {1, words, false};
}

bool isPlainObjectWords(const uint64_t* w, const ValueWords& v) {
    const auto* h = reinterpret_cast<const HeapObjectHeader*>(w);
    return v.end > 3 && h->tag == static_cast<uint16_t>(Tag::Object) && h->flags == HeapKind::Plain;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
inline void visitPlainValues(uint64_t* w, size_t end, Tracer& t) {
    const Shape* shape = reinterpret_cast<const ObjectHeader*>(w)->shape;
    if (!shape || shape->double_slots == 0) {
        t.visit_run(w + 2, end - 2);
        return;
    }
    const size_t inlineSlots = std::min<size_t>(end - 3, std::min<size_t>(shape->inline_slots, size_t{64}));
    const uint64_t mask = inlineSlots == 64 ? ~uint64_t{0} : ((uint64_t{1} << inlineSlots) - 1);
    const uint64_t skip = shape->double_slots & mask;
    if (skip == 0) {
        t.visit_run(w + 2, end - 2);
        return;
    }
    if (w[2] != Value::fromUndefined().rawBits()) t.visit(w + 2);
    uint64_t keep = (~skip) & mask;
    while (keep != 0) {
        const unsigned start = static_cast<unsigned>(__builtin_ctzll(keep));
        const uint64_t shifted = keep >> start;
        const uint64_t inv = ~shifted;
        const unsigned len = inv != 0 ? static_cast<unsigned>(__builtin_ctzll(inv))
                                      : static_cast<unsigned>(64 - start);
        t.visit_run(w + 3 + start, len);
        keep &= ~((len == 64 ? ~uint64_t{0} : ((uint64_t{1} << len) - 1)) << start);
    }
    if (3 + inlineSlots < end) t.visit_run(w + 3 + inlineSlots, end - (3 + inlineSlots));
}

// A full collection's trace of a Cell: its Values, and what only a full
// collection follows (a dynamic module's token, the shape's prototype).
BRONZE_NOINLINE void traceCellFull(uintptr_t payload, size_t payload_bytes, Tracer& t) {
    auto* w = reinterpret_cast<uint64_t*>(payload);
    const ValueWords v = valueWordsOf(payload, payload_bytes);
    if (runtime::rtHasDynamicModules()) {
        const auto* h = reinterpret_cast<const HeapObjectHeader*>(w);
        if (h->tag == static_cast<uint16_t>(Tag::Object) && h->flags == HeapKind::Function) {
            const auto* fn = reinterpret_cast<const FunctionHeader*>(w);
            if (fn->code) {
                runtime::rtTraceDynamicModuleToken(reinterpret_cast<const void*>(fn->code), t);
            }
        }
        for (size_t i = v.first; i < v.end; ++i) traceDynamicModuleFromWord(w[i], t);
    }
    if (v.first < v.end) {
        if (isPlainObjectWords(w, v)) visitPlainValues(w, v.end, t);
        else t.visit_run(w + v.first, v.end - v.first);
    }
    if (v.shaped) traceShapePrototype(w, t);
}

BRONZE_NOINLINE void traceCellSlow(uintptr_t payload, size_t payload_bytes, Tracer& t) {
    const ValueWords v = valueWordsOf(payload, payload_bytes);
    if (v.first < v.end) t.visit_run(reinterpret_cast<uint64_t*>(payload) + v.first, v.end - v.first);
}

void traceCell(uintptr_t payload, size_t payload_bytes, Tracer& t) {
    if (t.purpose() == Tracer::Purpose::Full) [[unlikely]] {
        return traceCellFull(payload, payload_bytes, t);
    }
    auto* w = reinterpret_cast<uint64_t*>(payload);
    const auto* h = reinterpret_cast<const HeapObjectHeader*>(payload);
    if (h->tag == static_cast<uint16_t>(Tag::Object)) {
        const size_t words = std::min<size_t>(h->size, payload_bytes) / sizeof(Value);
        switch (h->flags) {
            case HeapKind::NumberBlock: return;
            case HeapKind::Plain:
                if (words > 3) visitPlainValues(w, words, t);
                else if (words > 2 && w[2] != Value::fromUndefined().rawBits()) t.visit(w + 2);
                return;
            case HeapKind::Array:
                if (words > 2 && w[2] != Value::fromUndefined().rawBits()) t.visit(w + 2);
                if (words > 5) t.visit(w + 5);
                return;
            case HeapKind::ValueBlock:
            case HeapKind::SlotBlock:
                if (words > 1) t.visit_run(w + 1, words - 1);
                return;
            default: break;
        }
    }
    return traceCellSlow(payload, payload_bytes, t);
}

// The Value words of a Cell in payload bytes [begin, end): what a minor
// collection rescans for one dirty card of a large object (an array's
// elements, a big slot block), and one slice of a large object a parallel
// full collection splits between threads. The range holding offset 0 also
// carries the shape's prototype, which traceShapePrototype visits only in a
// full collection.
void traceCellRange(uintptr_t payload, size_t payload_bytes, size_t begin, size_t end, Tracer& t) {
    auto* w = reinterpret_cast<uint64_t*>(payload);
    const ValueWords v = valueWordsOf(payload, payload_bytes);
    const size_t first = std::max(v.first, begin / sizeof(Value));
    const size_t last = std::min(v.end, (end + sizeof(Value) - 1) / sizeof(Value));
    const bool traceDyn =
        (t.purpose() == Tracer::Purpose::Full) && runtime::rtHasDynamicModules();
    if (traceDyn && begin == 0) {
        const auto* h = reinterpret_cast<const HeapObjectHeader*>(payload);
        if (h->tag == static_cast<uint16_t>(Tag::Object) && h->flags == HeapKind::Function) {
            const auto* fn = reinterpret_cast<const FunctionHeader*>(payload);
            if (fn->code) {
                runtime::rtTraceDynamicModuleToken(reinterpret_cast<const void*>(fn->code), t);
            }
        }
    }
    if (traceDyn) {
        for (size_t i = first; i < last; ++i) traceDynamicModuleFromWord(w[i], t);
    }
    if (first < last) t.visit_run(w + first, last - first);
    if (v.shaped && begin == 0) traceShapePrototype(w, t);
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

brass::gc::LayoutId registerCustom(brass::gc::TraceFn fn, const char* name,
                                   brass::gc::TraceRangeFn range = nullptr) {
    brass::gc::LayoutDescriptor d;
    d.kind = brass::gc::LayoutKind::Custom;
    d.trace = fn;
    d.trace_range = range;
    d.type_tag = 0xB50E;
    d.name = name;
    return brass::gc::register_layout(d);
}

}  // namespace

const LayoutIds& layoutIds() {
    static const LayoutIds ids = [] {
        LayoutIds out;
        out.cell = registerCustom(&traceCell, "bronze.cell", &traceCellRange);
        out.weakLast = registerCustom(&traceWeakLast, "bronze.weak_last");
        out.ephemerons = registerCustom(&traceEphemerons, "bronze.ephemerons");
        brass::gc::LayoutDescriptor leaf;
        leaf.kind = brass::gc::LayoutKind::Leaf;
        leaf.type_tag = 0xB50E;
        leaf.name = "bronze.leaf";
        out.leaf = brass::gc::register_layout(leaf);
        brass::gc::LayoutDescriptor memento;
        memento.kind = brass::gc::LayoutKind::Leaf;
        memento.type_tag = 0xB50F;
        memento.name = "bronze.memento";
        out.memento = brass::gc::register_layout(memento);
        return out;
    }();
    return ids;
}

bool isBronzeLayout(brass::gc::LayoutId id) {
    const LayoutIds& ids = layoutIds();
    return id == ids.cell || id == ids.leaf || id == ids.weakLast || id == ids.ephemerons;
}

}  // namespace bronze::gc_detail
