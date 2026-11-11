#include "lower/bigint_reach.h"

#include <array>
#include <string_view>

#include "types/walk.h"

namespace bronze::lower {
namespace {

// The spellings a program that does arithmetic on BigInts uses. `BigInt`
// covers the constructor, `asIntN`/`asUintN` and `typeof x === "bigint"`
// comparisons reach it through this name too; `bigint` is the member hosts
// name a 64-bit clock or stat by (`process.hrtime.bigint()`).
//
// The BigInt views and DataView's 64-bit accessors are NOT here. They hand
// over a BigInt already built, but arithmetic on one needs another BigInt,
// and a program has no way to write one without the literal suffix or the
// constructor: `view[0] * view[1]` and `view[0]++` are the only forms that
// spell neither, and in a program that spells neither they throw the
// TypeError ToNumber gives a BigInt. That is this compiler's rule and not the
// language's. What it buys: pixi has one lazy `BigUint64Array` getter nothing
// calls, and with the view names counted that one mention boxed every `++`,
// `*`, `-`, `/` and `%` over a dynamic operand in the whole library
// (pixi_graphics ran 3% more instructions for it).
constexpr std::array<std::string_view, 2> kBigIntNames = {"BigInt", "bigint"};

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
