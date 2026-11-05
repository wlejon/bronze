#pragma once

// `Array.prototype.sort` with a comparator, merged in compiled code.
//
// The builtin (runtime/builtin_array_sort.cpp) snapshots the items into a
// rooted block, merges bottom-up and writes back. Its merge called the
// comparator through rtEnterJs once per comparison, and that entry — the
// trampoline, the C++ entry scope, the enter hook, the comparator's wrapper
// reached from C++ — cost more than the merge itself: three.js sorts its
// render list with a comparator every frame.
//
// So an image build gives each module one stub, `__bronze_sort_merge`, which
// the runtime enters ONCE per sort (bronze_array_sort_merged) and which runs
// the whole merge, calling the comparator the way compiled code calls any
// closure (il_lowering_direct.h, lower_closure_call_argv). Everything else —
// validating the comparator, the hole-skipping snapshot, the write-back with
// its frozen / sealed refusals — stays the builtin's, unchanged.
//
// The stub's merge is mergeRuns' exactly: the same widths, the same runs, the
// same comparisons in the same order (a comparator that logs its calls sees
// the same log), undefined last without a call, ToNumber of the answer with
// NaN as equal, and a tie taking the left run, which is the stability. It
// holds no Value across a call: the frame (BRONZE_ABI_SORT_FRAME_*) gives it
// addresses of root slots, which the collector updates in place, and it
// re-reads them after every comparison.

#include "il_ast.h"
#include <brass/mir/builder.hpp>

namespace il2mir {

constexpr const char* kSortMergeStub = "__bronze_sort_merge";

// Fills `fn` (env, this, argc, frame) -> the half the sorted items ended in.
void build_sort_merge_stub(Function* fn);

}  // namespace il2mir
