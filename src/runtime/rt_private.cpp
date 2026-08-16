// Private class elements (ECMA-262 6.2.12): the six helpers generated code
// reaches for `#x`, and the storage behind them.
//
// A Private Name is not a property key and cannot be one. 6.2.12 gives every
// object a [[PrivateElements]] list that no property operation touches: no
// shape carries a private name, `Object.keys`, `for-in`, spread, JSON and
// `getOwnPropertyNames` cannot see one, and `delete` has no step that removes
// one. Spelling `#x` as a reserved property name would have bought the first
// two and none of the rest.
//
// So the storage is a TABLE PER NAME PER CLASS EVALUATION, keyed by the object
// that carries the element — builtin_map.cpp's object-keyed table under a kind
// of its own (MapHeader::kPrivateFlags). Reading `o.#x` is a lookup of `o` in
// `#x`'s table, and the brand check ECMA-262 states as "PrivateElementFind
// returned empty" is exactly a miss.
//
// Per EVALUATION is the part that decides the shape of this. A class
// expression evaluated twice mints two sets of Private Names, and an instance
// of the first must fail the second's brand check — so the table cannot hang
// off the source position, and it cannot hang off the constructor either
// (a subclass shares neither). It hangs off the environment record the class
// evaluation created, which already has one instance per evaluation, and
// lowering hands the table in as an ordinary operand.
//
// STATIC private elements need no second mechanism: the constructor is the
// object that carries them, so `static #s` is one entry in `#s`'s table keyed
// by the class itself. `#instanceField in SomeClass` is therefore false and
// `#s in SomeClass` true, with no rule anywhere saying so.

#include <string>

#include "abi/bronze_abi.h"
#include "runtime/exception.h"
#include "runtime/fatal.h"
#include "runtime/map.h"
#include "runtime/rt_roots.h"
#include "runtime/rt_state.h"
#include "runtime/value.h"

namespace bronze::runtime {
namespace {

bool isPrivateTable(Value v) {
    return v.isObject() && v.asObject<HeapObjectHeader>()->flags == MapHeader::kPrivateFlags;
}

// Every helper here is handed a table lowering produced, so a value that is
// not one is a compiler bug and not something a program did — the same
// judgment `resolveEnv` makes about an environment record.
MapHeader* requireTable(Value v, const char* what) {
    if (!isPrivateTable(v)) {
        fatal((std::string("internal: ") + what +
               " on a value that is not a private-element table")
                  .c_str());
    }
    return v.asObject<MapHeader>();
}

// 6.2.12.1 PrivateElementFind's answer, as the brand check every access makes
// first. A primitive receiver can never carry an element — nothing installs
// one on a Number — so it misses without touching the table.
bool tableHas(Rooted<Value>& table, Rooted<Value>& obj) {
    if (!obj.get().isObject()) return false;
    return MapHeader::find(rtHeap(), table, obj) != UINT32_MAX;
}

// The one message for the one condition, so a program that catches it and
// prints `e.message` sees the same words wherever the miss happened.
uint64_t brandError(uint32_t nameKeyIndex) {
    return rtThrowTypeError("Cannot read private member " + rtKeyString(nameKeyIndex) +
                            " from an object whose class did not declare it")
        .rawBits();
}

}  // namespace
}  // namespace bronze::runtime

using namespace bronze;
using namespace bronze::runtime;

// One private name's table, minted where the class is EVALUATED. Two
// evaluations of one class expression call this twice and get two tables,
// which is the whole of private-name identity.
uint64_t bronze_private_new(void) {
    return Value::fromObject(MapHeader::create(rtHeap(), MapHeader::kPrivateFlags)).rawBits();
}

// `#x in o` (13.10.1). An OBJECT that lacks the element answers false — that
// is what the operator is for, and it is how a program asks a brand question
// without a `try`. A non-object is step 6's TypeError instead: there is no
// object to have been branded, and answering false would say that a string
// merely lacks the element.
bool bronze_private_has(uint64_t tableBits, uint64_t objBits, uint32_t nameKeyIndex) {
    Rooted<Value> table{Value(tableBits)};
    requireTable(table.get(), "private.has");
    Rooted<Value> obj{Value(objBits)};
    if (!obj.get().isObject()) {
        rtThrowTypeError("Cannot use 'in' operator to search for " +
                         rtKeyString(nameKeyIndex) + " in a value that is not an object");
        return false;
    }
    return tableHas(table, obj);
}

// 6.2.12.2 PrivateGet, minus the accessor dispatch: whether the name is a
// field, a method or an accessor pair is a compile-time fact, so lowering
// resolves it and this helper answers only the storage question.
uint64_t bronze_private_get(uint64_t tableBits, uint64_t objBits, uint32_t nameKeyIndex) {
    Rooted<Value> table{Value(tableBits)};
    requireTable(table.get(), "private.get");
    Rooted<Value> obj{Value(objBits)};
    if (!obj.get().isObject()) return brandError(nameKeyIndex);
    const uint32_t slot = MapHeader::find(rtHeap(), table, obj);
    if (slot == UINT32_MAX) return brandError(nameKeyIndex);
    return table.get().asObject<MapHeader>()->valueAt(slot).rawBits();
}

// 6.2.12.4 PrivateFieldAdd / PrivateMethodOrAccessorAdd. The only one with no
// brand check, because it is what ESTABLISHES the brand: the constructor runs
// it once per private element while initializing the instance, and the class
// evaluation runs it for the static ones on the constructor itself.
//
// A duplicate add cannot happen — 6.2.12.4's "already present" step is an
// error the early rules make unreachable — so the update MapHeader::set would
// perform for a repeated key is not a path a program can reach.
void bronze_private_add(uint64_t tableBits, uint64_t objBits, uint64_t valueBits) {
    Rooted<Value> table{Value(tableBits)};
    requireTable(table.get(), "private.add");
    Rooted<Value> obj{Value(objBits)};
    if (!obj.get().isObject()) {
        fatal("internal: private.add on a receiver that is not an object");
    }
    Rooted<Value> val{Value(valueBits)};
    MapHeader::set(rtHeap(), table, obj, val);
}

// 6.2.12.3 PrivateSet for a field: the element must already be there. A write
// to an object that never got one is the same TypeError a read gives, and
// deliberately the same words — the condition is one condition.
void bronze_private_set(uint64_t tableBits, uint64_t objBits, uint64_t valueBits,
                        uint32_t nameKeyIndex) {
    Rooted<Value> table{Value(tableBits)};
    requireTable(table.get(), "private.set");
    Rooted<Value> obj{Value(objBits)};
    if (!tableHas(table, obj)) {
        brandError(nameKeyIndex);
        return;
    }
    Rooted<Value> val{Value(valueBits)};
    MapHeader::set(rtHeap(), table, obj, val);
}

// The three ways a BRANDED private access is still a TypeError (6.2.12.2 step
// 3.b, 6.2.12.3 steps 2 and 4). Which one is decided where the code was
// compiled — a private name's kind is fixed by its declaration — so the code
// travels as an immediate and the runtime only has to say it.
uint64_t bronze_private_misuse(uint32_t nameKeyIndex, uint32_t code) {
    const std::string name = rtKeyString(nameKeyIndex);
    switch (code) {
        case 0:
            return rtThrowTypeError("Cannot write to private method " + name).rawBits();
        case 1:
            return rtThrowTypeError("'" + name + "' was defined without a getter").rawBits();
        case 2:
            return rtThrowTypeError("'" + name + "' was defined without a setter").rawBits();
        default:
            fatal("internal: private.misuse with an unknown code");
    }
}
