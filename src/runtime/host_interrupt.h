#pragma once

// Host interrupts: a function the host has a thread run at that thread's next
// interrupt poll (a loop backedge or a function entry in generated code), on
// the thread's own stack, between two steps of whatever script it is running.
//
// The host registers one on the thread it wants to reach (registerHost-
// Interrupt), then any thread may request it: the request arms the thread's
// interrupt (tls_block.h), and the poll it reaches runs the function once
// (rtServiceInterrupt, before the compartment scheduler's own check). bro uses
// it to keep a window answering the OS while its page thread runs a long
// stretch of script: the function lets the OS process the window's messages.
//
// The function runs with script frames below it and must not run script,
// allocate on the heap of the runtime or throw. It is not run when the stack
// left below the poll is short (the request stays pending and the next poll
// with room takes it), nor from inside itself. Natives are not interrupted: a
// poll only happens in generated code, so a long native call delays it.

namespace bronze::runtime {

struct HostInterrupt;

using HostInterruptFn = void (*)(void* arg);

// On the thread to be reached: its host interrupt, replacing any earlier one.
// The object lives for the process (a request racing the thread's exit or an
// unregister finds it inert).
HostInterrupt* rtRegisterHostInterrupt(HostInterruptFn fn, void* arg);
// On that thread: no function runs from its polls any more.
void rtUnregisterHostInterrupt();
// Any thread: run `h`'s function at its thread's next poll. Requests before
// that poll coalesce into one run. A no-op for null.
void rtRequestHostInterrupt(HostInterrupt* h) noexcept;

// From rtServiceInterrupt, on the interrupted thread.
void rtServiceHostInterrupt();

}  // namespace bronze::runtime
