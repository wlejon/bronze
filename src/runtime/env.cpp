#include "runtime/env.h"

#include "runtime/fatal.h"

namespace bronze {

EnvHeader* EnvHeader::create(Heap& heap, Rooted<Value>& parent, uint32_t slot_count) {
    size_t payload_bytes = sizeof(Value) + static_cast<size_t>(slot_count) * sizeof(Value);
    HeapObjectHeader* raw_hdr = heap.allocate(payload_bytes, Tag::Object);
    auto* env = reinterpret_cast<EnvHeader*>(raw_hdr);
    env->header.flags = kFlags;
    // Read the parent through the root: allocating may have moved it.
    env->parent = parent.get();

    Value* slots = env->slotsData();
    for (uint32_t i = 0; i < slot_count; ++i) {
        slots[i] = Value::fromUndefined();
    }
    return env;
}

EnvHeader* EnvHeader::ancestor(uint32_t depth) noexcept {
    EnvHeader* env = this;
    for (uint32_t i = 0; i < depth; ++i) {
        if (!env->parent.isObject()) {
            fatal("environment chain is shorter than the captured variable's depth");
        }
        env = env->parent.asObject<EnvHeader>();
        if (env->header.flags != kFlags) {
            fatal("environment parent link does not point at an environment record");
        }
    }
    return env;
}

}  // namespace bronze
