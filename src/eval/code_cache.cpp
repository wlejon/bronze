#include "eval/code_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>
#include <unordered_set>

#include "il/serialize.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <dlfcn.h>
#include <unistd.h>
extern char** environ;
#endif

namespace bronze::eval::cache {

namespace {

// ---------------------------------------------------------------------------
// MurmurHash3 x64/128 (public domain, Austin Appleby): fast, and 128 bits so a
// key or a file digest colliding by accident is not a case worth handling.
// ---------------------------------------------------------------------------

inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

inline uint64_t fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

inline uint64_t load64(const unsigned char* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

constexpr uint64_t kC1 = 0x87c37b91114253d5ULL;
constexpr uint64_t kC2 = 0x4cf5ad432745937fULL;

Digest murmur3(std::string_view bytes, uint64_t seed) {
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const size_t len = bytes.size();
    const size_t nblocks = len / 16;
    uint64_t h1 = seed;
    uint64_t h2 = seed;
    for (size_t i = 0; i < nblocks; ++i) {
        uint64_t k1 = load64(data + i * 16);
        uint64_t k2 = load64(data + i * 16 + 8);
        k1 *= kC1;
        k1 = rotl64(k1, 31);
        k1 *= kC2;
        h1 ^= k1;
        h1 = rotl64(h1, 27);
        h1 += h2;
        h1 = h1 * 5 + 0x52dce729;
        k2 *= kC2;
        k2 = rotl64(k2, 33);
        k2 *= kC1;
        h2 ^= k2;
        h2 = rotl64(h2, 31);
        h2 += h1;
        h2 = h2 * 5 + 0x38495ab5;
    }
    const unsigned char* tail = data + nblocks * 16;
    const size_t rem = len & 15;
    if (rem > 8) {
        uint64_t k2 = 0;
        for (size_t i = rem; i-- > 8;) k2 = (k2 << 8) | tail[i];
        k2 *= kC2;
        k2 = rotl64(k2, 33);
        k2 *= kC1;
        h2 ^= k2;
    }
    if (rem > 0) {
        uint64_t k1 = 0;
        for (size_t i = std::min<size_t>(rem, 8); i-- > 0;) k1 = (k1 << 8) | tail[i];
        k1 *= kC1;
        k1 = rotl64(k1, 31);
        k1 *= kC2;
        h1 ^= k1;
    }
    h1 ^= len;
    h2 ^= len;
    h1 += h2;
    h2 += h1;
    h1 = fmix64(h1);
    h2 = fmix64(h2);
    h1 += h2;
    h2 += h1;
    return Digest{h1, h2};
}

// ---------------------------------------------------------------------------
// The entry file: a fixed header, then a payload the header checksums.
// ---------------------------------------------------------------------------

constexpr uint32_t kMagic = 0x315A4342u;  // "BCZ1"
// The cache's own layout, combined with the IL encoding's version.
constexpr uint32_t kCacheLayout = 1;
constexpr uint32_t kVersion = (kCacheLayout << 16) | il::kFormatVersion;
constexpr size_t kHeaderSize = 48;
constexpr const char* kExt = ".bzc";
constexpr const char* kWarmExt = ".bzw";
constexpr uint64_t kFileByDigest = 0;
constexpr uint64_t kFileEmbedded = 1;

void putU32(std::string& out, uint32_t v) {
    char b[4];
    std::memcpy(b, &v, 4);
    out.append(b, 4);
}
void putU64(std::string& out, uint64_t v) {
    char b[8];
    std::memcpy(b, &v, 8);
    out.append(b, 8);
}
void putStr(std::string& out, std::string_view s) {
    putU64(out, s.size());
    out.append(s.data(), s.size());
}

class Cursor {
public:
    explicit Cursor(std::string_view b) : b_(b) {}
    bool ok() const { return ok_; }
    uint32_t u32() {
        uint32_t v = 0;
        if (!take(4)) return 0;
        std::memcpy(&v, b_.data() + pos_ - 4, 4);
        return v;
    }
    uint64_t u64() {
        uint64_t v = 0;
        if (!take(8)) return 0;
        std::memcpy(&v, b_.data() + pos_ - 8, 8);
        return v;
    }
    std::string_view str() {
        const uint64_t n = u64();
        if (!ok_ || n > b_.size() - pos_) {
            ok_ = false;
            return {};
        }
        std::string_view s = b_.substr(pos_, static_cast<size_t>(n));
        pos_ += static_cast<size_t>(n);
        return s;
    }
    // A count whose elements take at least `minBytes` each.
    size_t count(size_t minBytes) {
        const uint64_t n = u64();
        if (!ok_ || n > (b_.size() - pos_) / minBytes) {
            ok_ = false;
            return 0;
        }
        return static_cast<size_t>(n);
    }
    bool atEnd() const { return pos_ == b_.size(); }

private:
    bool take(size_t n) {
        if (!ok_ || b_.size() - pos_ < n) {
            ok_ = false;
            return false;
        }
        pos_ += n;
        return true;
    }
    std::string_view b_;
    size_t pos_ = 0;
    bool ok_ = true;
};

bool readWholeFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamoff size = in.tellg();
    if (size < 0) return false;
    out.resize(static_cast<size_t>(size));
    in.seekg(0);
    if (size > 0 && !in.read(out.data(), size)) return false;
    return true;
}

std::filesystem::path entryFile(const std::string& dir, const Digest& key) {
    return std::filesystem::path(dir) / (key.hex() + kExt);
}

std::string binaryIdentity() {
    std::filesystem::path self;
#ifdef _WIN32
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&binaryIdentity), &module)) {
        std::wstring buf(1024, L'\0');
        for (;;) {
            const DWORD n = GetModuleFileNameW(module, buf.data(), static_cast<DWORD>(buf.size()));
            if (n == 0) break;
            if (n < buf.size()) {
                buf.resize(n);
                self = std::filesystem::path(buf);
                break;
            }
            buf.resize(buf.size() * 2);
        }
    }
#else
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&binaryIdentity), &info) && info.dli_fname) self = info.dli_fname;
#endif
    if (self.empty()) return {};
    std::error_code ec;
    const auto size = std::filesystem::file_size(self, ec);
    if (ec) return {};
    const auto mtime = std::filesystem::last_write_time(self, ec);
    if (ec) return {};
    return self.generic_string() + "|" + std::to_string(size) + "|" +
           std::to_string(mtime.time_since_epoch().count());
}

// Every BRONZE_* variable but the ones that only report: the lowering's
// switches are all spelled that way, and one that is flipped is a different
// program.
std::vector<std::string> bronzeEnvironment() {
    std::vector<std::string> vars;
    auto consider = [&](const char* kv) {
        if (std::strncmp(kv, "BRONZE_", 7) != 0) return;
        if (std::strncmp(kv, "BRONZE_TIMINGS=", 15) == 0) return;
        vars.emplace_back(kv);
    };
#ifdef _WIN32
    if (char* block = GetEnvironmentStringsA()) {
        for (const char* p = block; *p; p += std::strlen(p) + 1) consider(p);
        FreeEnvironmentStringsA(block);
    }
#else
    for (char** e = environ; e && *e; ++e) consider(*e);
#endif
    std::sort(vars.begin(), vars.end());
    return vars;
}

uint64_t processId() {
#ifdef _WIN32
    return static_cast<uint64_t>(_getpid());
#else
    return static_cast<uint64_t>(getpid());
#endif
}

void trim(const std::string& dir, uint64_t maxBytes) {
    struct Item {
        std::filesystem::path path;
        uint64_t size;
        std::filesystem::file_time_type mtime;
    };
    std::vector<Item> items;
    uint64_t total = 0;
    std::error_code ec;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code e2;
        if (!entry.is_regular_file(e2)) continue;
        const auto& p = entry.path();
        const auto mtime = entry.last_write_time(e2);
        if (e2) continue;
        const std::string name = p.filename().string();
        if (name.find(".tmp") != std::string::npos) {
            // A writer that died between write and rename.
            if (now - mtime > std::chrono::minutes(10)) std::filesystem::remove(p, e2);
            continue;
        }
        if (p.extension() == kWarmExt) {
            // A warm list outlives nothing: its entry trimmed, it goes too.
            std::filesystem::path owner = p;
            if (!std::filesystem::exists(owner.replace_extension(kExt), e2)) std::filesystem::remove(p, e2);
            continue;
        }
        if (p.extension() != kExt) continue;
        const uint64_t size = entry.file_size(e2);
        if (e2) continue;
        items.push_back({p, size, mtime});
        total += size;
    }
    if (total <= maxBytes) return;
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.mtime < b.mtime; });
    for (const auto& item : items) {
        if (total <= maxBytes) break;
        std::error_code e2;
        if (std::filesystem::remove(item.path, e2)) total -= item.size;
    }
}

}  // namespace

std::string Digest::hex() const {
    static const char* kDigits = "0123456789abcdef";
    std::string s(32, '0');
    for (int i = 0; i < 16; ++i) {
        s[15 - i] = kDigits[(hi >> (i * 4)) & 15];
        s[31 - i] = kDigits[(lo >> (i * 4)) & 15];
    }
    return s;
}

Digest hashBytes(std::string_view bytes) { return murmur3(bytes, 0x62726f6e7a65ULL); }

void KeyBuilder::field(std::string_view s) {
    material_ += std::to_string(s.size());
    material_ += ':';
    material_.append(s.data(), s.size());
}

const std::string& processKeyMaterial() {
    static const std::string material = [] {
        const std::string identity = binaryIdentity();
        if (identity.empty()) return std::string();
        std::string m = "v" + std::to_string(kVersion) + "|abi" + std::to_string(BRONZE_ABI_FINGERPRINT) + "|" +
                        identity;
        for (const auto& v : bronzeEnvironment()) m += "|" + v;
        return m;
    }();
    return material;
}

bool load(const std::string& dir, const Digest& key, std::string_view entryText,
          const std::vector<modules::ModuleRoot>& moduleRoots, bool retainSource, Hit& out, std::string& why) {
    const std::filesystem::path path = entryFile(dir, key);
    std::string bytes;
    if (!readWholeFile(path, bytes)) {
        why = "no entry";
        return false;
    }
    Cursor h(bytes);
    const uint32_t magic = h.u32();
    const uint32_t version = h.u32();
    const uint32_t abi = h.u32();
    (void)h.u32();
    const uint64_t keyLo = h.u64();
    const uint64_t keyHi = h.u64();
    const uint64_t payloadLen = h.u64();
    const uint64_t checksum = h.u64();
    if (!h.ok() || magic != kMagic) {
        why = "not a cache entry";
        return false;
    }
    if (version != kVersion || abi != BRONZE_ABI_FINGERPRINT) {
        why = "format or ABI changed";
        return false;
    }
    if (keyLo != key.lo || keyHi != key.hi) {
        why = "key mismatch";
        return false;
    }
    if (payloadLen != bytes.size() - kHeaderSize) {
        why = "truncated";
        return false;
    }
    const std::string_view payload = std::string_view(bytes).substr(kHeaderSize);
    if (hashBytes(payload).lo != checksum) {
        why = "checksum mismatch";
        return false;
    }

    Cursor c(payload);
    const std::string resName(c.str());
    struct FileRef {
        std::string_view name;
        bool embedded = false;
        Digest digest;
        std::string_view text;
    };
    std::vector<FileRef> files(c.count(24));
    for (auto& f : files) {
        f.name = c.str();
        const uint64_t kind = c.u64();
        if (kind == kFileEmbedded) {
            f.embedded = true;
            f.text = c.str();
        } else if (kind == kFileByDigest) {
            f.digest.lo = c.u64();
            f.digest.hi = c.u64();
        } else {
            why = "malformed entry";
            return false;
        }
    }
    modules::DependencyLog deps;
    deps.globs.resize(c.count(40));
    for (auto& g : deps.globs) {
        g.dirSpecifier = c.str();
        g.importer = c.str();
        g.namePrefix = c.str();
        g.tail = c.str();
        g.names.resize(c.count(8));
        for (auto& n : g.names) n = c.str();
    }
    deps.edges.resize(c.count(24));
    for (auto& e : deps.edges) {
        e.specifier = c.str();
        e.importer = c.str();
        e.target = c.str();
    }
    const std::string_view ilBytes = c.str();
    if (!c.ok() || !c.atEnd() || files.empty() || files[0].embedded) {
        why = "malformed entry";
        return false;
    }

    // Every file the graph read, re-read and compared. File 0 is the entry,
    // whose text the caller holds.
    if (hashBytes(entryText) != files[0].digest) {
        why = "entry changed";
        return false;
    }
    out.sources.add(std::string(files[0].name), std::string(entryText));
    for (size_t i = 1; i < files.size(); ++i) {
        if (files[i].embedded) {
            out.sources.add(std::string(files[i].name), std::string(files[i].text));
            continue;
        }
        std::string text;
        if (!readWholeFile(std::filesystem::path(std::string(files[i].name)), text)) {
            why = "source missing: " + std::string(files[i].name);
            return false;
        }
        if (hashBytes(text) != files[i].digest) {
            why = "source changed: " + std::string(files[i].name);
            return false;
        }
        out.sources.add(std::string(files[i].name), std::move(text));
    }
    if (!modules::dependenciesUnchanged(deps, moduleRoots)) {
        why = "module resolution changed";
        return false;
    }

    auto module = std::make_unique<il::Module>();
    std::string err;
    if (!il::deserializeModule(ilBytes, *module, err)) {
        why = err;
        return false;
    }
    if (module->sourceFiles.size() != files.size()) {
        why = "malformed entry";
        return false;
    }
    std::vector<std::string_view> texts;
    texts.reserve(out.sources.size());
    for (size_t i = 0; i < out.sources.size(); ++i) texts.push_back(out.sources.at(static_cast<uint16_t>(i)).text());
    il::attachSources(*module, texts, retainSource);
    out.module = std::move(module);
    out.resName = resName;

    // Recently used: the trim keeps it.
    std::error_code ec;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), ec);
    return true;
}

void store(const std::string& dir, uint64_t maxBytes, const Digest& key, const std::string& resName,
           const SourceSet& sources, const modules::DependencyLog& deps, const il::Module& module) {
    if (sources.empty()) return;
    std::string payload;
    putStr(payload, resName);
    // A buffer read from disk is recorded by digest and re-read on a hit; one
    // the linker synthesized is not a file, and its text is stored. The entry
    // (buffer 0) is compared against the text the caller holds.
    const std::unordered_set<std::string_view> onDisk(deps.filesRead.begin(), deps.filesRead.end());
    putU64(payload, sources.size());
    for (size_t i = 0; i < sources.size(); ++i) {
        const SourceBuffer& buf = sources.at(static_cast<uint16_t>(i));
        putStr(payload, buf.name());
        if (i == 0 || onDisk.count(buf.name())) {
            const Digest d = hashBytes(buf.text());
            putU64(payload, kFileByDigest);
            putU64(payload, d.lo);
            putU64(payload, d.hi);
        } else {
            putU64(payload, kFileEmbedded);
            putStr(payload, buf.text());
        }
    }
    putU64(payload, deps.globs.size());
    for (const auto& g : deps.globs) {
        putStr(payload, g.dirSpecifier);
        putStr(payload, g.importer);
        putStr(payload, g.namePrefix);
        putStr(payload, g.tail);
        putU64(payload, g.names.size());
        for (const auto& n : g.names) putStr(payload, n);
    }
    putU64(payload, deps.edges.size());
    for (const auto& e : deps.edges) {
        putStr(payload, e.specifier);
        putStr(payload, e.importer);
        putStr(payload, e.target);
    }
    putStr(payload, il::serializeModule(module));

    std::string header;
    putU32(header, kMagic);
    putU32(header, kVersion);
    putU32(header, BRONZE_ABI_FINGERPRINT);
    putU32(header, 0);
    putU64(header, key.lo);
    putU64(header, key.hi);
    putU64(header, payload.size());
    putU64(header, hashBytes(payload).lo);

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    static std::atomic<uint64_t> counter{0};
    const std::filesystem::path finalPath = entryFile(dir, key);
    const std::filesystem::path tmp =
        std::filesystem::path(dir) / (key.hex() + ".tmp" + std::to_string(processId()) + "_" +
                                      std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, finalPath, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return;
    }
    trim(dir, maxBytes);
}

namespace {

constexpr std::string_view kWarmHeader = "bzw1";

std::filesystem::path warmFile(const std::string& dir, const Digest& key) {
    return std::filesystem::path(dir) / (key.hex() + kWarmExt);
}

}  // namespace

std::vector<std::string> loadWarmList(const std::string& dir, const Digest& key) {
    std::vector<std::string> names;
    std::ifstream in(warmFile(dir, key), std::ios::binary);
    if (!in) return names;
    std::string line;
    if (!std::getline(in, line) || line != kWarmHeader) return names;
    while (std::getline(in, line)) {
        if (!line.empty()) names.push_back(std::move(line));
    }
    return names;
}

void storeWarmList(const std::string& dir, const Digest& key, const std::vector<std::string>& names) {
    if (names.empty() || !std::filesystem::exists(entryFile(dir, key))) return;
    std::string text(kWarmHeader);
    text += '\n';
    for (const std::string& n : names) {
        if (n.find('\n') != std::string::npos) continue;
        text += n;
        text += '\n';
    }
    static std::atomic<uint64_t> counter{0};
    const std::filesystem::path tmp =
        std::filesystem::path(dir) / (key.hex() + ".tmpw" + std::to_string(processId()) + "_" +
                                      std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, warmFile(dir, key), ec);
    if (ec) std::filesystem::remove(tmp, ec);
}

}  // namespace bronze::eval::cache
