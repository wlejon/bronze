// The view table: for every buffer that can change length in place, the views
// over it, so `resize` and `grow` refresh exactly those views rather than
// walking the heap to find them (typed_array.h, closeOrReopenViews).
//
// The rows live off the bronze heap, one per tracked buffer (the buffer's
// `viewTable` field names its row), and the collector visits every word in
// them WEAKLY: a row keeps neither its buffer nor its views alive, a moved one
// is updated in place, and a dead one reads `undefined` afterwards. The sweep
// after each collection drops the dead views and frees the row of a dead
// buffer for reuse.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "runtime/heap.h"
#include "runtime/typed_array.h"

namespace bronze {

namespace {

struct Row {
    uint64_t bufferBits = 0;
    std::vector<uint64_t> views;
    bool live = false;
};

thread_local std::vector<Row> g_rows;
thread_local std::vector<uint32_t> g_freeRows;

uint64_t clearedBits() { return Value::fromUndefined().rawBits(); }

void traceViewTable(brass::gc::Tracer& t) {
    const uint64_t cleared = clearedBits();
    for (Row& row : g_rows) {
        if (!row.live) continue;
        t.visit_weak(&row.bufferBits, cleared);
        for (uint64_t& view : row.views) t.visit_weak(&view, cleared);
    }
}

// Allocates nothing on the bronze heap (add_post_collection_hook's contract).
void sweepViewTable() {
    const uint64_t cleared = clearedBits();
    for (uint32_t i = 0; i < g_rows.size(); ++i) {
        Row& row = g_rows[i];
        if (!row.live) continue;
        if (row.bufferBits == cleared) {
            row.live = false;
            row.views.clear();
            row.views.shrink_to_fit();
            g_freeRows.push_back(i);
            continue;
        }
        std::erase(row.views, cleared);
    }
}

// Registered on first use, for the cross-TU initialization-order reason
// weak_ref.cpp records at its own.
void ensureViewTable(Heap& heap) {
    static thread_local const bool registered = [&heap] {
        heap.add_tracer_source(traceViewTable);
        heap.add_post_collection_hook(sweepViewTable);
        return true;
    }();
    (void)registered;
}

}  // namespace

void trackBufferViews(Heap& heap, ArrayBufferHeader* buf) {
    ensureViewTable(heap);
    uint32_t index;
    if (!g_freeRows.empty()) {
        index = g_freeRows.back();
        g_freeRows.pop_back();
    } else {
        index = static_cast<uint32_t>(g_rows.size());
        g_rows.emplace_back();
    }
    Row& row = g_rows[index];
    row.bufferBits = Value::fromObject(buf).rawBits();
    row.live = true;
    buf->viewTable = index;
    buf->bufferFlags |= ArrayBufferHeader::kFlagViewsTracked;
}

void registerBufferView(TypedArrayHeader* view) {
    auto* buf = view->buffer.asObject<ArrayBufferHeader>();
    if ((buf->bufferFlags & ArrayBufferHeader::kFlagViewsTracked) == 0) return;
    g_rows[buf->viewTable].views.push_back(Value::fromObject(view).rawBits());
}

bool refreshTrackedViews(ArrayBufferHeader* buf) {
    if ((buf->bufferFlags & ArrayBufferHeader::kFlagViewsTracked) == 0) return false;
    const uint64_t cleared = clearedBits();
    for (uint64_t bits : g_rows[buf->viewTable].views) {
        if (bits == cleared) continue;
        Value(bits).asObject<TypedArrayHeader>()->refreshLength();
    }
    return true;
}

}  // namespace bronze
