#include "support/diagnostics.h"

namespace bronze {

static const char* severityName(Severity s) {
    switch (s) {
        case Severity::Error: return "error";
        case Severity::Warning: return "warning";
        case Severity::Note: return "note";
    }
    return "unknown";
}

std::string DiagnosticSink::render(const SourceBuffer& buffer) const {
    std::string out;
    for (const auto& d : diags_) {
        const auto lc = buffer.lineCol(d.span.begin);
        out += std::string(buffer.name());
        out += ':';
        out += std::to_string(lc.line);
        out += ':';
        out += std::to_string(lc.column);
        out += ": ";
        out += severityName(d.severity);
        out += ": ";
        out += d.message;
        out += '\n';
    }
    return out;
}

}  // namespace bronze
