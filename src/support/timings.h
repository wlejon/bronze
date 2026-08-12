#pragma once

namespace bronze::support {

// Whether `bronze build --timings` was asked for.
//
// A duration is the one thing bronze prints that cannot be deterministic, and
// docs/0001 decision 10 is about bronze's own output — so the whole mechanism
// is off unless asked for, writes to stderr, and is compared by nothing. The
// flag is process-global rather than a parameter because the two places that
// report are the CLI and the LLVM backend, and threading a bool through
// `codegen::Backend::emitObject` would put a debugging concern into the
// interface every future backend has to implement (docs/0033).
bool timingsEnabled();
void setTimingsEnabled(bool on);

}  // namespace bronze::support
