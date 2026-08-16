// Process-level I/O and failure setup for an embedding host.
//
// Its own translation unit for the same reason `drainMicrotasks` is not in
// embed_run.cpp: that unit names `bronze_main`, and a static library only
// surfaces an unresolved external when the object naming it is pulled in. A
// host that links MORE THAN ONE compiled module has no `bronze_main` at all —
// its modules are entered under their own `--entry-symbol` names — so it must
// be able to reach this without dragging the single-module entry in behind it.

#ifdef _WIN32
#include <cstdio>

#include <fcntl.h>
#include <io.h>
#endif

#include "embed/embed.h"
#include "runtime/fatal.h"

namespace bronze::embed {

void setupIo() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    // Before anything can fail: a hard error must reach stderr and exit,
    // never block on a modal dialog (fatal.h). Idempotent, so a host that
    // already called it — or calls runProgram after its own setup — loses
    // nothing.
    bronze::disableCrashDialogs();
}

}  // namespace bronze::embed
