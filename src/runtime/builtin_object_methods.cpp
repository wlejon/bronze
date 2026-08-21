#include "runtime/builtin_object.h"

#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_property.h"
#include "runtime/rt_receivers.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/symbol.h"
#include "runtime/value.h"

namespace bronze::runtime {

uint64_t objectGetOwnPropertySymbols(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    if (!args[0].isObject()) {
        return rtThrowTypeError(
                   "Object.getOwnPropertySymbols called on a value that is not an object")
            .rawBits();
    }
    if (rtIsModuleNamespace(args[0])) {
        Rooted<Value> out{Value(bronze_create_array(0))};
        Rooted<Value> tag{Value::fromSymbol(rtSymbolToStringTag())};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, tag);
        return out.get().rawBits();
    }
    Rooted<Value> self{args[0]};
    ObjectHeader* holder = nullptr;
    HeapObjectHeader* hdr = self.get().asObject<HeapObjectHeader>();
    if (hdr->flags == BRONZE_ABI_OBJ_FLAGS_PLAIN) {
        holder = reinterpret_cast<ObjectHeader*>(hdr);
    } else if (hdr->flags == HeapKind::Function) {
        Value props = self.get().asObject<FunctionHeader>()->properties;
        if (props.isObject()) holder = props.asObject<ObjectHeader>();
    }
    if (!holder) return bronze_create_array(0);

    const std::vector<PropertyKey> ordered =
        rtOwnKeysOrdered(holder, /*enumerableOnly=*/false);
    Rooted<Value> out{Value(bronze_create_array(0))};
    uint32_t at = 0;
    for (PropertyKey key : ordered) {
        if (!key.isSymbol()) continue;
        Rooted<Value> sym{key.toValue()};
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at++, sym);
    }
    return out.get().rawBits();
}

uint64_t objectKeys(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return bronze_object_keys(args[0].rawBits());
}

static uint64_t enumerableOwn(Value source, bool wantEntries) {
    Rooted<Value> src{source};
    Rooted<Value> keys{Value(bronze_object_keys(src.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    Rooted<Value> out{Value(bronze_create_array(0))};
    const uint32_t count = keys.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(i)};
        Rooted<Value> val{Value(bronze_elem_get(src.get().rawBits(), key.get().rawBits()))};
        if (rtExceptionPending()) return out.get().rawBits();
        Rooted<Value> item;
        if (wantEntries) {
            Rooted<Value> pair{Value(bronze_create_array(2))};
            pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 0, key);
            pair.get().asObject<ArrayHeader>()->setElem(rtHeap(), 1, val);
            item.set(pair.get());
        } else {
            item.set(val.get());
        }
        const uint32_t at = out.get().asObject<ArrayHeader>()->length;
        out.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, item);
    }
    return out.get().rawBits();
}

uint64_t objectValues(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return enumerableOwn(args[0], /*wantEntries=*/false);
}

uint64_t objectEntries(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    return enumerableOwn(args[0], /*wantEntries=*/true);
}

static Value toObjectForAssign(Value v) {
    if (v.isNull() || v.isUndefined()) {
        rtThrowTypeError("Object.assign called on a value that is not an object");
        return Value::fromUndefined();
    }
    if (v.isString()) {
        Rooted<Value> str{v};
        return rtMakeStringWrapper(str);
    }
    if (v.isBool()) return rtMakeBooleanWrapper(v.asBool());
    if (v.isNumber()) return rtMakeNumberWrapper(v.asNumber());
    if (v.isBigInt()) {
        fatal("unsupported: a BigInt wrapper object (7.1.18 ToObject boxes a BigInt, and bronze "
              "builds no BigInt object — 21.2.3 gives BigInt.prototype no [[BigIntData]] slot "
              "for one to carry)");
    }
    if (!v.isObject()) {
        fatal("unsupported: Object.assign with a symbol as the target (7.1.18 boxes it in a "
              "Symbol object, and bronze builds none — 20.4.3 makes Symbol.prototype an "
              "ordinary object with no [[SymbolData]] slot, so there is nothing for the box "
              "to be)");
    }
    if (rtObjectIsPlain(v)) return v;
    if (v.asObject<HeapObjectHeader>()->flags == HeapKind::Proxy) return v;
    refuseObjectKind(v, "assign");
}

uint64_t objectAssign(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> target{toObjectForAssign(args[0])};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    for (uint32_t i = 1; i < args.count(); ++i) {
        Rooted<Value> src{args[i]};
        bronze_object_spread(target.get().rawBits(), src.get().rawBits());
        if (rtExceptionPending()) break;
    }
    return target.get().rawBits();
}

uint64_t objectFromEntries(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    RootedArgs args(argc, argv);
    Rooted<Value> out{Value(bronze_create_object())};
    Rooted<Value> rec{Value(bronze_iter_open(args[0].rawBits()))};
    if (rtExceptionPending()) return out.get().rawBits();
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> pair{Value(bronze_iter_value(rec.get().rawBits()))};
        if (!pair.get().isObject()) {
            rtThrowTypeError("Iterator value is not an entry object");
            break;
        }
        Rooted<Value> k{Value(bronze_elem_get(pair.get().rawBits(),
                                              Value::fromDouble(0.0).rawBits()))};
        Rooted<Value> v{Value(bronze_elem_get(pair.get().rawBits(),
                                              Value::fromDouble(1.0).rawBits()))};
        bronze_elem_set(out.get().rawBits(), k.get().rawBits(), v.get().rawBits(), /*strict=*/false);
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
    return out.get().rawBits();
}

uint64_t objectConstructorBody(uint64_t, uint64_t, uint32_t argc, const uint64_t* argv) {
    if (argc == 0) return bronze_create_object();
    Rooted<Value> value{Value(argv[0])};
    if (value.get().isUndefined() || value.get().isNull()) return bronze_create_object();
    if (value.get().isObject()) return value.get().rawBits();
    if (value.get().isString()) return rtMakeStringWrapper(value).rawBits();
    if (value.get().isBool()) return rtMakeBooleanWrapper(value.get().asBool()).rawBits();
    if (value.get().isNumber()) return rtMakeNumberWrapper(value.get().asNumber()).rawBits();
    fatal(value.get().isSymbol()
              ? "unsupported: Object(symbol) (20.4.3 wraps it in an object with a "
                "[[SymbolData]] internal slot, and bronze builds none — 20.4.3 makes "
                "Symbol.prototype an ordinary object with no slot for one to carry)"
              : "unsupported: Object(bigint) (21.2.3 wraps it in an object with a "
                "[[BigIntData]] internal slot, and bronze builds none — 21.2.3 makes "
                "BigInt.prototype an ordinary object with no slot for one to carry)");
}

}  // namespace bronze::runtime
