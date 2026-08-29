// What an instruction DOES, as opposed to what it is called.
//
// Three predicates the backend reads before it emits anything: whether an
// instruction ends a block, whether it can leave an exception pending, and
// whether it can move a heap object. None of them is a question about text, and
// they sat in `print.cpp` only because `il.h` declares them next to the
// printer's names. They are the exception test after a call and the point a
// receiver proof has to give up its derived pointer, which is a different
// subject from the spelling of an op.
//
// Both `canThrow` and `canCollect` are written the SAFE WAY ROUND: the
// enumerated cases are the ones that provably cannot, and everything else can.
// An op added tomorrow gets a redundant branch rather than a missed unwind or a
// dangling pointer.

#include "il/print.h"

namespace bronze::il {

bool isTerminator(Op op) {
    return op == Op::Ret || op == Op::Jump || op == Op::Branch || op == Op::Throw;
}

// Can this instruction leave an exception pending? The backend emits one cell
// test after every instruction that answers yes, so the list is written the
// safe way round: the cases below are the ones that provably cannot, and
// everything else does. An op added tomorrow gets a redundant branch rather
// than a missed unwind.
//
// It is a property of the INSTRUCTION and not of the op because `add` is two
// operations: f64 arithmetic, which cannot throw, and `bronze_dynamic_add`,
// which reaches ToPrimitive.
bool canThrow(const Instruction& inst) {
    switch (inst.op) {
        // Constants and pure arithmetic: no call at all.
        case Op::ConstF64:
        case Op::ConstI32:
        case Op::ConstBool:
        case Op::ConstUndefined:
        case Op::ConstNull:
        // The literal is a parse and an allocation, and a failure to allocate
        // is fatal rather than catchable.
        case Op::ConstBigInt:
        case Op::CmpLt:
        case Op::CmpGt:
        // The ordered compares are machine instructions on two numbers, like
        // the two above them. Their boxed siblings — rel.lt and friends — are
        // NOT here: those reach ToPrimitive.
        case Op::CmpLe:
        case Op::CmpGe:
        case Op::CmpEq:
        case Op::CmpNe:
        case Op::NumTruthy:
        case Op::StrictEq:
        case Op::IsNullish:
        // One unsigned compare against a constant. It is the guarded-region
        // pass's whole barrier, and a barrier that could raise would need the
        // slow copy to be reachable from two places rather than one.
        case Op::IsNumber:
        case Op::TypeOf:
        case Op::Box:
        // The pin census is an INSTRUMENT (src/runtime/pin_census.h): it
        // records a tag and returns. An instrument that could change control
        // flow would be one whose readings are about itself, so it never
        // raises and the backend emits no exception test after it.
        case Op::CensusRecord:
        // Allocation and the environment: they can collect, and a failure to
        // allocate is fatal rather than catchable (there is no `RangeError:
        // out of memory` in bronze, and inventing one would let a program
        // continue past a heap that could not grow).
        case Op::CreateObject:
        case Op::CreateGeneratorObject:
        // The machine is allocation only; starting it and subscribing an
        // await are NOT here — the first runs the body and the second reads
        // `.then` off whatever was awaited, and either can reach user code.
        case Op::CreateAsyncMachine:
        case Op::ModuleNamespace:
        case Op::CreateArray:
        case Op::CreateFunction:
        case Op::FunctionRef:
        case Op::EnvCreate:
        case Op::EnvGet:
        case Op::EnvSet:
        case Op::EnvInitTdz:
        case Op::ModuleEnvSet:
        case Op::ModuleEnvGet:
        // Minting a private name's table is an allocation and nothing else.
        // The other five private ops CAN throw and are not here — `#x in o`
        // included, because 13.10.1 step 6 refuses a non-object right operand
        // before it ever asks about the element.
        case Op::PrivateNew:
        case Op::GlobalGet:
        // console.log never runs user code: inspect does not call a getter.
        case Op::Print:
        case Op::PrintErr:
        case Op::PrintSpread:
        case Op::PrintSpreadErr:
        case Op::ExcTake:
        // Raw bounds-checked loads and stores on a proven view: an invalid
        // index is NaN or a skipped store, never a throw, and neither op
        // allocates or reaches user code.
        case Op::ElemGetTyped:
        case Op::ElemSetTyped:
        // A machine-number math kernel: no coercion ladder, no user code.
        case Op::MathUnary:
        // One aligned load from the module's own template-slot table, at an
        // address the compiler already proved in range.
        case Op::TemplateCached:
            return false;
        case Op::Add:
        // The rest of the arithmetic and bitwise family, for the same reason
        // `add` is here: each is TWO operations. On machine numbers it is one
        // instruction that cannot fail; on BOXED operands it is 13.15.3, which
        // reaches ToPrimitive, refuses a mixed BigInt/Number pair by TypeError
        // and divides by 0n by RangeError.
        case Op::Neg:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::Pow:
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr:
        case Op::UShr:
            return inst.type == Type::Dynamic;
        // `bitnot` exists ONLY in its boxed form, so it has no cannot-throw
        // half to test for.
        case Op::BitNot:
            return true;
        // A `--pins` write barrier RAISES, but the branching forms raise and
        // LEAVE: the violating arm calls bronze_pin_violation and branches to
        // the block's handler itself, so there is no returning path for a cell
        // test to catch and no reason to emit one (llvm_pin.h). The
        // `numeric-elements` FIELD form is a plain call that returns on both
        // outcomes, and it keeps the ordinary check.
        case Op::PinGuard:
            return static_cast<PinBarrier>(inst.immI32) == PinBarrier::DenseArray;
        case Op::Unbox:
            // `unbox.f64` is ToNumber (7.1.4) — generated code's only numeric
            // coercion — and ToNumber runs ToPrimitive on an object and throws
            // for a Symbol. `unbox.bool` is ToBoolean, which is total, and
            // `unbox.i32` reads a number it has already tested for; neither
            // calls anything. The RAW form calls nothing either: its operand is
            // proven a Number, so ToNumber is the identity and the emitted code
            // is one bitcast.
            // The NULLISH-WIDENED form calls nothing either: its operand is one
            // of a number, `null` or `undefined`, and ToNumber over those three
            // is a compare and two constants.
            return inst.type == Type::F64 && !inst.rawUnbox && !inst.nullishUnbox;
        case Op::ToInt32:
            // Same conversion under a different name: 7.1.6 step 1 is ToNumber,
            // so `{} | 0` and `sym | 0` reach user code and the TypeError. Only
            // a BOXED operand does — `boxType` carries the operand's type here,
            // and an f64 or i32 one is a machine conversion with no call in it,
            // which is what keeps a proven-numeric bitwise chain branch-free.
            return inst.boxType == Type::Dynamic;
        default:
            return !isTerminator(inst.op);
    }
}

bool canCollect(const Instruction& inst) {
    switch (inst.op) {
        // Machine constants: a bit pattern, materialised inline. `const.bigint`
        // is NOT here — it parses and allocates.
        case Op::ConstF64:
        case Op::ConstI32:
        case Op::ConstBool:
        case Op::ConstUndefined:
        case Op::ConstNull:
        // The number compares and the two total predicates: register work on
        // operands already in hand. Their boxed siblings (`rel.lt` and friends,
        // `loose.eq`) reach ToPrimitive and are absent for that reason.
        case Op::CmpLt:
        case Op::CmpGt:
        case Op::CmpLe:
        case Op::CmpGe:
        case Op::CmpEq:
        case Op::CmpNe:
        case Op::NumTruthy:
        case Op::StrictEq:
        case Op::IsNullish:
        // Reads bits already in a register and compares them against a
        // constant: no allocation, no user code, nothing for a receiver proof
        // to have to give up its derived pointer for.
        case Op::IsNumber:
        // Reads a tag and names it from a table of immortal strings.
        case Op::TypeOf:
        // One aligned load from the module's own template-slot table.
        case Op::TemplateCached:
        // Bounds-checked load and store on a view the compiler proved: the
        // whole point of the pair is that they call nothing, and `canThrow`
        // says the same about them for the same reason.
        case Op::ElemGetTyped:
        case Op::ElemSetTyped:
        // A machine-number kernel. `Math.sin` reaches libm, which allocates
        // nothing and cannot see a JS heap to move.
        case Op::MathUnary:
        // The two control transfers that stay inside the function. A `jump`
        // writes its arguments into the target's block parameters and a `br`
        // picks between two of them; neither allocates and neither can reach
        // user code, so a receiver proof made before one is still a valid
        // derived pointer after it. This matters because a run may span a
        // straight-line CHAIN of blocks (llvm_recv_proof.h), and the terminator
        // is the instruction between one member and the next — before chains,
        // every run ended at its block anyway and this answer cost nothing.
        //
        // `ret` and `throw` are NOT here. They leave the function, so there is
        // nothing after them for a proof to be good for, and the conservative
        // answer is free.
        case Op::Jump:
        case Op::Branch:
            return false;
        // Boxing a number, a bool or an int is a bitcast and a select
        // (llvm_ops.cpp). A STRING box is not boxing at all — it mints the
        // key's Value through bronze_box_str_key — so it stays a barrier.
        case Op::Box:
            return inst.boxType == Type::Str;
        // Two operations under one name, exactly as `canThrow` reads them: on
        // machine numbers one instruction, on boxed operands 13.15.3 and the
        // whole ToPrimitive ladder behind it. The type is what says which.
        case Op::Add:
        case Op::Neg:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::Pow:
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr:
        case Op::UShr:
            return inst.type == Type::Dynamic;
        // ToNumber over a proven Number is the identity and emits a bitcast;
        // over anything else it is 7.1.4 and calls. Same split `canThrow`
        // makes, and for the same operand.
        case Op::Unbox:
            return inst.type == Type::F64 && !inst.rawUnbox && !inst.nullishUnbox;
        case Op::ToInt32:
            return inst.boxType == Type::Dynamic;
        // The pin barrier's branching forms are one unsigned compare against a
        // constant on the path that continues; the TypeError they mint is on
        // the arm that LEAVES for the handler and never comes back, so nothing
        // a proof holds is stale after one. That is what lets a run of element
        // reads span the barrier a `numeric-elements` manifest puts between
        // every store and the next read. The FIELD form is a call that returns.
        case Op::PinGuard:
            return static_cast<PinBarrier>(inst.immI32) == PinBarrier::DenseArray;
        // Everything else — every call, every property access, every
        // allocation, and anything added after this was written.
        default:
            return true;
    }
}

}  // namespace bronze::il
