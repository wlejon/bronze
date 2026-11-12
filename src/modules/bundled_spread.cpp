// Object spread and object rest, restored from esbuild's lowering.
//
// A bundle built for a target older than ES2018 carries no `{ ...a, ...b }`
// and no `const { x, ...rest } = o`. esbuild writes the first as
// `__spreadValues(__spreadValues({}, a), b)` and the second as
// `__objRest(o, ["x"])`, over helpers it emits into the file, and a helper
// copies one key at a time: a `for-in` step, a `hasOwnProperty.call`, an `in`
// test and a computed write, each of them a runtime call. The syntax they
// stand for is one walk of the source's own keys (rt_spread.cpp).
//
// So where a file's helpers are EXACTLY esbuild's, the calls are turned back:
//   - `__spreadValues(<object literal>, b)` becomes the literal with `...b`
//     appended (an inner `__spreadValues` call has already become a literal by
//     the time the outer one is seen);
//   - `__objRest(o, [<string literals>])` is marked `Call::isObjectRest`, the
//     one form with no expression syntax to restore.
//
// "Exactly" is decided by text, not by name: each helper and each builtin it
// holds in a variable is compared, as a canonical AST dump, with the same
// statement of `kHelperSource` parsed by the real parser. Rollup renames a
// helper per file it merged (`__spreadValues$10`, `__objRest$m`), which is the one difference
// allowed. A helper also has to be declared once at the top level, never
// rebound, not referenced above its declaration (a call that ran first would
// have thrown), and `Object` has to be the global.
//
// What the helpers do that the syntax does not, and is given up: a Proxy
// source sees CopyDataProperties' traps instead of for-in's, and a rest copy
// DEFINES each key where the helper assigned it, so an own `__proto__` key of
// the source becomes an own property of the rest object rather than its
// prototype. Both are what the program meant before it was bundled.

#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/assigned.h"
#include "ast/clone.h"
#include "ast/dump.h"
#include "ast/queries.h"
#include "lex/lexer.h"
#include "modules/graph.h"
#include "parse/parser.h"

namespace bronze::modules {

namespace {

constexpr const char* kHelperSource = R"js(
var __defProp = Object.defineProperty;
var __getOwnPropSymbols = Object.getOwnPropertySymbols;
var __hasOwnProp = Object.prototype.hasOwnProperty;
var __propIsEnum = Object.prototype.propertyIsEnumerable;
var __defNormalProp = (obj, key, value) => key in obj ? __defProp(obj, key, { enumerable: true, configurable: true, writable: true, value }) : obj[key] = value;
var __spreadValues = (a, b) => {
  for (var prop in b || (b = {}))
    if (__hasOwnProp.call(b, prop))
      __defNormalProp(a, prop, b[prop]);
  if (__getOwnPropSymbols)
    for (var prop of __getOwnPropSymbols(b)) {
      if (__propIsEnum.call(b, prop))
        __defNormalProp(a, prop, b[prop]);
    }
  return a;
};
var __objRest = (source, exclude) => {
  var target = {};
  for (var prop in source)
    if (__hasOwnProp.call(source, prop) && exclude.indexOf(prop) < 0)
      target[prop] = source[prop];
  if (source != null && __getOwnPropSymbols)
    for (var prop of __getOwnPropSymbols(source)) {
      if (exclude.indexOf(prop) < 0 && __propIsEnum.call(source, prop))
        target[prop] = source[prop];
    }
  return target;
};
)js";

constexpr const char* kSpreadRole = "__spreadValues";
constexpr const char* kRestRole = "__objRest";

// The helper a name stands for: itself, less the `$10` or `$m` Rollup appends
// to tell one merged file's copy from another's.
std::string roleOf(const std::string& name) {
    const size_t dollar = name.rfind('$');
    if (dollar == std::string::npos || dollar + 1 == name.size()) return name;
    for (size_t i = dollar + 1; i < name.size(); ++i) {
        const char c = name[i];
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        if (!alnum) return name;
    }
    return name.substr(0, dollar);
}

// One statement's canonical text, with `renames` applied to its module-scope
// names. Empty when the statement cannot be walked.
std::string statementText(const ast::Stmt& stmt, const std::map<std::string, std::string>& renames) {
    ast::Module one;
    one.body.push_back(ast::cloneStmt(stmt));
    // A helper reads the same in a module and in a script: the one thing its
    // mode decides is whether a write to a non-extensible target throws, and
    // the targets the calls below are restored for are fresh literals.
    if (auto* vd = dynamic_cast<ast::VarDecl*>(one.body[0].get())) {
        if (auto* fn = dynamic_cast<ast::FunctionExpr*>(vd->init.get())) fn->strict = false;
    }
    DiagnosticSink scratch;
    if (!renameModuleScope(one.body, renames, 0, {}, scratch)) return {};
    return ast::dump(one);
}

// Role -> the text of esbuild's statement declaring it.
std::map<std::string, std::string> helperTexts() {
    std::map<std::string, std::string> texts;
    SourceBuffer buffer("<bundler helpers>", kHelperSource);
    DiagnosticSink scratch;
    auto tokens = Lexer(buffer, scratch).lex();
    auto parsed = Parser(std::move(tokens), scratch).parseModule("<bundler helpers>");
    if (!parsed || scratch.hasErrors()) return texts;
    for (const auto& stmt : parsed->body) {
        const auto* vd = dynamic_cast<const ast::VarDecl*>(stmt.get());
        if (vd && !vd->pattern) texts[vd->name] = statementText(*stmt, {});
    }
    return texts;
}

const ast::VarDecl* helperCandidate(const ast::Stmt* stmt) {
    const auto* vd = dynamic_cast<const ast::VarDecl*>(stmt);
    if (!vd || vd->pattern || !vd->init || !vd->isVar) return nullptr;
    if (vd->name.rfind("__", 0) != 0) return nullptr;
    return vd;
}

// `__proto__: v` in a literal sets the prototype (B.3.1) instead of defining
// a key, and the helper's `in` test then asks an object the copy would not.
bool setsPrototype(const ast::ObjectProp& prop) {
    return prop.key == "__proto__" && !prop.computed() && !prop.shorthand && !prop.isMethod &&
           prop.accessor == ast::AccessorKind::None &&
           !dynamic_cast<const ast::SpreadElement*>(prop.value.get());
}

struct Helpers {
    std::set<std::string> spread;
    std::set<std::string> rest;
};

ast::ExprPtr restoreCall(const Helpers& helpers, const std::string& name, ast::Call& call) {
    if (call.optional || call.args.size() != 2) return nullptr;
    for (const auto& arg : call.args) {
        if (!arg || dynamic_cast<const ast::SpreadElement*>(arg.get())) return nullptr;
    }
    if (helpers.spread.count(name)) {
        auto* lit = dynamic_cast<ast::ObjectLit*>(call.args[0].get());
        if (!lit || lit->isModuleNamespace) return nullptr;
        for (const auto& prop : lit->props) {
            if (prop.coverInitialized || setsPrototype(prop)) return nullptr;
        }
        auto spread = std::make_unique<ast::SpreadElement>();
        spread->span = call.args[1]->span;
        spread->argument = std::move(call.args[1]);
        ast::ObjectProp prop;
        prop.value = std::move(spread);
        lit->props.push_back(std::move(prop));
        return std::move(call.args[0]);
    }
    if (helpers.rest.count(name)) {
        const auto* keys = dynamic_cast<const ast::ArrayLit*>(call.args[1].get());
        if (!keys) return nullptr;
        for (const auto& key : keys->elements) {
            if (!dynamic_cast<const ast::StringLit*>(key.get())) return nullptr;
        }
        call.isObjectRest = true;
    }
    return nullptr;
}

}  // namespace

bool restoreBundledObjectSpread(ModuleFile& file, DiagnosticSink& diags) {
    auto& body = file.ast->body;

    // Cheap first: most files hold no helper at all.
    bool any = false;
    for (const auto& stmt : body) {
        const auto* vd = helperCandidate(stmt.get());
        if (!vd) continue;
        const std::string role = roleOf(vd->name);
        if (role == kSpreadRole || role == kRestRole) any = true;
    }
    if (!any) return true;

    const std::map<std::string, std::string> texts = helperTexts();
    if (texts.empty()) return true;

    // Every top-level declaration of a name, and every `var` a nested
    // statement hoists up beside them: a helper name has to have one.
    std::map<std::string, size_t> declared;
    for (const auto& stmt : body) {
        if (const auto* vd = dynamic_cast<const ast::VarDecl*>(stmt.get())) {
            if (vd->pattern) {
                for (const auto& n : ast::patternBoundNames(*vd->pattern)) ++declared[n];
            } else {
                ++declared[vd->name];
            }
            continue;
        }
        if (const auto* imp = dynamic_cast<const ast::ImportDecl*>(stmt.get())) {
            for (const auto& spec : imp->specifiers) ++declared[spec.local];
            continue;
        }
        std::vector<const ast::Stmt*> one{stmt.get()};
        for (const auto& n : ast::getScopeDeclarations(one)) ++declared[n];
        for (const auto& n : ast::getHoistedVarDeclarations(one)) ++declared[n];
    }
    if (declared.count("Object")) return true;
    const std::unordered_set<std::string> rebound = ast::getDeeplyReboundNames(body);

    // A statement that is esbuild's, by text, once every candidate name is
    // read as its role.
    std::map<std::string, std::string> roles;
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < body.size(); ++i) {
        const auto* vd = helperCandidate(body[i].get());
        if (!vd || !texts.count(roleOf(vd->name))) continue;
        if (declared[vd->name] != 1 || rebound.contains(vd->name)) continue;
        roles[vd->name] = roleOf(vd->name);
        at[vd->name] = i;
    }
    std::set<std::string> exact;
    for (const auto& [name, role] : roles) {
        if (statementText(*body[at[name]], roles) == texts.at(role)) exact.insert(name);
    }

    // A helper stands only on helpers that stand: each name it reads that is
    // one of the roles has to be an exact statement above it. In body order,
    // so what a helper needs has been decided when it is reached. And it must
    // not have been named above its own declaration.
    std::set<std::string> good;
    Helpers helpers;
    std::unordered_set<std::string> prior;
    for (size_t i = 0; i < body.size(); ++i) {
        std::vector<ast::StmtPtr> single;
        single.push_back(std::move(body[i]));
        const std::unordered_set<std::string> refs = ast::getReferencedNames(single);
        body[i] = std::move(single[0]);

        const auto* vd = helperCandidate(body[i].get());
        if (vd && exact.count(vd->name) && at[vd->name] == i && !prior.contains(vd->name)) {
            bool stands = true;
            for (const auto& ref : refs) {
                if (ref == vd->name) continue;
                if (texts.count(roleOf(ref)) && !good.count(ref)) stands = false;
            }
            if (stands) {
                good.insert(vd->name);
                if (roles[vd->name] == kSpreadRole) helpers.spread.insert(vd->name);
                if (roles[vd->name] == kRestRole) helpers.rest.insert(vd->name);
            }
        }
        for (const auto& ref : refs) prior.insert(ref);
    }
    if (helpers.spread.empty() && helpers.rest.empty()) return true;

    const ModuleCallRewrite calls = [&helpers](const std::string& name, ast::Call& call) {
        return restoreCall(helpers, name, call);
    };
    return renameModuleScope(body, {}, file.id, {}, diags, nullptr, nullptr, &calls);
}

}  // namespace bronze::modules
