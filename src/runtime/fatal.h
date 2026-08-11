#pragma once

namespace bronze {

// Hard runtime errors name the unsupported construct and abort — never a
// silent fallback (docs/0000). abort, not throw: these fire from helpers
// called by generated code, whose frames carry no C++ EH metadata, so an
// exception must never unwind across that boundary.
// Tests may install a handler (which must not return; throwing is the
// intended use) so doctest can observe the rejection in-process.
using FatalHandler = void (*)(const char* msg);
[[noreturn]] void fatal(const char* msg);
void setFatalHandler(FatalHandler handler);  // nullptr restores print+abort

// Route the CRT's crash reporting to stderr instead of a modal dialog.
// On Windows a debug-CRT abort() pops "Debug Error!" and blocks until a
// human clicks it, which would hang the oracle harness (and any unattended
// run) on exactly the hard errors the harness exists to observe. Called at
// program entry and again on the fatal path; idempotent.
void disableCrashDialogs() noexcept;

}  // namespace bronze
