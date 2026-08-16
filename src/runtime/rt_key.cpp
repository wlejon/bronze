// The KEY, for both directions of the property path.
//
// `o.k = v` and `o.k` are different operations on different storage — that is
// the seam rt_prop.cpp and rt_prop_write.cpp are split on — but they are one
// question about the key: whether it names an ELEMENT rather than a property,
// and what string it names when it does not. Those two answers must not drift,
// because a write that stored `a["01"]` as element 1 and a read that looked for
// the named property "01" would lose the value silently. So they live together,
// once, and both files reach them through rt_internal.h.
//
// What is NOT here is 7.1.19's step 1. ToPropertyKey of an OBJECT is
// ToPrimitive, which runs user code, and every function in this file promises to
// allocate at most a string — its callers hold raw receiver headers across it.
// So the object case is converted at the ABI entry points (`bronze_elem_get`,
// `bronze_elem_set`, `bronze_elem_delete`, and the two computed definition
// forms), where the receiver can be rooted across the call, and what arrives
// here is always the primitive that step produced.

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/number_format.h"
#include "runtime/rt_internal.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

// Whether a key names an ELEMENT rather than a named property, for the
// receivers that store their elements by index. This is `rtIsIntegerLikeKey`
// and nothing else: the canonical-array-index test, the same one enumeration
// order and `Object.keys` ask, so the two answers cannot drift. A
// leading-digits parse would send `a["1x"]` and `a["01"]` to element 1, which
// the language calls named properties (`index_keys`).
bool rtKeyAsIndex(const std::string& key, uint32_t& out) {
    return rtIsIntegerLikeKey(key, out);
}

// A computed index that names an element. ToPropertyKey makes `a[0]` and
// `a["0"]` the same property, so a STRING index that is a canonical array
// index has to reach the elements too — which is not an edge case: `for-in`
// yields keys as strings, so `arr[k]` inside one is the ordinary way to write
// this. Before the string branch existed, `for (const i in arr) arr[i]` died
// with "computed index must be a number" on the loop's own idiom.
//
// Every other key answers FALSE, which is not a fallback: a non-canonical
// string ("01", "1x"), a boolean, `null` and `undefined` all name PROPERTIES,
// which is what `rtElemKeyAsString` below turns them into. Arrays and typed
// arrays do not carry named properties, so their callers answer `undefined` for
// one, exactly as for an out-of-range index.
bool rtValueToElementIndex(Value idxVal, uint32_t& out) {
    if (idxVal.isString()) {
        const StringHeader* s = idxVal.asString<StringHeader>();
        if (!s->isLatin1()) return false;
        return rtIsIntegerLikeKey(std::string_view(s->latin1Data(), s->getLength()), out);
    }
    if (!idxVal.isNumber()) return false;
    double d = idxVal.asNumber();
    if (!(d >= 0.0) || d != std::floor(d) || d > 4294967294.0) return false;
    out = static_cast<uint32_t>(d);
    return true;
}

// ToPropertyKey (ECMA-262 7.1.19) as a heap string: every property name is a
// string, so `o[2]` and `o["2"]` name the same property and `{ [2]: v }` and `{
// 2: v }` write the same one. ToString(Number) is `formatJsNumber` and not
// console.log's inspect spelling — ToString(-0) is "0", where inspect says
// "-0".
//
// ALLOCATES, so the caller must have the receiver rooted before it calls.
Value rtElemKeyAsString(Value idxVal) {
    if (idxVal.isString()) return idxVal;
    char buf[64];
    size_t len = 0;
    if (idxVal.isNumber()) {
        len = formatJsNumber(idxVal.asNumber(), buf);
    } else if (idxVal.isBool()) {
        len = idxVal.asBool() ? 4 : 5;
        std::memcpy(buf, idxVal.asBool() ? "true" : "false", len);
    } else if (idxVal.isUndefined()) {
        len = 9;
        std::memcpy(buf, "undefined", len);
    } else if (idxVal.isNull()) {
        len = 4;
        std::memcpy(buf, "null", len);
    } else {
        // An OBJECT: 7.1.19's step 1 has already run at the ABI entry point, for
        // the reason this file's header gives — it is user code, and this
        // function's callers hold raw headers across it. Reaching here means an
        // entry point skipped that step, which is a bug in the property path
        // rather than something a program did.
        //
        // A SYMBOL never arrives here either: it is ALREADY a property key, so
        // every caller branches on it before conversion — converting one is the
        // TypeError that would turn `o[sym]` into a throw.
        fatal("internal: a computed property key that is still an object (7.1.19 runs "
              "ToPrimitive at the ABI entry point, so every key reaching this conversion has "
              "already been through it)");
    }
    return Value::fromString(
        StringHeader::createFromUTF8(rtHeap(), std::string_view(buf, len)));
}

}  // namespace bronze::runtime
