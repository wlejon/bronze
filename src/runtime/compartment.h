#pragma once

// Compartments: a realm whose entries run on a fiber of its own, in time
// slices (docs/internals.md "Compartments"; bro's docs/app-isolation.md).
//
// A host running several apps on one thread (helm running its apps inline)
// gives each one a compartment. Every entry into the app — an event handler,
// a timer, a frame callback, a module's evaluation — is a job the host hands
// to rtCompartmentRun, which runs it on the compartment's fiber in the
// compartment's realm, then drains the fiber's own microtask queue (each
// stack has its own, microtask.cpp), so one app's promise jobs never run in
// another's turn.
//
// A slice: from the job's start, a watchdog thread arms the thread's
// interrupt (tls_block.h) once the slice is spent; the next loop backedge or
// function entry calls rtServiceInterrupt, which suspends the fiber and
// returns control to the host with Yielded. The host resumes it on a later
// turn (rtCompartmentResume), with a fresh slice. Natives are not preempted:
// a poll only happens in generated code, so a slice runs over by however long
// the native the app called takes (rtCompartmentRun reports it).
//
// Termination is uncatchable: rtCompartmentTerminate marks the compartment,
// arms the interrupt, and the next poll throws brass's TerminationException,
// which no guest catch or finally takes; it unwinds the fiber to its entry,
// which ends it. A suspended compartment is unwound the same way when it is
// terminated (it is resumed once, and the poll it is suspended in throws).
// Afterwards the compartment is dead: every later job is refused.

#include <cstdint>
#include <string>

namespace bronze {
class Realm;
}

namespace bronze::runtime {

struct Compartment;

struct CompartmentOptions {
    // How long one job, or one resumption of it, runs before it yields.
    double sliceMs = 8.0;
    // The fiber's stack.
    size_t stackBytes = 1u << 20;
    // For reports ("<name> ran 40 ms past its slice in ...").
    std::string name;
};

enum class CompartmentState : uint8_t {
    Idle,        // no job, or the last one finished
    Yielded,     // a job is suspended at a poll: resume it
    Terminated,  // dead; nothing runs in it again
};

struct CompartmentStatus {
    CompartmentState state = CompartmentState::Idle;
    // The job threw and nothing caught it (the text, as an uncaught error
    // prints), or "" when it did not.
    std::string uncaught;
    // Terminated: the reason given to rtCompartmentTerminate, and where the
    // app was when the termination reached it (its stack, innermost first).
    std::string terminationReason;
    std::string terminationSite;
};

using CompartmentJob = void (*)(void* arg);

Compartment* rtCreateCompartment(Realm* realm, const CompartmentOptions& options);
// Terminates it first when a job is suspended in it (unwinding the job).
void rtDestroyCompartment(Compartment* c);

// Runs `job(arg)` on the compartment's fiber, in its realm, then drains the
// fiber's microtasks. Returns when it finished (Idle), was suspended at a
// poll after its slice (Yielded) or was terminated (Terminated). Refused
// (returns Terminated at once) for a dead compartment, and must not be called
// while a job is Yielded: resume that one first.
CompartmentStatus rtCompartmentRun(Compartment* c, CompartmentJob job, void* arg);
// Resumes a Yielded job with a fresh slice.
CompartmentStatus rtCompartmentResume(Compartment* c);

// Ends the compartment. From the thread running it (inside one of its jobs,
// say in a native it called) or from the host between turns; the reason is
// reported. Safe to call on a dead one.
void rtCompartmentTerminate(Compartment* c, const std::string& reason);

CompartmentState rtCompartmentState(const Compartment* c);
Realm* rtCompartmentRealm(const Compartment* c);
// The compartment whose fiber is running on this thread, or null.
Compartment* rtRunningCompartment() noexcept;
// Slices a job of this compartment overran: each time a slice ended in a
// yield, plus each time a native held a slice past twice its length.
uint32_t rtCompartmentOverruns(const Compartment* c);
// The longest a single native call held a slice past its end, in ms, and the
// stack it was called from (the last time it was the longest).
double rtCompartmentLongestOverrunMs(const Compartment* c);
const std::string& rtCompartmentLongestOverrunSite(const Compartment* c);

// While one is alive on this thread, a spent slice does not yield (the poll
// returns at once, and the next one after the scope ends yields): for host
// code that calls into the app and cannot be re-entered by other apps'
// turns before it returns.
void rtBeginNoYield() noexcept;
void rtEndNoYield() noexcept;

}  // namespace bronze::runtime
