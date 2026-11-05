#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
// The type-feedback profile (src/opt/spec/profile.h) and the site keys it is
// read by (src/il/feedback_site.h): the reader's round trip and refusals, and
// the property the keys exist for — a function that did not change keeps its
// key when the program around it does, and one that did change loses it.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "../lower/lower_fixture.h"
#include "il/feedback_site.h"
#include "il/print.h"
#include "il/verifier.h"
#include "lower/lower_feedback.h"
#include "opt/spec/profile.h"

using namespace bronze;

namespace {

const char* const kProfile =
    "bronze-type-feedback 1\n"
    "fn\tVector3.add#00112233aabbccdd\t40\n"
    "site\t0\tentry\t40\t\n"
    "site\t1\tthis\t40\tobject=40\n"
    "site\t2\tparam\t40\tundefined=2 object=38\n"
    "site\t3\tshape\t40\tobject=40\n"
    "shape\t3\t40\tVector3{x,y,z}\n"
    "site\t4\tvalue\t80\tf64=60 i32=20\n"
    "fn\tmain.seg1\\tx#ffffffffffffffff\t0\n"
    "site\t0\tmethod\t7\tobject=7\n"
    "shape\t0\t5\tMesh{uuid,name}\n"
    "shape\t0\t2\t*other*\n"
    "target\t0\t7\tupdateMatrix@0badf00d\n"
    "site\t1\ttarget\t3\tfunction=3\n"
    "target\t1\t3\tnative:bound f\n";

// The keys of the module's functions whose IL name ends with `suffix`.
std::vector<std::string> keysEndingIn(const il::Module& mod, const std::string& suffix) {
    const std::vector<std::string> keys = il::feedbackFunctionKeys(mod);
    std::vector<std::string> out;
    for (size_t i = 0; i < mod.functions.size(); ++i) {
        const std::string& n = mod.functions[i].name;
        if (n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
            out.push_back(keys[i]);
        }
    }
    return out;
}

il::Module lowered(const std::string& src) {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto mod = lower_test::inferAndLower(src, diags, buf);
    REQUIRE(mod.has_value());
    return std::move(*mod);
}

}  // namespace

TEST_CASE("profile: parse and serialize round trip byte for byte") {
    std::string err;
    auto profile = spec::TypeProfile::parse(kProfile, &err);
    REQUIRE_MESSAGE(profile.has_value(), err);
    CHECK(profile->serialize() == kProfile);
    CHECK(profile->functions().size() == 2);
}

TEST_CASE("profile: queries by function key and ordinal") {
    auto profile = spec::TypeProfile::parse(kProfile, nullptr);
    REQUIRE(profile.has_value());
    const std::string vec = "Vector3.add#00112233aabbccdd";
    CHECK(profile->entryCount(vec) == 40);
    CHECK(profile->isHot(vec, 10));
    CHECK_FALSE(profile->isHot(vec, 41));
    CHECK(profile->entryCount("nope#0") == 0);
    CHECK_FALSE(profile->isHot("nope#0", 0));

    const spec::SiteFeedback* shape = profile->site(vec, 3);
    REQUIRE(shape != nullptr);
    CHECK(shape->kind == il::FeedbackKind::Shape);
    CHECK(shape->monomorphic());
    CHECK(shape->soleShape() == "Vector3{x,y,z}");

    const spec::SiteFeedback* param = profile->site(vec, 2);
    REQUIRE(param != nullptr);
    CHECK_FALSE(param->monomorphic());
    CHECK(param->tagMask() == ((1u << BRONZE_FEEDBACK_TAG_OBJECT) | (1u << BRONZE_FEEDBACK_TAG_UNDEFINED)));

    const spec::SiteFeedback* num = profile->site(vec, 4);
    REQUIRE(num != nullptr);
    CHECK(num->tags[BRONZE_FEEDBACK_TAG_F64] == 60);
    CHECK(num->tags[BRONZE_FEEDBACK_TAG_I32] == 20);
    CHECK(profile->site(vec, 99) == nullptr);

    // An escaped tab in a key, the folded tail, a native callee.
    const std::string seg = "main.seg1\tx#ffffffffffffffff";
    const spec::SiteFeedback* method = profile->site(seg, 0);
    REQUIRE(method != nullptr);
    CHECK(method->shapes.size() == 2);
    CHECK(method->soleShape().empty());
    CHECK(method->soleTarget() == "updateMatrix@0badf00d");
    REQUIRE(profile->site(seg, 1) != nullptr);
    CHECK(profile->site(seg, 1)->soleTarget() == "native:bound f");
}

TEST_CASE("profile: malformed input is refused with its line") {
    std::string err;
    CHECK_FALSE(spec::TypeProfile::parse("bronze-type-feedback 99\n", &err));
    CHECK(err.find("line 1") != std::string::npos);
    CHECK_FALSE(spec::TypeProfile::parse("", &err));
    CHECK_FALSE(spec::TypeProfile::parse("bronze-type-feedback 1\nsite\t0\tvalue\t1\tf64=1\n", &err));
    CHECK(err.find("line 2") != std::string::npos);
    CHECK_FALSE(spec::TypeProfile::parse("bronze-type-feedback 1\nfn\tf#1\t0\nsite\t0\tvalue\t1\tf32=1\n", &err));
    CHECK(err.find("f32") != std::string::npos);
    CHECK_FALSE(spec::TypeProfile::parse(
        "bronze-type-feedback 1\nfn\tf#1\t0\nsite\t0\tvalue\t1\t\nsite\t0\tvalue\t1\t\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse("bronze-type-feedback 1\nfn\tf#1\t0\nshape\t0\t1\tX{}\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse("bronze-type-feedback 1\nfn\tf#1\tmany\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse("bronze-type-feedback 1\nbogus\t1\n", &err));
}

TEST_CASE("site keys: stable names strip the counters other code renumbers") {
    CHECK(il::stableFunctionName("__anon_fn_12") == "__anon_fn_");
    CHECK(il::stableFunctionName("mod3.Foo.bar") == "mod.Foo.bar");
    CHECK(il::stableFunctionName("mod3.__anon_fn_7$typed_ff") == "mod.__anon_fn_$typed_ff");
    CHECK(il::stableFunctionName("module.x") == "module.x");
    CHECK(il::stableFunctionName("Vector3.add") == "Vector3.add");
}

TEST_CASE("site keys: an unchanged function keeps its key; a changed one does not") {
    const std::string before =
        "function f(a, b) { return a + b; }\n"
        "function g(x) { return x * 2; }\n"
        "const h = [1, 2].map(function (v) { return v + 1; });\n"
        "console.log(f(1, 2), g(3), h);\n";
    // A new anonymous function AHEAD of the old one renumbers it, and `g`'s
    // body changes; `f` and the old anonymous function do not.
    const std::string after =
        "[3].forEach(function (w) { console.log(w); });\n"
        "function f(a, b) { return a + b; }\n"
        "function g(x) { return x * 3; }\n"
        "const h = [1, 2].map(function (v) { return v + 1; });\n"
        "console.log(f(1, 2), g(3), h);\n";
    const il::Module a = lowered(before);
    const il::Module b = lowered(after);

    const auto fa = keysEndingIn(a, "f");
    const auto fb = keysEndingIn(b, "f");
    REQUIRE(fa.size() == 1);
    REQUIRE(fb.size() == 1);
    CHECK(fa[0] == fb[0]);

    const auto ga = keysEndingIn(a, "g");
    const auto gb = keysEndingIn(b, "g");
    REQUIRE(ga.size() == 1);
    REQUIRE(gb.size() == 1);
    CHECK(ga[0] != gb[0]);
    // Same name, different hash.
    CHECK(ga[0].substr(0, ga[0].find('#')) == gb[0].substr(0, gb[0].find('#')));

    // The `v + 1` callback, whichever counter it got.
    const std::vector<std::string> keysA = il::feedbackFunctionKeys(a);
    const std::vector<std::string> keysB = il::feedbackFunctionKeys(b);
    std::vector<std::string> anonA;
    std::vector<std::string> anonB;
    for (const auto& k : keysA) {
        if (k.rfind("__anon_fn_#", 0) == 0) anonA.push_back(k);
    }
    for (const auto& k : keysB) {
        if (k.rfind("__anon_fn_#", 0) == 0) anonB.push_back(k);
    }
    REQUIRE(anonA.size() == 1);
    REQUIRE(anonB.size() == 2);
    CHECK((anonB[0] == anonA[0] || anonB[1] == anonA[0]));
}

TEST_CASE("site keys: the enumeration is a function of the IL") {
    const std::string src =
        "class P { constructor(x) { this.x = x; } get2() { return this.x * 2; } }\n"
        "function use(o, k) { return o.get2() + k + (o.x < k ? 1 : 0); }\n"
        "let s = 0; for (let i = 0; i < 3; i++) s = use(new P(i), s);\n"
        "console.log(s);\n";
    const il::Module a = lowered(src);
    const il::Module b = lowered(src);
    REQUIRE(a.functions.size() == b.functions.size());
    size_t total = 0;
    for (size_t i = 0; i < a.functions.size(); ++i) {
        const auto sa = il::enumerateFeedbackSites(a, a.functions[i]);
        const auto sb = il::enumerateFeedbackSites(b, b.functions[i]);
        REQUIRE(sa.size() == sb.size());
        for (size_t s = 0; s < sa.size(); ++s) {
            CHECK(sa[s].ordinal == s);
            CHECK(sa[s].kind == sb[s].kind);
            CHECK(sa[s].block == sb[s].block);
            CHECK(sa[s].inst == sb[s].inst);
            CHECK(sa[s].value == sb[s].value);
        }
        total += sa.size();
    }
    CHECK(total > 0);
    CHECK(il::feedbackFunctionKeys(a) == il::feedbackFunctionKeys(b));
}

TEST_CASE("instrumentation: one verified record per site, registered through the census table") {
    const std::string src =
        "function use(o, k) { return o.v + k; }\n"
        "const objs = [{ v: 1 }, { v: 'a', w: 2 }];\n"
        "let s = 0; for (let i = 0; i < 2; i++) s = use(objs[i], s);\n"
        "console.log(s);\n";
    il::Module mod = lowered(src);
    size_t expected = 0;
    size_t methods = 0;
    for (const auto& fn : mod.functions) {
        for (const auto& site : il::enumerateFeedbackSites(mod, fn)) {
            ++expected;
            if (site.kind == il::FeedbackKind::Method) ++methods;
        }
    }
    REQUIRE(expected > 0);

    std::string err;
    lower::FeedbackStats stats;
    REQUIRE_MESSAGE(lower::instrumentFeedback(mod, "/tmp/x.tfb", &err, &stats), err);
    CHECK(stats.sites == expected);

    size_t records = 0;
    for (const auto& fn : mod.functions) {
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op == il::Op::FeedbackRecord) ++records;
            }
        }
    }
    CHECK(records == expected);
    DiagnosticSink diags;
    CHECK(il::verify(mod, diags));
    REQUIRE(!mod.censusSites.empty());
    CHECK((mod.censusSites[0].info & BRONZE_FEEDBACK_FLAG) != 0);
    CHECK(BRONZE_FEEDBACK_INFO_KIND(mod.censusSites[0].info) == BRONZE_FEEDBACK_KIND_MARKER);
    CHECK(mod.censusSites.size() == 1 + 2 * methods);
    CHECK(mod.censusOutPath == "/tmp/x.tfb");
    const std::string printed = il::print(mod);
    CHECK(printed.find("feedback.record") != std::string::npos);
    CHECK(printed.find("\nfeedback /tmp/x.tfb") != std::string::npos);

    // A second instrument on the same channel is refused.
    CHECK_FALSE(lower::instrumentFeedback(mod, "/tmp/y.tfb", &err));
}
