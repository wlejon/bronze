// Interpreted scene render benchmark for bro-headless.
// Sets up a 3D scene with animated meshes, advances virtual time for N frames,
// and computes a state checksum at the end.

const canvas = document.createElement('canvas');
canvas.setAttribute('width', '640');
canvas.setAttribute('height', '480');
document.body.appendChild(canvas);
flush();

const scene = canvas.getContext('scene');
if (scene) {
  scene.setCamera({
    fov: 60,
    near: 0.1,
    far: 1000,
    position: [0, 0, 50],
    target: [0, 0, 0],
    up: [0, 1, 0]
  });

  const meshCount = 200;
  const meshes = [];

  for (let i = 0; i < meshCount; i++) {
    const mesh = scene.createMesh({ mesh: 'box', color: 'cyan' });
    const angle = (i / meshCount) * Math.PI * 2;
    const radius = 10 + (i % 5) * 5;
    mesh.position = [
      Math.cos(angle) * radius,
      (i % 10) - 5,
      Math.sin(angle) * radius
    ];
    meshes.push(mesh);
  }

  const frames = 30;
  for (let f = 0; f < frames; f++) {
    const t = f * 0.05;
    for (let i = 0; i < meshCount; i++) {
      const m = meshes[i];
      m.x = m.x + Math.sin(t + i) * 0.1;
      m.y = m.y + Math.cos(t + i) * 0.1;
    }
    advanceTime(16);
  }

  let posSum = 0;
  for (let i = 0; i < meshCount; i++) {
    posSum += meshes[i].x + meshes[i].y;
  }

  console.log('APP render_interpreted checksum=' + Math.round(posSum * 100));
} else {
  console.log('APP scene context unavailable');
}
