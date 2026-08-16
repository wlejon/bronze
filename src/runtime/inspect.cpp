// console.log of a container, in the pinned inspect format.
//
// Nothing here allocates a JS value, which is why every raw pointer below stays
// valid for the whole walk: the collector cannot run while a string is being
// built.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/map.h"
#include "runtime/number_format.h"
#include "runtime/object.h"
#include "runtime/namespace.h"
#include "runtime/regexp.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
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

// A key as it appears on the left of a `:`. node brackets a symbol key —
// `{ [Symbol(tag)]: 1 }` — because the text is not a name a program could have
// written unbracketed, and that is the whole visual difference between a symbol
// key and a string one that happens to spell "Symbol(tag)".
std::string keyLabel(PropertyKey k) {
    if (k.isSymbol()) return "[" + rtSymbolDescriptiveString(k.toValue()) + "]";
    const std::string name = utf8Of(k.string());
    return isIdentifierKey(name) ? name : quoted(name);
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
        // node numbers every circularly-referenced object; bronze marks one, so
        // the prefix appears at most once.
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
        // A symbol prints as its descriptive string and NOT quoted, which is
        // what distinguishes `Symbol("a")` from the string "Symbol(a)" in
        // output — the same rule that prints a RegExp as `/a/g` rather than as
        // its source text.
        if (v.isSymbol()) return rtSymbolDescriptiveString(v);
        if (!v.isObject()) fatal("internal: console.log reached a value with an unknown tag");

        // An Error prints as `Name: message`, which is what node shows on the
        // first line of its output — and the whole of bronze's, because there
        // is no stack to print. Tested by walking the prototype chain rather
        // than by a header flag, so an error instance stays a plain object
        // (flags == HeapKind::Plain) and stays on the inline property fast path.
        if (std::string text; rtIsErrorInstance(v) && rtErrorText(v, text)) return text;

        // A primitive wrapper prints as node prints one — `[String: 'ab']`,
        // `[Number: 1]`, `[Boolean: false]` — and not as the `{}` an object with no own
        // properties would otherwise show. The whole content of one of these is
        // an internal slot, so `{}` would be a lie about the value rather than
        // a terse rendering of it. Tested by the brand, which reads two words
        // and allocates nothing, like everything else in this walk.
        // A Date prints as its ISO form, bare — `2020-01-01T00:00:00.000Z`,
        // with no quotes and no braces — which is node's rendering and the one
        // form of a Date that is the same on every machine. `Invalid Date` for
        // a NaN time value, again node's. The alternative was `toString`'s
        // output, which carries the local zone offset and would make a printed
        // Date depend on where the program ran.
        if (std::string text; rtDateInspectText(v, text)) return text;

        if (Value data; rtStringWrapperData(v, data)) {
            return "[String: " + quoted(utf8Of(data.asString<StringHeader>())) + "]";
        }
        if (Value data; rtBooleanWrapperData(v, data)) {
            return std::string("[Boolean: ") + (data.asBool() ? "true" : "false") + "]";
        }
        if (Value data; rtNumberWrapperData(v, data)) {
            return "[Number: " + numberText(data.asNumber()) + "]";
        }

        // One arm per heap kind, and a `default:` that REFUSES. It used to cast
        // whatever it had not been taught to an ObjectHeader and read a shape
        // word that a Map or a Set does not have there — a segfault, which is
        // the one failure the house rules rank below a wrong answer. A kind
        // added tomorrow lands on the diagnostic instead.
        auto* hdr = v.asObject<HeapObjectHeader>();
        switch (hdr->flags) {
            case BRONZE_ABI_OBJ_FLAGS_PLAIN:
                return object(reinterpret_cast<ObjectHeader*>(hdr), depth);
            case HeapKind::Array: return array(reinterpret_cast<ArrayHeader*>(hdr), depth);
            case HeapKind::Function: return "[Function]";
            // A module namespace shows its kind and its EXPORT NAMES, in
            // 10.4.6.2's order, and not its values. Reading a value here means
            // calling the getter that closes over the exporting binding, and
            // this walk deliberately runs no user code and allocates nothing —
            // the same rule that prints an accessor as `[Getter]` a few lines
            // below. The names are the whole of what the object IS, so this is
            // a complete rendering rather than a shortened one.
            case ModuleNamespaceHeader::kFlags: {
                std::string out;
                for (StringHeader* name : rtModuleNamespaceKeys(v)) {
                    out += (out.empty() ? ": " : " ") + utf8Of(name);
                }
                return "[Module" + out + "]";
            }
            case TypedArrayHeader::kFlags:
                return typedArray(reinterpret_cast<TypedArrayHeader*>(hdr), depth);
            case ArrayBufferHeader::kFlags:
                return arrayBuffer(reinterpret_cast<ArrayBufferHeader*>(hdr), depth);
            case DataViewHeader::kFlags:
                return dataView(reinterpret_cast<DataViewHeader*>(hdr), depth);
            case MapHeader::kMapFlags:
                return collection(reinterpret_cast<MapHeader*>(hdr), false, depth);
            case MapHeader::kSetFlags:
                return collection(reinterpret_cast<MapHeader*>(hdr), true, depth);
            // node's spelling, and the CONTENTS are withheld on purpose: a
            // WeakMap is non-iterable, so a console.log that listed its
            // entries would be the one place in the language they leak.
            case MapHeader::kWeakMapFlags: return "WeakMap { <items unknown> }";
            case MapHeader::kWeakSetFlags: return "WeakSet { <items unknown> }";
            // node prints a RegExp as its source form, and so does bronze:
            // `/ab+/gi`, with no quotes, which is what distinguishes it in
            // output from the string of the same characters.
            case RegExpHeader::kFlags: return rtRegExpText(v);
            default:
                fatal(("internal: console.log reached a heap object of an unknown kind (flags " +
                       std::to_string(hdr->flags) + ")")
                          .c_str());
        }
    }

    // A Map and a Set are ONE layout that differs in what an entry means: a
    // Map's has a key and a value with ` => ` between them, a Set's is a single
    // element with no second half to separate. The `Ctor(size)` prefix is the
    // one `Float32Array(3) [ 0, 0, 0 ]` already established here, and it is
    // what makes an empty collection say what it is instead of showing `{}`.
    std::string collection(MapHeader* map, bool isSet, int depth) {
        const char* name = isSet ? "Set" : "Map";
        if (!enter(map)) {
            sawCircular_ = true;
            return "[Circular *1]";
        }
        if (depth > kMaxDepth) {
            leave();
            return std::string("[") + name + "]";
        }
        std::string out = std::string(name) + "(" + std::to_string(map->liveSize()) + ") ";
        std::string body;
        for (uint32_t slot = 0; slot < map->used(); ++slot) {
            // A deleted entry is TOMBSTONED rather than erased, so that a live
            // iterator's cursor keeps meaning what it meant; nothing that reads
            // the collection may see one.
            if (!map->liveAt(slot)) continue;
            if (!body.empty()) body += ", ";
            body += format(map->keyAt(slot), depth + 1);
            if (!isSet) body += " => " + format(map->valueAt(slot), depth + 1);
        }
        leave();
        return body.empty() ? out + "{}" : out + "{ " + body + " }";
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
        // A run of HOLES prints as node prints it: `<2 empty items>`, one entry
        // for the whole run rather than one `undefined` each. The distinction
        // is the point — a hole and a stored `undefined` read the same and
        // enumerate differently.
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
        // carries named properties. Ordinary arrays have none and are
        // unchanged.
        if (arr->properties.isObject()) {
            auto* props = arr->properties.asObject<ObjectHeader>();
            const std::vector<PropertyKey> keys =
                props->shape ? props->shape->ownKeysInInsertionOrder()
                             : std::vector<PropertyKey>{};
            for (PropertyKey k : keys) {
                PropertyInfo info;
                if (!props->shape->lookupProperty(k, info) || info.accessor) continue;
                if (!out.empty()) out += ", ";
                out += keyLabel(k);
                out += ": " + format(props->getSlot(info.slot), depth + 1);
            }
        }
        leave();
        return out.empty() ? "[]" : "[ " + out + " ]";
    }

    std::string typedArray(TypedArrayHeader* view, int depth) {
        // The constructor name and length are part of the format, so an empty
        // one still says what it is — and the name comes from the element kind,
        // so all nine views print as themselves.
        std::string out = std::string(view->kindName()) + "(" + std::to_string(view->length) + ") ";
        if (view->length == 0) return out + "[]";
        if (depth > kMaxDepth) return out + "[Array]";
        std::string body;
        for (uint32_t i = 0; i < view->length; ++i) {
            if (i) body += ", ";
            body += numberText(view->get(i));
        }
        return out + "[ " + body + " ]";
    }

    // A buffer prints as its BYTES, in hex, because that is the whole of what
    // it is: it has no elements, no element kind and no way for a program to
    // read it except through a view. node's spelling, and the `[Uint8Contents]`
    // label is part of it — the brackets say the entry is an internal slot
    // rather than a property `Object.keys` would report.
    //
    // Long buffers are cut off at kMaxBytes with a count of what was dropped.
    // A 256 MiB buffer (typed_array.h's cap) would otherwise print a line of
    // 768 million characters, which is not a rendering of the value in any
    // useful sense.
    std::string arrayBuffer(ArrayBufferHeader* buf, int depth) {
        static constexpr uint32_t kMaxBytes = 100;
        if (depth > kMaxDepth) return "[ArrayBuffer]";
        const uint32_t length = buf->byteLength;
        const uint32_t shown = std::min(length, kMaxBytes);
        const uint8_t* data = buf->data();
        std::string body;
        for (uint32_t i = 0; i < shown; ++i) {
            if (i) body += ' ';
            // Lowercase hex, two digits, always — a byte is a fixed-width thing
            // and a printed buffer is read by counting columns.
            static const char* kHex = "0123456789abcdef";
            body += kHex[data[i] >> 4];
            body += kHex[data[i] & 0xF];
        }
        if (length > shown) {
            const uint32_t rest = length - shown;
            body += " ... " + std::to_string(rest) + (rest == 1 ? " more byte" : " more bytes");
        }
        return "ArrayBuffer { [Uint8Contents]: <" + body +
               ">, byteLength: " + std::to_string(length) + " }";
    }

    // The three slots 25.3.4.1..25.3.4.3 expose, in that order, and the buffer
    // expanded inside them. Nothing about the CONTENT of the window is printed:
    // a DataView has no element type, so there is no list of values to show —
    // which is exactly the distinction from a typed array above.
    std::string dataView(DataViewHeader* view, int depth) {
        if (depth > kMaxDepth) return "[DataView]";
        // The buffer is one level deeper than the view that names it, on the
        // same terms as an object's property value — so a buffer nested past
        // the cut prints as `[ArrayBuffer]` while the view around it still
        // shows its slots.
        return "DataView { byteLength: " + std::to_string(view->byteLength) +
               ", byteOffset: " + std::to_string(view->byteOffset) + ", buffer: " +
               arrayBuffer(view->buffer.asObject<ArrayBufferHeader>(), depth + 1) + " }";
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
        // reports, because they are the same question.
        std::vector<PropertyKey> keys =
            obj->shape ? obj->shape->ownKeysInInsertionOrder() : std::vector<PropertyKey>{};
        std::vector<std::pair<uint32_t, PropertyKey>> intKeys;
        std::vector<PropertyKey> strKeys;
        // Symbol keys print LAST, after every string key, which is the order
        // 6.1.7.1 gives them and the order node shows them in.
        std::vector<PropertyKey> symKeys;
        for (PropertyKey k : keys) {
            if (k.isSymbol()) {
                symKeys.push_back(k);
                continue;
            }
            const std::string name = utf8Of(k.string());
            uint32_t idx = 0;
            if (rtIsIntegerLikeKey(name, idx)) {
                intKeys.emplace_back(idx, k);
            } else {
                strKeys.push_back(k);
            }
        }
        std::sort(intKeys.begin(), intKeys.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        auto emit = [&](PropertyKey k) {
            PropertyInfo info;
            if (!obj->shape || !obj->shape->lookupProperty(k, info)) return;
            if (!out.empty()) out += ", ";
            out += keyLabel(k);
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
        for (PropertyKey k : strKeys) emit(k);
        for (PropertyKey k : symKeys) emit(k);

        leave();
        return out.empty() ? "{}" : "{ " + out + " }";
    }
};

}  // namespace

std::string rtInspect(Value v) { return Inspector().run(v); }

}  // namespace bronze::runtime
