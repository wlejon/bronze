#include "runtime/abi_guard.h"

#include <cstdio>

#include "runtime/fatal.h"

// The build computes it from bronze_abi.h's content (src/abi/CMakeLists.txt)
// and publishes it on the abi target; compiling this file without it means
// the definition plumbing broke, which must be a build error here, not an
// #ifdef'd-away check that silently guards nothing.
#ifndef BRONZE_ABI_FINGERPRINT
#error "BRONZE_ABI_FINGERPRINT is not defined - src/abi/CMakeLists.txt publishes it on the abi target"
#endif

namespace bronze::runtime {

void rtCheckObjectAbi(uint32_t objectFingerprint) {
    if (objectFingerprint == BRONZE_ABI_FINGERPRINT) return;
    static char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "bronze ABI mismatch: this program's object was compiled against ABI "
                  "%08x, but the runtime it is linked with speaks %08x. Recompile the "
                  "app with the bronze CLI built from the same tree as this runtime.",
                  objectFingerprint, static_cast<uint32_t>(BRONZE_ABI_FINGERPRINT));
    fatal(msg);
}

}  // namespace bronze::runtime
