# Unicode Character Database (UCD) 16.0.0

Vendored UCD data files used by `tools/gen_unicode_tables` to generate the
Unicode tables bronze carries. Two modules are served, because folding and
conversion are different operations over different data:

| Generated file | Module | What it holds |
|---|---|---|
| `src/regex/unicode_data.h` | regex | declarations for the three below |
| `src/regex/unicode_data_gc.cpp` | regex | General_Category runs, for `\p{...}` |
| `src/regex/unicode_data_scf.cpp` | regex | simple case FOLDING, for `u`+`i` canonicalization |
| `src/regex/unicode_data_script.cpp` | regex | Script runs, script name aliases and the Script_Extensions overrides, for `\p{Script=...}` / `\p{scx=...}` |
| `src/regex/unicode_data_binary.h` | regex | declarations for the binary property tables below |
| `src/regex/unicode_data_binary.cpp` | regex | the binary properties of ECMA-262 Table 69 and every spelling of each (canonical name and alias), for `\p{Alphabetic}` / `\p{Alpha}` |
| `src/regex/unicode_data_binary_N.cpp` | regex | each binary property's code points as merged ranges, packed into files of under 1000 lines |
| `src/runtime/unicode_case_data.h` | runtime | declarations for the two below |
| `src/runtime/unicode_case_data_simple.cpp` | runtime | 1:1 case mappings |
| `src/runtime/unicode_case_data_full.cpp` | runtime | 1:many case mappings, plus `Cased` / `Case_Ignorable` |
| `src/runtime/unicode_norm_data.h` | runtime | declarations and quick-check bounds for the one below |
| `src/runtime/unicode_norm_data.cpp` | runtime | normalization (UAX #15): combining class runs, one-level canonical and compatibility decomposition streams, the explicit composition exclusions |

## Vendored Files

| File | Upstream URL | SHA-256 Checksum | Size (bytes) |
|---|---|---|---|
| `UnicodeData.txt` | `https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt` | `ff58e5823bd095166564a006e47d111130813dcf8bf234ef79fa51a870edb48f` | 2,175,362 |
| `CaseFolding.txt` | `https://www.unicode.org/Public/16.0.0/ucd/CaseFolding.txt` | `6f1f9c588eb4a5c718d9e8f93b782685e5c7fec872cf05e8e6878053599e09bb` | 86,092 |
| `SpecialCasing.txt` | `https://www.unicode.org/Public/16.0.0/ucd/SpecialCasing.txt` | `8d5de354eef79f2395a54c9c7dcebbaf3d30fc962d0f85611ea97aa973a0c451` | 16,809 |
| `DerivedCoreProperties.txt` | `https://www.unicode.org/Public/16.0.0/ucd/DerivedCoreProperties.txt` | `39d35161f2954497f69e08bdb9e701493f476a3d30222de20028feda36c1dabd` | 1,115,959 |
| `Scripts.txt` | `https://www.unicode.org/Public/16.0.0/ucd/Scripts.txt` | `9e88f0a677df47311106340be8ede2ecdacd9c1c931831218d2be6d5508e0039` | 189,588 |
| `ScriptExtensions.txt` | `https://www.unicode.org/Public/16.0.0/ucd/ScriptExtensions.txt` | `049117ce26b9769fe2749b06eef51a50a89faef4a97764dd2d81daa715980700` | 20,576 |
| `PropertyValueAliases.txt` | `https://www.unicode.org/Public/16.0.0/ucd/PropertyValueAliases.txt` | `440fd3e5460b9bfe31da67b6f923992e1989d31fe2ed91e091c4b8f8e2620bf9` | 80,773 |
| `CompositionExclusions.txt` | `https://www.unicode.org/Public/16.0.0/ucd/CompositionExclusions.txt` | `89e83cf9cc8bef6c1f8bf77e42cf6f0341dfa42e66261f4dbe9b492e7a23c8ee` | 9,007 |
| `PropList.txt` | `https://www.unicode.org/Public/16.0.0/ucd/PropList.txt` | `53d614508e2a0b2305a8aa21cd60d993de9326cdf65993660dfcce4503548583` | 144,253 |
| `emoji-data.txt` | `https://www.unicode.org/Public/16.0.0/ucd/emoji/emoji-data.txt` | `f1365a5173eee18e1f98b240cdc492e84a25f1ce7e0c9d1094eb29c41a22696a` | 113,024 |
| `DerivedBinaryProperties.txt` | `https://www.unicode.org/Public/16.0.0/ucd/extracted/DerivedBinaryProperties.txt` | `1ed4d4791c0e34fcad4dfcb5dd4c4122e16428241b75452f05d780ffa661f89d` | 17,476 |
| `DerivedNormalizationProps.txt` | `https://www.unicode.org/Public/16.0.0/ucd/DerivedNormalizationProps.txt` | `4d4c03892dea9146d674b686e495df2d55a28d071ac474041d73518f887abddc` | 1,372,269 |

`SpecialCasing.txt` is what makes case conversion FULL rather than simple:
`UnicodeData.txt` carries only the mappings that are one code point to one, so
without this file `"ß".toUpperCase()` would answer `"ß"` where ECMA-262 11.1.3
says `"SS"`. Its locale-tailored lines (the ones marked `lt`, `tr`, `az`) are
excluded by Default Case Conversion itself and the generator drops them; the one
language-independent condition in the file, `Final_Sigma`, is context rather
than data and is applied by `src/runtime/unicode_case.cpp`.

The runtime reads `DerivedCoreProperties.txt` for two properties, `Cased` and
`Case_Ignorable`, because SpecialCasing's own definition of `Final_Sigma` is
written in terms of them.

The regex module's binary properties are the rows of ECMA-262 Table 69
("Binary Unicode property aliases and their canonical property names"), and
that list, with each property's alias, is written out in
`tools/gen_unicode_binary.cpp` because it is the specification's and not the
UCD's: 22.2.1 accepts exactly those spellings, so `\p{Hyphen}`, a UAX #44
binary property the table does not list, is a syntax error. Each property's
code points come from the file that defines it: `PropList.txt` (`White_Space`,
`Ideographic`, `Dash` and the other PropList properties),
`DerivedCoreProperties.txt` (`Alphabetic`, `Lowercase`, `ID_Start`, the
`Changes_When_*` casing properties, `Grapheme_Base` and the rest),
`emoji-data.txt` (`Emoji`, `Emoji_Presentation`, `Extended_Pictographic` and the
modifier and component properties), `DerivedBinaryProperties.txt`
(`Bidi_Mirrored`) and `DerivedNormalizationProps.txt`
(`Changes_When_NFKC_Casefolded`). `Any`, `ASCII` and `Assigned` need no file and
are answered in `src/regex/unicode.cpp`.

`Scripts.txt` and `ScriptExtensions.txt` are the two halves of UAX #24, and the
second is vendored as the OVERRIDE LIST it is: its own header states that every
code point it does not list has its `Script` value as its `Script_Extensions`,
so the generator derives `scx` from `sc` plus these exceptions rather than
emitting a second full table that could drift from the first.
`PropertyValueAliases.txt` is read for its `sc` rows alone, and it is what makes
`\p{Script=Greek}` and `\p{sc=Grek}` one set: ECMA-262 22.2.1 matches a property
value exactly against the names and aliases that file lists, so the spellings
bronze accepts come from the UCD rather than from a list written out by hand.

Normalization reads `UnicodeData.txt` fields 3 (Canonical_Combining_Class)
and 5 (the decomposition mapping, whose `<tag>` marks it compatibility) and
`CompositionExclusions.txt`'s data lines: the script-specific and
post-composition-version exclusions. The file's singleton and non-starter
sections are published commented out because both follow from
`UnicodeData.txt`, and `src/runtime/unicode_norm.cpp` derives them, so
Full_Composition_Exclusion is the explicit list plus those two clauses, as
UAX #15 defines it. The emitted mappings are one level deep; the full
decomposition, the composition pairs and Hangul are computed at run time.
`NormalizationTest.txt` (2.8 MB) is not vendored; checked against it once,
the implementation passes every line and the invariance of every code point
the file does not list.

## Provenance and Usage

These files are published by the Unicode Consortium for Unicode Standard Version 16.0.0.
They are parsed directly by the C++ tool `tools/gen_unicode_tables` (compiled via CMake).
The build of Bronze never invokes the generator; generated tables are checked in directly.
To regenerate the tables from these vendored UCD files, run:

```bash
# Build the generator target
cmake --build --preset dev --target gen_unicode_tables

# Run the generator (from the repository root)
./build/dev/tools/gen_unicode_tables
```

The generator writes into `src/regex/` and `src/runtime/`, and its structural
checks are what make a rerun meaningful: a wrong field index, a UCD release that
moves a mapping, or a new language-independent casing condition stops it rather
than producing a table that looks plausible.
