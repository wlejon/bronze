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
// Element `idx` of an Array (array_elements), a boxed value; loaded as an
// f64 for an element known to be a Number in its double form (its bits are
// the box: il_bridge_verifier.cpp is_value_bits_f64).
inline brass::Value* array_element_load(brass::Builder& b, const ArrayElements& e, brass::Value* idx,
                                        brass::Type type = brass::Type::i64()) {
    return with_alias_class(b.build_load_indexed(type, e.block, b.build_add(idx, e.head), 8,
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
// Slow site `index`'s counter, relative to the IC table: the module's
// `count` counters end where the table starts (brass_backend_sections.cpp).
constexpr int64_t ic_slow_count_offset(uint32_t index, uint32_t count) {
    return -8 * static_cast<int64_t>(count - index);
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

// The indices one slow site takes (il/feedback_site.h kSlowSiteArms): its
// slow arm's and its ARMS' (il_property.h arm_begin).
inline constexpr uint32_t kSlowSiteArms = 8;
// A named property site's last shape and its count of shape turns
// (il/feedback_site.h).
inline constexpr uint32_t kLastShapeArm = 6;
// An element read's or write's arms (opt/speculate/evidence.h kElemArmArray
// and on): an in-bounds present element of a dense Array at an int32 index, a
// typed array, a write that appended at the length (inline, within the
// capacity), and a read of an Array that found no element there (which goes
// on to count the slow arm). Anything else counts the site's slow arm only.
inline constexpr uint32_t kElemArmArray = 1;
inline constexpr uint32_t kElemArmView = 2;
inline constexpr uint32_t kElemArmGrow = 3;
inline constexpr uint32_t kElemArmMiss = 4;
// A build made to be harvested names the view's element kind too: every
// typed-array access marks kElemArmView, and the bits of its kind's code
// (kElemViewKinds' index) mark kElemArmKindBit0..2, so a site that met one
// kind has each bit arm's count either zero or kElemArmView's. A kind with no
// code (Int8, Float16, the BigInt kinds) and a view index at or past the
// length mark kElemArmMiss (il_elem_lowering.cpp mark_view_arm).
inline constexpr uint32_t kElemArmKindBit0 = 5;
inline constexpr uint32_t kElemArmKindBit1 = 6;
inline constexpr uint32_t kElemArmKindBit2 = 7;
// A read of an Array whose NUMBERS bit was clear and could not be claimed
// (bronze_array_claim_numbers): opt/speculate/evidence.h. An Array read never
// marks the kind bits, which only views mark.
inline constexpr uint32_t kElemArmNoNumbers = 5;
// `is.dense_array`'s NUMBERS bit above its max index (il/il_op.h kDenseNumbers).
inline constexpr int64_t kDenseNumbers = int64_t{1} << 30;
inline constexpr int64_t kDenseIndexMask = kDenseNumbers - 1;
// `elem.load`'s immI32 under such a test (il/il_op.h kElemLoadNumber).
inline constexpr int64_t kElemLoadNumber = 1;
// `elem.store`'s / `array.push`'s immI32: the value is a Number (`kStoreNumber`)
// or the index may equal `length` and append (`kStoreGrow`).
inline constexpr int64_t kStoreNumber = 1;
inline constexpr int64_t kStoreGrow = 2;
// `shape.has_own`'s mode bits and slot shift (il/il_op.h kHasOwnWrite).
inline constexpr int64_t kHasOwnWrite = 1;
inline constexpr int64_t kHasOwnNumber = 2;
inline constexpr int64_t kHasOwnSlotShift = 2;
// `slot.load` of a double slot (il/il_op.h SlotLoad with boxType F64): this
// bit set above the slot in the backend's immediate.
inline constexpr int64_t kSlotLoadNumber = int64_t{1} << 40;
inline constexpr int32_t kElemViewKinds[8] = {
    BRONZE_ABI_TA_KIND_FLOAT32, BRONZE_ABI_TA_KIND_FLOAT64, BRONZE_ABI_TA_KIND_INT32,
    BRONZE_ABI_TA_KIND_UINT32,  BRONZE_ABI_TA_KIND_UINT16,  BRONZE_ABI_TA_KIND_INT16,
    BRONZE_ABI_TA_KIND_UINT8,   BRONZE_ABI_TA_KIND_UINT8CLAMPED,
};
// A dynamic numeric op's arms (il_lowering_numeric.cpp; opt/speculate/
// evidence.h kArithArmNumbers and on): its Numbers computed inline, per
// operand one that was no int32, and a Number result that was no int32.
inline constexpr uint32_t kArithArmNumbers = 1;
inline constexpr uint32_t kArithArmDouble0 = 2;
inline constexpr uint32_t kArithArmDouble1 = 3;
inline constexpr uint32_t kArithArmResultDouble = 4;
// A `.length` read's arms (opt/speculate/evidence.h kLengthArmArray and on):
// answered from an Array's, a String's or a typed array's header.
inline constexpr uint32_t kLengthArmArray = 1;
inline constexpr uint32_t kLengthArmString = 2;
inline constexpr uint32_t kLengthArmView = 3;
inline constexpr uint32_t kShapeTurnArm = 7;
// A call's count of runs in a build made to be harvested (il/feedback_site.h).
inline constexpr uint32_t kCallCountArm = 1;
// An allocation site's counts of objects and of their promotions
// (il/feedback_site.h kAllocCountArm).
inline constexpr uint32_t kAllocCountArm = 2;
inline constexpr uint32_t kAllocTenuredArm = 3;
// A function's loop count, after its entry count (il/feedback_site.h
// kLoopCountOffset).
inline constexpr uint32_t kLoopCountOffset = 1;
// The bit a call site's kLastShapeArm word carries beside bit 63 when it holds
// a callee's code pointer rather than a shape (il/feedback_site.h).
inline constexpr uint32_t kLastCalleeBit = 62;
// THE SHAPE CELLS (il.h Module::shapeCells): `__bronze_shape_cells`, one u64
// per cell in the module's per-thread data, zero until the runtime fills it
// with the shape the harvest named (bronze_register_shape_cells); and
// `__bronze_shape_cell_keys`, read-only, each cell's key as a u32 byte length
// and its bytes, one after another (brass_backend_sections.cpp).
inline constexpr std::string_view kShapeCellsSymbol = "__bronze_shape_cells";
// THE COLD ENTRIES (TranslatorOptions::cold_entry_count): `__bronze_cold_entries`,
// one u64 per function of the module by its IL index, in the module's data:
// a cold function (one its build's harvest saw seldom or never) adds one on
// entry. Exported as `<entry>_cold_entries`, with its word count as the u32
// `<entry>_cold_entry_count` (brass_backend_sections.cpp), for the host
// that ran the module (eval::LoadedModule::coldEntries).
inline constexpr std::string_view kColdEntriesSymbol = "__bronze_cold_entries";
// THE COLD CELLS (TranslatorOptions::interpreted): `__bronze_cold_cells`, one
// code pointer per interpreted cold function, shared by every thread (the
// code it points at runs on any), zero until the function's first call fills
// it; and `__bronze_cold_link`, read-only, what that call hands
// bronze_cold_resolve (codegen-brass/cold_companion.h ColdLink).
inline constexpr std::string_view kColdCellsSymbol = "__bronze_cold_cells";
inline constexpr std::string_view kColdLinkSymbol = "__bronze_cold_link";
inline constexpr std::string_view kShapeCellKeysSymbol = "__bronze_shape_cell_keys";
// `__bronze_number_slot_keys`, read-only: the module's double-slot keys
// (il.h Module::numberSlotKeys) as u32 key constant indices, which the entry
// hands bronze_register_slot_repr with `__bronze_key_map`.
inline constexpr std::string_view kNumberSlotKeysSymbol = "__bronze_number_slot_keys";
// THE LITERAL SHAPES (il.h Module::objectLiterals): two u64 per object
// literal in the module's per-thread data, zero until bronze_object_literal
// fills them: the empty root the literal starts from (the TLS block's sized
// plain shape, which names the realm's Object.prototype) and the shape the
// literal ends at from it.
inline constexpr std::string_view kLiteralShapesSymbol = "__bronze_literal_shapes";
// THE PRETENURED SITES' RECORDS (il.h Module::pretenureSites; docs/type-feedback.md
// "Pretenuring"): one brass::gc::PretenureSite (kPretenureSiteBytes, zero) per
// pretenured site, in the order of the sorted list, which the collector keeps
// the site's probe counts and verdict in. The image's own copy, which every
// thread's code names (IlLowering::pretenure_site): a site's verdict is the
// program's, and a thread's copy may be freed while a probe names it.
inline constexpr std::string_view kPretenureSitesSymbol = "__bronze_pretenure_sites";
inline constexpr size_t kPretenureSiteBytes = 16;
// THE TRANSITION CELLS (il.h Module::protoCells): one u64 per cell in the
// module's per-thread data, the prototype epoch the runtime stamped it with
// (runtime/transition_cells.cpp), zero (no epoch) until then.
inline constexpr std::string_view kProtoCellsSymbol = "__bronze_proto_cells";
// THE MEMBER CELLS (il.h Module::memberCells): two u64 per cell in the
// module's per-thread data, (shape, epoch), zero until the runtime fills them
// (runtime/accessor_cell.cpp).
inline constexpr std::string_view kMemberCellsSymbol = "__bronze_member_cells";
// A member cell's kind as bronze_member_cell_publish numbers it (il.h
// Module::MemberKind): an absent cell names no function, and a data cell
// passes its holder's place (il.h Module::memberDataPlace) for the code.
inline constexpr uint32_t kMemberKindAbsent = 3;
inline constexpr uint32_t kMemberKindData = 4;
// THE ELEMENT CELLS (codegen-brass/elem_cells.h): five u64 per computed read
// in the module's per-thread data, (shape, key bits, slot word, fills,
// validity), zero until bronze_elem_get_cell fills one (runtime/elem_ic.cpp;
// bronze_abi_layout.h BRONZE_ABI_ELEM_CELL_WORDS says what the words mean).
inline constexpr std::string_view kElemCellsSymbol = "__bronze_elem_cells";
inline constexpr int32_t kElemCellBytes = BRONZE_ABI_ELEM_CELL_WORDS * 8;

// The function object's code and environment words (bronze_abi_layout.h,
// BRONZE_ABI_FN_*): what `fn.code_is` and `fn.env` read.
constexpr int32_t kBronzeFnCodeOffset = BRONZE_ABI_FN_CODE_OFFSET;
constexpr int32_t kBronzeFnEnvOffset = BRONZE_ABI_FN_ENV_OFFSET;

// A retirable speculated function's ENTRY CELL (codegen-brass/deopt_sites.h
// DeoptSites::retirable) and its RETIRED ENTRY, the function the cell holds
// once S retires (S's signature, calls G): base names, which the module's
// entry symbol suffixes as it does every module symbol.
inline std::string entry_cell_name(const std::string& s) { return "__bronze_entry_" + s; }
inline std::string retired_entry_name(const std::string& s) { return "__retired_" + s; }

} // namespace il2mir
