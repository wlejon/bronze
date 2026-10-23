# Formal Discovery Report: Bronze Static Prover Audit Across Production Ecosystem & Open-Source JS

## 1. Executive Summary

This report documents the findings, counterexample witnesses, formal equivalence proofs, and surgical bug fixes produced by dogfooding the **Bronze** formal verification suite (`bronze check`, `bronze check --deterministic`, `bronze prove-equiv`, and `bronze synth-tests`) across:

1. **Production Ecosystem JavaScript (`#125`)**: `broworkshop/lib/kit/`, `broworkshop/lib/arcade/`, `broworkshop/lib/`, `broworkshop/demos/planet/`, `ffmpeg-bro/supercut/` & `ui/`, `helm/ui/js/`, and `helmapps/`.
2. **Open-Source 3D Math, Geometry & Graphics (`#122`)**: `three.js` r160 (`MathUtils`, `Color`, `Vector3`, `Quaternion`, `Triangle`, `Box3`), `gl-matrix`, `earcut`, and `rbush`.
3. **Open-Source Codecs, Compression & Hashing (`#123`)**: Mozilla `source-map` Base64-VLQ, Protobuf ZigZag/Varint32, RFC 4648 Base64, UTF-8 scalar codecs, Morton Z-order curves, MurmurHash3, xxHash32, and FNV-1a.
4. **Open-Source Micro-Libraries & Branch Synthesis (`#124`)**: `date-fns` / Howard Hinnant civil calendar algorithms, `semver` ordering, `color-convert` RGB/HSL conversions, and financial basis-point rounding.
5. **PRNG Bijectivity ($2^{32}$), IEEE-754 Float16 ($2^{16}$), IPv4/CIDR `/0` Shift Traps & Web Audio DSP (`#126`)**: Murmur3 `fmix32` / `SplitMix32` / `xorshift32` $2^{32}$ bijectivity & absorbing zero-state proofs, Perlin/Simplex negative world-coordinate OOB, Box-Muller `u1 === 0` singularity, exhaustive 65,536-pattern `DataUtils.toHalfFloat` / `fromHalfFloat` verification, IPv4 `/0` CIDR 5-bit shift count wrapping, G.711 $\mu$-law & IMA ADPCM codecs, and RBJ Audio EQ Cookbook Biquad Jury stability proofs.
6. **Spring/Bezier Physics Singularities & Cross-Language CPU-JS $\leftrightarrow$ GPU-GLSL/SPIR-V Parity Proofs (`#127`)**: harmonic oscillator critical-damping (`zeta === 1.0`) `0/0` `NaN` & overdamped `sqrt` domain errors, `bezier-easing` flat-tangent Newton-Raphson `getSlope(t) === 0` division-by-zero, Kay-Kajiya Ray-AABB slab `0/0` `NaN`, GLSL `mod`/`fract` vs JS `%`/`trunc` negative coordinate tearing, and 0-ULP CPU-JS $\leftrightarrow$ Vulkan SPIR-V 1.5 / NVIDIA PTX `sm_80` formal equivalence proofs.

Across the `broworkshop` production libraries (`lib/kit/`, `lib/arcade/`, `lib/bot-aim.js`), the audit uncovered and fixed **14 real latent edge-case bugs and multiplayer lockstep determinism hazards** across 11 modules without breaking a single existing test.

---

## 2. Production Ecosystem Audit & Fixes (`#125` — `broworkshop/lib/`)

### Summary of Production Bugs Fixed

| # | Module | Function(s) | Hazard Class | Concrete Witness / Trigger | Fix Applied & Verification |
|---|--------|-------------|--------------|----------------------------|----------------------------|
| 1 | `lib/kit/math3d.js` | `quatYTo(dx, dy, dz)` | Non-unit quaternion (`norm = 0.7071`) on zero input | `quatYTo(0, 0, 0)` (synthesized by `bronze synth-tests` `[PATH 5]`) returns `[0, 0, 0, 0.70710678]` instead of `[0, 0, 0, 1]` | Guard `if (!(len > 0)) return [0, 0, 0, 1];` |
| 2 | `lib/kit/math3d.js` | `q.axis(ax, ay, az, angle)` | Zero / non-unit quaternion when axis is zero or unnormalized | `q.axis(0, 0, 0, Math.PI)` returns `{x:0, y:0, z:0, w:0}`, collapsing rotated vectors to zero | Normalize `(ax, ay, az)` by `al = Math.hypot(ax, ay, az)`; return identity `{x:0, y:0, z:0, w:1}` when `!(al > 0)` |
| 3 | `lib/kit/math3d.js` | `q.slerp(a, b, t)` | Zero-norm quaternion return | `q.slerp({x:0,y:0,z:0,w:0}, {x:0,y:0,z:0,w:0}, 0.5)` returns `{x:0,y:0,z:0,w:0}` | Return `{x:0, y:0, z:0, w:1}` when `!(L > 0)` |
| 4 | `lib/kit/camera.js` | `Camera.quatLook(f, up)` & `quatFromMat3(m)` | Rank-1 singular matrix when `f \|\| up` or `f == [0,0,0]` | `Camera.quatLook([0, s, s], [0, s, s])` produces `r = [0,0,0], u = [0,0,0]`, skewing camera forward to `[0, 0.653, 0.757]`; `quatLook([0,0,0], [0,1,0])` flips 180° to `[0,0,1,0]` | Fallback forward `[0,0,-1]` when `fl <= 1e-12`; fallback orthogonal up when `rl <= 1e-6`; clamp `Math.sqrt(Math.max(0, ...))` |
| 5 | `lib/kit/viewport3d.js` | `viewBasis(view)` (`screenRay`, `worldToScreen`) | Basis collapse (`r = [0,0,0], u = [0,0,0]`) in top-down views | `view = {position:[0,10,0], target:[0,0,0], up:[0,1,0]}` collapses all screen rays to `[0,-1,0]` and projects all world points to screen center | Fallback forward `[0,0,-1]` when `fl <= 1e-12` and orthogonal fallback up when `rl <= 1e-6` |
| 6 | `lib/bot-aim.js` | `angleDelta(from, to)` & `BotAim.tick(aim, dt)` | Unbounded `while` loop hang on `±Infinity` / `O(\|d\|)` stall & negative-`dt` inversion | `desiredYaw = Infinity` hangs `while (d > Math.PI)`; `dt < 0` inverts `maxStep` clamping | Loop-free $O(1)$ Euclidean modulo wrap in `angleDelta`; finite guard; `if (!(dt > 0) \|\| !Number.isFinite(dt)) return;` |
| 7 | `lib/arcade/grid.js` | `createFalls(durMs).offset(r, c)` | `0 / 0 = NaN` poisoning when `durMs === 0` | `const f = createFalls(0); f.add(2, 0, 0); f.offset(2, 0)` evaluates `0 / 0 = NaN`, poisoning tween offset | Guard `const p = tw.dur > 0 ? Math.min(1, Math.max(0, tw.t / tw.dur)) : 1;` |
| 8 | `lib/arcade/grid.js` | `fitBoard` & `cellAt(layout, x, y)` | `0 / 0 = NaN` cell coordinates and negative cell index leak | `fitBoard(200, 200, 8, 8)` sets `cell = 0` (`cellAt` returns `{r: NaN, c: NaN}`); `fitBoard(150, 200, 8, 8)` sets `cell = -7` (`cellAt` returns `{r: -1, c: -1}`) | Clamp `cell = Math.max(1, cell)` in `fitBoard`; in `cellAt`, require `layout.cell > 0` and positive conjunctive bounds `r >= 0 && r < layout.rows && c >= 0 && c < layout.cols` |
| 9 | `lib/arcade/grid.js` | `easeOutCubic` & `createWave` | Lockstep `[transcendental-float-drift]` (`Math.pow`, `Math.hypot`) & sort tie-order | Flagged by `bronze check --deterministic lib/arcade/grid.js` at lines 188 and 208 | Replace `1 - Math.pow(1 - t, 3)` with exact `1 - u * u * u` (`prove-equiv: EQUIVALENT`); use `Math.sqrt(dr*dr + dc*dc)` and tie-break `(a._d - b._d) \|\| (a.r - b.r) \|\| (a.c - b.c)` |
| 10 | `lib/arcade/input.js`, `pointer.js`, `save.js` | `actionForKey`, `bindPointer`, `load` | Lockstep `[object-iteration-order]` on `for..in` loops | Flagged by `bronze check --deterministic` at `input.js:73`, `pointer.js:35`, `save.js:28` | Iterate over `Object.keys(...).sort()` (`0 lockstep hazards found`) |
| 11 | `lib/kit/sway.js` | `analyse`, `plotTracks`, `levelDrive`, `treeLevel` | `0 / 0 = NaN` when track length `n <= 1` or damping `zeta === 0` | `analyse([[1,2,3]], 30, [1,0,0], [0.5])` divides by `n - 1 = 0` in Hann window & `speedRms`, and `inst.length = 0` in `flutterHz`; `zeta = 0` divides by `Math.sqrt(0)` | Guard `n === 0` and `n === 1` (`hannDenom = Math.max(1, n - 1)`, `il = inst.length \|\| 1`); clamp `z = Math.max(zeta, 1e-6)` |
| 12 | `lib/kit/motion.js` & `motion-sheet.js` | `MotionRun.series(s, axis)` & `pctl(a, p)` | `0 / 0 = NaN` detrend slope when `N === 1`; `undefined` on `p < 0` | `series` with `N = 1` computes `sxx = 0, sxy = 0, bb = 0/0 = NaN`; `pctl([10,20,30], -0.5)` indexes `s[-2] = undefined` | Guard `bb = sxx > 0 ? sxy / sxx : 0` in `series`; clamp index `Math.max(0, Math.min(s.length - 1, ...))` in `pctl` |
| 13 | `lib/kit/gauges.js` & `params.js` | `historyPlot.draw()` & `logMap(min, max)` | `0 / 0 = NaN` when `min === max` or `size === 1` | `logMap(100, 100).to(100)` divides `0 / (a - a) = NaN` (`Math.round(NaN) = NaN`); `historyPlot` with `min === max` or `size === 1` divides by `0` | Guard `span = (o.max - o.min) \|\| 1` and `denom = Math.max(1, o.size - 1)` in `gauges.js`; guard `span > 0` in `logMap` |

---

### Detailed Analysis of Key Production Findings

#### 2.1 `lib/kit/math3d.js` — `quatYTo(0, 0, 0)` Non-Unit Quaternion (Synthesized by `bronze synth-tests`)

Running `bronze synth-tests lib/kit/math3d.js` explored the symbolic execution paths of `quatYTo(dx, dy, dz)` and generated the zero-vector input:

```js
// [PATH 5] quatYTo (b0->b16->T(b17)->T(b18)->T(b19)->F(b21)->b22->F(b24)->b25->T(b26)->T(b27))
quatYTo(0, 0, 0)
```

In the unpatched implementation:
```js
export function quatYTo(dx, dy, dz) {
    const len = Math.hypot(dx, dy, dz) || 1;
    const x = dx / len, y = dy / len, z = dz / len;
    if (y > 0.999999) return [0, 0, 0, 1];
    if (y < -0.999999) return [1, 0, 0, 0];
    const ax = z, az = -x;
    const al = Math.hypot(ax, az) || 1;
    const half = Math.acos(y) / 2;
    const s = Math.sin(half);
    return [(ax / al) * s, 0, (az / al) * s, Math.cos(half)];
}
```
When `(dx, dy, dz) = (0, 0, 0)`, `len` becomes `0 || 1 = 1`, so `x = 0, y = 0, z = 0`. Neither `y > 0.999999` nor `y < -0.999999` triggers. Then `ax = 0, az = 0`, `al = 0 || 1 = 1`, and `half = Math.acos(0) / 2 = Math.PI / 4`. Because `ax = 0` and `az = 0`, the sine components vanish while `w = Math.cos(Math.PI / 4) = 0.7071067811865476`, returning `[0, 0, 0, 0.7071067811865476]` — a non-unit quaternion with magnitude $1/\sqrt{2}$ that scales geometry down when converted to a rotation matrix. Returning `[0, 0, 0, 1]` when `!(len > 0)` restores the unit-norm invariant across all inputs in $\mathbb{R}^3$.

#### 2.2 `lib/kit/camera.js` & `lib/kit/viewport3d.js` — Collinear `forward || up` Basis Collapse

Both `Camera.quatLook(f, up)` in `lib/kit/camera.js` and `viewBasis(view)` in `lib/kit/viewport3d.js` construct an orthonormal camera basis via `r = normalize(f × up)` and `u = r × f`:
```js
let r = [f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0]];
const rl = Math.hypot(r[0], r[1], r[2]) || 1;
r = r.map((x) => x / rl);
const u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]];
```
When `f` is parallel or antiparallel to `up` (for example, a top-down camera at `position: [0, 10, 0]` looking at `target: [0, 0, 0]` with default `up: [0, 1, 0]`, or diagonal `f = [0, 1/√2, 1/√2]` with `up = [0, 1/√2, 1/√2]`):
- `f × up` is `[0, 0, 0]`, so `rl = 0 || 1 = 1`, leaving `r = [0, 0, 0]` and `u = r × f = [0, 0, 0]`.
- In `Camera.quatLook`, two columns of the 3×3 rotation matrix passed to Shepperd's `quatFromMat3` are zero (a rank-1 singular matrix with $\det = 0$). For `f = [0, 1/√2, 1/√2]`, `Camera.quatRotVec(Camera.quatLook(f, up), [0, 0, -1])` points to `[0, 0.65328, 0.75711]` instead of `[0, 0.70711, 0.70711]`.
- In `viewport3d.js`, `screenRay` collapses every pixel `(px, py)` on the canvas to the single ray `b.f`, and `worldToScreen` projects every world point to the center pixel `(width / 2, height / 2)`.

Selecting an orthogonal fallback up vector (`Math.abs(f[1]) < 0.9 ? [0, 1, 0] : [1, 0, 0]`) whenever `!(rl > 1e-6)` guarantees a right-handed orthonormal basis $(r, u, -f) \in SO(3)$ for all camera poses.

#### 2.3 `lib/arcade/grid.js` — `fitBoard` & `cellAt` Counterexamples Verified by `bronze prove-equiv`

In `lib/arcade/grid.js`, `fitBoard(W, H, rows, cols)` computed:
```js
let cell = Math.floor(Math.min((W - padX) / cols, (H - padY) / rows));
```
and `cellAt(layout, x, y)` checked:
```js
const c = Math.floor((x - layout.ox) / layout.cell);
const r = Math.floor((y - layout.oy) / layout.cell);
if (x < layout.ox || y < layout.oy || r >= layout.rows || c >= layout.cols) return null;
return { r, c };
```
Running `bronze prove-equiv` between the unpatched bounds check and the conjunctive positive bounds check immediately produced a counterexample witness:
```text
fitBoardCell: COUNTEREXAMPLE (rawCell = 0, minCell = 0, maxCell = 0) -> old = 0, new = 1
cellAtBounds: COUNTEREXAMPLE (ox = 7, oy = -2147483648, rows = 110684461, cols = 128, x = 15, y = 475054858, r = -889607506, c = -1495383609) -> old = -2384991115, new = -1
```
Specifically:
1. When `W === padX` (e.g. `fitBoard(200, 200, 8, 8)`), `layout.cell === 0`. Calling `cellAt(layout, 100, 100)` evaluates `0 / 0 = NaN` for `r` and `c`. Because IEEE-754 comparisons `NaN >= 8` evaluate to `false`, the `if` guard falls through and returns `{ r: NaN, c: NaN }` instead of `null`, causing `grid[r][c]` to throw a `TypeError` in puzzle games!
2. When `W < padX` (e.g. `fitBoard(150, 200, 8, 8)` on a narrow split-screen window), `layout.cell === -7`. Calling `cellAt(layout, 140, 140)` has `x >= layout.ox`, so `(x - layout.ox) / layout.cell < 0`, yielding `r = -1, c = -1`, which also bypasses `r >= layout.rows` and returns `{ r: -1, c: -1 }`!
3. Under valid preconditions (`cell > 0 && rows > 0 && cols > 0 && x >= ox && y >= oy && r >= 0 && c >= 0`), `bronze prove-equiv` proves:
   ```text
   cellAtBoundsInRange: EQUIVALENT
   fitBoardCellInRange: EQUIVALENT
   ```

#### 2.4 Lockstep Determinism Audit Across `lib/arcade/` (`bronze check --deterministic`)

Running `bronze check --deterministic` across all `lib/arcade/` multiplayer/kernel modules flagged 5 lockstep hazards across 4 files:
- `lib/arcade/grid.js:188`: `[transcendental-float-drift]` on `Math.pow(1 - t, 3)` in `easeOutCubic`. Replaced with `const u = 1 - t; return 1 - u * u * u;`, proven bit-identical by `bronze prove-equiv` (`easeOutCubic: EQUIVALENT`).
- `lib/arcade/grid.js:208`: `[transcendental-float-drift]` on `Math.hypot(r - mid.r, c - mid.c)` used to sort wave cell firing order in `createWave`. Replaced with IEEE-754 hardware-rounded `Math.sqrt(dr * dr + dc * dc)` plus deterministic lexicographic `(r, c)` tie-breaking.
- `lib/arcade/input.js:73`, `lib/arcade/pointer.js:35`, `lib/arcade/save.js:28`: `[object-iteration-order]` on `for..in` loops. Replaced with `Object.keys(...).sort()`.

After these fixes, all arcade kernel modules pass `bronze check --deterministic` with **0 lockstep hazards**.

---

## 3. Open-Source Corpus Formal Discovery Summary (`#122`, `#123`, `#124`)

Alongside the production ecosystem audit (`#125`), the Bronze formal verification suite was applied to standard open-source algorithms across 3D graphics, binary codecs/hashing, and micro-libraries:

### 3.1 3D Math, Geometry & Graphics (`#122` — `three.js` r160, `gl-matrix`, `earcut`, `rbush`)

- **`MathUtils.ceilPowerOfTwo` / `floorPowerOfTwo` / `isPowerOfTwo` 32-bit Shift & Domain Breakdown**:
  - Unmodified `three.js` `isPowerOfTwo(v)` computes `(v & (v - 1)) === 0 && v !== 0`. For `v = -2147483648` (`0x80000000`, `INT32_MIN`), `v - 1` wraps in 32-bit two's complement to `0x7fffffff`, so `(-2147483648 & 2147483647) === 0`, falsely reporting `INT32_MIN` as a positive power of two.
  - `ceilPowerOfTwo(v)` and `floorPowerOfTwo(v)` use `Math.pow(2, Math.ceil(Math.log2(v)))`, returning `NaN` for `v < 0`, `-Infinity` / `0` for `v === 0`, and overflowing signed 32-bit bitwise shifts `(1 << 31) = -2147483648` when `v > 2^30` (`0x40000001`).
- **`Color.getHex()` Unclamped Channel Bit-Packing Corruption**:
  - `(r * 255) << 16 ^ (g * 255) << 8 ^ (b * 255) << 0` without `[0, 255]` clamping allows HDR or negative color components (`g = 1.5` or `b = -0.1`) to bleed bits into neighboring RGB channels or flip the sign bit.
- **`Quaternion.setFromUnitVectors(vFrom, vTo)` Antiparallel Singularity & `Triangle.getBarycoord` Collinearity**:
  - When `vFrom = -vTo`, `r = dot(vFrom, vTo) + 1 = 0`; without an explicit `r < EPSILON` orthogonal-axis branch, normalizing `[cross.x, cross.y, cross.z, 0]` divides `0 / 0 = NaN`. Similarly, collinear triangle vertices yield `denom = 0` in `getBarycoord`.

### 3.2 Bitwise Codecs, Compression & Hashing (`#123` — VLQ, Varint, Base64, UTF-8, Murmur3, xxHash32)

- **Lossless Round-Trip Proofs (`decode(encode(x)) === x`)**:
  - **Protobuf 32-bit ZigZag**: Proven lossless across all $2^{32}$ signed 32-bit integers `[-2147483648, 2147483647]` for `encode(x) = (x << 1) ^ (x >> 31)` and `decode(z) = (z >>> 1) ^ -(z & 1)`.
  - **RFC 4648 Base64 24-bit Quantum**: Proven lossless across all $2^{24}$ (`16,777,216`) 3-byte triples mapped to 4 6-bit sextets.
  - **UTF-8 Codepoint Scalar Round-Trip**: Proven lossless across 1-byte (`0x00..0x7F`), 2-byte (`0x80..0x7FF`), 3-byte (`0x800..0xFFFF` excluding surrogates), and 4-byte (`0x10000..0x10FFFF`) scalar ranges.
  - **Morton 2D Z-Order Curve**: Proven lossless bit-interleaving (`encodeMorton2D` / `decodeMorton2D`) across all $2^{32}$ `(x16, y16)` coordinate pairs.
- **Refuted Codec & Hash Bugs**:
  - **Mozilla `source-map` VLQ `INT32_MIN` Overflow**: Naive `x < 0 ? ((-x) << 1) | 1 : (x << 1)` fails at `x = -2147483648` (`-x` overflows 32-bit signed integer) and at `x = -1073741824` (`(-x) << 1` shifts into the sign bit).
  - **IEEE-754 `double` Multiplication vs `Math.imul` in 32-bit Hashes**: Naive JS `(h * 0xcc9e2d51) | 0` in MurmurHash3 / xxHash32 / FNV-1a loses low bits whenever the 64-bit integer product exceeds $2^{53} - 1$ (`Number.MAX_SAFE_INTEGER`), diverging from true 32-bit modular multiplication `Math.imul(h, 0xcc9e2d51)`.

### 3.3 Calendar Math, SemVer, Color Conversion & Financial Rounding (`#124`)

- **Calendar & Date-Time Math**:
  - Synthesized 100% branch-covering leap-year vectors (non-leap, divisible-by-4 leap, divisible-by-100 century non-leap, divisible-by-400 quadricentennial leap) via `bronze synth-tests`.
  - Exposed pre-1970 negative day-of-week modulo bug (`(days + 4) % 7 < 0` for `days < -4`) vs Euclidean `((days + 4) % 7 + 7) % 7`.
- **SemVer Ordering & Prerelease Overflow**:
  - Verified strict weak ordering (irreflexivity, asymmetry, transitivity) on bounded numeric SemVer triples, and refuted bitwise `(a - b) | 0` comparator overflow when numeric prerelease identifiers exceed $2^{31} - 1$ (e.g. timestamp build identifiers).
- **Color-Space `0/0` `NaN` Poisoning & Financial Basis-Point Rounding**:
  - Refuted unguarded `rgbToHsl(r, g, b)` dividing `(g - b) / (max - min) = 0 / 0 = NaN` on achromatic grays (`r === g === b`), and exposed IEEE-754 binary64 halfway rounding drift (`Math.round(1.005 * 100) === 100` because `1.005 * 100 = 100.49999999999999`).

### 3.4 PRNG Bijectivity ($2^{32}$), IEEE-754 Float16 ($2^{16}$), IPv4/CIDR `/0` Shift Traps & Web Audio DSP (`#126`)

- **PRNG & Hash Finalizer $2^{32}$ Bijectivity (`fmix32`, `SplitMix32`, `xorshift32`)**:
  - Proven via `proveRoundTripSource` across all $2^{32}$ 32-bit states that the Murmur3 / SplitMix32 avalanching xorshift stages (`x ^ (x >>> 16)` and `x ^ (x >>> 13)`) and Marsaglia's `xorshift32(x)` (`x ^= x << 13; x ^= x >>> 17; x ^= x << 5`) have exact bitvector inverses over $\mathbb{Z}_{2^{32}}$, and that `x === 0` is the unique absorbing fixed point of `xorshift32` (`SolveOutcome::Unsat`).
  - Synthesized concrete state collisions `(witA !== witB)` when an even multiplier constant is used, and demonstrated IEEE-754 `double` precision loss in `(x * 0x85ebca6b) | 0` vs `Math.imul(x, 0x85ebca6b)` when the product exceeds $2^{53} - 1$.
  - Refuted Perlin/Simplex `perm[xi % 256]` negative coordinate OOB (`xi = -1 -> -1`) vs `perm[xi & 255]` (proven bounded in `[0, 255]` across all $2^{32}$ ints), and caught Box-Muller `Math.sqrt(-2 * Math.log(u1))` domain breakdown at `u1 <= 0`.
- **Exhaustive $2^{16}$ IEEE-754 Float16 $\leftrightarrow$ Float32 Verification**:
  - Verified all 65,536 16-bit half-float bit patterns (`61,440` normal, `2,046` subnormal, `2` signed zeros, `2` infinities, `2,046` NaNs) for round-trip identity and strict monotonicity.
  - Proven normal-range binary16 $\leftrightarrow$ binary32 exponent/mantissa re-biasing via `BVSolver`, and refuted the bitwise-OR mantissa rounding carry bug `(exp16 << 10) | (0x03FF + 1)` vs `(exp16 << 10) + (0x03FF + 1)` when `exp16` is odd.
- **IPv4 / CIDR `/0` 5-Bit Shift Count Masking Bug & Signed Octet Comparison**:
  - Refuted `(~0 << (32 - prefixLen)) >>> 0` over `prefixLen in [0, 32]`: `BVSolver` synthesizes `prefixLen = 0` where `(32 - 0) & 31 === 0`, turning a `/0` mask (`0.0.0.0`) into `0xFFFFFFFF` (`255.255.255.255`, `/32`), and proves the guarded `prefixLen === 0 ? 0 : ((~0 << (32 - prefixLen)) >>> 0)` fix across `[0, 32]`.
  - Refuted signed 32-bit IPv4 range comparisons on addresses with first octet $\ge 128$ (`128.0.0.0`..`255.255.255.255` negative in signed int32).
- **Web Audio DSP — G.711 $\mu$-Law, IMA ADPCM & RBJ Biquad Stability**:
  - Verified all 256 G.711 $\mu$-law codewords for antisymmetry, round-trip identity, and strict segment/mantissa monotonicity via `BVSolver`.
  - Proven IMA ADPCM `[0, 88]` step-index clamping vs `stepTable[89]` overflow (`counterexampleInput >= 81`).
  - Detected RBJ Audio EQ Cookbook `Q === 0` division-by-zero in `alpha = sin(w0) / (2 * Q)` and proved Jury pole-radius stability (`-a0 < a2 < a0` where `a0 = 1 + alpha, a2 = 1 - alpha`) for all `alpha > 0`.

### 3.5 Spring/Bezier Physics Singularities & Cross-Language CPU-JS $\leftrightarrow$ GPU-GLSL/SPIR-V Parity Proofs (`#127`)

- **Harmonic Oscillator Spring Solver Singularities**:
  - Exposed both the critical-damping `0 / 0 = NaN` singularity at `zeta === 1.0` (`omega_d = omega0 * sqrt(1 - zeta^2) === 0`) and the overdamped `Math.sqrt(1 - zeta^2) = NaN` domain error (`zeta > 1.0`) in unbranched underdamped spring solvers.
  - Verified the 3-regime branched spring solver (underdamped `zeta < 1 - eps`, critically damped Taylor limit `(1 + omega0 * t) * exp(-omega0 * t)`, overdamped hyperbolic regime) passes `bronze check` with 0 findings, achieves 100% branch synthesis via `bronze synth-tests`, and maintains $C^0$ continuity across `zeta = 1`.
  - Proven via `BVSolver` that explicit Euler spring stepping strictly amplifies mechanical energy $E_1 = (1 + (\omega_0 \Delta t)^2) E_0 > E_0$ for all non-zero states.
- **Cubic-Bezier Easing Flat-Tangent Singularity & Ray-AABB Slab `0/0` `NaN`**:
  - Caught unguarded division by `currentSlope = getSlope(guessT, mX1, mX2)` in `bezier-easing` Newton-Raphson root finding when `mX1 === 0` and `guessT === 0` (`currentSlope === 0`), and proved bisection midpoint fallback stays bounded in `[aA, aB]`.
  - Caught Kay-Kajiya Ray-AABB slab intersection `(boxMinX - rayOrigX) / rayDirX = 0 / 0 = NaN` when an axis-parallel ray (`rayDirX === 0`) originates on a box face (`rayOrigX === boxMinX`), and verified the axis-parallel slab guard.
- **Cross-Language CPU-JS $\leftrightarrow$ GPU-GLSL/SPIR-V Parity Proofs**:
  - Refuted naive JS `x % 16` against GLSL `mod(x, 16) = x - 16 * floor(x / 16)` on negative coordinates (`x < 0`), and proved across `[-100000, 100000]` that Euclidean modulo `((x % 16) + 16) % 16` is bit-identical to GLSL `mod(x, 16)`.
  - Refuted naive JS `x - Math.trunc(x)` against GLSL `fract(x) = x - Math.floor(x)` on negative Q24.8 coordinates, and proved `x - (x & ~255) === (x & 255)` across all $2^{32}$ coordinates.
  - Proven 0-ULP CPU-JIT $\leftrightarrow$ Vulkan SPIR-V 1.5 & NVIDIA PTX `sm_80` equivalence (`cpuGpuProvenEquivalent`, `spirvEquivalence.is_equivalent()`, `maxCpuGpuUlpDiff == 0.0f`) on 3D SDF capsule smooth-union (`sdfCapsuleSmoothUnion`), PBR Schlick-GGX direct lighting (`pbrDirectLighting`), and GPU compute symplectic spring integration (`stepDampedSprings`).

---

## 4. Reproducibility & Verification Commands

All production fixes and regression tests in `broworkshop` can be verified headlessly and statically:

```bash
# 1. Static safety & lockstep determinism verification via Bronze CLI
/home/j/projects/bronze/build/src/cli/bronze check /home/j/projects/broworkshop/lib-tests/test_formal_dogfood.js
/home/j/projects/bronze/build/src/cli/bronze check --deterministic /home/j/projects/broworkshop/lib/arcade/grid.js
/home/j/projects/bronze/build/src/cli/bronze check --deterministic /home/j/projects/broworkshop/lib/arcade/input.js
/home/j/projects/bronze/build/src/cli/bronze check --deterministic /home/j/projects/broworkshop/lib/arcade/pointer.js
/home/j/projects/bronze/build/src/cli/bronze check --deterministic /home/j/projects/broworkshop/lib/arcade/save.js

# 2. Headless runtime regression suite in broworkshop
cd /home/j/projects/broworkshop
bash scripts/validate.sh --tests lib-tests/test_formal_dogfood.js
../bro/build-release/bro-headless lib-tests tests/test_formal_dogfood.js
```
