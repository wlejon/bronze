#include "runtime/fn.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace bronze {

FunctionHeader* FunctionHeader::create(Heap& heap, NativeFunctionCode code, Value env_record, uint32_t arity) {
    size_t payload_bytes = sizeof(FunctionHeader) - sizeof(HeapObjectHeader);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* fn = reinterpret_cast<FunctionHeader*>(raw_hdr);
    fn->code = code;
    fn->env_record = env_record;
    fn->prototype = Value::fromUndefined();
    fn->properties = Value::fromUndefined();
    fn->instance_shape = nullptr;
    fn->arity = arity;
    return fn;
}

Value FunctionHeader::call(Value thisArg, uint32_t argc, Value* argv) const {
    if (!code) {
        std::cerr << "Hard runtime error: Attempted to call uninitialized function code pointer" << std::endl;
        std::abort();
    }

    static_assert(sizeof(Value) == sizeof(uint64_t) && alignof(Value) == alignof(uint64_t));
    if (arity == 0 || argc >= arity) {
        return Value(code(env_record.rawBits(), thisArg.rawBits(), argc,
                          reinterpret_cast<const uint64_t*>(argv)));
    }

    // Arity adaptation: extend args with undefined up to arity. Unrooted, and
    // safe only because the callee's prologue stores its parameters into its
    // own root frame before it can allocate.
    std::vector<Value> args(arity, Value::fromUndefined());
    for (uint32_t i = 0; i < argc; ++i) {
        args[i] = argv[i];
    }

    return Value(code(env_record.rawBits(), thisArg.rawBits(), arity,
                      reinterpret_cast<const uint64_t*>(args.data())));
}

}  // namespace bronze
