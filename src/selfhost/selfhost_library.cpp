// The library as the compiler carries it (selfhost_library.h).

#include "selfhost/selfhost_library.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "modules/modules.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/infer.h"

namespace bronze::selfhost {

namespace {

// The name the library's frames and spans carry.
constexpr const char* kSourceName = "bronze:selfhost/builtins.js";

std::unique_ptr<lower::SelfHostLibrary> lowerLibrary() {
    SourceSet sources;
    DiagnosticSink diags;
    modules::ModuleOptions moduleOptions{{}, {}};
    auto fail = [&]() -> std::unique_ptr<lower::SelfHostLibrary> {
        std::fputs(diags.render(sources).c_str(), stderr);
        std::fputs("bronze: the self-hosted library does not compile\n", stderr);
        std::abort();
    };
    auto astModule = modules::loadProgramSource(std::string(librarySource()), kSourceName, sources, diags, moduleOptions);
    if (!astModule) return fail();
    auto inferred = types::inferModule(*astModule, diags, nullptr, nullptr);
    if (diags.hasErrors() || !inferred) return fail();
    auto lib = lower::lowerSelfHostLibrary(*astModule, diags, &*inferred, &sources);
    if (!lib) return fail();
    lib->sourceName = kSourceName;
    return std::make_unique<lower::SelfHostLibrary>(std::move(*lib));
}

}  // namespace

uint64_t libraryDigest() {
    static const uint64_t digest = [] {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : librarySource()) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h;
    }();
    return digest;
}

const lower::SelfHostLibrary* library() {
    static const std::unique_ptr<lower::SelfHostLibrary> lib = lowerLibrary();
    return lib.get();
}

}  // namespace bronze::selfhost
