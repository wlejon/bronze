#include "types/field_audit.h"

#include <algorithm>

#include "types/walk.h"

namespace bronze::types {

namespace {

// The names a builtin prototype already owns. No audit over the PROGRAM's
// writes can speak for one: `"abc".length` is a Number the program never
// wrote, `re.flags` is a String of exactly the same standing, and `size` is a
// getter on Map and Set. A candidate in this set is refused before a single
// write is read.
//
// It does not have to be exhaustive to be sound, because the claim it guards is
// only ever made about a receiver whose class this compilation PROVED — a
// `new C()` instance — whose own property shadows anything a builtin prototype
// carries. It is here for the second road to the same object, a program that
// makes a builtin's prototype the base of one of its own classes, and it is
// cheap insurance rather than the load-bearing rule.
const char* const kBuiltinNames[] = {
    "__proto__",  "constructor", "length",      "name",       "message",
    "size",       "byteLength",  "byteOffset",  "buffer",     "prototype",
    "flags",      "source",      "lastIndex",   "global",     "sticky",
    "unicode",    "ignoreCase",  "multiline",   "dotAll",     "index",
    "input",      "groups",      "stack",       "cause",      "description",
    "raw",        "arguments",   "caller",      "then",       "valueOf",
    "toString",   "hasOwnProperty",
};

// The one class of write that cannot land on a property of an object this
// analysis speaks for: one that builds a NEW object. `{ x: "hi" }` puts a
// string under the name `x`, and refuting `x` for it would be refuting the
// wrong claim — the object it built was not made by any `new C()`, so no site
// this pass licenses will ever read it as a proven field.
//
// `Object.assign({}, src)` is the same thing spelled as a call, and it is how
// three.js clones a parameter bag twenty-eight times over; recognising the
// fresh target is what keeps those from refuting every name in the program.
bool freshObjectExpr(const ast::Expr* e) {
    return dynamic_cast<const ast::ObjectLit*>(e) != nullptr ||
           dynamic_cast<const ast::ArrayLit*>(e) != nullptr;
}

// `<base>.<name>` where base is an identifier spelled exactly this way — the
// only receiver shape this pass reads, and only to recognise the handful of
// `Object.*` and `Reflect.*` forms that write properties without an assignment.
bool isMemberOf(const ast::Expr* e, const char* base, std::string& nameOut) {
    const auto* m = dynamic_cast<const ast::MemberAccess*>(e);
    if (m == nullptr) return false;
    const auto* id = dynamic_cast<const ast::Ident*>(m->object.get());
    if (id == nullptr || id->name != base) return false;
    nameOut = m->property;
    return true;
}

const ast::StringLit* asStringLit(const ast::Expr* e) {
    return dynamic_cast<const ast::StringLit*>(e);
}

}  // namespace

bool builtinOwnedName(const std::string& name) {
    for (const char* n : kBuiltinNames) {
        if (name == n) return true;
    }
    // Anything spelled with a leading `@@` or `#` is not a string key a shape
    // carries, and anything spelled as a number is an index.
    return !name.empty() && (name[0] == '#' || name[0] == '@');
}

bool couldBeNumericKey(const std::string& name) {
    if (name.empty()) return true;
    // ToPropertyKey of a Number is ToString of it, which begins with a digit, a
    // minus sign, `I` (Infinity) or `N` (NaN) — never with a letter that starts
    // an ordinary field name, except those two. Erring towards `true` costs a
    // refusal; erring towards `false` would be unsound, so the two words are
    // spelled out rather than approximated.
    if (name == "Infinity" || name == "NaN") return true;
    const char c = name[0];
    return (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '+';
}

// The syntactic half: every write the program performs to a property of an
// object that already exists, plus every construct that can write a name this
// pass cannot see.
class FieldWriteScan final : public Walker {
public:
    explicit FieldWriteScan(FieldAudit& audit) : a_(audit) {}

    void visit(const ast::Binary& n) override {
        if (ast::isAssignOp(n.op)) {
            assignment(n);
        }
        Walker::visit(n);
    }

    void visit(const ast::Unary& n) override {
        if (n.op == ast::UnaryOp::Delete) {
            // A deleted property reads `undefined`, which is not a Number, and
            // no guard can see the difference: the object drops into dictionary
            // mode, the shape guard misses, and a site that had already been
            // told the field is a Number would unbox the hole.
            if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.operand.get())) {
                if (!m->isPrivate) a_.refuse(m->property, "deleted");
            } else if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(n.operand.get())) {
                if (asStringLit(ix->index.get()) != nullptr) {
                    a_.refuse(asStringLit(ix->index.get())->value, "deleted");
                } else if (dynamic_cast<const ast::NumberLit*>(ix->index.get()) != nullptr) {
                    a_.noteNumericKeyWrite();
                } else {
                    a_.recordComputedDelete(ix->object.get(), ix->index.get());
                }
            }
        }
        // `++` and `--` are ToNumeric then one add. On a field this pass has
        // certified the operand is a Number, so the result is a Number: the
        // BigInt branch needs a BigInt operand, which the certification is
        // exactly the denial of. Nothing to record.
        Walker::visit(n);
    }

    void visit(const ast::DestructuringAssign& n) override {
        refuseTargets(n.pattern.get());
        Walker::visit(n);
    }

    void visit(const ast::Call& n) override {
        objectCall(n);
        Walker::visit(n);
    }

    void visit(const ast::NewExpr& n) override {
        if (const auto* id = dynamic_cast<const ast::Ident*>(n.callee.get())) {
            // A proxy answers a property read with a trap that can return
            // anything, and nothing about its shape says so.
            if (id->name == "Proxy") a_.refuseAll("the program builds a Proxy");
            if (id->name == "Function") a_.refuseAll("the program builds a Function from text");
        }
        Walker::visit(n);
    }

    void visit(const ast::Ident& n) override {
        if (n.name == "eval") a_.refuseAll("the program mentions eval");
    }

    void visit(const ast::ClassDecl& n) override {
        classBody(n.methods);
        Walker::visit(n);
    }
    void visit(const ast::ClassExpr& n) override {
        classBody(n.methods);
        Walker::visit(n);
    }

private:
    // A field declaration writes `this.<name>` in the constructor, so it is an
    // ordinary write; a METHOD writes the prototype, which is a fresh object
    // and not one this pass speaks for. An ACCESSOR is neither: it makes the
    // name a call in both directions on every object that inherits it, which is
    // a fact about the name and not about one object.
    void classBody(const std::vector<ast::ClassMethod>& methods) {
        for (const auto& m : methods) {
            if (m.name.empty() || m.isPrivate()) continue;
            if (m.accessor != ast::AccessorKind::None) {
                a_.refuse(m.name, "declared as a class accessor");
            } else if (m.isField && !m.isStatic) {
                a_.record(m.name, m.init.get());
            }
        }
    }

    void refuseTargets(const ast::BindingPattern* pattern) {
        if (pattern == nullptr) return;
        for (const auto& elem : pattern->elements) {
            if (elem.target) {
                // `({ x: o.a } = src)` stores whatever the source's `x` was.
                if (const auto* m = dynamic_cast<const ast::MemberAccess*>(elem.target.get())) {
                    a_.refuse(m->property, "a destructuring assignment target");
                } else {
                    a_.refuseAll("a computed destructuring assignment target");
                }
            }
            refuseTargets(elem.pattern.get());
        }
    }

    void assignment(const ast::Binary& n) {
        // Every assignment operator stores the same kind of thing here. A plain
        // `=` and the logical assigns store the right-hand side; a compound
        // arithmetic assign stores `<the field> op <rhs>`, and with the field
        // certified a Number that is a Number exactly when the rhs is one —
        // the BigInt branch of 13.15.3 needs a BigInt operand, and `+`'s
        // string branch needs a String. So the rhs is what all of them record.
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.lhs.get())) {
            if (!m->isPrivate) a_.record(m->property, n.rhs.get());
            return;
        }
        const auto* ix = dynamic_cast<const ast::IndexAccess*>(n.lhs.get());
        if (ix == nullptr) return;
        if (const auto* s = asStringLit(ix->index.get())) {
            a_.record(s->value, n.rhs.get());
            return;
        }
        if (dynamic_cast<const ast::NumberLit*>(ix->index.get()) != nullptr) {
            // A literal index is an array position. ToPropertyKey of a Number is
            // a canonical NUMERIC STRING, so it can only be a field whose name
            // is spelled as one — `this["0"] = 1` is legal and is the reason
            // this is a narrowing and not a skip.
            a_.noteNumericKeyWrite();
            return;
        }
        a_.recordComputed(ix->object.get(), ix->index.get(), n.rhs.get());
    }

    // `Object.assign`, `Object.defineProperty` and the rest of the family that
    // writes a property without an assignment expression.
    void objectCall(const ast::Call& n) {
        std::string method;
        const bool onObject = isMemberOf(n.callee.get(), "Object", method);
        const bool onReflect = isMemberOf(n.callee.get(), "Reflect", method);
        if (!onObject && !onReflect) {
            // `o.__defineGetter__('x', f)` is the pre-ES5 spelling of the one
            // construct this pass cannot model.
            std::string ignored;
            if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.callee.get())) {
                if (m->property == "__defineGetter__" || m->property == "__defineSetter__") {
                    a_.refuseAll("__defineGetter__/__defineSetter__");
                }
            }
            (void)ignored;
            return;
        }
        if (method == "assign" && !n.args.empty()) {
            assignCall(n);
            return;
        }
        if (method == "defineProperty" && n.args.size() >= 3) {
            defineCall(*n.args[1], *n.args[2]);
            return;
        }
        if (method == "defineProperties" && n.args.size() >= 2) {
            // Readable for the same reason `Object.assign` from a literal is:
            // the map is spelled out, so the writes are enumerable one by one.
            // three.js's `Object3D` constructor installs `position`, `rotation`,
            // `quaternion`, `scale` and six matrices through exactly one of
            // these, and refusing it wholesale refuses every name in the
            // program — this call and its twin were the whole audit's verdict
            // on the library until they were read properly.
            const auto* map = dynamic_cast<const ast::ObjectLit*>(n.args[1].get());
            if (map == nullptr) {
                a_.refuseAll("Object.defineProperties from a map that is not a literal");
                return;
            }
            for (const auto& p : map->props) {
                if (p.computed() || p.accessor != ast::AccessorKind::None || !p.value) {
                    a_.refuseAll("Object.defineProperties with a key this pass cannot read");
                    return;
                }
                defineNamed(p.key, *p.value);
            }
            return;
        }
        // `Object.create(proto)` builds a FRESH object and is harmless; the
        // two-argument form installs descriptors on it that this pass would
        // have to read the same way `defineProperties` is read, and does not.
        if (method == "setPrototypeOf" || (onObject && method == "create" && n.args.size() >= 2) ||
            (onReflect && method == "set")) {
            a_.refuseAll("Object." + method);
        }
    }

    // `Object.assign(target, ...sources)` copies each source's own enumerable
    // properties onto `target`, NAME FOR NAME. Two of the three shapes it comes
    // in are harmless: a fresh target is an object no proven site reads as an
    // instance, and a source that is an object LITERAL contributes exactly the
    // writes that literal spells, which are recorded one by one. Anything else
    // copies names and values this pass cannot enumerate.
    void assignCall(const ast::Call& n) {
        if (freshObjectExpr(n.args[0].get())) return;
        for (size_t i = 1; i < n.args.size(); ++i) {
            const auto* lit = dynamic_cast<const ast::ObjectLit*>(n.args[i].get());
            if (lit == nullptr) {
                a_.refuseAll("Object.assign from a source that is not a literal");
                return;
            }
            for (const auto& p : *&lit->props) {
                if (p.computed()) {
                    a_.refuseAll("Object.assign from a literal with a computed key");
                    return;
                }
                if (p.accessor != ast::AccessorKind::None) {
                    a_.refuse(p.key, "copied from a literal accessor");
                    continue;
                }
                a_.record(p.key, p.value.get());
            }
        }
    }

    void defineCall(const ast::Expr& key, const ast::Expr& descriptor) {
        const auto* name = asStringLit(&key);
        if (name == nullptr) {
            a_.refuseAll("Object.defineProperty under a computed key");
            return;
        }
        defineNamed(name->value, descriptor);
    }

    // One property descriptor. A data descriptor spelled as a literal names the
    // value it installs, which is an ordinary write; anything else — an
    // accessor pair, a descriptor built at run time — makes the name a call or
    // an unknown, and either way not a slot the harvest describes.
    void defineNamed(const std::string& name, const ast::Expr& descriptor) {
        const auto* lit = dynamic_cast<const ast::ObjectLit*>(&descriptor);
        bool accessor = lit == nullptr;
        const ast::Expr* value = nullptr;
        if (lit != nullptr) {
            for (const auto& p : lit->props) {
                if (p.computed() || p.key == "get" || p.key == "set") accessor = true;
                if (p.key == "value") value = p.value.get();
            }
        }
        if (accessor || value == nullptr) {
            a_.refuse(name, "defined through a descriptor this pass cannot read");
            return;
        }
        a_.record(name, value);
    }

    FieldAudit& a_;
};

void FieldAudit::scan(const ast::Module& module) {
    FieldWriteScan walk(*this);
    for (const auto& stmt : module.body) {
        if (stmt) stmt->accept(walk);
    }
    // Every write's right-hand side is a question for the flow pass; entering
    // them all now is what lets `observe` be a lookup that ignores the other
    // hundred thousand expressions in the program.
    for (const auto& w : writes_) {
        if (w.rhs != nullptr) rhsTypes_.emplace(w.rhs, Type::never());
    }
    for (const auto& c : computed_) {
        rhsTypes_.emplace(c.key, Type::never());
        if (c.value != nullptr) rhsTypes_.emplace(c.value, Type::never());
        if (c.receiver != nullptr) rhsTypes_.emplace(c.receiver, Type::never());
    }
}

void FieldAudit::record(const std::string& name, const ast::Expr* rhs) {
    if (name.empty()) return;
    names_.emplace(name, std::string{});
    if (builtinOwnedName(name)) {
        refuse(name, "a name a builtin owns");
        return;
    }
    writes_.push_back(Write{name, rhs});
    if (rhs == nullptr) refuse(name, "written by a form with no expression to type");
}

void FieldAudit::recordComputed(const ast::Expr* receiver, const ast::Expr* key,
                                const ast::Expr* value) {
    computed_.push_back(Computed{receiver, key, value, /*isDelete=*/false, /*refuted=*/false});
}

void FieldAudit::recordComputedDelete(const ast::Expr* receiver, const ast::Expr* key) {
    computed_.push_back(
        Computed{receiver, key, /*value=*/nullptr, /*isDelete=*/true, /*refuted=*/false});
}

void FieldAudit::observe(const ast::Expr* rhs, Type t) {
    const auto it = rhsTypes_.find(rhs);
    if (it == rhsTypes_.end()) return;
    it->second = join(it->second, t);
}

void FieldAudit::refuse(const std::string& name, std::string why) {
    if (name.empty()) return;
    auto& slot = names_[name];
    if (slot.empty()) slot = std::move(why);
}

void FieldAudit::refuseAll(std::string why) {
    ++globalRefusals_[std::move(why)];
}

Type FieldAudit::typeOfExpr(const ast::Expr* e) const {
    if (e == nullptr) return Type::dynamic();
    const auto it = rhsTypes_.find(e);
    // An expression the scan registered but no round has typed answers `Never`,
    // which is what "undecided" is spelled as here; one it never registered
    // could be anything.
    return it == rhsTypes_.end() ? Type::dynamic() : it->second;
}

bool FieldAudit::settle() {
    const size_t before = refusedCount();
    const size_t globalsBefore = globalRefusals_.size();
    const uint32_t computedBefore = computedRefuted_;

    // A computed write is harmless in exactly two cases, and both need a type:
    // a key the flow pass proved is a Number can only produce a numeric string,
    // which is never an ordinary field name; and a value it proved is a Number
    // preserves the invariant whatever name it lands on. A computed DELETE has
    // only the first of the two — there is no value, and the hole it leaves is
    // not a Number.
    //
    // `Never` on either is not evidence. The first round types nothing, so a
    // rule that refused on `Never` refused every computed site in the program
    // before a single expression had been walked — and the refusal is sticky,
    // so that verdict was final and its stated reason ("the key and value are
    // both unproven") was untrue of most of the sites it named. A settled
    // `Never` is dead code, whose write can refute nothing because it never
    // runs, which is exactly the rule the named writes below already keep.
    for (auto& c : computed_) {
        if (c.refuted) continue;
        const Type key = typeOfExpr(c.key);
        if (key.is(TypeKind::Number)) {
            numericKeyWrite_ = true;
            continue;
        }
        if (key.is(TypeKind::Never)) continue;
        if (!c.isDelete) {
            const Type val = typeOfExpr(c.value);
            if (val.is(TypeKind::Number) || val.is(TypeKind::Never)) continue;
        }
        c.refuted = true;
        ++computedRefuted_;
        refuseAll(c.isDelete ? "delete through a computed key"
                             : "a computed write whose key and value are both unproven");
    }

    for (const auto& w : writes_) {
        if (w.rhs == nullptr) continue;
        const auto it = rhsTypes_.find(w.rhs);
        if (it == rhsTypes_.end()) {
            refuse(w.name, "a write the flow pass never reached");
            continue;
        }
        // `Never` is a write no round has typed yet — the first round types
        // nothing — and is not evidence either way, so it is left alone and the
        // next round decides. A settled `Never` is dead code, whose write can
        // refute nothing because it never runs.
        if (it->second.is(TypeKind::Never) || it->second.is(TypeKind::Number)) continue;
        refuse(w.name, "written as " + it->second.str());
    }

    // `numericKeyWrite_` is not in the change test: it only ever refutes names
    // that are spelled as numbers, and `refusedCount` does not see those
    // (`numberClean` applies the bit at the query). Turning it on late cannot
    // therefore un-settle anything a later round would have decided
    // differently — every consumer reads it through `numberClean`, which reads
    // it fresh.
    return refusedCount() != before || globalRefusals_.size() != globalsBefore ||
           computedRefuted_ != computedBefore;
}

std::map<std::string, uint32_t> FieldAudit::computedKeyTypes() const {
    std::map<std::string, uint32_t> out;
    for (const auto& c : computed_) {
        if (!c.refuted) continue;
        ++out[typeOfExpr(c.key).str() + (c.isDelete ? " (delete)" : "")];
    }
    return out;
}

std::map<std::string, uint32_t> FieldAudit::computedReceiverTypes() const {
    std::map<std::string, uint32_t> out;
    for (const auto& c : computed_) {
        if (!c.refuted) continue;
        ++out[typeOfExpr(c.receiver).str() + (c.isDelete ? " (delete)" : "")];
    }
    return out;
}

uint32_t FieldAudit::cleanCount() const {
    if (!globalRefusals_.empty()) return 0;
    return locallyCleanCount();
}

uint32_t FieldAudit::locallyCleanCount() const {
    return static_cast<uint32_t>(names_.size() - refusedCount());
}

size_t FieldAudit::refusedCount() const {
    size_t n = 0;
    for (const auto& e : names_) {
        if (!e.second.empty()) ++n;
    }
    return n;
}

bool FieldAudit::numberClean(const std::string& name) const {
    if (!globalRefusals_.empty()) return false;
    if (numericKeyWrite_ && couldBeNumericKey(name)) return false;
    const auto it = names_.find(name);
    // A name nothing writes is not a field this pass can speak for either: a
    // class whose constructor installs it writes it, so an unwritten name is
    // one that reaches the heap by a road this pass did not model.
    if (it == names_.end()) return false;
    return it->second.empty();
}

std::string FieldAudit::refusalFor(const std::string& name) const {
    if (!globalRefusals_.empty()) return globalRefusals_.begin()->first;
    const auto it = names_.find(name);
    if (it == names_.end()) return "never written by the program";
    return it->second;
}

std::vector<std::pair<std::string, std::string>> FieldAudit::report() const {
    std::vector<std::pair<std::string, std::string>> rows(names_.begin(), names_.end());
    std::sort(rows.begin(), rows.end());
    return rows;
}

}  // namespace bronze::types
