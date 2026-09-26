#include "cli/run.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "eval/eval.h"
#include "embed/embed.h"
#include "runtime/exception.h"
#include "runtime/gc.h"
#include "runtime/sampler.h"

namespace bronze::cli {

// The report a built program makes when its top level throws
// (rtRunModuleEntry): an Error with its stack, anything else inspected. `bronze run` of the same program prints the same lines.
static int reportUncaught(Value thrown) {
    const std::string text = runtime::rtUncaughtReport(thrown);
    std::fflush(stdout);
    std::fprintf(stderr, "%s\n", text.c_str());
    return 1;
}

// `--tier` sets the process default, so the program's own eval() and
// new Function() run at the tier it names too.
static void applyTier(std::optional<ExecutionTier> tier) {
    if (tier) eval::setDefaultTier(*tier);
}

int runEvalReal(std::string_view code, std::optional<ExecutionTier> tier, bool emitDebugInfo) {
    applyTier(tier);
    embed::setupIo();
    bronze::ShadowStackFrame rootFrame;
    eval::installDefaultDynamicHooks();
    embed::CallResult res = eval::evalScript(
        code, eval::EvalOptions{.filename = "<eval>", .emitDebugInfo = emitDebugInfo, .tier = tier});
    if (res.thrown) return reportUncaught(res.value);
    if (!res.value.isUndefined()) {
        std::string out = embed::toUtf8(res.value);
        std::printf("%s\n", out.c_str());
    }
    return 0;
}

int runFileInJitReal(const std::string& filePath, const std::vector<std::string>& hostGlobals,
                     std::optional<ExecutionTier> tier, bool emitDebugInfo) {
    applyTier(tier);
    embed::setupIo();
    // This thread runs the program's JIT-compiled JS: the same note the
    // standalone main and the embed entry make, so BRONZE_SAMPLE=1 profiles
    // a `bronze run` exactly as it profiles a built program. A no-op unless
    // the sampler is armed.
    runtime::samplerNoteJsThread();
    bronze::ShadowStackFrame rootFrame;
    eval::installDefaultDynamicHooks();
    embed::CallResult res = eval::evalFile(
        filePath, eval::EvalOptions{.filename = filePath, .hostGlobals = hostGlobals, .emitDebugInfo = emitDebugInfo, .tier = tier});
    if (res.thrown) return reportUncaught(res.value);
    return 0;
}

}  // namespace bronze::cli
