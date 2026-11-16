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

#include "abi/bronze_abi.h"
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
// The most shapes a receiver's shape set (shape_sets.cpp) may hold.
inline constexpr size_t kMaxSetShapes = 8;
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
// In a build made to be harvested every typed-array access marks
// kElemArmView and the bits of its element kind's code (an index into
// kElemViewKinds) mark these; a kind with no code, and an index at or past a
// view's length, mark kElemArmMiss.
inline constexpr uint32_t kElemArmKindBit0 = 5;
inline constexpr uint32_t kElemArmKindBit1 = 6;
inline constexpr uint32_t kElemArmKindBit2 = 7;
// A read's Array arm whose array held something besides Numbers (its NUMBERS
// bit clear and not claimable). Shares kElemArmKindBit0's index: a site that
// met Arrays and views is not planned (planElem).
inline constexpr uint32_t kElemArmNoNumbers = 5;
inline constexpr int32_t kElemViewKinds[8] = {
    BRONZE_ABI_TA_KIND_FLOAT32, BRONZE_ABI_TA_KIND_FLOAT64, BRONZE_ABI_TA_KIND_INT32,
    BRONZE_ABI_TA_KIND_UINT32,  BRONZE_ABI_TA_KIND_UINT16,  BRONZE_ABI_TA_KIND_INT16,
    BRONZE_ABI_TA_KIND_UINT8,   BRONZE_ABI_TA_KIND_UINT8CLAMPED,
};
// A `.length` read's arms (il2mir il_ic_lowering.cpp lower_length_get): an
// Array's, a String's or a typed array's header answered it.
inline constexpr uint32_t kLengthArmArray = 1;
inline constexpr uint32_t kLengthArmString = 2;
inline constexpr uint32_t kLengthArmView = 3;
// The arms of a dynamic numeric op (il2mir il_abi.h, il_lowering_numeric.cpp:
// `+ - * / %`, unary `-`, `~`, the bitwise and shift ops, `< <= > >=`, `==`,
// `++`/`--`, ToNumeric): its Numbers computed inline, per operand a Number
// that was no int32 (a fraction, -0, NaN, an infinity, or out of range), and
// a Number result that was no int32. `%`'s inline arm is the int32 one
// (a non-negative int by a positive one), so its Numbers arm is int evidence.
inline constexpr uint32_t kArithArmNumbers = 1;
inline constexpr uint32_t kArithArmDouble0 = 2;
inline constexpr uint32_t kArithArmDouble1 = 3;
inline constexpr uint32_t kArithArmResultDouble = 4;
// The arms of a dynamic `===` (il2mir il_lowering_dynamic.cpp
// lower_dynamic_strict_eq): equal bits of a Number, and two Numbers with
// different bits. Any other arm is an operand that was no Number.
inline constexpr uint32_t kStrictEqArmSame = 1;
inline constexpr uint32_t kStrictEqArmSameNumber = 2;
inline constexpr uint32_t kStrictEqArmNumbers = 3;
inline constexpr uint32_t kStrictEqArmLast = 6;
// The arms of ToBoolean of a dynamic value (lower_dynamic_truthy): a
// Boolean or nullish value, an object, a Number, a String.
inline constexpr uint32_t kTruthyArmNumber = 3;
inline constexpr uint32_t kTruthyArmLast = 4;

inline constexpr uint32_t kNoFunction = UINT32_MAX;
inline constexpr int32_t kDenseArray = -1;
inline constexpr int32_t kNoConstIndex = -1;

// Numeric evidence for one operand of an arithmetic site: which Numbers it
// held. A typed operand has the bit of its type.
inline constexpr uint8_t kTagInt = 1;     // an int32-valued Number (`is.i32`)
inline constexpr uint8_t kTagDouble = 2;  // any other Number (`is.number`)

enum class SiteKind : uint8_t {
    PropRead,     // prop.get on own data slots of 1..kMaxChainShapes shapes
    PropWrite,    // prop.set (an assignment) to an existing own slot of 1..kMaxChainShapes shapes
    // The same on receivers of more shapes than a chain covers, all keeping
    // the key at one of 1..kMaxChainShapes slots: `shapes` holds the slots
    // (names empty), each tested with shape.has_own (prop_slot.cpp).
    PropSlotRead,
    PropSlotWrite,
    PropAdd,      // prop.set (an assignment) adding the key to one shape, inline
    Construct,    // construct with one target
    MethodSlot,   // method.call whose method is an own data slot of one shape
    MethodProto,  // method.call whose method is found on the prototype chain
    Call,         // call.dynamic with one target
    Arith,        // a numeric operation on Dynamic operands
    ElemRead,     // elem.get (or prop.get of an index key) of a dense Array or a typed array
    ElemWrite,    // elem.set (or prop.set of an index key): an existing element of either
    ArrayLength,  // prop.get "length" of an Array
    MathCall,     // method.call or call.dynamic of a Math intrinsic
    // A member found above the receiver, one function per receiver shape
    // (1..kMaxChainShapes), each behind a member cell (member_call.cpp):
    MemberMethod,  // method.call of a method in a prototype's data property
    MemberGet,     // prop.get of an accessor's getter
    MemberSet,     // prop.set (an assignment) of an accessor's setter
    MemberAbsent,  // prop.get of a key on neither the receiver nor its chain
    TdzRead,       // env.get.tdz of a binding fixed once initialized (tdz.cpp)
};

// Int: every operand an int32 and the operation computed on i32 (add, sub,
// `++`/`--` with an overflow guard; `%` with sign guards; the compares).
// Double: every operand a Number, the operation on f64. Bits: the bitwise
// family, each operand converted to int32 by the cheaper route its evidence
// allows (an int32 unboxed, any other Number truncated by ToInt32).
enum class ArithForm : uint8_t { Int, Double, Bits };

// A PropRead or PropWrite on a receiver value that several such sites of
// the function share (shape_sets.cpp): the LEAD, which dominates the others,
// tests the receiver's shape against the set of every shape they met and
// leaves when it is none of them; a FOLLOWER, dominated by a lead, tests the
// set and keeps a cold generic arm, so the test folds wherever the lead's
// fact still holds.
enum class ShapeSetRole : uint8_t { None, Lead, Follow };

struct ShapeSlot {
    std::string key;  // the shape's name: the shape cell's key
    int32_t slot = 0;
};

struct SitePlan {
    SiteKind kind = SiteKind::PropRead;
    BlockId block = 0;
    uint32_t index = 0;
    // PropRead and PropWrite (one or a chain), MethodSlot (one); PropAdd
    // the shape before the add, then the one after it with the key's slot;
    // MemberMethod / MemberGet / MemberSet the receiver shape of each callee
    // (slot unused).
    std::vector<ShapeSlot> shapes;
    ShapeSetRole shapeSet = ShapeSetRole::None;
    // MethodSlot, MethodProto, Call, Construct: the callee, by its index as
    // lowered. MethodProto over several (2..kMaxChainShapes): `callees`, and
    // `callee` unset. MemberMethod / MemberGet / MemberSet: `callees`, one
    // per receiver shape the harvest held (a function may repeat).
    uint32_t callee = kNoFunction;
    std::vector<uint32_t> callees;
    // MethodProto: a way of the harvest was a `slot` way, whose callee is the
    // one the cache latched first and never compared again, so a callee the
    // list lacks is no evidence against it: the last identity test's failure
    // calls generically and stays in S (method_call.cpp).
    bool calleesOpen = false;
    // Arith: one mask per operand, and the form chosen from them.
    std::vector<uint8_t> operandTags;
    ArithForm form = ArithForm::Int;
    // ElemRead / ElemWrite: the typed array's element kind
    // (types::TypedArrayElem), or kDenseArray; and for a prop.get / prop.set
    // of an index key its index, or kNoConstIndex.
    int32_t viewKind = kDenseArray;
    // A typed site that met two element kinds: the other one (tested after
    // `viewKind`), or kDenseArray.
    int32_t otherViewKind = kDenseArray;
    int32_t constIndex = kNoConstIndex;
    // A dense constant-index access: the largest constant index planned for
    // the same receiver, which its dense-array test covers.
    int32_t constBound = kNoConstIndex;
    // A dense read whose every Array held only Numbers (NUMBERS bit set or
    // claimed, kElemArmNoNumbers never ran): tested with kDenseNumbers.
    bool numbers = false;
    // MathCall: the il::Intrinsic.
    int32_t intrinsic = -1;
    // MathCall on the pinned global Math (il::Module::mathPinned): the callee
    // is the intrinsic by lockdown, so no read and no identity test.
    bool pinned = false;
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
    std::optional<SitePlan> planSlotKeyed(SiteKind kind, const std::vector<std::string>& names,
                                          const std::string& key, BlockId block, uint32_t index) const;
    // The il::Intrinsic `Math.<key>` is when `recv` is `global.get "Math"`
    // of a module whose Math is pinned and the call passes `argc` arguments
    // (what its computed form takes); -1 otherwise.
    int32_t pinnedMathIntrinsic(ValueId recv, uint32_t keyIndex, size_t argc, const SiteRefs& refs) const;
    std::optional<SitePlan> planMethod(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                       BlockId block, uint32_t index) const;
    std::optional<SitePlan> planConstruct(const il::Instruction& inst, const SiteRefs& refs, BlockId block,
                                          uint32_t index) const;
    // evidence_member.cpp: a harvested site whose every way found its member
    // above the receiver, as MemberMethod / MemberGet / MemberSet.
    std::optional<SitePlan> planMember(const spec::HarvestSite& h, SiteKind kind, size_t argc, BlockId block,
                                       uint32_t index) const;
    // The callee a harvested slow site entered, when it entered one only.
    uint32_t harvestedCallee(const il::Instruction& inst) const;
    // The harvest's name for the one callee a call site ever held, whether
    // or not its helper ran (a native is called through it).
    const std::string* harvestedCalleeName(const il::Instruction& inst) const;
    std::optional<SitePlan> planCall(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                     BlockId block, uint32_t index) const;
    std::optional<SitePlan> planArith(uint32_t fn, const il::Instruction& inst, const SiteRefs& refs,
                                      BlockId block, uint32_t index) const;
    // What a harvest's arms say of a numeric site (planArith).
    struct ArmEvidence {
        bool numbers = false;           // only Number arms ran
        bool intKnown = false;          // the arms tell int32s from other Numbers
        bool operandDouble[2] = {false, false};
        bool resultDouble = false;
    };
    ArmEvidence armEvidence(const il::Instruction& inst) const;
    std::optional<SitePlan> planElem(const il::Instruction& inst, const SiteRefs& refs, BlockId block,
                                     uint32_t index) const;
    void shareConstBounds(const il::Function& fn, FunctionPlan& plan) const;
    // shape_sets.cpp: the lead and follower roles of the PropRead and
    // PropWrite plans on one receiver value, their shapes widened to the set.
    void shareShapeSets(const il::Function& fn, FunctionPlan& plan) const;
    std::optional<SitePlan> planIntrinsic(const spec::HarvestSite& h, size_t argc, BlockId block,
                                          uint32_t index) const;
    std::optional<SitePlan> planLength(const il::Instruction& inst, BlockId block, uint32_t index) const;

    const spec::SiteFeedback* recorded(uint32_t fn, uint32_t ordinal) const;
    const spec::HarvestSite* harvested(uint32_t icIndex) const;
    bool slowRan(uint32_t slowIndex) const;
    uint64_t slowCount(uint32_t slowIndex) const;
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
