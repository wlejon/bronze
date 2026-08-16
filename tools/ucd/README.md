# Unicode Character Database (UCD) 16.0.0

Vendored UCD data files used by `tools/gen_unicode_tables` to generate the
Unicode tables bronze carries. Two modules are served, because folding and
conversion are different operations over different data:

| Generated file | Module | What it holds |
|---|---|---|
| `src/regex/unicode_data.h` | regex | declarations for the two below |
| `src/regex/unicode_data_gc.cpp` | regex | General_Category runs, for `\p{...}` |
| `src/regex/unicode_data_scf.cpp` | regex | simple case FOLDING, for `u`+`i` canonicalization |
| `src/runtime/unicode_case_data.h` | runtime | declarations for the two below |
| `src/runtime/unicode_case_data_simple.cpp` | runtime | 1:1 case mappings |
| `src/runtime/unicode_case_data_full.cpp` | runtime | 1:many case mappings, plus `Cased` / `Case_Ignorable` |

## Vendored Files

| File | Upstream URL | SHA-256 Checksum | Size (bytes) |
|---|---|---|---|
| `UnicodeData.txt` | `https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt` | `ff58e5823bd095166564a006e47d111130813dcf8bf234ef79fa51a870edb48f` | 2,175,362 |
| `CaseFolding.txt` | `https://www.unicode.org/Public/16.0.0/ucd/CaseFolding.txt` | `6f1f9c588eb4a5c718d9e8f93b782685e5c7fec872cf05e8e6878053599e09bb` | 86,092 |
| `SpecialCasing.txt` | `https://www.unicode.org/Public/16.0.0/ucd/SpecialCasing.txt` | `8d5de354eef79f2395a54c9c7dcebbaf3d30fc962d0f85611ea97aa973a0c451` | 16,809 |
| `DerivedCoreProperties.txt` | `https://www.unicode.org/Public/16.0.0/ucd/DerivedCoreProperties.txt` | `39d35161f2954497f69e08bdb9e701493f476a3d30222de20028feda36c1dabd` | 1,115,959 |

`SpecialCasing.txt` is what makes case conversion FULL rather than simple:
`UnicodeData.txt` carries only the mappings that are one code point to one, so
without this file `"ß".toUpperCase()` would answer `"ß"` where ECMA-262 11.1.3
says `"SS"`. Its locale-tailored lines (the ones marked `lt`, `tr`, `az`) are
excluded by Default Case Conversion itself and the generator drops them; the one
language-independent condition in the file, `Final_Sigma`, is context rather
than data and is applied by `src/runtime/unicode_case.cpp`.

`DerivedCoreProperties.txt` is read for exactly two properties, `Cased` and
`Case_Ignorable`, because SpecialCasing's own definition of `Final_Sigma` is
written in terms of them.

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
