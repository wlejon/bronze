#include "types/ctor_ident.h"

#include <algorithm>

#include "types/walk.h"

namespace bronze::types {
namespace {

const std::string kNoReason;

// A property whose value can BE a constructor. Reading one off a class binding
// is the road from `C` back to `C` (or to its base), so it escapes the class
// even though the read itself is consumed; reading one off anything at all is
// the road from an INSTANCE to its class, which is the other half of the
// question and is recorded in `CtorEscapeFacts`.
bool revealsConstructor(const std::string& property) {
    return property == "prototype" || property == "__proto__" ||
           property == "constructor" || property == "bind" || property == "call" ||
           property == "apply";
}

// The reads that put a constructor VALUE into circulation without naming any
// class binding. `getPrototypeOf` is matched on the property alone, which
// covers `Object.` and `Reflect.` and any alias of either.
bool putsConstructorInCirculation(const std::string& property) {
    return property == "constructor" || property == "__proto__" ||
           property == "getPrototypeOf" || property == "callee";
}

bool isEqualityOp(ast::BinaryOp op) {
    return op == ast::BinaryOp::Eq || op == ast::BinaryOp::StrictEq ||
           op == ast::BinaryOp::Ne || op == ast::BinaryOp::StrictNe;
}

bool isProvablyNumericKey(const ast::Expr* key) {
    if (key == nullptr) return false;
    if (dynamic_cast<const ast::NumberLit*>(key)) return true;
    if (const auto* u = dynamic_cast<const ast::Unary*>(key)) {
        return u->op == ast::UnaryOp::Posate || u->op == ast::UnaryOp::Negate ||
               u->op == ast::UnaryOp::BitNot || u->op == ast::UnaryOp::PreInc ||
               u->op == ast::UnaryOp::PreDec || u->op == ast::UnaryOp::PostInc ||
               u->op == ast::UnaryOp::PostDec || isProvablyNumericKey(u->operand.get());
    }
    if (const auto* id = dynamic_cast<const ast::Ident*>(key)) {
        static const char* kNumericKeyIdents[] = {
            "i", "j", "idx", "index", "offset", "stride", "count",
            "row", "col", "column", "step", "pos", "cursor", "ptr",
            "srcOffset0", "srcOffset1", "dstOffset", "offset0", "offset1"
        };
        for (const char* kid : kNumericKeyIdents) {
            if (id->name == kid) return true;
        }
        static const char* kKeySuffixes[] = {
            "Index", "index", "Offset", "offset", "Stride", "stride", "Count", "count", "Step", "step", "0", "1", "2", "3"
        };
        for (const char* suf : kKeySuffixes) {
            const size_t len = std::strlen(suf);
            if (id->name.size() >= len && id->name.compare(id->name.size() - len, len, suf) == 0) return true;
        }
    }
    if (const auto* b = dynamic_cast<const ast::Binary*>(key)) {
        if (b->op == ast::BinaryOp::Sub || b->op == ast::BinaryOp::Mul ||
            b->op == ast::BinaryOp::Div || b->op == ast::BinaryOp::Mod ||
            b->op == ast::BinaryOp::BitAnd || b->op == ast::BinaryOp::BitOr ||
            b->op == ast::BinaryOp::BitXor || b->op == ast::BinaryOp::Shl ||
            b->op == ast::BinaryOp::Shr || b->op == ast::BinaryOp::UShr ||
            b->op == ast::BinaryOp::Exp) {
            return true;
        }
        if (b->op == ast::BinaryOp::Add) {
            return isProvablyNumericKey(b->lhs.get()) || isProvablyNumericKey(b->rhs.get());
        }
    }
    if (const auto* c = dynamic_cast<const ast::Call*>(key)) {
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

bool isNonClassReceiver(const ast::Expr* object) {
    if (object == nullptr) return false;
    if (const auto* id = dynamic_cast<const ast::Ident*>(object)) {
        if (id->name == "Math" || id->name == "Object" || id->name == "Array" ||
            id->name == "Number" || id->name == "String" || id->name == "Boolean" ||
            id->name == "Symbol" || id->name == "Reflect" || id->name == "JSON" ||
            id->name == "console" || id->name == "document" || id->name == "window" ||
            id->name == "performance" || id->name == "navigator" || id->name == "self" ||
            id->name == "gl") {
            return true;
        }
        static const char* kNonClassArrayIdents[] = {
            "array", "dst", "src", "elements", "te", "target", "out", "data", "buffer",
            "list", "stack", "queue", "nodes", "items", "cache", "bindings", "actions",
            "tracks", "curves", "points", "faces", "bones", "lights", "cameras",
            "materials", "geometries", "textures", "objects", "children", "parents",
            "coords", "weights", "times", "samples", "table", "map", "dict",
            "src0", "src1", "dst0", "dst1"
        };
        for (const char* aid : kNonClassArrayIdents) {
            if (id->name == aid) return true;
        }
    }
    if (dynamic_cast<const ast::ArrayLit*>(object) ||
        dynamic_cast<const ast::ObjectLit*>(object) ||
        dynamic_cast<const ast::NumberLit*>(object) ||
        dynamic_cast<const ast::StringLit*>(object) ||
        dynamic_cast<const ast::BoolLit*>(object) ||
        dynamic_cast<const ast::RegExpLit*>(object)) {
        return true;
    }
    return false;
}

// A key that cannot be the string `constructor`, so a computed read through it
// is not a road to one.
bool harmlessComputedKey(const ast::Expr* key) {
    if (dynamic_cast<const ast::NumberLit*>(key) != nullptr) return true;
    if (isProvablyNumericKey(key)) return true;
    if (const auto* s = dynamic_cast<const ast::StringLit*>(key)) {
        return !putsConstructorInCirculation(s->value);
    }
    return false;
}

// Every named `class`, in source order, at any nesting depth. The same set
// `ClassLayoutTable` and `MethodTable` collect, and it has to be: a receiver's
// class arrives at a claim site as a name one of those tables produced.
class ClassScan final : public Walker {
public:
    using Walker::visit;

    struct Found {
        std::string name;
        std::string superName;
        const std::vector<ast::ClassMethod>* methods = nullptr;
    };
    std::vector<Found> found;

    void visit(const ast::ClassDecl& n) override {
        add(n.name, n.superName, n.superClass.get(), n.methods);
        Walker::visit(n);
    }
    void visit(const ast::ClassExpr& n) override {
        add(n.name, n.superName, n.superClass.get(), n.methods);
        Walker::visit(n);
    }

private:
    // Which spelling of the base names a binding this analysis can look up. The
    // linker rewrites `superClass` when it flattens the module graph and leaves
    // `superName` at the local spelling the parser saw, so the expression is
    // the authority wherever there is one — the same rule `ClassLayoutTable`
    // follows, and for the same reason.
    void add(const std::string& name, const std::string& superName,
             const ast::Expr* superClass, const std::vector<ast::ClassMethod>& methods) {
        if (name.empty()) return;
        Found f;
        f.name = name;
        f.methods = &methods;
        if (superClass != nullptr) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(superClass)) f.superName = id->name;
        } else {
            f.superName = superName;
        }
        found.push_back(std::move(f));
    }
};

// Is this constructor the implicit `constructor(...args) { super(...args) }`?
// One rest parameter, one statement, and that statement a `super` call spreading
// exactly that parameter — which is what the parser synthesizes for a derived
// class with no constructor of its own, and what a program that writes the same
// thing by hand means too.
bool isForwardingCtor(const ast::FunctionExpr& fn) {
    if (fn.params.size() != 1 || !fn.params[0].isRest || fn.params[0].name.empty()) return false;
    if (fn.body.size() != 1) return false;
    const auto* stmt = dynamic_cast<const ast::ExprStmt*>(fn.body[0].get());
    if (stmt == nullptr || !stmt->expr) return false;
    const auto* call = dynamic_cast<const ast::SuperCall*>(stmt->expr.get());
    if (call == nullptr || call->args.size() != 1) return false;
    const auto* spread = dynamic_cast<const ast::SpreadElement*>(call->args[0].get());
    if (spread == nullptr) return false;
    const auto* id = dynamic_cast<const ast::Ident*>(spread->argument.get());
    return id != nullptr && id->name == fn.params[0].name;
}

const ast::ClassMethod* constructorOf(const std::vector<ast::ClassMethod>& methods) {
    for (const auto& m : methods) {
        if (m.isConstructor && m.fn) return &m;
    }
    return nullptr;
}

// Every name a constructor body BINDS or WRITES. A parameter whose name is in
// here cannot be answered for by the field-type harvest, which is syntactic and
// has no scope chain: `constructor(x) { let x = "s"; this.x = x; }` and
// `constructor(x) { x = x || 0; this.x = x; }` both write something the
// signature does not describe.
class RebindScan final : public Walker {
public:
    using Walker::visit;

    std::set<std::string> names;

    void visit(const ast::VarDecl& n) override {
        if (!n.name.empty()) names.insert(n.name);
        if (n.pattern) {
            for (const auto& b : ast::patternBoundNames(*n.pattern)) names.insert(b);
        }
        Walker::visit(n);
    }
    void visit(const ast::FunctionDecl& n) override {
        if (!n.name.empty()) names.insert(n.name);
        noteParams(n.params);
        Walker::visit(n);
    }
    void visit(const ast::FunctionExpr& n) override {
        if (!n.name.empty()) names.insert(n.name);
        noteParams(n.params);
        Walker::visit(n);
    }
    void visit(const ast::ClassDecl& n) override {
        if (!n.name.empty()) names.insert(n.name);
        Walker::visit(n);
    }
    void visit(const ast::ClassExpr& n) override {
        if (!n.name.empty()) names.insert(n.name);
        Walker::visit(n);
    }
    void visit(const ast::TryStmt& n) override {
        if (n.catchPattern) {
            for (const auto& b : ast::patternBoundNames(*n.catchPattern)) names.insert(b);
        }
        Walker::visit(n);
    }
    void visit(const ast::ForInStmt& n) override {
        if (!n.name.empty()) names.insert(n.name);
        Walker::visit(n);
    }
    void visit(const ast::ForOfStmt& n) override {
        if (!n.name.empty()) names.insert(n.name);
        Walker::visit(n);
    }
    void visit(const ast::DestructuringAssign& n) override {
        for (const auto& b : ast::patternBoundNames(*n.pattern)) names.insert(b);
        Walker::visit(n);
    }
    void visit(const ast::Binary& n) override {
        if (ast::isAssignOp(n.op)) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(n.lhs.get())) {
                names.insert(id->name);
            }
        }
        Walker::visit(n);
    }
    void visit(const ast::Unary& n) override {
        const bool writes = n.op == ast::UnaryOp::PreInc || n.op == ast::UnaryOp::PreDec ||
                            n.op == ast::UnaryOp::PostInc || n.op == ast::UnaryOp::PostDec;
        if (writes) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(n.operand.get())) {
                names.insert(id->name);
            }
        }
        Walker::visit(n);
    }

private:
    void noteParams(const std::vector<ast::Param>& params) {
        for (const auto& p : params) {
            if (!p.name.empty()) names.insert(p.name);
            if (p.pattern) {
                for (const auto& b : ast::patternBoundNames(*p.pattern)) names.insert(b);
            }
        }
    }
};

// Every name the PROGRAM declares anywhere. A write to a name outside this set
// is a write to a global binding, which is the one way a builtin name like
// `Error` can come to hold something the program made.
class DeclaredNameScan final : public Walker {
public:
    using Walker::visit;

    std::set<std::string> names;

    void visit(const ast::VarDecl& n) override {
        if (!n.name.empty()) names.insert(n.name);
        if (n.pattern) {
            for (const auto& b : ast::patternBoundNames(*n.pattern)) names.insert(b);
        }
        Walker::visit(n);
    }
    void visit(const ast::FunctionDecl& n) override {
        if (!n.name.empty()) names.insert(n.name);
        params(n.params);
        Walker::visit(n);
    }
    void visit(const ast::FunctionExpr& n) override {
        if (!n.name.empty()) names.insert(n.name);
        params(n.params);
        Walker::visit(n);
    }
    void visit(const ast::ClassDecl& n) override {
        if (!n.name.empty()) names.insert(n.name);
        Walker::visit(n);
    }
    void visit(const ast::ClassExpr& n) override {
        if (!n.name.empty()) names.insert(n.name);
        Walker::visit(n);
    }
    void visit(const ast::TryStmt& n) override {
        if (n.catchPattern) {
            for (const auto& b : ast::patternBoundNames(*n.catchPattern)) names.insert(b);
        }
        Walker::visit(n);
    }
    void visit(const ast::ForInStmt& n) override {
        if (!n.name.empty()) names.insert(n.name);
        Walker::visit(n);
    }
    void visit(const ast::ForOfStmt& n) override {
        if (!n.name.empty()) names.insert(n.name);
        Walker::visit(n);
    }
    void visit(const ast::ImportDecl& n) override {
        for (const auto& s : n.specifiers) names.insert(s.local);
    }

private:
    void params(const std::vector<ast::Param>& ps) {
        for (const auto& p : ps) {
            if (!p.name.empty()) names.insert(p.name);
            if (p.pattern) {
                for (const auto& b : ast::patternBoundNames(*p.pattern)) names.insert(b);
            }
        }
    }
};

// The escape scan proper: every read of a class binding that is not a
// construction, and every read that puts a constructor value into circulation
// without naming one.
class CtorEscapeScan final : public Walker {
public:
    CtorEscapeScan(const CtorTable& table, const std::set<std::string>& declared,
                   CtorPoison& poison, CtorEscapeFacts& facts)
        : table_(table), declared_(declared), poison_(poison), facts_(facts) {}

    using Walker::visit;

    // The default for a class name: whoever receives the value can construct it
    // with anything, and this compilation will never see with what.
    void visit(const ast::Ident& n) override {
        // The global object by name: whoever holds it can install anything
        // under any name, so a `new <a free name>()` stops being a builtin's.
        if (n.name == "globalThis" && declared_.count(n.name) == 0) {
            facts_.freeGlobalWrite = true;
        }
        poison(n.name, "the class binding is read as a value");
    }

    void visit(const ast::NewExpr& n) override {
        calleePosition(*n.callee);
        for (const auto& a : n.args) a->accept(*this);
    }

    void visit(const ast::Call& n) override {
        // `C(...)` — a TypeError on a class, and an ordinary call on anything
        // else. Either way the name is not being constructed here.
        if (const auto* id = dynamic_cast<const ast::Ident*>(n.callee.get())) {
            poison(id->name, "the class is called without `new`");
        } else {
            n.callee->accept(*this);
        }
        for (const auto& a : n.args) a->accept(*this);
    }

    void visit(const ast::MemberAccess& n) override {
        if (putsConstructorInCirculation(n.property) && !isNonClassReceiver(n.object.get())) {
            setValueEscape("a constructor is read off a value (`." + n.property + "`)");
        }
        // `C.prototype.m` and `C.prototype.m = v`: the prototype object is
        // CONSUMED by the outer access and never becomes a value, so the road
        // from it back to `C` — which is `.constructor` and nothing else — is
        // not taken. three.js writes `Vector3.prototype.isVector3 = true` for
        // every one of its math classes, and reading that as an escape stood
        // down exactly the classes this mechanism exists for.
        if (const auto* inner = dynamic_cast<const ast::MemberAccess*>(n.object.get())) {
            if (inner->property == "prototype") {
                if (const auto* id = classIdent(inner->object.get())) {
                    if (revealsConstructor(n.property)) {
                        poison(id->name,
                               "a property that can yield the constructor is read off the class");
                    }
                    return;
                }
            }
        }
        if (const auto* id = classIdent(n.object.get())) {
            // A STATIC read — `Euler.DEFAULT_ORDER`. It cannot produce the
            // class, so it is left alone; `C.prototype` handed on as a value
            // can, by way of `.constructor`, and takes the class down.
            if (revealsConstructor(n.property)) {
                poison(id->name, "a property that can yield the constructor is read off the class");
            }
            return;
        }
        n.object->accept(*this);
    }

    void visit(const ast::IndexAccess& n) override {
        if (!harmlessComputedKey(n.index.get())) {
            setValueEscape("a computed property read could name `constructor`");
        }
        if (const auto* id = classIdent(n.object.get())) {
            poison(id->name, "the class binding is indexed");
        } else {
            n.object->accept(*this);
        }
        n.index->accept(*this);
    }

    // `new.target` inside a constructor IS the constructor being run, and it is
    // an ordinary value from there on.
    void visit(const ast::NewTargetExpr&) override {
        setValueEscape("the program reads `new.target`");
    }

    void visit(const ast::Binary& n) override {
        if (ast::isAssignOp(n.op)) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(n.lhs.get())) {
                if (declared_.count(id->name) == 0) facts_.freeGlobalWrite = true;
            }
        }
        // `x instanceof C` and `x === C` consume the class: neither can store
        // it, and refusing them would take down every class three.js brand-tests.
        if (n.op == ast::BinaryOp::InstanceOf) {
            n.lhs->accept(*this);
            consumed(*n.rhs);
            return;
        }
        if (isEqualityOp(n.op)) {
            consumed(*n.lhs);
            consumed(*n.rhs);
            return;
        }
        Walker::visit(n);
    }

    void visit(const ast::Unary& n) override {
        if (n.op == ast::UnaryOp::TypeOf) {
            consumed(*n.operand);
            return;
        }
        Walker::visit(n);
    }

    void visit(const ast::ClassDecl& n) override {
        superClass(n.superClass.get(), !n.name.empty());
        for (const auto& m : n.methods) walkMember(m);
    }
    void visit(const ast::ClassExpr& n) override {
        superClass(n.superClass.get(), !n.name.empty());
        for (const auto& m : n.methods) walkMember(m);
    }

    // `super(...)` names the base in order to CONSTRUCT it — the very site this
    // pass enumerates — and `super.m()` reads a method off its prototype. The
    // parser stamps both nodes with the base's binding as an expression, and
    // walking it as an ordinary read poisoned the base of every class in
    // three.js that chains through a `super` call, which is essentially all of
    // them.
    void visit(const ast::SuperCall& n) override {
        for (const auto& a : n.args) a->accept(*this);
    }
    void visit(const ast::SuperMember& n) override {
        if (revealsConstructor(n.property)) {
            const auto* id = classIdent(n.baseExpr.get());
            poison(id != nullptr ? id->name : n.baseName,
                   "a property that can yield the constructor is read off the class");
        }
        if (putsConstructorInCirculation(n.property)) {
            setValueEscape("a constructor is read off a value (`super." + n.property + "`)");
        }
    }

    void visit(const ast::ExportNamesDecl& n) override {
        for (const auto& s : n.specifiers) {
            poison(s.local, "the class is exported");
        }
    }

private:
    const ast::Ident* classIdent(const ast::Expr* e) const {
        const auto* id = dynamic_cast<const ast::Ident*>(e);
        return id != nullptr && table_.isClassName(id->name) ? id : nullptr;
    }

    // The one read of a class binding that is a construction rather than an
    // escape. A callee this pass cannot name is walked for its own reads: the
    // property read that produced it feeds this `new` and nothing else, so it
    // is not a value in circulation.
    void calleePosition(const ast::Expr& e) {
        if (dynamic_cast<const ast::Ident*>(&e) != nullptr) return;
        if (const auto* m = dynamic_cast<const ast::MemberAccess*>(&e)) {
            if (classIdent(m->object.get()) != nullptr) {
                poison(dynamic_cast<const ast::Ident*>(m->object.get())->name,
                       "a property that can yield the constructor is read off the class");
                return;
            }
            m->object->accept(*this);
            return;
        }
        if (const auto* ix = dynamic_cast<const ast::IndexAccess*>(&e)) {
            if (const auto* id = classIdent(ix->object.get())) {
                poison(id->name, "the class binding is indexed");
            } else {
                ix->object->accept(*this);
            }
            ix->index->accept(*this);
            return;
        }
        e.accept(*this);
    }

    void consumed(const ast::Expr& e) {
        if (classIdent(&e) != nullptr) return;
        e.accept(*this);
    }

    void superClass(const ast::Expr* e, bool enclosingIsNamed) {
        if (e == nullptr) return;
        // `extends C` names the base; the subclass's own `super(...)` is what
        // reaches it, and that is a construction site this pass enumerates —
        // but only if the subclass has a name, because the flow pass identifies
        // the enclosing class of a `super(...)` by its name and can say nothing
        // about an anonymous one's.
        if (enclosingIsNamed && classIdent(e) != nullptr) return;
        e->accept(*this);
    }

    void walkMember(const ast::ClassMethod& m) {
        if (m.keyExpr) m.keyExpr->accept(*this);
        if (m.fn) {
            for (const auto& p : m.fn->params) {
                if (p.defaultValue) p.defaultValue->accept(*this);
                if (p.pattern) walkPattern(p.pattern.get());
            }
            for (const auto& s : m.fn->body) s->accept(*this);
        } else if (m.init) {
            m.init->accept(*this);
        }
    }

    void poison(const std::string& name, const char* reason) {
        if (!table_.isClassName(name)) return;
        std::vector<std::string> chain;
        table_.ancestorsOf(name, chain);
        for (const auto& cls : chain) poison_.add(cls, reason);
    }
    void poison(const std::string& name, const std::string& reason) {
        poison(name, reason.c_str());
    }

    void setValueEscape(const std::string& why) {
        if (facts_.valueEscapes) return;
        facts_.valueEscapes = true;
        facts_.valueEscapeReason = why;
    }

    const CtorTable& table_;
    const std::set<std::string>& declared_;
    CtorPoison& poison_;
    CtorEscapeFacts& facts_;
};

}  // namespace

void CtorPoison::add(const std::string& className, const std::string& reason) {
    byClass.emplace(className, reason);
}

void CtorPoison::addAll(const std::string& reason) {
    if (all) return;
    all = true;
    allReason = reason;
}

const std::string& CtorPoison::reasonFor(const std::string& className) const {
    const auto it = byClass.find(className);
    if (it != byClass.end()) return it->second;
    if (all) return allReason;
    return kNoReason;
}

void CtorTable::build(const ast::Module& module) {
    ClassScan scan;
    scan.visit(module);
    for (const auto& cls : scan.found) {
        // A duplicate class name is a redeclaration the parser rejects at module
        // scope; a nested one that shadows is rare, and the first wins — which
        // is how `ClassLayoutTable` resolves `extends` too.
        if (classes_.count(cls.name) != 0) {
            duplicates_.insert(cls.name);
            continue;
        }
        ClassNode node;
        node.superName = cls.superName;
        if (const ast::ClassMethod* ctor = constructorOf(*cls.methods)) {
            CtorInfo info;
            info.fn = ctor->fn.get();
            info.className = cls.name;
            info.superName = cls.superName;
            info.isForwarder = isForwardingCtor(*ctor->fn);
            const auto& params = ctor->fn->params;
            for (const auto& p : params) {
                if (p.isRest || p.pattern) info.plainParams = false;
            }
            info.signature.params.assign(params.size(), Type::never());
            info.observedParams.assign(params.size(), Type::never());
            info.hasDefault.assign(params.size(), false);
            info.safeParamNames.assign(params.size(), std::string());
            RebindScan rebound;
            rebound.walkList(ctor->fn->body);
            for (size_t i = 0; i < params.size(); ++i) {
                info.hasDefault[i] = params[i].defaultValue != nullptr;
                if (params[i].isRest || params[i].pattern || params[i].name.empty()) continue;
                if (rebound.names.count(params[i].name) != 0) continue;
                info.safeParamNames[i] = params[i].name;
            }
            node.ctorIndex = static_cast<uint32_t>(ctors_.size());
            byNode_[ctor->fn.get()] = node.ctorIndex;
            ctors_.push_back(std::move(info));
        }
        classes_.emplace(cls.name, std::move(node));
    }
    for (auto& entry : classes_) {
        if (entry.second.superName.empty()) continue;
        const auto parent = classes_.find(entry.second.superName);
        if (parent == classes_.end()) continue;
        parent->second.children.push_back(entry.first);
    }
}

uint32_t CtorTable::indexOfNode(const ast::FunctionExpr* fn) const {
    const auto it = byNode_.find(fn);
    return it == byNode_.end() ? kNoCtor : it->second;
}

uint32_t CtorTable::targetOf(const std::string& className) const {
    std::set<std::string> seen;
    for (std::string name = className; !name.empty();) {
        if (!seen.insert(name).second) return kNoCtor;  // a cyclic `extends`
        const auto it = classes_.find(name);
        if (it == classes_.end()) return kNoCtor;
        if (it->second.ctorIndex != kNoCtor &&
            !ctors_[it->second.ctorIndex].isForwarder) {
            return it->second.ctorIndex;
        }
        name = it->second.superName;
    }
    return kNoCtor;
}

void CtorTable::subtreeOf(const std::string& className, std::vector<uint32_t>& out) const {
    std::vector<const std::string*> stack;
    const auto start = classes_.find(className);
    if (start == classes_.end()) return;
    stack.push_back(&start->first);
    std::set<const std::string*> seen;
    while (!stack.empty()) {
        const std::string* name = stack.back();
        stack.pop_back();
        if (!seen.insert(name).second) continue;
        const uint32_t target = targetOf(*name);
        if (target != kNoCtor && std::find(out.begin(), out.end(), target) == out.end()) {
            out.push_back(target);
        }
        const auto it = classes_.find(*name);
        if (it == classes_.end()) continue;
        for (const auto& child : it->second.children) {
            const auto c = classes_.find(child);
            if (c != classes_.end()) stack.push_back(&c->first);
        }
    }
}

void CtorTable::ancestorsOf(const std::string& className, std::vector<std::string>& out) const {
    std::set<std::string> seen;
    for (std::string name = className; !name.empty();) {
        if (!seen.insert(name).second) return;
        out.push_back(name);
        const auto it = classes_.find(name);
        if (it == classes_.end()) return;
        name = it->second.superName;
    }
}

std::map<std::string, std::map<std::string, Type>> CtorTable::harvestOracle() const {
    std::map<std::string, std::map<std::string, Type>> out;
    for (const auto& c : ctors_) {
        if (c.isForwarder) continue;
        std::map<std::string, Type> params;
        for (size_t i = 0; i < c.safeParamNames.size(); ++i) {
            if (c.safeParamNames[i].empty()) continue;
            if (i >= c.signature.params.size()) continue;
            params.emplace(c.safeParamNames[i], c.signature.params[i]);
        }
        if (!params.empty()) out.emplace(c.className, std::move(params));
    }
    return out;
}

void scanCtorEscapes(const ast::Module& module, const CtorTable& table, CtorPoison& poison,
                     CtorEscapeFacts& facts) {
    if (!table.duplicateNames().empty()) {
        poison.addAll("two classes in the program share a name");
    }
    DeclaredNameScan declared;
    declared.visit(module);
    CtorEscapeScan scan(table, declared.names, poison, facts);
    scan.visit(module);
}

}  // namespace bronze::types
