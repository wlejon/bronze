#pragma once

#include <string>

#include "types/result.h"

namespace bronze::types {

// The canonical text form of docs/0010 decision 8, printed by
// `bronze types <file>` and pinned byte-for-byte by the unit tests.
//
// Per function: the signature, whether it is direct-callable, then, per
// statement in source order, the bindings whose type that statement changed.
// Everything here comes out of ordered containers or is sorted first — no
// hash-map iteration reaches this output.
std::string dump(const InferenceResult& result);

}  // namespace bronze::types
