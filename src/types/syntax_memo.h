#pragma once

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ast/assigned.h"
#include "ast/ast.h"
#include "ast/queries.h"

namespace bronze::types {

// The syntactic questions the flow analysis asks of a statement list or a
// statement, answered once per compile.
//
// Every round of the module fixpoint re-walks every body, and a nested body is
// re-walked inside every walk of its parent: asking the AST again each time
// made these scans a tenth of inference. The answers are facts about program
// text, so the first one is the one every later walk would compute.
//
// A statement list is keyed by its first statement and its length: a statement
// belongs to exactly one list, and a list's length tells a borrowed prefix (a
// module's top level split from its function declarations) from the whole.
class SyntaxMemo {
public:
    // The names a body keeps in environment cells rather than flow bindings:
    // those a nested function captures, and those assigned inside a `try`.
    const std::set<std::string>& cellNames(const std::vector<const ast::Stmt*>& body) {
        if (body.empty()) return empty_;
        auto [it, fresh] = cellNames_.try_emplace(keyOf(body));
        if (fresh) {
            const auto captured = ast::getCapturedNames(body);
            it->second.insert(captured.begin(), captured.end());
            const auto tryAssigned = ast::getTryAssignedNames(body);
            it->second.insert(tryAssigned.begin(), tryAssigned.end());
        }
        return it->second;
    }

    template <typename List>
    const std::vector<std::string>& scopeDeclarations(const List& stmts) {
        if (stmts.empty()) return emptyList_;
        auto [it, fresh] = scopeDecls_.try_emplace(keyOf(stmts));
        if (fresh) it->second = ast::getScopeDeclarations(stmts);
        return it->second;
    }

    const std::vector<std::string>& hoistedVars(const std::vector<const ast::Stmt*>& body) {
        if (body.empty()) return emptyList_;
        auto [it, fresh] = hoistedVars_.try_emplace(keyOf(body));
        if (fresh) it->second = ast::getHoistedVarDeclarations(body);
        return it->second;
    }

    const std::unordered_set<std::string>& assignedNames(const ast::Node& node) {
        auto [it, fresh] = assigned_.try_emplace(&node);
        if (fresh) it->second = ast::getAssignedNames(node);
        return it->second;
    }

private:
    using Key = std::pair<const ast::Stmt*, size_t>;
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return std::hash<const void*>()(k.first) ^ (k.second * 0x9E3779B97F4A7C15ull);
        }
    };
    static const ast::Stmt* first(const ast::StmtPtr& s) { return s.get(); }
    static const ast::Stmt* first(const ast::Stmt* s) { return s; }
    template <typename List>
    static Key keyOf(const List& stmts) {
        return {first(stmts.front()), stmts.size()};
    }

    const std::set<std::string> empty_;
    const std::vector<std::string> emptyList_;
    // Node-based maps: the answers are handed out by reference and must not
    // move when another is added.
    std::unordered_map<Key, std::set<std::string>, KeyHash> cellNames_;
    std::unordered_map<Key, std::vector<std::string>, KeyHash> scopeDecls_;
    std::unordered_map<Key, std::vector<std::string>, KeyHash> hoistedVars_;
    std::unordered_map<const ast::Node*, std::unordered_set<std::string>> assigned_;
};

}  // namespace bronze::types
