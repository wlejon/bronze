#include "il/verifier_proofs.h"

namespace bronze::il {

bool verifyFunctionProofs(const Function& fn, const Module* module, DiagnosticSink& diags) {
    (void)fn;
    (void)module;
    (void)diags;
    return true;
}

bool verifyModuleProofs(const Module& module, DiagnosticSink& diags) {
    for (const Function& fn : module.functions) {
        if (!verifyFunctionProofs(fn, &module, diags)) {
            return false;
        }
    }
    return true;
}

}  // namespace bronze::il
