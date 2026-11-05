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
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
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
};

struct FeedbackPlanStats {
    uint32_t profileFunctions = 0;
    uint32_t matchedFunctions = 0;
    uint32_t valueSites = 0, shapeSites = 0, methodSites = 0, targetSites = 0;
};

struct FeedbackPlan {
    std::unordered_map<uint32_t, NameCounts> shapesByIc;   // prop.get / prop.set receivers
    std::unordered_map<uint32_t, NameCounts> methodsByIc;  // method.call callees
    std::map<SpanKey, NameCounts> dynamicCallees;          // call.dynamic callees
    std::map<ValueKey, NumberCounts> values;               // operands of numeric ops
    // Function name -> the times its entry site ran (a probe function's
    // name is the final module's).
    std::unordered_map<std::string, uint64_t> entries;
    // Callee name (`<name>@<hash>`) -> module function index, for the names
    // that resolve to exactly one function of the module the probe copied.
    std::unordered_map<std::string, uint32_t> callees;
    FeedbackPlanStats stats;
};

SpanKey spanKey(const Span& span);
// Operand `operand` of `inst`, as a value observation is filed.
ValueKey valueKey(const il::Instruction& inst, uint32_t operand);

// Reads `profile` against `probe`, the module after its final passes, as the
// recorded build had it. `functionCount` is how many functions the module had
// before those passes: a callee name resolves only to one of them (the passes
// add typed entries, which no closure is).
FeedbackPlan planFeedback(const il::Module& probe, const TypeProfile& profile, size_t functionCount);

// What the build did with a plan (`--wpo-stats` prints it).
struct FeedbackApplyStats {
    uint32_t directMethods = 0;    // B: method.call sites given a direct target
    uint32_t methodRefused = 0;    //    one profiled callee the direct edge cannot enter
    uint32_t closureCalls = 0;     // B: call.dynamic sites split on their closure
    uint32_t shapeIdentity = 0;    // A: identity claims
    uint32_t shapeFamily = 0;      // A: family claims
    uint32_t shapeRefused = 0;     //    a dominant shape the site cannot claim a slot of
    uint32_t numberHints = 0;      // C: numeric ops hinted
    // The profiled executions the claims cover, each identity counted once
    // however many copies of it were claimed.
    uint64_t shapeExecutions = 0, methodExecutions = 0, closureExecutions = 0, hintExecutions = 0;
};

// B, method calls: a direct target for each method.call whose callee the
// profile names alone and that nothing planned (no direct target, no
// own-slot callee), on the terms a whole-program direct target has. Runs
// before the passes that copy callee bodies, so they see it.
void applyFeedbackDirectMethods(il::Module& mod, const FeedbackPlan& plan,
                                const std::unordered_map<uint32_t, uint32_t>& ownCallees, FeedbackApplyStats* stats);

// B, closure calls: the function a `call.dynamic` should be split on (its
// one profiled callee), or UINT32_MAX (lower/own_callee_calls.h does the
// split).
uint32_t profiledClosureTarget(const il::Module& mod, const FeedbackPlan& plan, const il::Instruction& call);

// A and C over the finished module: a guarded static-slot claim for each
// property site left without one whose receivers had one dominant shape, and
// `numberHint` on each numeric op whose every Dynamic operand was a Number.
void applyFeedbackClaimsAndHints(il::Module& mod, const FeedbackPlan& plan, FeedbackApplyStats* stats);

std::string feedbackReport(const FeedbackPlan& plan, const FeedbackApplyStats& stats);

}  // namespace bronze::spec
