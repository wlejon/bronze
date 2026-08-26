// getenv, as shape_census.cpp: the CRT-deprecation opt-out, not a blanket
// C4996 disable that would also swallow real deprecated-API uses.
#define _CRT_SECURE_NO_WARNINGS

#include "runtime/pin_census.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/object.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

// One manifest ENTRY's evidence, joined across every site that names it.
//
// The join is the analysis. `Vector3.x` has a store in the constructor, one in
// `set`, one in `copy` and twenty more; `param render(iters)` has as many sites
// as the program has calls to the closure it handed out. All of them number is
// a pin. One of them anything else is not, and no weighting or threshold
// changes that: a pin is spent UNCHECKED at the read, so "almost always a
// number" is not a weaker version of the claim, it is a different and false
// one.
struct Target {
    uint32_t kind = BRONZE_ABI_CENSUS_ENV_SLOT;
    uint64_t hits = 0;
    uint64_t numbers = 0;
    uint64_t nullish = 0;
    // A plain dense JS Array whose every element is a Number — the evidence for
    // `numeric-elements`, and the reason this walk is affordable is that a
    // census build is never benchmarked.
    uint64_t numberArrays = 0;
    uint64_t otherObjects = 0;
    uint64_t strings = 0;
    uint64_t bools = 0;
    uint64_t others = 0;
    // Set by a site that disqualifies the whole entry on sight, with no run
    // needed: a return the body can fall off, an owner spelling that would
    // govern two different functions. Registration alone carries it, which is
    // why the site table exists.
    bool refused = false;
    // How many distinct sites the module registered for this entry. Reported in
    // the provenance comment: one site is one store or one call position, and a
    // reader deciding whether a run was representative wants to know.
    uint32_t sites = 0;
};

std::mutex g_mu;
std::map<std::string, Target>* g_targets = nullptr;  // leaked on purpose
// The field NAMES that some store in the program reaches through a receiver
// inference could not type. B1's negative 1: that store carries no barrier
// while a class-known read elsewhere still spends the claim, so any entry for
// a field of this name is enforced only in part.
std::set<std::string>* g_opaqueFields = nullptr;
std::string* g_outPath = nullptr;
bool g_armed = false;

std::string fieldOf(const std::string& target) {
    const auto dot = target.rfind('.');
    return dot == std::string::npos ? target : target.substr(dot + 1);
}

// Is `v` a plain dense JS Array of Numbers? The `numeric-elements` question,
// asked of the value at the moment it reaches the field.
//
// A hole answers no: the element form that pin licenses
// (`il::kElemKindPlainArrayF64`) reads the slot raw, and a hole's bits are not
// a double. An EMPTY array answers no as well, and deliberately — an array with
// nothing in it is evidence for every element claim and for none of them, and
// a `Matrix4.elements` that was empty at every observation is a manifest entry
// no run supports.
bool isDenseNumberArray(const Value& v) {
    if (!v.isObject()) return false;
    auto* header = v.asObject<HeapObjectHeader>();
    if (header->flags != HeapKind::Array) return false;
    auto* arr = v.asObject<ArrayHeader>();
    if (arr->length == 0) return false;
    if (!arr->elements.isObject()) return false;
    const Value* data = arr->elementsData();
    for (uint32_t i = 0; i < arr->length; ++i) {
        const Value e = data[i];
        if (!e.isNumber() && !e.isInt32()) return false;
    }
    return true;
}

void classify(Target& t, uint64_t bits) {
    const Value v(bits);
    ++t.hits;
    if (v.isNumber() || v.isInt32()) {
        ++t.numbers;
    } else if (v.isNull() || v.isUndefined() || v.isHole()) {
        ++t.nullish;
    } else if (v.isString()) {
        ++t.strings;
    } else if (v.isBool()) {
        ++t.bools;
    } else if (v.isObject()) {
        if (isDenseNumberArray(v)) ++t.numberArrays;
        else ++t.otherObjects;
    } else {
        ++t.others;
    }
}

const char* kindWord(uint32_t kind) {
    switch (kind) {
        case BRONZE_ABI_CENSUS_ENV_SLOT: return "env slot";
        case BRONZE_ABI_CENSUS_FIELD: return "field";
        case BRONZE_ABI_CENSUS_PARAM: return "parameter";
        case BRONZE_ABI_CENSUS_RETURN: return "return";
        default: return "store";
    }
}

// What the observations support, or an empty string for "nothing".
//
// The `param`, `return` and `function` forms admit `number` and nothing else
// (types/pins.h): each is spent on a raw unbox or an f64 calling-convention
// slot, and there is no `undefined` in an f64. Only the FIELD forms have a
// weaker kind to fall back to.
std::string kindFor(const Target& t) {
    if (t.hits == 0) return {};
    if (t.strings != 0 || t.bools != 0 || t.others != 0) return {};
    const bool fieldForm = t.kind == BRONZE_ABI_CENSUS_FIELD;
    if (t.numbers == t.hits) return "number";
    if (!fieldForm) return {};
    if (t.numberArrays == t.hits) return "numeric-elements";
    if (t.otherObjects != 0 || t.numberArrays != 0) return {};
    // Numbers and nullish and nothing else, and — the load-bearing clause — at
    // least one NUMBER among them. A field this run only ever saw `null` in is
    // the census's worst case: the pin buys nothing (what it licenses is the
    // coercing position on the NUMBER arm) and risks everything, because
    // "never assigned yet" is what an optional field looks like on a short run
    // and an object is what it holds on a long one. three.js's
    // `Material.clippingPlanes` is exactly that shape, and it is an ARRAY in
    // any program that uses it.
    if (t.numbers != 0 && t.nullish != 0 && t.numbers + t.nullish == t.hits) {
        return "number-or-nullish";
    }
    return {};
}

std::string entryText(const std::string& target, const std::string& kind) {
    return target + ": " + kind;
}

// The reason an entry the census considered is NOT in the file. Written as a
// comment beside the refused entries, because a census whose output is only its
// hits tells a reader nothing about the claim it declined to make — and the
// declines are where the interesting shapes are (a field that holds an object,
// a parameter one caller passes a string to).
std::string refusalOf(const Target& t) {
    if (t.refused) return "the compiler refuses this form here";
    if (t.hits == 0) return "never reached on this run";
    if (t.kind == BRONZE_ABI_CENSUS_FIELD && t.numbers == 0 && t.nullish == t.hits) {
        return "only ever nullish: no evidence it is a number, and an optional field is "
               "what an object-valued one looks like on a short run";
    }
    std::string what;
    auto add = [&](uint64_t n, const char* word) {
        if (n == 0) return;
        if (!what.empty()) what += ", ";
        what += std::to_string(n);
        what += " ";
        what += word;
    };
    add(t.numbers, "number");
    add(t.nullish, "nullish");
    add(t.numberArrays, "number-array");
    add(t.otherObjects, "object");
    add(t.strings, "string");
    add(t.bools, "boolean");
    add(t.others, "other");
    return "polymorphic: " + what;
}

std::string tally(const Target& t) {
    std::string s = std::to_string(t.hits) + " obs / " + std::to_string(t.sites) + " site";
    if (t.sites != 1) s += "s";
    return s;
}

}  // namespace

void censusRegister(const char* outPath, const uint32_t* table, uint32_t count,
                    const uint32_t* keyMap) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_targets == nullptr) {
        g_targets = new std::map<std::string, Target>();
        g_opaqueFields = new std::set<std::string>();
        g_outPath = new std::string();
    }
    if (outPath != nullptr && *outPath != '\0' && g_outPath->empty()) *g_outPath = outPath;
    if (!g_armed) {
        g_armed = true;
        std::atexit(censusWriteManifest);
    }
    if (table == nullptr || keyMap == nullptr) return;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t keyId = keyMap[table[i * 2 + 0]];
        const uint32_t info = table[i * 2 + 1];
        const std::string& name = rtKeyString(keyId);
        if ((info & BRONZE_ABI_CENSUS_KIND_MASK) == BRONZE_ABI_CENSUS_OPAQUE) {
            g_opaqueFields->insert(name);
            continue;
        }
        Target& t = (*g_targets)[name];
        t.kind = info & BRONZE_ABI_CENSUS_KIND_MASK;
        t.sites += 1;
        if ((info & BRONZE_ABI_CENSUS_REFUSES) != 0) t.refused = true;
    }
}

void censusRecord(uint32_t keyId, uint32_t siteInfo, uint64_t valueBits) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_targets == nullptr) return;
    const uint32_t kind = siteInfo & BRONZE_ABI_CENSUS_KIND_MASK;
    if (kind == BRONZE_ABI_CENSUS_OPAQUE) return;  // registration is the whole record
    auto it = g_targets->find(rtKeyString(keyId));
    if (it == g_targets->end()) return;
    classify(it->second, valueBits);
}

void censusWriteManifest() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_targets == nullptr) return;

    std::string path = *g_outPath;
    if (const char* env = std::getenv("BRONZE_PIN_CENSUS_OUT")) path = env;
    if (path.empty()) path = "bronze-census.pins";

    // The entries the run supports, and — separately — the ones it does not,
    // which are comments. Both are grouped by FORM rather than by hit count,
    // because the file's reader is a person deciding whether the promises are
    // true, and the four forms are four different questions.
    struct Line {
        std::string text;
        std::string comment;
    };
    std::vector<Line> lines[5];
    std::vector<Line> refused[5];
    size_t emitted = 0;
    size_t observedMarked = 0;
    for (auto& [name, t] : *g_targets) {
        const uint32_t bucket = t.kind < 5 ? t.kind : 4u;
        const std::string kind = t.refused ? std::string() : kindFor(t);
        if (kind.empty()) {
            refused[bucket].push_back({name, refusalOf(t)});
            continue;
        }
        std::string text = entryText(name, kind);
        std::string comment = tally(t);
        if (t.kind == BRONZE_ABI_CENSUS_FIELD &&
            g_opaqueFields->count(fieldOf(name)) != 0) {
            // B1's negative 1, carried into the file. `@observed` is refused by
            // `PinManifest::parse` unless the build passes
            // `--pins-allow-observed`, so an entry the compiler cannot hold to
            // its promise never ships by accident.
            text += " @observed";
            comment += "; a store to '" + fieldOf(name) +
                       "' goes through a receiver the compiler cannot type";
            ++observedMarked;
        }
        lines[bucket].push_back({text, comment});
        ++emitted;
    }

    std::string out;
    out += "# Written by `bronze build --census` (src/runtime/pin_census.h).\n";
    out += "#\n";
    out += "# Every entry below is an OBSERVATION of one run, promoted to a promise.\n";
    out += "# Nothing here is proved: what makes it shippable is that a violated pin\n";
    out += "# throws a TypeError naming the line (stage B1), so a wrong entry on a path\n";
    out += "# this run did not take is a diagnostic and not a corrupt read. Entries the\n";
    out += "# compiler already PROVES are absent by construction — a census site exists\n";
    out += "# only where lowering had no static answer left.\n";
    out += "#\n";
    out += "# An entry marked `@observed` is one whose stores are not all from sites the\n";
    out += "# compiler can type, so B1's write barrier cannot hold all of them. Those are\n";
    out += "# REFUSED by a default build; `--pins-allow-observed` accepts them.\n";
    out += "#\n";
    out += "# The commented `refused` lines are the entries the census considered and\n";
    out += "# declined, with the reason. They are the interesting half: a field that held\n";
    out += "# an object once is a field no manifest should have pinned.\n";
    out += "#\n";
    out += "# " + std::to_string(emitted) + " entries, " + std::to_string(observedMarked) +
           " marked @observed, " + std::to_string(g_targets->size()) + " candidate sites.\n";

    static const char* kHeadings[5] = {
        "\n# --- captured bindings (`function <fn>.<binding>`) ---------------------------\n",
        "\n# --- fields (`<Class>.<field>`) ---------------------------------------------\n",
        "\n# --- parameters (`param <owner>(<p>)`) --------------------------------------\n",
        "\n# --- returns (`return <owner>`) ---------------------------------------------\n",
        "\n# --- other -------------------------------------------------------------------\n",
    };
    for (uint32_t bucket = 0; bucket < 5; ++bucket) {
        if (lines[bucket].empty() && refused[bucket].empty()) continue;
        out += kHeadings[bucket];
        std::sort(lines[bucket].begin(), lines[bucket].end(),
                  [](const Line& a, const Line& b) { return a.text < b.text; });
        for (const Line& l : lines[bucket]) {
            out += l.text + "  # " + l.comment + "\n";
        }
        std::sort(refused[bucket].begin(), refused[bucket].end(),
                  [](const Line& a, const Line& b) { return a.text < b.text; });
        for (const Line& l : refused[bucket]) {
            out += "# refused " + l.text + " (" + std::string(kindWord(bucket)) + "): " +
                   l.comment + "\n";
        }
    }

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "bronze: census: cannot write '%s'\n", path.c_str());
        return;
    }
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);
}

}  // namespace bronze::runtime

// The ABI face. Thin on purpose: everything a generated module can say is one
// site's key, its packing, and a value.
extern "C" {

void bronze_census_register(const char* outPath, const uint32_t* table, uint32_t count,
                            const uint32_t* keyMap) {
    bronze::runtime::censusRegister(outPath, table, count, keyMap);
}

void bronze_census_record(uint32_t keyId, uint32_t siteInfo, uint64_t valueBits) {
    bronze::runtime::censusRecord(keyId, siteInfo, valueBits);
}

}  // extern "C"
