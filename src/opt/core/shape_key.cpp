#include "opt/core/shape_key.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "abi/bronze_abi_layout.h"

namespace bronze::opt {

namespace {

bool allDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

}  // namespace

std::optional<ShapeName> parseShapeName(std::string_view name) {
    const size_t open = name.find('{');
    if (open == std::string_view::npos || name.size() < open + 2 || name.back() != '}') return std::nullopt;
    const std::string_view head = name.substr(0, open);
    const std::string_view body = name.substr(open + 1, name.size() - open - 2);
    // One brace pair: anything else is a key or a prototype name with a
    // brace in it, which can print like a different key list.
    if (body.find('{') != std::string_view::npos || body.find('}') != std::string_view::npos) {
        return std::nullopt;
    }
    if (head.find('}') != std::string_view::npos) return std::nullopt;

    ShapeName out;
    out.inlineSlots = BRONZE_ABI_OBJ_INLINE_SLOTS;
    std::string_view proto = head;
    // `#N` is the capacity only when only digits follow the last `#`: a
    // constructor named `a#b` keeps its `#`.
    if (const size_t hash = head.rfind('#'); hash != std::string_view::npos && allDigits(head.substr(hash + 1))) {
        const std::string_view digits = head.substr(hash + 1);
        if (digits.size() > 3) return std::nullopt;
        uint32_t n = 0;
        for (char c : digits) n = n * 10 + static_cast<uint32_t>(c - '0');
        if (n == 0 || n > BRONZE_ABI_OBJ_OVERFLOW_SLOT) return std::nullopt;
        out.inlineSlots = n;
        proto = head.substr(0, hash);
    }
    out.prototype = std::string(proto);

    if (body == "dict") return std::nullopt;
    if (body.empty()) return out;  // `{}`: a shape with no own keys
    for (size_t at = 0; at <= body.size();) {
        const size_t comma = std::min(body.find(',', at), body.size());
        std::string_view key = body.substr(at, comma - at);
        if (!key.empty() && key.front() == '&') return std::nullopt;
        const bool readOnly = !key.empty() && key.front() == '!';
        if (readOnly) key.remove_prefix(1);
        if (key.empty() || key == "@@symbol") return std::nullopt;
        out.keys.emplace_back(key);
        out.readOnly.push_back(readOnly);
        at = comma + 1;
    }
    return out;
}

std::string shapeNameWithKeys(const ShapeName& shape, size_t keyCount) {
    std::string name = shape.prototype;
    if (shape.inlineSlots != BRONZE_ABI_OBJ_INLINE_SLOTS) name += "#" + std::to_string(shape.inlineSlots);
    name += "{";
    for (size_t k = 0; k < keyCount && k < shape.keys.size(); ++k) {
        if (k != 0) name += ",";
        if (shape.readOnly[k]) name += "!";
        name += shape.keys[k];
    }
    name += "}";
    return name;
}

std::optional<int32_t> ownSlot(const ShapeName& shape, std::string_view key) {
    std::optional<size_t> position;
    for (size_t k = 0; k < shape.keys.size(); ++k) {
        if (shape.keys[k] != key) continue;
        if (position) return std::nullopt;  // twice: the name is ambiguous
        position = k;
    }
    if (!position) return std::nullopt;
    const size_t k = *position;
    const size_t n = shape.inlineSlots;
    const size_t slot = k < n ? k : BRONZE_ABI_OBJ_OVERFLOW_SLOT + (k - n);
    if (slot > static_cast<size_t>(INT32_MAX)) return std::nullopt;
    return static_cast<int32_t>(slot);
}

std::optional<int32_t> writableSlot(const ShapeName& shape, std::string_view key) {
    const std::optional<int32_t> slot = ownSlot(shape, key);
    if (!slot) return std::nullopt;
    for (size_t k = 0; k < shape.keys.size(); ++k) {
        if (shape.keys[k] == key && shape.readOnly[k]) return std::nullopt;
    }
    return slot;
}

bool isNamedKey(std::string_view key) {
    if (key.empty()) return false;
    if (!allDigits(key)) return true;
    // A canonical array index has no leading zero (except "0" itself) and is
    // below 2^32 - 1.
    if (key.size() > 1 && key.front() == '0') return true;
    if (key.size() > 10) return true;
    uint64_t v = 0;
    for (char c : key) v = v * 10 + static_cast<uint64_t>(c - '0');
    return v >= 4294967295ull;
}

}  // namespace bronze::opt
