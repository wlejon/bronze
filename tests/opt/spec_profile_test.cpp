#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
// The type-feedback profile (src/opt/spec/profile.h) and the site keys it is
// read by (src/il/feedback_site.h): the reader's round trip and refusals, and
// the property the keys exist for — a function that did not change keeps its
// key when the program around it does, and one that did change loses it.

#include <doctest/doctest.h>

#include <set>
#include <string>
#include <vector>

#include "../lower/lower_fixture.h"
#include "il/feedback_site.h"
#include "il/print.h"
#include "il/verifier.h"
#include "lower/lower_feedback.h"
#include "opt/spec/feedback_plan.h"
#include "opt/spec/profile.h"

using namespace bronze;

namespace {

const char* const kProfile =
    "bronze-type-feedback 2\n"
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

// An inline-cache harvest (docs/type-feedback.md, "Harvest").
const char* const kHarvest =
    "bronze-type-feedback 2\n"
    "harvest\t00112233aabbccdd\t9\n"
    "ic\t0\tproperty\t-\n"
    "way\t0\town\tVector3{x,y,z,w,a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u,v,aa,bb,cc,dd,ee,ff,gg,hh}\n"
    "ic\t3\tproperty\tfull\n"
    "way\t3\tproto\tVec{}<Vec.prototype>\n"
    "way\t3\tabsent\tObject{a}\n"
    "ic\t8\tmethod\tmega\n"
    "way\t8\tdirect\tMesh{uuid,name}\t~1204@0badf00d\n"
    "way\t8\tnative\tarray\tnative:array\n"
    "sample\t0000002a\t7\n"
    "sample\tdeadbeef\t1500\n"
    "doubles\tMesh{uuid,name}\t1\n"
    "doubles\tVector3{x,y,z}\t0,1,2\n"
    "mixed\tname\n"
    "mixed\tw\n"
    "objects\tSprite{position,scale}\t0,1\n"
    "nonobject\tparent\n";

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
    CHECK(profile->harvest() == nullptr);
    // A version-1 file (a recorder that cut shapes at 32 keys) still reads.
    std::string v1 = kProfile;
    v1.replace(v1.find(" 2\n"), 3, " 1\n");
    CHECK(spec::TypeProfile::parse(v1, &err).has_value());
}

TEST_CASE("profile: a harvest parses, serializes byte for byte, and keeps every key") {
    std::string err;
    auto profile = spec::TypeProfile::parse(kHarvest, &err);
    REQUIRE_MESSAGE(profile.has_value(), err);
    CHECK(profile->serialize() == kHarvest);
    const spec::Harvest* h = profile->harvest();
    REQUIRE(h != nullptr);
    CHECK(h->digest == 0x00112233aabbccddull);
    CHECK(h->sites == 9);
    REQUIRE(h->ics.size() == 3);
    CHECK(h->ics.at(0).ways.front().receiver.find(",hh}") != std::string::npos);
    CHECK(h->ics.at(3).full);
    CHECK(h->ics.at(3).ways.size() == 2);
    CHECK(h->ics.at(8).method);
    CHECK(h->ics.at(8).mega);
    CHECK(h->ics.at(8).ways.front().callee == "~1204@0badf00d");
    REQUIRE(h->samples.size() == 2);
    CHECK(h->samples.at(0x2a) == 7);
    CHECK(h->samples.at(0xdeadbeef) == 1500);
    REQUIRE(h->doubles.size() == 2);
    CHECK(h->doubles.at("Vector3{x,y,z}") == std::vector<uint32_t>{0, 1, 2});
    CHECK(h->mixed == std::set<std::string>{"name", "w"});
    CHECK(h->objects.at("Sprite{position,scale}") == std::vector<uint32_t>{0, 1});
    CHECK(h->nonObject == std::set<std::string>{"parent"});
}

TEST_CASE("profile: a malformed harvest is refused") {
    std::string err;
    const std::string head = "bronze-type-feedback 2\n";
    // A row before the harvest row; a short digest; an index past the sites.
    CHECK_FALSE(spec::TypeProfile::parse(head + "ic\t0\tproperty\t-\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(head + "harvest\t0011\t4\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(head + "harvest\t00112233aabbccdd\t4\nic\t4\tproperty\t-\n", &err));
    // An unknown flag, form, or a method way without its callee.
    CHECK_FALSE(spec::TypeProfile::parse(head + "harvest\t00112233aabbccdd\t4\nic\t1\tproperty\thot\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(
        head + "harvest\t00112233aabbccdd\t4\nic\t1\tproperty\t-\nway\t1\tdirect\tA{x}\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(
        head + "harvest\t00112233aabbccdd\t4\nic\t1\tmethod\t-\nway\t1\tdirect\tA{x}\n", &err));
    // A way for an ic not listed, and an ic listed twice.
    CHECK_FALSE(spec::TypeProfile::parse(head + "harvest\t00112233aabbccdd\t4\nway\t1\town\tA{x}\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(
        head + "harvest\t00112233aabbccdd\t4\nic\t1\tproperty\t-\nic\t1\tproperty\t-\n", &err));
    // A sample before the harvest row, a short hash, a hash twice.
    CHECK_FALSE(spec::TypeProfile::parse(head + "sample\t0000002a\t7\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(head + "harvest\t00112233aabbccdd\t4\nsample\t2a\t7\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(
        head + "harvest\t00112233aabbccdd\t4\nsample\t0000002a\t7\nsample\t0000002a\t1\n", &err));
    // Positions out of order, empty, or with an empty member; a receiver or
    // a key listed twice.
    const std::string h = head + "harvest\t00112233aabbccdd\t4\n";
    CHECK_FALSE(spec::TypeProfile::parse(h + "doubles\tA{x,y}\t1,0\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(h + "doubles\tA{x,y}\t\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(h + "doubles\tA{x,y}\t0,\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(h + "doubles\tA{x}\t0\ndoubles\tA{x}\t0\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(h + "mixed\tx\nmixed\tx\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(h + "objects\tA{x,y}\t1,0\n", &err));
    CHECK_FALSE(spec::TypeProfile::parse(h + "nonobject\tx\nnonobject\tx\n", &err));
    CHECK(spec::TypeProfile::parse(h + "doubles\tA{x,y}\t0,1\nmixed\tz\n", &err));
    CHECK(spec::TypeProfile::parse(h + "objects\tA{x,y}\t0\nnonobject\ty\n", &err));
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

TEST_CASE("feedback apply: polymorphic class-family reads and writes get family claims") {
    const std::string src =
        "class Base {\n"
        "  x: any; y: any;\n"
        "  constructor(x: any, y: any) { this.x = x; this.y = y; }\n"
        "  readX() { return this.x; }\n"
        "}\n"
        "class Sub extends Base {\n"
        "  z: any;\n"
        "  constructor(x: any, y: any, z: any) { super(x, y); this.z = z; }\n"
        "}\n"
        "function step(o: any) { o.x = o.y; }\n"
        "const list: any = [new Base(1, 2), new Sub(3, 4, 5)];\n"
        "step(list[0]); step(list[1]);\n";
    il::Module mod = lowered(src);
    REQUIRE(mod.classFamilies.size() >= 2);

    const il::Function* stepFn = nullptr;
    for (const auto& fn : mod.functions) {
        if (fn.name.size() >= 4 && fn.name.compare(fn.name.size() - 4, 4, "step") == 0) stepFn = &fn;
    }
    REQUIRE(stepFn != nullptr);

    const size_t xKey = lower_test::keyIndex(mod, "x");
    const size_t yKey = lower_test::keyIndex(mod, "y");
    uint32_t getIc = UINT32_MAX, setIc = UINT32_MAX;
    for (const auto& b : stepFn->blocks) {
        for (const auto& inst : b.instructions) {
            if (inst.op == il::Op::PropGet && inst.keyIndex == yKey) getIc = inst.icIndex;
            if (inst.op == il::Op::PropSet && inst.keyIndex == xKey) setIc = inst.icIndex;
        }
    }
    REQUIRE(getIc != UINT32_MAX);
    REQUIRE(setIc != UINT32_MAX);

    spec::FeedbackPlan plan;
    for (uint32_t ic : {getIc, setIc}) {
        spec::NameCounts& nc = plan.shapesByIc[ic];
        nc.counts["Base#16{x,y}"] = 512;
        nc.counts["Sub#16{x,y,z}"] = 512;
        nc.total = 1024;
    }

    spec::FeedbackApplyStats stats;
    spec::applyFeedbackClaimsAndHints(mod, plan, &stats);
    CHECK(stats.shapeFamily == 2);
    CHECK(stats.shapeIdentity == 0);
    CHECK(stats.chainReads == 0);

    for (const auto& b : stepFn->blocks) {
        for (const auto& inst : b.instructions) {
            if (inst.icIndex == getIc && inst.op == il::Op::PropGet) {
                CHECK(inst.staticSlot == 1);
                CHECK(inst.staticSlotGuessed);
                CHECK(inst.familyLo != il::Instruction::kNoFamily);
                CHECK(inst.familySpan >= 1);
                CHECK(inst.icChain == 0);
            }
            if (inst.icIndex == setIc && inst.op == il::Op::PropSet) {
                CHECK(inst.staticSlot == 0);
                CHECK(inst.staticSlotGuessed);
                CHECK(inst.familyLo != il::Instruction::kNoFamily);
                CHECK(inst.familySpan >= 1);
            }
        }
    }
    DiagnosticSink diags;
    CHECK(il::verify(mod, diags));

    // A non-writable field in the family range or a prototype-like shape must
    // not receive a family write claim.
    il::Module nonWritable = lowered(src);
    for (auto& fam : nonWritable.classFamilies) {
        if (!fam.fields.empty()) fam.fields[0].writable = false;
    }
    spec::FeedbackApplyStats nwStats;
    spec::applyFeedbackClaimsAndHints(nonWritable, plan, &nwStats);
    CHECK(nwStats.shapeFamily == 1);  // read of y still claimed; write of x refused

    il::Module protoWrite = lowered(src);
    spec::FeedbackPlan protoPlan;
    protoPlan.shapesByIc[setIc].counts["Base.prototype{constructor,x,y}"] = 1024;
    protoPlan.shapesByIc[setIc].total = 1024;
    spec::FeedbackApplyStats protoStats;
    spec::applyFeedbackClaimsAndHints(protoWrite, protoPlan, &protoStats);
    CHECK(protoStats.shapeFamily == 0);
    CHECK(protoStats.shapeRefused == 1);
}

TEST_CASE("feedback apply: bimorphic method calls classify inherited vs own-slot targets") {
    const std::string src =
        "class A { m(v: any) { return v + 1; } }\n"
        "class B { m(v: any) { return v + 2; } }\n"
        "function invoke(o: any, x: any) { return o.m(x); }\n"
        "invoke(new A(), 1); invoke(new B(), 2);\n";
    il::Module mod = lowered(src);

    uint32_t fnA = UINT32_MAX, fnB = UINT32_MAX, callIc = UINT32_MAX;
    for (uint32_t i = 0; i < mod.functions.size(); ++i) {
        const std::string& n = mod.functions[i].name;
        if (n.find("A.m") != std::string::npos) fnA = i;
        if (n.find("B.m") != std::string::npos) fnB = i;
        if (n.find("invoke") != std::string::npos) {
            for (const auto& b : mod.functions[i].blocks) {
                for (const auto& inst : b.instructions) {
                    if (inst.op == il::Op::MethodCall) callIc = inst.icIndex;
                }
            }
        }
    }
    REQUIRE(fnA != UINT32_MAX);
    REQUIRE(fnB != UINT32_MAX);
    REQUIRE(callIc != UINT32_MAX);

    spec::FeedbackPlan protoPlan;
    protoPlan.callees["B.m@1"] = fnB;
    protoPlan.callees["A.m@2"] = fnA;
    protoPlan.methodsByIc[callIc].counts["A.m@2"] = 400;
    protoPlan.methodsByIc[callIc].counts["B.m@1"] = 600;
    protoPlan.methodsByIc[callIc].total = 1000;
    protoPlan.methodShapesByIc[callIc].counts["A#16{x}"] = 400;
    protoPlan.methodShapesByIc[callIc].counts["B#16{x}"] = 600;
    protoPlan.methodShapesByIc[callIc].total = 1000;

    il::Module m1 = mod;
    std::unordered_map<uint32_t, uint32_t> ownCallees;
    std::unordered_map<uint32_t, std::vector<uint32_t>> polyMethods, polyOwn;
    spec::FeedbackApplyStats stats;
    spec::applyFeedbackDirectMethods(m1, protoPlan, ownCallees, &stats, &polyMethods, &polyOwn);
    REQUIRE(polyMethods.count(callIc) == 1);
    CHECK(polyMethods[callIc] == std::vector<uint32_t>{fnB, fnA});
    CHECK(polyOwn.empty());

    // Own-slot receivers populate polyOwnCallees and set kMethodOwnCallee.
    spec::FeedbackPlan ownPlan = protoPlan;
    ownPlan.methodShapesByIc.clear();
    ownPlan.methodShapesByIc[callIc].counts["Mixer#16{m,buf}"] = 1000;
    ownPlan.methodShapesByIc[callIc].total = 1000;

    il::Module m2 = mod;
    polyMethods.clear();
    polyOwn.clear();
    spec::applyFeedbackDirectMethods(m2, ownPlan, ownCallees, &stats, &polyMethods, &polyOwn);
    CHECK(polyMethods.empty());
    REQUIRE(polyOwn.count(callIc) == 1);
    CHECK(polyOwn[callIc] == std::vector<uint32_t>{fnB, fnA});

    // Mixed own + inherited receivers populate neither.
    spec::FeedbackPlan mixedPlan = protoPlan;
    mixedPlan.methodShapesByIc.clear();
    mixedPlan.methodShapesByIc[callIc].counts["Mixer#16{m}"] = 500;
    mixedPlan.methodShapesByIc[callIc].counts["B#16{x}"] = 500;
    mixedPlan.methodShapesByIc[callIc].total = 1000;

    il::Module m3 = mod;
    polyMethods.clear();
    polyOwn.clear();
    spec::applyFeedbackDirectMethods(m3, mixedPlan, ownCallees, &stats, &polyMethods, &polyOwn);
    CHECK(polyMethods.empty());
    CHECK(polyOwn.empty());
}
