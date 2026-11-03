// What calling a builtin does to the abstract heap (builtins.h names the
// effects). Every effect is written so that what it leaves out is something
// the builtin provably does not do — keep an argument, call a callback, add a
// key — and an effect that cannot say that is `Escape`.

#include <algorithm>

#include "opt/wpo/solver.h"
#include "opt/wpo/solver_tags.h"

namespace bronze::wpo {

AVal Solver::globalValue(uint32_t key) {
    const std::string& name = keys_.name(key);
    if (name == "undefined") return AVal::prim(kUndef);
    if (name == "NaN" || name == "Infinity") return AVal::prim(kF64);
    if (hostGlobals_.count(name) != 0) return AVal::top();
    const uint32_t index = catalog_.find(name);
    if (index == UINT32_MAX) return AVal::top();
    const BuiltinDef& def = catalog_.def(index);
    if (!def.isObject) return AVal::prim(def.prims);
    return AVal::obj(builtinObj(def.path));
}

AVal Solver::arg(const CallArgs& args, size_t i) const {
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
    return v;
}

AVal Solver::elemsOf(const AVal& v) {
    AVal out;
    if (v.topObj) out.join(AVal::top());
    if ((v.prims & kStr) != 0) out.addPrims(kStr);
    for (ObjId o : v.objs) {
        depend(o, kKeyElems);
        depend(o, kKeyFlags);
        const AObj& a = objs_[o];
        if (a.escaped) out.join(AVal::top());
        if (a.kind == ObjKind::TypedArray) {
            out.addPrims(kNumber);
            continue;
        }
        out.join(a.elems);
    }
    return out;
}

AVal Solver::newArray(uint32_t tag, const AVal& elems, ObjKind kind) {
    const ObjId o = alloc(curSite_, tag, kind);
    if (objs_[o].proto.empty()) {
        if (kind == ObjKind::Array) {
            objs_[o].proto.addObj(builtinObj("Array.prototype"));
        } else if (kind == ObjKind::TypedArray) {
            objs_[o].proto.addObj(builtinObj("TypedArray.prototype"));
        } else {
            // An iterator's prototype is a builtin the catalog does not
            // model: `next` on it is an unknown call.
            objs_[o].proto.topObj = true;
        }
    }
    writeElems(o, elems);
    return AVal::obj(o);
}

// The values a for-of, a spread or a destructuring takes out of `v`.
AVal Solver::iterValues(const AVal& v) {
    AVal out;
    if (v.topObj) out.join(AVal::top());
    if ((v.prims & kStr) != 0) out.addPrims(kStr);
    const uint32_t mapIdx = catalog_.find("Map");
    const uint32_t weakMapIdx = catalog_.find("WeakMap");
    for (ObjId o : v.objs) {
        const AObj& a = objs_[o];
        switch (a.kind) {
            case ObjKind::Array:
            case ObjKind::TypedArray:
            case ObjKind::Iterator:
                out.join(elemsOf(AVal::obj(o)));
                break;
            case ObjKind::Collection: {
                depend(o, kKeyElems);
                depend(o, kKeyFlags);
                if (a.builtin == mapIdx || a.builtin == weakMapIdx) {
                    AVal pair = a.keys;
                    pair.join(a.elems);
                    out.join(newArray(kTagPair, pair));
                } else {
                    out.join(a.elems);
                }
                if (objs_[o].escaped) out.join(AVal::top());
                break;
            }
            default:
                // A program-defined iterable: its iterator method was made
                // to escape when it was stored under its symbol key, and what
                // it yields is unknown.
                escapeObj(o, internReason("iterated as a user-defined iterable"));
                out.join(AVal::top());
                break;
        }
    }
    return out;
}

AVal Solver::callback(const AVal& fn, const AVal& self, std::vector<AVal> args, int depth) {
    CallArgs a;
    a.args = std::move(args);
    return callValue(fn, self, a, depth + 1);
}

AVal Solver::callBuiltin(Effect effect, ObjId fnObj, const AVal& self, const CallArgs& args,
                         bool isConstruct, int depth) {
    const BuiltinDef& def = catalog_.def(objs_[fnObj].builtin);
    const AVal num = AVal::prim(kNumber);
    auto escapeAll = [&]() {
        escapeArgs(self, args, internReason("passed to builtin " + def.path));
    };
    auto allArgs = [&]() {
        AVal all;
        for (const auto& a : args.args) all.join(a);
        if (args.spread) all.join(*args.spread);
        return all;
    };
    auto selfTyped = [&]() {
        return !self.objs.empty() &&
               std::all_of(self.objs.begin(), self.objs.end(),
                           [&](ObjId o) { return objs_[o].kind == ObjKind::TypedArray; });
    };
    switch (effect) {
        case Effect::None: return AVal();
        case Effect::Prim: return AVal::prim(def.prims);
        case Effect::NoOp: return AVal::prim(kUndef);
        case Effect::Escape: escapeAll(); return AVal::top();
        case Effect::EscapePrim: escapeAll(); return AVal::prim(def.prims);
        case Effect::ReturnThis: return self;
        case Effect::ReturnArg0: return arg(args, 0);
        case Effect::FreezeArg0: {
            const AVal a0 = arg(args, 0);
            for (ObjId o : a0.objs) markFlag(o, &AObj::attrsChanged);
            return a0;
        }
        case Effect::ObjectCtor: {
            const AVal a0 = arg(args, 0);
            AVal out;
            out.objs = a0.objs;
            out.topObj = a0.topObj;
            if (a0.prims != 0 || isConstruct) {
                const ObjId o = alloc(curSite_, kTagResult, ObjKind::Plain);
                if (objs_[o].proto.empty()) objs_[o].proto.addObj(builtinObj("Object.prototype"));
                out.addObj(o);
            }
            return out;
        }
        case Effect::ObjectCreate: {
            const AVal a0 = arg(args, 0);
            const ObjId o = alloc(curSite_, kTagResult, ObjKind::Plain);
            AVal proto;
            proto.objs = a0.objs;
            proto.topObj = a0.topObj;
            if (recording_ && objs_[o].proto.join(proto)) notify(o, kKeyProto);
            if (args.args.size() > 1 || args.spread) {
                const AVal props = arg(args, 1);
                if (props.hasObjects()) {
                    escape(props, internReason("property descriptors of Object.create"));
                    markFlag(o, &AObj::anyKey);
                    writeElems(o, AVal::top());
                }
            }
            return AVal::obj(o);
        }
        case Effect::ObjectAssign: {
            const AVal target = arg(args, 0);
            AVal vals;
            for (size_t i = 1; i < args.args.size(); ++i) {
                const AVal& src = args.args[i];
                if (src.topObj) vals.join(AVal::top());
                readAll(src.objs, vals);
            }
            if (args.spread) vals.join(AVal::top());
            if (target.topObj) escape(vals, internReason("assigned into an unknown object"));
            for (ObjId t : target.objs) {
                writeElems(t, vals);
                markFlag(t, &AObj::anyKey);
                callAllSetters(t, AVal::obj(t), vals, 0);
            }
            return target;
        }
        case Effect::ObjectDefineProperty: {
            const AVal target = arg(args, 0);
            const AVal key = arg(args, 1);
            const AVal desc = arg(args, 2);
            const AVal value = readProp(desc, keys_.intern("value"));
            AVal getter = readProp(desc, keys_.intern("get"));
            AVal setter = readProp(desc, keys_.intern("set"));
            getter.prims = 0;
            setter.prims = 0;
            if (key.isStrConst()) {
                writeProp(target, key.strKey, value, true);
                if (getter.hasObjects() || setter.hasObjects()) defineAccessor(target, key.strKey, getter, setter);
            } else {
                writeElem(target, key, value);
                const uint32_t why = internReason("accessor under a computed key");
                escape(getter, why);
                escape(setter, why);
            }
            return target;
        }
        case Effect::ObjectKeys: return newArray(kTagResult, AVal::prim(kStr));
        case Effect::ObjectEnumKeys: return newArray(kTagResult, AVal::enumKeys());
        case Effect::ObjectValues:
        case Effect::ObjectEntries: {
            AVal vals;
            const AVal a0 = arg(args, 0);
            if (a0.topObj) vals.join(AVal::top());
            readAll(a0.objs, vals);
            if (effect == Effect::ObjectValues) return newArray(kTagResult, vals);
            vals.addPrims(kStr);
            return newArray(kTagResult, newArray(kTagPair, vals));
        }
        case Effect::ObjectGetProto: {
            AVal out;
            const AVal a0 = arg(args, 0);
            if (a0.topObj || a0.prims != 0) out.join(AVal::top());
            for (ObjId o : a0.objs) {
                depend(o, kKeyProto);
                out.join(objs_[o].proto);
            }
            if (out.empty()) out.addPrims(kNull);
            return out;
        }
        case Effect::ArrayCtor: {
            AVal elems;
            if (args.args.size() == 1 && !args.spread && args.args[0].onlyNumbers()) {
                elems.addPrims(kUndef);
            } else {
                elems = allArgs();
            }
            return newArray(kTagResult, elems);
        }
        case Effect::ArrayOf: return newArray(kTagResult, allArgs());
        case Effect::ArrayFrom: {
            const AVal src = arg(args, 0);
            AVal vals;
            if (src.topObj) vals.join(AVal::top());
            if ((src.prims & kStr) != 0) vals.addPrims(kStr);
            for (ObjId o : src.objs) {
                const ObjKind kind = objs_[o].kind;
                if (kind == ObjKind::Plain || kind == ObjKind::Instance) {
                    // An array-like (`{length: n}`): its index-keyed values,
                    // or a hole.
                    depend(o, kKeyElems);
                    vals.join(objs_[o].elems);
                    vals.addPrims(kUndef);
                    if (objs_[o].escaped || !objs_[o].sym.empty()) vals.join(iterValues(AVal::obj(o)));
                } else {
                    vals.join(iterValues(AVal::obj(o)));
                }
            }
            const AVal mapFn = arg(args, 1);
            if (mapFn.hasObjects()) vals = callback(mapFn, arg(args, 2), {vals, num}, depth);
            return newArray(kTagResult, vals);
        }
        case Effect::ArrayPush: {
            const AVal all = allArgs();
            for (ObjId o : self.objs) writeElems(o, all);
            if (self.topObj) escape(all, internReason("pushed onto an unknown array"));
            return AVal::prim(kI32);
        }
        case Effect::ArrayPop:
        case Effect::ArrayAt:
        case Effect::ArrayFind: {
            AVal out = elemsOf(self);
            out.addPrims(kUndef);
            if (effect == Effect::ArrayFind) callback(arg(args, 0), arg(args, 1), {elemsOf(self), num, self}, depth);
            return out;
        }
        case Effect::ArraySplice: {
            AVal inserted;
            for (size_t i = 2; i < args.args.size(); ++i) inserted.join(args.args[i]);
            if (args.spread) inserted.join(*args.spread);
            const AVal removed = newArray(kTagResult, elemsOf(self));
            for (ObjId o : self.objs) writeElems(o, inserted);
            if (self.topObj) escape(inserted, internReason("spliced into an unknown array"));
            return removed;
        }
        case Effect::ArraySlice:
            return newArray(kTagResult, elemsOf(self), selfTyped() ? ObjKind::TypedArray : ObjKind::Array);
        case Effect::ArrayConcat: {
            AVal vals = elemsOf(self);
            for (const auto& a : args.args) {
                AVal plain = a;
                plain.objs.clear();
                for (ObjId o : a.objs) {
                    if (objs_[o].kind == ObjKind::Array) {
                        vals.join(elemsOf(AVal::obj(o)));
                    } else {
                        plain.addObj(o);
                    }
                }
                vals.join(plain);
                if (a.topObj) vals.join(AVal::top());
            }
            if (args.spread) vals.join(AVal::top());
            return newArray(kTagResult, vals);
        }
        case Effect::ArraySort: {
            const AVal e = elemsOf(self);
            callback(arg(args, 0), AVal::prim(kUndef), {e, e}, depth);
            return self;
        }
        case Effect::ArrayFill: {
            const AVal a0 = arg(args, 0);
            for (ObjId o : self.objs) writeElems(o, a0);
            if (self.topObj) escape(a0, internReason("filled into an unknown array"));
            return self;
        }
        case Effect::ArrayForEach:
            callback(arg(args, 0), arg(args, 1), {elemsOf(self), num, self}, depth);
            return AVal::prim(kUndef);
        case Effect::ArrayMap: {
            const AVal r = callback(arg(args, 0), arg(args, 1), {elemsOf(self), num, self}, depth);
            return newArray(kTagResult, r);
        }
        case Effect::ArrayFilter:
            callback(arg(args, 0), arg(args, 1), {elemsOf(self), num, self}, depth);
            return newArray(kTagResult, elemsOf(self));
        case Effect::ArrayPredicate:
            callback(arg(args, 0), arg(args, 1), {elemsOf(self), num, self}, depth);
            return AVal::prim(def.prims);
        case Effect::ArrayReduce: {
            // The accumulator is the initial value, an element, or what the
            // callback returned for an earlier one: its own return flows back
            // into it, and the worklist carries that to a fixpoint.
            AVal acc = elemsOf(self);
            if (args.args.size() > 1 || args.spread) acc.join(arg(args, 1));
            const AVal cb = arg(args, 0);
            const AVal before = callback(cb, AVal::prim(kUndef), {acc, elemsOf(self), num, self}, depth);
            acc.join(before);
            acc.join(callback(cb, AVal::prim(kUndef), {acc, elemsOf(self), num, self}, depth));
            return acc;
        }
        case Effect::ArrayValues: return newArray(kTagResult, elemsOf(self), ObjKind::Iterator);
        case Effect::ArrayKeys: return newArray(kTagResult, num, ObjKind::Iterator);
        case Effect::ArrayEntries: {
            AVal pair = elemsOf(self);
            pair.join(num);
            return newArray(kTagResult, newArray(kTagPair, pair), ObjKind::Iterator);
        }
        case Effect::ArrayFlat: {
            if (!args.args.empty() || args.spread) {
                escapeAll();
                return AVal::top();
            }
            AVal vals = elemsOf(self);
            vals.join(elemsOf(vals));
            return newArray(kTagResult, vals);
        }
        case Effect::TypedCtor: {
            const ObjId o = alloc(curSite_, kTagResult, ObjKind::TypedArray);
            objs_[o].builtin = objs_[fnObj].builtin;
            if (objs_[o].proto.empty()) objs_[o].proto.addObj(builtinObj("TypedArray.prototype"));
            const AVal mapFn = arg(args, 1);
            if (!isConstruct && mapFn.hasObjects()) callback(mapFn, arg(args, 2), {iterValues(arg(args, 0)), num}, depth);
            return AVal::obj(o);
        }
        case Effect::TypedSubarray: {
            const AVal cb = arg(args, 0);
            if (cb.hasObjects()) callback(cb, arg(args, 1), {num, num, self}, depth);
            return newArray(kTagResult, num, ObjKind::TypedArray);
        }
        case Effect::TypedSet: return AVal::prim(kUndef);
        case Effect::CollectionCtor: {
            const ObjId c = alloc(curSite_, kTagResult, ObjKind::Collection);
            objs_[c].builtin = objs_[fnObj].builtin;
            if (objs_[c].proto.empty() && !def.instanceProto.empty()) {
                objs_[c].proto.addObj(builtinObj(def.instanceProto));
            }
            const AVal src = arg(args, 0);
            if (src.hasObjects()) {
                const AVal entries = iterValues(src);
                const bool isMap = def.path == "Map" || def.path == "WeakMap";
                const AVal parts = isMap ? elemsOf(entries) : entries;
                if (recording_ && objs_[c].keys.join(parts)) notify(c, kKeyElems);
                writeElems(c, parts);
            }
            return AVal::obj(c);
        }
        case Effect::MapGet: {
            AVal out = elemsOf(self);
            out.addPrims(kUndef);
            return out;
        }
        case Effect::MapSet:
        case Effect::SetAdd: {
            const AVal k = arg(args, 0);
            const AVal v = effect == Effect::SetAdd ? k : arg(args, 1);
            for (ObjId o : self.objs) {
                if (objs_[o].kind != ObjKind::Collection) continue;
                if (recording_ && objs_[o].keys.join(k)) {
                    notify(o, kKeyElems);
                    if (objs_[o].escaped) escape(k, internReason("stored into an escaped object"));
                }
                writeElems(o, v);
            }
            if (self.topObj) {
                const uint32_t why = internReason("stored into an unknown collection");
                escape(k, why);
                escape(v, why);
            }
            return self;
        }
        case Effect::MapForEach: {
            AVal keysVal;
            for (ObjId o : self.objs) {
                depend(o, kKeyElems);
                keysVal.join(objs_[o].keys);
            }
            if (self.topObj) keysVal.join(AVal::top());
            callback(arg(args, 0), arg(args, 1), {elemsOf(self), keysVal, self}, depth);
            return AVal::prim(kUndef);
        }
        case Effect::MapValues: return newArray(kTagResult, elemsOf(self), ObjKind::Iterator);
        case Effect::MapKeys:
        case Effect::MapEntries: {
            AVal keysVal;
            for (ObjId o : self.objs) {
                depend(o, kKeyElems);
                keysVal.join(objs_[o].keys);
            }
            if (self.topObj) keysVal.join(AVal::top());
            if (effect == Effect::MapKeys) return newArray(kTagResult, keysVal, ObjKind::Iterator);
            keysVal.join(elemsOf(self));
            return newArray(kTagResult, newArray(kTagPair, keysVal), ObjKind::Iterator);
        }
        case Effect::FnCall: {
            CallArgs rest;
            for (size_t i = 1; i < args.args.size(); ++i) rest.args.push_back(args.args[i]);
            rest.spread = args.spread;
            return callValue(self, arg(args, 0), rest, depth + 1);
        }
        case Effect::FnApply: {
            const AVal list = arg(args, 1);
            AVal spread = elemsOf(list);
            if ((list.prims & ~(kUndef | kNull)) != 0) spread.join(AVal::top());
            CallArgs rest;
            rest.spread = &spread;
            return callValue(self, arg(args, 0), rest, depth + 1);
        }
        case Effect::FnBind: {
            const ObjId b = alloc(curSite_, kTagBound, ObjKind::Bound);
            if (objs_[b].proto.empty()) objs_[b].proto.addObj(builtinObj("Function.prototype"));
            if (recording_) {
                bool changed = objs_[b].env.join(self);
                if (objs_[b].boundThis.join(arg(args, 0))) changed = true;
                if (objs_[b].boundArgs.size() + 1 < args.args.size()) {
                    objs_[b].boundArgs.resize(args.args.size() - 1);
                }
                for (size_t i = 1; i < args.args.size(); ++i) {
                    if (objs_[b].boundArgs[i - 1].join(args.args[i])) changed = true;
                }
                if (args.spread) {
                    // The bound arguments' positions are unknown: the target
                    // is called with unknown arguments.
                    const uint32_t why = internReason("bound by a spread");
                    escape(*args.spread, why);
                    escape(self, why);
                }
                if (changed) {
                    notify(b, kKeyEnv);
                    if (objs_[b].escaped) {
                        // What an escaped bound function holds escapes with it.
                        const uint32_t why = objs_[b].escapeReason;
                        escape(self, why);
                        escape(arg(args, 0), why);
                        for (size_t i = 1; i < args.args.size(); ++i) escape(args.args[i], why);
                    }
                }
            }
            return AVal::obj(b);
        }
        case Effect::StrReplace: {
            const AVal fn = arg(args, 1);
            if (fn.hasObjects()) callback(fn, AVal::prim(kUndef), {AVal::prim(kStr), num, AVal::prim(kStr)}, depth);
            return AVal::prim(kStr);
        }
        case Effect::StrArray: {
            AVal out = newArray(kTagResult, AVal::prim(kStr | kUndef));
            out.addPrims(kNull);
            return out;
        }
        case Effect::SymbolFn: return AVal::prim(kSym);
        case Effect::NewOther: {
            const ObjId o = alloc(curSite_, kTagResult, ObjKind::Other);
            objs_[o].builtin = objs_[fnObj].builtin;
            if (objs_[o].proto.empty()) {
                const ObjId p = def.instanceProto.empty() ? kNoObj : builtinObj(def.instanceProto);
                if (p != kNoObj) objs_[o].proto.addObj(p);
                else objs_[o].proto.topObj = true;
            }
            return AVal::obj(o);
        }
        case Effect::JsonParse:
            escape(arg(args, 1), internReason("reviver of JSON.parse"));
            return AVal::top();
    }
    return AVal::top();
}

}  // namespace bronze::wpo
