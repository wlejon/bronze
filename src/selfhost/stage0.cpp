// bronze_stage0: the compiler that builds the self-hosted builtins
// (README.md). It is bronze's own front end and backend, linked against a
// runtime WITHOUT the library it is about to produce — the one image that
// cannot have it — so the library's entry is defined here as a refusal.
// Nothing in a compile reaches a builtin's function object.

#include <cstdio>

#include "selfhost/selfhost_build.h"
#include "runtime/fatal.h"

extern "C" void bronze_selfhost_main() {
    bronze::fatal("bronze_stage0 has no self-hosted builtins (it is what builds them)");
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("usage: bronze_stage0 <output object>\n", stderr);
        return 2;
    }
    return bronze::selfhost::runSelfHostBuild(argv[1]);
}
