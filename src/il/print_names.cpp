#include "il/print.h"

// The spelling of each type and op in printed IL. Kept apart from print.cpp,
// which lays out whole modules, because this table grows with every new op.

namespace bronze::il {

const char* typeName(Type t) {
    switch (t) {
        case Type::Void: return "void";
        case Type::Bool: return "bool";
        case Type::I32: return "i32";
        case Type::F64: return "f64";
        case Type::Str: return "str";
        case Type::Dynamic: return "dynamic";
    }
    return "?";
}

const char* opName(Op op) {
    switch (op) {
        case Op::ConstF64: return "const.f64";
        case Op::ConstI32: return "const.i32";
        case Op::ConstBool: return "const.bool";
        case Op::ConstUndefined: return "const.undefined";
        case Op::ConstNull: return "const.null";
        case Op::ConstBigInt: return "const.bigint";
        case Op::Add: return "add";
        case Op::ConcatBegin: return "concat.begin";
        case Op::ConcatAppend: return "concat.append";
        case Op::ConcatEnd: return "concat.end";
        case Op::Sub: return "sub";
        case Op::Neg: return "neg";
        case Op::Mul: return "mul";
        case Op::Div: return "div";
        case Op::Mod: return "mod";
        case Op::Pow: return "pow";
        case Op::ToNumeric: return "to.numeric";
        case Op::NumericStep: return "numeric.step";
        case Op::ToInt32: return "to.int32";
        case Op::BitAnd: return "and";
        case Op::BitOr: return "or";
        case Op::BitXor: return "xor";
        case Op::MathImul: return "math.imul";
        case Op::Shl: return "shl";
        case Op::Shr: return "shr";
        case Op::UShr: return "ushr";
        case Op::BitNot: return "bitnot";
        case Op::CmpLt: return "cmp.lt";
        case Op::CmpGt: return "cmp.gt";
        case Op::CmpLe: return "cmp.le";
        case Op::CmpGe: return "cmp.ge";
        case Op::CmpEq: return "cmp.eq";
        case Op::CmpNe: return "cmp.ne";
        case Op::NumTruthy: return "num.truthy";
        case Op::StrictEq: return "strict.eq";
        case Op::LooseEq: return "loose.eq";
        case Op::RelLt: return "rel.lt";
        case Op::RelGt: return "rel.gt";
        case Op::RelLe: return "rel.le";
        case Op::RelGe: return "rel.ge";
        case Op::TypeOf: return "typeof";
        case Op::ToStr: return "to.string";
        case Op::InstanceOf: return "instanceof";
        case Op::In: return "in";
        case Op::IsNullish: return "is.nullish";
        case Op::IsNumber: return "is.number";
        case Op::IsDenseArray: return "is.dense_array";
        case Op::Ret: return "ret";
        case Op::Throw: return "throw";
        case Op::ExcTake: return "exc.take";
        case Op::Jump: return "jump";
        case Op::Branch: return "br";
        case Op::Call: return "call";
        case Op::Box: return "box";
        case Op::Unbox: return "unbox";
        case Op::PropGet: return "prop.get";
        case Op::SuperGet: return "super.get";
        case Op::SuperSet: return "super.set";
        case Op::PropSet: return "prop.set";
        case Op::ElemGet: return "elem.get";
        case Op::ElemSet: return "elem.set";
        case Op::ElemGetTyped: return "elem.get.typed";
        case Op::ElemSetTyped: return "elem.set.typed";
        case Op::MathUnary: return "math.unary";
        case Op::DynamicCall: return "call.dynamic";
        case Op::FunctionRef: return "func.ref";
        case Op::Construct: return "new";
        case Op::CreateObject: return "create.object";
        case Op::CoroStart: return "coro.start";
        case Op::CoroSuspend: return "coro.suspend";
        case Op::CoroMode: return "coro.mode";
        case Op::DynamicImport: return "dynamic_import";
        case Op::ModuleNamespace: return "module.namespace";
        case Op::ObjectKeys: return "object.keys";
        case Op::ForInKeys: return "forin.keys";
        case Op::MethodDef: return "method.def";
        case Op::MethodDefComputed: return "method.def.computed";
        case Op::AccessorDef: return "accessor.def";
        case Op::AccessorDefComputed: return "accessor.def.computed";
        case Op::DefineOwnAttr: return "define.own.attr";
        case Op::GetNewTarget: return "get.new_target";
        case Op::ImportMeta: return "import.meta";
        case Op::SuperCall: return "call.super";
        case Op::SuperCallSpread: return "call.super.spread";
        case Op::TemplateCached: return "template.cached";
        case Op::TemplateObject: return "template.object";
        case Op::ArrayAppendHole: return "array.append.hole";
        case Op::PropDelete: return "prop.delete";
        case Op::ElemDelete: return "elem.delete";
        case Op::GlobalGet: return "global.get";
        case Op::ResolveName: return "name.resolve";
        case Op::ImmutableAssign: return "immutable.assign";
        case Op::PinGuard: return "pin.guard";
        case Op::CensusRecord: return "census.record";
        case Op::ClassExtend: return "class.extend";
        case Op::PrivateNew: return "private.new";
        case Op::PrivateHas: return "private.has";
        case Op::PrivateGet: return "private.get";
        case Op::PrivateAdd: return "private.add";
        case Op::PrivateSet: return "private.set";
        case Op::PrivateMisuse: return "private.misuse";
        case Op::IterOpen: return "iter.open";
        case Op::AsyncIterOpen: return "async_iter.open";
        case Op::AsyncIterNext: return "async_iter.next";
        case Op::AsyncIterClose: return "async_iter.close";
        case Op::IterStep: return "iter.step";
        case Op::IterValue: return "iter.value";
        case Op::IterClose: return "iter.close";
        case Op::IterRest: return "iter.rest";
        case Op::IterDelegate: return "iter.delegate";
        case Op::PatternCheck: return "pattern.check";
        case Op::ArrayAppend: return "array.append";
        case Op::ArraySpread: return "array.spread";
        case Op::ObjectSpread: return "object.spread";
        case Op::ObjectRest: return "object.rest";
        case Op::DynamicCallSpread: return "call.dynamic.spread";
        case Op::MethodCall: return "method.call";
        case Op::MethodCallSpread: return "method.call.spread";
        case Op::ConstructSpread: return "new.spread";
        case Op::CreateArray: return "create.array";
        case Op::CreateFunction: return "create.func";
        case Op::EnvCreate: return "env.create";
        case Op::EnvGet: return "env.get";
        case Op::EnvSet: return "env.set";
        case Op::EnvInitTdz: return "env.init.tdz";
        case Op::EnvGetTdz: return "env.get.tdz";
        case Op::ModuleEnvSet: return "module.env.set";
        case Op::ModuleEnvGet: return "module.env.get";
        case Op::Print: return "print";
        case Op::PrintErr: return "print.err";
        case Op::PrintSpread: return "print.spread";
        case Op::PrintSpreadErr: return "print.spread.err";
        case Op::KeepAlive: return "keep.alive";
        case Op::MethodIcIs: return "method.ic.is";
        case Op::AccessorIcIs: return "accessor.ic.is";
        case Op::IsTypedArray: return "is.typed_array";
        case Op::IsNumberArray: return "is.number_array";
        case Op::MethodSlotFn: return "method.slot.fn";
        case Op::ClosureIs: return "closure.is";
        case Op::ClosureEnv: return "closure.env";
        case Op::ObjectLitProto: return "object.proto";
    }
    return "?";
}

}  // namespace bronze::il
