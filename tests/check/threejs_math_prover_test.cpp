#include <doctest/doctest.h>

#include "check/js_source_rewriter.h"
#include "check/prove_equiv.h"
#include "check/prove_loop_induction.h"

using namespace bronze::check;

TEST_CASE("Three.js Vector3 class methods: symbolic object-field equivalence and counterexamples") {
    const char* vec3A = R"(
        class Vector3 {
            constructor(x, y, z) {
                this.x = x;
                this.y = y;
                this.z = z;
            }
            set(x, y, z) {
                this.x = x;
                this.y = y;
                this.z = z;
                return this;
            }
            addVectors(a, b) {
                this.x = a.x + b.x;
                this.y = a.y + b.y;
                this.z = a.z + b.z;
                return this;
            }
            addScaledVector(v, s) {
                this.x += v.x * s;
                this.y += v.y * s;
                this.z += v.z * s;
                return this;
            }
            lengthSq() {
                return this.x * this.x + this.y * this.y + this.z * this.z;
            }
        }
    )";

    // Equivalent implementation using local temporaries and helper method call `this.set(...)`
    const char* vec3B = R"(
        class Vector3 {
            constructor(x, y, z) {
                this.x = x;
                this.y = y;
                this.z = z;
            }
            set(x, y, z) {
                this.x = x;
                this.y = y;
                this.z = z;
                return this;
            }
            addVectors(a, b) {
                const nx = a.x + b.x;
                const ny = a.y + b.y;
                const nz = a.z + b.z;
                return this.set(nx, ny, nz);
            }
            addScaledVector(v, s) {
                const vx = v.x;
                const vy = v.y;
                const vz = v.z;
                this.x = this.x + vx * s;
                this.y = this.y + vy * s;
                this.z = this.z + vz * s;
                return this;
            }
            lengthSq() {
                const x = this.x, y = this.y, z = this.z;
                return x * x + y * y + z * z;
            }
        }
    )";

    auto repAddVec = proveSourceEquivalent(vec3A, vec3B, "Vector3.addVectors", -100, 100);
    CHECK(repAddVec.status == EquivStatus::Equivalent);
    CHECK(repAddVec.isEquivalent());

    auto repAddScaled = proveSourceEquivalent(vec3A, vec3B, "Vector3.addScaledVector", -100, 100);
    CHECK(repAddScaled.status == EquivStatus::Equivalent);

    auto repLenSq = proveSourceEquivalent(vec3A, vec3B, "Vector3.lengthSq", -100, 100);
    CHECK(repLenSq.status == EquivStatus::Equivalent);

    // Whole-module equivalence across all modified methods in the class
    auto repAll = proveSourceEquivalent(vec3A, vec3B, "", -100, 100);
    CHECK(repAll.status == EquivStatus::Equivalent);

    // Buggy implementation that mutates `this.z` with subtraction instead of addition
    const char* vec3Buggy = R"(
        class Vector3 {
            addVectors(a, b) {
                this.x = a.x + b.x;
                this.y = a.y + b.y;
                this.z = a.z - b.z;
                return this;
            }
        }
    )";
    auto repBuggy = proveSourceEquivalent(vec3A, vec3Buggy, "Vector3.addVectors", -100, 100);
    CHECK(repBuggy.status == EquivStatus::Counterexample);
}

TEST_CASE("Three.js Matrix4 and Color class methods: indexed elements and bit-packing") {
    const char* matA = R"(
        class Matrix4 {
            multiplyScalar(s) {
                const te = this.elements;
                te[0] = te[0] * s;
                te[1] = te[1] * s;
                te[2] = te[2] * s;
                te[3] = te[3] * s;
                return this;
            }
        }
    )";

    const char* matB = R"(
        class Matrix4 {
            multiplyScalar(s) {
                this.elements[0] *= s;
                this.elements[1] *= s;
                this.elements[2] *= s;
                this.elements[3] *= s;
                return this;
            }
        }
    )";

    auto repMat = proveSourceEquivalent(matA, matB, "Matrix4.multiplyScalar", -100, 100);
    CHECK(repMat.status == EquivStatus::Equivalent);

    const char* matBuggy = R"(
        class Matrix4 {
            multiplyScalar(s) {
                this.elements[0] *= s;
                this.elements[1] *= s;
                this.elements[2] *= s;
                this.elements[3] += s;
                return this;
            }
        }
    )";
    auto repMatBuggy = proveSourceEquivalent(matA, matBuggy, "Matrix4.multiplyScalar", -100, 100);
    CHECK(repMatBuggy.status == EquivStatus::Counterexample);

    // Three.js Color hex extraction equivalence
    const char* colorA = R"(
        class Color {
            setHexInt(hex) {
                const h = hex & 16777215;
                this.r = (h >> 16) & 255;
                this.g = (h >> 8) & 255;
                this.b = h & 255;
                return this;
            }
        }
    )";
    const char* colorB = R"(
        class Color {
            setHexInt(hex) {
                this.r = (hex >> 16) & 255;
                this.g = (hex >> 8) & 255;
                this.b = hex & 255;
                return this;
            }
        }
    )";
    auto repColor = proveSourceEquivalent(colorA, colorB, "Color.setHexInt", 0, 16777215);
    CHECK(repColor.status == EquivStatus::Equivalent);
}

TEST_CASE("Three.js vertex buffer loop: relational loop induction over array mutations") {
    const char* bufLoopA = R"(
        function transformBuffer(dst, src, n, scale) {
            for (let i = 0; i < n; i++) {
                const v = src[i] | 0;
                dst[i] = ((v * 2) + (v * 2)) ^ (scale & 255);
            }
            return n;
        }
    )";

    const char* bufLoopB = R"(
        function transformBuffer(dst, src, n, scale) {
            for (let i = 0; i < n; i++) {
                const v = src[i] | 0;
                dst[i] = (v << 2) ^ (scale & 255);
            }
            return n;
        }
    )";

    auto repLoop = proveLoopClosedFormInductive(bufLoopA, bufLoopB, 0, 10000);
    CHECK(repLoop.status == EquivStatus::Equivalent);

    auto repFull = proveSourceEquivalent(bufLoopA, bufLoopB, "transformBuffer", 0, 10000);
    CHECK(repFull.status == EquivStatus::Equivalent);

    const char* bufLoopBuggy = R"(
        function transformBuffer(dst, src, n, scale) {
            for (let i = 0; i < n; i++) {
                const v = src[i] | 0;
                dst[i] = (v << 1) ^ (scale & 255);
            }
            return n;
        }
    )";
    auto repBuggy = proveLoopClosedFormInductive(bufLoopA, bufLoopBuggy, 0, 10000);
    CHECK(repBuggy.status == EquivStatus::Counterexample);
}

TEST_CASE("Three.js class rewriting: multi-method optimization and per-method SMT verification") {
    const char* threeClassSrc = R"(
class Vector3Math {
    lengthSq() {
        return Math.pow(this.x, 2) + Math.pow(this.y, 2) + Math.pow(this.z, 2);
    }
    quantizeChannel(x) {
        const u = x & 255;
        if (u < 0) {
            return -1;
        }
        return Math.floor(u / 16);
    }
    evalPoly(x) {
        const u = x & 15;
        return 2 * Math.pow(u, 3) + 3 * Math.pow(u, 2) + 4 * u + 5;
    }
}
)";

    JsRewriteOptions opts;
    opts.requireFormalProof = true;
    opts.domainMin = -100;
    opts.domainMax = 100;

    auto report = rewriteJsSource(threeClassSrc, opts);
    CHECK(report.error.empty());
    CHECK(report.allProvenEquivalent);
    CHECK(report.rewrites.size() >= 4);

    // Check that Math.pow(this.x, 2) was strength-reduced to (this.x * this.x)
    CHECK(report.rewrittenSource.find("(this.x * this.x)") != std::string::npos);
    // Check that both methods declaring `const u = ...` had their range facts resolved independently:
    // 1. Dead branch `if (u < 0)` eliminated and `Math.floor(u / 16)` reduced to `(u >> 4)` in quantizeChannel
    CHECK(report.rewrittenSource.find("(u >> 4)") != std::string::npos);
    // 2. Degree-3 polynomial over `u` factored into Horner's rule in evalPoly
    CHECK(report.rewrittenSource.find("(((2 * u + 3) * u + 4) * u + 5)") != std::string::npos);
}

TEST_CASE("Pixi.js Matrix 2D affine transform and Rectangle hit-testing: symbolic equivalence") {
    const char* pixiMatA = R"(
        class Matrix {
            constructor(a, b, c, d, tx, ty) {
                this.a = a;
                this.b = b;
                this.c = c;
                this.d = d;
                this.tx = tx;
                this.ty = ty;
            }
            append(matrix) {
                const a1 = this.a;
                const b1 = this.b;
                const c1 = this.c;
                const d1 = this.d;
                this.a = matrix.a * a1 + matrix.b * c1;
                this.b = matrix.a * b1 + matrix.b * d1;
                this.c = matrix.c * a1 + matrix.d * c1;
                this.d = matrix.c * b1 + matrix.d * d1;
                this.tx = matrix.tx * a1 + matrix.ty * c1 + this.tx;
                this.ty = matrix.tx * b1 + matrix.ty * d1 + this.ty;
                return this;
            }
            applyX(x, y) {
                return this.a * x + this.c * y + this.tx;
            }
            applyY(x, y) {
                return this.b * x + this.d * y + this.ty;
            }
        }
    )";

    const char* pixiMatB = R"(
        class Matrix {
            constructor(a, b, c, d, tx, ty) {
                this.a = a;
                this.b = b;
                this.c = c;
                this.d = d;
                this.tx = tx;
                this.ty = ty;
            }
            append(matrix) {
                const a1 = this.a, b1 = this.b, c1 = this.c, d1 = this.d;
                const ma = matrix.a, mb = matrix.b, mc = matrix.c, md = matrix.d;
                const mtx = matrix.tx, mty = matrix.ty;
                this.a = ma * a1 + mb * c1;
                this.b = ma * b1 + mb * d1;
                this.c = mc * a1 + md * c1;
                this.d = mc * b1 + md * d1;
                this.tx += mtx * a1 + mty * c1;
                this.ty += mtx * b1 + mty * d1;
                return this;
            }
            applyX(x, y) {
                return x * this.a + y * this.c + this.tx;
            }
            applyY(x, y) {
                return x * this.b + y * this.d + this.ty;
            }
        }
    )";

    auto repAppend = proveSourceEquivalent(pixiMatA, pixiMatB, "Matrix.append", -100, 100);
    CHECK(repAppend.status == EquivStatus::Equivalent);

    auto repApplyX = proveSourceEquivalent(pixiMatA, pixiMatB, "Matrix.applyX", -100, 100);
    CHECK(repApplyX.status == EquivStatus::Equivalent);

    auto repApplyY = proveSourceEquivalent(pixiMatA, pixiMatB, "Matrix.applyY", -100, 100);
    CHECK(repApplyY.status == EquivStatus::Equivalent);

    // Buggy Matrix.append that reads already-clobbered `this.a` instead of `a1`
    const char* pixiMatBuggy = R"(
        class Matrix {
            append(matrix) {
                this.a = matrix.a * this.a + matrix.b * this.c;
                this.b = matrix.a * this.b + matrix.b * this.d;
                this.c = matrix.c * this.a + matrix.d * this.c;
                this.d = matrix.c * this.b + matrix.d * this.d;
                this.tx = matrix.tx * this.a + matrix.ty * this.c + this.tx;
                this.ty = matrix.tx * this.b + matrix.ty * this.d + this.ty;
                return this;
            }
        }
    )";
    auto repBuggy = proveSourceEquivalent(pixiMatA, pixiMatBuggy, "Matrix.append", -10, 10);
    CHECK(repBuggy.status == EquivStatus::Counterexample);
}

TEST_CASE("Pixi.js Color BGR/ABGR bit-packing superoptimization and batch vertex loop induction") {
    const char* pixiColorSrc = R"(
class PixiColor {
    toBgrNumber() {
        const r = (this._int >> 16) & 255;
        const g = ((this._int >> 8) & 255) << 8;
        const b = (this._int & 255) << 16;
        return r | g | b;
    }
    packPremultipliedAlpha(rgbInt, alphaByte) {
        const a = alphaByte & 255;
        const bgr = ((rgbInt & 255) << 16) | (((rgbInt >> 8) & 255) << 8) | ((rgbInt >> 16) & 255);
        return (a << 24) | bgr;
    }
}
)";

    JsRewriteOptions opts;
    opts.requireFormalProof = true;
    opts.domainMin = 0;
    opts.domainMax = 16777215;

    auto report = rewriteJsSource(pixiColorSrc, opts);
    CHECK(report.error.empty());
    CHECK(report.allProvenEquivalent);
    CHECK(!report.rewrites.empty());
    // Verify that `((this._int >> 8) & 255) << 8` was superoptimized to `(this._int & 65280)`
    // and `((rgbInt >> 8) & 255) << 8` was superoptimized to `(rgbInt & 65280)`
    CHECK(report.rewrittenSource.find("(this._int & 65280)") != std::string::npos);
    CHECK(report.rewrittenSource.find("(rgbInt & 65280)") != std::string::npos);

    // Pixi.js sprite batch vertex/tint buffer loop induction
    const char* pixiBatchA = R"(
        function packSpriteTints(dst, src, count) {
            for (let i = 0; i < count; i++) {
                const rgb = src[i] | 0;
                dst[i] = ((rgb & 255) << 16) | (((rgb >> 8) & 255) << 8) | ((rgb >> 16) & 255);
            }
            return count;
        }
    )";
    const char* pixiBatchB = R"(
        function packSpriteTints(dst, src, count) {
            for (let i = 0; i < count; i++) {
                const rgb = src[i] | 0;
                dst[i] = ((rgb & 255) << 16) | (rgb & 65280) | ((rgb >> 16) & 255);
            }
            return count;
        }
    )";
    auto repBatch = proveLoopClosedFormInductive(pixiBatchA, pixiBatchB, 0, 16777215);
    CHECK(repBatch.status == EquivStatus::Equivalent);
}
