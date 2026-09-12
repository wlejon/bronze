#include "cli/run.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "eval/eval.h"
#include "embed/embed.h"
#include "runtime/gc.h"

namespace bronze::cli {



int runEval(std::string_view code) {
    embed::setupIo();
    bronze::ShadowStackFrame rootFrame;
    eval::installDefaultDynamicHooks();
    embed::CallResult res = eval::evalScript(code, eval::EvalOptions{.filename = "<eval>"});
    if (res.thrown) {
        std::string errStr = embed::toUtf8(res.value);
        std::fprintf(stderr, "Uncaught %s\n", errStr.c_str());
        return 1;
    }
    if (!res.value.isUndefined()) {
        std::string out = embed::toUtf8(res.value);
        std::printf("%s\n", out.c_str());
    }
    return 0;
}

int runFileInJit(const std::string& filePath) {
    embed::setupIo();
    bronze::ShadowStackFrame rootFrame;
    eval::installDefaultDynamicHooks();
    embed::CallResult res = eval::evalFile(filePath, eval::EvalOptions{.filename = filePath});
    if (res.thrown) {
        std::string errStr = embed::toUtf8(res.value);
        std::fprintf(stderr, "Uncaught %s\n", errStr.c_str());
        return 1;
    }
    return 0;
}

}  // namespace bronze::cli
