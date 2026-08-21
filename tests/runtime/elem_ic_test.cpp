// The computed-read cache (runtime/elem_ic.h) at the level JS cannot see it.
//
// tests/oracle/cases/elem_ic_computed_read.js pins the ANSWERS — every way an
// entry can go stale, observed through `o[k]`. What that case cannot show is
// whether a right answer came from the cache or from the walk that the cache
// was supposed to replace, and a cache that never hits passes all sixteen of
// its scenarios. So this file asks the table directly: did the entry fill, is
// it the entry for THIS key, and is it still good after the collector has run.
//
// The last of those is the GC contract in one line. An entry holds a `Shape*`,
// an arena-interned `StringHeader*` and integers — all immortal, none of them
// scanned — precisely so that a collection between a fill and a hit changes
// nothing. Module BSS is not a root, so an entry that held a `Value` would be a
// dangling one at the first flip and this table would be a use-after-free
// generator rather than a cache.

#include <doctest/doctest.h>

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/elem_ic.h"
#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

Value str(const char* s) { return Value::fromString(StringHeader::createFromUTF8(rtHeap(), s)); }

// Every string this file compares is ASCII, so it is latin1 and reading it back
// is a length and a pointer.
std::string text(Value v) {
    const StringHeader* s = v.asString<StringHeader>();
    REQUIRE(s != nullptr);
    REQUIRE(s->isLatin1());
    return std::string(s->latin1Data(), s->length);
}

// A plain object with a prototype, the way a program gets one.
Value plainObject() { return Value(bronze_create_object()); }

void put(Rooted<Value>& obj, const char* name, Value value) {
    // `value` arrives unrooted, and building the key below ALLOCATES. Root it
    // first or a collection between the two leaves a from-space pointer to be
    // stored — which under BRONZE_GC_STRESS=1 is every call, and showed up as
    // the property reading back as its own key.
    Rooted<Value> val{value};
    Rooted<Value> key{str(name)};
    InlineCache ic;
    obj.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, val, &ic);
}

// `o[k]` as generated code reaches it.
Value elemGet(Rooted<Value>& obj, Value key) {
    return Value(bronze_elem_get(obj.get().rawBits(), key.rawBits()));
}

// Reads enough times that a cache which fills on second sight has filled.
Value warm(Rooted<Value>& obj, Value key, int times = 4) {
    Value last = Value::fromUndefined();
    for (int i = 0; i < times; ++i) {
        Rooted<Value> k{key};
        last = elemGet(obj, k.get());
    }
    return last;
}

// Every test below asks whether the MECHANISM works, so each one turns it on
// regardless of the ambient BRONZE_NO_ELEM_IC — an env var that switches a
// cache off must not be able to turn a suite red, and the seam's own effect is
// pinned by the last test here and by the oracle case, which is seam-invariant
// because it asserts answers rather than mechanism. Restores on the way out so
// one failure cannot leak a setting into the next test.
struct SeamOn {
    // `rtHeap()` first, and not incidentally: runtime start-up is LAZY and it
    // is what reads the BRONZE_NO_* environment. Setting the flag before the
    // first heap touch means start-up runs afterwards and puts the env's answer
    // back, which made this depend on whether some earlier test in the binary
    // had already woken the runtime.
    SeamOn() {
        (void)rtHeap();
        saved = bronze_tls_block_addr()->elem_ic_enabled;
        savedAbsent = bronze_tls_block_addr()->elem_absent_enabled;
        savedNeg = bronze_tls_block_addr()->negative_ic_enabled;
        bronze_tls_block_addr()->elem_ic_enabled = 1;
        bronze_tls_block_addr()->elem_absent_enabled = 1;
        // The absent half of this table is filled by the SAME
        // `rtInstallAbsentEntry` the named negative IC uses, so
        // BRONZE_NO_NEG_IC=1 switches it off too — an ambient one would make
        // the absence tests below fail for a reason that is not their subject.
        bronze_tls_block_addr()->negative_ic_enabled = 1;
    }
    ~SeamOn() {
        bronze_tls_block_addr()->elem_ic_enabled = saved;
        bronze_tls_block_addr()->elem_absent_enabled = savedAbsent;
        bronze_tls_block_addr()->negative_ic_enabled = savedNeg;
    }

private:
    uint64_t saved = 0;
    uint64_t savedAbsent = 0;
    uint64_t savedNeg = 0;
};

}  // namespace

TEST_CASE("a warm computed read is answered by the cache and not by the walk") {
    ShadowStackFrame frame;
    SeamOn seam;

    Rooted<Value> obj{plainObject()};
    put(obj, "position", Value::fromDouble(7.0));

    CHECK(warm(obj, str("position")).asNumber() == 7.0);

    // The probe is the same one the helper makes, so a hit here is proof the
    // helper's read was a hit too.
    Rooted<Value> key{str("position")};
    const ElemProbe probe = elemCacheProbe(obj.get(), key.get());
    CHECK(probe.hit);
    CHECK(probe.value.asNumber() == 7.0);
    CHECK(probe.kind == ElemKeyKind::String);

    // An equal string built somewhere else must reach the same entry: the
    // witness is a hash and the identity is the CONTENT, not the pointer.
    Rooted<Value> rebuilt{str("position")};
    CHECK(rebuilt.get().rawBits() != key.get().rawBits());
    const ElemProbe again = elemCacheProbe(obj.get(), rebuilt.get());
    CHECK(again.hit);
    CHECK(again.value.asNumber() == 7.0);
}

TEST_CASE("a number key naming a string property is cached without materialising the key") {
    ShadowStackFrame frame;
    SeamOn seam;

    // The three.js bucket this table was written for: integer-like own names
    // read with a number.
    Rooted<Value> obj{plainObject()};
    put(obj, "32926", str("src-alpha"));
    put(obj, "2960", str("stencil"));

    CHECK(text(warm(obj, Value::fromDouble(32926.0))) == "src-alpha");

    const ElemProbe probe = elemCacheProbe(obj.get(), Value::fromDouble(32926.0));
    CHECK(probe.hit);
    CHECK(probe.kind == ElemKeyKind::Number);
    // The witness is the double's bits, so nothing was stringified to answer.
    CHECK(probe.witness == Value::fromDouble(32926.0).rawBits());
}

TEST_CASE("an entry is about one KEY, not merely one shape") {
    ShadowStackFrame frame;
    SeamOn seam;

    // The failure a property site never has to think about: one shape, two
    // keys. An entry keyed on the shape alone would answer `beta` with alpha's
    // slot.
    Rooted<Value> obj{plainObject()};
    put(obj, "alpha", Value::fromDouble(10.0));
    put(obj, "beta", Value::fromDouble(20.0));

    CHECK(warm(obj, str("alpha")).asNumber() == 10.0);
    CHECK(warm(obj, str("beta")).asNumber() == 20.0);

    Rooted<Value> ka{str("alpha")};
    Rooted<Value> kb{str("beta")};
    const ElemProbe pa = elemCacheProbe(obj.get(), ka.get());
    const ElemProbe pb = elemCacheProbe(obj.get(), kb.get());
    CHECK(pa.hit);
    CHECK(pb.hit);
    CHECK(pa.value.asNumber() == 10.0);
    CHECK(pb.value.asNumber() == 20.0);
    // Two keys of one shape are two entries, so neither evicted the other.
    CHECK(pa.entry != pb.entry);
}

TEST_CASE("an entry names a SLOT, so every receiver of the shape reads its own") {
    ShadowStackFrame frame;
    SeamOn seam;

    Rooted<Value> a{plainObject()};
    Rooted<Value> b{plainObject()};
    put(a, "v", Value::fromDouble(1.0));
    put(b, "v", Value::fromDouble(2.0));

    CHECK(warm(a, str("v")).asNumber() == 1.0);
    CHECK(warm(b, str("v")).asNumber() == 2.0);

    // A write through the same key under a warm entry: a cache that remembered
    // the VALUE would answer 1 here.
    Rooted<Value> k{str("v")};
    Rooted<Value> nine{Value::fromDouble(9.0)};
    InlineCache ic;
    a.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), k, nine, &ic);
    CHECK(elemGet(a, str("v")).asNumber() == 9.0);
}

TEST_CASE("the entry survives collections, because it holds nothing the collector moves") {
    ShadowStackFrame frame;
    SeamOn seam;

    Rooted<Value> obj{plainObject()};
    put(obj, "uniform", Value::fromDouble(5.0));
    CHECK(warm(obj, str("uniform")).asNumber() == 5.0);

    const uint64_t before = obj.get().rawBits();
    rtHeap().collect();
    // The receiver really did move; the shape it points at did not, and the
    // entry is about the shape.
    CHECK(obj.get().rawBits() != before);

    Rooted<Value> key{str("uniform")};
    const ElemProbe probe = elemCacheProbe(obj.get(), key.get());
    CHECK(probe.hit);
    CHECK(probe.value.asNumber() == 5.0);
    CHECK(elemGet(obj, str("uniform")).asNumber() == 5.0);

    // Twice more, so the semispace has flipped back and forth under the entry.
    rtHeap().collect();
    rtHeap().collect();
    CHECK(elemGet(obj, str("uniform")).asNumber() == 5.0);
}

TEST_CASE("a shape change retires the entry rather than answering from it") {
    ShadowStackFrame frame;
    SeamOn seam;

    Rooted<Value> obj{plainObject()};
    put(obj, "first", Value::fromDouble(1.0));
    CHECK(warm(obj, str("first")).asNumber() == 1.0);

    put(obj, "second", Value::fromDouble(2.0));  // a transition: a new shape

    Rooted<Value> k{str("first")};
    const ElemProbe stale = elemCacheProbe(obj.get(), k.get());
    CHECK_FALSE(stale.hit);
    // And the read still answers, through the walk.
    CHECK(elemGet(obj, str("first")).asNumber() == 1.0);
}

TEST_CASE("a proven-absent computed read arms the cache and answers undefined from it") {
    ShadowStackFrame frame;
    SeamOn seam;

    Rooted<Value> obj{plainObject()};
    put(obj, "present", Value::fromDouble(1.0));

    CHECK(warm(obj, str("missing")).isUndefined());
    Rooted<Value> k{str("missing")};
    const ElemProbe probe = elemCacheProbe(obj.get(), k.get());
    // The whole point of this chunk: chunk 3 measured 1.80 M of 1.80 M residual
    // misses as `entry_empty` on stable pairs, and every one of them is now a
    // hit — from the ENTRY, with no own-slot lookup and no chain walk.
    CHECK(probe.hit);
    CHECK(probe.value.isUndefined());
    REQUIRE(probe.entry != nullptr);
    CHECK(probe.entry->ic.isAbsent());
}

TEST_CASE("an absent entry is retired by the key appearing, on the object and above it") {
    ShadowStackFrame frame;
    SeamOn seam;

    SUBCASE("an own add transitions the shape") {
        Rooted<Value> obj{plainObject()};
        put(obj, "present", Value::fromDouble(1.0));
        CHECK(warm(obj, str("late")).isUndefined());

        put(obj, "late", Value::fromDouble(9.0));
        CHECK(elemGet(obj, str("late")).asNumber() == 9.0);
    }

    SUBCASE("an add to the PROTOTYPE bumps the epoch") {
        Rooted<Value> proto{plainObject()};
        put(proto, "anchor", Value::fromDouble(0.0));
        Rooted<Value> obj{Value::fromObject(ObjectHeader::create(
            rtHeap(), rtArena(), rtRootShapeForPrototype(proto.get())))};
        put(obj, "own", Value::fromDouble(1.0));

        CHECK(warm(obj, str("inherited")).isUndefined());
        Rooted<Value> k{str("inherited")};
        CHECK(elemCacheProbe(obj.get(), k.get()).hit);

        put(proto, "inherited", Value::fromDouble(7.0));
        // The receiver's own shape did not change; the epoch is what retires
        // the entry, and it is asked by `describesAbsent` and nothing else.
        CHECK_FALSE(elemCacheProbe(obj.get(), k.get()).hit);
        CHECK(elemGet(obj, str("inherited")).asNumber() == 7.0);
    }
}

TEST_CASE("an absent entry survives collection, because it holds nothing movable") {
    ShadowStackFrame frame;
    SeamOn seam;

    Rooted<Value> obj{plainObject()};
    put(obj, "present", Value::fromDouble(1.0));
    CHECK(warm(obj, str("gone")).isUndefined());

    rtHeap().collect();
    rtHeap().collect();

    Rooted<Value> k{str("gone")};
    // A FRESH key string, built after two flips: the entry is keyed on the
    // arena copy and matched by content, so the heap string it was filled from
    // being long dead is not a question the table has to answer.
    CHECK(elemCacheProbe(obj.get(), k.get()).hit);
    CHECK(elemGet(obj, str("gone")).isUndefined());
}

TEST_CASE("the absent seam narrows this table without touching the present half") {
    ShadowStackFrame frame;
    SeamOn seam;
    bronze_tls_block_addr()->elem_absent_enabled = 0;

    Rooted<Value> obj{plainObject()};
    put(obj, "here", Value::fromDouble(3.0));

    CHECK(warm(obj, str("here")).asNumber() == 3.0);
    Rooted<Value> present{str("here")};
    CHECK(elemCacheProbe(obj.get(), present.get()).hit);

    CHECK(warm(obj, str("nowhere")).isUndefined());
    Rooted<Value> absent{str("nowhere")};
    CHECK_FALSE(elemCacheProbe(obj.get(), absent.get()).hit);
}

TEST_CASE("the arena key table deduplicates, so a repeated absent key is copied once") {
    ShadowStackFrame frame;
    SeamOn seam;

    Rooted<Value> a{str("repeated")};
    StringHeader* first = elemCacheInternKey(a.get().asString<StringHeader>());
    REQUIRE(first != nullptr);
    // A different heap string with the same content, after a collection has
    // moved everything: same arena copy, because the table matches on content.
    rtHeap().collect();
    Rooted<Value> b{str("repeated")};
    CHECK(elemCacheInternKey(b.get().asString<StringHeader>()) == first);
}

TEST_CASE("a key kind with no string form is refused before anything is recorded") {
    ShadowStackFrame frame;
    SeamOn seam;

    Rooted<Value> obj{plainObject()};
    put(obj, "x", Value::fromDouble(1.0));

    // A symbol has no name to reach the string path with; the probe declines to
    // offer an entry at all, so nothing can be filled for it.
    Rooted<Value> desc{str("s")};
    Rooted<Value> sym{rtMakeSymbol(desc.get())};
    const ElemProbe probe = elemCacheProbe(obj.get(), sym.get());
    CHECK_FALSE(probe.hit);
    CHECK(probe.entry == nullptr);
    CHECK(probe.kind == ElemKeyKind::Empty);
}

TEST_CASE("a boolean key is a name, and gets its own kind") {
    ShadowStackFrame frame;
    SeamOn seam;

    Rooted<Value> obj{plainObject()};
    put(obj, "false", str("F"));
    put(obj, "true", str("T"));

    CHECK(text(warm(obj, Value::fromBool(false))) == "F");
    const ElemProbe probe = elemCacheProbe(obj.get(), Value::fromBool(false));
    CHECK(probe.hit);
    CHECK(probe.kind == ElemKeyKind::Boolean);
    CHECK(probe.witness == 0);

    CHECK(text(warm(obj, Value::fromBool(true))) == "T");
    const ElemProbe other = elemCacheProbe(obj.get(), Value::fromBool(true));
    CHECK(other.hit);
    CHECK(other.witness == 1);
}

TEST_CASE("BRONZE_NO_ELEM_IC turns the table off without changing an answer") {
    ShadowStackFrame frame;
    SeamOn seam;

    Rooted<Value> obj{plainObject()};
    put(obj, "seam", Value::fromDouble(11.0));
    CHECK(warm(obj, str("seam")).asNumber() == 11.0);

    bronze_tls_block_addr()->elem_ic_enabled = 0;
    CHECK_FALSE(elemCacheEnabled());
    Rooted<Value> k{str("seam")};
    const ElemProbe off = elemCacheProbe(obj.get(), k.get());
    CHECK_FALSE(off.hit);
    // No entry offered means no fill either, so the seam is off for both halves.
    CHECK(off.entry == nullptr);
    // The read is unchanged; only where the answer came from changed.
    CHECK(elemGet(obj, str("seam")).asNumber() == 11.0);

    bronze_tls_block_addr()->elem_ic_enabled = 1;
    CHECK(elemCacheEnabled());
    CHECK(warm(obj, str("seam")).asNumber() == 11.0);
    Rooted<Value> k2{str("seam")};
    CHECK(elemCacheProbe(obj.get(), k2.get()).hit);
}
