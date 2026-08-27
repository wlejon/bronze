// Three.js InstancedMesh benchmark:
// 5,000 instances with per-frame matrix transformation and color updates.

import {
  Scene,
  PerspectiveCamera,
  BoxGeometry,
  InstancedMesh,
  MeshBasicMaterial,
  Object3D,
  Color
} from 'three';
import { measure } from './harness.js';

function makeLCG(seed) {
  let s = seed % 2147483647;
  if (s <= 0) s += 2147483646;
  return function () {
    s = (s * 16807) % 2147483647;
    return (s - 1) / 2147483646;
  };
}

function runInstancedMeshChurn(instanceCount, frames) {
  const rng = makeLCG(12345);
  const scene = new Scene();
  const camera = new PerspectiveCamera(60, 16 / 9, 0.1, 1000);
  camera.position.set(0, 0, 50);

  const geometry = new BoxGeometry(0.5, 0.5, 0.5);
  const material = new MeshBasicMaterial();
  const instancedMesh = new InstancedMesh(geometry, material, instanceCount);
  scene.add(instancedMesh);

  const dummy = new Object3D();
  const tempColor = new Color();

  // Pre-generate deterministic instance base offsets
  const basePos = [];
  const rotVel = [];
  for (let i = 0; i < instanceCount; i++) {
    basePos.push({
      x: (rng() - 0.5) * 100.0,
      y: (rng() - 0.5) * 100.0,
      z: (rng() - 0.5) * 100.0
    });
    rotVel.push({
      rx: (rng() - 0.5) * 0.1,
      ry: (rng() - 0.5) * 0.1,
      rz: (rng() - 0.5) * 0.1
    });
  }

  // Animation frames loop
  for (let f = 0; f < frames; f++) {
    const time = f * 0.016;

    for (let i = 0; i < instanceCount; i++) {
      const bp = basePos[i];
      const rv = rotVel[i];

      const x = bp.x + Math.sin(time + i * 0.02) * 2.0;
      const y = bp.y + Math.cos(time + i * 0.02) * 2.0;
      const z = bp.z + Math.sin(time + i * 0.01) * 2.0;

      dummy.position.set(x, y, z);
      dummy.rotation.set(f * rv.rx, f * rv.ry, f * rv.rz);

      const s = 0.5 + 0.5 * Math.sin(time + i * 0.05);
      dummy.scale.set(s, s, s);
      dummy.updateMatrix();

      instancedMesh.setMatrixAt(i, dummy.matrix);

      const r = 0.5 + 0.5 * Math.sin(time + bp.x * 0.1);
      const g = 0.5 + 0.5 * Math.cos(time + bp.y * 0.1);
      const b = 0.5 + 0.5 * Math.sin(time + bp.z * 0.1);
      tempColor.setRGB(r, g, b);
      instancedMesh.setColorAt(i, tempColor);
    }

    instancedMesh.instanceMatrix.needsUpdate = true;
    if (instancedMesh.instanceColor !== null) {
      instancedMesh.instanceColor.needsUpdate = true;
    }

    scene.updateMatrixWorld(true);
  }

  // Deterministic validation checksum over all matrix and color elements
  let matrixSum = 0;
  const matArray = instancedMesh.instanceMatrix.array;
  for (let k = 0; k < matArray.length; k++) {
    matrixSum += matArray[k];
  }

  let colorSum = 0;
  const colArray = instancedMesh.instanceColor.array;
  for (let k = 0; k < colArray.length; k++) {
    colorSum += colArray[k];
  }

  const checksum = Math.round((matrixSum + colorSum) * 100);
  return checksum;
}

console.log('instanced_mesh_churn checksum=' +
  measure('instanced_mesh_churn', () => runInstancedMeshChurn(5000, 30)));
