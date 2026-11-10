// The self-hosted builtins' compile (src/selfhost/README.md): the dialect of
// JavaScript the runtime's own builtins are written in, and what turns its
// functions into builtins.
//
// The dialect is ordinary strict JavaScript with two rules:
//   - `$Name(...)` is an INTRINSIC: an operation of the specification's
//     (Call, ToObject, ArraySpeciesCreate, ...) that the builtin must perform
//     without going through anything a program can patch. Each lowers to an
//     IL instruction or to a call of one runtime helper by symbol, never to a
//     property read, so `Function.prototype.call = f` or a replaced global
//     cannot reach it. Only this mode reads the `$` prefix; in any other
//     compile `$Call` is an ordinary (unresolved) name, so no program can name
//     an intrinsic.
//   - no global is read. A builtin that looked up `Array` or `TypeError` would
//     see what the program put there; the check runs over the finished IL, so
//     a global the lowering itself reaches for (a RegExp literal's `RegExp`)
//     is refused too.
//
// A top-level `$Builtin(fn, "Array.prototype.map", "Array.map", "map", 1)`
// names a function as a builtin: the runtime's registry key, the name its
// frame has in Error.stack, its `name` and its `length`. The function object
// is made NATIVE (`toString` gives the NativeFunction form, 20.2.3.5), with
// no [[Construct]] and no `prototype` (10.3: a built-in function that is not
// a constructor has neither), and lives in no realm (the intrinsics are the
// thread's, docs/internals.md "Realms and lockdown"). Every other function of
// the library is a HIDDEN frame: part of a builtin's algorithm, which a stack
// trace leaves out as it leaves out a C++ builtin's own calls.

#include <string>

#include "lower/lowerer.h"

namespace bronze::lower {

namespace {

// The helpers an intrinsic calls: the runtime's (src/runtime/selfhost.cpp),
// each taking and returning Value bits. `argc` is the exact count.
struct IntrinsicHelper {
    const char* name;
    const char* symbol;
    uint32_t argc;
};

constexpr IntrinsicHelper kHelpers[] = {
    {"$ToObject", "bronze_sh_to_object", 2},
    {"$ThrowNotCallable", "bronze_sh_throw_not_callable", 1},
    {"$ThrowTypeError", "bronze_sh_throw_type_error", 1},
    {"$ThrowRangeError", "bronze_sh_throw_range_error", 1},
    {"$ArraySpeciesCreate", "bronze_sh_array_species_create", 2},
    {"$LengthOfArrayLike", "bronze_sh_length_of_array_like", 1},
    {"$Builtin", "bronze_sh_register", 2},
};

const ast::Call* builtinRow(const ast::Stmt* stmt) {
    const auto* es = dynamic_cast<const ast::ExprStmt*>(stmt);
    if (!es) return nullptr;
    const auto* call = dynamic_cast<const ast::Call*>(es->expr.get());
    if (!call) return nullptr;
    const auto* callee = dynamic_cast<const ast::Ident*>(call->callee.get());
    return callee && callee->name == "$Builtin" ? call : nullptr;
}

const std::string* stringArg(const ast::Call& call, size_t i) {
    if (i >= call.args.size()) return nullptr;
    const auto* s = dynamic_cast<const ast::StringLit*>(call.args[i].get());
    return s ? &s->value : nullptr;
}

}  // namespace

bool Lowerer::planSelfHostBuiltins() {
    std::unordered_set<std::string> declared;
    for (const auto& stmt : astModule_.body) {
        if (const auto* fn = dynamic_cast<const ast::FunctionDecl*>(stmt.get())) declared.insert(fn->name);
    }
    for (const auto& stmt : astModule_.body) {
        const ast::Call* row = builtinRow(stmt.get());
        if (!row) continue;
        const auto* fn = row->args.empty() ? nullptr : dynamic_cast<const ast::Ident*>(row->args[0].get());
        const std::string* key = stringArg(*row, 1);
        const std::string* display = stringArg(*row, 2);
        const std::string* name = stringArg(*row, 3);
        const auto* length = row->args.size() == 5 ? dynamic_cast<const ast::NumberLit*>(row->args[4].get()) : nullptr;
        if (!fn || !key || !display || !name || !length || !declared.contains(fn->name)) {
            diags_.error(row->span,
                         "$Builtin(fn, key, display, name, length): fn a top-level function declaration, "
                         "three string literals and a number literal");
            return false;
        }
        if (!selfHostBuiltins_
                 .emplace(fn->name, SelfHostBuiltin{*key, *display, getKeyConstantIndex(*name),
                                                    static_cast<uint32_t>(length->value)})
                 .second) {
            diags_.error(row->span, "$Builtin: " + fn->name + " is registered twice");
            return false;
        }
    }
    return true;
}

bool Lowerer::lowerIntrinsicCall(const ast::Call* call, il::Function& ilFn, std::optional<Value>& out) {
    const auto* callee = dynamic_cast<const ast::Ident*>(call->callee.get());
    if (!selfHost_ || !callee || callee->name.empty() || callee->name[0] != '$') return false;
    out.reset();
    for (const auto& a : call->args) {
        if (dynamic_cast<const ast::SpreadElement*>(a.get())) {
            diags_.error(call->span, callee->name + ": an intrinsic takes no spread argument");
            return true;
        }
    }
    std::vector<il::ValueId> operands;
    // `$Builtin(fn, key, ...)`: the registration is the function and its key;
    // the rest of the row was read before lowering (planSelfHostBuiltins).
    const size_t evaluated = callee->name == "$Builtin" ? std::min<size_t>(call->args.size(), 2) : call->args.size();
    for (size_t i = 0; i < evaluated; ++i) {
        auto v = lowerExpr(*call->args[i], ilFn);
        if (!v) return true;
        operands.push_back(boxValueIfNeeded(*v, ilFn).id);
    }
    if (callee->name == "$Builtin") std::swap(operands[0], operands[1]);

    // 7.3.14 Call(F, V, args): the callee entered with `V` as its receiver,
    // through no property a program can replace.
    if (callee->name == "$Call") {
        if (operands.size() < 2) {
            diags_.error(call->span, "$Call(f, thisArg, ...args)");
            return true;
        }
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::DynamicCall;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = std::move(operands);
        inst.span = call->span;
        emitInst(ilFn, inst);
        out = Value{res, il::Type::Dynamic};
        return true;
    }
    // 7.3.7 CreateDataPropertyOrThrow(A, k, v): the define form of elem.set
    // (il.h kWriteDefine), whose backend arm stores into an open Array inline.
    if (callee->name == "$CreateDataPropertyOrThrow") {
        if (operands.size() != 3) {
            diags_.error(call->span, "$CreateDataPropertyOrThrow(A, k, v)");
            return true;
        }
        il::Instruction inst;
        inst.op = il::Op::ElemSet;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.operands = std::move(operands);
        inst.immI32 = il::kWriteDefine;
        inst.span = call->span;
        emitInst(ilFn, inst);
        out = Value{emitConstUndefined(ilFn), il::Type::Dynamic};
        return true;
    }
    for (const IntrinsicHelper& h : kHelpers) {
        if (callee->name != h.name) continue;
        if (operands.size() != h.argc) {
            diags_.error(call->span, callee->name + " takes " + std::to_string(h.argc) + " arguments");
            return true;
        }
        const uint32_t fnIdx = registerExternalFunction(
            h.symbol, il::Type::Dynamic, std::vector<il::Type>(h.argc, il::Type::Dynamic));
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::Call;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.calleeIndex = fnIdx;
        inst.operands = std::move(operands);
        inst.span = call->span;
        emitInst(ilFn, inst);
        out = Value{res, il::Type::Dynamic};
        return true;
    }
    diags_.error(callee->span, "unknown intrinsic " + callee->name);
    return true;
}

bool Lowerer::finishSelfHost() {
    bool ok = true;
    for (il::Function& fn : ilModule_.functions) {
        if (fn.isExternal) continue;
        for (const il::Block& block : fn.blocks) {
            for (const il::Instruction& inst : block.instructions) {
                if (inst.op != il::Op::GlobalGet && inst.op != il::Op::ResolveName) continue;
                const std::string& key = ilModule_.keyConstants[inst.keyIndex];
                diags_.error(inst.span, "self-hosted code reads the global '" + key +
                                            "', which a program can replace; use an intrinsic");
                ok = false;
            }
        }
        if (fn.isEntryPoint) continue;
        const std::string& origin =
            fn.typedOrigin != il::Function::kNoTypedOrigin ? ilModule_.functions[fn.typedOrigin].name : fn.name;
        auto it = selfHostBuiltins_.find(origin);
        if (it == selfHostBuiltins_.end()) {
            fn.descFlags |= BRONZE_FN_DESC_HIDDEN;
            continue;
        }
        if (fn.needsEnv || fn.needsArguments || fn.isGenerator || fn.isCoroutineBody() ||
            (fn.fnFlags & (BRONZE_ABI_FN_FLAG_GENERATOR | BRONZE_ABI_FN_FLAG_ASYNC)) != 0) {
            diags_.error(Span{}, "self-hosted builtin " + fn.name +
                                     " captures, reads `arguments`, or is a generator or async function");
            ok = false;
            continue;
        }
        fn.fnFlags = BRONZE_ABI_FN_FLAG_NATIVE;
        fn.nameKeyIndex = it->second.nameKey;
        fn.requiredArgs = it->second.length;
        fn.displayName = it->second.display;
        fn.descFlags |= BRONZE_FN_DESC_BUILTIN;
    }
    return ok;
}

}  // namespace bronze::lower
