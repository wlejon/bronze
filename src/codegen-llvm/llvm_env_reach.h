#pragma once

// WHAT CAN SEE AN ENVIRONMENT SLOT — the observability half of stage R3.
//
// Stage R3 promotes an environment-record slot to an SSA value over a region of
// code, which is only sound if nothing inside that region can read or write the
// slot except the accesses the promotion rewrites. This header owns that
// question and nothing else; `llvm_env_promote.h` owns the regions and the
// rewrite.
//
// The question is asked of OPTIMIZED LLVM IR, after the inliner has run, and
// that is the whole reason the stage can win anything. bronze's canonical shape
// is a factory closure whose loop calls sibling closures over the same record
// (`bench/env_slot_kernel.js`); asked of the IL, every one of those calls
// observes the record and no region survives a single iteration. Asked of the
// IR after stage E1's direct edges have been inlined, the calls are gone and
// their accesses are ordinary loads and stores in the caller's loop — which the
// promotion rewrites along with the caller's own. An edge that did NOT inline is
// still a call, and still ends the region: the analysis reads what the binary
// will contain, so "inlined" and "not inlined" need no separate argument.
//
// Three facts make an LLVM-level answer possible at all:
//
//  - Every environment access generated code makes is a plain load or store
//    tagged with the `EnvRecordSlots` alias scope (llvm_env.cpp, llvm_alias.h),
//    through `inttoptr (and %bits, PAYLOAD_MASK)` plus a constant offset. So an
//    access names its record by an SSA value and its slot by a byte offset, and
//    both survive inlining — the callee's `env` argument IS the caller's record
//    value once the call is gone.
//  - The accepted language has no construct that can reify or mutate a record:
//    `with` is a parse error, `eval` runs with indirect semantics, and no helper
//    but the five `bronze_env_*` entries in the ABI registry touches a slot.
//  - An exception is a pending cell plus a RETURN, not an unwind, so there are
//    no `invoke` edges and a throw leaves a region through the same terminator
//    an ordinary return does.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_set>

#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

namespace llvm {
class CallBase;
class DataLayout;
class Function;
class Module;
class StoreInst;
}  // namespace llvm

namespace bronze::codegen_llvm {

// The metadata `emitEnvSet` attaches to a slot store whose value stage R2 proved
// is not a heap address. Absent means "not proven", never "proven false".
inline constexpr const char* kEnvNonPointerMD = "bronze.env.nonptr";

// One environment slot, as an optimized module names it: the i64 Value bits the
// access resolved its record from, and the byte offset of the slot inside that
// record.
//
// The record is an SSA VALUE and not a symbolic (scope, depth) pair on purpose.
// Two accesses that came from different source functions name the same slot
// here exactly when the inliner and CSE made them share a value, which is the
// only condition under which promoting them together is correct.
struct EnvSlotKey {
    llvm::Value* record = nullptr;
    uint64_t offset = 0;

    bool operator==(const EnvSlotKey& other) const {
        return record == other.record && offset == other.offset;
    }
};

struct EnvSlotKeyHash {
    size_t operator()(const EnvSlotKey& key) const {
        return std::hash<const void*>()(key.record) ^ (std::hash<uint64_t>()(key.offset) << 1);
    }
};

// Why a candidate region was not formed, or was cut short. Counted into a
// histogram (`BRONZE_ENV_PROMOTION_STATS=1`) because the shape of the refusals
// is what says where the next mechanism should go: a corpus dominated by
// `UnknownCall` wants callee summaries, one dominated by `AliasingEnvSlot`
// wants a record-identity analysis, one dominated by `HeapValueStore` wants the
// promoted value rooted rather than excluded.
enum class RegionEnd : uint8_t {
    // A call whose callee can reach an environment record — an escaped closure,
    // a dynamic call, a helper that runs user code.
    UnknownCall,
    // A call to one of the five `bronze_env_*` ABI entries, which touch a slot
    // by construction. Counted apart from `UnknownCall` because it is the
    // TDZ-marker path and the guard-failure path, not a user call.
    EnvHelperCall,
    // A call through a value: nothing enumerates what it can reach.
    IndirectCall,
    // A load or store alias analysis could not separate from the slot.
    AliasingMemory,
    // Another env-slot access at the SAME offset through a DIFFERENT record
    // value. Different offsets are disjoint bytes and are never a barrier;
    // the same offset through two values that might be one record is.
    AliasingEnvSlot,
    // The record value is not defined outside the loop, so the key names a
    // different slot on different iterations.
    RecordNotInvariant,
    // The loop is not in simplify form, or has no preheader, or has an exit
    // block shared with code outside the loop: there is nowhere to put the
    // entry load or a write-back that every exiting edge reaches.
    LoopShape,
    // A store into the slot whose value might be a heap pointer, in a region
    // that might collect. The heap slot is the collector's only view of that
    // pointer and promotion would make it stale.
    HeapValueStore,
    // The region is sound but the write-backs cost more loads and stores than
    // the accesses they replace.
    NoBenefit,
    Count
};

const char* regionEndName(RegionEnd cause);

// The slot a load or store names, if it names one. Recognizes exactly the shape
// `llvm_env.cpp` emits — an 8-byte access carrying the `EnvRecordSlots` alias
// scope, through constant-offset GEPs off `inttoptr (and %bits, PAYLOAD_MASK)`
// — and refuses everything else, including an access whose offset falls inside
// the record header rather than the slot array.
std::optional<EnvSlotKey> matchEnvSlotAccess(const llvm::Instruction& inst,
                                             const llvm::DataLayout& layout);

// Whether a memory access provably touches something that is not in the
// collector's heap, and therefore cannot be any environment record's slot.
//
// An `EnvHeader` is allocated by `EnvHeader::create(rtHeap(), ...)` and lives
// nowhere else, so a `GlobalVariable`'s bytes and an `alloca`'s bytes are never
// a record's. That is the same disjointness `tagStackAndControlAccesses` claims
// for the module's own tables and for the stack, and it is asked here because
// the two accesses this stage most often meets are one of each: a root-slot
// store into the GC frame's alloca, and the store into `bronze_exception_cell`
// that IS a `throw` in this runtime.
//
// The walk stops at `inttoptr` exactly as `llvm_alias.cpp`'s does, which is
// what keeps the whole JS heap — every pointer that came out of a Value payload
// — out of the claim.
bool accessIsOffHeap(const llvm::Instruction& inst, const llvm::DataLayout& layout);

// Whether the value a store writes into an environment slot cannot be a heap
// pointer, which is what decides whether promoting the slot can hide a live
// object from the collector.
//
// Two answers, and the first is stage R2 speaking:
//
//  - `!bronze.env.nonptr`, attached by `emitEnvSet` when the representation
//    plan calls the stored IL value `NeverPointer` (llvm_repr.h). This is not a
//    new claim: R2 already spends the same fact on giving that value no GC root
//    slot at all, which is strictly the stronger consequence.
//  - The structural fallback, for a store the optimizer rebuilt without the
//    metadata: a double-typed store, a `bitcast` of a double, a constant whose
//    NaN-box tag is not one of the three heap tags, or a `select`/`phi` all of
//    whose arms are one of those.
bool storedValueNeverPointer(const llvm::StoreInst& store);

// Which functions in the module cannot reach ANY environment record, computed
// once as a greatest fixpoint over the module's call graph.
//
// A defined function is env-blind unless it contains an env-scoped memory
// access, an indirect call, or a call to a function that is not env-blind. A
// declaration is env-blind only if it is `memory(none)` — which cannot touch a
// record because it cannot touch memory — or if it is on the named list in the
// .cpp, where each entry is an explicit soundness claim about one ABI helper.
//
// Starting optimistic and lowering is what makes a recursive pair of leaf
// helpers blind rather than mutually suspicious; the property is a greatest
// fixpoint on the call graph, the same shape `planRepr` solves for
// representations.
class EnvReach {
public:
    explicit EnvReach(const llvm::Module& llvmModule);

    // Whether this call site can be stepped over by a promotion: it cannot read
    // or write any environment-record slot. A `noreturn` call is blind for the
    // same reason a trap is: control does not come back, so no later reader
    // exists to be given a stale slot, and the process does not survive it.
    bool callIsBlind(const llvm::CallBase& call) const;

    // Whether this call site might run the collector. A blind call still can —
    // an allocation moves nothing here, but it can FREE what only a register
    // holds — so this is the second question every promoted key must answer.
    bool callMayCollect(const llvm::CallBase& call) const;

    // Which histogram bucket a non-blind call belongs to.
    static RegionEnd callCause(const llvm::CallBase& call);

private:
    std::unordered_set<const llvm::Function*> blind_;
};

}  // namespace bronze::codegen_llvm
