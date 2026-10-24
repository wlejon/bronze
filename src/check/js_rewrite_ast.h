#pragma once

// The parsed program `rewriteJsSource` finds its rewrite sites in (#162).
//
// A site is a node of bronze's own AST, never a span of text that happens to
// look like one, so a string, a comment or a template literal's text can never
// match, and a name is only ever what it RESOLVES to: `Math` is the global only
// when nothing in the program declares or writes it, and a fact about a
// variable holds only for the one `const`/`let` binding it was derived from,
// at a use that binding's initializer is known to have run before. A
// replacement is a splice at the node's extent in the original text; nothing
// outside the extents of the sites changes.
//
// Internal to src/check: js_source_rewriter.cpp drives it, js_rewrite_passes.cpp
// holds the patterns.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/ast.h"
#include "check/js_source_rewriter.h"
#include "lex/token.h"

namespace bronze {
class SourceSet;
}

namespace bronze::check::jsrw {

// A half-open byte range of the source text.
struct Extent {
    uint32_t begin = 0;
    uint32_t end = 0;
};

// Where a node sits: every statement list enclosing it, outermost first, with
// the index of the statement it is in. A Function frame marks a function (or
// class) boundary, which no fact about a binding is carried across.
enum class FrameKind : uint8_t { List, SwitchCase, Function };
struct Frame {
    const void* list = nullptr;
    size_t index = 0;
    FrameKind kind = FrameKind::List;
    const std::string* functionName = nullptr;  // Function frames only
};
using Path = std::vector<Frame>;

// What a numeric expression is known to evaluate to: an integer-valued Number
// in [lo, hi] — never NaN, never -0, never a BigInt, a string or an object.
struct IntRange {
    double lo = 0;
    double hi = 0;
    bool isInt32() const { return lo >= -2147483648.0 && hi <= 2147483647.0; }
};

class ParsedJs {
public:
    // Lexes and parses `text` as a module. Null, with the diagnostics in
    // `error`, when it does not parse.
    static std::unique_ptr<ParsedJs> parse(const std::string& text, std::string* error);
    ~ParsedJs();

    const std::string& text() const { return text_; }
    const ast::Module& module() const { return *module_; }

    // Every node of these kinds in the program, in walk (source) order.
    const std::vector<const ast::Call*>& calls() const { return calls_; }
    const std::vector<const ast::Binary*>& binaries() const { return binaries_; }
    const std::vector<const ast::IfStmt*>& ifs() const { return ifs_; }
    const std::vector<const ast::ReturnStmt*>& returns() const { return returns_; }
    const std::vector<const std::vector<ast::StmtPtr>*>& lists() const { return lists_; }
    const std::vector<const ast::Ident*>& idents() const { return idents_; }

    // The exact source range of an expression, without the parentheses that
    // may group it (`inner`) or with them (`outer`).
    Extent inner(const ast::Expr& e) const;
    Extent outer(const ast::Expr& e) const;
    // A statement's range, through its terminating `;` when it has one. Null
    // for the statement kinds this does not measure.
    std::optional<Extent> statement(const ast::Stmt& s) const;
    // The `else` token's position when an `if` has one (even an empty one).
    bool hasElse(const ast::IfStmt& s) const;

    std::string slice(Extent x) const { return text_.substr(x.begin, x.end - x.begin); }
    std::string exprText(const ast::Expr& e) const { return slice(inner(e)); }
    // The expression's text as an operand of a binary operator: bare when it
    // is a primary expression, parenthesized otherwise.
    std::string operandText(const ast::Expr& e) const;
    // Leading whitespace of the line `offset` is on.
    std::string indentAt(uint32_t offset) const;

    // The token just before `offset`, or null at the start of the text.
    const Token* tokenBefore(uint32_t offset) const;

    // Is `name` the global binding everywhere in this program — declared
    // nowhere (no var/let/const, parameter, function, class, catch or import of
    // that name), assigned nowhere, no property of it written or deleted, and
    // no direct `eval` that could declare one?
    bool isGlobal(const std::string& name) const;
    // Is `name` mentioned nowhere in the program, so a binding of it that a
    // rewrite introduces cannot capture or shadow anything?
    bool isFreshName(const std::string& name) const;
    // How many declarations of `name` the program has, anywhere.
    size_t declarationCount(const std::string& name) const;
    // `Math.<method>(...)` with `Math` the global, no optional link, no spread.
    bool isMathCall(const ast::Call& c, std::string_view method) const;

    // The initializer of the binding `use` refers to, when that binding is a
    // `const`, or a `let` assigned nowhere, declared exactly once in the
    // program, in a statement list `use` sits later in, within the same
    // function and not across a `switch` case: so `use` reads that binding and
    // its initializer has run. Null otherwise.
    const ast::Expr* constInit(const ast::Ident& use) const;
    // What `e` is known to evaluate to (see IntRange), or null.
    std::optional<IntRange> rangeOf(const ast::Expr& e) const;

    // The name of the function `n` is written in, or empty.
    std::string functionNameOf(const ast::Node& n) const;

    // A replacement for `at` that cannot fuse with the token before it: a text
    // starting with `(` after an expression that automatic semicolon insertion
    // ended would otherwise call it.
    std::string asiSafe(Extent at, std::string text) const;

    struct Facts;

private:
    ParsedJs() = default;
    size_t tokenIndexAt(uint32_t begin) const;     // first token beginning at or after
    size_t tokenIndexEndingAt(uint32_t end) const;  // last token ending at or before
    std::optional<size_t> statementLastToken(const ast::Stmt& s) const;
    std::optional<size_t> bodyLastToken(size_t firstToken, const std::vector<ast::StmtPtr>& body) const;
    std::optional<IntRange> rangeOf(const ast::Expr& e, int depth) const;
    const Path* pathOf(const ast::Node& n) const;

    std::string text_;
    std::unique_ptr<SourceSet> sources_;
    std::vector<Token> tokens_;
    std::vector<size_t> match_;  // bracket partner per token, or SIZE_MAX
    std::unique_ptr<ast::Module> module_;
    std::unique_ptr<Facts> facts_;

    std::vector<const ast::Call*> calls_;
    std::vector<const ast::Binary*> binaries_;
    std::vector<const ast::IfStmt*> ifs_;
    std::vector<const ast::ReturnStmt*> returns_;
    std::vector<const std::vector<ast::StmtPtr>*> lists_;
    std::vector<const ast::Ident*> idents_;
};

// One replacement: the text that takes the place of `at`, and the hit it
// reports.
struct Edit {
    Extent at;
    std::string text;
    JsRewritePassHit hit;
};

// Splices `edits` into `text`. Edits that overlap an earlier (or enclosing)
// one are dropped, with their hits; the hits of the applied ones are appended
// to `hits` in source order.
std::string applyEdits(const std::string& text,
                       std::vector<Edit> edits,
                       std::vector<JsRewritePassHit>& hits);

// An edit that removes the statement at `at`: the whole line when the
// statement is alone on it, or an empty statement `;` where removing it would
// leave a braceless `if`/`else`/loop body empty or let two statements fuse.
Edit deletionEdit(const ParsedJs& p, Extent at);

std::string fnv1aHex(std::string_view text);

// The passes. Each appends the edits it finds in `p`; `fallbackFn` names the
// function a site outside any function is reported under.
void collectDeadGuardEdits(const ParsedJs& p, const std::string& fallbackFn, std::vector<Edit>& out);
void collectStrengthEdits(const ParsedJs& p, const std::string& fallbackFn, std::vector<Edit>& out);
void collectHornerEdits(const ParsedJs& p, const std::string& fallbackFn, std::vector<Edit>& out);
void collectLoopEdits(const ParsedJs& p, const std::string& fallbackFn, std::vector<Edit>& out);

}  // namespace bronze::check::jsrw
