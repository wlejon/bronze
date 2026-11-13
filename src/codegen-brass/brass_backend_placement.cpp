// Where brass lays out a module's functions (brass_backend_sections.h,
// functionPlacement; lower/function_placement.h decides the IL's part).

#include "codegen-brass/brass_backend_sections.h"

namespace bronze::codegen {

namespace {

// The stubs nearly every cached site of a module calls (il2mir: kPropGetStub,
// kPropGetFrontStub, kPropSetStub, il_method_site.cpp's kMethodCallStub,
// kElemGetStub, kElemSetStub, kIndexGetStub, kIndexSetStub). With a profile they lead the text: they are
// the module's hottest code whatever the program.
constexpr const char* kLeadingStubs[] = {
    "__bronze_prop_get_ic",  "__bronze_prop_get_ic_front", "__bronze_prop_set_ic",
    "__bronze_method_call_ic", "__bronze_elem_get_keyed",  "__bronze_elem_set_keyed",
    "__bronze_index_get",    "__bronze_index_set",
};
// The stubs a module calls on slower paths (kPropGetSlowStub: a getter or a
// receiver no way names; kPropSetSlowStub: a write past its site's way 0;
// kSortMergeStub: Array.prototype.sort's merge): hot
// in some programs, so they follow the hot functions rather than lead them.
constexpr const char* kTrailingStubs[] = {
    "__bronze_prop_get_slow",
    "__bronze_prop_set_slow",
    "__bronze_sort_merge",
};

}  // namespace

brass::object::FunctionPlacement functionPlacement(const il::Module& module,
                                                   const std::vector<std::string>& uniqueNames,
                                                   const std::string& entrySymbol) {
    const auto symbolOf = [&](uint32_t f) -> std::string {
        return uniqueNames[f] == "main" ? entrySymbol : uniqueNames[f];
    };
    brass::object::FunctionPlacement out;
    // No profile, no placement: module order (function_placement.h).
    if (module.hotFunctions.empty()) return out;
    for (const char* stub : kLeadingStubs) out.hot.emplace_back(stub);
    for (uint32_t f : module.hotFunctions) {
        if (f >= uniqueNames.size()) continue;
        out.hot.push_back(symbolOf(f));
        // Its call wrapper, which every call through a function object or a
        // method site's cache enters first.
        out.hot.push_back("__wrapper_" + uniqueNames[f]);
    }
    for (const char* stub : kTrailingStubs) out.hot.emplace_back(stub);
    for (uint32_t f : module.coldFunctions) {
        if (f >= uniqueNames.size()) continue;
        out.cold.push_back(symbolOf(f));
        out.cold.push_back("__wrapper_" + uniqueNames[f]);
    }
    // A hot function's resume twin (il2mir make_resume_twins), where it has
    // one: entered only when a guard of the function failed, so it goes
    // behind everything, out of the hot text's pages.
    for (uint32_t f : module.hotFunctions) {
        if (f < uniqueNames.size()) out.cold.push_back(symbolOf(f) + "__twin");
    }
    return out;
}

}  // namespace bronze::codegen
