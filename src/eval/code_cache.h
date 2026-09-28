#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "il/il.h"
#include "modules/modules.h"
#include "support/source.h"

// The on-disk code cache behind EvalOptions::codeCacheDir.
//
// WHAT IS STORED. One file per compiled program (`<key>.bzc`), holding the
// program's IL as lowering finished it (il/serialize.h) and what that IL was
// built from: the name and a 128-bit digest of every source file the module
// graph read, plus the graph's non-textual dependencies (modules.h,
// DependencyLog). A hit skips parsing, inference and lowering — the front end,
// nearly all of a cold compile — and hands the IL to the tiered engine, which
// still lowers function bodies to MIR lazily, one at a time, on first call.
//
// THE KEY. Everything else a compile reads, hashed: the IL format version, the
// runtime's ABI fingerprint, the identity of the binary doing the compiling
// (its path, size and modification time, so any rebuild of the compiler is a
// new key space), the entry's name and text, the entry-resolution path, the
// module roots, the module-registry flags and the externals it names, the host
// globals, the natives manifest, the pins text, the census path, whether
// function source is retained, and every `BRONZE_*` environment variable (the
// lowering's switches), minus BRONZE_TIMINGS. The tier is NOT in the key: the
// IL is the same at every tier.
//
// SAFETY. A file is used only when its header, key, length and payload checksum
// all match and every recorded source file still hashes to its digest and every
// dependency still resolves as recorded; anything else — no file, a key
// collision, a truncated or damaged file, an edited source — is a miss, and the
// program compiles from source and replaces the entry. Writes go to a temporary
// file renamed into place, so a reader never sees half an entry.
//
// BOUND. After each store the directory is trimmed to the byte limit by
// deleting the least recently used entries (a hit refreshes its entry's
// modification time).
namespace bronze::eval::cache {

struct Digest {
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool operator==(const Digest& o) const { return lo == o.lo && hi == o.hi; }
    bool operator!=(const Digest& o) const { return !(*this == o); }
    std::string hex() const;
};

Digest hashBytes(std::string_view bytes);

// Accumulates length-prefixed fields, so no two keys run together.
class KeyBuilder {
public:
    void field(std::string_view s);
    Digest finish() const { return hashBytes(material_); }

private:
    std::string material_;
};

// The part of every key that describes this process rather than the program:
// format version, ABI fingerprint, binary identity, BRONZE_* environment.
// Empty when the binary cannot be identified — then nothing is cached.
const std::string& processKeyMaterial();

// A valid entry, ready to compile from.
struct Hit {
    std::unique_ptr<il::Module> module;
    SourceSet sources;
    std::string resName;
};

// Looks up `key`. `entryText` is the text of file 0 (the entry), which the
// caller already holds. False, with `why` set, on any miss.
bool load(const std::string& dir, const Digest& key, std::string_view entryText,
          const std::vector<modules::ModuleRoot>& moduleRoots, bool retainSource, Hit& out, std::string& why);

// Writes the entry for `key` and trims the directory to `maxBytes`. Failures
// are silent: a cache that cannot be written is a cache that misses.
void store(const std::string& dir, uint64_t maxBytes, const Digest& key, const std::string& resName,
           const SourceSet& sources, const modules::DependencyLog& deps, const il::Module& module);

}  // namespace bronze::eval::cache
