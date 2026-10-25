// TypeScript type annotations, generic type parameters, speculative call type
// arguments, and generic/annotated arrow function lookahead.

#include "parse/parser.h"

namespace bronze {

using namespace ast;

namespace {

struct SpeculativeScope {
    Parser& p;
    size_t savedPos;
    size_t savedEditCount;
    size_t savedDiagCount;
    size_t savedDeclaredTypesCount;
    bool committed = false;

    explicit SpeculativeScope(Parser& parser, size_t startPos, size_t editCount,
                              size_t diagCount, size_t declaredCount)
        : p(parser),
          savedPos(startPos),
          savedEditCount(editCount),
          savedDiagCount(diagCount),
          savedDeclaredTypesCount(declaredCount) {}
};

void trimSpaces(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) {
        s.pop_back();
    }
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\n' || s[start] == '\r')) {
        ++start;
    }
    if (start > 0) s.erase(0, start);
}

}  // namespace

void Parser::recordDeclaredType(const std::string& name) {
    if (name.empty() || !currentModule_) return;
    if (currentModule_->declaredTypes.insert(name).second) {
        if (speculativeDepth_ > 0) {
            declaredTypesUndo_.push_back(name);
        }
    }
}

void Parser::recordTypeAlias(const std::string& name, const std::string& rhs) {
    if (name.empty() || !currentModule_) return;
    recordDeclaredType(name);
    if (speculativeDepth_ == 0 && !rhs.empty()) {
        currentModule_->typeAliases[name] = rhs;
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

bool Parser::parseTypeParams() {
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
        recordDeclaredType(std::string(nameTok->text));

        if (match(TokenKind::KwExtends)) {
            if (parseTypeAnnotation().empty()) return false;
        }
        if (match(TokenKind::Assign)) {
            if (parseTypeAnnotation().empty()) return false;
        }
        if (match(TokenKind::Comma)) continue;
        return expectTypeGreater("'>' after type parameters");
    }
    error("unterminated type parameter list");
    return false;
}

std::string Parser::parseTypeAnnotation(bool inArrowReturn) {
    auto rollbackFrom = [&](size_t savedPos, size_t savedEditCount, size_t savedDiagCount,
                            size_t savedDeclaredCount) {
        while (tokenEditsLog_.size() > savedEditCount) {
            const auto& entry = tokenEditsLog_.back();
            tokens_[entry.first] = entry.second;
            tokenEditsLog_.pop_back();
        }
        while (declaredTypesUndo_.size() > savedDeclaredCount) {
            if (currentModule_) currentModule_->declaredTypes.erase(declaredTypesUndo_.back());
            declaredTypesUndo_.pop_back();
        }
        diags_.truncate(savedDiagCount);
        pos_ = savedPos;
    };

    auto parseTypeInternal = [&](auto& self, bool arrowRet) -> std::string {
        // `asserts x` or `asserts x is T`
        if (check(TokenKind::Identifier) && peek().text == "asserts" && !peek(1).newlineBefore &&
            (isIdentifierName(peek(1).kind) || peek(1).kind == TokenKind::KwThis)) {
            advance();  // 'asserts'
            advance();  // param name or 'this'
            if (check(TokenKind::Identifier) && peek().text == "is") {
                advance();  // 'is'
                if (self(self, false).empty()) return {};
            }
            return "void";
        }
        // `x is T` or `this is T` type predicate
        if ((check(TokenKind::Identifier) || check(TokenKind::KwThis)) &&
            peek(1).kind == TokenKind::Identifier && peek(1).text == "is" && !peek(1).newlineBefore) {
            advance();  // param or 'this'
            advance();  // 'is'
            if (self(self, false).empty()) return {};
            return "boolean";
        }

        auto parsePrimaryAndPostfix = [&](auto& primarySelf) -> std::string {
            // Prefix type operators: `readonly`, `keyof`, `typeof`, `infer`, `unique`
            if (check(TokenKind::Identifier) &&
                (peek().text == "readonly" || peek().text == "keyof" || peek().text == "unique") &&
                !peek(1).newlineBefore &&
                peek(1).kind != TokenKind::Colon && peek(1).kind != TokenKind::LParen &&
                peek(1).kind != TokenKind::Comma && peek(1).kind != TokenKind::Semicolon &&
                peek(1).kind != TokenKind::RBrace && peek(1).kind != TokenKind::RBracket &&
                peek(1).kind != TokenKind::RParen && peek(1).kind != TokenKind::Pipe &&
                peek(1).kind != TokenKind::Amp && peek(1).kind != TokenKind::Assign &&
                peek(1).kind != TokenKind::Greater && peek(1).kind != TokenKind::GreaterGreater) {
                const std::string op(advance().text);
                std::string sub = primarySelf(primarySelf);
                if (sub.empty()) return {};
                return op + " " + sub;
            }
            if (check(TokenKind::Identifier) && peek().text == "infer" && !peek(1).newlineBefore &&
                isIdentifierName(peek(1).kind)) {
                advance();  // 'infer'
                const std::string varName(advance().text);
                recordDeclaredType(varName);
                if (match(TokenKind::KwExtends)) {
                    // `infer U extends V`: consume V at non-conditional level
                    if (primarySelf(primarySelf).empty()) return {};
                }
                return "infer " + varName;
            }
            if (check(TokenKind::KwTypeof)) {
                advance();  // 'typeof'
                if (check(TokenKind::KwImport) && peek(1).kind == TokenKind::LParen) {
                    advance();  // 'import'
                    advance();  // '('
                    if (!expect(TokenKind::StringLiteral, "module specifier in typeof import(...)")) {
                        return {};
                    }
                    if (!expect(TokenKind::RParen, "')' after import type")) return {};
                } else if (isIdentifierName(peek().kind) || check(TokenKind::KwThis)) {
                    advance();
                } else {
                    error("expected identifier after 'typeof' in type annotation");
                    return {};
                }
                while (match(TokenKind::Dot)) {
                    if (!expectPropertyName("property name in typeof type")) return {};
                }
                if (check(TokenKind::Less)) {
                    const size_t sp = pos_, se = tokenEditsLog_.size(), sd = diags_.size(),
                                 st = declaredTypesUndo_.size();
                    ++speculativeDepth_;
                    bool okArgs = true;
                    advance();  // '<'
                    while (!check(TokenKind::EndOfFile)) {
                        if (matchTypeGreater()) break;
                        if (self(self, false).empty()) {
                            okArgs = false;
                            break;
                        }
                        if (match(TokenKind::Comma)) continue;
                        if (!matchTypeGreater()) okArgs = false;
                        break;
                    }
                    --speculativeDepth_;
                    if (!okArgs) rollbackFrom(sp, se, sd, st);
                }
                return "any";
            }

            // Function or constructor type starting with `new`, `abstract new`, or `<TypeParams>(...) =>`
            bool isAbstractCtor = false;
            if (check(TokenKind::Identifier) && peek().text == "abstract" &&
                peek(1).kind == TokenKind::KwNew) {
                advance();  // 'abstract'
                isAbstractCtor = true;
            }
            if (isAbstractCtor || check(TokenKind::KwNew) || check(TokenKind::Less)) {
                if (check(TokenKind::KwNew)) advance();
                if (check(TokenKind::Less)) {
                    if (!parseTypeParams()) return {};
                }
                if (!expect(TokenKind::LParen, "'(' in function type")) return {};
                int parenDepth = 1;
                while (parenDepth > 0 && !check(TokenKind::EndOfFile)) {
                    if (match(TokenKind::LParen)) ++parenDepth;
                    else if (match(TokenKind::RParen)) --parenDepth;
                    else advance();
                }
                if (parenDepth != 0) {
                    error("unterminated parameter list in function type");
                    return {};
                }
                if (!expect(TokenKind::Arrow, "'=>' after function type parameters")) return {};
                std::string ret = self(self, arrowRet);
                if (ret.empty()) return {};
                return "() => " + ret;
            }

            std::string base;
            // Parenthesized type `(T)` OR Function type `(a: A, b: B) => R`
            if (check(TokenKind::LParen)) {
                // Speculatively check if this is `(params) => ReturnType`
                bool isFnType = false;
                {
                    const size_t sp = pos_, se = tokenEditsLog_.size(), sd = diags_.size(),
                                 st = declaredTypesUndo_.size();
                    ++speculativeDepth_;
                    advance();  // '('
                    // Validate that contents inside `(...)` can be function type parameters:
                    // empty `()`, or parameters separated by commas where each param starts with
                    // `...`, `[`, `{`, `this`, or an identifier/modifier followed by `,`, `)`, `?`, `:`.
                    bool paramsLookValid = true;
                    while (!check(TokenKind::RParen) && !check(TokenKind::EndOfFile)) {
                        while (check(TokenKind::Identifier) &&
                               (peek().text == "public" || peek().text == "private" ||
                                peek().text == "protected" || peek().text == "readonly" ||
                                peek().text == "override") &&
                               peek(1).kind != TokenKind::Colon && peek(1).kind != TokenKind::Question &&
                               peek(1).kind != TokenKind::Comma && peek(1).kind != TokenKind::RParen) {
                            advance();
                        }
                        match(TokenKind::Ellipsis);
                        if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) {
                            int d = 1;
                            advance();
                            while (d > 0 && !check(TokenKind::EndOfFile)) {
                                if (check(TokenKind::LBracket) || check(TokenKind::LBrace)) ++d;
                                else if (check(TokenKind::RBracket) || check(TokenKind::RBrace)) --d;
                                advance();
                            }
                        } else if (isIdentifierName(peek().kind)) {
                            advance();
                        } else {
                            paramsLookValid = false;
                            break;
                        }
                        match(TokenKind::Question);
                        if (match(TokenKind::Colon)) {
                            if (self(self, false).empty()) {
                                paramsLookValid = false;
                                break;
                            }
                        } else if (!check(TokenKind::Comma) && !check(TokenKind::RParen)) {
                            paramsLookValid = false;
                            break;
                        }
                        if (!match(TokenKind::Comma)) break;
                    }
                    if (paramsLookValid && match(TokenKind::RParen) && check(TokenKind::Arrow)) {
                        advance();  // '=>'
                        std::string ret = self(self, false);
                        if (!ret.empty()) {
                            // If we are parsing an arrow function's return type (`arrowRet`),
                            // `(T) => body` uses the `=>` for the arrow body unless a second `=>`
                            // follows `ret`!
                            if (!arrowRet || check(TokenKind::Arrow)) {
                                isFnType = true;
                                base = "() => " + ret;
                            }
                        }
                    }
                    --speculativeDepth_;
                    if (!isFnType) {
                        rollbackFrom(sp, se, sd, st);
                    }
                }
                if (!isFnType) {
                    advance();  // '('
                    std::string inner = self(self, false);
                    if (inner.empty()) return {};
                    if (!expect(TokenKind::RParen, "')' after parenthesized type")) return {};
                    base = "(" + inner + ")";
                }
            } else if (check(TokenKind::LBracket)) {
                // Tuple type: `[number, string?, ...boolean[]]` or `[x: number, y?: string]`
                advance();  // '['
                std::string tuple = "[";
                bool first = true;
                while (!check(TokenKind::RBracket) && !check(TokenKind::EndOfFile)) {
                    if (!first) tuple += ", ";
                    first = false;
                    if (match(TokenKind::Ellipsis)) tuple += "...";
                    // Optional tuple element label `name:` or `name?:`
                    if (isIdentifierName(peek().kind) &&
                        (peek(1).kind == TokenKind::Colon ||
                         (peek(1).kind == TokenKind::Question && peek(2).kind == TokenKind::Colon))) {
                        advance();  // label
                        match(TokenKind::Question);
                        advance();  // ':'
                    }
                    std::string elem = self(self, false);
                    if (elem.empty()) return {};
                    tuple += elem;
                    if (match(TokenKind::Question)) tuple += "?";
                    if (!match(TokenKind::Comma)) break;
                }
                if (!expect(TokenKind::RBracket, "']' after tuple type")) return {};
                tuple += "]";
                base = std::move(tuple);
            } else if (check(TokenKind::LBrace)) {
                // Object / inline interface type literal `{ ... }`
                advance();  // '{'
                int depth = 1;
                while (depth > 0 && !check(TokenKind::EndOfFile)) {
                    if (match(TokenKind::LBrace)) ++depth;
                    else if (match(TokenKind::RBrace)) --depth;
                    else advance();
                }
                if (depth != 0) {
                    error("unterminated object type literal");
                    return {};
                }
                base = "object";
            } else if (match(TokenKind::KwVoid)) {
                base = "void";
            } else if (match(TokenKind::KwNull)) {
                base = "null";
            } else if (match(TokenKind::KwUndefined)) {
                base = "undefined";
            } else if (match(TokenKind::KwTrue)) {
                base = "true";
            } else if (match(TokenKind::KwFalse)) {
                base = "false";
            } else if (match(TokenKind::KwThis)) {
                base = "this";
            } else if (check(TokenKind::StringLiteral)) {
                base = std::string(advance().text);
            } else if (check(TokenKind::NumberLiteral)) {
                base = std::string(advance().text);
            } else if ((check(TokenKind::Minus) || check(TokenKind::Plus)) &&
                       peek(1).kind == TokenKind::NumberLiteral) {
                std::string sign(advance().text);
                base = sign + std::string(advance().text);
            } else if (check(TokenKind::TemplateWhole)) {
                advance();
                base = "string";
            } else if (check(TokenKind::TemplateHead)) {
                advance();  // TemplateHead
                for (;;) {
                    if (self(self, false).empty()) return {};
                    if (match(TokenKind::TemplateMiddle)) continue;
                    if (match(TokenKind::TemplateTail)) break;
                    error("unterminated template literal type");
                    return {};
                }
                base = "string";
            } else if (check(TokenKind::KwImport) && peek(1).kind == TokenKind::LParen) {
                advance();  // 'import'
                advance();  // '('
                if (!expect(TokenKind::StringLiteral, "module specifier in import(...) type")) return {};
                if (!expect(TokenKind::RParen, "')' after import(...) type")) return {};
                base = "import";
                while (match(TokenKind::Dot)) {
                    const Token* prop = expectPropertyName("type name after 'import(...).'");
                    if (!prop) return {};
                    base += "." + std::string(prop->text);
                }
                if (match(TokenKind::Less)) {
                    base += "<";
                    bool firstArg = true;
                    while (!check(TokenKind::EndOfFile)) {
                        if (matchTypeGreater()) break;
                        if (!firstArg) base += ", ";
                        firstArg = false;
                        std::string arg = self(self, false);
                        if (arg.empty()) return {};
                        base += arg;
                        if (match(TokenKind::Comma)) continue;
                        if (!expectTypeGreater("'>' after type arguments")) return {};
                        break;
                    }
                    base += ">";
                }
            } else if (isIdentifierName(peek().kind)) {
                base = std::string(advance().text);
                while (match(TokenKind::Dot)) {
                    const Token* member = expectPropertyName("qualified type name");
                    if (!member) return {};
                    base += "." + std::string(member->text);
                }
                if (match(TokenKind::Less)) {
                    base += "<";
                    bool firstArg = true;
                    while (!check(TokenKind::EndOfFile)) {
                        if (matchTypeGreater()) break;
                        if (!firstArg) base += ", ";
                        firstArg = false;
                        std::string arg = self(self, false);
                        if (arg.empty()) return {};
                        base += arg;
                        if (match(TokenKind::Comma)) continue;
                        if (!expectTypeGreater("'>' after type arguments")) return {};
                        break;
                    }
                    base += ">";
                }
            } else {
                const Token* t = expect(TokenKind::Identifier, "type name");
                if (!t) return {};
                base = std::string(t->text);
            }

            // Postfix `[]` (array type) or `[K]` (indexed access type)
            while (check(TokenKind::LBracket) && !atLineBreak()) {
                advance();  // '['
                if (match(TokenKind::RBracket)) {
                    base += "[]";
                } else {
                    std::string idxType = self(self, false);
                    if (idxType.empty()) return {};
                    if (!expect(TokenKind::RBracket, "']' after indexed access type")) return {};
                    base += "[" + idxType + "]";
                }
            }
            return base;
        };

        auto parseIntersection = [&]() -> std::string {
            match(TokenKind::Amp);  // optional leading `&`
            std::string lhs = parsePrimaryAndPostfix(parsePrimaryAndPostfix);
            if (lhs.empty()) return {};
            while (match(TokenKind::Amp)) {
                std::string rhs = parsePrimaryAndPostfix(parsePrimaryAndPostfix);
                if (rhs.empty()) return {};
                lhs += " & " + rhs;
            }
            return lhs;
        };

        auto parseUnion = [&]() -> std::string {
            match(TokenKind::Pipe);  // optional leading `|`
            std::string lhs = parseIntersection();
            if (lhs.empty()) return {};
            while (match(TokenKind::Pipe)) {
                std::string rhs = parseIntersection();
                if (rhs.empty()) return {};
                lhs += " | " + rhs;
            }
            return lhs;
        };

        std::string result = parseUnion();
        if (result.empty()) return {};

        // Conditional type: `T extends U ? X : Y`
        if (match(TokenKind::KwExtends)) {
            std::string extType = parseUnion();
            if (extType.empty()) return {};
            if (!expect(TokenKind::Question, "'?' in conditional type")) return {};
            std::string trueType = self(self, false);
            if (trueType.empty()) return {};
            if (!expect(TokenKind::Colon, "':' in conditional type")) return {};
            std::string falseType = self(self, arrowRet);
            if (falseType.empty()) return {};
            return result + " extends " + extType + " ? " + trueType + " : " + falseType;
        }

        trimSpaces(result);
        return result;
    };

    return parseTypeInternal(parseTypeInternal, inArrowReturn);
}

bool Parser::tryConsumeTypeArgsForCall() {
    if (!check(TokenKind::Less)) return false;
    const size_t savedPos = pos_;
    const size_t savedEditCount = tokenEditsLog_.size();
    const size_t savedDiagCount = diags_.size();
    const size_t savedDeclaredCount = declaredTypesUndo_.size();

    ++speculativeDepth_;
    advance();  // '<'
    bool ok = false;
    bool sawAny = false;
    while (!check(TokenKind::EndOfFile)) {
        if (sawAny && matchTypeGreater()) {
            ok = true;
            break;
        }
        std::string arg = parseTypeAnnotation(false);
        if (arg.empty() || diags_.size() > savedDiagCount) {
            ok = false;
            break;
        }
        sawAny = true;
        if (match(TokenKind::Comma)) continue;
        if (matchTypeGreater()) {
            ok = true;
        }
        break;
    }
    --speculativeDepth_;

    // Type arguments on a call / new / tagged-template MUST be immediately followed by
    // `(`, TemplateWhole, or TemplateHead (and for `(`, on the same line or valid call).
    const bool followedByCallOrTemplate =
        ok && diags_.size() == savedDiagCount &&
        (check(TokenKind::LParen) || check(TokenKind::TemplateWhole) ||
         check(TokenKind::TemplateHead));

    if (!followedByCallOrTemplate) {
        while (tokenEditsLog_.size() > savedEditCount) {
            const auto& entry = tokenEditsLog_.back();
            tokens_[entry.first] = entry.second;
            tokenEditsLog_.pop_back();
        }
        while (declaredTypesUndo_.size() > savedDeclaredCount) {
            if (currentModule_) currentModule_->declaredTypes.erase(declaredTypesUndo_.back());
            declaredTypesUndo_.pop_back();
        }
        diags_.truncate(savedDiagCount);
        pos_ = savedPos;
        return false;
    }

    if (speculativeDepth_ == 0) {
        tokenEditsLog_.clear();
        declaredTypesUndo_.clear();
    }
    return true;
}

bool Parser::looksLikeArrowFrom(size_t offset) const {
    const TokenKind first = peek(offset).kind;
    if ((first == TokenKind::Identifier || first == TokenKind::KwOf) &&
        peek(offset + 1).kind == TokenKind::Arrow && !peek(offset + 1).newlineBefore) {
        return true;
    }
    // `<T>(x) => x` is a generic arrow only in TypeScript; no JavaScript
    // expression starts with `<`.
    if (first != TokenKind::LParen && !(first == TokenKind::Less && typescript_)) return false;

    auto* self = const_cast<Parser*>(this);
    const size_t savedPos = self->pos_;
    const size_t savedEditCount = self->tokenEditsLog_.size();
    const size_t savedDiagCount = self->diags_.size();
    const size_t savedDeclaredCount = self->declaredTypesUndo_.size();

    self->pos_ = savedPos + offset;
    ++self->speculativeDepth_;

    bool isArrow = false;
    bool ok = true;
    if (self->check(TokenKind::Less)) {
        ok = self->parseTypeParams() && self->diags_.size() == savedDiagCount;
    }
    if (ok && self->match(TokenKind::LParen)) {
        // Track whether parameters have explicit TS/param syntax (empty `()`, `:`, `?`, `=`, `...`, pattern)
        bool hasExplicitParamSyntax = self->check(TokenKind::RParen);
        const size_t paramsStart = self->pos_;
        std::vector<ast::Param> dummyParams;
        if (self->parseParams(dummyParams) && self->diags_.size() == savedDiagCount &&
            self->match(TokenKind::RParen)) {
            for (size_t k = paramsStart; k + 1 < self->pos_; ++k) {
                const TokenKind tk = self->tokens_[k].kind;
                if (tk == TokenKind::Colon || tk == TokenKind::Question ||
                    tk == TokenKind::Assign || tk == TokenKind::Ellipsis ||
                    tk == TokenKind::LBracket || tk == TokenKind::LBrace) {
                    hasExplicitParamSyntax = true;
                    break;
                }
            }
            if (self->check(TokenKind::Arrow) && !self->atLineBreak()) {
                isArrow = true;
            } else if (self->match(TokenKind::Colon)) {
                // In `cond ? (a) : b => c`, `(a)` is a parenthesized identifier in the ternary's
                // then-branch and `:` is the ternary's colon, unless `(a)` has explicit param syntax.
                // In a `.js` file a return type there is never read: every
                // `c ? (…) : …` is a JavaScript conditional, whatever the
                // parentheses hold (`c ? (a = 1) : b => b` included).
                if (!(inConditionalThen_ && first == TokenKind::LParen &&
                      (!hasExplicitParamSyntax || !typescript_))) {
                    std::string ret = self->parseTypeAnnotation(/*inArrowReturn=*/true);
                    if (!ret.empty() && self->diags_.size() == savedDiagCount &&
                        self->check(TokenKind::Arrow) && !self->atLineBreak()) {
                        isArrow = true;
                    }
                }
            }
        }
    }

    --self->speculativeDepth_;
    while (self->tokenEditsLog_.size() > savedEditCount) {
        const auto& entry = self->tokenEditsLog_.back();
        self->tokens_[entry.first] = entry.second;
        self->tokenEditsLog_.pop_back();
    }
    while (self->declaredTypesUndo_.size() > savedDeclaredCount) {
        if (self->currentModule_) {
            self->currentModule_->declaredTypes.erase(self->declaredTypesUndo_.back());
        }
        self->declaredTypesUndo_.pop_back();
    }
    self->diags_.truncate(savedDiagCount);
    self->pos_ = savedPos;
    return isArrow;
}

}  // namespace bronze
