#include "modules/modules.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "json/json.h"

namespace bronze::modules {

namespace {

json::Units toUnits(std::string_view utf8) {
    json::Units out;
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0;
        size_t extra = 0;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            extra = 3;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + extra >= utf8.size()) {
            out.push_back(0xFFFD);
            break;
        }
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        i += extra + 1;
        if (cp > 0x10FFFF) {
            out.push_back(0xFFFD);
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string toUtf8(const json::Units& units) {
    std::string out;
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[++i] - 0xDC00);
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

bool loadImportMap(const std::filesystem::path& path, std::vector<ModuleRoot>& outRoots,
                   std::string& err) {
    std::string text;
    if (!readFile(path, text)) {
        err = "cannot read import map " + path.string();
        return false;
    }

    std::string jsonError;
    json::ValuePtr rootJson = json::parse(toUnits(text), jsonError);
    if (!rootJson) {
        err = path.string() + " is not valid JSON: " + jsonError;
        return false;
    }

    if (rootJson->kind != json::Value::Kind::Object) {
        err = path.string() + " is not a JSON object";
        return false;
    }

    const json::Value* imports = nullptr;
    for (const auto& m : rootJson->members) {
        if (toUtf8(m.key) == "imports") {
            imports = m.value.get();
            break;
        }
    }

    if (!imports) {
        return true;
    }

    if (imports->kind != json::Value::Kind::Object) {
        err = "\"imports\" in " + path.string() + " must be a JSON object";
        return false;
    }

    std::error_code ec;
    std::filesystem::path mapDir = std::filesystem::weakly_canonical(path, ec).parent_path();
    if (ec || mapDir.empty()) {
        mapDir = std::filesystem::current_path();
    }

    for (const auto& m : imports->members) {
        std::string key = toUtf8(m.key);
        if (key.empty()) {
            err = "empty key in \"imports\" in " + path.string();
            return false;
        }
        if (!m.value || m.value->kind != json::Value::Kind::String) {
            err = "\"imports\" mapping for \"" + key + "\" in " + path.string() +
                  " must be a string";
            return false;
        }
        std::string targetStr = toUtf8(m.value->text);
        std::filesystem::path targetPath(targetStr);
        std::filesystem::path resolvedTarget;
        if (targetPath.is_absolute()) {
            resolvedTarget = targetPath;
        } else {
            resolvedTarget = mapDir / targetPath;
        }
        std::filesystem::path canonical = std::filesystem::weakly_canonical(resolvedTarget, ec);
        if (!ec) {
            resolvedTarget = std::move(canonical);
        }
        outRoots.push_back({std::move(key), std::move(resolvedTarget)});
    }

    return true;
}

}  // namespace bronze::modules
