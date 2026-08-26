#include "codegen-llvm/llvm_env_reach.h"

#include <array>
#include <string_view>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_alias.h"

namespace bronze::codegen_llvm {

namespace {

// The three NaN-box tags whose payload is a heap address (bronze_abi.h). Every
// other tag — Number, Int32, Bool, Null, Undefined, Hole, the uninitialized
// marker — is a value the collector never follows.
bool tagIsHeap(uint64_t bits) {
    const uint64_t tag = bits >> BRONZE_ABI_VALUE_TAG_SHIFT;
    return tag == BRONZE_ABI_TAG_OBJECT || tag == BRONZE_ABI_TAG_STRING ||
           tag == BRONZE_ABI_TAG_BIGINT;
}

// Whether an instruction's `!alias.scope` names the environment-slot family.
// CONTAINS rather than EQUALS: the inliner may add its own argument scopes to a
// list it copies, and an access that carries EnvRecordSlots plus something else
// is still an environment access. Nothing but `tagEnvRecordAccess` ever puts
// this scope on an instruction, so the converse never happens.
bool hasEnvScope(const llvm::Instruction& inst) {
    const llvm::MDNode* scopes = inst.getMetadata(llvm::LLVMContext::MD_alias_scope);
    if (scopes == nullptr) return false;
    const ScopedAliasInfo alias = getScopedAliasInfo(inst.getContext());
    const llvm::Metadata* envScope = alias.envScopeList->getOperand(0);
    for (unsigned i = 0; i < scopes->getNumOperands(); ++i) {
        if (scopes->getOperand(i) == envScope) return true;
    }
    return false;
}

// The intrinsics that annotate rather than compute. None of them can name a
// heap Value: `lifetime` and `invariant` take the alloca they bound, the debug
// and assume families take metadata and predicates. Listed rather than derived
// from `memory(...)`, because `memory(argmem: readwrite)` on a lifetime marker
// would otherwise read as a call that writes memory this analysis must fear.
bool isAnnotationIntrinsic(const llvm::CallBase& call) {
    const llvm::Function* callee = call.getCalledFunction();
    if (callee == nullptr || !callee->isIntrinsic()) return false;
    switch (callee->getIntrinsicID()) {
        case llvm::Intrinsic::lifetime_start:
        case llvm::Intrinsic::lifetime_end:
        case llvm::Intrinsic::invariant_start:
        case llvm::Intrinsic::invariant_end:
        case llvm::Intrinsic::assume:
        case llvm::Intrinsic::experimental_noalias_scope_decl:
            return true;
        default:
            return false;
    }
}

// THE ALLOWLIST, and every entry is a soundness claim about one ABI helper:
// this function cannot read or write an environment-record slot, and cannot
// call user JavaScript. Adding a name here is a claim of the same weight as
// dropping a guard; the runtime source is the only thing that can license one.
//
//  - `bronze_unbox_bool` and `bronze_truthy` read the heap object a Value
//    names. An environment record is never a Value user code or a helper can
//    hold: it travels as the `env` argument of a call and nowhere else.
//  - `bronze_is_nullish` touches no memory at all, and is here rather than left
//    to `memory(none)` because `BRONZE_NO_PURE_PREDICATES=1` takes the
//    attribute away and must not silently take the region with it.
//  - `bronze_env_create` and `bronze_create_function` READ a record's Value —
//    the parent link, the captured environment — and store it into an object
//    they have just allocated. Neither reads or writes a SLOT of it. They can
//    collect, which is the other question, and `callMayCollect` says so.
//  - `bronze_to_int32_f64` is arithmetic on a double.
//
// Three of the five `bronze_env_*` entries of the ABI registry are deliberately
// absent: `bronze_env_get`, `bronze_env_get_tdz` and `bronze_env_set` are a slot
// access by definition. `bronze_env_access_failed` needs no entry — it is
// `noreturn`, which is answered before this list is consulted.
constexpr std::array<std::string_view, 6> kEnvBlindHelpers{
    "bronze_create_function", "bronze_env_create", "bronze_is_nullish",
    "bronze_to_int32_f64",    "bronze_truthy",     "bronze_unbox_bool",
};

bool isNamedBlindHelper(llvm::StringRef name) {
    for (std::string_view known : kEnvBlindHelpers) {
        if (name == llvm::StringRef(known.data(), known.size())) return true;
    }
    return false;
}

// Whether a function's own body can reach an environment record, ignoring what
// its callees do — the callee half is the fixpoint's job.
bool bodyTouchesEnv(const llvm::Function& fn, const EnvReach& reach) {
    for (const llvm::BasicBlock& bb : fn) {
        for (const llvm::Instruction& inst : bb) {
            if (llvm::isa<llvm::LoadInst>(inst) || llvm::isa<llvm::StoreInst>(inst)) {
                if (hasEnvScope(inst)) return true;
                continue;
            }
            if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
                if (!reach.callIsBlind(*call)) return true;
            }
        }
    }
    return false;
}

// A bounded structural walk answering "these 64 bits are not a heap address".
// Depth-limited and cycle-guarded, because a loop-carried phi is exactly the
// shape a promoted slot produces and the walk would otherwise chase it.
bool neverPointerValue(const llvm::Value* value, unsigned depth,
                       std::unordered_set<const llvm::Value*>& seen) {
    if (value == nullptr || depth == 0) return false;
    if (!seen.insert(value).second) return true;  // already assumed on this path

    if (const auto* ci = llvm::dyn_cast<llvm::ConstantInt>(value)) {
        return !tagIsHeap(ci->getValue().getLimitedValue(UINT64_MAX));
    }
    if (const auto* cf = llvm::dyn_cast<llvm::ConstantFP>(value)) {
        return !tagIsHeap(cf->getValueAPF().bitcastToAPInt().getLimitedValue(UINT64_MAX));
    }
    if (llvm::isa<llvm::UndefValue>(value) || llvm::isa<llvm::PoisonValue>(value)) return true;

    const auto* inst = llvm::dyn_cast<llvm::Instruction>(value);
    if (inst == nullptr) return false;

    switch (inst->getOpcode()) {
        // A reinterpretation carries the question to the other type.
        case llvm::Instruction::BitCast:
            return neverPointerValue(inst->getOperand(0), depth - 1, seen);
        // Real floating-point arithmetic. Its result is a number or a QUIET
        // NaN, and every heap tag is a signaling NaN pattern — IEEE arithmetic
        // does not produce one. `box.f64`'s canonicalizing select is what keeps
        // an operand from carrying one in (llvm_convert.cpp).
        case llvm::Instruction::FAdd:
        case llvm::Instruction::FSub:
        case llvm::Instruction::FMul:
        case llvm::Instruction::FDiv:
        case llvm::Instruction::FRem:
        case llvm::Instruction::FNeg:
        case llvm::Instruction::SIToFP:
        case llvm::Instruction::UIToFP:
        case llvm::Instruction::FPTrunc:
        case llvm::Instruction::FPExt:
            return true;
        // A widened narrow integer has zero in the tag bits.
        case llvm::Instruction::ZExt:
            return inst->getOperand(0)->getType()->getScalarSizeInBits() <=
                   BRONZE_ABI_VALUE_TAG_SHIFT;
        // Boxing a non-number: `or disjoint <payload>, <tag> << 48`.
        case llvm::Instruction::Or: {
            const auto* rhs = llvm::dyn_cast<llvm::ConstantInt>(inst->getOperand(1));
            if (rhs == nullptr) return false;
            return !tagIsHeap(rhs->getValue().getLimitedValue(UINT64_MAX)) &&
                   neverPointerValue(inst->getOperand(0), depth - 1, seen);
        }
        case llvm::Instruction::Select:
            return neverPointerValue(inst->getOperand(1), depth - 1, seen) &&
                   neverPointerValue(inst->getOperand(2), depth - 1, seen);
        case llvm::Instruction::PHI: {
            const auto* phi = llvm::cast<llvm::PHINode>(inst);
            for (const llvm::Value* in : phi->incoming_values()) {
                if (!neverPointerValue(in, depth - 1, seen)) return false;
            }
            return true;
        }
        default:
            return false;
    }
}

}  // namespace

const char* regionEndName(RegionEnd cause) {
    switch (cause) {
        case RegionEnd::UnknownCall: return "unknown-call";
        case RegionEnd::EnvHelperCall: return "env-helper";
        case RegionEnd::IndirectCall: return "indirect-call";
        case RegionEnd::AliasingMemory: return "aliasing-memory";
        case RegionEnd::AliasingEnvSlot: return "aliasing-env-slot";
        case RegionEnd::RecordNotInvariant: return "record-not-invariant";
        case RegionEnd::LoopShape: return "loop-shape";
        case RegionEnd::HeapValueStore: return "heap-value-store";
        case RegionEnd::NoBenefit: return "no-benefit";
        case RegionEnd::Count: break;
    }
    return "?";
}

std::optional<EnvSlotKey> matchEnvSlotAccess(const llvm::Instruction& inst,
                                             const llvm::DataLayout& layout) {
    const llvm::Value* pointer = nullptr;
    llvm::Type* accessTy = nullptr;
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        if (!load->isSimple()) return std::nullopt;
        pointer = load->getPointerOperand();
        accessTy = load->getType();
    } else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (!store->isSimple()) return std::nullopt;
        pointer = store->getPointerOperand();
        accessTy = store->getValueOperand()->getType();
    } else {
        return std::nullopt;
    }
    if (!hasEnvScope(inst)) return std::nullopt;
    if (!accessTy->isSized() || layout.getTypeStoreSize(accessTy) != 8) return std::nullopt;

    // Peel the constant-offset GEPs the emitter wrote and the optimizer merged.
    uint64_t offset = 0;
    const llvm::Value* base = pointer;
    for (unsigned step = 0; step < 8; ++step) {
        const llvm::Value* stripped = base->stripPointerCasts();
        const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(stripped);
        if (gep == nullptr) {
            base = stripped;
            break;
        }
        llvm::APInt delta(layout.getIndexTypeSizeInBits(gep->getType()), 0);
        if (!gep->accumulateConstantOffset(layout, delta)) return std::nullopt;
        offset += delta.getZExtValue();
        base = gep->getPointerOperand();
    }

    const auto* toPtr = llvm::dyn_cast<llvm::IntToPtrInst>(base);
    if (toPtr == nullptr) return std::nullopt;

    // `and %bits, PAYLOAD_MASK` is the untagging every emitter writes; the
    // record is what went INTO it, so two accesses that untagged the same Value
    // agree on the key whether or not CSE happened to share the `and`.
    llvm::Value* record = toPtr->getOperand(0);
    if (const auto* andOp = llvm::dyn_cast<llvm::BinaryOperator>(record)) {
        if (andOp->getOpcode() == llvm::Instruction::And) {
            for (unsigned side = 0; side < 2; ++side) {
                const auto* mask = llvm::dyn_cast<llvm::ConstantInt>(andOp->getOperand(side));
                if (mask != nullptr &&
                    mask->getValue().getLimitedValue(UINT64_MAX) ==
                        BRONZE_ABI_VALUE_PAYLOAD_MASK) {
                    record = andOp->getOperand(1 - side);
                    break;
                }
            }
        }
    }

    // Inside the slot array, and slot-aligned. An access to the header — the
    // brand, the size, the parent link — is not a slot and is never promoted.
    if (offset < BRONZE_ABI_ENV_SLOTS_OFFSET) return std::nullopt;
    if (((offset - BRONZE_ABI_ENV_SLOTS_OFFSET) % 8) != 0) return std::nullopt;

    EnvSlotKey key;
    key.record = record;
    key.offset = offset;
    return key;
}

bool accessIsOffHeap(const llvm::Instruction& inst, const llvm::DataLayout& layout) {
    const llvm::Value* pointer = nullptr;
    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        pointer = load->getPointerOperand();
    } else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        pointer = store->getPointerOperand();
    } else {
        return false;
    }
    const llvm::Value* base = pointer;
    for (unsigned step = 0; step < 8; ++step) {
        const llvm::Value* stripped = base->stripPointerCasts();
        const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(stripped);
        if (gep == nullptr) {
            base = stripped;
            break;
        }
        llvm::APInt delta(layout.getIndexTypeSizeInBits(gep->getType()), 0);
        if (!gep->accumulateConstantOffset(layout, delta)) return false;
        base = gep->getPointerOperand();
    }
    return llvm::isa<llvm::GlobalVariable>(base) || llvm::isa<llvm::AllocaInst>(base);
}

bool storedValueNeverPointer(const llvm::StoreInst& store) {
    if (store.getMetadata(kEnvNonPointerMD) != nullptr) return true;
    std::unordered_set<const llvm::Value*> seen;
    return neverPointerValue(store.getValueOperand(), 8, seen);
}

EnvReach::EnvReach(const llvm::Module& llvmModule) {
    for (const llvm::Function& fn : llvmModule) {
        if (!fn.isDeclaration()) blind_.insert(&fn);
    }
    // Greatest fixpoint: assume every defined function blind, then lower any
    // whose body or callees contradict it, until nothing moves.
    for (bool changed = true; changed;) {
        changed = false;
        for (const llvm::Function& fn : llvmModule) {
            if (fn.isDeclaration()) continue;
            if (blind_.find(&fn) == blind_.end()) continue;
            if (bodyTouchesEnv(fn, *this)) {
                blind_.erase(&fn);
                changed = true;
            }
        }
    }
}

bool EnvReach::callIsBlind(const llvm::CallBase& call) const {
    // Control does not come back, so nothing downstream can be handed a stale
    // slot. bronze's `noreturn` helpers are fatals: the process does not
    // survive one, and a throw is a pending cell plus a return, never this.
    if (call.doesNotReturn()) return true;
    if (isAnnotationIntrinsic(call)) return true;
    if (call.doesNotAccessMemory()) return true;

    const llvm::Function* callee = call.getCalledFunction();
    if (callee == nullptr) return false;
    if (callee->isDeclaration()) return isNamedBlindHelper(callee->getName());
    return blind_.find(callee) != blind_.end();
}

bool EnvReach::callMayCollect(const llvm::CallBase& call) const {
    if (call.doesNotReturn()) return false;
    if (isAnnotationIntrinsic(call)) return false;
    // An allocation WRITES memory. A call that provably does not is a call that
    // provably did not allocate, and therefore did not collect.
    return !call.doesNotAccessMemory() && !call.onlyReadsMemory();
}

RegionEnd EnvReach::callCause(const llvm::CallBase& call) {
    const llvm::Function* callee = call.getCalledFunction();
    if (callee == nullptr) return RegionEnd::IndirectCall;
    if (callee->getName().starts_with("bronze_env_")) return RegionEnd::EnvHelperCall;
    return RegionEnd::UnknownCall;
}

}  // namespace bronze::codegen_llvm
