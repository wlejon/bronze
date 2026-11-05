#pragma once

#include <cstdint>
#include <string>

#include "il/il.h"

namespace bronze::lower {

// LAYOUT FACTS (docs/aot-specialization.md "S2"): a receiver's static-slot
// guard, asked once, answers the receiver's later static sites in the same
// function across calls and across blocks, for as long as the layout epoch
// has not moved.
//
// THE FACT. `layout.fact %r` (il_op.h `LayoutFact`) carries one site's claim
// (an identity cell or a family range) and answers the layout epoch when `r`
// passes that claim's guard, else bits no epoch equals. S1's contract is what
// it is worth: after `r` was seen at a watched shape S (every shape a cell
// publishes or a family stamp names is watched) and the epoch is unchanged
// since, `r` still has every slot S described at the same index, attributes
// and representation. A site whose claim the fact asked is therefore a bare
// load (or a store of a Number) wherever the epoch still equals the fact, and
// whatever ran in between — calls, getters, other sites' misses, a collection
// — cannot have broken that, or it would have moved the epoch. An append
// moves `r` to a child shape that keeps the slot; an out-of-line slot block an
// append reallocated is found again because the bare access reads the block
// pointer at the access (il_static_slot.cpp `static_slot_base`), never before.
//
// THE CHECK is per site, in the backend: one load of the epoch word, one
// compare against the fact (il2mir `lower_fact_site`), and the site's ordinary
// lowering — its own guard, cache and fill — where it fails. A site whose
// fact failed costs what it cost before plus that compare; nothing is ever
// re-run, nothing joins a slow copy, and a fact that never holds (a claim the
// receiver does not meet where the fact is asked) is a cost and not an
// answer. Adjacent covered sites the backend lowers as one STATIC RUN share
// one epoch read and one compare per fact (il_lowering_static_run.cpp).
//
// WHERE FACTS GO. Per receiver (an SSA value), the static sites on it are
// grouped under ANCHORS: a site no other site on the receiver dominates. Every
// other site on the receiver has exactly one anchor dominating it. Dominance
// is asked of a graph in which a handler is entered from the START of a
// protected block, so a fact made in front of an anchor in a try block is
// never assumed in its handler. A member is LOCAL when it shares the anchor's
// stretch of the backend's shape memo (same block, nothing between that can
// run user code but static sites), and REMOTE otherwise. A remote stretch is
// covered when one family claim answers all of it and that claim is asked in
// two or more remote stretches (layout_facts.cpp `selectCovered` has why:
// an identity guard costs what the fact's check costs). The anchor gets one
// `layout.fact` per covered claim, in front of it, where the memo's keyed
// shape makes each a compare; the covered members hold it. Local members keep
// their static guards: the memo already makes them a compare, and the
// backend's static runs batch them.
//
// AOT ONLY: it runs in `lowerModule` after the whole-program claims are
// stamped (`optimizeForBuild`), which only `bronze build` asks for. The JIT
// tiers never see a fact.
//
// Coroutine bodies are skipped (a fact would live across a suspension in a
// frame slot; correct, but the resume path is not worth the analysis).

struct LayoutFactStats {
    uint32_t functions = 0;  // functions holding at least one candidate site
    uint32_t sites = 0;      // candidate static sites
    uint32_t anchors = 0;    // anchors with at least one remote member
    uint32_t facts = 0;      // `layout.fact` instructions emitted
    uint32_t covered = 0;    // sites made fact sites
    uint32_t local = 0;      // members left to their own guard (local)
};

// Rewrites every function of `module`. Returns true when it changed anything.
// `perFunction`, when given, receives one line per function whose name
// contains one of the comma-separated substrings of `filter` (empty: none).
bool applyLayoutFacts(il::Module& module, LayoutFactStats* stats = nullptr, const std::string& filter = {},
                      std::string* perFunction = nullptr);

// The counters as one report line.
std::string layoutFactReport(const LayoutFactStats& stats);

}  // namespace bronze::lower
