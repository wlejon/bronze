#pragma once

#include <string>

#include "runtime/gc.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace bronze {

// A Proxy exotic object (ECMA-262 10.5), scoped to the traps bronze has
// built: `get`, `set` and `has`. The scope is enforced where it can be — at
// construction — rather than discovered later: rtProxyCreate walks the
// handler's own keys and refuses any other trap by name, and it refuses a
// callable target the same way. That one gate is what lets every operation
// bronze has NOT built (delete, ownKeys, defineProperty, and the rest)
// forward to the target unconditionally: a handler that could have
// intercepted them never gets past construction, so forwarding IS the
// spec's answer for the proxies that exist.
struct ProxyHeader {
    HeapObjectHeader header;
    Value target;   // always an object, never callable (the gate refuses those)
    Value handler;  // always a plain object whose own keys ⊆ {get, set, has}

    static constexpr uint16_t kFlags = HeapKind::Proxy;

    // Rooted operands, not Values: the allocation inside can move both, and
    // a by-value copy would store the from-space address it was handed.
    static ProxyHeader* create(Heap& heap, Rooted<Value>& target, Rooted<Value>& handler);
};

namespace runtime {

// 28.2.1.1 Proxy(target, handler), behind the gate described above.
uint64_t rtProxyConstructor(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv);

// [[Get]] (10.5.8): the `get` trap if the handler carries one, else the
// target's own read through the ordinary funnel.
Value rtProxyGet(Value proxyVal, Value keyVal);

// [[Set]] (10.5.9): the `set` trap or the forwarded write. The trap's
// boolean result is read the way 13.15.2 reads it: false under strict is a
// TypeError, false otherwise is a quiet refusal.
void rtProxySet(Value proxyVal, Value keyVal, Value val, bool strict);

// [[HasProperty]] (10.5.7): the `has` trap or the forwarded `in`.
bool rtProxyHas(Value proxyVal, Value keyVal);

}  // namespace runtime
}  // namespace bronze
