#include "types/pins.h"

#include <sstream>

namespace bronze::types {
namespace {

// Identifier grammar, strict and ASCII, for the reason the `--host-globals`
// loader gives: the manifest is a contract between a build and a program, and
// a contract is the wrong place for Unicode spellings two editors disagree
// about.
bool isIdentStart(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$';
}
bool isIdentPart(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }

bool isIdent(const std::string& s) {
    if (s.empty() || !isIdentStart(s[0])) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!isIdentPart(s[i])) return false;
    }
    return true;
}

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}

// The class name as the manifest spells it: the last dotted component of the
// linked name. See the header for why the prefix is dropped rather than
// matched.
std::string baseName(const std::string& name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

// A dotted run of identifiers — `Matrix4.multiplyMatrices`, or a bare
// `setBlending`. The owner half of the two signature forms.
bool isDottedIdent(const std::string& s) {
    size_t begin = 0;
    while (true) {
        const auto dot = s.find('.', begin);
        const std::string part = s.substr(begin, dot == std::string::npos ? dot : dot - begin);
        if (!isIdent(part)) return false;
        if (dot == std::string::npos) return true;
        begin = dot + 1;
    }
}

// Every spelling of `ilName` a signature entry may use: the whole name, and
// every suffix that begins just after a dot. `mod1.Matrix4.multiplyMatrices` is
// reachable as itself, as `Matrix4.multiplyMatrices` and as
// `multiplyMatrices` — the same last-components rule `baseName` gives the field
// forms, widened because a method needs its class to be told from a same-named
// method elsewhere.
template <typename Fn>
void forEachSpelling(const std::string& ilName, Fn&& fn) {
    if (fn(ilName)) return;
    size_t begin = 0;
    while ((begin = ilName.find('.', begin)) != std::string::npos) {
        ++begin;
        if (fn(ilName.substr(begin))) return;
    }
}

}  // namespace

bool PinManifest::parse(const std::string& text, const std::string& path, std::string& err) {
    std::istringstream lines(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(lines, line)) {
        ++lineNo;
        if (auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        const std::string entry = trim(line);
        if (entry.empty()) continue;

        auto bad = [&](const std::string& why) {
            err = "error: " + path + ":" + std::to_string(lineNo) + ": " + why + ": '" + entry +
                  "'\n";
            return false;
        };

        const auto colon = entry.find(':');
        if (colon == std::string::npos) {
            return bad("pin entry needs '<class>.<field>: <kind>'");
        }
        std::string target = trim(entry.substr(0, colon));
        const std::string kindText = trim(entry.substr(colon + 1));

        // The two SIGNATURE forms, recognized before the kind is read for the
        // reason the `function` form is: they admit `number` and nothing else.
        // A calling convention has one unboxed slot shape, and `ilTypeOf` is
        // what says which lattice element earns it.
        if (target.rfind("param ", 0) == 0) {
            if (kindText != "number") return bad("a parameter pin's only kind is 'number'");
            const std::string rest = trim(target.substr(6));
            const auto open = rest.find('(');
            if (open == std::string::npos || rest.empty() || rest.back() != ')') {
                return bad("parameter pin target needs '<owner>(<parameter>)'");
            }
            const std::string owner = trim(rest.substr(0, open));
            const std::string param = trim(rest.substr(open + 1, rest.size() - open - 2));
            if (!isDottedIdent(owner)) return bad("not a valid function name in parameter pin");
            if (!isIdent(param)) return bad("not a valid parameter name in parameter pin");
            paramsBySignature_[owner].insert(param);
            continue;
        }
        if (target.rfind("return ", 0) == 0) {
            if (kindText != "number") return bad("a return pin's only kind is 'number'");
            const std::string owner = trim(target.substr(7));
            if (!isDottedIdent(owner)) return bad("not a valid function name in return pin");
            pinnedReturns_.insert(owner);
            continue;
        }

        // The `function` form. Recognized before the kind is read, because the
        // kinds it admits are a subset: an env slot pin is spent on a raw unbox
        // at the read, so `number` is the only promise strong enough for it.
        const bool envSlot = target.rfind("function ", 0) == 0;
        if (envSlot) {
            target = trim(target.substr(9));
            if (kindText != "number") {
                return bad("an env-slot pin's only kind is 'number'");
            }
        }

        PinKind kind{};
        if (kindText == "number") {
            kind = PinKind::Number;
        } else if (kindText == "numeric-elements") {
            kind = PinKind::NumericElements;
        } else if (kindText == "number-or-nullish") {
            kind = PinKind::NumberOrNullish;
        } else {
            return bad(
                "unknown pin kind (expected 'number', 'numeric-elements' or "
                "'number-or-nullish')");
        }

        const auto dot = target.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == target.size()) {
            return bad(envSlot ? "env-slot pin target needs '<function>.<binding>'"
                               : "pin target needs '<class>.<field>'");
        }
        const std::string ownerName = baseName(target.substr(0, dot));
        const std::string member = target.substr(dot + 1);
        if (!isIdent(ownerName)) {
            return bad(envSlot ? "not a valid function name in pin target"
                               : "not a valid class name in pin target");
        }
        if (envSlot) {
            // No `*`: see the header. The set of captured bindings a function
            // has is not a list the manifest author can read off the source the
            // way a class's fields are, and a wildcard over it would pin the
            // receiver slot and the loop temporaries too.
            if (!isIdent(member)) return bad("not a valid binding name in env-slot pin target");
            byFunction_[ownerName].insert(member);
            continue;
        }
        if (member != "*" && !isIdent(member)) {
            return bad("not a valid field name in pin target");
        }

        byClass_[ownerName][member] = kind;
    }
    return true;
}

const PinKind* PinManifest::lookup(const std::string& className, const std::string& field) const {
    const auto cls = byClass_.find(baseName(className));
    if (cls == byClass_.end()) return nullptr;
    const auto exact = cls->second.find(field);
    if (exact != cls->second.end()) return &exact->second;
    const auto wild = cls->second.find("*");
    if (wild != cls->second.end()) return &wild->second;
    return nullptr;
}

bool PinManifest::envSlotPinned(const std::string& functionName,
                                const std::string& binding) const {
    // No `extends` walk and no wildcard: a function's captured bindings are its
    // own, and a same-named function elsewhere shares the entry for the reason
    // two same-named classes do.
    const auto fn = byFunction_.find(baseName(functionName));
    return fn != byFunction_.end() && fn->second.count(binding) != 0;
}

bool PinManifest::paramPinned(const std::string& ilName, const std::string& param) const {
    bool found = false;
    forEachSpelling(ilName, [&](const std::string& spelling) {
        const auto it = paramsBySignature_.find(spelling);
        if (it == paramsBySignature_.end()) return false;
        // The first spelling that names the owner AT ALL answers, hit or miss:
        // an entry for `Matrix4.multiplyMatrices` and one for
        // `multiplyMatrices` are two different claims, and the more specific
        // one is the one that was written about this function.
        found = it->second.count(param) != 0;
        return true;
    });
    return found;
}

bool PinManifest::returnPinned(const std::string& ilName) const {
    bool found = false;
    forEachSpelling(ilName, [&](const std::string& spelling) {
        if (pinnedReturns_.count(spelling) == 0) return false;
        found = true;
        return true;
    });
    return found;
}

size_t PinManifest::size() const {
    size_t n = 0;
    for (const auto& [cls, fields] : byClass_) n += fields.size();
    for (const auto& [fn, slots] : byFunction_) n += slots.size();
    for (const auto& [sig, params] : paramsBySignature_) n += params.size();
    n += pinnedReturns_.size();
    return n;
}

}  // namespace bronze::types
