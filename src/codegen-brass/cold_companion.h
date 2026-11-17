#pragma once

// INTERPRETED COLD FUNCTIONS (docs/type-feedback.md, "Cold functions").
//
// A module built ahead of time on a harvest compiles the functions the
// harvest saw run (heat Unknown or Hot). The ones it saw seldom or never
// (heat Never or Entered) are not compiled: the image carries their IL, and
// each function has, in place of its body, a STUB with the body's signature
// that calls through a CELL (il2mir/il_lowering_cold.cpp). The first call
// through any cell finds it zero and calls bronze_cold_resolve, which builds
// the image's COMPANION once: the IL lowered to MIR as a program of the
// tiered pipeline, whose functions are these alone and whose every other
// reference (compiled functions, wrappers, descriptors, data tables, module
// stubs) is the image's own symbol, through the LINK TABLE the build wrote.
// A function's cell then holds its address in that pipeline, filled on its
// first call: Tier 0 first, Tier 1 and 2 as the invocation and loop counts
// warm it, as any in-process program. Calls from compiled code to a cold function go
// through its stub; a cold function calls a compiled one at the image's
// own code. Both count the function's entries in the image's cold-entry
// table (bro reads it: a profile that missed what the app does).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "il/il.h"

namespace brass {
class Module;
namespace object {
struct ObjectFile;
}
}  // namespace brass

namespace bronze {
class BrassBackend;
}

namespace bronze::codegen {

// What a companion's lowering needs of the image's whole module that its IL,
// holding only the interpreted bodies, cannot compute again: tables
// numbered over every body, and the per-function facts read from bodies.
struct ColdCompanionInputs {
    // Per IL function: its cell (il2mir::TranslatorOptions::interpreted).
    std::vector<uint32_t> interpreted;
    // codegen::lowerToBrassAst's global-read keys, elemCellSites,
    // il::Module::methodIcSites, as the image computed them.
    std::vector<uint32_t> globalReadKeys;
    std::vector<uint32_t> elemCellSites;
    std::vector<uint32_t> methodIcSites;
    // Per IL function, the descriptor flags the image gave it
    // (il2mir::FunctionMeta::fn_flags).
    std::vector<uint32_t> fnFlags;
    // The retirable speculated functions (deopt_sites.h): S's index and G's,
    // and an optimistic S+'s fallback S.
    std::vector<std::pair<uint32_t, uint32_t>> speculated;
    std::vector<std::pair<uint32_t, uint32_t>> fallbacks;
    bool perThreadData = true;
};

// Which of a harvested module's functions are interpreted, and what stays
// compiled cold for which reason.
struct ColdSelection {
    std::vector<uint32_t> interpreted;  // per function: cell or UINT32_MAX
    uint32_t cells = 0;
    struct Kept {
        uint32_t functions = 0;
        uint64_t instructions = 0;
    };
    Kept coroutine;    // a coroutine body, or one starting or suspending one
    Kept speculation;  // speculated, or holding resume points or leaves
    Kept params;       // more parameters than a native-to-Tier-0 bridge takes
    Kept toplevel;     // the program's top-level code (BRONZE_FN_DESC_TOPLEVEL)
};
ColdSelection selectInterpreted(const il::Module& module, const std::vector<std::string>& uniqueNames);

// The companion's IL and inputs as one blob, and back (the blob is the
// module with every body not interpreted dropped, then the inputs).
std::string encodeCompanion(const il::Module& module, const ColdCompanionInputs& in);
bool decodeCompanion(std::string_view bytes, il::Module& module, ColdCompanionInputs& in, std::string& err);

// The image's `__bronze_cold_link` (il2mir kColdLinkSymbol), read-only.
struct ColdLink {
    uint64_t magic;  // kColdLinkMagic
    const char* blob;
    uint64_t blobSize;
    uint64_t* cells;
    uint64_t cellCount;
    // linkCount names, each NUL-terminated, one after another, and the
    // address of each in the loaded image.
    const char* names;
    void* const* addrs;
    uint64_t linkCount;
    const char* entrySymbol;
};
inline constexpr uint64_t kColdLinkMagic = 0x314B4E494C444C43ull;  // "CLDLINK1"

// Appends the cells, the blob, the link table and the link record to `obj`,
// the image `backend` just built with `inputs.interpreted`. Builds the
// companion's MIR once to find what it references; false (the image must
// then be built again without interpreted functions) when it references a
// symbol neither the image nor the runtime defines.
bool emitColdCompanion(brass::object::ObjectFile& obj, const BrassBackend& backend, const il::Module& module,
                       const std::vector<std::string>& uniqueNames, const ColdCompanionInputs& inputs,
                       std::string& why);

// bronze_cold_resolve's work: builds the link's companion (once per link, any
// thread) and fills cell `cell`.
void resolveColdCell(const ColdLink* link, uint32_t cell);

}  // namespace bronze::codegen
