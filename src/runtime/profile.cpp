// getenv, as heap.cpp: the CRT-deprecation opt-out, not a blanket C4996
// disable that would also swallow real deprecated-API uses.
#define _CRT_SECURE_NO_WARNINGS

#include "runtime/profile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/fn.h"
#include "runtime/object.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_state.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

bool g_profileEnabled = false;

namespace {

struct HelperProfile {
    std::string name;
    uint64_t totalCount = 0;
    std::unordered_map<std::string, uint64_t> subSites;
};

static std::unordered_map<std::string, HelperProfile> g_helpers;
static bool s_initialized = false;

struct AutoProfileInit {
    AutoProfileInit() {
        initProfile();
    }
} s_autoProfileInit;

}  // namespace

void initProfile() {
    if (s_initialized) return;
    s_initialized = true;
    const char* env = std::getenv("BRONZE_PROFILE");
    if (env && std::strcmp(env, "1") == 0) {
        g_profileEnabled = true;
        std::atexit(dumpProfileReport);
    }
}

void profileRecordHelper(const char* helperName) {
    if (!helperName) return;
    auto& h = g_helpers[helperName];
    h.name = helperName;
    h.totalCount++;
}

void profileRecordProp(const char* helperName, uint32_t keyIndex, const void* icSite) {
    if (!helperName) return;
    auto& h = g_helpers[helperName];
    h.name = helperName;
    h.totalCount++;

    std::string key = "." + rtKeyString(keyIndex);
    if (icSite) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), " (site %p)", icSite);
        key += buf;
    }
    h.subSites[key]++;
}

void profileRecordElem(const char* helperName) {
    if (!helperName) return;
    auto& h = g_helpers[helperName];
    h.name = helperName;
    h.totalCount++;
}

void profileRecordCall(const char* helperName, uint64_t calleeBits) {
    if (!helperName) return;
    auto& h = g_helpers[helperName];
    h.name = helperName;
    h.totalCount++;

    std::string calleeName = "(unknown)";
    Value v(calleeBits);
    if (v.isObject()) {
        HeapObjectHeader* hdr = v.asObject<HeapObjectHeader>();
        if (hdr && hdr->flags == HeapKind::Function) {
            auto* fn = reinterpret_cast<FunctionHeader*>(hdr);
            if (fn->name) {
                if (fn->name->getLength() == 0) {
                    calleeName = "fn (anonymous)";
                } else {
                    calleeName = "fn \"" + rtUtf8Chars(fn->name) + "\"";
                }
            } else {
                calleeName = "fn (native/unnamed)";
            }
        }
    }
    h.subSites[calleeName]++;
}

void dumpProfileReport() {
    if (!g_profileEnabled) return;

    uint64_t grandTotal = 0;
    for (const auto& [name, h] : g_helpers) {
        grandTotal += h.totalCount;
    }
    if (grandTotal == 0) return;

    std::vector<HelperProfile> sortedHelpers;
    sortedHelpers.reserve(g_helpers.size());
    for (auto& [name, h] : g_helpers) {
        sortedHelpers.push_back(h);
    }

    std::sort(sortedHelpers.begin(), sortedHelpers.end(),
              [](const HelperProfile& a, const HelperProfile& b) {
                  if (a.totalCount != b.totalCount) {
                      return a.totalCount > b.totalCount;
                  }
                  return a.name < b.name;
              });

    std::fprintf(stderr, "\n=== Bronze Runtime Profile (BRONZE_PROFILE=1) ===\n");
    std::fprintf(stderr, "Total Dynamic ABI Helper Invocations: %llu\n\n",
                 static_cast<unsigned long long>(grandTotal));
    std::fprintf(stderr, "%-48s %12s %8s\n", "Helper / Site", "Count", "% Total");
    std::fprintf(stderr, "-----------------------------------------------------------------------\n");

    for (const auto& h : sortedHelpers) {
        double pct = grandTotal > 0 ? (100.0 * h.totalCount / grandTotal) : 0.0;
        std::fprintf(stderr, "%-48s %12llu %7.1f%%\n",
                     h.name.c_str(),
                     static_cast<unsigned long long>(h.totalCount),
                     pct);

        if (!h.subSites.empty()) {
            std::vector<std::pair<std::string, uint64_t>> sortedSites(
                h.subSites.begin(), h.subSites.end());
            std::sort(sortedSites.begin(), sortedSites.end(),
                      [](const auto& a, const auto& b) {
                          if (a.second != b.second) return a.second > b.second;
                          return a.first < b.first;
                      });

            // Show top sub-sites (up to 10)
            size_t count = 0;
            for (const auto& site : sortedSites) {
                if (++count > 10) {
                    size_t remaining = sortedSites.size() - 10;
                    std::fprintf(stderr, "    ... and %zu more site(s)\n", remaining);
                    break;
                }
                double sitePct = grandTotal > 0 ? (100.0 * site.second / grandTotal) : 0.0;
                std::string label = "  " + site.first;
                if (label.size() > 48) label = label.substr(0, 45) + "...";
                std::fprintf(stderr, "%-48s %12llu %7.1f%%\n",
                             label.c_str(),
                             static_cast<unsigned long long>(site.second),
                             sitePct);
            }
        }
    }
    std::fprintf(stderr, "-----------------------------------------------------------------------\n\n");
    std::fflush(stderr);
}

}  // namespace bronze::runtime
