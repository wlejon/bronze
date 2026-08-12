// A module specifier to a file on disk. The whole algorithm is "join it to the
// importing file's directory", and everything it deliberately does NOT do is
// the point: no `node_modules` walk, no `exports` map, no conditions, and no
// extension guessing. Each of those is a place to pick a different file than
// the one the program meant, and picking wrong there is not a compile error —
// it is a different program.

#include "modules/modules.h"

namespace bronze::modules {

namespace {
bool isRelative(const std::string& s) {
    return s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0;
}
}  // namespace

bool resolveSpecifier(const std::string& specifier, const std::filesystem::path& importerPath,
                      Span span, DiagnosticSink& diags, std::filesystem::path& out) {
    if (specifier.empty()) {
        diags.error(span, "empty module specifier");
        return false;
    }
    if (!isRelative(specifier)) {
        diags.error(span, "unsupported module specifier \"" + specifier +
                              "\": bronze resolves relative specifiers only ('./x.js', "
                              "'../lib/y.js'); a bare specifier needs a package resolution "
                              "algorithm bronze does not have");
        return false;
    }

    std::error_code ec;
    std::filesystem::path candidate = importerPath.parent_path() / specifier;
    // `weakly_canonical` and not `canonical`: the file may not exist, and the
    // error below has to be able to print the path it looked for. It is also
    // what makes `./a.js` and `./sub/../a.js` one module rather than two —
    // "a file imported twice is instantiated once" is a property of this key.
    std::filesystem::path resolved = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) resolved = candidate;

    if (!std::filesystem::exists(resolved, ec) ||
        std::filesystem::is_directory(resolved, ec)) {
        diags.error(span, "cannot resolve module specifier \"" + specifier + "\" from " +
                              importerPath.string() + ": no such file " + resolved.string() +
                              " (bronze does not guess an extension or a directory index)");
        return false;
    }
    out = std::move(resolved);
    return true;
}

}  // namespace bronze::modules
