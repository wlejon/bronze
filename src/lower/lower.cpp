#include "lower/lower.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "ast/assigned.h"
#include "ast/queries.h"
#include "lower/guard_region.h"
#include "lower/lowerer.h"
#include "support/timings.h"

namespace bronze::lower {

std::optional<il::Module> Lowerer::lower() {
    ilModule_.name = astModule_.name;

    std::vector<const ast::Stmt*> topLevelStmts;
    for (const auto& stmtPtr : astModule_.body) {
        if (const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmtPtr.get())) {
            if (functionIndices_.contains(fnDecl->name)) {
                diags_.error(fnDecl->span, "duplicate function name: " + fnDecl->name);
                return std::nullopt;
            }
            il::Function fn;
            fn.name = fnDecl->name;
            fn.isExported = fnDecl->isExported;
            // A body that mentions `this` gets the synthetic receiver parameter
            // ahead of its source parameters; every call site supplies it,
            // direct ones included, so this costs nothing to functions that do
            // not use it.
            if (ast::usesThis(fnDecl->params, fnDecl->body)) {
                fn.needsThis = true;
                fn.params.push_back({"__this", il::Type::Dynamic});
            }
            // `arguments` is the caller's real argument list, which only the
            // call wrapper can see — so it arrives as a synthetic parameter
            // too, and the function stops being a direct-call target.
            if (ast::usesArguments(fnDecl->params, fnDecl->body)) {
                fn.needsArguments = true;
                fn.params.push_back({"__arguments", il::Type::Dynamic});
            }
            // Every source parameter starts on the uniform dynamic convention
            // and stays there unless a PROOF moves it. An annotation is not a
            // proof: it is checked below, after the proof is in hand, and it
            // never appears on the left of an assignment to a type.
            for (const auto& param : fnDecl->params) {
                // A pattern parameter binds names but has none of its own; the
                // IL still needs one slot, and naming it for its position is
                // what makes an IL dump of it readable.
                fn.params.push_back({param.pattern
                                         ? "__pattern" + std::to_string(fn.params.size())
                                         : param.name,
                                     il::Type::Dynamic});
            }
            applyParamShape(fnDecl->params, fn);
            // 10.2.9: a declaration's `name` is the name it declares. The
            // `length` is `requiredArgs`, which `applyParamShape` has just set
            // to 15.1.5's ExpectedArgumentCount.
            fn.nameKeyIndex = getKeyConstantIndex(fnDecl->name);
            fn.returnType = il::Type::Void;
            // The module function index: the position among the top-level
            // declarations, which is exactly how inference numbers them.
            const uint32_t moduleFnIndex = static_cast<uint32_t>(ilModule_.functions.size());
            if (!applyProvenSignature(*fnDecl, moduleFnIndex, fn)) return std::nullopt;
            // Now the hints, against the proof that just typed the signature.
            // Source order — parameters left to right, then the return —
            // and the enclosing loop is in source order too, so the warning
            // stream is deterministic.
            for (size_t i = 0; i < fnDecl->params.size(); ++i) {
                if (!checkAnnotation(fnDecl->params[i].typeAnnotation, fnDecl->span,
                                     fnDecl->params[i].name, provenParamType(moduleFnIndex, i))) {
                    return std::nullopt;
                }
            }
            // A return annotation is reported on the function's own name:
            // there is no other name for the position.
            if (!checkAnnotation(fnDecl->returnType, fnDecl->span, fnDecl->name,
                                 provenReturnType(moduleFnIndex))) {
                return std::nullopt;
            }
            // Every module function's return type is settled here, before
            // ANY body is lowered, because it is part of the calling
            // convention: `lowerCall` reads it off this entry, and for
            // mutual recursion it reads it while the callee's body is still
            // unlowered. Left to `lowerReturnStmt` to discover from the
            // first `return` it happens to reach, that read finds `Void` —
            // "returns nothing" — and the caller emits `ret` of a value
            // that does not exist, which the IL verifier rejects.
            //
            // Inference may already have pinned something better (an f64
            // return for a direct-callable function). What is left is a
            // function whose callers are unknown, and the sound convention
            // for those is the uniform dynamic one: `dynamic` if the body
            // can return a value at all, `void` if it demonstrably cannot.
            // That also retires first-return-wins, which was a live
            // miscompile of its own — `return 1; ... return "s"` unboxed a
            // string pointer as a double.
            if (fn.returnType == il::Type::Void && ast::returnsAValue(fnDecl->body)) {
                fn.returnType = il::Type::Dynamic;
            }
            // A generator function returns the GENERATOR OBJECT, whatever its
            // body returns — a `return` in there is the final result's `value`
            // and never this call's (27.5.1.2). So the factory returns a value
            // even when the body has no `return` at all, which is the case the
            // two rules above cannot reach: `returnsAValue` is false for
            // `function* g() { yield 1; }` and the proof for a generator is
            // `dynamic`, so without this the factory would be typed Void and
            // its `ret` of the generator object would not verify.
            //
            // This states the convention; it does not correct inference. That
            // it once had to — inference read the body as an ordinary one and
            // proved `number` for `function* f() { return 2; }`, which every
            // caller then unboxed — is why `analyzeFunction` now takes
            // `isGenerator` too. A convention only one side of the compiler
            // knows is not a convention.
            if (fnDecl->isGenerator) fn.returnType = il::Type::Dynamic;
            // An async function returns the PROMISE, whatever its body
            // returns — the body's `return v` is what the promise settles
            // with (27.7.5.2), never this call's value. The same convention
            // statement the generator line above makes, for the same reason:
            // `returnsAValue` is false for `async function f() { await g(); }`
            // and the factory still returns a value.
            if (fnDecl->isAsync) fn.returnType = il::Type::Dynamic;
            fn.fnFlags = functionObjectFlags(ast::FunctionKind::Normal, fnDecl->isGenerator,
                                             fnDecl->isAsync);
            // Last, because a pin is a promise about a convention every proof
            // and every convention statement above has already had its say on.
            if (!applySignaturePins(fnDecl->params, fnDecl->span, fn)) return std::nullopt;
            fn.sourceFile = fnDecl->span.file;
            fn.sourceBegin = fnDecl->span.begin;
            fn.sourceEnd = fnDecl->span.end;
            fn.valueCount = static_cast<uint32_t>(fn.params.size());
            functionIndices_[fn.name] = moduleFnIndex;
            ilModule_.functions.push_back(std::move(fn));
        } else if (dynamic_cast<const ast::ExportNamesDecl*>(stmtPtr.get())) {
            // An export clause names bindings and evaluates nothing (ECMA-262
            // 16.2.3), so it is not a top-level statement. Counting it as one
            // would give a module whose whole content is exported function
            // declarations an empty `main` it never had — the IL of every
            // such file would change with nothing running in it.
            continue;
        } else {
            topLevelStmts.push_back(stmtPtr.get());
        }
    }

    // Whether the top level lowers as one body or as segments, decided from
    // its SOURCE size — the only measure available before anything lowers.
    // The threshold is far above any handwritten program: it exists for
    // bundles, where one `main` was the floor under parallel object emission.
    {
        size_t topLevelBytes = 0;
        for (const ast::Stmt* s : topLevelStmts) topLevelBytes += s->span.end - s->span.begin;
        constexpr size_t kSegmentSourceBytes = 128 * 1024;
        segmentTopLevel_ = topLevelBytes >= kSegmentSourceBytes;
        if (support::timingsEnabled()) {
            std::fprintf(stderr, "  segment? stmts=%zu bytes=%zu -> %d\n", topLevelStmts.size(),
                         topLevelBytes, static_cast<int>(segmentTopLevel_));
        }
    }

    // The module scope's environment layout, before any body is lowered: a
    // top-level function declaration resolves module-level names against it.
    planModuleEnv(topLevelStmts);

    size_t fnIndex = 0;
    for (const auto& stmtPtr : astModule_.body) {
        if (const auto* fnDecl = dynamic_cast<const ast::FunctionDecl*>(stmtPtr.get())) {
            // Lowered in place: a recursive call reads this very entry
            // for its arity and its still-being-inferred return type.
            // Safe only because Module::functions is reference-stable —
            // lowering this body can append nested closures to it.
            // A top-level declaration does not go through `lowerClosure`,
            // so its mode is set here — from the node, for the same reason.
            const bool outerStrict = strictCode_;
            strictCode_ = fnDecl->strict;
            const bool ok = lowerFunctionBody(*fnDecl, ilModule_.functions[fnIndex]);
            strictCode_ = outerStrict;
            if (!ok) return std::nullopt;
            fnIndex++;
        }
    }

    if (!topLevelStmts.empty()) {
        il::Function mainFn;
        mainFn.name = "main";
        // Nothing above `main` can catch, so its unwind path reports and exits
        // rather than returning. No handler BLOCK, and so no IL at all in a
        // program that never throws — which is what keeps every pinned dump of
        // one byte-identical.
        mainFn.isEntryPoint = true;
        mainFn.returnType = il::Type::Void;
        mainFn.valueCount = 0;
        mainFn.blocks.push_back(il::Block{.id = 0});
        currentBlockIdx_ = 0;

        // The module top level is a function body like any other, and every
        // function body starts from an empty scope. This reset is not
        // housekeeping: without it the top level inherited the LAST module
        // function's bindings, so a top-level `let` whose name matched one of
        // that function's locals was rejected as a redeclaration, and a read of
        // such a name resolved to a binding whose SSA value id names an
        // unrelated instruction in `main`.
        varBindings_.clear();
        activeVarMap_.clear();
        functionRefMap_.clear();
        currentScopeDepth_ = 0;
        varDeclCounter_ = 0;
        jumpStack_.clear();
        scopeHasEnv_.clear();
        currentEnvValue_ = il::kNoValue;
        currentThisValue_ = il::kNoValue;
        currentFunctionIsArrow_ = false;
        // `main` is the Script's own code, so it takes the Script's mode.
        strictCode_ = astModule_.strict;
        functionEnvBase_ = 0;
        functionEnvScope_ = SIZE_MAX;

        openModuleEnv(topLevelStmts, mainFn);

        const auto topLevelVars = ast::getTopLevelVarDeclarations(topLevelStmts);
        const auto allHoistedVars = ast::getHoistedVarDeclarations(topLevelStmts);
        for (const auto& varName : topLevelVars) {
            if (activeVarMap_.find(varName) == activeVarMap_.end()) {
                il::ValueId undefVal = emitConstUndefined(mainFn);
                if (!declareVariable(varName, il::Type::Dynamic, /*isConst=*/false, /*isLet=*/false,
                                     /*isVar=*/true, /*isInitialized=*/true, undefVal, Span{})) {
                    return std::nullopt;
                }
                VarBinding& b = varBindings_[activeVarMap_[varName]];
                if (b.inEnv) {
                    emitEnvSet(envDepthOf(b.envScopeIndex), b.envSlot,
                               Value{undefVal, il::Type::Dynamic}, mainFn);
                }
            }
        }
        functionVarNames_.clear();
        for (const auto& v : allHoistedVars) {
            if (std::find(topLevelVars.begin(), topLevelVars.end(), v) == topLevelVars.end()) {
                functionVarNames_.push_back(v);
            }
        }

        if (segmentTopLevel_) {
            if (!lowerTopLevelSegments(topLevelStmts, mainFn)) return std::nullopt;
        } else if (!lowerStmtList(topLevelStmts, mainFn)) {
            return std::nullopt;
        }
        auto& insts = mainFn.blocks[currentBlockIdx_].instructions;
        if (insts.empty() || !il::isTerminator(insts.back().op)) {
            il::Instruction retInst;
            retInst.op = il::Op::Ret;
            retInst.type = il::Type::Void;
            emitInst(mainFn, retInst);
        }
        ilModule_.functions.push_back(std::move(mainFn));
    }

    if (diags_.hasErrors()) return std::nullopt;
    // The census's one WHOLE-MODULE pass, and it must run before the key
    // constants are frozen because it can add rows (lower_census.cpp): a
    // `param`/`return` owner spelling that would govern two different IL
    // functions is refused, and the set of IL function names is not complete
    // until every body is lowered.
    ilModule_.censusOutPath = censusOutPath_;
    refuseAmbiguousCensusOwners();
    // BEFORE the key pool is snapshotted: it interns the field names it emits.
    buildSlotReprTable();
    ilModule_.keyConstants.resize(keyConstants_.size());
    for (const auto& entry : keyConstants_) {
        ilModule_.keyConstants[entry.second] = entry.first;
    }
    // The site counter is now the size of a real allocation: the backend emits
    // exactly this many IC entries as a global array in the object file.
    ilModule_.icSiteCount = icSiteCounter_;
    ilModule_.templateSiteCount = templateSiteCounter_;
    ilModule_.staticSiteCount = staticSiteCounter_;
    // The one pass that needs the module WHOLE: a method-call site and the
    // class body that answers it are lowered in the opposite order to the one
    // resolution wants (lowerer.h, `methodCallSites_`).
    resolveDirectMethodTargets();
    reportClassLayouts();
    // Every file, whole, in SourceSet order — the order `Span::file` indexes.
    // Files a function was never lowered from are carried too: the alternative
    // is a remap, and the only file that can be present without a function in
    // it is one that declared none, which costs nothing the backend does not
    // then drop (it emits a blob only for a file some function names).
    if (sources_) {
        ilModule_.sourceTexts.reserve(sources_->size());
        for (size_t i = 0; i < sources_->size(); ++i) {
            ilModule_.sourceTexts.emplace_back(sources_->at(static_cast<uint16_t>(i)).text());
        }
    }
    // The one IL -> IL pass, and it runs LAST: it reads the finished module and
    // hands out no module-global identity of its own, so everything above —
    // inline-cache indices, static-slot cells, template sites, the census table
    // — is already frozen and a copied instruction shares the site the original
    // named. It is also why `bronze il` shows the transformed IL: `lowerModule`
    // is the single funnel both `runIl` and `runBuild` come through.
    {
        GuardRegionStats stats;
        applyGuardedRegions(ilModule_, &stats);
        guardRegionStatsReport(stats);
    }
    return ilModule_;
}

// One function's environment record: the slots for every name the function
// declares that some nested function references, in a fixed source order. The
// module top level takes exactly the same rule with no parameters, which is why
// this is one function and not two — two copies of "captured names intersected
// with this scope's declarations, then the hoisted vars" is two copies that can
// drift, and a drift between them is a closure reading a slot the declaring
// scope never allocated.
//
// Must run after `currentEnvValue_` names the environment this one chains
// to: `emitEnvCreate` reads it as the parent link.
void Lowerer::enterFunctionEnv(const std::vector<ast::Param>& params,
                               const std::vector<const ast::Stmt*>& body, il::Function& ilFn,
                               bool isGenerator, bool isAsync) {
    // An async body IS a machine body for every rule below: an await is a
    // suspension, so the whole frame takes the generator's treatment, plus
    // one slot the generator does not have (the machine value).
    const bool machineBody = isGenerator || isAsync;
    // Which of this function's own variables must live in an environment
    // record. The environment STACK is not cleared with it: enclosing
    // scopes' environments are how this function's free variables resolve.
    capturedNames_ = ast::getCapturedNames(body);
    functionRefMap_.clear();
    // The second reason a binding cannot be in SSA. Unioned here and nowhere
    // else, so `capturedNames_` keeps meaning exactly "a closure can reach it"
    // for the one consumer that needs that narrower question.
    memoryNames_ = capturedNames_;
    for (auto& name : ast::getTryAssignedNames(body)) memoryNames_.insert(std::move(name));
    // The third reason, and the one that has nothing to do with where a value
    // can be read FROM: a lexical binding that can be read before its
    // declaration has run needs somewhere to hold the uninitialized marker, and
    // SSA has no room for a value that is not a value.
    for (auto& name : ast::getTdzExposedNames(body)) memoryNames_.insert(std::move(name));
    // The fourth, and the one that takes the whole frame rather than a name at a
    // time: a generator suspends, and an edge from its resume dispatch defines
    // no SSA value at all. `this` and `arguments` join it unconditionally,
    // because the resume function reads both through the record exactly as an
    // arrow does and neither is a declaration anything above would find.
    if (machineBody) {
        for (auto& name : ast::getGeneratorFrameNames(body)) memoryNames_.insert(std::move(name));
        // The parameters too, which no body query can see: they are bound by
        // the generator function and read by the resume function, so they cross
        // every suspension there is.
        for (const auto& p : params) {
            if (p.pattern) {
                for (auto& bound : ast::patternBoundNames(*p.pattern)) {
                    memoryNames_.insert(std::move(bound));
                }
            } else if (!p.name.empty()) {
                memoryNames_.insert(p.name);
            }
        }
        if (ast::usesThis(params, body)) memoryNames_.insert("this");
        if (ilFn.needsArguments) memoryNames_.insert("arguments");
    }
    functionEnvBase_ = envScopes_.size();
    functionEnvScope_ = SIZE_MAX;

    std::vector<std::string> slots;
    auto addSlot = [&](const std::string& slotName) {
        if (!memoryNames_.contains(slotName)) return;
        if (std::find(slots.begin(), slots.end(), slotName) != slots.end()) return;
        slots.push_back(slotName);
    };
    // The machine's own, ahead of every binding so that a dump of a generator's
    // frame reads with them first. Their names hold no source identifier, so
    // nothing can collide with them.
    if (machineBody) {
        slots.emplace_back(generatorStateSlotName());
        slots.emplace_back(generatorEnvSlotName());
        // The third only where a `yield*` needs it. A generator with no
        // delegation gets the two-slot frame it always had, so its IL is
        // unchanged and a reader of one is not left asking what the empty
        // third word is for.
        if (isGenerator && ast::hasDelegating(ast::yieldFormsIn(body))) {
            slots.emplace_back(generatorIterSlotName());
        }
        // An async body's third slot instead holds the MACHINE: the object
        // the await sites subscribe resumption through. Written once by the
        // factory (lowerAsyncTail), read at every await.
        if (isAsync) slots.emplace_back(asyncMachineSlotName());
        // And one anonymous slot per level of nested `for-of`/`for-in` whose
        // body suspends. The iteration record such a loop is stepping is the
        // one live thing in a machine body that no NAME reaches — the source
        // never spelled it — so `getGeneratorFrameNames` above cannot put it
        // here and this asks for it separately.
        const uint32_t iterLoops = ast::maxSuspendingIterationDepth(body);
        for (uint32_t depth = 0; depth < iterLoops; ++depth) {
            slots.emplace_back(loopIterSlotName(depth));
        }
    }
    // Parameters first, then the body's own let/const/function declarations,
    // then `var`s hoisted from anywhere below (they are function-scoped
    // wherever they are written, so they belong to this record and not to the
    // block that spells them). A pattern parameter has no name of its own; the
    // names it binds are the ones a closure can capture.
    for (const auto& p : params) {
        if (p.pattern) {
            for (const auto& bound : ast::patternBoundNames(*p.pattern)) addSlot(bound);
        } else {
            addSlot(p.name);
        }
    }
    for (const auto& declName : ast::getScopeDeclarations(body)) addSlot(declName);
    for (const auto& varName : ast::getHoistedVarDeclarations(body)) addSlot(varName);
    // The receiver, when an arrow in this body reads it. It is not a
    // declaration, so nothing above finds it.
    //
    // Never for an ARROW's own record, however deeply its body reads `this`:
    // an arrow binds no receiver, so a slot here would be one nothing can
    // fill — the entry copy below has no `__this` to take — and an arrow
    // nested inside it would find that empty slot first and read `undefined`
    // instead of walking on to the function that does bind one.
    if (!currentFunctionIsArrow_) addSlot("this");
    // The arguments object, when an arrow in this body reads it. Same rule as
    // the receiver above and for the same reason: never for an arrow's own
    // record, because an arrow binds no `arguments` and a slot here would be
    // one nothing can fill — and one a more deeply nested arrow would find
    // first, reading `undefined` instead of walking on to the function that
    // does bind one.
    if (!currentFunctionIsArrow_ && ilFn.needsArguments) addSlot("arguments");
    if (slots.empty()) return;

    EnvScopeInfo info;
    // The frame's own downward link is `gen.env`, placed at a fixed index by the
    // block above so that the machine's two slots read first.
    if (machineBody) info.childSlot = 1;
    for (uint32_t i = 0; i < slots.size(); ++i) info.slotOf[slots[i]] = i;
    info.slotNames = slots;
    info.slotIsLexical.assign(slots.size(), false);
    info.slotIsDefiniteInit.assign(slots.size(), false);
    info.slotImmutable.assign(slots.size(), SlotImmutability::Mutable);
    info.slotIsF64.assign(slots.size(), false);
    // Which of the slots hold Numbers, decided once, here, because this is
    // where the layout is fixed and every reader of the record — the body and
    // every closure written in it — is lowered after it. A machine body is
    // excluded: its frame holds the generator's own state words, and its
    // bindings are re-entered from an edge this analysis does not model.
    if (!machineBody) planEnvSlotNumberTypes(params, body, ilFn.name, info);
    // And which of them hold a function declaration nothing can rebind — the
    // static call plan, lower_scope.cpp. A machine body is excluded for a
    // reason of its own: a resume edge defines no SSA value, so a record there
    // is reached by walking DOWN the child links from the frame, and the plan's
    // claim is about counting parent links UP from the record a closure was
    // created over. The two agree, but nothing here proves that they do, and a
    // generator is not what this exists for.
    if (!machineBody) planStableFunctionSlots(body, &params, info);
    info.envValue = emitEnvCreate(static_cast<uint32_t>(slots.size()), ilFn);
    envScopes_.push_back(std::move(info));
    savedEnvValues_.push_back(currentEnvValue_);
    currentEnvValue_ = envScopes_.back().envValue;
    functionEnvScope_ = envScopes_.size() - 1;
}

// The module scope's slot layout, fixed here so that every consumer agrees on
// it: the module functions, which are lowered next and read it through the
// runtime, and `main`, which is lowered last and creates the record it
// describes.
//
// The capture scan covers the WHOLE module body, top-level function
// declarations included — that is the fix. `lower()` splits those
// declarations out into module functions, so scanning only what is left meant
// a module-level `let` read by a top-level function was not captured by
// anything as far as this pass could see, got no slot, and the read reported
// `undefined variable` for a binding written three lines above it.
void Lowerer::planModuleEnv(const std::vector<const ast::Stmt*>& topLevelStmts) {
    // Deliberately a LOCAL set, not `capturedNames_`. The two answer
    // different questions and the difference is not cosmetic:
    //
    //   - this one asks what the module scope's record must hold, so it must
    //     see the top-level function declarations' bodies;
    //   - `capturedNames_` asks which of the scope's BLOCK-level bindings a
    //     closure can reach, and it drives the for-header capture diagnostic,
    //     which is a hard error.
    //
    // Widening `capturedNames_` to this set makes a top-level
    // `for (let i = 0; ...)` illegal because some unrelated function's body
    // also happens to declare an `i`. A block-level binding is not a module
    // declaration, so it can never be in the record anyway, and nothing is
    // lost by keeping the sets apart.
    std::unordered_set<std::string> moduleCaptures = ast::getCapturedNames(astModule_.body);
    // A module-level binding assigned inside a top-level `try` needs a slot
    // in the module record for the same reason a captured one does, and the
    // widening is safe HERE for the reason it is not safe for
    // `capturedNames_`: the record holds only the top level's own
    // declarations, and a for-header binding is not one of them.
    for (auto& name : ast::getTryAssignedNames(astModule_.body)) {
        moduleCaptures.insert(std::move(name));
    }
    // And a top-level lexical binding read above its declaration, for the
    // reason `enterFunctionEnv` widens the function's set: the marker needs a
    // slot. Widened HERE and not into `capturedNames_`, for the reason above.
    for (auto& name : ast::getTdzExposedNames(topLevelStmts)) {
        moduleCaptures.insert(std::move(name));
    }

    auto addSlot = [&](const std::string& name) {
        // A segmented top level gives EVERY declaration a slot, captured or
        // not: a binding declared in one segment and read in another crosses
        // a function boundary exactly as a captured one crosses a closure's,
        // and the slot is what carries it. This is the one cost of
        // segmentation — top-level code reads its own bindings through the
        // record instead of SSA — paid by modules big enough that compile
        // time, not top-level throughput, is the binding constraint.
        if (!segmentTopLevel_ && !moduleCaptures.contains(name)) return;
        if (std::find(moduleEnvSlots_.begin(), moduleEnvSlots_.end(), name) !=
            moduleEnvSlots_.end()) {
            return;
        }
        moduleEnvSlots_.push_back(name);
    };
    // Only the top level's OWN declarations. A top-level function
    // declaration is deliberately absent: it is a module symbol resolved
    // through `functionIndices_`, and a slot for it would shadow that symbol
    // with a slot nothing ever writes — every call to it would find
    // undefined.
    for (const auto& name : ast::getScopeDeclarations(topLevelStmts)) addSlot(name);
    for (const auto& name : ast::getHoistedVarDeclarations(topLevelStmts)) addSlot(name);
    addSlot("this");
    if (moduleEnvSlots_.empty()) return;

    EnvScopeInfo info;
    for (uint32_t i = 0; i < moduleEnvSlots_.size(); ++i) info.slotOf[moduleEnvSlots_[i]] = i;
    info.slotNames = moduleEnvSlots_;
    // Which of them are lexical is settled HERE and not in `openModuleEnv`,
    // for the same reason the layout is: a module function is lowered before
    // `main` exists, and a read of a module-level `const` from inside one has
    // to know it is reading a slot that can be uninitialized.
    info.slotIsLexical.assign(moduleEnvSlots_.size(), false);
    info.slotIsDefiniteInit.assign(moduleEnvSlots_.size(), false);
    info.slotImmutable.assign(moduleEnvSlots_.size(), SlotImmutability::Mutable);
    for (const auto& name : ast::getLexicalDeclarations(topLevelStmts)) {
        auto slot = info.slotOf.find(name);
        if (slot != info.slotOf.end()) info.slotIsLexical[slot->second] = true;
    }
    // And which of them no read can catch uninitialized, settled here for the
    // same reason: the module function that reads one is lowered before `main`
    // and has to know then whether the read carries a check.
    if (!definiteInitDisabled()) {
        for (const auto& name : ast::getDefinitelyAssignedLexicalNames(astModule_.body)) {
            auto slot = info.slotOf.find(name);
            if (slot != info.slotOf.end()) info.slotIsDefiniteInit[slot->second] = true;
        }
    }
    if (segmentTopLevel_) {
        // One body refuses a write to a top-level `const` at COMPILE time,
        // because the binding is in scope at the write. A later segment
        // resolves the name through the record like a module function does,
        // where no compile-time check exists — so the slot is marked
        // immutable and the write takes 9.1.1.1.5's runtime arm instead,
        // which in strict code (every module) is the TypeError the spec
        // gives an assignment to `const`. Only simple-name declarations are
        // marked: a pattern-bound const keeps the module-function semantics
        // writes to it always had.
        for (const ast::Stmt* s : topLevelStmts) {
            const auto* vd = dynamic_cast<const ast::VarDecl*>(s);
            if (!vd || !vd->isConst || vd->name.empty()) continue;
            auto slot = info.slotOf.find(vd->name);
            if (slot != info.slotOf.end()) {
                info.slotImmutable[slot->second] = SlotImmutability::Throws;
            }
        }
    }
    // No value yet, and that is the point: the record is created by `main`,
    // which does not exist until every module function has been lowered.
    // Readers in those functions load it from the runtime instead.
    info.envValue = il::kNoValue;
    envScopes_.push_back(std::move(info));
    moduleEnvScope_ = envScopes_.size() - 1;
}

// The top level of a LARGE module, lowered as segments: `main` still creates
// and publishes the module record, initializes the hoisted vars and opens the
// lexical dead zones, but the statements themselves lower into a chain of
// `main.seg<K>` functions that `main` calls in order. The name carries a dot
// so no JS identifier can collide with it.
//
// Why: object emission runs partitions of a large module in parallel
// (llvm_backend.cpp), and a function cannot be partitioned — a bundle's top
// level was 6–10% of the whole module in ONE function, optimizing ~2x slower
// per instruction than the same volume as small functions, so that one
// function was the wall-clock floor of the whole compile.
//
// A segment lowers exactly as a MODULE FUNCTION does: fresh per-function
// state, the module record loaded from the runtime, every module-level name
// reached through its slot — planModuleEnv gave every top-level declaration a
// slot when it decided to segment, which is what makes any statement boundary
// a safe cut: no SSA value crosses segments. Cuts are taken only between
// top-level statements, where no block scope is open.
//
// A top-level `throw` makes the current block terminated; one body DROPS the
// statements after it (lowerStmtList says why that is the language's answer),
// so the segmented form drops them too rather than lowering segments no call
// would ever reach — `main`'s pending-exception check after each call is what
// makes the calls after a throwing segment unreachable at run time.
bool Lowerer::lowerTopLevelSegments(const std::vector<const ast::Stmt*>& topLevelStmts,
                                    il::Function& mainFn) {
    // Sized in IL instructions — the only size lowering can see. One IL
    // instruction expands to ~50 LLVM instructions (measured on the three.js
    // bundle: the inline IC fast paths are most of it), so 800 IL is roughly
    // a 40k-instruction function: big enough that per-function pass overhead
    // stays noise, small enough that emission partitions balance dozens of
    // them evenly.
    constexpr size_t kSegmentIlInsts = 800;
    const size_t mainBlockIdx = currentBlockIdx_;
    size_t stmtIdx = 0;
    unsigned segNo = 0;
    while (stmtIdx < topLevelStmts.size()) {
        il::Function segFn;
        segFn.name = "main.seg" + std::to_string(segNo++);
        segFn.returnType = il::Type::Void;
        segFn.isStrict = strictCode_;
        segFn.blocks.push_back(il::Block{.id = 0});

        // The reset `main` itself got above, less what stays module-wide:
        // functionVarNames_ (a `var` nested in any top-level statement is
        // module-scoped wherever it is written), the env layout, strict mode.
        varBindings_.clear();
        activeVarMap_.clear();
        currentScopeDepth_ = 0;
        varDeclCounter_ = 0;
        jumpStack_.clear();
        scopeHasEnv_.clear();
        currentBlockIdx_ = 0;
        currentThisValue_ = il::kNoValue;
        functionEnvBase_ = 0;
        functionEnvScope_ = moduleEnvScope_;
        // The module record, loaded the way every module function loads it.
        currentEnvValue_ =
            moduleEnvScope_ != SIZE_MAX ? emitModuleEnvGet(segFn) : il::kNoValue;

        while (stmtIdx < topLevelStmts.size()) {
            if (!lowerStmt(*topLevelStmts[stmtIdx], segFn)) return false;
            ++stmtIdx;
            if (currentBlockIsTerminated(segFn)) {
                stmtIdx = topLevelStmts.size();
                break;
            }
            size_t segInsts = 0;
            for (const auto& b : segFn.blocks) segInsts += b.instructions.size();
            if (segInsts >= kSegmentIlInsts) break;
        }

        if (!currentBlockIsTerminated(segFn)) {
            il::Instruction retInst;
            retInst.op = il::Op::Ret;
            retInst.type = il::Type::Void;
            emitInst(segFn, retInst);
        }

        // Appended AFTER the closures the segment's own statements appended,
        // so the index is taken here, not before the statements lowered.
        const uint32_t segIndex = static_cast<uint32_t>(ilModule_.functions.size());
        ilModule_.functions.push_back(std::move(segFn));

        currentBlockIdx_ = mainBlockIdx;
        il::Instruction call;
        call.op = il::Op::Call;
        call.type = il::Type::Void;
        call.result = il::kNoValue;
        call.calleeIndex = segIndex;
        emitInst(mainFn, call);
    }
    return true;
}

// `main` creates the module scope's record and publishes it, ahead of every
// statement — including the hoisted closures, which capture it as their
// parent environment.
void Lowerer::openModuleEnv(const std::vector<const ast::Stmt*>& topLevelStmts,
                            il::Function& mainFn) {
    // The narrow set: what closures WRITTEN at top level capture. See
    // planModuleEnv for why this is not the set the record's layout came
    // from.
    capturedNames_ = ast::getCapturedNames(topLevelStmts);
    memoryNames_ = capturedNames_;
    for (auto& name : ast::getTryAssignedNames(topLevelStmts)) memoryNames_.insert(std::move(name));
    for (auto& name : ast::getTdzExposedNames(topLevelStmts)) memoryNames_.insert(std::move(name));
    if (moduleEnvScope_ == SIZE_MAX) return;
    envScopes_[moduleEnvScope_].envValue =
        emitEnvCreate(static_cast<uint32_t>(moduleEnvSlots_.size()), mainFn);
    savedEnvValues_.push_back(currentEnvValue_);
    currentEnvValue_ = envScopes_[moduleEnvScope_].envValue;
    functionEnvScope_ = moduleEnvScope_;
    emitModuleEnvSet(currentEnvValue_, mainFn);
    // Ahead of every top-level statement, and ahead of the hoisted closures
    // that capture this record: 16.2.1.6.4 instantiates the module's lexical
    // bindings — uninitialized — before any of its body runs, and a function
    // called during that window reading one is the ReferenceError this puts
    // there.
    // The WHOLE module body and not `topLevelStmts`: a top-level `function` is
    // split out of that list into a module function, and those declarations are
    // precisely the closures over this record that 16.2.1.6.4 instantiates
    // before any of it runs — so the scan has to see their names to refuse a
    // statement that can call one.
    openLexicalBindings(moduleEnvScope_, ast::getLexicalDeclarations(topLevelStmts),
                        ast::getDefinitelyAssignedLexicalNames(astModule_.body),
                        ast::getConstDeclarations(topLevelStmts), mainFn);
}

// Does this module function need the module scope's record at entry? An
// over-approximation on purpose: it costs one load in a function that turns
// out not to need it, where guessing the other way would leave a name
// unresolved. `getReferencedNames` descends into nested functions, which is
// what makes it right for a body whose own statements mention nothing but
// whose closures do.
bool Lowerer::referencesModuleEnv(const std::vector<ast::Param>& params,
                                  const std::vector<ast::StmtPtr>& body) const {
    if (moduleEnvScope_ == SIZE_MAX) return false;
    auto referenced = ast::getReferencedNames(body);
    // A parameter DEFAULT is code in this function that the body does not
    // contain, and it can read a module-level binding like any other
    // expression — `function bump(v = ++calls)`. Left out, the record is
    // never loaded and the name resolves to nothing at all.
    for (auto& name : ast::getParamReferencedNames(params)) referenced.insert(std::move(name));
    for (const auto& slot : moduleEnvSlots_) {
        if (referenced.contains(slot)) return true;
    }
    return false;
}

bool Lowerer::lowerFunctionBody(const std::vector<ast::Param>& params,
                                const std::vector<ast::StmtPtr>& body, il::Function& ilFn,
                                bool isGenerator, bool isAsync) {
    // Bodies lower one at a time (the state resets below assume it), so a
    // plain pointer is the whole bookkeeping the typed-element binding scan
    // needs.
    currentBodyStmts_ = &body;
    // Which of this body's own nested declarations have parameters every call
    // site proves to be Numbers (lower_scope.cpp). Decided BEFORE any statement
    // of the body is lowered, because the first thing lowering does with a
    // `function f() {}` statement is build `f`'s IL skeleton — parameter types
    // and all — and the whole point of the plan is to be part of it.
    planClosureParamNumbers(params, body);
    ilFn.blocks.push_back(il::Block{.id = 0});
    ilFn.isStrict = strictCode_;
    ilFn.isGenerator = isGenerator;
    currentBlockIdx_ = 0;
    varBindings_.clear();
    activeVarMap_.clear();
    currentScopeDepth_ = 0;
    varDeclCounter_ = 0;
    jumpStack_.clear();
    scopeHasEnv_.clear();
    functionVarNames_ = ast::getHoistedVarDeclarations(body);

    // Synthetic parameters lead: [__env?][__this?] then source params.
    const uint32_t paramBase = static_cast<uint32_t>(ilFn.firstSourceParam());
    if (ilFn.needsEnv) {
        // A closure: its environment arrives as the first parameter, and the
        // chain from there already reaches every enclosing scope including the
        // module's.
        currentEnvValue_ = 0;
    } else {
        // A module function. It has no environment parameter and never will —
        // that is what keeps it a direct-call target — so the one scope it can
        // still need, the module's, is loaded from the runtime.
        currentEnvValue_ =
            referencesModuleEnv(params, body) ? emitModuleEnvGet(ilFn) : il::kNoValue;
    }
    currentThisValue_ = ilFn.needsThis ? (ilFn.needsEnv ? 1u : 0u) : il::kNoValue;

    std::vector<const ast::Stmt*> stmts;
    stmts.reserve(body.size());
    for (const auto& s : body) stmts.push_back(s.get());
    enterFunctionEnv(params, stmts, ilFn, isGenerator, isAsync);

    // An arrow in this body reads the receiver out of the environment, so
    // the receiver has to be IN it: copy `__this` across on entry, once,
    // exactly as a captured parameter is copied below. Undefined where
    // there is no receiver, which is what `this` means at module level.
    if (functionEnvScope_ != SIZE_MAX && envScopes_[functionEnvScope_].slotOf.contains("this")) {
        Value thisVal{currentThisValue_, il::Type::Dynamic};
        if (currentThisValue_ == il::kNoValue) {
            il::ValueId undef = ilFn.valueCount++;
            il::Instruction undefInst;
            undefInst.op = il::Op::ConstUndefined;
            undefInst.type = il::Type::Dynamic;
            undefInst.result = undef;
            emitInst(ilFn, undefInst);
            thisVal = Value{undef, il::Type::Dynamic};
        }
        emitEnvSet(envDepthOf(functionEnvScope_), envScopes_[functionEnvScope_].slotOf.at("this"),
                   thisVal, ilFn);
    }

    // The arguments object is a BINDING named `arguments`, not a keyword:
    // that is what makes an arrow in this body see it through the ordinary
    // capture machinery, and what makes a `let arguments` shadow it without a
    // rule of its own. Declared before the parameters so that any real
    // declaration of the name — which `ast::usesArguments` already refuses to
    // create this for — would shadow rather than collide with it.
    if (ilFn.needsArguments) {
        const il::ValueId argsVal = static_cast<il::ValueId>(ilFn.firstSourceParam() - 1);
        if (functionEnvScope_ != SIZE_MAX &&
            envScopes_[functionEnvScope_].slotOf.contains("arguments")) {
            emitEnvSet(envDepthOf(functionEnvScope_),
                       envScopes_[functionEnvScope_].slotOf.at("arguments"),
                       Value{argsVal, il::Type::Dynamic}, ilFn);
        }
        if (!declareVariable("arguments", il::Type::Dynamic, /*isConst=*/false, /*isLet=*/false,
                             /*isVar=*/true, /*isInitialized=*/true, argsVal, Span{})) {
            return false;
        }
    }

    if (!lowerParamBindings(params, paramBase, ilFn)) return false;

    const auto allHoistedVars = ast::getHoistedVarDeclarations(body);
    for (const auto& varName : allHoistedVars) {
        if (activeVarMap_.find(varName) == activeVarMap_.end()) {
            il::ValueId undefVal = emitConstUndefined(ilFn);
            if (!declareVariable(varName, il::Type::Dynamic, /*isConst=*/false, /*isLet=*/false,
                                 /*isVar=*/true, /*isInitialized=*/true, undefVal, Span{})) {
                return false;
            }
            VarBinding& b = varBindings_[activeVarMap_[varName]];
            if (b.inEnv) {
                emitEnvSet(envDepthOf(b.envScopeIndex), b.envSlot,
                           Value{undefVal, il::Type::Dynamic}, ilFn);
            }
        }
    }
    functionVarNames_.clear();

    // A generator's body does not run here at all (15.5.3): what is left of
    // this function is to close the resume function over the frame the
    // prologue above has just filled in, and hand back the generator object.
    // Its lexical bindings are opened in the resume function's start block, for
    // the reason recorded there. An async function's tail differs in one
    // fact — 27.7.5.1 runs the body synchronously to the first await — and
    // that fact lives in the runtime driver its tail calls, not here.
    if (isGenerator || isAsync) {
        const bool ok = (isGenerator && isAsync) ? lowerAsyncGeneratorTail(stmts, ilFn)
                        : isGenerator             ? lowerGeneratorTail(stmts, ilFn)
                                                  : lowerAsyncTail(stmts, ilFn);
        if (functionEnvScope_ != SIZE_MAX) {
            envScopes_.pop_back();
            currentEnvValue_ = savedEnvValues_.back();
            savedEnvValues_.pop_back();
        }
        return ok;
    }

    // After the parameters, so that a body that redeclares one is still the
    // redeclaration error it was rather than a parameter slot holding the
    // uninitialized marker; before the statements, because 14.3.1 creates the
    // binding when the scope is entered and the declaration only initializes
    // it.
    if (functionEnvScope_ != SIZE_MAX) {
        openLexicalBindings(functionEnvScope_, ast::getLexicalDeclarations(stmts),
                            ast::getDefinitelyAssignedLexicalNames(stmts, &params),
                            ast::getConstDeclarations(stmts), ilFn);
    }

    if (!lowerStmtList(stmts, ilFn)) return false;

    if (!currentBlockIsTerminated(ilFn)) {
        if (currentBlockIdx_ < ilFn.blocks.size()) {
            // A tail block no edge targets (e.g. the join of an if whose
            // arms both return) is unreachable; give it any well-typed
            // ret. A reachable tail means the function can actually fall
            // off the end, which yields undefined.
            bool reachable = currentBlockIdx_ == 0;
            for (const auto& block : ilFn.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.op == il::Op::Jump || inst.op == il::Op::Branch) {
                        if (inst.target.block == currentBlockIdx_ ||
                            (inst.op == il::Op::Branch && inst.elseTarget.block == currentBlockIdx_)) {
                            reachable = true;
                        }
                    }
                }
            }

            // A body that can REACH its end returns `undefined` there, and a
            // pinned return is refused by the arm below when it does — so a
            // census entry for this function would be a manifest that does not
            // compile. Refused by the site table, which is the only way a
            // static fact reaches a dynamic instrument: reachability is a
            // property of the program, and no run can be asked about it.
            if (censusEnabled() && reachable && ilFn.returnType == il::Type::Dynamic &&
                !ilFn.name.empty() && !ilFn.isGenerator &&
                (ilFn.fnFlags & (BRONZE_ABI_FN_FLAG_GENERATOR | BRONZE_ABI_FN_FLAG_ASYNC)) == 0) {
                addCensusSite("return " + manifestOwnerName(ilFn.name), il::CensusSite::Return,
                              /*refuses=*/true);
            }

            il::Instruction retInst;
            retInst.op = il::Op::Ret;
            if (ilFn.returnType == il::Type::Void) {
                retInst.type = il::Type::Void;
            } else if (ilFn.returnType == il::Type::Dynamic ||
                       (!reachable && ilFn.returnType != il::Type::Str)) {
                Value retVal{il::kNoValue, il::Type::Void};
                if (ilFn.returnType == il::Type::Dynamic) {
                    il::ValueId undefVal = ilFn.valueCount++;
                    il::Instruction constInst;
                    constInst.op = il::Op::ConstUndefined;
                    constInst.type = il::Type::Dynamic;
                    constInst.result = undefVal;
                    emitInst(ilFn, constInst);
                    retVal = Value{undefVal, il::Type::Dynamic};
                } else {
                    il::ValueId dummyVal = ilFn.valueCount++;
                    il::Instruction constInst;
                    constInst.op = ilFn.returnType == il::Type::Bool ? il::Op::ConstBool
                                   : ilFn.returnType == il::Type::I32 ? il::Op::ConstI32
                                                                      : il::Op::ConstF64;
                    constInst.type = ilFn.returnType;
                    constInst.result = dummyVal;
                    emitInst(ilFn, constInst);
                    retVal = Value{dummyVal, ilFn.returnType};
                }
                retInst.type = retVal.type;
                retInst.operands = {retVal.id};
            } else {
                diags_.error(Span{}, "function " + ilFn.name +
                                         " can fall off the end but returns typed " +
                                         il::typeName(ilFn.returnType) +
                                         "; falling off yields undefined");
                return false;
            }
            emitInst(ilFn, retInst);
        }
    }

    if (functionEnvScope_ != SIZE_MAX) {
        envScopes_.pop_back();
        currentEnvValue_ = savedEnvValues_.back();
        savedEnvValues_.pop_back();
    }
    return true;
}

// The return annotation is deliberately not passed down: it is a hint, and the
// callers apply it — `lower()` and `lowerClosure` both check it against the
// proof BEFORE the body is lowered, because the IL return type is part of the
// calling convention.
bool Lowerer::lowerFunctionBody(const ast::FunctionDecl& fnDecl, il::Function& ilFn) {
    return lowerFunctionBody(fnDecl.params, fnDecl.body, ilFn, fnDecl.isGenerator,
                             fnDecl.isAsync);
}

std::optional<il::Module> lowerModule(const ast::Module& astModule, DiagnosticSink& diags,
                                      const types::InferenceResult* inference,
                                      const std::vector<std::string>* hostGlobals,
                                      const SourceSet* sources,
                                      InferStatsCollector* stats,
                                      bool assumeNoBigInt,
                                      const types::PinManifest* pins,
                                      const std::string& censusOutPath) {
    Lowerer lowerer(astModule, diags, inference, hostGlobals, sources, stats,
                    assumeNoBigInt, pins, censusOutPath);
    return lowerer.lower();
}

}  // namespace bronze::lower
