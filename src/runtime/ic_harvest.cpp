#include "runtime/ic_harvest.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "abi/bronze_abi.h"
#include "abi/bronze_feedback_site.h"
#include "runtime/harvest_sampler.h"
#include "runtime/ic_hot.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/slot_repr.h"
#include "runtime/type_feedback.h"

namespace bronze::runtime {

namespace {

// One thing a cache held: how the site's key was found (a property way's
// form, or a method way's), the receiver it was latched for, and for a method
// site the callee (`*other*` when the entry does not name a source function,
// such as a native or a build without function source names).
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
    // The stub's table answered the site (a method site, or a property write
    // that overflowed its BRONZE_ABI_IC_SET_WAYS ways): more shapes than its ways.
    bool mega = false;
    // rtIcHarvestNoteUncached: the times the site met a receiver no way of
    // it could hold, written as the site's `other` row.
    uint64_t uncached = 0;
    std::set<Fact> facts;
};

struct Module {
    uint64_t digest = 0;
    uint32_t sites = 0;
    std::string path;
    std::map<uint32_t, Site> ics;
    // The sampler's counts (harvest_sampler.h), by the sampled function's
    // descriptor name_hash.
    std::map<uint32_t, uint64_t> samples;
};

struct State {
    std::vector<Module> modules;
    // The identity records of the modules being harvested, by address. A
    // registered table whose record is not one of these is never read: every
    // module registers its tables, a compiled-in-process one's memory goes
    // with its program, and a range it left behind names freed memory.
    std::unordered_set<const void*> records;
    std::unordered_map<const Shape*, std::string> shapeNames;
    std::unordered_map<const Shape*, std::string> protoNames;
    // The shapes named here that were not dictionaries when named: shape-tree
    // nodes, which live as long as the arena, so the heap walk at the write
    // may follow them (a dictionary shape goes with its object).
    std::unordered_set<const Shape*> plainNamed;
    // rtIcHarvestNoteBirth: per node, the kinds (kHarvestBirth*) every add
    // through the runtime that made it held.
    std::unordered_map<const Shape*, uint8_t> births;
    // Made by `measureRepresentation` at the write, once per representation
    // (Numbers, plain objects): by receiver name, the key positions (root
    // first) that were born of the kind and that every object of every shape
    // of that name held one of the kind in; and every key some named shape
    // was born or found holding anything else under.
    struct Kept {
        std::map<std::string, std::vector<uint32_t>> held;
        std::set<std::string> other;
    };
    Kept doubles;
    Kept objects;
    bool atexitArmed = false;
    bool dirty = false;
};

std::mutex g_mu;
State* g_state = nullptr;  // leaked on purpose: the atexit writer may run after statics die
std::atomic<bool> g_armed{false};
// rtIcHarvestNoteUncached's counts, by site address, until the thread's
// instance holding the site is folded (a table is per thread and realm, and
// folded on its thread).
thread_local std::unordered_map<const uint64_t*, uint64_t> t_uncached;

struct Identity {
    uint32_t flags = 0;
    uint32_t sites = 0;
    uint64_t digest = 0;
    const bronze_code_range* ranges = nullptr;
    uint32_t rangeCount = 0;
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
    std::memcpy(&out.ranges, bytes + BRONZE_ABI_IC_IDENTITY_RANGES_OFFSET, sizeof(out.ranges));
    std::memcpy(&out.rangeCount, bytes + BRONZE_ABI_IC_IDENTITY_RANGE_COUNT_OFFSET, 4);
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
    if (it == s.shapeNames.end()) {
        it = s.shapeNames.emplace(shape, typeFeedbackShapeName(shape, s.protoNames)).first;
        if (shape != nullptr && !shape->isDictionary()) s.plainNamed.insert(shape);
    }
    return it->second;
}

// `shape`'s key nodes, root first, as its name lists the keys.
std::vector<const Shape*> keyNodes(const Shape* shape) {
    std::vector<const Shape*> chain;
    for (const Shape* n = shape; n != nullptr && n->parent != nullptr; n = n->parent) chain.push_back(n);
    std::reverse(chain.begin(), chain.end());
    return chain;
}

// The representation the run's objects kept, into `s.doubles` and
// `s.objects`. A position is a double (an object) one when the runtime saw its
// node born holding a Number (a plain object) — rtIcHarvestNoteBirth, what a
// build that makes the key eligible gives a claimed node — and no object of the
// shape the heap holds now, live or dead and not yet collected, has anything
// else there. Both are samples: nothing rests on them but the build's choice
// of keys, since the runtime keeps a claimed slot's discipline whatever a
// store brings (slot_repr.h).
void measureRepresentation(State& s) {
    s.doubles = {};
    s.objects = {};
    // Per named shape, a position's marks: bit 0 set when an object held a
    // non-Number there, bit 1 when one held anything but a plain object.
    std::unordered_map<const Shape*, std::vector<uint8_t>> held;
    std::unordered_map<const Shape*, std::vector<const Shape*>> nodes;
    for (const Shape* shape : s.plainNamed) nodes.emplace(shape, keyNodes(shape));
    rtHeap().walk_objects([&](HeapObjectHeader* hdr) {
        if (hdr->flags != HeapKind::Plain) return;
        const auto* obj = reinterpret_cast<const ObjectHeader*>(hdr);
        const auto found = nodes.find(obj->shape);
        if (found == nodes.end()) return;
        std::vector<uint8_t>& marks = held[obj->shape];
        marks.resize(found->second.size(), 0);
        for (size_t i = 0; i < found->second.size(); ++i) {
            const Shape* node = found->second[i];
            if (node->accessor || marks[i] == 3) continue;
            const Value v = obj->getSlot(node->slot_index);
            if (!slotReprAcceptsValue(v)) marks[i] |= 1;
            if (!slotReprAcceptsObject(v)) marks[i] |= 2;
        }
    });
    for (const auto& [shape, chain] : nodes) {
        const auto marks = held.find(shape);
        for (const uint8_t kind : {kHarvestBirthNumber, kHarvestBirthObject}) {
            State::Kept& kept = kind == kHarvestBirthNumber ? s.doubles : s.objects;
            std::vector<uint32_t> mine;
            for (size_t i = 0; i < chain.size(); ++i) {
                const Shape* node = chain[i];
                const auto birth = s.births.find(node);
                const bool other = marks != held.end() && (marks->second[i] & kind) != 0;
                const bool bornOther = birth != s.births.end() && (birth->second & kind) == 0;
                if (bornOther || other) {
                    if (StringHeader* key = node->key.valid() && !node->key.isSymbol() ? node->key.string() : nullptr) {
                        kept.other.insert(rtUtf8Chars(key));
                    }
                }
                if (birth == s.births.end() || bornOther || other || node->accessor ||
                    node->slot_index >= BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT) {
                    continue;
                }
                mine.push_back(static_cast<uint32_t>(i));
            }
            auto [d, fresh] = kept.held.try_emplace(s.shapeNames.at(shape), mine);
            if (!fresh) {
                std::vector<uint32_t> both;
                std::set_intersection(d->second.begin(), d->second.end(), mine.begin(), mine.end(),
                                      std::back_inserter(both));
                d->second = std::move(both);
            }
        }
    }
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
        const char* form = way->isAbsent() ? "absent" : way->isAccessor() ? "accessor" : way->realDepth() != 0 ? "proto" : way->isStoreFilled() ? "store" : "own";
        site.facts.insert(Fact{form, shapeName(s, way->cached_shape), {}});
    }
    if (used == BRONZE_ABI_IC_WAYS) site.full = true;
    // A write uses ways 0..2 (`BRONZE_ABI_IC_SET_WAYS`), leaving way 3's
    // cached_shape null and word 11 as its count of misses after all three
    // write ways filled; a read's word 11 is way 3's epoch, which a read whose
    // way 3 was never filled leaves zero.
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
        // A kind in the tag space is a primitive receiver's: a string, whose
        // methods are String.prototype's, or the number word.
        if (kind >= BRONZE_ABI_METHOD_IC_KIND_NUMBER) {
            const char* prim = kind == BRONZE_ABI_METHOD_IC_KIND_NUMBER ? "number" : "string";
            site.facts.insert(Fact{"native", prim, std::string("native:") + prim});
            return;
        }
        const std::string word = typeFeedbackKindWord(static_cast<uint16_t>(kind));
        site.facts.insert(Fact{"native", word, "native:" + word});
        return;
    }
    const std::string& receiver = shapeName(s, reinterpret_cast<const Shape*>(shapeWord));
    if (slotForm) {
        site.facts.insert(Fact{"slot", receiver,
                               codeWord != 0 ? calleeOf(reinterpret_cast<const void*>(codeWord))
                                             : std::string("*other*")});
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

std::string textOf(const State& s, const Module& m) {
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
        if (site.uncached != 0) {
            out += "other\t" + std::to_string(index) + "\t" + std::to_string(site.uncached) + "\n";
        }
    }
    for (const auto& [hash, count] : m.samples) {
        char h[9];
        std::snprintf(h, sizeof h, "%08x", hash);
        out += "sample\t" + std::string(h) + "\t" + std::to_string(count) + "\n";
    }
    // The representation the run kept (docs/type-feedback.md, "Harvest"):
    // per receiver its caches held, the key positions that stayed Numbers
    // (then plain objects), then every key something else was found under
    // (measureRepresentation).
    std::set<std::string> receivers;
    for (const auto& [index, site] : m.ics) {
        for (const Fact& f : site.facts) receivers.insert(f.receiver);
    }
    const auto rows = [&](const State::Kept& kept, const char* heldRow, const char* otherRow) {
        for (const std::string& receiver : receivers) {
            const auto it = kept.held.find(receiver);
            if (it == kept.held.end() || it->second.empty()) continue;
            out += heldRow;
            out += '\t';
            appendEscaped(out, receiver);
            out += '\t';
            for (size_t i = 0; i < it->second.size(); ++i) {
                if (i != 0) out += ',';
                out += std::to_string(it->second[i]);
            }
            out += '\n';
        }
        if (receivers.empty()) return;
        for (const std::string& key : kept.other) {
            out += otherRow;
            out += '\t';
            appendEscaped(out, key);
            out += '\n';
        }
    };
    rows(s.doubles, "doubles", "mixed");
    rows(s.objects, "objects", "nonobject");
    return out;
}

// The samples of the module's functions since the last fold, into `m`.
void takeSamples(Module& m, const Identity& id) {
    if (id.ranges == nullptr || id.rangeCount == 0) return;
    std::vector<uint64_t> counts(id.rangeCount, 0);
    rtHarvestTakeSamples(id.ranges, id.rangeCount, counts.data());
    for (uint32_t i = 0; i < id.rangeCount; ++i) {
        if (counts[i] == 0 || id.ranges[i].desc == nullptr) continue;
        m.samples[id.ranges[i].desc->name_hash] += counts[i];
    }
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
    rtHarvestSamplerStop();
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr || !g_state->dirty) return;
    for (const Module& m : g_state->modules) {
        if (!m.path.empty()) writeFile(m.path, textOf(*g_state, m));
    }
    g_state->dirty = false;
}

}  // namespace

bool rtIcHarvestArmed() noexcept { return g_armed.load(std::memory_order_relaxed); }

void rtIcHarvestNoteBirth(const Shape* node, uint8_t kinds) {
    if (node == nullptr) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr) return;
    auto [it, fresh] = g_state->births.emplace(node, kinds);
    if (!fresh) it->second &= kinds;
}

void rtIcHarvestNoteUncached(const uint64_t* site) {
    if (site != nullptr && rtIcHotCovers(site)) ++t_uncached[site];
}

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
    if (path.empty()) {
        s.records.erase(identity);
    } else {
        s.records.insert(identity);
    }
    if (!path.empty() && !s.atexitArmed) {
        s.atexitArmed = true;
        std::atexit(writeAtExit);
    }
    bool any = false;
    for (const Module& each : s.modules) any = any || !each.path.empty();
    g_armed.store(any, std::memory_order_relaxed);
    // The samples run while some harvest is armed (harvest_sampler.h).
    if (!path.empty()) {
        rtHarvestSamplerStart();
    } else if (!any) {
        rtHarvestSamplerStop();
    }
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
    constexpr uint64_t kWords = BRONZE_ABI_IC_SITE_SIZE / 8;
    // This table's uncached counts leave the thread's map whether or not the
    // module is harvested: the table's memory may be another module's next.
    std::vector<std::pair<uint64_t, uint64_t>> uncached;
    for (auto it = t_uncached.begin(); it != t_uncached.end();) {
        if (it->first < table || it->first >= table + siteCount * kWords) {
            ++it;
            continue;
        }
        uncached.emplace_back(static_cast<uint64_t>(it->first - table) / kWords, it->second);
        it = t_uncached.erase(it);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr) return;
    State& s = *g_state;
    // Before the record or the table is read (State::records).
    if (s.records.count(identity) == 0) return;
    Identity id;
    if (!readIdentity(identity, id) || id.sites != siteCount) return;
    Module* m = moduleOf(s, id.digest, id.sites);
    if (m == nullptr || m->path.empty()) return;
    const auto isMethod = [&](uint64_t i) {
        return methodSites != nullptr && std::binary_search(methodSites->begin(), methodSites->end(), i);
    };
    for (const auto& [i, count] : uncached) {
        Site& site = m->ics[static_cast<uint32_t>(i)];
        site.method = isMethod(i);
        site.uncached += count;
        s.dirty = true;
    }
    for (uint64_t i = 0; i < siteCount; ++i) {
        const uint64_t* words = table + i * kWords;
        const bool method = isMethod(i);
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
            // Both ways missed a relatch period's worth of calls: fewer is a
            // site of two receivers that met a third while starting up (or
            // whose entries a prototype change made stale once), which still
            // reads as the two it holds.
            if (words[BRONZE_ABI_METHOD_IC_MEGA_COUNT_WORD] >= BRONZE_ABI_METHOD_IC_MEGA_RELATCH) site.mega = true;
        } else {
            foldProperty(s, site, words);
        }
        s.dirty = true;
    }
    takeSamples(*m, id);
    if (!m->samples.empty()) s.dirty = true;
}

int rtIcHarvestWriteAll() {
    if (!rtIcHarvestArmed()) return 0;
    rtIcHotFoldLiveRanges();
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr) return 0;
    measureRepresentation(*g_state);
    int written = 0;
    for (const Module& m : g_state->modules) {
        if (!m.path.empty() && writeFile(m.path, textOf(*g_state, m))) ++written;
    }
    g_state->dirty = false;
    // Written: what the samples were for is on disk. A later write still
    // carries them; nothing is sampled after this one.
    rtHarvestSamplerStop();
    return written;
}

std::string rtIcHarvestText(const void* identity) {
    Identity id;
    if (!readIdentity(identity, id)) return {};
    rtIcHotFoldLiveRanges();
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_state == nullptr) return {};
    const Module* m = moduleOf(*g_state, id.digest, id.sites);
    if (m == nullptr || m->path.empty()) return {};
    measureRepresentation(*g_state);
    return textOf(*g_state, *m);
}

}  // namespace bronze::runtime
