#include "cli/link_order.h"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

#include "cli/link.h"
#include "support/diagnostics.h"

namespace bronze::cli {
namespace {

std::optional<uint64_t> s_seed;
std::string s_keptObjectDir;

// splitmix64. A named, fixed generator rather than anything from <random>,
// whose engines are portable but whose distributions are not: the order has to
// be a function of the seed and the count on every machine, or two runs of a
// sweep are not comparable and a reported spread means nothing.
uint64_t nextRandom(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// The objects a `--keep-objs` directory holds, in emission order. Emission
// order is lexicographic order because `retainObjects` renamed them that way.
std::vector<std::string> objectsIn(const std::filesystem::path& dir, std::error_code& ec) {
    std::vector<std::string> found;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return {};
        if (!entry.is_regular_file()) continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".obj" && ext != ".o") continue;
        found.push_back(entry.path().string());
    }
    std::sort(found.begin(), found.end());
    return found;
}

std::string keptObjectName(size_t index, const std::string& ext) {
    // Zero padded to three digits: the partition cap is 16, and padding is
    // what makes the lexicographic sort in `objectsIn` agree with the index.
    char digits[8];
    std::snprintf(digits, sizeof(digits), "%03zu", index);
    return std::string("part") + digits + ext;
}

// Everything a link diagnoses, as one message, for the callers that have no
// SourceSet to render spans against — a link from a directory of objects has
// no source text in the process at all.
std::string collectDiagnostics(const DiagnosticSink& diags) {
    std::string out;
    for (const auto& d : diags.all()) {
        if (d.severity != Severity::Error) continue;
        out += "error: " + d.message + "\n";
    }
    return out;
}

// The value of `--name <v>` or `--name=<v>`, advancing `i` past a spaced one.
// Nothing, and `error` set, when the value is missing — an empty seed or an
// empty directory is a mistake, never a default.
std::optional<std::string> flagValue(const std::string& arg, const std::string& name, int& i,
                                     int argc, char** argv, std::string& error) {
    if (arg == name) {
        if (i + 1 >= argc) {
            error = "error: missing argument for " + name + "\n";
            return std::nullopt;
        }
        return std::string(argv[++i]);
    }
    std::string value = arg.substr(name.size() + 1);
    if (value.empty()) {
        error = "error: missing argument for " + name + "\n";
        return std::nullopt;
    }
    return value;
}

// Digits only, in range. Hand-rolled rather than `stoull` because that one
// accepts leading signs and whitespace and stops at the first junk character,
// so `--link-seed 3q` would silently mean 3 and two sweeps would collide on
// one arm.
bool parseUnsigned(const std::string& text, uint64_t& out) {
    if (text.empty()) return false;
    uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool consumeLinkSeedFlag(const std::string& arg, int& i, int argc, char** argv,
                         std::string& error) {
    error.clear();
    if (arg != "--link-seed" && arg.rfind("--link-seed=", 0) != 0) return false;
    const std::optional<std::string> text = flagValue(arg, "--link-seed", i, argc, argv, error);
    if (!text) return true;
    uint64_t seed = 0;
    if (!parseUnsigned(*text, seed)) {
        error = "error: --link-seed expects an unsigned 64-bit integer, got " + *text + "\n";
        return true;
    }
    setLinkSeed(seed);
    return true;
}

}  // namespace

void setLinkSeed(std::optional<uint64_t> seed) { s_seed = seed; }

std::optional<uint64_t> linkSeed() { return s_seed; }

std::vector<std::string> orderForLink(const std::vector<std::string>& objPaths) {
    if (!s_seed || objPaths.size() < 2) return objPaths;
    std::vector<std::string> ordered = objPaths;
    uint64_t state = *s_seed;
    for (size_t i = ordered.size() - 1; i > 0; --i) {
        const size_t j = static_cast<size_t>(nextRandom(state) % (i + 1));
        std::swap(ordered[i], ordered[j]);
    }
    return ordered;
}

void setKeptObjectDir(const std::string& dir) { s_keptObjectDir = dir; }

const std::string& keptObjectDir() { return s_keptObjectDir; }

std::string retainObjects(const std::vector<std::string>& objPaths) {
    if (s_keptObjectDir.empty()) return {};
    const std::filesystem::path dir(s_keptObjectDir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec)) {
        return "error: --keep-objs: cannot create directory " + s_keptObjectDir + "\n";
    }

    // A directory left over from a build that emitted MORE partitions would
    // otherwise contribute its extras to the next link, which resolves and
    // runs and is a different program. Clearing is the only safe reading of
    // "these are the objects of that build".
    for (const std::string& stale : objectsIn(dir, ec)) {
        std::filesystem::remove(stale, ec);
    }

    for (size_t i = 0; i < objPaths.size(); ++i) {
        const std::filesystem::path src(objPaths[i]);
        const std::filesystem::path dst = dir / keptObjectName(i, src.extension().string());
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing,
                                   ec);
        if (ec) {
            return "error: --keep-objs: cannot copy " + objPaths[i] + " to " + dst.string() +
                   ": " + ec.message() + "\n";
        }
    }
    return {};
}

int linkFromObjectDir(const std::string& objDir, const std::string& outputPath,
                      std::string* errOut) {
    auto fail = [&](const std::string& message) {
        if (errOut) *errOut = message;
        else std::fputs(message.c_str(), stderr);
        return 1;
    };

    std::error_code ec;
    const std::filesystem::path dir(objDir);
    if (!std::filesystem::is_directory(dir, ec)) {
        return fail("error: link: " + objDir + " is not a directory\n");
    }
    const std::vector<std::string> objects = objectsIn(dir, ec);
    if (ec) return fail("error: link: cannot read " + objDir + ": " + ec.message() + "\n");
    if (objects.empty()) {
        return fail("error: link: no object files (.obj/.o) in " + objDir +
                    ". A `bronze build --keep-objs <dir>` fills one.\n");
    }

    DiagnosticSink diags;
    if (!linkExecutable(objects, outputPath, diags)) {
        const std::string rendered = collectDiagnostics(diags);
        return fail(rendered.empty() ? std::string("error: linking failed\n") : rendered);
    }
    return 0;
}

bool consumeLinkMeasurementFlag(const std::string& arg, int& i, int argc, char** argv,
                                std::string& error) {
    if (consumeLinkSeedFlag(arg, i, argc, argv, error)) return true;
    if (arg == "--keep-objs" || arg.rfind("--keep-objs=", 0) == 0) {
        const std::optional<std::string> dir =
            flagValue(arg, "--keep-objs", i, argc, argv, error);
        if (dir) setKeptObjectDir(*dir);
        return true;
    }
    return false;
}

int runLink(int argc, char** argv) {
    std::string objDir;
    std::string outputPath;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string error;
        if (consumeLinkSeedFlag(arg, i, argc, argv, error)) {
            if (!error.empty()) {
                std::fputs(error.c_str(), stderr);
                return 1;
            }
        } else if (arg == "-o") {
            if (i + 1 >= argc) {
                std::fputs("error: missing argument for -o\n", stderr);
                return 1;
            }
            outputPath = argv[++i];
        } else if (objDir.empty()) {
            objDir = arg;
        } else {
            std::fputs(("error: unexpected argument " + arg + "\n").c_str(), stderr);
            return 1;
        }
    }
    if (objDir.empty()) {
        std::fputs("error: missing <objdir>\n", stderr);
        return 1;
    }
    if (outputPath.empty()) {
        std::fputs("error: link: -o <output> is required\n", stderr);
        return 1;
    }
    return linkFromObjectDir(objDir, outputPath, nullptr);
}

}  // namespace bronze::cli
