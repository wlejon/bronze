#pragma once

// TYPE FEEDBACK SITES (docs/type-feedback.md): which places in a function's
// final IL a feedback build observes, and the stable key each one is filed
// under.
//
// Two compiles have to agree on this to the site: the instrumented build
// (`--feedback-out`, lower/lower_feedback.cpp) that records under these keys,
// and the build that later reads the profile back (`--feedback-in`) and asks
// "what did site N of this function see?" without instrumenting anything. So
// the enumeration is ONE function both call, over the IL exactly as it stands
// at the end of lowering, and a site's ordinal is its position in that walk.
//
// A key is (function key, ordinal). The function key is the function's IL name
// with the counters a change elsewhere renumbers stripped (`__anon_fn_N`, the
// `modN.` prefix), plus a hash of its IL with every module-global index
// resolved to what it names. So a changed function loses its feedback — its
// hash moves and its old rows match nothing — while an unchanged one keeps it
// across edits to the rest of the program.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "abi/bronze_feedback_site.h"
#include "il/il.h"
#include "support/diagnostics.h"

namespace bronze::il {

enum class FeedbackKind : uint8_t {
    Marker = BRONZE_FEEDBACK_KIND_MARKER,
    Entry = BRONZE_FEEDBACK_KIND_ENTRY,
    Value = BRONZE_FEEDBACK_KIND_VALUE,
    Param = BRONZE_FEEDBACK_KIND_PARAM,
    This = BRONZE_FEEDBACK_KIND_THIS,
    Result = BRONZE_FEEDBACK_KIND_RESULT,
    Shape = BRONZE_FEEDBACK_KIND_SHAPE,
    Target = BRONZE_FEEDBACK_KIND_TARGET,
    Method = BRONZE_FEEDBACK_KIND_METHOD,
    MethodName = BRONZE_FEEDBACK_KIND_METHOD_NAME,
};

const char* feedbackKindName(FeedbackKind kind);

// One observation point. `inst` indexes the block's instructions as they are
// BEFORE instrumentation; an entry site (Entry / This / Param) has
// `block == kEntrySite`. `operand` is the operand observed (the parameter's
// ValueId for an entry site); a Result site observes `inst`'s result and an
// Entry site nothing.
struct FeedbackSite {
    static constexpr uint32_t kEntrySite = UINT32_MAX;
    FeedbackKind kind = FeedbackKind::Value;
    uint32_t ordinal = 0;
    uint32_t block = kEntrySite;
    uint32_t inst = 0;
    ValueId value = kNoValue;
    // Method sites: the key the call resolves (Instruction::keyIndex).
    uint32_t keyIndex = 0;
    // Result sites are observed AFTER their instruction, every other site
    // before it.
    bool after() const { return kind == FeedbackKind::Result; }
};

// Whether `inst`'s Dynamic operands are VALUE sites: arithmetic, bitwise,
// relational and equality compares, ToNumeric / ToInt32, and a checked unbox
// (truthiness or ToNumber).
bool observesOperandTags(const Instruction& inst);

// Every site of `fn`, entry sites first, then in block / instruction order.
// Empty for an external function. The ordinals are 0..n-1 in that order.
std::vector<FeedbackSite> enumerateFeedbackSites(const Module& module, const Function& fn);

// The function's name with renumbered counters stripped: `__anon_fn_12` ->
// `__anon_fn_`, `mod3.Foo.bar` -> `mod.Foo.bar`.
std::string stableFunctionName(const std::string& ilName);

// A 64-bit FNV-1a hash of the function's IL: its signature, blocks, and every
// instruction's op, types, operands, immediates, flags and targets, with key
// indices resolved to their strings and function indices to their stable
// names. Spans, inline-cache indices and static cells are left out: they move
// when OTHER code changes and say nothing about this function's behaviour.
uint64_t functionIlHash(const Module& module, const Function& fn);

// The verifier's rule for `feedback.record` (il/verifier.cpp calls it per
// function): void, a feedback site word naming a recording kind, and the one
// Dynamic value it observes — or none, for an entry site, which observes only
// that the function ran.
bool verifyFeedbackRecords(const Function& fn, DiagnosticSink& diags);

// The feedback key of every function, indexed like `module.functions`
// (empty for an external one): `<stable name>#<16 hex digits>`, with `~N`
// appended to the Nth further function that would otherwise share the key.
std::vector<std::string> feedbackFunctionKeys(const Module& module);

// The identity an inline-cache HARVEST is filed under (docs/type-feedback.md,
// "Harvest"): a 64-bit FNV-1a hash of every source text and of what each IC
// index names — for each index, in ascending order, the op and key string of
// the first prop.get / prop.set / method.call carrying it — and the site
// count. A harvest is a table indexed by IC number, so it says something about
// a build only when that build numbers its sites the same way over the same
// text; a different digest is a stale harvest, ignored. The slow-site count
// (below) is mixed in too.
uint64_t icLayoutDigest(const Module& module);

// SLOW SITES (docs/type-feedback.md, "Slow paths"): hands every instruction
// whose lowering has an inline fast path with a generic fallback (an element
// read or write, a named or indexed read or write and the runs of them, a
// strict compare, a checked unbox) the next index in
// `slowSiteCount`, in function, block and instruction order. Run first thing
// in the final passes, on the module every build of the program starts them
// from, so a harvest's counts name the same instructions in the build that
// reads it; the passes after it copy an instruction with its index.
void numberSlowSites(Module& module);

// What a function's descriptor carries as `name_hash` (bronze_abi.h), and
// what a harvest's samples name a function by: FNV-1a (32-bit) of its IL
// name. The harvested build's names are its probe's (the same final passes),
// and the build reading the harvest matches them back by this hash.
inline uint32_t functionNameHash(std::string_view name) {
    uint32_t h = 2166136261u;
    for (unsigned char c : name) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

}  // namespace bronze::il
