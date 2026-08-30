#pragma once

#include <cstdint>

#include "abi/bronze_abi.h"

namespace bronze::il {

// What one `Op::CensusRecord` observes, carried in the instruction's `immI32`
// and in the module's census site table (src/runtime/pin_census.h, stage C1).
//
// The low byte says which MANIFEST FORM the site's key names, because the
// forms admit different kinds and the kind is decided at exit from the tags. A
// census site exists only where lowering ran out of static answers: an env
// slot the fixpoint refused, a parameter no proof typed, a return the
// convention left Dynamic, a store to a field whose type is unknown. That is
// what makes the census complementary to the proofs rather than a duplicate of
// them (stage E4's HANDOFF (c)).
//
// Its own header because it is a fact shared with the RUNTIME's census table
// and not with the instruction set: `il.h` includes it so every reader of an
// instruction still reaches it by the name it always had.
enum class CensusSite : uint8_t {
    EnvSlot = BRONZE_ABI_CENSUS_ENV_SLOT,
    Field = BRONZE_ABI_CENSUS_FIELD,
    Param = BRONZE_ABI_CENSUS_PARAM,
    Return = BRONZE_ABI_CENSUS_RETURN,
    // Not a form: a store to a field NAME through a receiver inference could
    // not type. It can never become an entry — it names no class — and what it
    // does is mark every entry for a field of that name `@observed`, because
    // B1's barrier cannot reach this store (src/types/pins.h).
    OpaqueFieldStore = BRONZE_ABI_CENSUS_OPAQUE,
};

}  // namespace bronze::il
