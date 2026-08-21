// The layout-family registry (runtime/class_family.h) at the level JS cannot
// see it.
//
// `tests/oracle/cases/family_guard.js` pins the ANSWERS — a base site over five
// classes, a sibling branch refused, a foreign receiver, a dictionary, a
// subclass the compiler never modelled. What that case cannot show is whether
// an answer came from the family FAST PATH or from the inline cache behind it:
// a stamper that never stamps anything passes every one of its scenarios, and
// so does one that stamps everything with the wrong id, because the wrong id
// only ever makes a guard miss.
//
// So this file asks the registry directly, and pins the four things the guard's
// soundness rests on: that a prefix match is a match on NAMES, on SLOTS, on
// DATA-ness and on WRITABILITY; that the most specific match wins; that a
// dictionary shape is never stamped; and that a module's ids are its own.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/class_family.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// A module's registration, expressed the way a compiled object file expresses
// it: class rows into a flat field table, field words carrying the module's own
// key index and the writable bit, and a key map turning those indices into the
// process-wide ids the registry stores.
struct FakeModule {
    std::vector<uint32_t> classes;
    std::vector<uint32_t> fields;
    std::vector<uint32_t> keyMap;
    uint64_t base = 0;

    // Returns the module-local key index for `name`, registering the string
    // process-wide on the way.
    uint32_t key(const std::string& name) {
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == name) return static_cast<uint32_t>(i);
        }
        names.push_back(name);
        keyMap.push_back(bronze_register_key_string(name.c_str()));
        return static_cast<uint32_t>(names.size() - 1);
    }

    void addClass(const std::vector<std::pair<std::string, bool>>& layout) {
        classes.push_back(static_cast<uint32_t>(fields.size()));
        classes.push_back(static_cast<uint32_t>(layout.size()));
        for (const auto& f : layout) {
            fields.push_back((key(f.first) << 1) | (f.second ? 1u : 0u));
        }
    }

    void registerAll() {
        bronze_register_class_family(classes.data(),
                                     static_cast<uint32_t>(classes.size() / 2), fields.data(),
                                     keyMap.data(), &base);
    }

    std::vector<std::string> names;
};

// A shape carrying exactly `layout`, built the way the transition tree builds
// one. `writable` is part of the transition KEY, so two shapes differing only
// in it are two nodes — which is the whole reason the stamper can compare it.
Shape* shapeWith(NonMovingArena& arena, Heap& heap, Shape* root,
                 const std::vector<std::pair<std::string, bool>>& layout) {
    Shape* cur = root;
    for (const auto& f : layout) {
        Rooted<Value> name(Value::fromString(StringHeader::createFromUTF8(heap, f.first)));
        uint32_t slot = 0;
        cur = cur->addProperty(arena, heap, name, slot, /*is_enumerable=*/true,
                               /*is_accessor=*/false, /*is_writable=*/f.second,
                               /*is_configurable=*/true);
    }
    return cur;
}

using Layout = std::vector<std::pair<std::string, bool>>;

}  // namespace

TEST_CASE("class family stamps the most specific matching layout") {
    classFamilyResetForTesting();
    NonMovingArena& arena = rtArena();
    Heap& heap = rtHeap();
    ShadowStackFrame frame;

    // Base [id(ro), a, b] -> Mid [.. c] -> Leaf [.. d], and a sibling of Mid
    // whose slot 3 is `g`. Preorder, exactly as the compiler emits it.
    const Layout base = {{"id", false}, {"a", true}, {"b", true}};
    Layout mid = base;
    mid.push_back({"c", true});
    Layout leaf = mid;
    leaf.push_back({"d", true});
    Layout sib = base;
    sib.push_back({"g", true});

    FakeModule mod;
    mod.addClass(base);
    mod.addClass(mid);
    mod.addClass(leaf);
    mod.addClass(sib);
    mod.registerAll();
    CHECK(mod.base == BRONZE_ABI_FAMILY_FIRST_ID);
    CHECK(classFamilyCount() == 4);

    Shape* root = Shape::createRoot(arena);
    // The deepest full prefix match wins, which is what lets one stamp serve
    // both the base's sites and the subclass's own.
    CHECK(classFamilyIdFor(shapeWith(arena, heap, root, base)) == mod.base + 0);
    CHECK(classFamilyIdFor(shapeWith(arena, heap, root, mid)) == mod.base + 1);
    CHECK(classFamilyIdFor(shapeWith(arena, heap, root, leaf)) == mod.base + 2);
    CHECK(classFamilyIdFor(shapeWith(arena, heap, root, sib)) == mod.base + 3);

    // A LATE field — one a method installs after construction — leaves the
    // prefix intact, so the shape is still the class it was.
    Layout leafPlus = leaf;
    leafPlus.push_back({"_listeners", true});
    CHECK(classFamilyIdFor(shapeWith(arena, heap, root, leafPlus)) == mod.base + 2);

    // A shape stopped PART WAY through a class's own fields — what a receiver
    // looks like inside a constructor — matches the deepest class it completed
    // and no further.
    Layout partial = base;
    partial.push_back({"c", true});
    partial.push_back({"zz", true});
    CHECK(classFamilyIdFor(shapeWith(arena, heap, root, partial)) == mod.base + 1);
}

TEST_CASE("class family refuses a prefix that differs in name, order or attributes") {
    classFamilyResetForTesting();
    NonMovingArena& arena = rtArena();
    Heap& heap = rtHeap();
    ShadowStackFrame frame;

    const Layout base = {{"id", false}, {"a", true}, {"b", true}};
    FakeModule mod;
    mod.addClass(base);
    mod.registerAll();

    Shape* root = Shape::createRoot(arena);
    CHECK(classFamilyIdFor(shapeWith(arena, heap, root, base)) == mod.base);

    // The same three names in a different ORDER is a different layout: slot 1
    // would be `b` where the class says `a`.
    CHECK(classFamilyIdFor(shapeWith(arena, heap, root, {{"id", false}, {"b", true}, {"a", true}})) ==
          BRONZE_ABI_FAMILY_NONE);
    // A name the class does not have at all.
    CHECK(classFamilyIdFor(shapeWith(arena, heap, root, {{"id", false}, {"a", true}, {"q", true}})) ==
          BRONZE_ABI_FAMILY_NONE);
    // SHORTER than the class: the tail slots are not there to be claimed.
    CHECK(classFamilyIdFor(shapeWith(arena, heap, root, {{"id", false}, {"a", true}})) ==
          BRONZE_ABI_FAMILY_NONE);
    // The right names in the right order, but `id` WRITABLE. A write site that
    // accepted this would be storing into a slot the class proved it could
    // store into, on an object where that is a different property entirely —
    // and, the other way round, a class that declared it writable must not
    // recognise a shape that froze it.
    CHECK(classFamilyIdFor(shapeWith(arena, heap, root, {{"id", true}, {"a", true}, {"b", true}})) ==
          BRONZE_ABI_FAMILY_NONE);
    // An empty shape claims nothing.
    CHECK(classFamilyIdFor(root) == BRONZE_ABI_FAMILY_NONE);
}

TEST_CASE("class family refuses an accessor in the prefix") {
    classFamilyResetForTesting();
    NonMovingArena& arena = rtArena();
    Heap& heap = rtHeap();
    ShadowStackFrame frame;

    FakeModule mod;
    mod.addClass({{"a", true}, {"b", true}, {"c", true}});
    mod.registerAll();

    Shape* root = Shape::createRoot(arena);
    // `b` as an accessor occupies slots 1 AND 2, so `c` lands at slot 3 and
    // nothing owns slot 2 — the exact shift a name-only check would miss.
    Rooted<Value> a(Value::fromString(StringHeader::createFromUTF8(heap, "a")));
    Rooted<Value> b(Value::fromString(StringHeader::createFromUTF8(heap, "b")));
    Rooted<Value> c(Value::fromString(StringHeader::createFromUTF8(heap, "c")));
    uint32_t slot = 0;
    Shape* s = root->addProperty(arena, heap, a, slot);
    s = s->addProperty(arena, heap, b, slot, /*is_enumerable=*/true, /*is_accessor=*/true);
    CHECK(slot == 1);
    s = s->addProperty(arena, heap, c, slot);
    CHECK(slot == 3);
    CHECK(classFamilyIdFor(s) == BRONZE_ABI_FAMILY_NONE);
}

TEST_CASE("class family ids are per module") {
    classFamilyResetForTesting();
    NonMovingArena& arena = rtArena();
    Heap& heap = rtHeap();
    ShadowStackFrame frame;

    // Two modules that both declare a class with the same field list. They are
    // two classes: a shape is stamped with whichever the registry answers, and
    // the OTHER module's guard — a range compare around its own base — has to
    // miss rather than accept a class it never proved anything about.
    const Layout same = {{"x", true}, {"y", true}};
    FakeModule modA;
    modA.addClass(same);
    modA.registerAll();
    FakeModule modB;
    modB.addClass(same);
    modB.registerAll();

    CHECK(modA.base != modB.base);
    CHECK(modB.base == modA.base + 1);
    CHECK(classFamilyCount() == 2);

    Shape* root = Shape::createRoot(arena);
    const uint64_t id = classFamilyIdFor(shapeWith(arena, heap, root, same));
    // Ties go to the later registration, so B wins here — and A's range, which
    // is [modA.base, modA.base], does not contain it.
    CHECK(id == modB.base);
    CHECK(id - modA.base > 0u);
}

TEST_CASE("class family never stamps a dictionary shape") {
    classFamilyResetForTesting();
    NonMovingArena& arena = rtArena();
    Heap& heap = rtHeap();
    ShadowStackFrame frame;

    FakeModule mod;
    mod.addClass({{"a", true}, {"b", true}});
    mod.registerAll();

    Shape* root = Shape::createRoot(arena);
    Shape* shaped = shapeWith(arena, heap, root, {{"a", true}, {"b", true}});
    REQUIRE(classFamilyIdFor(shaped) == mod.base);

    // A dictionary shape belongs to ONE object and its slot numbering is a
    // run-time fact, so there is nothing here a compile-time offset could name.
    // Every escape a program has from a shaped object — delete, freeze, an
    // attribute demotion, setPrototypeOf — ends here, which is why refusing
    // this one shape covers all of them.
    Rooted<Value> obj(Value::fromObject(ObjectHeader::create(heap, arena, shaped)));
    ObjectHeader::toDictionary(arena, obj);
    Shape* dict = obj.get().asObject<ObjectHeader>()->shape;
    REQUIRE(dict->isDictionary());
    CHECK(classFamilyIdFor(dict) == BRONZE_ABI_FAMILY_NONE);

    // The helper records that refusal in the word — NONE, not left UNSTAMPED —
    // so the site behind it stops asking. A dictionary shape belongs to one
    // object and never becomes anyone's parent, so the note dies with it.
    bronze_family_stamp(obj.get().rawBits());
    CHECK(dict->family_stamp == BRONZE_ABI_FAMILY_NONE);
}

TEST_CASE("bronze_family_stamp writes once per shape") {
    classFamilyResetForTesting();
    NonMovingArena& arena = rtArena();
    Heap& heap = rtHeap();
    ShadowStackFrame frame;

    FakeModule mod;
    mod.addClass({{"a", true}, {"b", true}});
    mod.registerAll();

    Shape* root = Shape::createRoot(arena);
    Shape* shaped = shapeWith(arena, heap, root, {{"a", true}, {"b", true}});
    Rooted<Value> obj(Value::fromObject(ObjectHeader::create(heap, arena, shaped)));

    CHECK(shaped->family_stamp == BRONZE_ABI_FAMILY_UNSTAMPED);
    bronze_family_stamp(obj.get().rawBits());
    CHECK(shaped->family_stamp == mod.base);

    // A shape that matched nothing is stamped NONE, not left at zero — which is
    // what stops a site's slow path calling the stamper again on every miss for
    // the life of the program.
    Shape* alien = shapeWith(arena, heap, root, {{"q", true}});
    Rooted<Value> other(Value::fromObject(ObjectHeader::create(heap, arena, alien)));
    bronze_family_stamp(other.get().rawBits());
    CHECK(alien->family_stamp == BRONZE_ABI_FAMILY_NONE);

    // A second module registering later cannot revise either answer. That is a
    // missed optimization and never a wrong one, and it is the price of the
    // word being written once.
    FakeModule late;
    late.addClass({{"q", true}});
    late.registerAll();
    bronze_family_stamp(other.get().rawBits());
    CHECK(alien->family_stamp == BRONZE_ABI_FAMILY_NONE);
}
