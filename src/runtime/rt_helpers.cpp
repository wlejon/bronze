#include "runtime/rt_helpers.h"

#include <string>
#include <vector>

#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

static std::vector<std::string> g_keyStrings;
static std::vector<InlineCache> g_inlineCaches;

extern "C" {

uint64_t bronze_box_f64(double v) {
    return Value::fromDouble(v).rawBits();
}

uint64_t bronze_box_i32(int32_t v) {
    return Value::fromTagAndPayload(static_cast<uint16_t>(Tag::Int32), static_cast<uint32_t>(v)).rawBits();
}

uint64_t bronze_box_bool(bool v) {
    return Value::fromBool(v).rawBits();
}

uint64_t bronze_box_str(const char* s) {
    if (!s) return Value::fromUndefined().rawBits();
    static Heap heap(1024 * 1024);
    StringHeader* sh = StringHeader::createFromUTF8(heap, std::string_view(s));
    return Value::fromString(sh).rawBits();
}

double bronze_unbox_f64(uint64_t bits) {
    Value v(bits);
    if (v.isNumber()) return v.asNumber();
    if (v.isBool()) return v.asBool() ? 1.0 : 0.0;
    return 0.0;
}

int32_t bronze_unbox_i32(uint64_t bits) {
    Value v(bits);
    if (v.isInt32()) return static_cast<int32_t>(v.payload());
    if (v.isNumber()) return static_cast<int32_t>(v.asNumber());
    if (v.isBool()) return v.asBool() ? 1 : 0;
    return 0;
}

bool bronze_unbox_bool(uint64_t bits) {
    Value v(bits);
    if (v.isBool()) return v.asBool();
    if (v.isNumber()) return v.asNumber() != 0.0;
    return false;
}

uint64_t bronze_prop_get(uint64_t objBits, uint32_t keyIndex, uint32_t icIndex) {
    Value objVal(objBits);
    if (!objVal.isObject()) {
        return Value::fromUndefined().rawBits();
    }
    static Heap heap(1024 * 1024);
    if (icIndex >= g_inlineCaches.size()) {
        g_inlineCaches.resize(icIndex + 1);
    }
    InlineCache* ic = &g_inlineCaches[icIndex];
    std::string keyStr = (keyIndex < g_keyStrings.size()) ? g_keyStrings[keyIndex] : "";
    StringHeader* propName = StringHeader::createFromUTF8(heap, std::string_view(keyStr));
    Rooted<Value> key(Value::fromString(propName));
    ObjectHeader* obj = objVal.asObject<ObjectHeader>();
    Value result = obj->getProp(heap, key, ic);
    return result.rawBits();
}

void bronze_prop_set(uint64_t objBits, uint32_t keyIndex, uint64_t valBits, uint32_t icIndex) {
    Value objVal(objBits);
    Value valVal(valBits);
    if (!objVal.isObject()) return;
    static Heap heap(1024 * 1024);
    static NonMovingArena arena;
    if (icIndex >= g_inlineCaches.size()) {
        g_inlineCaches.resize(icIndex + 1);
    }
    InlineCache* ic = &g_inlineCaches[icIndex];
    std::string keyStr = (keyIndex < g_keyStrings.size()) ? g_keyStrings[keyIndex] : "";
    StringHeader* propName = StringHeader::createFromUTF8(heap, std::string_view(keyStr));
    Rooted<Value> key(Value::fromString(propName));
    Rooted<Value> val(valVal);
    ObjectHeader* obj = objVal.asObject<ObjectHeader>();
    obj->setProp(heap, arena, key, val, ic);
}

uint64_t bronze_dynamic_call(uint64_t calleeBits, uint64_t thisBits, uint32_t argc, const uint64_t* argvBits) {
    Value calleeVal(calleeBits);
    Value thisVal(thisBits);
    if (!calleeVal.isObject()) {
        return Value::fromUndefined().rawBits();
    }
    FunctionHeader* fn = calleeVal.asObject<FunctionHeader>();
    std::vector<Value> args(argc);
    for (uint32_t i = 0; i < argc; ++i) {
        args[i] = Value(argvBits[i]);
    }
    Value res = fn->call(thisVal, argc, args.data());
    return res.rawBits();
}

void bronze_register_key_string(uint32_t index, const char* str) {
    if (index >= g_keyStrings.size()) {
        g_keyStrings.resize(index + 1);
    }
    g_keyStrings[index] = str ? str : "";
}

}  // extern "C"
}  // namespace bronze::runtime
