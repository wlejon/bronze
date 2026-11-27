#include "opt/spec/profile.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace bronze::spec {

namespace {

std::vector<std::string_view> splitTabs(std::string_view line) {
    std::vector<std::string_view> out;
    size_t begin = 0;
    while (true) {
        const size_t tab = line.find('\t', begin);
        out.push_back(line.substr(begin, tab == std::string_view::npos ? std::string_view::npos : tab - begin));
        if (tab == std::string_view::npos) return out;
        begin = tab + 1;
    }
}

bool parseU64(std::string_view s, uint64_t& out) {
    if (s.empty()) return false;
    const auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

std::string escapeName(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

bool unescapeName(std::string_view in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '\\') {
            out += in[i];
            continue;
        }
        if (++i == in.size()) return false;
        switch (in[i]) {
            case '\\': out += '\\'; break;
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            default: return false;
        }
    }
    return true;
}

bool isPropertyForm(std::string_view form) {
    return form == "own" || form == "store" || form == "proto" || form == "absent" || form == "accessor" || form == "array";
}

bool isMethodForm(std::string_view form) { return form == "direct" || form == "slot" || form == "native"; }

// One row of a harvest section:
//   harvest <16 hex digits> <sites>
//   ic      <index> property|method <flags: -, or full / mega, comma-separated>
//   way     <index> <form> <receiver> [<callee>, method sites]
//                                     [<getter> <setter>, a property site's
//                                      `accessor` way, version 7 on]
//                                     [<depth> <slot>, a property site's
//                                      `proto` way, version 8 on]
//   other   <index> <times a receiver no way could hold reached the runtime>
//   slowsites <the build's slow-site count> <the sites it armed: per 64, a
//             word of 16 hex digits whose bit i is site 64 * word + i>
//   slow    <slow-site index> <times its generic code ran>
//   (`sample` rows of a version 8 or older file, the sampler's, are skipped)
//   doubles <receiver> <ascending key positions, comma-separated>
//   mixed   <key>
//   objects <receiver> <ascending key positions, comma-separated>
//   nonobject <key>
bool parseHarvestRow(const std::vector<std::string_view>& f, std::optional<Harvest>& harvest, std::string& why) {
    if (f[0] == "harvest") {
        Harvest h;
        uint64_t sites = 0;
        if (harvest || f.size() != 3 || f[1].size() != 16 || !parseU64(f[2], sites) || sites > UINT32_MAX) {
            why = "malformed harvest row";
            return false;
        }
        const auto res = std::from_chars(f[1].data(), f[1].data() + 16, h.digest, 16);
        if (res.ec != std::errc() || res.ptr != f[1].data() + 16) {
            why = "malformed harvest digest";
            return false;
        }
        h.sites = static_cast<uint32_t>(sites);
        harvest = std::move(h);
        return true;
    }
    if (!harvest) {
        why = "an '" + std::string(f[0]) + "' row before the harvest row";
        return false;
    }
    if (f[0] == "slowsites") {
        uint64_t n = 0;
        if (f.size() != 3 || !parseU64(f[1], n) || n == 0 || n > UINT32_MAX || harvest->slowSites != 0 ||
            f[2].size() != (n + 63) / 64 * 16) {
            why = "malformed slowsites row";
            return false;
        }
        std::vector<uint64_t> armed((n + 63) / 64);
        for (size_t w = 0; w < armed.size(); ++w) {
            const char* at = f[2].data() + w * 16;
            const auto res = std::from_chars(at, at + 16, armed[w], 16);
            if (res.ec != std::errc() || res.ptr != at + 16) {
                why = "malformed slowsites bitmap";
                return false;
            }
        }
        harvest->slowSites = static_cast<uint32_t>(n);
        harvest->slowArmed = std::move(armed);
        return true;
    }
    if (f[0] == "slow") {
        uint64_t index = 0, count = 0;
        if (f.size() != 3 || !parseU64(f[1], index) || index >= harvest->slowSites || !parseU64(f[2], count) ||
            count == 0 || !harvest->slow.emplace(static_cast<uint32_t>(index), count).second) {
            why = "malformed slow row";
            return false;
        }
        return true;
    }
    if (f[0] == "last") {
        uint64_t index = 0;
        std::string name;
        if (f.size() != 3 || !parseU64(f[1], index) || index >= harvest->slowSites || !unescapeName(f[2], name) ||
            name.empty() || !harvest->last.emplace(static_cast<uint32_t>(index), std::move(name)).second) {
            why = "malformed last row";
            return false;
        }
        return true;
    }
    if (f[0] == "callee") {
        uint64_t index = 0;
        std::string name;
        if (f.size() != 3 || !parseU64(f[1], index) || index >= harvest->slowSites || !unescapeName(f[2], name) ||
            name.empty() || !harvest->callees.emplace(static_cast<uint32_t>(index), std::move(name)).second) {
            why = "malformed callee row";
            return false;
        }
        return true;
    }
    if (f[0] == "doubles" || f[0] == "objects") {
        std::string receiver;
        std::vector<uint32_t> positions;
        bool ok = f.size() == 3 && unescapeName(f[1], receiver) && !receiver.empty() && !f[2].empty();
        for (size_t at = 0; ok && at <= f[2].size();) {
            const size_t comma = std::min(f[2].find(',', at), f[2].size());
            uint64_t n = 0;
            ok = parseU64(f[2].substr(at, comma - at), n) && n <= UINT32_MAX &&
                 (positions.empty() || n > positions.back());
            positions.push_back(static_cast<uint32_t>(n));
            at = comma + 1;
        }
        auto& rows = f[0] == "doubles" ? harvest->doubles : harvest->objects;
        if (!ok || !rows.emplace(std::move(receiver), std::move(positions)).second) {
            why = "malformed " + std::string(f[0]) + " row";
            return false;
        }
        return true;
    }
    if (f[0] == "mixed" || f[0] == "nonobject") {
        std::string key;
        auto& keys = f[0] == "mixed" ? harvest->mixed : harvest->nonObject;
        // The empty string is a key like any other (`o[""] = v`).
        if (f.size() != 2 || !unescapeName(f[1], key) || !keys.insert(key).second) {
            why = "malformed " + std::string(f[0]) + " row";
            return false;
        }
        return true;
    }
    uint64_t index = 0;
    if (f.size() < 2 || !parseU64(f[1], index) || index >= harvest->sites) {
        why = "malformed or out-of-range IC index";
        return false;
    }
    const auto ic = static_cast<uint32_t>(index);
    if (f[0] == "ic") {
        HarvestSite site;
        if (f.size() != 4 || (f[2] != "property" && f[2] != "method")) {
            why = "malformed ic row";
            return false;
        }
        site.method = f[2] == "method";
        for (std::string_view rest = f[3]; rest != "-" && !rest.empty();) {
            const size_t comma = std::min(rest.find(','), rest.size());
            const std::string_view flag = rest.substr(0, comma);
            if (flag == "full") site.full = true;
            else if (flag == "mega") site.mega = true;
            else {
                why = "unknown ic flag '" + std::string(flag) + "'";
                return false;
            }
            rest = rest.substr(std::min(comma + 1, rest.size()));
        }
        if (!harvest->ics.emplace(ic, std::move(site)).second) {
            why = "ic listed twice";
            return false;
        }
        return true;
    }
    auto it = harvest->ics.find(ic);
    if (it == harvest->ics.end()) {
        why = "a " + std::string(f[0]) + " row for an ic not yet listed";
        return false;
    }
    if (f[0] == "other") {
        uint64_t count = 0;
        if (f.size() != 3 || !parseU64(f[2], count) || count == 0 || it->second.uncached != 0) {
            why = "malformed other row";
            return false;
        }
        it->second.uncached = count;
        return true;
    }
    HarvestWay way;
    const bool method = it->second.method;
    // An accessor way names its getter and setter from version 7 on, a proto
    // way its holder's depth and slot from version 8 on; an older one names
    // neither.
    const bool halves = !method && f.size() == 6 && (f[2] == "accessor" || f[2] == "proto");
    uint64_t depth = 0;
    uint64_t slot = 0;
    if (f.size() != (method ? 5u : halves ? 6u : 4u) || !(method ? isMethodForm(f[2]) : isPropertyForm(f[2])) ||
        !unescapeName(f[3], way.receiver) || way.receiver.empty() ||
        (method && (!unescapeName(f[4], way.callee) || way.callee.empty())) ||
        (halves && (!unescapeName(f[4], way.callee) || way.callee.empty() || !unescapeName(f[5], way.setter) ||
                    way.setter.empty())) ||
        (halves && f[2] == "proto" &&
         (!parseU64(f[4], depth) || depth == 0 || !parseU64(f[5], slot) || slot > INT32_MAX))) {
        why = "malformed way row";
        return false;
    }
    way.form = std::string(f[2]);
    it->second.ways.push_back(std::move(way));
    return true;
}

}  // namespace

std::optional<TypeProfile> TypeProfile::parse(std::string_view text, std::string* error) {
    TypeProfile profile;
    std::optional<Harvest> harvest;
    size_t lineNo = 0;
    bool sawHeader = false;
    bool sampled = false;  // a version 8 or older file, whose `sample` rows are skipped
    auto fail = [&](const std::string& what) -> std::optional<TypeProfile> {
        if (error != nullptr) *error = "type feedback line " + std::to_string(lineNo) + ": " + what;
        return std::nullopt;
    };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) nl = text.size();
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        if (!sawHeader) {
            const std::string want =
                "bronze-type-feedback " + std::to_string(BRONZE_FEEDBACK_FORMAT_VERSION);
            if (line != want && line != "bronze-type-feedback 1" && line != "bronze-type-feedback 2" &&
                line != "bronze-type-feedback 3" && line != "bronze-type-feedback 4" &&
                line != "bronze-type-feedback 5" && line != "bronze-type-feedback 6" &&
                line != "bronze-type-feedback 7" && line != "bronze-type-feedback 8") {
                return fail("expected '" + want + "', got '" + std::string(line) + "'");
            }
            sawHeader = true;
            sampled = line != want;
            continue;
        }
        const std::vector<std::string_view> f = splitTabs(line);
        if (sampled && f[0] == "sample") continue;
        if (f[0] == "harvest" || f[0] == "ic" || f[0] == "way" || f[0] == "other" ||
            f[0] == "slowsites" || f[0] == "slow" || f[0] == "last" || f[0] == "callee" ||
            f[0] == "doubles" || f[0] == "mixed" || f[0] == "objects" || f[0] == "nonobject") {
            if (std::string why; !parseHarvestRow(f, harvest, why)) return fail(why);
            continue;
        }
        return fail("unknown row '" + std::string(f[0]) + "'");
    }
    if (!sawHeader) {
        lineNo = 0;
        return fail("empty profile (no 'bronze-type-feedback' header)");
    }
    if (harvest) profile.harvest_ = std::move(harvest);
    return profile;
}

std::optional<TypeProfile> TypeProfile::load(const std::string& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = "cannot read type feedback file '" + path + "'";
        return std::nullopt;
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string err;
    auto profile = parse(buf.str(), &err);
    if (!profile && error != nullptr) *error = path + ": " + err;
    return profile;
}

std::string TypeProfile::serialize() const {
    std::string out = "bronze-type-feedback " + std::to_string(BRONZE_FEEDBACK_FORMAT_VERSION) + "\n";
    if (harvest_) {
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(harvest_->digest));
        out += "harvest\t" + std::string(hex) + "\t" + std::to_string(harvest_->sites) + "\n";
        if (harvest_->slowSites != 0) {
            out += "slowsites\t" + std::to_string(harvest_->slowSites) + "\t";
            for (size_t w = 0; w < (harvest_->slowSites + 63) / 64; ++w) {
                char word[17];
                const uint64_t bits = w < harvest_->slowArmed.size() ? harvest_->slowArmed[w] : 0;
                std::snprintf(word, sizeof word, "%016llx", static_cast<unsigned long long>(bits));
                out += word;
            }
            out += "\n";
            for (const auto& [index, count] : harvest_->slow) {
                out += "slow\t" + std::to_string(index) + "\t" + std::to_string(count) + "\n";
            }
            for (const auto& [index, name] : harvest_->last) {
                out += "last\t" + std::to_string(index) + "\t" + name + "\n";
            }
            for (const auto& [index, name] : harvest_->callees) {
                out += "callee\t" + std::to_string(index) + "\t" + escapeName(name) + "\n";
            }
        }
        for (const auto& [index, site] : harvest_->ics) {
            std::string flags;
            if (site.full) flags = "full";
            if (site.mega) flags += flags.empty() ? "mega" : ",mega";
            out += "ic\t" + std::to_string(index) + "\t" + (site.method ? "method" : "property") + "\t" +
                   (flags.empty() ? "-" : flags) + "\n";
            for (const HarvestWay& way : site.ways) {
                out += "way\t" + std::to_string(index) + "\t" + way.form + "\t" + escapeName(way.receiver);
                if (site.method) out += "\t" + escapeName(way.callee);
                if (!site.method && !way.callee.empty()) {
                    out += "\t" + escapeName(way.callee) + "\t" + escapeName(way.setter);
                }
                out += "\n";
            }
            if (site.uncached != 0) {
                out += "other\t" + std::to_string(index) + "\t" + std::to_string(site.uncached) + "\n";
            }
        }
        const auto rows = [&](const std::map<std::string, std::vector<uint32_t>>& held, const char* heldRow,
                              const std::set<std::string>& other, const char* otherRow) {
            for (const auto& [receiver, positions] : held) {
                out += std::string(heldRow) + "\t" + escapeName(receiver) + "\t";
                for (size_t i = 0; i < positions.size(); ++i) {
                    if (i != 0) out += ",";
                    out += std::to_string(positions[i]);
                }
                out += "\n";
            }
            for (const std::string& key : other) out += std::string(otherRow) + "\t" + escapeName(key) + "\n";
        };
        rows(harvest_->doubles, "doubles", harvest_->mixed, "mixed");
        rows(harvest_->objects, "objects", harvest_->nonObject, "nonobject");
    }
    return out;
}

}  // namespace bronze::spec
