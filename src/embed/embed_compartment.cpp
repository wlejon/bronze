// Compartments for a host (embed.h, "compartments"): the runtime's, exported.

#include "embed/embed.h"

#include "runtime/compartment.h"

namespace bronze::embed {

Compartment* createCompartment(Realm* realm, const CompartmentOptions& options) {
    return runtime::rtCreateCompartment(realm, options);
}

void destroyCompartment(Compartment* c) { runtime::rtDestroyCompartment(c); }

CompartmentStatus runInCompartment(Compartment* c, CompartmentJob job, void* arg) {
    return runtime::rtCompartmentRun(c, job, arg);
}

CompartmentStatus resumeCompartment(Compartment* c) { return runtime::rtCompartmentResume(c); }

void terminateCompartment(Compartment* c, const std::string& reason) {
    runtime::rtCompartmentTerminate(c, reason);
}

CompartmentState compartmentState(const Compartment* c) { return runtime::rtCompartmentState(c); }
Realm* compartmentRealm(const Compartment* c) { return runtime::rtCompartmentRealm(c); }
Compartment* runningCompartment() { return runtime::rtRunningCompartment(); }
uint32_t compartmentOverruns(const Compartment* c) { return runtime::rtCompartmentOverruns(c); }
double compartmentLongestOverrunMs(const Compartment* c) { return runtime::rtCompartmentLongestOverrunMs(c); }
std::string compartmentLongestOverrunSite(const Compartment* c) {
    return runtime::rtCompartmentLongestOverrunSite(c);
}

void beginNoYield() { runtime::rtBeginNoYield(); }
void endNoYield() { runtime::rtEndNoYield(); }

}  // namespace bronze::embed
