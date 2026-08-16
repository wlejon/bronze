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

// The Script (UAX #24) of one code point, as its canonical long name, for the
// same reason and with the same use: `Unknown` for every code point Scripts.txt
// does not name, which is a real Script value and not a failure.
std::string_view scriptOf(uint32_t code);

// Does this code point's Script_Extensions set hold the script spelled `alias`?
// The property is a SET per code point rather than a value, so membership is
// the only question it answers — and `alias` is any spelling
// PropertyValueAliases.txt gives, since that is what a pattern may write.
// False for a spelling that names no script at all.
bool scriptExtensionsContain(uint32_t code, std::string_view alias);

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

// The code points the fold DOES leave alone, as ranges. 22.2.2.9's
// AllCharacters is exactly this set when `v` and `i` are both set, so it is
// the alphabet a complement is taken over there — which is what makes
// `[^\P{X}]` come back to `[\p{X}]` under `vi` where under `ui` it does not.
// Built from `simpleCaseFoldSources` and therefore its exact complement.
const RangeList& simpleCaseFoldFixedPoints();

// Every code point the fold does NOT leave alone, ascending. It is exactly the
// set over which `simpleCaseFold` can differ from the identity, so a search
// for "the code points whose canonicalization is something else" can walk this
// instead of all 1114112 of them and reach the same answer.
const std::vector<uint32_t>& simpleCaseFoldSources();

}  // namespace bronze::regex
