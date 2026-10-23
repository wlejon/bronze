#include <doctest/doctest.h>

#include <cstdlib>
#include "embed/embed.h"
#include "eval/eval.h"
#include "runtime/rt_state.h"

using namespace bronze;
using namespace bronze::eval;

TEST_CASE("JSON.stringify clamps negative/infinite space and unwraps boxed space/replacer and rawJSON") {
    embed::CallResult r1 = evalScript("JSON.stringify({a: 1}, null, -1e100);");
    CHECK(!r1.thrown);
    CHECK(embed::toUtf8(r1.value) == "{\"a\":1}");

    embed::CallResult r2 = evalScript("JSON.stringify({a: 1}, null, -Infinity);");
    CHECK(!r2.thrown);
    CHECK(embed::toUtf8(r2.value) == "{\"a\":1}");

    embed::CallResult r3 = evalScript("JSON.stringify({a: 1}, null, new Number(2));");
    CHECK(!r3.thrown);
    CHECK(embed::toUtf8(r3.value) == "{\n  \"a\": 1\n}");

    embed::CallResult r4 = evalScript("JSON.stringify({a: 1, b: 2}, [new String('b')]);");
    CHECK(!r4.thrown);
    CHECK(embed::toUtf8(r4.value) == "{\"b\":2}");

    embed::CallResult r5 = evalScript(
        "const rj = JSON.rawJSON('12345');"
        "JSON.isRawJSON(rj) + ':' + JSON.stringify({x: rj});");
    CHECK(!r5.thrown);
    CHECK(embed::toUtf8(r5.value) == "true:{\"x\":12345}");
}

TEST_CASE("JSON.parse accepts callable Proxy reviver") {
    embed::CallResult r = evalScript(
        "const rev = new Proxy((k, v) => typeof v === 'number' ? v * 2 : v, {});"
        "JSON.parse('{\"a\":21}', rev).a;");
    CHECK(!r.thrown);
    CHECK(r.value.asNumber() == 42.0);
}

TEST_CASE("RegExp replace and split survive RegExpCache eviction inside callbacks") {
    embed::CallResult r1 = evalScript(
        "const re = /(a)/g;"
        "const out = 'aaa'.replace(re, (m) => {"
        "  for (let i = 0; i < 600; ++i) new RegExp('pat_' + i).test('x');"
        "  return m.toUpperCase();"
        "});"
        "out;");
    CHECK(!r1.thrown);
    CHECK(embed::toUtf8(r1.value) == "AAA");

    embed::CallResult r2 = evalScript(
        "const re2 = /,/;"
        "const lim = { valueOf() {"
        "  for (let i = 0; i < 600; ++i) new RegExp('sp_' + i).test('x');"
        "  return 2;"
        "}};"
        "'a,b,c'.split(re2, lim).join('|');");
    CHECK(!r2.thrown);
    CHECK(embed::toUtf8(r2.value) == "a|b");
}

TEST_CASE("RegExp open bracket in non-v character class with capture group works") {
    embed::CallResult r = evalScript(
        "const m = /[[](a)/.exec('[a');"
        "m[0] + ':' + m[1];");
    CHECK(!r.thrown);
    CHECK(embed::toUtf8(r.value) == "[a:a");
}

TEST_CASE("Callable Proxy works in yield*, Map.forEach, Set.forEach, String.replace, TypedArray.map") {
    embed::CallResult r1 = evalScript(
        "const pNext = new Proxy(function(v) { return { value: 42, done: false }; }, {});"
        "const it = { [Symbol.iterator]() { return { next: pNext }; } };"
        "function* gen() { yield* it; }"
        "gen().next().value;");
    CHECK(!r1.thrown);
    CHECK(r1.value.asNumber() == 42.0);

    embed::CallResult r2 = evalScript(
        "let sum = 0;"
        "const cb = new Proxy((v) => { sum += v; }, {});"
        "new Map([[1, 10], [2, 20]]).forEach(cb);"
        "new Set([3, 4]).forEach(cb);"
        "const s = 'ab'.replace(/a/, new Proxy(() => 'A', {}));"
        "const ta = new Int32Array([1, 2]).map(new Proxy((x) => x * 10, {}));"
        "sum + ':' + s + ':' + ta[0] + ',' + ta[1];");
    CHECK(!r2.thrown);
    CHECK(embed::toUtf8(r2.value) == "37:Ab:10,20");
}

TEST_CASE("Array fill, copyWithin, and splice survive re-entrant length/shift mutation in valueOf") {
    embed::CallResult r1 = evalScript(
        "const a = [1, 2, 3, 4, 5];"
        "a.fill(9, { valueOf() { a.length = 1; return 0; } });"
        "a.length + ':' + a[0] + ',' + a[4];");
    CHECK(!r1.thrown);
    CHECK(embed::toUtf8(r1.value) == "5:9,9");

    embed::CallResult r2 = evalScript(
        "const b = [1, 2, 3, 4, 5];"
        "b.copyWithin(0, { valueOf() { b.shift(); b.shift(); return 1; } });"
        "b.join(',');");
    CHECK(!r2.thrown);
    CHECK(embed::toUtf8(r2.value) == "4,5,");

    embed::CallResult r3 = evalScript(
        "const c = [10, 20, 30, 40, 50];"
        "const rem = c.splice({ valueOf() { c.length = 1; return 0; } }, 4, 99);"
        "c.join(',') + '|' + rem.length;");
    CHECK(!r3.thrown);
    CHECK(embed::toUtf8(r3.value) == "99,|4");
}

TEST_CASE("Object.fromEntries defines __proto__ as own property without prototype pollution") {
    embed::CallResult r = evalScript(
        "const o = Object.fromEntries([['__proto__', { polluted: 123 }]]);"
        "(o.polluted === undefined) + ':' + Object.prototype.hasOwnProperty.call(o, '__proto__');");
    CHECK(!r.thrown);
    CHECK(embed::toUtf8(r.value) == "true:true");
}

TEST_CASE("TypedArray methods survive re-entrant ArrayBuffer resize/transfer in valueOf") {
    embed::CallResult r1 = evalScript(
        "const rab = new ArrayBuffer(16, { maxByteLength: 32 });"
        "const ta = new Int32Array(rab);"
        "ta.set([10, { valueOf() { rab.resize(4); return 20; } }, 30]);"
        "ta.length + ':' + ta[0];");
    CHECK(!r1.thrown);
    CHECK(embed::toUtf8(r1.value) == "1:10");

    embed::CallResult r2 = evalScript(
        "const rab2 = new ArrayBuffer(16, { maxByteLength: 32 });"
        "const ta2 = new Int32Array(rab2);"
        "ta2[0] = 1; ta2[1] = 2; ta2[2] = 3; ta2[3] = 4;"
        "ta2.copyWithin(0, { valueOf() { rab2.resize(8); return 1; } });"
        "ta2.join(',');");
    CHECK(!r2.thrown);
    CHECK(embed::toUtf8(r2.value) == "2,2");

    embed::CallResult r3 = evalScript(
        "const rab3 = new ArrayBuffer(16, { maxByteLength: 32 });"
        "const ta3 = new Int32Array(rab3);"
        "ta3[0] = 7; ta3[1] = 8;"
        "const idx = ta3.indexOf(8, { valueOf() { rab3.resize(4); return 0; } });"
        "const j = ta3.join({ toString() { rab3.resize(0); return '-'; } });"
        "idx + ':' + j;");
    CHECK(!r3.thrown);
    CHECK(embed::toUtf8(r3.value) == "-1:");
}

TEST_CASE("String.prototype.repeat and padStart reject huge lengths with RangeError") {
    embed::CallResult r1 = evalScript(
        "let ok1 = false;"
        "try { 'a'.repeat(1e20); } catch (e) { ok1 = e instanceof RangeError; }"
        "let ok2 = false;"
        "try { 'a'.padStart(1e20, 'b'); } catch (e) { ok2 = e instanceof RangeError; }"
        "const emptyRep = ''.repeat(1e10);"
        "ok1 + ':' + ok2 + ':' + emptyRep.length;");
    CHECK(!r1.thrown);
    CHECK(embed::toUtf8(r1.value) == "true:true:0");
}

namespace {
void setEnvFlag(const char* name, const char* val) {
#ifdef _WIN32
    _putenv_s(name, val ? val : "");
#else
    if (val) {
        setenv(name, val, 1);
    } else {
        unsetenv(name);
    }
#endif
}
}  // namespace

TEST_CASE("Differential equivalence across default, --no-infer, no-guarded-regions, no-pin-barriers, and gc-stress") {
    const char* workload =
        "function compute(n) {"
        "  const obj = JSON.parse('{\"pts\":[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4}],\"tag\":\"alpha-42-beta\"}');"
        "  const m = new Map([['a', 10], ['b', 20]]);"
        "  const s = new Set([1, 2, 3, 2]);"
        "  const ta = new Float64Array([1.5, 2.5, 3.5, 4.5]);"
        "  ta.copyWithin(1, 0, 2);"
        "  let acc = 0;"
        "  for (let i = 0; i < obj.pts.length; ++i) {"
        "    const p = obj.pts[i];"
        "    acc += p.x * 10 + p.y;"
        "  }"
        "  m.forEach((v) => { acc += v; });"
        "  for (const v of s) { acc += v; }"
        "  for (let i = 0; i < ta.length; ++i) { acc += ta[i]; }"
        "  const rx = /([a-z]+)-(\\d+)/g;"
        "  const rep = obj.tag.replace(rx, (_, w, d) => w.toUpperCase() + ':' + (Number(d) + n));"
        "  const entries = Object.fromEntries([['sum', acc], ['rep', rep]]);"
        "  return JSON.stringify(entries);"
        "}"
        "compute(8);";

    embed::CallResult rDefault = evalScript(workload);
    REQUIRE(!rDefault.thrown);
    const std::string expected = embed::toUtf8(rDefault.value);

    setEnvFlag("BRONZE_NO_INFER", "1");
    embed::CallResult rNoInfer = evalScript(workload);
    setEnvFlag("BRONZE_NO_INFER", nullptr);
    REQUIRE(!rNoInfer.thrown);
    CHECK(embed::toUtf8(rNoInfer.value) == expected);

    setEnvFlag("BRONZE_NO_GUARDED_REGIONS", "1");
    embed::CallResult rNoGr = evalScript(workload);
    setEnvFlag("BRONZE_NO_GUARDED_REGIONS", nullptr);
    REQUIRE(!rNoGr.thrown);
    CHECK(embed::toUtf8(rNoGr.value) == expected);

    setEnvFlag("BRONZE_NO_PIN_BARRIERS", "1");
    embed::CallResult rNoPin = evalScript(workload);
    setEnvFlag("BRONZE_NO_PIN_BARRIERS", nullptr);
    REQUIRE(!rNoPin.thrown);
    CHECK(embed::toUtf8(rNoPin.value) == expected);

    const bool prevStress = runtime::rtHeap().gc_stress();
    runtime::rtHeap().set_gc_stress(true);
    embed::CallResult rGcStress = evalScript(workload);
    runtime::rtHeap().set_gc_stress(prevStress);
    REQUIRE(!rGcStress.thrown);
    CHECK(embed::toUtf8(rGcStress.value) == expected);
}
