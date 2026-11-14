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
// An object's header words: its kind word, its shape and its overflow-block
// pointer. Compiled code writes the last two in a transition (stores of this
// class) and the first only into an object it has just allocated (class 0).
constexpr uint8_t kShapeWordAliasClass = 5;
// A word only the runtime writes, so only a call changes it and only loads
// carry the class: a shape's family stamp and double-slot map, a module's
// family base and static cells, the thread's layout epoch. With these and the
// two classes above on a static guard's loads, a store into a slot (or a
// typed-array element) between two sites does not make brass read the
// receiver's shape, the cell and the stamp again, and the second guard of a
// claim on one receiver is the first one's condition.
constexpr uint8_t kRuntimeWordAliasClass = 6;
// An Array's header words: length, capacity, head offset, the elements
// block's pointer, the properties word and the numbers-only bits
// (BRONZE_ABI_ARRAY_*_OFFSET). Compiled code writes length, head and bits
// (an append, a push, a shift, a non-Number store), always with this class;
// the rest only the runtime moves (a call) or an allocation initializes
// (class 0). So an element store, a slot store or a transition between two
// reads of one array's length or block leaves the second read the first's
// value, and a loop whose only stores are of those classes reads them once.
constexpr uint8_t kArrayHeaderAliasClass = 7;
// A slow site's counter (il.h `slowSite`): written by its slow arm only.
constexpr uint8_t kSlowCountAliasClass = 8;

// Marks the access `v` (a load's result) or `inst` (a store) with `cls`.
inline brass::Value* with_alias_class(brass::Value* v, uint8_t cls) {
    v->defining_instruction()->set_alias_class(cls);
    return v;
}
inline brass::Instruction* with_alias_class(brass::Instruction* inst, uint8_t cls) {
    inst->set_alias_class(cls);
    return inst;
}
// A load or store of an Array header word (kArrayHeaderAliasClass).
inline brass::Value* array_header_load(brass::Builder& b, brass::Type type, brass::Value* ptr, int32_t offset) {
    return with_alias_class(b.build_load(type, ptr, offset), kArrayHeaderAliasClass);
}
inline void array_header_store(brass::Builder& b, brass::Type type, brass::Value* ptr, int32_t offset,
                               brass::Value* value) {
    with_alias_class(b.build_store(type, ptr, offset, value), kArrayHeaderAliasClass);
}

// An Array's elements block (its address) and head offset: element i is the
// word at block + 8 * (head + i) + kBronzeHdrBytes. A read takes them with
// the length before its bounds test, so in a loop that holds the array brass
// hoists all three (it moves no load past a test of the load's object).
struct ArrayElements {
    brass::Value* block = nullptr;
    brass::Value* head = nullptr;
};
inline ArrayElements array_elements(brass::Builder& b, brass::Value* ptr) {
    brass::Value* block = b.build_and(array_header_load(b, brass::Type::i64(), ptr, BRONZE_ABI_ARRAY_ELEMS_OFFSET),
                                      b.build_iconst_i64(0x0000FFFFFFFFFFFFLL));
    return {block, b.build_zext_i64(array_header_load(b, brass::Type::i32(), ptr, BRONZE_ABI_ARRAY_HEAD_OFFSET))};
}
// Element `idx` of an Array (array_elements), a boxed value.
inline brass::Value* array_element_load(brass::Builder& b, const ArrayElements& e, brass::Value* idx) {
    return with_alias_class(b.build_load_indexed(brass::Type::i64(), e.block, b.build_add(idx, e.head), 8,
                                                 static_cast<int32_t>(BRONZE_ABI_HDR_BYTES)),
                            kBoxedElementAliasClass);
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
// Slow site `index`'s counter, relative to the IC table: the module's
// `count` counters end where the table starts (brass_backend_sections.cpp).
constexpr int64_t ic_slow_count_offset(uint32_t index, uint32_t count) {
    return -8 * static_cast<int64_t>(count - index);
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
// The indices one slow site takes (il/feedback_site.h kSlowSiteArms): its
// slow arm's and its ARMS' (il_speculation.h).
inline constexpr uint32_t kSlowSiteArms = 8;
// A named property site's last shape and its count of shape turns
// (il/feedback_site.h).
inline constexpr uint32_t kLastShapeArm = 6;
inline constexpr uint32_t kShapeTurnArm = 7;
// PROVEN FACTS (il_speculation.h, resolve_fact_markers): the two marker
// calls of an image's lowering, each taking a fact's id, both gone before
// any pass runs.
inline constexpr std::string_view kFactProofSymbol = "__bronze_fact_proof";
inline constexpr std::string_view kFactProvenSymbol = "__bronze_fact_proven";
inline constexpr std::string_view kTwinProbeSuffix = "__probe";
inline constexpr std::string_view kTwinEntryCellPrefix = "__bronze_twin_entry_";
inline constexpr int32_t kTwinCellEntry = 0;
inline constexpr int32_t kTwinCellExits = 8;
inline constexpr int32_t kTwinCellCalls = 16;
inline constexpr size_t kTwinCellBytes = 24;

} // namespace il2mir
