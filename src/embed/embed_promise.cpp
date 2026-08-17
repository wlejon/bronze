// Promise creation and settlement for the embedding API.
//
// Every function here allocates or may allocate (resolving adopts thenables,
// which runs user getters), so each opens a ShadowStackFrame and holds its
// values through Rooted<> handles. Settling schedules reaction jobs through the
// runtime's microtask queue, which drainMicrotasks() drains.

#include "embed/embed.h"
#include "runtime/gc.h"
#include "runtime/promise.h"
#include "runtime/value.h"

namespace bronze::embed {

Value createPromise() {
    ShadowStackFrame frame;
    return runtime::rtNewPromise();
}

void resolvePromise(Value promise, Value value) {
    ShadowStackFrame frame;
    Rooted<Value> pRoot{promise};
    Rooted<Value> vRoot{value};
    runtime::rtResolvePromise(pRoot, vRoot);
}

void rejectPromise(Value promise, Value reason) {
    ShadowStackFrame frame;
    Rooted<Value> pRoot{promise};
    Rooted<Value> rRoot{reason};
    runtime::rtRejectPromise(pRoot, rRoot);
}

bool isPromise(Value v) {
    // A frame even though this only answers a question: rtIsPromise reads the
    // promise prototype, and taking it can BUILD the intrinsics — an
    // allocation, which is why that function roots its receiver. Rooting into
    // no frame is rooting into nothing, so the brand check would compare a
    // pre-collection address on the one call that triggers the build.
    ShadowStackFrame frame;
    return runtime::rtIsPromise(v);
}

}  // namespace bronze::embed
