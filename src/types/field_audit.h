#pragma once

#include <map>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "types/literal_scan.h"
#include "types/type.h"

namespace bronze::types {

class ClassLayoutTable;

// What the whole program can put in a property called `x`.
class FieldAudit {
public:
    // The class table the RECEIVER-SCOPED refusals are measured against; see
    // `settle`. Bound once, before `scan`, and never rebuilt: the audit is read
    // on every round of the fixpoint and a table that filled in as it went
    // would answer differently on the probe rounds and the recording round.
    void bindClasses(const ClassLayoutTable& classes) { classes_ = &classes; }

    void scan(const ast::Module& module);
    void observe(const ast::Expr* rhs, Type t);
    bool settle();

    // Whether the NAME is clean everywhere, ignoring the receiver-scoped
    // refusals. The reporting question ("which names did the audit certify at
    // all"), never the one a read site asks — a read knows which class it is
    // reading, and `numberCleanFor` is the question that takes that in.
    bool numberClean(const std::string& name) const;
    // Whether a read of `name` off an instance of `cls` may spend a Number
    // claim: the name is clean AND no computed write reached this class.
    bool numberCleanFor(ShapeClassId cls, const std::string& name) const;
    std::string refusalFor(const std::string& name) const;

    const std::map<std::string, uint32_t>& globalRefusals() const { return globalRefusals_; }
    // shape class -> why every field of it was refused. See `settle`.
    const std::map<ShapeClassId, std::string>& classRefusals() const { return classRefusals_; }
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

    // How far a refuted computed site's refusal reaches.
    enum class Reach : uint8_t {
        None,     // not refuted (yet)
        Class,    // one shape class and its `extends` family
        Program,  // every name in the program
    };

    struct Computed {
        const ast::Expr* receiver = nullptr;
        const ast::Expr* key = nullptr;
        const ast::Expr* value = nullptr;  // null for a delete
        bool isDelete = false;
        bool refuted = false;
        Reach reach = Reach::None;
    };

    Type typeOfExpr(const ast::Expr* e) const;
    size_t refusedCount() const;
    // Records a refusal covering every field of `cls` and of every class its
    // `extends` family reaches. False when the class carries no bound the
    // refusal could be measured against, and the caller must go program-wide.
    bool refuseClass(ShapeClassId cls, std::string why);

    const ClassLayoutTable* classes_ = nullptr;
    std::vector<Write> writes_;
    std::vector<Computed> computed_;
    uint32_t computedRefuted_ = 0;
    std::map<const ast::Expr*, Type> rhsTypes_;
    std::map<std::string, std::string> names_;
    std::map<std::string, uint32_t> globalRefusals_;
    std::map<ShapeClassId, std::string> classRefusals_;
    bool numericKeyWrite_ = false;
};

bool builtinOwnedName(const std::string& name);
bool couldBeNumericKey(const std::string& name);

}  // namespace bronze::types
