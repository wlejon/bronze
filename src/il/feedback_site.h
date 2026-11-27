#pragma once

// INLINE-CACHE HARVEST & SLOW SITES: identity and counter numbering for
// functions and slow/allocation sites in final IL.

#include <cstdint>
#include <string>
#include <vector>

#include "il/il.h"

namespace bronze::il {

// Whether `inst`'s Dynamic operands are value/numeric/compare/unbox ops.
bool observesOperandTags(const Instruction& inst);

// The identity an inline-cache HARVEST is filed under: a 64-bit FNV-1a hash of
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
// strict compare, a checked unbox, a dynamic numeric op — `+ - * / %`, unary
// `-`, `~`, the bitwise and shift ops, `< <= > >=`, `==`, `++`/`--`,
// ToNumeric — whose form computes Numbers inline), and every call (`call`,
// `call.dynamic`, `method.call`, construct: kCallCountArm) and every object
// literal (an allocation site, as a construct is: kAllocCountArm) the next index in
// `slowSiteCount`, in function, block and instruction order. Run first thing
// in the final passes, on the module every build of the program starts them
// from, so a harvest's counts name the same instructions in the build that
// reads it; the passes after it copy an instruction with its index.
// An instruction takes kSlowSiteArms indices: its own is its slow arm's (the
// generic fallback), the ones after it the ARMS its lowering names (il2mir
// il_property.h, "ARMS"): the alternatives of an inline path, each marked
// where it begins.
inline constexpr uint32_t kSlowSiteArms = 8;
// Of a named property read's or write's indices, the two a build made to be
// harvested keeps its receivers' SHAPE TURNS in: kLastShapeArm's word holds
// the shape of the site's last receiver (the stand-in for one that is no
// plain object) with bit 63 set, which no count has and the harvest leaves
// where it is; kShapeTurnArm's counts the receivers whose shape was not that
// one. A site that met one shape while its objects were being built and
// another ever after turned twice (its first receiver, and the first of the
// later shape), and one that meets both for good turns each time they
// alternate.
// A call.dynamic or a construct keeps its CALLEE TURNS the same way: the last
// callee its code entered directly (its code pointer, with bit 62 set as well
// as bit 63) and the times that callee changed; a call the helper made (any
// callee the inline test refused) is its slow arm's count instead. A method
// call keeps them too, for the callee it found in an own slot (the slot way).
inline constexpr uint32_t kLastShapeArm = 6;
inline constexpr uint32_t kShapeTurnArm = 7;
// A call's CALL COUNT: every call, `call.dynamic`, `method.call` and
// construct of a build made to be harvested adds one to this index's word
// as it starts, so a harvest says how often each call ran; the inliner's
// budget reads it (opt/inline driver.cpp). Instructions copied by inlining
// share their original's count.
inline constexpr uint32_t kCallCountArm = 1;
// An ALLOCATION SITE's counts (docs/type-feedback.md, "Pretenuring"): an
// object literal's, a construct's (and the create.this a speculated one
// becomes, which keeps its site) objects a build made to be harvested
// allocated, and how many of them the collector promoted, counted through
// the memento behind each (brass set_memento_layout). Their ratio is what
// pretenures a site (opt/pipeline/pipeline.cpp planPretenure).
inline constexpr uint32_t kAllocCountArm = 2;
inline constexpr uint32_t kAllocTenuredArm = 3;
// Each function with a body is handed kFunctionSlowSites indices of its own
// first: its ENTRY COUNT (Function::entrySlowSite), which a build made to be
// harvested adds one to at every entry (a coroutine body's at every resume
// too), and after it its LOOP COUNT, which that build adds one to each time
// a loop header of the function's code runs (a back edge's target in a
// depth-first walk from the entry: an iteration or a loop's start). The two
// are the run's measure of where its work went (opt/speculate evidence.h,
// "Which functions are hot"): counts, the same for every run of a
// deterministic program.
inline constexpr uint32_t kLoopCountOffset = 1;
inline constexpr uint32_t kFunctionSlowSites = 2;
void numberSlowSites(Module& module);

// FNV-1a (32-bit), as the runtime's fnv32: what a harvest names a callee's
// source text by (`~<offset>@<hash>`), which the build reading it matches
// against its own functions' text.
inline uint32_t fnv32(std::string_view name) {
    uint32_t h = 2166136261u;
    for (unsigned char c : name) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

}  // namespace bronze::il
