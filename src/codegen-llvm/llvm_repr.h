#pragma once

// WHAT A DYNAMIC VALUE IS MADE OF (stage R2).
//
// Stage R1 gave a SHAPE the ability to say, per slot, that the slot's eight
// bytes are a double. Nothing in generated code spent that: every store still
// tested its value for Number at the site, every `dynamic` SSA value still took
// a GC root slot, and a load still came back as bits a consumer had to test.
// This is the compile-time half — a fact about an IL VALUE, computed from the
// IL alone, that the emitters spend at the three places R1 left a test.
//
// TWO FACTS, AND THEY ARE NOT THE SAME QUESTION.
//
//   NeverPointer — the value's bits are never a heap address, whatever else
//                  they are. That is what the GC ROOT FRAME asks: a slot exists
//                  so the collector can forward what moved, and nothing that is
//                  not a pointer can move. A `dynamic` value that is provably
//                  not a pointer needs no slot, which removes its root store
//                  AND the reload every use of it performs.
//
//   Number       — the value's bits satisfy `bits <= NUMBER_MAX`, i.e. they ARE
//                  a double (bronze NaN-boxes directly; runtime/value.h). That
//                  is what R1's STORE-SIDE tests ask: a double slot may be
//                  written from generated code only when the value is a Number,
//                  because a boxed Number's bits are exactly the canonical
//                  double the slot's representation promises. A store that has
//                  proved it needs no test at all — which is the raw store.
//
// Number implies NeverPointer; the reverse does not (a boxed `Bool`, `null` or
// `undefined` is not a pointer and not a Number either).
//
// A third answer, Int32Boxed, is the one R1 named as its cost: `il::Type::I32`
// boxes to a `Tag::Int32` Value, whose bits are a tag and a payload rather than
// an f64, so it is NeverPointer but NOT Number and R1's arms refuse it — a
// helper call per store, forever, for a field written `this.n = i | 0`. Naming
// it separately is what lets the store sites convert it inline instead
// (`sitofp`, exactly what `slotReprCanonicalize` does in the helper).
//
// WHY THIS IS A PURE FUNCTION OF THE IL, like `planFrame` beside it. Nothing
// about LLVM enters, so the plan can be computed for every function before any
// body is emitted, and a test can assert the whole answer against an IL text
// rather than against generated code.
//
// THE SEAM. `BRONZE_NO_REPR_CODEGEN=1` is read by the COMPILER, once per
// invocation: every value comes back `Unknown`, so every site emits exactly the
// stage R1 sequence and every `dynamic` value keeps its root. It is a build-time
// A/B and not a run-time one deliberately — what it isolates is the emitted
// code, and a runtime flag could not change that.

#include <cstdint>
#include <vector>

#include "il/il.h"

namespace bronze::codegen_llvm {

// What the eight bytes of one IL value are known to be.
//
// Ordered so that a JOIN over incoming edges is a minimum: `Unknown` is the
// bottom every disagreement falls to, and `Number` is the strongest claim.
enum class ValueRepr : uint8_t {
    Unknown = 0,
    // Not a Number, not a pointer: a boxed Bool, `null`, `undefined`, the hole.
    NotPointer = 1,
    // `Tag::Int32`: a Number by 6.1.6.1 and by `typeof`, but NOT by the bit
    // test, so it is its own answer and every consumer here says which it wants.
    Int32Boxed = 2,
    // `bits <= NUMBER_MAX`: the bits ARE an IEEE double, NaN canonicalized.
    Number = 3,
};

inline bool reprIsNumber(ValueRepr r) { return r == ValueRepr::Number; }
inline bool reprIsInt32(ValueRepr r) { return r == ValueRepr::Int32Boxed; }
// Everything but `Unknown`. A pointer is the only thing the collector has to
// see, so this is exactly the GC frame's question.
inline bool reprNeverPointer(ValueRepr r) { return r != ValueRepr::Unknown; }

// One function's answer, plus the counters the build reports.
struct ReprPlan {
    // Per `il::ValueId`. Sized to `func.valueCount`.
    std::vector<ValueRepr> reprOf;
    // Values that are `dynamic` in the IL and NeverPointer here — the roots
    // this plan removes.
    uint32_t unrootedValues = 0;

    ValueRepr at(il::ValueId id) const {
        return id < reprOf.size() ? reprOf[id] : ValueRepr::Unknown;
    }
};

// Computes the plan for one function. Same IL in, same plan out.
ReprPlan planRepr(const il::Function& func);

// WHAT THE STAGE ACTUALLY BOUGHT, counted while the module is emitted.
//
// A representation stage is easy to believe in and hard to check: the arms are
// all conditional on a proof, so a stage that proves nothing emits exactly the
// code it replaced and every test still passes. These counters are the only
// thing that distinguishes "the fast arm is correct" from "the fast arm is
// ever taken", which is why they count EMITTED SITES rather than plan entries —
// an inlined callee's sites are emitted once per region and count once per
// region, because that is how many of them the binary has.
//
// Process-global on purpose: one `bronze build` is one module, and threading a
// counter through six emitters to report a number nothing else reads would cost
// more in signatures than the number is worth.
struct ReprStats {
    uint32_t functions = 0;
    // Plan entries: `dynamic` values proven to be a Number, and the subset of
    // NeverPointer values that therefore need no GC root.
    uint32_t provenNumber = 0;
    uint32_t rootsElided = 0;
    // Store sites emitted with no representation test, and those emitted with
    // an inline `sitofp` in place of R1's miss to the helper.
    uint32_t rawStores = 0;
    uint32_t sitofpStores = 0;
};

// The one instance. Mutable so emitters can bump it in place.
ReprStats& reprStats();

// Prints the counters to stderr when `BRONZE_REPR_CODEGEN_STATS=1`, and does
// nothing otherwise. Called once, after the module is emitted.
void reprStatsReport();

// The store-site question, asked about a PropSet/ElemSet's VALUE operand.
//
// Separate from `plan.at(value)` because a store may spend one more fact than
// the value itself carries: a `pin.guard <v>, number` immediately in front of
// the store proves the value is a Number ON THE PATH THAT REACHES THE STORE,
// and nowhere else. The guard's failing edge leaves an exception pending and
// `il::canThrow` puts the check between the two instructions, so the store is
// skipped — which is what makes a FLOW fact usable by a flow-insensitive plan
// without leaking into the value's own answer (`planRepr` deliberately does not
// record it, because the same value read in a handler is not proven anything).
//
// `blockIndex` and `instIndex` name the store inside `func`.
ValueRepr storeValueRepr(const il::Function& func, const ReprPlan& plan, size_t blockIndex,
                         size_t instIndex, il::ValueId value);

// Whether the stage R2 codegen seam is down for this invocation
// (`BRONZE_NO_REPR_CODEGEN=1`). Read once and cached.
bool reprCodegenDisabled();

}  // namespace bronze::codegen_llvm
