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

namespace {
void renderOne(std::string& out, const Diagnostic& d, const SourceBuffer& buffer) {
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
}  // namespace

std::string DiagnosticSink::render(const SourceBuffer& buffer) const {
    std::string out;
    for (const auto& d : diags_) renderOne(out, d, buffer);
    return out;
}

std::string DiagnosticSink::render(const SourceSet& sources) const {
    std::string out;
    if (sources.empty()) return out;
    for (const auto& d : diags_) renderOne(out, d, sources.at(d.span.file));
    return out;
}

}  // namespace bronze
