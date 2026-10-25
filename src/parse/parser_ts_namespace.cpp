// TypeScript `namespace` (and its older spelling `module`), lowered to the
// JavaScript TypeScript emits for it.
//
// A namespace is an object, and its body a function run once with that object
// as its parameter:
//
//     var M;
//     (function (M) {
//         M.count = 0;                       // export let count = 0
//         function inc() { M.count++; }      // export function inc() { count++ }
//         M.inc = inc;
//     })(M || (M = {}));
//
// The rules that make that the right shape, each observable:
//
// - An exported VARIABLE has no local binding at all. It is the object's
//   property, and every mention of it in the body is rewritten to read and
//   write `M.count`, so the export is live: `M.inc(); M.count` sees the
//   increment, and an assignment from outside is seen inside.
// - An exported function or class keeps its local binding, and the property
//   is assigned from it right after the declaration; the body goes on
//   reaching it by its local name. An exported enum or namespace inside one
//   is, like a variable, only the property (`M.E`, `M.Inner`).
// - `M || (M = {})` is what merges declarations: a second `namespace M {}`,
//   or a namespace after a class, a function or an enum of the same name, runs
//   its body on the object that is already there — so a class gets its static
//   helpers this way. Only the first declaration in a scope emits the `var`.
// - A dotted `namespace A.B.C {}` is `A` holding an exported `B` holding an
//   exported `C`.
// - A namespace with nothing in it but types emits nothing.
//
// One thing TypeScript's checker does that a per-file transform cannot, and
// node's transform (the oracle) does not do either: a body naming an export
// of ANOTHER block of the same namespace by its bare name. Only the block's
// own exports are rewritten, as node's are.

#include "parse/parser.h"
#include "parse/parser_ts_rewrite.h"

#include "ast/queries.h"

namespace bronze {

using namespace ast;
using namespace ts_rewrite;

namespace {

// The `ns.name || (ns.name = {})` / `name || (name = {})` argument that makes
// the object the first time and reuses it every time after.
ExprPtr objectOrNew(const std::string* exportInto, const std::string& name, Span span) {
    auto ref = [&]() {
        return exportInto ? makeMember(makeIdent(*exportInto, span), name, span)
                          : makeIdent(name, span);
    };
    return makeBinary(BinaryOp::LogicalOr, ref(),
                      makeBinary(BinaryOp::Assign, ref(), std::make_unique<ObjectLit>(), span),
                      span);
}

// The declaration of `name` already in `scope`, if any: what a namespace
// merges into instead of declaring a binding of its own.
Stmt* priorDeclaration(std::vector<StmtPtr>& scope, const std::string& name) {
    for (auto& s : scope) {
        if (auto* vd = dynamic_cast<VarDecl*>(s.get()); vd && vd->name == name) return vd;
        if (auto* cd = dynamic_cast<ClassDecl*>(s.get()); cd && cd->name == name) return cd;
        if (auto* fd = dynamic_cast<FunctionDecl*>(s.get()); fd && fd->name == name) return fd;
    }
    return nullptr;
}

// A declaration's binding pattern turned into the assignment pattern that
// writes the same values to the namespace's properties:
// `export const { a, b: [c] } = o` is `({ a: M.a, b: [M.c] } = o)`.
void patternToMembers(BindingPattern& p, const std::string& param) {
    for (auto& elem : p.elements) {
        if (elem.pattern) patternToMembers(*elem.pattern, param);
        if (!elem.name.empty()) {
            elem.target = makeMember(makeIdent(param, elem.span), elem.name, elem.span);
            elem.name.clear();
        }
    }
}

}  // namespace

bool Parser::parseNamespaceDecl(std::vector<StmtPtr>& out, std::string* outName,
                                const std::string* exportInto) {
    const Span startSpan = peek().span;
    advance();  // 'namespace' or 'module'

    if (check(TokenKind::StringLiteral)) {
        // `module "name" { … }` declares an ambient module: types only.
        advance();
        if (match(TokenKind::LBrace)) {
            int depth = 1;
            while (depth > 0 && !check(TokenKind::EndOfFile)) {
                if (match(TokenKind::LBrace)) ++depth;
                else if (match(TokenKind::RBrace)) --depth;
                else advance();
            }
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

    bool emitted = false;
    if (!parseNamespaceLevel(segments, 0, out, exportInto, startSpan, emitted)) return false;
    if (emitted && outName) *outName = segments.front();
    return true;
}

bool Parser::parseNamespaceLevel(const std::vector<std::string>& segments, size_t index,
                                 std::vector<StmtPtr>& out, const std::string* exportInto,
                                 Span startSpan, bool& outEmitted) {
    outEmitted = false;
    const std::string& name = segments[index];
    // The body's parameter is the namespace's own name, as TypeScript spells
    // it, which is also why the body may not declare that name for itself.
    const std::string& param = name;
    std::vector<StmtPtr> body;
    std::set<std::string> qualified;
    bool instantiated = false;
    if (index + 1 < segments.size()) {
        // `A.B.C`: this level's whole body is the next level, exported.
        if (!parseNamespaceLevel(segments, index + 1, body, &param, startSpan, instantiated)) {
            return false;
        }
    } else if (!parseNamespaceBody(body, param, qualified, instantiated)) {
        return false;
    }
    const Span fullSpan = {startSpan.begin, previous().span.end};
    if (!instantiated) return true;

    // A name the body also declares — an exported function with an exported
    // namespace of the same name merged into it — is reached through its
    // local binding, which is the same object; only the names with no local
    // binding are read through the namespace.
    std::set<std::string> locals;
    for (const auto& n : getScopeDeclarations(body)) locals.insert(n);
    for (const auto& n : getHoistedVarDeclarations(body)) locals.insert(n);
    for (const auto& n : locals) qualified.erase(n);
    if (locals.count(param) || qualified.count(param)) {
        diags_.error(fullSpan, "unsupported construct: namespace '" + name +
                                   "' declares or exports its own name inside its body");
        return false;
    }
    if (!qualified.empty()) {
        QualifyNames qualify(diags_, qualified, param);
        if (!qualify.walkStmts(body)) return false;
    }

    ExprPtr arg;
    if (exportInto) {
        arg = objectOrNew(exportInto, name, fullSpan);
    } else if (priorDeclaration(out, name)) {
        // Merging into what an earlier declaration in this scope made. That
        // value exists by the time this statement runs — a function is
        // hoisted, a class, an enum or an earlier namespace was initialized by
        // a statement above this one — so TypeScript's `M || (M = {})` would
        // only ever read `M`, and the binding may be one nothing can assign
        // (a function declaration's, an enum's `const`).
        arg = makeIdent(name, fullSpan);
    } else {
        auto decl = std::make_unique<VarDecl>();
        decl->span = fullSpan;
        decl->isVar = true;
        decl->name = name;
        out.push_back(std::move(decl));
        arg = objectOrNew(nullptr, name, fullSpan);
    }
    out.push_back(
        makeExprStmt(makeIife(param, std::move(body), std::move(arg), strict_, fullSpan), fullSpan));
    outEmitted = true;
    return true;
}

bool Parser::parseNamespaceBody(std::vector<StmtPtr>& body, const std::string& param,
                                std::set<std::string>& qualified, bool& outInstantiated) {
    const bool savedTopLevel = atModuleTopLevel_;
    atModuleTopLevel_ = false;
    bool sawDeclare = false;

    // `ns.name = name;`, right after the declaration it exports.
    auto exportLocal = [&](const std::string& name, Span span) {
        body.push_back(makeExprStmt(
            makeBinary(BinaryOp::Assign, makeMember(makeIdent(param, span), name, span),
                       makeIdent(name, span), span),
            span));
    };

    bool ok = true;
    while (ok && !check(TokenKind::RBrace) && !check(TokenKind::EndOfFile)) {
        if (match(TokenKind::Semicolon)) continue;
        if (!match(TokenKind::KwExport)) {
            atNamespaceTopLevel_ = true;
            ok = parseStatement(body);
            atNamespaceTopLevel_ = false;
            continue;
        }
        const Span exportSpan = previous().span;
        if (isTypeAliasDeclStart()) {
            ok = parseTypeAliasDecl();
        } else if (isInterfaceDeclStart()) {
            ok = parseInterfaceDecl();
        } else if (isDeclareStmtStart()) {
            // node's transform counts an ambient member as making the
            // namespace exist, though the member itself emits nothing.
            sawDeclare = true;
            ok = parseDeclareStmt();
        } else if (isEnumDeclStart()) {
            std::string ename;
            ok = parseEnumDecl(body, &ename, &param);
            if (ok) qualified.insert(ename);
        } else if (isNamespaceDeclStart()) {
            std::string nname;
            ok = parseNamespaceDecl(body, &nname, &param);
            if (ok && !nname.empty()) qualified.insert(nname);
        } else if (check(TokenKind::KwVar) || check(TokenKind::KwLet) ||
                   check(TokenKind::KwConst)) {
            // An exported variable is the object's property and nothing else:
            // its initializer is assigned to the property where the
            // declaration stood, and a declarator without one emits nothing.
            std::vector<StmtPtr> decls;
            ok = parseVarDecl(decls);
            for (auto& d : decls) {
                auto* vd = dynamic_cast<VarDecl*>(d.get());
                if (!vd) continue;
                const Span span = vd->span;
                if (vd->pattern) {
                    for (const auto& n : patternBoundNames(*vd->pattern)) qualified.insert(n);
                    if (!vd->init) continue;
                    patternToMembers(*vd->pattern, param);
                    auto da = std::make_unique<DestructuringAssign>();
                    da->span = span;
                    da->pattern = std::move(vd->pattern);
                    da->value = std::move(vd->init);
                    body.push_back(makeExprStmt(std::move(da), span));
                } else {
                    qualified.insert(vd->name);
                    if (!vd->init) continue;
                    body.push_back(makeExprStmt(
                        makeBinary(BinaryOp::Assign,
                                   makeMember(makeIdent(param, span), vd->name, span),
                                   std::move(vd->init), span),
                        span));
                }
            }
            if (ok) sawDeclare = true;  // `export let x;` alone still makes the object
        } else if (check(TokenKind::KwFunction) || asyncModifiesFunction()) {
            lastFunctionWasOverload_ = false;
            auto fn = check(TokenKind::KwFunction) ? parseFunctionDecl(false)
                                                   : parseAsyncFunctionDecl(false);
            if (!fn) {
                ok = lastFunctionWasOverload_;
                continue;
            }
            const std::string fname = static_cast<const FunctionDecl*>(fn.get())->name;
            const Span span = fn->span;
            body.push_back(std::move(fn));
            exportLocal(fname, span);
        } else if (check(TokenKind::KwClass) || isAbstractClassDeclStart()) {
            if (isAbstractClassDeclStart()) advance();  // 'abstract'
            auto cls = parseClass();
            if (!cls) {
                ok = false;
                continue;
            }
            const std::string cname = static_cast<const ClassDecl*>(cls.get())->name;
            const Span span = cls->span;
            body.push_back(std::move(cls));
            exportLocal(cname, span);
        } else {
            diags_.error(exportSpan, "unsupported export inside a namespace: only declarations "
                                     "can be exported from one");
            ok = false;
        }
    }

    atModuleTopLevel_ = savedTopLevel;
    if (!ok) return false;
    if (!expect(TokenKind::RBrace, "'}' after namespace body")) return false;
    outInstantiated = !body.empty() || sawDeclare;
    return true;
}

}  // namespace bronze
