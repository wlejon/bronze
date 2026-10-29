#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "il/print.h"
#include "lower_fixture.h"

using namespace bronze;
using bronze::lower_test::inferAndLower;
using bronze::lower_test::parseAndLower;

namespace {

const il::Function* findFunction(const il::Module& mod, const std::string& name) {
    for (const auto& fn : mod.functions) {
        if (fn.name == name) return &fn;
    }
    return nullptr;
}

bool blockHasPropGet(const il::Block& blk) {
    for (const auto& inst : blk.instructions) {
        if (inst.op == il::Op::PropGet) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("loop property hoisting does not speculatively hoist guarded nullable property read") {
    // Issue 1: `while (o !== null) { o.prop; }` with nullable receiver `o`.
    // The block containing `o.prop` is in the body, which is NOT guaranteed to execute.
    // Speculatively hoisting it into the preheader would cause eager TypeError when o is null.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function test(o) {\n"
        "  while (o !== null) {\n"
        "    let x = o.prop;\n"
        "  }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    // Block 0 is entry/preheader jumping to header b1.
    // b0 must NOT contain PropGet!
    CHECK_FALSE(blockHasPropGet(fn->blocks[0]));

    // PropGet must remain in a subsequent body block
    bool foundInBody = false;
    for (size_t b = 1; b < fn->blocks.size(); ++b) {
        if (blockHasPropGet(fn->blocks[b])) {
            foundInBody = true;
            break;
        }
    }
    CHECK(foundInBody);
}

TEST_CASE("loop property hoisting does not hoist property read guarded by conditional if") {
    // `for (...) { if (o !== null) o.prop; }`
    // `o.prop` is inside the conditional block, not guaranteed to execute.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function test(o) {\n"
        "  for (let i = 0; i < 10; i++) {\n"
        "    if (o !== null) {\n"
        "      let x = o.prop;\n"
        "    }\n"
        "  }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    // Preheader is block 0 (entry before the loop). It must NOT contain PropGet.
    CHECK_FALSE(blockHasPropGet(fn->blocks[0]));
}

TEST_CASE("loop property hoisting hoists invariant property read on provably non-nullish object") {
    // `o` is a known newly allocated object, provably non-nullish.
    // `o.prop` can be safely hoisted into preheader.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function test(cond) {\n"
        "  let m = Math;\n"
        "  let sum = 0;\n"
        "  while (cond) {\n"
        "    sum += m.PI;\n"
        "  }\n"
        "  return sum;\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    INFO(il::print(*optMod));
    CHECK(blockHasPropGet(fn->blocks[0]));
}

TEST_CASE("loop property hoisting hoists property read in loop header before conditional branch") {
    // `while (o.prop > 0) { ... }`
    // `o.prop` is in the loop header block, which is guaranteed to execute whenever the loop is entered.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function test(o) {\n"
        "  while (o.prop > 0) {\n"
        "    let y = 1;\n"
        "  }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    // Hoisted to preheader (b0)
    CHECK(blockHasPropGet(fn->blocks[0]));
}

TEST_CASE("loop property hoisting detects interprocedural aliasing mutations") {
    // Issue 2: A callee mutates property "x". The receiver `obj` is passed into `test`,
    // so it is NOT a proven non-escaping local object. The callee could mutate `obj.x`
    // via global or heap aliasing. Property hoisting must NOT hoist `obj.x` across the call.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "let g = { x: 1 };\n"
        "function mutateX() {\n"
        "  g.x = 99;\n"
        "}\n"
        "function test(obj) {\n"
        "  for (let i = 0; i < 10; i++) {\n"
        "    mutateX();\n"
        "    let val = obj.x;\n"
        "  }\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    // Preheader of loop in `test` must NOT contain PropGet for x!
    CHECK_FALSE(blockHasPropGet(fn->blocks[0]));
}

TEST_CASE("loop property hoisting permits hoisting for proven non-escaping local object across call mutating same key") {
    // When `local` is a newly created local object that does NOT escape,
    // a call that mutates "x" on other objects cannot mutate `local.x`.
    // Hoisting is permitted.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "let g = { x: 1 };\n"
        "function mutateX() {\n"
        "  g.x = 99;\n"
        "}\n"
        "function getX() { return 10; }\n"
        "function test() {\n"
        "  let local = { x: getX() };\n"
        "  let sum = 0;\n"
        "  for (let i = 0; i < 10; i++) {\n"
        "    mutateX();\n"
        "    sum += local.x;\n"
        "  }\n"
        "  return sum;\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "test");
    REQUIRE(fn != nullptr);

    // Inside the loop, local.x was hoisted out, so no PropGet remains in the loop blocks!
    bool loopBodyHasPropGet = false;
    for (size_t b = 1; b < fn->blocks.size(); ++b) {
        if (blockHasPropGet(fn->blocks[b])) loopBodyHasPropGet = true;
    }
    CHECK_FALSE(loopBodyHasPropGet);
}

TEST_CASE("module-wide reflection does not disable hoisting in independent functions") {
    // Issue 3: Mentioning Proxy or reflection keys in one function must not disable
    // loop property hoisting in independent functions that do not touch reflection.
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = parseAndLower(
        "function usesReflection() {\n"
        "  let p = new Proxy({}, {});\n"
        "  return p;\n"
        "}\n"
        "function independent(cond) {\n"
        "  let m = Math;\n"
        "  let sum = 0;\n"
        "  while (cond) {\n"
        "    sum += m.PI;\n"
        "  }\n"
        "  return sum;\n"
        "}\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "independent");
    REQUIRE(fn != nullptr);

    // Hoisting should have succeeded in independent function's preheader (b0)
    CHECK(blockHasPropGet(fn->blocks[0]));
}

TEST_CASE("loop property hoisting proves non-nullishness across module env slots and direct call args") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "class Base {}\n"
        "const shared = new Base();\n"
        "function sumProps(o, n) {\n"
        "  let s = 0;\n"
        "  for (let i = 0; i < n; i++) {\n"
        "    s = s + o.a + o.b + shared.c;\n"
        "  }\n"
        "  return s;\n"
        "}\n"
        "const obj = { a: 1, b: 2 };\n"
        "console.log(sumProps(obj, 100));\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* fn = findFunction(*optMod, "sumProps");
    REQUIRE(fn != nullptr);

    // All three loop-invariant property reads (`o.a`, `o.b`, `shared.c`) are hoisted to b0.
    size_t preheaderPropGets = 0;
    for (const auto& inst : fn->blocks[0].instructions) {
        if (inst.op == il::Op::PropGet) ++preheaderPropGets;
    }
    CHECK(preheaderPropGets == 3);
    for (size_t b = 1; b < fn->blocks.size(); ++b) {
        CHECK_FALSE(blockHasPropGet(fn->blocks[b]));
    }
}

TEST_CASE("TypedArray and plain-array numeric loops type sum as f64 and CSE ElemGetTyped") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    auto astMod = lower_test::parseOnly(
        "function overTyped(rounds) {\n"
        "  const a = new Float32Array(16);\n"
        "  let sum = 0;\n"
        "  for (let r = 0; r < rounds; r++) {\n"
        "    for (let i = 0; i < 16; i++) {\n"
        "      sum = sum + a[i];\n"
        "      a[i] = a[i] * 1.5;\n"
        "    }\n"
        "  }\n"
        "  return sum;\n"
        "}\n"
        "function overPlain(rounds) {\n"
        "  const a = [];\n"
        "  for (let i = 0; i < 16; i++) a.push(i * 0.5);\n"
        "  let sum = 0;\n"
        "  for (let r = 0; r < rounds; r++) {\n"
        "    for (let i = 0; i < 16; i++) {\n"
        "      sum = sum + a[i];\n"
        "      a[i] = a[i] * 1.5;\n"
        "    }\n"
        "  }\n"
        "  return sum;\n"
        "}\n"
        "console.log(overTyped(4), overPlain(4));\n",
        diags, buf);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(astMod != nullptr);
    auto inferred = types::inferModule(*astMod, diags);
    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(inferred.has_value());
    const auto optMod = lower::lowerModule(*astMod, diags, &*inferred, nullptr, nullptr, nullptr,
                                           /*assumeNoBigInt=*/true);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* typedFn = findFunction(*optMod, "overTyped");
    REQUIRE(typedFn != nullptr);
    CHECK(typedFn->returnType == il::Type::F64);

    size_t typedGets = 0;
    for (const auto& blk : typedFn->blocks) {
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::ElemGetTyped) ++typedGets;
        }
    }
    CHECK(typedGets == 1);

    const il::Function* plainFn = findFunction(*optMod, "overPlain");
    REQUIRE(plainFn != nullptr);
    size_t fastElemGets = 0;
    for (const auto& blk : plainFn->blocks) {
        if (blk.copyClass != il::CopyClass::Fast) continue;
        for (const auto& inst : blk.instructions) {
            if (inst.op == il::Op::ElemGetTyped) ++fastElemGets;
        }
    }
    // Two: a plain array's typed read is ToNumber of the element
    // (il::elemGetCoerces), and an element with a `valueOf` is coerced once
    // per read, so the fast copy does not share the two the way it shares a
    // view's raw loads above.
    CHECK(fastElemGets == 2);
}

TEST_CASE("static-slot intra-block CSE and store-to-load forwarding eliminate redundant PropGet and Unbox") {
    DiagnosticSink diags;
    SourceBuffer buf("test.ts", "");
    const auto optMod = inferAndLower(
        "class Node {\n"
        "  constructor(x, y) {\n"
        "    this.x = x;\n"
        "    this.y = y;\n"
        "  }\n"
        "}\n"
        "class Acc {\n"
        "  constructor() {\n"
        "    this.visited = 0;\n"
        "    this.sumX = 0;\n"
        "  }\n"
        "}\n"
        "function step(node, acc) {\n"
        "  node.x = node.x * 0.99 + node.y * 0.01;\n"
        "  node.y = node.y * 0.99 - node.x * 0.01;\n"
        "  acc.visited = acc.visited + 1;\n"
        "  acc.sumX = acc.sumX + node.x;\n"
        "  return acc.visited + acc.sumX;\n"
        "}\n"
        "const n = new Node(1.0, 2.0);\n"
        "const a = new Acc();\n"
        "console.log(step(n, a));\n",
        diags, buf);

    REQUIRE_FALSE(diags.hasErrors());
    REQUIRE(optMod.has_value());

    const il::Function* stepFn = findFunction(*optMod, "step");
    REQUIRE(stepFn != nullptr);
    CHECK(stepFn->returnType == il::Type::F64);

    // `node.x`, `node.y`, `acc.visited`, and `acc.sumX` are each loaded once in b0;
    // subsequent reads after stores in the same block are forwarded directly.
    size_t propGets = 0;
    size_t rawUnboxes = 0;
    for (const auto& inst : stepFn->blocks[0].instructions) {
        if (inst.op == il::Op::PropGet) ++propGets;
        if (inst.op == il::Op::Unbox && inst.rawUnbox) ++rawUnboxes;
    }
    CHECK(propGets == 4);
    CHECK(rawUnboxes == 4);
}

TEST_CASE("store-to-load forwarding does not carry a literal's store across an object spread") {
    // `{ from: -1, ...p }` stores `from` and then the spread may redefine it,
    // so the read after the spread must stay a read: forwarded, it answers -1
    // whatever `p.from` is. Inferred and dynamic lowering both.
    const char* src =
        "function lit(p) { const P = { from: -1, ...p }; return P.from; }\n"
        "console.log(lit({ from: 5 }));\n";
    for (const bool infer : {true, false}) {
        CAPTURE(infer);
        DiagnosticSink diags;
        SourceBuffer buf("test.ts", "");
        const auto optMod = infer ? inferAndLower(src, diags, buf) : parseAndLower(src, diags, buf);
        REQUIRE_FALSE(diags.hasErrors());
        REQUIRE(optMod.has_value());

        const il::Function* fn = findFunction(*optMod, "lit");
        REQUIRE(fn != nullptr);
        bool spreadSeen = false;
        bool readAfterSpread = false;
        for (const auto& blk : fn->blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::ObjectSpread) spreadSeen = true;
                if (spreadSeen && inst.op == il::Op::PropGet) readAfterSpread = true;
            }
        }
        CHECK(spreadSeen);
        CHECK(readAfterSpread);
    }
}

TEST_CASE("TypedArray in-bounds proof and length hoisting are disabled across coercion, setter, Proxy, and buffer resize/transfer hazards") {
    // 1. Pure numeric loop keeps kElemFlagProvenInBounds and hoists a.length to preheader.
    {
        DiagnosticSink diags;
        SourceBuffer buf("test.ts", "");
        const auto optMod = inferAndLower(
            "function pureSum(n) {\n"
            "  const a = new Uint8Array(64);\n"
            "  let s = 0;\n"
            "  for (let i = 0; i < a.length; i++) {\n"
            "    s += a[i];\n"
            "  }\n"
            "  return s;\n"
            "}\n"
            "console.log(pureSum(1));\n",
            diags, buf);
        REQUIRE_FALSE(diags.hasErrors());
        REQUIRE(optMod.has_value());
        const il::Function* fn = findFunction(*optMod, "pureSum");
        REQUIRE(fn != nullptr);
        CHECK(blockHasPropGet(fn->blocks[0]));
        bool foundProvenInBounds = false;
        for (const auto& blk : fn->blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::ElemGetTyped &&
                    (inst.immI32 & il::kElemFlagProvenInBounds) != 0) {
                    foundProvenInBounds = true;
                }
            }
        }
        CHECK(foundProvenInBounds);
    }

    // 2. Unary +evil coercion in loop body strips kElemFlagProvenInBounds and prevents hoisting a.length.
    {
        DiagnosticSink diags;
        SourceBuffer buf("test.ts", "");
        const auto optMod = inferAndLower(
            "const ab = new ArrayBuffer(64);\n"
            "const a = new Uint8Array(ab);\n"
            "function sumWithCoercion(evil) {\n"
            "  let s = 0;\n"
            "  for (let i = 0; i < a.length; i++) {\n"
            "    if (i === 0) +evil;\n"
            "    s += a[i];\n"
            "  }\n"
            "  return s;\n"
            "}\n"
            "console.log(sumWithCoercion(0), sumWithCoercion({ valueOf() { ab.transfer(); return 0; } }));\n",
            diags, buf);
        REQUIRE_FALSE(diags.hasErrors());
        REQUIRE(optMod.has_value());
        const il::Function* fn = findFunction(*optMod, "sumWithCoercion");
        REQUIRE(fn != nullptr);
        CHECK_FALSE(blockHasPropGet(fn->blocks[0]));
        for (const auto& blk : fn->blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::ElemGetTyped) {
                    CHECK((inst.immI32 & il::kElemFlagProvenInBounds) == 0);
                }
            }
        }
    }

    // 3. Property setter / Proxy read / resize in loop body strips kElemFlagProvenInBounds and prevents hoisting a.length.
    {
        DiagnosticSink diags;
        SourceBuffer buf("test.ts", "");
        const auto optMod = inferAndLower(
            "const rab = new ArrayBuffer(64, { maxByteLength: 128 });\n"
            "const a = new Uint8Array(rab);\n"
            "function sumWithSetter(sink) {\n"
            "  let s = 0;\n"
            "  for (let i = 0; i < a.length; i++) {\n"
            "    if (i === 1) sink.x = i;\n"
            "    s += a[i];\n"
            "  }\n"
            "  return s;\n"
            "}\n"
            "console.log(sumWithSetter({ x: 0 }));\n",
            diags, buf);
        REQUIRE_FALSE(diags.hasErrors());
        REQUIRE(optMod.has_value());
        const il::Function* fn = findFunction(*optMod, "sumWithSetter");
        REQUIRE(fn != nullptr);
        CHECK_FALSE(blockHasPropGet(fn->blocks[0]));
        for (const auto& blk : fn->blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::ElemGetTyped) {
                    CHECK((inst.immI32 & il::kElemFlagProvenInBounds) == 0);
                }
            }
        }
    }
}

TEST_CASE("typed element update out[a[i] & 255]++ and affine matmul indices lower to proven ElemGetTyped/ElemSetTyped (#154)") {
    {
        DiagnosticSink diags;
        SourceBuffer buf("test.ts", "");
        const auto optMod = inferAndLower(
            "function hist(a) {\n"
            "  const out = new Int32Array(256);\n"
            "  for (let i = 0; i < a.length; i++) {\n"
            "    out[a[i] & 255]++;\n"
            "  }\n"
            "  return out;\n"
            "}\n"
            "console.log(hist(new Uint8Array(4096)));\n",
            diags, buf);
        REQUIRE_FALSE(diags.hasErrors());
        REQUIRE(optMod.has_value());
        const il::Function* fn = findFunction(*optMod, "hist");
        REQUIRE(fn != nullptr);
        CHECK(blockHasPropGet(fn->blocks[0]));
        size_t typedGets = 0;
        size_t typedSets = 0;
        for (const auto& blk : fn->blocks) {
            for (const auto& inst : blk.instructions) {
                CHECK(inst.op != il::Op::ElemGet);
                CHECK(inst.op != il::Op::ElemSet);
                CHECK(inst.op != il::Op::NumericStep);
                if (inst.op == il::Op::ElemGetTyped) {
                    ++typedGets;
                    CHECK((inst.immI32 & il::kElemFlagProvenIntegralIndex) != 0);
                    CHECK((inst.immI32 & il::kElemFlagProvenInBounds) != 0);
                } else if (inst.op == il::Op::ElemSetTyped) {
                    ++typedSets;
                    CHECK((inst.immI32 & il::kElemFlagProvenIntegralIndex) != 0);
                    CHECK((inst.immI32 & il::kElemFlagProvenInBounds) != 0);
                }
            }
        }
        CHECK(typedGets >= 2);
        CHECK(typedSets >= 1);
    }

    {
        DiagnosticSink diags;
        SourceBuffer buf("test.ts", "");
        const auto optMod = inferAndLower(
            "function matmul() {\n"
            "  const n = 192;\n"
            "  const a = new Float64Array(n * n);\n"
            "  const b = new Float64Array(n * n);\n"
            "  const c = new Float64Array(n * n);\n"
            "  for (let i = 0; i < n; i++) {\n"
            "    for (let k = 0; k < n; k++) {\n"
            "      const aik = a[i * n + k];\n"
            "      for (let j = 0; j < n; j++) {\n"
            "        c[i * n + j] += aik * b[k * n + j];\n"
            "      }\n"
            "    }\n"
            "  }\n"
            "  return c;\n"
            "}\n"
            "console.log(matmul());\n",
            diags, buf);
        REQUIRE_FALSE(diags.hasErrors());
        REQUIRE(optMod.has_value());
        const il::Function* fn = findFunction(*optMod, "matmul");
        REQUIRE(fn != nullptr);
        size_t typedGets = 0;
        size_t typedSets = 0;
        for (const auto& blk : fn->blocks) {
            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::ElemGetTyped) {
                    ++typedGets;
                    CHECK((inst.immI32 & il::kElemFlagProvenIntegralIndex) != 0);
                } else if (inst.op == il::Op::ElemSetTyped) {
                    ++typedSets;
                    CHECK((inst.immI32 & il::kElemFlagProvenIntegralIndex) != 0);
                }
            }
        }
        CHECK(typedGets >= 3);
        CHECK(typedSets >= 1);
    }

    // Parameterized blur/mat/scalar inside an IIFE prove integral-index, in-bounds,
    // knownConstInt parameter facts, and raw ToInt32 bitwise conversions.
    {
        DiagnosticSink diags;
        SourceBuffer buf("test.ts", "");
        const auto optMod = inferAndLower(
            "(function() {\n"
            "  const W = 512, H = 512;\n"
            "  const src = new Float32Array(W * H);\n"
            "  const dst = new Float32Array(W * H);\n"
            "  function blur(s, d, w, h) {\n"
            "    for (let y = 1; y < h - 1; y++) {\n"
            "      const row = y * w;\n"
            "      for (let x = 1; x < w - 1; x++) {\n"
            "        const i = row + x;\n"
            "        d[i] = (s[i - w] + s[i - 1] + s[i] + s[i + 1] + s[i + w]) * 0.2;\n"
            "      }\n"
            "    }\n"
            "  }\n"
            "  const N = 192;\n"
            "  const ma = new Float64Array(N * N);\n"
            "  const mb = new Float64Array(N * N);\n"
            "  const mc = new Float64Array(N * N);\n"
            "  function mat(a, b, c, n) {\n"
            "    for (let i = 0; i < n; i++) {\n"
            "      for (let k = 0; k < n; k++) {\n"
            "        const aik = a[i * n + k];\n"
            "        for (let j = 0; j < n; j++) {\n"
            "          c[i * n + j] += aik * b[k * n + j];\n"
            "        }\n"
            "      }\n"
            "    }\n"
            "  }\n"
            "  function scalar(x, y) {\n"
            "    let acc = 0;\n"
            "    for (let i = 0; i < x.length; i++) {\n"
            "      const u = (x[i] ^ y[i]) + (x[i] & y[i]);\n"
            "      acc = (acc + (u >>> 3)) | 0;\n"
            "    }\n"
            "    return acc;\n"
            "  }\n"
            "  const sx = new Int32Array(4096);\n"
            "  const sy = new Int32Array(4096);\n"
            "  blur(src, dst, W, H);\n"
            "  mat(ma, mb, mc, N);\n"
            "  console.log(dst[0], mc[0], scalar(sx, sy));\n"
            "})();\n",
            diags, buf);
        REQUIRE_FALSE(diags.hasErrors());
        REQUIRE(optMod.has_value());

        bool foundBlur = false;
        bool foundMat = false;
        bool foundScalar = false;
        for (const auto& fn : optMod->functions) {
            if (fn.displayName == "blur") {
                foundBlur = true;
                for (const auto& blk : fn.blocks) {
                    for (const auto& inst : blk.instructions) {
                        if (inst.op == il::Op::ElemGetTyped || inst.op == il::Op::ElemSetTyped) {
                            CHECK((inst.immI32 & il::kElemFlagProvenIntegralIndex) != 0);
                            CHECK((inst.immI32 & il::kElemFlagProvenInBounds) != 0);
                        }
                    }
                }
            } else if (fn.displayName == "mat") {
                foundMat = true;
                for (const auto& blk : fn.blocks) {
                    for (const auto& inst : blk.instructions) {
                        if (inst.op == il::Op::ElemGetTyped || inst.op == il::Op::ElemSetTyped) {
                            CHECK((inst.immI32 & il::kElemFlagProvenIntegralIndex) != 0);
                            CHECK((inst.immI32 & il::kElemFlagProvenInBounds) != 0);
                        }
                    }
                }
            } else if (fn.displayName == "scalar") {
                foundScalar = true;
                size_t rawToInt32 = 0;
                for (const auto& blk : fn.blocks) {
                    for (const auto& inst : blk.instructions) {
                        if (inst.op == il::Op::ToInt32 && inst.rawUnbox) {
                            ++rawToInt32;
                        }
                    }
                }
                CHECK(rawToInt32 >= 2);
            }
        }
        CHECK(foundBlur);
        CHECK(foundMat);
        CHECK(foundScalar);
    }
}




