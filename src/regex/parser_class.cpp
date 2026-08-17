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

#include <algorithm>
#include <string>
#include <vector>

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

// ---- the string members' half of the same algebra ---------------------------
//
// 22.2.2.9's CharSet operations are set operations over SEQUENCES, so each of
// the three has to say what it does to the members that are not characters.
// Union takes both lists; intersection keeps a string both sides hold;
// difference drops a string the right side holds. The zero-length member follows
// exactly the same three rules, which is why it rides along as a flag rather
// than as an empty vector in the list — an empty `std::vector` in `strings`
// would have to be kept distinct from "no strings at all".

namespace {

using ClassString = std::vector<uint32_t>;

bool hasString(const std::vector<ClassString>& list, const ClassString& s) {
    for (const ClassString& candidate : list) {
        if (candidate == s) return true;
    }
    return false;
}

void addString(std::vector<ClassString>& list, const ClassString& s) {
    if (!hasString(list, s)) list.push_back(s);
}

// LONGEST FIRST, then by code points, which is 22.2.2.9.6's order made TOTAL.
// The specification only requires the longest match to win; ordering the ties
// makes the tree — and therefore every diagnostic and every dump of it —
// independent of the order the alternatives happened to be written in.
void sortLongestFirst(std::vector<ClassString>& list) {
    std::sort(list.begin(), list.end(), [](const ClassString& a, const ClassString& b) {
        if (a.size() != b.size()) return a.size() > b.size();
        return a < b;
    });
}

}  // namespace

// ---- ClassSetExpression (22.2.1, +UnicodeSetsMode) --------------------------

// `[ ClassSetExpression ]` or `[^ ClassSetExpression ]`, at the top of a pattern
// or nested inside another class — 22.2.1's CharacterClass and its NestedClass
// are the same two productions, and the early error below is written twice in
// the specification for exactly that reason.
bool PatternParser::readNestedClass(ClassSetValue& out) {
    out = ClassSetValue{};
    ++pos_;  // '['
    const bool negated = eat('^');
    ClassSetValue set;
    if (!readClassSetExpression(set)) return false;
    if (!eat(']')) return refuse("`]` was expected to close a character class");

    // 22.2.1's static semantics: a NEGATED class whose contents MayContainStrings
    // is an early SyntaxError. It is not an implementation limit — there is no
    // answer for it. A complement is taken over an alphabet of characters, and
    // the complement of a set containing "abc" would have to be every sequence
    // of every length that is not "abc".
    if (negated && set.mayContainStrings) {
        return refuse("a negated `v`-mode character class cannot contain a set of strings "
                      "(22.2.1 makes `[^...\\q{...}...]` an early error, because the complement "
                      "of a set of sequences is not a set of characters)");
    }

    // A `v`-mode class complements its SET and never its answer. That is the
    // whole of the difference from `[^...]` under `u`, where 22.2.2.7.1 inverts
    // the membership test after canonicalizing.
    out.ranges = negated ? complementInMode(set.ranges) : set.ranges;
    normalizeRanges(out.ranges);
    out.strings = std::move(set.strings);
    out.empty = set.empty;
    // 22.2.1 gives `[^...]` MayContainStrings false outright — it is the one
    // production that CANNOT contain strings, since the line above just refused
    // the case where its contents could. Everywhere else the answer is passed
    // through untouched, and never recomputed from `strings`: `[\q{ab}--\q{ab}]`
    // may contain strings although its value holds none, which is what makes
    // `[^[\q{ab}--\q{ab}]]` an early error rather than an empty class.
    out.mayContainStrings = negated ? false : set.mayContainStrings;
    return true;
}

NodePtr PatternParser::parseClassSet() {
    ClassSetValue set;
    if (!readNestedClass(set)) return nullptr;

    NodePtr node = make(NodeKind::Class);
    // Never `negated`: the complement has already been taken, over the alphabet
    // `complementInMode` picks.
    node->negated = false;
    node->ranges = std::move(set.ranges);
    node->strings = std::move(set.strings);
    node->matchesEmpty = set.empty;
    sortLongestFirst(node->strings);
    return node;
}

bool PatternParser::readClassSetExpression(ClassSetValue& out) {
    out = ClassSetValue{};
    // `[]` is an empty set and `[^]` its complement. 22.2.1 lets ClassContents
    // be empty in both grammars, and it is the one class that is legal with no
    // members at all.
    if (peek() == ']') return true;

    bool isCharacter = false;
    uint32_t code = 0;
    ClassSetValue first;
    if (!readClassSetOperand(first, isCharacter, code)) return false;

    // Which of the three productions this is, decided by what follows the FIRST
    // operand. They do not mix: 22.2.1 gives `--` and `&&` separate productions
    // with no precedence between them, so `[[a]--[b]&&[c]]` has no parse and is
    // a syntax error rather than a guess about which binds tighter.
    if (peek() == '&' && peek(1) == '&') {
        ClassSetValue acc = std::move(first);
        while (eatTwo('&')) {
            ClassSetValue rhs;
            bool rhsIsCharacter = false;
            uint32_t rhsCode = 0;
            if (!readClassSetOperand(rhs, rhsIsCharacter, rhsCode)) return false;
            acc.ranges = intersectRanges(foldedForOperation(acc.ranges),
                                         foldedForOperation(rhs.ranges));
            // A string survives an intersection only if BOTH sides hold it, and
            // 22.2.1's MayContainStrings for `&&` is likewise the AND of its
            // operands': an intersection with anything that cannot hold a string
            // cannot either.
            std::vector<ClassString> kept;
            for (const ClassString& s : acc.strings) {
                if (hasString(rhs.strings, s)) kept.push_back(s);
            }
            acc.strings = std::move(kept);
            acc.empty = acc.empty && rhs.empty;
            acc.mayContainStrings = acc.mayContainStrings && rhs.mayContainStrings;
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
        ClassSetValue acc = std::move(first);
        while (eatTwo('-')) {
            ClassSetValue rhs;
            bool rhsIsCharacter = false;
            uint32_t rhsCode = 0;
            if (!readClassSetOperand(rhs, rhsIsCharacter, rhsCode)) return false;
            acc.ranges = subtractRanges(foldedForOperation(acc.ranges),
                                        foldedForOperation(rhs.ranges));
            // A difference drops what the RIGHT side holds and asks nothing of
            // it, which is also why 22.2.1 takes MayContainStrings for `--` from
            // the first operand alone.
            std::vector<ClassString> kept;
            for (const ClassString& s : acc.strings) {
                if (!hasString(rhs.strings, s)) kept.push_back(s);
            }
            acc.strings = std::move(kept);
            acc.empty = acc.empty && !rhs.empty;
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
bool PatternParser::readClassSetUnion(ClassSetValue& out, ClassSetValue first,
                                      bool firstIsCharacter, uint32_t firstCode) {
    out = ClassSetValue{};
    ClassSetValue pending = std::move(first);
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
            addRange(out.ranges, code, hi);
        } else {
            out.ranges.insert(out.ranges.end(), pending.ranges.begin(), pending.ranges.end());
            for (const ClassString& s : pending.strings) addString(out.strings, s);
            out.empty = out.empty || pending.empty;
            // Union: MayContainStrings if ANY operand's does.
            out.mayContainStrings = out.mayContainStrings || pending.mayContainStrings;
        }
        if (atEnd() || peek() == ']') break;
        pending = ClassSetValue{};
        isCharacter = false;
        code = 0;
        if (!readClassSetOperand(pending, isCharacter, code)) return false;
    }
    normalizeRanges(out.ranges);
    return true;
}

// ClassSetOperand: a nested class, `\q{...}`, a class escape, or one character.
// `\q` is taken HERE rather than in `readEscapeValue`, because it is the one
// escape whose value is not an `EscapeValue`: a character or a set of characters
// cannot express "abc".
bool PatternParser::readClassSetOperand(ClassSetValue& out, bool& isCharacter, uint32_t& code) {
    out = ClassSetValue{};
    isCharacter = false;
    code = 0;
    if (atEnd()) return refuse("`]` was expected to close a character class");

    // A nested class carries both its value and 22.2.1's syntactic answer up.
    if (peek() == '[') return readNestedClass(out);
    if (peek() == '\\') {
        if (peek(1) == 'q') {
            pos_ += 2;  // `\q`
            return readClassStringDisjunction(out);
        }
        ++pos_;
        if (atEnd()) return refuse("a trailing `\\` with nothing to escape");
        EscapeValue value;
        if (!readEscapeValue(value, /*inClass=*/true)) return false;
        if (value.isSet) {
            out.ranges = std::move(value.set);
            return true;
        }
        isCharacter = true;
        code = value.code;
        addRange(out.ranges, code, code);
        return true;
    }
    if (!readClassSetCharacter(code)) return false;
    isCharacter = true;
    addRange(out.ranges, code, code);
    return true;
}

// ClassStringDisjunction (22.2.1): `{ alt | alt | ... }`, each alternative a run
// of ClassSetCharacters — so every character that must be escaped in a `v`-mode
// class must be escaped here too, and `|` and `}` are what end an alternative.
//
// An alternative of exactly ONE character is not a string: it is a member of
// `ranges` like any other character, which is what makes `[\q{a}]` and `[a]`
// the same class and lets the set operators work over both without a special
// case. Length 0 and length 2+ are the members a CharSet of characters could
// not hold.
bool PatternParser::readClassStringDisjunction(ClassSetValue& out) {
    if (!eat('{')) {
        return refuse("`{` was expected after `\\q` (22.2.1's ClassStringDisjunction is "
                      "`\\q{...}`)");
    }
    // 22.2.1 gives ClassStringDisjunction MayContainStrings when any of its
    // alternatives is not exactly one character long. Decided from the SYNTAX,
    // so `\q{ab}` sets it whatever the surrounding algebra later does.
    while (true) {
        ClassString alternative;
        while (!atEnd() && peek() != '|' && peek() != '}') {
            if (peek() == '\\') {
                ++pos_;
                if (atEnd()) return refuse("a trailing `\\` with nothing to escape");
                EscapeValue value;
                if (!readEscapeValue(value, /*inClass=*/true)) return false;
                if (value.isSet) {
                    return refuse("a character class escape cannot appear inside `\\q{...}` "
                                  "(22.2.1's ClassString is a run of single characters)");
                }
                alternative.push_back(value.code);
                continue;
            }
            uint32_t c = 0;
            if (!readClassSetCharacter(c)) return false;
            alternative.push_back(c);
        }
        if (alternative.size() == 1) {
            addRange(out.ranges, alternative[0], alternative[0]);
        } else {
            out.mayContainStrings = true;
            if (alternative.empty()) {
                out.empty = true;
            } else {
                addString(out.strings, alternative);
            }
        }
        if (eat('|')) continue;
        if (eat('}')) break;
        return refuse("`}` was expected to close `\\q{...}`");
    }
    normalizeRanges(out.ranges);
    // Under `i` a string member would have to go through 22.2.2.9's
    // MaybeSimpleCaseFolding element by element, and the matcher would have to
    // fold each input character it compares against one — which is a second
    // canonicalization path beside `classMatches`'s, and a second path is how
    // the two would come to disagree. Refused by name rather than left to fold
    // the characters and not the strings, which would match "ABC" against
    // `[\q{abc}]` only sometimes.
    //
    // The EMPTY member is exempt, and not by leniency: it has no element to fold,
    // so `[\q{|a}]` under `i` is the same set folded or not.
    if (flags_.ignoreCase && !out.strings.empty()) {
        return refuse("unsupported: `\\q{...}` with a member longer than one character, under "
                      "the `i` flag (22.2.2.9 folds each element of each member, and bronze "
                      "canonicalizes characters only)");
    }
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
