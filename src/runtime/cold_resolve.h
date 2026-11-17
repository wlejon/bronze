#pragma once

// Who builds an AOT image's interpreted cold functions on their first call
// (bronze_cold_resolve; codegen-brass/cold_companion.h).

#include <cstdint>

namespace bronze::runtime {

// Fills cell `cell` of the image's link record (codegen::ColdLink) or does
// not return.
using ColdResolver = void (*)(const void* link, uint32_t cell);
void setColdResolver(ColdResolver resolver);

}  // namespace bronze::runtime

extern "C" void bronze_cold_resolve(const void* link, uint32_t cell);
