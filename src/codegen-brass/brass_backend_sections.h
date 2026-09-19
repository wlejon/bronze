#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <brass/object/object_writer.hpp>
#include <brass/target/target.hpp>

#include "il/il.h"
#include "support/timings.h"

namespace bronze::codegen {

// The name of a bronze-owned symbol in a module whose entry is `entrySymbol`:
// `<base>` for the default entry (`bronze_main`, or a bare `main`), and
// `<base>_<entry>` otherwise, so that two compiled modules linked into one
// image never define the same key map or IC table.
std::string moduleSymbolName(const std::string& entrySymbol, const std::string& base);

// Everything the object carries beside brass's code that the runtime reads by
// name (bronze_abi.h has each layout); what BrassBackend::buildObjectFile
// appends after brass has compiled the module.
struct SectionInputs {
    const il::Module& module;
    // Per function, the symbol name it was compiled under ("main" for the entry).
    const std::vector<std::string>& uniqueNames;
    const std::string& entrySymbol;
    const std::vector<std::string>& hostGlobals;
    // Cells in `__bronze_global_cache`: one per distinct key the module reads.
    size_t globalCacheCount;
    // The method-call inline-cache site numbers, ascending.
    const std::vector<uint32_t>& methodIcSites;
};

// Appends the bronze-owned sections to `obj`, in place:
//
//   rodata  the ABI stamp, the host-globals manifest, the key-constant table,
//           the census tables, the method-IC site list, the source texts with
//           their function-range indexes, the file and function name strings,
//           one function descriptor per compiled function, one pc->line table
//           per function brass recorded locations for, and the code-range
//           table that ties each function's text to its descriptor and pc
//           table for the stack walker;
//   data    the module env cell, the key map, the template cells, the
//           host-global read cache, the inline-cache table and the native
//           import table.
//
// Ends by demoting every defined symbol but the loadable-module contract's
// (the entry, its stamp, manifest, key constants, import table and code
// ranges) to local binding. `timer` reports each table under --timings.
void emitBronzeSections(brass::object::ObjectFile& obj,
                        const brass::Target& target,
                        const SectionInputs& in,
                        support::PhaseTimer& timer);

}  // namespace bronze::codegen
