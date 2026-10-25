// TypeScript `enum` and `const enum`, lowered to the JavaScript TypeScript
// emits for them.
//
// An enum block is an object built member by member: a numeric member `A = 1`
// is `E[E["A"] = 1] = "A"` — the forward key and the reverse mapping from the
// value back to the name — and a string member `S = "s"` is `E["S"] = "s"`,
// with no reverse mapping. A member's initializer is folded when it is a
// constant expression over literals and earlier members, exactly as the
// TypeScript compiler folds it; otherwise it is evaluated where the member is
// defined, with every bare mention of a member read through the enum (inside
// the body, `A` means `E.A`), and it gets the reverse mapping too, since what
// it evaluates to is not known until then.
//
// The emitted shape is TypeScript's:
//
//     let E = (function (E) { E[E["A"] = 0] = "A"; …; return E; })({});
//
// and a later block of the same enum in the same scope — declaration merging —
// adds its members to the same object: `(function (E) { … })(E)`. An enum
// whose members are all constants and which nothing has merged into is
// written as the object literal those statements would build, which is the
// same object and a cheaper one to compile.
//
// `const enum` is emitted as a plain enum, as node's TypeScript transform does
// (its uses are not inlined, so the object has to exist). `declare enum` is
// ambient and emits nothing (parseDeclareStmt).

#include <cmath>
#include <unordered_set>
#include <variant>

#include "parse/parser.h"
#include "parse/parser_ts_rewrite.h"

namespace bronze {

using namespace ast;
using namespace ts_rewrite;

namespace {

using EnumVal = std::variant<std::monostate, double, std::string>;

struct EnumContext {
    const std::string& enumName;
    const std::unordered_map<std::string, double>& numbers;
    const std::unordered_map<std::string, std::string>& strings;

    EnumVal member(const std::string& name) const {
        auto nit = numbers.find(name);
        if (nit != numbers.end()) return nit->second;
        auto sit = strings.find(name);
        if (sit != strings.end()) return sit->second;
        return std::monostate{};
    }
};

// ECMA-262 6.1.6.1.3, Number::exponentiate. std::pow agrees except where
// IEEE 754's pow and ECMAScript's `**` part ways: a NaN exponent is always
// NaN, and ±1 to an infinite power is NaN rather than 1.
double exponentiate(double base, double exponent) {
    if (std::isnan(exponent)) return std::nan("");
    if (exponent == 0.0) return 1.0;
    if (std::fabs(base) == 1.0 && std::isinf(exponent)) return std::nan("");
    return std::pow(base, exponent);
}

// The value of a constant enum expression (TypeScript's "constant enum
// expression": literals, earlier members by bare name or through the enum,
// and the unary and binary arithmetic operators over them), or monostate for
// anything that has to be evaluated at run time.
EnumVal evalEnumExpr(const Expr* expr, const EnumContext& cx) {
    if (!expr) return std::monostate{};
    if (auto* n = dynamic_cast<const NumberLit*>(expr)) return n->value;
    if (auto* s = dynamic_cast<const StringLit*>(expr)) return s->value;
    if (auto* t = dynamic_cast<const TemplateLit*>(expr)) {
        if (t->exprs.empty() && !t->quasis.empty()) return t->quasis[0];
        return std::monostate{};
    }
    if (auto* id = dynamic_cast<const Ident*>(expr)) return cx.member(id->name);
    if (auto* mem = dynamic_cast<const MemberAccess*>(expr)) {
        auto* base = dynamic_cast<const Ident*>(mem->object.get());
        if (base && base->name == cx.enumName && !mem->optional) return cx.member(mem->property);
        return std::monostate{};
    }
    if (auto* idx = dynamic_cast<const IndexAccess*>(expr)) {
        auto* base = dynamic_cast<const Ident*>(idx->object.get());
        auto* key = dynamic_cast<const StringLit*>(idx->index.get());
        if (base && key && base->name == cx.enumName && !idx->optional) {
            return cx.member(key->value);
        }
        return std::monostate{};
    }
    if (auto* u = dynamic_cast<const Unary*>(expr)) {
        const EnumVal op = evalEnumExpr(u->operand.get(), cx);
        if (!std::holds_alternative<double>(op)) return std::monostate{};
        const double v = std::get<double>(op);
        switch (u->op) {
            case UnaryOp::Posate:
                return +v;
            case UnaryOp::Negate:
                return -v;
            case UnaryOp::BitNot:
                return static_cast<double>(~toInt32(v));
            default:
                return std::monostate{};
        }
    }
    if (auto* b = dynamic_cast<const Binary*>(expr)) {
        const EnumVal l = evalEnumExpr(b->lhs.get(), cx);
        const EnumVal r = evalEnumExpr(b->rhs.get(), cx);
        if (std::holds_alternative<std::monostate>(l) || std::holds_alternative<std::monostate>(r)) {
            return std::monostate{};
        }
        if (b->op == BinaryOp::Add &&
            (std::holds_alternative<std::string>(l) || std::holds_alternative<std::string>(r))) {
            auto text = [](const EnumVal& v) {
                return std::holds_alternative<std::string>(v) ? std::get<std::string>(v)
                                                              : numberToString(std::get<double>(v));
            };
            return text(l) + text(r);
        }
        if (!std::holds_alternative<double>(l) || !std::holds_alternative<double>(r)) {
            return std::monostate{};
        }
        const double lv = std::get<double>(l);
        const double rv = std::get<double>(r);
        const uint32_t shift = toUint32(rv) & 31u;
        switch (b->op) {
            case BinaryOp::Add:
                return lv + rv;
            case BinaryOp::Sub:
                return lv - rv;
            case BinaryOp::Mul:
                return lv * rv;
            case BinaryOp::Div:
                return lv / rv;
            case BinaryOp::Mod:
                return std::fmod(lv, rv);
            case BinaryOp::Exp:
                return exponentiate(lv, rv);
            case BinaryOp::BitOr:
                return static_cast<double>(toInt32(lv) | toInt32(rv));
            case BinaryOp::BitAnd:
                return static_cast<double>(toInt32(lv) & toInt32(rv));
            case BinaryOp::BitXor:
                return static_cast<double>(toInt32(lv) ^ toInt32(rv));
            case BinaryOp::Shl:
                return static_cast<double>(static_cast<int32_t>(toUint32(lv) << shift));
            case BinaryOp::Shr:
                return static_cast<double>(toInt32(lv) >> shift);
            case BinaryOp::UShr:
                return static_cast<double>(toUint32(lv) >> shift);
            default:
                return std::monostate{};
        }
    }
    return std::monostate{};
}

struct EnumMember {
    std::string name;
    Span span;
    EnumVal value;
    ExprPtr runtime;  // the initializer to evaluate, when `value` is not a constant
};

// A numeric value the object-literal form can spell as a key at compile time:
// an integer the key's ToString is the plain decimal of.
bool isPlainIntegerKey(double v) {
    return std::isfinite(v) && std::floor(v) == v && std::fabs(v) < 1e21;
}

}  // namespace

Parser::EnumRecord* Parser::findEnumRecord(const std::vector<StmtPtr>& scope,
                                           const std::string& name) {
    for (auto& rec : enumRecords_) {
        if (rec.scope != &scope || rec.name != name) continue;
        for (const auto& s : scope) {
            if (s.get() == rec.marker) return &rec;
        }
    }
    return nullptr;
}

bool Parser::parseEnumDecl(std::vector<StmtPtr>& out, std::string* outName,
                           const std::string* exportInto) {
    const Span startSpan = peek().span;
    match(TokenKind::KwConst);  // `const enum` is emitted as an enum
    advance();                  // 'enum'
    const Token* nameTok = expectPropertyName("enum name");
    if (!nameTok) return false;
    const std::string enumName(nameTok->text);
    declareType(enumName, ast::kTypeDeclEnum);
    if (outName) *outName = enumName;
    if (!expect(TokenKind::LBrace, "'{' after enum name")) return false;

    EnumRecord* prior = findEnumRecord(out, enumName);
    std::unordered_map<std::string, double> numbers;
    std::unordered_map<std::string, std::string> strings;
    std::vector<std::string> known;
    if (prior) {
        numbers = prior->numbers;
        strings = prior->strings;
        known = prior->members;
    }
    const EnumContext cx{enumName, numbers, strings};

    std::vector<EnumMember> members;
    double next = 0.0;
    bool hasNext = true;
    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
        EnumMember m;
        m.span = peek().span;
        if (check(TokenKind::StringLiteral)) {
            const Token& sTok = advance();
            m.name = decodeStringLiteral(sTok.text.substr(1, sTok.text.size() - 2), sTok.span);
        } else if (check(TokenKind::LBracket)) {
            // TypeScript admits a computed member name only when it is a
            // string literal, which is then just that name.
            advance();
            if (!check(TokenKind::StringLiteral)) {
                error("computed property names are not allowed in enums");
                return false;
            }
            const Token& sTok = advance();
            m.name = decodeStringLiteral(sTok.text.substr(1, sTok.text.size() - 2), sTok.span);
            if (!expect(TokenKind::RBracket, "']' after a computed enum member name")) return false;
        } else {
            const Token* mTok = expectPropertyName("enum member name");
            if (!mTok) return false;
            m.name = std::string(mTok->text);
        }

        if (match(TokenKind::Assign)) {
            ExprPtr init = parseAssign();
            if (!init) return false;
            m.value = evalEnumExpr(init.get(), cx);
            if (std::holds_alternative<std::monostate>(m.value)) {
                // Evaluated at run time, inside the enum's body: a bare
                // member name there is the member, read through the object.
                QualifyNames qualify(diags_, std::set<std::string>(known.begin(), known.end()),
                                     enumName);
                if (!qualify.walkExpr(init)) return false;
                m.runtime = std::move(init);
            }
        } else if (hasNext) {
            m.value = next;
        } else {
            // A member with no initializer after a string or a computed one
            // has no value TypeScript can give it; node's transform writes
            // `void 0`, and so does this.
            auto undef = std::make_unique<UndefinedLit>();
            undef->span = m.span;
            m.runtime = std::move(undef);
        }

        numbers.erase(m.name);
        strings.erase(m.name);
        if (std::holds_alternative<double>(m.value)) {
            const double v = std::get<double>(m.value);
            numbers[m.name] = v;
            next = v + 1.0;
            hasNext = true;
        } else {
            if (std::holds_alternative<std::string>(m.value)) {
                strings[m.name] = std::get<std::string>(m.value);
            }
            hasNext = false;
        }
        known.push_back(m.name);
        members.push_back(std::move(m));
        if (!match(TokenKind::Comma) && !match(TokenKind::Semicolon)) break;
    }
    if (!expect(TokenKind::RBrace, "'}' after enum body")) return false;
    const Span fullSpan = {startSpan.begin, previous().span.end};

    // The object-literal form: a first block, every member a constant, and
    // every key — names and reverse mappings alike — distinct, so that the
    // literal builds exactly the object the statements would.
    bool literal = !prior && !exportInto;
    std::unordered_set<std::string> keys;
    for (const auto& m : members) {
        if (!literal) break;
        if (m.runtime) literal = false;
        if (!keys.insert(m.name).second) literal = false;
        if (std::holds_alternative<double>(m.value)) {
            const double v = std::get<double>(m.value);
            if (!isPlainIntegerKey(v) || !keys.insert(numberToString(v)).second) literal = false;
        }
    }

    if (literal) {
        auto obj = std::make_unique<ObjectLit>();
        obj->span = fullSpan;
        for (auto& m : members) {
            ObjectProp prop;
            prop.key = m.name;
            if (std::holds_alternative<double>(m.value)) {
                const double v = std::get<double>(m.value);
                prop.value = makeNumber(v, m.span);
                obj->props.push_back(std::move(prop));
                ObjectProp rev;
                rev.key = numberToString(v);
                rev.value = makeString(m.name, m.span);
                obj->props.push_back(std::move(rev));
            } else {
                prop.value = makeString(std::get<std::string>(m.value), m.span);
                obj->props.push_back(std::move(prop));
            }
        }
        auto decl = std::make_unique<VarDecl>();
        decl->span = fullSpan;
        decl->isConst = true;
        decl->name = enumName;
        decl->init = std::move(obj);
        VarDecl* declPtr = decl.get();
        out.push_back(std::move(decl));
        enumRecords_.push_back(
            {&out, out.back().get(), enumName, declPtr, known, numbers, strings});
        return true;
    }

    // `E[E["A"] = v] = "A"` for a numeric or run-time member, `E["S"] = "s"`
    // for a string one.
    std::vector<StmtPtr> body;
    for (auto& m : members) {
        ExprPtr value;
        bool reverse = true;
        if (m.runtime) {
            value = std::move(m.runtime);
        } else if (std::holds_alternative<double>(m.value)) {
            value = makeNumber(std::get<double>(m.value), m.span);
        } else {
            value = makeString(std::get<std::string>(m.value), m.span);
            reverse = false;
        }
        ExprPtr forward = makeBinary(
            BinaryOp::Assign,
            makeIndex(makeIdent(enumName, m.span), makeString(m.name, m.span), m.span),
            std::move(value), m.span);
        if (reverse) {
            forward = makeBinary(BinaryOp::Assign,
                                 makeIndex(makeIdent(enumName, m.span), std::move(forward), m.span),
                                 makeString(m.name, m.span), m.span);
        }
        body.push_back(makeExprStmt(std::move(forward), m.span));
    }

    if (exportInto) {
        // `(function (E) { … })(ns.E || (ns.E = {}))`: the object is the
        // namespace's property, made by the first block that runs.
        ExprPtr arg = makeBinary(
            BinaryOp::LogicalOr, makeMember(makeIdent(*exportInto, fullSpan), enumName, fullSpan),
            makeBinary(BinaryOp::Assign,
                       makeMember(makeIdent(*exportInto, fullSpan), enumName, fullSpan),
                       std::make_unique<ObjectLit>(), fullSpan),
            fullSpan);
        out.push_back(makeExprStmt(makeIife(enumName, std::move(body), std::move(arg), strict_,
                                            fullSpan),
                                   fullSpan));
    } else if (prior) {
        // A merged block adds to the object the first one made. That first
        // block may have been the `const` literal form, which is a binding
        // nothing may assign; a merge makes it an ordinary `let`.
        if (prior->decl) prior->decl->isConst = false;
        out.push_back(makeExprStmt(makeIife(enumName, std::move(body),
                                            makeIdent(enumName, fullSpan), strict_, fullSpan),
                                   fullSpan));
    } else {
        auto ret = std::make_unique<ReturnStmt>();
        ret->span = fullSpan;
        ret->value = makeIdent(enumName, fullSpan);
        body.push_back(std::move(ret));
        auto decl = std::make_unique<VarDecl>();
        decl->span = fullSpan;
        decl->name = enumName;
        decl->init = makeIife(enumName, std::move(body), std::make_unique<ObjectLit>(), strict_,
                              fullSpan);
        VarDecl* declPtr = decl.get();
        out.push_back(std::move(decl));
        enumRecords_.push_back(
            {&out, out.back().get(), enumName, declPtr, known, numbers, strings});
        return true;
    }

    if (prior) {
        prior->members = known;
        prior->numbers = numbers;
        prior->strings = strings;
    } else {
        enumRecords_.push_back({&out, out.back().get(), enumName, nullptr, known, numbers, strings});
    }
    return true;
}

}  // namespace bronze
