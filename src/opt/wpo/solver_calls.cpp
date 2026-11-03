// The call half of the solver: binding a call's receiver and arguments to a
// callee's parameters, `new`, `super(...)`, direct calls, method calls, and the
// coroutine entry.
//
// The calling convention is the IL's (il.h, Function): the synthetic
// parameters come first — `__env` from the function object, `__this` from the
// call, `__arguments` — then the source parameters, then a rest parameter,
// which receives one array per function whose elements are every argument past
// the fixed ones. A function that reads `arguments` lets every argument escape:
// the object aliases them and goes wherever the body sends it.

#include <algorithm>
#include <iterator>
#include <unordered_map>

#include "opt/wpo/solver.h"
#include "opt/wpo/solver_tags.h"

namespace bronze::wpo {

namespace {

// How many bound / builtin indirections a call follows before it calls the
// rest unknown.
constexpr int kMaxCallDepth = 12;

}  // namespace

void Solver::escapeArgs(const AVal& self, const CallArgs& args, uint32_t reason) {
    escape(self, reason);
    for (const auto& a : args.args) escape(a, reason);
    if (args.spread) escape(*args.spread, reason);
}

AVal Solver::coerceThis(const AVal& self, bool strict) const {
    if (strict) return self;
    // Sloppy code sees the global object for undefined / null and a wrapper
    // for any other primitive: neither is an object the program made.
    AVal r = self;
    if (r.prims != 0) r.topObj = true;
    r.prims = 0;
    return r;
}

ObjId Solver::restArray(uint32_t fn) {
    auto it = restArrays_.find(fn);
    if (it != restArrays_.end()) return it->second;
    const ObjId o = alloc(fn, kTagRest, ObjKind::Array);
    objs_[o].site = UINT32_MAX;
    objs_[o].fn = fn;
    objs_[o].proto.addObj(builtinObj("Array.prototype"));
    restArrays_.emplace(fn, o);
    return o;
}

AVal Solver::bindFunction(uint32_t f, ObjId fnObj, const AVal& self, const CallArgs& args) {
    const il::Function& fn = mod_.functions[f];
    if (fn.isExternal) {
        if (recording_) escapeArgs(self, args, internReason("passed to an external function"));
        return AVal::top();
    }
    if (!recording_) return fns_[f].ret;
    markReachable(f);
    auto bind = [&](size_t index, const AVal& v, bool receiver = false) {
        auto& values = fns_[f].values;
        if (index < values.size() && index < fn.params.size() &&
            mergeInto(values[index], v, fn.params[index].type, receiver)) {
            enqueueUsers(f, static_cast<il::ValueId>(index));
        }
    };
    size_t p = 0;
    if (fn.needsEnv) {
        if (fnObj != kNoObj) {
            depend(fnObj, kKeyEnv);
            const AVal env = objs_[fnObj].env;
            bind(p, env);
        } else {
            bind(p, AVal::top());
        }
        ++p;
    }
    if (fn.needsThis) bind(p++, coerceThis(self, fn.isStrict), true);
    if (fn.needsArguments) {
        bind(p++, AVal::top());
        escapeArgs(AVal(), args, internReason("aliased by an arguments object"));
    }
    const size_t fixed = fn.callerParamCount();
    for (size_t i = 0; i < fixed; ++i) {
        AVal v;
        if (i < args.args.size()) {
            v = args.args[i];
        } else {
            v.addPrims(kUndef);
        }
        if (args.spread) {
            v.join(*args.spread);
            v.addPrims(kUndef);
        }
        bind(p + i, v);
    }
    p += fixed;
    if (fn.hasRestParam) {
        const ObjId rest = restArray(f);
        AVal extra;
        for (size_t i = fixed; i < args.args.size(); ++i) extra.join(args.args[i]);
        if (args.spread) extra.join(*args.spread);
        writeElems(rest, extra);
        bind(p, AVal::obj(rest));
    }
    if (curSite_ != UINT32_MAX) fns_[f].callers.insert(curSite_);
    return fns_[f].ret;
}

void Solver::bindTop(uint32_t f, ObjId fnObj) {
    if (!recording_ || mod_.functions[f].isExternal) return;
    markReachable(f);
    FnState& st = fns_[f];
    static const AVal kTop = AVal::top();
    CallArgs a;
    a.spread = &kTop;
    // A different closure of the same function brings its own environment,
    // so the binding is per function object; the rest is idempotent.
    bindFunction(f, fnObj, kTop, a);
    st.calledTop = true;
    if (!st.retEscapes) {
        st.retEscapes = true;
        escape(st.ret, internReason("returned to unknown code"));
    }
}

AVal Solver::callValue(const AVal& callee, const AVal& self, const CallArgs& args, int depth) {
    AVal out;
    if (depth > kMaxCallDepth) {
        escapeArgs(self, args, internReason("call chain too deep"));
        return AVal::top();
    }
    if (callee.topObj) {
        escapeArgs(self, args, internReason("passed to an unknown function"));
        out.join(AVal::top());
    }
    for (ObjId o : callee.objs) {
        const ObjKind kind = objs_[o].kind;
        if (kind == ObjKind::Function) {
            out.join(bindFunction(objs_[o].fn, o, self, args));
        } else if (kind == ObjKind::Bound) {
            depend(o, kKeyEnv);
            CallArgs merged;
            merged.args = objs_[o].boundArgs;
            for (const auto& a : args.args) merged.args.push_back(a);
            merged.spread = args.spread;
            const AVal target = objs_[o].env;
            const AVal boundThis = objs_[o].boundThis;
            out.join(callValue(target, boundThis, merged, depth + 1));
        } else if (kind == ObjKind::Builtin) {
            const Effect effect = catalog_.def(objs_[o].builtin).call;
            if (effect != Effect::None) out.join(callBuiltin(effect, o, self, args, false, depth));
        }
    }
    return out;
}

AVal Solver::construct(const AVal& callee, const CallArgs& args, int depth) {
    AVal out;
    if (depth > kMaxCallDepth) {
        escapeArgs(AVal(), args, internReason("call chain too deep"));
        return AVal::top();
    }
    if (callee.topObj) {
        escapeArgs(AVal(), args, internReason("passed to an unknown constructor"));
        out.join(AVal::top());
    }
    const uint32_t protoKey = keys_.intern("prototype");
    for (ObjId o : callee.objs) {
        const ObjKind kind = objs_[o].kind;
        if (kind == ObjKind::Function) {
            const uint32_t f = objs_[o].fn;
            if ((mod_.functions[f].fnFlags & BRONZE_ABI_FN_FLAG_CONSTRUCT) == 0) continue;
            // Instances are abstracted by constructor, not by `new` site: a
            // class's shape is its constructor's, and per-site instances
            // would make every receiver set as large as the program's count
            // of `new` sites for the class.
            auto known = instances_.find(f);
            ObjId inst = kNoObj;
            if (known != instances_.end()) {
                inst = known->second;
            } else {
                inst = alloc(curSite_, kTagConstruct + f, ObjKind::Instance);
                objs_[inst].fn = f;
                instances_.emplace(f, inst);
            }
            const AVal pv = readProp(AVal::obj(o), protoKey);
            AVal proto;
            proto.objs = pv.objs;
            proto.topObj = pv.topObj;
            // A `prototype` that is not an object gives the instance
            // %Object.prototype% (10.1.14 GetPrototypeFromConstructor).
            if (pv.prims != 0) proto.addObj(builtinObj("Object.prototype"));
            if (recording_ && objs_[inst].proto.join(proto)) notify(inst, kKeyProto);
            bindFunction(f, o, AVal::obj(inst), args);
            // The instance, or an object the constructor returned instead.
            // Returning the receiver itself adds nothing: the receiver
            // parameter holds every instance of every `new` site, which the
            // site's own result must not.
            const AVal& r = fns_[f].foreignRet;
            out.addObj(inst);
            unionInto(out.objs, r.objs);
            if (r.topObj) out.topObj = true;
        } else if (kind == ObjKind::Bound) {
            depend(o, kKeyEnv);
            CallArgs merged;
            merged.args = objs_[o].boundArgs;
            for (const auto& a : args.args) merged.args.push_back(a);
            merged.spread = args.spread;
            const AVal target = objs_[o].env;
            out.join(construct(target, merged, depth + 1));
        } else if (kind == ObjKind::Builtin) {
            const Effect effect = catalog_.def(objs_[o].builtin).construct;
            if (effect != Effect::None) out.join(callBuiltin(effect, o, AVal(), args, true, depth));
        }
    }
    return out;
}

AVal Solver::superCall(const AVal& base, const AVal& self, const CallArgs& args, AVal& foreign) {
    // 13.3.7.1 step 7: the receiver from here on is the object the base
    // constructor returned when it returned one, and `self` otherwise.
    // `foreign` is the first part alone.
    AVal out = self;
    if (base.topObj) {
        escapeArgs(self, args, internReason("initialised by an unknown base class"));
        foreign.join(AVal::top());
    }
    for (ObjId o : base.objs) {
        const ObjKind kind = objs_[o].kind;
        if (kind == ObjKind::Function) {
            bindFunction(objs_[o].fn, o, self, args);
            const AVal& r = fns_[objs_[o].fn].foreignRet;
            unionInto(foreign.objs, r.objs);
            if (r.topObj) foreign.topObj = true;
        } else if (kind == ObjKind::Builtin) {
            // `extends Error` / `extends Array`: the builtin initialises the
            // receiver with keys of its own.
            for (ObjId s : self.objs) {
                markFlag(s, &AObj::anyKey);
                writeElems(s, AVal::prim(kAllPrims));
            }
            escapeArgs(AVal(), args, internReason("passed to a builtin base class"));
        } else {
            escapeArgs(self, args, internReason("initialised by an unknown base class"));
            foreign.join(AVal::top());
        }
    }
    unionInto(out.objs, foreign.objs);
    if (foreign.topObj) out.join(AVal::top());
    return out;
}

il::ValueId Solver::thisParam(uint32_t fn) const {
    const il::Function& f = mod_.functions[fn];
    if (!f.needsThis) return il::kNoValue;
    return static_cast<il::ValueId>(f.needsEnv ? 1 : 0);
}

AVal Solver::directCall(const il::Instruction& inst) {
    const uint32_t g = inst.calleeIndex;
    const il::Function& fn = mod_.functions[g];
    if (fn.isExternal) {
        const uint32_t why = internReason("passed to runtime helper " + fn.name);
        for (size_t i = 0; i < inst.operands.size(); ++i) escape(op(inst, i), why);
        return AVal::top();
    }
    if (!recording_) return fns_[g].ret;
    markReachable(g);
    for (size_t i = 0; i < inst.operands.size() && i < fn.params.size(); ++i) {
        AVal v = op(inst, i);
        if (i == 0 && inst.callEnvHops != il::Instruction::kNoEnvHops) v = envWalk(v, inst.callEnvHops);
        if (mergeInto(fns_[g].values[i], v, fn.params[i].type)) enqueueUsers(g, static_cast<il::ValueId>(i));
    }
    fns_[g].callers.insert(curSite_);
    return fns_[g].ret;
}

AVal Solver::methodCall(const il::Instruction& inst, const AVal& recv, const CallArgs& args) {
    // Each target is called with only the receivers whose lookup finds it:
    // `v.clone()` over vectors and colours gives Vector3.clone the vectors
    // alone. Without the split every method of a polymorphic site would see
    // every receiver of the site as `this`.
    AVal out;
    // The lookups are kept per site, and only receivers not looked up yet
    // are, unless the heap under the earlier ones changed.
    MethodLookup& memo = lookups_[curSite_];
    const ObjSet fresh = unreadReceivers(recv.objs, memo.seen);
    for (ObjId r : fresh) {
        AVal fnv;
        readKey(r, inst.keyIndex, AVal::obj(r), fnv, 0);
        if (fnv.topObj) memo.unknownTarget = true;
        for (ObjId f : fnv.objs) insertInto(memo.byTarget[f], r);
    }
    if (!fresh.empty()) unionInto(memo.seen, fresh);
    std::unordered_map<ObjId, ObjSet> byTarget = memo.byTarget;
    bool unknownTarget = memo.unknownTarget;
    if (recv.topObj) {
        // An unknown receiver may be any object that escaped, and a method
        // its chain holds runs with it as `this`. An escaped function, and a
        // builtin, are what calling an unknown function already covers (the
        // first is called with unknown arguments, the second lets its
        // arguments escape), so only the program's own unescaped methods are
        // called here.
        //
        // A prediction keeps escaped objects in the values that reach them
        // (joinTracked), so an unknown receiver there is an object the
        // program never made, and the dispatch is left out.
        unknownTarget = true;
        if (!opts_.predict) topCallSites_.insert(curSite_);
        static const std::vector<ObjId> kNone;
        for (ObjId r : opts_.predict ? kNone : escapedList_) {
            if (contains(recv.objs, r) || !userChain(r)) continue;
            AVal fnv;
            readKey(r, inst.keyIndex, AVal::obj(r), fnv, 0);
            for (ObjId f : fnv.objs) {
                if (objs_[f].kind == ObjKind::Function && !objs_[f].escaped) byTarget[f].push_back(r);
            }
        }
    }
    if (unknownTarget) out.join(callValue(AVal::top(), AVal::top(), args));
    if (recv.prims != 0) {
        AVal fnv;
        readPrimProp(recv.prims, inst.keyIndex, fnv);
        out.join(callValue(fnv, AVal::prim(recv.prims), args));
    }
    for (auto& [f, selves] : byTarget) {
        std::sort(selves.begin(), selves.end());
        selves.erase(std::unique(selves.begin(), selves.end()), selves.end());
        AVal self;
        self.objs = std::move(selves);
        const AVal target = AVal::obj(f);
        out.join(callValue(target, self, args));
        // The direct edge of a site with a typed entry enters the entry, a
        // copy of the target's body, when the cache names the target.
        if (inst.typedTarget != il::Instruction::kNoDirectTarget &&
            inst.directTarget != il::Instruction::kNoDirectTarget && objs_[f].kind == ObjKind::Function &&
            objs_[f].fn == inst.directTarget) {
            out.join(bindFunction(inst.typedTarget, f, self, args));
        }
    }
    return out;
}

bool Solver::userChain(ObjId r) {
    // Its own methods escaped with it; only a prototype the program made can
    // hold one that did not. A builtin prototype's chain is builtins.
    depend(r, kKeyProto);
    const AVal& proto = objs_[r].proto;
    if (proto.topObj) return false;
    for (ObjId p : proto.objs) {
        if (objs_[p].kind != ObjKind::Builtin) return true;
    }
    return false;
}

AVal Solver::coroStart(const il::Instruction& inst) {
    const uint32_t body = inst.calleeIndex;
    const il::Function& fn = mod_.functions[body];
    if (recording_ && !fn.isExternal) {
        markReachable(body);
        for (size_t i = 0; i < inst.operands.size() && i < fn.params.size(); ++i) {
            if (mergeInto(fns_[body].values[i], op(inst, i), fn.params[i].type)) {
                enqueueUsers(body, static_cast<il::ValueId>(i));
            }
        }
        // What the body returns resolves a promise or ends a generator: it
        // goes to whoever drives the coroutine, which is the runtime.
        FnState& st = fns_[body];
        if (!st.retEscapes) {
            st.retEscapes = true;
            escape(st.ret, internReason("returned from a coroutine"));
        }
    }
    const ObjId g = alloc(curSite_, kTagCoro, ObjKind::Other);
    objs_[g].proto.topObj = true;
    return AVal::obj(g);
}

Solver::Callees Solver::callees(const AVal& callee) const {
    Callees out;
    out.unknown = callee.topObj;
    std::vector<ObjId> work(callee.objs.begin(), callee.objs.end());
    std::vector<ObjId> seen;
    while (!work.empty()) {
        const ObjId o = work.back();
        work.pop_back();
        if (std::find(seen.begin(), seen.end(), o) != seen.end()) continue;
        seen.push_back(o);
        const AObj& a = objs_[o];
        if (a.kind == ObjKind::Function) {
            if (std::find(out.fns.begin(), out.fns.end(), a.fn) == out.fns.end()) out.fns.push_back(a.fn);
        } else if (a.kind == ObjKind::Bound) {
            if (a.env.topObj) out.unknown = true;
            for (ObjId t : a.env.objs) work.push_back(t);
        } else if (a.kind == ObjKind::Builtin) {
            out.builtin = true;
            const Effect e = catalog_.def(a.builtin).call;
            if (e != Effect::MapSet && e != Effect::MapGet && e != Effect::SetAdd && e != Effect::ArrayPush &&
                e != Effect::NoOp) {
                out.builtinKeepsOnly = false;
            }
        } else {
            out.builtinKeepsOnly = false;
        }
    }
    std::sort(out.fns.begin(), out.fns.end());
    return out;
}

}  // namespace bronze::wpo
