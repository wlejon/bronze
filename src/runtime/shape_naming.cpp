#include "runtime/shape_naming.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include "runtime/dictionary.h"
#include "runtime/fn.h"
#include "runtime/fn_source.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/shape.h"
#include "runtime/string.h"

namespace bronze::runtime {

namespace {

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

bool keyIs(const Shape* node, const char* ascii) {
    const StringHeader* ks = node->key.string();
    if (ks == nullptr || !ks->isLatin1()) return false;
    const size_t n = std::strlen(ascii);
    return ks->getLength() == n && std::memcmp(ks->latin1Data(), ascii, n) == 0;
}

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
        Value ctor = Value::fromUndefined();
        if (sh->isDictionary()) {
            for (const DictEntry& e : sh->dict->entries) {
                const StringHeader* ks = e.live() ? e.key.string() : nullptr;
                if (ks == nullptr || e.accessor || !ks->isLatin1() || ks->getLength() != 11 ||
                    std::memcmp(ks->latin1Data(), "constructor", 11) != 0) {
                    continue;
                }
                ctor = obj->getSlot(e.slot);
                break;
            }
        } else {
            for (const Shape* n = sh; n != nullptr && n->parent != nullptr; n = n->parent) {
                if (n->accessor || !keyIs(n, "constructor")) continue;
                ctor = obj->getSlot(n->slot_index);
                break;
            }
        }
        if (ctor.isObject() && ctor.asObject<HeapObjectHeader>()->flags == HeapKind::Function) {
            const auto* fn = ctor.asObject<FunctionHeader>();
            std::string name = fn->name != nullptr ? rtUtf8Chars(fn->name) : std::string();
            return climbed + (name.empty() ? "<anonymous>" : name);
        }
        climbed += "^";
        proto = sh->prototypeValue();
    }
    return climbed + "?";
}

std::string shapeText(const Shape* sh, std::unordered_map<const Shape*, std::string>& protoNames) {
    const Shape* root = sh->root;
    auto pit = protoNames.find(root);
    if (pit == protoNames.end()) pit = protoNames.emplace(root, protoName(sh->prototypeValue())).first;
    std::string name = pit->second;
    if (sh->inline_slots != Shape::kDefaultInlineSlots) name += "#" + std::to_string(sh->inline_slots);
    name += "{";
    if (sh->isDictionary()) {
        name += "dict";
    } else {
        std::vector<std::string> keys;
        for (const Shape* n = sh; n != nullptr && n->parent != nullptr; n = n->parent) {
            const char* mark = n->accessor                    ? "&"
                               : !n->writable                 ? "!"
                               : n->repr == SlotRepr::Double ? "%"
                                                              : "";
            if (StringHeader* ks = n->key.string()) keys.push_back(mark + rtUtf8Chars(ks));
            else keys.push_back(std::string(mark) + "@@symbol");
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

}  // namespace bronze::runtime
