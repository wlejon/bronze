#pragma once

// EVIDENCE: the profile read into per-site decisions. The only file of the
// step that reads a TypeProfile; everything after it works from the plans.
//
// Two kinds of evidence exist (docs/type-feedback.md):
//   - a RECORDED profile (`--feedback-out` run): per function key, per site
//     ordinal (il/feedback_site.h enumerateFeedbackSites), the tags, shapes
//     and callees one run saw, with counts;
//   - an inline-cache HARVEST: per IC index the ways each cache held, the
//     slow-site counters, and samples per function. Used only when its digest
//     is the module's (il::icLayoutDigest), i.e. it was taken from a build
//     numbering its sites as this one does.
// Both are read against the module as lowering left it, before anything
// changes: keys hash the IL, and IC and slow-site indices are positions.
//
// A site is planned only on DECISIVE evidence: one shape (or a short chain of
// own-property shapes), one callee, one numeric tag pair, one array kind; and
// only in a function hot enough to matter. Everything else stays generic in S.

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "il/feedback_site.h"
#include "il/il.h"
#include "opt/spec/profile.h"
#include "opt/speculate/speculate.h"

namespace bronze::opt::speculate {

// ---- thresholds -------------------------------------------------------------
// A function is hot when the recorded run entered it this often...
inline constexpr uint64_t kMinEntries = 64;
// ...or a site of it ran this often (a function entered once that loops)...
inline constexpr uint64_t kMinSiteRunsForHot = 4096;
// ...or the harvest's sampler caught its code this many times.
inline constexpr uint64_t kMinSamples = 2;
// A recorded site must have run this often for its tags or names to count.
inline constexpr uint64_t kMinSiteObservations = 16;
// A property read with this many own-property shapes, at most, becomes a
// chain of shape tests; more stays generic.
inline constexpr size_t kMaxChainShapes = 4;
// Calls whose receiver no way of the cache could hold (the harvest's `other`
// row) a site may have and still be speculated on. Zero: every such receiver
// would leave, and a receiver kind the cache cannot even hold is not a rare
// event the evidence can call decisive.
inline constexpr uint64_t kMaxUncachedReceivers = 0;
// The ARMS of an element read or write (il/feedback_site.h kSlowSiteArms):
// the slow-site words after the instruction's own, which the backend's
// element lowering marks where its Array and its typed-array arms begin, where
// a write appended (grew the Array by one in place) and where a read of an
// Array found no element. They must match the backend's numbering for
// `elem.get` / `elem.set` (il2mir il_abi.h).
inline constexpr uint32_t kElemArmArray = 1;
inline constexpr uint32_t kElemArmView = 2;
inline constexpr uint32_t kElemArmGrow = 3;
inline constexpr uint32_t kElemArmMiss = 4;
// The arms of a dynamic `+ - * /` (il2mir il_abi.h): two Numbers computed
// inline, and per operand a Number that was no int32 (a fraction, -0, NaN,
// an infinity, or out of range).
inline constexpr uint32_t kArithArmNumbers = 1;
inline constexpr uint32_t kArithArmDouble0 = 2;
inline constexpr uint32_t kArithArmDouble1 = 3;

inline constexpr uint32_t kNoFunction = UINT32_MAX;

// Numeric evidence for one operand of an arithmetic site: which Numbers it
// held. A typed operand has the bit of its type.
inline constexpr uint8_t kTagInt = 1;     // an int32-valued Number (`is.i32`)
inline constexpr uint8_t kTagDouble = 2;  // any other Number (`is.number`)

enum class SiteKind : uint8_t {
    PropRead,     // prop.get on own data slots of 1..kMaxChainShapes shapes
    PropWrite,    // prop.set (an assignment) to an existing own slot of one shape
    MethodSlot,   // method.call whose method is an own data slot of one shape
    MethodProto,  // method.call whose method is found on the prototype chain
    Call,         // call.dynamic with one target
    Arith,        // a numeric operation on Dynamic operands
    ElemRead,     // elem.get of a dense Array at an int index
    ElemWrite,    // elem.set of an existing element of a dense Array
};

enum class ArithForm : uint8_t { Int, Double };

struct ShapeSlot {
    std::string key;  // the shape's name: the shape cell's key
    int32_t slot = 0;
};

struct SitePlan {
    SiteKind kind = SiteKind::PropRead;
    BlockId block = 0;
    uint32_t index = 0;
    // PropRead (one or a chain), PropWrite and MethodSlot (one).
    std::vector<ShapeSlot> shapes;
    // MethodSlot, MethodProto, Call: the callee, by its index as lowered.
    uint32_t callee = kNoFunction;
    // Arith: one mask per operand, and the form chosen from them.
    std::vector<uint8_t> operandTags;
    ArithForm form = ArithForm::Int;
};

struct FunctionPlan {
    uint32_t function = 0;
    std::vector<SitePlan> sites;  // in block, then instruction order
};

class Evidence {
public:
    // Reads the keys and the harvest's identity off `module`, which must be
    // the module as lowering left it. `stats` gets `harvestIgnored`.
    Evidence(const il::Module& module, const spec::TypeProfile& profile, const SpeculateOptions& options,
             SpeculateStats& stats);

    // The decisions for function `fn`, or nullopt when it is not hot, may not
    // be speculated, or has no site with decisive evidence.
    std::optional<FunctionPlan> plan(uint32_t fn) const;

private:
    struct SiteRefs;  // the feedback sites of one function, by instruction

    bool isHot(uint32_t fn) const;
    std::optional<SitePlan> planSite(uint32_t fn, BlockId block, uint32_t index, const SiteRefs& refs) const;
    std::optional<SitePlan> planPropRead(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                         BlockId block, uint32_t index) const;
    std::optional<SitePlan> planPropWrite(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                          BlockId block, uint32_t index) const;
    std::optional<SitePlan> planMethod(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                       BlockId block, uint32_t index) const;
    std::optional<SitePlan> planCall(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                     BlockId block, uint32_t index) const;
    std::optional<SitePlan> planArith(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                      BlockId block, uint32_t index) const;
    std::optional<SitePlan> planElem(const il::Instruction& inst, const SiteRefs& refs, BlockId block,
                                     uint32_t index) const;

    const spec::SiteFeedback* recorded(uint32_t fn, uint32_t ordinal) const;
    const spec::HarvestSite* harvested(uint32_t icIndex) const;
    bool slowRan(uint32_t slowIndex) const;
    bool slowArmed(uint32_t slowSite) const;
    // The module function a profile's callee name names, if exactly one.
    uint32_t calleeByRecordedName(const std::string& name) const;
    uint32_t calleeByHarvestName(const std::string& name) const;
    void buildCalleeNames() const;
    std::string sourceText(const il::Function& fn) const;

    const il::Module& module_;
    const spec::TypeProfile& profile_;
    const spec::Harvest* harvest_ = nullptr;  // null when absent or stale
    std::vector<std::string> keys_;
    uint64_t minEntries_ = kMinEntries;
    uint64_t minSamples_ = kMinSamples;
    // Callee name -> function, built on first use (kNoFunction: ambiguous).
    mutable bool namesBuilt_ = false;
    mutable std::unordered_map<std::string, uint32_t> recordedNames_;
    mutable std::unordered_map<std::string, uint32_t> harvestNames_;
};

}  // namespace bronze::opt::speculate
