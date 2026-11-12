// `RegExp.prototype.exec` (ECMA-262 22.2.6.2) and everything it stands on:
// the compiled-pattern table, the match array, and the `lastIndex` protocol
// of 22.2.7.2. Every other regular-expression operation in bronze — `test`,
// the five symbol-keyed members, the string members that call them — is built
// from what is here.
//
// The pattern grammar and the matcher are `src/regex` and know nothing about
// values; what is here is the JavaScript surface over them. Three things it
// owns and they do not:
//
//  - the COMPILED-PATTERN TABLE. A `regex::Pattern` is a C++ tree the moving
//    collector must never touch, so it lives outside the heap and a RegExp
//    holds its index. Two regular expressions with the same source and flags
//    share one entry, which is what stops a literal inside a loop from
//    recompiling its pattern per iteration.
//  - `lastIndex`, and the `g`/`y` protocol around it (22.2.7.2).
//  - the MATCH ARRAY: an array of captures that also carries `index`, `input`
//    and `groups`, which is why arrays grew a named-property object.

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "abi/bronze_abi.h"
#include "regex/regex.h"
#include "runtime/array.h"
#include "runtime/builtin_regexp_internal.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/integrity.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// Bounded LRU cache for compiled regex patterns (capped at 512 entries).
// When capacity is exceeded, the least recently used pattern is evicted and deleted.
// If an older RegExp whose pattern was evicted is executed, programOf detects the
// eviction via the encoded slot/generation ID and re-compiles the pattern transparently.
class RegExpCache {
public:
    static constexpr size_t kCapacity = 512;
    static constexpr uint32_t kInvalid = UINT32_MAX;

    struct Entry {
        std::string key;
        std::shared_ptr<const regex::Pattern> pattern;
        uint32_t prev{kInvalid};
        uint32_t next{kInvalid};
        uint64_t id{0};
    };

    RegExpCache() : entries_(kCapacity) {}

    uint32_t lookup(const std::string& key) {
        auto it = keyToSlot_.find(key);
        if (it == keyToSlot_.end()) return kInvalid;
        uint32_t slot = it->second;
        touch(slot);
        return slot;
    }

    uint32_t insert(const std::string& key, regex::PatternPtr pattern) {
        auto it = keyToSlot_.find(key);
        if (it != keyToSlot_.end()) {
            uint32_t slot = it->second;
            entries_[slot].pattern = std::move(pattern);
            entries_[slot].id = makeId(slot);
            touch(slot);
            return slot;
        }

        uint32_t slot = kInvalid;
        if (size_ < kCapacity) {
            slot = static_cast<uint32_t>(size_++);
        } else {
            // Evict least-recently used (tail)
            slot = tail_;
            keyToSlot_.erase(entries_[slot].key);
            unlink(slot);
            entries_[slot].pattern.reset();
        }

        entries_[slot].key = key;
        entries_[slot].pattern = std::move(pattern);
        entries_[slot].id = makeId(slot);
        keyToSlot_[key] = slot;
        pushFront(slot);
        return slot;
    }

    bool isValid(uint32_t slot, uint64_t expectedId) const {
        return slot < size_ && entries_[slot].id == expectedId && entries_[slot].pattern != nullptr;
    }

    const Entry& entry(uint32_t slot) const { return entries_[slot]; }
    uint64_t entryId(uint32_t slot) const { return entries_[slot].id; }

    void touch(uint32_t slot) {
        if (head_ == slot) return;
        unlink(slot);
        pushFront(slot);
    }

    size_t size() const { return size_; }

private:
    uint64_t makeId(uint32_t slot) {
        generation_ = (generation_ + 1) & 0x1FFFFFFFFULL;
        return (generation_ << 16) | (static_cast<uint64_t>(slot) & 0xFFFFULL);
    }

    void unlink(uint32_t slot) {
        Entry& e = entries_[slot];
        if (e.prev != kInvalid) {
            entries_[e.prev].next = e.next;
        } else if (head_ == slot) {
            head_ = e.next;
        }
        if (e.next != kInvalid) {
            entries_[e.next].prev = e.prev;
        } else if (tail_ == slot) {
            tail_ = e.prev;
        }
        e.prev = kInvalid;
        e.next = kInvalid;
    }

    void pushFront(uint32_t slot) {
        Entry& e = entries_[slot];
        e.prev = kInvalid;
        e.next = head_;
        if (head_ != kInvalid) {
            entries_[head_].prev = slot;
        }
        head_ = slot;
        if (tail_ == kInvalid) {
            tail_ = slot;
        }
    }

    std::vector<Entry> entries_;
    std::map<std::string, uint32_t> keyToSlot_;
    uint32_t head_{kInvalid};
    uint32_t tail_{kInvalid};
    size_t size_{0};
    uint64_t generation_{0};
};

RegExpCache& regExpCache() {
    static thread_local RegExpCache cache;
    return cache;
}

regex::Units unitsOfString(const StringHeader* str) {
    const std::vector<uint16_t> units = rtStringUnits(str);
    return regex::Units(units.begin(), units.end());
}

// Compiles, or answers the index of an identical earlier compilation. Throws
// when it cannot: 22.2.3.1 step 4 makes a pattern that does not parse a
// SyntaxError, and a pattern bronze refuses names itself in the same message
// (`src/regex` writes both).
bool programFor(const std::string& sourceUtf8, const regex::Units& source,
                const std::string& flagsText, regex::Flags& flags, uint64_t& out) {
    std::string error;
    if (!regex::parseFlags(flagsText, flags, error)) {
        rtThrowSyntaxError(error);
        return false;
    }
    // Keyed on the CANONICAL flags, so `/a/gi` and `/a/ig` are one
    // compilation: the flag letters are a set, and two spellings of the same
    // set describe the same pattern.
    const std::string key = flags.text() + "\n" + sourceUtf8;
    auto& cache = regExpCache();
    uint32_t slot = cache.lookup(key);
    if (slot != RegExpCache::kInvalid) {
        out = cache.entryId(slot);
        return true;
    }
    regex::PatternPtr pattern = regex::compile(source, flags, error);
    if (!pattern) {
        rtThrowSyntaxError("Invalid regular expression: /" + sourceUtf8 + "/" + flagsText + ": " +
                           error);
        return false;
    }
    slot = cache.insert(key, std::move(pattern));
    out = cache.entryId(slot);
    return true;
}

bool isRegExp(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == RegExpHeader::kFlags;
}

std::shared_ptr<const regex::Pattern> programSharedOf(Value re) {
    auto* header = re.asObject<RegExpHeader>();
    uint64_t id = static_cast<uint64_t>(header->programIndex.asNumber());
    uint32_t slot = static_cast<uint32_t>(id & 0xFFFF);
    auto& cache = regExpCache();
    if (cache.isValid(slot, id)) {
        cache.touch(slot);
        return cache.entry(slot).pattern;
    }

    // Cache miss or slot was evicted: recover using source and flagsText.
    const std::string flagsText = rtUtf8Chars(header->flagsText.asString<StringHeader>());
    const std::string sourceUtf8 = rtUtf8Chars(header->source.asString<StringHeader>());
    const std::string key = flagsText + "\n" + sourceUtf8;

    uint32_t existing = cache.lookup(key);
    if (existing != RegExpCache::kInvalid) {
        header->programIndex = Value::fromDouble(static_cast<double>(cache.entryId(existing)));
        return cache.entry(existing).pattern;
    }

    regex::Flags flags;
    std::string error;
    if (!regex::parseFlags(flagsText, flags, error)) {
        fatal("internal: stored RegExp flags failed to re-parse");
    }
    const regex::Units source = unitsOfString(header->source.asString<StringHeader>());
    regex::PatternPtr pattern = regex::compile(source, flags, error);
    if (!pattern) {
        fatal("internal: stored RegExp pattern failed to re-compile");
    }
    uint32_t newSlot = cache.insert(key, std::move(pattern));
    header->programIndex = Value::fromDouble(static_cast<double>(cache.entryId(newSlot)));
    return cache.entry(newSlot).pattern;
}

const regex::Pattern& programOf(Value re) { return *programSharedOf(re); }

// 22.2.6.10 EscapeRegExpPattern. `source` is the text a LITERAL of this
// pattern would carry, so it must be a pattern that can sit between two
// slashes on one line: the empty pattern is spelled `(?:)` (`//` is a line
// comment), and an unescaped `/` or line terminator — which only a pattern
// built from a string can contain — is escaped. Escaping happens before the
// compile, and every escape introduced here compiles back to the character it
// replaced, so the pattern the source describes is still the pattern that runs.
std::string escapeRegExpPattern(const std::string& pattern) {
    if (pattern.empty()) return "(?:)";
    std::string out;
    out.reserve(pattern.size());
    // A `/` inside a character class does not end a literal (12.9.5's
    // RegularExpressionClass), so it stays as written there, as node keeps it.
    bool inClass = false;
    for (size_t i = 0; i < pattern.size(); ++i) {
        const char c = pattern[i];
        if (c == '\\' && i + 1 < pattern.size()) {
            out.push_back(c);
            out.push_back(pattern[++i]);
            continue;
        }
        switch (c) {
            case '[': inClass = true; out.push_back(c); break;
            case ']': inClass = false; out.push_back(c); break;
            case '/':
                if (inClass) {
                    out.push_back(c);
                } else {
                    out += "\\/";
                }
                break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default:
                // U+2028 and U+2029 are line terminators too, and are the only
                // multi-byte ones; matching their UTF-8 bytes is exact because
                // no other code point encodes to a sequence containing them.
                if (i + 2 < pattern.size() && static_cast<unsigned char>(c) == 0xE2 &&
                    static_cast<unsigned char>(pattern[i + 1]) == 0x80 &&
                    (static_cast<unsigned char>(pattern[i + 2]) == 0xA8 ||
                     static_cast<unsigned char>(pattern[i + 2]) == 0xA9)) {
                    out += static_cast<unsigned char>(pattern[i + 2]) == 0xA8 ? "\\u2028"
                                                                             : "\\u2029";
                    i += 2;
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    return out;
}

struct MatchPropsShapes {
    Shape* nullRoot{nullptr};
    Shape* match3{nullptr};
    Shape* matchWithIdx{nullptr};
    Shape* indices1{nullptr};
    uint32_t indexSlot{0};
    uint32_t inputSlot{0};
    uint32_t groupsSlot{0};
    uint32_t indicesSlot{0};
    uint32_t idxGroupsSlot{0};
};

// Cached per-thread hidden classes for the null-prototype `properties` bucket of
// match arrays (`index`, `input`, `groups`, optional `indices`) and `/d` index
// arrays (`groups`), keyed on the current thread's null-prototype root shape so
// thread-state teardown/recreation rebuilds cleanly.
const MatchPropsShapes& matchPropsShapes() {
    static thread_local MatchPropsShapes cached;
    Shape* root = rtRootShapeForPrototype(Value::fromNull());
    if (BRONZE_LIKELY(cached.nullRoot == root)) return cached;

    NonMovingArena& arena = rtArena();
    auto internAscii = [&](std::string_view sv) {
        StringHeader* s = StringHeader::adoptIntoArena(
            arena, StringHeader::createLatin1InArena(arena, sv.data(),
                                                     static_cast<uint32_t>(sv.size())));
        return PropertyKey::forString(s);
    };

    MatchPropsShapes built;
    built.nullRoot = root;
    Shape* s1 = root->addPropertyKey(arena, internAscii("index"), built.indexSlot,
                                     /*is_enumerable=*/true, /*is_accessor=*/false,
                                     /*is_writable=*/true, /*is_configurable=*/true,
                                     SlotRepr::Boxed);
    Shape* s2 = s1->addPropertyKey(arena, internAscii("input"), built.inputSlot,
                                   true, false, true, true, SlotRepr::Boxed);
    built.match3 = s2->addPropertyKey(arena, internAscii("groups"), built.groupsSlot,
                                      true, false, true, true, SlotRepr::Boxed);
    built.matchWithIdx = built.match3->addPropertyKey(
        arena, internAscii("indices"), built.indicesSlot, true, false, true, true, SlotRepr::Boxed);
    built.indices1 = root->addPropertyKey(arena, internAscii("groups"), built.idxGroupsSlot,
                                          true, false, true, true, SlotRepr::Boxed);
    cached = built;
    return cached;
}

// 22.2.7.8 MakeMatchIndicesIndexPairArray: the same captures as POSITIONS.
// Entry i is the two-element array `[start, end]` — half-open, like every other
// range in the language — or `undefined` for a group that did not participate,
// which is the same distinction `m[i] === undefined` draws one array over.
//
// It needs no input string and no slicing: the extents are what the matcher
// already recorded to cut the captures out of, so this reads the very numbers
// `buildMatchArray` throws away.
//
// `groups` here is OrdinaryObjectCreate(NULL) (step 4), which is why it is
// built from a null root shape rather than with `bronze_create_object`.
Value buildMatchIndices(const regex::Pattern& pattern, const regex::MatchResult& match) {
    Heap& heap = rtHeap();
    NonMovingArena& arena = rtArena();
    const uint32_t groups = regex::captureCount(pattern);
    ArrayHeader* rawArr = ArrayHeader::create(heap, groups + 1);
    rawArr->length = groups + 1;
    Rooted<Value> array{Value::fromObject(rawArr)};

    Rooted<Value> groupsObject;
    if (regex::hasNamedGroups(pattern)) {
        ObjectHeader* fresh = ObjectHeader::create(
            heap, arena, rtRootShapeForPrototype(Value::fromNull()));
        fresh->header.flags = BRONZE_ABI_OBJ_FLAGS_PLAIN;
        groupsObject.set(Value::fromObject(fresh));
    }

    // Step 5 puts `groups` on the array BEFORE the pairs, so it is the first
    // named property and the indices are the elements — the same shape the
    // match array itself has, and the order both enumerate in.
    const MatchPropsShapes& shapes = matchPropsShapes();
    ObjectHeader* rawProps = ObjectHeader::create(heap, arena, shapes.indices1);
    rawProps->header.flags = HeapKind::Plain;
    Rooted<Value> propsRoot{Value::fromObject(rawProps)};
    ObjectHeader* props = shapes.idxGroupsSlot >= ObjectHeader::kOverflowSlot
                              ? ObjectHeader::ensureSlots(heap, propsRoot, shapes.idxGroupsSlot + 1)
                              : rawProps;
    props->rawSetSlot(shapes.idxGroupsSlot, groupsObject.get());
    {
        ArrayHeader* arr = array.get().asObject<ArrayHeader>();
        arr->forgetNumbers();
        arr->properties = propsRoot.get();
    }

    const bool hasNamed = groupsObject.get().isObject();
    Rooted<Value> pair;
    for (uint32_t i = 0; i <= groups; ++i) {
        const int64_t from = match.captures[static_cast<size_t>(i) * 2];
        const int64_t to = match.captures[static_cast<size_t>(i) * 2 + 1];
        if (from != regex::MatchResult::kUnset) {
            ArrayHeader* pairArr = ArrayHeader::create(heap, 2);
            pairArr->length = 2;
            HeapValue* pairSlots = pairArr->elementsKeepingNumbers();
            pairSlots[0] = Value::fromDouble(static_cast<double>(from));
            pairSlots[1] = Value::fromDouble(static_cast<double>(to));
            pairArr->reserved |= ArrayHeader::kElementsAreNumbers;
            pair.set(Value::fromObject(pairArr));
        } else {
            pair.set(Value::fromUndefined());
        }
        array.get().asObject<ArrayHeader>()->elementsData()[i] = pair.get();
        if (!hasNamed || i == 0) continue;
        const std::string& name = regex::groupName(pattern, i);
        if (name.empty()) continue;
        Rooted<Value> key{rtMakeString(name)};
        groupsObject.get().asObject<ObjectHeader>()->setProp(heap, arena, key, pair);
    }
    return array.get();
}

// 22.2.7.2 steps 16-28: the captures as an array that also carries `index`,
// `input` and `groups`. The three named properties are created in the order
// the specification creates them, because that is the order they enumerate and
// print in.
Value buildMatchArray(const regex::Pattern& pattern, Rooted<Value>& inputStr,
                      const regex::MatchResult& match) {
    Heap& heap = rtHeap();
    NonMovingArena& arena = rtArena();
    const uint32_t groups = regex::captureCount(pattern);
    const bool hasIndices = regex::patternFlags(pattern).hasIndices;

    ArrayHeader* rawArr = ArrayHeader::create(heap, groups + 1);
    rawArr->length = groups + 1;
    Rooted<Value> array{Value::fromObject(rawArr)};

    // `groups` is an object only when the pattern has a named group, and
    // `undefined` otherwise (22.2.7.2 step 8) — which is why an ordinary match
    // array prints `groups: undefined` rather than `groups: {}`.
    Rooted<Value> groupsObject;
    if (regex::hasNamedGroups(pattern)) {
        groupsObject.set(Value(bronze_create_object()));
    }

    const MatchPropsShapes& shapes = matchPropsShapes();
    Shape* propsShape = hasIndices ? shapes.matchWithIdx : shapes.match3;
    const uint32_t maxSlot = hasIndices ? shapes.indicesSlot : shapes.groupsSlot;
    ObjectHeader* rawProps = ObjectHeader::create(heap, arena, propsShape);
    rawProps->header.flags = HeapKind::Plain;
    Rooted<Value> propsRoot{Value::fromObject(rawProps)};
    ObjectHeader* props = maxSlot >= ObjectHeader::kOverflowSlot
                              ? ObjectHeader::ensureSlots(heap, propsRoot, maxSlot + 1)
                              : rawProps;
    props->rawSetSlot(shapes.indexSlot, Value::fromDouble(static_cast<double>(match.start())));
    props->rawSetSlot(shapes.inputSlot, inputStr.get());
    props->rawSetSlot(shapes.groupsSlot, groupsObject.get());
    {
        ArrayHeader* arr = array.get().asObject<ArrayHeader>();
        arr->forgetNumbers();
        arr->properties = propsRoot.get();
    }

    if (!groupsObject.get().isObject()) {
        for (uint32_t i = 0; i <= groups; ++i) {
            const int64_t from = match.captures[static_cast<size_t>(i) * 2];
            const int64_t to = match.captures[static_cast<size_t>(i) * 2 + 1];
            const Value elem =
                from != regex::MatchResult::kUnset
                    ? Value::fromString(StringHeader::slice(heap, arena, inputStr,
                                                            static_cast<uint32_t>(from),
                                                            static_cast<uint32_t>(to)))
                    : Value::fromUndefined();
            array.get().asObject<ArrayHeader>()->elementsData()[i] = elem;
        }
    } else {
        Rooted<Value> element;
        for (uint32_t i = 0; i <= groups; ++i) {
            const int64_t from = match.captures[static_cast<size_t>(i) * 2];
            const int64_t to = match.captures[static_cast<size_t>(i) * 2 + 1];
            if (from != regex::MatchResult::kUnset) {
                element.set(Value::fromString(StringHeader::slice(
                    heap, arena, inputStr, static_cast<uint32_t>(from), static_cast<uint32_t>(to))));
            } else {
                element.set(Value::fromUndefined());
            }
            array.get().asObject<ArrayHeader>()->elementsData()[i] = element.get();
            if (i == 0) continue;
            const std::string& name = regex::groupName(pattern, i);
            if (name.empty()) continue;
            Rooted<Value> key{rtMakeString(name)};
            groupsObject.get().asObject<ObjectHeader>()->setProp(heap, arena, key, element);
        }
    }

    // Step 35, and last for the reason the other three are in their order: the
    // pair array is built from the captures the loop above has just walked, so
    // `indices` is the final named property and prints after `groups`.
    if (hasIndices) {
        Rooted<Value> indices{buildMatchIndices(pattern, match)};
        propsRoot.get().asObject<ObjectHeader>()->rawSetSlot(shapes.indicesSlot, indices.get());
    }
    return array.get();
}

// 22.2.7.2 steps 1-2: ToLength (7.1.20) of whatever the program assigned to
// `lastIndex`. The slot holds the assigned VALUE — a string stays a string —
// so this may run user code (an object's `valueOf`), and the RegExp is read
// through its root afterwards, and its throw propagates. `ok` is always true.
size_t lastIndexOf(Rooted<Value>& re, bool& ok) {
    ok = true;
    const Value stored = re.get().asObject<RegExpHeader>()->lastIndex;
    double raw = 0.0;
    if (stored.isNumber()) {
        raw = stored.asNumber();
    } else {
        raw = rtToNumber(stored);
    }
    if (std::isnan(raw) || raw <= 0.0) return 0;
    if (raw >= 9007199254740991.0) return static_cast<size_t>(-1);
    return static_cast<size_t>(raw);
}

// 22.2.7.2 step 15 / 22.2.7.1: the `lastIndex` write is Set(R, "lastIndex",
// v, true), a TypeError when the property is not writable — which a frozen
// RegExp's is not.
bool storeLastIndex(Rooted<Value>& re, double value) {
    if (rtIntegrityLevel(re.get()) == IntegrityLevel::Frozen) {
        rtThrowTypeError("Cannot assign to read only property 'lastIndex' of a frozen RegExp");
        return false;
    }
    re.get().asObject<RegExpHeader>()->lastIndex = Value::fromDouble(value);
    return true;
}

// What one initialisation from an ARENA source string decided, so that the
// next one from the same string and flag spelling — a regular expression
// literal evaluated again, which is every evaluation after its first —
// copies three slots instead of re-deriving them. The source is keyed by
// address, which an arena string keeps for as long as the thread lives; both
// answers are arena strings too, so the memo holds nothing the collector
// moves or frees. The program is still the cache's: its slot and id are
// checked as programSharedOf checks them, and an evicted program misses here
// and is compiled again by the ordinary path.
struct LiteralMemo {
    const StringHeader* written{nullptr};
    // The flags as the arena string a literal's evaluation passes, when one
    // has (rtRegExpLiteral); `flagsText` is its text.
    const StringHeader* writtenFlags{nullptr};
    // At most the eight flag letters; a longer spelling is never remembered.
    // Characters and not a std::string so that the table is trivially
    // destructible: the arena's teardown clears it (rtForgetRegExpLiteralMemo).
    char flagsText[8]{};
    uint8_t flagsLength{0};

    bool flagsAre(const std::string& text) const noexcept {
        return text.size() == flagsLength && std::memcmp(flagsText, text.data(), flagsLength) == 0;
    }
    Value source;
    Value canonicalFlags;
    uint64_t programId{0};
};

constexpr size_t kLiteralMemoSize = 64;

static_assert(std::is_trivially_destructible_v<LiteralMemo>);
thread_local LiteralMemo t_literalMemo[kLiteralMemoSize];

LiteralMemo& literalMemoFor(const StringHeader* written) {
    const auto bits = reinterpret_cast<uintptr_t>(written);
    return t_literalMemo[(bits >> 4) % kLiteralMemoSize];
}

}  // namespace

bool rtInitializeRegExp(Rooted<Value>& re, Rooted<Value>& sourceStr, const std::string& flagsText) {
    const StringHeader* writtenStr = sourceStr.get().asString<StringHeader>();
    LiteralMemo* memo = writtenStr->isArena() ? &literalMemoFor(writtenStr) : nullptr;
    if (memo != nullptr && memo->written == writtenStr && memo->flagsAre(flagsText)) {
        const auto slot = static_cast<uint32_t>(memo->programId & 0xFFFF);
        auto& cache = regExpCache();
        if (cache.isValid(slot, memo->programId)) {
            cache.touch(slot);
            auto* header = re.get().asObject<RegExpHeader>();
            header->source = memo->source;
            header->flagsText = memo->canonicalFlags;
            header->lastIndex = Value::fromDouble(0.0);
            header->programIndex = Value::fromDouble(static_cast<double>(memo->programId));
            return true;
        }
    }

    const std::string written = rtUtf8Chars(writtenStr);
    const std::string sourceUtf8 = escapeRegExpPattern(written);
    if (sourceUtf8 != written) sourceStr.set(rtMakeString(sourceUtf8.c_str()));
    const regex::Units source = unitsOfString(sourceStr.get().asString<StringHeader>());
    uint64_t index = 0;
    regex::Flags flags;
    if (!programFor(sourceUtf8, source, flagsText, flags, index)) return false;

    // The flags are re-spelled in 22.2.6.4's order, so `/a/yg`.flags is "gy".
    Rooted<Value> canonicalFlags{rtMakeString(flags.text())};

    auto* header = re.get().asObject<RegExpHeader>();
    header->source = sourceStr.get();
    header->flagsText = canonicalFlags.get();
    header->lastIndex = Value::fromDouble(0.0);
    header->programIndex = Value::fromDouble(static_cast<double>(index));

    if (memo != nullptr && flagsText.size() <= sizeof(memo->flagsText)) {
        // Interned, so both answers are immortal: the escaped source is the
        // written string itself when escaping changed nothing, and a flag
        // spelling is one of a handful.
        NonMovingArena& arena = rtArena();
        StringHeader* src = StringHeader::internToArena(arena, sourceStr.get().asString<StringHeader>());
        StringHeader* fl = StringHeader::internToArena(arena, canonicalFlags.get().asString<StringHeader>());
        memo->written = writtenStr;
        memo->writtenFlags = nullptr;
        memo->flagsLength = static_cast<uint8_t>(flagsText.size());
        std::memcpy(memo->flagsText, flagsText.data(), flagsText.size());
        memo->source = Value::fromString(src);
        memo->canonicalFlags = Value::fromString(fl);
        memo->programId = index;
    }
    return true;
}

Value rtMakeRegExp(Rooted<Value>& sourceStr, const std::string& flagsText) {
    Rooted<Value> re{rtAllocateRegExp(rtRegExpInstanceShape())};
    if (!rtInitializeRegExp(re, sourceStr, flagsText)) return Value::fromUndefined();
    return re.get();
}

void rtForgetRegExpLiteralMemo() noexcept {
    for (LiteralMemo& memo : t_literalMemo) {
        memo.written = nullptr;
        memo.writtenFlags = nullptr;
    }
}

Value rtRegExpLiteral(Value sourceStr, Value flagsStr) {
    const StringHeader* written = sourceStr.asString<StringHeader>();
    const StringHeader* writtenFlags = flagsStr.asString<StringHeader>();
    LiteralMemo& memo = literalMemoFor(written);
    if (memo.written == written && memo.writtenFlags == writtenFlags) {
        const auto slot = static_cast<uint32_t>(memo.programId & 0xFFFF);
        auto& cache = regExpCache();
        if (cache.isValid(slot, memo.programId)) {
            cache.touch(slot);
            // Nothing below is a pointer the allocation can move: the memo's
            // strings are arena strings.
            const Value re = rtAllocateRegExp(rtRegExpInstanceShape());
            auto* header = re.asObject<RegExpHeader>();
            header->source = memo.source;
            header->flagsText = memo.canonicalFlags;
            header->lastIndex = Value::fromDouble(0.0);
            header->programIndex = Value::fromDouble(static_cast<double>(memo.programId));
            return re;
        }
    }
    Rooted<Value> source{sourceStr};
    const std::string flagsText = rtUtf8Chars(writtenFlags);
    const Value re = rtMakeRegExp(source, flagsText);
    // The initialisation above has just filled the memo for this source; the
    // flag string's address is what the next evaluation is recognised by.
    if (re.isObject() && memo.written == written && memo.flagsAre(flagsText)) {
        memo.writtenFlags = writtenFlags;
    }
    return re;
}

Value rtAllocateRegExp(Shape* shape) {
    ObjectHeader* obj = ObjectHeader::createWithInternalSlots(rtHeap(), rtArena(), shape,
                                                              RegExpHeader::kInternalSlots);
    obj->header.flags = RegExpHeader::kFlags;
    // The four internal slots came back `undefined`, which is exactly the
    // "allocated, not yet initialised" state 22.2.3.2 RegExpAlloc leaves a
    // derived constructor's `this` in until `super()` runs.
    return Value::fromObject(obj);
}

bool rtIsRegExp(Value v) {
    return isRegExp(v) && v.asObject<RegExpHeader>()->programIndex.isNumber();
}

Value rtRegExpExec(Rooted<Value>& re, Rooted<Value>& inputStr) {
    if (!isRegExp(re.get()) || !re.get().asObject<RegExpHeader>()->programIndex.isNumber()) {
        return rtThrowTypeError("RegExp.prototype.exec called on an incompatible receiver");
    }
    // 22.2.7.2 step 6: a pattern with neither `g` nor `y` ignores `lastIndex`
    // entirely, which is what makes `/a/.exec(s)` idempotent and `/a/g.exec(s)`
    // a cursor. Read FIRST: it can run user code, and every raw pointer below
    // is taken after it.
    const bool tracksLastIndex = [&] {
        const regex::Flags& f = regex::patternFlags(programOf(re.get()));
        return f.global || f.sticky;
    }();
    size_t from = 0;
    if (tracksLastIndex) {
        bool ok = true;
        from = lastIndexOf(re, ok);
        if (!ok) return Value::fromUndefined();
    }
    std::shared_ptr<const regex::Pattern> patternPtr = programSharedOf(re.get());
    const regex::Pattern& pattern = *patternPtr;
    const regex::Flags& flags = regex::patternFlags(pattern);

    ScratchMatchResult scratch;
    std::string error;
    regex::ExecStatus status = regex::ExecStatus::NoMatch;
    {
        ScopedRegexUnits input(inputStr.get().asString<StringHeader>());
        if (from <= input.size()) {
            status = flags.sticky ? regex::matchAt(pattern, input.view(), from, scratch.result, error)
                                  : regex::search(pattern, input.view(), from, scratch.result, error);
        }
    }
    if (status == regex::ExecStatus::Error) {
        return rtThrowRangeError(error);
    }
    if (status != regex::ExecStatus::Match) {
        if (tracksLastIndex && !storeLastIndex(re, 0.0)) return Value::fromUndefined();
        return Value::fromNull();
    }
    if (tracksLastIndex && !storeLastIndex(re, static_cast<double>(scratch.result.end()))) {
        return Value::fromUndefined();
    }
    return buildMatchArray(pattern, inputStr, scratch.result);
}

bool rtRegExpExecTest(Rooted<Value>& re, Rooted<Value>& inputStr) {
    if (!isRegExp(re.get()) || !re.get().asObject<RegExpHeader>()->programIndex.isNumber()) {
        rtThrowTypeError("RegExp.prototype.test called on an incompatible receiver");
        return false;
    }
    const bool tracksLastIndex = [&] {
        const regex::Flags& f = regex::patternFlags(programOf(re.get()));
        return f.global || f.sticky;
    }();
    size_t from = 0;
    if (tracksLastIndex) {
        bool ok = true;
        from = lastIndexOf(re, ok);
        if (!ok) return false;
    }
    std::shared_ptr<const regex::Pattern> patternPtr = programSharedOf(re.get());
    const regex::Pattern& pattern = *patternPtr;
    const regex::Flags& flags = regex::patternFlags(pattern);

    ScratchMatchResult scratch;
    std::string error;
    regex::ExecStatus status = regex::ExecStatus::NoMatch;
    {
        ScopedRegexUnits input(inputStr.get().asString<StringHeader>());
        if (from <= input.size()) {
            status = flags.sticky
                         ? regex::matchAt(pattern, input.view(), from, scratch.result, error,
                                          /*extractCaptures=*/false)
                         : regex::search(pattern, input.view(), from, scratch.result, error,
                                         /*extractCaptures=*/false);
        }
    }
    if (status == regex::ExecStatus::Error) {
        rtThrowRangeError(error);
        return false;
    }
    if (status != regex::ExecStatus::Match) {
        if (tracksLastIndex && !storeLastIndex(re, 0.0)) return false;
        return false;
    }
    if (tracksLastIndex && !storeLastIndex(re, static_cast<double>(scratch.result.end()))) {
        return false;
    }
    return true;
}

const regex::Pattern& rtRegExpPattern(Value re) { return programOf(re); }

std::shared_ptr<const regex::Pattern> rtRegExpPatternShared(Value re) {
    return programSharedOf(re);
}

Value rtRegExpBuildMatchArray(const regex::Pattern& pattern, Rooted<Value>& inputStr,
                              const regex::MatchResult& match) {
    return buildMatchArray(pattern, inputStr, match);
}

bool rtRegExpSetLastIndex(Rooted<Value>& re, double value) { return storeLastIndex(re, value); }

Value rtRegExpLastIndexValue(Value re) { return re.asObject<RegExpHeader>()->lastIndex; }

void rtRegExpRestoreLastIndex(Value re, Value saved) {
    re.asObject<RegExpHeader>()->lastIndex = saved;
}

double rtRegExpLastIndexLength(Rooted<Value>& re, bool& ok) {
    const size_t n = lastIndexOf(re, ok);
    return n == static_cast<size_t>(-1) ? 9007199254740991.0 : static_cast<double>(n);
}

Value rtRegExpFromParts(Rooted<Value>& sourceStr, const std::string& flagsText) {
    return rtMakeRegExp(sourceStr, flagsText);
}

uint64_t rtRegExpExecBody(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    // 22.2.6.2 step 3 before step 4: the receiver is checked before the
    // argument is converted, and ToString can run user code.
    if (!isRegExp(self.get()) || !self.get().asObject<RegExpHeader>()->programIndex.isNumber()) {
        return rtThrowTypeError("RegExp.prototype.exec called on an incompatible receiver")
            .rawBits();
    }
    Rooted<Value> input{rtValueToString(args[0])};
    return rtRegExpExec(self, input).rawBits();
}

size_t rtRegExpCacheSize() {
    return regExpCache().size();
}

}  // namespace bronze::runtime
