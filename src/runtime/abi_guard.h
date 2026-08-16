#pragma once

#include <cstdint>

namespace bronze::runtime {

// The object-vs-runtime ABI guard (bronze_abi.h, "Drift between two
// BUILDS"). Generated objects carry a `bronze_object_abi_fingerprint`
// constant stamped by codegen from the hash of the bronze_abi.h the
// COMPILER was built against; this runtime carries the same hash from its
// own build. The two program entries pass the object's value here before
// any compiled code runs; a mismatch is a fatal naming both values and the
// remedy.
//
// Takes the value rather than reading the symbol so the reference to
// `bronze_object_abi_fingerprint` stays quarantined in the entry TUs
// beside `bronze_main` (embed_run.cpp's header comment says why): the
// runtime library itself must never name a symbol only a linked program
// defines, or every runtime-level test binary would fail to link.
void rtCheckObjectAbi(uint32_t objectFingerprint);

}  // namespace bronze::runtime
