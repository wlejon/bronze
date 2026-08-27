// THE `--pins` WRITE BARRIERS, end to end (src/types/pins.h, stage B1).
//
// These are here rather than beside `tests/types/pins_test.cpp` — which is the
// manifest PARSER's unit test — and rather than in the oracle suite, for the
// same reason in both directions: what is asserted is a claim about a program
// COMPILED WITH A MANIFEST, and node has no manifests, so there is no oracle to
// be byte-identical to. The composition root that takes a manifest is the
// driver, so the driver is what these drive.
//
// Two kinds of assertion, and the pair is the point:
//
//   - the SHAPE, off `runIl`: which stores carry a `pin.guard` and — far more
//     important — which do not. A barrier where a proof already stands is the
//     tax this stage was chartered to avoid, so the absences are asserted as
//     hard as the presences.
//   - the BEHAVIOUR, off `runBuild` and the produced executable: the throw is
//     a TypeError, it is CATCHABLE, it names the manifest line, the program
//     keeps running afterwards, and the violating value was NOT stored.

#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "cli/driver.h"
#include "types/pins.h"

namespace {

// `BRONZE_NO_PIN_BARRIERS=1` is a MEASUREMENT instrument, not a contract
// configuration: it puts the compiler back where stage E5 left it, where a
// violating write is undefined behaviour. Every case below that asserts a
// barrier is asserting the thing the seam removes, so under the seam they are
// skipped rather than inverted — there is no second correct answer to check
// against, and the seam is read once per process so one run cannot have both.
// The cases that assert an ABSENCE stay on: a proof is still a licence with
// the seam set, and "no barrier here" had better not depend on it.
bool barriersOff() { return !bronze::types::pinBarriersEnabled(); }

std::filesystem::path workDir() {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "bronze_pin_barrier";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void write(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

// The IL of `source` compiled against `manifest`.
std::string ilWithPins(const std::string& name, const std::string& source,
                       const std::string& manifest) {
    const std::filesystem::path dir = workDir();
    const std::filesystem::path js = dir / (name + ".js");
    const std::filesystem::path pins = dir / (name + ".pins");
    write(js, source);
    write(pins, manifest);
    std::string il;
    const int status =
        bronze::cli::runIl(js.string(), &il, /*infer=*/true, /*hostGlobalsPath=*/{},
                           /*moduleRoots=*/{}, /*importMapPath=*/{}, /*inferStats=*/false,
                           /*assumeNoBigInt=*/false, pins.string());
    REQUIRE(status == 0);
    return il;
}

#if BRONZE_WITH_LLVM
std::string runOutput(const std::filesystem::path& exePath) {
    std::string result;
#ifdef _WIN32
    FILE* pipe = _popen(exePath.string().c_str(), "r");
#else
    FILE* pipe = popen(exePath.string().c_str(), "r");
#endif
    if (!pipe) return result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) result += buffer;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return result;
}

// Compile `source` against `manifest`, run it, and hand back everything it
// printed. A pin violation must never end the process, so a test that gets no
// output at all has found the failure this stage exists to prevent.
std::string buildAndRun(const std::string& name, const std::string& source,
                        const std::string& manifest) {
    const std::filesystem::path dir = workDir();
    const std::filesystem::path js = dir / (name + ".js");
    const std::filesystem::path pins = dir / (name + ".pins");
    const std::filesystem::path exe = dir / (name + ".exe");
    write(js, source);
    write(pins, manifest);
    std::error_code ec;
    std::filesystem::remove(exe, ec);

    std::string err;
    const int status = bronze::cli::runBuild(
        js.string(), exe.string(), &err, /*infer=*/true, /*timings=*/false, /*emitObj=*/false,
        /*hostGlobalsPath=*/{}, /*inferStats=*/false, /*statsOut=*/nullptr, /*moduleRoots=*/{},
        /*entrySymbol=*/{}, /*emitShared=*/false, /*retainFnSource=*/true, /*importMapPath=*/{},
        /*assumeNoBigInt=*/false, pins.string());
    REQUIRE_MESSAGE(status == 0, err);
    return runOutput(exe);
}
#endif

// The harness every behavioural program below shares: a violation has to be
// DATA, so that the assertions can be about the message and about the program
// still being alive after it.
const char* kReport =
    "function report(label, fn) {\n"
    "  try { fn(); console.log(label + ' NOTHROW'); }\n"
    "  catch (e) { console.log(label + ' ' + (e instanceof TypeError) + ' ' + e.message); }\n"
    "}\n";

}  // namespace

// ---- env-slot pins ----------------------------------------------------------

TEST_CASE("a violating write to a pinned env slot throws a TypeError naming the slot") {
    if (barriersOff()) return;
    const std::string src = std::string(kReport) +
                            "function makeState() {\n"
                            "  let cur = 0;\n"
                            "  function set(v) { cur = v; }\n"
                            "  function get() { return cur; }\n"
                            "  return { set: set, get: get };\n"
                            "}\n"
                            "const s = makeState();\n"
                            "s.set(7);\n"
                            "console.log('before ' + s.get());\n"
                            "report('slot', function () { s.set('boom'); });\n"
                            "console.log('after ' + s.get());\n";
    const std::string manifest = "function makeState.cur: number\n";

    const std::string il = ilWithPins("env_slot", src, manifest);
    CHECK(il.find("pin.guard") != std::string::npos);
    CHECK(il.find("\"function makeState.cur: number\"") != std::string::npos);

#if BRONZE_WITH_LLVM
    const std::string out = buildAndRun("env_slot", src, manifest);
    CHECK(out.find("before 7") != std::string::npos);
    CHECK(out.find("slot true pin 'function makeState.cur: number' violated") !=
          std::string::npos);
    CHECK(out.find("NOTHROW") == std::string::npos);
    // The store was DROPPED, not performed and then complained about: a
    // barrier that let the value through would leave the next raw read of the
    // slot reading a string pointer as a double, which is the whole failure
    // this stage exists to close.
    CHECK(out.find("after 7") != std::string::npos);
#endif
}

TEST_CASE("a slot the fixpoint proved carries no barrier") {
    // `n` is written only by `n + 1`, so `planEnvSlotNumberTypes` types it with
    // no manifest at all — and a manifest naming it changes nothing. The proof
    // is the licence, so there is nothing to check.
    const std::string src =
        "function counter() {\n"
        "  let n = 0;\n"
        "  function bump() { n = n + 1; }\n"
        "  function read() { return n; }\n"
        "  return { bump: bump, read: read };\n"
        "}\n"
        "const c = counter();\n"
        "c.bump();\n"
        "console.log(c.read());\n";
    CHECK(ilWithPins("env_proved", src, "function counter.n: number\n").find("pin.guard") ==
          std::string::npos);
}

// ---- field pins -------------------------------------------------------------

TEST_CASE("a violating write to a pinned field throws a TypeError naming the field") {
    if (barriersOff()) return;
    const std::string src = std::string(kReport) +
                            "class Vec { constructor(x) { this.x = x; } }\n"
                            "function put(v, y) { v.x = y; }\n"
                            "const v = new Vec(3);\n"
                            "put(v, 4);\n"
                            "console.log('before ' + v.x);\n"
                            "report('field', function () { put(v, 'boom'); });\n"
                            "console.log('after ' + v.x);\n";
    const std::string manifest = "Vec.x: number\n";

    const std::string il = ilWithPins("field_number", src, manifest);
    CHECK(il.find("pin.guard") != std::string::npos);
    CHECK(il.find("\"Vec.x: number\"") != std::string::npos);

#if BRONZE_WITH_LLVM
    const std::string out = buildAndRun("field_number", src, manifest);
    CHECK(out.find("before 4") != std::string::npos);
    CHECK(out.find("field true pin 'Vec.x: number' violated") != std::string::npos);
    CHECK(out.find("NOTHROW") == std::string::npos);
    CHECK(out.find("after 4") != std::string::npos);
#endif
}

TEST_CASE("a nullish-widened field admits null and undefined and refuses a string") {
    if (barriersOff()) return;
    const std::string src = std::string(kReport) +
                            "class Node { constructor() { this.limit = null; } }\n"
                            "function put(n, y) { n.limit = y; }\n"
                            "const n = new Node();\n"
                            "put(n, 5); put(n, null); put(n, undefined);\n"
                            "console.log('nullish-ok');\n"
                            "report('nullish', function () { put(n, 'boom'); });\n";
    const std::string manifest = "Node.limit: number-or-nullish\n";

    const std::string il = ilWithPins("field_nullish", src, manifest);
    CHECK(il.find("pin.guard") != std::string::npos);
    CHECK(il.find("number-or-nullish") != std::string::npos);

#if BRONZE_WITH_LLVM
    const std::string out = buildAndRun("field_nullish", src, manifest);
    CHECK(out.find("nullish-ok") != std::string::npos);
    CHECK(out.find("nullish true pin 'Node.limit: number-or-nullish' violated") !=
          std::string::npos);
    CHECK(out.find("NOTHROW") == std::string::npos);
#endif
}

TEST_CASE("a numeric-elements field refuses a non-array and its elements refuse a non-number") {
    if (barriersOff()) return;
    const std::string src =
        std::string(kReport) +
        "class M { constructor() { this.elements = [1, 2, 3]; } }\n"
        "function setAll(m, a) { m.elements = a; }\n"
        "function setOne(m, i, y) { const te = m.elements; te[i] = y; }\n"
        "const m = new M();\n"
        "setAll(m, [9, 8, 7]);\n"
        "setOne(m, 0, 5);\n"
        "console.log('before ' + m.elements[0]);\n"
        "report('whole', function () { setAll(m, 'boom'); });\n"
        "report('elem', function () { setOne(m, 0, 'boom'); });\n"
        "console.log('after ' + m.elements[0]);\n";
    const std::string manifest = "M.elements: numeric-elements\n";

    const std::string il = ilWithPins("field_elements", src, manifest);
    CHECK(il.find("dense-array") != std::string::npos);
    CHECK(il.find("<numeric-elements element>: number") != std::string::npos);

#if BRONZE_WITH_LLVM
    const std::string out = buildAndRun("field_elements", src, manifest);
    CHECK(out.find("before 5") != std::string::npos);
    CHECK(out.find("whole true pin 'M.elements: numeric-elements' violated") !=
          std::string::npos);
    CHECK(out.find("elem true pin '<numeric-elements element>: number' violated") !=
          std::string::npos);
    CHECK(out.find("NOTHROW") == std::string::npos);
    CHECK(out.find("after 5") != std::string::npos);
#endif
}

TEST_CASE("a field store the compiler already typed f64 carries no barrier") {
    // Every write is arithmetic, so the value reaching the store is an f64 IL
    // value and there is no question left to ask. This is the shape the pinned
    // kernels are made of, and it is why their measured tax is nothing.
    const std::string src =
        "class Vec { constructor(x) { this.x = x * 2; } }\n"
        "function bump(v, d) { v.x = v.x + d * 1.5; }\n"
        "const v = new Vec(3);\n"
        "bump(v, 2);\n"
        "console.log(v.x);\n";
    CHECK(ilWithPins("field_proved", src, "Vec.x: number\n").find("pin.guard") ==
          std::string::npos);
}

// ---- signature pins ---------------------------------------------------------

TEST_CASE("a violating argument through the boxed wrapper throws and names the parameter") {
    if (barriersOff()) return;
    // `apply` reaches `scale` through a function VALUE, so the call is the
    // uniform one and the boxed wrapper is the door the argument comes
    // through. Before this stage the wrapper ran ToNumber and `scale('4')`
    // quietly saw 4.
    const std::string src = std::string(kReport) +
                            "function scale(k) { return k * 2; }\n"
                            "function apply(f, x) { return f(x); }\n"
                            "console.log('before ' + apply(scale, 3));\n"
                            "report('param', function () { apply(scale, 'boom'); });\n"
                            "console.log('after ' + apply(scale, 3));\n";
    const std::string manifest = "param scale(k): number\n";

#if BRONZE_WITH_LLVM
    const std::string out = buildAndRun("param_pin", src, manifest);
    CHECK(out.find("before 6") != std::string::npos);
    CHECK(out.find("param true pin 'param scale(k): number' violated") != std::string::npos);
    CHECK(out.find("NOTHROW") == std::string::npos);
    CHECK(out.find("after 6") != std::string::npos);
#endif
}

TEST_CASE("a violating return throws and names the return pin") {
    if (barriersOff()) return;
    const std::string src = std::string(kReport) +
                            "function pick(flag) {\n"
                            "  if (flag) { return 'boom'; }\n"
                            "  return 41;\n"
                            "}\n"
                            "function use(f) { return f(0) + 1; }\n"
                            "console.log('before ' + use(pick));\n"
                            "report('return', function () { pick(1); });\n"
                            "console.log('after ' + use(pick));\n";
    const std::string manifest = "return pick: number\n";

    const std::string il = ilWithPins("return_pin", src, manifest);
    CHECK(il.find("\"return pick: number\"") != std::string::npos);

#if BRONZE_WITH_LLVM
    const std::string out = buildAndRun("return_pin", src, manifest);
    CHECK(out.find("before 42") != std::string::npos);
    CHECK(out.find("return true pin 'return pick: number' violated") != std::string::npos);
    CHECK(out.find("NOTHROW") == std::string::npos);
    CHECK(out.find("after 42") != std::string::npos);
#endif
}

// ---- what a pinned array's stores are made of -------------------------------

TEST_CASE("a pinned element read on the right of a store reads raw") {
    // `te[ i ] = me[ i ]` is the whole body of a matrix copy, and the RAW form
    // is reached from the COERCING positions — the value side of an assignment
    // is not one, so this read took the property cache while the identical
    // expression one line above it took the load.
    const std::string src =
        "class M { constructor() { this.a = [1, 2, 3]; this.b = [4, 5, 6]; } }\n"
        "function move(m) { const x = m.a; const y = m.b; y[0] = x[0]; y[1] = x[1]; }\n"
        "const m = new M();\n"
        "move(m);\n"
        "console.log(m.b[0] + ' ' + m.b[1]);\n";
    const std::string manifest = "M.a: numeric-elements\nM.b: numeric-elements\n";

    const std::string il = ilWithPins("elem_rhs_raw", src, manifest);
    // Two reads, both raw — and nothing left for a barrier to ask about, which
    // is the half that says the proof was really spent rather than re-checked.
    size_t reads = 0;
    for (size_t at = il.find("elem.get.typed"); at != std::string::npos;
         at = il.find("elem.get.typed", at + 1)) {
        ++reads;
    }
    CHECK(reads == 2);
    // The constructor's `this.a = [...]` still carries the FIELD half of the
    // pin. What must be gone is the ELEMENT half: an f64 value satisfies the
    // claim by its IL type, so the four accesses in `move` ask nothing.
    CHECK(il.find("<numeric-elements element>: number") == std::string::npos);

#if BRONZE_WITH_LLVM
    CHECK(buildAndRun("elem_rhs_raw", src, manifest).find("1 2") != std::string::npos);
#endif
}

TEST_CASE("a guarded element store spends its guard on the raw store") {
    if (barriersOff()) return;
    // The value here IS dynamic — it arrives through a parameter two call sites
    // disagree about — so the barrier is emitted. What it leaves behind is a
    // proof: `pin.guard` throws on anything but a Number, so the store on its
    // other side may take the raw form instead of the dynamic element ladder it
    // used to take after asking the very same question.
    const std::string src =
        std::string(kReport) +
        "class M { constructor() { this.a = [1, 2, 3]; } }\n"
        "function put(m, y) { const te = m.a; te[0] = y; }\n"
        "const m = new M();\n"
        "put(m, 7);\n"
        "console.log('before ' + m.a[0]);\n"
        "report('elem', function () { put(m, 'boom'); });\n"
        "console.log('after ' + m.a[0]);\n";
    const std::string manifest = "M.a: numeric-elements\n";

    const std::string il = ilWithPins("elem_guard_raw", src, manifest);
    CHECK(il.find("<numeric-elements element>: number") != std::string::npos);
    CHECK(il.find("elem.set.typed") != std::string::npos);
    // A plain `elem.set` here would mean the barrier asked and then threw the
    // answer away.
    CHECK(il.find("elem.set %") == std::string::npos);

#if BRONZE_WITH_LLVM
    const std::string out = buildAndRun("elem_guard_raw", src, manifest);
    CHECK(out.find("before 7") != std::string::npos);
    CHECK(out.find("elem true pin '<numeric-elements element>: number' violated") !=
          std::string::npos);
    CHECK(out.find("NOTHROW") == std::string::npos);
    // The violating value was not stored: the guard runs BEFORE the unbox.
    CHECK(out.find("after 7") != std::string::npos);
#endif
}

// ---- the seam ---------------------------------------------------------------

TEST_CASE("the pin text a barrier names reads back as a line of the manifest") {
    if (barriersOff()) return;
    // The message exists to be grepped for in the file that caused it, so the
    // module linker's `modN.` prefix must not be in it. One module here, so
    // the names are bare; `tests/modules` covers the prefixed form.
    // Two call sites disagreeing about `y` is what keeps the E4 parameter proof
    // from typing it f64 — a store the proof covers needs no barrier, so a
    // one-call-site version of this program emits nothing to inspect.
    const std::string il = ilWithPins("pin_text",
                                      "class Vec { constructor(x) { this.x = x; } }\n"
                                      "function put(v, y) { v.x = y; }\n"
                                      "const v = new Vec(1);\n"
                                      "put(v, 2);\n"
                                      "put(v, 'nope');\n"
                                      "console.log(v.x);\n",
                                      "Vec.x: number\n");
    size_t guards = 0;
    for (size_t at = il.find("pin.guard"); at != std::string::npos;
         at = il.find("pin.guard", at + 1)) {
        ++guards;
        const std::string line = il.substr(at, il.find('\n', at) - at);
        const bool namesTheManifestLine = line.find("\"Vec.x: number\"") != std::string::npos;
        CHECK(namesTheManifestLine);
    }
    CHECK(guards > 0);
}
