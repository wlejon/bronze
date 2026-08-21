#pragma once

#include <map>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "types/type.h"

namespace bronze::types {

// What the whole program can put in a property called `x`.
//
// `ClassLayout::fieldTypes` is a HARVEST: the join over every `this.<f> = ...`
// a class body writes. That is evidence about the class body and nothing else,
// and until this file existed it was spent as a proof:
//
//     class V { constructor() { this.x = 0; } m() { let s = this.x; ... } }
//     const v = new V(); v.x = "hi"; v.m();
//
// harvested `x: number`, which reached `mergeParamType`, which made the loop
// header's block parameter an `f64`, which hard-unboxed the string — `NaN`
// where the language says `hi`. The write is three lines from the class and the
// harvest never looked at it.
//
// So a field's PRIMITIVE type is a proof only when every write in the program
// preserves it. This is the pass that decides that, and its unit is the
// property NAME rather than the class, because that is the only unit the
// question can be asked in: a write through a receiver whose class is a guess
// — which is every `this`, every method parameter, every dynamic receiver —
// may land on an instance of any class at all, so no class-scoped audit can
// speak for it.
//
// The invariant it certifies is therefore name-global and coinductive:
//
//     every property named `f` anywhere in the heap holds a Number
//
// which is established by construction (the constructor's write is a write this
// pass sees) and preserved by every write this pass sees. A write whose value
// is itself typed Number preserves it; anything else refutes the name. The
// fixpoint in infer.cpp runs the two together, because the type of `this.x =
// v.x` depends on whether `x` is clean, which depends on the type of that very
// write. Poison is monotone — a name only ever goes from clean to refuted — so
// the loop settles.
//
// IDENTITY claims are untouched. `Type::object(C)` is a guess that licenses
// exactly one thing, the guarded property-site form, and a wrong guess there
// costs a cache miss (types/type.h, `objectIdentityOnly`). This file is about
// the one road from an identity to a VALUE, which is a soundness obligation and
// now requires a proof.
class FieldAudit {
public:
    // One walk of the whole (already module-flattened) program, before any body
    // is analysed. Collects every property write, and every construct that can
    // write a name this pass cannot see.
    void scan(const ast::Module& module);

    // The type the flow pass gave one recorded write's right-hand side, this
    // round. Joined, never replaced: signature types only widen across the
    // call-graph fixpoint, so the join over the rounds IS the fixpoint value.
    void observe(const ast::Expr* rhs, Type t);

    // Re-decides which names are clean from what has been observed so far.
    // Returns whether anything moved, which is what makes this part of the
    // outer fixpoint. Monotone: clean -> refuted only.
    bool settle();

    // Is `name` proven to hold nothing but Numbers, program-wide?
    bool numberClean(const std::string& name) const;

    // Why a name is not clean, or empty when it is (or was never a candidate).
    std::string refusalFor(const std::string& name) const;

    // The constructs that refuted EVERY name at once — a computed write whose
    // key could be anything, a computed `delete`, `eval`, a `Proxy` — with how
    // many sites of each the program contains. The single most valuable rows in
    // this pass's report: on a real library one of these decides the whole
    // verdict, and knowing WHICH is the difference between a chunk that has a
    // target and one that has a percentage.
    const std::map<std::string, uint32_t>& globalRefusals() const { return globalRefusals_; }

    // How many names carry no refusal OF THEIR OWN. Equal to `cleanCount()`
    // when no global refusal stands, and the size of the prize when one does.
    uint32_t locallyCleanCount() const;

    // Deterministic report rows: (name, refusal) for every name written
    // anywhere, refusal empty for a clean one. Sorted by name.
    std::vector<std::pair<std::string, std::string>> report() const;

    // How many `o[k] = v` / `delete o[k]` sites the program contains, and how
    // many of them the flow pass proved harmless. The gap is what a global
    // refusal costs, and the two numbers together are the only way to tell "the
    // library barely does this" from "the key types are not there yet".
    uint32_t computedSiteCount() const { return static_cast<uint32_t>(computed_.size()); }
    uint32_t computedRefutedCount() const { return computedRefuted_; }
    // What the unproven sites' KEYS were typed, so the next attempt has a
    // target rather than a percentage. Sorted by type name.
    std::map<std::string, uint32_t> computedKeyTypes() const;
    // The same for their RECEIVERS. See `Computed::receiver`.
    std::map<std::string, uint32_t> computedReceiverTypes() const;

    uint32_t writeSiteCount() const { return static_cast<uint32_t>(writes_.size()); }
    uint32_t nameCount() const { return static_cast<uint32_t>(names_.size()); }
    // How many of `nameCount` still stand clean. Zero whenever a global
    // refusal is in force, which is what makes the two numbers a report.
    uint32_t cleanCount() const;

    // The scan's own recording surface, public because the walk that fills it
    // is a separate class and this is the whole of what it may do.
    void record(const std::string& name, const ast::Expr* rhs);
    void recordComputed(const ast::Expr* receiver, const ast::Expr* key, const ast::Expr* value);
    // `delete o[k]`. Recorded rather than refused outright for the same reason a
    // computed WRITE is: a key the flow pass proves is a Number can only name a
    // canonical numeric string, and deleting an array element is not a fact
    // about any field this pass speaks for.
    void recordComputedDelete(const ast::Expr* receiver, const ast::Expr* key);
    void refuse(const std::string& name, std::string why);
    void refuseAll(std::string why);
    // A write or delete through a key that is a NUMBER. It can only reach a
    // property whose name is a canonical numeric string, so it refutes those
    // and nothing else — which is what keeps `array[i] = <an object>`, of which
    // three.js has thousands, from refuting `x`, `y` and `z`.
    void noteNumericKeyWrite() { numericKeyWrite_ = true; }

private:
    // One recorded write: the name it targets and the expression whose value it
    // stores. `rhs` is null for a write whose value this pass cannot name,
    // which refutes the name outright.
    struct Write {
        std::string name;
        const ast::Expr* rhs = nullptr;
    };

    // `o[k] = v`, and `delete o[k]`: neither the name nor, in general, the type
    // is known here. Harmless when the flow pass proves the KEY is a Number
    // (ToPropertyKey of one is a numeric string, which no ordinary field is
    // called) or — for a write — the VALUE is a Number (the invariant survives
    // whatever name it lands on).
    struct Computed {
        // Read for the report only. The audit's unit is the property NAME and
        // not the receiver's class, for the reason argued at the top of this
        // file — but which OBJECTS the unproven sites write to is the one fact
        // that says whether a receiver-shaped narrowing could ever pay, and it
        // is not knowable from the source without the types.
        const ast::Expr* receiver = nullptr;
        const ast::Expr* key = nullptr;
        const ast::Expr* value = nullptr;  // null for a delete
        bool isDelete = false;
        // Sticky, and the reason this is a per-site record rather than a
        // running count. The verdict is re-taken every round, because the types
        // it reads only settle at the fixpoint; without the flag the same site
        // was counted once per round, and the report's headline number was the
        // program's computed writes multiplied by however many rounds the
        // fixpoint happened to take.
        bool refuted = false;
    };

    Type typeOfExpr(const ast::Expr* e) const;
    size_t refusedCount() const;

    std::vector<Write> writes_;
    std::vector<Computed> computed_;
    uint32_t computedRefuted_ = 0;
    std::map<const ast::Expr*, Type> rhsTypes_;
    // Every name any write targets, with the refusal that stands against it.
    std::map<std::string, std::string> names_;
    std::map<std::string, uint32_t> globalRefusals_;
    bool numericKeyWrite_ = false;
};

// Names a builtin already owns, which no audit over the PROGRAM's writes can
// speak for: `"abc".length` is a number nothing in the program wrote, and
// `re.flags` is a string of exactly the same standing. A candidate name in this
// set is refused before any write is looked at.
bool builtinOwnedName(const std::string& name);

// Can a property key produced by ToPropertyKey of a NUMBER ever be `name`?
// Only for a name that is itself a canonical numeric string, which is what
// keeps `array[i] = <an object>` from refuting `x`, `y` and `z`. Every
// identifier-shaped name answers false.
bool couldBeNumericKey(const std::string& name);

}  // namespace bronze::types
