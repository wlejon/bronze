#pragma once

// The object layouts bronze registers with brass's collector (heap_trace.cpp).

#include <brass/gc/object.hpp>

namespace bronze::gc_detail {

struct LayoutIds {
    brass::gc::LayoutId cell;
    brass::gc::LayoutId leaf;
    brass::gc::LayoutId weakLast;
    brass::gc::LayoutId ephemerons;
    // An allocation memento's (bronze_abi_tls.h gc_memento_header): a leaf of
    // one word, never a bronze object.
    brass::gc::LayoutId memento;
};

// Registered once per process on first use; the ids are the same on every
// thread.
const LayoutIds& layoutIds();

// Whether an object of layout `id` is a bronze object (brass's own runtime may
// place objects of its own layouts on the same heap).
bool isBronzeLayout(brass::gc::LayoutId id);

}  // namespace bronze::gc_detail
