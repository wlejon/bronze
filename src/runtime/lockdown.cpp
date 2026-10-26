#include "runtime/lockdown.h"

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/array.h"
#include "runtime/builtin_object.h"
#include "runtime/fatal.h"
#include "runtime/gc.h"
#include "runtime/integrity.h"
#include "runtime/iterator.h"
#include "runtime/object.h"
#include "runtime/realm.h"
#include "runtime/rt_builtins.h"
#include "runtime/rt_convert.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::runtime {

namespace {

thread_local bool g_lockedDown = false;

// Queue `v` for the walk unless it is not an object or has been hardened
// already. An object can be queued twice before its turn — two parents reach it
// — and the second pop skips it; marking at the pop rather than at the push is
// what lets the walk read an object's members BEFORE it is frozen, which the
// intrinsics that materialize members on first read depend on.
void enqueue(Rooted<Value>& work, Value v) {
    if (!v.isObject() || rtIsHardened(v)) return;
    Rooted<Value> val{v};
    const uint32_t at = work.get().asObject<ArrayHeader>()->length;
    work.get().asObject<ArrayHeader>()->setElem(rtHeap(), at, val);
}

// Every value an own property of `obj` holds: a data property's value, an
// accessor's getter and setter. Through the descriptor a program would read, so
// a member answered from a table is produced the way a program sees it.
void enqueueMembers(Rooted<Value>& work, Rooted<Value>& obj, Rooted<Value>& keys,
                    Rooted<Value>* const (&fields)[3]) {
    const uint32_t count = keys.get().asObject<ArrayHeader>()->length;
    for (uint32_t i = 0; i < count; ++i) {
        Rooted<Value> key{keys.get().asObject<ArrayHeader>()->getElem(i)};
        RootedBlock args(2);
        args.set(0, obj.get());
        args.set(1, key.get());
        Rooted<Value> desc{Value(rtObjectGetOwnPropertyDescriptor(0, 0, 2, args.data()))};
        if (!desc.get().isObject()) continue;
        for (Rooted<Value>* field : fields) {
            enqueue(work, desc.get().asObject<ObjectHeader>()->getProp(rtHeap(), *field));
        }
    }
}

}  // namespace

void rtLockdown() {
    if (g_lockedDown) return;
    ShadowStackFrame frame;
    Rooted<Value> work{Value(bronze_create_array(0))};

    for (const char* name : bronze::rtBuiltinGlobalNames()) {
        Value v = Value::fromUndefined();
        if (!rtResolveBuiltinGlobal(name, v)) {
            fatal((std::string("lockdown: the builtin ladder cannot resolve '") + name + "'")
                      .c_str());
        }
        enqueue(work, v);
    }
    // The intrinsics no global name leads to. %IteratorPrototype% and
    // %AsyncIteratorPrototype% are reached from these through [[Prototype]],
    // %GeneratorPrototype% through %GeneratorFunction.prototype%.prototype.
    for (IteratorProto kind :
         {IteratorProto::Map, IteratorProto::Set, IteratorProto::Array,
          IteratorProto::RegExpString, IteratorProto::Generator, IteratorProto::String,
          IteratorProto::AsyncGenerator, IteratorProto::Helper, IteratorProto::Wrap}) {
        enqueue(work, rtIteratorPrototype(kind));
    }
    enqueue(work, rtIteratorSharedPrototype());
    for (uint32_t i = 0; i < 3; ++i) enqueue(work, rtFunctionKindIntrinsic(i));

    Rooted<Value> valueKey{rtMakeString("value")};
    Rooted<Value> getKey{rtMakeString("get")};
    Rooted<Value> setKey{rtMakeString("set")};
    Rooted<Value>* const fields[3] = {&valueKey, &getKey, &setKey};

    while (work.get().asObject<ArrayHeader>()->length > 0) {
        const uint32_t last = work.get().asObject<ArrayHeader>()->length - 1;
        Rooted<Value> obj{work.get().asObject<ArrayHeader>()->getElem(last)};
        ArrayHeader::setLength(rtHeap(), work, last);
        if (rtIsHardened(obj.get())) continue;

        const uint64_t namesArg[1] = {obj.get().rawBits()};
        Rooted<Value> names{Value(rtObjectGetOwnPropertyNames(0, 0, 1, namesArg))};
        enqueueMembers(work, obj, names, fields);
        const uint64_t symbolsArg[1] = {obj.get().rawBits()};
        Rooted<Value> symbols{Value(objectGetOwnPropertySymbols(0, 0, 1, symbolsArg))};
        enqueueMembers(work, obj, symbols, fields);
        const uint64_t protoArg[1] = {obj.get().rawBits()};
        enqueue(work, Value(objectGetPrototypeOf(0, 0, 1, protoArg)));

        rtHarden(obj.get());
    }
    g_lockedDown = true;
}

bool rtLockedDown() { return g_lockedDown; }

}  // namespace bronze::runtime
