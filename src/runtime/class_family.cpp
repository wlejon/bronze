// Recognising a run-time shape as an instance of a compile-time class layout.
//
// `static_shape.cpp` pins ONE shape per site, which is exactly right for a
// receiver that only ever has one — `new Vector3()`, a field whose only writes
// are `new Vector3()` — and permanently wrong for the receiver that motivated
// this file: `this` inside a method of a class somebody extends. three.js never
// constructs a bare `Object3D`, so every `this.matrixWorld` in
// `updateMatrixWorld` runs on a `Group`, a `Mesh` or a `Scene`. Those are three
// shapes. An identity compare pins one and misses the other two forever, at the
// hottest site in the scene.
//
// The layout was never what failed. A subclass constructor runs `super()` to
// completion before it installs a field of its own, so a proven subclass's
// property order BEGINS with its base's, at the same slots. What failed is the
// shape compare, which is an identity test on purpose. So the guard changes:
// instead of "is this the shape I saw first", it asks "is this shape's layout
// one of the layouts that begin with my class's fields" — and that is a
// question a single word on the shape can answer, because the answer is the
// same for every object at that shape.
//
// The word is written HERE, and only after this file has walked the shape's own
// transition chain and checked, name by name and slot by slot, that the class's
// declared field list really is a prefix of it. That is what keeps the whole
// mechanism sound the way `bronze_static_shape_publish` is sound: the compiler
// predicts, the runtime verifies, and a wrong prediction costs a guard that
// never matches rather than a wrong answer. It is also why a subclass declared
// by dynamic code — an `eval`'d `class extends Object3D` this compilation never
// saw — is HANDLED rather than refused: its shape begins with Object3D's
// fields, so it verifies, so it gets the base's stamp and hits the base's
// sites.
//
// Ids are handed out in preorder blocks per module, biased by a base the module
// stores in one global of its own, so a site's whole guard is
//
//     stamp - (base + lo)  <=u  span
//
// with `lo` and `span` compile-time constants. `UNSTAMPED` (0) and `NONE` (1)
// are below every real id, so an unstamped shape fails the compare with no
// extra test — which is what lets the miss path stay the path it already was.

#include "runtime/class_family.h"

#include <cstddef>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/property_key.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/shape_census.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// One field of one registered class: the process-wide key id its name interned
// to, and whether the construction sequence installs it WRITABLE.
//
// The attribute is not decoration. `Object.defineProperty(this, 'id', {value:
// n})` installs a non-writable data property, and three.js roots four `extends`
// chains — Object3D, BufferGeometry, Material, Texture — exactly that way. A
// stamp is only allowed to stand for a slot a write site may store into if the
// shape agrees with the compiler about that bit, so the check is an equality
// and not a "must be writable": a class that declares `id` non-writable expects
// a non-writable node there, and a shape carrying a writable one is a different
// layout wearing the same names.
struct FamilyField {
    uint32_t keyId = 0;
    bool writable = true;
};

struct FamilyClass {
    uint64_t id = BRONZE_ABI_FAMILY_NONE;
    uint32_t fieldStart = 0;
    uint32_t fieldCount = 0;
};

// Process-wide, because a Shape is process-wide: two modules that both extend
// `Object3D` meet the same shapes, and the id space has to be one space for the
// stamp on a shape to mean anything to either of them.
//
// No lock. Registration happens from a module's init, which runs on the thread
// that loaded the module, before any of its code executes; the stamper only
// reads. That is the same discipline `bronze_register_key_string` keeps, and
// for the same reason.
std::vector<FamilyClass>& classes() {
    static std::vector<FamilyClass> v;
    return v;
}
std::vector<FamilyField>& fields() {
    static std::vector<FamilyField> v;
    return v;
}
uint64_t& nextId() {
    static uint64_t id = BRONZE_ABI_FAMILY_FIRST_ID;
    return id;
}

// The longest field list any registered class declares. Kept as the walk below
// only ever needs the first that many slots, and recomputing it per stamp would
// make a one-shot check scan the whole registry twice.
uint32_t& longestFieldCount() {
    static uint32_t n = 0;
    return n;
}

// `out[i]` = the transition node that owns SLOT i, or null when nothing does.
//
// Indexed by slot rather than by chain position, because the two are not the
// same number: an accessor node is two slots wide, so one anywhere leaves a
// slot no node owns — and a null there is exactly the refusal wanted, since a
// class claiming that slot would be claiming the second half of a getter/setter
// pair. A chain shorter than `want` leaves the tail null for the same reason.
//
// The whole chain is walked. Truncating from the leaf end is not an option: the
// nodes wanted are the ones NEAREST THE ROOT, which are the last a leaf-first
// walk reaches. It is bounded by the object's own property count, and a
// dictionary — the one unbounded shape — never gets here.
void ownNodesBySlot(const Shape* shape, uint32_t want, std::vector<const Shape*>& out) {
    out.assign(want, nullptr);
    if (want == 0) return;
    for (const Shape* cur = shape; cur != nullptr; cur = cur->parent) {
        if (!cur->key.valid()) continue;
        if (cur->slot_index < want) out[cur->slot_index] = cur;
    }
}

// Does `cls`'s declared field list describe the first `cls.fieldCount`
// properties of `shape`?
//
// Every condition here is one a family guard will NOT re-check, so each is a
// thing the stamp stands for afterwards:
//
//   - the node at position i owns slot i. That is what makes the site's
//     compile-time offset the right offset, and it is not implied by the names:
//     an accessor node is two slots wide, so one anywhere in the prefix shifts
//     everything after it.
//   - the node is a DATA property. An accessor's slot holds a getter.
//   - the node's writability is the one the class declared, so a write site's
//     bare store cannot fall into 10.4.5's refusal.
//   - the names match, in order. Content equality, because a shape's key is an
//     arena COPY and two equal names are two pointers.
bool prefixMatches(const FamilyClass& cls, const std::vector<const Shape*>& nodes) {
    if (nodes.size() < cls.fieldCount) return false;
    const std::vector<FamilyField>& all = fields();
    for (uint32_t i = 0; i < cls.fieldCount; ++i) {
        const Shape* node = nodes[i];
        if (node == nullptr || node->accessor) return false;
        const FamilyField& f = all[cls.fieldStart + i];
        if (node->writable != f.writable) return false;
        StringHeader* name = rtKeyHeader(f.keyId);
        if (name == nullptr) return false;
        if (!node->key.matches(PropertyKey::forString(name))) return false;
    }
    return true;
}

}  // namespace

uint64_t classFamilyIdFor(Shape* shape) {
    if (shape == nullptr || shape->isDictionary() || classes().empty()) {
        return BRONZE_ABI_FAMILY_NONE;
    }
    // A shape with a DOUBLE SLOT (slot_repr.h) is stamped like any other. The
    // family guard licenses a bare load and a bare store at a compile-time
    // offset; the load is right as it stands, because a double slot holds the
    // number's box, and the store tests the shape's `double_slots` word for its
    // own slot before it writes (llvm_static_slot.cpp). A family guard has no
    // per-site direction hook the way `bronze_static_shape_publish` does, which
    // is exactly why the test lives in the emitted site rather than here.
    std::vector<const Shape*> nodes;
    ownNodesBySlot(shape, longestFieldCount(), nodes);

    // The MOST SPECIFIC match wins, and ties go to the later registration.
    // Preorder puts a class after its base, so a tie between a base and a
    // subclass that adds no field of its own — which have, by definition, the
    // same layout — resolves to the subclass, and the subclass's sites hit as
    // well as the base's. A tie between two unrelated classes resolves
    // arbitrarily but deterministically, and costs at most a miss: the stamp is
    // only ever written for a class whose fields this shape genuinely begins
    // with, so whichever wins, every site that accepts it reads the name it
    // meant to read.
    uint64_t best = BRONZE_ABI_FAMILY_NONE;
    uint32_t bestCount = 0;
    for (const FamilyClass& cls : classes()) {
        if (cls.fieldCount < bestCount) continue;
        if (!prefixMatches(cls, nodes)) continue;
        best = cls.id;
        bestCount = cls.fieldCount;
    }
    return best;
}

uint32_t classFamilyCount() { return static_cast<uint32_t>(classes().size()); }

void classFamilyResetForTesting() {
    classes().clear();
    fields().clear();
    longestFieldCount() = 0;
    nextId() = BRONZE_ABI_FAMILY_FIRST_ID;
}

extern "C" {

void bronze_register_class_family(const uint32_t* classTable, uint32_t classCount,
                                  const uint32_t* fieldTable, const uint32_t* keyMap,
                                  uint64_t* baseCell) {
    recordHelperCall("bronze_register_class_family");
    if (baseCell != nullptr) *baseCell = nextId();
    if (classTable == nullptr || fieldTable == nullptr || keyMap == nullptr || classCount == 0) {
        return;
    }

    const uint32_t fieldBase = static_cast<uint32_t>(fields().size());
    // The whole field table first, so a class entry's start index is a
    // process-wide index by the time any class is appended. The table's length
    // is not passed: it is the high-water mark of the class entries that index
    // it, which is a fact the two tables already agree on.
    uint32_t total = 0;
    for (uint32_t c = 0; c < classCount; ++c) {
        const uint32_t end = classTable[c * 2 + 0] + classTable[c * 2 + 1];
        if (end > total) total = end;
    }
    for (uint32_t i = 0; i < total; ++i) {
        FamilyField f;
        // Bit 0 is the writable flag; the rest is the module's own key index,
        // which `keyMap` turns into the process-wide id every other part of the
        // runtime speaks.
        f.writable = (fieldTable[i] & 1u) != 0;
        f.keyId = keyMap[fieldTable[i] >> 1];
        fields().push_back(f);
    }
    for (uint32_t c = 0; c < classCount; ++c) {
        FamilyClass cls;
        cls.id = nextId() + c;
        cls.fieldStart = fieldBase + classTable[c * 2 + 0];
        cls.fieldCount = classTable[c * 2 + 1];
        if (cls.fieldCount > longestFieldCount()) longestFieldCount() = cls.fieldCount;
        classes().push_back(cls);
    }
    nextId() += classCount;
}

void bronze_family_stamp(uint64_t objBits) {
    recordHelperCall("bronze_family_stamp");
    // Census mode: leave every shape UNSTAMPED so the family-guarded static
    // sites keep missing into the ordinary IC sequence, whose helpers the
    // census records. The emitter calls-and-continues, so this is one extra
    // helper call per access, never a loop.
    if (censusFillsSuppressed()) return;
    Value objVal(objBits);
    if (!objVal.isObject()) return;
    HeapObjectHeader* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Plain) return;
    Shape* shape = reinterpret_cast<ObjectHeader*>(hdr)->shape;
    // One-shot per shape, re-checked here as well as in generated code: a site
    // whose guard just missed calls this, and two sites can miss on the same
    // shape before either has run.
    if (shape == nullptr || shape->family_stamp != BRONZE_ABI_FAMILY_UNSTAMPED) return;
    shape->family_stamp = classFamilyIdFor(shape);
}

}  // extern "C"

}  // namespace bronze::runtime
