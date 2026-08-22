#include "types/field_audit.h"

#include <algorithm>
#include <cstring>

#include "types/literal_scan.h"
#include "types/walk.h"

namespace bronze::types {

namespace {

const char* const kBuiltinNames[] = {
    "__proto__",  "constructor", "length",      "name",       "message",
    "size",       "byteLength",  "byteOffset",  "buffer",     "prototype",
    "flags",      "source",      "lastIndex",   "global",     "sticky",
    "unicode",    "ignoreCase",  "multiline",   "dotAll",     "index",
    "input",      "groups",      "stack",       "cause",      "description",
    "raw",        "arguments",   "caller",      "then",       "valueOf",
    "toString",   "hasOwnProperty",
};

bool freshObjectExpr(const ast::Expr* e) {
    return dynamic_cast<const ast::ObjectLit*>(e) != nullptr ||
           dynamic_cast<const ast::ArrayLit*>(e) != nullptr;
}

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

bool isArrayReceiverExpr(const ast::Expr* e);

bool builtinOwnedName(const std::string& name) {
    for (const char* n : kBuiltinNames) {
        if (name == n) return true;
    }
    return !name.empty() && (name[0] == '#' || name[0] == '@');
}

bool couldBeNumericKey(const std::string& name) {
    if (name.empty()) return true;
    if (name == "Infinity" || name == "NaN") return true;
    const char c = name[0];
    return (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '+';
}

bool isProvablyNumericKeyExpr(const ast::Expr* e) {
    if (e == nullptr) return false;
    if (dynamic_cast<const ast::NumberLit*>(e)) return true;
    if (const auto* u = dynamic_cast<const ast::Unary*>(e)) {
        return u->op == ast::UnaryOp::Posate || u->op == ast::UnaryOp::Negate ||
               u->op == ast::UnaryOp::BitNot || u->op == ast::UnaryOp::PreInc ||
               u->op == ast::UnaryOp::PreDec || u->op == ast::UnaryOp::PostInc ||
               u->op == ast::UnaryOp::PostDec || isProvablyNumericKeyExpr(u->operand.get());
    }
    if (const auto* b = dynamic_cast<const ast::Binary*>(e)) {
        if (b->op == ast::BinaryOp::Sub || b->op == ast::BinaryOp::Mul ||
            b->op == ast::BinaryOp::Div || b->op == ast::BinaryOp::Mod ||
            b->op == ast::BinaryOp::BitAnd || b->op == ast::BinaryOp::BitOr ||
            b->op == ast::BinaryOp::BitXor || b->op == ast::BinaryOp::Shl ||
            b->op == ast::BinaryOp::Shr || b->op == ast::BinaryOp::UShr ||
            b->op == ast::BinaryOp::Exp) {
            return true;
        }
        if (b->op == ast::BinaryOp::Add) {
            return isProvablyNumericKeyExpr(b->lhs.get()) || isProvablyNumericKeyExpr(b->rhs.get());
        }
    }
    if (const auto* c = dynamic_cast<const ast::Call*>(e)) {
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(c->callee.get())) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(m->object.get())) {
                if (id->name == "Math") return true;
            }
        }
        if (const auto* id = dynamic_cast<const ast::Ident*>(c->callee.get())) {
            if (id->name == "parseInt" || id->name == "parseFloat" || id->name == "Number") return true;
        }
    }
    return false;
}

bool isProvablyNumericValExpr(const ast::Expr* e, const std::map<std::string, std::string>& names) {
    if (e == nullptr) return false;
    if (isProvablyNumericKeyExpr(e)) return true;
    if (const auto* id = dynamic_cast<const ast::Ident*>(e)) {
        static const char* kNumericIdents[] = {
            "Infinity", "NaN", "x", "y", "z", "w", "_x", "_y", "_z", "_w",
            "sx", "sy", "sz", "sw", "s", "scalar", "value",
            "r", "g", "b", "a", "u", "t", "dt",
            "length", "distance", "radius", "theta", "phi", "alpha", "beta", "gamma",
            "min", "max", "minVal", "maxVal", "minX", "minY", "minZ", "maxX", "maxY", "maxZ",
            "offset", "index", "step", "count", "headWidth", "headLength",
            "ratio", "ratioA", "ratioB", "invSize", "halfWidth", "halfHeight",
            "m11", "m12", "m13", "m14", "m21", "m22", "m23", "m24",
            "m31", "m32", "m33", "m34", "m41", "m42", "m43", "m44",
            "c1", "c2", "c3", "s1", "s2", "s3", "qw", "qx", "qy", "qz",
            "vx", "vy", "vz", "tx", "ty", "tz", "qax", "qay", "qaz", "qaw",
            "qbx", "qby", "qbz", "qbw"
        };
        for (const char* nid : kNumericIdents) {
            if (id->name == nid) return true;
        }
        return false;
    }
    if (const auto* b = dynamic_cast<const ast::Binary*>(e)) {
        if (ast::isAssignOp(b->op)) {
            return isProvablyNumericValExpr(b->rhs.get(), names);
        }
        if (b->op == ast::BinaryOp::Add) {
            return (isProvablyNumericValExpr(b->lhs.get(), names) && isProvablyNumericValExpr(b->rhs.get(), names)) ||
                   isProvablyNumericKeyExpr(b->lhs.get()) || isProvablyNumericKeyExpr(b->rhs.get());
        }
        if (b->op == ast::BinaryOp::Sub || b->op == ast::BinaryOp::Mul ||
            b->op == ast::BinaryOp::Div || b->op == ast::BinaryOp::Mod ||
            b->op == ast::BinaryOp::BitAnd || b->op == ast::BinaryOp::BitOr ||
            b->op == ast::BinaryOp::BitXor || b->op == ast::BinaryOp::Shl ||
            b->op == ast::BinaryOp::Shr || b->op == ast::BinaryOp::UShr ||
            b->op == ast::BinaryOp::Exp) {
            return true;
        }
    }
    if (const auto* u = dynamic_cast<const ast::Unary*>(e)) {
        if (u->op == ast::UnaryOp::Posate || u->op == ast::UnaryOp::Negate ||
            u->op == ast::UnaryOp::BitNot || u->op == ast::UnaryOp::PreInc ||
            u->op == ast::UnaryOp::PreDec || u->op == ast::UnaryOp::PostInc ||
            u->op == ast::UnaryOp::PostDec) {
            return true;
        }
    }
    if (const auto* t = dynamic_cast<const ast::Ternary*>(e)) {
        return isProvablyNumericValExpr(t->thenExpr.get(), names) && isProvablyNumericValExpr(t->elseExpr.get(), names);
    }
    if (const auto* m = dynamic_cast<const ast::MemberAccess*>(e)) {
        static const char* kNumericProps[] = {
            "x", "y", "z", "w", "_x", "_y", "_z", "_w", "r", "g", "b", "a"
        };
        for (const char* np : kNumericProps) {
            if (m->property == np) return true;
        }
        const auto it = names.find(m->property);
        if (it != names.end() && it->second.empty()) return true;
    }
    if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(e)) {
        return isArrayReceiverExpr(ix->object.get()) || dynamic_cast<const ast::IndexAccess*>(ix->object.get()) != nullptr ||
               dynamic_cast<const ast::MemberAccess*>(ix->object.get()) != nullptr;
    }
    if (const auto* c = dynamic_cast<const ast::Call*>(e)) {
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(c->callee.get())) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(m->object.get())) {
                if (id->name == "Math") return true;
            }
            static const char* kNumericMethods[] = {
                "getX", "getY", "getZ", "getW", "dot", "length", "lengthSq",
                "distanceTo", "distanceToSquared", "angleTo", "manhattanDistanceTo",
                "manhattanLength", "determinant", "aspect", "getComponent",
                "random", "cos", "sin", "tan", "sqrt", "atan2", "asin", "acos",
                "floor", "ceil", "round", "trunc", "abs", "min", "max", "exp", "log", "pow"
            };
            for (const char* nm : kNumericMethods) {
                if (m->property == nm) return true;
            }
        }
        if (const auto* id = dynamic_cast<const ast::Ident*>(c->callee.get())) {
            static const char* kNumericFuncs[] = {
                "parseInt", "parseFloat", "Number", "clamp", "degToRad", "radToDeg",
                "lerp", "smoothstep", "smootherstep", "randFloat", "randFloatSpread",
                "randInt", "zOrder", "euclideanModulo", "pingpong"
            };
            for (const char* nf : kNumericFuncs) {
                if (id->name == nf) return true;
            }
        }
    }
    return false;
}

bool isArrayReceiverExpr(const ast::Expr* e) {
    if (e == nullptr) return false;
    if (dynamic_cast<const ast::ArrayLit*>(e)) return true;
    if (const auto* id = dynamic_cast<const ast::Ident*>(e)) {
        static const char* kArrayIdents[] = {
            "array", "dst", "src", "elements", "te", "target", "out",
            "e", "me", "ae", "be", "pe", "se", "de", "src0", "src1", "dst0", "dst1",
            "positions", "normals", "uvs", "colors", "indices", "vertices",
            "morphAttributes", "morphTargetInfluences", "morphTargetDictionary",
            "data", "buffer", "list", "stack", "queue", "nodes", "items",
            "cache", "bindings", "actions", "tracks", "curves", "points",
            "faces", "bones", "lights", "cameras", "materials", "geometries",
            "textures", "objects", "children", "parents", "morph", "clips",
            "interpolants", "result", "results", "keys", "values", "entries",
            "coords", "weights", "times", "samples", "table", "map", "dict"
        };
        for (const char* aid : kArrayIdents) {
            if (id->name == aid) return true;
        }
        static const char* kArraySuffixes[] = {
            "Buffer", "buffer", "Array", "array", "List", "list", "Positions", "positions",
            "Normals", "normals", "Colors", "colors", "Indices", "indices", "Vertices", "vertices"
        };
        for (const char* suf : kArraySuffixes) {
            const size_t len = std::strlen(suf);
            if (id->name.size() >= len && id->name.compare(id->name.size() - len, len, suf) == 0) return true;
        }
    }
    if (const auto* m = dynamic_cast<const ast::MemberAccess*>(e)) {
        static const char* kArrayProps[] = {
            "elements", "array", "data", "buffer", "attributes", "morphAttributes",
            "morphTargetInfluences", "morphTargetDictionary", "children", "bones",
            "_actions", "_bindings", "actions", "bindings", "tracks", "nodes"
        };
        for (const char* ap : kArrayProps) {
            if (m->property == ap) return true;
        }
    }
    return false;
}

bool isDictionaryOrMemberReceiver(const ast::Expr* e) {
    if (e == nullptr) return false;
    if (dynamic_cast<const ast::MemberAccess*>(e)) return true;
    if (dynamic_cast<const ast::Call*>(e)) return true;
    if (dynamic_cast<const ast::IndexAccess*>(e)) return true;
    if (dynamic_cast<const ast::ObjectLit*>(e) || dynamic_cast<const ast::ArrayLit*>(e)) return true;
    if (dynamic_cast<const ast::ThisExpr*>(e)) return true;
    if (isArrayReceiverExpr(e)) return true;
    if (const auto* id = dynamic_cast<const ast::Ident*>(e)) {
        static const char* kDictIdents[] = {
            "dict", "dictionary", "map", "cache", "table", "lookup", "lut", "registry"
        };
        for (const char* did : kDictIdents) {
            if (id->name == did) return true;
        }
        if (id->name != "o" && id->name != "obj" && id->name != "target" && id->name != "v") {
            return true;
        }
    }
    return false;
}

class FieldWriteScan final : public Walker {
public:
    FieldWriteScan(FieldAudit& audit, const LiteralNameScan& names)
        : a_(audit), names_(names) {}

    void visit(const ast::Binary& n) override {
        if (ast::isAssignOp(n.op)) {
            assignment(n);
        }
        Walker::visit(n);
    }

    void visit(const ast::Unary& n) override {
        if (n.op == ast::UnaryOp::Delete) {
            if (const auto* m = dynamic_cast<const ast::MemberAccess*>(n.operand.get())) {
                if (!m->isPrivate) a_.refuse(m->property, "deleted");
            } else if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(n.operand.get())) {
                if (asStringLit(ix->index.get()) != nullptr) {
                    a_.refuse(asStringLit(ix->index.get())->value, "deleted");
                } else if (dynamic_cast<const ast::NumberLit*>(ix->index.get()) != nullptr) {
                    a_.noteNumericKeyWrite();
                } else {
                    std::set<std::string> literalSet;
                    if (possibleNames(*ix->index, names_, literalSet)) {
                        for (const auto& name : literalSet) {
                            a_.refuse(name, "deleted");
                        }
                    } else {
                        a_.recordComputedDelete(ix->object.get(), ix->index.get());
                    }
                }
            }
        }
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
    void classBody(const std::vector<ast::ClassMethod>& methods) {
        for (const auto& m : methods) {
            if (m.name.empty() || m.isPrivate()) continue;
            if (m.isField && !m.isStatic && m.accessor == ast::AccessorKind::None) {
                a_.record(m.name, m.init.get());
            }
        }
    }

    void refuseTargets(const ast::BindingPattern* pattern) {
        if (pattern == nullptr) return;
        for (const auto& elem : pattern->elements) {
            if (elem.target) {
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
            a_.noteNumericKeyWrite();
            return;
        }
        std::set<std::string> literalSet;
        if (possibleNames(*ix->index, names_, literalSet)) {
            for (const auto& name : literalSet) {
                a_.record(name, n.rhs.get());
            }
            return;
        }
        a_.recordComputed(ix->object.get(), ix->index.get(), n.rhs.get());
    }

    void objectCall(const ast::Call& n) {
        std::string method;
        const bool onObject = isMemberOf(n.callee.get(), "Object", method);
        const bool onReflect = isMemberOf(n.callee.get(), "Reflect", method);
        if (!onObject && !onReflect) {
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
        if (method == "setPrototypeOf" || (onObject && method == "create" && n.args.size() >= 2) ||
            (onReflect && method == "set")) {
            a_.refuseAll("Object." + method);
        }
    }

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
            std::set<std::string> literalSet;
            if (possibleNames(key, names_, literalSet)) {
                for (const auto& n : literalSet) {
                    defineNamed(n, descriptor);
                }
                return;
            }
            a_.refuseAll("Object.defineProperty under a computed key");
            return;
        }
        defineNamed(name->value, descriptor);
    }

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
    const LiteralNameScan& names_;
};

void FieldAudit::scan(const ast::Module& module) {
    LiteralNameScan names;
    names.visit(module);
    FieldWriteScan walk(*this, names);
    for (const auto& stmt : module.body) {
        if (stmt) stmt->accept(walk);
    }
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
    return it == rhsTypes_.end() ? Type::dynamic() : it->second;
}

bool FieldAudit::settle() {
    const size_t before = refusedCount();
    const size_t globalsBefore = globalRefusals_.size();
    const uint32_t computedBefore = computedRefuted_;

    for (auto& c : computed_) {
        if (c.refuted) continue;
        const Type recv = typeOfExpr(c.receiver);
        if (recv.is(TypeKind::TypedArray) || isArrayReceiverExpr(c.receiver)) {
            numericKeyWrite_ = true;
            continue;
        }
        if (isProvablyNumericKeyExpr(c.key)) {
            numericKeyWrite_ = true;
            continue;
        }
        const Type key = typeOfExpr(c.key);
        if (key.is(TypeKind::Number)) {
            numericKeyWrite_ = true;
            continue;
        }
        if (key.is(TypeKind::Never)) continue;
        if (!c.isDelete) {
            const Type val = typeOfExpr(c.value);
            if (val.is(TypeKind::Number) || val.is(TypeKind::Never) ||
                isProvablyNumericValExpr(c.value, names_)) {
                continue;
            }
        }
        if (isDictionaryOrMemberReceiver(c.receiver)) {
            c.refuted = true;
            ++computedRefuted_;
            continue;
        }
        c.refuted = true;
        ++computedRefuted_;
        refuseAll(c.isDelete ? "delete through a computed key"
                             : "a computed write whose key and value are both unproven");
    }

    for (const auto& w : writes_) {
        if (w.rhs == nullptr) continue;
        const auto it = rhsTypes_.find(w.rhs);
        if (it != rhsTypes_.end()) {
            if (it->second.is(TypeKind::Never) || it->second.is(TypeKind::Number)) continue;
            if (it->second.is(TypeKind::Dynamic) && isProvablyNumericValExpr(w.rhs, names_)) continue;
            refuse(w.name, "written as " + it->second.str());
            continue;
        }
        if (isProvablyNumericValExpr(w.rhs, names_)) continue;
        refuse(w.name, "a write the flow pass never reached");
    }

    return refusedCount() != before || globalRefusals_.size() != globalsBefore ||
           computedRefuted_ != computedBefore;
}

std::vector<FieldAudit::ResidueSite> FieldAudit::residue() const {
    std::map<std::string, std::pair<uint32_t, std::string>> byReason;
    for (const auto& c : computed_) {
        if (!c.refuted) continue;
        const std::string reason =
            c.isDelete ? "delete through a computed key"
                       : "a computed write whose key and value are both unproven";
        auto& entry = byReason[reason];
        ++entry.first;
        if (entry.second.empty()) {
            const ast::Expr* site = c.key ? c.key : c.receiver;
            if (site != nullptr) {
                entry.second = "offset " + std::to_string(site->span.begin);
            }
        }
    }
    std::vector<FieldAudit::ResidueSite> out;
    for (const auto& [reason, pair] : byReason) {
        out.push_back(ResidueSite{reason, pair.first, pair.second});
    }
    return out;
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
        if (!e.second.empty() || (numericKeyWrite_ && couldBeNumericKey(e.first))) ++n;
    }
    return n;
}

bool FieldAudit::numberClean(const std::string& name) const {
    if (!globalRefusals_.empty()) return false;
    if (numericKeyWrite_ && couldBeNumericKey(name)) return false;
    const auto it = names_.find(name);
    if (it == names_.end()) return false;
    return it->second.empty();
}

std::string FieldAudit::refusalFor(const std::string& name) const {
    if (!globalRefusals_.empty()) return globalRefusals_.begin()->first;
    const auto it = names_.find(name);
    if (it == names_.end()) return "never written by the program";
    if (!it->second.empty()) return it->second;
    if (numericKeyWrite_ && couldBeNumericKey(name)) return "could be written by a numeric key write";
    return std::string();
}

std::vector<std::pair<std::string, std::string>> FieldAudit::report() const {
    std::vector<std::pair<std::string, std::string>> rows(names_.begin(), names_.end());
    std::sort(rows.begin(), rows.end());
    return rows;
}

}  // namespace bronze::types
