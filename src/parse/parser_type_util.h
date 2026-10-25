#pragma once

// Node construction shared by the files that parse types (parser_type*.cpp).

#include <memory>

#include "ast/type.h"
#include "support/source.h"

namespace bronze::type_parse {

// A node of `kind` starting where `start` starts. The parser fills it, then
// closes its span with `finish` once the last token is consumed.
inline std::shared_ptr<ast::TypeNode> newType(ast::TypeNodeKind kind, Span start) {
    auto node = std::make_shared<ast::TypeNode>();
    node->kind = kind;
    node->span = start;
    return node;
}

inline ast::TypeNodePtr finish(std::shared_ptr<ast::TypeNode> node, Span last) {
    node->span.end = last.end;
    return node;
}

}  // namespace bronze::type_parse
