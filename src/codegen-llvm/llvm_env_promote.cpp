#include "codegen-llvm/llvm_env_promote.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Transforms/Utils/LoopUtils.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

#include "abi/bronze_abi.h"
#include "codegen-llvm/llvm_alias.h"

namespace bronze::codegen_llvm {

namespace {

// One recognized environment-slot access, with the key it names. `inst` becomes
// null once a region has rewritten it, which is how a key with accesses in two
// sibling loops stops offering the first loop's accesses to the second.
struct Access {
    llvm::Instruction* inst = nullptr;
    EnvSlotKey key;
    bool isStore = false;
    bool valueNeverPointer = false;
};

// What one instruction is, to a region scan. Computed once per function so a
// per-key scan costs a walk over a short list rather than a re-analysis.
enum class InstKind : uint8_t {
    Ignore,       // nothing that can touch memory, or a call proven env-blind
    EnvAccess,    // a recognized slot access; `key` says which slot
    Observer,     // a call the analysis cannot see through
    OtherMemory,  // a load or store alias analysis must be asked about
};

struct InstFact {
    llvm::Instruction* inst = nullptr;
    InstKind kind = InstKind::Ignore;
    EnvSlotKey key;
    RegionEnd cause = RegionEnd::UnknownCall;
    bool mayCollect = false;
};

// A region, once one has been formed: where the entry load goes, where the
// write-backs go, and which accesses it owns.
struct Region {
    llvm::Instruction* entryPoint = nullptr;
    llvm::SmallVector<llvm::Instruction*, 4> writeBackPoints;
    llvm::SmallVector<Access*, 8> accesses;
    bool overLoop = false;
    bool hasStore = false;
};

bool envPromotionStatsEnabled() {
    const char* env = std::getenv("BRONZE_ENV_PROMOTION_STATS");
    return env != nullptr && std::strcmp(env, "1") == 0;
}

// The two 8-byte scalar types a slot access is ever seen with: the i64 the
// emitter writes, and the double InstCombine rewrites it to once it has proved
// the value is one. A slot access of any other type is refused rather than
// converted, because the conversion would be a claim about bits.
bool promotableAccessType(const llvm::Type* ty) {
    return ty->isIntegerTy(64) || ty->isDoubleTy();
}

// The address of a key's slot, materialized at `builder`'s insertion point.
// The same three instructions `emitEnvSlotPtrUnguarded` writes, and no guard —
// see the licence in llvm_env_promote.h.
llvm::Value* slotAddress(llvm::IRBuilder<>& builder, const EnvSlotKey& key) {
    llvm::LLVMContext& ctx = builder.getContext();
    llvm::Type* i8Ty = llvm::Type::getInt8Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::getUnqual(ctx);
    llvm::Value* payload =
        builder.CreateAnd(key.record, builder.getInt64(BRONZE_ABI_VALUE_PAYLOAD_MASK));
    llvm::Value* header = builder.CreateIntToPtr(payload, ptrTy, "env.promote.hdr");
    return builder.CreateConstInBoundsGEP1_32(i8Ty, header, static_cast<unsigned>(key.offset));
}

// A block whose terminator is `unreachable` takes no write-back. It is a guard
// tripwire or a fatal; the process does not survive it, and the record pointer
// it would store through is the one the guard just rejected.
bool exitTakesWriteBack(const llvm::BasicBlock& block) {
    return !llvm::isa<llvm::UnreachableInst>(block.getTerminator());
}

// Everything one function's promotion needs, gathered once.
class FunctionScan {
public:
    FunctionScan(llvm::Function& fn, const EnvReach& reach, llvm::AAResults& aa)
        : fn_(fn), aa_(aa) {
        const llvm::DataLayout& layout = fn.getParent()->getDataLayout();
        for (llvm::BasicBlock& block : fn) {
            std::vector<InstFact>& facts = facts_[&block];
            for (llvm::Instruction& inst : block) {
                InstFact fact;
                fact.inst = &inst;
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
                    fact.mayCollect = reach.callMayCollect(*call);
                    if (!reach.callIsBlind(*call)) {
                        fact.kind = InstKind::Observer;
                        fact.cause = EnvReach::callCause(*call);
                        if (!hasObserver_) firstObserverCause_ = fact.cause;
                        hasObserver_ = true;
                    }
                } else if (inst.mayReadOrWriteMemory()) {
                    std::optional<EnvSlotKey> key = matchEnvSlotAccess(inst, layout);
                    if (key.has_value()) {
                        fact.kind = InstKind::EnvAccess;
                        fact.key = *key;
                    } else if (!accessIsOffHeap(inst, layout)) {
                        fact.kind = InstKind::OtherMemory;
                        fact.cause = RegionEnd::AliasingMemory;
                    }
                }
                if (fact.kind == InstKind::Ignore && !fact.mayCollect) continue;
                if (fact.kind == InstKind::EnvAccess) {
                    Access access;
                    access.inst = &inst;
                    access.key = fact.key;
                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
                        access.isStore = true;
                        access.valueNeverPointer = storedValueNeverPointer(*store);
                    }
                    accesses_.push_back(access);
                }
                facts.push_back(fact);
            }
        }
        for (Access& access : accesses_) byKey_[access.key].push_back(&access);
    }

    llvm::Function& function() const { return fn_; }
    const std::vector<Access>& accesses() const { return accesses_; }
    const std::unordered_map<EnvSlotKey, std::vector<Access*>, EnvSlotKeyHash>& byKey() const {
        return byKey_;
    }

    // Whether any call in the function is one the analysis cannot see through,
    // and which kind the first one was. Asked before a whole-function region is
    // attempted, so a function with a single opaque call costs one flag rather
    // than one whole-body scan per key.
    bool hasObserver() const { return hasObserver_; }
    RegionEnd firstObserverCause() const { return firstObserverCause_; }

    // An access this pass has rewritten is no longer in the module, and must
    // stop counting as an observer of every OTHER key it shares an offset with.
    void markErased(const llvm::Instruction* inst) { erased_.insert(inst); }

    // An access this pass CREATED — a region's entry load, a region's
    // write-back — is an ordinary access from the next key's point of view.
    // Without this, a region promoted over an outer loop would not see the
    // write-back an inner region put in that loop, and two keys that share an
    // offset through two record values would stop being observers of each
    // other exactly where they are one record.
    void recordNewAccess(llvm::Instruction* inst, const EnvSlotKey& key) {
        InstFact fact;
        fact.inst = inst;
        fact.kind = InstKind::EnvAccess;
        fact.key = key;
        facts_[inst->getParent()].push_back(fact);
    }

    // Whether anything in `blocks` can observe `key`'s slot. `location` names
    // the slot, taken from one of its own accesses.
    //
    // The three rules, in the order they are cheapest to apply:
    //   - another access to the same key is not an observer, it IS the region;
    //   - a recognized env access at a DIFFERENT offset is disjoint bytes,
    //     whichever record it names, so it is never an observer — two 8-byte
    //     slot accesses at different 8-aligned offsets cannot overlap in one
    //     record, and cannot overlap at all in two;
    //   - a recognized env access at the SAME offset through a different record
    //     value might be the same record, and is an observer.
    bool blocksAreClean(llvm::ArrayRef<llvm::BasicBlock*> blocks, const EnvSlotKey& key,
                        const llvm::MemoryLocation& location, RegionEnd& causeOut,
                        bool& mayCollectOut) {
        mayCollectOut = false;
        for (llvm::BasicBlock* block : blocks) {
            auto found = facts_.find(block);
            if (found == facts_.end()) continue;
            for (const InstFact& fact : found->second) {
                if (erased_.contains(fact.inst)) continue;
                if (fact.mayCollect) mayCollectOut = true;
                switch (fact.kind) {
                    case InstKind::Ignore:
                        break;
                    case InstKind::EnvAccess:
                        if (fact.key.offset == key.offset && fact.key.record != key.record) {
                            causeOut = RegionEnd::AliasingEnvSlot;
                            return false;
                        }
                        break;
                    case InstKind::Observer:
                        causeOut = fact.cause;
                        return false;
                    case InstKind::OtherMemory:
                        if (llvm::isModOrRefSet(aa_.getModRefInfo(fact.inst, location))) {
                            causeOut = RegionEnd::AliasingMemory;
                            return false;
                        }
                        break;
                }
            }
        }
        return true;
    }

private:
    llvm::Function& fn_;
    llvm::AAResults& aa_;
    llvm::DenseMap<const llvm::BasicBlock*, std::vector<InstFact>> facts_;
    llvm::DenseSet<const llvm::Instruction*> erased_;
    std::vector<Access> accesses_;
    std::unordered_map<EnvSlotKey, std::vector<Access*>, EnvSlotKeyHash> byKey_;
    bool hasObserver_ = false;
    RegionEnd firstObserverCause_ = RegionEnd::UnknownCall;
};

// The collector's question, asked of a formed region: would promoting this key
// hide a live object from a collection that happens inside the region?
//
// No, if the region writes nothing (the heap slot stays exactly right), or if
// nothing in the region can collect, or if every value the region writes is
// provably not a heap address. The value the region STARTS with needs no
// clause: until the first store the heap slot still holds it, and the record is
// what the collector scans.
bool regionIsCollectorSafe(const Region& region, bool mayCollect) {
    if (!region.hasStore || !mayCollect) return true;
    for (const Access* access : region.accesses) {
        if (access->isStore && !access->valueNeverPointer) return false;
    }
    return true;
}

// Rewrites one key over one region: an alloca shadow, the entry load, every
// access redirected into it, the write-back at every exit that takes one.
// `PromoteMemToReg` turns the alloca into phis afterwards.
void applyRegion(const EnvSlotKey& key, Region& region, FunctionScan& scan,
                 llvm::SmallVectorImpl<llvm::AllocaInst*>& allocas, EnvPromotionStats& stats) {
    llvm::Function& fn = scan.function();
    llvm::LLVMContext& ctx = fn.getContext();
    llvm::Type* i64Ty = llvm::Type::getInt64Ty(ctx);
    const llvm::DataLayout& layout = fn.getParent()->getDataLayout();

    llvm::IRBuilder<> entryBuilder(&*fn.getEntryBlock().getFirstInsertionPt());
    auto* shadow =
        entryBuilder.CreateAlloca(i64Ty, layout.getAllocaAddrSpace(), nullptr, "env.promoted");
    shadow->setAlignment(llvm::Align(8));
    allocas.push_back(shadow);

    llvm::IRBuilder<> builder(region.entryPoint);
    llvm::Value* address = slotAddress(builder, key);
    auto* initial = builder.CreateAlignedLoad(i64Ty, address, llvm::Align(8), "env.promote.in");
    tagEnvRecordAccess(initial, ctx);
    scan.recordNewAccess(initial, key);
    builder.CreateAlignedStore(initial, shadow, llvm::Align(8));
    ++stats.entryLoads;

    for (Access* access : region.accesses) {
        llvm::IRBuilder<> at(access->inst);
        if (access->isStore) {
            auto* store = llvm::cast<llvm::StoreInst>(access->inst);
            llvm::Value* value = store->getValueOperand();
            if (value->getType() != i64Ty) value = at.CreateBitCast(value, i64Ty);
            at.CreateAlignedStore(value, shadow, llvm::Align(8));
            ++stats.storesElided;
        } else {
            llvm::Value* value =
                at.CreateAlignedLoad(i64Ty, shadow, llvm::Align(8), "env.promote.val");
            if (access->inst->getType() != i64Ty) {
                value = at.CreateBitCast(value, access->inst->getType());
            }
            access->inst->replaceAllUsesWith(value);
            ++stats.loadsElided;
        }
        scan.markErased(access->inst);
        access->inst->eraseFromParent();
        access->inst = nullptr;
    }

    if (region.hasStore) {
        for (llvm::Instruction* point : region.writeBackPoints) {
            llvm::IRBuilder<> back(point);
            auto* current =
                back.CreateAlignedLoad(i64Ty, shadow, llvm::Align(8), "env.promote.out");
            auto* stored = back.CreateAlignedStore(current, address, llvm::Align(8));
            tagEnvRecordAccess(stored, ctx);
            scan.recordNewAccess(stored, key);
            ++stats.writeBacks;
        }
    }
    ++stats.slotsPromoted;
    if (region.overLoop) {
        ++stats.loopRegions;
    } else {
        ++stats.functionRegions;
    }
}

// Whether every live access of a key can be rewritten at all — the type guard,
// kept apart from the region questions because it disqualifies the KEY and not
// one candidate region.
bool keyIsRewritable(const std::vector<Access*>& accesses) {
    for (const Access* access : accesses) {
        if (access->inst == nullptr) continue;
        const llvm::Type* ty =
            access->isStore
                ? llvm::cast<llvm::StoreInst>(access->inst)->getValueOperand()->getType()
                : access->inst->getType();
        if (!promotableAccessType(ty)) return false;
    }
    return true;
}

// Tries the whole function body as one region. The record must be an argument
// or defined in the entry block: the shadow alloca lives in the entry block and
// every `ret` must be dominated by the entry load, or a path that never reached
// the record's definition would write back a value that was never loaded.
bool tryFunctionRegion(FunctionScan& scan, const EnvSlotKey& key,
                       const std::vector<Access*>& accesses, const llvm::MemoryLocation& location,
                       Region& out, EnvPromotionStats& stats) {
    llvm::Function& fn = scan.function();
    if (scan.hasObserver()) {
        ++stats.ends[static_cast<size_t>(scan.firstObserverCause())];
        return false;
    }
    llvm::BasicBlock& entry = fn.getEntryBlock();
    llvm::Instruction* entryPoint = nullptr;
    if (auto* recordInst = llvm::dyn_cast<llvm::Instruction>(key.record)) {
        if (recordInst->getParent() != &entry) return false;
        entryPoint = recordInst->getNextNode();
        if (entryPoint == nullptr) return false;
    } else {
        entryPoint = &*entry.getFirstInsertionPt();
    }

    llvm::SmallVector<llvm::BasicBlock*, 32> blocks;
    for (llvm::BasicBlock& block : fn) blocks.push_back(&block);

    RegionEnd cause = RegionEnd::UnknownCall;
    bool mayCollect = false;
    if (!scan.blocksAreClean(blocks, key, location, cause, mayCollect)) {
        ++stats.ends[static_cast<size_t>(cause)];
        return false;
    }

    out.entryPoint = entryPoint;
    out.overLoop = false;
    for (Access* access : accesses) {
        if (access->inst == nullptr) continue;
        out.accesses.push_back(access);
        if (access->isStore) out.hasStore = true;
    }
    // One access replaced by one entry load and one write-back is a wash; the
    // region has to remove more memory operations than it adds.
    if (out.accesses.size() < 2) {
        ++stats.ends[static_cast<size_t>(RegionEnd::NoBenefit)];
        return false;
    }
    if (!regionIsCollectorSafe(out, mayCollect)) {
        ++stats.ends[static_cast<size_t>(RegionEnd::HeapValueStore)];
        return false;
    }
    for (llvm::BasicBlock& block : fn) {
        if (llvm::isa<llvm::ReturnInst>(block.getTerminator())) {
            out.writeBackPoints.push_back(block.getTerminator());
        }
    }
    return true;
}

// Tries one loop as a region. Called outermost-first: the outermost loop that
// is clean is the one promoted, because it subsumes every loop inside it.
//
// A loop region is beneficial whenever the loop runs at all. What it removes is
// per-ITERATION; what it adds is one load at the loop's entry and one store on
// the single exit edge that is taken. Only the code SIZE grows with the exit
// count, which is why there is no benefit test here and there is one above.
bool tryLoopRegion(FunctionScan& scan, llvm::Loop& loop, const EnvSlotKey& key,
                   const std::vector<Access*>& accesses, const llvm::MemoryLocation& location,
                   llvm::DominatorTree& dt, llvm::LoopInfo& loopInfo, bool& cfgChanged,
                   Region& out, EnvPromotionStats& stats) {
    llvm::SmallVector<Access*, 8> inside;
    for (Access* access : accesses) {
        if (access->inst == nullptr) continue;
        if (loop.contains(access->inst->getParent())) inside.push_back(access);
    }
    if (inside.empty()) return false;

    if (const auto* recordInst = llvm::dyn_cast<llvm::Instruction>(key.record)) {
        if (loop.contains(recordInst->getParent())) {
            ++stats.ends[static_cast<size_t>(RegionEnd::RecordNotInvariant)];
            return false;
        }
    }
    llvm::BasicBlock* preheader = loop.getLoopPreheader();
    if (preheader == nullptr || loop.getLoopLatch() == nullptr) {
        ++stats.ends[static_cast<size_t>(RegionEnd::LoopShape)];
        return false;
    }

    RegionEnd cause = RegionEnd::UnknownCall;
    bool mayCollect = false;
    if (!scan.blocksAreClean(loop.getBlocks(), key, location, cause, mayCollect)) {
        ++stats.ends[static_cast<size_t>(cause)];
        return false;
    }

    out.entryPoint = preheader->getTerminator();
    out.overLoop = true;
    for (Access* access : inside) {
        out.accesses.push_back(access);
        if (access->isStore) out.hasStore = true;
    }
    if (!regionIsCollectorSafe(out, mayCollect)) {
        ++stats.ends[static_cast<size_t>(RegionEnd::HeapValueStore)];
        return false;
    }
    // An exit block shared with code outside the loop has no place to put a
    // write-back: an ordinary path into it would run one. Splitting the loop's
    // edges into a block of their own is what LICM does for the same reason,
    // and it is done HERE — after the region is known clean — so a refused key
    // never costs the module a basic block.
    //
    // This is why `__wrapper_render`'s loop needs it at all: the block a
    // pending exception returns through is reached from the loop AND from the
    // epilogue, so the kernel this stage exists for has no dedicated exit
    // until one is made.
    if (!loop.hasDedicatedExits()) {
        cfgChanged |= llvm::formDedicatedExitBlocks(&loop, &dt, &loopInfo, /*MSSAU=*/nullptr,
                                                    /*PreserveLCSSA=*/true);
        if (!loop.hasDedicatedExits()) {
            ++stats.ends[static_cast<size_t>(RegionEnd::LoopShape)];
            return false;
        }
    }
    llvm::SmallVector<llvm::BasicBlock*, 4> exits;
    loop.getExitBlocks(exits);
    for (llvm::BasicBlock* exit : exits) {
        if (!exitTakesWriteBack(*exit)) continue;
        llvm::Instruction* point = &*exit->getFirstInsertionPt();
        bool already = false;
        for (llvm::Instruction* seen : out.writeBackPoints) already = already || seen == point;
        if (!already) out.writeBackPoints.push_back(point);
    }
    return true;
}

void promoteInFunction(llvm::Function& fn, const EnvReach& reach,
                       llvm::FunctionAnalysisManager& fam, EnvPromotionStats& stats,
                       bool& cfgChanged) {
    llvm::AAResults& aa = fam.getResult<llvm::AAManager>(fn);
    FunctionScan scan(fn, reach, aa);
    if (scan.byKey().empty()) return;
    ++stats.functions;

    llvm::DominatorTree& dt = fam.getResult<llvm::DominatorTreeAnalysis>(fn);
    llvm::LoopInfo& loopInfo = fam.getResult<llvm::LoopAnalysis>(fn);
    llvm::AssumptionCache& ac = fam.getResult<llvm::AssumptionAnalysis>(fn);
    llvm::SmallVector<llvm::AllocaInst*, 8> allocas;

    // Keys in the order their first access appears, because the byKey map is a
    // hash table and a module whose promotion order depends on pointer values
    // is a module whose output does.
    std::vector<EnvSlotKey> order;
    std::unordered_set<EnvSlotKey, EnvSlotKeyHash> seen;
    for (const Access& access : scan.accesses()) {
        if (seen.insert(access.key).second) order.push_back(access.key);
    }
    stats.keys += static_cast<uint32_t>(order.size());

    for (const EnvSlotKey& key : order) {
        const std::vector<Access*>& accesses = scan.byKey().at(key);
        if (!keyIsRewritable(accesses)) continue;
        const llvm::MemoryLocation location = llvm::MemoryLocation::get(accesses.front()->inst);

        Region region;
        if (tryFunctionRegion(scan, key, accesses, location, region, stats)) {
            applyRegion(key, region, scan, allocas, stats);
            continue;
        }

        // Outermost-first over the loop nest: a clean outer loop subsumes every
        // loop inside it, so its children are not visited.
        llvm::SmallVector<llvm::Loop*, 8> worklist(loopInfo.begin(), loopInfo.end());
        while (!worklist.empty()) {
            llvm::Loop* loop = worklist.pop_back_val();
            Region loopRegion;
            if (tryLoopRegion(scan, *loop, key, accesses, location, dt, loopInfo, cfgChanged,
                              loopRegion, stats)) {
                applyRegion(key, loopRegion, scan, allocas, stats);
                continue;
            }
            worklist.append(loop->begin(), loop->end());
        }
    }

    if (!allocas.empty()) llvm::PromoteMemToReg(allocas, dt, &ac);
}

}  // namespace

EnvPromotionStats& envPromotionStats() {
    static EnvPromotionStats stats;
    return stats;
}

void envPromotionStatsReport() {
    if (!envPromotionStatsEnabled()) return;
    const EnvPromotionStats& s = envPromotionStats();
    std::fprintf(stderr,
                 "[envpromote] functions=%u keys=%u regions=%u (loop=%u fn=%u) slots=%u "
                 "loadsElided=%u storesElided=%u entryLoads=%u writeBacks=%u\n",
                 s.functions, s.keys, s.loopRegions + s.functionRegions, s.loopRegions,
                 s.functionRegions, s.slotsPromoted, s.loadsElided, s.storesElided, s.entryLoads,
                 s.writeBacks);
    std::fprintf(stderr, "[envpromote] region-end");
    for (size_t i = 0; i < static_cast<size_t>(RegionEnd::Count); ++i) {
        std::fprintf(stderr, " %s=%u", regionEndName(static_cast<RegionEnd>(i)), s.ends[i]);
    }
    std::fprintf(stderr, "\n");
}

bool envPromotionDisabled() {
    static const bool off = [] {
        const char* env = std::getenv("BRONZE_NO_ENV_PROMOTION");
        return env != nullptr && std::strcmp(env, "1") == 0;
    }();
    return off;
}

llvm::PreservedAnalyses EnvPromotionPass::run(llvm::Module& llvmModule,
                                              llvm::ModuleAnalysisManager& mam) {
    if (envPromotionDisabled()) return llvm::PreservedAnalyses::all();

    EnvPromotionStats local;
    EnvPromotionStats& stats = out_ != nullptr ? *out_ : local;
    const EnvReach reach(llvmModule);
    bool cfgChanged = false;
    llvm::FunctionAnalysisManager& fam =
        mam.getResult<llvm::FunctionAnalysisManagerModuleProxy>(llvmModule).getManager();
    for (llvm::Function& fn : llvmModule) {
        if (fn.isDeclaration()) continue;
        promoteInFunction(fn, reach, fam, stats, cfgChanged);
    }

    // A partitioned module runs one of these pipelines per thread
    // (llvm_backend.cpp), so the one global the counters land in is the one
    // piece of this stage that is shared state.
    static std::mutex mergeMutex;
    const std::lock_guard<std::mutex> guard(mergeMutex);
    EnvPromotionStats& global = envPromotionStats();
    if (&global != &stats) {
        global.functions += stats.functions;
        global.keys += stats.keys;
        global.loopRegions += stats.loopRegions;
        global.functionRegions += stats.functionRegions;
        global.slotsPromoted += stats.slotsPromoted;
        global.loadsElided += stats.loadsElided;
        global.storesElided += stats.storesElided;
        global.entryLoads += stats.entryLoads;
        global.writeBacks += stats.writeBacks;
        for (size_t i = 0; i < static_cast<size_t>(RegionEnd::Count); ++i) {
            global.ends[i] += stats.ends[i];
        }
    }

    if (stats.slotsPromoted == 0 && !cfgChanged) return llvm::PreservedAnalyses::all();
    if (cfgChanged) return llvm::PreservedAnalyses::none();
    llvm::PreservedAnalyses preserved;
    preserved.preserveSet<llvm::CFGAnalyses>();
    return preserved;
}

}  // namespace bronze::codegen_llvm
