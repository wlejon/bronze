// The KEY, for both directions of the property path.
//
// `o.k = v` and `o.k` are different operations on different storage — that is
// the seam rt_prop.cpp and rt_prop_write.cpp are split on — but they are one
// question about the key: whether it names an ELEMENT rather than a property,
// and what string it names when it does not. Those two answers must not drift,
// because a write that stored `a["01"]` as element 1 and a read that looked for
// the named property "01" would lose the value silently. So they live together,
// once, and both files reach them through rt_property.h.
//
// What is NOT here is 7.1.19's step 1. ToPropertyKey of an OBJECT is
// ToPrimitive, which runs user code, and every function in this file promises to
// allocate at most a string — its callers hold raw receiver headers across it.
// So the object case is converted at the ABI entry points (`bronze_elem_get`,
// `bronze_elem_set`, `bronze_elem_delete`, and the two computed definition
// forms), where the receiver can be rooted across the call, and what arrives
// here is always the primitive that step produced.

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "runtime/bigint.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/number_format.h"
#include "runtime/rt_property.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

std::mutex g_keyMutex;
std::deque<std::string> g_keyStrings;
std::deque<KeyInfo> g_keyInfos;
std::unordered_map<std::string, uint32_t> g_keyIndex;
const std::string g_emptyKey;
const KeyInfo g_emptyKeyInfo{};

thread_local std::vector<StringHeader*> g_keyHeaders;

// BRONZE_NO_KEY_HASH_MEMO=1: a key header's hash is left to whoever asks first,
// as before (rtKeyHeader).
bool keyHashMemoDisabled() {
    static const bool off = std::getenv("BRONZE_NO_KEY_HASH_MEMO") != nullptr;
    return off;
}

// Mirrors g_keyHeaders' storage into the TLS block (bronze_abi_tls.h,
// key_header_tbl), which generated code's inline `box.str` reads. Called after
// every change of the vector's data pointer or size; a store into an existing
// entry needs nothing, the block already points at it.
void publishKeyHeaders() {
    bronze_tls_block* tls = rtTls();
    tls->key_header_tbl = reinterpret_cast<uint64_t*>(g_keyHeaders.data());
    tls->key_header_count = g_keyHeaders.size();
}
thread_local std::vector<const KeyInfo*> g_threadKeyInfos;
// Each thread's copy of the strings' addresses, as for the infos: a key's
// string never changes or moves once registered (a deque grows without
// moving its elements), and the lock was a contended mutex on every property
// write the helper answers (the miss path asks for the string up front).
thread_local std::vector<const std::string*> g_threadKeyStrings;

}  // namespace

const std::string& rtKeyString(uint32_t index) {
    if (index < g_threadKeyStrings.size()) {
        const std::string* cached = g_threadKeyStrings[index];
        if (cached != nullptr) return *cached;
    }
    std::lock_guard<std::mutex> lock(g_keyMutex);
    if (index >= g_keyStrings.size()) return g_emptyKey;
    const std::string* ptr = &g_keyStrings[index];
    if (index >= g_threadKeyStrings.size()) {
        g_threadKeyStrings.resize(index + 1, nullptr);
    }
    g_threadKeyStrings[index] = ptr;
    return *ptr;
}

StringHeader* rtKeyHeader(uint32_t index) {
    if (index >= g_keyHeaders.size()) {
        g_keyHeaders.resize(index + 1, nullptr);
        publishKeyHeaders();
    }
    StringHeader* hdr = g_keyHeaders[index];
    if (!hdr) {
        bool valid = false;
        std::string str;
        {
            std::lock_guard<std::mutex> lock(g_keyMutex);
            if (index < g_keyStrings.size()) {
                str = g_keyStrings[index];
                valid = true;
            }
        }
        if (valid) {
            // One copy with the shape keys: a key constant that is the very
            // pointer an object's shape holds matches it without reading the
            // characters, which is every lookup of a property that is there.
            hdr = StringHeader::adoptIntoArena(
                rtArena(), StringHeader::createFromUTF8InArena(rtArena(), std::string_view(str)));
            // The hash memoized now, once a key: the inline `===` rejects two
            // Strings with different bits by their memoized hashes and calls
            // the contents compare otherwise (il_lowering_ops.cpp
            // lower_dynamic_strict_eq), and a key constant compared against
            // another key — a `switch` on a string, a computed store split on
            // its key (lower_keyed_field_store.cpp) — is that pair. hash()
            // memoizes the same bits every time it runs, so asking early
            // changes no answer.
            if (!keyHashMemoDisabled()) hdr->hash();
            g_keyHeaders[index] = hdr;
        }
    }
    return hdr;
}

const KeyInfo& rtKeyInfo(uint32_t index) {
    if (index < g_threadKeyInfos.size()) {
        const KeyInfo* cached = g_threadKeyInfos[index];
        if (cached != nullptr) return *cached;
    }
    std::lock_guard<std::mutex> lock(g_keyMutex);
    if (index >= g_keyInfos.size()) return g_emptyKeyInfo;
    const KeyInfo* ptr = &g_keyInfos[index];
    if (index >= g_threadKeyInfos.size()) {
        g_threadKeyInfos.resize(index + 1, nullptr);
    }
    g_threadKeyInfos[index] = ptr;
    return *ptr;
}

void rtTeardownThreadKeys() {
    g_keyHeaders.clear();
    g_keyHeaders.shrink_to_fit();
    publishKeyHeaders();
    g_threadKeyInfos.clear();
    g_threadKeyInfos.shrink_to_fit();
    g_threadKeyStrings.clear();
    g_threadKeyStrings.shrink_to_fit();
}

extern "C" {

uint32_t bronze_register_key_string_len(const char* str, size_t len) {
    const std::string text(str ? str : "", str ? len : 0);
    std::lock_guard<std::mutex> lock(g_keyMutex);
    if (auto it = g_keyIndex.find(text); it != g_keyIndex.end()) return it->second;

    const uint32_t index = static_cast<uint32_t>(g_keyStrings.size());
    g_keyStrings.push_back(text);

    KeyInfo info;
    uint32_t elemIdx = 0;
    if (rtIsIntegerLikeKey(text, elemIdx)) {
        info.isElemIndex = true;
        info.elemIndex = elemIdx;
    } else {
        info.isElemIndex = false;
        info.elemIndex = UINT32_MAX;
    }
    info.isLength = (text == "length");
    g_keyInfos.push_back(info);
    g_keyIndex.emplace(text, index);
    return index;
}

uint32_t bronze_register_key_string(const char* str) {
    return bronze_register_key_string_len(str, str ? std::strlen(str) : 0);
}

}  // extern "C"

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
    // ToString(1n) is "1", so `a[1n]` is `a[1]`.
    if (idxVal.isBigInt()) {
        BigNum n = rtBigIntValue(idxVal);
        uint64_t mag = 0;
        if (n.isNegative() || !n.magnitudeToUint64(mag) || mag > 4294967294ULL) return false;
        out = static_cast<uint32_t>(mag);
        return true;
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
    if (idxVal.isBigInt()) {
        // 6.1.6.2.20 BigInt::toString; the digits are off the GC heap, so the
        // BigInt need not stay put across the allocation.
        std::string digits = rtBigIntToString(idxVal, 10);
        return Value::fromString(StringHeader::createFromUTF8(rtHeap(), digits));
    }
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
