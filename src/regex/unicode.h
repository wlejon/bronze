#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "regex/chars.h"

// The Unicode data `src/regex` reads, and the only place the generated tables
// are consulted from.
//
// It is separate from `chars.cpp` because the two carry different kinds of
// knowledge and are checked differently. `chars.cpp` writes the case mappings
// bronze uses WITHOUT `u` as rules plus their exceptions, so a reader can
// audit them without a data file; everything here comes out of the UCD by way
// of `tools/gen_unicode_tables` and is audited by regenerating it. Mixing
// the two would leave a file where half the lines can be reasoned about and
// half have to be trusted.

namespace bronze::regex {

// 22.2.1 UnicodePropertyValueExpression: the set `\p{...}` denotes.
//
// `name` is the text before `=`, and is EMPTY for the lone form `\p{Lu}` —
// which 22.2.1 reads as a General_Category value or a binary property name,
// and never as a Script. Returns false and fills `error` with the body of a
// diagnostic (the caller adds the position) for a property bronze does not
// carry as well as for one that does not exist. Both are syntax errors: a
// property escape that quietly matched nothing would be a pattern silently
// meaning something other than what it says.
bool unicodePropertySet(std::string_view name, std::string_view value, RangeList& out,
                        std::string& error);

// The General_Category of one code point, as its two-letter alias. This is the
// generated table's lookup itself — a binary search over the runs — exposed so
// a test can walk the whole code space against the sets `unicodePropertySet`
// builds and prove the two never disagree.
std::string_view generalCategoryOf(uint32_t code);

// Simple case folding: CaseFolding.txt statuses C and S, which is what
// 22.2.2.9 Canonicalize applies when `u` and `i` are both set. Identity for
// every code point the table does not name, which includes every uncased one
// and — deliberately — U+0130 and U+0131, whose folds are status T and belong
// to the Turkic table ECMA-262 does not use.
uint32_t simpleCaseFold(uint32_t code);

// Every code point whose simple case folding is `folded` and is not `folded`
// itself: the reverse direction 22.2.2.7.1 needs, since it asks whether the
// SET holds a member canonicalizing to the input's canonicalization rather
// than whether the input's canonicalization is in the set.
const std::vector<uint32_t>& simpleCaseFoldCandidates(uint32_t folded);

// Every code point the fold does NOT leave alone, ascending. It is exactly the
// set over which `simpleCaseFold` can differ from the identity, so a search
// for "the code points whose canonicalization is something else" can walk this
// instead of all 1114112 of them and reach the same answer.
const std::vector<uint32_t>& simpleCaseFoldSources();

}  // namespace bronze::regex
