// What an instruction DOES, as opposed to what it is called.
//
// The effect table (effects.h) and the predicates the backend reads before it
// emits anything: whether an instruction ends a block, whether it can throw,
// and whether it can move a heap object. `canThrow` and `canCollect` are thin
// readers of the table now; they keep the answers they always gave for every
// op that existed before it, so the backend's exception tests and receiver
// proofs are unchanged.
//
// Written the SAFE WAY ROUND: the enumerated cases are the ones known to do
// less, and everything else does everything. An op added tomorrow is a throw
// point, a collection point and a write of the whole heap rather than a missed
// unwind, a dangling pointer or a stale load.

#include "il/effects.h"


namespace bronze::il {

bool isTerminator(Op op) {
    return op == Op::Ret || op == Op::Jump || op == Op::Branch || op == Op::Throw ||
           op == Op::Leave;
}

Effects everythingEffects() {
    Effects e;
    e.reads = LocSet::all();
    e.writes = LocSet::all();
    e.throws = true;
    e.collects = true;
    e.callsUnknown = true;
    e.allocates = true;
    // User code can print, so its order against `print` is observable.
    e.observableOrder = true;
    return e;
}

namespace {

Effects none() { return Effects{}; }

// A read of `r` and nothing else.
Effects reading(LocSet r) {
    Effects e;
    e.reads = r;
    return e;
}

// A fresh heap object: nothing existing is read or written, and nothing can
// have seen it before, so an unused one is dead.
Effects allocation() {
    Effects e;
    e.allocates = true;
    e.collects = true;
    return e;
}

// Ops the old `canCollect` answered conservatively: they reach a runtime path
// the receiver proof never vouched for, so they keep counting as collection
// points even where the honest answer is likely no. Tightening one is a
// separate, measured change.
Effects heapOp(LocSet r, LocSet w) {
    Effects e;
    e.reads = r;
    e.writes = w;
    e.collects = true;
    return e;
}

// The speculation forms (il_op.h): one machine step under a precondition a
// dominating branch established.
Effects guardedOp(LocSet r, LocSet w) {
    Effects e;
    e.reads = r;
    e.writes = w;
    e.guarded = true;
    return e;
}

// Throws (a TypeError or ReferenceError it allocates) and touches `r`/`w`.
Effects throwing(LocSet r, LocSet w) {
    Effects e;
    e.reads = r;
    e.writes = w;
    e.throws = true;
    e.collects = true;
    return e;
}

// Before the derived flags: the effects proper.
Effects rawEffects(const Instruction& inst) {
    switch (inst.op) {
        // Machine constants: a bit pattern, materialised inline.
        case Op::ConstF64:
        case Op::ConstI32:
        case Op::ConstBool:
        case Op::ConstUndefined:
        case Op::ConstNull:
            return none();
        // Parses and allocates; a failure to allocate is fatal, not a throw.
        case Op::ConstBigInt:
            return allocation();

        // Two operations under one name each: on machine numbers one
        // instruction, on boxed operands 13.15.3 and the ToPrimitive ladder
        // behind it (user code, a mixing TypeError, a RangeError for 0n). The
        // result type says which.
        // An i32 `/` or `%` traps on a zero divisor (and INT32_MIN by -1):
        // speculated code puts one behind guards on its operands, and it may
        // not move above them.
        case Op::Div:
        case Op::Mod:
            if (inst.type == Type::I32) return guardedOp(LocSet::none(), LocSet::none());
            return inst.type == Type::Dynamic ? everythingEffects() : none();
        case Op::Add:
        case Op::Neg:
        case Op::Sub:
        case Op::Mul:
        case Op::Pow:
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr:
        case Op::UShr:
            return inst.type == Type::Dynamic ? everythingEffects() : none();
        // 7.1.6 step 1 is ToNumber: a boxed operand reaches user code.
        case Op::ToInt32:
            return inst.boxType == Type::Dynamic ? everythingEffects() : none();

        // Register compares and total predicates on operands in hand. `===` on
        // two strings reads their text, which is immutable.
        case Op::CmpLt:
        case Op::CmpGt:
        case Op::CmpLe:
        case Op::CmpGe:
        case Op::CmpEq:
        case Op::CmpNe:
        case Op::NumTruthy:
        case Op::StrictEq:
        case Op::IsNullish:
        case Op::IsNumber:
            return none();
        // The answer is one of eleven immortal strings, but the table is built
        // lazily by the first `typeof` the process runs, which interns.
        case Op::TypeOf: {
            Effects e = allocation();
            return e;
        }
        // A tag test, a kind byte, a length compare and whether the array has
        // an element block (its pointer, under the index-hazard mask). The
        // block's pointer and capacity move only with the length (an append
        // that grows writes `ArrayLen`) or under code that writes everything;
        // the block's CONTENTS (`Elements`, what `elem.store` writes) are not
        // read.
        // The NUMBERS form also reads the bits word an element store clears.
        case Op::IsDenseArray:
            return (inst.immI32 & kDenseNumbers) != 0 ? reading(Loc::Shapes | Loc::ArrayLen | Loc::Elements)
                                                      : reading(Loc::Shapes | Loc::ArrayLen);
        // The array's shape word (bare or not) is what the
        // test reads; a write that changes it is a transition.
        case Op::IsPlainArray:
            return reading(Loc::Shapes);
        // The kind is fixed when the view is made. With the pristine-length
        // bit it also reads the instance shape and the prototype epoch.
        case Op::IsTypedArray:
            return (inst.immI32 & kTypedArrayPristineLength) != 0
                       ? reading(Loc::Shapes | Loc::Heap)
                       : none();

        // Boxing a number, bool or int is a bitcast. A STRING box mints the
        // key's Value (bronze_box_str_key), which allocates.
        case Op::Box:
            return inst.boxType == Type::Str ? allocation() : none();
        // `unbox.f64` is ToNumber (7.1.4), which runs ToPrimitive. `unbox.bool`
        // is ToBoolean, which is total; `unbox.i32` reads a number already
        // tested for.
        case Op::Unbox:
            return inst.type == Type::F64 ? everythingEffects() : none();

        // Control. A jump or branch does nothing but pick an edge. Leaving the
        // function hands the whole heap to whoever is next, so `ret`, `throw`
        // and `leave` read everything: a store before one is never dead. They
        // count as collection points because there is nothing after them for
        // a receiver proof to be good for, so the conservative answer is free.
        case Op::Jump:
        case Op::Branch:
            return none();
        case Op::Ret: {
            Effects e = reading(LocSet::all());
            e.collects = true;
            return e;
        }
        case Op::Throw: {
            Effects e = reading(LocSet::all());
            e.throws = true;
            e.collects = true;
            return e;
        }
        case Op::Leave: {
            Effects e = reading(LocSet::all());
            e.collects = true;
            e.observableOrder = true;
            return e;
        }
        // An entry into G: resumption arrives here with the heap as S left it
        // and only the state values in hand. It reads everything (a store in
        // front of it is seen by the code after it) and nothing may cross it.
        case Op::ResumePoint: {
            Effects e = reading(LocSet::all());
            e.observableOrder = true;
            e.barrier = true;
            return e;
        }

        // The pending exception: read and cleared.
        case Op::ExcTake:
            return heapOp(Loc::Heap, Loc::Heap);

        // Allocation. A failure to allocate is fatal rather than catchable.
        case Op::CreateObject:
        case Op::CreateArray:
        case Op::ObjectLiteral:
        case Op::ArrayLiteral:
        case Op::CreateFunction:
        case Op::EnvCreate:
        case Op::PrivateNew:
        // Mints or loads the function object of a module function.
        case Op::FunctionRef:
            return allocation();
        // Built from the getters object's own properties, read without calls.
        case Op::ModuleNamespace: {
            Effects e = allocation();
            e.reads = Loc::Slots | Loc::Shapes | Loc::Heap;
            return e;
        }

        // The frame header's resume word.
        case Op::CoroMode:
            return heapOp(Loc::Heap, LocSet::none());
        // Reads NewTargetScope::current(); refuses outside a constructor scope.
        case Op::GetNewTarget:
            return throwing(Loc::Heap, LocSet::none());
        // One aligned load from the module's template-slot table.
        case Op::TemplateCached:
            return reading(Loc::Heap);

        // The environment. env.get.tdz raises 9.1.1.1.6's ReferenceError.
        case Op::EnvGet:
            return heapOp(Loc::EnvSlots, LocSet::none());
        case Op::EnvSet:
        case Op::EnvInitTdz:
            return heapOp(LocSet::none(), Loc::EnvSlots);
        case Op::EnvGetTdz:
            return throwing(Loc::EnvSlots, LocSet::none());
        case Op::ModuleEnvGet:
            return heapOp(Loc::ModuleEnv, LocSet::none());
        case Op::ModuleEnvSet:
            return heapOp(LocSet::none(), Loc::ModuleEnv);
        // A global binding read; a miss is `undefined`, never a getter call.
        case Op::GlobalGet:
            return heapOp(Loc::Globals, LocSet::none());

        // The private-element tables (6.2.12): every access but the mint
        // brand-checks and raises a TypeError on a receiver without the
        // element, and `#x in o` refuses a non-object.
        case Op::PrivateHas:
        case Op::PrivateGet:
            return throwing(Loc::PrivateFields, LocSet::none());
        case Op::PrivateAdd:
        case Op::PrivateSet:
            return throwing(Loc::PrivateFields, Loc::PrivateFields);
        case Op::PrivateMisuse:
            return throwing(LocSet::none(), LocSet::none());

        // A `--pins` write barrier raises. The branching forms leave for the
        // handler on their own arm (canThrow below keeps the backend's answer
        // for that); the field form tests the slot's array.
        case Op::PinGuard: {
            const bool field = static_cast<PinBarrier>(inst.immI32) == PinBarrier::DenseArray;
            Effects e;
            e.throws = true;
            e.collects = field;
            if (field) e.reads = Loc::Shapes | Loc::ArrayLen | Loc::Elements;
            return e;
        }

        // console.log never runs user code (inspect calls no getter), but it
        // reads whatever it formats, and its order is the program's output.
        case Op::Print:
        case Op::PrintErr:
        case Op::PrintSpread:
        case Op::PrintSpreadErr: {
            Effects e = reading(LocSet::all());
            e.collects = true;
            e.observableOrder = true;
            return e;
        }
        // A use of its operands up to here, after the native call it guards;
        // kept in order with that call.
        case Op::KeepAlive: {
            Effects e;
            e.observableOrder = true;
            return e;
        }
        // The type-feedback instrument: records a tag from immortal memory and
        // returns. Its order is what the profile reads.
        case Op::FeedbackRecord: {
            Effects e;
            e.observableOrder = true;
            return e;
        }

        // ---- the speculation forms (il_op.h) ----------------------------
        case Op::ShapeCell:   // filled once at load, never written after
        case Op::PtrEq:
        case Op::ShapeIn:
        case Op::IsObject:
        case Op::IsI32:
        case Op::UnboxI32:    // garbage, not a fault, off its guard
        case Op::UnboxF64:
        case Op::I32ToF64:
        case Op::AddWouldOvf:
        case Op::SubWouldOvf:
        case Op::MulWouldOvf:
        case Op::FnCodeIs:    // a function's code never changes
        case Op::InBounds:
        case Op::IsHole:
        case Op::IsUninit:
        case Op::IsArray:     // an object's kind never changes
        case Op::FnNativeIs:
        case Op::MathFn:
            return none();
        case Op::TypedLength:
            return guardedOp(Loc::Views, LocSet::none());
        case Op::TypedLoad:
            return guardedOp(Loc::Views | Loc::TypedElements, LocSet::none());
        case Op::TypedStore:
            return guardedOp(Loc::Views, Loc::TypedElements);
        case Op::LoadShape:
            return guardedOp(Loc::Shapes, LocSet::none());
        // A shape's layout is fixed; whether it is a prototype's (the write
        // form's last condition) changes with the program's Shapes.
        case Op::ShapeHasOwn:
            return (inst.immI32 & 1) != 0 ? guardedOp(Loc::Shapes, LocSet::none()) : none();
        case Op::SlotLoad:
            return guardedOp(Loc::Slots, LocSet::none());
        case Op::SlotStore:
            return guardedOp(LocSet::none(), Loc::Slots);
        // The captured record is fixed when the closure is made.
        case Op::FnEnv:
            return guardedOp(LocSet::none(), LocSet::none());
        case Op::ArrayLength:
            return guardedOp(Loc::ArrayLen, LocSet::none());
        case Op::ElemLoad:
            return guardedOp(Loc::Elements, LocSet::none());
        case Op::ElemStore:
            return guardedOp(LocSet::none(), Loc::Elements);
        // Construction: the vetted byte and the instance shape live on the
        // function object; the instance is a fresh allocation.
        case Op::FnConstructs:
        case Op::FnInstanceShape:
            return guardedOp(Loc::FnFields, LocSet::none());
        // The new object's shape word is written as it is made, which is what
        // lets a later `load.shape` of it be the operand.
        case Op::CreateThis: {
            Effects e = allocation();
            e.writes = Loc::Shapes;
            return e;
        }
        case Op::ShapeStore:
            return guardedOp(LocSet::none(), Loc::Shapes);
        case Op::ConstructResult:
            return Effects{};
        // A module cell the runtime empties at every epoch bump.
        case Op::ProtoCell:
            return reading(Loc::Protos);
        case Op::ProtoPublish: {
            Effects e;
            e.reads = Loc::Protos;
            e.writes = Loc::Protos;
            return e;
        }
        case Op::MemberCell:
            return reading(Loc::Protos);
        // Reads the receiver's shape and its chain; fills a cell.
        case Op::MemberPublish: {
            Effects e;
            e.reads = Loc::Protos | Loc::Shapes | Loc::Slots;
            e.writes = Loc::Protos;
            return e;
        }
        // Runs the rest of a generic function: any user code at all. Listed
        // rather than left to the default so that nobody tightens it by
        // accident.
        case Op::ResumeCall:
            return everythingEffects();

        // Every call, every generic property, element and iterator operation,
        // every coercion that reaches ToPrimitive, every definition that can
        // meet a proxy or a setter, and anything added after this was written.
        default:
            return everythingEffects();
    }
}

}  // namespace

Effects effectsOf(const Instruction& inst) {
    Effects e = rawEffects(inst);
    if (e.callsUnknown) e = everythingEffects();
    e.pure = e.reads.empty() && e.writes.empty() && !e.throws && !e.collects && !e.allocates &&
             !e.observableOrder && !e.guarded && !e.barrier;
    e.removableIfUnused = e.writes.empty() && !e.throws && !e.callsUnknown && !e.observableOrder &&
                          !e.barrier && !isTerminator(inst.op);
    return e;
}

// Whether a NON-TERMINATOR can throw, so that the backend emits an exception
// test after it. A terminator's way out is its own (a `throw` branches to the
// handler itself), so the answer for one is no. The pin barrier's branching
// forms raise and LEAVE on their own arm (llvm_pin.h): there is no returning
// path for a test to catch, so they answer no as well, although the effect
// table — which a CFG reads for its handler edges — says they throw.
bool canThrow(const Instruction& inst) {
    if (isTerminator(inst.op)) return false;
    if (inst.op == Op::PinGuard) {
        return static_cast<PinBarrier>(inst.immI32) == PinBarrier::DenseArray;
    }
    return effectsOf(inst).throws;
}

bool canCollect(const Instruction& inst) { return effectsOf(inst).collects; }

// Written the safe way round too: the ops that read the scope are named, and a
// function the lowering marks a generator, an async function or a coroutine
// body is refused whatever its instructions are.
bool functionEntersDirectly(const Function& fn) {
    if (fn.isExternal || fn.isEntryPoint || fn.isGenerator || fn.isCoroutineBody()) return false;
    if (fn.fnFlags & (BRONZE_ABI_FN_FLAG_GENERATOR | BRONZE_ABI_FN_FLAG_ASYNC | BRONZE_ABI_FN_FLAG_NATIVE)) {
        return false;
    }
    for (const Block& block : fn.blocks) {
        for (const Instruction& inst : block.instructions) {
            switch (inst.op) {
                // The three that read NewTargetScope::current() (rt_object.cpp):
                // new.target itself, and `super()`, whose proxy-base path
                // constructs with it.
                case Op::GetNewTarget:
                case Op::SuperCall:
                case Op::SuperCallSpread:
                    return false;
                default:
                    break;
            }
        }
    }
    return true;
}

}  // namespace bronze::il
