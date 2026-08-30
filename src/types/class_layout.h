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

    // Parallel to `fields`: whether the construction sequence installs that
    // field as a WRITABLE data property. True for an ordinary assignment and a
    // field declaration; false for `Object.defineProperty(this, 'id', {value:
    // n})`, whose descriptor defaults every unstated attribute to false — which
    // is how three.js gives Object3D, BufferGeometry, Material and Texture
    // their `id`, i.e. how it roots nearly every `extends` chain in the
    // library.
    //
    // A read does not care. A WRITE does: a fixed-offset store is a bare store
    // and cannot fall into 10.4.5's refusal, so a site may only claim a slot the
    // runtime confirmed writable — and the runtime confirms it by checking the
    // shape node's attribute against this bit.
    std::vector<bool> fieldWritable;

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

    // Every name this class declares a `get` or `set` for. A property with an
    // accessor on the prototype chain is a CALL in both directions: the read is
    // the getter's return value, not the slot, and the constructor's own write
    // goes to the setter and creates no own property at all. Either way what
    // the field-type harvest says about the slot describes something no site
    // touches, so a name in here can never carry a primitive claim.
    std::vector<std::string> accessorNames;

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

    // This class's position in the PREORDER walk of the proven-layout classes,
    // ordered by `extends`, and how many further classes its own subtree spans.
    // `kNoFamily` for a class that is not in the forest: an unproven layout, or
    // one with no fields at all (whose field list is a prefix of everything and
    // would recognise every shape in the program).
    //
    // The pair is what a family guard compares against: a shape stamped with
    // any id in [familyIndex, familyIndex + familySpan] has this class's whole
    // field list as a prefix, because every class in that range is this class or
    // a class that extends it, and a subclass's layout begins with its base's.
    // That last sentence is the invariant the mechanism rests on, and `resolve`
    // checks it rather than assuming it.
    static constexpr uint32_t kNoFamily = 0xFFFFFFFFu;
    uint32_t familyIndex = kNoFamily;
    uint32_t familySpan = 0;

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

    // The type of a proven class's field, or `Dynamic`. A HARVEST over the
    // class body, which is evidence and not proof — see `fieldValueCandidate`
    // and `types/field_audit.h` for the two things a caller must add before
    // spending a primitive answer on anything.
    Type fieldTypeOf(ShapeClassId id, const std::string& field) const;

    // Everything about the class that a PRIMITIVE field claim needs: the layout
    // is proven, the construction sequence installs `field` on every path, and
    // no accessor of that name sits anywhere on the prototype chain. The
    // remaining condition is program-wide and lives in `FieldAudit`.
    bool fieldValueCandidate(ShapeClassId id, const std::string& field) const;

    // Which of `fieldValueCandidate`'s four conditions failed, named, or the
    // empty string when none did. A count of refusals says how big the pile is
    // and nothing about what is in it; the four conditions are removed by four
    // different pieces of work, and a report that cannot tell them apart cannot
    // rank them. Returned as a short constant string so the caller can key a
    // histogram on it without formatting.
    std::string fieldValueRefusal(ShapeClassId id, const std::string& field) const;

    // Whether any class in the program extends this one. A `this` receiver of
    // an extended class is shape-polymorphic; see `ClassLayout::extended`.
    bool isExtended(ShapeClassId id) const;

    // The class a receiver of this shape class belongs to, when it is in the
    // layout-family forest; null otherwise. What a `this` site consults to turn
    // an identity claim it cannot use into a family claim it can.
    const ClassLayout* familyMemberOf(ShapeClassId id) const;

    // The forest in preorder — the order `familyIndex` numbers, and the order
    // the module's registration table has to be emitted in.
    const std::vector<const ClassLayout*>& familyPreorder() const { return preorder_; }

    // Every class, in first-declaration order — the reason it is a vector.
    const std::vector<ClassLayout>& all() const { return classes_; }

    // reason -> how many classes it refused. The chunk's histogram.
    std::map<std::string, uint32_t> refusalHistogram() const;

    // Re-runs the field-type harvest with a constructor's PARAMETERS answered
    // from `byClass` (class name -> parameter name -> type), and says whether
    // any field type moved.
    //
    // `this.x = x` is the write three.js's math classes make, and until the
    // call-graph fixpoint has typed `x` there is nothing to harvest from it but
    // `dynamic`. The fixpoint types it (types/ctor_ident.h), so the harvest has
    // to be able to run again — which is why it is separated from `build` and
    // why it recomputes rather than joins. Monotone in the fixpoint's sense: the
    // parameter types only widen, so the field types re-harvested from them do
    // too, and the loop in infer.cpp settles.
    bool reharvestFieldTypes(
        const std::map<std::string, std::map<std::string, Type>>& byClass,
        const std::map<std::string, std::map<std::string, std::map<std::string, Type>>>* methodByClass = nullptr);

private:
    // Pass 3 of `build`, and the body of `reharvestFieldTypes`. Null runs the
    // purely syntactic harvest.
    void harvestFieldTypes(
        const std::map<std::string, std::map<std::string, Type>>* byClass,
        const std::map<std::string, std::map<std::string, std::map<std::string, Type>>>* methodByClass = nullptr);
    // Joins one class's base's field types into its own, base first. A
    // subclass instance HAS the base's fields; without this the table claimed a
    // slot and knew nothing about its contents.
    void inheritFieldTypes(size_t index, std::vector<bool>& done);

    // Resolves one class's `fields`/`layoutProven`, recursing through
    // `extends` first. `resolving` breaks a cyclic `extends`, which is a
    // TypeError at run time but must not be an infinite loop here.
    void resolve(size_t index, std::set<size_t>& resolving);

    // Numbers the proven classes in preorder over `extends` and fills
    // `familyIndex`/`familySpan`. Runs after every class is resolved, because
    // the forest is only known then.
    void buildFamilies();
    uint32_t numberSubtree(size_t index, uint32_t next,
                           const std::map<size_t, std::vector<size_t>>& children);

    // The collector's record for each class, kept only for the span of
    // `build` so `resolve` can reach a class's methods by index.
    std::vector<const FoundRef*> found_;
    // The class members, by class index, kept for the life of the table: the
    // field-type harvest re-runs on them every round of the call-graph
    // fixpoint. The AST outlives every analysis, so borrowing is safe; the
    // collector's own records are not, which is why this is a second vector and
    // not a longer life for `found_`.
    std::vector<const std::vector<ast::ClassMethod>*> methodsByIndex_;

    std::vector<ClassLayout> classes_;
    std::map<std::string, size_t> byName_;
    std::map<ShapeClassId, size_t> byShape_;
    std::vector<bool> resolved_;
    std::vector<const ClassLayout*> preorder_;
    ShapeClassTable* shapes_ = nullptr;
};

}  // namespace bronze::types
