#include "runtime/fn.h"

#include <stdexcept>
#include <vector>

namespace bronze {

FunctionHeader* FunctionHeader::create(Heap& heap, NativeFunctionCode code, void* env_record, uint32_t arity) {
    // FunctionHeader payload holds environment record pointer if any, but since env_record field is inside struct:
    // Header sizeof(FunctionHeader) is HeapObjectHeader + code + env_record + arity = 8 + 8 + 8 + 4 = 28 -> rounded up to 32.
    // Heap::allocate takes bytes after HeapObjectHeader.
    size_t payload_bytes = sizeof(FunctionHeader) - sizeof(HeapObjectHeader);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* fn = reinterpret_cast<FunctionHeader*>(raw_hdr);
    fn->code = code;
    fn->env_record = env_record;
    fn->arity = arity;
    return fn;
}

Value FunctionHeader::call(Value thisArg, uint32_t argc, Value* argv) const {
    if (!code) {
        throw std::runtime_error("Attempted to call uninitialized function code pointer");
    }

    if (arity == 0 || argc >= arity) {
        return code(thisArg, argc, argv);
    }

    // Arity adaptation: extend args with undefined up to arity
    std::vector<Value> args(arity, Value::fromUndefined());
    for (uint32_t i = 0; i < argc; ++i) {
        args[i] = argv[i];
    }

    return code(thisArg, arity, args.data());
}

}  // namespace bronze
