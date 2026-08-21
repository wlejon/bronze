#pragma once

#include <map>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "types/literal_scan.h"
#include "types/type.h"

namespace bronze::types {

// What the whole program can put in a property called `x`.
class FieldAudit {
public:
    void scan(const ast::Module& module);
    void observe(const ast::Expr* rhs, Type t);
    bool settle();

    bool numberClean(const std::string& name) const;
    std::string refusalFor(const std::string& name) const;

    const std::map<std::string, uint32_t>& globalRefusals() const { return globalRefusals_; }
    uint32_t locallyCleanCount() const;
    std::vector<std::pair<std::string, std::string>> report() const;

    uint32_t computedSiteCount() const { return static_cast<uint32_t>(computed_.size()); }
    uint32_t computedRefutedCount() const { return computedRefuted_; }
    std::map<std::string, uint32_t> computedKeyTypes() const;
    std::map<std::string, uint32_t> computedReceiverTypes() const;

    uint32_t writeSiteCount() const { return static_cast<uint32_t>(writes_.size()); }
    uint32_t nameCount() const { return static_cast<uint32_t>(names_.size()); }
    uint32_t cleanCount() const;

    void record(const std::string& name, const ast::Expr* rhs);
    void recordComputed(const ast::Expr* receiver, const ast::Expr* key, const ast::Expr* value);
    void recordComputedDelete(const ast::Expr* receiver, const ast::Expr* key);
    void refuse(const std::string& name, std::string why);
    void refuseAll(std::string why);
    void noteNumericKeyWrite() { numericKeyWrite_ = true; }

    struct ResidueSite {
        std::string reason;
        uint32_t count = 0;
        std::string representativeSite;
    };
    std::vector<ResidueSite> residue() const;

private:
    struct Write {
        std::string name;
        const ast::Expr* rhs = nullptr;
    };

    struct Computed {
        const ast::Expr* receiver = nullptr;
        const ast::Expr* key = nullptr;
        const ast::Expr* value = nullptr;  // null for a delete
        bool isDelete = false;
        bool refuted = false;
    };

    Type typeOfExpr(const ast::Expr* e) const;
    size_t refusedCount() const;

    std::vector<Write> writes_;
    std::vector<Computed> computed_;
    uint32_t computedRefuted_ = 0;
    std::map<const ast::Expr*, Type> rhsTypes_;
    std::map<std::string, std::string> names_;
    std::map<std::string, uint32_t> globalRefusals_;
    bool numericKeyWrite_ = false;
};

bool builtinOwnedName(const std::string& name);
bool couldBeNumericKey(const std::string& name);

}  // namespace bronze::types
