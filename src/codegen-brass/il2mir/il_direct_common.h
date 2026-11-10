// What the module's out-of-line dispatch stubs share (il_lowering_direct.cpp,
// the method IC's; il_prop_stubs.cpp, the property sites'): the tests a direct
// entry makes of a callee, a property site's ways as the stubs scan them, the
// thread's megamorphic table's probe, and the prototype walk. Internal to the
// lowering: the names live in il2mir::direct, which only those files open.
#pragma once

#include "il_abi.h"
#include "il_property.h"
#include <brass/mir/builder.hpp>
#include <cstdint>
#include <string>

namespace il2mir {

class IlLowering;

namespace direct {

inline constexpr uint64_t kObjectTagBits = uint64_t{BRONZE_ABI_TAG_OBJECT} << 48;
inline constexpr int64_t kPayloadLimit = int64_t{1} << 48;
inline constexpr int32_t kFunctionHeaderLow = (BRONZE_ABI_OBJ_FLAGS_FUNCTION << 16) | BRONZE_ABI_TAG_OBJECT;
inline constexpr int32_t kPlainHeaderLow = (BRONZE_ABI_OBJ_FLAGS_PLAIN << 16) | BRONZE_ABI_TAG_OBJECT;
inline constexpr int32_t kWord = 8;
inline constexpr int64_t kSlotFormLimit = int64_t{1} << BRONZE_ABI_METHOD_IC_SLOT_SHIFT;
// The flags word's realm bytes, as a mask over the whole word.
inline constexpr uint64_t kRealmMask = ~uint64_t{0} << BRONZE_ABI_FN_REALM_SHIFT;
// A call's test: the direct-call bit set and the class-constructor bit clear
// (a call of a class constructor throws, which only the helper does).
inline constexpr uint64_t kCallMask = kRealmMask | BRONZE_ABI_FN_FLAG_DIRECT_CALL | BRONZE_ABI_FN_FLAG_CLASS_CTOR;
inline constexpr uint64_t kCallBits = BRONZE_ABI_FN_FLAG_DIRECT_CALL;
// A construction's: the direct-call bit set and the vet byte exactly 1 — the
// helper's plain path has run for this function object, so it is no bound
// function, no primitive-wrapper intrinsic and no native base, and its
// prototype and instance shape exist (bronze_abi.h, the inline `new`
// contract). A class constructor may be constructed this way.
inline constexpr int kVetShift = (BRONZE_ABI_FN_CTOR_VETTED_OFFSET - BRONZE_ABI_FN_FLAGS_WORD_OFFSET) * 8;
inline constexpr uint64_t kConstructMask = kRealmMask | (uint64_t{0xFF} << kVetShift) | BRONZE_ABI_FN_FLAG_DIRECT_CALL;
inline constexpr uint64_t kConstructBits = (uint64_t{1} << kVetShift) | BRONZE_ABI_FN_FLAG_DIRECT_CALL;
static_assert(BRONZE_ABI_FN_REALM_SHIFT > kVetShift + 8, "the realm bytes sit above the vet byte");

// An entry's slot word is (depth << 32) | slot, the flags in the depth half:
// below kOwnDataLimit it is an own data slot.
inline constexpr int64_t kOwnDataLimit = int64_t{1} << 32;
inline constexpr int64_t kAccessorDepth = int64_t{BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG};
inline constexpr int64_t kStaticsDepth = int64_t{BRONZE_ABI_IC_DEPTH_STATICS_FLAG};
inline constexpr int64_t kSlotMask = 0xFFFFFFFFLL;
// The deepest entry the stubs walk to; deeper ones are the helper's. Four
// links cover a three.js leaf class's accessors (Mesh -> Object3D ->
// EventDispatcher) with one to spare.
inline constexpr int64_t kMaxStubDepth = 4;

Value* tls_block(Builder& b);
std::string block_prefix(Builder& b, const char* what);

// Whether `callee` is a function object whose flags word, under `mask`, is
// `bits` with this thread's current realm in the realm bytes; and the
// address its fields are read through, which is the TLS block (mapped, and
// never read for an answer) for anything else, so no load here can fault.
struct EntryTest {
    Value* ok = nullptr;
    Value* fn = nullptr;
};
EntryTest entry_test(Builder& b, Value* tls, Value* callee, uint64_t mask, uint64_t bits);
// entry_test as branches: a callee that is no object, or no function, goes to
// `miss` before anything past its header is read, and `ok` is the flags word's
// test alone, made where both held; `fn` is the payload.
EntryTest entry_test_br(Builder& b, Value* tls, Value* callee, uint64_t mask, uint64_t bits, BasicBlock* miss);

Value* call_code(Builder& b, Value* code, Value* env, Value* this_val, Value* argc, Value* argv);
Value* call_code(Builder& b, Value* code, Value* env, Value* this_val, uint32_t argc, Value* argv);

// What a property site's cache says about a plain receiver, as the stubs
// read it: whether one of the `ways` entries names the receiver's shape, that
// entry's slot word and the epoch it was filled at, which a depth > 0 answer
// needs to be the thread's.
struct PropEntry {
    // Null when `shape` is the receiver's keyed shape (keyed_shape), whose
    // compare with any way or table entry already says "a plain object".
    Value* plain = nullptr;
    Value* ptr = nullptr;
    Value* shape = nullptr;
    Value* hit = nullptr;
    Value* slot_word = nullptr;
    Value* epoch = nullptr;
};

// The scan of ways [first, ways) for the receiver `e` already describes.
void scan_ways(Builder& b, PropEntry& e, Value* site, int first, int ways);
// The receiver as the stubs read it: its keyed shape and its payload.
void keyed_receiver(PropertyLoweringHelper& pl, Builder& b, Value* obj, PropEntry& e);
PropEntry prop_entry(PropertyLoweringHelper& pl, Builder& b, Value* obj, Value* site, int ways);

// The probe of the thread's megamorphic table (bronze_abi_tls.h, mega_prop)
// for the receiver `e` names under `key`: branches to `hit` with the entry's
// slot word and epoch, or to `miss`. Only a FULL site probes (`full`, or the
// site's last way holding a shape).
void build_mega_probe_if(Builder& b, Value* tls, const PropEntry& e, Value* full, Value* key, BasicBlock* hit,
                         BasicBlock* miss);
void build_mega_probe(Builder& b, Value* tls, const PropEntry& e, Value* site, int ways, Value* key,
                      BasicBlock* hit, BasicBlock* miss);

// Whether `depth` (the slot word's high half) is an entry at 1..kMaxStubDepth
// with no flag but, at most, the accessor's: not absent, not an f64 slot.
Value* walkable_depth(Builder& b, Value* depth);
// The holder of a depth-`depth` entry (the accessor flag ignored), walked the
// way ObjectHeader::cachedProtoHolder walks: every link a plain object whose
// shape is no dictionary, else `slow`. The builder leaves in the block where
// the result is the holder's payload.
Value* walk_to_prototype(Builder& b, Value* shape, Value* depth, BasicBlock* slow, const std::string& prefix);

// The property sites' stubs (il_prop_stubs.cpp).
void build_prop_get_stub(IlLowering* lowering, Function* fn);
void build_prop_set_stub(IlLowering* lowering, Function* fn);

}  // namespace direct
}  // namespace il2mir
