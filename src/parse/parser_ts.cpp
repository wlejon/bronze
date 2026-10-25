// TypeScript statement and declaration constructs: where each one starts,
// `type`, `interface`, `declare`, and `abstract`. `enum` lives in
// parser_ts_enum.cpp and `namespace` in parser_ts_namespace.cpp, since each
// lowers to JavaScript of its own rather than being erased.

#include <string>

#include "parse/parser.h"

namespace bronze {

using namespace ast;

namespace {

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

// The declaration starts below are TypeScript's, and every one of them is
// spelled with a word JavaScript reads as an ordinary identifier (`type = 1`,
// `declare(x)`) or, for `enum`, as a reserved word no JavaScript program may
// write. So outside a TypeScript file none of them is a declaration at all.
// Inside one, TypeScript's own rule decides the contextual words: the word is
// a keyword only when what follows it is on the same line, which is what
// keeps `type\nFoo = 1` the two expression statements ASI makes of it.
bool Parser::isTypeAliasDeclStart() const {
    if (!typescript_) return false;
    if (!(check(TokenKind::Identifier) && peek().text == "type" && !peek(1).newlineBefore)) {
        return false;
    }
    if (!isIdentifierName(peek(1).kind)) return false;
    const TokenKind k2 = peek(2).kind;
    return k2 == TokenKind::Assign || k2 == TokenKind::Less;
}

bool Parser::isInterfaceDeclStart() const {
    if (!typescript_) return false;
    if (!(check(TokenKind::Identifier) && peek().text == "interface" && !peek(1).newlineBefore)) {
        return false;
    }
    if (!isIdentifierName(peek(1).kind)) return false;
    const TokenKind k2 = peek(2).kind;
    return k2 == TokenKind::LBrace || k2 == TokenKind::Less || k2 == TokenKind::KwExtends ||
           (k2 == TokenKind::Identifier && peek(2).text == "implements");
}

// `enum` is a reserved word in both grammars (ECMA-262 12.7.2), so unlike the
// contextual words it starts a declaration whatever line its name is on.
bool Parser::isEnumDeclStart() const {
    if (!typescript_) return false;
    if (check(TokenKind::Identifier) && peek().text == "enum") {
        return isIdentifierName(peek(1).kind) && peek(2).kind == TokenKind::LBrace;
    }
    if (check(TokenKind::KwConst) && peek(1).kind == TokenKind::Identifier &&
        peek(1).text == "enum") {
        return isIdentifierName(peek(2).kind) && peek(3).kind == TokenKind::LBrace;
    }
    return false;
}

bool Parser::isNamespaceDeclStart() const {
    if (!typescript_) return false;
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
    if (!typescript_) return false;
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
    return typescript_ && check(TokenKind::Identifier) && peek().text == "abstract" &&
           !peek(1).newlineBefore && peek(1).kind == TokenKind::KwClass;
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

    // `declare enum` / `declare const enum` is ambient: the object is someone
    // else's to create, so nothing is emitted for it.
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
