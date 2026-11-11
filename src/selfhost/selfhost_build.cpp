// The self-hosted builtins' build (src/selfhost/README.md): the library's
// source compiled ahead of time, as the object every runtime image links.
//
// The pipeline is `bronze build --emit-obj`'s, with the self-host dialect on
// (lower_selfhost.cpp), the entry named `bronze_selfhost_main` (the runtime
// enters it once per thread, runtime/selfhost.cpp), no function source kept
// (a builtin's toString is the NativeFunction form), and direct calls, which
// an image build has: the library is linked into the runtime and is machine
// code for the life of the process.

#include "selfhost/selfhost_build.h"
#include "selfhost/selfhost_library.h"

#include <cstdio>
#include <string>

#include "codegen-brass/brass_backend.h"
#include "lower/lower.h"
#include "modules/modules.h"
#include "support/diagnostics.h"
#include "support/source.h"
#include "types/infer.h"

namespace bronze::selfhost {

int runSelfHostBuild(const std::string& outputPath) {
    SourceSet sources;
    DiagnosticSink diags;
    modules::ModuleOptions moduleOptions{{}, {}};
    // The text the compiler carries (selfhost_library.h), which is also what
    // a program compiles in: one source, one meaning.
    auto astModule = modules::loadProgramSource(std::string(librarySource()), "bronze:selfhost/builtins.js",
                                                sources, diags, moduleOptions);
    const auto fail = [&]() {
        std::fputs(diags.render(sources).c_str(), stderr);
        return 1;
    };
    if (!astModule) return fail();
    auto inferred = types::inferModule(*astModule, diags, nullptr, nullptr);
    if (diags.hasErrors() || !inferred) return fail();

    lower::AheadOfTimeOptions aot;
    aot.selfHost = true;
    auto ilModule = lower::lowerAheadOfTime(*astModule, diags, &*inferred, &sources, aot);
    if (diags.hasErrors() || !ilModule) return fail();
    ilModule->sourceTexts.clear();

    BrassBackend backend;
    backend.setEntrySymbol("bronze_selfhost_main");
    backend.setDirectJsCalls(true);
    if (!backend.emitObject(*ilModule, outputPath, diags)) return fail();
    return 0;
}

}  // namespace bronze::selfhost
