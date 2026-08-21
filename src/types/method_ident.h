#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "types/class_layout.h"
#include "types/literal_scan.h"
#include "types/result.h"
#include "types/type.h"

namespace bronze::types {

inline constexpr uint32_t kNoMethod = 0xFFFFFFFFu;

// One class method as the interprocedural fixpoint sees it.
struct MethodInfo {
    const ast::FunctionExpr* fn = nullptr;
    std::string className;
    std::string methodName;
    // No rest or destructured parameter: the value bound is the value
    // passed, position by position, which is what a joined signature IS.
    // Defaults are allowed (mirroring ctor_ident).
    bool plainParams = true;
    // The current estimate, `Never` per parameter until a call site is seen and
    // only ever widening.
    Signature signature;
    std::vector<Type> observedParams;
    Type observedReturn = Type::never();
    std::vector<bool> hasDefault;
    std::vector<std::string> safeParamNames;
    bool unreached = false;
};

// Forward declaration
struct MethodPoison;

// Every `class` method in the program, and the `extends` forest that decides
// which of them a call on a receiver of a given class can reach.
class MethodTable {
public:
    void build(const ast::Module& module);

    uint32_t indexOfNode(const ast::FunctionExpr* fn) const;
    bool isMethodName(const std::string& name) const;
    void reachableFrom(const std::string& className, const std::string& methodName,
                       std::vector<uint32_t>& out) const;
    const std::vector<uint32_t>* declarationsOf(const std::string& name) const;
    void subtreeOf(const std::string& className, std::vector<uint32_t>& out) const;
    void ancestorsOf(const std::string& className, std::vector<std::string>& out) const;

    std::vector<MethodInfo>& methods() { return methods_; }
    const std::vector<MethodInfo>& methods() const { return methods_; }

    // Parameter names the field-type harvest may answer for:
    // className -> methodName -> paramName -> Type
    std::map<std::string, std::map<std::string, std::map<std::string, Type>>> harvestOracle() const;

    const std::set<std::string>& duplicateNames() const { return duplicates_; }

private:
    struct ClassNode {
        std::string superName;
        std::map<std::string, uint32_t> ownMethods;  // name -> method index
        std::vector<std::string> children;
    };

    std::vector<MethodInfo> methods_;
    std::map<const ast::FunctionExpr*, uint32_t> byNode_;
    std::map<std::string, std::vector<uint32_t>> byName_;
    std::map<std::string, ClassNode> classes_;
    std::set<std::string> duplicates_;
};

// Why a method's parameters cannot carry an identity or primitive type.
// Per-method sticky poison mirroring CtorPoison.
struct MethodPoison {
    // Method index -> reason
    std::map<uint32_t, std::string> byMethod;
    bool all = false;
    std::string allReason;
    uint32_t unboundedCalls = 0;

    bool poisons(uint32_t methodIndex) const {
        return all || byMethod.find(methodIndex) != byMethod.end();
    }
    size_t version() const { return byMethod.size() + (all ? 1u : 0u); }
    const std::string& reasonFor(uint32_t methodIndex) const;
    void add(uint32_t methodIndex, const std::string& reason);
    void addDeclarations(const MethodTable& table, const std::string& methodName,
                         const std::string& reason);
    void addSubtree(const MethodTable& table, const std::string& className,
                    const std::string& methodName, const std::string& reason);
    void addClassMethods(const MethodTable& table, const std::string& className,
                         const std::string& reason);
    void addAll(const std::string& reason);
};

void scanMethodEscapes(const ast::Module& module, const MethodTable& table,
                       MethodPoison& out);

}  // namespace bronze::types
