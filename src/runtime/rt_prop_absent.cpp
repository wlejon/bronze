// The NEGATIVE half of the property read cache: deciding whether a lookup that
// found nothing may say so in the site's inline cache.
//
// Its own file because it is the only place in the runtime that reasons about
// a read's ABSENCE rather than about where a value lives, and because the
// decision is a list of refusals — the shape of code that grows, and that must
// stay readable as one list rather than as a condition wedged into the middle
// of rt_prop.cpp's plain-object tail.
//
// What makes an absent answer cacheable comes in two halves, and neither is
// re-derived here. The KEY half — the property was found nowhere and the walk
// reached the chain's end — is the witness `ObjectHeader::getProp` records as
// it walks, because a second walk would repeat a shape lookup per link. The
// STRUCTURE half — every link plain, non-dictionary and marked as somebody's
// prototype, which is what makes the epoch cover this chain's mutations — is
// `ObjectHeader::chainIsCacheable`, a few pointer loads and no lookup at all.
//
// This file adds the two refusals that are about neither, but about what ELSE
// a read of this receiver and this key would have done:
//
//  1. A receiver a `*CheckMissingMember` refusal CLAIMED is never cached. Those
//     refusals key on object IDENTITY (`obj.rawBits() == g_mathObject`), and an
//     entry keys on SHAPE — so a cached miss could let a later read of another
//     object with the same shape skip a diagnostic. The caller passes the claim
//     down rather than this file re-deriving it.
//
//  2. An index-like key or `length` is never cached, because a String exotic
//     object synthesises exactly those two families of own property (10.4.3.4,
//     10.4.3.5) ahead of the ordinary walk, and that synthesis is a fact about
//     the receiver's string data rather than about its shape.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/object.h"
#include "runtime/property_key.h"
#include "runtime/rt_property.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace bronze::runtime {

void rtInstallAbsentEntry(InlineCacheSite* site, Value objVal, const std::string& keyStr) {
    if (!site) return;
    // The seam. Gating only the INSTALL is the whole of BRONZE_NO_NEG_IC=1:
    // with no absent entry ever written, generated code's absent arm is
    // unreachable and every absent read takes the helper it always took.
    if (!rtNegativeIcEnabled()) return;
    if (!objVal.isObject()) return;
    auto* hdr = objVal.asObject<HeapObjectHeader>();
    if (hdr->flags != BRONZE_ABI_OBJ_FLAGS_PLAIN) return;

    uint32_t index = 0;
    if (keyStr == "length" || rtKeyAsIndex(keyStr, index)) return;

    auto* obj = reinterpret_cast<ObjectHeader*>(hdr);
    if (!obj->chainIsCacheable()) return;

    if (InlineCache* into = site->slotForInstall(obj->shape, rtIcWayLimit())) {
        into->fillAbsent(obj->shape);
    }
}

}  // namespace bronze::runtime
