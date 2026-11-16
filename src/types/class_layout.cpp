// Class layouts, proven from the construction sequence.
//
// Three.js is disciplined ES6: a class whose constructor assigns every field it
// will ever have, in one order, on every path. That is the shape of program
// this file is written for, and the refusals below are the exact list of things
// that make the order un-modellable rather than a general-purpose bail-out.
//
// What the runtime does, and what this therefore has to reproduce: `this.x = v`
// on an object that has no `x` walks the shape transition tree, takes (or
// mints) the `x` edge, and the new node's `slot_index` is the previous node's
// `nextSlotIndex()` — i.e. slot N for the Nth distinct data property, counting
// from the ROOT shape of the constructor's `.prototype`. So the layout is the
// deduplicated sequence of property names installed on the instance, and
// "installed" for a class means, in ECMA-262 15.7.14 order:
//
//     base class's whole layout   (super() runs first and returns before any
//                                  derived field initializer or ctor statement)
//   ++ this class's field declarations, in declaration order
//   ++ this class's constructor body's `this.<name> = ...`, in source order
//
// Deduplicated keeping the FIRST occurrence, because a second write to a name
// the object already has is a slot store and not a transition.

#include "types/class_layout.h"

#include <algorithm>
#include <utility>

#include "types/this_write_walker.h"
#include "types/walk.h"

namespace bronze::types {

// One collected `class` node: everything `resolve` needs from the AST, so the
// resolution pass never walks it again.
struct ClassLayoutTable::FoundRef {
    std::string name;
    std::string superName;
    bool hasSuperExpr = false;
    const std::vector<ast::ClassMethod>* methods = nullptr;
};

namespace {

using detail::ThisWriteWalker;

// Every `class` in the program, at any nesting depth. Nesting is not a problem
// for identity — a class expression evaluated twice makes two prototypes, but
// both instances agree on the layout, which is what is claimed here.
class ClassCollector final : public Walker {
public:
    using Found = ClassLayoutTable::FoundRef;
    std::vector<Found> found;

    void visit(const ast::ClassDecl& n) override {
        found.push_back(make(n.name, n.superName, n.superClass.get(), n.methods));
        Walker::visit(n);
    }
    void visit(const ast::ClassExpr& n) override {
        // An anonymous class expression has no name to resolve `extends`
        // against and no `new <name>` site to type, so it contributes nothing.
        if (!n.name.empty()) {
            found.push_back(make(n.name, n.superName, n.superClass.get(), n.methods));
        }
        Walker::visit(n);
    }

private:
    // Which spelling of the base names the binding this analysis can look up.
    //
    // `ClassDecl::superName` is what the PARSER resolved, and it is the wrong
    // one after module linking: the linker flattens the graph by renaming every
    // module-level binding into `mod<N>.<local>`, and it rewrites `superClass`
    // — an expression — while leaving `superName` at the local spelling the
    // parser saw. Reading `superName` therefore failed to resolve every
    // cross-module `extends` in the program, which in three.js is essentially
    // all of them: 157 of 169 refusals were this one line.
    static Found make(const std::string& name, const std::string& superName,
                      const ast::Expr* superClass,
                      const std::vector<ast::ClassMethod>& methods) {
        Found f;
        f.name = name;
        f.methods = &methods;
        if (superClass != nullptr) {
            if (const auto* id = dynamic_cast<const ast::Ident*>(superClass)) {
                f.superName = id->name;
            } else {
                // `class X extends (expr)`: a base this analysis cannot name.
                f.hasSuperExpr = true;
            }
            return f;
        }
        f.superName = superName;
        return f;
    }
};

const ast::ClassMethod* constructorOf(const std::vector<ast::ClassMethod>& methods) {
    for (const auto& m : methods) {
        if (m.isConstructor && m.fn) return &m;
    }
    return nullptr;
}

}  // namespace

void ClassLayoutTable::build(const ast::Module& module, ShapeClassTable& shapes) {
    shapes_ = &shapes;

    ClassCollector collector;
    for (const auto& stmt : module.body) {
        if (stmt) stmt->accept(collector);
    }

    // Pass 1: names and identities. Identity has to exist for every class
    // before any layout is resolved, because a field's harvested type can name
    // another class and `extends` resolves by name.
    for (const auto& f : collector.found) {
        if (byName_.count(f.name) != 0) {
            // Two classes bound to one name in one program. The linker's
            // renaming makes this impossible for module-level declarations; a
            // nested one can still do it, and the first wins, exactly as
            // `indexByName` lets the first top-level function win.
            continue;
        }
        ClassLayout cl;
        cl.name = f.name;
        cl.superName = f.superName;
        cl.superUnknown = f.hasSuperExpr;
        classes_.push_back(std::move(cl));
        byName_.emplace(f.name, classes_.size() - 1);
    }

    // Pass 2: layouts. `resolve` recurses through `extends`, so the loop order
    // does not matter and a base declared after its derived class still works.
    resolved_.assign(classes_.size(), false);
    // The collector's order matches `classes_`' order only for first
    // occurrences, which is what `byName_` already indexes.
    std::vector<const FoundRef*> byIndex(classes_.size(), nullptr);
    for (const auto& f : collector.found) {
        const auto it = byName_.find(f.name);
        if (it != byName_.end() && byIndex[it->second] == nullptr) byIndex[it->second] = &f;
    }
    found_ = std::move(byIndex);
    for (size_t i = 0; i < classes_.size(); ++i) {
        std::set<size_t> resolving;
        resolve(i, resolving);
    }

    buildFamilies();

    // The class members each class declares, kept past `build` so the field-type
    // harvest can be re-run as constructor parameter types widen.
    methodsByIndex_.assign(classes_.size(), nullptr);
    for (size_t i = 0; i < classes_.size(); ++i) {
        if (found_[i] != nullptr) methodsByIndex_[i] = found_[i]->methods;
    }
    found_.clear();
    collectOwnNames();

    // Pass 3: field types. Deferred to its own pass because a field's type can
    // name a class whose own identity is only minted in pass 2.
    harvestFieldTypes(nullptr);
}

void ClassLayoutTable::resolve(size_t index, std::set<size_t>& resolving) {
    if (resolved_[index]) return;
    ClassLayout& cl = classes_[index];
    const FoundRef* f = found_[index];

    if (!resolving.insert(index).second) {
        // A cycle through `extends`. The program throws at run time; here it
        // is simply a layout nobody can name.
        cl.refusal = "cyclic extends";
        resolved_[index] = true;
        return;
    }

    std::vector<std::string> fields;
    std::vector<bool> fieldWritable;
    bool proven = true;
    std::string refusal;
    std::map<std::string, size_t> ctorThisCalls;

    if (!cl.superName.empty()) {
        const auto it = byName_.find(cl.superName);
        if (it == byName_.end()) {
            // `extends` a name this program does not declare as a class — a
            // builtin (`Array`, `Error`), a host global, an imported binding
            // that is not a class declaration. The base's own field list is
            // unknown, so every derived slot number is unknown too.
            proven = false;
            refusal = "base class is not a modelled class";
        } else {
            resolve(it->second, resolving);
            classes_[it->second].extended = true;
            const ClassLayout& base = classes_[it->second];
            if (!base.layoutProven) {
                // The base's OWN reason is not repeated: a chain six deep
                // produced a histogram row six parentheses long that named the
                // same root cause as its base's row. Naming the base is what a
                // reader needs — the root cause is one lookup away.
                proven = false;
                refusal = "base class layout refused: " + base.name;
            } else {
                fields = base.fields;
                fieldWritable = base.fieldWritable;
                // A base method's late field is a late field of every derived
                // class too — instances of the derived class are what the
                // method runs on — and so is the method name, for the
                // constructor-calls check below.
                cl.lateFields = base.lateFields;
                cl.lateMethods = base.lateMethods;
                cl.lateCallTail = base.lateCallTail;
                cl.openTail = base.openTail;
            }
        }
    } else if (f != nullptr && f->hasSuperExpr) {
        // `class X extends (expr)` with no name the parser could resolve.
        proven = false;
        refusal = "base class is a computed expression";
    }

    // Accessor names are collected whether or not the layout is proven, and
    // before every refusal below: they are what a PRIMITIVE field claim is
    // checked against (`fieldValueCandidate`), and that question is asked of a
    // class through its whole `extends` chain, so a link that refused its own
    // layout still has to answer it.
    if (f != nullptr && f->methods != nullptr) {
        for (const auto& m : *f->methods) {
            if (m.accessor == ast::AccessorKind::None || m.name.empty()) continue;
            if (std::find(cl.accessorNames.begin(), cl.accessorNames.end(), m.name) ==
                cl.accessorNames.end()) {
                cl.accessorNames.push_back(m.name);
            }
        }
    }

    if (proven && f != nullptr && f->methods != nullptr) {
        // Past an open tail (the base's or this constructor's own, below) a
        // new name's slot is unknown: it is a late field.
        auto add = [&fields, &fieldWritable, &cl](const std::string& name, bool isWritable) {
            if (std::find(fields.begin(), fields.end(), name) != fields.end()) return;
            if (cl.openTail) {
                if (std::find(cl.lateFields.begin(), cl.lateFields.end(), name) == cl.lateFields.end()) {
                    cl.lateFields.push_back(name);
                }
                return;
            }
            {
                fields.push_back(name);
                fieldWritable.push_back(isWritable);
            }
        };

        // 15.7.14: field initializers run before the constructor body (for a
        // base class) or immediately after `super()` returns (for a derived
        // one). Either way they precede every statement of this constructor.
        for (const auto& m : *f->methods) {
            if (!m.isField || m.isStatic) continue;
            if (m.computed()) {
                proven = false;
                refusal = "class field has a computed key";
                break;
            }
            if (m.isPrivate()) continue;  // no shape carries a private name
            add(m.name, true);
        }

        if (proven) {
            if (const ast::ClassMethod* ctor = constructorOf(*f->methods)) {
                ThisWriteWalker walker;
                walker.methods = f->methods;
                walker.walkList(ctor->fn->body);
                if (walker.sawThisReflection) {
                    proven = false;
                    refusal = "constructor reflects on this (defineProperty/assign/freeze)";
                } else if (walker.sawUnmodellableWrite) {
                    proven = false;
                    refusal = "constructor writes a computed key on this";
                } else if (walker.sawConditionalNewField) {
                    proven = false;
                    refusal = "constructor assigns a field conditionally";
                } else {
                    // Past `openAt` the slots are unknown: those fields are
                    // the class's, but land wherever the escaped `this`'s
                    // writers put them, so they are late fields, and the
                    // order is open after the last one claimed.
                    for (size_t i = 0; i < walker.names.size(); ++i) {
                        if (i == walker.openAt) cl.openTail = true;
                        add(walker.names[i], walker.writable[i]);
                    }
                    if (walker.openAt != SIZE_MAX) cl.openTail = true;
                    for (const auto& lf : walker.lateFields) {
                        if (std::find(cl.lateFields.begin(), cl.lateFields.end(), lf) ==
                            cl.lateFields.end()) {
                            cl.lateFields.push_back(lf);
                        }
                    }
                    for (const auto& acc : walker.accessorNames) {
                        if (std::find(cl.accessorNames.begin(), cl.accessorNames.end(), acc) ==
                            cl.accessorNames.end()) {
                            cl.accessorNames.push_back(acc);
                        }
                    }
                    ctorThisCalls = std::move(walker.thisCalls);
                }
            }
        }
    }

    // Methods get to REFUSE, and to name fields the layout must NOT contain.
    if (proven && f != nullptr && f->methods != nullptr) {
        for (const auto& m : *f->methods) {
            if (m.isStatic || m.isConstructor || !m.fn) continue;
            ThisWriteWalker walker;
            walker.walkList(m.fn->body);
            if (walker.sawThisReflection) {
                proven = false;
                refusal = "a method reflects on this (defineProperty/assign/freeze)";
                break;
            }
            (void)walker.sawUnmodellableWrite;
            bool installsLate = false;
            for (const auto& name : walker.names) {
                if (std::find(fields.begin(), fields.end(), name) != fields.end()) continue;
                installsLate = true;
                if (std::find(cl.lateFields.begin(), cl.lateFields.end(), name) ==
                    cl.lateFields.end()) {
                    cl.lateFields.push_back(name);
                }
            }
            for (const auto& lf : walker.lateFields) {
                if (std::find(fields.begin(), fields.end(), lf) != fields.end()) continue;
                installsLate = true;
                if (std::find(cl.lateFields.begin(), cl.lateFields.end(), lf) ==
                    cl.lateFields.end()) {
                    cl.lateFields.push_back(lf);
                }
            }
            if (installsLate && std::find(cl.lateMethods.begin(), cl.lateMethods.end(), m.name) ==
                                    cl.lateMethods.end()) {
                cl.lateMethods.push_back(m.name);
            }
        }
    }

    if (proven && !ctorThisCalls.empty()) {
        for (const auto& [called, installedByThen] : ctorThisCalls) {
            const bool late =
                std::find(cl.lateMethods.begin(), cl.lateMethods.end(), called) !=
                cl.lateMethods.end();
            if (!late) continue;
            cl.lateCallTail = true;
        }
    }

    // THE FAMILY INVARIANT, checked rather than assumed: this class's layout
    // has to BEGIN with its base's, name for name, or a site in a base method
    // that accepts a subclass shape would read the wrong slot.
    //
    // It holds by construction above — `super()` runs to completion before any
    // derived field initializer (15.7.14), the base's fields seed `fields`, and
    // `add` deduplicates so a derived class re-assigning an inherited name does
    // not move it. That is exactly why it is worth a check: the whole family
    // mechanism rests on a property of this loop, and a future edit to the loop
    // that broke it would otherwise be silent. A violation refuses the class
    // rather than firing an assertion — an unprovable layout is a thing this
    // file already knows how to say.
    if (proven && !cl.superName.empty()) {
        const auto it = byName_.find(cl.superName);
        if (it != byName_.end() && classes_[it->second].layoutProven) {
            const ClassLayout& base = classes_[it->second];
            const bool prefixOk =
                fields.size() >= base.fields.size() &&
                std::equal(base.fields.begin(), base.fields.end(), fields.begin()) &&
                std::equal(base.fieldWritable.begin(), base.fieldWritable.end(),
                           fieldWritable.begin());
            if (!prefixOk) {
                proven = false;
                refusal = "layout does not extend the base's prefix";
            }
        }
    }

    // The identity is minted whatever the layout verdict is: a refused class is
    // still one compile-time object kind, and the inline-cache form it already
    // licensed is unaffected by anything decided here.
    cl.shapeClass = shapes_->intern(cl.name, std::vector<std::string>(fields));
    byShape_.emplace(cl.shapeClass, index);

    cl.fields = std::move(fields);
    cl.fieldWritable = std::move(fieldWritable);
    cl.layoutProven = proven;
    cl.refusal = proven ? std::string() : refusal;
    resolved_[index] = true;
    resolving.erase(index);
}

// The layout-family forest: proven classes, linked by `extends`, numbered in
// preorder so that a class's descendants occupy [familyIndex, familyIndex +
// familySpan] and a guard is one unsigned range compare.
//
// Two classes are left out, both because their field list would recognise
// shapes it has no business recognising:
//
//   - a class whose layout was refused. There is nothing to verify a shape
//     against, so it can neither be stamped nor guard.
//   - a class with NO fields. Its field list is the empty prefix, which every
//     shape in the program begins with, so it would stamp every object with its
//     own id. It has no slot to claim either, so nothing is lost — and its
//     proven subclasses become forest roots in their own right rather than
//     being dragged out with it.
void ClassLayoutTable::buildFamilies() {
    std::map<size_t, std::vector<size_t>> children;
    std::vector<size_t> roots;
    auto inForest = [this](size_t i) {
        return classes_[i].layoutProven && !classes_[i].fields.empty();
    };
    for (size_t i = 0; i < classes_.size(); ++i) {
        if (!inForest(i)) continue;
        const auto parent = byName_.find(classes_[i].superName);
        if (!classes_[i].superName.empty() && parent != byName_.end() && inForest(parent->second)) {
            children[parent->second].push_back(i);
        } else {
            roots.push_back(i);
        }
    }
    uint32_t next = 0;
    for (size_t root : roots) next = numberSubtree(root, next, children);

    preorder_.assign(next, nullptr);
    for (const ClassLayout& cl : classes_) {
        if (cl.familyIndex != ClassLayout::kNoFamily) preorder_[cl.familyIndex] = &cl;
    }
}

uint32_t ClassLayoutTable::numberSubtree(size_t index, uint32_t next,
                                         const std::map<size_t, std::vector<size_t>>& children) {
    const uint32_t self = next++;
    classes_[index].familyIndex = self;
    const auto kids = children.find(index);
    if (kids != children.end()) {
        for (size_t child : kids->second) next = numberSubtree(child, next, children);
    }
    // The span is what the subtree consumed: every id in (self, next) belongs to
    // a class that extends this one, transitively, and therefore lays this
    // one's fields out at this one's slots.
    classes_[index].familySpan = next - self - 1;
    return next;
}

const ClassLayout* ClassLayoutTable::familyMemberOf(ShapeClassId id) const {
    const ClassLayout* cl = byShapeClass(id);
    if (cl == nullptr || cl->familyIndex == ClassLayout::kNoFamily) return nullptr;
    return cl;
}

const ClassLayout* ClassLayoutTable::byName(const std::string& name) const {
    const auto it = byName_.find(name);
    return it == byName_.end() ? nullptr : &classes_[it->second];
}

const ClassLayout* ClassLayoutTable::byShapeClass(ShapeClassId id) const {
    if (id == kNoShapeClass) return nullptr;
    const auto it = byShape_.find(id);
    return it == byShape_.end() ? nullptr : &classes_[it->second];
}

uint32_t ClassLayoutTable::slotOf(ShapeClassId id, const std::string& field) const {
    const ClassLayout* cl = byShapeClass(id);
    if (cl == nullptr || !cl->layoutProven) return kNoSlot;
    const auto it = std::find(cl->fields.begin(), cl->fields.end(), field);
    if (it == cl->fields.end()) return kNoSlot;
    return static_cast<uint32_t>(it - cl->fields.begin());
}

Type ClassLayoutTable::fieldTypeOf(ShapeClassId id, const std::string& field) const {
    const ClassLayout* cl = byShapeClass(id);
    if (cl == nullptr) return Type::dynamic();
    const auto it = cl->fieldTypes.find(field);
    return it == cl->fieldTypes.end() ? Type::dynamic() : it->second;
}

// Everything about the CLASS that a primitive field claim needs, which is
// everything except the program-wide write audit (field_audit.h).
//
// Three conditions, and each is a way the claim has failed in practice:
//
//   - the layout is proven, and `field` is in it. `fieldTypes` also carries
//     the fields a METHOD installs, which the constructor does not: reading one
//     off a fresh instance produces `undefined`, and a `number` claim over that
//     is the same miscompile in a different disguise.
//   - no ACCESSOR of that name anywhere on the prototype chain. A getter
//     answers the read with a call, so the harvest describes a slot the read
//     never touches; a setter intercepts the constructor's own write, so the
//     own property the layout claims is never created at all.
//   - the chain is walked to the root, because an accessor two classes up
//     shadows exactly as well as one declared here.
bool ClassLayoutTable::fieldValueCandidate(ShapeClassId id, const std::string& field) const {
    const ClassLayout* cl = byShapeClass(id);
    if (cl == nullptr || !cl->layoutProven) return false;
    if (std::find(cl->fields.begin(), cl->fields.end(), field) == cl->fields.end()) return false;
    for (const ClassLayout* walk = cl; walk != nullptr;) {
        if (std::find(walk->accessorNames.begin(), walk->accessorNames.end(), field) !=
            walk->accessorNames.end()) {
            return false;
        }
        if (walk->superName.empty()) break;
        const ClassLayout* base = byName(walk->superName);
        if (base == walk) break;
        walk = base;
    }
    return true;
}

std::string ClassLayoutTable::fieldValueRefusal(ShapeClassId id, const std::string& field) const {
    const ClassLayout* cl = byShapeClass(id);
    if (cl == nullptr) return "the shape class is not a declared class";
    if (!cl->layoutProven) {
        return cl->refusal.empty() ? std::string("layout unproven") : "layout: " + cl->refusal;
    }
    if (std::find(cl->fields.begin(), cl->fields.end(), field) == cl->fields.end()) {
        return "not installed by the construction sequence";
    }
    for (const ClassLayout* walk = cl; walk != nullptr;) {
        if (std::find(walk->accessorNames.begin(), walk->accessorNames.end(), field) !=
            walk->accessorNames.end()) {
            return "an accessor of that name on the prototype chain";
        }
        if (walk->superName.empty()) break;
        const ClassLayout* base = byName(walk->superName);
        if (base == walk) break;
        walk = base;
    }
    return {};
}

bool ClassLayoutTable::isExtended(ShapeClassId id) const {
    const ClassLayout* cl = byShapeClass(id);
    return cl != nullptr && cl->extended;
}

std::map<std::string, uint32_t> ClassLayoutTable::refusalHistogram() const {
    std::map<std::string, uint32_t> out;
    for (const auto& cl : classes_) {
        if (cl.layoutProven) continue;
        ++out[cl.refusal.empty() ? std::string("unknown") : cl.refusal];
    }
    return out;
}

}  // namespace bronze::types
