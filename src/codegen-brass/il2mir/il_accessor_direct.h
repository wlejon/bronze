#pragma once

// Accessor sites (il.h `directTarget` on PropGet/PropSet): a named read or
// write whose receiver's class declares an accessor for its key calls that
// accessor's getter or setter directly behind the site's ACCESSOR CELL, in
// front of the cache's sequence rather than instead of it
// (runtime/accessor_cell.cpp states what the cell stands for).

#include "il_ast.h"
#include <brass/mir/builder.hpp>
#include <functional>

namespace il2mir {

class IlLowering;

// Whether this site takes the direct form: it names an accessor, the module
// enters callees directly (an image build), and the block runs more than
// once (code that runs once calls the helper, as every other site there does).
bool accessor_direct_here(IlLowering* lowering, const BronzeInstruction& inst);

// THE ACCESSOR GUARD: whether `recv` is a plain object whose shape the cell
// pair at `cell` names at the thread's current prototype-mutation epoch. One
// i1; `accessor.ic.is` is exactly this. `keyed`, when given, is `recv`'s
// keyed shape from the shape memo (il_shape_memo.cpp), which the guard then
// compares instead of reading the receiver's header and shape again.
Value* accessor_cell_hit(IlLowering* lowering, Builder& b, Value* recv, uint32_t cell, Value* keyed = nullptr);

// The fill of an inlined method body's cell pair after its guard's miss ran
// the generic call (runtime/accessor_cell.cpp, `bronze_method_cell_publish`):
// the publish, only while the cell speaks for another epoch. `obj` is the
// receiver's bits read after the call. Leaves the builder after it.
void emit_method_cell_fill(IlLowering* lowering, Builder& b, Value* obj, uint32_t cell, uint32_t key_index,
                           const std::string& callee);

// What a direct accessor site does on a miss: the ordinary read (`get`) or
// write (`set`), and the receiver's bits read afresh after it, for the fill.
struct AccessorMiss {
    std::function<Value*(Builder&)> get;
    std::function<void(Builder&)> set;
    std::function<Value*(Builder&)> receiver;
};

// `obj.key` with the getter entered directly on a hit; the miss is the
// ordinary read, then the cell's fill.
Value* lower_accessor_get(IlLowering* lowering, const BronzeInstruction& inst, Builder& b, Value* obj,
                          const AccessorMiss& miss);
// `obj.key = val` with the setter entered directly on a hit, likewise.
void lower_accessor_set(IlLowering* lowering, const BronzeInstruction& inst, Builder& b, Value* obj, Value* val,
                        const AccessorMiss& miss);

}  // namespace il2mir
