#include "il_translator.h"
#include "il_lowering.h"

namespace il2mir {

TranslationResult translate_bronze_ast(
    const BronzeModuleAST& ast,
    const TranslatorOptions& options,
    DiagnosticReporter* diag
) {
    TranslationResult result;
    DiagnosticReporter default_diag;
    DiagnosticReporter* active_diag = diag ? diag : &default_diag;

    IlLowering lowering(options, active_diag);
    auto mod = lowering.lower_module(ast);
    if (!mod) {
        result.success = false;
        result.error_message = active_diag->has_errors() ? active_diag->format_all() : "Failed to lower Bronze IL AST to MIR";
        return result;
    }

    result.success = true;
    result.module = std::move(mod);
    return result;
}

} // namespace il2mir
