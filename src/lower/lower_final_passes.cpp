// The end of lowering: what the finished module is stamped with before it is
// handed on.
//
// It runs LAST: it reads the finished module and hands out no module-global
// identity of its own, so everything lowering did before it — inline-cache
// indices, static-slot cells, template sites, the census table — is already
// frozen. It is also why `bronze il` shows the final IL: `lowerModule` is the
// single funnel both `runIl` and `runBuild` come through.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "il/feedback_site.h"
#include "il/print.h"
#include "lower/lowerer.h"

namespace bronze::lower {

namespace {

// BRONZE_FINAL_IL_DUMP=<substr>[,...]: the final IL of every function whose
// name contains one of them, on stderr. A diagnostic.
void dumpFinalIl(const il::Module& mod) {
    const char* spec = std::getenv("BRONZE_FINAL_IL_DUMP");
    if (spec == nullptr || *spec == 0) return;
    const std::string all = spec;
    for (const il::Function& fn : mod.functions) {
        bool hit = false;
        for (size_t at = 0; at <= all.size() && !hit;) {
            const size_t comma = std::min(all.find(',', at), all.size());
            const std::string part = all.substr(at, comma - at);
            if (!part.empty() && fn.name.find(part) != std::string::npos) hit = true;
            at = comma + 1;
        }
        if (!hit) continue;
        il::Module one;
        one.keyConstants = mod.keyConstants;
        one.functions.push_back(fn);
        std::fputs(il::print(one).c_str(), stderr);
    }
}

}  // namespace

bool Lowerer::finishIlPasses() {
    il::Module& mod = ilModule_;
    // The slow sites, numbered on the finished module.
    il::numberSlowSites(mod);
    // The harvest identity (il.h icLayoutDigest).
    mod.icLayoutDigest = il::icLayoutDigest(mod);
    dumpFinalIl(mod);
    return true;
}

}  // namespace bronze::lower
