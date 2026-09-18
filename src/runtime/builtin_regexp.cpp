// The `RegExp` object: the constructor, `test` and `toString`, and the members
// a program reads off an instance. `exec` — which every other regular-
// expression operation in bronze is built from — and the compiled-pattern
// table under it are builtin_regexp_exec.cpp; `RegExp.escape` is
// builtin_regexp_escape.cpp; the symbol-keyed members are
// builtin_regexp_symbols.cpp. builtin_regexp_internal.h is the seam.
//
// A RegExp has no prototype object, for the reason a Map has none: it carries
// no shape, so there is nothing to hang one on. Its methods are handed out by
// the property path, and `re instanceof RegExp` is false, which is a deliberate
// divergence from node.

#include <cstring>
#include <iterator>
#include <string>

#include "abi/bronze_abi.h"
#include "regex/regex.h"
#include "runtime/builtin_regexp_internal.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/regexp.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

bool isRegExp(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == RegExpHeader::kFlags;
}

// 22.2.6.16: `test` is `exec` and a null check. Written as exactly that, so
// the two can never disagree about `lastIndex`.
uint64_t regexpTest(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> self{Value(thisBits)};
    Rooted<Value> input{rtValueToString(args[0])};
    Value result = rtRegExpExec(self, input);
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    return Value::fromBool(!result.isNull()).rawBits();
}

uint64_t regexpToString(uint64_t, uint64_t thisBits, uint32_t, const uint64_t*) {
    Value self(thisBits);
    if (!isRegExp(self)) {
        return rtThrowTypeError("RegExp.prototype.toString called on an incompatible receiver")
            .rawBits();
    }
    const std::string text = rtRegExpText(self);
    return rtMakeString(text).rawBits();
}

// 22.2.3.1. The first argument may be a RegExp, in which case its source is
// reused and — when no flags argument is given — so are its flags.
uint64_t regexpConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> pattern{args[0]};
    std::string flagsText;
    if (isRegExp(pattern.get())) {
        Rooted<Value> inner{pattern.get().asObject<RegExpHeader>()->flagsText};
        flagsText = args.count() > 1 && !args[1].isUndefined()
                        ? rtUtf8Chars(rtValueToString(args[1]).asString<StringHeader>())
                        : rtUtf8Chars(inner.get().asString<StringHeader>());
        Rooted<Value> source{pattern.get().asObject<RegExpHeader>()->source};
        return rtMakeRegExp(source, flagsText).rawBits();
    }
    // 22.2.3.1 step 5: no pattern argument is the EMPTY pattern, not the string
    // "undefined". What the empty pattern's source reads as is rtMakeRegExp's.
    Rooted<Value> source{pattern.get().isUndefined() ? rtMakeString("")
                                                     : rtValueToString(pattern.get())};
    if (args.count() > 1 && !args[1].isUndefined()) {
        flagsText = rtUtf8Chars(rtValueToString(args[1]).asString<StringHeader>());
    }
    return rtMakeRegExp(source, flagsText).rawBits();
}

using RegExpMethod = NativeMethod;

const RegExpMethod kRegExpMethods[] = {
    {"exec", rtRegExpExecBody, 1, 1},
    {"test", regexpTest, 1, 1},
    {"toString", regexpToString, 0, 0},
};

// RegExp.prototype, minus everything above and minus the flag accessors, which
// are real. A member ECMA-262 defines and bronze has not built is a named error
// rather than `undefined`.
//
// The SYMBOL-keyed members — `[Symbol.match]`, `[Symbol.replace]` and the rest
// of 22.2.6 — are not here and cannot be: this table is matched against a
// string, and no string names one of them. They live in
// builtin_regexp_symbols.cpp and are answered by key, through
// `rtRegExpSymbolMethod`, from the symbol-keyed read path.
const char* const kRegExpMembers[] = {
    "compile", "constructor",
};

// The two groups of REAL members, as tables rather than as a ladder of `if`s,
// so that `rtRegExpMember` and `rtRegExpHasMember` read one list each. They are
// pointers-to-member rather than names alone because the reader needs the
// field, and a name list beside the ladder would be a second list to keep in
// step — which for `in` is exactly the failure mode being fixed elsewhere in
// this change.

// 22.2.6.10, 22.2.6.5 and 22.2.6.9: the three the header carries verbatim.
// `lastIndex` is the only own property of the three; the other two are
// prototype accessors, and `in` cannot tell the difference because a RegExp
// has no chain here for it to stop at.
struct HeaderMember {
    const char* name;
    Value RegExpHeader::* field;
};

const HeaderMember kRegExpHeaderMembers[] = {
    {"source", &RegExpHeader::source},
    {"flags", &RegExpHeader::flagsText},
    {"lastIndex", &RegExpHeader::lastIndex},
};

// The flag accessors of 22.2.6, each reading one bool out of the compiled
// pattern's flags.
struct FlagMember {
    const char* name;
    bool regex::Flags::* field;
};

const FlagMember kRegExpFlagMembers[] = {
    // 22.2.6.6: `d` decides whether `exec` attaches `indices`, and this is how
    // a program asks which it will get without running a match.
    {"hasIndices", &regex::Flags::hasIndices},
    {"global", &regex::Flags::global},       {"ignoreCase", &regex::Flags::ignoreCase},
    {"multiline", &regex::Flags::multiline}, {"dotAll", &regex::Flags::dotAll},
    // 22.2.6.18: a real accessor now that the flag is a real mode, and the one
    // way a program can ask which alphabet a pattern was compiled over.
    {"unicode", &regex::Flags::unicode},
    // 22.2.6.19: the second reading of the same mode, which is why it is a bit
    // of its own and not `unicode` again — a program can tell `/a/u` from `/a/v`.
    {"unicodeSets", &regex::Flags::unicodeSets},
    {"sticky", &regex::Flags::sticky},
};

}  // namespace

Value rtRegExpConstructor(const std::string& name) {
    if (name != "RegExp") return Value::fromUndefined();
    return rtNativeFunction(regexpConstructor, 2, "RegExp", 2);
}

// Which function object is %RegExp%, asked WITHOUT building it. The obvious
// spelling — materialise `RegExp` through `rtRegExpConstructor` and compare
// addresses — makes a question an allocation, and `rtNativeFunction` interns on
// first use, so the very first caller collects. That is not a cost, it is a
// correctness bug for the caller: this predicate is one rung of the
// function-object miss ladder in rt_prop.cpp, and a collection there retires the
// property box the rungs on either side of it are holding. The code pointer is
// the identity the intern table itself keys on, so comparing it answers the same
// question and allocates nothing — which is how `rtIsArrayConstructor` and
// `rtIsPromiseConstructor` have always answered theirs.
bool rtIsRegExpConstructor(Value fn) {
    return fn.isObject() && fn.asObject<HeapObjectHeader>()->flags == HeapKind::Function &&
           fn.asObject<FunctionHeader>()->code == regexpConstructor;
}

// The own members of the `RegExp` constructor FUNCTION object — today just
// `escape` (22.2.5.2). Answered from a table beside the value for the reason
// `rtTypedArrayStatic` is: the constructor is an interned function singleton
// with no property object of its own, so the property path asks this instead
// of walking a chain that does not exist.
//
// Both guards come before the only allocation, so a receiver that is not
// %RegExp% — which is every receiver, on almost every property miss in the
// program — costs two comparisons and nothing else.
bool rtRegExpStatic(Value fn, const std::string& key, Value& out) {
    if (!rtIsRegExpConstructor(fn)) return false;
    if (key != "escape") return false;
    out = rtNativeFunction(rtRegExpEscapeBody, 1, "escape", 1);
    return true;
}

Value rtRegExpMethod(const std::string& key) {
    for (const RegExpMethod& m : kRegExpMethods) {
        if (key == m.name) return rtNativeFunction(m.code, m.arity, m.name, m.length);
    }
    return Value::fromUndefined();
}

std::string rtRegExpText(Value re) {
    const auto* header = re.asObject<RegExpHeader>();
    return "/" + rtUtf8Chars(header->source.asString<StringHeader>()) + "/" +
           rtUtf8Chars(header->flagsText.asString<StringHeader>());
}

Value rtRegExpMember(Value re, const std::string& key) {
    const auto* header = re.asObject<RegExpHeader>();
    for (const HeaderMember& m : kRegExpHeaderMembers) {
        if (key == m.name) return header->*m.field;
    }
    const regex::Flags& flags = regex::patternFlags(rtRegExpPattern(re));
    for (const FlagMember& m : kRegExpFlagMembers) {
        if (key == m.name) return Value::fromBool(flags.*m.field);
    }
    Value method = rtRegExpMethod(key);
    if (!method.isUndefined()) return method;
    rtCheckUnimplementedMember("RegExp.prototype", kRegExpMembers, std::size(kRegExpMembers), key);
    return Value::fromUndefined();
}

// The same three tables, asked whether the member EXISTS rather than what it
// is — which is all `in` needs, and is answerable for a member whose value
// bronze refuses to produce. A name in none of them is refused by name if
// ECMA-262 defines it, exactly as a read of it is, and is `false` only when the
// read path would answer `undefined`.
bool rtRegExpHasMember(const std::string& key) {
    for (const HeaderMember& m : kRegExpHeaderMembers) {
        if (key == m.name) return true;
    }
    for (const FlagMember& m : kRegExpFlagMembers) {
        if (key == m.name) return true;
    }
    for (const RegExpMethod& m : kRegExpMethods) {
        if (key == m.name) return true;
    }
    rtCheckUnimplementedMember("RegExp.prototype", kRegExpMembers, std::size(kRegExpMembers), key);
    return false;
}

bool rtRegExpSetMember(Value re, const std::string& key, Value value) {
    if (key != "lastIndex") return false;
    // `re.lastIndex = {valueOf(){...}}` runs 7.1.4 on the value, which is user
    // code: the receiver is rooted across it and the header taken afterwards.
    Rooted<Value> reRoot{re};
    const double num = rtToNumber(value);
    if (rtExceptionPending()) return true;
    reRoot.get().asObject<RegExpHeader>()->lastIndex = Value::fromDouble(num);
    return true;
}

}  // namespace bronze::runtime
