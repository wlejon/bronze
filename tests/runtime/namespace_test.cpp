// The module namespace exotic object's OWN-KEY ORDER, asked directly.
//
// The oracle suite pins what a program sees (`cases/module_namespace_object`),
// and it can only ever see the order for the export names that case happens to
// use. What it cannot see is that the order is a SORT and not the order the
// literal was built in — a namespace whose exports were already alphabetical
// would pass every oracle case with no sort at all, and so would one that
// answered in a hash table's order for names the case never tries.
//
// So these build namespaces whose declaration order and sorted order disagree,
// including at the places a naive comparison gets wrong: capitals against
// lower case (10.4.6.2 sorts by CODE UNIT, so "Z" precedes "a"), a prefix
// against what extends it, and a digit against a letter.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/gc.h"
#include "runtime/fn.h"
#include "runtime/namespace.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

using namespace bronze;
using namespace bronze::runtime;

namespace {

// A getter that answers the same number every time. The namespace holds the
// FUNCTION, not the value, so what this returns only has to be distinguishable.
uint64_t constantSeven(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromDouble(7.0).rawBits();
}

// The object of getters the linker synthesizes, built here by hand: one
// get-only accessor per name, defined in the order given.
Value gettersFor(const std::vector<std::string>& names) {
    Rooted<Value> obj{Value(bronze_create_object())};
    Rooted<Value> getter{Value(bronze_function_singleton(constantSeven, 0))};
    Rooted<Value> absent{Value::fromUndefined()};
    for (const std::string& name : names) {
        Rooted<Value> key{rtMakeString(name)};
        ObjectHeader::defineAccessor(rtHeap(), rtArena(), obj, key, getter, absent,
                                     /*enumerable=*/true);
    }
    return obj.get();
}

std::vector<std::string> namespaceKeys(const std::vector<std::string>& declared) {
    Rooted<Value> ns{Value(bronze_module_namespace(gettersFor(declared).rawBits()))};
    std::vector<std::string> out;
    for (StringHeader* key : rtModuleNamespaceKeys(ns.get())) out.push_back(rtUtf8Chars(key));
    return out;
}

}  // namespace

TEST_CASE("a namespace's own keys are sorted, not the order they were declared") {
    ShadowStackFrame frame;
    CHECK(namespaceKeys({"z", "a"}) == std::vector<std::string>{"a", "z"});
    CHECK(namespaceKeys({"delta", "charlie", "bravo", "alpha"}) ==
          std::vector<std::string>{"alpha", "bravo", "charlie", "delta"});
}

TEST_CASE("the sort is by code unit and not by anything a locale would say") {
    ShadowStackFrame frame;
    // 'Z' is 0x5A and 'a' is 0x61, so every capital precedes every lower-case
    // letter. A case-insensitive or collating comparison answers the other way
    // round, which is exactly the drift 10.4.6.2 is written to exclude.
    CHECK(namespaceKeys({"a", "Z"}) == std::vector<std::string>{"Z", "a"});
    CHECK(namespaceKeys({"apple", "Apple"}) == std::vector<std::string>{"Apple", "apple"});
    // A digit (0x30..0x39) precedes every letter, and an underscore (0x5F)
    // sits between the capitals and the lower case.
    CHECK(namespaceKeys({"b", "_b", "0b", "B"}) ==
          std::vector<std::string>{"0b", "B", "_b", "b"});
}

TEST_CASE("a prefix sorts before what extends it") {
    ShadowStackFrame frame;
    CHECK(namespaceKeys({"parseInt", "parse", "parseFloat"}) ==
          std::vector<std::string>{"parse", "parseFloat", "parseInt"});
}

TEST_CASE("a namespace with no exports has no keys") {
    ShadowStackFrame frame;
    CHECK(namespaceKeys({}).empty());
}

TEST_CASE("a namespace answers a name it does not export with undefined") {
    ShadowStackFrame frame;
    Rooted<Value> ns{Value(bronze_module_namespace(gettersFor({"a"}).rawBits()))};
    Rooted<Value> present{rtMakeString("a")};
    Rooted<Value> absent{rtMakeString("missing")};

    Value found;
    REQUIRE(rtModuleNamespaceGet(ns.get(), present.get().asString<StringHeader>(), found));
    CHECK(found.isNumber());
    CHECK(found.asNumber() == 7.0);

    // Not an error, and not a diagnostic: 10.4.6.7 step 3 falls through to
    // `undefined` exactly as an ordinary object does. `import { missing }` is
    // where the early error lives.
    REQUIRE(rtModuleNamespaceGet(ns.get(), absent.get().asString<StringHeader>(), found));
    CHECK(found.isUndefined());
}

TEST_CASE("console.log of a namespace names the kind and the exports, in sorted order") {
    ShadowStackFrame frame;
    Rooted<Value> ns{
        Value(bronze_module_namespace(gettersFor({"z", "a", "Z"}).rawBits()))};
    // The VALUES are deliberately absent: reading one means calling the getter
    // that closes over the exporting binding, and this walk runs no user code
    // and allocates nothing — the same rule that prints an ordinary accessor as
    // `[Getter]`. The names are the whole of what the object is.
    CHECK(rtInspect(ns.get()) == "[Module: Z a z]");
    Rooted<Value> empty{Value(bronze_module_namespace(gettersFor({}).rawBits()))};
    CHECK(rtInspect(empty.get()) == "[Module]");
}

TEST_CASE("a namespace refuses every write, exported name or not") {
    ShadowStackFrame frame;
    Rooted<Value> ns{Value(bronze_module_namespace(gettersFor({"a"}).rawBits()))};

    // Sloppy: 10.4.6.9 still answers false, and the caller discards it. The
    // point of the `true` is that the write did NOT happen — a namespace that
    // reported "not my receiver" here would fall through to the ordinary
    // property path and store something.
    CHECK(rtModuleNamespaceWriteRefused(ns.get(), "a", /*strict=*/false));
    CHECK(rtModuleNamespaceWriteRefused(ns.get(), "neverExported", /*strict=*/false));
    CHECK_FALSE(bronze_exception_cell != BRONZE_ABI_NO_EXCEPTION_BITS);

    // An ordinary object is not this receiver kind, which is what keeps the
    // refusal from leaking onto every write in the program.
    Rooted<Value> plain{Value(bronze_create_object())};
    CHECK_FALSE(rtModuleNamespaceWriteRefused(plain.get(), "a", /*strict=*/true));
}
