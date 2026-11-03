#pragma once

// The value lattice of the whole-program analysis (src/opt/wpo/solver.h).
//
// An abstract value is a SET of concrete values, described by three parts that
// join independently: which primitive kinds it may be, whether it may be an
// object the analysis does not track, and which tracked abstract objects it
// may be. The primitive part keeps one fact more than the kind for strings: the
// key-pool string a value is when it can only be one, which is what turns a
// computed key spelled with a constant (`o['x']`, `defineProperty(o, 'id', d)`)
// back into a named access.
//
// `topObj` is the open world. It stands for every object the program did not
// allocate (host and builtin objects) and for every tracked object that ESCAPED
// to code the analysis cannot see. A tracked object is never represented by it
// before it escapes, which is what lets a write through a `topObj` receiver be
// modelled as "the value escapes" rather than as a write to every object.

#include <cstdint>
#include <string>
#include <vector>

namespace bronze::wpo {

using ObjId = uint32_t;
inline constexpr ObjId kNoObj = UINT32_MAX;

// Primitive kinds. A number that is an int32 sets kI32; any other number sets
// kF64, so "some number" is either bit and a join keeps both.
enum Prim : uint16_t {
    kUndef = 1u << 0,
    kNull = 1u << 1,
    kBool = 1u << 2,
    kI32 = 1u << 3,
    kF64 = 1u << 4,
    kStr = 1u << 5,
    kSym = 1u << 6,
    kBigInt = 1u << 7,
    // Set beside kStr for a string that may be other than a key name read
    // off an object by enumeration (`for...in`, `Object.keys`). A value with
    // kStr and without this bit names only enumerable properties, which is
    // what lets `o[k]` in a `for (k in src)` loop skip class methods.
    kStrOther = 1u << 8,
};
inline constexpr uint16_t kNumber = kI32 | kF64;
inline constexpr uint16_t kAllPrims = 0x1FF;

// `strKey` when a string value may be more than one string.
inline constexpr uint32_t kAnyStr = UINT32_MAX;

// A sorted, duplicate-free set of abstract object ids.
using ObjSet = std::vector<ObjId>;

// Adds every element of `from` to `into`; true when `into` grew.
bool unionInto(ObjSet& into, const ObjSet& from);
// Adds one element; true when it was not there.
bool insertInto(ObjSet& into, ObjId id);
bool contains(const ObjSet& set, ObjId id);

struct AVal {
    uint16_t prims = 0;
    bool topObj = false;
    // The one key-pool string this value is, when `prims & kStr` and it can be
    // only that one; kAnyStr otherwise. Meaningless without kStr.
    uint32_t strKey = kAnyStr;
    // `topObj` took on tracked objects that escaped (Solver::joinTracked),
    // rather than standing only for objects the program never made. Kept
    // for the census, which reports the two apart.
    bool viaEscape = false;
    ObjSet objs;

    bool empty() const { return prims == 0 && !topObj && objs.empty(); }
    bool hasObjects() const { return topObj || !objs.empty(); }
    bool onlyNumbers() const { return prims != 0 && (prims & ~kNumber) == 0 && !hasObjects(); }
    // A known single string constant and nothing else.
    bool isStrConst() const {
        return (prims & ~kStrOther) == kStr && strKey != kAnyStr && !hasObjects();
    }
    // Only strings, each an enumerated key name.
    bool onlyEnumKeys() const { return prims == kStr && !hasObjects(); }

    // Joins `other` in; true when this value grew.
    bool join(const AVal& other);
    bool addPrims(uint16_t bits);
    bool addObj(ObjId id);

    static AVal top();
    static AVal prim(uint16_t bits);
    static AVal str(uint32_t key);
    // Key names enumeration produced: kStr without kStrOther.
    static AVal enumKeys();
    static AVal obj(ObjId id);
};

bool operator==(const AVal& a, const AVal& b);

}  // namespace bronze::wpo
