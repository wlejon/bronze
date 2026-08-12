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

#include <algorithm>
#include <string>

#include "regex/pattern.h"

namespace bronze::regex {

namespace {

constexpr uint32_t kUnbounded = 0xFFFFFFFFu;

bool isDecimalDigit(uint16_t c) { return c >= '0' && c <= '9'; }

bool isIdentifierStart(uint16_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}

bool isIdentifierPart(uint16_t c) { return isIdentifierStart(c) || isDecimalDigit(c); }

// What an IdentityEscape may NOT be. 22.2.1 lets `\` precede a SyntaxCharacter
// — `^ $ \ . * + ? ( ) [ ] { } |` — or `/`, and Annex B widens that to almost
// anything; a LETTER OR DIGIT is where the two readings stop agreeing, and a
// pattern written for another engine's `\q` extension would otherwise match a
// plain `q` here. `$` and `_` are neither, so `\$` is the ordinary dollar sign.
bool isAlphanumeric(uint16_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDecimalDigit(c);
}

// A group name is written back out in diagnostics and used as a property key,
// so it is carried as UTF-8. Names are restricted to ASCII identifiers
// (bronze's own identifier rule), which is narrower than the
// specification's IdentifierName and is refused by name rather than silently.
std::string asciiOf(UnitsView units) {
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

// Counts `(` that opens a capture and reads `(?<name>`. Deliberately not a
// parse: it only needs to know which parentheses capture, so it tracks escapes
// and class brackets and nothing else. Getting this wrong in either direction
// is visible — an uncounted group turns a backreference into an error, and an
// overcounted one turns an error into a backreference — so the two readers
// agree on exactly one thing: what `\` and `[` do.
GroupInfo prescanGroups(UnitsView src) {
    GroupInfo info;
    bool inClass = false;
    for (size_t i = 0; i < src.size(); ++i) {
        const uint16_t c = src[i];
        if (c == '\\') {
            ++i;  // whatever follows is escaped, brackets included
            continue;
        }
        if (inClass) {
            if (c == ']') inClass = false;
            continue;
        }
        if (c == '[') {
            inClass = true;
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

class PatternParser {
public:
    PatternParser(UnitsView src, const Flags& flags, const GroupInfo& groups)
        : src_(src), flags_(flags), groups_(groups) {}

    NodePtr parse(std::string& error) {
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

    std::string describe(const std::string& what) const {
        return what + " at index " + std::to_string(pos_) + " of the pattern";
    }

    std::nullptr_t fail(const std::string& what) {
        if (error_.empty()) error_ = describe(what);
        return nullptr;
    }

    NodePtr make(NodeKind kind) {
        auto n = std::make_unique<Node>();
        n->kind = kind;
        return n;
    }

    // ---- Disjunction / Alternative / Term ---------------------------------

    NodePtr parseDisjunction() {
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

    NodePtr parseAlternative() {
        NodePtr node = make(NodeKind::Sequence);
        while (!atEnd() && peek() != '|' && peek() != ')') {
            NodePtr term = parseTerm();
            if (!term) return nullptr;
            node->children.push_back(std::move(term));
        }
        return node;
    }

    NodePtr parseTerm() {
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
        node->simpleAtom = node->captureCount == 0 &&
                           (atom->kind == NodeKind::Char || atom->kind == NodeKind::Class ||
                            atom->kind == NodeKind::Dot);
        node->children.push_back(std::move(atom));
        return node;
    }

    // Returns false with no error when the next character does not start a
    // quantifier at all — including a `{` that is not a valid one, which
    // Annex B B.1.2 makes an ordinary pattern character and which real code
    // relies on (`/\d{/` is a legal pattern matching "1{").
    bool readQuantifier(uint32_t& min, uint32_t& max, bool& greedy) {
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
    bool readBoundedDigits(size_t& at, uint32_t& out) {
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

    // ---- Atom -------------------------------------------------------------

    NodePtr parseAtom() {
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
            default:
                break;
        }
        ++pos_;
        return charNode(c);
    }

    NodePtr charNode(uint16_t unit) {
        if (flags_.ignoreCase && isUnknownCasedUnit(unit)) return caseTableFailure(unit);
        NodePtr node = make(NodeKind::Char);
        node->ch = canonicalize(unit, flags_.ignoreCase);
        return node;
    }

    std::nullptr_t caseTableFailure(uint32_t unit) {
        char buf[8];
        static const char* kHex = "0123456789ABCDEF";
        for (int i = 0; i < 4; ++i) buf[3 - i] = kHex[(unit >> (i * 4)) & 0xF];
        buf[4] = '\0';
        return fail("unsupported: case-insensitive matching of U+" + std::string(buf) +
                    " (bronze carries no Unicode case tables; only ASCII, Latin-1, Latin "
                    "Extended-A, Greek, Cyrillic and Armenian fold under the `i` flag)");
    }

    NodePtr parseGroup() {
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

    // ---- Escapes ----------------------------------------------------------

    // What an escape denotes: either one code unit, or a set of them. The two
    // are one return type because ClassAtom accepts both and the atom position
    // accepts both, and splitting them would duplicate the whole escape table.
    struct EscapeValue {
        bool isSet = false;
        uint16_t unit = 0;
        RangeList set;
    };

    NodePtr parseAtomEscape() {
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
        return charNode(value.unit);
    }

    // The escapes that denote a character or a class set, shared by the atom
    // position and the inside of a class. `inClass` is what makes `\b` a
    // backspace here and a word boundary there — the one production whose
    // meaning depends on where it is written.
    bool readEscapeValue(EscapeValue& out, bool inClass) {
        const uint16_t c = peek();
        switch (c) {
            case 'd': ++pos_; out.isSet = true; out.set = digitRanges(); return true;
            case 'D': ++pos_; out.isSet = true; out.set = complementRanges(digitRanges()); return true;
            case 's': ++pos_; out.isSet = true; out.set = spaceRanges(); return true;
            case 'S': ++pos_; out.isSet = true; out.set = complementRanges(spaceRanges()); return true;
            case 'w': ++pos_; out.isSet = true; out.set = wordRanges(flags_.ignoreCase); return true;
            case 'W':
                ++pos_;
                out.isSet = true;
                out.set = complementRanges(wordRanges(flags_.ignoreCase));
                return true;
            case 'p':
            case 'P':
                fail("unsupported: unicode property escapes `\\p{...}` are not implemented "
                     "(they need the `u` flag, which bronze does not support)");
                return false;
            case 'f': ++pos_; out.unit = 0x000C; return true;
            case 'n': ++pos_; out.unit = 0x000A; return true;
            case 'r': ++pos_; out.unit = 0x000D; return true;
            case 't': ++pos_; out.unit = 0x0009; return true;
            case 'v': ++pos_; out.unit = 0x000B; return true;
            case '0':
                ++pos_;
                if (!atEnd() && isDecimalDigit(peek())) {
                    fail("a legacy octal escape is not implemented");
                    return false;
                }
                out.unit = 0;
                return true;
            case 'c': {
                if (pos_ + 1 >= src_.size()) {
                    fail("a control letter was expected after `\\c`");
                    return false;
                }
                const uint16_t letter = src_[pos_ + 1];
                const bool ok = (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z');
                if (!ok) {
                    fail("a control letter was expected after `\\c`");
                    return false;
                }
                pos_ += 2;
                out.unit = static_cast<uint16_t>(letter % 32);
                return true;
            }
            case 'x': {
                uint32_t v = 0;
                if (!readHex(pos_ + 1, 2, v)) {
                    fail("two hexadecimal digits were expected after `\\x`");
                    return false;
                }
                pos_ += 3;
                out.unit = static_cast<uint16_t>(v);
                return true;
            }
            case 'u': {
                if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '{') {
                    fail("unsupported: `\\u{...}` needs the `u` flag, which bronze "
                         "does not support");
                    return false;
                }
                uint32_t v = 0;
                if (!readHex(pos_ + 1, 4, v)) {
                    fail("four hexadecimal digits were expected after `\\u`");
                    return false;
                }
                pos_ += 5;
                out.unit = static_cast<uint16_t>(v);
                return true;
            }
            case 'b':
                // Only reachable inside a class: the atom position took `\b`
                // as a word boundary before it got here.
                ++pos_;
                out.unit = 0x0008;
                return true;
            default:
                break;
        }
        // IdentityEscape. Annex B lets a `\` precede almost any character, but
        // an ALPHANUMERIC one is refused: `\q` reading as `q` is how a pattern
        // meant for another engine's extension silently matches the wrong
        // thing. Every escape bronze implements is named above.
        if (isAlphanumeric(c)) {
            fail("`\\" + std::string(1, static_cast<char>(c)) +
                 "` is not an escape sequence bronze implements");
            return false;
        }
        (void)inClass;
        ++pos_;
        out.unit = c;
        return true;
    }

    bool readHex(size_t at, size_t count, uint32_t& out) {
        if (at + count > src_.size()) return false;
        uint32_t value = 0;
        for (size_t i = 0; i < count; ++i) {
            const uint16_t d = src_[at + i];
            uint32_t digit = 0;
            if (d >= '0' && d <= '9') digit = static_cast<uint32_t>(d - '0');
            else if (d >= 'a' && d <= 'f') digit = static_cast<uint32_t>(d - 'a' + 10);
            else if (d >= 'A' && d <= 'F') digit = static_cast<uint32_t>(d - 'A' + 10);
            else return false;
            value = (value << 4) | digit;
        }
        out = value;
        return true;
    }

    // ---- Character class --------------------------------------------------

    NodePtr parseCharacterClass() {
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
                if (lhs.unit > rhs.unit) {
                    return fail("a character class range whose start is after its end");
                }
                if (flags_.ignoreCase &&
                    (isUnknownCasedUnit(lhs.unit) || isUnknownCasedUnit(rhs.unit))) {
                    return caseTableFailure(isUnknownCasedUnit(lhs.unit) ? lhs.unit : rhs.unit);
                }
                addRange(node->ranges, lhs.unit, rhs.unit);
                continue;
            }
            if (lhs.isSet) {
                node->ranges.insert(node->ranges.end(), lhs.set.begin(), lhs.set.end());
            } else {
                if (flags_.ignoreCase && isUnknownCasedUnit(lhs.unit)) {
                    return caseTableFailure(lhs.unit);
                }
                addRange(node->ranges, lhs.unit, lhs.unit);
            }
        }
        if (!eat(']')) return fail("`]` was expected to close a character class");
        normalizeRanges(node->ranges);
        return node;
    }

    bool readClassAtom(EscapeValue& out) {
        if (atEnd()) {
            fail("`]` was expected to close a character class");
            return false;
        }
        if (peek() != '\\') {
            out.unit = src_[pos_];
            ++pos_;
            return true;
        }
        ++pos_;  // '\'
        if (atEnd()) {
            fail("a trailing `\\` with nothing to escape");
            return false;
        }
        return readEscapeValue(out, /*inClass=*/true);
    }
};

}  // namespace

std::string Flags::text() const {
    // 22.2.6.5's order, which is also the order `toString` prints and the
    // order `source`/`flags` round-trips through: d g i m s u v y, minus the
    // four bronze does not have.
    std::string out;
    if (global) out += 'g';
    if (ignoreCase) out += 'i';
    if (multiline) out += 'm';
    if (dotAll) out += 's';
    if (sticky) out += 'y';
    return out;
}

bool parseFlags(std::string_view text, Flags& out, std::string& error) {
    out = Flags{};
    for (char c : text) {
        bool* slot = nullptr;
        switch (c) {
            case 'g': slot = &out.global; break;
            case 'i': slot = &out.ignoreCase; break;
            case 'm': slot = &out.multiline; break;
            case 's': slot = &out.dotAll; break;
            case 'y': slot = &out.sticky; break;
            case 'd':
                error = "unsupported: the RegExp `d` flag (match indices) is not implemented";
                return false;
            case 'u':
                error = "unsupported: the RegExp `u` flag is not implemented (bronze matches "
                        "per UTF-16 code unit)";
                return false;
            case 'v':
                error = "unsupported: the RegExp `v` flag is not implemented";
                return false;
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
