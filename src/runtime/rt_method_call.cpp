#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/profile.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_state.h"
#include "runtime/tls_block.h"
#include "runtime/value.h"

namespace bronze::runtime {

extern "C" {

uint64_t bronze_call_method(uint64_t thisBits, uint32_t keyIndex, uint32_t argc,
                            const uint64_t* argvBits, uint64_t* icEntry) {
    Rooted<Value> thisRoot{Value(thisBits)};
    uint64_t fnBits = bronze_prop_get(thisBits, keyIndex, nullptr);
    if (rtExceptionPending()) return BRONZE_ABI_UNDEFINED_BITS;

    recordCallSite("bronze_call_method", fnBits);

    Rooted<Value> fnRoot{Value(fnBits)};
    Value thisVal = thisRoot.get();
    Value fnVal = fnRoot.get();

    if (thisVal.isObject() && fnVal.isObject()) {
        auto* objHdr = thisVal.asObject<HeapObjectHeader>();
        auto* fnHdr = fnVal.asObject<HeapObjectHeader>();
        if (objHdr->flags == HeapKind::Plain && fnHdr->flags == HeapKind::Function) {
            auto* obj = reinterpret_cast<ObjectHeader*>(objHdr);
            auto* fn = reinterpret_cast<FunctionHeader*>(fnHdr);
            if (icEntry && rtTls()->method_call_ic_enabled != 0 && fn->code) {
                if (fn->env_record.isUndefined() ||
                    fn->env_record.rawBits() == Value::fromObject(fn).rawBits()) {
                    icEntry[0] = reinterpret_cast<uint64_t>(obj->shape);
                    icEntry[1] = reinterpret_cast<uint64_t>(fn->code);
                    icEntry[2] = static_cast<uint64_t>(fn->arity);
                }
            }
        }
    }

    return bronze_dynamic_call(fnRoot.get().rawBits(), thisRoot.get().rawBits(), argc, argvBits);
}

uint64_t bronze_call_method_spread(uint64_t thisBits, uint32_t keyIndex, uint64_t argsArrBits,
                                   uint64_t* icEntry) {
    Rooted<Value> thisRoot{Value(thisBits)};
    Rooted<Value> argsRoot{Value(argsArrBits)};
    uint64_t fnBits = bronze_prop_get(thisBits, keyIndex, nullptr);
    if (rtExceptionPending()) return BRONZE_ABI_UNDEFINED_BITS;

    recordCallSite("bronze_call_method_spread", fnBits);

    Rooted<Value> fnRoot{Value(fnBits)};
    Value thisVal = thisRoot.get();
    Value fnVal = fnRoot.get();

    if (thisVal.isObject() && fnVal.isObject()) {
        auto* objHdr = thisVal.asObject<HeapObjectHeader>();
        auto* fnHdr = fnVal.asObject<HeapObjectHeader>();
        if (objHdr->flags == HeapKind::Plain && fnHdr->flags == HeapKind::Function) {
            auto* obj = reinterpret_cast<ObjectHeader*>(objHdr);
            auto* fn = reinterpret_cast<FunctionHeader*>(fnHdr);
            if (icEntry && rtTls()->method_call_ic_enabled != 0 && fn->code) {
                if (fn->env_record.isUndefined() ||
                    fn->env_record.rawBits() == Value::fromObject(fn).rawBits()) {
                    icEntry[0] = reinterpret_cast<uint64_t>(obj->shape);
                    icEntry[1] = reinterpret_cast<uint64_t>(fn->code);
                    icEntry[2] = static_cast<uint64_t>(fn->arity);
                }
            }
        }
    }

    return bronze_dynamic_call_spread(fnRoot.get().rawBits(), thisRoot.get().rawBits(),
                                      argsRoot.get().rawBits());
}

}  // extern "C"

}  // namespace bronze::runtime
