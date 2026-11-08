#include "check/package_audit.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bronze::check {
namespace {

namespace fs = std::filesystem;

std::string readFileString(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::string trimStr(std::string_view sv) {
    size_t b = 0;
    while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
    size_t e = sv.size();
    while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
    return std::string(sv.substr(b, e - b));
}

std::string toLowerAscii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return out;
}

std::string jsonEscapeStr(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string extractJsonStringField(const std::string& jsonText, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t pos = jsonText.find(pat);
    if (pos == std::string::npos) return {};
    size_t colon = jsonText.find(':', pos + pat.size());
    if (colon == std::string::npos) return {};
    size_t q1 = colon + 1;
    while (q1 < jsonText.size() && std::isspace(static_cast<unsigned char>(jsonText[q1]))) ++q1;
    if (q1 >= jsonText.size() || jsonText[q1] != '"') return {};
    size_t q2 = q1 + 1;
    while (q2 < jsonText.size() && jsonText[q2] != '"') {
        if (jsonText[q2] == '\\') ++q2;
        ++q2;
    }
    if (q2 >= jsonText.size()) return {};
    return jsonText.substr(q1 + 1, q2 - (q1 + 1));
}

DocValType classifyTypeToken(std::string_view rawType) {
    std::string t = toLowerAscii(trimStr(rawType));
    if (t.empty()) return DocValType::Unknown;
    if (t.find("[]") != std::string::npos || t.find("array") != std::string::npos ||
        t.find("uint8") != std::string::npos || t.find("int32array") != std::string::npos ||
        t.find("float32array") != std::string::npos || t.find("float64array") != std::string::npos ||
        t.find("buffer") != std::string::npos || t.find("vec") != std::string::npos ||
        t.find("mat") != std::string::npos || t.find("quat") != std::string::npos) {
        return DocValType::Array;
    }
    if (t.find("int") != std::string::npos || t.find("byte") != std::string::npos) {
        return DocValType::Integer;
    }
    if (t.find("number") != std::string::npos || t.find("float") != std::string::npos ||
        t.find("double") != std::string::npos) {
        return DocValType::Number;
    }
    if (t.find("string") != std::string::npos || t.find("char") != std::string::npos) {
        return DocValType::String;
    }
    if (t.find("bool") != std::string::npos) {
        return DocValType::Boolean;
    }
    if (t == "any" || t == "unknown" || t == "*") {
        return DocValType::Any;
    }
    return DocValType::Object;
}

void applyTextDomainRefinements(std::string_view commentText,
                                int32_t& ioMin,
                                int32_t& ioMax,
                                bool& ioHasExplicit,
                                bool* outSym = nullptr,
                                bool* outAnti = nullptr,
                                bool* outCmp = nullptr) {
    std::string low = toLowerAscii(commentText);
    if (low.find("non-negative") != std::string::npos ||
        low.find("nonnegative") != std::string::npos ||
        low.find("unsigned") != std::string::npos ||
        low.find(">= 0") != std::string::npos ||
        low.find("uint") != std::string::npos) {
        ioMin = std::max(ioMin, 0);
        ioHasExplicit = true;
    }
    if (low.find("positive") != std::string::npos ||
        low.find("> 0") != std::string::npos ||
        low.find(">= 1") != std::string::npos) {
        ioMin = std::max(ioMin, 1);
        ioHasExplicit = true;
    }
    if (low.find("0..255") != std::string::npos ||
        low.find("0-255") != std::string::npos ||
        low.find("0 to 255") != std::string::npos ||
        low.find("uint8") != std::string::npos ||
        low.find("single byte") != std::string::npos) {
        ioMin = 0;
        ioMax = 255;
        ioHasExplicit = true;
    }
    if (low.find("ascii") != std::string::npos ||
        low.find("0..127") != std::string::npos ||
        low.find("7-bit") != std::string::npos) {
        ioMin = 0;
        ioMax = 127;
        ioHasExplicit = true;
    }
    if (outSym && (low.find("commutative") != std::string::npos ||
                   low.find("symmetric") != std::string::npos ||
                   low.find("euclidean distance") != std::string::npos ||
                   low.find("dot product") != std::string::npos)) {
        *outSym = true;
    }
    if (outAnti && low.find("antisymmetric") != std::string::npos) {
        *outAnti = true;
    }
    if (outCmp && (low.find("-1, 0, or 1") != std::string::npos ||
                   low.find("-1, 0, 1") != std::string::npos ||
                   low.find("returns 0 if") != std::string::npos ||
                   low.find("negative if") != std::string::npos)) {
        *outCmp = true;
    }
}

void parseDtsSignatures(const std::string& content,
                        std::map<std::string, FunctionDocMeta>& outMeta) {
    std::istringstream iss(content);
    std::string line;
    std::string currentClass;
    std::string pendingComment;

    while (std::getline(iss, line)) {
        std::string trimmed = trimStr(line);
        if (trimmed.rfind("/**", 0) == 0 || trimmed.rfind("*", 0) == 0 || trimmed.rfind("//", 0) == 0) {
            pendingComment += " " + trimmed;
            continue;
        }
        auto classPos = trimmed.find("class ");
        if (classPos != std::string::npos) {
            size_t ns = classPos + 6;
            while (ns < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[ns]))) ++ns;
            size_t ne = ns;
            while (ne < trimmed.size() && (std::isalnum(static_cast<unsigned char>(trimmed[ne])) || trimmed[ne] == '_')) ++ne;
            if (ne > ns) currentClass = trimmed.substr(ns, ne - ns);
        }
        auto ifacePos = trimmed.find("interface ");
        if (ifacePos != std::string::npos) {
            size_t ns = ifacePos + 10;
            while (ns < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[ns]))) ++ns;
            size_t ne = ns;
            while (ne < trimmed.size() && (std::isalnum(static_cast<unsigned char>(trimmed[ne])) || trimmed[ne] == '_')) ++ne;
            if (ne > ns) currentClass = trimmed.substr(ns, ne - ns);
        }
        if (trimmed == "}") {
            currentClass.clear();
            pendingComment.clear();
            continue;
        }

        size_t lp = trimmed.find('(');
        size_t rp = (lp != std::string::npos) ? trimmed.find(')', lp + 1) : std::string::npos;
        if (lp == std::string::npos || rp == std::string::npos || lp == 0) {
            if (!trimmed.empty() && trimmed.back() == ';') pendingComment.clear();
            continue;
        }
        size_t nameEnd = lp;
        while (nameEnd > 0 && (trimmed[nameEnd - 1] == ' ' || trimmed[nameEnd - 1] == '<' || trimmed[nameEnd - 1] == '>')) {
            if (trimmed[nameEnd - 1] == '>') {
                while (nameEnd > 0 && trimmed[nameEnd - 1] != '<') --nameEnd;
                if (nameEnd > 0) --nameEnd;
            } else {
                --nameEnd;
            }
        }
        size_t nameStart = nameEnd;
        while (nameStart > 0 && (std::isalnum(static_cast<unsigned char>(trimmed[nameStart - 1])) ||
                                 trimmed[nameStart - 1] == '_' || trimmed[nameStart - 1] == '$')) {
            --nameStart;
        }
        if (nameStart == nameEnd) continue;
        std::string fnName = trimmed.substr(nameStart, nameEnd - nameStart);
        if (fnName == "if" || fnName == "for" || fnName == "while" || fnName == "switch" ||
            fnName == "catch" || fnName == "constructor" || fnName == "new") {
            pendingComment.clear();
            continue;
        }

        FunctionDocMeta meta;
        meta.name = fnName;
        if (trimmed.rfind("private ", 0) == 0 || trimmed.rfind("protected ", 0) == 0 ||
            pendingComment.find("@private") != std::string::npos ||
            pendingComment.find("@internal") != std::string::npos) {
            meta.isPrivate = true;
        } else {
            meta.isExported = true;
        }
        applyTextDomainRefinements(pendingComment, meta.domainMin, meta.domainMax, meta.hasExplicitRange,
                                   &meta.documentedSymmetric, &meta.documentedAntisymmetric, &meta.documentedCompare);

        std::string paramList = trimmed.substr(lp + 1, rp - (lp + 1));
        size_t start = 0;
        while (start < paramList.size()) {
            size_t comma = paramList.find(',', start);
            if (comma == std::string::npos) comma = paramList.size();
            std::string item = trimStr(paramList.substr(start, comma - start));
            start = comma + 1;
            if (item.empty()) continue;
            size_t colon = item.find(':');
            ParamDocMeta pm;
            if (colon != std::string::npos) {
                pm.name = trimStr(item.substr(0, colon));
                if (!pm.name.empty() && pm.name.back() == '?') {
                    pm.optional = true;
                    pm.name.pop_back();
                }
                pm.type = classifyTypeToken(item.substr(colon + 1));
            } else {
                pm.name = item;
            }
            pm.domainMin = meta.domainMin;
            pm.domainMax = meta.domainMax;
            pm.hasExplicitRange = meta.hasExplicitRange;
            if (pm.name != "this") meta.params.push_back(std::move(pm));
        }

        size_t retColon = trimmed.find(':', rp + 1);
        if (retColon != std::string::npos) {
            std::string retStr = trimStr(trimmed.substr(retColon + 1));
            if (!retStr.empty() && retStr.back() == ';') retStr.pop_back();
            meta.returnType = classifyTypeToken(retStr);
        }

        outMeta[fnName] = meta;
        if (!currentClass.empty()) {
            outMeta[currentClass + "." + fnName] = meta;
        }
        pendingComment.clear();
    }
}

void parseJsDocComments(const std::string& content,
                        std::map<std::string, FunctionDocMeta>& outMeta) {
    size_t pos = 0;
    while (true) {
        size_t cStart = content.find("/**", pos);
        if (cStart == std::string::npos) break;
        size_t cEnd = content.find("*/", cStart + 3);
        if (cEnd == std::string::npos) break;
        std::string comment = content.substr(cStart + 3, cEnd - (cStart + 3));
        pos = cEnd + 2;

        size_t nextLineEnd = content.find('\n', pos);
        if (nextLineEnd != std::string::npos) nextLineEnd = content.find('\n', nextLineEnd + 1);
        std::string after = content.substr(pos, (nextLineEnd == std::string::npos) ? 240 : std::min<size_t>(240, nextLineEnd - pos + 120));

        std::string fnName;
        size_t fnKw = after.find("function ");
        if (fnKw != std::string::npos) {
            size_t ns = fnKw + 9;
            while (ns < after.size() && std::isspace(static_cast<unsigned char>(after[ns]))) ++ns;
            size_t ne = ns;
            while (ne < after.size() && (std::isalnum(static_cast<unsigned char>(after[ne])) || after[ne] == '_')) ++ne;
            if (ne > ns) fnName = after.substr(ns, ne - ns);
        }
        if (fnName.empty()) {
            size_t eqFn = after.find("= function");
            if (eqFn != std::string::npos && eqFn > 0) {
                size_t ne = eqFn;
                while (ne > 0 && std::isspace(static_cast<unsigned char>(after[ne - 1]))) --ne;
                size_t ns = ne;
                while (ns > 0 && (std::isalnum(static_cast<unsigned char>(after[ns - 1])) || after[ns - 1] == '_' || after[ns - 1] == '.')) --ns;
                if (ne > ns) {
                    fnName = after.substr(ns, ne - ns);
                    auto dot = fnName.rfind('.');
                    if (dot != std::string::npos) fnName = fnName.substr(dot + 1);
                }
            }
        }
        if (fnName.empty()) continue;

        FunctionDocMeta& meta = outMeta[fnName];
        if (meta.name.empty()) meta.name = fnName;
        if (comment.find("@private") != std::string::npos ||
            comment.find("@internal") != std::string::npos) {
            meta.isPrivate = true;
        } else if (comment.find("@public") != std::string::npos ||
                   comment.find("@memberOf") != std::string::npos ||
                   comment.find("@memberof") != std::string::npos ||
                   comment.find("@exports") != std::string::npos) {
            meta.isExported = true;
        }
        applyTextDomainRefinements(comment, meta.domainMin, meta.domainMax, meta.hasExplicitRange,
                                   &meta.documentedSymmetric, &meta.documentedAntisymmetric, &meta.documentedCompare);

        size_t pPos = 0;
        while ((pPos = comment.find("@param", pPos)) != std::string::npos) {
            pPos += 6;
            size_t lb = comment.find('{', pPos);
            size_t rb = (lb != std::string::npos) ? comment.find('}', lb + 1) : std::string::npos;
            size_t nl = comment.find('\n', pPos);
            if (lb != std::string::npos && rb != std::string::npos && (nl == std::string::npos || rb < nl)) {
                DocValType pt = classifyTypeToken(comment.substr(lb + 1, rb - (lb + 1)));
                std::string rest = trimStr(comment.substr(rb + 1, (nl == std::string::npos ? comment.size() : nl) - (rb + 1)));
                size_t sp = 0;
                while (sp < rest.size() && !std::isspace(static_cast<unsigned char>(rest[sp]))) ++sp;
                std::string pName = rest.substr(0, sp);
                bool opt = (!pName.empty() && pName.front() == '[');
                if (opt && pName.size() >= 2) pName = pName.substr(1, pName.size() - 2);
                ParamDocMeta pm;
                pm.name = pName;
                pm.type = pt;
                pm.optional = opt;
                pm.domainMin = meta.domainMin;
                pm.domainMax = meta.domainMax;
                pm.hasExplicitRange = meta.hasExplicitRange;
                applyTextDomainRefinements(rest, pm.domainMin, pm.domainMax, pm.hasExplicitRange);
                if (pm.hasExplicitRange) {
                    meta.domainMin = std::max(meta.domainMin, pm.domainMin);
                    meta.domainMax = std::min(meta.domainMax, pm.domainMax);
                    meta.hasExplicitRange = true;
                }
                meta.params.push_back(std::move(pm));
            }
        }
        size_t rPos = comment.find("@return");
        if (rPos != std::string::npos) {
            size_t lb = comment.find('{', rPos);
            size_t rb = (lb != std::string::npos) ? comment.find('}', lb + 1) : std::string::npos;
            if (lb != std::string::npos && rb != std::string::npos && rb - lb < 64) {
                meta.returnType = classifyTypeToken(comment.substr(lb + 1, rb - (lb + 1)));
            }
            std::string retDesc = toLowerAscii(comment.substr(rPos));
            if (retDesc.find("in the range `0` to") != std::string::npos ||
                retDesc.find("in the range 0 to") != std::string::npos ||
                retDesc.find("non-negative") != std::string::npos) {
                meta.hasReturnMin = true;
                meta.returnMin = 0;
            }
        }
    }

    // Scan source lines for ESM/CommonJS export declarations to mark exported functions.
    std::istringstream ess(content);
    std::string eline;
    while (std::getline(ess, eline)) {
        std::string t = trimStr(eline);
        auto markExported = [&](const std::string& id) {
            if (id.empty() || id == "default" || id == "function" || id == "class" ||
                id == "const" || id == "let" || id == "var") {
                return;
            }
            FunctionDocMeta& m = outMeta[id];
            if (m.name.empty()) m.name = id;
            if (!m.isPrivate) m.isExported = true;
        };
        for (const char* kw : {"export function ", "export const ", "export let ", "export var ", "export default function "}) {
            if (t.rfind(kw, 0) == 0) {
                size_t ns = std::strlen(kw);
                while (ns < t.size() && std::isspace(static_cast<unsigned char>(t[ns]))) ++ns;
                size_t ne = ns;
                while (ne < t.size() && (std::isalnum(static_cast<unsigned char>(t[ne])) || t[ne] == '_' || t[ne] == '$')) ++ne;
                if (ne > ns) markExported(t.substr(ns, ne - ns));
            }
        }
        if (t.rfind("export {", 0) == 0 || t.rfind("export{", 0) == 0) {
            size_t lb = t.find('{');
            size_t rb = t.find('}', lb + 1);
            if (lb != std::string::npos && rb != std::string::npos) {
                std::string list = t.substr(lb + 1, rb - (lb + 1));
                size_t s = 0;
                while (s < list.size()) {
                    size_t c = list.find(',', s);
                    if (c == std::string::npos) c = list.size();
                    std::string item = trimStr(list.substr(s, c - s));
                    s = c + 1;
                    auto asPos = item.find(" as ");
                    if (asPos != std::string::npos) {
                        markExported(trimStr(item.substr(0, asPos)));
                        markExported(trimStr(item.substr(asPos + 4)));
                    } else {
                        markExported(item);
                    }
                }
            }
        }
        for (const char* cjs : {"exports.", "module.exports."}) {
            auto cpos = t.find(cjs);
            if (cpos != std::string::npos) {
                size_t ns = cpos + std::strlen(cjs);
                size_t ne = ns;
                while (ne < t.size() && (std::isalnum(static_cast<unsigned char>(t[ne])) || t[ne] == '_' || t[ne] == '$')) ++ne;
                if (ne > ns) markExported(t.substr(ns, ne - ns));
            }
        }
    }
}

}  // namespace

const char* docValTypeName(DocValType t) {
    switch (t) {
        case DocValType::Unknown: return "unknown";
        case DocValType::Number: return "number";
        case DocValType::Integer: return "int32";
        case DocValType::String: return "string(<=8)";
        case DocValType::Boolean: return "boolean";
        case DocValType::Array: return "array(<=8)";
        case DocValType::Object: return "object";
        case DocValType::Any: return "any";
    }
    return "unknown";
}

bool discoverPackageAuditTarget(const std::string& targetPath,
                                std::string& outPkgName,
                                std::string& outPkgVersion,
                                std::vector<std::string>& outEntryFiles,
                                std::map<std::string, FunctionDocMeta>& outDocMeta,
                                std::string& outError) {
    outEntryFiles.clear();
    outDocMeta.clear();
    std::error_code ec;
    fs::path root(targetPath);
    if (!fs::exists(root, ec)) {
        outError = "target path does not exist: " + targetPath;
        return false;
    }

    if (fs::is_regular_file(root, ec)) {
        outPkgName = root.stem().string();
        outPkgVersion = "local";
        outEntryFiles.push_back(root.generic_string());
        std::string src = readFileString(root);
        parseJsDocComments(src, outDocMeta);
        fs::path dts = root;
        dts.replace_extension(".d.ts");
        if (fs::exists(dts, ec)) {
            parseDtsSignatures(readFileString(dts), outDocMeta);
        }
        return true;
    }

    outPkgName = root.filename().string();
    outPkgVersion = "0.0.0";
    fs::path pkgJsonPath = root / "package.json";
    std::vector<std::string> candidateRelPaths;
    if (fs::exists(pkgJsonPath, ec)) {
        std::string pkgText = readFileString(pkgJsonPath);
        std::string n = extractJsonStringField(pkgText, "name");
        std::string v = extractJsonStringField(pkgText, "version");
        if (!n.empty()) outPkgName = n;
        if (!v.empty()) outPkgVersion = v;

        for (const char* field : {"module", "jsnext:main", "import", "main", "browser"}) {
            std::string rel = extractJsonStringField(pkgText, field);
            if (!rel.empty()) candidateRelPaths.push_back(rel);
        }
        for (const char* tfield : {"types", "typings"}) {
            std::string trel = extractJsonStringField(pkgText, tfield);
            if (!trel.empty()) {
                fs::path tp = root / trel;
                if (fs::exists(tp, ec) && fs::is_regular_file(tp, ec)) {
                    parseDtsSignatures(readFileString(tp), outDocMeta);
                }
            }
        }
    }

    static const char* const kFallbackEntries[] = {
        "esm/index.js", "src/index.js", "dist/index.mjs", "dist/index.js",
        "lib/index.js", "index.mjs", "index.js"
    };
    for (const char* fb : kFallbackEntries) candidateRelPaths.push_back(fb);

    auto addFileIfExists = [&](const fs::path& p) {
        if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec)) return;
        std::string s = p.generic_string();
        if (std::find(outEntryFiles.begin(), outEntryFiles.end(), s) == outEntryFiles.end()) {
            outEntryFiles.push_back(s);
        }
    };

    for (const std::string& rel : candidateRelPaths) {
        fs::path p = root / rel;
        if (fs::exists(p, ec) && fs::is_directory(p, ec)) {
            addFileIfExists(p / "index.js");
            addFileIfExists(p / "index.mjs");
        } else {
            addFileIfExists(p);
        }
    }

    // Also scan sub-modules in esm/ or src/ or root if the package is a multi-module directory.
    for (const char* subdir : {"esm", "src", "."}) {
        fs::path sd = ( std::string(subdir) == "." ) ? root : (root / subdir);
        if (!fs::exists(sd, ec) || !fs::is_directory(sd, ec)) continue;
        std::vector<fs::path> dirFiles;
        for (const auto& entry : fs::directory_iterator(sd, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            std::string ext = entry.path().extension().string();
            std::string fname = entry.path().filename().string();
            if (fname.find(".min.") != std::string::npos || fname.find(".test.") != std::string::npos ||
                fname.find(".spec.") != std::string::npos || fname.find("rollup") != std::string::npos ||
                fname.find("webpack") != std::string::npos || fname.find("gulpfile") != std::string::npos) {
                continue;
            }
            if (fname.size() > 5 && fname.substr(fname.size() - 5) == ".d.ts") {
                parseDtsSignatures(readFileString(entry.path()), outDocMeta);
                continue;
            }
            if (ext == ".js" || ext == ".mjs") {
                dirFiles.push_back(entry.path());
            }
        }
        std::sort(dirFiles.begin(), dirFiles.end());
        for (const auto& df : dirFiles) {
            if (outEntryFiles.size() < 16) addFileIfExists(df);
        }
    }

    for (const std::string& ef : outEntryFiles) {
        parseJsDocComments(readFileString(ef), outDocMeta);
    }

    if (outEntryFiles.empty()) {
        outError = "no JS entry files found in: " + targetPath;
        return false;
    }
    return true;
}

std::string PackageAuditReport::renderText() const {
    std::ostringstream os;
    os << "=== bronze audit: " << packageName << "@" << packageVersion;
    if (isDiffMode) os << " (diff -> " << secondEntryPath << ")";
    os << " ===\n";
    double pct = (totalPublicFunctions > 0)
                     ? (100.0 * static_cast<double>(analyzedFunctions) / static_cast<double>(totalPublicFunctions))
                     : 0.0;
    os << "Analyzed functions: " << analyzedFunctions << " / " << totalPublicFunctions
       << " (" << static_cast<int>(pct + 0.5) << "%)\n";
    os << "Verdicts: EQUIVALENT=" << equivalentCount
       << ", BOUNDED=" << boundedCount
       << ", COUNTEREXAMPLE=" << counterexampleCount
       << ", PRECONDITION=" << preconditionBucketCount
       << ", UNKNOWN=" << unknownCount << "\n";

    if (!unknownReasons.empty()) {
        os << "Unknown reasons:\n";
        for (const auto& [reason, cnt] : unknownReasons) {
            os << "  [" << cnt << "x] " << reason << "\n";
        }
    }
    for (const auto& c : checks) {
        if (c.inPreconditionBucket) continue;
        os << "  " << c.functionName << " [" << c.propertyKind << "] "
           << c.formalProperty << " -> " << equivStatusTag(c.status);
        if (c.status == EquivStatus::Bounded && c.bounds.hasBounds()) {
            os << " (loop<=" << c.bounds.maxLoopIterations
               << ", arrayLen<=" << c.bounds.maxArrayLength
               << ", stringLen<=" << c.bounds.maxStringLength << ")";
        } else if (c.status == EquivStatus::Counterexample) {
            os << " (";
            for (size_t i = 0; i < c.counterexampleArgs.size(); ++i) {
                if (i > 0) os << ", ";
                std::string pn = (i < c.paramNames.size()) ? c.paramNames[i] : ("arg_" + std::to_string(i));
                os << pn << "=" << c.counterexampleArgs[i];
            }
            os << " => expected=" << (c.oldThrows ? std::string("throws") : std::to_string(c.expectedValue))
               << ", actual=" << (c.newThrows ? std::string("throws") : std::to_string(c.actualValue)) << ")";
        } else if (c.status == EquivStatus::Unknown && !c.detail.empty()) {
            os << " (" << c.detail << ")";
        }
        os << "\n";
    }
    if (internalHelperCount > 0) {
        os << "Internal helpers (reported separately): " << internalAnalyzedCount
           << " / " << internalHelperCount << " analyzed, COUNTEREXAMPLE="
           << internalCounterexampleCount << "\n";
        for (const auto& c : internalChecks) {
            if (c.status != EquivStatus::Counterexample) continue;
            os << "  [internal-helper] " << c.functionName << " [" << c.propertyKind << "] "
               << c.formalProperty << " -> COUNTEREXAMPLE (";
            for (size_t i = 0; i < c.counterexampleArgs.size(); ++i) {
                if (i > 0) os << ", ";
                std::string pn = (i < c.paramNames.size()) ? c.paramNames[i] : ("arg_" + std::to_string(i));
                os << pn << "=" << c.counterexampleArgs[i];
            }
            os << " => expected=" << (c.oldThrows ? std::string("throws") : std::to_string(c.expectedValue))
               << ", actual=" << (c.newThrows ? std::string("throws") : std::to_string(c.actualValue)) << ")\n";
        }
    }
    return os.str();
}

std::string PackageAuditReport::renderJson() const {
    std::ostringstream os;
    os << "{\"package\":\"" << jsonEscapeStr(packageName) << "\","
       << "\"version\":\"" << jsonEscapeStr(packageVersion) << "\","
       << "\"entry\":\"" << jsonEscapeStr(entryPath) << "\","
       << "\"diffMode\":" << (isDiffMode ? "true" : "false") << ","
       << "\"totalPublicFunctions\":" << totalPublicFunctions << ","
       << "\"analyzedFunctions\":" << analyzedFunctions << ","
       << "\"equivalentCount\":" << equivalentCount << ","
       << "\"boundedCount\":" << boundedCount << ","
       << "\"counterexampleCount\":" << counterexampleCount << ","
       << "\"preconditionBucketCount\":" << preconditionBucketCount << ","
       << "\"unknownCount\":" << unknownCount << ","
       << "\"internalHelperCount\":" << internalHelperCount << ","
       << "\"internalAnalyzedCount\":" << internalAnalyzedCount << ","
       << "\"internalCounterexampleCount\":" << internalCounterexampleCount << ","
       << "\"unknownReasons\":{";
    bool firstR = true;
    for (const auto& [reason, cnt] : unknownReasons) {
        if (!firstR) os << ",";
        firstR = false;
        os << "\"" << jsonEscapeStr(reason) << "\":" << cnt;
    }
    os << "},\"checks\":[";
    for (size_t i = 0; i < checks.size(); ++i) {
        if (i > 0) os << ",";
        const auto& c = checks[i];
        os << "{\"function\":\"" << jsonEscapeStr(c.functionName) << "\","
           << "\"kind\":\"" << jsonEscapeStr(c.propertyKind) << "\","
           << "\"property\":\"" << jsonEscapeStr(c.formalProperty) << "\","
           << "\"status\":\"" << equivStatusTag(c.status) << "\","
           << "\"preconditionBucket\":" << (c.inPreconditionBucket ? "true" : "false") << ","
           << "\"detail\":\"" << jsonEscapeStr(c.detail) << "\"";
        if (c.status == EquivStatus::Bounded || c.bounds.hasBounds()) {
            os << ",\"bounds\":{\"loopTripCount\":" << c.bounds.maxLoopIterations
               << ",\"arrayLength\":" << c.bounds.maxArrayLength
               << ",\"stringLength\":" << c.bounds.maxStringLength << "}";
        }
        if (c.status == EquivStatus::Counterexample) {
            os << ",\"counterexample\":{\"paramNames\":[";
            for (size_t j = 0; j < c.paramNames.size(); ++j) {
                if (j > 0) os << ",";
                os << "\"" << jsonEscapeStr(c.paramNames[j]) << "\"";
            }
            os << "],\"args\":[";
            for (size_t j = 0; j < c.counterexampleArgs.size(); ++j) {
                if (j > 0) os << ",";
                os << c.counterexampleArgs[j];
            }
            os << "],\"expected\":" << c.expectedValue
               << ",\"actual\":" << c.actualValue
               << ",\"oldThrows\":" << (c.oldThrows ? "true" : "false")
               << ",\"newThrows\":" << (c.newThrows ? "true" : "false") << "}";
        }
        os << "}";
    }
    os << "],\"internalChecks\":[";
    for (size_t i = 0; i < internalChecks.size(); ++i) {
        if (i > 0) os << ",";
        const auto& c = internalChecks[i];
        os << "{\"function\":\"" << jsonEscapeStr(c.functionName) << "\","
           << "\"kind\":\"" << jsonEscapeStr(c.propertyKind) << "\","
           << "\"property\":\"" << jsonEscapeStr(c.formalProperty) << "\","
           << "\"status\":\"" << equivStatusTag(c.status) << "\","
           << "\"detail\":\"" << jsonEscapeStr(c.detail) << "\"";
        if (c.status == EquivStatus::Counterexample) {
            os << ",\"counterexample\":{\"paramNames\":[";
            for (size_t j = 0; j < c.paramNames.size(); ++j) {
                if (j > 0) os << ",";
                os << "\"" << jsonEscapeStr(c.paramNames[j]) << "\"";
            }
            os << "],\"args\":[";
            for (size_t j = 0; j < c.counterexampleArgs.size(); ++j) {
                if (j > 0) os << ",";
                os << c.counterexampleArgs[j];
            }
            os << "],\"expected\":" << c.expectedValue
               << ",\"actual\":" << c.actualValue
               << ",\"oldThrows\":" << (c.oldThrows ? "true" : "false")
               << ",\"newThrows\":" << (c.newThrows ? "true" : "false") << "}";
        }
        os << "}";
    }
    os << "]}\n";
    return os.str();
}

}  // namespace bronze::check
