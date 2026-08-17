// Small shared helpers every other lowering unit leans on: the interned key
// table, block creation and instruction emission, the box/unbox coercions, and
// JS truthiness.

#include <string>
#include <vector>

#include "lower/lowerer.h"

namespace bronze::lower {

// The globals bronze provides, resolved by name because there is no global
// object to look them up in. The builtin list is closed: a free identifier
// that is not on it stays a compile error, so adding a builtin is a
// deliberate act here and never a program silently reading `undefined` at
// runtime. `hostGlobals_` is the one sanctioned opening in that fence — names
// a `--host-globals` manifest admitted, still a deliberate act, just the
// host's rather than this file's: the manifest is the host PROMISING the
// runtime registry will hold the value (host_globals.h), so the name resolves
// to the same `global.get` a builtin does.
bool Lowerer::isProvidedGlobal(const std::string& name) const {
    if (hostGlobals_.contains(name)) return true;
    // The Error constructors are globals for the same reason `Math` is: a free
    // identifier lowering can resolve, rather than a diagnosed unknown name.
    // They are also what the runtime raises its own spec'd TypeErrors with, so
    // a `catch` cannot tell a bronze-raised error from a hand-written one.
    // `Object` joined the list once it had more than one member: `Object.keys`
    // was recognized at the CALL, which is an answer that does not survive a
    // second member — `assign`, `defineProperty` and the rest would each need
    // their own IL op and arity check here. The `Object.keys` recognition stays
    // as a fast path over the same runtime function. `ReferenceError` is on the
    // list because bronze RAISES one: a program that catches what an
    // unresolvable name throws must be able to name its class, exactly as it
    // can for the TypeErrors the runtime raises.
    //
    // The four global function properties of 19.2 are here rather than
    // recognized at the call, for the reason `Math` was: `arr.map(parseFloat)`
    // is ordinary JS and a call-site recognition cannot express it. Each is
    // interned by code pointer, so `parseInt === Number.parseInt` — which is
    // what 21.1.2.13's "same function object" says.
    //
    // `ArrayBuffer` and the nine views joined when typed arrays landed. They
    // were reachable before only through a `new` that lowering recognised by
    // name, which is not the same thing as a value: `switch
    // (array.constructor)` and `{ Float32Array: Float32Array }` both need the
    // constructor OBJECT, and `new source.array.constructor(...)` needs it
    // reached through a member expression that no name recognition can see.
    // Putting them here deletes the special case rather than adding to it.
    //
    // `Array`, `String` and `Boolean` joined later, through exactly the
    // mechanism the line above describes — the list, an interned function
    // object, and no special case anywhere — and `WeakMap` / `WeakSet` joined
    // beside `Map` and `Set` the same way. `Function` joined as the global
    // constructor and namespace holding `Function.prototype` (dynamic compilation
    // from string source remains diagnosed by name at run time).
    // `Promise` joined with async/await: an async function RETURNS one, so a
    // program must be able to name the class it is handed — `Promise.all`,
    // `p instanceof Promise` — the same obligation the Error classes state.
    // `AggregateError` rides beside the Error constructors because
    // `Promise.any` raises one, and what bronze raises a program can catch by
    // name.
    return name == "Math" || name == "Object" || name == "Number" || name == "JSON" ||
           name == "Array" || name == "String" || name == "Boolean" ||
           name == "Symbol" || name == "BigInt" || name == "RegExp" || name == "Promise" ||
           name == "Map" || name == "Set" || name == "WeakMap" || name == "WeakSet" ||
           // `WeakRef` and `FinalizationRegistry` (26.1, 26.2). Both are
           // FEATURE-DETECTED far more often than they are used — a library
           // writes `typeof WeakRef === 'function' ? cacheWeakly() :
           // cacheStrongly()` — so the NAME has to resolve to a value even in a
           // program that never constructs one.
           name == "WeakRef" || name == "FinalizationRegistry" ||
           name == "Error" || name == "TypeError" || name == "AggregateError" ||
           name == "RangeError" || name == "SyntaxError" || name == "ReferenceError" ||
           name == "isNaN" || name == "isFinite" || name == "parseInt" || name == "parseFloat" ||
           name == "ArrayBuffer" || name == "Int8Array" || name == "Uint8Array" ||
           name == "Uint8ClampedArray" || name == "Int16Array" || name == "Uint16Array" ||
           name == "Int32Array" || name == "Uint32Array" || name == "Float32Array" ||
           name == "Float64Array" || name == "DataView" || name == "Function" ||
           // The three views 23.2 has that bronze had not built: half-precision
           // floats, and the two whose elements are BigInts rather than Numbers.
           name == "Float16Array" || name == "BigInt64Array" ||
           name == "BigUint64Array" ||
           // 25.2 and 25.4. bronze runs one agent, so the memory a
           // SharedArrayBuffer names is shared with nobody and every `Atomics`
           // operation is an ordinary access — the surface is real, the three
           // operations that need a second agent are named refusals.
           name == "SharedArrayBuffer" || name == "Atomics" ||
           name == "globalThis" || name == "Proxy" || name == "URIError" ||
           name == "Reflect" || name == "Date" || name == "encodeURI" ||
           name == "encodeURIComponent" || name == "decodeURI" ||
           name == "decodeURIComponent" ||
           // Annex B B.2.1. Normative for a web browser, and bronze's target is
           // browser code, so a program that escapes a string the old way
           // compiles rather than warning about a missing global.
           name == "escape" || name == "unescape" ||
           // %Iterator% (27.1.3). A program reaches the helper methods through
           // any iterator it holds, but the CONSTRUCTOR is what
           // `Iterator.from(x)` and `x instanceof Iterator` need to name.
           name == "Iterator";
}

// The `file:` URL of one module of the graph, for `import.meta.url`
// (16.2.1.10). The SourceSet holds each module's path as its generic (forward
// slash) form, so the only work here is the scheme and the leading slash a
// Windows drive letter needs: "file:///D:/x/main.js" against
// "file:///home/x/main.js".
//
// A build with no SourceSet has no path to name — `bronze il` used to be such
// a build and an embedder could be — so the fallback is the bare scheme rather
// than a fabricated path: "file:///" is honestly empty where a made-up file
// name would be a lie a program could read. The rule is spelled here so the
// callers of `lowerModule` cannot answer differently.
std::string Lowerer::moduleUrl(uint16_t fileId) const {
    if (!sources_ || fileId >= sources_->size()) return "file:///";
    std::string path(sources_->at(fileId).name());
    if (path.empty()) return "file:///";
    return path.front() == '/' ? "file://" + path : "file:///" + path;
}

uint32_t Lowerer::getKeyConstantIndex(const std::string& key) {
    auto it = keyConstants_.find(key);
    if (it != keyConstants_.end()) return it->second;
    uint32_t idx = static_cast<uint32_t>(keyConstants_.size());
    keyConstants_[key] = idx;
    return idx;
}

il::BlockId Lowerer::createBlock(il::Function& ilFn) {
    il::BlockId id = static_cast<il::BlockId>(ilFn.blocks.size());
    // Every block made inside a `try` names that try's handler, and the backend
    // derives its cell tests from that. It is stamped here, at creation, rather
    // than set by each construct: a handler that had to be assigned by hand
    // would be forgotten by exactly the construct that most needs it.
    ilFn.blocks.push_back(il::Block{.id = id, .handler = currentHandler_});
    return id;
}

void Lowerer::setCurrentBlock(size_t blockIdx) {
    currentBlockIdx_ = blockIdx;
}

void Lowerer::emitInst(il::Function& ilFn, const il::Instruction& inst) {
    if (currentBlockIdx_ >= ilFn.blocks.size()) {
        ilFn.blocks.push_back(il::Block{.id = static_cast<il::BlockId>(currentBlockIdx_)});
    }
    ilFn.blocks[currentBlockIdx_].instructions.push_back(inst);
}

bool Lowerer::currentBlockIsTerminated(const il::Function& ilFn) const {
    if (currentBlockIdx_ >= ilFn.blocks.size()) return false;
    const auto& insts = ilFn.blocks[currentBlockIdx_].instructions;
    return !insts.empty() && il::isTerminator(insts.back().op);
}

Lowerer::Value Lowerer::boxValueIfNeeded(Value val, il::Function& ilFn) {
    if (val.type == il::Type::Dynamic) return val;
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::Box;
    inst.type = il::Type::Dynamic;
    inst.boxType = val.type;
    inst.result = res;
    inst.operands = {val.id};
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

Lowerer::Value Lowerer::unboxValueIfNeeded(Value val, il::Type targetType, il::Function& ilFn) {
    if (val.type == targetType) return val;
    if (val.type == il::Type::Bool && targetType == il::Type::F64) {
        // ToNumber(bool) — route through the boxed form.
        val = boxValueIfNeeded(val, ilFn);
    }
    if (val.type == il::Type::Dynamic) {
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::Unbox;
        inst.type = targetType;
        inst.result = res;
        inst.operands = {val.id};
        emitInst(ilFn, inst);
        return Value{res, targetType};
    }
    diags_.error(Span{}, std::string("cannot convert ") + il::typeName(val.type) + " to " +
                             il::typeName(targetType));
    return val;
}

// Combine the pre-read target value with the rhs of a compound assignment.
//
// `x op= y` is `x op y` (13.15.2), so every operator here has to reach the
// SAME instruction its plain binary form reaches — that identity is the whole
// contract of this function, and it is the one that broke: `-=`, `*=`, `/=`
// and `%=` were unboxed to f64 unconditionally on the belief that they are
// "ToNumber on both operands whatever came in". They are ToNumERIC, and a
// BigInt operand takes the other branch, so `x -= 2n` threw out of the unbox
// while `x - 2n` next to it was exact.
//
// `+=` needs the extra proof the other four do not: JS `+` concatenates as
// soon as either side is a string, so it routes through the dynamic add unless
// inference *proved* the result is a Number. Unproven is the dynamic path,
// never the numeric one — unboxing a string pointer as a double is a
// miscompile, not a pessimisation.
Lowerer::Value Lowerer::emitCompoundCombine(Value cur, Value rhs, ast::BinaryOp binOp,
                                            bool provenNumeric, il::Function& ilFn) {
    if (binOp == ast::BinaryOp::PlusAssign && !provenNumeric) {
        Value l = boxValueIfNeeded(cur, ilFn);
        Value r = boxValueIfNeeded(rhs, ilFn);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::Add;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {l.id, r.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }
    // `&=`, `|=`, `^=`, `<<=`, `>>=`, `>>>=` and `**=` need no proof of
    // NUMBER-ness at all: unlike `+=` above, there is no reading of them under
    // which a string operand concatenates. They still need the boxed/unboxed
    // fork, and they get it inside `emitBitwise` and `emitPow`, which are the
    // same two functions the plain binary forms call.
    const ast::BinaryOp plain = ast::compoundAssignBase(binOp);
    if (const auto bitOp = bitwiseOpFor(plain)) {
        return emitBitwise(*bitOp, cur, rhs, ilFn);
    }
    if (plain == ast::BinaryOp::Exp) {
        return emitPow(cur, rhs, ilFn);
    }
    il::Op op = il::Op::Sub;
    switch (binOp) {
        case ast::BinaryOp::PlusAssign: op = il::Op::Add; break;
        case ast::BinaryOp::MinusAssign: op = il::Op::Sub; break;
        case ast::BinaryOp::StarAssign: op = il::Op::Mul; break;
        case ast::BinaryOp::SlashAssign: op = il::Op::Div; break;
        case ast::BinaryOp::PercentAssign: op = il::Op::Mod; break;
        default:
            diags_.error(Span{}, "unsupported compound assignment operator");
            return cur;
    }
    // The same fork `lowerBinary` makes, and stated the same way round: a
    // BOXED operand keeps its box and the instruction becomes the dynamic
    // form, whose helper owns ToNumeric, the BigInt algorithm and the mixing
    // TypeError. Only two proven-numeric operands may be unboxed.
    if (cur.type == il::Type::Dynamic || rhs.type == il::Type::Dynamic) {
        Value l = boxValueIfNeeded(cur, ilFn);
        Value r = boxValueIfNeeded(rhs, ilFn);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = op;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {l.id, r.id};
        emitInst(ilFn, inst);
        return Value{res, il::Type::Dynamic};
    }
    Value l = unboxValueIfNeeded(cur, il::Type::F64, ilFn);
    Value r = unboxValueIfNeeded(rhs, il::Type::F64, ilFn);
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = op;
    inst.type = il::Type::F64;
    inst.result = res;
    inst.operands = {l.id, r.id};
    emitInst(ilFn, inst);
    return Value{res, il::Type::F64};
}

// Conform a value flowing along a branch edge to the target block
// parameter's type. Box into dynamic params; unbox out of dynamic
// values (runtime-checked); anything else is a type conflict.
Lowerer::Value Lowerer::coerceToType(Value val, il::Type target, il::Function& ilFn) {
    if (val.type == target) return val;
    if (target == il::Type::Dynamic) return boxValueIfNeeded(val, ilFn);
    return unboxValueIfNeeded(val, target, ilFn);
}

Lowerer::Value Lowerer::lowerCondition(const ast::Expr& expr, il::Function& ilFn) {
    auto valOpt = lowerExpr(expr, ilFn);
    if (!valOpt) return Value{il::kNoValue, il::Type::Bool};
    return lowerConditionFromVal(*valOpt, ilFn);
}

Lowerer::Value Lowerer::lowerConditionFromVal(Value val, il::Function& ilFn) {
    if (val.type == il::Type::Bool) return val;
    if (val.type == il::Type::F64) {
        // Its own op rather than `cmp.ne %val, 0`: ToBoolean of a number is
        // false for NaN, while `!==` must answer true for `NaN !== NaN`. The
        // two questions have the same shape and opposite answers at NaN.
        il::ValueId truthyRes = ilFn.valueCount++;
        il::Instruction truthyInst;
        truthyInst.op = il::Op::NumTruthy;
        truthyInst.type = il::Type::Bool;
        truthyInst.result = truthyRes;
        truthyInst.operands = {val.id};
        emitInst(ilFn, truthyInst);
        return Value{truthyRes, il::Type::Bool};
    }
    if (val.type == il::Type::I32) {
        il::ValueId zeroRes = ilFn.valueCount++;
        il::Instruction zeroInst;
        zeroInst.op = il::Op::ConstI32;
        zeroInst.type = il::Type::I32;
        zeroInst.result = zeroRes;
        zeroInst.immI32 = 0;
        emitInst(ilFn, zeroInst);

        il::ValueId cmpRes = ilFn.valueCount++;
        il::Instruction cmpInst;
        cmpInst.op = il::Op::CmpNe;
        cmpInst.type = il::Type::Bool;
        cmpInst.result = cmpRes;
        cmpInst.operands = {val.id, zeroRes};
        emitInst(ilFn, cmpInst);
        return Value{cmpRes, il::Type::Bool};
    }
    // Dynamic
    return unboxValueIfNeeded(val, il::Type::Bool, ilFn);
}

}  // namespace bronze::lower
