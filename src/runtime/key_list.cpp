// The thread's live own-key snapshots, as a root source (key_list.h).

#include "runtime/key_list.h"

#include <brass/gc/tracer.hpp>

namespace bronze {

namespace {
// Doubly linked so a list can leave from anywhere: a fiber switched out with a
// snapshot alive interleaves its lists with the stack that resumed.
constinit thread_local KeyListLink* t_head = nullptr;
}  // namespace

KeyListLink::KeyListLink() noexcept : next_(t_head) {
    if (t_head) t_head->prev_ = this;
    t_head = this;
}

KeyListLink::~KeyListLink() {
    if (prev_) {
        prev_->next_ = next_;
    } else {
        t_head = next_;
    }
    if (next_) next_->prev_ = prev_;
}

void KeyListLink::visitAll(brass::gc::Tracer& t) noexcept {
    for (KeyListLink* list = t_head; list != nullptr; list = list->next_) {
        uintptr_t* data = nullptr;
        size_t count = 0;
        list->words(data, count);
        // A raw header address (tag 0): an old dictionary key is marked, an
        // arena key or a symbol lies outside the heap and is passed over.
        for (size_t i = 0; i < count; ++i) t.visit_address(&data[i]);
    }
}

}  // namespace bronze
