#pragma once

// The GC root frame: what one function's frame holds, and how the frames of
// functions inlined into one another nest.
//
// A frame is the array of Value slots the collector walks. Laying it out is a
// pure function of the IL — no LLVM value is involved — which is what lets the
// backend plan every function's frame BEFORE it emits any of them, and that is
// what the region plan below needs.
//
// ---- why regions exist ------------------------------------------------
//
// A direct edge (a sibling-closure call, a direct method-call hit) asks to be
// inlined at the site, and the inliner grants it. What arrives in the caller is
// the callee's whole prologue as well as its body: another alloca, another
// linking of the frame list, another fetch of the thread's ABI block. Six
// inlined bodies in one loop meant six of each, per iteration.
//
// Those cost their own stores, but the second-order cost is larger: every
// environment-slot access re-reads its record out of ITS OWN frame, so twenty
// accesses to one record derive twenty unrelated pointers and no guard folds
// into the guard before it. One frame is what makes them one pointer.
//
// So a merged callee is emitted FRAMELESS: its slots are a region of the
// caller's frame, handed over as a pointer, and its view of the thread block is
// the caller's. The caller sizes its frame for its own slots plus the deepest
// region beneath it — MAX and not sum, because two merged calls in one caller
// are sequential and never both in flight, exactly as the argv region already
// is.
//
// The rooting contract is unchanged and that is the point: every value that had
// a slot still has one, at every safepoint, in a frame that is linked for the
// whole of the callee's execution. The collector sees one frame where it used
// to see two, with the same slots in it. A merged callee the inliner then
// REFUSES is still correct — it reads and writes the caller's frame through the
// pointer it was handed — which is what keeps the merge independent of whether
// inlining happens.

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "codegen-llvm/llvm_repr.h"
#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze::codegen_llvm {

inline constexpr uint32_t kNoFrameSlot = UINT32_MAX;

// One function's frame layout.
struct FramePlan {
    // Slot index per IL value, kNoFrameSlot for a value that needs no root.
    std::vector<uint32_t> slotOf;
    // First slot of the shared argument-vector region.
    uint32_t argvBase = 0;
    // How many slots the function needs for itself — its roots, its argv
    // region and the inline-`new` instance slot.
    uint32_t ownSlots = 0;
    // The inline `new` fast path's fresh-instance slot, or kNoFrameSlot.
    uint32_t constructSelfSlot = kNoFrameSlot;
    // A `dynamic` value was read from a block belonging to a different copy of
    // a guarded region than the one that defines it (il.h `CopyClass`), so the
    // overlay's premise — the two copies never coexist — does not hold and the
    // layout above cannot be trusted. The IL verifier refuses such a module
    // before this runs, so this is a tripwire on a bug in the pass rather than
    // a diagnosis of a program: the backend stops rather than emit a frame
    // whose slots two live values share.
    bool crossCopyUse = false;
};

// Lays out one IL function's frame. Pure: same IL in, same layout out.
//
// `repr` is what stage R2 adds: a `dynamic` value the representation plan
// proves is never a heap POINTER needs no slot at all (llvm_repr.h). The slot
// exists so the collector can forward what moved, and a boxed double, a boxed
// Int32 and `undefined` do not move — so the value's root store and the reload
// every use of it performs both go, and the frame shrinks by one slot for each.
// Under `BRONZE_NO_REPR_CODEGEN=1` the plan is empty and the layout is exactly
// what it was before the stage.
//
// `diags` is where the copy-class tripwire reports; passing none leaves
// `FramePlan::crossCopyUse` as the only signal, which is what a unit test wants
// and what no build should ever see set.
FramePlan planFrame(const il::Function& func, bool moduleHasNewTarget, const ReprPlan& repr,
                    DiagnosticSink* diags = nullptr);

// Which direct edges become one frame, and how big each function's frame has
// to be as a result.
struct RegionPlan {
    // Frame slots to allocate where this function OWNS its frame: its own
    // slots plus the deepest merged region under it.
    std::vector<uint32_t> totalSlots;
    // True where at least one merged edge targets this function, so it is
    // emitted frameless and its entry becomes a forwarder.
    std::vector<bool> isMergeTarget;
    // The (caller, callee) pairs that are merged. A pair absent from this set
    // keeps the shape it had before regions existed: an ordinary call to the
    // callee's own framed entry.
    std::set<std::pair<uint32_t, uint32_t>> merged;

    bool isMerged(uint32_t caller, uint32_t callee) const {
        return merged.count({caller, callee}) != 0;
    }
};

// Plans the regions over the module's inline-asking direct edges — the
// sibling-closure edges (`callEnvHops`) and the direct method-call edges
// (`directTarget`).
//
// An edge that would close a CYCLE is refused: a region's size is defined by
// what nests inside it, and a recursive nest has no finite size. Refusal is
// per (caller, callee) pair and leaves that site exactly as it was.
//
// `BRONZE_NO_FRAME_MERGE=1` refuses every edge, which is the A/B seam: the
// module then emits the pre-region shape out of the same binary.
RegionPlan planRegions(const il::Module& module, const std::vector<FramePlan>& plans);

// Whether the frame-merge seam is off for this invocation.
bool frameMergeDisabled();

}  // namespace bronze::codegen_llvm
