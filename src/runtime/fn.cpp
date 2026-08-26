#include "runtime/fn.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace bronze {

FunctionHeader* FunctionHeader::create(Heap& heap, NativeFunctionCode code, Value env_record,
                                       uint32_t arity, uint32_t function_flags) {
    size_t payload_bytes = sizeof(FunctionHeader) - sizeof(HeapObjectHeader);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    // HERE, and not at the four call sites that used to do it after this
    // returns. `Heap::allocate` leaves `flags` zero, which reads as
    // `HeapKind::Plain` — and the collector now takes a plain header at its
    // word: it reads a `Shape*` at offset 8, which for this layout is the
    // `code` pointer. The window was only ever as wide as the caller's next
    // few lines; it is now not a window at all.
    raw_hdr->flags = HeapKind::Function;
    auto* fn = reinterpret_cast<FunctionHeader*>(raw_hdr);
    fn->code = code;
    fn->env_record = env_record;
    fn->prototype = Value::fromUndefined();
    fn->properties = Value::fromUndefined();
    fn->instance_shape = nullptr;
    fn->name = nullptr;
    fn->arity = arity;
    fn->length = 0;
    fn->function_flags = static_cast<uint8_t>(function_flags);
    // Explicit, like every field here: the memory is a raw heap block, so a
    // field the constructor-syntax initializers name is still garbage until
    // this function writes it.
    fn->construct_vetted = false;
    // NOT ordinary padding, and the difference is why it is written here rather
    // than left to the loop below: `bronze_construct` DISPATCHES on this byte,
    // so residue in it makes an ordinary closure allocate a Map — a type
    // confusion whose symptom is a segfault in whatever reads the object next,
    // and which only appears once the heap has recycled a block with the right
    // byte in the right place.
    fn->native_base = 0;
    fn->prototype_readonly = false;
    // The word these bools share is scanned as a Value; unwritten padding in
    // it is recycled-memory residue that can parse as a heap pointer (fn.h).
    for (uint8_t& b : fn->padding_to_value_scan) b = 0;
    // Undefined, not null: null is a legal [[Prototype]] a program can set,
    // so the "never written" state needs a value of its own.
    fn->parent = Value::fromUndefined();
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
    constexpr uint32_t kStackArgsCap = 32;
    Value stack_args[kStackArgsCap];
    Value* args_data = stack_args;
    std::vector<Value> heap_args;
    if (arity > kStackArgsCap) {
        heap_args.resize(arity, Value::fromUndefined());
        args_data = heap_args.data();
    } else {
        for (uint32_t i = argc; i < arity; ++i) {
            stack_args[i] = Value::fromUndefined();
        }
    }
    for (uint32_t i = 0; i < argc; ++i) {
        args_data[i] = argv[i];
    }

    return Value(code(env_record.rawBits(), thisArg.rawBits(), arity,
                      reinterpret_cast<const uint64_t*>(args_data)));
}

}  // namespace bronze
