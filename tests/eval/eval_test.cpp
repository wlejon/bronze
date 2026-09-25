#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "../test_temp_dir.h"
#include "eval/eval.h"
#include "embed/embed.h"
#include "runtime/exception.h"
#include "runtime/rt_state.h"
#include <filesystem>
#include <fstream>

using namespace bronze;
using namespace bronze::eval;

TEST_CASE("evalScript evaluates basic arithmetic expressions") {
    embed::CallResult r1 = evalScript("1 + 2");
    CHECK(!r1.thrown);
    CHECK(r1.value.asNumber() == 3.0);

    embed::CallResult r2 = evalScript("40 / 2 + 2");
    CHECK(!r2.thrown);
    CHECK(r2.value.asNumber() == 22.0);

    embed::CallResult r3 = evalScript("Math.min(10, 5)");
    CHECK(!r3.thrown);
    CHECK(r3.value.asNumber() == 5.0);
}

TEST_CASE("evalScript evaluates string concatenation and boolean expressions") {
    embed::CallResult r1 = evalScript("'hello ' + 'world'");
    CHECK(!r1.thrown);
    CHECK(embed::isString(r1.value));
    CHECK(embed::toUtf8(r1.value) == "hello world");

    embed::CallResult r2 = evalScript("10 > 5");
    CHECK(!r2.thrown);
    CHECK(embed::isBool(r2.value));
    CHECK(r2.value.asBool() == true);
}

TEST_CASE("evalScript evaluates multiple statements and returns last expression") {
    embed::CallResult r1 = evalScript("var a = 10; var b = 20; a + b;");
    CHECK(!r1.thrown);
    CHECK(r1.value.asNumber() == 30.0);

    embed::CallResult r2 = evalScript("let x = 7; let y = 8; x * y;");
    CHECK(!r2.thrown);
    CHECK(r2.value.asNumber() == 56.0);
}

TEST_CASE("evalScript exports top-level var and function to globalThis") {
    embed::CallResult r1 = evalScript("var globalCounter = 100;");
    CHECK(!r1.thrown);

    embed::CallResult r2 = evalScript("globalCounter + 5;");
    CHECK(!r2.thrown);
    CHECK(r2.value.asNumber() == 105.0);

    embed::CallResult r3 = evalScript("function addTen(n) { return n + 10; }");
    CHECK(!r3.thrown);

    embed::CallResult r4 = evalScript("addTen(32);");
    CHECK(!r4.thrown);
    CHECK(r4.value.asNumber() == 42.0);
}

TEST_CASE("evalScript handles statement-only code returning undefined") {
    embed::CallResult r1 = evalScript("var z = 99;");
    CHECK(!r1.thrown);
    CHECK(embed::isUndefined(r1.value));

    embed::CallResult r2 = evalScript("");
    CHECK(!r2.thrown);
    CHECK(embed::isUndefined(r2.value));

    embed::CallResult r3 = evalScript("   \n   ");
    CHECK(!r3.thrown);
    CHECK(embed::isUndefined(r3.value));
}

TEST_CASE("evalScript handles syntax errors gracefully") {
    embed::CallResult r = evalScript("var 123 invalid syntax ;;;");
    CHECK(r.thrown);
    CHECK(embed::isObject(r.value));
}

TEST_CASE("evalScript handles runtime exceptions gracefully") {
    embed::CallResult r = evalScript("throw new Error('test runtime error');");
    CHECK(r.thrown);
    CHECK(embed::isObject(r.value));
    Value msg = embed::getProperty(r.value, "message");
    CHECK(embed::toUtf8(msg) == "test runtime error");
}

TEST_CASE("evalFunction creates callable ordinary function") {
    std::vector<std::string> params{"a", "b"};
    Value fn = evalFunction(params, "return a * b + 2;");
    REQUIRE(embed::isFunction(fn));

    std::vector<Value> args{embed::fromDouble(6.0), embed::fromDouble(7.0)};
    embed::CallResult r = embed::call(fn, embed::undefined(), args);
    CHECK(!r.thrown);
    CHECK(r.value.asNumber() == 44.0);
}

TEST_CASE("evalFunction creates callable generator function") {
    Value genFn = evalFunction({}, "yield 10; yield 20; return 30;",
                               runtime::DynamicFunctionKind::Generator);
    REQUIRE(embed::isFunction(genFn));

    embed::CallResult r = embed::call(genFn, embed::undefined(), {});
    CHECK(!r.thrown);
    Value gen = r.value;
    REQUIRE(embed::isObject(gen));

    Value nextFn = embed::getProperty(gen, "next");
    REQUIRE(embed::isFunction(nextFn));

    embed::CallResult step1 = embed::call(nextFn, gen, {});
    CHECK(!step1.thrown);
    Value val1 = embed::getProperty(step1.value, "value");
    CHECK(val1.asNumber() == 10.0);
    Value done1 = embed::getProperty(step1.value, "done");
    CHECK(done1.asBool() == false);

    embed::CallResult step2 = embed::call(nextFn, gen, {});
    CHECK(!step2.thrown);
    Value val2 = embed::getProperty(step2.value, "value");
    CHECK(val2.asNumber() == 20.0);
    Value done2 = embed::getProperty(step2.value, "done");
    CHECK(done2.asBool() == false);

    embed::CallResult step3 = embed::call(nextFn, gen, {});
    CHECK(!step3.thrown);
    Value val3 = embed::getProperty(step3.value, "value");
    CHECK(val3.asNumber() == 30.0);
    Value done3 = embed::getProperty(step3.value, "done");
    CHECK(done3.asBool() == true);
}

TEST_CASE("evalFunction creates callable async function") {
    std::vector<std::string> params{"x"};
    Value asyncFn = evalFunction(params, "return x + 5;",
                                 runtime::DynamicFunctionKind::Async);
    REQUIRE(embed::isFunction(asyncFn));

    std::vector<Value> args{embed::fromDouble(10.0)};
    embed::CallResult r = embed::call(asyncFn, embed::undefined(), args);
    CHECK(!r.thrown);
    CHECK(embed::isPromise(r.value));
    embed::drainMicrotasks();
}

TEST_CASE("evalFunction handles syntax error in function body") {
    std::vector<std::string> params{"a"};
    Value thrown;
    CHECK(runtime::rtTryCatch([&] { (void)evalFunction(params, "return a +++ ;;; {{{;"); }, thrown));
    std::string text;
    CHECK(runtime::rtErrorText(thrown, text));
    CHECK(text.find("SyntaxError") == 0);
}

TEST_CASE("evalScript with top-level await wraps into async promise") {
    embed::CallResult cr = evalScript("await Promise.resolve(42);");
    CHECK(!cr.thrown);
    CHECK(embed::isPromise(cr.value));
    embed::drainMicrotasks();
}

// A top-level-await script runs everything after its first await inside the
// microtask drain evalScript ends with, so a script that allocates there
// collects before evalScript looks at the promise its entry returned. Poison
// makes a stale copy of that promise's bits fault (or fail isPromise) on the
// first read instead of reading plausible from-space residue.
TEST_CASE("evalScript keeps a top-level-await promise alive across a collecting drain") {
    Heap& heap = runtime::rtHeap();
    const bool wasPoison = heap.gc_poison();
    heap.set_gc_poison(true);
    const uint64_t epochBefore = embed::relocationEpoch();
    embed::CallResult cr = evalScript(
        "await 0;\n"
        "let keep = [];\n"
        "for (let i = 0; i < 200000; i++) keep.push({ i, s: 'churn' + i });\n"
        "if (keep) globalThis.__churnLength = keep.length;\n");
    const uint64_t epochAfter = embed::relocationEpoch();
    heap.set_gc_poison(wasPoison);
    CHECK(epochAfter != epochBefore);
    CHECK(!cr.thrown);
    CHECK(embed::isPromise(cr.value));
    embed::CallResult len = evalScript("__churnLength;");
    CHECK(!len.thrown);
    CHECK(len.value.asNumber() == 200000.0);
}

TEST_CASE("multiple consecutive evalScript calls retain memory and work together") {
    for (int i = 0; i < 5; ++i) {
        std::string code = "globalThis.loopVal = " + std::to_string(i) + "; globalThis.loopVal * 2;";
        embed::CallResult cr = evalScript(code);
        CHECK(!cr.thrown);
        CHECK(cr.value.asNumber() == static_cast<double>(i * 2));
    }
    embed::CallResult finalCheck = evalScript("loopVal;");
    CHECK(!finalCheck.thrown);
    CHECK(finalCheck.value.asNumber() == 4.0);
}

TEST_CASE("installDefaultDynamicHooks hooks into runtime eval and Function") {
    installDefaultDynamicHooks();

    embed::GlobalValue evalGlobal = embed::globalValue("eval");
    REQUIRE(evalGlobal.found);
    REQUIRE(embed::isFunction(evalGlobal.value));

    std::vector<Value> args{embed::fromUtf8("50 + 50")};
    embed::CallResult r = embed::call(evalGlobal.value, embed::undefined(), args);
    CHECK(!r.thrown);
    CHECK(r.value.asNumber() == 100.0);

    // Clean up dynamic hook to avoid affecting any subsequent tests
    embed::setDynamicEvalHook({});
    embed::setDynamicFunctionHook({});
}

TEST_CASE("evalFile evaluates script file and returns result") {
    std::string tempPath = (bronze_test::tempDir() / "bronze_test_eval_file.js").string();
    {
        std::ofstream out(tempPath);
        out << "function multiply(a, b) { return a * b; }\n";
        out << "multiply(6, 7);\n";
    }

    embed::CallResult r = evalFile(tempPath);
    CHECK(!r.thrown);
    CHECK(r.value.asNumber() == 42.0);

    std::filesystem::remove(tempPath);
}

TEST_CASE("evalScript evaluates script with module imports") {
    std::filesystem::path tempDir = bronze_test::tempDir() / "bronze_test_modules";
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);

    std::filesystem::path helperPath = tempDir / "helper.js";
    {
        std::ofstream out(helperPath);
        out << "export function add(a, b) { return a + b; }\n";
    }

    EvalOptions opts;
    opts.filename = (tempDir / "main.js").string();
    opts.entryResolvesAs = tempDir / "main.js";

    std::string script = "import { add } from './helper.js'; add(19, 23);";
    embed::CallResult r = evalScript(script, opts);
    CHECK(!r.thrown);
    CHECK(r.value.asNumber() == 42.0);

    std::filesystem::remove_all(tempDir, ec);
}

TEST_CASE("a compiled host-global read caches and sees re-registration") {
    // A compiled `hostTick` read is a load of the module's cache cell once
    // the first read has filled it. Re-registering the name must pour the
    // hole back so the next read resolves again — a host that swaps a
    // global (bro re-registers `document` per iframe) is not answered a
    // stale value by a module compiled before the swap.
    embed::registerGlobal("hostTick", embed::fromDouble(1.0));

    embed::CallResult def = evalScript("function readHostTick() { return hostTick; }");
    REQUIRE(!def.thrown);
    embed::GlobalValue fnLookup = embed::globalValue("readHostTick");
    REQUIRE(fnLookup.found);
    embed::Persistent fn(fnLookup.value);

    auto read = [&]() -> double {
        embed::CallResult r = embed::call(fn.get(), embed::undefined(), {});
        REQUIRE(!r.thrown);
        REQUIRE(r.value.isNumber());
        return r.value.asNumber();
    };
    CHECK(read() == 1.0);
    CHECK(read() == 1.0);

    embed::registerGlobal("hostTick", embed::fromDouble(2.0));
    CHECK(read() == 2.0);

    // A second module compiled after the swap gets its own cell, and both
    // follow a further replacement.
    embed::CallResult def2 = evalScript("function readHostTickAgain() { return hostTick + 10; }");
    REQUIRE(!def2.thrown);
    embed::GlobalValue fn2Lookup = embed::globalValue("readHostTickAgain");
    REQUIRE(fn2Lookup.found);
    embed::Persistent fn2(fn2Lookup.value);
    embed::CallResult r2 = embed::call(fn2.get(), embed::undefined(), {});
    REQUIRE(!r2.thrown);
    CHECK(r2.value.asNumber() == 12.0);

    embed::registerGlobal("hostTick", embed::fromDouble(3.0));
    CHECK(read() == 3.0);
    embed::CallResult r3 = embed::call(fn2.get(), embed::undefined(), {});
    REQUIRE(!r3.thrown);
    CHECK(r3.value.asNumber() == 13.0);
}

TEST_CASE("evalScript evaluates code within isolated realms") {
    embed::Realm* realmA = embed::createRealm();
    embed::Realm* realmB = embed::createRealm();

    {
        embed::RealmScope scopeA(realmA);
        auto r1 = evalScript("var x = 100; var y = 20; x + y;");
        CHECK(!r1.thrown);
        CHECK(r1.value.asNumber() == 120.0);
    }

    {
        embed::RealmScope scopeB(realmB);
        // x and y should not exist in realmB!
        auto r2 = evalScript("var x = 200; var y = 50; x + y;");
        CHECK(!r2.thrown);
        CHECK(r2.value.asNumber() == 250.0);
    }

    {
        embed::RealmScope scopeA(realmA);
        auto r3 = evalScript("x + y;");
        CHECK(!r3.thrown);
        CHECK(r3.value.asNumber() == 120.0);
        auto r4 = evalScript("globalThis.x;");
        CHECK(!r4.thrown);
        CHECK(r4.value.asNumber() == 100.0);
    }

    {
        embed::RealmScope scopeB(realmB);
        auto r5 = evalScript("x + y;");
        CHECK(!r5.thrown);
        CHECK(r5.value.asNumber() == 250.0);
        auto r6 = evalScript("globalThis.x;");
        CHECK(!r6.thrown);
        CHECK(r6.value.asNumber() == 200.0);
    }

    embed::destroyRealm(realmA);
    embed::destroyRealm(realmB);
}

TEST_CASE("proxy array refused mutator throws TypeError") {
    embed::CallResult r1 = evalScript(
        "let p = new Proxy([], {});\n"
        "let res = 'no-throw';\n"
        "try {\n"
        "  p.pop();\n"
        "} catch (e) {\n"
        "  res = (e instanceof TypeError ? 'type-error' : 'other-error');\n"
        "}\n"
        "res;\n");
    CHECK(!r1.thrown);
    CHECK(embed::toUtf8(r1.value) == "type-error");
}

TEST_CASE("Atomics.wait, waitAsync, notify behave safely without fatal") {
    embed::CallResult r1 = evalScript(
        "let res = 'no-throw';\n"
        "try {\n"
        "  let w = Atomics.wait;\n"
        "} catch (e) {\n"
        "  res = (e instanceof TypeError ? 'type-error' : 'other-error');\n"
        "}\n"
        "res;\n");
    CHECK(!r1.thrown);
    CHECK(embed::toUtf8(r1.value) == "type-error");

    embed::CallResult r2 = evalScript(
        "let res = 'no-throw';\n"
        "try {\n"
        "  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0);\n"
        "} catch (e) {\n"
        "  res = (e instanceof TypeError ? 'type-error' : 'other-error');\n"
        "}\n"
        "res;\n");
    CHECK(!r2.thrown);
    CHECK(embed::toUtf8(r2.value) == "type-error");
}

TEST_CASE("performance marks and measures") {
    embed::CallResult r1 = evalScript(
        "performance.clearMarks();\n"
        "performance.clearMeasures();\n"
        "performance.mark('m1');\n"
        "performance.mark('m2');\n"
        "performance.measure('meas1', 'm1', 'm2');\n"
        "let entries = performance.getEntries();\n"
        "let marks = performance.getEntriesByType('mark');\n"
        "let measures = performance.getEntriesByType('measure');\n"
        "let m1List = performance.getEntriesByName('m1');\n"
        "entries.length === 3 && marks.length === 2 && measures.length === 1 && m1List.length === 1;\n");
    CHECK(!r1.thrown);
    CHECK(r1.value.asBool() == true);

    embed::CallResult r2 = evalScript(
        "performance.clearMarks('m1');\n"
        "let marksAfter = performance.getEntriesByType('mark');\n"
        "marksAfter.length === 1 && marksAfter[0].name === 'm2';\n");
    CHECK(!r2.thrown);
    CHECK(r2.value.asBool() == true);

    embed::CallResult r3 = evalScript(
        "performance.clearMeasures();\n"
        "performance.getEntriesByType('measure').length === 0;\n");
    CHECK(!r3.thrown);
    CHECK(r3.value.asBool() == true);
}

TEST_CASE("dynamic functions and eval clean up globalThis temporary bindings") {
    std::vector<std::string> params{"x"};
    Value fn = evalFunction(params, "return x + 1;");
    REQUIRE(embed::isFunction(fn));

    embed::CallResult r = evalScript(
        "let found = false;\n"
        "for (let k of Object.keys(globalThis)) {\n"
        "  if (k.startsWith('__bronze_dyn_fn_')) found = true;\n"
        "}\n"
        "found;\n");
    CHECK(!r.thrown);
    CHECK(r.value.asBool() == false);

    evalScript("123 + 456;");
    embed::CallResult r2 = evalScript(
        "let count = 0;\n"
        "for (let k of Object.keys(globalThis)) {\n"
        "  if (k.startsWith('__bronze_eval_res_')) count++;\n"
        "}\n"
        "count;\n");
    CHECK(!r2.thrown);
    CHECK(r2.value.asNumber() <= 1.0);
}

TEST_CASE("clearRetainedPrograms and embed::deleteProperty") {
    clearRetainedPrograms();

    Value obj = embed::createObject();
    embed::setProperty(obj, "testKey", embed::fromDouble(42.0));
    CHECK(embed::getProperty(obj, "testKey").asNumber() == 42.0);

    bool deleted = embed::deleteProperty(obj, "testKey");
    CHECK(deleted);
    CHECK(embed::isUndefined(embed::getProperty(obj, "testKey")));
}

TEST_CASE("Part B: Object, String, RegExp, and block var improvements") {
    // 1. Object.defineProperty on TypedArray, RegExp, ArrayBuffer, DataView
    {
        embed::CallResult r1 = evalScript(
            "let ta = new Uint8Array(4);\n"
            "Object.defineProperty(ta, '0', { value: 99 });\n"
            "ta[0] === 99;\n");
        CHECK(!r1.thrown);
        CHECK(r1.value.asBool() == true);

        embed::CallResult r2 = evalScript(
            "let re = /abc/;\n"
            "Object.defineProperty(re, 'lastIndex', { value: 5 });\n"
            "re.lastIndex === 5;\n");
        CHECK(!r2.thrown);
        CHECK(r2.value.asBool() == true);

        embed::CallResult r3 = evalScript(
            "let ab = new ArrayBuffer(8);\n"
            "Object.defineProperty(ab, 'foo', { value: 123 });\n"
            "ab.foo === 123;\n");
        CHECK(!r3.thrown);
        CHECK(r3.value.asBool() == true);

        embed::CallResult r4 = evalScript(
            "let dv = new DataView(new ArrayBuffer(8));\n"
            "Object.defineProperty(dv, 'bar', { value: 456 });\n"
            "dv.bar === 456;\n");
        CHECK(!r4.thrown);
        CHECK(r4.value.asBool() == true);

        embed::CallResult r5 = evalScript(
            "let caught = false;\n"
            "try {\n"
            "  Object.defineProperty(123, 'foo', { value: 1 });\n"
            "} catch (e) {\n"
            "  caught = (e instanceof TypeError);\n"
            "}\n"
            "caught;\n");
        CHECK(!r5.thrown);
        CHECK(r5.value.asBool() == true);
    }

    // 2. Object.keys and Object.getOwnPropertyNames on ArrayBuffer and DataView
    {
        embed::CallResult r1 = evalScript(
            "let ab = new ArrayBuffer(8);\n"
            "let k1 = Object.keys(ab);\n"
            "let p1 = Object.getOwnPropertyNames(ab);\n"
            "k1.length === 0 && p1.length === 0;\n");
        CHECK(!r1.thrown);
        CHECK(r1.value.asBool() == true);

        embed::CallResult r2 = evalScript(
            "let dv = new DataView(new ArrayBuffer(8));\n"
            "let k2 = Object.keys(dv);\n"
            "let p2 = Object.getOwnPropertyNames(dv);\n"
            "k2.length === 0 && p2.length === 0;\n");
        CHECK(!r2.thrown);
        CHECK(r2.value.asBool() == true);
    }

    // 3. Object.getPrototypeOf on RegExp
    {
        embed::CallResult r = evalScript(
            "let re = /test/;\n"
            "Object.getPrototypeOf(re) === RegExp.prototype;\n");
        CHECK(!r.thrown);
        CHECK(r.value.asBool() == true);
    }

    // 4. String.prototype.split with object separator
    {
        embed::CallResult r = evalScript(
            "let sep = { toString() { return '-'; } };\n"
            "let parts = 'foo-bar-baz'.split(sep);\n"
            "parts.length === 3 && parts[0] === 'foo' && parts[1] === 'bar' && parts[2] === 'baz';\n");
        CHECK(!r.thrown);
        CHECK(r.value.asBool() == true);
    }

    // 5. RegExp.prototype.compile throws catchable TypeError
    {
        embed::CallResult r = evalScript(
            "let caught = false;\n"
            "try {\n"
            "  RegExp.prototype.compile('abc');\n"
            "} catch (e) {\n"
            "  caught = (e instanceof TypeError);\n"
            "}\n"
            "caught;\n");
        CHECK(!r.thrown);
        CHECK(r.value.asBool() == true);
    }

    // 6. Block-scoped var hoisting
    {
        embed::CallResult r1 = evalScript(
            "{\n"
            "  var blockVar1 = 123;\n"
            "}\n"
            "blockVar1 === 123;\n");
        CHECK(!r1.thrown);
        CHECK(r1.value.asBool() == true);

        embed::CallResult r2 = evalScript(
            "function testBlockVar() {\n"
            "  if (true) {\n"
            "    var inner = 456;\n"
            "  }\n"
            "  return inner;\n"
            "}\n"
            "testBlockVar() === 456;\n");
        CHECK(!r2.thrown);
        CHECK(r2.value.asBool() == true);
    }

    // 7. A trap's throw through deleteProperty reaches the host's caller;
    // setElement drops the write at the host boundary (embed.h).
    {
        embed::CallResult r1 = evalScript(
            "new Proxy({}, {\n"
            "  deleteProperty() { throw new Error('delete trap error'); }\n"
            "});\n");
        CHECK(!r1.thrown);
        embed::Persistent target1{r1.value};
        Value thrown;
        CHECK(runtime::rtTryCatch([&] { (void)embed::deleteProperty(target1.get(), "foo"); }, thrown));

        embed::CallResult r2 = evalScript(
            "new Proxy([], {\n"
            "  set() { throw new Error('set trap error'); }\n"
            "});\n");
        CHECK(!r2.thrown);
        embed::Persistent target2{r2.value};
        CHECK_FALSE(runtime::rtTryCatch(
            [&] { (void)embed::setElement(target2.get(), 0, embed::fromDouble(42.0)); }, thrown));
    }

    // 8. Loop property hoisting correctness: guarded nullable reads and interprocedural mutation
    {
        // Guarded nullable property read in while loop should not eagerly throw TypeError
        embed::CallResult r1 = evalScript(
            "function testNullableWhile(o) {\n"
            "  var count = 0;\n"
            "  while (o !== null) {\n"
            "    count += o.prop;\n"
            "  }\n"
            "  return count;\n"
            "}\n"
            "testNullableWhile(null);\n");
        CHECK(!r1.thrown);
        CHECK(r1.value.asNumber() == 0.0);

        // Guarded nullable property read in for loop with if guard
        embed::CallResult r2 = evalScript(
            "function testNullableFor(o) {\n"
            "  var sum = 0;\n"
            "  for (var i = 0; i < 3; i++) {\n"
            "    if (o !== null) {\n"
            "      sum += o.prop;\n"
            "    }\n"
            "  }\n"
            "  return sum;\n"
            "}\n"
            "testNullableFor(null);\n");
        CHECK(!r2.thrown);
        CHECK(r2.value.asNumber() == 0.0);

        // Interprocedural mutation: callee mutates property on global object
        embed::CallResult r3 = evalScript(
            "var gObj = { x: 1 };\n"
            "function mutateG() {\n"
            "  gObj.x = 2;\n"
            "}\n"
            "function testMutation(obj) {\n"
            "  var sum = 0;\n"
            "  for (var i = 0; i < 2; i++) {\n"
            "    mutateG();\n"
            "    sum += obj.x;\n"
            "  }\n"
            "  return sum;\n"
            "}\n"
            "testMutation(gObj);\n");
        CHECK(!r3.thrown);
        CHECK(r3.value.asNumber() == 4.0);
    }
}

TEST_CASE("debugger statement executes as no-op") {
    embed::CallResult r1 = evalScript("debugger; 42;");
    CHECK(!r1.thrown);
    CHECK(r1.value.asNumber() == 42.0);

    embed::CallResult r2 = evalScript(
        "function foo() {\n"
        "  debugger;\n"
        "  return 100;\n"
        "}\n"
        "foo();\n");
    CHECK(!r2.thrown);
    CHECK(r2.value.asNumber() == 100.0);
}

TEST_CASE("unicode identifier evaluation") {
    embed::CallResult r1 = evalScript("const π = 3.14; π;");
    CHECK(!r1.thrown);
    CHECK(r1.value.asNumber() == 3.14);

    embed::CallResult r2 = evalScript("let α = 10, café = 20; α + café;");
    CHECK(!r2.thrown);
    CHECK(r2.value.asNumber() == 30.0);

    embed::CallResult r3 = evalScript(
        "function area(r) {\n"
        "  const π = 3.14159;\n"
        "  return π * r * r;\n"
        "}\n"
        "area(2);\n");
    CHECK(!r3.thrown);
    CHECK(doctest::Approx(r3.value.asNumber()) == 12.56636);

    embed::CallResult r4 = evalScript("const obj = { café: 'espresso' }; obj.café;");
    CHECK(!r4.thrown);
    CHECK(embed::toUtf8(r4.value) == "espresso");
}

