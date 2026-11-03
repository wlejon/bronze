#include "opt/wpo/value.h"

#include <algorithm>
#include <iterator>

namespace bronze::wpo {

bool unionInto(ObjSet& into, const ObjSet& from) {
    if (from.empty()) return false;
    if (into.empty()) {
        into = from;
        return true;
    }
    // The common case is a set that already holds everything: one linear
    // pass decides it without building anything.
    if (std::includes(into.begin(), into.end(), from.begin(), from.end())) return false;
    ObjSet merged;
    merged.reserve(into.size() + from.size());
    std::set_union(into.begin(), into.end(), from.begin(), from.end(), std::back_inserter(merged));
    into.swap(merged);
    return true;
}

bool insertInto(ObjSet& into, ObjId id) {
    auto it = std::lower_bound(into.begin(), into.end(), id);
    if (it != into.end() && *it == id) return false;
    into.insert(it, id);
    return true;
}

bool contains(const ObjSet& set, ObjId id) {
    return std::binary_search(set.begin(), set.end(), id);
}

bool AVal::join(const AVal& other) {
    bool changed = false;
    if ((other.prims & kStr) != 0) {
        if ((prims & kStr) == 0) {
            strKey = other.strKey;
            changed = true;
        } else if (strKey != other.strKey && strKey != kAnyStr) {
            strKey = kAnyStr;
            changed = true;
        }
    }
    const uint16_t bits = prims | other.prims;
    if (bits != prims) {
        prims = bits;
        changed = true;
    }
    if (other.topObj && !topObj) {
        topObj = true;
        changed = true;
    }
    if (other.viaEscape && !viaEscape) {
        viaEscape = true;
        changed = true;
    }
    if (unionInto(objs, other.objs)) changed = true;
    return changed;
}

bool AVal::addPrims(uint16_t bits) {
    // A string added by kind alone may be any string.
    if ((bits & kStr) != 0) bits |= kStrOther;
    if ((bits & kStr) != 0 && (prims & kStr) == 0) strKey = kAnyStr;
    const uint16_t merged = prims | bits;
    if (merged == prims) return false;
    prims = merged;
    return true;
}

bool AVal::addObj(ObjId id) { return insertInto(objs, id); }

AVal AVal::top() {
    AVal v;
    v.prims = kAllPrims;
    v.topObj = true;
    return v;
}

AVal AVal::prim(uint16_t bits) {
    AVal v;
    v.addPrims(bits);
    return v;
}

AVal AVal::str(uint32_t key) {
    AVal v;
    v.prims = kStr | kStrOther;
    v.strKey = key;
    return v;
}

AVal AVal::enumKeys() {
    AVal v;
    v.prims = kStr;
    return v;
}

AVal AVal::obj(ObjId id) {
    AVal v;
    v.objs.push_back(id);
    return v;
}

bool operator==(const AVal& a, const AVal& b) {
    return a.prims == b.prims && a.topObj == b.topObj && a.viaEscape == b.viaEscape && a.objs == b.objs &&
           ((a.prims & kStr) == 0 || a.strKey == b.strKey);
}

}  // namespace bronze::wpo
