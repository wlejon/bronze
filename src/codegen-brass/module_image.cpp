#include "codegen-brass/module_image.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <unordered_set>

#include <brass/target/aot_linker.hpp>

#include "abi/bronze_abi.h"

namespace bronze::codegen {
namespace {

namespace fs = std::filesystem;

// The export surface of the shared runtime, as the object sees it: the ABI
// registry, expanded here the same way cmake/bronze_abi_exports.cmake expands
// it into the .def / version script / exported-symbols list, plus the brass
// raise entry points and the personality routine the target's unwind
// information names. One header, two consumers, no drift.
#define BRONZE_LINK_STR2(x) #x
#define BRONZE_LINK_STR(x) BRONZE_LINK_STR2(x)
const char* personalityFor(const brass::Target& target) {
    return target.is_windows() ? BRONZE_LINK_STR(BRONZE_ABI_PERSONALITY_WINDOWS)
                               : BRONZE_LINK_STR(BRONZE_ABI_PERSONALITY_SYSV);
}
#undef BRONZE_LINK_STR
#undef BRONZE_LINK_STR2

bool isRuntimeExport(const std::string& name, const brass::Target& target) {
    static const std::unordered_set<std::string> s_names = [] {
        std::unordered_set<std::string> names;
#define BRONZE_LINK_EXPORT_NAME(name, ret, args) names.insert(#name);
        BRONZE_ABI_FUNCTIONS(BRONZE_LINK_EXPORT_NAME)
#undef BRONZE_LINK_EXPORT_NAME
#define BRONZE_LINK_BRASS_NAME(name) names.insert(#name);
        BRONZE_ABI_BRASS_SYMBOLS(BRONZE_LINK_BRASS_NAME)
        BRONZE_ABI_BRASS_PLAIN_SYMBOLS(BRONZE_LINK_BRASS_NAME)
#undef BRONZE_LINK_BRASS_NAME
        return names;
    }();
    return s_names.count(name) != 0 || name == personalityFor(target);
}

// The C math functions the backend may name (brass's il_lowering_ops and the
// kernel builders), every one of them a `double(double)` or `double(double,
// double)` the platform's math library exports under exactly that name.
const std::unordered_set<std::string>& mathExports() {
    static const std::unordered_set<std::string> s_names = {
        "acos", "asin", "atan", "atan2", "cbrt", "ceil", "cos", "cosh", "exp", "exp2",
        "expm1", "fabs", "floor", "fmax", "fmin", "fmod", "hypot", "log", "log10", "log1p",
        "log2", "pow", "round", "sin", "sinh", "sqrt", "tan", "tanh", "trunc",
    };
    return s_names;
}

// Every undefined symbol a relocation in the object names, once each, in
// first-reference order. A symbol the object declares undefined but never
// references is nobody's business.
std::vector<std::string> referencedUndefined(const brass::object::ObjectFile& obj) {
    std::unordered_set<std::string> seen;
    std::vector<std::string> names;
    for (const auto& sec : obj.sections) {
        for (const auto& r : sec.relocations) {
            if (r.symbol_name.empty() || seen.count(r.symbol_name)) continue;
            const auto* sym = obj.find_symbol(r.symbol_name);
            if (sym && sym->section_index >= 0) continue;
            if (!sym && obj.get_section(r.symbol_name) != nullptr) continue;
            seen.insert(r.symbol_name);
            names.push_back(r.symbol_name);
        }
    }
    return names;
}

// The exported names of the loadable-module contract (bronze_abi.h) for an
// entry, plus the code-range pair, kept to the ones the object defines.
std::vector<std::string> moduleExports(const brass::object::ObjectFile& obj,
                                       const std::string& entry) {
    const bool defaultEntry = entry == "bronze_main";
    const std::string prefix = defaultEntry ? "bronze_object" : entry;
    const std::vector<std::string> wanted = {
        entry,
        prefix + "_abi_fingerprint",
        entry + "_host_globals",
        entry + "_native_imports",
        prefix + "_code_ranges",
        prefix + "_code_range_count",
    };
    std::vector<std::string> present;
    for (const std::string& name : wanted) {
        const auto* sym = obj.find_symbol(name);
        if (sym && sym->section_index >= 0) present.push_back(name);
    }
    return present;
}

}  // namespace

// The C math library is the one other thing generated code calls: brass
// lowers Math.sqrt, Math.floor and their kin to the C functions of those
// names, and a statically linked program took them from the C runtime it
// was linked with. A module imports them from the platform's own library —
// the UCRT's math API set on Windows, libm on ELF, libSystem on Mach-O.
ModuleTargetNames moduleTargetNames(const brass::Target& target) {
    if (target.is_windows()) {
        return {"bronze_runtime_shared.dll", "api-ms-win-crt-math-l1-1-0.dll", ".dll", nullptr};
    }
    if (target.is_macos()) {
        return {"@rpath/libbronze_runtime_shared.dylib", "/usr/lib/libSystem.B.dylib", ".dylib",
                "@loader_path"};
    }
    return {"libbronze_runtime_shared.so", "libm.so.6", ".so", "$ORIGIN"};
}

bool writeModuleImage(const brass::object::ObjectFile& obj, const std::string& outputPath,
                      DiagnosticSink& diags, const std::string& entrySymbol,
                      const brass::object::MachOBuildVersion& machoVersion,
                      const std::vector<std::string>& extraRpaths) {
    const std::string entry = entrySymbol.empty() ? "bronze_main" : entrySymbol;

    // Every import is a runtime export or a C math function, checked here by
    // the registry rather than left to the loader: an object naming anything
    // else is a bronze whose backend and ABI header disagree, and that is a
    // build error to report at build time.
    std::vector<std::string> runtimeImports;
    std::vector<std::string> mathImports;
    std::string unknown;
    for (const std::string& name : referencedUndefined(obj)) {
        if (isRuntimeExport(name, obj.target)) runtimeImports.push_back(name);
        else if (mathExports().count(name)) mathImports.push_back(name);
        else unknown += (unknown.empty() ? "" : ", ") + name;
    }
    // The linker writes the unwind information, and with it the reference to
    // the personality routine, after this scan; offering the symbol costs
    // nothing when no function has landing pads, since only a referenced
    // import is bound.
    const std::string personality = personalityFor(obj.target);
    if (std::find(runtimeImports.begin(), runtimeImports.end(), personality) == runtimeImports.end()) {
        runtimeImports.push_back(personality);
    }
    if (!unknown.empty()) {
        diags.error(Span{}, "the object references " + unknown +
                                ", which the shared bronze runtime does not export (every "
                                "symbol generated code may name is an X(...) or Y(...) line in "
                                "src/abi/bronze_abi.h, or its personality routine) and the "
                                "C math library does not "
                                "provide. The backend and the ABI header this bronze was "
                                "built from disagree.");
        return false;
    }

    const ModuleTargetNames names = moduleTargetNames(obj.target);
    brass::target::LinkerOptions options;
    options.module_name = fs::path(outputPath).filename().string();
    options.soname = options.module_name;
    options.export_all_functions = false;
    options.explicit_exports = moduleExports(obj, entry);
    options.macho_build_version = machoVersion;
    if (!runtimeImports.empty()) options.imports.push_back({names.runtimeImport, runtimeImports});
    if (!mathImports.empty()) options.imports.push_back({names.mathImport, mathImports});
    if (names.ownDirRpath) {
        options.rpaths.push_back(names.ownDirRpath);
        for (const std::string& rpath : extraRpaths) options.rpaths.push_back(rpath);
    }

    std::error_code ec;
    const fs::path out(outputPath);
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);

    std::string error;
    if (!brass::target::AotLinker::link_to_file(obj, outputPath, options, &error)) {
        diags.error(Span{}, "cannot write the module " + outputPath + ": " +
                                (error.empty() ? std::string("the image writer failed") : error));
        return false;
    }
    return true;
}

}  // namespace bronze::codegen
