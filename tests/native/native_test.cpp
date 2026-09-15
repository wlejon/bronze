// The JIT half of the native suite: every native in natives.h registered on
// this thread, then programs that call them compiled by evalScript against
// the registry and bound before they run. tests/native/harness.cpp is the
// same list through the ahead-of-time path (a manifest, `bronze build
// --emit-shared`, a loader that binds the module's import table).
//
// What is pinned here, and why each line is a test rather than a demo:
//
//  * every vocabulary type crosses the call in both directions, including the
//    two that are not a register (str, and each typed-array kind);
//  * a class round-trips: the constructor's void* comes back as a handle the
//    class's methods, getter, setter and destructor apply to, and a method
//    can hand back a handle of ANOTHER class the program then calls into;
//  * every refusal is a TypeError the program can catch, naming what was
//    expected and what arrived — never a crash inside the native;
//  * the registry refuses at registration, by message, and the bind step
//    refuses a module by name.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "embed/embed.h"
#include "eval/eval.h"
#include "natives.h"

using namespace bronze;
namespace embed = bronze::embed;
using eval::evalScript;

namespace {

// Registered once per process (the registry is per thread and doctest runs
// every case on this one), on first use so case order is free.
void ensureRegistered() {
    static bool done = false;
    if (done) return;
    done = true;
    std::string err;
    REQUIRE_MESSAGE(nt_natives::registerAll(err), err);

    // The namespace root, as a host global holding a plain object — the
    // arrangement a real host has (`bro` is an object with the non-native
    // surface on it). A compiled native call never reads it; a dynamic read
    // of `nt.something` the registry does not know lands on this object.
    embed::registerGlobal("nt", embed::createObject());

    // A JS-visible method on nt.Agent's prototype, the way a host installs
    // the part of a class that is not native. The compiled direct calls
    // never consult the prototype; a dynamic `a.describe()` does.
    embed::GlobalValue proto = embed::nativeClassPrototype("nt.Agent");
    REQUIRE(proto.found);
    embed::Persistent protoRoot{proto.value};
    embed::Persistent describe{embed::makeFunction(
        [](Value, std::span<const Value>) -> Value { return embed::fromUtf8("an agent"); }, 0,
        "describe")};
    embed::setProperty(protoRoot.get(), "describe", describe.get());
    embed::registerGlobal("AgentProto", protoRoot.get());

    // A host function that detaches a buffer, for the detached-argument case.
    embed::registerGlobal("detach", embed::makeFunction(
        [](Value, std::span<const Value> args) -> Value {
            if (!args.empty()) embed::detachArrayBuffer(args[0]);
            return Value::fromUndefined();
        }, 1, "detach"));
}

double number(const char* src) {
    ensureRegistered();
    embed::CallResult r = evalScript(src);
    if (r.thrown) {
        FAIL_CHECK("threw: " << embed::toUtf8(embed::getProperty(r.value, "message")) << " in: " << src);
        return -1;
    }
    REQUIRE_MESSAGE(embed::isNumber(r.value), src);
    return r.value.asNumber();
}

std::string text(const char* src) {
    ensureRegistered();
    embed::CallResult r = evalScript(src);
    if (r.thrown) {
        FAIL_CHECK("threw: " << embed::toUtf8(embed::getProperty(r.value, "message")) << " in: " << src);
        return "<threw>";
    }
    return embed::toUtf8(r.value);
}

// The message of the TypeError `src` throws; fails the case if it does not
// throw, or throws something else. The thrown value is ROOTED across the two
// property reads: each read allocates its key, and under BRONZE_GC_STRESS
// the first one moves the error object out from under a raw copy.
std::string typeErrorOf(const char* src) {
    ensureRegistered();
    embed::CallResult r = evalScript(src);
    if (!r.thrown) {
        FAIL_CHECK("did not throw: " << src);
        return "";
    }
    embed::Persistent thrown{r.value};
    const std::string name = embed::toUtf8(embed::getProperty(thrown.get(), "name"));
    CHECK_MESSAGE(name == "TypeError", src);
    return embed::toUtf8(embed::getProperty(thrown.get(), "message"));
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---- scalars ---------------------------------------------------------------

TEST_CASE("native scalars cross the call as doubles, int32s and bools") {
    CHECK(number("nt.math.add(2, 3)") == 5.0);
    CHECK(number("nt.math.add3(1, 2, 3.5)") == 6.5);
    CHECK(number("nt.math.bits(6, 3)") == 2.0);
    // i32 is ToInt32: a fractional double and a numeric string both convert.
    CHECK(number("nt.math.bits(6.9, '3')") == 2.0);
    CHECK(number("nt.math.bits(-1, 0xff)") == 255.0);
    CHECK(text("String(nt.math.flip(false))") == "true");
    CHECK(text("String(nt.math.flip(true))") == "false");
    CHECK(text("typeof nt.math.flip(0)") == "boolean");
}

TEST_CASE("a missing scalar argument arrives as zero, false or empty") {
    CHECK(number("nt.math.add(4)") == 4.0);
    CHECK(number("nt.math.add()") == 0.0);
    CHECK(text("String(nt.math.flip())") == "true");
    CHECK(number("nt.str.len()") == 0.0);
    // Extra arguments are evaluated and dropped, as JS drops them.
    CHECK(number("let side = 0; nt.math.add(1, 2, (side = 7)); side") == 7.0);
}

TEST_CASE("a native call is an expression like any other") {
    CHECK(number("nt.math.add(nt.math.add(1, 2), nt.math.add(3, 4))") == 10.0);
    CHECK(number("[1, 2, 3].map(x => nt.math.add(x, 10)).reduce((a, b) => a + b, 0)") == 36.0);
    CHECK(number("function twice(x) { return nt.math.add(x, x); } twice(21)") == 42.0);
}

// ---- strings ---------------------------------------------------------------

TEST_CASE("a str argument is the UTF-8 text and a str result is a string") {
    CHECK(text("nt.str.greet('bob')") == "hi, bob");
    CHECK(text("typeof nt.str.greet('x')") == "string");
    // Byte length, not code units: é is two UTF-8 bytes.
    CHECK(number("nt.str.len('h\\u00e9llo')") == 6.0);
    CHECK(number("nt.str.len('')") == 0.0);
    // ToString for a non-string argument.
    CHECK(number("nt.str.len(12345)") == 5.0);
    CHECK(text("nt.str.greet({ toString() { return 'obj'; } })") == "hi, obj");
    // Two str arguments in one program, and one native's text feeding another.
    CHECK(text("nt.str.greet(nt.str.greet('nested'))") == "hi, hi, nested");
}

// ---- dynamic ---------------------------------------------------------------

TEST_CASE("a dynamic parameter and result are the raw value") {
    CHECK(number("nt.dyn.identity({ a: 41 }).a + 1") == 42.0);
    CHECK(text("nt.dyn.identity('same')") == "same");
    CHECK(text("String(nt.dyn.identity(undefined))") == "undefined");
    CHECK(text("typeof nt.dyn.identity(() => 1)") == "function");
}

// ---- typed arrays ----------------------------------------------------------

TEST_CASE("each typed-array kind crosses as (pointer, length)") {
    CHECK(number("nt.arr.sumF32(new Float32Array([1.5, 2.5, 3]))") == 7.0);
    CHECK(number("nt.arr.sumF64(new Float64Array([0.25, 0.5]))") == 0.75);
    CHECK(number("nt.arr.sumI32(new Int32Array([-5, 10, 100000]))") == 100005.0);
    CHECK(number("nt.arr.sumU8(new Uint8Array([200, 100]))") == 300.0);
    CHECK(number("nt.arr.sumU16(new Uint16Array([65535, 1]))") == 65536.0);
    CHECK(number("nt.arr.sumU32(new Uint32Array([4294967295, 1]))") == 4294967296.0);
    CHECK(number("nt.arr.sumI8(new Int8Array([-128, 127]))") == -1.0);
    CHECK(number("nt.arr.sumI16(new Int16Array([-32768, 32767, 1]))") == 0.0);
    // An empty view is a zero length, not a refusal.
    CHECK(number("nt.arr.sumF32(new Float32Array(0))") == 0.0);
    // A view over part of a buffer crosses as that window.
    CHECK(number("const buf = new Float64Array([1, 2, 3, 4]).buffer;"
                 "nt.arr.sumF64(new Float64Array(buf, 16, 2))") == 7.0);
}

TEST_CASE("a native writes through a typed-array pointer") {
    CHECK(text("const v = new Float64Array(4); nt.arr.fill(v, 1.5); Array.from(v).join(',')") ==
          "0,1.5,3,4.5");
}

// ---- typed-array returns ---------------------------------------------------

TEST_CASE("a copy-mode return is a fresh JS-owned typed array of the declared kind") {
    CHECK(text("(function () { const r = nt.buf.rangeF32(4);"
               "return r.constructor.name + ':' + Array.from(r).join(','); })()") ==
          "Float32Array:0,1,2,3");
    CHECK(text("(function () { const s = nt.buf.squaresI32(5);"
               "return s.constructor.name + ':' + Array.from(s).join(','); })()") ==
          "Int32Array:0,1,4,9,16");
    // JS-owned: the native's scratch is overwritten by the next call and the
    // first array keeps its values; the buffer is an ordinary one.
    CHECK(text("(function () { const a = nt.buf.rangeF32(3); const b = nt.buf.rangeF32(2);"
               "return Array.from(a).join(',') + '|' + Array.from(b).join(',') + '|' +"
               "a.buffer.byteLength; })()") == "0,1,2|0,1|12");
    // The result is a value like any other, and a method answers one too.
    CHECK(number("nt.buf.squaresI32(4).reduce((x, y) => x + y, 0)") == 14.0);
    CHECK(text("(function () { const ag = new nt.Agent(40); ag.hit(4); const st = ag.stats();"
               "return st.constructor.name + ':' + st[0] + ':' + (st[1] === ag.id()); })()") ==
          "Float64Array:36:true");
}

TEST_CASE("an empty return is a zero-length typed array, not undefined") {
    CHECK(text("(function () { const e = nt.buf.empty();"
               "return e.constructor.name + ':' + e.length; })()") == "Float64Array:0");
}

TEST_CASE("a transfer-mode return views the native's block and releases it on collection") {
    ensureRegistered();
    const int madeBefore = nt_natives::g_bufMade;
    const int releasedBefore = nt_natives::g_bufReleased;
    CHECK(text("(function () { const o = nt.buf.ownedU8(6); globalThis.keptBuf = o;"
               "return o.constructor.name + ':' + Array.from(o).join(',') + ':' +"
               "o.buffer.byteLength; })()") == "Uint8Array:0,3,6,9,12,15:6");
    // Writes land in the native's block (the view is over it, not a copy).
    CHECK(number("keptBuf[2] = 200; new Uint8Array(keptBuf.buffer)[2]") == 200.0);
    CHECK(nt_natives::g_bufMade - madeBefore == 1);
    CHECK(nt_natives::g_bufReleased == releasedBefore);

    embed::CallResult r =
        evalScript("(function () { for (let i = 0; i < 8; i++) nt.buf.ownedU8(i + 1); })()");
    REQUIRE(!r.thrown);
    embed::collectGarbage();
    embed::collectGarbage();
    embed::drainFinalizers();
    // The eight the program dropped are released; the kept one is not.
    CHECK(nt_natives::g_bufReleased - releasedBefore >= 8);
    CHECK(number("keptBuf.length") == 6.0);
    CHECK(number("keptBuf[2]") == 200.0);

    CHECK(number("globalThis.keptBuf = undefined; 0") == 0.0);
    const int releasedKept = nt_natives::g_bufReleased;
    embed::collectGarbage();
    embed::collectGarbage();
    embed::drainFinalizers();
    CHECK(nt_natives::g_bufReleased > releasedKept);
}

TEST_CASE("a native that throws after filling a transfer descriptor still has its release run") {
    ensureRegistered();
    const int releasedBefore = nt_natives::g_bufReleased;
    CHECK(contains(typeErrorOf("nt.buf.throwAfterFill()"), "filled, then refused"));
    // Released synchronously by the wrap, no collection needed: the block
    // was never given to a buffer.
    CHECK(nt_natives::g_bufReleased - releasedBefore == 1);
    // And the program is left in order: the next buffer return works.
    CHECK(text("Array.from(nt.buf.rangeF32(2)).join(',')") == "0,1");
    CHECK(text("(function () { try { nt.buf.throwAfterFill(); return 'no throw'; }"
               "catch (e) { return e.message; } })()") == "filled, then refused");
    CHECK(nt_natives::g_bufReleased - releasedBefore == 2);
}

// ---- classes ---------------------------------------------------------------

TEST_CASE("a native class: constructor, method, getter, setter, zero-arg method") {
    CHECK(number("const a = new nt.Agent(10); a.hit(3)") == 7.0);
    CHECK(number("const a = new nt.Agent(10); a.hit(3); a.hp") == 7.0);
    CHECK(number("const a = new nt.Agent(10); a.hp = 20; a.hp") == 20.0);
    CHECK(number("const a = new nt.Agent(10); a.hp += 5; a.hp") == 15.0);
    CHECK(number("const a = new nt.Agent(10); a.hp -= 4") == 6.0);
    // Two instances, two receivers: the id is per object.
    CHECK(number("const a = new nt.Agent(1); const b = new nt.Agent(1); b.id() - a.id()") == 1.0);
    // A let binding reassigned to another instance follows the new one.
    CHECK(number("let a = new nt.Agent(1); a = new nt.Agent(50); a.hp") == 50.0);
    // Handles are objects the program can hold in structures and pass around.
    CHECK(number("const list = [new nt.Agent(3), new nt.Agent(4)];"
                 "let total = 0; for (const a of list) total += nt.peek(a); total") == 7.0);
}

TEST_CASE("a method can answer a handle of another class") {
    CHECK(number("const a = new nt.Agent(21); a.target().value()") == 42.0);
    CHECK(number("const a = new nt.Agent(21); const t = a.target(); t.value()") == 42.0);
    CHECK(number("const t = new nt.Target(5); t.value()") == 5.0);
    // A str-returning method.
    CHECK(text("const a = new nt.Agent(1); a.label().slice(0, 6)") == "agent#");
}

TEST_CASE("a function takes a class handle") {
    CHECK(number("nt.peek(new nt.Agent(9))") == 9.0);
    CHECK(number("const a = new nt.Agent(9); a.hit(2); nt.peek(a)") == 7.0);
}

TEST_CASE("a handle is born on the class prototype the host can extend") {
    CHECK(text("const a = new nt.Agent(1); String(Object.getPrototypeOf(a) === AgentProto)") ==
          "true");
    // A dynamic call reaches the JS-installed method through the prototype.
    CHECK(text("const a = new nt.Agent(1); a.describe()") == "an agent");
    CHECK(text("const a = new nt.Agent(1); String(a.target() instanceof Object)") == "true");
}

TEST_CASE("the class of a binding is known per function, not leaked across them") {
    // `a` in `g` is a parameter of unknown class: the read is a dynamic
    // property get, which the prototype does not answer. The same name in
    // `f` is a construction the compiler saw, so its read is the native
    // getter. A class table that leaked across functions would have made
    // g's read a direct native call on whatever it received.
    CHECK(number("function f() { const a = new nt.Agent(5); return a.hp; } f()") == 5.0);
    CHECK(text("function g(a) { return String(a.hp); } g(new nt.Agent(5))") == "undefined");
    CHECK(text("function f() { const a = new nt.Agent(5); return a.hp; }"
               "function g(a) { return String(a.hp); } f(); g({})") == "undefined");
}

TEST_CASE("a captured const keeps its class inside the closures that read it") {
    CHECK(number("const a = new nt.Agent(8); const f = () => a.hp; f()") == 8.0);
    CHECK(number("const a = new nt.Agent(8); function f() { return a.hit(3); } f(); f()") == 2.0);
    CHECK(number("function outer() { const a = new nt.Agent(4); return () => a.target().value(); }"
                 "outer()()") == 8.0);
    // A captured `let` is not: it could be reassigned from either side, so
    // its read is the dynamic path, which the prototype does not answer.
    CHECK(text("let a = new nt.Agent(8); const f = () => String(a.hp); f()") == "undefined");
}

TEST_CASE("the destructor runs when the collector reclaims the handle") {
    ensureRegistered();
    const int before = nt_natives::g_agentsFreed;
    embed::CallResult r = evalScript("(function () { for (let i = 0; i < 5; i++) new nt.Agent(i); })()");
    REQUIRE(!r.thrown);
    embed::collectGarbage();
    embed::collectGarbage();
    embed::drainFinalizers();
    CHECK(nt_natives::g_agentsFreed - before >= 5);
}

TEST_CASE("a handle the program still holds is not destroyed") {
    ensureRegistered();
    // Parked on globalThis, where a global PROPERTY (not a tracked binding)
    // holds it: reading it back goes through nt.peek's run-time tag check,
    // which is what a handle that reached the program through an untyped
    // path always gets.
    CHECK(number("const a = new nt.Agent(77); globalThis.keptAgent = a; a.id() > 0 ? 1 : 0") == 1.0);
    embed::collectGarbage();
    embed::collectGarbage();
    embed::drainFinalizers();
    // Other cases' garbage may be reclaimed here; what must NOT happen is the
    // kept one going: its data still answers.
    CHECK(number("nt.peek(keptAgent)") == 77.0);
    CHECK(text("keptAgent.describe()") == "an agent");
}

// ---- a namespace property --------------------------------------------------

TEST_CASE("a namespace property reads and writes through its getter and setter") {
    nt_natives::g_timeScale = 1.0;
    CHECK(number("nt.time.scale") == 1.0);
    CHECK(number("nt.time.scale = 2; nt.time.scale * 3") == 6.0);
    CHECK(nt_natives::g_timeScale == 2.0);
    CHECK(number("nt.time.scale += 1") == 3.0);
    CHECK(nt_natives::g_timeScale == 3.0);
    // The setter's effect is visible to the C side directly, and vice versa.
    nt_natives::g_timeScale = 0.5;
    CHECK(number("nt.time.scale * 4") == 2.0);
}

// ---- refusals at the call --------------------------------------------------

TEST_CASE("a TypeError thrown inside an argument's coercion is the program's, not the native's") {
    // The coercion runs user code before any pointer is taken, so a throw
    // there unwinds without the native having been entered.
    CHECK(contains(typeErrorOf("nt.math.bits({ valueOf() { throw new TypeError('inner'); } }, 1)"), "inner"));
}

TEST_CASE("a handle of the wrong class is a TypeError naming both classes") {
    const std::string msg = typeErrorOf("nt.peek(new nt.Target(1))");
    CHECK(contains(msg, "expected a nt.Agent handle"));
    CHECK(contains(msg, "got a nt.Target handle"));
}

TEST_CASE("a non-handle where a class is expected is a TypeError naming the class") {
    CHECK(contains(typeErrorOf("nt.peek({})"), "expected a nt.Agent handle, got an ordinary object"));
    CHECK(contains(typeErrorOf("nt.peek()"), "got undefined (missing argument)"));
    CHECK(contains(typeErrorOf("nt.peek(null)"), "got null"));
    CHECK(contains(typeErrorOf("nt.peek(3)"), "got a non-object"));
}

TEST_CASE("a typed array of another element kind is a TypeError naming both") {
    const std::string msg = typeErrorOf("nt.arr.sumF32(new Float64Array(2))");
    CHECK(contains(msg, "expected a Float32Array"));
    CHECK(contains(msg, "got a Float64Array"));
    CHECK(contains(typeErrorOf("nt.arr.sumU8(new Uint8ClampedArray(2))"), "got a Uint8ClampedArray"));
    CHECK(contains(typeErrorOf("nt.arr.sumI32([1, 2])"), "expected a Int32Array, got a non-typed-array object"));
    CHECK(contains(typeErrorOf("nt.arr.sumI32()"), "got undefined (missing argument)"));
}

TEST_CASE("a detached buffer is a TypeError, and the native is not called") {
    const std::string msg = typeErrorOf("const v = new Float32Array(2); detach(v.buffer); nt.arr.sumF32(v)");
    CHECK(contains(msg, "Float32Array"));
    CHECK(contains(msg, "detached"));
}

TEST_CASE("a TypeError from a native argument is catchable in the program") {
    CHECK(text("let r = 'no'; try { nt.peek({}); } catch (e) { r = e instanceof TypeError ? 'caught' : 'other'; } r") ==
          "caught");
    // And the program continues: the next native call works.
    CHECK(number("try { nt.peek({}); } catch (e) {} nt.math.add(1, 1)") == 2.0);
}

// ---- refusals at registration ----------------------------------------------

TEST_CASE("an unknown type spelling is refused at registration, by name") {
    ensureRegistered();
    std::string err;
    embed::NativeSignature sig;
    sig.returnType = "f64";
    sig.paramTypes = {"float"};
    CHECK(!embed::registerNative("nt.bad.fn", reinterpret_cast<void*>(&nt_natives::add), sig, &err));
    CHECK(contains(err, "float"));
    CHECK(contains(err, "nt.bad.fn"));

    sig.paramTypes = {"f64"};
    sig.returnType = "double";
    err.clear();
    CHECK(!embed::registerNative("nt.bad.fn", reinterpret_cast<void*>(&nt_natives::add), sig, &err));
    CHECK(contains(err, "double"));

    // Not registered by the refusal: the name is still free.
    for (const auto& name : embed::hostNativeNames()) CHECK(name != "nt.bad.fn");
}

TEST_CASE("a member of an unregistered class is refused") {
    ensureRegistered();
    std::string err;
    embed::NativeSignature sig;
    sig.kind = embed::NativeKind::Method;
    sig.className = "nt.Ghost";
    sig.returnType = "void";
    CHECK(!embed::registerNative("nt.Ghost.walk", reinterpret_cast<void*>(&nt_natives::add), sig, &err));
    CHECK(contains(err, "nt.Ghost"));
    CHECK(contains(err, "not a registered class"));
}

TEST_CASE("a bare name that is already a host global is refused") {
    ensureRegistered();
    embed::registerGlobal("clashName", embed::fromDouble(1.0));
    std::string err;
    embed::NativeSignature sig;
    sig.returnType = "f64";
    CHECK(!embed::registerNative("clashName", reinterpret_cast<void*>(&nt_natives::add), sig, &err));
    CHECK(contains(err, "clashName"));
    CHECK(contains(err, "host global"));
}

TEST_CASE("a duplicate registration is refused, and a typed-array return is accepted") {
    ensureRegistered();
    std::string err;
    embed::NativeSignature sig;
    sig.returnType = "f64";
    sig.paramTypes = {"f64", "f64"};
    CHECK(!embed::registerNative("nt.math.add", reinterpret_cast<void*>(&nt_natives::add), sig, &err));
    CHECK(contains(err, "already registered"));

    // The same path twice with a `T[]` return: the SECOND refusal is the
    // duplicate, not the return type, which proves the spelling passed
    // resolution (nt.buf.rangeF32 registered with it in registerAll).
    sig.returnType = "f32[]";
    sig.paramTypes = {"i32"};
    err.clear();
    CHECK(!embed::registerNative("nt.buf.rangeF32", reinterpret_cast<void*>(&nt_natives::rangeF32), sig,
                                 &err));
    CHECK(contains(err, "already registered"));
    CHECK(!contains(err, "parameter-only"));
}

// ---- the registry as the manifest ------------------------------------------

TEST_CASE("the manifest names every registration with its signature") {
    ensureRegistered();
    const std::string json = embed::nativeManifestJson();
    CHECK(contains(json, "\"version\": 1"));
    CHECK(contains(json, "\"path\": \"nt.math.add\""));
    CHECK(contains(json, "\"signature\": \"f64(f64,f64)\""));
    CHECK(contains(json, "\"path\": \"nt.Agent.hit\", \"kind\": \"method\", \"class\": \"nt.Agent\""));
    CHECK(contains(json, "\"signature\": \"f64(self,f64)\""));
    CHECK(contains(json, "\"path\": \"nt.Agent\", \"kind\": \"constructor\""));
    CHECK(contains(json, "\"returns\": \"nt.Target\""));
    CHECK(contains(json, "\"params\": [\"f32[]\"]"));

    const std::vector<embed::NativeEntry> entries = embed::hostNatives();
    bool sawSetter = false;
    for (const auto& e : entries) {
        if (e.path == "nt.time.scale" && e.signature.kind == embed::NativeKind::Setter) {
            sawSetter = true;
            CHECK(e.signatureText == "void(f64)");
            CHECK(e.fn == reinterpret_cast<void*>(&nt_natives::timeScaleSet));
        }
        if (e.path == "nt.Agent") {
            CHECK(e.signature.destructor == &nt_natives::agentFree);
        }
    }
    CHECK(sawSetter);
}

// ---- the bind step ---------------------------------------------------------

namespace {

// An import table in the layout a compiled module exports
// (bronze_abi.h, `<entry>_native_imports`): u32 count, u32 namesOffset,
// u64 slots[count], then count × ("name\0" "signature\0").
std::vector<uint64_t> makeImportTable(const std::vector<std::pair<std::string, std::string>>& imports) {
    const uint32_t count = static_cast<uint32_t>(imports.size());
    const uint32_t namesOffset = 8 + 8 * count;
    std::string names;
    for (const auto& [name, sig] : imports) {
        names += name;
        names.push_back('\0');
        names += sig;
        names.push_back('\0');
    }
    std::vector<uint64_t> table((namesOffset + names.size() + 7) / 8 + 1, 0);
    auto* bytes = reinterpret_cast<unsigned char*>(table.data());
    std::memcpy(bytes, &count, 4);
    std::memcpy(bytes + 4, &namesOffset, 4);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t unbound = 0xDEADBEEF;
        std::memcpy(bytes + 8 + 8 * i, &unbound, 8);
    }
    std::memcpy(bytes + namesOffset, names.data(), names.size());
    return table;
}

uint64_t slotOf(const std::vector<uint64_t>& table, uint32_t i) {
    uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const unsigned char*>(table.data()) + 8 + 8 * i, 8);
    return v;
}

}  // namespace

TEST_CASE("binding fills a table from the registry and names every gap") {
    ensureRegistered();
    std::vector<uint64_t> table = makeImportTable({
        {"function nt.math.add", "f64(f64,f64)"},
        {"function nt.nope.fn", "f64()"},
        {"function nt.math.bits", "i32(i32)"},
        {"class nt.Agent", ""},
        {"class nt.Nothing", ""},
        {"method nt.Agent.hit", "f64(self,f64)"},
        {"getter nt.time.scale", "f64()"},
    });
    std::vector<std::string> missing;
    CHECK(!embed::bindNativeImports(table.data(), &missing));
    REQUIRE(missing.size() == 3);
    CHECK(missing[0] == "function nt.nope.fn (not registered)");
    CHECK(contains(missing[1], "function nt.math.bits (module compiled against i32(i32), registered as i32(i32,i32))"));
    CHECK(missing[2] == "class nt.Nothing (no class of that name is registered)");

    // The slots it could fill, it filled; the ones it could not, it left.
    CHECK(slotOf(table, 0) == reinterpret_cast<uint64_t>(&nt_natives::add));
    CHECK(slotOf(table, 1) == 0xDEADBEEF);
    CHECK(slotOf(table, 2) == 0xDEADBEEF);
    CHECK(slotOf(table, 3) != 0);
    CHECK(slotOf(table, 3) != 0xDEADBEEF);
    CHECK(slotOf(table, 4) == 0xDEADBEEF);
    CHECK(slotOf(table, 5) == reinterpret_cast<uint64_t>(&nt_natives::agentHit));
    CHECK(slotOf(table, 6) == reinterpret_cast<uint64_t>(&nt_natives::timeScaleGet));

    // A complete table binds true, and binding twice is the same answer.
    std::vector<uint64_t> good = makeImportTable({
        {"function nt.math.add", "f64(f64,f64)"},
        {"class nt.Target", ""},
    });
    CHECK(embed::bindNativeImports(good.data(), &missing));
    CHECK(embed::bindNativeImports(good.data(), &missing));
    CHECK(slotOf(good, 0) == reinterpret_cast<uint64_t>(&nt_natives::add));
}

TEST_CASE("a native unregistered after the program was compiled is a refusal by name") {
    ensureRegistered();
    std::string err;
    embed::NativeSignature sig;
    sig.returnType = "f64";
    REQUIRE(embed::registerNative("nt.tmp.gone", reinterpret_cast<void*>(&nt_natives::absentPing), sig, &err));
    CHECK(number("nt.tmp.gone()") == 99.0);
    CHECK(embed::unregisterNative("nt.tmp.gone", embed::NativeKind::Function));

    // The registry no longer has it, so a table naming it cannot bind.
    std::vector<uint64_t> table = makeImportTable({{"function nt.tmp.gone", "f64()"}});
    std::vector<std::string> missing;
    CHECK(!embed::bindNativeImports(table.data(), &missing));
    REQUIRE(missing.size() == 1);
    CHECK(missing[0] == "function nt.tmp.gone (not registered)");

    // And a new program compiled now does not see it as a native: `nt.tmp`
    // is a dynamic read on the host's `nt` object, which has no such thing.
    embed::CallResult r = evalScript("nt.tmp.gone()");
    CHECK(r.thrown);
}

// ---- host-global cache beside natives --------------------------------------

TEST_CASE("a host-global read cached by a native-calling program follows re-registration") {
    ensureRegistered();
    embed::registerGlobal("nativeTick", embed::fromDouble(1.0));
    embed::CallResult def = evalScript("function tickPlus() { return nt.math.add(nativeTick, 100); }");
    REQUIRE(!def.thrown);
    CHECK(number("tickPlus()") == 101.0);
    CHECK(number("tickPlus()") == 101.0);
    embed::registerGlobal("nativeTick", embed::fromDouble(2.0));
    CHECK(number("tickPlus()") == 102.0);
}
