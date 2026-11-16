// The module-level half of the IL verifier: what one function cannot answer
// alone. Each function's body (verifier.cpp verifyFunction), its feedback
// records, the direct calls against their callees' conventions, the
// speculation tables (shape, prototype and member cells, object literals) and
// every `leave` / `resume.call` against the resume point it names, the
// inline-cache and template site counts, and the proofs (verifier_proofs.cpp).

#include "il/verifier.h"
#include "il/verifier_proofs.h"
#include "abi/bronze_abi.h"
#include "il/feedback_site.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace bronze::il {

// Every value's type, by id: parameters, block parameters and results.
static std::vector<Type> valueTypes(const Function& fn) {
    std::vector<Type> types(fn.valueCount, Type::Void);
    auto set = [&](ValueId id, Type t) {
        if (id < types.size()) types[id] = t;
    };
    for (size_t i = 0; i < fn.params.size(); ++i) set(static_cast<ValueId>(i), fn.params[i].type);
    for (const auto& block : fn.blocks) {
        for (const auto& p : block.params) set(p.id, p.type);
        for (const auto& inst : block.instructions) {
            if (inst.result != kNoValue) set(inst.result, inst.type);
        }
    }
    return types;
}

// The `resume.point <id>` of `generic`, or nullptr.
static const Instruction* findResumePoint(const Function& generic, int32_t id) {
    for (const auto& block : generic.blocks) {
        for (const auto& inst : block.instructions) {
            if (inst.op == Op::ResumePoint && inst.immI32 == id) return &inst;
        }
    }
    return nullptr;
}

// What a `leave` and a `resume.call` of the speculated function `fn` owe the
// point they start G at, alike: G is a generic function of the module, it has
// the point, and the state matches the point's operands one for one in type.
// A leave's G is `fn`'s own; a resume.call's is any generic function (the
// callee's, of a body the inliner put in `fn`), and its result has G's
// return type. `ownTypes` is valueTypes(fn).
static bool checkResumeState(const Module& module, const Function& fn, const Instruction& inst,
                             const std::vector<Type>& ownTypes, DiagnosticSink& diags) {
    const std::string what = std::string("Function ") + fn.name + ": " + opName(inst.op);
    if (inst.op == Op::Leave && inst.calleeIndex != fn.speculatedFrom) {
        diags.error(Span{}, what + " names a function other than the one it was speculated from");
        return false;
    }
    if (inst.calleeIndex >= module.functions.size() || module.functions[inst.calleeIndex].isExternal ||
        module.functions[inst.calleeIndex].isSpeculated()) {
        diags.error(Span{}, what + " names no generic function of the module");
        return false;
    }
    const Function& generic = module.functions[inst.calleeIndex];
    if (inst.op == Op::ResumeCall &&
        (inst.type != generic.returnType || (inst.result != kNoValue) != (generic.returnType != Type::Void))) {
        diags.error(Span{}, what + " to @" + generic.name + " is " + typeName(inst.type) +
                                " where the function returns " + typeName(generic.returnType));
        return false;
    }
    const Instruction* point = findResumePoint(generic, inst.immI32);
    if (point == nullptr) {
        diags.error(Span{}, what + " to @" + generic.name + " names resume point " +
                                std::to_string(inst.immI32) + ", which it does not have");
        return false;
    }
    if (point->operands.size() != inst.operands.size()) {
        diags.error(Span{}, what + " to @" + generic.name + ", " + std::to_string(inst.immI32) +
                                " carries " + std::to_string(inst.operands.size()) + " state values for " +
                                std::to_string(point->operands.size()));
        return false;
    }
    const std::vector<Type> genericTypes = valueTypes(generic);
    for (size_t i = 0; i < inst.operands.size(); ++i) {
        const ValueId mine = inst.operands[i];
        const ValueId theirs = point->operands[i];
        const Type a = mine < ownTypes.size() ? ownTypes[mine] : Type::Void;
        const Type b = theirs < genericTypes.size() ? genericTypes[theirs] : Type::Void;
        if (a != b || a == Type::Void) {
            diags.error(Span{}, what + " to @" + generic.name + ", " + std::to_string(inst.immI32) +
                                    " state " + std::to_string(i) + " is " + typeName(a) +
                                    " where the resume point has " + typeName(b));
            return false;
        }
    }
    return true;
}

bool verify(const Module& module, DiagnosticSink& diags) {
    for (const auto& fn : module.functions) {
        // A declaration has no body to check; its calls are checked from the
        // caller's side like any other direct call.
        if (fn.isExternal) {
            if (!fn.blocks.empty()) {
                diags.error(Span{}, "External function " + fn.name + " has blocks");
                return false;
            }
            continue;
        }
        if (!verifyFunction(fn, diags) || !verifyFeedbackRecords(fn, diags)) {
            return false;
        }
    }

    // A closure's environment reaches it through the dynamic calling
    // convention, which a direct `call` does not use — so a direct call to a
    // function that needs an environment would enter it with garbage in the
    // environment parameter. Lowering routes every call to a closure through
    // call.dynamic; this checks that rather than trusting it.
    for (const auto& fn : module.functions) {
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                const bool refersToFunction = inst.op == Op::Call ||
                                              inst.op == Op::CreateFunction ||
                                              inst.op == Op::FunctionRef ||
                                              inst.op == Op::CoroStart;
                if (!refersToFunction) continue;
                if (inst.calleeIndex >= module.functions.size()) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                            " names an out-of-range function index");
                    return false;
                }
                // A coroutine body is entered only through the frame
                // `coro.start` makes, whose arguments are the body's
                // parameters one for one; nothing else may name it.
                const bool bodyTarget = module.functions[inst.calleeIndex].isCoroutineBody();
                if (bodyTarget != (inst.op == Op::CoroStart) ||
                    (bodyTarget && (inst.operands.size() !=
                                        module.functions[inst.calleeIndex].params.size() ||
                                    inst.immI32 != module.functions[inst.calleeIndex].coroKind))) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                            " @" + module.functions[inst.calleeIndex].name +
                                            (bodyTarget ? " does not match the coroutine body"
                                                        : " names a function that is no coroutine body"));
                    return false;
                }
                if (inst.op != Op::Call) continue;
                const Function& callee = module.functions[inst.calleeIndex];
                if (callee.needsEnv && inst.callEnvHops == Instruction::kNoEnvHops) {
                    diags.error(Span{}, "Function " + fn.name + ": direct call to closure @" +
                                            callee.name + ", which needs an environment");
                    return false;
                }
                // The converse, which is the same bug the other way round: a
                // site claiming to supply an environment to a function that
                // takes none would hand the record to its first SOURCE
                // parameter.
                if (!callee.needsEnv && inst.callEnvHops != Instruction::kNoEnvHops) {
                    diags.error(Span{}, "Function " + fn.name + ": direct call supplies an "
                                        "environment to @" + callee.name + ", which takes none");
                    return false;
                }
                // A direct call's operand list IS the callee's parameter list —
                // that is the whole of the convention — so a length mismatch is
                // a wrong value in every slot after the first missing one, and
                // it is worth naming here rather than meeting it as a type
                // error in the backend.
                if (inst.operands.size() != callee.params.size()) {
                    diags.error(Span{}, "Function " + fn.name + ": direct call to @" +
                                            callee.name + " passes " +
                                            std::to_string(inst.operands.size()) +
                                            " operands for " +
                                            std::to_string(callee.params.size()) + " parameters");
                    return false;
                }
            }
        }
    }

    // Speculation: a speculated function names a real generic one, every
    // `leave` resumes a point that function has, with state of the same
    // shape; every shape cell and code identity names something that exists.
    {
        std::unordered_set<std::string> keys;
        for (const auto& cell : module.shapeCells) {
            if (!keys.insert(cell.key).second) {
                diags.error(Span{}, "shape cell \"" + cell.key + "\" appears twice in the table");
                return false;
            }
        }
    }
    for (const auto& fn : module.functions) {
        if (fn.isSpeculated()) {
            if (fn.speculatedFrom >= module.functions.size() ||
                module.functions[fn.speculatedFrom].isExternal ||
                module.functions[fn.speculatedFrom].isSpeculated()) {
                diags.error(Span{}, "Function " + fn.name +
                                        " is speculated from a function index that is out of range, "
                                        "external or itself speculated");
                return false;
            }
        }
        std::vector<Type> ownTypes;
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == Op::ShapeCell &&
                    static_cast<uint32_t>(inst.immI32) >= module.shapeCells.size()) {
                    diags.error(Span{}, "Function " + fn.name + ": shape.cell " +
                                            std::to_string(inst.immI32) + " is past the module's " +
                                            std::to_string(module.shapeCells.size()) + " cells");
                    return false;
                }
                if ((inst.op == Op::ProtoCell || inst.op == Op::ProtoPublish) &&
                    static_cast<uint32_t>(inst.immI32) >= module.protoCells.size()) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) + " " +
                                            std::to_string(inst.immI32) + " is past the module's " +
                                            std::to_string(module.protoCells.size()) + " prototype cells");
                    return false;
                }
                if ((inst.op == Op::MemberCell || inst.op == Op::MemberPublish) &&
                    (static_cast<uint32_t>(inst.immI32) >= module.memberCells.size() ||
                     (module.memberCells[inst.immI32].kind != Module::MemberKind::Absent &&
                      module.memberCells[inst.immI32].callee >= module.functions.size()))) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) + " " +
                                            std::to_string(inst.immI32) + " names no member cell of the module");
                    return false;
                }
                if (inst.op == Op::ObjectLiteral &&
                    (inst.immI32 < 0 || static_cast<uint32_t>(inst.immI32) >= module.objectLiterals.size() ||
                     module.objectLiterals[inst.immI32].keys.size() != inst.operands.size())) {
                    diags.error(Span{}, "Function " + fn.name + ": object.literal " + std::to_string(inst.immI32) +
                                            " names no literal of the module, or not with " +
                                            std::to_string(inst.operands.size()) + " values");
                    return false;
                }
                if (inst.op == Op::FnCodeIs && inst.calleeIndex >= module.functions.size()) {
                    diags.error(Span{}, "Function " + fn.name +
                                            ": fn.code_is names an out-of-range function index");
                    return false;
                }
                // verifyFunction let a resume-after id stand only on a direct
                // call of a speculated function.
                if (inst.resumeAfter != Instruction::kNoResumeAfter && fn.isSpeculated() &&
                    inst.op == Op::Call &&
                    findResumePoint(module.functions[fn.speculatedFrom],
                                    static_cast<int32_t>(inst.resumeAfter)) == nullptr) {
                    diags.error(Span{}, "Function " + fn.name + ": call @" +
                                            module.functions[inst.calleeIndex].name +
                                            " resumes after at point " +
                                            std::to_string(inst.resumeAfter) + ", which @" +
                                            module.functions[fn.speculatedFrom].name +
                                            " does not have");
                    return false;
                }
                if (inst.op != Op::Leave && inst.op != Op::ResumeCall) continue;
                if (ownTypes.empty()) ownTypes = valueTypes(fn);
                if (!checkResumeState(module, fn, inst, ownTypes, diags)) return false;
            }
        }
    }

    // The IC table is a fixed-size global array in the generated object file,
    // so an icIndex past the module's site count is an out-of-bounds store into
    // the object file's data, not a missed optimization. Named here rather than
    // clamped in the backend.
    for (const auto& fn : module.functions) {
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                // Write modes past the literal element's are no mode at all,
                // and an element store carries only the element flags above it.
                if ((inst.op == Op::PropSet || inst.op == Op::ElemSet) &&
                    (writeMode(inst) > kWriteLiteralElement ||
                     (inst.immI32 & ~(inst.op == Op::ElemSet ? kElemSetFlagBits : kWriteModeMask)) != 0 ||
                     (inst.op == Op::ElemSet && writeMode(inst) == kWriteLiteralElement))) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) + " in write mode " +
                                            std::to_string(inst.immI32) + " carries no mode");
                    return false;
                }
                if (inst.op == Op::PropGet || inst.op == Op::PropSet || inst.op == Op::MethodCall ||
                    inst.op == Op::MethodCallSpread) {
                    if (inst.icIndex >= module.icSiteCount) {
                        diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                                " names inline-cache site " +
                                                std::to_string(inst.icIndex) +
                                                ", past the module's site count " +
                                                std::to_string(module.icSiteCount));
                        return false;
                    }
                    continue;
                }
                // The template-slot table is the same kind of fixed-size global
                // array, so an index past the count is the same out-of-bounds
                // store — checked here for the same reason and not in the
                // backend.
                if (inst.op != Op::TemplateCached && inst.op != Op::TemplateObject) continue;
                if (inst.immI32 < 0 ||
                    static_cast<uint32_t>(inst.immI32) >= module.templateSiteCount) {
                    diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                            " names template site " +
                                            std::to_string(inst.immI32) +
                                            ", past the module's site count " +
                                            std::to_string(module.templateSiteCount));
                    return false;
                }
            }
        }
    }

    // A site is either a property site or a method-call site, never both: the
    // two read the same bytes under different contracts (bronze_abi.h, "the
    // METHOD-CALL site"), and the runtime registers a method site's env words
    // as heap-Value cells — a property entry's shape pointer in that word
    // would be forwarded as if it were a Value. Lowering hands every site its
    // own number; this is what makes that discipline a checked fact.
    {
        std::unordered_set<uint32_t> methodSites;
        for (const auto& fn : module.functions) {
            for (const auto& block : fn.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.op == Op::MethodCall || inst.op == Op::MethodCallSpread) {
                        methodSites.insert(inst.icIndex);
                    }
                }
            }
        }
        for (const auto& fn : module.functions) {
            for (const auto& block : fn.blocks) {
                for (const auto& inst : block.instructions) {
                    if ((inst.op == Op::PropGet || inst.op == Op::PropSet) &&
                        methodSites.count(inst.icIndex)) {
                        diags.error(Span{}, "Function " + fn.name + ": " + opName(inst.op) +
                                                " shares inline-cache site " +
                                                std::to_string(inst.icIndex) +
                                                " with a method call");
                        return false;
                    }
                }
            }
        }
    }
    return verifyModuleProofs(module, diags);
}

}  // namespace bronze::il
