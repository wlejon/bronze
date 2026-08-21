#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "types/shape_class.h"
#include "types/type.h"

namespace bronze::types {

// What a `class` declaration says about the objects it constructs.
//
// Two separate claims live here, and keeping them apart is the whole design:
//
//   1. IDENTITY — "instances of `Vector3` are one compile-time object kind".
//      That is a `ShapeClassId`, it is what `Type::object(cls)` already
//      carries, and it licenses exactly what a shape class licensed before
//      this file existed: the inline-cache form, whose runtime shape compare
//      is what makes it sound. An identity claim can be wrong about the
//      layout and cost nothing but a cache miss.
//
//   2. LAYOUT — "field `y` of a `Vector3` is at slot 1". That is a much
//      stronger claim, and it is the one a fixed-offset load consumes. It is
//      granted only when the whole construction sequence is modellable, and
//      it is REFUSED with a named reason otherwise (`ClassLayout::refusal`).
//
// The layout claim is still not a soundness obligation — generated code
// compares the shape word before it uses the slot, and the runtime publishes
// the expected shape only after checking that the key really is an own data
// property at that slot. A wrong layout therefore costs a permanently missing
// guard, never a wrong answer. What it buys by being right is the whole point,
// which is why the refusals below are strict rather than optimistic: a site
// that guesses wrong is SLOWER than one that never guessed.
struct ClassLayout {
    std::string name;       // the binding the class is declared under
    std::string superName;  // resolved base class name; empty when no `extends`
    ShapeClassId shapeClass = kNoShapeClass;

    // The own instance properties in the order the runtime's shape transition
    // tree will install them, INCLUDING the base class's prefix. Meaningful
    // only when `layoutProven`.
    std::vector<std::string> fields;

    // Per field, the type joined over every `this.<field> = ...` the class body
    // writes. Syntactic and deliberately shallow — see `harvestFieldType`.
    std::map<std::string, Type> fieldTypes;

    // Fields a METHOD installs, which the constructor does not. Deliberately
    // not in `fields`: they land after every constructor field, so they take no
    // slot the layout claims — but they do split the class's instances into two
    // shapes, and a site that names one of these keys has no constant slot to
    // load from. Kept so `slotOf` cannot answer for them and so the count is
    // reportable.
    std::vector<std::string> lateFields;

    // The methods that install those, by name, including the base's. Only the
    // constructor-calls-a-late-method check reads this.
    std::vector<std::string> lateMethods;

    // The construction sequence ends with a call to one of those methods, so
    // the fields listed above are all correct but an unknown number of others
    // may follow them. Harmless for this class; fatal for one that extends it
    // and installs anything of its own.
    bool lateCallTail = false;

    // Some other class in the program declares `extends` this one. Its
    // instances are then not the only instances its METHODS run on, and `this`
    // inside them holds a subclass's shape — a different shape, even though the
    // subclass's layout begins with this one's fields.
    bool extended = false;

    // Whether `fields` is a claim about slot numbers, or only a name list.
    bool layoutProven = false;
    // Why not, when it is not. Named, never silent: this string is what
    // `--infer-stats` reports and what a future chunk reads to decide which
    // refusal is worth removing.
    std::string refusal;
};

// Every class in the (already module-flattened) program, keyed by the name it
// is bound to. The linker renames module-level bindings into one namespace
// before inference runs, so a name here is unique across the whole program and
// `extends` resolves by name without any module plumbing.
class ClassLayoutTable {
public:
    // One collected `class` node, defined in class_layout.cpp. Named here only
    // so the member below can hold it; it carries nothing a caller could use.
    struct FoundRef;

    // Collects and resolves every class in the module. `shapes` is the same
    // table object-literal and constructor-function sites intern into, so a
    // class's identity is the same kind of thing theirs is.
    void build(const ast::Module& module, ShapeClassTable& shapes);

    const ClassLayout* byName(const std::string& name) const;
    const ClassLayout* byShapeClass(ShapeClassId id) const;

    // The slot a proven layout puts `field` at, or `kNoSlot`. Answers
    // `kNoSlot` for every unproven class, so a caller cannot accidentally
    // consume a name list as a layout.
    static constexpr uint32_t kNoSlot = 0xFFFFFFFFu;
    uint32_t slotOf(ShapeClassId id, const std::string& field) const;

    // The type of a proven class's field, or `Dynamic`.
    Type fieldTypeOf(ShapeClassId id, const std::string& field) const;

    // Whether any class in the program extends this one. A `this` receiver of
    // an extended class is shape-polymorphic; see `ClassLayout::extended`.
    bool isExtended(ShapeClassId id) const;

    // Every class, in first-declaration order — the reason it is a vector.
    const std::vector<ClassLayout>& all() const { return classes_; }

    // reason -> how many classes it refused. The chunk's histogram.
    std::map<std::string, uint32_t> refusalHistogram() const;

private:
    // Resolves one class's `fields`/`layoutProven`, recursing through
    // `extends` first. `resolving` breaks a cyclic `extends`, which is a
    // TypeError at run time but must not be an infinite loop here.
    void resolve(size_t index, std::set<size_t>& resolving);

    // The collector's record for each class, kept only for the span of
    // `build` so `resolve` can reach a class's methods by index.
    std::vector<const FoundRef*> found_;

    std::vector<ClassLayout> classes_;
    std::map<std::string, size_t> byName_;
    std::map<ShapeClassId, size_t> byShape_;
    std::vector<bool> resolved_;
    ShapeClassTable* shapes_ = nullptr;
};

}  // namespace bronze::types
