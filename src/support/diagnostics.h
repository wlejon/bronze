#pragma once
#include <string>
#include <vector>

#include "support/source.h"

namespace bronze {

enum class Severity { Error, Warning, Note };

struct Diagnostic {
    Severity severity;
    Span span;
    std::string message;
};

// Collects diagnostics for one compilation. Components never print — they
// report here and the driver decides presentation. A component that cannot
// continue reports an error and returns; there are no silent fallbacks.
class DiagnosticSink {
public:
    void error(Span span, std::string message) {
        diags_.push_back({Severity::Error, span, std::move(message)});
    }
    void warning(Span span, std::string message) {
        diags_.push_back({Severity::Warning, span, std::move(message)});
    }

    bool hasErrors() const {
        for (const auto& d : diags_)
            if (d.severity == Severity::Error) return true;
        return false;
    }
    const std::vector<Diagnostic>& all() const { return diags_; }

    // Renders "name:line:col: severity: message" lines against a buffer.
    std::string render(const SourceBuffer& buffer) const;
    // The same, for a build that read more than one file: each span names
    // the buffer its offsets are into (docs/0023 decision 1).
    std::string render(const SourceSet& sources) const;

private:
    std::vector<Diagnostic> diags_;
};

}  // namespace bronze
