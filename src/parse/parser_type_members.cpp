// TypeScript's bracketed types: parenthesized and function types, tuples,
// object type literals with their members, and mapped types. Each is parsed
// into its node in full — a member list is not skipped as balanced braces —
// so that what an object type says is in the tree for whoever reads it.

#include "parse/parser.h"
#include "parse/parser_type_util.h"

namespace bronze {

using namespace ast;
using type_parse::finish;
using type_parse::newType;

namespace {

bool isParameterModifier(std::string_view word) {
    return word == "public" || word == "private" || word == "protected" || word == "readonly" ||
           word == "override";
}

}  // namespace

// `(` starts either a parenthesized type or a function type's parameter
// list, and only what follows the matching `)` tells them apart: a function
// type's `)` is followed by `=>`. So the parameter list is tried first,
// speculatively, and given back when it is not one.
//
// In an arrow function's return annotation, `(a): (T) => body` writes the
// arrow's own `=>` after a parenthesized type, so there the reading is a
// function type only if a second `=>` follows its return type.
ast::TypeNodePtr Parser::parseParenOrFunctionType(bool inArrowReturn) {
    const Span start = peek().span;
    {
        const SpeculationMark mark = markSpeculation();
        ++speculativeDepth_;
        TypeScopeGuard scope(*this);
        auto node = newType(TypeNodeKind::Function, start);
        advance();  // '('
        bool ok = parseTypeFnParams(node->params) && diags_.size() == mark.diags &&
                  match(TokenKind::RParen) && check(TokenKind::Arrow);
        if (ok) {
            advance();  // '=>'
            auto ret = parseTypeAnnotation();
            ok = ret && diags_.size() == mark.diags && (!inArrowReturn || check(TokenKind::Arrow));
            if (ok) node->types.push_back(std::move(ret));
        }
        --speculativeDepth_;
        if (ok) return finish(node, previous().span);
        rollbackSpeculation(mark);
    }
    advance();  // '('
    auto node = newType(TypeNodeKind::Parenthesized, start);
    auto inner = parseTypeAnnotation();
    if (!inner) return nullptr;
    if (!expect(TokenKind::RParen, "')' after parenthesized type")) return nullptr;
    node->types.push_back(std::move(inner));
    return finish(node, previous().span);
}

// `new (a: A) => R`, `abstract new () => R`, `<T>(x: T) => T`, cursor on the
// first of those words.
ast::TypeNodePtr Parser::parseFunctionType(bool inArrowReturn) {
    auto node = newType(TypeNodeKind::Function, peek().span);
    if (check(TokenKind::Identifier) && peek().text == "abstract") {
        advance();
        node->isAbstract = true;
    }
    if (match(TokenKind::KwNew)) node->isConstructor = true;
    TypeScopeGuard scope(*this);
    if (check(TokenKind::Less) && !parseTypeParams(&node->typeParams)) return nullptr;
    if (!expect(TokenKind::LParen, "'(' in function type")) return nullptr;
    if (!parseTypeFnParams(node->params)) return nullptr;
    if (!expect(TokenKind::RParen, "')' after function type parameters")) return nullptr;
    if (!expect(TokenKind::Arrow, "'=>' after function type parameters")) return nullptr;
    auto ret = parseTypeAnnotation(inArrowReturn);
    if (!ret) return nullptr;
    node->types.push_back(std::move(ret));
    return finish(node, previous().span);
}

// The parameters of a function type or signature: a name (or `this`, or a
// destructuring pattern), `?`, and an annotation, each optional but the
// first. A pattern's names bind nothing a type can see, so it is passed over
// as balanced brackets.
bool Parser::parseTypeFnParams(std::vector<ast::TypeFnParam>& out) {
    while (!check(TokenKind::RParen)) {
        if (check(TokenKind::EndOfFile)) {
            error("unterminated parameter list in function type");
            return false;
        }
        ast::TypeFnParam param;
        while (check(TokenKind::Identifier) && isParameterModifier(peek().text) &&
               !peek(1).newlineBefore && peek(1).kind != TokenKind::Colon &&
               peek(1).kind != TokenKind::Question && peek(1).kind != TokenKind::Comma &&
               peek(1).kind != TokenKind::RParen) {
            advance();
        }
        param.rest = match(TokenKind::Ellipsis);
        if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) {
            int depth = 0;
            do {
                if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) ++depth;
                else if (check(TokenKind::RBracket) || check(TokenKind::RBrace)) --depth;
                advance();
            } while (depth > 0 && !check(TokenKind::EndOfFile));
            if (depth != 0) {
                error("unterminated binding pattern in function type");
                return false;
            }
        } else if (check(TokenKind::KwThis) || isIdentifierName(peek().kind)) {
            param.name = std::string(advance().text);
        } else {
            error("expected a parameter name in function type");
            return false;
        }
        param.optional = match(TokenKind::Question);
        if (match(TokenKind::Colon)) {
            param.type = parseTypeAnnotation();
            if (!param.type) return false;
        }
        out.push_back(std::move(param));
        if (!match(TokenKind::Comma)) break;
    }
    return true;
}

// `[A, B?, ...C[]]` and the labelled `[x: A, y?: B, ...rest: C[]]`.
ast::TypeNodePtr Parser::parseTupleType() {
    auto node = newType(TypeNodeKind::Tuple, peek().span);
    advance();  // '['
    while (!check(TokenKind::RBracket) && !check(TokenKind::EndOfFile)) {
        ast::TypeTupleElement element;
        element.rest = match(TokenKind::Ellipsis);
        if (isIdentifierName(peek().kind) &&
            (peek(1).kind == TokenKind::Colon ||
             (peek(1).kind == TokenKind::Question && peek(2).kind == TokenKind::Colon))) {
            element.label = std::string(advance().text);
            element.optional = match(TokenKind::Question);
            advance();  // ':'
        }
        element.type = parseTypeAnnotation();
        if (!element.type) return nullptr;
        if (match(TokenKind::Question)) element.optional = true;
        node->elements.push_back(std::move(element));
        if (!match(TokenKind::Comma)) break;
    }
    if (!expect(TokenKind::RBracket, "']' after tuple type")) return nullptr;
    return finish(node, previous().span);
}

// `{ [K in C]: T }`, possibly with `readonly` or `+readonly` / `-readonly`
// before the bracket: the bracket holding a name and then `in` is what makes
// it one.
bool Parser::isMappedTypeStart() const {
    size_t i = 1;
    if (peek(i).kind == TokenKind::Plus || peek(i).kind == TokenKind::Minus) {
        ++i;
        if (!(peek(i).kind == TokenKind::Identifier && peek(i).text == "readonly")) return false;
        ++i;
    } else if (peek(i).kind == TokenKind::Identifier && peek(i).text == "readonly") {
        ++i;
    }
    return peek(i).kind == TokenKind::LBracket && isIdentifierName(peek(i + 1).kind) &&
           peek(i + 2).kind == TokenKind::KwIn;
}

ast::TypeNodePtr Parser::parseObjectOrMappedType() {
    const Span start = peek().span;
    if (isMappedTypeStart()) return parseMappedType(start);
    auto node = newType(TypeNodeKind::Object, start);
    advance();  // '{'
    while (!check(TokenKind::RBrace)) {
        if (check(TokenKind::EndOfFile)) {
            error("unterminated object type literal");
            return nullptr;
        }
        ast::TypeMember member;
        if (!parseTypeMember(member)) return nullptr;
        node->members.push_back(std::move(member));
        // Members are separated by `;`, `,`, or a line break.
        if (match(TokenKind::Semicolon) || match(TokenKind::Comma)) continue;
        if (check(TokenKind::RBrace) || atLineBreak()) continue;
        error("expected ';' or ',' between the members of an object type");
        return nullptr;
    }
    advance();  // '}'
    return finish(node, previous().span);
}

ast::TypeNodePtr Parser::parseMappedType(Span start) {
    auto node = newType(TypeNodeKind::Mapped, start);
    advance();  // '{'
    if (check(TokenKind::Plus) || check(TokenKind::Minus)) {
        node->readonlyMod = check(TokenKind::Plus) ? '+' : '-';
        advance();
        advance();  // 'readonly' (isMappedTypeStart checked it)
    } else if (check(TokenKind::Identifier) && peek().text == "readonly") {
        node->readonlyMod = ' ';
        advance();
    }
    advance();  // '['
    // The key is a type parameter whose scope is the mapped type.
    TypeScopeGuard scope(*this);
    ast::TypeParameter key;
    key.name = std::string(advance().text);
    declareType(key.name, ast::kTypeDeclParameter);
    advance();  // 'in'
    key.constraint = parseTypeAnnotation();
    if (!key.constraint) return nullptr;
    node->typeParams.push_back(std::move(key));
    ast::TypeNodePtr nameType;
    if (check(TokenKind::Identifier) && peek().text == "as") {
        advance();
        nameType = parseTypeAnnotation();
        if (!nameType) return nullptr;
    }
    if (!expect(TokenKind::RBracket, "']' in mapped type")) return nullptr;
    if (check(TokenKind::Plus) || check(TokenKind::Minus)) {
        node->optionalMod = check(TokenKind::Plus) ? '+' : '-';
        advance();
        if (!expect(TokenKind::Question, "'?' after '+' or '-' in mapped type")) return nullptr;
    } else if (match(TokenKind::Question)) {
        node->optionalMod = ' ';
    }
    ast::TypeNodePtr valueType;
    if (match(TokenKind::Colon)) {
        valueType = parseTypeAnnotation();
        if (!valueType) return nullptr;
    }
    node->types = {std::move(nameType), std::move(valueType)};
    if (!match(TokenKind::Semicolon)) match(TokenKind::Comma);
    if (!expect(TokenKind::RBrace, "'}' to close mapped type")) return nullptr;
    return finish(node, previous().span);
}

// One member of an object type: a property, a method, a call or construct
// signature, an index signature, or an accessor.
bool Parser::parseTypeMember(ast::TypeMember& out) {
    if (check(TokenKind::LParen) || check(TokenKind::Less)) {
        out.kind = ast::TypeMemberKind::Call;
        return parseSignatureTail(out);
    }
    if (check(TokenKind::KwNew) &&
        (peek(1).kind == TokenKind::LParen || peek(1).kind == TokenKind::Less)) {
        advance();  // 'new'
        out.kind = ast::TypeMemberKind::Construct;
        return parseSignatureTail(out);
    }
    // A modifier word is one only when a member name follows it: `readonly:
    // number` is a property called readonly, `get(): T` a method called get.
    auto nameFollows = [&](size_t ahead) {
        const Token& t = peek(ahead);
        return !t.newlineBefore &&
               (isIdentifierName(t.kind) || t.kind == TokenKind::StringLiteral ||
                t.kind == TokenKind::NumberLiteral || t.kind == TokenKind::LBracket ||
                t.kind == TokenKind::PrivateName);
    };
    if (check(TokenKind::Identifier) && peek().text == "readonly" && nameFollows(1)) {
        advance();
        out.readonly = true;
    }
    if (check(TokenKind::Identifier) && (peek().text == "get" || peek().text == "set") &&
        nameFollows(1)) {
        out.kind = peek().text == "get" ? ast::TypeMemberKind::Getter : ast::TypeMemberKind::Setter;
        advance();
    }
    // `[key: K]: V`. A bracket holding anything else is a computed key.
    if (check(TokenKind::LBracket) && isIdentifierName(peek(1).kind) &&
        peek(2).kind == TokenKind::Colon) {
        advance();  // '['
        ast::TypeFnParam key;
        key.name = std::string(advance().text);
        advance();  // ':'
        key.type = parseTypeAnnotation();
        if (!key.type) return false;
        if (!expect(TokenKind::RBracket, "']' after index signature parameter")) return false;
        out.kind = ast::TypeMemberKind::Index;
        out.params.push_back(std::move(key));
        if (match(TokenKind::Colon)) {
            out.type = parseTypeAnnotation();
            if (!out.type) return false;
        }
        return true;
    }
    if (check(TokenKind::LBracket)) {
        // The key's expression is a value, so it is kept only as text.
        out.name = "[";
        advance();
        int depth = 1;
        while (depth > 0 && !check(TokenKind::EndOfFile)) {
            if (check(TokenKind::LBracket)) ++depth;
            if (check(TokenKind::RBracket) && --depth == 0) break;
            out.name += std::string(advance().text);
        }
        if (!expect(TokenKind::RBracket, "']' after computed member name")) return false;
        out.name += "]";
    } else if (isIdentifierName(peek().kind) || check(TokenKind::StringLiteral) ||
               check(TokenKind::NumberLiteral) || check(TokenKind::PrivateName)) {
        out.name = std::string(advance().text);
    } else {
        error("expected a member name in object type");
        return false;
    }
    out.optional = match(TokenKind::Question);
    if (out.kind == ast::TypeMemberKind::Getter || out.kind == ast::TypeMemberKind::Setter) {
        return parseSignatureTail(out);
    }
    if (check(TokenKind::LParen) || check(TokenKind::Less)) {
        out.kind = ast::TypeMemberKind::Method;
        return parseSignatureTail(out);
    }
    out.kind = ast::TypeMemberKind::Property;
    if (match(TokenKind::Colon)) {
        out.type = parseTypeAnnotation();
        if (!out.type) return false;
    }
    return true;
}

// `<T>(params): R` after a signature's name, with the type parameters in a
// scope of the signature's own.
bool Parser::parseSignatureTail(ast::TypeMember& out) {
    TypeScopeGuard scope(*this);
    if (check(TokenKind::Less) && !parseTypeParams(&out.typeParams)) return false;
    if (!expect(TokenKind::LParen, "'(' in a signature")) return false;
    if (!parseTypeFnParams(out.params)) return false;
    if (!expect(TokenKind::RParen, "')' after signature parameters")) return false;
    if (match(TokenKind::Colon)) {
        out.type = parseTypeAnnotation();
        if (!out.type) return false;
    }
    return true;
}

}  // namespace bronze
