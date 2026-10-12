// The view table: for every tracked buffer, the views over it, so `resize`,
// `grow` and a detach refresh exactly those views rather than walking the
// heap to find them (typed_array.h, closeOrReopenViews).
//
// Which buffers are tracked: every one that can change length in place, from
// its creation; and, once a thread has detached a buffer it did not track,
// every buffer the thread makes from then on. A detach of an untracked buffer
// walks the whole heap, and a thread that detaches one (a `transfer`, a
// postMessage transfer list) usually goes on detaching one a message: a page
// relaying images to a worker paid a walk of its growing heap per image. A
// thread that never detaches pays nothing for the table; one that does pays a
// row per buffer and a word per view instead of a walk per detach.
//
// The rows live off the bronze heap, one per tracked buffer (the buffer's
// `viewTable` field names its row), and the collector visits every word in
// them WEAKLY: a row keeps neither its buffer nor its views alive, a moved one
// is updated in place, and a dead one reads `undefined` afterwards. The sweep
// after each collection drops the dead views and frees the row of a dead
// buffer for reuse. Most buffers have one view, so a row holds its first
// inline and only the others in a vector: a row costs no allocation of its
// own once the table has grown to the program's working set.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "runtime/heap.h"
#include "runtime/typed_array.h"

namespace bronze {

namespace {

uint64_t clearedBits() { return Value::fromUndefined().rawBits(); }

struct Row {
    uint64_t bufferBits = 0;
    uint64_t first = 0;               // the first live view, or the cleared word
    std::vector<uint64_t> more;       // any others
    bool live = false;
};

thread_local std::vector<Row> g_rows;
thread_local std::vector<uint32_t> g_freeRows;
// Set by the first walk for an untracked buffer's views: every buffer made on
// this thread afterwards is tracked from its creation.
thread_local bool g_trackNewBuffers = false;

void traceViewTable(brass::gc::Tracer& t) {
    const uint64_t cleared = clearedBits();
    for (Row& row : g_rows) {
        if (!row.live) continue;
        t.visit_weak(&row.bufferBits, cleared);
        t.visit_weak(&row.first, cleared);
        for (uint64_t& view : row.more) t.visit_weak(&view, cleared);
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
            row.more.clear();
            row.more.shrink_to_fit();
            g_freeRows.push_back(i);
            continue;
        }
        if (row.more.empty()) continue;
        std::erase(row.more, cleared);
        if (row.first == cleared && !row.more.empty()) {
            row.first = row.more.back();
            row.more.pop_back();
        }
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
    if (buf->bufferFlags & ArrayBufferHeader::kFlagViewsTracked) return;
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
    row.first = clearedBits();
    row.live = true;
    buf->viewTable = index;
    buf->bufferFlags |= ArrayBufferHeader::kFlagViewsTracked;
}

void trackNewBuffer(Heap& heap, ArrayBufferHeader* buf) {
    if (g_trackNewBuffers) trackBufferViews(heap, buf);
}

void trackNewBuffersFromNowOn() { g_trackNewBuffers = true; }

void registerBufferView(TypedArrayHeader* view) {
    auto* buf = view->buffer.asObject<ArrayBufferHeader>();
    if ((buf->bufferFlags & ArrayBufferHeader::kFlagViewsTracked) == 0) return;
    Row& row = g_rows[buf->viewTable];
    const uint64_t bits = Value::fromObject(view).rawBits();
    if (row.first == clearedBits()) row.first = bits;
    else row.more.push_back(bits);
}

bool refreshTrackedViews(ArrayBufferHeader* buf) {
    if ((buf->bufferFlags & ArrayBufferHeader::kFlagViewsTracked) == 0) return false;
    const uint64_t cleared = clearedBits();
    const Row& row = g_rows[buf->viewTable];
    if (row.first != cleared) Value(row.first).asObject<TypedArrayHeader>()->refreshLength();
    for (uint64_t bits : row.more) {
        if (bits == cleared) continue;
        Value(bits).asObject<TypedArrayHeader>()->refreshLength();
    }
    return true;
}

}  // namespace bronze
