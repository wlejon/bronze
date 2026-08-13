// A module specifier to a file on disk: which KIND of specifier this is, and
// the one file rule all of them share.
//
// There are three kinds and bronze answers each differently. A RELATIVE
// specifier is joined to the importing file's directory, which is the whole
// algorithm. A BARE specifier is a package, and package.cpp walks
// `node_modules` for it. Everything else — an absolute path, a URL, a `node:`
// builtin, a `#imports` entry — is a named error, because bronze has no answer
// for it and a guess would be a different program.
//
// What no kind does is GUESS: no extension is appended and no directory index
// is looked for. Each of those is a place to pick a different file than the one
// the program meant, and picking wrong there is not a compile error — it is a
// different program.

#include <vector>

#include "modules/modules.h"
#include "modules/resolve.h"

namespace bronze::modules {

namespace {

bool isRelative(const std::string& s) {
    return s.rfind("./", 0) == 0 || s.rfind("../", 0) == 0;
}

// A specifier bronze will not read as a package name, each for its own reason.
// Answering with the empty string means "this is an ordinary bare specifier".
const char* unsupportedSpecifierKind(const std::string& s) {
    if (s[0] == '/' || s[0] == '\\') return "an absolute path";
    if (s.size() >= 3 && s[1] == ':' && (s[2] == '/' || s[2] == '\\')) {
        return "an absolute path";  // a Windows drive letter
    }
    if (s.rfind("#", 0) == 0) {
        return "a `#` import, which names an entry of the package's own \"imports\" map";
    }
    if (s.find("://") != std::string::npos) return "a URL";
    // A scheme with no `//` after it: `node:fs`, `data:text/javascript,...`.
    // Tested before the package split so that `node:fs` is refused as the
    // builtin it is rather than looked for as a package called `node:fs`.
    const size_t colon = s.find(':');
    if (colon != std::string::npos && colon > 0 && s.find('/') > colon) {
        return "a URL scheme (bronze has no builtin modules and no `data:` evaluator)";
    }
    return nullptr;
}

// The paths a resolver that DID guess would have reached. Ordered, and the
// order is the message's: a fixed list, never a directory listing, so two runs
// on one tree say the same thing.
std::vector<std::filesystem::path> nearMisses(const std::filesystem::path& path) {
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    const std::filesystem::path candidates[] = {
        std::filesystem::path(path).concat(".js"),
        std::filesystem::path(path).concat(".mjs"),
        path / "index.js",
        path / "index.mjs",
    };
    for (const auto& candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec)) found.push_back(candidate);
    }
    return found;
}

}  // namespace

bool requireExistingFile(const std::filesystem::path& path, const std::string& what, Span span,
                         DiagnosticSink& diags) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) return true;

    std::string message = "cannot resolve " + what + ": " + path.string() + " is no file";
    const std::vector<std::filesystem::path> misses = nearMisses(path);
    if (misses.empty()) {
        message += " (bronze does not guess an extension or a directory index)";
    } else if (misses.size() == 1) {
        message += ", and bronze does not guess an extension or a directory index — name " +
                   misses[0].string() + " exactly";
    } else {
        // The ambiguity itself. A precedence rule would pick one of these and
        // compile a program that is not the one on disk, which is the failure
        // this whole file exists to refuse.
        message += ", and it is ambiguous which file was meant:";
        for (size_t i = 0; i < misses.size(); ++i) {
            message += (i ? ", " : " ") + misses[i].string();
        }
        message += " all exist; bronze picks none of them — name one exactly";
    }
    diags.error(span, message);
    return false;
}

bool resolveSpecifier(const std::string& specifier, const std::filesystem::path& importerPath,
                      Span span, DiagnosticSink& diags, std::filesystem::path& out) {
    if (specifier.empty()) {
        diags.error(span, "empty module specifier");
        return false;
    }
    if (!isRelative(specifier)) {
        if (const char* kind = unsupportedSpecifierKind(specifier)) {
            diags.error(span, "unsupported module specifier \"" + specifier + "\": it is " +
                                  kind + ", which bronze does not resolve");
            return false;
        }
        return resolvePackageSpecifier(specifier, importerPath, span, diags, out);
    }

    std::error_code ec;
    std::filesystem::path candidate = importerPath.parent_path() / specifier;
    // `weakly_canonical` and not `canonical`: the file may not exist, and the
    // error below has to be able to print the path it looked for. It is also
    // what makes `./a.js` and `./sub/../a.js` one module rather than two —
    // "a file imported twice is instantiated once" is a property of this key.
    std::filesystem::path resolved = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) resolved = candidate;

    if (!requireExistingFile(resolved, "module specifier \"" + specifier + "\" from " +
                                           importerPath.string(),
                             span, diags)) {
        return false;
    }
    out = std::move(resolved);
    return true;
}

}  // namespace bronze::modules
