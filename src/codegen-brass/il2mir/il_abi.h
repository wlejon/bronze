#pragma once

// The runtime ABI facts the translator bakes into the MIR it emits, taken
// from the ABI headers themselves so a change there moves the translator
// with it.

#include "abi/bronze_abi.h"
#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <brass/mir/instruction.hpp>
#include <cstdint>
#include <string>
#include <string_view>

namespace brass {
class Module;
}

namespace il2mir {

constexpr uint64_t kUndefinedTag = BRONZE_ABI_UNDEFINED_BITS;
constexpr uint64_t kNullTag = uint64_t{BRONZE_ABI_TAG_NULL} << 48;
constexpr uint64_t kBoolTag = uint64_t{BRONZE_ABI_TAG_BOOL} << 48;
constexpr uint64_t kInt32Tag = uint64_t{BRONZE_ABI_TAG_INT32} << 48;
// Marker values a NameResolve of `print` / `print.err` lowers to.
constexpr uint64_t kPrintTag = 0xFFFE000000000001ULL;
constexpr uint64_t kPrintErrTag = 0xFFFE000000000002ULL;

// The brass alias class (Instruction::alias_class) of an f64 load of a boxed
// Array element, the double form of a read il.h `numberBits` licenses: its
// eight bytes are what a store of a Value put there, so as an i64 they are a
// valid Value already and need no NaN canonicalization (il_bridge_verifier).
// Nothing else carries it; every other access is class 0, which overlaps it.
constexpr uint8_t kBoxedElementAliasClass = 1;
// The other memory il2mir names, so brass keeps the accesses apart (an access
// of class 0 overlaps everything). Array element stores share the class of
// the element loads above, since they write the same words.
// A TypedArray's buffer, window and length, and a buffer's external-storage
// word: written only by the runtime (a call), never by compiled code, and the
// header of a view is laid out after its four inline slots, so no slot store
// reaches it. Only loads carry this class.
constexpr uint8_t kTypedArrayHeaderAliasClass = 2;
// A TypedArray element: the bytes of a buffer's store.
constexpr uint8_t kTypedElementAliasClass = 3;
// An object's property slot, inline or in its overflow block, and a static
// slot cell.
constexpr uint8_t kObjectSlotAliasClass = 4;
// An object's header words: its shape and its overflow-block pointer.
constexpr uint8_t kShapeWordAliasClass = 5;

// Marks the access `v` (a load's result) or `inst` (a store) with `cls`.
inline brass::Value* with_alias_class(brass::Value* v, uint8_t cls) {
    v->defining_instruction()->set_alias_class(cls);
    return v;
}
inline brass::Instruction* with_alias_class(brass::Instruction* inst, uint8_t cls) {
    inst->set_alias_class(cls);
    return inst;
}

// The thread-local block, as pinned-register code reads it.
constexpr int32_t kBronzeTlsStackLimitOff = BRONZE_TLS_STACK_LIMIT_OFF;
// The thread's module-delta array, indexed by a module's slot cell: what
// `TranslatorOptions::per_thread_module_data` adds to every module-data
// address.
constexpr int32_t kBronzeTlsModuleDeltasOff = BRONZE_TLS_MODULE_DELTAS_OFF;
// One inline-cache SITE in the module's `__bronze_ic_table`. The table is
// indexed by site, and a helper takes the address of a site's way 0.
constexpr uint32_t kBronzeIcSiteSize = BRONZE_ABI_IC_SITE_SIZE;
// Where the module's packed way-0 rows (bronze_abi_layout.h, "the HOT
// array") start, relative to its IC table: right after it, at the next line.
// The backend lays the two out so (brass_backend_sections.cpp, both 64-byte
// aligned), so a row is addressed off the same base as its site.
constexpr int64_t ic_hot_table_offset(uint64_t site_count) {
    const uint64_t n = site_count != 0 ? site_count : 1;
    return static_cast<int64_t>((n * BRONZE_ABI_IC_SITE_SIZE + 63) & ~uint64_t{63});
}
// Where the method rows (bronze_abi_layout.h, "the METHOD ROWS") start,
// relative to the IC table: after the hot array, at the next line.
constexpr int64_t ic_method_rows_offset(uint64_t site_count) {
    const uint64_t n = site_count != 0 ? site_count : 1;
    const uint64_t end = static_cast<uint64_t>(ic_hot_table_offset(n)) + n * BRONZE_ABI_IC_HOT_ENTRY_SIZE;
    return static_cast<int64_t>((end + 63) & ~uint64_t{63});
}
// Where the static-slot guards' cells (`__bronze_static_cells`) and, right
// after them, the family base words (`__bronze_family_base`) start, relative
// to the IC table: after the method rows, at the next word. One section, laid
// out in this order (brass_backend_sections.cpp), so generated code reaches
// all of it off the IC table's address and a function keeps one base, not
// three, across its calls.
constexpr int64_t ic_static_cells_offset(uint64_t site_count, uint64_t method_sites) {
    const uint64_t rows = method_sites != 0 ? method_sites : 1;
    const uint64_t end = static_cast<uint64_t>(ic_method_rows_offset(site_count)) + rows * BRONZE_ABI_METHOD_ROW_SIZE;
    return static_cast<int64_t>((end + 7) & ~uint64_t{7});
}
constexpr int64_t ic_family_base_offset(uint64_t site_count, uint64_t method_sites, uint64_t static_sites) {
    const uint64_t cells = static_sites != 0 ? static_sites : 1;
    return ic_static_cells_offset(site_count, method_sites) + static_cast<int64_t>(cells * 8);
}
// The plain object's slots (ObjectHeader) and a heap block's header, which the
// inline property paths address an overflow slot through.
constexpr int32_t kBronzeObjOverflowOffset = BRONZE_ABI_OBJ_OVERFLOW_OFFSET;
constexpr int32_t kBronzeObjSlotsOffset = BRONZE_ABI_OBJ_SLOTS_OFFSET;
constexpr int64_t kBronzeObjOverflowSlot = BRONZE_ABI_OBJ_OVERFLOW_SLOT;
constexpr int64_t kBronzeHdrBytes = BRONZE_ABI_HDR_BYTES;
// Every Number's bits are at most this; every other value's are above it.
constexpr uint64_t kBronzeNumberMaxBits = BRONZE_ABI_NUMBER_MAX_BITS;

// The address of the object whose Value bits are `bits`: the bits with the
// object tag taken off by xor, which answers "is it an object at all" too (the
// result is below 2^48 exactly when the tag was there), so a tested receiver
// costs one xor and one compare where a tag mask, a compare and a payload mask
// were three. It is also the form brass's untag_reuse carries across a join
// whose cold arm called something, so the next access to the same receiver
// finds the address in a register. Not an address unless the value is an
// object: read through it only behind is_object_address or a proof.
inline brass::Value* object_address(brass::Builder& b, brass::Value* bits) {
    return b.build_xor(bits, b.build_iconst_i64(static_cast<int64_t>(uint64_t{BRONZE_ABI_TAG_OBJECT} << 48)));
}
inline brass::Value* is_object_address(brass::Builder& b, brass::Value* address) {
    return b.build_ult(address, b.build_iconst_i64(int64_t{1} << 48));
}

// Declare every runtime helper and module data symbol the lowered module
// may reference, with the optimizer contracts (SymbolRole) of the ones it
// has them for. `entry_symbol` names the module-suffixed data symbols.
void register_all_module_external_symbols(brass::Module* mod, const std::string& entry_symbol);

// The module's code-range table and its length (bronze_abi.h,
// bronze_code_range), as the object emitter names them for `entry_symbol`.
std::string code_ranges_symbol(const std::string& entry_symbol);
std::string code_range_count_symbol(const std::string& entry_symbol);

// A twinned function `f`'s generic entry `f__generic`, its probe `f__probe`
// and its entry cell `__bronze_twin_entry_f` (il_speculation.h, "FAILURE
// POLICY"), which the backend defines in the image's shared data: the
// address f's direct calls call, f's exits and a probe's calls.
inline constexpr std::string_view kTwinGenericSuffix = "__generic";
inline constexpr std::string_view kTwinProbeSuffix = "__probe";
inline constexpr std::string_view kTwinEntryCellPrefix = "__bronze_twin_entry_";
inline constexpr int32_t kTwinCellEntry = 0;
inline constexpr int32_t kTwinCellExits = 8;
inline constexpr int32_t kTwinCellCalls = 16;
inline constexpr size_t kTwinCellBytes = 24;

} // namespace il2mir
