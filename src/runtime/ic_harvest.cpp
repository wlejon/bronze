#include "runtime/ic_harvest.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>

#include "abi/bronze_abi.h"
#include "abi/bronze_feedback_site.h"
#include "runtime/ic_hot.h"
#include "runtime/object.h"
#include "runtime/shape.h"
#include "runtime/type_feedback.h"

namespace bronze::runtime {

namespace {

// One thing a cache held: how the site's key was found (a property way's
// form, or a method way's), the receiver it was latched for, and for a method
// site the callee (`*other*` when the entry does not say: an own slot holds a
// per-instance function, and a native or a build without function source
// names no source).
struct Fact {
    std::string form;
    std::string receiver;
    std::string callee;
    bool operator<(const Fact& o) const {
        return std::tie(form, receiver, callee) < std::tie(o.form, o.receiver, o.callee);
    }
};

struct Site {
    bool method = false;
    // A property read whose four ways were all in use: it met at least four
    // shapes, and the stub's table answered the rest.
    bool full = false;
    // The stub's table answered the site (a method site, or a write, which
    // keeps one way): more shapes than its ways.
    bool mega = false;
    std::set<Fact> facts;
};

struct Module {
    uint64_t digest = 0;
    uint32_t sites = 0;
    std::string path;
    std::map<uint32_t, Site> ics;
};

struct State {
    std::vector<Module> modules;
    std::unordered_map<const Shape*, std::string> shapeNames;
    std::unordered_map<const Shape*, std::string> protoNames;
    bool atexitArmed = false;
    bool dirty = false;
};

std::mutex g_mu;
State* g_state = nullptr;  // leaked on purpose: the atexit writer may run after statics die
std::atomic<bool> g_armed{false};

struct Identity {
    uint32_t flags = 0;
    uint32_t sites = 0;
    uint64_t digest = 0;
    std::string path;
};

bool readIdentity(const void* record, Identity& out) {
    if (record == nullptr) return false;
    const auto* bytes = static_cast<const uint8_t*>(record);
    uint32_t version = 0, pathLen = 0;
    std::memcpy(&version, bytes, 4);
    if (version != BRONZE_ABI_IC_IDENTITY_VERSION) return false;
    std::memcpy(&out.flags, bytes + BRONZE_ABI_IC_IDENTITY_FLAGS_OFFSET, 4);
    std::memcpy(&out.sites, bytes + BRONZE_ABI_IC_IDENTITY_SITES_OFFSET, 4);
    std::memcpy(&pathLen, bytes + BRONZE_ABI_IC_IDENTITY_PATHLEN_OFFSET, 4);
    std::memcpy(&out.digest, bytes + BRONZE_ABI_IC_IDENTITY_DIGEST_OFFSET, 8);
    out.path.assign(reinterpret_cast<const char*>(bytes + BRONZE_ABI_IC_IDENTITY_PATH_OFFSET), pathLen);
    return true;
}

Module* moduleOf(State& s, uint64_t digest, uint32_t sites) {
    for (Module& m : s.modules) {
        if (m.digest == digest && m.sites == sites) return &m;
    }
    return nullptr;
}

const std::string& shapeName(State& s, const Shape* shape) {
    auto it = s.shapeNames.find(shape);
    if (it == s.shapeNames.end()) it = s.shapeNames.emplace(shape, typeFeedbackShapeName(shape, s.protoNames)).first;
    return it->second;
}

std::string calleeOf(const void* code) {
    std::string name = typeFeedbackCodeName(code);
    return name.empty() ? std::string("*other*") : name;
}

// A property site: its ways, one fact each, in the forms the read and write
// paths fill (object.h InlineCache).
void foldProperty(State& s, Site& site, const uint64_t* words) {
    uint32_t used = 0;
    for (uint32_t w = 0; w < BRONZE_ABI_IC_WAYS; ++w) {
        const auto* way = reinterpret_cast<const InlineCache*>(words + w * (BRONZE_ABI_IC_ENTRY_SIZE / 8));
        if (way->cached_shape == nullptr) continue;
        ++used;
        if (way->isArrayMethod()) {
            site.facts.insert(Fact{"array", "array", {}});
            continue;
        }
        const char* form = way->isAbsent() ? "absent" : way->isAccessor() ? "accessor" : way->realDepth() != 0 ? "proto" : "own";
        site.facts.insert(Fact{form, shapeName(s, way->cached_shape), {}});
    }
    if (used == BRONZE_ABI_IC_WAYS) site.full = true;
    // A write keeps way 0 only, and word 11 is its count of the table's
    // answers after a way-0 miss; a read's word 11 is way 3's epoch, which a
    // read whose way 3 was never filled leaves zero.
    const auto* way3 = reinterpret_cast<const InlineCache*>(words + 3 * (BRONZE_ABI_IC_ENTRY_SIZE / 8));
    if (way3->cached_shape == nullptr && words[BRONZE_ABI_IC_SET_MEGA_COUNT_WORD] != 0) site.mega = true;
}

// One way of a method site (bronze_abi_layout.h, "the METHOD-CALL site"):
// `shapeWord`, `codeWord`, `arityWord` are its words 0, 1, 2 (or 6, 7, 8).
void foldMethodWay(State& s, Site& site, uint64_t shapeWord, uint64_t codeWord, uint64_t arityWord) {
    if (shapeWord == 0) return;
    const bool slotForm = (arityWord >> BRONZE_ABI_METHOD_IC_SLOT_SHIFT) != 0;
    if (shapeWord & BRONZE_ABI_METHOD_IC_EXOTIC_BIT) {
        const uint32_t kind = static_cast<uint32_t>(shapeWord) >> BRONZE_ABI_METHOD_IC_KIND_SHIFT;
        // A kind at or above the first value tag is a primitive receiver's
        // tag: a string, whose methods are String.prototype's.
        if (kind >= 0xFFF0u) {
            site.facts.insert(Fact{"native", "string", "native:string"});
            return;
        }
        const std::string word = typeFeedbackKindWord(static_cast<uint16_t>(kind));
        site.facts.insert(Fact{"native", word, "native:" + word});
        return;
    }
    const std::string& receiver = shapeName(s, reinterpret_cast<const Shape*>(shapeWord));
    if (slotForm) {
        site.facts.insert(Fact{"slot", receiver, "*other*"});
        return;
    }
    site.facts.insert(Fact{"direct", receiver, calleeOf(reinterpret_cast<const void*>(codeWord))});
}

// Appends `in` to `out`, escaped. Most names need nothing escaped and are
// appended whole: a harvest of a large program is a megabyte of names.
void appendEscaped(std::string& out, const std::string& in) {
    if (in.find_first_of("\\\t\n\r") == std::string::npos) {
        out += in;
        return;
    }
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
}

std::string textOf(const Module& m) {
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(m.digest));
    std::string out = "bronze-type-feedback " + std::to_string(BRONZE_FEEDBACK_FORMAT_VERSION) + "\n";
    out += "harvest\t" + std::string(hex) + "\t" + std::to_string(m.sites) + "\n";
    for (const auto& [index, site] : m.ics) {
        if (site.facts.empty()) continue;
        std::string flags;
        if (site.full) flags = "full";
        if (site.mega) flags += flags.empty() ? "mega" : ",mega";
        out += "ic\t" + std::to_string(index) + "\t" + (site.method ? "method" : "property") + "\t" +
               (flags.empty() ? "-" : flags) + "\n";
        for (const Fact& f : site.facts) {
            out += "way\t";
            out += std::to_string(index);
            out += '\t';
            out += f.form;
            out += '\t';
            appendEscaped(out, f.receiver);
            if (site.method) {
                out += '\t';
                appendEscaped(out, f.callee);
            }
            out += '\n';
        }
    }
    return out;
}

bool writeFile(const std::string& path, const std::string& text) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "bronze: inline-cache harvest: cannot write '%s'\n", path.c_str());
        return false;
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

// What is already folded, as it stands: a program that ended without
// reaching rtIcHarvestWriteAll (a host's exit, a thrown top level) still
// leaves what its dropped instances held. Reads no heap.
void writeAtExit() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr || !g_state->dirty) return;
    for (const Module& m : g_state->modules) {
        if (!m.path.empty()) writeFile(m.path, textOf(m));
    }
    g_state->dirty = false;
}

}  // namespace

bool rtIcHarvestArmed() noexcept { return g_armed.load(std::memory_order_relaxed); }

bool rtIcHarvestConfigure(const void* identity, const std::string& path) {
    Identity id;
    if (!readIdentity(identity, id) || (id.flags & BRONZE_ABI_IC_IDENTITY_PROFILED) != 0) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr) g_state = new State();
    State& s = *g_state;
    Module* m = moduleOf(s, id.digest, id.sites);
    if (m == nullptr) {
        if (path.empty()) return true;
        s.modules.push_back(Module{id.digest, id.sites, path, {}});
        m = &s.modules.back();
    }
    m->path = path;
    if (!path.empty() && !s.atexitArmed) {
        s.atexitArmed = true;
        std::atexit(writeAtExit);
    }
    bool any = false;
    for (const Module& each : s.modules) any = any || !each.path.empty();
    g_armed.store(any, std::memory_order_relaxed);
    return true;
}

void rtIcHarvestNoteIdentity(const uint64_t* identity) {
    Identity id;
    if (!readIdentity(identity, id) || id.path.empty()) return;
    rtIcHarvestConfigure(identity, id.path);
}

void rtIcHarvestFold(const uint64_t* table, uint64_t siteCount, const uint64_t* identity,
                     const std::vector<uint64_t>* methodSites) {
    if (!rtIcHarvestArmed() || table == nullptr) return;
    Identity id;
    if (!readIdentity(identity, id) || id.sites != siteCount) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr) return;
    State& s = *g_state;
    Module* m = moduleOf(s, id.digest, id.sites);
    if (m == nullptr || m->path.empty()) return;
    constexpr uint64_t kWords = BRONZE_ABI_IC_SITE_SIZE / 8;
    for (uint64_t i = 0; i < siteCount; ++i) {
        const uint64_t* words = table + i * kWords;
        const bool method =
            methodSites != nullptr && std::binary_search(methodSites->begin(), methodSites->end(), i);
        if (words[0] == 0 && (method || words[BRONZE_ABI_IC_SET_MEGA_COUNT_WORD] == 0)) {
            // Nothing latched at way 0; a read's later ways fill only after
            // way 0 (move to front), and a method's way 1 only by displacing
            // a way 0.
            continue;
        }
        Site& site = m->ics[static_cast<uint32_t>(i)];
        site.method = method;
        if (method) {
            foldMethodWay(s, site, words[0], words[BRONZE_ABI_METHOD_IC_CODE_WORD],
                          words[BRONZE_ABI_METHOD_IC_ARITY_WORD]);
            foldMethodWay(s, site, words[BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD],
                          words[BRONZE_ABI_METHOD_IC_WAY1_CODE_WORD], words[BRONZE_ABI_METHOD_IC_WAY1_ARITY_WORD]);
            if (words[BRONZE_ABI_METHOD_IC_MEGA_COUNT_WORD] != 0) site.mega = true;
        } else {
            foldProperty(s, site, words);
        }
        s.dirty = true;
    }
}

int rtIcHarvestWriteAll() {
    if (!rtIcHarvestArmed()) return 0;
    rtIcHotFoldLiveRanges();
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr) return 0;
    int written = 0;
    for (const Module& m : g_state->modules) {
        if (!m.path.empty() && writeFile(m.path, textOf(m))) ++written;
    }
    g_state->dirty = false;
    return written;
}

std::string rtIcHarvestText(const void* identity) {
    Identity id;
    if (!readIdentity(identity, id)) return {};
    rtIcHotFoldLiveRanges();
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr) return {};
    const Module* m = moduleOf(*g_state, id.digest, id.sites);
    return m == nullptr || m->path.empty() ? std::string() : textOf(*m);
}

}  // namespace bronze::runtime
