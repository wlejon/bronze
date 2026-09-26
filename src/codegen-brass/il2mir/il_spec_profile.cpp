#include "il_spec_profile.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <istream>
#include <mutex>
#include <ostream>
#include <sstream>
#include <unordered_set>

namespace il2mir {

namespace {

std::string encode(std::string_view s) {
    if (s.empty()) return "%";
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c <= 0x20 || c >= 0x7f || c == '%') {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

bool decode(std::string_view s, std::string& out) {
    out.clear();
    if (s == "%") return true;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size()) return false;
        const int hi = hex(s[i + 1]);
        const int lo = hex(s[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>((hi << 4) | lo);
        i += 2;
    }
    return true;
}

struct DumpRegistry {
    std::mutex mutex;
    std::string path;
    std::vector<std::shared_ptr<SpecFeedback>> programs;
};

DumpRegistry& dump_registry() {
    static DumpRegistry* r = new DumpRegistry();  // outlives every static destructor
    return *r;
}

void write_dump_at_exit() {
    DumpRegistry& r = dump_registry();
    std::vector<std::vector<SpecFeedback::SiteRecord>> programs;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        for (const auto& fb : r.programs) programs.push_back(fb->snapshot());
    }
    std::ofstream out(r.path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "bronze: cannot write the speculation profile to %s\n", r.path.c_str());
        return;
    }
    write_spec_profile(out, programs);
}

} // namespace

std::string spec_site_key(std::string_view fn, SpecKind kind, std::string_view tag) {
    std::string key(fn);
    key += '\x1f';
    key += std::to_string(static_cast<uint32_t>(kind));
    key += '\x1f';
    key += tag;
    return key;
}

std::shared_ptr<const SpecProfile> SpecProfile::read_file(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "error: cannot read speculation profile " + path + "\n";
        return nullptr;
    }
    auto profile = parse(in, err);
    if (!profile) err = "error: " + path + ": " + err + "\n";
    return profile;
}

std::shared_ptr<const SpecProfile> SpecProfile::parse(std::istream& in, std::string& err) {
    std::string line;
    if (!std::getline(in, line)) {
        err = "empty speculation profile";
        return nullptr;
    }
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::string header = "bronze-spec-profile ";
    if (line.rfind(header, 0) != 0) {
        err = "not a bronze speculation profile";
        return nullptr;
    }
    if (line.substr(header.size()) != std::to_string(kSpecProfileVersion)) {
        err = "speculation profile version " + line.substr(header.size()) + ", this bronze reads version " +
              std::to_string(kSpecProfileVersion);
        return nullptr;
    }
    auto profile = std::make_shared<SpecProfile>();
    std::unordered_set<std::string> earlier_fns;  // named by a previous program
    std::unordered_set<std::string> this_fns;
    size_t lineno = 1;
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line == "program") {
            earlier_fns.insert(this_fns.begin(), this_fns.end());
            this_fns.clear();
            continue;
        }
        std::istringstream fields(line);
        std::string tag_s, fn_enc, tag_enc;
        uint32_t kind = 0;
        uint64_t misses = 0;
        if (!(fields >> tag_s >> kind >> misses >> fn_enc >> tag_enc) || tag_s != "s" ||
            kind > static_cast<uint32_t>(SpecKind::PolyProperty)) {
            err = "line " + std::to_string(lineno) + ": malformed site";
            return nullptr;
        }
        std::string fn, tag;
        if (!decode(fn_enc, fn) || !decode(tag_enc, tag)) {
            err = "line " + std::to_string(lineno) + ": malformed encoding";
            return nullptr;
        }
        if (earlier_fns.count(fn)) continue;
        this_fns.insert(fn);
        profile->sites_[spec_site_key(fn, static_cast<SpecKind>(kind), tag)].push_back(
            static_cast<uint32_t>(misses > UINT32_MAX ? UINT32_MAX : misses));
        ++profile->site_count_;
    }
    return profile;
}

std::optional<uint32_t> SpecProfile::misses(std::string_view fn, SpecKind kind, std::string_view tag,
                                            uint32_t n) const {
    auto it = sites_.find(spec_site_key(fn, kind, tag));
    if (it == sites_.end() || n >= it->second.size()) return std::nullopt;
    return it->second[n];
}

void write_spec_profile(std::ostream& out, const std::vector<std::vector<SpecFeedback::SiteRecord>>& programs) {
    out << "bronze-spec-profile " << kSpecProfileVersion << '\n';
    for (const auto& sites : programs) {
        out << "program\n";
        for (const auto& s : sites) {
            out << "s " << static_cast<uint32_t>(s.kind) << ' ' << s.misses << ' ' << encode(s.fn) << ' '
                << encode(s.tag) << '\n';
        }
    }
}

void register_spec_profile_dump(std::shared_ptr<SpecFeedback> feedback) {
    static const char* path = std::getenv("BRONZE_SPEC_PROFILE_OUT");
    if (!path || !*path || !feedback) return;
    DumpRegistry& r = dump_registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    if (r.path.empty()) {
        r.path = path;
        std::atexit(write_dump_at_exit);
    }
    r.programs.push_back(std::move(feedback));
}

} // namespace il2mir
