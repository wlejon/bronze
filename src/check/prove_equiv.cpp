#include "check/prove_equiv.h"

#include <sstream>
#include <string>
#include <vector>

#include "lex/lexer.h"
#include "lower/lower.h"
#include "modules/modules.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/infer.h"

namespace bronze::check {
namespace {

bool functionMatchesName(const il::Function& fn, std::string_view query) {
    if (fn.name == query) return true;
    auto pos = fn.name.rfind("::");
    if (pos != std::string::npos && std::string_view(fn.name).substr(pos + 2) == query) {
        return true;
    }
    return false;
}

std::string jsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 4);
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

bool lowerFileToModule(const std::string& path,
                       const std::vector<modules::ModuleRoot>& moduleRoots,
                       const std::string& importMapPath,
                       il::Module& outMod,
                       std::string& errorMsg) {
    SourceSet sources;
    DiagnosticSink diags;
    auto astMod = modules::loadProgram(path, sources, diags, {moduleRoots, importMapPath});
    if (diags.hasErrors() || !astMod) {
        errorMsg = diags.render(sources);
        return false;
    }
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) {
        errorMsg = diags.render(sources);
        return false;
    }
    auto lowered = lower::lowerModule(*astMod, diags, &*inferred, nullptr, &sources);
    if (diags.hasErrors() || !lowered) {
        errorMsg = diags.render(sources);
        return false;
    }
    outMod = std::move(*lowered);
    return true;
}

}  // namespace

bool lowerSingleSourceForEquiv(std::string_view src,
                               std::string_view virtualName,
                               il::Module& outMod,
                               std::string* errorOut) {
    SourceSet sources;
    const auto& buf = sources.add(std::string(virtualName), std::string(src));
    DiagnosticSink diags;
    auto tokens = Lexer(buf, diags).lex();
    if (diags.hasErrors()) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    auto astMod = Parser(std::move(tokens), diags).parseModule(std::string(virtualName));
    if (diags.hasErrors() || !astMod) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    auto inferred = types::inferModule(*astMod, diags);
    if (diags.hasErrors() || !inferred) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    auto lowered = lower::lowerModule(*astMod, diags, &*inferred, nullptr, &sources);
    if (diags.hasErrors() || !lowered) {
        if (errorOut) *errorOut = diags.render(buf);
        return false;
    }
    outMod = std::move(*lowered);
    return true;
}

const char* equivStatusTag(EquivStatus status) {
    switch (status) {
        case EquivStatus::Equivalent: return "EQUIVALENT";
        case EquivStatus::Counterexample: return "COUNTEREXAMPLE";
        case EquivStatus::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

bool ProveEquivReport::allEquivalent() const {
    if (functions.empty()) return false;
    for (const auto& f : functions) {
        if (f.status != EquivStatus::Equivalent) return false;
    }
    return true;
}

bool ProveEquivReport::hasCounterexample() const {
    for (const auto& f : functions) {
        if (f.status == EquivStatus::Counterexample) return true;
    }
    return false;
}

std::string ProveEquivReport::renderText() const {
    std::ostringstream os;
    for (const auto& f : functions) {
        os << f.name << ": " << equivStatusTag(f.status);
        if (f.status == EquivStatus::Counterexample) {
            os << " (";
            for (size_t i = 0; i < f.counterexampleArgs.size(); ++i) {
                if (i > 0) os << ", ";
                std::string pname = (i < f.paramNames.size()) ? f.paramNames[i] : ("arg_" + std::to_string(i));
                os << pname << " = " << f.counterexampleArgs[i];
            }
            os << ") -> old = " << f.oldReturnValue << ", new = " << f.newReturnValue;
        } else if (f.status == EquivStatus::Unknown && !f.detail.empty()) {
            os << " (" << f.detail << ")";
        }
        os << "\n";
    }
    return os.str();
}

std::string ProveEquivReport::renderJson() const {
    std::ostringstream os;
    os << "{\"functions\":[";
    for (size_t i = 0; i < functions.size(); ++i) {
        if (i > 0) os << ",";
        const auto& f = functions[i];
        os << "{\"name\":\"" << jsonEscape(f.name) << "\","
           << "\"status\":\"" << equivStatusTag(f.status) << "\","
           << "\"detail\":\"" << jsonEscape(f.detail) << "\"";
        if (f.status == EquivStatus::Counterexample) {
            os << ",\"counterexample\":{\"args\":[";
            for (size_t j = 0; j < f.counterexampleArgs.size(); ++j) {
                if (j > 0) os << ",";
                os << f.counterexampleArgs[j];
            }
            os << "],\"oldReturn\":" << f.oldReturnValue
               << ",\"newReturn\":" << f.newReturnValue << "}";
        }
        os << "}";
    }
    os << "],\"allEquivalent\":" << (allEquivalent() ? "true" : "false") << "}\n";
    return os.str();
}

ProveEquivReport proveModuleEquiv(const il::Module& oldMod,
                                  const il::Module& newMod,
                                  const std::string& fnFilter) {
    ProveEquivReport report;

    if (!fnFilter.empty()) {
        const il::Function* oldFn = nullptr;
        const il::Function* newFn = nullptr;
        for (const auto& f : oldMod.functions) {
            if (!f.isExternal && functionMatchesName(f, fnFilter)) {
                oldFn = &f;
                break;
            }
        }
        for (const auto& f : newMod.functions) {
            if (!f.isExternal && functionMatchesName(f, fnFilter)) {
                newFn = &f;
                break;
            }
        }
        if (!oldFn || !newFn) {
            FunctionEquivResult missing;
            missing.name = fnFilter;
            missing.status = EquivStatus::Unknown;
            missing.detail = !oldFn ? "function not found in old module"
                                    : "function not found in new module";
            report.functions.push_back(std::move(missing));
            return report;
        }
        report.functions.push_back(compareIlFunctionPair(oldMod, *oldFn, newMod, *newFn));
        return report;
    }

    for (const auto& oldFn : oldMod.functions) {
        if (oldFn.isExternal || oldFn.isEntryPoint || oldFn.name == "main") continue;
        const il::Function* matchedNew = nullptr;
        for (const auto& newFn : newMod.functions) {
            if (newFn.isExternal || newFn.isEntryPoint || newFn.name == "main") continue;
            if (functionMatchesName(newFn, oldFn.name) || functionMatchesName(oldFn, newFn.name)) {
                matchedNew = &newFn;
                break;
            }
        }
        if (matchedNew) {
            report.functions.push_back(compareIlFunctionPair(oldMod, oldFn, newMod, *matchedNew));
        }
    }

    if (report.functions.empty()) {
        const il::Function* oldMain = nullptr;
        const il::Function* newMain = nullptr;
        for (const auto& f : oldMod.functions) {
            if (f.isEntryPoint || f.name == "main") { oldMain = &f; break; }
        }
        for (const auto& f : newMod.functions) {
            if (f.isEntryPoint || f.name == "main") { newMain = &f; break; }
        }
        if (oldMain && newMain) {
            report.functions.push_back(compareIlFunctionPair(oldMod, *oldMain, newMod, *newMain));
        }
    }

    return report;
}

ProveEquivReport proveSourceEquiv(std::string_view oldSrc,
                                  std::string_view newSrc,
                                  const std::string& fnFilter,
                                  std::string* errorOut) {
    il::Module oldMod;
    il::Module newMod;
    if (!lowerSingleSourceForEquiv(oldSrc, "old.js", oldMod, errorOut)) {
        return {};
    }
    if (!lowerSingleSourceForEquiv(newSrc, "new.js", newMod, errorOut)) {
        return {};
    }
    return proveModuleEquiv(oldMod, newMod, fnFilter);
}

int runProveEquiv(const std::string& oldPath,
                  const std::string& newPath,
                  const std::string& fnFilter,
                  bool jsonOutput,
                  std::ostream& out,
                  std::ostream& err,
                  const std::vector<modules::ModuleRoot>& moduleRoots,
                  const std::string& importMapPath) {
    il::Module oldMod;
    il::Module newMod;
    std::string errMsg;
    if (!lowerFileToModule(oldPath, moduleRoots, importMapPath, oldMod, errMsg)) {
        err << errMsg;
        return 1;
    }
    if (!lowerFileToModule(newPath, moduleRoots, importMapPath, newMod, errMsg)) {
        err << errMsg;
        return 1;
    }

    ProveEquivReport report = proveModuleEquiv(oldMod, newMod, fnFilter);
    if (report.empty()) {
        err << "error: no matching functions found to compare between " << oldPath << " and " << newPath << "\n";
        return 1;
    }

    if (jsonOutput) {
        out << report.renderJson();
    } else {
        out << report.renderText();
    }

    if (report.hasCounterexample()) return 1;
    if (!report.allEquivalent()) return 2;
    return 0;
}

}  // namespace bronze::check
