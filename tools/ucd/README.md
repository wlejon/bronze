# Unicode Character Database (UCD) 16.0.0

Vendored UCD data files used by `tools/gen_unicode_tables` to generate `src/regex/unicode_data.h`, `src/regex/unicode_data_gc.cpp`, and `src/regex/unicode_data_scf.cpp`.

## Vendored Files

| File | Upstream URL | SHA-256 Checksum | Size (bytes) |
|---|---|---|---|
| `UnicodeData.txt` | `https://www.unicode.org/Public/16.0.0/ucd/UnicodeData.txt` | `ff58e5823bd095166564a006e47d111130813dcf8bf234ef79fa51a870edb48f` | 2,175,362 |
| `CaseFolding.txt` | `https://www.unicode.org/Public/16.0.0/ucd/CaseFolding.txt` | `6f1f9c588eb4a5c718d9e8f93b782685e5c7fec872cf05e8e6878053599e09bb` | 86,092 |

## Provenance and Usage

These files are published by the Unicode Consortium for Unicode Standard Version 16.0.0.
They are parsed directly by the C++ tool `tools/gen_unicode_tables` (compiled via CMake).
The build of Bronze never invokes the generator; generated tables are checked in directly under `src/regex/`.
To regenerate the tables from these vendored UCD files, run:

```bash
# Build the generator target
cmake --build --preset dev --target gen_unicode_tables

# Run the generator
./build/dev/tools/gen_unicode_tables
```
