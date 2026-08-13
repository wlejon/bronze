// Pure-compute Three.js math loop against vendored three.js modules.
// Measures Vector3, Matrix4, Euler, and Quaternion math performance:
// compositions, decompositions, quaternion rotations, matrix multiplications,
// inversions, and vector transformations.

import { Vector3 } from '../tests/oracle/threejs/three/math/Vector3.js';
import { Matrix4 } from '../tests/oracle/threejs/three/math/Matrix4.js';
import { Euler } from '../tests/oracle/threejs/three/math/Euler.js';
import { Quaternion } from '../tests/oracle/threejs/three/math/Quaternion.js';

function runThreeMathBench(iterations) {
  const vPos = new Vector3(1.5, 2.5, 3.5);
  const vScale = new Vector3(1.0, 1.0, 1.0);
  const vTarget = new Vector3(0.0, 0.0, 0.0);

  const euler = new Euler(0.1, 0.2, 0.3, 'XYZ');
  const quat = new Quaternion();
  quat.setFromEuler(euler);

  const mLocal = new Matrix4();
  const mRot = new Matrix4();
  const mWorld = new Matrix4();
  const mInv = new Matrix4();

  let accX = 0;
  let accY = 0;
  let accZ = 0;

  for (let i = 0; i < iterations; i++) {
    const t = i * 0.001;
    euler.set(
      0.1 + Math.sin(t) * 0.05,
      0.2 + Math.cos(t) * 0.05,
      0.3 + Math.sin(t * 0.5) * 0.05,
      'XYZ'
    );
    quat.setFromEuler(euler);

    vPos.set(1.0 + Math.sin(t), 2.0 + Math.cos(t), 3.0 + Math.sin(t * 2.0));
    vScale.set(1.0 + 0.1 * Math.cos(t), 1.0 + 0.1 * Math.sin(t), 1.0);

    mLocal.compose(vPos, quat, vScale);
    mRot.makeRotationY(0.01 * (i % 100));
    mWorld.multiplyMatrices(mLocal, mRot);

    mInv.copy(mWorld).invert();

    vTarget.set(i % 10, (i + 1) % 10, (i + 2) % 10);
    vTarget.applyMatrix4(mWorld);
    vTarget.applyMatrix4(mInv);

    accX += vTarget.x;
    accY += vTarget.y;
    accZ += vTarget.z;
  }

  const checksum = Math.round(accX + accY + accZ);
  console.log('three_math checksum=' + checksum);
}

runThreeMathBench(30000);
