// THE PIN CENSUS, lowering half (`--census`, src/runtime/pin_census.h, C1).
//
// This file is the mirror image of lower_pin.cpp. That one emits a barrier
// where a manifest HAS made a claim; this one emits an observation where a
// manifest COULD make one and no part of the compiler can.
//
// The "and no part of the compiler can" is the whole design, and it is stage
// E4's HANDOFF (c) written as code: the closure parameter proof, the env-slot
// fixpoint and the signature join all run BEFORE any site here is created, and
// every one of them removes sites. A parameter the proof typed f64 is not a
// census site, because there is no manifest line to write for it — the compiler
// already knows. What is left is exactly the residue a hand-written manifest
// was for: the escaped closure the enumeration cannot follow, the slot written
// from a parameter, the field whose values nothing audits.
//
// A census build is an INSTRUMENT. It is never benchmarked, never shipped and
// never linked into anything that is, so every decision here is made for
// clarity over cost: one plain call per site, a string per target, no fast
// path.

#include "lower/lowerer.h"

#include <map>
#include <set>

#include "types/class_layout.h"
#include "types/result.h"

namespace bronze::lower {

namespace {

// The LAST dotted component, which is how a manifest spells a class or a
// captured binding's owner: the module linker renames every module-level
// binding into one namespace before lowering runs, and a manifest is written
// against the source (types/pins.h).
std::string lastComponent(const std::string& name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(dot + 1);
}

// The manifest's identifier grammar, which is types/pins.cpp's `isIdent` and
// `isDottedIdent` and has to stay them: a target this predicate accepts and the
// parser then rejects is a manifest that FAILS THE BUILD, which is the one
// outcome the census exists to prevent.
//
// It is not a hypothetical. A class accessor lowers to an IL function named
// `Euler.set x` — a space in the middle — so a run of the three.js oracle
// proposed `param Euler.set x(value): number` and the build handed that file
// refused to parse it. A getter, a computed method name, a quoted field key
// (`o["a b"] = 1`) and a `Symbol`-keyed member all reach here the same way.
bool isIdentStart(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$';
}
bool isIdent(const std::string& s) {
    if (s.empty() || !isIdentStart(s[0])) return false;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!isIdentStart(s[i]) && (s[i] < '0' || s[i] > '9')) return false;
    }
    return true;
}
bool isDottedIdent(const std::string& s) {
    size_t begin = 0;
    while (true) {
        const auto dot = s.find('.', begin);
        if (!isIdent(s.substr(begin, dot == std::string::npos ? dot : dot - begin))) return false;
        if (dot == std::string::npos) return true;
        begin = dot + 1;
    }
}

// Can a manifest LINE be written for this target at all? The target is already
// in the file's own spelling, so the test is the parser's, applied to the parts
// the parser will split out.
bool emittableTarget(const std::string& target, il::CensusSite kind) {
    switch (kind) {
        case il::CensusSite::Param: {
            const auto open = target.find('(');
            if (target.rfind("param ", 0) != 0 || open == std::string::npos ||
                target.back() != ')') {
                return false;
            }
            return isDottedIdent(target.substr(6, open - 6)) &&
                   isIdent(target.substr(open + 1, target.size() - open - 2));
        }
        case il::CensusSite::Return:
            return target.rfind("return ", 0) == 0 && isDottedIdent(target.substr(7));
        case il::CensusSite::EnvSlot: {
            if (target.rfind("function ", 0) != 0) return false;
            const std::string rest = target.substr(9);
            const auto dot = rest.rfind('.');
            if (dot == std::string::npos) return false;
            return isDottedIdent(rest.substr(0, dot)) && isIdent(rest.substr(dot + 1));
        }
        case il::CensusSite::Field: {
            const auto dot = target.rfind('.');
            if (dot == std::string::npos) return false;
            return isIdent(target.substr(0, dot)) && isIdent(target.substr(dot + 1));
        }
        case il::CensusSite::OpaqueFieldStore:
            return isIdent(target);
    }
    return false;
}

}  // namespace

// The pin's message exists to be grepped for in the manifest, so it has to read
// back as a line of the file — `param Uniform.setValue(x): number`, not
// `param mod1.Uniform.setValue(x): number`. Only the leading module component
// is dropped, because a `param` entry's owner may legitimately be two
// components deep and `Matrix4.multiplyMatrices` is not a prefix to strip.
std::string Lowerer::manifestOwnerName(const std::string& ilName) {
    const auto dot = ilName.find('.');
    if (dot == std::string::npos || dot < 4 || ilName.compare(0, 3, "mod") != 0) return ilName;
    for (size_t i = 3; i < dot; ++i) {
        if (ilName[i] < '0' || ilName[i] > '9') return ilName;
    }
    return ilName.substr(dot + 1);
}

void Lowerer::addCensusSite(const std::string& target, il::CensusSite kind, bool refuses) {
    if (!censusEnabled() || target.empty()) return;
    if (!emittableTarget(target, kind)) {
        // A name no manifest line can spell. An OPAQUE row is dropped outright
        // — it names no entry, so it can mark none `@observed`, and a row for it
        // could only ever be noise. Everything else becomes a REFUSAL, because
        // the shape is real and a reader of the file should see that the census
        // met it and could not write it down.
        if (kind == il::CensusSite::OpaqueFieldStore) return;
        refuses = true;
    }
    il::Module::CensusSiteEntry entry;
    entry.keyIndex = getKeyConstantIndex(target);
    entry.info = static_cast<uint32_t>(kind) | (refuses ? BRONZE_ABI_CENSUS_REFUSES : 0u);
    ilModule_.censusSites.push_back(entry);
    if (kind == il::CensusSite::Param || kind == il::CensusSite::Return ||
        kind == il::CensusSite::EnvSlot) {
        censusSignatureOwners_.emplace_back(target, kind);
    }
}

void Lowerer::emitCensusRecord(Value val, const std::string& target, il::CensusSite kind,
                               il::Function& ilFn) {
    if (!censusEnabled() || target.empty()) return;
    addCensusSite(target, kind, /*refuses=*/false);
    // The row above is a refusal now, and a refusal outranks every observation
    // in the writer (`refusalOf`), so the instruction would only cost the run
    // time it takes to be ignored.
    if (!emittableTarget(target, kind)) return;
    // The BOXED form, for the reason the barrier tests the boxed form: it is
    // the one word every shape question is asked of, and it is the word the
    // store about to follow will write.
    Value boxed = boxValueIfNeeded(val, ilFn);
    il::Instruction inst;
    inst.op = il::Op::CensusRecord;
    inst.type = il::Type::Void;
    inst.result = il::kNoValue;
    inst.operands = {boxed.id};
    inst.keyIndex = getKeyConstantIndex(target);
    inst.immI32 = static_cast<int32_t>(static_cast<uint32_t>(kind));
    emitInst(ilFn, inst);
}

std::string Lowerer::censusFieldOwner(const ast::Expr& receiver, bool* opaque) const {
    if (opaque != nullptr) *opaque = true;
    if (inference_ == nullptr) return {};
    const types::Type recv = inferredType(receiver);
    // Inference types this receiver `dynamic`, or as an object it cannot place
    // in a shape class. This is B1's negative 1 exactly: a store here gets no
    // barrier while a class-known read elsewhere still spends the claim, so
    // whatever entry this field name reaches is enforced only in part.
    if (!recv.is(types::TypeKind::Object) || recv.shapeClass() == types::kNoShapeClass) {
        return {};
    }
    const types::ClassLayout* layout = inference_->classLayouts.byShapeClass(recv.shapeClass());
    const std::string name = layout != nullptr
                                 ? layout->name
                                 : inference_->shapes.at(recv.shapeClass()).constructorName;
    if (name.empty()) {
        // A shape class with no class name: an object literal, or a shape the
        // program builds without a constructor. Inference DID type this
        // receiver — it just typed it as something no `<Class>.<field>` entry
        // can name, so a read through it spends no claim either and there is no
        // promise for this store to break. Not opaque, and not a site.
        //
        // THE FORK, recorded because the other answer is defensible: treating
        // these as opaque too would mark almost every field entry on a real
        // library `@observed`, on the strength of a store whose receiver the
        // read side cannot reach. types/pins.h names the hole as "a receiver
        // inference types `dynamic`", and this is not that.
        if (opaque != nullptr) *opaque = false;
        return {};
    }
    if (opaque != nullptr) *opaque = false;
    return lastComponent(name);
}

void Lowerer::emitCensusFieldRecord(const ast::Expr& receiver, const std::string& key, Value val,
                                    il::Function& ilFn) {
    // With `--no-infer` there is no receiver type anywhere, so every store
    // would register an opaque row and mark every entry in the file
    // `@observed` — on the strength of a compilation mode that also produces no
    // field entries at all. The census asks the same analysis the pins do, or
    // it asks nothing.
    if (!censusEnabled() || inference_ == nullptr) return;
    bool opaque = false;
    const std::string owner = censusFieldOwner(receiver, &opaque);
    // An INDEX, not a field. `te[0] = x` reaches this on the literal-key path,
    // and a manifest cannot spell `<Class>.0` at all (types/pins.cpp `isIdent`
    // refuses a leading digit), so a row for it could only ever be noise —
    // nineteen of them on `Matrix4` alone. Dropped before the opaque arm below,
    // because the harm a spurious row does is real: it marks entries
    // `@observed` on the strength of a store no entry could name.
    if (!key.empty() && key.find_first_not_of("0123456789") == std::string::npos) return;
    if (opaque) {
        // Not an entry — it names no class — and not an observation either. The
        // ROW is the whole record: it says the program contains a store to a
        // field of this name that B1's barrier cannot reach, which is what
        // marks every entry for that name `@observed`. Registered rather than
        // observed on purpose: a dynamic store on a path this run did not take
        // is compiled and unguarded all the same.
        addCensusSite(key, il::CensusSite::OpaqueFieldStore, /*refuses=*/false);
        return;
    }
    if (owner.empty()) return;
    emitCensusRecord(val, owner + "." + key, il::CensusSite::Field, ilFn);
}

// An entry matches an IL function by SUFFIX (types/pins.cpp `forEachSpelling`):
// `param clamp(x): number` written for a module function `mod4.clamp` also
// governs `mod7.Bar.clamp`, because `clamp` is a spelling of that name too. If
// the two disagree about what `x` holds the manifest is a wrong promise; worse,
// if the second's `x` has a default or is a rest parameter, `applySignaturePins`
// refuses the entry and the BUILD FAILS.
//
// So an owner spelling that reaches more than one IL function is refused
// outright. Conservative — the two may well agree — and cheap, because the
// alternative is a manifest whose failure mode is a compile error in the build
// the census exists to make possible.
void Lowerer::refuseAmbiguousCensusOwners() {
    if (!censusEnabled() || censusSignatureOwners_.empty()) return;

    // Every spelling of every IL function name, and how many distinct functions
    // each reaches. `mod1.Matrix4.multiplyMatrices` is reachable as itself, as
    // `Matrix4.multiplyMatrices` and as `multiplyMatrices`. Keyed on function
    // IDENTITY, not name: two factories each declaring a nested `get` produce
    // two distinct IL functions with the SAME name (three.js does exactly this,
    // one of them with a defaulted parameter), and a set of names would
    // collapse them into an owner that looks unambiguous.
    std::map<std::string, std::set<const il::Function*>> spellingOwners;
    // The env-slot form matches on the LAST component alone
    // (`PinManifest::envSlotPinned`), which is a narrower rule and needs its
    // own table.
    std::map<std::string, std::set<const il::Function*>> lastComponentOwners;
    for (const il::Function& fn : ilModule_.functions) {
        if (fn.name.empty()) continue;
        spellingOwners[fn.name].insert(&fn);
        size_t begin = 0;
        while ((begin = fn.name.find('.', begin)) != std::string::npos) {
            ++begin;
            spellingOwners[fn.name.substr(begin)].insert(&fn);
        }
        lastComponentOwners[lastComponent(fn.name)].insert(&fn);
    }

    std::vector<std::pair<std::string, il::CensusSite>> refusals;
    for (const auto& [target, kind] : censusSignatureOwners_) {
        std::string owner;
        if (kind == il::CensusSite::Param) {
            // `param <owner>(<p>)`
            const auto open = target.find('(');
            if (open == std::string::npos || target.rfind("param ", 0) != 0) continue;
            owner = target.substr(6, open - 6);
        } else if (kind == il::CensusSite::Return) {
            if (target.rfind("return ", 0) != 0) continue;
            owner = target.substr(7);
        } else {
            // `function <owner>.<binding>`
            if (target.rfind("function ", 0) != 0) continue;
            const std::string rest = target.substr(9);
            const auto dot = rest.rfind('.');
            if (dot == std::string::npos) continue;
            owner = rest.substr(0, dot);
        }
        const auto& table =
            kind == il::CensusSite::EnvSlot ? lastComponentOwners : spellingOwners;
        const auto it = table.find(owner);
        if (it != table.end() && it->second.size() > 1) refusals.push_back({target, kind});
    }
    // Appended AFTER the walk, and through the module rather than through
    // `addCensusSite`: that one files the row under its owner too, and a
    // refusal filed as an owner would be walked again by the next call.
    for (const auto& [target, kind] : refusals) {
        il::Module::CensusSiteEntry entry;
        entry.keyIndex = getKeyConstantIndex(target);
        entry.info = static_cast<uint32_t>(kind) | BRONZE_ABI_CENSUS_REFUSES;
        ilModule_.censusSites.push_back(entry);
    }
}

}  // namespace bronze::lower
