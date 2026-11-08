#pragma once

#include <climits>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "check/prove_equiv.h"
#include "modules/resolve.h"

namespace bronze::check {

enum class DocValType : uint8_t {
    Unknown,
    Number,
    Integer,
    String,
    Boolean,
    Array,
    Object,
    Any
};

const char* docValTypeName(DocValType t);

struct ParamDocMeta {
    std::string name;
    DocValType type = DocValType::Unknown;
    int32_t domainMin = INT32_MIN;
    int32_t domainMax = INT32_MAX;
    bool hasExplicitRange = false;
    bool optional = false;
};

struct FunctionDocMeta {
    std::string name;
    std::vector<ParamDocMeta> params;
    DocValType returnType = DocValType::Unknown;
    int32_t domainMin = INT32_MIN;
    int32_t domainMax = INT32_MAX;
    bool hasExplicitRange = false;
    bool hasReturnMin = false;
    int32_t returnMin = INT32_MIN;
    bool documentedSymmetric = false;
    bool documentedAntisymmetric = false;
    bool documentedCompare = false;
};

struct AuditPropertyCheck {
    std::string functionName;
    std::string propertyKind;      // "domain-safety", "round-trip", "commutativity", "antisymmetry", "compare-antisymmetry", "diff-equiv"
    std::string formalProperty;    // e.g. "forall x in [-2147483648, 2147483647]: decode(encode(x)) === x"
    EquivStatus status = EquivStatus::Unknown;
    ProofBounds bounds;
    std::vector<std::string> paramNames;
    std::vector<int64_t> counterexampleArgs;
    int64_t expectedValue = 0;
    int64_t actualValue = 0;
    bool oldThrows = false;
    bool newThrows = false;
    bool inPreconditionBucket = false;
    std::string detail;
};

struct PackageAuditReport {
    std::string packageName;
    std::string packageVersion;
    std::string entryPath;
    bool isDiffMode = false;
    std::string secondEntryPath;

    uint32_t totalPublicFunctions = 0;
    uint32_t analyzedFunctions = 0;
    uint32_t equivalentCount = 0;
    uint32_t boundedCount = 0;
    uint32_t counterexampleCount = 0;
    uint32_t preconditionBucketCount = 0;
    uint32_t unknownCount = 0;

    std::map<std::string, uint32_t> unknownReasons;
    std::vector<AuditPropertyCheck> checks;

    std::string renderText() const;
    std::string renderJson() const;
};

// Resolves a package directory or JS file path into one or more entry JS file paths,
// and extracts mechanical parameter/return domain metadata from .d.ts and JSDoc.
bool discoverPackageAuditTarget(const std::string& targetPath,
                                std::string& outPkgName,
                                std::string& outPkgVersion,
                                std::vector<std::string>& outEntryFiles,
                                std::map<std::string, FunctionDocMeta>& outDocMeta,
                                std::string& outError);

// Runs the generic mechanical audit on a package directory or JS file.
PackageAuditReport auditPackage(const std::string& targetPath,
                                const std::string& fnFilter = "",
                                const std::vector<modules::ModuleRoot>& moduleRoots = {},
                                const std::string& importMapPath = "");

// Runs differential equivalence audit between two versions of a package or file.
PackageAuditReport auditPackageDiff(const std::string& oldTargetPath,
                                    const std::string& newTargetPath,
                                    const std::string& fnFilter = "",
                                    const std::vector<modules::ModuleRoot>& moduleRoots = {},
                                    const std::string& importMapPath = "");

// CLI entry point for `bronze audit <pkg_dir>` and `bronze audit --diff <old_dir> <new_dir>`.
int runAuditCli(int argc, char** argv,
                std::ostream& out = std::cout,
                std::ostream& err = std::cerr);

}  // namespace bronze::check
