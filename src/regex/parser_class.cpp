// Everything between `[` and `]`, which ECMA-262 22.2.1 gives TWO grammars.
//
// Without `v` it is ClassRanges: a flat list of characters, class escapes and
// `a-z` ranges, where `-` is a range only between two single characters and is
// otherwise an ordinary member. With `v` it is ClassSetExpression, and a class
// becomes a set EXPRESSION — classes nest, `--` takes a difference, `&&` takes
// an intersection, and the characters that could ever begin an operator are
// reserved rather than literal so that a pattern written for a future operator
// cannot quietly match today.
//
// The two are one file because they answer the same question and are chosen
// between by one flag; they are not the main parser's file because they share
// nothing with Disjunction/Term/Atom but the cursor and the escape table
// (parser_internal.h says why the seam is there).
//
// Under `v` with `i` the set operations are where case folding enters, and it
// is not a detail: 22.2.2.9 applies MaybeSimpleCaseFolding to BOTH operands of
// `&&` and `--`, and takes a complement over AllCharacters — which under those
// two flags is the code points that are their own folding. That ordering is
// what makes `[^\P{Lowercase_Letter}]` come back to `[\p{Lowercase_Letter}]`
// under `vi`, where the same pattern under `ui` matches nothing of the sort.

#include <string>

#include "regex/parser_internal.h"
#include "regex/pattern.h"
#include "regex/unicode.h"

namespace bronze::regex {

namespace {

// A character named in a diagnostic. Only ASCII punctuation reaches these
// messages — every refusal below is about a reserved OPERATOR character — so
// one byte is the whole of it.
std::string asText(uint32_t c) { return std::string(1, static_cast<char>(c)); }

}  // namespace

// ---- ClassRanges (22.2.1, ~UnicodeSetsMode) ---------------------------------

NodePtr PatternParser::parseCharacterClass() {
    if (flags_.unicodeSets) return parseClassSet();

    ++pos_;  // '['
    NodePtr node = make(NodeKind::Class);
    node->negated = eat('^');

    while (!atEnd() && peek() != ']') {
        EscapeValue lhs;
        if (!readClassAtom(lhs)) return nullptr;
        // A `-` is a range only between two SINGLE characters: `[\d-x]`
        // is three members, not a range from a set, which is what
        // 22.2.1's NonemptyClassRangesNoDash says and what a `-` at the
        // end of a class relies on.
        if (!lhs.isSet && peek() == '-' && pos_ + 1 < src_.size() && src_[pos_ + 1] != ']') {
            ++pos_;
            EscapeValue rhs;
            if (!readClassAtom(rhs)) return nullptr;
            if (rhs.isSet) {
                // `[a-\d]` is a syntax error in the specification, and
                // reading it as three members would be a guess.
                return fail("a character class escape cannot be the end of a range");
            }
            if (lhs.code > rhs.code) {
                return fail("a character class range whose start is after its end");
            }
            // Every unit the range CONTAINS, not just the two it spells.
            // Testing the endpoints alone let `[ÿ- ]` through, and
            // the pattern then answered plain containment for U+1E9E — the
            // fold skipped rather than diagnosed, which is the exact silent
            // wrong answer this refusal exists to prevent. The offender is
            // named, because a message that says only "somewhere in this
            // range" cannot be acted on.
            if (flags_.ignoreCase && !flags_.unicodeMode()) {
                uint32_t offender = 0;
                if (firstUnknownCasedUnitInRange(lhs.code, rhs.code, offender)) {
                    return caseTableFailure(offender);
                }
            }
            // Under `u` the endpoints may be code POINTS, so this range can
            // span above U+FFFF and `[\u{1F600}-\u{1F64F}]` is one interval
            // rather than a set of surrogate halves that would also match
            // every other astral character sharing a lead.
            addRange(node->ranges, lhs.code, rhs.code);
            continue;
        }
        if (lhs.isSet) {
            node->ranges.insert(node->ranges.end(), lhs.set.begin(), lhs.set.end());
        } else {
            if (flags_.ignoreCase && !flags_.unicodeMode() && isUnknownCasedUnit(lhs.code)) {
                return caseTableFailure(lhs.code);
            }
            addRange(node->ranges, lhs.code, lhs.code);
        }
    }
    if (!eat(']')) return fail("`]` was expected to close a character class");
    normalizeRanges(node->ranges);
    return node;
}

bool PatternParser::readClassAtom(EscapeValue& out) {
    if (atEnd()) {
        return refuse("`]` was expected to close a character class");
    }
    if (peek() != '\\') {
        out.code = readSourceCharacter();
        return true;
    }
    ++pos_;  // '\'
    if (atEnd()) {
        return refuse("a trailing `\\` with nothing to escape");
    }
    return readEscapeValue(out, /*inClass=*/true);
}

// ---- the two set operations' shared arithmetic ------------------------------

// 22.2.2.9 applies MaybeSimpleCaseFolding at the LEAVES — every ClassSetOperand
// that is a single character, every ClassSetRange, `\w`, `\p{...}` — and bronze
// applies it at the OPERATIONS instead. The two agree everywhere it can be
// observed, and the reason is 22.2.2.7.1: the matcher canonicalizes the input
// character AND every candidate member before comparing them, so a set that
// reaches it unfolded answers exactly as its folded twin does. Folding is
// observable only where set ALGEBRA happens — a complement, an intersection, a
// difference — because those ask which members a set HAS and not which
// characters it matches. Those three are where it is applied.
//
// One deliberate departure from the text: 22.2.2.9's
// LoneUnicodePropertyNameOrValue returns its General_Category set at step 2.a
// WITHOUT the fold that its own step 6 and the `name=value` production's step 7
// both apply. Read literally, `[^\p{Ll}]` and `[^\p{gc=Ll}]` would then disagree
// under `vi` at U+13F0, the uppercase Cherokee letter whose lowercase folds onto
// it. bronze folds both spellings, because two spellings of one property that
// mean different things is not something a source can be written to depend on.
RangeList PatternParser::foldedForOperation(const RangeList& set) const {
    if (!flags_.ignoreCase) return set;
    // MaybeSimpleCaseFolding maps every member through scf. Only the code
    // points the fold MOVES can change the answer, and the generated table
    // names exactly those — 1484 of them — so this is a walk over the table
    // and not over the alphabet.
    RangeList moved;
    RangeList arrived;
    for (uint32_t code : simpleCaseFoldSources()) {
        if (!rangesContain(set, code)) continue;
        addRange(moved, code, code);
        const uint32_t folded = simpleCaseFold(code);
        addRange(arrived, folded, folded);
    }
    if (moved.empty()) return set;
    normalizeRanges(moved);
    normalizeRanges(arrived);
    RangeList out = subtractRanges(set, moved);
    out.insert(out.end(), arrived.begin(), arrived.end());
    normalizeRanges(out);
    return out;
}

RangeList PatternParser::complementInMode(const RangeList& set) const {
    // 22.2.2.9 AllCharacters: under `v` AND `i` the alphabet a complement is
    // taken over is the code points that are their own simple case folding,
    // not the whole code space. Every other mode complements over the ceiling
    // its alphabet reaches.
    if (flags_.unicodeSets && flags_.ignoreCase) {
        return subtractRanges(simpleCaseFoldFixedPoints(), foldedForOperation(set));
    }
    return complementRanges(set, alphabetCeiling(flags_.unicodeMode()));
}

// ---- ClassSetExpression (22.2.1, +UnicodeSetsMode) --------------------------

NodePtr PatternParser::parseClassSet() {
    ++pos_;  // '['
    const bool negated = eat('^');
    RangeList set;
    if (!readClassSetExpression(set)) return nullptr;
    if (!eat(']')) return fail("`]` was expected to close a character class");

    NodePtr node = make(NodeKind::Class);
    // A `v`-mode class complements its SET and never its answer. That is the
    // whole of the difference from `[^...]` under `u`, where 22.2.2.7.1 inverts
    // the membership test after canonicalizing — and it is why this node is
    // never `negated`: the complement has already been taken, over the alphabet
    // `complementInMode` picks.
    node->negated = false;
    node->ranges = negated ? complementInMode(set) : set;
    normalizeRanges(node->ranges);
    return node;
}

bool PatternParser::readClassSetExpression(RangeList& out) {
    out.clear();
    // `[]` is an empty set and `[^]` its complement. 22.2.1 lets ClassContents
    // be empty in both grammars, and it is the one class that is legal with no
    // members at all.
    if (peek() == ']') return true;

    bool isCharacter = false;
    uint32_t code = 0;
    RangeList first;
    if (!readClassSetOperand(first, isCharacter, code)) return false;

    // Which of the three productions this is, decided by what follows the FIRST
    // operand. They do not mix: 22.2.1 gives `--` and `&&` separate productions
    // with no precedence between them, so `[[a]--[b]&&[c]]` has no parse and is
    // a syntax error rather than a guess about which binds tighter.
    if (peek() == '&' && peek(1) == '&') {
        RangeList acc = std::move(first);
        while (eatTwo('&')) {
            RangeList rhs;
            bool rhsIsCharacter = false;
            uint32_t rhsCode = 0;
            if (!readClassSetOperand(rhs, rhsIsCharacter, rhsCode)) return false;
            acc = intersectRanges(foldedForOperation(acc), foldedForOperation(rhs));
        }
        if (peek() == '-' && peek(1) == '-') {
            return refuse("`--` and `&&` cannot be mixed in one `v`-mode character class "
                          "(22.2.1 gives them separate productions and no precedence; write "
                          "the inner operation as a nested class)");
        }
        out = std::move(acc);
        return true;
    }
    if (peek() == '-' && peek(1) == '-') {
        RangeList acc = std::move(first);
        while (eatTwo('-')) {
            RangeList rhs;
            bool rhsIsCharacter = false;
            uint32_t rhsCode = 0;
            if (!readClassSetOperand(rhs, rhsIsCharacter, rhsCode)) return false;
            acc = subtractRanges(foldedForOperation(acc), foldedForOperation(rhs));
        }
        if (peek() == '&' && peek(1) == '&') {
            return refuse("`--` and `&&` cannot be mixed in one `v`-mode character class "
                          "(22.2.1 gives them separate productions and no precedence; write "
                          "the inner operation as a nested class)");
        }
        out = std::move(acc);
        return true;
    }
    return readClassSetUnion(out, std::move(first), isCharacter, code);
}

// ClassUnion: members and `a-z` ranges, one after another, until the `]`.
// A range is a ClassSetRange and so takes two ClassSetCharacters — a nested
// class or a class escape on either side is not a parse, which is the same rule
// `[a-\d]` already lives under in the other grammar.
bool PatternParser::readClassSetUnion(RangeList& out, RangeList first, bool firstIsCharacter,
                                      uint32_t firstCode) {
    out.clear();
    RangeList pending = std::move(first);
    bool isCharacter = firstIsCharacter;
    uint32_t code = firstCode;

    while (true) {
        // `-` between two characters is a range; `--` is the difference
        // operator and has already been taken above, so it cannot be here.
        if (isCharacter && peek() == '-' && peek(1) != '-' && peek(1) != ']') {
            ++pos_;
            uint32_t hi = 0;
            if (peek() == '\\') {
                ++pos_;
                if (atEnd()) return refuse("a trailing `\\` with nothing to escape");
                EscapeValue value;
                if (!readEscapeValue(value, /*inClass=*/true)) return false;
                if (value.isSet) {
                    return refuse("a character class escape cannot be the end of a range");
                }
                hi = value.code;
            } else if (!readClassSetCharacter(hi)) {
                return false;
            }
            if (code > hi) {
                return refuse("a character class range whose start is after its end");
            }
            addRange(out, code, hi);
        } else {
            out.insert(out.end(), pending.begin(), pending.end());
        }
        if (atEnd() || peek() == ']') break;
        pending.clear();
        isCharacter = false;
        code = 0;
        if (!readClassSetOperand(pending, isCharacter, code)) return false;
    }
    normalizeRanges(out);
    return true;
}

// ClassSetOperand: a nested class, a class escape, or one character. The
// `\q{...}` form — a set whose members are STRINGS — is refused inside
// `readEscapeValue` together with the properties of strings, because they are
// one feature and half of it would be a set that sometimes matches two
// characters.
bool PatternParser::readClassSetOperand(RangeList& out, bool& isCharacter, uint32_t& code) {
    out.clear();
    isCharacter = false;
    code = 0;
    if (atEnd()) return refuse("`]` was expected to close a character class");

    if (peek() == '[') {
        NodePtr nested = parseClassSet();
        if (!nested) return false;
        out = std::move(nested->ranges);
        return true;
    }
    if (peek() == '\\') {
        ++pos_;
        if (atEnd()) return refuse("a trailing `\\` with nothing to escape");
        EscapeValue value;
        if (!readEscapeValue(value, /*inClass=*/true)) return false;
        if (value.isSet) {
            out = std::move(value.set);
            return true;
        }
        isCharacter = true;
        code = value.code;
        addRange(out, code, code);
        return true;
    }
    if (!readClassSetCharacter(code)) return false;
    isCharacter = true;
    addRange(out, code, code);
    return true;
}

// ClassSetCharacter: one SourceCharacter that is neither a
// ClassSetSyntaxCharacter nor the first half of a reserved double punctuator.
// Both refusals are the point of the `v` grammar rather than an inconvenience
// of it: every character it holds back is one a later edition can give a
// meaning to, and a pattern that spelled one bare would change meaning under
// the reader that has it.
bool PatternParser::readClassSetCharacter(uint32_t& out) {
    if (atEnd()) return refuse("`]` was expected to close a character class");
    const uint16_t c = peek();
    if (isClassSetSyntaxCharacter(c)) {
        return refuse("`" + asText(c) +
                      "` must be escaped inside a `v`-mode character class (22.2.1 reserves "
                      "`( ) [ ] { } / - \\ |` there, so that a set operator spelled with one "
                      "cannot be read as the character)");
    }
    if (isClassSetDoubledPunctuator(c) && peek(1) == c) {
        return refuse("`" + asText(c) + asText(c) +
                      "` is a reserved double punctuator inside a `v`-mode character class "
                      "(22.2.1 holds every doubled punctuator for a set operator; write `\\" +
                      asText(c) + "\\" + asText(c) + "` for the two characters)");
    }
    out = readSourceCharacter();
    return true;
}

}  // namespace bronze::regex
