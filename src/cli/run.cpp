#include "cli/run.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "eval/eval.h"
#include "embed/embed.h"
#include "runtime/gc.h"

namespace bronze::cli {

namespace {

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

}  // namespace

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
    std::string source;
    if (!readFile(filePath, source)) {
        std::fprintf(stderr, "error: cannot read %s\n", filePath.c_str());
        return 1;
    }
    eval::installDefaultDynamicHooks();
    embed::CallResult res = eval::evalScript(source, eval::EvalOptions{.filename = filePath});
    if (res.thrown) {
        std::string errStr = embed::toUtf8(res.value);
        std::fprintf(stderr, "Uncaught %s\n", errStr.c_str());
        return 1;
    }
    return 0;
}

}  // namespace bronze::cli
