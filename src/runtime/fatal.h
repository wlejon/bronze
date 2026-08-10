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

}  // namespace bronze
