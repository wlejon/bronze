// `import` and `export` (ECMA-262 16.2.2 and 16.2.3). Both productions are
// module-level only and both reduce to a list of (local name, other name) pairs
// plus, sometimes, a specifier — which is all `src/modules` needs to build the
// graph and resolve the bindings.
//
// The declaration an `export` fronts is left in the statement list as an
// ordinary declaration, with the export recorded beside it. Nothing after the
// parser then has to know that a `class` or a `let` can be wrapped.

#include "parse/parser.h"

namespace bronze {

using namespace ast;

namespace {
// `from` and `as` are not reserved words — they are ordinary identifiers
// everywhere except in these two productions, which is why they are matched
// on text rather than on a token kind.
bool isContextual(const Token& t, std::string_view word) {
    return t.kind == TokenKind::Identifier && t.text == word;
}
}  // namespace

void Parser::declaredNamesOf(const Stmt& stmt, std::vector<std::string>& out) {
    if (const auto* v = dynamic_cast<const VarDecl*>(&stmt)) {
        if (v->pattern) {
            for (auto& n : patternBoundNames(*v->pattern)) out.push_back(std::move(n));
        } else {
            out.push_back(v->name);
        }
    } else if (const auto* f = dynamic_cast<const FunctionDecl*>(&stmt)) {
        out.push_back(f->name);
    } else if (const auto* c = dynamic_cast<const ClassDecl*>(&stmt)) {
        out.push_back(c->name);
    }
}

bool Parser::parseFromClause(std::string& outSpecifier, Span& outSpan) {
    if (!isContextual(peek(), "from")) {
        error("expected 'from' after an import or export clause");
        return false;
    }
    advance();
    if (!check(TokenKind::StringLiteral)) {
        error("expected a module specifier string after 'from'");
        return false;
    }
    const Token& tok = advance();
    outSpan = tok.span;
    outSpecifier = decodeStringLiteral(tok.text.substr(1, tok.text.size() - 2), tok.span);
    return true;
}

bool Parser::parseImportDecl(std::vector<StmtPtr>& out) {
    const Token& kw = advance();  // 'import'

    // Two things that begin with `import` and are not declarations. Both are
    // real JavaScript bronze has not built, so both are named: falling
    // through to the clause grammar below would report a missing '{'.
    if (check(TokenKind::LParen)) {
        error("unsupported construct: dynamic import() (bronze has no promises)");
        return false;
    }
    if (check(TokenKind::Dot)) {
        error("unsupported construct: import.meta");
        return false;
    }

    auto decl = std::make_unique<ImportDecl>();
    decl->span.begin = kw.span.begin;

    // `import "./x.js";` — no clause at all. The module is evaluated for its
    // side effects and binds nothing here.
    if (check(TokenKind::StringLiteral)) {
        const Token& tok = advance();
        decl->specifierSpan = tok.span;
        decl->specifier = decodeStringLiteral(tok.text.substr(1, tok.text.size() - 2), tok.span);
        if (!consumeSemicolon("an import declaration")) return false;
        decl->span.end = peek().span.begin;
        out.push_back(std::move(decl));
        return true;
    }

    bool expectFrom = false;

    // ImportedDefaultBinding, which may be followed by `, { ... }` or
    // `, * as ns`.
    if (check(TokenKind::Identifier)) {
        const Token& nameTok = advance();
        if (check(TokenKind::Assign)) {
            error("unsupported construct: 'import x = require(...)' (TypeScript import "
                  "assignment); bronze reads ES module syntax only");
            return false;
        }
        ImportSpecifier spec;
        spec.isDefault = true;
        spec.imported = "default";
        spec.local = std::string(nameTok.text);
        spec.span = nameTok.span;
        decl->specifiers.push_back(std::move(spec));
        expectFrom = true;
        if (match(TokenKind::Comma)) expectFrom = false;
    }

    if (check(TokenKind::Star)) {
        const Token& star = advance();
        if (!isContextual(peek(), "as")) {
            error("expected 'as' after '*' in an import clause");
            return false;
        }
        advance();
        const Token* nameTok = expect(TokenKind::Identifier, "a namespace binding name after 'as'");
        if (!nameTok) return false;
        ImportSpecifier spec;
        spec.isNamespace = true;
        spec.local = std::string(nameTok->text);
        spec.span = star.span;
        decl->specifiers.push_back(std::move(spec));
    } else if (check(TokenKind::LBrace)) {
        advance();
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
            // The imported name is an IdentifierName — `import { default as d }`
            // and `import { delete as del }` are both legal — while the local
            // binding is an Identifier, because it is a binding.
            const Token* importedTok = expectPropertyName("an imported name");
            if (!importedTok) return false;
            ImportSpecifier spec;
            spec.imported = std::string(importedTok->text);
            spec.local = spec.imported;
            spec.span = importedTok->span;
            if (isContextual(peek(), "as")) {
                advance();
                const Token* localTok = expect(TokenKind::Identifier, "a binding name after 'as'");
                if (!localTok) return false;
                spec.local = std::string(localTok->text);
            } else if (importedTok->kind != TokenKind::Identifier) {
                // `import { default }` binds a name that cannot be written.
                error("a reserved word imported by name needs 'as': write "
                      "'import { default as d }'");
                return false;
            }
            decl->specifiers.push_back(std::move(spec));
            if (!match(TokenKind::Comma)) break;
        }
        if (!expect(TokenKind::RBrace, "'}' to close an import clause")) return false;
    } else if (!expectFrom) {
        error("expected a binding, '{', '*' or a module specifier after 'import'");
        return false;
    }

    if (!parseFromClause(decl->specifier, decl->specifierSpan)) return false;
    if (!consumeSemicolon("an import declaration")) return false;
    decl->span.end = peek().span.begin;
    out.push_back(std::move(decl));
    return true;
}

bool Parser::parseExportDecl(std::vector<StmtPtr>& out) {
    const Token& kw = advance();  // 'export'
    auto names = std::make_unique<ExportNamesDecl>();
    names->span.begin = kw.span.begin;

    if (check(TokenKind::Assign)) {
        error("unsupported construct: 'export =' (TypeScript export assignment); bronze reads "
              "ES module syntax only");
        return false;
    }

    // `export * from './x'` and `export * as ns from './x'`. Which names the
    // first form contributes is a question about the OTHER module, so the
    // parser records the star and the linker expands it.
    if (check(TokenKind::Star)) {
        advance();
        names->isStar = true;
        if (isContextual(peek(), "as")) {
            advance();
            const Token* aliasTok = expectPropertyName("an export name after 'as'");
            if (!aliasTok) return false;
            names->starAlias = std::string(aliasTok->text);
        }
        if (!parseFromClause(names->fromSpecifier, names->fromSpan)) return false;
        names->hasFrom = true;
        if (!consumeSemicolon("an export declaration")) return false;
        names->span.end = peek().span.begin;
        out.push_back(std::move(names));
        return true;
    }

    // `export { a, b as c }` and `export { a } from './x'`.
    if (check(TokenKind::LBrace)) {
        advance();
        while (!check(TokenKind::RBrace) && !check(TokenKind::EndOfFile) && !diags_.hasErrors()) {
            const Token* localTok = expectPropertyName("an exported name");
            if (!localTok) return false;
            ExportSpecifier spec;
            spec.local = std::string(localTok->text);
            spec.exported = spec.local;
            spec.span = localTok->span;
            if (isContextual(peek(), "as")) {
                advance();
                const Token* asTok = expectPropertyName("an export name after 'as'");
                if (!asTok) return false;
                spec.exported = std::string(asTok->text);
            }
            names->specifiers.push_back(std::move(spec));
            if (!match(TokenKind::Comma)) break;
        }
        if (!expect(TokenKind::RBrace, "'}' to close an export clause")) return false;
        if (isContextual(peek(), "from")) {
            if (!parseFromClause(names->fromSpecifier, names->fromSpan)) return false;
            names->hasFrom = true;
        }
        if (!consumeSemicolon("an export declaration")) return false;
        names->span.end = peek().span.begin;
        out.push_back(std::move(names));
        return true;
    }

    // `export default ...`. The three right-hand sides are a hoisted function
    // declaration, a class declaration, and an AssignmentExpression — and the
    // first two may be anonymous, which is the one place in the grammar a
    // declaration has no name. They get one anyway ("default", a reserved
    // word, so it can never collide with something the source wrote) because
    // everything downstream identifies a function by its name.
    if (check(TokenKind::KwDefault)) {
        const Token& defTok = advance();
        ExportSpecifier spec;
        spec.exported = "default";
        spec.span = defTok.span;

        if (check(TokenKind::KwFunction)) {
            auto fn = parseFunctionDecl(/*isExported=*/true, "default");
            if (!fn) return false;
            spec.local = static_cast<const FunctionDecl*>(fn.get())->name;
            out.push_back(std::move(fn));
        } else if (check(TokenKind::KwClass)) {
            auto cls = parseClass("default");
            if (!cls) return false;
            spec.local = static_cast<const ClassDecl*>(cls.get())->name;
            out.push_back(std::move(cls));
        } else {
            // `export default <expr>;` binds an anonymous constant that only
            // an importer can name. A `const` and not a `let`: 16.2.3.7 makes
            // it immutable, and nothing in this file can refer to it at all.
            auto expr = parseAssign();
            if (!expr) return false;
            auto decl = std::make_unique<VarDecl>();
            decl->span = expr->span;
            decl->isConst = true;
            decl->name = "default";
            decl->init = std::move(expr);
            spec.local = "default";
            out.push_back(std::move(decl));
            if (!consumeSemicolon("an export default declaration")) return false;
        }
        names->specifiers.push_back(std::move(spec));
        names->span.end = peek().span.begin;
        out.push_back(std::move(names));
        return true;
    }

    // `export <declaration>` — the declaration is an ordinary statement and
    // is parsed by the ordinary production. Every name it binds is exported
    // under its own spelling, patterns included: `export const { a, b } = o`
    // exports two.
    const size_t firstDecl = out.size();
    if (check(TokenKind::KwFunction)) {
        auto fn = parseFunctionDecl(/*isExported=*/true);
        if (!fn) return false;
        out.push_back(std::move(fn));
    } else if (check(TokenKind::KwClass)) {
        auto cls = parseClass();
        if (!cls) return false;
        out.push_back(std::move(cls));
    } else if (check(TokenKind::KwConst) || check(TokenKind::KwLet) || check(TokenKind::KwVar)) {
        if (!parseVarDecl(out)) return false;
    } else {
        error("expected a declaration, '{', '*' or 'default' after 'export'");
        return false;
    }

    std::vector<std::string> declared;
    for (size_t i = firstDecl; i < out.size(); ++i) declaredNamesOf(*out[i], declared);
    for (const auto& name : declared) {
        ExportSpecifier spec;
        spec.local = name;
        spec.exported = name;
        spec.span = out[firstDecl]->span;
        names->specifiers.push_back(std::move(spec));
    }
    names->span.end = peek().span.begin;
    out.push_back(std::move(names));
    return true;
}

}  // namespace bronze
