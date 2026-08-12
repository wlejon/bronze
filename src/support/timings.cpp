#include "support/timings.h"

namespace bronze::support {

namespace {
bool g_timingsEnabled = false;
}

bool timingsEnabled() { return g_timingsEnabled; }
void setTimingsEnabled(bool on) { g_timingsEnabled = on; }

}  // namespace bronze::support
