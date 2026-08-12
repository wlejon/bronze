// The milestone case: UNMODIFIED three.js r160 source, compiled by bronze.
//
// `three/` is the verbatim 28-file transitive closure of these five imports
// (see README.md). Nothing in it is patched, so this case answers the only
// question that matters about it: does bronze compile the real library, and
// does the scene graph it builds behave the way the library's own arithmetic
// says it must.
//
// EVERY line this prints is a boolean, an integer, or a decimal that IEEE-754
// represents exactly. That is the rule the expectation is derived under: a
// pinned float that came out of an accumulation would be a record of what
// bronze printed, not of what is true, and the ratchet forbids that. Where a
// value genuinely is an accumulation — the rotated world matrix — the case
// prints the INVARIANT instead (a rotation's determinant is 1, its rows are
// orthonormal, its inverse composed with it is the identity), inside a
// tolerance far wider than double rounding over ~20 flops and far tighter than
// any real miscompilation.
//
// Derivations are on each line. Clause references are ECMA-262 (2024) where
// the answer is a language question and three.js source lines where it is a
// library question.

import { Scene } from './three/scenes/Scene.js';
import { PerspectiveCamera } from './three/cameras/PerspectiveCamera.js';
import { Object3D } from './three/core/Object3D.js';
import { Mesh } from './three/objects/Mesh.js';
import { BoxGeometry } from './three/geometries/BoxGeometry.js';
import { MeshBasicMaterial } from './three/materials/MeshBasicMaterial.js';
import { Vector3 } from './three/math/Vector3.js';
import { Quaternion } from './three/math/Quaternion.js';
import { Euler } from './three/math/Euler.js';

// A deviation this wide is not reachable by double rounding over the tens of
// flops each check covers (2^-52 ~ 2.2e-16), and no miscompilation lands
// inside it either: a wrong operand, a wrong order or a lost store moves a
// trig result by ~1e-2, not by ~1e-13.
const EPS = 1e-12;
function near(a, b) { return a - b < EPS && b - a < EPS; }
function say(label, value) { console.log(label + '=' + value); }

const scene = new Scene();
const camera = new PerspectiveCamera(75, 2, 0.1, 1000);
const geometry = new BoxGeometry(1, 1, 1);
const material = new MeshBasicMaterial();
const mesh = new Mesh(geometry, material);
scene.add(mesh);

// --- 1. The module graph: nine imports resolved across 28 files ------------
// Each `type`/`is*` field is a string or `true` assigned in the class body of
// a DIFFERENT file from the one that reads it, so a wrong answer here means
// the graph, not the arithmetic.
say('scene.type', scene.type);                       // Scene.js:11
say('scene.isScene', scene.isScene);                 // Scene.js:9
say('camera.type', camera.type);                     // PerspectiveCamera.js:12
say('camera.isPerspectiveCamera', camera.isPerspectiveCamera);  // :10
say('material.type', material.type);                 // MeshBasicMaterial.js:13
say('material.color.isColor', material.color.isColor);
say('geometry.type', geometry.type);                 // BoxGeometry.js:11
say('mesh.isMesh', mesh.isMesh);                     // Mesh.js:41
say('mesh.isObject3D', mesh.isObject3D);             // Object3D.js:34, via super()

// --- 2. Prototype chains that cross module boundaries ----------------------
// Mesh -> Object3D and PerspectiveCamera -> Camera -> Object3D are `extends`
// links between separately compiled files.
say('mesh instanceof Mesh', mesh instanceof Mesh);
say('mesh instanceof Object3D', mesh instanceof Object3D);
say('camera instanceof Object3D', camera instanceof Object3D);
say('scene instanceof Object3D', scene instanceof Object3D);

// --- 3. Constructor arguments and defaults ---------------------------------
// PerspectiveCamera.js:6 `constructor( fov = 50, aspect = 1, near = 0.1,
// far = 2000 )`; the call above passes all four, so each is what was passed.
say('camera.fov', camera.fov);                       // 75
say('camera.aspect', camera.aspect);                 // 2
say('camera.near', camera.near);                     // 0.1
say('camera.far', camera.far);                       // 1000
say('camera.zoom', camera.zoom);                     // :19, literal 1
say('camera.filmGauge', camera.filmGauge);           // :27, literal 35

// --- 4. The scene graph ----------------------------------------------------
// Object3D.add (:319-345) sets object.parent and pushes onto this.children.
say('scene.children.length', scene.children.length);   // 1
say('mesh.parent===scene', mesh.parent === scene);     // true
say('scene.parent', scene.parent);                     // null (Object3D.js:37)

// --- 5. BoxGeometry structure: exact integers, derived from the source -----
// BoxGeometry.js:44-49 calls buildPlane six times, each with gridX = gridY = 1
// (all three *Segments default to 1). buildPlane emits (gridX+1)*(gridY+1) = 4
// vertices and 2 triangles = 6 indices per plane, and calls addGroup once.
//   vertices: 6 * 4  = 24      indices: 6 * 6 = 36      groups: 6
say('index.count', geometry.index.count);                          // 36
say('position.count', geometry.attributes.position.count);         // 24
say('normal.count', geometry.attributes.normal.count);             // 24
say('uv.count', geometry.attributes.uv.count);                     // 24
say('position.itemSize', geometry.attributes.position.itemSize);   // 3
say('uv.itemSize', geometry.attributes.uv.itemSize);               // 2
say('groups.length', geometry.groups.length);                      // 6

// --- 6. Typed arrays as real objects ---------------------------
// Float32BufferAttribute (BufferAttribute.js:615) is `new Float32Array(array)`;
// setIndex (BufferGeometry.js) picks Uint16 because the largest index is 23,
// which is below the Uint32 threshold. The `.constructor ===` identities are
// The constructor is one interned object, not a name.
say('position.array.length', geometry.attributes.position.array.length);   // 24*3
say('index.array.length', geometry.index.array.length);                    // 36
say('position.array is Float32Array',
    geometry.attributes.position.array.constructor === Float32Array);
say('index.array is Uint16Array',
    geometry.index.array.constructor === Uint16Array);
say('Float32Array.BYTES_PER_ELEMENT', Float32Array.BYTES_PER_ELEMENT);     // 4
say('Uint16Array.BYTES_PER_ELEMENT', Uint16Array.BYTES_PER_ELEMENT);       // 2

// --- 7. Math.random reached, without pinning a random number ---------------
// Object3D.js:36 and BufferGeometry.js both call MathUtils.generateUUID(),
// which is four Math.random() draws formatted through a 256-entry lookup table:
// 16 two-character entries plus 4 hyphens = 36 characters. The VALUE is
// nondeterministic by design and is never printed; that two independent 122-bit
// draws differ is what proves the generator is not a constant.
say('typeof uuid', typeof mesh.uuid);                       // string
say('uuid.length', mesh.uuid.length);                       // 36
say('uuids differ', mesh.uuid !== geometry.uuid);           // true

// --- 8. Exact matrix arithmetic: compose with an identity quaternion -------
// Object3D.updateMatrix (:566) is matrix.compose(position, quaternion, scale).
// Matrix4.compose with quaternion (0,0,0,1) has every xx..wz product equal to
// 0, so it reduces to te = diag(sx,sy,sz) with the position in the fourth
// column and te[15] = 1 — every entry a product or difference of exact small
// integers, so IEEE-754 (6.1.6.1) computes it with no rounding at all.
scene.updateMatrixWorld(true);
say('root world===local',
    scene.matrixWorld.elements.join(',') === scene.matrix.elements.join(','));
say('identity', scene.matrixWorld.elements.join(','));
//     1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1

scene.position.set(10, 20, 30);
scene.scale.set(2, 2, 2);
mesh.position.set(1, 2, 3);
scene.updateMatrixWorld(true);
say('scene.matrix', scene.matrix.elements.join(','));
//     2,0,0,0, 0,2,0,0, 0,0,2,0, 10,20,30,1
say('mesh.matrix', mesh.matrix.elements.join(','));
//     1,0,0,0, 0,1,0,0, 0,0,1,0, 1,2,3,1
// multiplyMatrices(scene.matrixWorld, mesh.matrix): the linear part is
// 2I * I = 2I, and the translation is 2I*(1,2,3) + (10,20,30) = (12,24,36).
say('mesh.matrixWorld', mesh.matrixWorld.elements.join(','));
//     2,0,0,0, 0,2,0,0, 0,0,2,0, 12,24,36,1

// Idempotence: updateMatrixWorld is a pure recomputation from position /
// quaternion / scale, so running it again cannot move anything.
const beforeAgain = mesh.matrixWorld.elements.join(',');
scene.updateMatrixWorld(true);
scene.updateMatrixWorld(true);
say('updateMatrixWorld idempotent', mesh.matrixWorld.elements.join(',') === beforeAgain);

// --- 9. The animation loop: invariants, not accumulated digits -------------
// This is the milestone program. 60 frames of `rotation.x += 0.01` and
// `rotation.y += 0.02` accumulate rounding in every element, so no element is
// pinned. What is pinned is what stays true of ANY rotation matrix.
scene.position.set(0, 0, 0);
scene.scale.set(1, 1, 1);
mesh.position.set(0, 0, 0);
mesh.rotation.set(0, 0, 0);
for (let frame = 0; frame < 60; frame++) {
    mesh.rotation.x += 0.01;
    mesh.rotation.y += 0.02;
    scene.updateMatrixWorld();
}
const e = mesh.matrixWorld.elements;

// The object sits at the origin under an identity parent, so the translation
// column is a sum of products of exact zeros — exactly 0, not nearly 0.
say('translation column', e[12] + ',' + e[13] + ',' + e[14] + ',' + e[15]);
//     0,0,0,1

// A rotation is orthonormal: each column of the 3x3 part is a unit vector and
// distinct columns are perpendicular.
const c0 = new Vector3(e[0], e[1], e[2]);
const c1 = new Vector3(e[4], e[5], e[6]);
const c2 = new Vector3(e[8], e[9], e[10]);
say('columns unit', near(c0.dot(c0), 1) && near(c1.dot(c1), 1) && near(c2.dot(c2), 1));
say('columns orthogonal',
    near(c0.dot(c1), 0) && near(c0.dot(c2), 0) && near(c1.dot(c2), 0));
// det = 1 distinguishes a rotation from a reflection; |det| = 1 alone would not.
say('determinant 1', near(mesh.matrixWorld.determinant(), 1));

// M^-1 * M = I, elementwise. This exercises the cofactor expansion in
// Matrix4.invert on a matrix bronze itself produced.
const round = mesh.matrixWorld.clone().invert().multiply(mesh.matrixWorld).elements;
let identityOk = true;
for (let i = 0; i < 16; i++) {
    const want = (i === 0 || i === 5 || i === 10 || i === 15) ? 1 : 0;
    if (!near(round[i], want)) identityOk = false;
}
say('inverse round trip', identityOk);

// Re-running after the loop must not move a single element.
const afterLoop = mesh.matrixWorld.elements.join(',');
scene.updateMatrixWorld(true);
say('post-loop idempotent', mesh.matrixWorld.elements.join(',') === afterLoop);

// --- 10. Generators: Vector3's *[Symbol.iterator] --------------
// Vector3.js:711 is `*[ Symbol.iterator ]() { yield this.x; yield this.y; yield
// this.z; }` — three straight-line yields, the exact subset bronze desugars.
// for-of over it (14.7.5) visits them in order.
const parts = [];
for (const component of new Vector3(7, 8, 9)) parts.push(component);
say('vector iterator', parts.join(','));                 // 7,8,9

// --- 11. Exact library arithmetic -----------------------------------------
// 3^2 + 4^2 = 25 and sqrt(25) = 5, all exactly representable, and 21.3.2.17
// requires sqrt to be correctly rounded — so this is 5, not 4.999...
say('length', new Vector3(3, 4, 0).length());                          // 5
say('dot', new Vector3(1, 2, 3).dot(new Vector3(4, 5, 6)));            // 4+10+18
const cross = new Vector3(1, 0, 0).cross(new Vector3(0, 1, 0));
say('cross', cross.x + ',' + cross.y + ',' + cross.z);                 // 0,0,1

// Quaternion.setFromEuler with all angles 0: every cos is 1 and every sin is
// 0, so x = y = z = 0 and w = 1*1*1 - 0*0*0 = 1. Exact.
const q = new Quaternion().setFromEuler(new Euler(0, 0, 0, 'XYZ'));
say('quaternion', q.x + ',' + q.y + ',' + q.z + ',' + q.w);            // 0,0,0,1
say('euler order', mesh.rotation.order);                               // XYZ
