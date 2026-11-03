// The heap half of the solver: property reads and writes, the prototype chain,
// environment records, and escape.
//
// Index-keyed properties of every object share one bucket, `elems`, whatever
// spelled the key — `a[i]` with a number, `a[0]` lowered to a named read of
// "0", an array literal's element. A key that LOOKS numeric without being an
// index ("-1", "1.5") reads and writes both its named field and the bucket, so
// a write by number and a read by name still meet.

#include <algorithm>
#include <cstdlib>
#include <unordered_set>

#include "opt/wpo/solver.h"
#include "opt/wpo/solver_tags.h"

namespace bronze::wpo {

namespace {

// How deep a prototype chain the analysis follows before it gives up and calls
// the rest unknown. A cycle (`Object.setPrototypeOf` is an escape, so only an
// imprecision can make one) ends here too.

bool looksNumeric(const std::string& name) {
    if (name.empty()) return false;
    if (name == "NaN" || name == "Infinity" || name == "-Infinity") return true;
    for (char c : name) {
        if (!((c >= '0' && c <= '9') || c == '-' || c == '.' || c == 'e' || c == '+')) return false;
    }
    char* end = nullptr;
    std::strtod(name.c_str(), &end);
    return end != nullptr && *end == '\0';
}

}  // namespace

void Solver::depend(ObjId obj, uint32_t key) {
    if (!recording_ || curSite_ == UINT32_MAX) return;
    if (!readerSeen_.insert(DepFn{obj, key, curSite_}).second) return;
    readers_[DepKey{obj, key}].push_back(curSite_);
}

void Solver::notify(ObjId obj, uint32_t key) {
    if (!recording_) return;
    auto wake = [&](uint32_t k) {
        auto it = readers_.find(DepKey{obj, k});
        if (it == readers_.end()) return;
        for (uint32_t site : it->second) {
            insertInto(dirtyObjs_[site], obj);
            enqueueInst(site);
        }
    };
    wake(key);
    if (key != kKeyAny && key != kKeyFlags) wake(kKeyAny);
}

void Solver::markFlag(ObjId obj, bool AObj::*flag) {
    if (!recording_) return;
    AObj& o = objs_[obj];
    if (o.*flag) return;
    o.*flag = true;
    notify(obj, kKeyFlags);
}

bool Solver::isNumericKey(uint32_t key) {
    return keys_.isIndex(key) || looksNumeric(keys_.name(key));
}

bool Solver::isImplicitKey(uint32_t key) const {
    return std::binary_search(implicitKeys_.begin(), implicitKeys_.end(), key);
}

ObjId Solver::builtinObj(const std::string& path) {
    const uint32_t index = catalog_.find(path);
    if (index == UINT32_MAX) return kNoObj;
    auto it = builtinObjs_.find(index);
    if (it != builtinObjs_.end()) return it->second;
    const ObjId id = static_cast<ObjId>(objs_.size());
    objs_.emplace_back();
    objs_.back().kind = ObjKind::Builtin;
    objs_.back().builtin = index;
    builtinObjs_.emplace(index, id);
    const std::string& proto = catalog_.def(index).proto;
    if (!proto.empty()) {
        const ObjId p = builtinObj(proto);
        if (p != kNoObj) objs_[id].proto.addObj(p);
    }
    return id;
}

AVal Solver::readProp(const AVal& recv, uint32_t key) {
    AVal out;
    if (recv.topObj) out.join(AVal::top());
    if (recv.prims != 0) readPrimProp(recv.prims, key, out);
    for (ObjId o : recv.objs) readKey(o, key, AVal::obj(o), out, 0);
    return out;
}

AVal Solver::queryProp(const AVal& recv, uint32_t key) {
    const bool saved = recording_;
    recording_ = false;
    AVal out = readProp(recv, key);
    recording_ = saved;
    return out;
}

void Solver::readPrimProp(uint16_t prims, uint32_t key, AVal& out) {
    if ((prims & kStr) != 0) {
        if (keys_.name(key) == "length") {
            out.addPrims(kI32);
        } else if (isNumericKey(key)) {
            out.addPrims(kStr | kUndef);
        } else {
            readKey(builtinObj("String.prototype"), key, AVal::prim(kStr), out, 0);
        }
    }
    if ((prims & kNumber) != 0) readKey(builtinObj("Number.prototype"), key, AVal::prim(kNumber), out, 0);
    if ((prims & kBool) != 0) readKey(builtinObj("Boolean.prototype"), key, AVal::prim(kBool), out, 0);
    if ((prims & kSym) != 0) readKey(builtinObj("Symbol.prototype"), key, AVal::prim(kSym), out, 0);
    if ((prims & kBigInt) != 0) out.join(AVal::top());
}

void Solver::readBuiltinMember(ObjId obj, uint32_t key, AVal& out, bool& absent) {
    const BuiltinDef& def = catalog_.def(objs_[obj].builtin);
    absent = false;
    const std::string& name = keys_.name(key);
    const bool callable = def.call != Effect::None || def.construct != Effect::None;
    if (callable && (name == "length" || name == "name")) {
        out.addPrims(name == "length" ? kI32 : kStr);
        return;
    }
    const uint32_t member = catalog_.find(def.path + "." + name);
    if (member != UINT32_MAX) {
        const BuiltinDef& m = catalog_.def(member);
        if (m.isObject) {
            const auto it = builtinObjs_.find(member);
            out.addObj(it != builtinObjs_.end() ? it->second : builtinObj(m.path));
        } else {
            out.addPrims(m.prims);
        }
        return;
    }
    // A function member lists no own keys but `length` and `name`, so it is
    // complete like a prototype; a namespace or a constructor is not.
    const bool fnMember = def.isObject && callable && def.instanceProto.empty() &&
                          def.path.find('.') != std::string::npos;
    if (def.complete || fnMember) {
        absent = true;
        return;
    }
    out.join(AVal::top());
}

void Solver::readKey(ObjId start, uint32_t key, const AVal& self, AVal& out, int) {
    // The chain is a graph once prototypes are sets: each object is read
    // once, whichever path reaches it.
    // Chains are short: a vector searched linearly is the cheaper set.
    std::vector<ObjId> work{start};
    std::vector<ObjId> seen;
    const std::string& name = keys_.name(key);
    const bool numeric = isNumericKey(key);
    // A prediction reads an escaped object's own facts: unknown code is
    // assumed to have written only keys the program never names on it, so
    // the read is unknown only when nothing on the chain has the key.
    bool escapedOnChain = false;
    bool found = false;
    while (!work.empty()) {
        const ObjId obj = work.back();
        work.pop_back();
        if (obj == kNoObj || std::find(seen.begin(), seen.end(), obj) != seen.end()) continue;
        seen.push_back(obj);
        depend(obj, key);
        depend(obj, kKeyFlags);
        const AObj& o = objs_[obj];
        if (o.escaped) {
            if (opts_.predict) escapedOnChain = true;
            else out.join(AVal::top());
        }
        if (name == "__proto__") {
            depend(obj, kKeyProto);
            out.join(o.proto);
            found = true;
            continue;
        }
        if ((o.kind == ObjKind::Array || o.kind == ObjKind::TypedArray) && name == "length") {
            out.addPrims(kI32);
            found = true;
            continue;
        }
        if (o.kind == ObjKind::Function || o.kind == ObjKind::Bound) {
            if (name == "length") {
                out.addPrims(kI32);
                found = true;
                continue;
            }
            if (name == "name" && o.fields.find(key) == o.fields.end()) {
                out.addPrims(kStr);
                found = true;
                continue;
            }
        }
        if (numeric) {
            if (o.kind == ObjKind::TypedArray) {
                out.addPrims(kNumber | kUndef);
                continue;
            }
            depend(obj, kKeyElems);
            out.join(o.elems);
            // A hole, or an index past the end, reads on through the chain.
            if (keys_.isIndex(key)) out.addPrims(kUndef);
        }
        auto field = o.fields.find(key);
        if (field != o.fields.end()) {
            out.join(field->second);
            found = true;
        }
        auto acc = o.accessors.find(key);
        if (acc != o.accessors.end()) {
            const AVal getters = acc->second.getters;
            found = true;
            out.join(callValue(getters, self, CallArgs{}));
        }
        if (objs_[obj].anyKey && !numeric) {
            depend(obj, kKeyElems);
            out.join(objs_[obj].elems);
        }
        if (objs_[obj].kind == ObjKind::Builtin) {
            bool absent = false;
            readBuiltinMember(obj, key, out, absent);
            if (!absent) {
                found = true;
                continue;
            }
        }
        if (objs_[obj].kind == ObjKind::Env) continue;
        const AObj& cur = objs_[obj];
        if (!cur.deleted && std::binary_search(cur.definite.begin(), cur.definite.end(), key)) continue;
        depend(obj, kKeyProto);
        const AVal& proto = objs_[obj].proto;
        if (proto.topObj) out.join(AVal::top());
        if (proto.objs.empty() || (proto.prims & kNull) != 0) out.addPrims(kUndef);
        for (ObjId p : proto.objs) work.push_back(p);
    }
    if (escapedOnChain && !found) out.join(AVal::top());
}

void Solver::readAll(const ObjSet& starts, AVal& out, bool enumerableOnly) {
    // A prediction does not follow a read by an unknown key: it would join
    // every field of every receiver, which is no prediction a guard can use,
    // and it is what would spread each object to every site.
    if (opts_.predict) {
        if (!starts.empty()) out.join(AVal::top());
        return;
    }
    // Every object on the receivers' chains once, however many receivers
    // share it. A read under an enumerated key name sees only what
    // enumeration can name: not a class's methods, accessors or
    // `constructor`, not a function's `prototype`, not a builtin's members.
    std::vector<ObjId> work(starts.begin(), starts.end());
    std::unordered_set<ObjId> seen;
    while (!work.empty()) {
        const ObjId obj = work.back();
        work.pop_back();
        if (obj == kNoObj || !seen.insert(obj).second) continue;
        for (uint32_t k : {kKeyAny, kKeyFlags, kKeyElems, kKeySym, kKeyProto}) depend(obj, k);
        const AObj& o = objs_[obj];
        if (o.escaped) out.join(AVal::top());
        const bool hides = enumerableOnly && (o.kind == ObjKind::Proto || o.kind == ObjKind::Function) &&
                           !o.deleted && !o.attrsChanged && !o.definite.empty();
        if (hides) {
            for (const auto& [k, v] : o.fields) {
                if (!std::binary_search(o.definite.begin(), o.definite.end(), k)) out.join(v);
            }
        } else {
            out.join(o.allFields);
        }
        out.join(o.elems);
        if (!enumerableOnly) out.join(o.sym);
        if (o.kind == ObjKind::TypedArray) out.addPrims(kNumber);
        if (o.kind == ObjKind::Array || o.kind == ObjKind::Function) out.addPrims(kI32 | kStr);
        std::vector<AVal> getters;
        for (const auto& [k, a] : o.accessors) {
            if (hides && std::binary_search(o.definite.begin(), o.definite.end(), k)) continue;
            getters.push_back(a.getters);
        }
        for (const auto& g : getters) out.join(callValue(g, AVal::obj(obj), CallArgs{}));
        if (objs_[obj].kind == ObjKind::Builtin) {
            // A builtin's own members are builtins: unknown values as far as
            // a read by an unknown key goes. None is enumerable.
            if (!enumerableOnly) out.join(AVal::top());
            continue;
        }
        if (objs_[obj].kind == ObjKind::Env) continue;
        const AVal& proto = objs_[obj].proto;
        if (proto.topObj) out.join(AVal::top());
        out.addPrims(kUndef);
        for (ObjId p : proto.objs) work.push_back(p);
    }
}

std::vector<uint32_t> Solver::primKeys(const AVal& idx) {
    std::vector<uint32_t> out;
    if (idx.hasObjects() || (idx.prims & (kStr | kSym | kBigInt)) != 0 || idx.prims == 0) return out;
    if ((idx.prims & kNumber) != 0) out.push_back(keys_.intern("0"));
    if ((idx.prims & kUndef) != 0) out.push_back(keys_.intern("undefined"));
    if ((idx.prims & kNull) != 0) out.push_back(keys_.intern("null"));
    if ((idx.prims & kBool) != 0) {
        out.push_back(keys_.intern("true"));
        out.push_back(keys_.intern("false"));
    }
    return out;
}

void Solver::readElem(const AVal& recv, const AVal& idx, AVal& out) {
    if (recv.topObj) out.join(AVal::top());
    if (idx.onlyNumbers()) {
        const uint32_t zero = keys_.intern("0");
        if ((recv.prims & kStr) != 0) out.addPrims(kStr | kUndef);
        for (ObjId o : recv.objs) readKey(o, zero, AVal::obj(o), out, 0);
        return;
    }
    if (const auto named = primKeys(idx); !named.empty()) {
        // A key that is a number, `undefined`, `null` or a boolean names
        // what its string spells: each spelling is one named read.
        for (uint32_t k : named) out.join(readProp(recv, k));
        return;
    }
    if (idx.isStrConst()) {
        out.join(readProp(recv, idx.strKey));
        return;
    }
    if (idx.prims == kSym && !idx.hasObjects()) {
        if (recv.prims != 0) out.join(AVal::top());
        for (ObjId start : recv.objs) {
            // The symbol bucket of each object on the chain; a builtin's
            // symbol-keyed members (`Symbol.iterator`) are unknown values.
            std::vector<ObjId> chain{start};
            std::unordered_set<ObjId> seen;
            while (!chain.empty()) {
                const ObjId o = chain.back();
                chain.pop_back();
                if (!seen.insert(o).second) continue;
                depend(o, kKeySym);
                depend(o, kKeyFlags);
                depend(o, kKeyProto);
                const AObj& a = objs_[o];
                if (a.escaped || a.kind == ObjKind::Builtin || a.proto.topObj) out.join(AVal::top());
                out.join(a.sym);
                if (a.anyKey) out.join(a.elems);
                for (ObjId p : a.proto.objs) chain.push_back(p);
            }
            out.addPrims(kUndef);
        }
        return;
    }
    if (recv.prims != 0) out.join(AVal::top());
    readAll(recv.objs, out, idx.onlyEnumKeys());
}

void Solver::writeField(ObjId obj, uint32_t key, const AVal& val) {
    if (!recording_) return;
    AObj& o = objs_[obj];
    AVal& slot = o.fields[key];
    if (!joinTracked(slot, val)) return;
    if (slot.objs.size() > opts_.setCap) widen(slot);
    objs_[obj].allFields.join(objs_[obj].fields[key]);
    notify(obj, key);
    if (objs_[obj].escaped) escape(val, internReason("stored into an escaped object"));
    if (isImplicitKey(key)) escapeCallables(val, internReason("stored under a key conversions call"));
}

void Solver::writeElems(ObjId obj, const AVal& val) {
    if (!recording_) return;
    AObj& o = objs_[obj];
    if (o.kind == ObjKind::TypedArray) return;
    if (!joinTracked(o.elems, val)) return;
    if (objs_[obj].elems.objs.size() > opts_.setCap) widen(objs_[obj].elems);
    notify(obj, kKeyElems);
    if (objs_[obj].escaped) escape(val, internReason("stored into an escaped object"));
}

void Solver::writeProp(const AVal& recv, uint32_t key, const AVal& val, bool define) {
    if (!recording_) return;
    if (recv.topObj) escape(val, internReason("stored into an unknown object"));
    const std::string& name = keys_.name(key);
    const bool numeric = isNumericKey(key);
    const bool index = keys_.isIndex(key);
    for (ObjId o : recv.objs) {
        const ObjKind kind = objs_[o].kind;
        if (kind == ObjKind::Builtin) {
            writeField(o, key, val);
            escape(val, internReason("stored on a builtin object"));
            continue;
        }
        if (name == "__proto__" && !define) {
            AVal proto;
            proto.objs = val.objs;
            proto.topObj = val.topObj;
            if (objs_[o].proto.join(proto)) notify(o, kKeyProto);
            markFlag(o, &AObj::protoSet);
            continue;
        }
        if ((kind == ObjKind::Array || kind == ObjKind::TypedArray) && name == "length") continue;
        if (numeric) {
            writeElems(o, val);
            if (index) continue;
        }
        if (!define && callSetters(o, key, AVal::obj(o), val, 0)) continue;
        writeField(o, key, val);
    }
}

void Solver::writeElem(const AVal& recv, const AVal& idx, const AVal& val) {
    if (!recording_) return;
    if (recv.topObj) escape(val, internReason("stored into an unknown object"));
    if (idx.onlyNumbers()) {
        writeProp(recv, keys_.intern("0"), val, false);
        return;
    }
    if (const auto named = primKeys(idx); !named.empty()) {
        for (uint32_t k : named) writeProp(recv, k, val, false);
        return;
    }
    if (idx.isStrConst()) {
        writeProp(recv, idx.strKey, val, false);
        return;
    }
    // Under a symbol, or under a key the analysis cannot name — which may be
    // a symbol too. Either way a function in the value may be called by a
    // protocol (`Symbol.iterator`, `valueOf`) the program never spells.
    escapeCallables(val, internReason("stored under a computed key protocols may call"));
    const bool symOnly = idx.prims == kSym && !idx.hasObjects();
    for (ObjId o : recv.objs) {
        if (objs_[o].kind == ObjKind::Builtin) continue;
        if (joinTracked(objs_[o].sym, val)) notify(o, kKeySym);
        if (symOnly) continue;
        writeElems(o, val);
        markFlag(o, &AObj::anyKey);
        callAllSetters(o, AVal::obj(o), val, 0);
    }
}

void Solver::defineAccessor(const AVal& recv, uint32_t key, const AVal& getter, const AVal& setter) {
    if (!recording_) return;
    if (recv.topObj) {
        const uint32_t why = internReason("accessor of an unknown object");
        escape(getter, why);
        escape(setter, why);
    }
    for (ObjId o : recv.objs) {
        Accessor& a = objs_[o].accessors[key];
        bool changed = a.getters.join(getter);
        if (a.setters.join(setter)) changed = true;
        if (!changed) continue;
        notify(o, key);
        if (objs_[o].escaped || objs_[o].kind == ObjKind::Builtin || isImplicitKey(key)) {
            const uint32_t why = internReason("accessor of an escaped object");
            escape(getter, why);
            escape(setter, why);
        }
    }
}

bool Solver::callSetters(ObjId start, uint32_t key, const AVal& self, const AVal& val, int) {
    // Every object on the chain is visited once. The write makes no own key
    // when no path from `start` reaches the end of the chain, or a data
    // property, without first meeting an accessor.
    std::vector<ObjId> work{start};
    std::unordered_set<ObjId> seen;
    bool endsAtAccessor = true;
    while (!work.empty()) {
        const ObjId obj = work.back();
        work.pop_back();
        if (obj == kNoObj || !seen.insert(obj).second) continue;
        depend(obj, key);
        depend(obj, kKeyProto);
        depend(obj, kKeyFlags);
        auto acc = objs_[obj].accessors.find(key);
        const bool hasAccessor = acc != objs_[obj].accessors.end();
        if (hasAccessor) {
            const AVal setters = acc->second.setters;
            CallArgs a;
            a.args.push_back(val);
            callValue(setters, self, a);
        }
        const AObj& cur = objs_[obj];
        // An own accessor is where the lookup ends unless the key may also be
        // a data property here (a redefinition) or may be gone (a delete).
        if (hasAccessor && cur.fields.count(key) == 0 && !cur.deleted && !cur.attrsChanged && !cur.escaped) {
            continue;
        }
        const bool definite = !cur.deleted && !cur.attrsChanged &&
                              std::binary_search(cur.definite.begin(), cur.definite.end(), key);
        if (cur.fields.count(key) != 0 || cur.kind == ObjKind::Builtin || definite) {
            endsAtAccessor = false;
            continue;
        }
        const AVal& proto = cur.proto;
        if (proto.topObj) escape(val, internReason("stored through an unknown prototype"));
        if (proto.topObj || proto.objs.empty()) endsAtAccessor = false;
        for (ObjId p : objs_[obj].proto.objs) work.push_back(p);
    }
    return endsAtAccessor;
}

void Solver::callAllSetters(ObjId start, const AVal& self, const AVal& val, int) {
    std::vector<ObjId> work{start};
    std::unordered_set<ObjId> seen;
    while (!work.empty()) {
        const ObjId obj = work.back();
        work.pop_back();
        if (obj == kNoObj || !seen.insert(obj).second) continue;
        depend(obj, kKeyAny);
        depend(obj, kKeyProto);
        std::vector<AVal> setters;
        for (const auto& [k, a] : objs_[obj].accessors) setters.push_back(a.setters);
        for (const auto& s : setters) {
            CallArgs a;
            a.args.push_back(val);
            callValue(s, self, a);
        }
        if (objs_[obj].kind == ObjKind::Builtin) continue;
        const AVal& proto = objs_[obj].proto;
        if (proto.topObj) escape(val, internReason("stored through an unknown prototype"));
        for (ObjId p : objs_[obj].proto.objs) work.push_back(p);
    }
}

AVal Solver::envWalk(const AVal& env, uint32_t depth) {
    AVal cur = env;
    for (uint32_t d = 0; d < depth; ++d) {
        AVal next;
        if (cur.topObj) next.topObj = true;
        for (ObjId o : cur.objs) {
            depend(o, kKeyProto);
            next.join(objs_[o].proto);
        }
        cur = std::move(next);
    }
    return cur;
}

AVal Solver::envGet(const AVal& env, uint32_t depth, uint32_t index) {
    const AVal rec = envWalk(env, depth);
    AVal out;
    if (rec.topObj) out.join(AVal::top());
    for (ObjId o : rec.objs) {
        depend(o, index);
        auto it = objs_[o].fields.find(index);
        if (it != objs_[o].fields.end()) out.join(it->second);
    }
    return out;
}

void Solver::envSet(const AVal& env, uint32_t depth, uint32_t index, const AVal& val) {
    if (!recording_) return;
    const AVal rec = envWalk(env, depth);
    if (rec.topObj) escape(val, internReason("stored into an unknown environment"));
    for (ObjId o : rec.objs) {
        AVal& slot = objs_[o].fields[index];
        if (!joinTracked(slot, val)) continue;
        if (slot.objs.size() > opts_.setCap) widen(slot);
        objs_[o].allFields.join(objs_[o].fields[index]);
        notify(o, index);
    }
}

void Solver::escapeCallables(const AVal& v, uint32_t reason) {
    // A protocol calls what it finds: only a function is run by it.
    if (!recording_) return;
    for (ObjId o : v.objs) {
        if (objs_[o].kind == ObjKind::Function || objs_[o].kind == ObjKind::Bound) escapeObj(o, reason);
    }
}

void Solver::escape(const AVal& v, uint32_t reason) {
    if (!recording_) return;
    for (ObjId o : v.objs) escapeObj(o, reason);
}

void Solver::escapeObj(ObjId id, uint32_t reason) {
    if (!recording_) return;
    std::vector<ObjId> stack{id};
    while (!stack.empty()) {
        const ObjId o = stack.back();
        stack.pop_back();
        AObj& a = objs_[o];
        if (a.escaped || a.kind == ObjKind::Env || a.kind == ObjKind::Builtin) continue;
        a.escaped = true;
        a.escapeReason = reason;
        a.escapeSite = curSite_;
        escapedList_.push_back(o);
        notify(o, kKeyFlags);
        auto push = [&](const AVal& v) {
            for (ObjId x : v.objs) {
                if (!objs_[x].escaped) stack.push_back(x);
            }
        };
        for (const auto& [k, v] : a.fields) push(v);
        for (const auto& [k, acc] : a.accessors) {
            push(acc.getters);
            push(acc.setters);
        }
        push(a.elems);
        push(a.keys);
        push(a.sym);
        push(a.boundThis);
        for (const auto& b : a.boundArgs) push(b);
        if (a.kind == ObjKind::Bound) push(a.env);
        if (a.kind == ObjKind::Function) {
            const uint32_t fn = a.fn;
            // Binding allocates rest arrays and may reach `objs_` again.
            bindTop(fn, o);
        }
    }
}

}  // namespace bronze::wpo
