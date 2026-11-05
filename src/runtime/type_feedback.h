#pragma once

// THE TYPE-FEEDBACK RECORDER (`bronze build --feedback-out <file.tfb>`,
// docs/type-feedback.md).
//
// An instrumented build carries one `feedback.record` per site
// il/feedback_site.h enumerates. Each record lands here with the site's
// function key, its packed site word (abi/bronze_feedback_site.h) and the
// value it saw, and the recorder keeps, per site:
//
//   - a count per VALUE TAG (f64, int32-valued number, undefined, null, bool,
//     string, plain object, array, typed array, function, other);
//   - for an inline-cache receiver, the SHAPES it saw, by a stable name — the
//     prototype's constructor name and the own keys in order
//     (`Vector3{x,y,z}`) — never an address, so two runs and two builds agree;
//   - for a call, the CALLEES, by name and a hash of their source text
//     (`multiplyMatrices@1a2b3c4d`), which is what an IL function of the
//     reading build can be matched against.
//
// It is reached through the pin census's two ABI entry points (the site
// word's top bit routes a call here), so it adds nothing to the generated-code
// ABI. Every call takes one mutex, as the census does: a feedback build is an
// instrument, run for its profile and never timed. Nothing here allocates on
// the JS heap — the names are read from immortal shape and string memory —
// which is what lets a record sit anywhere in the IL (il_op.h, canCollect).
//
// The profile is written at process exit, or on demand through
// `typeFeedbackWrite` / the `__bronze_feedback_flush()` intrinsic (for a host
// that leaves through `_exit`). BRONZE_FEEDBACK_OUT overrides the path the
// build baked in.

#include <cstdint>
#include <string>

#include "runtime/value.h"

namespace bronze::runtime {

// Registration (the census table's FEEDBACK rows): the output path and the
// method-name links. Arms the atexit writer.
void typeFeedbackRegister(const char* outPath, const uint32_t* table, uint32_t count,
                          const uint32_t* keyMap);

// One observation at the site `siteInfo` of the function whose key is the
// interned key `keyId`.
void typeFeedbackRecord(uint32_t keyId, uint32_t siteInfo, uint64_t valueBits);

// The profile as it stands, in the file format. Empty when nothing registered.
std::string typeFeedbackText();

// Writes the profile now; false when nothing registered or the file could not
// be written. Writing again later overwrites it with the counts to that point.
bool typeFeedbackWrite();

// Forgets every site and the output path. Tests only.
void typeFeedbackResetForTesting();

// `__bronze_feedback_flush`, the JS face of `typeFeedbackWrite`, for the
// global resolver (rt_global.cpp); undefined for any other name.
Value rtTypeFeedbackIntrinsic(const std::string& name);

}  // namespace bronze::runtime
