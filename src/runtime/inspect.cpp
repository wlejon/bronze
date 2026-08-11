// console.log of a container — the format docs/0013 pins.
//
// Nothing here allocates a JS value, which is why every raw pointer below
// stays valid for the whole walk: the collector cannot run while a string
// is being built (docs/0006).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/number_format.h"
#include "runtime/object.h"
#include "runtime/regexp.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::runtime {
namespace {

// node's default `depth: 2`: a container nested deeper than this prints as
// `[Array]` / `[Object]` rather than being expanded.
constexpr int kMaxDepth = 2;

// The one UTF-8 encoder lives in rt_convert.cpp, next to rtAsciiChars: two
// of them drifting is how a printed string and a printed error message start
// disagreeing about an astral character.
std::string utf8Of(StringHeader* str) { return rtUtf8Chars(str); }

// node's quote choice: single quotes, unless the string contains one and no
// double quote, and backticks when it contains both.
char quoteFor(const std::string& s) {
    const bool hasSingle = s.find('\'') != std::string::npos;
    if (!hasSingle) return '\'';
    if (s.find('"') == std::string::npos) return '"';
    if (s.find('`') == std::string::npos) return '`';
    return '\'';
}

std::string quoted(const std::string& s) {
    const char q = quoteFor(s);
    std::string out(1, q);
    for (char c : s) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\v': out += "\\v"; break;
            case '\\': out += "\\\\"; break;
            default:
                if (c == q) {
                    out.push_back('\\');
                    out.push_back(c);
                } else if (static_cast<unsigned char>(c) < 0x20) {
                    static const char* kHex = "0123456789abcdef";
                    out += "\\x";
                    out.push_back(kHex[(c >> 4) & 0xF]);
                    out.push_back(kHex[c & 0xF]);
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back(q);
    return out;
}

bool isIdentifierKey(const std::string& s) {
    if (s.empty()) return false;
    auto isStart = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
    };
    if (!isStart(s[0])) return false;
    for (char c : s) {
        if (!isStart(c) && !(c >= '0' && c <= '9')) return false;
    }
    return true;
}

std::string numberText(double num) {
    char buf[64];
    size_t len = 0;
    // -0 is distinguishable here, as it is at the top level: this is
    // inspect formatting, not ToString(Number).
    if (num == 0.0 && std::signbit(num)) buf[len++] = '-';
    len += formatJsNumber(num, buf + len);
    return std::string(buf, len);
}

class Inspector {
public:
    std::string run(Value v) {
        std::string body = format(v, 0);
        // node numbers every circularly-referenced object; bronze marks one
        // (docs/0013), so the prefix appears at most once.
        return sawCircular_ ? "<ref *1> " + body : body;
    }

private:
    std::vector<const void*> ancestors_;
    bool sawCircular_ = false;

    bool enter(const void* p) {
        if (std::find(ancestors_.begin(), ancestors_.end(), p) != ancestors_.end()) return false;
        ancestors_.push_back(p);
        return true;
    }
    void leave() { ancestors_.pop_back(); }

    std::string format(Value v, int depth) {
        if (v.isNumber()) return numberText(v.asNumber());
        if (v.isString()) return quoted(utf8Of(v.asString<StringHeader>()));
        if (v.isBool()) return v.asBool() ? "true" : "false";
        if (v.isNull()) return "null";
        if (v.isUndefined()) return "undefined";
        if (v.isHole()) fatal("internal: the hole sentinel reached console.log");
        if (v.isSymbol()) fatal("printing a symbol is unsupported (bronze has no symbols)");
        if (!v.isObject()) fatal("internal: console.log reached a value with an unknown tag");

        // An Error prints as `Name: message`, which is what node shows on the
        // first line of its output — and the whole of bronze's, because there
        // is no stack to print (docs/0020 decision 7). Tested by walking the
        // prototype chain rather than by a header flag, so an error instance
        // stays a plain object (flags == 0) and stays on the inline property
        // fast path.
        if (std::string text; rtIsErrorInstance(v) && rtErrorText(v, text)) return text;

        auto* hdr = v.asObject<HeapObjectHeader>();
        switch (hdr->flags) {
            case 1: return array(reinterpret_cast<ArrayHeader*>(hdr), depth);
            case 2: return "[Function]";
            case 3: return typedArray(reinterpret_cast<Float32ArrayHeader*>(hdr), depth);
            case 4: fatal("printing an ArrayBuffer is not implemented");
            // node prints a RegExp as its source form, and so does bronze:
            // `/ab+/gi`, with no quotes, which is what distinguishes it in
            // output from the string of the same characters.
            case RegExpHeader::kFlags: return rtRegExpText(v);
            default: return object(reinterpret_cast<ObjectHeader*>(hdr), depth);
        }
    }

    std::string array(ArrayHeader* arr, int depth) {
        if (!enter(arr)) {
            sawCircular_ = true;
            return "[Circular *1]";
        }
        if (depth > kMaxDepth) {
            leave();
            return "[Array]";
        }
        std::string out;
        // A run of HOLES prints as node prints it: `<2 empty items>`, one
        // entry for the whole run rather than one `undefined` each. The
        // distinction is the point — a hole and a stored `undefined` read
        // the same and enumerate differently (docs/0019 decision 2).
        for (uint32_t i = 0; i < arr->length;) {
            if (!out.empty()) out += ", ";
            if (!arr->hasElem(i)) {
                uint32_t run = 0;
                while (i + run < arr->length && !arr->hasElem(i + run)) ++run;
                out += "<" + std::to_string(run) + (run == 1 ? " empty item>" : " empty items>");
                i += run;
                continue;
            }
            out += format(arr->getElem(i), depth + 1);
            ++i;
        }
        // A match array's `index`, `input` and `groups` print after the
        // elements as `key: value`, which is node's format for an array that
        // carries named properties (docs/0024 decision 6). Ordinary arrays
        // have none and are unchanged.
        if (arr->properties.isObject()) {
            auto* props = arr->properties.asObject<ObjectHeader>();
            const std::vector<StringHeader*> keys =
                props->shape ? props->shape->ownKeysInInsertionOrder()
                             : std::vector<StringHeader*>{};
            for (StringHeader* k : keys) {
                PropertyInfo info;
                if (!props->shape->lookupProperty(k, info) || info.accessor) continue;
                if (!out.empty()) out += ", ";
                const std::string name = utf8Of(k);
                out += isIdentifierKey(name) ? name : quoted(name);
                out += ": " + format(props->getSlot(info.slot), depth + 1);
            }
        }
        leave();
        return out.empty() ? "[]" : "[ " + out + " ]";
    }

    std::string typedArray(Float32ArrayHeader* view, int depth) {
        // The constructor name and length are part of the format, so an
        // empty one still says what it is.
        std::string out = "Float32Array(" + std::to_string(view->length) + ") ";
        if (view->length == 0) return out + "[]";
        if (depth > kMaxDepth) return out + "[Array]";
        std::string body;
        for (uint32_t i = 0; i < view->length; ++i) {
            if (i) body += ", ";
            body += numberText(static_cast<double>(view->data()[i]));
        }
        return out + "[ " + body + " ]";
    }

    std::string object(ObjectHeader* obj, int depth) {
        if (!enter(obj)) {
            sawCircular_ = true;
            return "[Circular *1]";
        }
        if (depth > kMaxDepth) {
            leave();
            return "[Object]";
        }
        std::string out;
        // Own keys in the language's order — the same order Object.keys
        // reports (docs/0009), because they are the same question.
        std::vector<StringHeader*> keys =
            obj->shape ? obj->shape->ownKeysInInsertionOrder() : std::vector<StringHeader*>{};
        std::vector<std::pair<uint32_t, StringHeader*>> intKeys;
        std::vector<StringHeader*> strKeys;
        for (StringHeader* k : keys) {
            const std::string name = utf8Of(k);
            uint32_t idx = 0;
            if (rtIsIntegerLikeKey(name, idx)) {
                intKeys.emplace_back(idx, k);
            } else {
                strKeys.push_back(k);
            }
        }
        std::sort(intKeys.begin(), intKeys.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        auto emit = [&](StringHeader* k) {
            PropertyInfo info;
            if (!obj->shape || !obj->shape->lookupProperty(k, info)) return;
            const std::string name = utf8Of(k);
            if (!out.empty()) out += ", ";
            out += isIdentifierKey(name) ? name : quoted(name);
            out += ": ";
            if (info.accessor) {
                // node names the halves rather than RUNNING the getter, and
                // so does bronze: inspecting a value must not have effects,
                // and this walk deliberately allocates nothing.
                const bool hasGet = !obj->getSlot(info.slot).isUndefined();
                const bool hasSet = !obj->getSlot(info.slot + 1).isUndefined();
                out += hasGet ? (hasSet ? "[Getter/Setter]" : "[Getter]") : "[Setter]";
                return;
            }
            out += format(obj->getSlot(info.slot), depth + 1);
        };
        for (const auto& [idx, k] : intKeys) emit(k);
        for (StringHeader* k : strKeys) emit(k);

        leave();
        return out.empty() ? "{}" : "{ " + out + " }";
    }
};

}  // namespace

std::string rtInspect(Value v) { return Inspector().run(v); }

}  // namespace bronze::runtime
