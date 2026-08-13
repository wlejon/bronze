// The classic Three.js CPU profile render benchmark:
// 2,000 animated meshes organized in hierarchical clusters with per-frame
// matrix world updates, Euler/Quaternion rotations, and dynamic geometry churn.

import { Scene } from '../tests/oracle/threejs/three/scenes/Scene.js';
import { PerspectiveCamera } from '../tests/oracle/threejs/three/cameras/PerspectiveCamera.js';
import { Object3D } from '../tests/oracle/threejs/three/core/Object3D.js';
import { Mesh } from '../tests/oracle/threejs/three/objects/Mesh.js';
import { BoxGeometry } from '../tests/oracle/threejs/three/geometries/BoxGeometry.js';
import { MeshBasicMaterial } from '../tests/oracle/threejs/three/materials/MeshBasicMaterial.js';
import { Vector3 } from '../tests/oracle/threejs/three/math/Vector3.js';

function makeLCG(seed) {
  let s = seed % 2147483647;
  if (s <= 0) s += 2147483646;
  return function () {
    s = (s * 16807) % 2147483647;
    return (s - 1) / 2147483646;
  };
}

function runMeshChurn2k(meshCount, frames) {
  const rng = makeLCG(98765);
  const scene = new Scene();
  const camera = new PerspectiveCamera(60, 16 / 9, 0.1, 1000);
  camera.position.set(0, 0, 100);

  const sharedGeometry = new BoxGeometry(1, 1, 1);
  const sharedMaterial = new MeshBasicMaterial();

  const clusters = [];
  const clusterCount = 20;
  const meshesPerCluster = Math.floor(meshCount / clusterCount);

  for (let c = 0; c < clusterCount; c++) {
    const cluster = new Object3D();
    cluster.position.set(
      (rng() - 0.5) * 80.0,
      (rng() - 0.5) * 80.0,
      (rng() - 0.5) * 80.0
    );
    scene.add(cluster);
    clusters.push(cluster);
  }

  const meshes = [];
  const meshData = [];

  for (let i = 0; i < meshCount; i++) {
    // 1 in 10 meshes gets its own geometry instance for dynamic vertex churn
    const hasOwnGeometry = i % 10 === 0;
    const geom = hasOwnGeometry ? new BoxGeometry(1, 1, 1) : sharedGeometry;
    const mesh = new Mesh(geom, sharedMaterial);

    mesh.position.set(
      (rng() - 0.5) * 20.0,
      (rng() - 0.5) * 20.0,
      (rng() - 0.5) * 20.0
    );
    mesh.rotation.set(
      rng() * Math.PI * 2,
      rng() * Math.PI * 2,
      rng() * Math.PI * 2
    );
    const scale = 0.5 + rng() * 1.5;
    mesh.scale.set(scale, scale, scale);

    const clusterIdx = i % clusterCount;
    clusters[clusterIdx].add(mesh);
    meshes.push(mesh);

    meshData.push({
      rotSpeedX: (rng() - 0.5) * 0.05,
      rotSpeedY: (rng() - 0.5) * 0.05,
      rotSpeedZ: (rng() - 0.5) * 0.05,
      hasOwnGeometry: hasOwnGeometry,
    });
  }

  // Initial matrix update
  scene.updateMatrixWorld(true);

  // Animation frames loop
  for (let f = 0; f < frames; f++) {
    const time = f * 0.016;

    // Rotate clusters
    for (let c = 0; c < clusterCount; c++) {
      clusters[c].rotation.y += 0.01;
      clusters[c].rotation.x += 0.005;
    }

    // Update individual meshes & churn geometry
    for (let i = 0; i < meshCount; i++) {
      const m = meshes[i];
      const data = meshData[i];

      m.rotation.x += data.rotSpeedX;
      m.rotation.y += data.rotSpeedY;
      m.rotation.z += data.rotSpeedZ;

      m.position.y += Math.sin(time + i) * 0.02;

      // Dynamic vertex buffer churn on unshared geometry
      if (data.hasOwnGeometry && f % 2 === 0) {
        const posAttr = m.geometry.attributes.position;
        const arr = posAttr.array;
        for (let v = 0; v < arr.length; v += 3) {
          arr[v] += Math.sin(time + v) * 0.001;
          arr[v + 1] += Math.cos(time + v) * 0.001;
        }
        posAttr.needsUpdate = true;
      }
    }

    // Recalculate complete scene graph world transforms
    scene.updateMatrixWorld(true);
  }

  // Deterministic validation checksum over all world matrices
  let matrixSum = 0;
  for (let i = 0; i < meshCount; i++) {
    const el = meshes[i].matrixWorld.elements;
    for (let k = 0; k < 16; k++) {
      matrixSum += el[k];
    }
  }

  const checksum = Math.round(matrixSum * 100);
  console.log('mesh_churn_2k checksum=' + checksum);
}

runMeshChurn2k(2000, 30);
