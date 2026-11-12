#include "regex/first_units.h"

#include "regex/pattern.h"

namespace bronze::regex {

namespace {

// What a node can begin with, and whether it can match without consuming
// anything — in which case what FOLLOWS it can begin the match too.
struct First {
    uint64_t ascii[2] = {0, 0};
    bool aboveAscii = false;
    bool canBeEmpty = false;

    void addAll() {
        ascii[0] = ascii[1] = ~uint64_t{0};
        aboveAscii = true;
    }
    void add(const First& other) {
        ascii[0] |= other.ascii[0];
        ascii[1] |= other.ascii[1];
        aboveAscii = aboveAscii || other.aboveAscii;
    }
};

First firstOf(const Node& node) {
    First out;
    switch (node.kind) {
    case NodeKind::Char:
        if (node.ch < 128) {
            out.ascii[node.ch >> 6] |= uint64_t{1} << (node.ch & 63);
        } else {
            out.aboveAscii = true;
        }
        return out;
    case NodeKind::Class: {
        // Without `i` a negated class is the complement of its ranges; with
        // it this function is never reached (computeFirstUnits).
        uint64_t in[2] = {0, 0};
        bool above = false;
        for (const Range& r : node.ranges) {
            if (r.hi >= 128) above = true;
            for (uint32_t c = r.lo; c <= r.hi && c < 128; ++c) in[c >> 6] |= uint64_t{1} << (c & 63);
        }
        if (node.negated) {
            out.ascii[0] = ~in[0];
            out.ascii[1] = ~in[1];
            out.aboveAscii = true;
        } else {
            out.ascii[0] = in[0];
            out.ascii[1] = in[1];
            out.aboveAscii = above;
        }
        // `v` mode's string members, never present here; kept exact anyway.
        if (!node.strings.empty()) out.addAll();
        out.canBeEmpty = node.matchesEmpty;
        return out;
    }
    case NodeKind::Dot:
        out.addAll();
        return out;
    case NodeKind::Assertion:
    case NodeKind::Lookahead:
    case NodeKind::Lookbehind:
        // Consumes nothing, so the match begins with whatever follows. What
        // the assertion itself demands is left to the matcher.
        out.canBeEmpty = true;
        return out;
    case NodeKind::Backreference:
        // The captured text, which can be anything, or nothing at all when
        // the group has not participated.
        out.addAll();
        out.canBeEmpty = true;
        return out;
    case NodeKind::Group:
    case NodeKind::Sequence: {
        out.canBeEmpty = true;
        for (const NodePtr& child : node.children) {
            const First c = firstOf(*child);
            out.add(c);
            if (!c.canBeEmpty) {
                out.canBeEmpty = false;
                break;
            }
        }
        return out;
    }
    case NodeKind::Alternation:
        // No alternative at all is the empty pattern.
        out.canBeEmpty = node.children.empty();
        for (const NodePtr& child : node.children) {
            const First c = firstOf(*child);
            out.add(c);
            out.canBeEmpty = out.canBeEmpty || c.canBeEmpty;
        }
        return out;
    case NodeKind::Repeat: {
        if (node.children.empty()) {
            out.addAll();
            out.canBeEmpty = true;
            return out;
        }
        out = firstOf(*node.children[0]);
        if (node.min == 0) out.canBeEmpty = true;
        return out;
    }
    }
    out.addAll();
    out.canBeEmpty = true;
    return out;
}

}  // namespace

FirstUnits computeFirstUnits(const Node& root, const Flags& flags) {
    FirstUnits out;
    // Under `i` a Char holds the CANONICAL character and a Class answers
    // after canonicalizing; under `u` / `v` the alphabet is the code point.
    // Neither is a comparison with the unit at the index.
    if (flags.ignoreCase || flags.unicodeMode()) return out;
    const First first = firstOf(root);
    if (first.canBeEmpty) return out;
    out.usable = true;
    out.ascii[0] = first.ascii[0];
    out.ascii[1] = first.ascii[1];
    out.aboveAscii = first.aboveAscii;
    return out;
}

}  // namespace bronze::regex
