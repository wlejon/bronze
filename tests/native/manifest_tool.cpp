// Prints the native manifest the ahead-of-time half of this suite compiles
// against: the same registrations natives.h makes in the harness, through
// the same registerNative, written by embed::writeNativeManifest. Run at
// BUILD time, so `bronze build --native-manifest` reads a file the registry
// printed rather than one a person typed — the manifest is an output of the
// host, never an input a human maintains beside it.
//
//   manifest_tool <out.json>                the suite's natives
//   manifest_tool <out.json> --with-absent  plus nt.absent.ping, which no
//                                           harness registers: the module
//                                           compiled against this one is the
//                                           load-time refusal case

#include <cstdio>
#include <cstring>
#include <string>

#include "embed/embed.h"
#include "natives.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: manifest_tool <out.json> [--with-absent]\n");
        return 2;
    }
    const bool withAbsent = argc > 2 && std::strcmp(argv[2], "--with-absent") == 0;
    std::string err;
    if (!nt_natives::registerAll(err, withAbsent)) {
        std::fprintf(stderr, "manifest_tool: %s\n", err.c_str());
        return 1;
    }
    if (!bronze::embed::writeNativeManifest(argv[1], &err)) {
        std::fprintf(stderr, "manifest_tool: %s\n", err.c_str());
        return 1;
    }
    return 0;
}
