#include "support/timings.h"

namespace bronze::support {

namespace {
bool g_timingsEnabled = false;
thread_local bool t_timingsMuted = false;
}

bool timingsEnabled() { return g_timingsEnabled && !t_timingsMuted; }

TimingsMuted::TimingsMuted() : was_(t_timingsMuted) { t_timingsMuted = true; }
TimingsMuted::~TimingsMuted() { t_timingsMuted = was_; }
void setTimingsEnabled(bool on) { g_timingsEnabled = on; }

}  // namespace bronze::support
