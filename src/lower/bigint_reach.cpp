#include "lower/bigint_reach.h"

#include <array>
#include <string_view>

#include "types/walk.h"

namespace bronze::lower {
namespace {

// Every spelling that can name or build a BigInt. `BigInt` covers the
// constructor, `asIntN`/`asUintN` and `typeof x === "bigint"` comparisons
// reach it through this name too; the views and the DataView accessors are the
// non-constructor routes by which one arrives already built.
constexpr std::array<std::string_view, 7> kBigIntNames = {
    "BigInt", "BigInt64Array", "BigUint64Array",
    "getBigInt64", "getBigUint64", "setBigInt64", "setBigUint64",
};

bool namesBigInt(std::string_view name) {
    for (std::string_view candidate : kBigIntNames) {
        if (name == candidate) return true;
    }
    return false;
}

class BigIntScan final : public types::Walker {
public:
    bool found = false;

    // The literal suffix: `1n`. The only route that is not a name.
    void visit(const ast::BigIntLit&) override { found = true; }

    // Any identifier, in any position — a read, a write, a shorthand, a
    // re-export. A program that never spells one of these names cannot reach
    // the constructor or a view, whatever it does with the values it has.
    void visit(const ast::Ident& n) override {
        if (namesBigInt(n.name)) found = true;
    }

    // `dv.getBigInt64(0)` names its accessor here rather than as an Ident, and
    // `globalThis.BigInt` reaches the constructor the same way.
    void visit(const ast::MemberAccess& n) override {
        if (!n.isPrivate && namesBigInt(n.property)) found = true;
        types::Walker::visit(n);
    }
};

}  // namespace

bool bigIntMayReach(const ast::Module& module,
                    const std::vector<std::string>& hostGlobals) {
    // A host that registers one of these names can hand the program a BigInt
    // without the program ever spelling the name itself.
    for (const std::string& global : hostGlobals) {
        if (namesBigInt(global)) return true;
    }
    BigIntScan scan;
    for (const auto& stmt : module.body) stmt->accept(scan);
    return scan.found;
}

}  // namespace bronze::lower
