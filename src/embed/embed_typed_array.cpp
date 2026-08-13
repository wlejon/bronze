// Raw access to the program's typed arrays and ArrayBuffers, for a host that
// consumes bytes in place — the seam a GL binding uploads through.
//
// Neither function allocates, roots, or opens a frame: each is a read of a
// header the caller's Value already names, and keeping them allocation-free
// is what makes the answered pointer usable at all. The pointer contract is
// embed.h's, repeated because it is the entire risk of this file: `data`
// points into the moving semispace heap and dies at the NEXT allocation —
// the caller consumes it synchronously or copies it out, never stores it.

#include "embed/embed.h"
#include "runtime/heap.h"
#include "runtime/typed_array.h"
#include "runtime/value.h"

namespace bronze::embed {

TypedArrayInfo typedArrayInfo(Value v) {
    if (!v.isObject()) return {};
    auto* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::TypedArray) return {};
    auto* view = reinterpret_cast<TypedArrayHeader*>(hdr);
    // bytes() recomputes the address from the buffer Value it holds, so this
    // is the buffer's CURRENT location — current, that is, until the caller
    // lets anything allocate.
    return TypedArrayInfo{view->bytes(), view->byteLength(), view->length,
                          view->bytesPerElement(), view->elementKind()};
}

ArrayBufferInfo arrayBufferInfo(Value v) {
    if (!v.isObject()) return {};
    auto* hdr = v.asObject<HeapObjectHeader>();
    if (hdr->flags != HeapKind::ArrayBuffer) return {};
    auto* buf = reinterpret_cast<ArrayBufferHeader*>(hdr);
    return ArrayBufferInfo{buf->data(), buf->byteLength};
}

}  // namespace bronze::embed
