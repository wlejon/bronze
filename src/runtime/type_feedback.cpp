#include "runtime/type_feedback.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "abi/bronze_abi.h"
#include "abi/bronze_feedback_site.h"
#include "runtime/fn.h"
#include "runtime/fn_source.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"

namespace bronze::runtime {

namespace {

// How many distinct names one site tracks before the rest are only counted.
// A megamorphic site's tail is not a specialization candidate, and a bounded
// list keeps every record a short scan.
constexpr size_t kTrackedNames = 32;

struct Named {
    uint32_t id = 0;  // into State::names
    uint64_t count = 0;
};

struct Fn;

struct Site {
    Fn* fn = nullptr;
    uint32_t kind = 0;
    uint64_t total = 0;
    uint64_t tags[BRONZE_FEEDBACK_TAG_COUNT] = {};
    std::vector<Named> shapes;
    uint64_t shapeOverflow = 0;
    std::vector<Named> targets;
    uint64_t targetOverflow = 0;
};

struct Fn {
    uint64_t entries = 0;
    std::map<uint32_t, Site> sites;  // by ordinal; node-stable
};

struct MethodKey {
    const Shape* shape;
    uint32_t keyId;
    bool operator==(const MethodKey& o) const { return shape == o.shape && keyId == o.keyId; }
};
struct MethodKeyHash {
    size_t operator()(const MethodKey& k) const {
        return std::hash<const void*>()(k.shape) ^ (static_cast<size_t>(k.keyId) * 0x9E3779B97F4A7C15ull);
    }
};
// Where a method key resolved on a receiver shape: an own data slot (read on
// every record, since two objects at one shape hold different values there),
// or a callee found on the prototype chain, valid while the prototype epoch
// holds.
struct MethodEntry {
    uint64_t epoch = 0;
    bool own = false;
    uint32_t slot = 0;
    uint32_t nameId = 0;
};

struct State {
    std::string outPath;
    bool armed = false;
    std::map<std::string, Fn> fns;  // by function key; sorted for the writer
    std::unordered_map<uint32_t, Fn*> fnByKeyId;
    std::unordered_map<uint64_t, Site*> siteByWord;          // keyId << 32 | info
    std::unordered_map<uint64_t, uint32_t> methodKeyOfSite;  // fnKeyId << 32 | ordinal
    std::vector<std::string> names;
    std::unordered_map<std::string, uint32_t> nameIds;
    std::unordered_map<const Shape*, uint32_t> shapeNames;
    std::unordered_map<const Shape*, std::string> protoNames;  // by root shape
    std::unordered_map<const void*, std::pair<const StringHeader*, uint32_t>> calleeNames;
    std::unordered_map<MethodKey, MethodEntry, MethodKeyHash> methods;
};

std::mutex g_mu;
State* g_state = nullptr;  // leaked on purpose: the atexit writer may run after statics die

uint32_t internName(State& s, const std::string& name) {
    auto [it, inserted] = s.nameIds.emplace(name, static_cast<uint32_t>(s.names.size()));
    if (inserted) s.names.push_back(name);
    return it->second;
}

std::string hex8(uint32_t v) {
    char buf[9];
    std::snprintf(buf, sizeof buf, "%08x", v);
    return buf;
}

uint32_t fnv32(std::string_view text) {
    uint32_t h = 2166136261u;
    for (unsigned char c : text) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

uint32_t tagOf(Value v) {
    if (v.isInt32()) return BRONZE_FEEDBACK_TAG_I32;
    if (v.isNumber()) {
        const double d = v.asNumber();
        if (d >= -2147483648.0 && d <= 2147483647.0 && d == std::trunc(d) &&
            !(d == 0 && std::signbit(d))) {
            return BRONZE_FEEDBACK_TAG_I32;
        }
        return BRONZE_FEEDBACK_TAG_F64;
    }
    if (v.isUndefined()) return BRONZE_FEEDBACK_TAG_UNDEFINED;
    if (v.isNull()) return BRONZE_FEEDBACK_TAG_NULL;
    if (v.isBool()) return BRONZE_FEEDBACK_TAG_BOOL;
    if (v.isString()) return BRONZE_FEEDBACK_TAG_STRING;
    if (v.isObject()) {
        switch (v.asObject<HeapObjectHeader>()->flags) {
            case HeapKind::Plain: return BRONZE_FEEDBACK_TAG_OBJECT;
            case HeapKind::Array: return BRONZE_FEEDBACK_TAG_ARRAY;
            case HeapKind::TypedArray: return BRONZE_FEEDBACK_TAG_TYPEDARRAY;
            case HeapKind::Function: return BRONZE_FEEDBACK_TAG_FUNCTION;
            default: return BRONZE_FEEDBACK_TAG_OTHER;
        }
    }
    return BRONZE_FEEDBACK_TAG_OTHER;
}

const char* kindWord(uint16_t flags) {
    switch (flags) {
        case HeapKind::Array: return "array";
        case HeapKind::Function: return "function";
        case HeapKind::TypedArray: return "typedarray";
        case HeapKind::ArrayBuffer: return "arraybuffer";
        case HeapKind::DataView: return "dataview";
        case HeapKind::Iterator: return "iterator";
        case HeapKind::RegExp: return "regexp";
        case HeapKind::ModuleNamespace: return "namespace";
        case HeapKind::Proxy: return "proxy";
        default: return "exotic";
    }
}

// What a value that is not a plain object is called where a shape or a callee
// would be named: its kind.
const char* primitiveWord(Value v) {
    if (v.isNumber() || v.isInt32()) return "number";
    if (v.isUndefined()) return "undefined";
    if (v.isNull()) return "null";
    if (v.isBool()) return "boolean";
    if (v.isString()) return "string";
    if (v.isSymbol()) return "symbol";
    if (v.isBigInt()) return "bigint";
    return "other";
}

bool keyIs(const Shape* node, const char* ascii) {
    const StringHeader* ks = node->key.string();
    if (ks == nullptr || !ks->isLatin1()) return false;
    const size_t n = std::strlen(ascii);
    return ks->getLength() == n && std::memcmp(ks->latin1Data(), ascii, n) == 0;
}

std::string calleeText(const FunctionHeader* fn) {
    std::string name = fn->name != nullptr ? rtUtf8Chars(fn->name) : std::string();
    if (name.empty()) name = "<anonymous>";
    const std::string_view src = rtFunctionSourceText(reinterpret_cast<const void*>(fn->code));
    if (src.empty()) return "native:" + name;
    return name + "@" + hex8(fnv32(src));
}

// A callee's stable name: its `name` and a hash of its source text, which is
// what a reading build can compute for an IL function too. Cached by code
// pointer, the identity every closure over one body shares.
uint32_t calleeName(State& s, Value v) {
    if (!v.isObject()) return internName(s, primitiveWord(v));
    auto* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::Function) return internName(s, std::string("not-callable:") + kindWord(hdr->flags));
    auto* fn = v.asObject<FunctionHeader>();
    const void* code = reinterpret_cast<const void*>(fn->code);
    auto it = s.calleeNames.find(code);
    if (it != s.calleeNames.end() && it->second.first == fn->name) return it->second.second;
    const uint32_t id = internName(s, calleeText(fn));
    s.calleeNames[code] = {fn->name, id};
    return id;
}

// The prototype's name: the `name` of the first constructor found as an own
// data property `constructor` up the chain, with a `^` per level climbed to
// reach it. `Vector3`, `Object`, `^Material` (an `Object.create(proto)`
// object), `null`.
std::string protoName(Value proto) {
    std::string climbed;
    for (int depth = 0; depth < 8; ++depth) {
        if (proto.isNull()) return climbed + "null";
        if (!proto.isObject()) return climbed + "?";
        auto* hdr = proto.asObject<HeapObjectHeader>();
        if (hdr->flags == HeapKind::Function) return climbed + "Function";
        if (hdr->flags != HeapKind::Plain) return climbed + kindWord(hdr->flags);
        auto* obj = proto.asObject<ObjectHeader>();
        const Shape* sh = obj->shape;
        if (sh == nullptr) return climbed + "?";
        if (!sh->isDictionary()) {
            for (const Shape* n = sh; n != nullptr && n->parent != nullptr; n = n->parent) {
                if (n->accessor || !keyIs(n, "constructor")) continue;
                const Value ctor = obj->getSlot(n->slot_index);
                if (ctor.isObject() && ctor.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
                    const auto* fn = ctor.asObject<FunctionHeader>();
                    std::string name = fn->name != nullptr ? rtUtf8Chars(fn->name) : std::string();
                    return climbed + (name.empty() ? "<anonymous>" : name);
                }
                break;
            }
        }
        climbed += "^";
        proto = sh->prototypeValue();
    }
    return climbed + "?";
}

// A shape's stable name: `<prototype>{k1,k2,...}`, its own keys in the order
// they were added (a dictionary is `<prototype>{dict}`), with `#N` before the
// brace for an inline capacity N other than the default 4. Every key is listed
// (format 2; format 1 cut the list at 32 with `,+N`, which left a wide
// object's later slots unclaimable). Two shapes that differ only in
// attributes or slot representation share a name, which is the granularity a
// specialization guards on anyway. `protoNames` caches the prototype part by
// root shape.
std::string shapeText(const Shape* sh, std::unordered_map<const Shape*, std::string>& protoNames) {
    const Shape* root = sh->root;
    auto pit = protoNames.find(root);
    if (pit == protoNames.end()) pit = protoNames.emplace(root, protoName(sh->prototypeValue())).first;
    std::string name = pit->second;
    // A capacity other than the default is part of the name: the same keys
    // sit at other slots past it (shape.h `Shape::placeSlot`).
    if (sh->inline_slots != Shape::kDefaultInlineSlots) name += "#" + std::to_string(sh->inline_slots);
    name += "{";
    if (sh->isDictionary()) {
        name += "dict";
    } else {
        std::vector<std::string> keys;
        for (const Shape* n = sh; n != nullptr && n->parent != nullptr; n = n->parent) {
            if (StringHeader* ks = n->key.string()) keys.push_back(rtUtf8Chars(ks));
            else keys.push_back("@@symbol");
        }
        std::reverse(keys.begin(), keys.end());
        for (size_t i = 0; i < keys.size(); ++i) {
            if (i != 0) name += ",";
            name += keys[i];
        }
    }
    name += "}";
    return name;
}

// Cached by the shape, which is immortal and non-moving — except a dictionary
// shape that is not frozen, which is freed with its object (dictionary.cpp) and
// whose address another shape may take; its name is cheap (`{dict}`) and is
// not cached.
uint32_t shapeName(State& s, const ObjectHeader* obj) {
    const Shape* sh = obj->shape;
    if (sh == nullptr) return internName(s, "object{?}");
    if (sh->isDictionary() && !sh->readCacheable()) return internName(s, shapeText(sh, s.protoNames));
    auto it = s.shapeNames.find(sh);
    if (it != s.shapeNames.end()) return it->second;
    const uint32_t id = internName(s, shapeText(sh, s.protoNames));
    s.shapeNames.emplace(sh, id);
    return id;
}

uint32_t receiverName(State& s, Value v) {
    if (!v.isObject()) return internName(s, primitiveWord(v));
    auto* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags == HeapKind::Plain) return shapeName(s, v.asObject<ObjectHeader>());
    return internName(s, kindWord(hdr->flags));
}

// The callee `receiver[methodKey]` resolves to, found WITHOUT running anything:
// own and prototype data slots by shape, an accessor named as one. The
// receiver of a static call is the function's own-properties object.
uint32_t methodTarget(State& s, Value receiver, uint32_t methodKeyId) {
    const std::string& method = rtKeyString(methodKeyId);
    if (!receiver.isObject()) return internName(s, std::string(primitiveWord(receiver)) + "." + method);
    auto* hdr = receiver.asObject<HeapObjectHeader>();
    Value holder = receiver;
    if (hdr->flags == HeapKind::Function) {
        holder = receiver.asObject<FunctionHeader>()->properties.get();
        if (!holder.isObject()) return internName(s, "function." + method);
    } else if (hdr->flags != HeapKind::Plain) {
        return internName(s, std::string(kindWord(hdr->flags)) + "." + method);
    }
    StringHeader* keyHeader = rtKeyHeader(methodKeyId);
    if (keyHeader == nullptr) return internName(s, "?." + method);
    const PropertyKey key(keyHeader);
    auto* first = holder.asObject<ObjectHeader>();
    const Shape* firstShape = first->shape;
    const uint64_t epoch = protoMutationEpoch();
    if (firstShape != nullptr && !firstShape->isDictionary()) {
        auto it = s.methods.find(MethodKey{firstShape, methodKeyId});
        if (it != s.methods.end() && it->second.epoch == epoch) {
            if (!it->second.own) return it->second.nameId;
            return calleeName(s, first->getSlot(it->second.slot));
        }
    }
    Value cur = holder;
    for (int depth = 0; depth < 32; ++depth) {
        if (!cur.isObject()) return internName(s, "absent." + method);
        auto* curHdr = cur.asObject<HeapObjectHeader>();
        if (curHdr->flags != HeapKind::Plain) {
            return internName(s, std::string(kindWord(curHdr->flags)) + "-proto." + method);
        }
        auto* obj = cur.asObject<ObjectHeader>();
        const Shape* sh = obj->shape;
        if (sh == nullptr || sh->isDictionary()) return internName(s, "dict." + method);
        PropertyInfo info;
        if (sh->lookupProperty(key, info)) {
            if (info.accessor) return internName(s, "accessor." + method);
            const Value found = obj->getSlot(info.slot);
            const uint32_t id = calleeName(s, found);
            if (firstShape != nullptr && !firstShape->isDictionary()) {
                MethodEntry e;
                e.epoch = epoch;
                e.own = depth == 0;
                e.slot = info.slot;
                e.nameId = id;
                s.methods[MethodKey{firstShape, methodKeyId}] = e;
            }
            return id;
        }
        cur = sh->prototypeValue();
    }
    return internName(s, "deep." + method);
}

void bump(std::vector<Named>& list, uint64_t& overflow, uint32_t id) {
    for (Named& n : list) {
        if (n.id == id) {
            ++n.count;
            return;
        }
    }
    if (list.size() < kTrackedNames) {
        list.push_back(Named{id, 1});
    } else {
        ++overflow;
    }
}

Site* siteFor(State& s, uint32_t keyId, uint32_t info) {
    const uint64_t word = (static_cast<uint64_t>(keyId) << 32) | info;
    auto it = s.siteByWord.find(word);
    if (it != s.siteByWord.end()) return it->second;
    Fn*& fn = s.fnByKeyId[keyId];
    if (fn == nullptr) fn = &s.fns[rtKeyString(keyId)];
    Site& site = fn->sites[BRONZE_FEEDBACK_INFO_ORDINAL(info)];
    site.fn = fn;
    site.kind = BRONZE_FEEDBACK_INFO_KIND(info);
    s.siteByWord.emplace(word, &site);
    return &site;
}

std::string escapeName(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

void writeNames(std::string& out, const char* word, uint32_t ordinal, const State& s,
                const std::vector<Named>& list, uint64_t overflow) {
    std::vector<Named> sorted = list;
    std::sort(sorted.begin(), sorted.end(), [&](const Named& a, const Named& b) {
        if (a.count != b.count) return a.count > b.count;
        return s.names[a.id] < s.names[b.id];
    });
    uint64_t other = overflow;
    for (size_t i = 0; i < sorted.size(); ++i) {
        if (i >= BRONZE_FEEDBACK_MAX_NAMES) {
            other += sorted[i].count;
            continue;
        }
        out += std::string(word) + "\t" + std::to_string(ordinal) + "\t" +
               std::to_string(sorted[i].count) + "\t" + escapeName(s.names[sorted[i].id]) + "\n";
    }
    if (other != 0) {
        out += std::string(word) + "\t" + std::to_string(ordinal) + "\t" + std::to_string(other) +
               "\t*other*\n";
    }
}

std::string textLocked() {
    if (g_state == nullptr) return {};
    static const char* const kKinds[] = BRONZE_FEEDBACK_KIND_NAMES;
    static const char* const kTags[] = BRONZE_FEEDBACK_TAG_NAMES;
    std::string out;
    out += "bronze-type-feedback " + std::to_string(BRONZE_FEEDBACK_FORMAT_VERSION) + "\n";
    for (const auto& [key, fn] : g_state->fns) {
        out += "fn\t" + escapeName(key) + "\t" + std::to_string(fn.entries) + "\n";
        for (const auto& [ordinal, site] : fn.sites) {
            out += "site\t" + std::to_string(ordinal) + "\t" +
                   (site.kind < BRONZE_FEEDBACK_KIND_COUNT ? kKinds[site.kind] : "?") + "\t" +
                   std::to_string(site.total) + "\t";
            bool first = true;
            for (uint32_t t = 0; t < BRONZE_FEEDBACK_TAG_COUNT; ++t) {
                if (site.tags[t] == 0) continue;
                if (!first) out += " ";
                first = false;
                out += std::string(kTags[t]) + "=" + std::to_string(site.tags[t]);
            }
            out += "\n";
            writeNames(out, "shape", ordinal, *g_state, site.shapes, site.shapeOverflow);
            writeNames(out, "target", ordinal, *g_state, site.targets, site.targetOverflow);
        }
    }
    return out;
}

void writeAtExit() { typeFeedbackWrite(); }

uint64_t flushIntrinsic(uint64_t, uint64_t, uint32_t, const uint64_t*) {
    return Value::fromBool(typeFeedbackWrite()).rawBits();
}

}  // namespace

std::string typeFeedbackShapeName(const Shape* shape, std::unordered_map<const Shape*, std::string>& protoNames) {
    if (shape == nullptr) return "object{?}";
    return shapeText(shape, protoNames);
}

const char* typeFeedbackKindWord(uint16_t flags) { return kindWord(flags); }

std::string typeFeedbackCodeName(const void* code) {
    if (code == nullptr) return {};
    const std::string_view src = rtFunctionSourceText(code);
    if (src.empty()) return {};
    return "~" + std::to_string(rtFunctionSourceBegin(code)) + "@" + hex8(fnv32(src));
}

void typeFeedbackRegister(const char* outPath, const uint32_t* table, uint32_t count,
                          const uint32_t* keyMap) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr) g_state = new State();
    State& s = *g_state;
    if (outPath != nullptr && *outPath != '\0' && s.outPath.empty()) s.outPath = outPath;
    if (!s.armed) {
        s.armed = true;
        std::atexit(writeAtExit);
    }
    if (table == nullptr || keyMap == nullptr) return;
    // A METHOD row names (function, ordinal) and the METHOD_NAME row after it
    // the key that site calls.
    for (uint32_t i = 0; i + 1 < count; ++i) {
        const uint32_t info = table[i * 2 + 1];
        if ((info & BRONZE_FEEDBACK_FLAG) == 0 ||
            BRONZE_FEEDBACK_INFO_KIND(info) != BRONZE_FEEDBACK_KIND_METHOD) {
            continue;
        }
        const uint32_t next = table[(i + 1) * 2 + 1];
        if (BRONZE_FEEDBACK_INFO_KIND(next) != BRONZE_FEEDBACK_KIND_METHOD_NAME) continue;
        const uint32_t fnKeyId = keyMap[table[i * 2]];
        const uint32_t methodKeyId = keyMap[table[(i + 1) * 2]];
        s.methodKeyOfSite[(static_cast<uint64_t>(fnKeyId) << 32) | BRONZE_FEEDBACK_INFO_ORDINAL(info)] =
            methodKeyId;
    }
}

void typeFeedbackRecord(uint32_t keyId, uint32_t siteInfo, uint64_t valueBits) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr) return;
    State& s = *g_state;
    Site* site = siteFor(s, keyId, siteInfo);
    ++site->total;
    if (site->kind == BRONZE_FEEDBACK_KIND_ENTRY) {
        ++site->fn->entries;
        return;
    }
    const Value v = Value::fromRawBits(valueBits);
    ++site->tags[tagOf(v)];
    switch (site->kind) {
        case BRONZE_FEEDBACK_KIND_SHAPE:
            bump(site->shapes, site->shapeOverflow, receiverName(s, v));
            break;
        case BRONZE_FEEDBACK_KIND_TARGET:
            bump(site->targets, site->targetOverflow, calleeName(s, v));
            break;
        case BRONZE_FEEDBACK_KIND_METHOD: {
            bump(site->shapes, site->shapeOverflow, receiverName(s, v));
            auto it = s.methodKeyOfSite.find((static_cast<uint64_t>(keyId) << 32) |
                                             BRONZE_FEEDBACK_INFO_ORDINAL(siteInfo));
            if (it != s.methodKeyOfSite.end()) {
                bump(site->targets, site->targetOverflow, methodTarget(s, v, it->second));
            }
            break;
        }
        default:
            break;
    }
}

std::string typeFeedbackText() {
    std::lock_guard<std::mutex> lock(g_mu);
    return textLocked();
}

bool typeFeedbackWrite() {
    std::string text;
    std::string path;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_state == nullptr) return false;
        text = textLocked();
        path = g_state->outPath;
    }
    if (const char* env = std::getenv("BRONZE_FEEDBACK_OUT"); env != nullptr && *env != '\0') path = env;
    if (path.empty()) path = "bronze-feedback.tfb";
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "bronze: type feedback: cannot write '%s'\n", path.c_str());
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

void typeFeedbackResetForTesting() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr) return;
    const bool armed = g_state->armed;
    *g_state = State();
    g_state->armed = armed;
}

Value rtTypeFeedbackIntrinsic(const std::string& name) {
    if (name == "__bronze_feedback_flush") {
        return rtNativeFunction(flushIntrinsic, 0, "__bronze_feedback_flush", 0);
    }
    return Value::fromUndefined();
}

}  // namespace bronze::runtime
