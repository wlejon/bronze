// The pattern grammar of ECMA-262 22.2.1, by recursive descent: Disjunction
// over Alternative over Term over Atom, with the quantifier read as a suffix
// of the term it repeats.
//
// Two things about this grammar are worth stating once, because both shape
// everything below. First, it is read TWICE: `\1` means a backreference only
// when group 1 exists, and group 1 may be written after it, so a pre-scan
// counts the groups and collects their names before the real parse begins.
// Second, its escapes are not JavaScript's — `\d`, `\b` and `\k<name>` have
// no string-literal meaning at all, and `\n` means the same thing in both only
// by coincidence — which is the whole reason this module does not share a
// character reader with `src/lex`.
//
// What is NOT here is everything between `[` and `]`. A character class is its
// own set of productions, and the `v` flag gives it a second set; both live in
// `parser_class.cpp`, and `parser_internal.h` says why the seam falls there.

#include <algorithm>
#include <string>

#include "regex/parser_internal.h"
#include "regex/pattern.h"
#include "regex/unicode.h"

namespace bronze::regex {

// Counts `(` that opens a capture and reads `(?<name>`. Deliberately not a
// parse: it only needs to know which parentheses capture, so it tracks escapes
// and class brackets and nothing else. Getting this wrong in either direction
// is visible — an uncounted group turns a backreference into an error, and an
// overcounted one turns an error into a backreference — so the two readers
// agree on exactly one thing: what `\` and `[` do.
//
// A `v`-mode class may NEST, so the bracket depth is counted rather than
// flagged: `[[a]]` closes at the second `]` and a pre-scan that stopped at the
// first would read the rest of the class as pattern text.
GroupInfo prescanGroups(UnitsView src) {
    GroupInfo info;
    uint32_t classDepth = 0;
    for (size_t i = 0; i < src.size(); ++i) {
        const uint16_t c = src[i];
        if (c == '\\') {
            ++i;  // whatever follows is escaped, brackets included
            continue;
        }
        if (classDepth != 0) {
            if (c == '[') ++classDepth;
            if (c == ']') --classDepth;
            continue;
        }
        if (c == '[') {
            classDepth = 1;
            continue;
        }
        if (c != '(') continue;
        if (i + 1 < src.size() && src[i + 1] == '?') {
            // `(?<` is a named group unless the third character makes it a
            // lookbehind, which is the one place `?<` does not capture.
            if (i + 2 < src.size() && src[i + 2] == '<' && i + 3 < src.size() &&
                src[i + 3] != '=' && src[i + 3] != '!') {
                size_t j = i + 3;
                std::string name;
                while (j < src.size() && src[j] != '>') {
                    name.push_back(static_cast<char>(src[j]));
                    ++j;
                }
                ++info.count;
                info.names.push_back(name);
            }
            continue;
        }
        ++info.count;
        info.names.push_back(std::string());
    }
    return info;
}

NodePtr PatternParser::parse(std::string& error) {
    NodePtr root = parseDisjunction();
    if (!root) {
        error = error_;
        return nullptr;
    }
    // Every parser consumes all its input or says so. The only way to be
    // left here is a `)` with no `(`, which the group parser cannot see
    // from the inside.
    if (pos_ < src_.size()) {
        error = describe("unmatched ')'");
        return nullptr;
    }
    return root;
}

// ---- Disjunction / Alternative / Term ---------------------------------------

NodePtr PatternParser::parseDisjunction() {
    NodePtr alt = parseAlternative();
    if (!alt) return nullptr;
    if (peek() != '|') return alt;
    NodePtr node = make(NodeKind::Alternation);
    node->children.push_back(std::move(alt));
    while (eat('|')) {
        NodePtr next = parseAlternative();
        if (!next) return nullptr;
        node->children.push_back(std::move(next));
    }
    return node;
}

NodePtr PatternParser::parseAlternative() {
    NodePtr node = make(NodeKind::Sequence);
    while (!atEnd() && peek() != '|' && peek() != ')') {
        NodePtr term = parseTerm();
        if (!term) return nullptr;
        node->children.push_back(std::move(term));
    }
    return node;
}

NodePtr PatternParser::parseTerm() {
    const uint32_t capturesBefore = captureCounter_;
    NodePtr atom = parseAtom();
    if (!atom) return nullptr;

    uint32_t min = 0;
    uint32_t max = 0;
    bool greedy = true;
    if (!readQuantifier(min, max, greedy)) {
        if (!error_.empty()) return nullptr;
        return atom;
    }
    // 22.2.1's Term production has no `Assertion Quantifier`: `^*` is a
    // syntax error, and a lookahead is quantifiable only under Annex B,
    // which bronze does not take (a quantified lookahead consumes nothing
    // and its only effect is on captures). A lookbehind is not quantifiable
    // even there.
    if (atom->kind == NodeKind::Assertion || atom->kind == NodeKind::Lookahead ||
        atom->kind == NodeKind::Lookbehind) {
        return fail("a quantifier applied to an assertion");
    }
    if (min > max) return fail("a quantifier whose minimum exceeds its maximum");

    NodePtr node = make(NodeKind::Repeat);
    node->min = min;
    node->max = max;
    node->greedy = greedy;
    node->firstCapture = capturesBefore + 1;
    node->captureCount = captureCounter_ - capturesBefore;
    // "Simple" means the atom consumes exactly one character whenever it
    // matches, which is what lets `matchSimpleRepeat` count matches in a loop
    // instead of recursing. A `v`-mode class with a member that is not one
    // character is precisely the Class that breaks that: `/[\q{ab|a}]*/v` has to
    // be able to take two characters at one step and to give them back.
    const bool classOfCharacters = atom->kind == NodeKind::Class && atom->strings.empty() &&
                                   !atom->matchesEmpty;
    node->simpleAtom = node->captureCount == 0 &&
                       (atom->kind == NodeKind::Char || classOfCharacters ||
                        atom->kind == NodeKind::Dot);
    node->children.push_back(std::move(atom));
    return node;
}

// Returns false with no error when the next character does not start a
// quantifier at all — including a `{` that is not a valid one, which
// Annex B B.1.2 makes an ordinary pattern character and which real code
// relies on (`/\d{/` is a legal pattern matching "1{").
bool PatternParser::readQuantifier(uint32_t& min, uint32_t& max, bool& greedy) {
    if (eat('*')) {
        min = 0;
        max = kUnbounded;
    } else if (eat('+')) {
        min = 1;
        max = kUnbounded;
    } else if (eat('?')) {
        min = 0;
        max = 1;
    } else if (peek() == '{') {
        size_t at = pos_ + 1;
        uint32_t lo = 0;
        if (!readBoundedDigits(at, lo)) return false;
        uint32_t hi = lo;
        if (at < src_.size() && src_[at] == ',') {
            ++at;
            if (at < src_.size() && src_[at] == '}') {
                hi = kUnbounded;
            } else if (!readBoundedDigits(at, hi)) {
                return false;
            }
        }
        if (at >= src_.size() || src_[at] != '}') return false;
        pos_ = at + 1;
        min = lo;
        max = hi;
    } else {
        return false;
    }
    greedy = !eat('?');
    return true;
}

// A repetition count. Capped rather than wrapped: `a{4294967296}` would
// otherwise become `a{0}` and match the empty string, which is the exact
// shape of silent wrong answer this project rules out.
bool PatternParser::readBoundedDigits(size_t& at, uint32_t& out) {
    const size_t begin = at;
    uint64_t value = 0;
    while (at < src_.size() && isDecimalDigit(src_[at])) {
        value = value * 10 + static_cast<uint64_t>(src_[at] - '0');
        if (value > 0xFFFFFFFEull) value = 0xFFFFFFFEull;
        ++at;
    }
    if (at == begin) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

// ---- Atom -------------------------------------------------------------------

NodePtr PatternParser::parseAtom() {
    if (atEnd()) return fail("an atom was expected");
    const uint16_t c = peek();
    switch (c) {
        case '^':
        case '$': {
            ++pos_;
            NodePtr node = make(NodeKind::Assertion);
            node->assertion = c == '^' ? AssertionKind::Start : AssertionKind::End;
            return node;
        }
        case '.': {
            ++pos_;
            return make(NodeKind::Dot);
        }
        case '(':
            return parseGroup();
        case '[':
            return parseCharacterClass();
        case '\\':
            return parseAtomEscape();
        case '*':
        case '+':
        case '?':
            return fail("a quantifier with nothing to repeat");
        case ')':
            return fail("an atom was expected");
        // The three characters Annex B B.1.2 lets a pattern write bare and
        // 22.2.1's PatternCharacter does not. They reach here only after
        // `readQuantifier` has already declined the `{`, so an incomplete
        // quantifier — `/a{2/u` — is diagnosed here too rather than read as
        // three literal characters.
        case ']':
            if (!flags_.unicodeMode()) break;
            return fail("a lone `]`, which the `u` and `v` flags make a syntax error "
                        "(write `\\]` for the character)");
        case '{':
            if (!flags_.unicodeMode()) break;
            return fail("a `{` that does not begin a quantifier, which the `u` and `v` flags "
                        "make a syntax error (write `\\{` for the character)");
        case '}':
            if (!flags_.unicodeMode()) break;
            return fail("a lone `}`, which the `u` and `v` flags make a syntax error "
                        "(write `\\}` for the character)");
        default:
            break;
    }
    return charNode(readSourceCharacter());
}

NodePtr PatternParser::charNode(uint32_t code) {
    // The uppercase table's holes are refused only in the mode that reads
    // it. Under `u` or `v` with `i` the fold is generated from the UCD and has
    // none, so asking here would refuse patterns bronze answers exactly.
    if (flags_.ignoreCase && !flags_.unicodeMode() && isUnknownCasedUnit(code)) {
        return caseTableFailure(code);
    }
    NodePtr node = make(NodeKind::Char);
    node->ch = canonicalize(code, flags_.ignoreCase, flags_.unicodeMode());
    return node;
}

std::nullptr_t PatternParser::caseTableFailure(uint32_t unit) {
    char buf[8];
    static const char* kHex = "0123456789ABCDEF";
    for (int i = 0; i < 4; ++i) buf[3 - i] = kHex[(unit >> (i * 4)) & 0xF];
    buf[4] = '\0';
    return fail("unsupported: case-insensitive matching of U+" + std::string(buf) +
                " without the `u` flag (only ASCII, Latin-1, Latin Extended-A, Greek, "
                "Cyrillic and Armenian fold under `i` alone; adding `u` switches "
                "22.2.2.9 to simple case folding, which bronze carries in full)");
}

NodePtr PatternParser::parseGroup() {
    ++pos_;  // '('
    uint32_t captureIndex = 0;
    std::string name;
    // Which production this `(` opens. `(?<` is the ambiguous prefix —
    // only the character after it separates a lookbehind from a named
    // group — and it is the one thing the pre-scan has to agree with,
    // since a lookbehind does not capture and a named group does.
    NodeKind kind = NodeKind::Group;
    bool negative = false;

    if (eat('?')) {
        if (eat(':')) {
            // non-capturing
        } else if (eat('=')) {
            kind = NodeKind::Lookahead;
        } else if (eat('!')) {
            kind = NodeKind::Lookahead;
            negative = true;
        } else if (peek() == '<' && (peek(1) == '=' || peek(1) == '!')) {
            negative = peek(1) == '!';
            pos_ += 2;
            kind = NodeKind::Lookbehind;
        } else if (eat('<')) {
            const size_t begin = pos_;
            if (atEnd() || !isIdentifierStart(peek())) {
                return fail("a capture group name must start with a letter, `_` or `$`");
            }
            while (!atEnd() && isIdentifierPart(peek())) ++pos_;
            name = asciiOf(src_.substr(begin, pos_ - begin));
            if (!eat('>')) return fail("`>` was expected to close a capture group name");
            captureIndex = ++captureCounter_;
        } else {
            return fail("an unsupported group modifier after `(?`");
        }
    } else {
        captureIndex = ++captureCounter_;
    }

    NodePtr body = parseDisjunction();
    if (!body) return nullptr;
    if (!eat(')')) return fail("`)` was expected to close a group");

    NodePtr node = make(kind);
    node->lookaroundNegative = negative;
    node->captureIndex = captureIndex;
    node->children.push_back(std::move(body));
    return node;
}

// ---- Escapes ----------------------------------------------------------------

NodePtr PatternParser::parseAtomEscape() {
    ++pos_;  // '\'
    if (atEnd()) return fail("a trailing `\\` with nothing to escape");
    const uint16_t c = peek();

    if (c == 'b' || c == 'B') {
        ++pos_;
        NodePtr node = make(NodeKind::Assertion);
        node->assertion = c == 'b' ? AssertionKind::WordBoundary
                                   : AssertionKind::NotWordBoundary;
        return node;
    }
    if (c == 'k') {
        ++pos_;
        if (!eat('<')) return fail("`<` was expected after `\\k`");
        const size_t begin = pos_;
        while (!atEnd() && isIdentifierPart(peek())) ++pos_;
        const std::string name = asciiOf(src_.substr(begin, pos_ - begin));
        if (!eat('>')) return fail("`>` was expected to close `\\k<`");
        for (size_t i = 0; i < groups_.names.size(); ++i) {
            if (groups_.names[i] != name) continue;
            NodePtr node = make(NodeKind::Backreference);
            node->backreference = static_cast<uint32_t>(i + 1);
            return node;
        }
        return fail("`\\k<" + name + ">` names no capture group in this pattern");
    }
    // A DecimalEscape. `\0` is NUL and is handled with the other character
    // escapes; `\1`..`\9` is a backreference, and a number past the group
    // count is refused rather than read as Annex B's legacy octal escape —
    // `/\1/` matching a control character is a wrong answer given quietly.
    if (c >= '1' && c <= '9') {
        size_t at = pos_;
        uint64_t value = 0;
        while (at < src_.size() && isDecimalDigit(src_[at])) {
            value = value * 10 + static_cast<uint64_t>(src_[at] - '0');
            if (value > 0xFFFFFFull) break;
            ++at;
        }
        if (value > groups_.count) {
            return fail("`\\" + std::to_string(value) + "` refers to capture group " +
                        std::to_string(value) + ", and this pattern has " +
                        std::to_string(groups_.count) +
                        " (legacy octal escapes are not implemented)");
        }
        pos_ = at;
        NodePtr node = make(NodeKind::Backreference);
        node->backreference = static_cast<uint32_t>(value);
        return node;
    }

    EscapeValue value;
    if (!readEscapeValue(value, /*inClass=*/false)) return nullptr;
    if (value.isSet) {
        NodePtr node = make(NodeKind::Class);
        node->ranges = std::move(value.set);
        normalizeRanges(node->ranges);
        return node;
    }
    return charNode(value.code);
}

// The escapes that denote a character or a class set, shared by the atom
// position and the inside of a class. `inClass` is what makes `\b` a
// backspace here and a word boundary there — the one production whose
// meaning depends on where it is written.
bool PatternParser::readEscapeValue(EscapeValue& out, bool inClass) {
    const uint16_t c = peek();
    switch (c) {
        case 'd': ++pos_; out.isSet = true; out.set = digitRanges(); return true;
        case 'D':
            ++pos_;
            out.isSet = true;
            out.set = complementInMode(digitRanges());
            return true;
        case 's': ++pos_; out.isSet = true; out.set = spaceRanges(); return true;
        case 'S':
            ++pos_;
            out.isSet = true;
            out.set = complementInMode(spaceRanges());
            return true;
        case 'w':
            ++pos_;
            out.isSet = true;
            out.set = wordRanges(flags_.ignoreCase, flags_.unicodeMode());
            return true;
        case 'W':
            ++pos_;
            out.isSet = true;
            out.set = complementInMode(wordRanges(flags_.ignoreCase, flags_.unicodeMode()));
            return true;
        case 'p':
        case 'P':
            return readPropertyEscape(out, /*negated=*/c == 'P');
        case 'q':
            // ClassStringDisjunction exists only inside a `v`-mode class.
            // Anywhere else `\q` is an ordinary alphanumeric identity escape,
            // and refusing it here would answer the wrong question about
            // `/\q/`, which has never been legal for its own reason.
            if (!flags_.unicodeSets || !inClass) break;
            // `readClassSetOperand` takes `\q` before ever calling this, so
            // reaching here means a ClassStringDisjunction was written somewhere
            // an OPERAND is not — as the far end of a range (`[a-\q{b}]`), or
            // inside another `\q{...}`. 22.2.1 has no production for either, so
            // this is a syntax error about position and not a refusal of the
            // feature.
            return refuse("`\\q{...}` can only stand where a `v`-mode class expects a whole "
                          "operand (22.2.1's ClassSetOperand) — it is a set of strings, so it "
                          "cannot be one end of a range or a member of another `\\q{...}`");
        case 'f': ++pos_; out.code = 0x000C; return true;
        case 'n': ++pos_; out.code = 0x000A; return true;
        case 'r': ++pos_; out.code = 0x000D; return true;
        case 't': ++pos_; out.code = 0x0009; return true;
        case 'v': ++pos_; out.code = 0x000B; return true;
        case '0':
            ++pos_;
            if (!atEnd() && isDecimalDigit(peek())) {
                return refuse("a legacy octal escape is not implemented");
            }
            out.code = 0;
            return true;
        case 'c': {
            if (pos_ + 1 >= src_.size()) {
                return refuse("a control letter was expected after `\\c`");
            }
            const uint16_t letter = src_[pos_ + 1];
            const bool ok = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z');
            if (!ok) {
                return refuse("a control letter was expected after `\\c`");
            }
            pos_ += 2;
            out.code = static_cast<uint32_t>(letter % 32);
            return true;
        }
        case 'x': {
            uint32_t v = 0;
            if (!readHex(pos_ + 1, 2, v)) {
                return refuse("two hexadecimal digits were expected after `\\x`");
            }
            pos_ += 3;
            out.code = v;
            return true;
        }
        case 'u':
            return readUnicodeEscape(out);
        case 'b':
            // Only reachable inside a class: the atom position took `\b`
            // as a word boundary before it got here.
            ++pos_;
            out.code = 0x0008;
            return true;
        default:
            break;
    }
    // IdentityEscape. Annex B lets a `\` precede almost any character, but
    // an ALPHANUMERIC one is refused: `\q` reading as `q` is how a pattern
    // meant for another engine's extension silently matches the wrong
    // thing. Every escape bronze implements is named above.
    if (isAlphanumeric(c)) {
        return refuse("`\\" + std::string(1, static_cast<char>(c)) +
                      "` is not an escape sequence bronze implements");
    }
    // Under `v` a class escape may also precede a ClassSetReservedPunctuator,
    // which is what makes `[\&]` and `[\!]` writable where the bare character
    // is reserved for an operator. Outside a class the `v` grammar is `u`'s.
    if (flags_.unicodeSets && inClass && isClassSetReservedPunctuator(c)) {
        ++pos_;
        out.code = c;
        return true;
    }
    // Under `u` the Annex B widening is gone entirely: IdentityEscape takes
    // a SyntaxCharacter or `/`, and ClassEscape adds `-` inside a class so
    // that `[\-]` stays writable. `/\-/u` is a syntax error where `/\-/` is
    // a hyphen, which is the leniency switching off rather than a new rule.
    if (flags_.unicodeMode() && !isSyntaxCharacter(c) && c != '/' && !(inClass && c == '-')) {
        return refuse("`\\" + std::string(1, static_cast<char>(c)) +
                      "` is not a valid escape under the `u` flag (an identity escape there may "
                      "only precede a syntax character or `/`)");
    }
    ++pos_;
    out.code = c;
    return true;
}

// 22.2.1's `p{ UnicodePropertyValueExpression }`, and its negated twin.
//
// The production exists only under +UnicodeMode, and that is not a detail
// bronze can be lenient about: without `u`, Annex B would read `\p{L}` as
// the letter `p` quantified, so a pattern meant to match letters would
// match the letter p. Refused by name there rather than reinterpreted.
//
// The expression itself is two productions that look alike — `name=value`
// and a lone name-or-value — and they mean different things: the lone form
// may be a General_Category VALUE or a binary property, and may never be a
// Script. `unicodePropertySet` is where that is decided, because it is
// where the tables are; here the job is only to read the text exactly and
// hand on whatever a refusal says.
bool PatternParser::readPropertyEscape(EscapeValue& out, bool negated) {
    if (!flags_.unicodeMode()) {
        return refuse("unsupported: unicode property escapes `\\p{...}` are a +UnicodeMode "
                      "production and this pattern has no `u` flag (bronze does not implement "
                      "Annex B's reading of `\\p` as the letter `p`)");
    }
    ++pos_;  // `p` or `P`
    if (!eat('{')) {
        return refuse("`{` was expected after `\\p`, which begins a unicode property escape");
    }
    const size_t firstBegin = pos_;
    while (!atEnd() && isPropertyCharacter(peek())) ++pos_;
    std::string first = asciiOf(src_.substr(firstBegin, pos_ - firstBegin));
    std::string name;
    std::string value = first;
    if (eat('=')) {
        name = first;
        const size_t valueBegin = pos_;
        while (!atEnd() && isPropertyCharacter(peek())) ++pos_;
        value = asciiOf(src_.substr(valueBegin, pos_ - valueBegin));
    }
    if (!eat('}')) {
        return refuse("`}` was expected to close a unicode property escape");
    }
    if (value.empty()) {
        return refuse("a unicode property escape names no property");
    }
    RangeList set;
    std::string why;
    if (!unicodePropertySet(name, value, set, why)) {
        return refuse(why);
    }
    out.isSet = true;
    // `\P` complements over the CODE POINT ceiling, not the code unit one.
    // `u` or `v` is required to be here at all, so the ceiling is never in
    // doubt — and a `\P{L}` that stopped at U+FFFF would silently exclude every
    // astral code point from a set whose whole meaning is "not a letter".
    out.set = negated ? complementInMode(set) : std::move(set);
    return true;
}

// 22.2.1 RegExpUnicodeEscapeSequence. Three productions, and which of them
// is available is the `u`/`v` flag's business: `\uHHHH` always, a LeadSurrogate
// escape immediately followed by a TrailSurrogate escape as ONE code point
// there, and `\u{...}` there alone.
bool PatternParser::readUnicodeEscape(EscapeValue& out) {
    if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '{') {
        if (!flags_.unicodeMode()) {
            // Annex B keeps this legal without `u` — `/\u{2}/` is `\u`
            // quantified — and bronze has always refused it rather than
            // implementing that reading. Refused still, and now for the
            // reason that survives: the flag says which grammar is in play.
            return refuse("unsupported: `\\u{...}` is a code point escape only under the `u` "
                          "flag; without it, `\\u` takes exactly four hexadecimal digits and "
                          "bronze does not implement Annex B's quantified reading");
        }
        size_t at = pos_ + 2;
        uint64_t value = 0;
        size_t digits = 0;
        uint32_t digit = 0;
        while (at < src_.size() && hexDigitValue(src_[at], digit)) {
            value = value * 16 + digit;
            // Capped rather than wrapped, so an absurd escape is refused
            // below as out of range instead of becoming some other
            // character entirely.
            if (value > kMaxCodePoint) value = kMaxCodePoint + 1;
            ++at;
            ++digits;
        }
        if (digits == 0 || at >= src_.size() || src_[at] != '}') {
            return refuse("`\\u{` must be closed by `}` around at least one hexadecimal digit");
        }
        if (value > kMaxCodePoint) {
            return refuse("`\\u{...}` names a value above U+10FFFF, which is not a code point");
        }
        pos_ = at + 1;
        out.code = static_cast<uint32_t>(value);
        return true;
    }
    uint32_t v = 0;
    if (!readHex(pos_ + 1, 4, v)) {
        return refuse("four hexadecimal digits were expected after `\\u`");
    }
    pos_ += 5;
    if (flags_.unicodeMode() && isLeadSurrogate(v) && pos_ + 1 < src_.size() &&
        src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
        uint32_t trail = 0;
        if (readHex(pos_ + 2, 4, trail) && isTrailSurrogate(trail)) {
            pos_ += 6;
            v = 0x10000 + ((v - 0xD800) << 10) + (trail - 0xDC00);
        }
    }
    out.code = v;
    return true;
}

bool PatternParser::readHex(size_t at, size_t count, uint32_t& out) {
    if (at + count > src_.size()) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint16_t d = src_[at + i];
        uint32_t digit = 0;
        if (!hexDigitValue(d, digit)) return false;
        value = (value << 4) | digit;
    }
    out = value;
    return true;
}

// ---- flags and the compiled pattern -----------------------------------------

std::string Flags::text() const {
    // 22.2.6.5's order, which is also the order `toString` prints and the
    // order `source`/`flags` round-trips through: d g i m s u v y.
    std::string out;
    if (hasIndices) out += 'd';
    if (global) out += 'g';
    if (ignoreCase) out += 'i';
    if (multiline) out += 'm';
    if (dotAll) out += 's';
    if (unicode) out += 'u';
    if (unicodeSets) out += 'v';
    if (sticky) out += 'y';
    return out;
}

bool parseFlags(std::string_view text, Flags& out, std::string& error) {
    out = Flags{};
    for (char c : text) {
        bool* slot = nullptr;
        switch (c) {
            case 'd': slot = &out.hasIndices; break;
            case 'g': slot = &out.global; break;
            case 'i': slot = &out.ignoreCase; break;
            case 'm': slot = &out.multiline; break;
            case 's': slot = &out.dotAll; break;
            case 'u': slot = &out.unicode; break;
            case 'v': slot = &out.unicodeSets; break;
            case 'y': slot = &out.sticky; break;
            default:
                error = std::string("Invalid regular expression flags: `") + c + "`";
                return false;
        }
        if (*slot) {
            error = std::string("Invalid regular expression flags: `") + c +
                    "` appears more than once";
            return false;
        }
        *slot = true;
    }
    // `u` and `v` together is a SyntaxError, and the specification puts it in
    // an odd place: the flag LETTERS are both legal (22.2.3.3 step 5 accepts
    // either), and the conflict is caught one level down in 22.2.3.4
    // ParsePattern step 1, which produces a SyntaxError list rather than
    // choosing a grammar. They are two readings of one mode and not two
    // independent bits — `v` is `u` plus a second class grammar — so a pattern
    // that asked for both has asked which of two incompatible grammars its
    // classes are in. Diagnosed here because bronze parses flags before the
    // pattern and the answer does not depend on the pattern.
    if (out.unicode && out.unicodeSets) {
        error = "Invalid regular expression flags: `u` and `v` cannot both be set (22.2.3.4 "
                "makes them alternatives — `v` already implies the code point alphabet `u` "
                "gives)";
        return false;
    }
    // `u` and `i` together were refused here until bronze carried a second
    // case table, and the refusal is gone rather than relaxed: 22.2.2.9 step 1
    // canonicalizes by simple case folding under both flags, `chars.cpp` picks
    // that table on exactly that condition, and the table is generated from
    // the UCD instead of written from memory. Nothing here has to know which
    // table is which — which is why there is no longer a rule at this level.
    return true;
}

void PatternDeleter::operator()(Pattern* pattern) const noexcept { delete pattern; }

PatternPtr compile(UnitsView source, const Flags& flags, std::string& error) {
    const GroupInfo groups = prescanGroups(source);
    PatternParser parser(source, flags, groups);
    NodePtr root = parser.parse(error);
    if (!root) return nullptr;
    // The pre-scan and the parse must agree about which parentheses capture.
    // They read the source with different rules, so a disagreement is a bug in
    // one of them and never something a program did — and a silent one would
    // renumber every backreference in the pattern.
    if (parser.capturesSeen() != groups.count) {
        error = "internal: the capture-group pre-scan and the parser disagree (" +
                std::to_string(groups.count) + " vs " + std::to_string(parser.capturesSeen()) + ")";
        return nullptr;
    }

    PatternPtr pattern(new Pattern());
    pattern->root = std::move(root);
    pattern->flags = flags;
    pattern->groupCount = groups.count;
    pattern->groupNames = groups.names;
    pattern->hasNamedGroups =
        std::any_of(groups.names.begin(), groups.names.end(),
                    [](const std::string& n) { return !n.empty(); });
    return pattern;
}

const Flags& patternFlags(const Pattern& pattern) { return pattern.flags; }
uint32_t captureCount(const Pattern& pattern) { return pattern.groupCount; }
bool hasNamedGroups(const Pattern& pattern) { return pattern.hasNamedGroups; }

const std::string& groupName(const Pattern& pattern, uint32_t index) {
    static const std::string empty;
    if (index == 0 || index > pattern.groupNames.size()) return empty;
    return pattern.groupNames[index - 1];
}

}  // namespace bronze::regex
