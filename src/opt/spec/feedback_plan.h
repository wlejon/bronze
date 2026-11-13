#pragma once

// TYPE-FEEDBACK SPECIALIZATION (`bronze build --feedback-in <file.tfb>`,
// docs/type-feedback.md "Consumers"): what a recorded run says about each
// site, filed under an identity that survives the passes that copy code, and
// the guarded claims and hints a build spends it on.
//
// A profile's keys name sites of the FINAL IL of a build without a profile.
// The build that reads it changes that IL (a direct target planned before
// inlining moves every caller's hash), so it cannot look its own sites up by
// key. Lowering instead runs its last passes once on a copy of the module
// (the PROBE, whose final IL is the recorded build's), and `planFeedback`
// reads the profile there and refiles every observation under an identity
// the real run's IL shares:
//
//   - a property or method site by its inline-cache index, which every copy
//     of the instruction keeps (inlining, versioning, guarded regions);
//   - a `call.dynamic` by its span, and an arithmetic, compare or unbox
//     operand by (span, op, operand position): an inlined copy keeps its
//     callee's span.
//
// Observations of copies are summed, so a site in a body inlined into ten
// callers is claimed on what all ten saw, and a copy the recorded build did
// not have (a body inlined only because of a direct target planned here)
// still finds what its original saw.
//
// Every claim is behind a guard, and a hint only moves blocks: a profile that
// is wrong about a site costs speed there, never an answer (the
// `oracle-feedback` suite builds every case on an adversarial profile).

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "il/il.h"
#include "opt/spec/profile.h"

namespace bronze::spec {

// A site must have run this often before the build specializes it: below it,
// the guard and the code it adds are not paid back.
inline constexpr uint64_t kMinExecutions = 32;
// The share of a property site's executions one shape must have for a claim.
inline constexpr uint64_t kDominantPercent = 99;

struct SpanKey {
    uint16_t file = 0;
    uint32_t begin = 0;
    uint32_t end = 0;
    bool operator<(const SpanKey& o) const {
        return std::tie(file, begin, end) < std::tie(o.file, o.begin, o.end);
    }
};

// The result type is part of the identity: `a && b` gives its `unbox.bool`
// truthiness tests and the `unbox.f64` coercions inside it one span, and a
// Boolean seen by the first is no evidence about the second.
struct ValueKey {
    SpanKey span;
    il::Op op = il::Op::Add;
    il::Type type = il::Type::Void;
    uint32_t operand = 0;
    bool operator<(const ValueKey& o) const {
        return std::tie(span, op, type, operand) < std::tie(o.span, o.op, o.type, o.operand);
    }
};

// Names (shapes or callees) seen at one identity, summed over its sites.
struct NameCounts {
    uint64_t total = 0;
    std::map<std::string, uint64_t> counts;
    // The most frequent name, if it is a real one (not `*other*`) and has at
    // least `percent` of the executions and `kMinExecutions` of them.
    const std::string* dominant(uint64_t percent) const;
};

struct NumberCounts {
    uint64_t total = 0;
    uint64_t numbers = 0;  // f64 + i32
    // For truthiness hints: Booleans, objects of every kind, undefined, null.
    uint64_t bools = 0, objects = 0, undefineds = 0, nulls = 0;
};

// What a property read returned: how often, and how often `undefined`.
struct ReadResults {
    uint64_t total = 0;
    uint64_t undefineds = 0;
};

// The executions a harvested site is taken to stand for (feedback_plan.cpp,
// "Harvest"): a cache holds what a site met, not how often, so a filled one
// counts as a site that ran past every threshold here, its shapes sharing
// that count evenly.
inline constexpr uint64_t kHarvestExecutions = 1024;

struct FeedbackPlanStats {
    uint32_t profileFunctions = 0;
    uint32_t matchedFunctions = 0;
    uint32_t valueSites = 0, shapeSites = 0, methodSites = 0, targetSites = 0;
    // An inline-cache harvest: whether the file carried one, whether it was
    // made from another build (a different icLayoutDigest: ignored), and the
    // property and method sites it supplied.
    bool harvest = false;
    bool harvestStale = false;
    uint32_t harvestSites = 0, harvestProperty = 0, harvestMethod = 0;
    uint64_t harvestSamples = 0;  // the samples matched to a function
};

struct FeedbackPlan {
    std::unordered_map<uint32_t, NameCounts> shapesByIc;   // prop.get / prop.set receivers
    std::unordered_map<uint32_t, NameCounts> methodsByIc;  // method.call callees
    std::unordered_map<uint32_t, NameCounts> methodShapesByIc;  // method.call receivers
    std::unordered_map<uint32_t, ReadResults> readResultsByIc;  // prop.get results
    // Property sites whose harvested cache held two shapes under one name:
    // the same keys in the same order, laid out differently (a slot's
    // representation, an attribute), so one identity cell answers one of them.
    std::unordered_set<uint32_t> shapeVariantIcs;
    std::map<SpanKey, NameCounts> dynamicCallees;          // call.dynamic callees
    std::map<ValueKey, NumberCounts> values;               // operands of numeric ops
    // Function name -> the times its entry site ran (a probe function's
    // name is the final module's).
    std::unordered_map<std::string, uint64_t> entries;
    // Function name -> the harvest's samples of it (runtime/harvest_sampler.h):
    // how often the run was found in its code.
    std::unordered_map<std::string, uint64_t> samples;
    // Callee name (`<name>@<hash>`) -> module function index, for every name
    // a method or closure site saw that resolves to exactly one function of
    // the module the probe copied (the claims read the dominant ones,
    // function placement all).
    std::unordered_map<std::string, uint32_t> callees;
    FeedbackPlanStats stats;
};

SpanKey spanKey(const Span& span);
// Operand `operand` of `inst`, as a value observation is filed.
ValueKey valueKey(const il::Instruction& inst, uint32_t operand);

// Reads `profile` against `probe`, the module after its final passes, as the
// recorded build had it. `functionCount` is how many functions the module had
// before those passes: a callee name resolves only to one of them (the passes
// add typed entries, which no closure is). A harvest is read when its digest
// is the probe's `icLayoutDigest` (the probe is the build harvested), and
// filed by IC index like a recorded property or method site.
FeedbackPlan planFeedback(const il::Module& probe, const TypeProfile& profile, size_t functionCount);

// What the build did with a plan (`--wpo-stats` prints it).
struct FeedbackApplyStats {
    uint32_t directMethods = 0;    // B: method.call sites given a direct target
    uint32_t methodRefused = 0;    //    one profiled callee the direct edge cannot enter
    uint32_t closureCalls = 0;     // B: call.dynamic sites split on their closure
    uint32_t shapeIdentity = 0;    // A: identity claims
    uint32_t shapeRegrow = 0;      //    of them, cells that may latch an unfinished object (staticRegrow)
    uint32_t shapeFamily = 0;      // A: family claims
    uint32_t shapeRefused = 0;     //    a dominant shape the site cannot claim a slot of
    uint32_t numberHints = 0;      // C: numeric ops hinted
    uint32_t truthHints = 0;       // C: truthiness unboxes hinted with the kinds they saw
    uint32_t chainReads = 0;       // E: reads given an inline way chain (icChain)
    uint32_t chainProto = 0;       //    of them, with the one-link prototype arm
    uint32_t ownCallees = 0;       // B: method.call sites split on an own slot's closure
    uint32_t ownSlotSites = 0;     // B: own-slot sites with no one closure, dispatched at the site
    uint32_t nativeTargetsDropped = 0;  // B: predicted direct targets the run saw a native callee at
    uint32_t accessorGets = 0;     // F: prop.get sites given their getter as direct target
    uint32_t accessorSets = 0;     // F: prop.set sites given their setter as direct target
    uint32_t accessorNone = 0;     //    a dominant shape lacking the key whose class names no one accessor
    uint64_t accessorExecutions = 0;
    // The profiled executions the claims cover, each identity counted once
    // however many copies of it were claimed.
    uint64_t shapeExecutions = 0, methodExecutions = 0, closureExecutions = 0, hintExecutions = 0;
    uint64_t chainExecutions = 0;
};

// B, method calls: a direct target for each method.call whose callee the
// profile names alone and that nothing planned (no direct target, no
// own-slot callee), on the terms a whole-program direct target has. Runs
// before the passes that copy callee bodies, so they see it.
// A site whose receivers the run saw hold the key as their own gets an
// own-slot callee instead (`ownCallees`, split on the slot's closure later),
// and a site the prediction gave one keeps it unless every receiver the run
// saw inherited the key; with no one closure in the slot the site gets
// kMethodOwnCallee. A site whose executions split across two compiled callees
// gets both in `polyMethods` (inherited) or `polyOwnCallees` (own slot),
// ordered more frequent first, for guarded inlining to chain. A site that met
// two or more shapes gets kMethodPolyShapes, and a predicted direct target
// where the run called a native is dropped.
void applyFeedbackDirectMethods(il::Module& mod, const FeedbackPlan& plan, std::unordered_map<uint32_t, uint32_t>& ownCallees,
                                FeedbackApplyStats* stats,
                                std::unordered_map<uint32_t, std::vector<uint32_t>>* polyMethods = nullptr,
                                std::unordered_map<uint32_t, std::vector<uint32_t>>* polyOwnCallees = nullptr);

// B, closure calls: the function a `call.dynamic` should be split on (its
// one profiled callee), or UINT32_MAX (lower/own_callee_calls.h does the
// split).
uint32_t profiledClosureTarget(const il::Module& mod, const FeedbackPlan& plan, const il::Instruction& call);

// A and C over the finished module: a guarded static-slot claim for each
// property site left without one whose receivers had one dominant shape, and
// `numberHint` on each numeric op whose every Dynamic operand was a Number;
// and E, an inline way chain (`icChain`) on each read that met 1..4 plain
// shapes.
//
// `missedClaims` receives the inline-cache index of every claimed property
// site the run saw a receiver at that its claim does not answer: a shape with
// the key elsewhere or nowhere, one outside the claim's family range, a second
// layout where the claim is one identity cell, a receiver that is no plain
// object, or more receivers than the cache held. A claim the run never saw
// miss names no site there, and neither does a site the run never reached. The
// guarded regions read it (lower/guard_region.h, "SHAPE GUARDS"): a site in it
// keeps its miss in line, where one that is not may leave its fast copy.
void applyFeedbackClaimsAndHints(il::Module& mod, const FeedbackPlan& plan, FeedbackApplyStats* stats,
                                 std::unordered_set<uint32_t>* missedClaims = nullptr);

// LAYOUT CELLS (il.h `Module::layoutCells`), for the functions `hot` marks
// (by function index): after the guarded regions, every claim on a receiver
// whose property sites all met one and the same shape in the run, and none of
// which the run saw miss, asks that shape's layout cell: the property sites,
// bare or not, and the `is.shape` tests in front of them. The claims they had
// (an identity cell a site, or a family range) said the same of the receiver
// with as many different compares. A receiver some site of which met a second
// shape keeps its claims: a family range admits the subclasses a layout cell
// would refuse. Returns the number of sites and tests rewritten.
uint32_t applyFeedbackLayoutCells(il::Module& mod, const FeedbackPlan& plan, const std::vector<uint8_t>& hot,
                                  const std::unordered_set<uint32_t>& missedClaims);

// Layout families read off the profile, for the classes whose construction
// the compiler could not model (a constructor that copies an options object,
// mixins): where a property site met several shapes that all begin with the
// keys of the shortest (`this` in a base class's method: the base and its
// subclasses, or an object before and after it grew a key), that key list is
// appended to `mod.classFamilies`, and the runtime stamps every shape that
// begins with it (runtime/class_family.cpp verifies, name by name). The
// claims that follow (the whole-program stamps, applyFeedbackClaimsAndHints)
// then guard such a site by one range test where it had an inline-cache
// chain. A list that a family already there begins, equals or extends is left
// out: the shape would trade that family's stamp for the new one, which the
// sites already claimed for the old do not admit. Run before any claim of the
// build reads the table; returns how many it added.
uint32_t addProfiledFamilies(il::Module& mod, const FeedbackPlan& plan);

// F, accessor targets: (prototype name, key, setter) -> the getter or setter
// the classes so named resolve the key to, or UINT32_MAX (lowering's class
// table answers it, lower/direct_method_table.h `accessorOfShownClass`).
using AccessorResolver = std::function<uint32_t(const std::string& protoName, const std::string& key, bool setter)>;

// F: a `prop.get` / `prop.set` of a named key that nothing gave a slot or a
// target, whose receivers had one dominant shape whose listed keys lack the
// key (a list the recorder cut short included), gets the accessor half that shape's class resolves the key to as its
// `directTarget`, with a fresh accessor cell pair per site (il.h), exactly
// what lowering's class guess stamps. Runs where that guess is stamped,
// before guarded inlining, which copies the body in behind the cell.
void applyFeedbackAccessors(il::Module& mod, const FeedbackPlan& plan, const AccessorResolver& resolve,
                            FeedbackApplyStats* stats);

std::string feedbackReport(const FeedbackPlan& plan, const FeedbackApplyStats& stats);

}  // namespace bronze::spec
