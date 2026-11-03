#pragma once

// The own-key ORDER of abstract objects: which keys an object of an
// allocation site has, at which slot, when its construction is over.
//
// The runtime gives an object's properties slots in the order they are added
// (an accessor takes two), so a key's slot is decided by the keys added before
// it. For each object the pass walks its INITIALIZATION WINDOW — the
// constructor chain for `new F`, the rest of the creating function for a
// literal or a class prototype — tracking the one concrete object through the
// SSA values that must be it (the receiver, a block parameter every edge feeds
// it to, a `super(...)` that returns it, a callee parameter it is passed to).
// Along the way it keeps the sequence of keys added, joined at control-flow
// merges to their longest common prefix: the keys every path adds, in the
// same order, before any path differs.
//
// The sequence stays EXACT (it is the object's whole key list) only while no
// one else can add a key: once the object is stored anywhere, passed to a
// call the pass cannot follow, or used by an operation that may run user code
// with it, anything that can run code ends exactness — but never the prefix,
// because later additions only append. So a key in the prefix has its slot on
// every object of the site, for as long as no key is deleted (a delete or a
// prototype change voids the object's facts).

#include <cstdint>
#include <string>
#include <vector>

#include "opt/wpo/solver.h"

namespace bronze::wpo {

struct SeqEntry {
    uint32_t key = 0;
    bool accessor = false;  // an accessor takes two slots
    bool operator==(const SeqEntry& o) const { return key == o.key && accessor == o.accessor; }
};

struct ObjShape {
    // The pass ran for this object (an instance, a literal, a prototype).
    bool modelled = false;
    // The keys every construction adds, in order: a definite prefix.
    std::vector<SeqEntry> seq;
    // `seq` is every own key when construction finishes.
    bool exact = false;
    // Construction may end part way with the object already reachable (an
    // exception after it was exposed), leaving a shorter key list.
    bool partial = false;
    // Keys written to the object outside the window, which follow `seq` in
    // an order the pass does not know.
    std::vector<uint32_t> extras;
    // Why `exact` is false, for the census.
    std::string why;

    // The slot `key` has on every object of the site, or -1.
    int slotOf(uint32_t key) const;
};

class ShapeAnalysis {
public:
    explicit ShapeAnalysis(Solver& solver);
    void run();
    const ObjShape& shape(ObjId id) const { return shapes_[id]; }
    // One runtime shape for every object of the site: the exact key list,
    // nothing added later, nothing escaped, deleted or reattributed.
    bool singleShape(ObjId id) const;
    // The slot facts hold: no delete, no prototype change.
    bool slotsHold(ObjId id) const;

private:
    struct State {
        bool valid = false;
        std::vector<SeqEntry> seq;
        bool exact = true;
        bool exposed = false;
        bool partial = false;
        std::string why;
    };
    struct WalkOut {
        State exit;
        bool returnsAlias = false;
    };
    static bool join(State& into, const State& from);
    static void inexact(State& s, const std::string& why);
    void computeObject(ObjId id);
    WalkOut walk(uint32_t fn, uint32_t startBlock, uint32_t startInst, const std::vector<il::ValueId>& seeds,
                 const std::vector<il::ValueId>& fnSeeds, const State& in, int depth);
    bool chainHasAccessor(uint32_t key) const;

    Solver& solver_;
    std::vector<ObjShape> shapes_;
    // The object whose window is being walked, and the functions on the walk
    // stack (a recursive constructor is not followed).
    ObjId target_ = kNoObj;
    std::vector<uint32_t> stack_;
    uint32_t protoKey_ = 0;
};

}  // namespace bronze::wpo
