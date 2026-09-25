// Direct calls to host natives (native_manifest.h). A call site the manifest
// names lowers to a machine call through the module's import table: the
// arguments are unboxed to the C signature here, the pointer-producing
// conversions (a handle's data, a typed array's bytes) come LAST — after
// every scalar coercion, since ToInt32 can run user code that allocates —
// and the result is boxed, or wrapped into a handle of the class the native
// returns. Nothing here resolves a symbol: the import function
// `__bronze_native_<i>` is a declaration the backend turns into a thunk over
// slot `i` of `<entry>_native_imports`.

#include <string>
#include <vector>

#include "lower/lowerer.h"
#include "lower/native_manifest.h"

namespace bronze::lower {

namespace {

il::Type nativeIlType(const abi::NativeTypeRef& ref) {
    switch (ref.kind) {
        case abi::NativeType::Void: return il::Type::Void;
        case abi::NativeType::F64: return il::Type::F64;
        case abi::NativeType::I32: return il::Type::I32;
        case abi::NativeType::Bool: return il::Type::Bool;
        default:
            // dynamic, a handle (its data pointer), a typed array's data
            // pointer, a str (the `const char*` the runtime made of it):
            // all the 64-bit word.
            return il::Type::Dynamic;
    }
}

// The IL parameter list of an import: the receiver, then each parameter as
// it crosses the call — a typed array is two (pointer, length).
std::vector<il::Type> importParamTypes(const NativeSig& sig) {
    std::vector<il::Type> out;
    if (sig.hasSelf()) out.push_back(il::Type::Dynamic);
    for (const auto& p : sig.paramTypes) {
        if (abi::nativeTypeIsTypedArray(p.kind)) {
            out.push_back(il::Type::Dynamic);
            out.push_back(il::Type::I32);
        } else {
            out.push_back(nativeIlType(p));
        }
    }
    return out;
}

}  // namespace

std::string Lowerer::getDottedPath(const ast::Expr* expr) const {
    if (const auto* id = dynamic_cast<const ast::Ident*>(expr)) {
        return isFreeIdentifier(id->name) ? id->name : std::string();
    }
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(expr)) {
        if (mem->isPrivate) return "";
        std::string base = getDottedPath(mem->object.get());
        if (base.empty()) return "";
        return base + "." + mem->property;
    }
    return "";
}

bool Lowerer::isFreeIdentifier(const std::string& name) const {
    if (name.empty() || name[0] == '#') return false;
    if (activeVarMap_.contains(name)) return false;
    uint32_t depth = 0, index = 0;
    if (currentEnvValue_ != il::kNoValue && findEnclosingEnvVar(name, depth, index)) return false;
    if (functionIndices_.contains(name)) return false;
    return true;
}

std::string Lowerer::getNativeClassOfExpr(const ast::Expr* expr) const {
    if (!nativeManifest_ || !expr) return "";
    if (const auto* id = dynamic_cast<const ast::Ident*>(expr)) {
        auto it = activeVarMap_.find(id->name);
        if (it == activeVarMap_.end()) {
            // A captured binding of an enclosing function: known only when
            // it is a `const` whose slot carries the class.
            return capturedConstNativeClass(id->name);
        }
        auto cls = varNativeClasses_.find(it->second);
        return cls == varNativeClasses_.end() ? std::string() : cls->second;
    }
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(expr)) {
        if (mem->isPrivate) return "";
        // A namespace getter that hands back a handle: `bro.time.clock`.
        const std::string dotted = getDottedPath(mem);
        if (!dotted.empty()) {
            const auto dot = dotted.rfind('.');
            if (const NativeSig* g = nativeManifest_->findGetter(dotted.substr(0, dot), dotted.substr(dot + 1))) {
                return g->producedClass();
            }
            return "";
        }
        const std::string owner = getNativeClassOfExpr(mem->object.get());
        if (owner.empty()) return "";
        if (const NativeSig* g = nativeManifest_->findGetter(owner, mem->property)) {
            return g->producedClass();
        }
        return "";
    }
    if (const auto* call = dynamic_cast<const ast::Call*>(expr)) {
        const std::string dotted = getDottedPath(call->callee.get());
        if (!dotted.empty()) {
            if (const NativeSig* f = nativeManifest_->findFunction(dotted)) return f->producedClass();
            return "";
        }
        if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
            if (mem->isPrivate) return "";
            const std::string owner = getNativeClassOfExpr(mem->object.get());
            if (owner.empty()) return "";
            if (const NativeSig* m = nativeManifest_->findMethod(owner, mem->property)) {
                return m->producedClass();
            }
        }
        return "";
    }
    if (const auto* newExpr = dynamic_cast<const ast::NewExpr*>(expr)) {
        const std::string dotted = getDottedPath(newExpr->callee.get());
        if (!dotted.empty() && nativeManifest_->findConstructor(dotted)) return dotted;
        return "";
    }
    return "";
}

void Lowerer::noteNativeClassOfBinding(const std::string& name, const std::string& cls) {
    if (!nativeManifest_) return;
    auto it = activeVarMap_.find(name);
    if (it == activeVarMap_.end()) return;
    if (cls.empty()) {
        varNativeClasses_.erase(it->second);
    } else {
        varNativeClasses_[it->second] = cls;
    }
    // A captured `const` carries its class on the environment slot too, so
    // the closures that read it see the same handle class this scope does.
    if (it->second < varBindings_.size()) {
        const VarBinding& b = varBindings_[it->second];
        if (b.inEnv && b.isConst && b.envScopeIndex < envScopes_.size()) {
            auto& slots = envScopes_[b.envScopeIndex].slotNativeClass;
            if (cls.empty()) {
                slots.erase(b.envSlot);
            } else {
                slots[b.envSlot] = cls;
            }
        }
    }
}

void Lowerer::planEnvSlotNativeClasses(size_t scopeIndex, const std::vector<const ast::Stmt*>& stmts) {
    if (!nativeManifest_ || scopeIndex >= envScopes_.size()) return;
    EnvScopeInfo& info = envScopes_[scopeIndex];
    for (const ast::Stmt* s : stmts) {
        const auto* vd = dynamic_cast<const ast::VarDecl*>(s);
        if (!vd || !vd->isConst || vd->name.empty() || !vd->init) continue;
        auto slot = info.slotOf.find(vd->name);
        if (slot == info.slotOf.end()) continue;
        // Only an initializer that names its class without going through
        // another binding of THIS scope — a construction, or a call on a
        // dotted namespace path. `const t = a.target()` is noted when the
        // declaration itself is lowered (noteNativeClassOfBinding), which is
        // before any closure written after it; what this pass adds is the
        // hoisted function declared above the `const` it reads.
        const ast::Expr* init = vd->init.get();
        bool selfContained = dynamic_cast<const ast::NewExpr*>(init) != nullptr;
        if (const auto* call = dynamic_cast<const ast::Call*>(init)) {
            selfContained = !getDottedPath(call->callee.get()).empty();
        } else if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(init)) {
            selfContained = !getDottedPath(mem).empty();
        }
        if (!selfContained) continue;
        const std::string cls = getNativeClassOfExpr(init);
        if (!cls.empty()) info.slotNativeClass[slot->second] = cls;
    }
}

void Lowerer::planEnvSlotNativeClasses(size_t scopeIndex, const std::vector<ast::StmtPtr>& stmts) {
    if (!nativeManifest_) return;
    std::vector<const ast::Stmt*> raw;
    raw.reserve(stmts.size());
    for (const auto& s : stmts) raw.push_back(s.get());
    planEnvSlotNativeClasses(scopeIndex, raw);
}

std::string Lowerer::capturedConstNativeClass(const std::string& name) const {
    if (!nativeManifest_) return "";
    // The same walk as findEnclosingEnvVar, innermost scope first, stopping
    // at the first scope that has the name — that is the binding the read
    // resolves to, whether or not it carries a class.
    for (size_t i = envScopes_.size(); i-- > 0;) {
        auto it = envScopes_[i].slotOf.find(name);
        if (it == envScopes_[i].slotOf.end()) continue;
        auto cls = envScopes_[i].slotNativeClass.find(it->second);
        return cls == envScopes_[i].slotNativeClass.end() ? std::string() : cls->second;
    }
    return "";
}

void Lowerer::initNativeManifestGlobals() {
    if (!nativeManifest_) return;
    // The root of every dotted path is a name the program mentions as a free
    // identifier. It joins the provided set so the accesses the lowering does
    // NOT short-circuit (`bro.mesh` as a value, `x instanceof bro.ai.AIAgent`)
    // read the host's object for it, exactly like any host global.
    for (const auto& root : nativeManifest_->namespaceRoots()) {
        hostGlobals_.insert(root);
    }
}

uint32_t Lowerer::registerExternalFunction(const std::string& symbol, il::Type returnType,
                                          const std::vector<il::Type>& paramTypes) {
    auto it = functionIndices_.find(symbol);
    if (it != functionIndices_.end()) {
        return it->second;
    }
    uint32_t idx = static_cast<uint32_t>(ilModule_.functions.size());
    il::Function fn;
    fn.name = symbol;
    fn.returnType = returnType;
    fn.isExternal = true;
    for (size_t i = 0; i < paramTypes.size(); ++i) {
        il::Param p;
        p.name = "arg" + std::to_string(i);
        p.type = paramTypes[i];
        fn.params.push_back(p);
    }
    ilModule_.functions.push_back(std::move(fn));
    functionIndices_[symbol] = idx;
    return idx;
}

uint32_t Lowerer::nativeImportFunction(const NativeSig& sig) {
    const std::string name = std::string(nativeKindName(sig.kind)) + " " + sig.path;
    auto it = nativeImportIndex_.find(name);
    if (it != nativeImportIndex_.end()) return ilModule_.nativeImports[it->second].functionIndex;
    const auto slot = static_cast<uint32_t>(ilModule_.nativeImports.size());
    const uint32_t fnIdx = registerExternalFunction("__bronze_native_" + std::to_string(slot),
                                                    nativeIlType(sig.returnType), importParamTypes(sig));
    // A typed-array return is a Dynamic to the program (the view the thunk
    // wraps); the descriptor argument and the wrap are the thunk's own and
    // never appear in the IL signature.
    const uint32_t bufferKind = abi::nativeTypeIsTypedArray(sig.returnType.kind)
                                    ? abi::nativeTypedArrayElementKind(sig.returnType.kind)
                                    : UINT32_MAX;
    ilModule_.nativeImports.push_back({name, sig.signature, fnIdx, bufferKind});
    nativeImportIndex_[name] = slot;
    return fnIdx;
}

uint32_t Lowerer::nativeClassTagFunction(const std::string& className) {
    const std::string name = "class " + className;
    auto it = nativeImportIndex_.find(name);
    if (it != nativeImportIndex_.end()) return ilModule_.nativeImports[it->second].functionIndex;
    const auto slot = static_cast<uint32_t>(ilModule_.nativeImports.size());
    const uint32_t fnIdx = registerExternalFunction("__bronze_native_" + std::to_string(slot),
                                                    il::Type::Dynamic, {});
    ilModule_.nativeImports.push_back({name, std::string(), fnIdx});
    nativeImportIndex_[name] = slot;
    return fnIdx;
}

Lowerer::Value Lowerer::emitNativeClassTag(const std::string& className, il::Function& ilFn) {
    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.op = il::Op::Call;
    inst.type = il::Type::Dynamic;
    inst.result = res;
    inst.calleeIndex = nativeClassTagFunction(className);
    emitInst(ilFn, inst);
    return Value{res, il::Type::Dynamic};
}

void Lowerer::emitNativeBindPrologue() {
    if (ilModule_.nativeImports.empty()) return;
    for (auto& fn : ilModule_.functions) {
        if (!fn.isEntryPoint || fn.blocks.empty()) continue;
        const uint32_t bindIdx = registerExternalFunction("__bronze_native_bind", il::Type::Void, {});
        il::Instruction inst;
        inst.op = il::Op::Call;
        inst.type = il::Type::Void;
        inst.result = il::kNoValue;
        inst.calleeIndex = bindIdx;
        auto& insts = fn.blocks[0].instructions;
        insts.insert(insts.begin(), inst);
        return;
    }
}

Lowerer::Value Lowerer::emitDefaultValueForType(il::Type type, il::Function& ilFn) {
    if (type == il::Type::Str) {
        uint32_t keyIdx = getKeyConstantIndex("");
        il::ValueId boxId = ilFn.valueCount++;
        il::Instruction boxInst;
        boxInst.op = il::Op::Box;
        boxInst.type = il::Type::Dynamic;
        boxInst.boxType = il::Type::Str;
        boxInst.result = boxId;
        boxInst.keyIndex = keyIdx;
        emitInst(ilFn, boxInst);
        return unboxValueIfNeeded(Value{boxId, il::Type::Dynamic}, il::Type::Str, ilFn);
    }

    il::ValueId res = ilFn.valueCount++;
    il::Instruction inst;
    inst.result = res;
    inst.type = type;
    switch (type) {
        case il::Type::F64:
            inst.op = il::Op::ConstF64;
            inst.immF64 = 0.0;
            break;
        case il::Type::I32:
            inst.op = il::Op::ConstI32;
            inst.immI32 = 0;
            break;
        case il::Type::Bool:
            inst.op = il::Op::ConstBool;
            inst.immI32 = 0;
            break;
        default:
            inst.op = il::Op::ConstUndefined;
            inst.type = il::Type::Dynamic;
            break;
    }
    emitInst(ilFn, inst);
    return Value{res, inst.type};
}

std::optional<Lowerer::Value> Lowerer::emitNativeInvoke(const NativeSig& sig, const ast::Expr* selfExpr,
                                                        std::optional<Value> selfValue,
                                                        const std::vector<const ast::Expr*>& args,
                                                        std::optional<Value> preLowered,
                                                        il::Function& ilFn) {
    // 1. Every operand expression, in source order, receiver first: the
    // evaluation order the language gives the call, before any coercion.
    std::optional<Value> self = selfValue;
    if (sig.hasSelf() && !self) {
        if (!selfExpr) return std::nullopt;
        self = lowerExpr(*selfExpr, ilFn);
        if (!self) return std::nullopt;
    }
    std::vector<std::optional<Value>> argVals(sig.paramTypes.size());
    if (preLowered) {
        argVals[0] = preLowered;
    } else {
        for (size_t i = 0; i < sig.paramTypes.size() && i < args.size(); ++i) {
            auto v = lowerExpr(*args[i], ilFn);
            if (!v) return std::nullopt;
            argVals[i] = v;
        }
        // Trailing arguments beyond the signature: evaluated for their
        // effects, then dropped, as a JS function drops them.
        for (size_t i = sig.paramTypes.size(); i < args.size(); ++i) {
            if (!lowerExpr(*args[i], ilFn)) return std::nullopt;
        }
    }

    // 2. Scalar coercions, in parameter order. Each can run user code
    // (valueOf, toString) and so allocate, which is why no pointer has been
    // taken yet.
    std::vector<Value> scalars(sig.paramTypes.size(), Value{});
    uint32_t strCount = 0;
    for (size_t i = 0; i < sig.paramTypes.size(); ++i) {
        const auto& p = sig.paramTypes[i];
        if (p.kind == abi::NativeType::Str) {
            // The UTF-8 copy the C native reads: pushed on the runtime's
            // scratch stack here, popped right after the call. Undefined (a
            // missing argument) is "", anything else is ToString.
            const uint32_t utf8Idx = registerExternalFunction(
                "bronze_native_str_utf8", il::Type::Dynamic, {il::Type::Dynamic});
            Value boxed = argVals[i] ? boxValueIfNeeded(*argVals[i], ilFn)
                                     : Value{emitConstUndefined(ilFn), il::Type::Dynamic};
            il::ValueId textId = ilFn.valueCount++;
            il::Instruction textInst;
            textInst.op = il::Op::Call;
            textInst.type = il::Type::Dynamic;
            textInst.result = textId;
            textInst.operands = {boxed.id};
            textInst.calleeIndex = utf8Idx;
            emitInst(ilFn, textInst);
            scalars[i] = Value{textId, il::Type::Dynamic};
            ++strCount;
            continue;
        }
        if (p.kind == abi::NativeType::Class || abi::nativeTypeIsTypedArray(p.kind)) {
            // A pointer-producing parameter: its VALUE is boxed now (an
            // absent argument is undefined, which the helper reports as a
            // TypeError naming what was expected), its pointer taken in 3.
            scalars[i] = argVals[i] ? boxValueIfNeeded(*argVals[i], ilFn)
                                    : Value{emitConstUndefined(ilFn), il::Type::Dynamic};
            continue;
        }
        const il::Type want = nativeIlType(p);
        if (!argVals[i]) {
            scalars[i] = emitDefaultValueForType(want, ilFn);
            continue;
        }
        Value v = *argVals[i];
        switch (p.kind) {
            case abi::NativeType::F64: v = unboxValueIfNeeded(v, il::Type::F64, ilFn); break;
            case abi::NativeType::I32: v = emitToInt32(v, ilFn); break;
            case abi::NativeType::Bool: v = lowerConditionFromVal(v, ilFn); break;  // ToBoolean
            default: v = boxValueIfNeeded(v, ilFn); break;
        }
        scalars[i] = v;
    }

    // 3. The pointers, immediately before the call: the receiver's data, then
    // each handle's data and each typed array's (bytes, length), in order.
    // Each helper throws a TypeError for a wrong value, which unwinds out
    // before the native runs.
    std::vector<il::ValueId> operands;
    const uint32_t handleDataIdx = registerExternalFunction(
        "bronze_native_handle_data", il::Type::Dynamic, {il::Type::Dynamic, il::Type::Dynamic});
    auto emitHandleData = [&](Value boxed, const std::string& cls) -> il::ValueId {
        Value tag = emitNativeClassTag(cls, ilFn);
        il::ValueId res = ilFn.valueCount++;
        il::Instruction inst;
        inst.op = il::Op::Call;
        inst.type = il::Type::Dynamic;
        inst.result = res;
        inst.operands = {boxed.id, tag.id};
        inst.calleeIndex = handleDataIdx;
        emitInst(ilFn, inst);
        return res;
    };
    // The values whose data the native is handed a pointer into. A handle's
    // destructor frees its data and a typed array's buffer can be collected,
    // so each is kept alive until the result has been converted (step 6),
    // across the native, any program it re-enters, and the allocation that
    // wraps or copies what it returned — which may point into that data.
    std::vector<il::ValueId> owners;
    if (sig.hasSelf()) {
        const Value boxedSelf = boxValueIfNeeded(*self, ilFn);
        owners.push_back(boxedSelf.id);
        operands.push_back(emitHandleData(boxedSelf, sig.className));
    }
    for (size_t i = 0; i < sig.paramTypes.size(); ++i) {
        const auto& p = sig.paramTypes[i];
        if (p.kind == abi::NativeType::Class || abi::nativeTypeIsTypedArray(p.kind)) {
            owners.push_back(scalars[i].id);
        }
        if (p.kind == abi::NativeType::Class) {
            operands.push_back(emitHandleData(scalars[i], p.className));
        } else if (abi::nativeTypeIsTypedArray(p.kind)) {
            const uint32_t dataIdx = registerExternalFunction(
                "bronze_native_typed_array_data", il::Type::Dynamic, {il::Type::Dynamic, il::Type::I32});
            const uint32_t lenIdx = registerExternalFunction(
                "bronze_native_typed_array_length", il::Type::I32, {il::Type::Dynamic});
            il::ValueId kindId = ilFn.valueCount++;
            il::Instruction kindInst;
            kindInst.op = il::Op::ConstI32;
            kindInst.type = il::Type::I32;
            kindInst.result = kindId;
            kindInst.immI32 = static_cast<int32_t>(abi::nativeTypedArrayElementKind(p.kind));
            emitInst(ilFn, kindInst);

            il::ValueId dataId = ilFn.valueCount++;
            il::Instruction dataInst;
            dataInst.op = il::Op::Call;
            dataInst.type = il::Type::Dynamic;
            dataInst.result = dataId;
            dataInst.operands = {scalars[i].id, kindId};
            dataInst.calleeIndex = dataIdx;
            emitInst(ilFn, dataInst);

            il::ValueId lenId = ilFn.valueCount++;
            il::Instruction lenInst;
            lenInst.op = il::Op::Call;
            lenInst.type = il::Type::I32;
            lenInst.result = lenId;
            lenInst.operands = {scalars[i].id};
            lenInst.calleeIndex = lenIdx;
            emitInst(ilFn, lenInst);
            operands.push_back(dataId);
            operands.push_back(lenId);
        } else {
            operands.push_back(scalars[i].id);
        }
    }

    // 4. The call, through the import slot.
    const il::Type retType = nativeIlType(sig.returnType);
    il::ValueId res = (retType != il::Type::Void) ? ilFn.valueCount++ : il::kNoValue;
    il::Instruction call;
    call.op = il::Op::Call;
    call.type = retType;
    call.result = res;
    call.operands = std::move(operands);
    call.calleeIndex = nativeImportFunction(sig);
    emitInst(ilFn, call);

    // The str copies, popped now that the native has returned. On the
    // exception path (the check after the call) they stay until the next
    // release — a bounded leftover, never a dangling pointer, since a live
    // outer call's entries sit below them.
    if (strCount > 0) {
        const uint32_t releaseIdx = registerExternalFunction(
            "bronze_native_str_release", il::Type::Void, {il::Type::I32});
        il::ValueId countId = ilFn.valueCount++;
        il::Instruction countInst;
        countInst.op = il::Op::ConstI32;
        countInst.type = il::Type::I32;
        countInst.result = countId;
        countInst.immI32 = static_cast<int32_t>(strCount);
        emitInst(ilFn, countInst);
        il::Instruction release;
        release.op = il::Op::Call;
        release.type = il::Type::Void;
        release.result = il::kNoValue;
        release.operands = {countId};
        release.calleeIndex = releaseIdx;
        emitInst(ilFn, release);
    }

    // 5. The result, as the program sees it; 6. the owners, kept to here.
    const Value result = convertNativeResult(sig, retType, res, ilFn);
    if (!owners.empty()) {
        il::Instruction keep;
        keep.op = il::Op::KeepAlive;
        keep.type = il::Type::Void;
        keep.result = il::kNoValue;
        keep.operands = std::move(owners);
        emitInst(ilFn, keep);
    }
    return result;
}

Lowerer::Value Lowerer::convertNativeResult(const NativeSig& sig, il::Type retType, il::ValueId res,
                                            il::Function& ilFn) {
    if (retType == il::Type::Void) {
        return Value{emitConstUndefined(ilFn), il::Type::Dynamic};
    }
    if (sig.returnType.kind == abi::NativeType::Class) {
        // A raw pointer: wrapped into a handle of the class, which is what
        // makes the class's methods, its prototype and its destructor apply.
        const uint32_t wrapIdx = registerExternalFunction(
            "bronze_native_wrap", il::Type::Dynamic, {il::Type::Dynamic, il::Type::Dynamic});
        Value tag = emitNativeClassTag(sig.returnType.className, ilFn);
        il::ValueId wrapped = ilFn.valueCount++;
        il::Instruction wrap;
        wrap.op = il::Op::Call;
        wrap.type = il::Type::Dynamic;
        wrap.result = wrapped;
        wrap.operands = {res, tag.id};
        wrap.calleeIndex = wrapIdx;
        emitInst(ilFn, wrap);
        return Value{wrapped, il::Type::Dynamic, sig.returnType.className};
    }
    if (sig.returnType.kind == abi::NativeType::Str) {
        // The `const char*` the native answered, copied into a string value.
        const uint32_t fromIdx = registerExternalFunction(
            "bronze_native_str_from_utf8", il::Type::Dynamic, {il::Type::Dynamic});
        il::ValueId strId = ilFn.valueCount++;
        il::Instruction from;
        from.op = il::Op::Call;
        from.type = il::Type::Dynamic;
        from.result = strId;
        from.operands = {res};
        from.calleeIndex = fromIdx;
        emitInst(ilFn, from);
        return Value{strId, il::Type::Dynamic};
    }
    if (retType == il::Type::I32) {
        // An int32 is an intermediate the lattice has no element for
        // (lower_expr_binary.cpp): read back as the number it denotes.
        return unboxValueIfNeeded(Value{res, il::Type::I32}, il::Type::F64, ilFn);
    }
    return Value{res, retType, sig.returnClass};
}

std::optional<Lowerer::Value> Lowerer::tryLowerNativeCall(const ast::Call* call, il::Function& ilFn) {
    if (!nativeManifest_) return std::nullopt;
    std::vector<const ast::Expr*> args;
    args.reserve(call->args.size());
    for (const auto& a : call->args) args.push_back(a.get());

    // 1. A namespace function, `bro.mesh.box(w, h)`: a dotted path rooted at
    // a free identifier that the manifest names.
    const std::string dotted = getDottedPath(call->callee.get());
    if (!dotted.empty()) {
        if (const NativeSig* sig = nativeManifest_->findFunction(dotted)) {
            return emitNativeInvoke(*sig, nullptr, std::nullopt, args, std::nullopt, ilFn);
        }
        return std::nullopt;
    }

    // 2. A method on a binding known to hold a handle of a class.
    if (const auto* mem = dynamic_cast<const ast::MemberAccess*>(call->callee.get())) {
        if (mem->isPrivate) return std::nullopt;
        const std::string cls = getNativeClassOfExpr(mem->object.get());
        if (cls.empty()) return std::nullopt;
        if (const NativeSig* sig = nativeManifest_->findMethod(cls, mem->property)) {
            return emitNativeInvoke(*sig, mem->object.get(), std::nullopt, args, std::nullopt, ilFn);
        }
    }
    return std::nullopt;
}

std::optional<Lowerer::Value> Lowerer::tryLowerNativeNew(const ast::NewExpr* newExpr, il::Function& ilFn) {
    if (!nativeManifest_) return std::nullopt;
    const std::string dotted = getDottedPath(newExpr->callee.get());
    if (dotted.empty()) return std::nullopt;
    const NativeSig* sig = nativeManifest_->findConstructor(dotted);
    if (!sig) return std::nullopt;
    std::vector<const ast::Expr*> args;
    for (const auto& a : newExpr->args) args.push_back(a.get());
    return emitNativeInvoke(*sig, nullptr, std::nullopt, args, std::nullopt, ilFn);
}

std::optional<Lowerer::Value> Lowerer::tryLowerNativePropertyGet(const ast::MemberAccess* mem,
                                                                il::Function& ilFn, bool onSpine) {
    if (!nativeManifest_ || mem->isPrivate) return std::nullopt;

    // 1. A namespace property, `bro.time.scale`.
    const std::string dotted = getDottedPath(mem);
    if (!dotted.empty()) {
        const auto dot = dotted.rfind('.');
        if (const NativeSig* g = nativeManifest_->findGetter(dotted.substr(0, dot), dotted.substr(dot + 1))) {
            return emitNativeInvoke(*g, nullptr, std::nullopt, {}, std::nullopt, ilFn);
        }
        return std::nullopt;
    }

    // 2. An instance property of a known class, `agent.position`.
    const std::string cls = getNativeClassOfExpr(mem->object.get());
    if (cls.empty()) return std::nullopt;
    const NativeSig* g = nativeManifest_->findGetter(cls, mem->property);
    if (!g) return std::nullopt;
    auto objVal = lowerChainBase(*mem->object, ilFn, onSpine);
    if (!objVal) return std::nullopt;
    return emitNativeInvoke(*g, nullptr, objVal, {}, std::nullopt, ilFn);
}

std::optional<Lowerer::Value> Lowerer::tryLowerNativeAssignment(const ast::Binary* bin,
                                                               il::Function& ilFn) {
    if (!nativeManifest_) return std::nullopt;
    const auto* mem = dynamic_cast<const ast::MemberAccess*>(bin->lhs.get());
    if (!mem || mem->isPrivate) return std::nullopt;

    // Which setter (and, for a compound assignment, which getter): a
    // namespace property or an instance property of a known class.
    std::string owner;
    const NativeSig* setter = nullptr;
    const NativeSig* getter = nullptr;
    std::optional<Value> self;
    const std::string dotted = getDottedPath(mem);
    if (!dotted.empty()) {
        const auto dot = dotted.rfind('.');
        owner = dotted.substr(0, dot);
        setter = nativeManifest_->findSetter(owner, mem->property);
        getter = nativeManifest_->findGetter(owner, mem->property);
        if (!setter && !getter) return std::nullopt;
    } else {
        owner = getNativeClassOfExpr(mem->object.get());
        if (owner.empty()) return std::nullopt;
        setter = nativeManifest_->findSetter(owner, mem->property);
        getter = nativeManifest_->findGetter(owner, mem->property);
        if (!setter && !getter) return std::nullopt;
    }
    if (!setter) {
        diags_.error(bin->lhs->span, "cannot assign to read-only native property '" + owner + "." +
                                         mem->property + "'");
        return std::nullopt;
    }
    const bool compound = bin->op != ast::BinaryOp::Assign;
    if (compound && !getter) {
        diags_.error(bin->lhs->span, "native property '" + owner + "." + mem->property +
                                         "' has no getter, so it cannot be read for a compound assignment");
        return std::nullopt;
    }
    if (compound && (bin->op == ast::BinaryOp::LogicalAndAssign ||
                     bin->op == ast::BinaryOp::LogicalOrAssign ||
                     bin->op == ast::BinaryOp::NullishAssign)) {
        diags_.error(bin->lhs->span, "a logical assignment to native property '" + owner + "." +
                                         mem->property + "' is not supported; write it out");
        return std::nullopt;
    }

    // The receiver once, shared by the read and the write.
    if (setter->hasSelf()) {
        self = lowerExpr(*mem->object, ilFn);
        if (!self) return std::nullopt;
        self = boxValueIfNeeded(*self, ilFn);
    }

    std::optional<Value> current;
    if (compound) {
        current = emitNativeInvoke(*getter, nullptr, self, {}, std::nullopt, ilFn);
        if (!current) return std::nullopt;
    }
    auto rhs = lowerExpr(*bin->rhs, ilFn);
    if (!rhs) return std::nullopt;
    Value stored = compound ? emitCompoundCombine(*current, *rhs, bin->op, provenNumber(*bin), ilFn)
                            : *rhs;
    // The setter's one parameter, already lowered; the invoke coerces it.
    if (!emitNativeInvoke(*setter, nullptr, self, {}, stored, ilFn)) return std::nullopt;
    // An assignment's value is the RHS as assigned (13.15.2), not what the
    // setter made of it.
    return boxValueIfNeeded(stored, ilFn);
}

}  // namespace bronze::lower
