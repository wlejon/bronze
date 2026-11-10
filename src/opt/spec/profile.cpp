#include "opt/spec/profile.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace bronze::spec {

namespace {

const char* const kTagNames[] = BRONZE_FEEDBACK_TAG_NAMES;
const char* const kKindNames[] = BRONZE_FEEDBACK_KIND_NAMES;

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

int kindFromName(std::string_view name) {
    for (uint32_t k = 0; k < BRONZE_FEEDBACK_KIND_COUNT; ++k) {
        if (name == kKindNames[k]) return static_cast<int>(k);
    }
    return -1;
}

bool isPropertyForm(std::string_view form) {
    return form == "own" || form == "proto" || form == "absent" || form == "accessor" || form == "array";
}

bool isMethodForm(std::string_view form) { return form == "direct" || form == "slot" || form == "native"; }

// One row of a harvest section:
//   harvest <16 hex digits> <sites>
//   ic      <index> property|method <flags: -, or full / mega, comma-separated>
//   way     <index> <form> <receiver> [<callee>, method sites]
//   sample  <8 hex digits: the function's name hash> <count>
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
    if (f[0] == "sample") {
        uint32_t hash = 0;
        uint64_t count = 0;
        bool ok = f.size() == 3 && f[1].size() == 8;
        if (ok) {
            const auto res = std::from_chars(f[1].data(), f[1].data() + 8, hash, 16);
            ok = res.ec == std::errc() && res.ptr == f[1].data() + 8;
        }
        if (!ok || !parseU64(f[2], count) || !harvest->samples.emplace(hash, count).second) {
            why = "malformed sample row";
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
        why = "a way row for an ic not yet listed";
        return false;
    }
    HarvestWay way;
    const bool method = it->second.method;
    if (f.size() != (method ? 5u : 4u) || !(method ? isMethodForm(f[2]) : isPropertyForm(f[2])) ||
        !unescapeName(f[3], way.receiver) || way.receiver.empty() ||
        (method && (!unescapeName(f[4], way.callee) || way.callee.empty()))) {
        why = "malformed way row";
        return false;
    }
    way.form = std::string(f[2]);
    it->second.ways.push_back(std::move(way));
    return true;
}

std::string sole(const std::vector<std::pair<std::string, uint64_t>>& list) {
    if (list.size() != 1 || list.front().first == "*other*") return {};
    return list.front().first;
}

}  // namespace

const char* tagName(uint32_t tag) { return tag < BRONZE_FEEDBACK_TAG_COUNT ? kTagNames[tag] : "?"; }

int tagFromName(std::string_view name) {
    for (uint32_t t = 0; t < BRONZE_FEEDBACK_TAG_COUNT; ++t) {
        if (name == kTagNames[t]) return static_cast<int>(t);
    }
    return -1;
}

uint32_t SiteFeedback::tagMask() const {
    uint32_t mask = 0;
    for (uint32_t t = 0; t < BRONZE_FEEDBACK_TAG_COUNT; ++t) {
        if (tags[t] != 0) mask |= 1u << t;
    }
    return mask;
}

bool SiteFeedback::monomorphic() const {
    const uint32_t m = tagMask();
    return m != 0 && (m & (m - 1)) == 0;
}

std::string SiteFeedback::soleShape() const { return sole(shapes); }
std::string SiteFeedback::soleTarget() const { return sole(targets); }

std::optional<TypeProfile> TypeProfile::parse(std::string_view text, std::string* error) {
    TypeProfile profile;
    std::optional<Harvest> harvest;
    FunctionFeedback* fn = nullptr;
    size_t lineNo = 0;
    bool sawHeader = false;
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
            if (line != want && line != "bronze-type-feedback 1") {
                return fail("expected '" + want + "', got '" + std::string(line) + "'");
            }
            sawHeader = true;
            continue;
        }
        const std::vector<std::string_view> f = splitTabs(line);
        if (f[0] == "harvest" || f[0] == "ic" || f[0] == "way" || f[0] == "sample") {
            if (std::string why; !parseHarvestRow(f, harvest, why)) return fail(why);
            continue;
        }
        if (f[0] == "fn") {
            FunctionFeedback next;
            if (f.size() != 3 || !unescapeName(f[1], next.key) || next.key.empty() ||
                !parseU64(f[2], next.entries)) {
                return fail("malformed fn row");
            }
            if (profile.functions_.count(next.key) != 0) return fail("function '" + next.key + "' listed twice");
            const std::string key = next.key;
            fn = &(profile.functions_[key] = std::move(next));
            continue;
        }
        if (fn == nullptr) return fail("a '" + std::string(f[0]) + "' row before any fn row");
        uint64_t ordinal = 0;
        if (f.size() < 2 || !parseU64(f[1], ordinal) || ordinal > BRONZE_FEEDBACK_ORDINAL_MASK) {
            return fail("malformed site ordinal");
        }
        const auto ord = static_cast<uint32_t>(ordinal);
        if (f[0] == "site") {
            SiteFeedback site;
            site.ordinal = ord;
            const int kind = f.size() == 5 ? kindFromName(f[2]) : -1;
            if (kind < 0 || !parseU64(f[3], site.total)) return fail("malformed site row");
            site.kind = static_cast<il::FeedbackKind>(kind);
            std::string_view tags = f[4];
            while (!tags.empty()) {
                const size_t sp = tags.find(' ');
                const std::string_view item = tags.substr(0, sp);
                tags = sp == std::string_view::npos ? std::string_view() : tags.substr(sp + 1);
                const size_t eq = item.find('=');
                const int tag = eq == std::string_view::npos ? -1 : tagFromName(item.substr(0, eq));
                uint64_t n = 0;
                if (tag < 0 || !parseU64(item.substr(eq + 1), n)) {
                    return fail("malformed tag count '" + std::string(item) + "'");
                }
                site.tags[static_cast<uint32_t>(tag)] = n;
            }
            if (!fn->sites.emplace(ord, std::move(site)).second) return fail("site listed twice");
            continue;
        }
        if (f[0] == "shape" || f[0] == "target") {
            auto it = fn->sites.find(ord);
            uint64_t count = 0;
            std::string name;
            if (it == fn->sites.end()) return fail("a name row for a site not yet listed");
            if (f.size() != 4 || !parseU64(f[2], count) || !unescapeName(f[3], name) || name.empty()) {
                return fail("malformed " + std::string(f[0]) + " row");
            }
            auto& list = f[0] == "shape" ? it->second.shapes : it->second.targets;
            list.emplace_back(std::move(name), count);
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
    for (const auto& [key, fn] : functions_) {
        out += "fn\t" + escapeName(key) + "\t" + std::to_string(fn.entries) + "\n";
        for (const auto& [ordinal, site] : fn.sites) {
            const auto kind = static_cast<uint32_t>(site.kind);
            out += "site\t" + std::to_string(ordinal) + "\t" +
                   (kind < BRONZE_FEEDBACK_KIND_COUNT ? kKindNames[kind] : "?") + "\t" +
                   std::to_string(site.total) + "\t";
            bool first = true;
            for (uint32_t t = 0; t < BRONZE_FEEDBACK_TAG_COUNT; ++t) {
                if (site.tags[t] == 0) continue;
                if (!first) out += " ";
                first = false;
                out += std::string(kTagNames[t]) + "=" + std::to_string(site.tags[t]);
            }
            out += "\n";
            for (const auto& [name, count] : site.shapes) {
                out += "shape\t" + std::to_string(ordinal) + "\t" + std::to_string(count) + "\t" +
                       escapeName(name) + "\n";
            }
            for (const auto& [name, count] : site.targets) {
                out += "target\t" + std::to_string(ordinal) + "\t" + std::to_string(count) + "\t" +
                       escapeName(name) + "\n";
            }
        }
    }
    if (harvest_) {
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(harvest_->digest));
        out += "harvest\t" + std::string(hex) + "\t" + std::to_string(harvest_->sites) + "\n";
        for (const auto& [index, site] : harvest_->ics) {
            std::string flags;
            if (site.full) flags = "full";
            if (site.mega) flags += flags.empty() ? "mega" : ",mega";
            out += "ic\t" + std::to_string(index) + "\t" + (site.method ? "method" : "property") + "\t" +
                   (flags.empty() ? "-" : flags) + "\n";
            for (const HarvestWay& way : site.ways) {
                out += "way\t" + std::to_string(index) + "\t" + way.form + "\t" + escapeName(way.receiver);
                if (site.method) out += "\t" + escapeName(way.callee);
                out += "\n";
            }
        }
        for (const auto& [hash, count] : harvest_->samples) {
            char h[9];
            std::snprintf(h, sizeof h, "%08x", hash);
            out += "sample\t" + std::string(h) + "\t" + std::to_string(count) + "\n";
        }
    }
    return out;
}

const FunctionFeedback* TypeProfile::function(const std::string& fnKey) const {
    auto it = functions_.find(fnKey);
    return it == functions_.end() ? nullptr : &it->second;
}

const SiteFeedback* TypeProfile::site(const std::string& fnKey, uint32_t ordinal) const {
    const FunctionFeedback* fn = function(fnKey);
    if (fn == nullptr) return nullptr;
    auto it = fn->sites.find(ordinal);
    return it == fn->sites.end() ? nullptr : &it->second;
}

uint64_t TypeProfile::entryCount(const std::string& fnKey) const {
    const FunctionFeedback* fn = function(fnKey);
    return fn == nullptr ? 0 : fn->entries;
}

bool TypeProfile::isHot(const std::string& fnKey, uint64_t minEntries) const {
    return entryCount(fnKey) >= minEntries && entryCount(fnKey) != 0;
}

void TypeProfile::put(FunctionFeedback fn) {
    const std::string key = fn.key;
    functions_[key] = std::move(fn);
}

}  // namespace bronze::spec
