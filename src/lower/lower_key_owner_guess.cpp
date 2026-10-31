// A family guard for a field read off a value that came out of another
// class's field: `this.parent.matrixWorld`.
//
// `this.parent` has no type. three.js writes it from outside the class
// (`object.parent = this` in `add`), so the field harvest sees only the
// constructor's `null`, and the read of `.matrixWorld` off it takes the inline
// cache's stub: 85 instructions an object per updateMatrixWorld on mesh_churn
// (ledger chunk 13). What IS known is the key. When exactly one family of
// proven classes, the subtree under one class, declares `matrixWorld` as a
// field, and no class outside that subtree names it at all, the receiver is
// guessed to be of that family, and the site gets the family guard a guessed
// parameter gets (claimStaticSlot's guessedParamShapeAt arm).
//
// Sound for the same reason that guard is sound anywhere: the guess picks
// which compare is emitted and nothing else. The runtime stamps a shape with a
// class only after checking that class's whole field list against the shape,
// name by name, so a receiver of any other shape (a plain object with the same
// key, a subclass's late field, null, a number) fails the compare and takes
// the inline cache. A wrong guess costs the compare. It cannot give a wrong
// load, and it changes no type: the read's value stays as dynamic as before.
//
// Limited to a receiver that is itself a read of a proven class's field, so
// the guess is made only where a value flows from one class's slot to another
// class's key. A receiver the program built some other way (a parameter, an
// array element, a call's result) keeps whatever the existing arms decide.
//
// BRONZE_NO_KEY_OWNER_GUESS=1 turns it off for A/B.

#include "lower/lowerer.h"

#include <cstdlib>

namespace bronze::lower {

bool Lowerer::keyOwnerGuessSeamDisabled() {
    return std::getenv("BRONZE_NO_KEY_OWNER_GUESS") != nullptr;
}

types::ShapeClassId Lowerer::keyOwnerFamily(const std::string& key) {
    if (auto it = keyOwnerCache_.find(key); it != keyOwnerCache_.end()) return it->second;
    const types::ClassLayoutTable& layouts = inference_->classLayouts;
    // The topmost class in the forest that declares `key`: the preorder visits
    // a class before every class that extends it, so the first one found is
    // the root of any family that holds the key.
    const types::ClassLayout* root = nullptr;
    for (const types::ClassLayout* c : layouts.familyPreorder()) {
        if (layouts.slotOf(c->shapeClass, key) != types::ClassLayoutTable::kNoSlot) {
            root = c;
            break;
        }
    }
    types::ShapeClassId owner = types::kNoShapeClass;
    if (root != nullptr && root->familyIndex != types::ClassLayout::kNoFamily) {
        owner = root->shapeClass;
        const uint32_t lo = root->familyIndex;
        const uint32_t hi = lo + root->familySpan;
        for (const types::ClassLayout& c : layouts.all()) {
            bool names = false, accessor = false;
            for (const std::string& f : c.fields) names = names || f == key;
            for (const std::string& f : c.lateFields) names = names || f == key;
            for (const std::string& f : c.accessorNames) accessor = accessor || f == key;
            if (!names && !accessor) continue;
            // A class that names the key outside the subtree (another root's
            // field of the same name, an unproven layout) means two kinds of
            // receiver carry it, and the guess is a coin toss. An accessor of
            // that name anywhere means some receiver's read is a call.
            const bool inside = c.familyIndex != types::ClassLayout::kNoFamily &&
                                c.familyIndex >= lo && c.familyIndex <= hi;
            if (!inside || accessor) {
                owner = types::kNoShapeClass;
                break;
            }
        }
    }
    keyOwnerCache_.emplace(key, owner);
    return owner;
}

types::ShapeClassId Lowerer::keyOwnerGuessAt(const ast::Expr& receiver, const std::string& key) {
    if (keyOwnerGuessDisabled_) return types::kNoShapeClass;
    const auto* ma = dynamic_cast<const ast::MemberAccess*>(&receiver);
    if (ma == nullptr || ma->isPrivate || ma->optional || ma->object == nullptr) {
        return types::kNoShapeClass;
    }
    // The receiver must be read out of a proven class's slot.
    if (inference_->staticSlotAt(&inferenceExpr(*ma->object), ma->property) ==
        types::ClassLayoutTable::kNoSlot) {
        return types::kNoShapeClass;
    }
    return keyOwnerFamily(key);
}

}  // namespace bronze::lower
