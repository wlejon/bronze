// TypeScript statement and declaration constructs: `type`, `interface`,
// `enum` / `const enum`, `namespace` / `module`, `declare`, and `abstract`.

#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <variant>

#include "parse/parser.h"

namespace bronze {

using namespace ast;

namespace {

using EnumVal = std::variant<std::monostate, double, std::string>;

std::string formatEnumNum(double v) {
    if (std::isfinite(v) && std::floor(v) == v && v >= -9007199254740991.0 &&
        v <= 9007199254740991.0) {
        return std::to_string(static_cast<int64_t>(v));
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

EnumVal evalEnumExpr(const Expr* expr, const std::string& enumName,
                     const std::unordered_map<std::string, double>& numMap,
                     const std::unordered_map<std::string, std::string>& strMap) {
    if (!expr) return std::monostate{};
    if (auto* n = dynamic_cast<const NumberLit*>(expr)) {
        return n->value;
    }
    if (auto* s = dynamic_cast<const StringLit*>(expr)) {
        return s->value;
    }
    if (auto* t = dynamic_cast<const TemplateLit*>(expr)) {
        if (t->exprs.empty() && !t->quasis.empty()) {
            return t->quasis[0];
        }
        return std::monostate{};
    }
    if (auto* id = dynamic_cast<const Ident*>(expr)) {
        auto nit = numMap.find(id->name);
        if (nit != numMap.end()) return nit->second;
        auto sit = strMap.find(id->name);
        if (sit != strMap.end()) return sit->second;
        return std::monostate{};
    }
    if (auto* mem = dynamic_cast<const MemberAccess*>(expr)) {
        if (auto* base = dynamic_cast<const Ident*>(mem->object.get())) {
            if (base->name == enumName) {
                auto nit = numMap.find(mem->property);
                if (nit != numMap.end()) return nit->second;
                auto sit = strMap.find(mem->property);
                if (sit != strMap.end()) return sit->second;
            }
        }
        return std::monostate{};
    }
    if (auto* u = dynamic_cast<const Unary*>(expr)) {
        EnumVal op = evalEnumExpr(u->operand.get(), enumName, numMap, strMap);
        if (!std::holds_alternative<double>(op)) return std::monostate{};
        const double v = std::get<double>(op);
        switch (u->op) {
            case UnaryOp::Posate:
                return +v;
            case UnaryOp::Negate:
                return -v;
            case UnaryOp::BitNot:
                return static_cast<double>(~static_cast<int32_t>(static_cast<int64_t>(v)));
            case UnaryOp::Not:
                return (v == 0.0 || std::isnan(v)) ? 1.0 : 0.0;
            default:
                return std::monostate{};
        }
    }
    if (auto* b = dynamic_cast<const Binary*>(expr)) {
        EnumVal l = evalEnumExpr(b->lhs.get(), enumName, numMap, strMap);
        EnumVal r = evalEnumExpr(b->rhs.get(), enumName, numMap, strMap);
        if (b->op == BinaryOp::Add) {
            if (std::holds_alternative<std::string>(l) && std::holds_alternative<std::string>(r)) {
                return std::get<std::string>(l) + std::get<std::string>(r);
            }
            if (std::holds_alternative<std::string>(l) && std::holds_alternative<double>(r)) {
                return std::get<std::string>(l) + formatEnumNum(std::get<double>(r));
            }
            if (std::holds_alternative<double>(l) && std::holds_alternative<std::string>(r)) {
                return formatEnumNum(std::get<double>(l)) + std::get<std::string>(r);
            }
        }
        if (!std::holds_alternative<double>(l) || !std::holds_alternative<double>(r)) {
            return std::monostate{};
        }
        const double lv = std::get<double>(l);
        const double rv = std::get<double>(r);
        const auto li = static_cast<int32_t>(static_cast<int64_t>(lv));
        const auto ri = static_cast<int32_t>(static_cast<int64_t>(rv));
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
                return std::pow(lv, rv);
            case BinaryOp::BitOr:
                return static_cast<double>(li | ri);
            case BinaryOp::BitAnd:
                return static_cast<double>(li & ri);
            case BinaryOp::BitXor:
                return static_cast<double>(li ^ ri);
            case BinaryOp::Shl:
                return static_cast<double>(li << (ri & 31));
            case BinaryOp::Shr:
                return static_cast<double>(li >> (ri & 31));
            case BinaryOp::UShr:
                return static_cast<double>(static_cast<uint32_t>(li) >> (ri & 31));
            default:
                return std::monostate{};
        }
    }
    return std::monostate{};
}

template <typename AtEndFn, typename MatchFn, typename AdvanceFn>
void skipBalancedBraces(AtEndFn&& atEnd, MatchFn&& matchFn, AdvanceFn&& advanceFn) {
    if (!matchFn(TokenKind::LBrace)) return;
    int depth = 1;
    while (depth > 0 && !atEnd()) {
        if (matchFn(TokenKind::LBrace)) ++depth;
        else if (matchFn(TokenKind::RBrace)) --depth;
        else advanceFn();
    }
}

}  // namespace

bool Parser::isTypeAliasDeclStart() const {
    if (!(check(TokenKind::Identifier) && peek().text == "type" && !peek(1).newlineBefore)) {
        return false;
    }
    if (!isIdentifierName(peek(1).kind)) return false;
    const TokenKind k2 = peek(2).kind;
    return k2 == TokenKind::Assign || k2 == TokenKind::Less;
}

bool Parser::isInterfaceDeclStart() const {
    if (!(check(TokenKind::Identifier) && peek().text == "interface" && !peek(1).newlineBefore)) {
        return false;
    }
    if (!isIdentifierName(peek(1).kind)) return false;
    const TokenKind k2 = peek(2).kind;
    return k2 == TokenKind::LBrace || k2 == TokenKind::Less || k2 == TokenKind::KwExtends ||
           (k2 == TokenKind::Identifier && peek(2).text == "implements");
}

bool Parser::isEnumDeclStart() const {
    if (check(TokenKind::Identifier) && peek().text == "enum" && !peek(1).newlineBefore) {
        return isIdentifierName(peek(1).kind) && peek(2).kind == TokenKind::LBrace;
    }
    if (check(TokenKind::KwConst) && peek(1).kind == TokenKind::Identifier &&
        peek(1).text == "enum" && !peek(1).newlineBefore && !peek(2).newlineBefore) {
        return isIdentifierName(peek(2).kind) && peek(3).kind == TokenKind::LBrace;
    }
    return false;
}

bool Parser::isNamespaceDeclStart() const {
    if (!(check(TokenKind::Identifier) &&
          (peek().text == "namespace" || peek().text == "module") && !peek(1).newlineBefore)) {
        return false;
    }
    const TokenKind k1 = peek(1).kind;
    const TokenKind k2 = peek(2).kind;
    if (k1 == TokenKind::StringLiteral && k2 == TokenKind::LBrace) return true;
    return isIdentifierName(k1) && (k2 == TokenKind::LBrace || k2 == TokenKind::Dot);
}

bool Parser::isDeclareStmtStart() const {
    if (!(check(TokenKind::Identifier) && peek().text == "declare" && !peek(1).newlineBefore)) {
        return false;
    }
    const TokenKind k1 = peek(1).kind;
    const std::string_view t1 = peek(1).text;
    if (k1 == TokenKind::KwVar || k1 == TokenKind::KwLet || k1 == TokenKind::KwConst ||
        k1 == TokenKind::KwFunction || k1 == TokenKind::KwClass) {
        return true;
    }
    if (k1 == TokenKind::Identifier &&
        (t1 == "type" || t1 == "interface" || t1 == "enum" || t1 == "namespace" ||
         t1 == "module" || t1 == "global" || t1 == "abstract" || t1 == "async")) {
        return true;
    }
    return false;
}

bool Parser::isAbstractClassDeclStart() const {
    return check(TokenKind::Identifier) && peek().text == "abstract" && !peek(1).newlineBefore &&
           peek(1).kind == TokenKind::KwClass;
}

bool Parser::parseTypeAliasDecl() {
    advance();  // 'type'
    const Token* nameTok = expectPropertyName("type alias name");
    if (!nameTok) return false;
    const std::string aliasName(nameTok->text);
    recordDeclaredType(aliasName);

    if (check(TokenKind::Less)) {
        if (!parseTypeParams()) return false;
    }
    if (!expect(TokenKind::Assign, "'=' in type alias declaration")) return false;
    const std::string rhs = parseTypeAnnotation();
    if (rhs.empty()) return false;
    recordTypeAlias(aliasName, rhs);
    if (!consumeSemicolon("type alias declaration")) return false;
    return true;
}

bool Parser::parseInterfaceDecl() {
    advance();  // 'interface'
    if (isIdentifierName(peek().kind)) {
        const std::string name(advance().text);
        recordDeclaredType(name);
    }
    if (check(TokenKind::Less)) {
        if (!parseTypeParams()) return false;
    }
    while (match(TokenKind::KwExtends) ||
           (check(TokenKind::Identifier) && peek().text == "implements" && (advance(), true))) {
        do {
            if (parseTypeAnnotation().empty()) return false;
        } while (match(TokenKind::Comma));
    }
    if (!check(TokenKind::LBrace)) {
        error("expected '{' to start interface body");
        return false;
    }
    skipBalancedBraces(
        [this]() { return check(TokenKind::EndOfFile); },
        [this](TokenKind k) { return match(k); }, [this]() { advance(); });
    match(TokenKind::Semicolon);
    return true;
}

bool Parser::parseEnumDecl(std::vector<ast::StmtPtr>& out, std::string* outName) {
    const Span startSpan = peek().span;
    match(TokenKind::KwConst);  // optional 'const'
    advance();                  // 'enum'
    const Token* nameTok = expectPropertyName("enum name");
    if (!nameTok) return false;
    const std::string enumName(nameTok->text);
    recordDeclaredType(enumName);
    if (outName) *outName = enumName;

    if (!expect(TokenKind::LBrace, "'{' after enum name")) return false;

    auto obj = std::make_unique<ObjectLit>();
    obj->span.begin = peek().span.begin;

    std::unordered_map<std::string, double> numValues;
    std::unordered_map<std::string, std::string> strValues;
    double nextNumeric = 0.0;
    bool hasNextNumeric = true;

    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
        std::string memberName;
        Span memberSpan = peek().span;
        if (check(TokenKind::StringLiteral)) {
            const Token& sTok = advance();
            memberName = decodeStringLiteral(sTok.text.substr(1, sTok.text.size() - 2), sTok.span);
        } else if (check(TokenKind::LBracket)) {
            advance();  // '['
            ExprPtr keyExpr = parseAssign();
            if (!keyExpr || !expect(TokenKind::RBracket, "']' in computed enum member")) {
                return false;
            }
            if (auto* s = dynamic_cast<const StringLit*>(keyExpr.get())) {
                memberName = s->value;
            } else {
                memberName = "computed";
            }
        } else {
            const Token* mTok = expectPropertyName("enum member name");
            if (!mTok) return false;
            memberName = std::string(mTok->text);
        }

        auto addReverseMapping = [&](double numVal) {
            ObjectProp revProp;
            revProp.key = formatEnumNum(numVal);
            auto revVal = std::make_unique<StringLit>();
            revVal->span = memberSpan;
            revVal->value = memberName;
            revProp.value = std::move(revVal);
            obj->props.push_back(std::move(revProp));
        };

        if (match(TokenKind::Assign)) {
            ExprPtr initExpr = parseAssign();
            if (!initExpr) return false;
            EnumVal ev = evalEnumExpr(initExpr.get(), enumName, numValues, strValues);
            if (std::holds_alternative<double>(ev)) {
                const double val = std::get<double>(ev);
                numValues[memberName] = val;
                nextNumeric = val + 1.0;
                hasNextNumeric = true;

                ObjectProp prop;
                prop.key = memberName;
                auto numLit = std::make_unique<NumberLit>();
                numLit->span = initExpr->span;
                numLit->value = val;
                prop.value = std::move(numLit);
                obj->props.push_back(std::move(prop));
                addReverseMapping(val);
            } else if (std::holds_alternative<std::string>(ev)) {
                const std::string& sval = std::get<std::string>(ev);
                strValues[memberName] = sval;
                hasNextNumeric = false;

                ObjectProp prop;
                prop.key = memberName;
                auto strLit = std::make_unique<StringLit>();
                strLit->span = initExpr->span;
                strLit->value = sval;
                prop.value = std::move(strLit);
                obj->props.push_back(std::move(prop));
            } else {
                hasNextNumeric = false;
                ObjectProp prop;
                prop.key = memberName;
                prop.value = std::move(initExpr);
                obj->props.push_back(std::move(prop));
            }
        } else {
            const double val = hasNextNumeric ? nextNumeric : 0.0;
            numValues[memberName] = val;
            nextNumeric = val + 1.0;
            hasNextNumeric = true;

            ObjectProp prop;
            prop.key = memberName;
            auto numLit = std::make_unique<NumberLit>();
            numLit->span = memberSpan;
            numLit->value = val;
            prop.value = std::move(numLit);
            obj->props.push_back(std::move(prop));
            addReverseMapping(val);
        }

        if (!match(TokenKind::Comma) && !match(TokenKind::Semicolon)) break;
    }

    if (!expect(TokenKind::RBrace, "'}' after enum body")) return false;
    obj->span.end = previous().span.end;

    auto decl = std::make_unique<VarDecl>();
    decl->span = {startSpan.begin, previous().span.end};
    decl->isConst = true;
    decl->name = enumName;
    decl->init = std::move(obj);
    out.push_back(std::move(decl));
    return true;
}

bool Parser::parseNamespaceDecl(std::vector<ast::StmtPtr>& out, std::string* outName) {
    const Span startSpan = peek().span;
    advance();  // 'namespace' or 'module'

    if (check(TokenKind::StringLiteral)) {
        advance();
        if (check(TokenKind::LBrace)) {
            skipBalancedBraces(
                [this]() { return check(TokenKind::EndOfFile); },
                [this](TokenKind k) { return match(k); }, [this]() { advance(); });
        } else {
            consumeSemicolon("ambient module declaration");
        }
        return true;
    }

    std::vector<std::string> segments;
    const Token* firstTok = expectPropertyName("namespace name");
    if (!firstTok) return false;
    segments.emplace_back(firstTok->text);
    recordDeclaredType(segments.front());
    while (match(TokenKind::Dot)) {
        const Token* segTok = expectPropertyName("namespace segment name");
        if (!segTok) return false;
        segments.emplace_back(segTok->text);
    }

    if (!expect(TokenKind::LBrace, "'{' after namespace name")) return false;

    const bool savedTopLevel = atModuleTopLevel_;
    atModuleTopLevel_ = false;

    std::vector<StmtPtr> bodyStmts;
    std::vector<std::string> exportedNames;

    auto recordVarExports = [&](size_t fromIdx) {
        for (size_t i = fromIdx; i < bodyStmts.size(); ++i) {
            if (auto* vd = dynamic_cast<const VarDecl*>(bodyStmts[i].get())) {
                if (!vd->name.empty()) {
                    exportedNames.push_back(vd->name);
                } else if (vd->pattern) {
                    for (const auto& nm : patternBoundNames(*vd->pattern)) {
                        exportedNames.push_back(nm);
                    }
                }
            }
        }
    };

    bool ok = true;
    while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
        if (match(TokenKind::Semicolon)) continue;
        if (match(TokenKind::KwExport)) {
            if (isTypeAliasDeclStart()) {
                if (!parseTypeAliasDecl()) { ok = false; break; }
            } else if (isInterfaceDeclStart()) {
                if (!parseInterfaceDecl()) { ok = false; break; }
            } else if (isDeclareStmtStart()) {
                if (!parseDeclareStmt()) { ok = false; break; }
            } else if (isEnumDeclStart()) {
                std::string ename;
                if (!parseEnumDecl(bodyStmts, &ename)) { ok = false; break; }
                if (!ename.empty()) exportedNames.push_back(ename);
            } else if (isNamespaceDeclStart()) {
                std::string nname;
                if (!parseNamespaceDecl(bodyStmts, &nname)) { ok = false; break; }
                if (!nname.empty()) exportedNames.push_back(nname);
            } else if (check(TokenKind::KwVar) || check(TokenKind::KwLet) ||
                       check(TokenKind::KwConst)) {
                const size_t before = bodyStmts.size();
                if (!parseVarDecl(bodyStmts, false)) { ok = false; break; }
                recordVarExports(before);
            } else if (check(TokenKind::KwFunction)) {
                lastFunctionWasOverload_ = false;
                auto fn = parseFunctionDecl(false);
                if (!fn && !lastFunctionWasOverload_) { ok = false; break; }
                if (fn) {
                    if (auto* fd = dynamic_cast<const FunctionDecl*>(fn.get())) {
                        exportedNames.push_back(fd->name);
                    }
                    bodyStmts.push_back(std::move(fn));
                }
            } else if (check(TokenKind::Identifier) && peek().text == "async" &&
                       !peek(1).newlineBefore && peek(1).kind == TokenKind::KwFunction) {
                lastFunctionWasOverload_ = false;
                auto fn = parseAsyncFunctionDecl(false);
                if (!fn && !lastFunctionWasOverload_) { ok = false; break; }
                if (fn) {
                    if (auto* fd = dynamic_cast<const FunctionDecl*>(fn.get())) {
                        exportedNames.push_back(fd->name);
                    }
                    bodyStmts.push_back(std::move(fn));
                }
            } else if (check(TokenKind::KwClass) || isAbstractClassDeclStart()) {
                if (isAbstractClassDeclStart()) advance();
                auto cls = parseClass();
                if (!cls) { ok = false; break; }
                if (auto* cd = dynamic_cast<const ClassDecl*>(cls.get())) {
                    exportedNames.push_back(cd->name);
                }
                bodyStmts.push_back(std::move(cls));
            } else {
                error("unsupported export inside namespace");
                ok = false;
                break;
            }
        } else {
            if (!parseStatement(bodyStmts)) {
                ok = false;
                break;
            }
        }
    }

    atModuleTopLevel_ = savedTopLevel;
    if (!ok) return false;
    if (!expect(TokenKind::RBrace, "'}' after namespace body")) return false;
    const Span fullSpan = {startSpan.begin, previous().span.end};

    // Purely type-only namespace: erase completely.
    if (bodyStmts.empty()) {
        return true;
    }

    if (outName) *outName = segments.front();

    // Build IIFE `(() => { const __ns = {}; ...bodyStmts...; __ns.x = x; return __ns; })()`
    auto buildNamespaceIife = [&](std::vector<StmtPtr> stmts,
                                  const std::vector<std::string>& exports) -> ExprPtr {
        const std::string nsVar = "__ns_" + std::to_string(namespaceOrdinal_++);
        auto fn = std::make_unique<FunctionExpr>();
        fn->span = fullSpan;
        fn->isArrow = true;
        fn->kind = FunctionKind::Arrow;
        fn->strict = strict_;

        auto nsDecl = std::make_unique<VarDecl>();
        nsDecl->span = fullSpan;
        nsDecl->isConst = true;
        nsDecl->name = nsVar;
        auto emptyObj = std::make_unique<ObjectLit>();
        emptyObj->span = fullSpan;
        nsDecl->init = std::move(emptyObj);
        fn->body.push_back(std::move(nsDecl));

        for (auto& st : stmts) {
            fn->body.push_back(std::move(st));
        }

        for (const auto& expName : exports) {
            auto lhs = std::make_unique<MemberAccess>();
            lhs->span = fullSpan;
            auto objId = std::make_unique<Ident>();
            objId->span = fullSpan;
            objId->name = nsVar;
            lhs->object = std::move(objId);
            lhs->property = expName;

            auto rhs = std::make_unique<Ident>();
            rhs->span = fullSpan;
            rhs->name = expName;

            auto assign = std::make_unique<Binary>();
            assign->span = fullSpan;
            assign->op = BinaryOp::Assign;
            assign->lhs = std::move(lhs);
            assign->rhs = std::move(rhs);

            auto exprStmt = std::make_unique<ExprStmt>();
            exprStmt->span = fullSpan;
            exprStmt->expr = std::move(assign);
            fn->body.push_back(std::move(exprStmt));
        }

        auto ret = std::make_unique<ReturnStmt>();
        ret->span = fullSpan;
        auto retId = std::make_unique<Ident>();
        retId->span = fullSpan;
        retId->name = nsVar;
        ret->value = std::move(retId);
        fn->body.push_back(std::move(ret));

        auto call = std::make_unique<Call>();
        call->span = fullSpan;
        call->callee = std::move(fn);
        return call;
    };

    ExprPtr innerExpr = buildNamespaceIife(std::move(bodyStmts), exportedNames);
    for (size_t i = segments.size() - 1; i >= 1; --i) {
        auto decl = std::make_unique<VarDecl>();
        decl->span = fullSpan;
        decl->isConst = true;
        decl->name = segments[i];
        decl->init = std::move(innerExpr);
        std::vector<StmtPtr> wrapStmts;
        wrapStmts.push_back(std::move(decl));
        innerExpr = buildNamespaceIife(std::move(wrapStmts), {segments[i]});
    }

    auto topDecl = std::make_unique<VarDecl>();
    topDecl->span = fullSpan;
    topDecl->isConst = true;
    topDecl->name = segments.front();
    topDecl->init = std::move(innerExpr);
    out.push_back(std::move(topDecl));
    return true;
}

bool Parser::parseDeclareStmt() {
    advance();  // 'declare'

    if (isTypeAliasDeclStart()) return parseTypeAliasDecl();
    if (isInterfaceDeclStart()) return parseInterfaceDecl();

    if (check(TokenKind::Identifier) && peek().text == "global" && peek(1).kind == TokenKind::LBrace) {
        advance();  // 'global'
        skipBalancedBraces(
            [this]() { return check(TokenKind::EndOfFile); },
            [this](TokenKind k) { return match(k); }, [this]() { advance(); });
        match(TokenKind::Semicolon);
        return true;
    }

    if (isNamespaceDeclStart()) {
        advance();  // 'namespace' or 'module'
        if (check(TokenKind::StringLiteral)) {
            advance();
        } else {
            const Token* firstTok = expectPropertyName("namespace name");
            if (!firstTok) return false;
            recordDeclaredType(std::string(firstTok->text));
            while (match(TokenKind::Dot)) {
                if (!expectPropertyName("namespace segment name")) return false;
            }
        }
        if (check(TokenKind::LBrace)) {
            skipBalancedBraces(
                [this]() { return check(TokenKind::EndOfFile); },
                [this](TokenKind k) { return match(k); }, [this]() { advance(); });
        }
        match(TokenKind::Semicolon);
        return true;
    }

    if (isEnumDeclStart()) {
        match(TokenKind::KwConst);
        advance();  // 'enum'
        const Token* nameTok = expectPropertyName("enum name");
        if (!nameTok) return false;
        recordDeclaredType(std::string(nameTok->text));
        if (check(TokenKind::LBrace)) {
            skipBalancedBraces(
                [this]() { return check(TokenKind::EndOfFile); },
                [this](TokenKind k) { return match(k); }, [this]() { advance(); });
        }
        match(TokenKind::Semicolon);
        return true;
    }

    if (check(TokenKind::KwClass) || isAbstractClassDeclStart()) {
        if (isAbstractClassDeclStart()) advance();  // 'abstract'
        advance();                                  // 'class'
        if (isIdentifierName(peek().kind)) {
            recordDeclaredType(std::string(advance().text));
        }
        if (check(TokenKind::Less)) {
            if (!parseTypeParams()) return false;
        }
        while (!check(TokenKind::LBrace) && !check(TokenKind::EndOfFile)) {
            advance();
        }
        if (check(TokenKind::LBrace)) {
            skipBalancedBraces(
                [this]() { return check(TokenKind::EndOfFile); },
                [this](TokenKind k) { return match(k); }, [this]() { advance(); });
        }
        match(TokenKind::Semicolon);
        return true;
    }

    if (check(TokenKind::KwFunction) ||
        (check(TokenKind::Identifier) && peek().text == "async" && peek(1).kind == TokenKind::KwFunction)) {
        if (check(TokenKind::Identifier)) advance();  // 'async'
        advance();                                    // 'function'
        match(TokenKind::Star);
        if (isIdentifierName(peek().kind)) advance();
        if (check(TokenKind::Less)) {
            if (!parseTypeParams()) return false;
        }
        if (!expect(TokenKind::LParen, "'(' in declare function")) return false;
        int depth = 1;
        while (depth > 0 && !check(TokenKind::EndOfFile)) {
            if (match(TokenKind::LParen)) ++depth;
            else if (match(TokenKind::RParen)) --depth;
            else advance();
        }
        if (match(TokenKind::Colon)) {
            if (parseTypeAnnotation().empty()) return false;
        }
        if (check(TokenKind::LBrace)) {
            skipBalancedBraces(
                [this]() { return check(TokenKind::EndOfFile); },
                [this](TokenKind k) { return match(k); }, [this]() { advance(); });
        } else {
            consumeSemicolon("declare function");
        }
        return true;
    }

    if (check(TokenKind::KwVar) || check(TokenKind::KwLet) || check(TokenKind::KwConst)) {
        advance();  // var/let/const
        do {
            if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) {
                if (!parsePattern()) return false;
            } else if (isIdentifierName(peek().kind)) {
                advance();
            } else {
                error("expected binding name in declare variable statement");
                return false;
            }
            match(TokenKind::Bang);
            if (match(TokenKind::Colon)) {
                if (parseTypeAnnotation().empty()) return false;
            }
            if (match(TokenKind::Assign)) {
                if (!parseAssign()) return false;
            }
        } while (match(TokenKind::Comma));
        return consumeSemicolon("declare variable statement");
    }

    error("unsupported 'declare' declaration");
    return false;
}

}  // namespace bronze
