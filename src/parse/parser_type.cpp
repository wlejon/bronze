// TypeScript types: the grammar from the conditional type down to a type
// reference, the generic parameter and argument lists, and the type scopes
// the names are declared in. The bracketed forms — parenthesized and
// function types, tuples, object and mapped types — are in
// parser_type_members.cpp, and the lookaheads that decide whether a `<` or a
// `(` in an expression starts something typed are in parser_type_lookahead.cpp.
//
// Every production builds a node (ast/type.h). The tree is all lowering
// reads: what an annotation hints is decided from its shape, never from its
// text.

#include "parse/parser.h"
#include "parse/parser_type_util.h"

namespace bronze {

using namespace ast;
using type_parse::finish;
using type_parse::newType;

namespace {

// TypeScript's predefined type names, which the grammar reads as keywords:
// no declaration can take one of these names, so they never need resolving.
bool isPredefinedTypeName(std::string_view name) {
    return name == "any" || name == "unknown" || name == "never" || name == "number" ||
           name == "string" || name == "boolean" || name == "bigint" || name == "symbol" ||
           name == "object" || name == "undefined";
}

// The text between a template piece's delimiters: a head runs from the
// backtick to the `${`, a middle from the `}` to the `${`, a tail from the
// `}` to the backtick.
std::string templatePieceText(const Token& tok) {
    std::string_view text = tok.text;
    const size_t open = 1;
    size_t close = 1;
    if (tok.kind == TokenKind::TemplateHead || tok.kind == TokenKind::TemplateMiddle) close = 2;
    if (text.size() < open + close) return {};
    return std::string(text.substr(open, text.size() - open - close));
}

}  // namespace

Parser::SpeculationMark Parser::markSpeculation() const {
    return SpeculationMark{pos_, tokenEditsLog_.size(), diags_.size(), typeDeclUndo_.size()};
}

void Parser::rollbackSpeculation(const SpeculationMark& mark) {
    while (tokenEditsLog_.size() > mark.edits) {
        const auto& entry = tokenEditsLog_.back();
        tokens_[entry.first] = entry.second;
        tokenEditsLog_.pop_back();
    }
    while (typeDeclUndo_.size() > mark.typeDecls) {
        TypeDeclUndo& undo = typeDeclUndo_.back();
        if (undo.previous) {
            undo.scope->decls[undo.name] = std::move(undo.previous);
        } else {
            undo.scope->decls.erase(undo.name);
        }
        typeDeclUndo_.pop_back();
    }
    diags_.truncate(mark.diags);
    pos_ = mark.pos;
}

// Only a TypeScript file opens scopes below the module's. A `.js` file can
// write an annotation but can declare no type except a class, and opening a
// scope for every block of a large JavaScript program would be paid by every
// program for a feature none of them uses.
Parser::TypeScopeGuard::TypeScopeGuard(Parser& parser) : p(parser) {
    if (!p.currentModule_ || !p.typescript_) return;
    auto scope = std::make_unique<ast::TypeScope>();
    scope->parent = p.currentTypeScope();
    p.typeScopes_.push_back(scope.get());
    p.currentModule_->typeScopes.push_back(std::move(scope));
    pushed = true;
}

Parser::TypeScopeGuard::~TypeScopeGuard() {
    if (pushed) p.typeScopes_.pop_back();
}

// Copy on write, so that a declaration made while speculating can be undone
// by putting the old pointer back: merging into the existing declaration in
// place would leave nothing to restore. A type parameter never merges — it
// is a fresh name in its own scope, whatever the scope held before.
ast::TypeDeclPtr Parser::declareType(const std::string& name, uint8_t kind) {
    ast::TypeScope* scope = currentTypeScope();
    if (scope == nullptr || name.empty()) return nullptr;
    ast::TypeDeclPtr& slot = scope->decls[name];
    ast::TypeDeclPtr previous = slot;
    const bool merges = previous && kind != ast::kTypeDeclParameter &&
                        (previous->kinds & ast::kTypeDeclParameter) == 0;
    auto fresh = merges ? std::make_shared<ast::TypeDecl>(*previous)
                        : std::make_shared<ast::TypeDecl>();
    fresh->name = name;
    fresh->kinds |= kind;
    if (speculativeDepth_ > 0) typeDeclUndo_.push_back({scope, name, std::move(previous)});
    slot = fresh;
    return fresh;
}

// Filled in once the `extends` clause is read: whether an instance is an
// ordinary object depends on the base, and the base is named from the scope
// the class statement stands in (`scope`, captured before the class opened
// its own).
void Parser::setClassHeritage(const ast::TypeDeclPtr& decl, const ast::Expr* superClass,
                              const std::string& superName, const ast::TypeScope* scope) {
    if (!decl) return;
    decl->classExtends = superClass != nullptr;
    decl->heritage.reset();
    if (!superName.empty()) {
        auto base = std::make_shared<ast::TypeNode>();
        base->kind = TypeNodeKind::Reference;
        base->name = superName;
        base->scope = scope;
        base->span = superClass->span;
        decl->heritage = std::move(base);
    }
}

bool Parser::matchTypeGreater() {
    if (pos_ >= tokens_.size()) return false;
    Token& tok = tokens_[pos_];
    if (tok.kind == TokenKind::Greater) {
        advance();
        return true;
    }
    TokenKind replacementKind = TokenKind::EndOfFile;
    if (tok.kind == TokenKind::GreaterGreater) {
        replacementKind = TokenKind::Greater;
    } else if (tok.kind == TokenKind::GreaterGreaterGreater) {
        replacementKind = TokenKind::GreaterGreater;
    } else if (tok.kind == TokenKind::GreaterEqual) {
        replacementKind = TokenKind::Assign;
    } else if (tok.kind == TokenKind::GreaterGreaterAssign) {
        replacementKind = TokenKind::GreaterEqual;
    } else if (tok.kind == TokenKind::GreaterGreaterGreaterAssign) {
        replacementKind = TokenKind::GreaterGreaterAssign;
    } else {
        return false;
    }
    if (speculativeDepth_ > 0) {
        tokenEditsLog_.emplace_back(pos_, tok);
    }
    tok.kind = replacementKind;
    if (!tok.text.empty()) tok.text = tok.text.substr(1);
    tok.span.begin += 1;
    tok.newlineBefore = false;
    return true;
}

bool Parser::expectTypeGreater(const char* what) {
    if (matchTypeGreater()) return true;
    std::string msg = std::string("expected ") + what + ", got '" +
                      std::string(peek().text.empty() ? tokenKindName(peek().kind) : peek().text) + "'";
    diags_.error(peek().span, msg);
    return false;
}

bool Parser::parseTypeParams(std::vector<ast::TypeParameter>* out) {
    if (!match(TokenKind::Less)) return false;
    while (!check(TokenKind::EndOfFile)) {
        if (matchTypeGreater()) return true;
        // Optional `const`, `in`, `out` variance/const modifiers
        while (check(TokenKind::KwConst) || check(TokenKind::KwIn) ||
               (check(TokenKind::Identifier) && peek().text == "out")) {
            const TokenKind nextKind = peek(1).kind;
            if (nextKind == TokenKind::Comma || nextKind == TokenKind::Greater ||
                nextKind == TokenKind::GreaterGreater || nextKind == TokenKind::GreaterGreaterGreater ||
                nextKind == TokenKind::KwExtends || nextKind == TokenKind::Assign) {
                break;
            }
            advance();
        }
        const Token* nameTok = expectPropertyName("type parameter name");
        if (!nameTok) return false;
        ast::TypeParameter param;
        param.name = std::string(nameTok->text);
        declareType(param.name, ast::kTypeDeclParameter);
        if (match(TokenKind::KwExtends)) {
            param.constraint = parseTypeAnnotation();
            if (!param.constraint) return false;
        }
        if (match(TokenKind::Assign)) {
            param.defaultType = parseTypeAnnotation();
            if (!param.defaultType) return false;
        }
        if (out) out->push_back(std::move(param));
        if (match(TokenKind::Comma)) continue;
        return expectTypeGreater("'>' after type parameters");
    }
    error("unterminated type parameter list");
    return false;
}

bool Parser::parseTypeArguments(std::vector<ast::TypeNodePtr>& out) {
    if (!match(TokenKind::Less)) return false;
    while (!check(TokenKind::EndOfFile)) {
        if (matchTypeGreater()) return true;
        auto arg = parseTypeAnnotation();
        if (!arg) return false;
        out.push_back(std::move(arg));
        if (match(TokenKind::Comma)) continue;
        return expectTypeGreater("'>' after type arguments");
    }
    error("unterminated type argument list");
    return false;
}

ast::TypeNodePtr Parser::parseTypeAnnotation(bool inArrowReturn) {
    const Span start = peek().span;
    struct ExtendsReset {
        bool& flag;
        bool saved;
        ~ExtendsReset() { flag = saved; }
    } reset{inConditionalExtends_, inConditionalExtends_};
    inConditionalExtends_ = false;
    // `asserts x` or `asserts x is T`
    if (check(TokenKind::Identifier) && peek().text == "asserts" && !peek(1).newlineBefore &&
        (isIdentifierName(peek(1).kind) || peek(1).kind == TokenKind::KwThis)) {
        auto node = newType(TypeNodeKind::Predicate, start);
        node->asserts = true;
        advance();  // 'asserts'
        node->name = std::string(advance().text);
        if (check(TokenKind::Identifier) && peek().text == "is" && !atLineBreak()) {
            advance();  // 'is'
            auto type = parseTypeAnnotation();
            if (!type) return nullptr;
            node->types.push_back(std::move(type));
        }
        return finish(node, previous().span);
    }
    // `x is T` or `this is T`
    if ((check(TokenKind::Identifier) || check(TokenKind::KwThis)) &&
        peek(1).kind == TokenKind::Identifier && peek(1).text == "is" && !peek(1).newlineBefore) {
        auto node = newType(TypeNodeKind::Predicate, start);
        node->name = std::string(advance().text);
        advance();  // 'is'
        auto type = parseTypeAnnotation();
        if (!type) return nullptr;
        node->types.push_back(std::move(type));
        return finish(node, previous().span);
    }

    auto checked = parseTypeUnion(inArrowReturn);
    if (!checked) return nullptr;
    // `C extends E ? T : F`. The extends type is read without a conditional
    // of its own — `A extends B extends C ? …` is not one — and a line break
    // before `extends` ends the type, TypeScript's rule.
    if (!check(TokenKind::KwExtends) || atLineBreak()) return checked;
    advance();  // 'extends'
    auto node = newType(TypeNodeKind::Conditional, start);
    inConditionalExtends_ = true;
    auto extendsType = parseTypeUnion(false);
    inConditionalExtends_ = false;
    if (!extendsType) return nullptr;
    if (!expect(TokenKind::Question, "'?' in conditional type")) return nullptr;
    auto trueType = parseTypeAnnotation();
    if (!trueType) return nullptr;
    if (!expect(TokenKind::Colon, "':' in conditional type")) return nullptr;
    auto falseType = parseTypeAnnotation(inArrowReturn);
    if (!falseType) return nullptr;
    node->types = {std::move(checked), std::move(extendsType), std::move(trueType),
                   std::move(falseType)};
    return finish(node, previous().span);
}

// `A | B`, with the leading `|` TypeScript allows; a single member is that
// member and not a one-member union.
ast::TypeNodePtr Parser::parseTypeUnion(bool inArrowReturn) {
    const Span start = peek().span;
    match(TokenKind::Pipe);
    auto first = parseTypeIntersection(inArrowReturn);
    if (!first || !check(TokenKind::Pipe)) return first;
    auto node = newType(TypeNodeKind::Union, start);
    node->types.push_back(std::move(first));
    while (match(TokenKind::Pipe)) {
        auto member = parseTypeIntersection(inArrowReturn);
        if (!member) return nullptr;
        node->types.push_back(std::move(member));
    }
    return finish(node, previous().span);
}

ast::TypeNodePtr Parser::parseTypeIntersection(bool inArrowReturn) {
    const Span start = peek().span;
    match(TokenKind::Amp);
    auto first = parseTypeOperand(inArrowReturn);
    if (!first || !check(TokenKind::Amp)) return first;
    auto node = newType(TypeNodeKind::Intersection, start);
    node->types.push_back(std::move(first));
    while (match(TokenKind::Amp)) {
        auto member = parseTypeOperand(inArrowReturn);
        if (!member) return nullptr;
        node->types.push_back(std::move(member));
    }
    return finish(node, previous().span);
}

// The prefix operators, which bind looser than the array postfix: `keyof
// T[]` is `keyof (T[])`.
ast::TypeNodePtr Parser::parseTypeOperand(bool inArrowReturn) {
    const Span start = peek().span;
    // `readonly`, `keyof` and `unique` are operators only when an operand
    // follows on the same line; otherwise they are names (`{ keyof: 1 }` has
    // a property called keyof, and a type may be named `unique`).
    if (check(TokenKind::Identifier) &&
        (peek().text == "readonly" || peek().text == "keyof" || peek().text == "unique") &&
        !peek(1).newlineBefore &&
        peek(1).kind != TokenKind::Colon &&
        peek(1).kind != TokenKind::Comma && peek(1).kind != TokenKind::Semicolon &&
        peek(1).kind != TokenKind::RBrace && peek(1).kind != TokenKind::RBracket &&
        peek(1).kind != TokenKind::RParen && peek(1).kind != TokenKind::Pipe &&
        peek(1).kind != TokenKind::Amp && peek(1).kind != TokenKind::Assign &&
        peek(1).kind != TokenKind::Greater && peek(1).kind != TokenKind::GreaterGreater &&
        peek(1).kind != TokenKind::Dot && peek(1).kind != TokenKind::Question &&
        peek(1).kind != TokenKind::Less &&
        peek(1).kind != TokenKind::EndOfFile) {
        auto node = newType(TypeNodeKind::Operator, start);
        node->name = std::string(advance().text);
        auto operand = parseTypeOperand(inArrowReturn);
        if (!operand) return nullptr;
        node->types.push_back(std::move(operand));
        return finish(node, previous().span);
    }
    // `infer U`, or `infer U extends C`. The constraint is TypeScript's
    // ambiguity, resolved its way: in a conditional's extends type, where no
    // unbracketed conditional may stand, `extends C` is always the
    // constraint (`T extends infer U extends C ? X : Y`). Elsewhere, as in
    // `[infer U extends C ? X : Y]`, a `?` after it shows the `extends`
    // begins a conditional, so the constraint is read speculatively and
    // given back when one follows.
    if (check(TokenKind::Identifier) && peek().text == "infer" && !peek(1).newlineBefore &&
        isIdentifierName(peek(1).kind)) {
        auto node = newType(TypeNodeKind::Infer, start);
        advance();  // 'infer'
        node->name = std::string(advance().text);
        declareType(node->name, ast::kTypeDeclParameter);
        if (check(TokenKind::KwExtends)) {
            const SpeculationMark mark = markSpeculation();
            ++speculativeDepth_;
            advance();  // 'extends'
            auto constraint = parseTypeUnion(false);
            --speculativeDepth_;
            if (constraint && diags_.size() == mark.diags &&
                (inConditionalExtends_ || !check(TokenKind::Question))) {
                node->types.push_back(std::move(constraint));
            } else {
                rollbackSpeculation(mark);
            }
        }
        return finish(node, previous().span);
    }
    return parseTypePrimary(inArrowReturn);
}

ast::TypeNodePtr Parser::parseTypePrimary(bool inArrowReturn) {
    const Span start = peek().span;
    ast::TypeNodePtr base;
    if ((check(TokenKind::Identifier) && peek().text == "abstract" &&
         peek(1).kind == TokenKind::KwNew) ||
        check(TokenKind::KwNew) || check(TokenKind::Less)) {
        return parseFunctionType(inArrowReturn);
    }
    if (check(TokenKind::LParen)) {
        base = parseParenOrFunctionType(inArrowReturn);
    } else if (check(TokenKind::LBracket)) {
        base = parseTupleType();
    } else if (check(TokenKind::LBrace)) {
        base = parseObjectOrMappedType();
    } else if (check(TokenKind::KwVoid) || check(TokenKind::KwNull) ||
               check(TokenKind::KwUndefined) || check(TokenKind::KwThis)) {
        auto node = newType(TypeNodeKind::Keyword, start);
        node->name = std::string(advance().text);
        base = finish(node, previous().span);
    } else if (check(TokenKind::KwTrue) || check(TokenKind::KwFalse)) {
        auto node = newType(TypeNodeKind::Literal, start);
        node->literal = TypeLiteralKind::Boolean;
        node->name = std::string(advance().text);
        base = finish(node, previous().span);
    } else if (check(TokenKind::StringLiteral)) {
        auto node = newType(TypeNodeKind::Literal, start);
        node->literal = TypeLiteralKind::String;
        node->name = std::string(advance().text);
        base = finish(node, previous().span);
    } else if (check(TokenKind::NumberLiteral) ||
               ((check(TokenKind::Minus) || check(TokenKind::Plus)) &&
                peek(1).kind == TokenKind::NumberLiteral)) {
        auto node = newType(TypeNodeKind::Literal, start);
        if (!check(TokenKind::NumberLiteral)) node->name = std::string(advance().text);
        const Token& number = advance();
        node->name += std::string(number.text);
        node->literal = hasBigIntSuffix(number.text) ? TypeLiteralKind::BigInt
                                                     : TypeLiteralKind::Number;
        base = finish(node, previous().span);
    } else if (check(TokenKind::TemplateWhole) || check(TokenKind::TemplateHead)) {
        base = parseTemplateLiteralType();
    } else if (check(TokenKind::KwImport) && peek(1).kind == TokenKind::LParen) {
        base = parseImportType();
    } else if (check(TokenKind::KwTypeof)) {
        base = parseTypeQuery();
    } else if (isIdentifierName(peek().kind)) {
        if (check(TokenKind::Identifier) && isPredefinedTypeName(peek().text) &&
            peek(1).kind != TokenKind::Dot) {
            auto node = newType(TypeNodeKind::Keyword, start);
            node->name = std::string(advance().text);
            base = finish(node, previous().span);
        } else {
            base = parseTypeReference();
        }
    } else {
        const Token* t = expect(TokenKind::Identifier, "type name");
        if (!t) return nullptr;
    }
    if (!base) return nullptr;

    // Postfix `[]` (array type) or `[K]` (indexed access type), never across
    // a line break.
    while (check(TokenKind::LBracket) && !atLineBreak()) {
        advance();  // '['
        if (match(TokenKind::RBracket)) {
            auto node = newType(TypeNodeKind::Array, start);
            node->types.push_back(std::move(base));
            base = finish(node, previous().span);
            continue;
        }
        auto index = parseTypeAnnotation();
        if (!index) return nullptr;
        if (!expect(TokenKind::RBracket, "']' after indexed access type")) return nullptr;
        auto node = newType(TypeNodeKind::Indexed, start);
        node->types = {std::move(base), std::move(index)};
        base = finish(node, previous().span);
    }
    return base;
}

// `A`, `ns.A`, `A<B>`. The reference remembers the scope it is written in;
// which declaration it names is decided when the tree is read, because the
// declaration may come later in the scope and an import is bound into the
// module's scope only by the linker.
ast::TypeNodePtr Parser::parseTypeReference() {
    auto node = newType(TypeNodeKind::Reference, peek().span);
    node->scope = currentTypeScope();
    node->name = std::string(advance().text);
    while (match(TokenKind::Dot)) {
        const Token* member = expectPropertyName("qualified type name");
        if (!member) return nullptr;
        node->name += "." + std::string(member->text);
    }
    if (check(TokenKind::Less) && !atLineBreak()) {
        if (!parseTypeArguments(node->types)) return nullptr;
    }
    return finish(node, previous().span);
}

// `typeof x`, `typeof x.y<T>`, `typeof import("m")`. The arguments are
// optional and read speculatively, the way the expression grammar reads an
// instantiation expression.
ast::TypeNodePtr Parser::parseTypeQuery() {
    auto node = newType(TypeNodeKind::Query, peek().span);
    node->scope = currentTypeScope();
    advance();  // 'typeof'
    if (check(TokenKind::KwImport) && peek(1).kind == TokenKind::LParen) {
        advance();  // 'import'
        advance();  // '('
        const Token* spec = expect(TokenKind::StringLiteral, "module specifier in typeof import(...)");
        if (!spec) return nullptr;
        node->name = "import(" + std::string(spec->text) + ")";
        if (!expect(TokenKind::RParen, "')' after import type")) return nullptr;
    } else if (isIdentifierName(peek().kind) || check(TokenKind::KwThis)) {
        node->name = std::string(advance().text);
    } else {
        error("expected identifier after 'typeof' in type annotation");
        return nullptr;
    }
    while (match(TokenKind::Dot)) {
        const Token* member = expectPropertyName("property name in typeof type");
        if (!member) return nullptr;
        node->name += "." + std::string(member->text);
    }
    if (check(TokenKind::Less) && !atLineBreak()) {
        const SpeculationMark mark = markSpeculation();
        ++speculativeDepth_;
        std::vector<ast::TypeNodePtr> args;
        const bool ok = parseTypeArguments(args) && diags_.size() == mark.diags;
        --speculativeDepth_;
        if (ok) {
            node->types = std::move(args);
        } else {
            rollbackSpeculation(mark);
        }
    }
    return finish(node, previous().span);
}

ast::TypeNodePtr Parser::parseImportType() {
    auto node = newType(TypeNodeKind::Import, peek().span);
    advance();  // 'import'
    advance();  // '('
    const Token* spec = expect(TokenKind::StringLiteral, "module specifier in import(...) type");
    if (!spec) return nullptr;
    node->name = decodeStringLiteral(spec->text.substr(1, spec->text.size() - 2), spec->span);
    if (!expect(TokenKind::RParen, "')' after import(...) type")) return nullptr;
    while (match(TokenKind::Dot)) {
        const Token* prop = expectPropertyName("type name after 'import(...).'");
        if (!prop) return nullptr;
        node->qualifier.emplace_back(prop->text);
    }
    if (check(TokenKind::Less) && !atLineBreak()) {
        if (!parseTypeArguments(node->types)) return nullptr;
    }
    return finish(node, previous().span);
}

// `` `a${T}b` ``. The lexer hands a template over as its pieces with the
// substitutions' own tokens between them, which in a type are types.
ast::TypeNodePtr Parser::parseTemplateLiteralType() {
    auto node = newType(TypeNodeKind::TemplateLiteral, peek().span);
    if (check(TokenKind::TemplateWhole)) {
        node->quasis.push_back(templatePieceText(advance()));
        return finish(node, previous().span);
    }
    node->quasis.push_back(templatePieceText(advance()));  // the head
    for (;;) {
        auto substitution = parseTypeAnnotation();
        if (!substitution) return nullptr;
        node->types.push_back(std::move(substitution));
        if (check(TokenKind::TemplateMiddle)) {
            node->quasis.push_back(templatePieceText(advance()));
            continue;
        }
        if (check(TokenKind::TemplateTail)) {
            node->quasis.push_back(templatePieceText(advance()));
            break;
        }
        error("unterminated template literal type");
        return nullptr;
    }
    return finish(node, previous().span);
}

}  // namespace bronze
