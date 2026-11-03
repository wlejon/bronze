#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
// The whole-program analysis (src/opt/wpo): the points-to solver, the shape
// pass and the census, on small programs compiled through the real front end.
//
// Most cases are SOUNDNESS cases: each one builds the situation in which a
// fact the analysis could be tempted to report does not hold at run time — an
// object a native received, a prototype method replaced, a key added on one
// path only — and checks that the census does not report it.

#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "../lower/lower_fixture.h"
#include "il/print.h"
#include "il/verifier.h"
#include "opt/wpo/census.h"
#include "opt/wpo/shape.h"
#include "opt/wpo/solver.h"
#include "opt/wpo/stamp.h"
#include "opt/wpo/wpo.h"

using namespace bronze;
using namespace bronze::wpo;

namespace {

struct Analysis {
    std::unique_ptr<il::Module> mod;
    std::unique_ptr<Solver> solver;
    std::unique_ptr<ShapeAnalysis> shapes;
    std::vector<SiteFact> facts;
};

Analysis analyze(const std::string& src, bool predict = false) {
    Analysis a;
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lower_test::inferAndLower(src, diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(mod.has_value());
    a.mod = std::make_unique<il::Module>(std::move(*mod));
    SolverOptions opts;
    opts.predict = predict;
    a.solver = std::make_unique<Solver>(*a.mod, opts);
    a.solver->run();
    a.shapes = std::make_unique<ShapeAnalysis>(*a.solver);
    a.shapes->run();
    a.facts = classifySites(*a.solver, *a.shapes);
    return a;
}

uint32_t fnIndex(const Analysis& a, const std::string& name) {
    for (uint32_t f = 0; f < a.mod->functions.size(); ++f) {
        if (a.mod->functions[f].name == name) return f;
    }
    return UINT32_MAX;
}

// The sites of `op` on `key` in the function named `fn` (every function when
// `fn` is empty).
std::vector<SiteFact> sites(const Analysis& a, const std::string& fn, il::Op op, const std::string& key) {
    std::vector<SiteFact> out;
    for (const auto& f : a.facts) {
        if (!fn.empty() && a.mod->functions[f.fn].name != fn) continue;
        if (f.op != op || a.solver->keys().name(f.key) != key) continue;
        out.push_back(f);
    }
    return out;
}

// The one site of `op` on `key` in `fn`, which the case is written to have.
SiteFact site(const Analysis& a, const std::string& fn, il::Op op, const std::string& key) {
    const auto all = sites(a, fn, op, key);
    REQUIRE_MESSAGE(!all.empty(), "no " << il::opName(op) << " ." << key << " in " << fn);
    return all.front();
}

std::string targetNames(const Analysis& a, const SiteFact& f) {
    std::string out;
    for (uint32_t t : f.targets) out += (out.empty() ? "" : ",") + a.mod->functions[t].name;
    return out;
}

}  // namespace

TEST_CASE("a class's fields have one shape and its method one target") {
    const auto a = analyze(
        "class P {\n"
        "  constructor() { this.x = 1; this.y = 2; }\n"
        "  getY() { return this.y; }\n"
        "}\n"
        "const p = new P();\n"
        "console.log(p.getY());\n");
    const auto get = site(a, "P.getY", il::Op::PropGet, "y");
    CHECK(get.prop == PropClass::SingleShape);
    CHECK(get.slot == 1);
    const auto call = site(a, "main", il::Op::MethodCall, "getY");
    CHECK(call.call == CallClass::Single);
    CHECK(targetNames(a, call) == "P.getY");
}

TEST_CASE("an object passed to an unknown native escapes") {
    const auto a = analyze(
        "function rd(q) { return q.a; }\n"
        "function rb(q) { return q.b; }\n"
        "const o = { a: 1 };\n"
        "someHostFunction(o);\n"
        "console.log(rd(o), rb(o));\n");
    // Once escaped the object is one of the objects the open world stands
    // for: a parameter it flows to holds "unknown", and the census says an
    // escaped object is why.
    const auto get = site(a, "rd", il::Op::PropGet, "a");
    CHECK(get.prop == PropClass::Unknown);
    CHECK(get.cause.find("escaped objects") == 0);
    CHECK(get.slot == -1);
    // A key the program never wrote may have been added by the native.
    const auto other = site(a, "rb", il::Op::PropGet, "b");
    CHECK(other.prop == PropClass::Unknown);
    CHECK(a.solver->returned(fnIndex(a, "rb")).topObj);
}

TEST_CASE("a method called on an unknown receiver runs for every escaped instance") {
    const auto a = analyze(
        "class K { constructor() { this.a = 1; } m() { return this.a; } }\n"
        "hostKeep(new K());\n"
        "const t = hostGet();\n"
        "console.log(t.m());\n");
    // The host may hand the instance back: `t.m()` is K.m on it.
    const uint32_t m = fnIndex(a, "K.m");
    REQUIRE(m != UINT32_MAX);
    CHECK(a.solver->reachable(m));
    const auto get = site(a, "K.m", il::Op::PropGet, "a");
    CHECK(get.prop == PropClass::SameSlotEscaped);
    CHECK(get.slot == 0);
}

TEST_CASE("a callback a native receives is called with unknown arguments") {
    const auto a = analyze(
        "someHostFunction(function cb(p) { return p.q; });\n");
    const uint32_t cb = fnIndex(a, "cb");
    REQUIRE(cb != UINT32_MAX);
    CHECK(a.solver->reachable(cb));
    CHECK(site(a, "cb", il::Op::PropGet, "q").prop == PropClass::Unknown);
}

TEST_CASE("a builtin's callback is called with the array's elements") {
    const auto a = analyze(
        "class E { constructor() { this.v = 1; } }\n"
        "let sum = 0;\n"
        "[new E(), new E()].forEach(function each(e) { sum += e.v; });\n"
        "console.log(sum);\n");
    const auto get = site(a, "each", il::Op::PropGet, "v");
    CHECK(get.prop == PropClass::SingleShape);
    CHECK(get.slot == 0);
}

TEST_CASE("a patched prototype method is one more target") {
    const auto a = analyze(
        "class A { m() { return 1; } }\n"
        "A.prototype.m = function patched() { return 2; };\n"
        "const x = new A();\n"
        "console.log(x.m());\n");
    const auto call = site(a, "main", il::Op::MethodCall, "m");
    CHECK(call.call == CallClass::Multi);
    CHECK(call.targets.size() == 2);
}

TEST_CASE("a key added on one path makes two shapes") {
    const auto a = analyze(
        "function mk(c) {\n"
        "  const o = { x: 1 };\n"
        "  if (c) o.z = 2;\n"
        "  o.y = 3;\n"
        "  return o;\n"
        "}\n"
        "function rx(o) { return o.x; }\n"
        "function ry(o) { return o.y; }\n"
        "function rz(o) { return o.z; }\n"
        "const v = mk(Math.random() > 0.5);\n"
        "console.log(rx(v), ry(v), rz(v));\n");
    const auto x = site(a, "rx", il::Op::PropGet, "x");
    CHECK(x.prop == PropClass::SameSlot);
    CHECK(x.slot == 0);
    // `y` is slot 1 or slot 2 depending on the path: no slot is claimed.
    CHECK(site(a, "ry", il::Op::PropGet, "y").prop == PropClass::NotInPrefix);
    CHECK(site(a, "rz", il::Op::PropGet, "z").prop == PropClass::NotInPrefix);
}

TEST_CASE("subclasses share their base's slots") {
    const auto a = analyze(
        "class B { constructor() { this.a = 1; this.b = 2; } }\n"
        "class C extends B { constructor() { super(); this.c = 3; } }\n"
        "class D extends B { constructor() { super(); this.d = 4; } }\n"
        "function gb(o) { return o.b; }\n"
        "function gc(o) { return o.c; }\n"
        "console.log(gb(new C()), gb(new D()), gc(new C()));\n");
    const auto b = site(a, "gb", il::Op::PropGet, "b");
    CHECK(b.prop == PropClass::SameSlot);
    CHECK(b.slot == 1);
    const auto c = site(a, "gc", il::Op::PropGet, "c");
    CHECK(c.prop == PropClass::SingleShape);
    CHECK(c.slot == 2);
}

TEST_CASE("a method overridden in a subclass resolves per receiver") {
    const auto a = analyze(
        "class B { f() { return 1; } }\n"
        "class C extends B { f() { return 2; } }\n"
        "function callC(o) { return o.f(); }\n"
        "function callAny(o) { return o.f(); }\n"
        "console.log(callC(new C()), callAny(new B()), callAny(new C()));\n");
    const auto c = site(a, "callC", il::Op::MethodCall, "f");
    CHECK(c.call == CallClass::Single);
    CHECK(targetNames(a, c) == "C.f");
    CHECK(site(a, "callAny", il::Op::MethodCall, "f").call == CallClass::Multi);
}

TEST_CASE("an object a closure captures is followed through the environment") {
    const auto a = analyze(
        "const o = { k: 5 };\n"
        "const read = () => o.k;\n"
        "const grow = () => { o.w = 1; };\n"
        "grow();\n"
        "console.log(read());\n");
    const auto k = sites(a, "", il::Op::PropGet, "k");
    REQUIRE_FALSE(k.empty());
    for (const auto& f : k) {
        if (f.prop == PropClass::Unreached) continue;
        // One allocation site, but `w` is added later: same slot, two shapes.
        CHECK(f.prop == PropClass::SameSlot);
        CHECK(f.slot == 0);
    }
    const auto w = sites(a, "", il::Op::PropSet, "w");
    REQUIRE_FALSE(w.empty());
    CHECK(w.front().prop == PropClass::NotInPrefix);
}

TEST_CASE("a setter on the prototype runs for an assignment in the constructor") {
    const auto a = analyze(
        "class S {\n"
        "  constructor() { this.v = 1; this.w = 2; }\n"
        "  set v(x) { this._v = x; }\n"
        "  get v() { return this._v; }\n"
        "}\n"
        "function rw(o) { return o.w; }\n"
        "function rv(o) { return o.v; }\n"
        "console.log(rw(new S()), rv(new S()));\n");
    // `this.v = 1` runs the setter, which adds `_v`; it makes no key `v`. The
    // shape pass does not follow a setter, so it claims no slot for `w`
    // rather than the wrong one.
    const auto w = site(a, "rw", il::Op::PropGet, "w");
    CHECK(w.prop == PropClass::NotInPrefix);
    CHECK(w.slot == -1);
    CHECK(site(a, "rv", il::Op::PropGet, "v").prop == PropClass::Accessor);
    // The getter and the setter were called.
    CHECK(a.solver->reachable(fnIndex(a, "S.set v")));
    CHECK(a.solver->reachable(fnIndex(a, "S.get v")));
}

TEST_CASE("an object stored on the global object escapes") {
    const auto a = analyze(
        "function rd(q) { return q.a; }\n"
        "const o = { a: 1 };\n"
        "globalThis.saved = o;\n"
        "console.log(rd(o));\n");
    const auto get = site(a, "rd", il::Op::PropGet, "a");
    CHECK(get.prop == PropClass::Unknown);
    CHECK(get.cause.find("escaped objects") == 0);
}

TEST_CASE("a value kept in a WeakMap comes back out of it") {
    const auto a = analyze(
        "const props = new WeakMap();\n"
        "function info(k) {\n"
        "  let r = props.get(k);\n"
        "  if (r === undefined) { r = { count: 0 }; props.set(k, r); }\n"
        "  return r;\n"
        "}\n"
        "function bump(k) { const r = info(k); r.count = r.count + 1; return r.count; }\n"
        "console.log(bump({}));\n");
    const auto reads = sites(a, "bump", il::Op::PropGet, "count");
    REQUIRE_FALSE(reads.empty());
    for (const auto& f : reads) CHECK(f.prop == PropClass::SingleShape);
}

TEST_CASE("a key enumerated off an object reads its fields, not its class's methods") {
    const auto a = analyze(
        "class V { constructor() { this.x = 1; } copy(o) { this.x = o.x; return this; } }\n"
        "function cloneAll(src) {\n"
        "  const dst = {};\n"
        "  for (const k in src) dst[k] = src[k];\n"
        "  return dst;\n"
        "}\n"
        "const d = cloneAll({ v: new V() });\n"
        "console.log(d.v.x);\n");
    // `src[k]` is the literal's `v`, not V.prototype.copy: the copied
    // object's values are the instance alone.
    const auto x = sites(a, "main", il::Op::PropGet, "x");
    REQUIRE_FALSE(x.empty());
    CHECK(x.front().receivers == 1);
}

TEST_CASE("a deleted key voids the object's slots") {
    const auto a = analyze(
        "function mk() { const o = { a: 1, b: 2 }; return o; }\n"
        "function rb(o) { return o.b; }\n"
        "const v = mk();\n"
        "delete v.a;\n"
        "console.log(rb(v));\n");
    const auto b = site(a, "rb", il::Op::PropGet, "b");
    CHECK(b.prop != PropClass::SingleShape);
    CHECK(b.prop != PropClass::SameSlot);
    CHECK(b.slot == -1);
}

// ---- predictions (Solver `predict`) and the claims made from them ----

TEST_CASE("a prediction keeps an escaped object's fields; a proof does not") {
    const char* src =
        "class K { constructor() { this.a = 1; this.b = 2; } }\n"
        "function rb(o) { return o.b; }\n"
        "const k = new K();\n"
        "globalThis.kept = k;\n"
        "console.log(rb(k));\n";
    const auto proved = analyze(src);
    const auto pb = site(proved, "rb", il::Op::PropGet, "b");
    CHECK(pb.prop == PropClass::Unknown);
    const auto predicted = analyze(src, /*predict=*/true);
    const auto qb = site(predicted, "rb", il::Op::PropGet, "b");
    CHECK(qb.slot == 1);
    CHECK((qb.prop == PropClass::SingleShape || qb.prop == PropClass::SameSlotEscaped));
}

TEST_CASE("a typeof of a name nothing defines prunes its branch in a prediction only") {
    const char* src =
        "class S { constructor() { this.a = 1; if (typeof __DEV__ !== 'undefined') { __DEV__.see(this); } } }\n"
        "function ra(o) { return o.a; }\n"
        "console.log(ra(new S()));\n";
    const auto proved = analyze(src);
    CHECK(site(proved, "ra", il::Op::PropGet, "a").prop == PropClass::Unknown);
    const auto predicted = analyze(src, /*predict=*/true);
    const auto a = site(predicted, "ra", il::Op::PropGet, "a");
    CHECK(a.prop == PropClass::SingleShape);
    CHECK_FALSE(a.partial);
    // The guarded call is in a block control never reaches.
    CHECK(site(predicted, "S", il::Op::MethodCall, "see").call == CallClass::Unreached);
}

TEST_CASE("a branch on a constant boolean takes one edge in a proof") {
    const auto a = analyze(
        "function f(o, flag) { if (flag) { globalThis.kept = o; } return o.a; }\n"
        "console.log(f({ a: 1 }, false));\n");
    const auto s = site(a, "f", il::Op::PropGet, "a");
    CHECK(s.prop == PropClass::SingleShape);
    CHECK(s.slot == 0);
}

TEST_CASE("an unknown receiver beside known ones still predicts the known ones") {
    const auto a = analyze(
        "class K { constructor() { this.a = 1; } }\n"
        "function ra(o) { return o.a; }\n"
        "globalThis.cb = ra;\n"
        "console.log(ra(new K()));\n",
        /*predict=*/true);
    const auto s = site(a, "ra", il::Op::PropGet, "a");
    CHECK(s.partial);
    CHECK(s.prop == PropClass::SingleShape);
    CHECK(s.slot == 0);
}

TEST_CASE("a predicted single shape is stamped with a cell of its own") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lower_test::inferAndLower(
        "function mk() { return { a: 1, b: 2 }; }\n"
        "function rb(o) { return o.b; }\n"
        "globalThis.cb = rb;\n"
        "console.log(rb(mk()));\n",
        diags, buf);
    REQUIRE(mod.has_value());
    const uint32_t before = mod->staticSiteCount;
    optimizeForBuild(*mod, BuildOptions{});
    bool found = false;
    for (const auto& fn : mod->functions) {
        if (fn.name != "rb") continue;
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op != il::Op::PropGet || mod->keyConstants[inst.keyIndex] != "b") continue;
                found = true;
                CHECK(inst.staticSlot == 1);
                CHECK(inst.staticSlotGuessed);
                CHECK(inst.familyLo == il::Instruction::kNoFamily);
                CHECK(inst.staticCellIndex >= before);
                CHECK(inst.staticCellIndex < mod->staticSiteCount);
            }
        }
    }
    CHECK(found);
    DiagnosticSink vdiags;
    CHECK(il::verify(*mod, vdiags));
}

TEST_CASE("receivers of one class family get a family range that declares the key at the slot") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lower_test::inferAndLower(
        "class A { constructor() { this.a = 1; this.b = 2; } getb() { return this.b; } }\n"
        "class B extends A { constructor() { super(); this.c = 3; } }\n"
        "class C { constructor() { this.b = 5; } }\n"
        "const arr = [new A(), new B()];\n"
        "let sum = 0;\n"
        "for (const x of arr) sum += x.b + x.getb();\n"
        "const other = [new C()];\n"
        "for (const y of other) sum += y.b;\n"
        "console.log(sum);\n",
        diags, buf);
    REQUIRE(mod.has_value());
    BuildOptions opts;
    opts.stats = true;
    optimizeForBuild(*mod, opts);
    DiagnosticSink vdiags;
    CHECK(il::verify(*mod, vdiags));
    if (mod->classFamilies.empty()) return;
    for (const auto& fn : mod->functions) {
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op != il::Op::PropGet || !inst.staticSlotGuessed) continue;
                if (inst.familyLo == il::Instruction::kNoFamily) continue;
                // Every family the guard admits declares the key at the slot.
                for (uint32_t c = inst.familyLo; c <= inst.familyLo + inst.familySpan; ++c) {
                    const auto& fields = mod->classFamilies[c].fields;
                    REQUIRE(inst.staticSlot < fields.size());
                    CHECK(fields[inst.staticSlot].keyIndex == inst.keyIndex);
                }
            }
        }
    }
}

TEST_CASE("a store into a prototype keeps the generic set") {
    // The generic set tells the method caches a prototype changed; a static
    // store would leave `h.m()` calling the method it replaced.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lower_test::inferAndLower(
        "class Host { m(a) { return a; } }\n"
        "const h = new Host();\n"
        "function swap(f) { Host.prototype.m = f; }\n"
        "swap(function (a) { return a + 1; });\n"
        "console.log(h.m(1));\n",
        diags, buf);
    REQUIRE(mod.has_value());
    optimizeForBuild(*mod, BuildOptions{});
    for (const auto& fn : mod->functions) {
        if (fn.name != "swap") continue;
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == il::Op::PropSet && mod->keyConstants[inst.keyIndex] == "m") {
                    CHECK(inst.staticSlot == il::Instruction::kNoStaticSlot);
                }
            }
        }
    }
}

TEST_CASE("a predicted method target is set only for a callee no own slot holds") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lower_test::inferAndLower(
        "class P { m() { return 1; } }\n"
        "function f() { return 2; }\n"
        "function viaProto(o) { return o.m(); }\n"
        "function viaSlot(o) { return o.n(); }\n"
        "globalThis.cb = viaProto;\n"
        "console.log(viaProto(new P()), viaSlot({ n: f }));\n",
        diags, buf);
    REQUIRE(mod.has_value());
    optimizeForBuild(*mod, BuildOptions{});
    auto directOf = [&](const std::string& name) {
        for (const auto& fn : mod->functions) {
            if (fn.name != name) continue;
            for (const auto& block : fn.blocks) {
                for (const auto& inst : block.instructions) {
                    if (inst.op == il::Op::MethodCall) return inst.directTarget;
                }
            }
        }
        return il::Instruction::kNoDirectTarget;
    };
    const uint32_t proto = directOf("viaProto");
    REQUIRE(proto != il::Instruction::kNoDirectTarget);
    CHECK(mod->functions[proto].name == "P.m");
    CHECK(directOf("viaSlot") == il::Instruction::kNoDirectTarget);
}

TEST_CASE("the census does not change what the build does to the module") {
    const char* src =
        "class P { constructor() { this.x = 1; } get() { return this.x; } }\n"
        "console.log(new P().get());\n";
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto plain = lower_test::inferAndLower(src, diags, buf);
    auto counted = lower_test::inferAndLower(src, diags, buf);
    REQUIRE(plain.has_value());
    REQUIRE(counted.has_value());
    BuildOptions quiet;
    CHECK(optimizeForBuild(*plain, quiet).empty());
    BuildOptions opts;
    opts.stats = true;
    opts.perSite = true;
    opts.filter = "P.get";
    const std::string report = optimizeForBuild(*counted, opts);
    CHECK(report.find("wpo: functions") != std::string::npos);
    CHECK(report.find("wpo-pred: functions") != std::string::npos);
    CHECK(report.find("wpo-site P.get") != std::string::npos);
    CHECK(report.find("wpo-stamp: predicted static slots") != std::string::npos);
    CHECK(il::print(*plain) == il::print(*counted));
}
