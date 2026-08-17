#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "regex/pattern.h"

// The recursive-descent parser's own surface, shared by the two translation
// units that hold it: `parser.cpp` is the Pattern grammar down to the Atom, and
// `parser_class.cpp` is everything between `[` and `]`.
//
// The seam is a GRAMMAR one and not a line count. 22.2.1 gives a character
// class its own productions, and the `v` flag gives it a SECOND set of them —
// ClassSetExpression, with nesting, `--`, `&&` and its own reserved punctuation
// — which share nothing with Disjunction/Term/Atom but the cursor and the
// escape table. Both readers therefore need the same parser object, and it is
// declared here rather than duplicated: a second cursor is how the two would
// come to disagree about where a class ends.
//
// Nothing here is public. `regex.h` hands out a compiled `Pattern` and never a
// parser.

namespace bronze::regex {

constexpr uint32_t kUnbounded = 0xFFFFFFFFu;

inline bool isDecimalDigit(uint16_t c) { return c >= '0' && c <= '9'; }

inline bool isIdentifierStart(uint16_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

inline bool isIdentifierPart(uint16_t c) { return isIdentifierStart(c) || isDecimalDigit(c); }

// What an IdentityEscape may NOT be. 22.2.1 lets `\` precede a SyntaxCharacter
// — `^ $ \ . * + ? ( ) [ ] { } |` — or `/`, and Annex B widens that to almost
// anything; a LETTER OR DIGIT is where the two readings stop agreeing, and a
// pattern written for another engine's `\q` extension would otherwise match a
// plain `q` here. `$` and `_` are neither, so `\$` is the ordinary dollar sign.
inline bool isAlphanumeric(uint16_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDecimalDigit(c);
}

// 22.2.1's UnicodePropertyNameCharacters is letters and `_`, and
// UnicodePropertyValueCharacters adds the digits. One predicate serves both: a
// digit written in a name position simply names no property, which is a better
// message than "unexpected character" for the same mistake.
inline bool isPropertyCharacter(uint16_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDecimalDigit(c) || c == '_';
}

// 22.2.1 SyntaxCharacter. It is what `\` may precede under +UnicodeMode, and —
// as PatternCharacter's exclusion list — what may NOT be written bare there.
// Annex B's ~UnicodeMode productions widen both, which is why this list is
// consulted only when `u` or `v` is set.
inline bool isSyntaxCharacter(uint16_t c) {
    switch (c) {
        case '^': case '$': case '\\': case '.': case '*': case '+': case '?':
        case '(': case ')': case '[': case ']': case '{': case '}': case '|':
            return true;
        default:
            return false;
    }
}

// 22.2.1 ClassSetSyntaxCharacter: what a `v`-mode class may NOT hold bare.
// The list is longer than the non-`v` one on purpose — `(`, `{` and `|` mean
// nothing inside a class today and are reserved so that a future set operator
// can be spelled with them, and `-` is reserved because `--` is one. Writing
// one bare is a syntax error rather than the character, which is the whole
// point: a pattern using tomorrow's operator must not quietly match today.
inline bool isClassSetSyntaxCharacter(uint16_t c) {
    switch (c) {
        case '(': case ')': case '[': case ']': case '{': case '}':
        case '/': case '-': case '\\': case '|':
            return true;
        default:
            return false;
    }
}

// 22.2.1 ClassSetReservedPunctuator: the characters a `v`-mode class may hold
// bare ONCE but not twice, and which `\` may therefore precede. `[\&]` is the
// ampersand; `[&&]` is the intersection operator with nothing to intersect.
inline bool isClassSetReservedPunctuator(uint16_t c) {
    switch (c) {
        case '&': case '-': case '!': case '#': case '%': case ',': case ':':
        case ';': case '<': case '=': case '>': case '@': case '`': case '~':
            return true;
        default:
            return false;
    }
}

// 22.2.1 ClassSetReservedDoublePunctuator, as the doubling rule it is: each of
// these nineteen characters is ordinary alone and reserved written TWICE in a
// row. `&&` is the one that means something today and the rest are held for the
// same reason the syntax characters are. The list is not the punctuator list
// above: `$ * + ? .` double but may not be escaped, and `-` is a syntax
// character rather than a doubled one because `--` is already an operator.
inline bool isClassSetDoubledPunctuator(uint16_t c) {
    switch (c) {
        case '&': case '!': case '#': case '$': case '%': case '*': case '+':
        case ',': case '.': case ':': case ';': case '<': case '=': case '>':
        case '?': case '@': case '^': case '`': case '~':
            return true;
        default:
            return false;
    }
}

inline bool hexDigitValue(uint16_t c, uint32_t& out) {
    if (c >= '0' && c <= '9') out = static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') out = static_cast<uint32_t>(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') out = static_cast<uint32_t>(c - 'A' + 10);
    else return false;
    return true;
}

inline bool isLeadSurrogate(uint32_t c) { return c >= 0xD800 && c <= 0xDBFF; }
inline bool isTrailSurrogate(uint32_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

// A group name is written back out in diagnostics and used as a property key,
// so it is carried as UTF-8. Names are restricted to ASCII identifiers
// (bronze's own identifier rule), which is narrower than the
// specification's IdentifierName and is refused by name rather than silently.
inline std::string asciiOf(UnitsView units) {
    std::string out;
    out.reserve(units.size());
    for (uint16_t u : units) out.push_back(static_cast<char>(u));
    return out;
}

// What the pre-scan collects: how many capturing groups there are, and what
// each is called. Both are needed BEFORE the parse, because a backreference
// may precede the group it names.
struct GroupInfo {
    uint32_t count = 0;
    std::vector<std::string> names;  // one per group, empty when unnamed
};

GroupInfo prescanGroups(UnitsView src);

class PatternParser {
public:
    PatternParser(UnitsView src, const Flags& flags, const GroupInfo& groups)
        : src_(src), flags_(flags), groups_(groups) {}

    NodePtr parse(std::string& error);

    uint32_t capturesSeen() const { return captureCounter_; }

private:
    UnitsView src_;
    const Flags& flags_;
    const GroupInfo& groups_;
    size_t pos_ = 0;
    uint32_t captureCounter_ = 0;
    std::string error_;

    bool atEnd() const { return pos_ >= src_.size(); }
    uint16_t peek(size_t ahead = 0) const {
        const size_t at = pos_ + ahead;
        return at < src_.size() ? src_[at] : 0;
    }

    bool eat(uint16_t c) {
        if (atEnd() || src_[pos_] != c) return false;
        ++pos_;
        return true;
    }

    // Two characters in a row, which is how every operator of the ClassSet
    // grammar is spelled (`--`, `&&`) and how its reserved double punctuators
    // are recognised.
    bool eatTwo(uint16_t c) {
        if (pos_ + 1 >= src_.size() || src_[pos_] != c || src_[pos_ + 1] != c) return false;
        pos_ += 2;
        return true;
    }

    // One SourceCharacter of the PATTERN TEXT, which is one code POINT under
    // `u` or `v`. A surrogate pair written between the slashes is a single atom
    // there, so `/😀+/u` repeats the whole character rather than its trailing
    // half — and without either flag the same text is two units and two atoms,
    // which is what makes `/😀+/` repeat only the second of them.
    uint32_t readSourceCharacter() {
        const CodePointStep step = codePointAt(src_, pos_, flags_.unicodeMode());
        pos_ += step.width;
        return step.code;
    }

    std::string describe(const std::string& what) const {
        return what + " at index " + std::to_string(pos_) + " of the pattern";
    }

    std::nullptr_t fail(const std::string& what) {
        if (error_.empty()) error_ = describe(what);
        return nullptr;
    }

    // The same refusal for a caller whose return type is `bool`. Two spellings
    // of one act, so that neither reader has to write the message twice.
    bool refuse(const std::string& what) {
        fail(what);
        return false;
    }

    NodePtr make(NodeKind kind) {
        auto n = std::make_unique<Node>();
        n->kind = kind;
        return n;
    }

    // ---- Disjunction / Alternative / Term (parser.cpp) ---------------------

    NodePtr parseDisjunction();
    NodePtr parseAlternative();
    NodePtr parseTerm();
    bool readQuantifier(uint32_t& min, uint32_t& max, bool& greedy);
    bool readBoundedDigits(size_t& at, uint32_t& out);

    // ---- Atom (parser.cpp) -------------------------------------------------

    NodePtr parseAtom();
    NodePtr charNode(uint32_t code);
    std::nullptr_t caseTableFailure(uint32_t unit);
    NodePtr parseGroup();

    // ---- Escapes (parser.cpp) ----------------------------------------------

    // What an escape denotes: either one character, or a set of them. The two
    // are one return type because ClassAtom accepts both and the atom position
    // accepts both, and splitting them would duplicate the whole escape table.
    // `code` is a code POINT under `u` or `v` — `\u{1F600}` is one of these —
    // and a code unit without them.
    struct EscapeValue {
        bool isSet = false;
        uint32_t code = 0;
        RangeList set;
    };

    NodePtr parseAtomEscape();
    bool readEscapeValue(EscapeValue& out, bool inClass);
    bool readPropertyEscape(EscapeValue& out, bool negated);
    bool readUnicodeEscape(EscapeValue& out);
    bool readHex(size_t at, size_t count, uint32_t& out);

    // ---- Character class (parser_class.cpp) --------------------------------

    // `[` ... `]`, in whichever of the two grammars the flags picked: 22.2.1's
    // ClassRanges without `v`, ClassSetExpression with it.
    NodePtr parseCharacterClass();
    bool readClassAtom(EscapeValue& out);

    // A ClassSetExpression's value: 22.2.2.9's CharSet, whose members are
    // SEQUENCES of code points rather than characters.
    //
    // `ranges` holds every ONE-character member, because a one-element sequence
    // IS a character and set algebra with two places to look for one is how the
    // two would come to disagree. `strings` holds the members of length two or
    // more and `empty` the zero-length one — the two things a CharSet of
    // characters could not hold, and the whole of what `\q{...}` adds.
    //
    // `mayContainStrings` is 22.2.1's static semantics of that name, and it is
    // deliberately NOT `empty || !strings.empty()`: the specification decides it
    // from the SYNTAX, so `[\q{ab}--\q{ab}]` may contain strings even though
    // its value holds none — which is what makes `[^[\q{ab}--\q{ab}]]` an early
    // SyntaxError. Computing it from the contents would quietly admit that
    // pattern.
    struct ClassSetValue {
        RangeList ranges;
        std::vector<std::vector<uint32_t>> strings;
        bool empty = false;
        bool mayContainStrings = false;
    };

    NodePtr parseClassSet();
    bool readNestedClass(ClassSetValue& out);
    bool readClassSetExpression(ClassSetValue& out);
    bool readClassSetUnion(ClassSetValue& out, ClassSetValue first, bool firstIsCharacter,
                           uint32_t firstCode);
    bool readClassSetOperand(ClassSetValue& out, bool& isCharacter, uint32_t& code);
    bool readClassSetCharacter(uint32_t& out);

    // ClassStringDisjunction (22.2.1): `\q{ alt | alt | ... }`, where an
    // alternative of exactly one character is an ordinary member of `ranges` and
    // one of any other length is a string. The `\q` has already been consumed.
    bool readClassStringDisjunction(ClassSetValue& out);

    // 22.2.2.9's MaybeSimpleCaseFolding, which `&&` and `--` apply to both
    // their operands. It is the identity without `i`, so the two operators can
    // be written once.
    RangeList foldedForOperation(const RangeList& set) const;

    // The complement, over the alphabet 22.2.2.9's AllCharacters gives this
    // mode. Under `v` with `i` that alphabet is the code points that are their
    // OWN simple case folding, which is what makes `[^\P{X}]` come back to
    // `[\p{X}]` there where under `u` it does not.
    RangeList complementInMode(const RangeList& set) const;
};

}  // namespace bronze::regex
