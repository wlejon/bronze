// A bare specifier to a file: the `node_modules` walk and the `package.json`
// read.
//
// This is the part of module resolution that can quietly compile a different
// program. Node's real algorithm walks directories, consults a map that can be
// keyed on conditions the resolver picks for itself, and — in its legacy half —
// appends extensions and looks for directory indexes. Every one of those steps
// has more than one answer, and choosing among them wrong produces a program
// that builds and runs and is not the one on disk.
//
// So the shape of this file is: implement the steps that have exactly ONE
// answer, and make every step that has more than one a hard error that names
// the ambiguity. Concretely —
//
//   - the upward walk is implemented, because "nearest `node_modules` wins" is
//     unambiguous and is what every tool agrees on;
//   - `"main"` and the two unconditional spellings of `"exports"` (a string,
//     and a subpath map to strings) are implemented, because each names one
//     file;
//   - a CONDITIONAL exports object is refused by name. Reading one means
//     choosing a condition set — `import`/`require`/`node`/`browser`/`default`
//     and whatever else a package invented — and a half-read map silently
//     resolving to the wrong entry point is exactly the failure above;
//   - a `"exports"` PATTERN (`"./*"`), a fallback ARRAY and a `null` (blocked)
//     target are refused by name for the same reason;
//   - no extension is appended and no directory index is looked for, which is
//     the rule `resolve.cpp` already applies to a relative specifier, and
//     `requireExistingFile` is literally the same function.

#include <string>
#include <vector>

#include <fstream>
#include <sstream>

#include "json/json.h"
#include "modules/resolve.h"

namespace bronze::modules {

namespace {

// package.json is a JSON text and `src/json` reads UTF-16 code units, because
// that is what a JavaScript string is. Two conversions rather than a second
// parser over bytes: the grammar's decisions live in one place, and this file
// only has to carry the text across.
json::Units toUnits(std::string_view utf8) {
    json::Units out;
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0;
        size_t extra = 0;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
        else { out.push_back(0xFFFD); ++i; continue; }
        if (i + extra >= utf8.size()) { out.push_back(0xFFFD); break; }
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok) { out.push_back(0xFFFD); ++i; continue; }
        i += extra + 1;
        if (cp > 0x10FFFF) { out.push_back(0xFFFD); continue; }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

std::string toUtf8(const json::Units& units) {
    std::string out;
    for (size_t i = 0; i < units.size(); ++i) {
        uint32_t cp = units[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 &&
            units[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (units[++i] - 0xDC00);
        }
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

bool readFile(const std::filesystem::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

// A specifier split into the package it names and the path INSIDE that package.
// `subpath` is spelled the way an `"exports"` key is — "." for the package
// itself, "./sub/x.js" for anything below it — so the lookup below is a string
// comparison and never a second parse.
struct Specifier {
    std::string package;
    std::string subpath;
};

bool splitSpecifier(const std::string& specifier, Span span, DiagnosticSink& diags,
                    Specifier& out) {
    size_t cut = specifier.find('/');
    if (specifier[0] == '@') {
        // A scoped name is TWO segments, and `@scope` alone is not a package.
        cut = cut == std::string::npos ? cut : specifier.find('/', cut + 1);
        if (specifier.find('/') == std::string::npos) {
            diags.error(span, "unsupported module specifier \"" + specifier +
                                  "\": a scoped package name needs two segments (\"@scope/name\")");
            return false;
        }
    }
    out.package = cut == std::string::npos ? specifier : specifier.substr(0, cut);
    out.subpath = cut == std::string::npos ? "." : "./" + specifier.substr(cut + 1);
    if (out.package.empty() || out.package == "." || out.package == "..") {
        diags.error(span, "unsupported module specifier \"" + specifier +
                              "\": it names no package");
        return false;
    }
    if (out.subpath == "./") {
        diags.error(span, "unsupported module specifier \"" + specifier +
                              "\": it ends in a slash, which names a directory and not a file");
        return false;
    }
    return true;
}

// `node_modules/<package>` in this directory and then in each of its ancestors,
// nearest first. That order IS the rule — a package installed beside the
// importing file shadows one installed above it — and it is the one step here
// with a single answer, so it is implemented rather than refused.
//
// Ancestors are walked by `parent_path`, not by reading directories, so nothing
// about the answer depends on a filesystem's enumeration order.
bool findPackageDir(const std::string& package, const std::filesystem::path& fromDir,
                    std::filesystem::path& out, std::vector<std::filesystem::path>& searched) {
    std::error_code ec;
    std::filesystem::path dir = fromDir;
    for (;;) {
        std::filesystem::path candidate = dir / "node_modules" / package;
        searched.push_back(candidate);
        if (std::filesystem::is_directory(candidate, ec)) {
            out = candidate;
            return true;
        }
        std::filesystem::path parent = dir.parent_path();
        if (parent == dir || parent.empty()) return false;
        dir = parent;
    }
}

const json::Value* member(const json::Value& object, const std::string& key) {
    for (const auto& m : object.members) {
        if (toUtf8(m.key) == key) return m.value.get();
    }
    return nullptr;
}

std::string kindName(const json::Value& v) {
    switch (v.kind) {
        case json::Value::Kind::Null: return "null";
        case json::Value::Kind::Bool: return "a boolean";
        case json::Value::Kind::Number: return "a number";
        case json::Value::Kind::String: return "a string";
        case json::Value::Kind::Array: return "an array";
        case json::Value::Kind::Object: return "an object";
    }
    return "a value";
}

// The `"exports"` forms bronze reads, and the ones it refuses. `target` is the
// package-relative path the specifier maps to; false means the error is already
// diagnosed.
//
// The classification is Node's own: an object whose keys ALL begin with "." is
// a subpath map, and an object whose keys do not is a conditions object. Mixed
// keys are an error in Node too, and they are the case where guessing which
// half was meant would be least defensible.
bool exportsTarget(const json::Value& exports, const Specifier& spec,
                   const std::filesystem::path& manifest, Span span, DiagnosticSink& diags,
                   std::string& target) {
    const std::string where = "\"exports\" in " + manifest.string();
    if (exports.kind == json::Value::Kind::String) {
        if (spec.subpath != ".") {
            diags.error(span, where + " is a single string, which maps only the package root; " +
                                  "the specifier asks for \"" + spec.subpath + "\"");
            return false;
        }
        target = toUtf8(exports.text);
        return true;
    }
    if (exports.kind != json::Value::Kind::Object) {
        diags.error(span, "unsupported: " + where + " is " + kindName(exports) +
                              "; bronze reads a string or a map of \"./\" subpaths, and refuses "
                              "anything else rather than guess which entry point was meant");
        return false;
    }
    if (exports.members.empty()) {
        diags.error(span, where + " is an empty object, which exposes nothing");
        return false;
    }
    size_t dotted = 0;
    for (const auto& m : exports.members) {
        if (!toUtf8(m.key).empty() && toUtf8(m.key)[0] == '.') ++dotted;
    }
    if (dotted == 0) {
        diags.error(span, "unsupported: " + where +
                              " is a CONDITIONAL exports object (its keys are conditions such as "
                              "\"import\", \"require\" or \"default\", not \"./\" subpaths). "
                              "Reading one means choosing a condition set, and a condition chosen "
                              "differently from the way the package was written resolves to a "
                              "different entry point without any error — so bronze refuses it "
                              "rather than pick. Add a \"main\", or an \"exports\" that maps "
                              "subpaths to single strings.");
        return false;
    }
    if (dotted != exports.members.size()) {
        diags.error(span, where +
                              " mixes \"./\" subpath keys with condition keys, which names two "
                              "different maps at once");
        return false;
    }
    for (const auto& m : exports.members) {
        const std::string key = toUtf8(m.key);
        if (key.find('*') != std::string::npos) {
            diags.error(span, "unsupported: " + where + " has the PATTERN key \"" + key +
                                  "\"; bronze matches exact subpaths only, because a pattern "
                                  "expands to a file name the program never wrote");
            return false;
        }
    }
    const json::Value* entry = member(exports, spec.subpath);
    if (!entry) {
        std::string message = where + " does not map \"" + spec.subpath + "\"; it maps";
        for (size_t i = 0; i < exports.members.size(); ++i) {
            message += (i ? ", " : " ") + toUtf8(exports.members[i].key);
        }
        diags.error(span, message);
        return false;
    }
    if (entry->kind == json::Value::Kind::Object) {
        diags.error(span, "unsupported: " + where + " maps \"" + spec.subpath +
                              "\" to a CONDITIONAL object; bronze refuses to choose a condition "
                              "(see the note on conditional exports — the wrong choice is a "
                              "different program and not an error)");
        return false;
    }
    if (entry->kind == json::Value::Kind::Array) {
        diags.error(span, "unsupported: " + where + " maps \"" + spec.subpath +
                              "\" to a FALLBACK ARRAY; which element applies depends on what the "
                              "resolver can load, which bronze will not decide for it");
        return false;
    }
    if (entry->kind == json::Value::Kind::Null) {
        diags.error(span, where + " maps \"" + spec.subpath +
                              "\" to null, which BLOCKS it: the package says this path is not "
                              "importable");
        return false;
    }
    if (entry->kind != json::Value::Kind::String) {
        diags.error(span, where + " maps \"" + spec.subpath + "\" to " + kindName(*entry) +
                              ", which names no file");
        return false;
    }
    target = toUtf8(entry->text);
    if (target.rfind("./", 0) != 0) {
        diags.error(span, where + " maps \"" + spec.subpath + "\" to \"" + target +
                              "\", which is not a package-relative path (an exports target "
                              "begins with \"./\")");
        return false;
    }
    return true;
}

}  // namespace

bool resolvePackageSpecifier(const std::string& specifier,
                             const std::filesystem::path& importerPath, Span span,
                             DiagnosticSink& diags, std::filesystem::path& out) {
    Specifier spec;
    if (!splitSpecifier(specifier, span, diags, spec)) return false;

    std::vector<std::filesystem::path> searched;
    std::filesystem::path packageDir;
    if (!findPackageDir(spec.package, importerPath.parent_path(), packageDir, searched)) {
        std::string message = "cannot resolve module specifier \"" + specifier + "\" from " +
                              importerPath.string() + ": no package named \"" + spec.package +
                              "\" in any node_modules on the way up. Looked at:";
        for (const auto& path : searched) message += "\n    " + path.string();
        diags.error(span, message);
        return false;
    }

    const std::filesystem::path manifest = packageDir / "package.json";
    std::string text;
    if (!readFile(manifest, text)) {
        diags.error(span, "cannot resolve module specifier \"" + specifier + "\": " +
                              packageDir.string() + " has no readable package.json");
        return false;
    }
    std::string jsonError;
    json::ValuePtr manifestJson = json::parse(toUnits(text), jsonError);
    if (!manifestJson) {
        diags.error(span, manifest.string() + " is not valid JSON: " + jsonError);
        return false;
    }
    if (manifestJson->kind != json::Value::Kind::Object) {
        diags.error(span, manifest.string() + " is " + kindName(*manifestJson) +
                              " rather than an object");
        return false;
    }

    // `"exports"` WINS over `"main"` when it is present, which is the one thing
    // every resolver agrees on: a package that has both means the exports map,
    // and reading `main` instead would reach an entry point the package has
    // deliberately stopped exposing.
    std::string target;
    const json::Value* exports = member(*manifestJson, "exports");
    if (exports) {
        if (!exportsTarget(*exports, spec, manifest, span, diags, target)) return false;
    } else if (const json::Value* main = member(*manifestJson, "main")) {
        if (spec.subpath != ".") {
            // Without `exports` a subpath is a path inside the package, and
            // that IS unambiguous — it is joined below like any other one.
            target = spec.subpath;
        } else if (main->kind != json::Value::Kind::String) {
            diags.error(span, "\"main\" in " + manifest.string() + " is " + kindName(*main) +
                                  ", which names no file");
            return false;
        } else {
            target = toUtf8(main->text);
        }
    } else if (spec.subpath != ".") {
        target = spec.subpath;
    } else {
        diags.error(span, manifest.string() +
                              " has neither \"exports\" nor \"main\", so nothing in it names the "
                              "package's entry point; bronze does not fall back to index.js, "
                              "because that is a file the program never named");
        return false;
    }

    std::error_code ec;
    std::filesystem::path resolved =
        std::filesystem::weakly_canonical(packageDir / target, ec);
    if (ec) resolved = packageDir / target;
    // An exports target that escapes its own package is the package lying about
    // what it contains; `main` is left alone because Node has never constrained
    // it and a package that points outside itself is still naming one file.
    const std::string canonicalPackage =
        std::filesystem::weakly_canonical(packageDir, ec).generic_string();
    if (!ec && exports && resolved.generic_string().rfind(canonicalPackage + "/", 0) != 0) {
        diags.error(span, "\"exports\" in " + manifest.string() + " maps \"" + spec.subpath +
                              "\" to " + resolved.string() +
                              ", which is outside the package directory");
        return false;
    }

    if (!requireExistingFile(resolved,
                             "module specifier \"" + specifier + "\" from " +
                                 importerPath.string() + " (via " + manifest.string() + ")",
                             span, diags)) {
        return false;
    }
    out = std::move(resolved);
    return true;
}

}  // namespace bronze::modules
