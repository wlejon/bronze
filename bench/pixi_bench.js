// Pure-compute Pixi.js v8.19.0 benchmark against the vendored pixi.mjs bundle.
// Exercises Container/Sprite scene-graph transforms (position, scale, rotation,
// pivot, updateLocalTransform, toGlobal), Matrix 2D affine operations
// (setTransform, appendFrom, apply, applyInverse), Rectangle hit-testing
// (contains), and Color/tint BGR conversions.

import '../tests/oracle/pixi/setup.mjs';
import { Container, Sprite, Texture, Matrix, Point, Rectangle, Color } from '../tests/oracle/pixi/pixi.mjs';
import { measure } from './harness.js';

function makeLCG(seed) {
  let s = seed % 2147483647;
  if (s <= 0) s += 2147483646;
  return function () {
    s = (s * 16807) % 2147483647;
    return (s - 1) / 2147483646;
  };
}

function runPixiSceneBench(spriteCount, frames) {
  const rng = makeLCG(54321);
  const stage = new Container();
  const groupCount = 16;
  const groups = [];

  for (let g = 0; g < groupCount; g++) {
    const group = new Container();
    group.position.set((rng() - 0.5) * 400.0, (rng() - 0.5) * 400.0);
    group.scale.set(0.8 + rng() * 0.4, 0.8 + rng() * 0.4);
    group.pivot.set(16.0, 16.0);
    stage.addChild(group);
    groups.push(group);
  }

  const tex = Texture.WHITE;
  const sprites = [];
  const velocities = [];

  for (let i = 0; i < spriteCount; i++) {
    const sp = new Sprite(tex);
    sp.position.set((rng() - 0.5) * 200.0, (rng() - 0.5) * 200.0);
    const sc = 0.5 + rng() * 1.5;
    sp.scale.set(sc, sc);
    sp.pivot.set(8.0, 8.0);
    sp.rotation = rng() * Math.PI * 2.0;
    sp.alpha = 0.5 + rng() * 0.5;
    groups[i % groupCount].addChild(sp);
    sprites.push(sp);
    velocities.push({
      vx: (rng() - 0.5) * 2.5,
      vy: (rng() - 0.5) * 2.5,
      vr: (rng() - 0.5) * 0.08,
    });
  }

  const worldMat = new Matrix();
  const invMat = new Matrix();
  const tempPt = new Point(4.0, 4.0);
  const outPt = new Point(0.0, 0.0);
  const viewport = new Rectangle(-250.0, -250.0, 500.0, 500.0);
  const sharedColor = new Color();

  let accumX = 0.0;
  let accumY = 0.0;
  let visibleHits = 0;
  let bgrSum = 0;

  for (let f = 0; f < frames; f++) {
    const time = f * 0.016;

    for (let g = 0; g < groupCount; g++) {
      const grp = groups[g];
      grp.rotation += 0.01;
      grp.position.x += Math.sin(time + g) * 0.5;
      grp.position.y += Math.cos(time + g) * 0.5;
      grp.updateLocalTransform();
    }

    for (let i = 0; i < spriteCount; i++) {
      const sp = sprites[i];
      const vel = velocities[i];

      sp.position.x += vel.vx;
      sp.position.y += vel.vy;
      sp.rotation += vel.vr;
      sp.updateLocalTransform();

      const grp = groups[i % groupCount];
      worldMat.appendFrom(sp.localTransform, grp.localTransform);
      worldMat.apply(tempPt, outPt);

      if (viewport.contains(outPt.x, outPt.y)) {
        visibleHits++;
      }

      invMat.set(
        worldMat.a,
        worldMat.b,
        worldMat.c,
        worldMat.d,
        worldMat.tx,
        worldMat.ty
      );
      invMat.applyInverse(outPt, outPt);

      accumX += outPt.x + worldMat.tx * 0.001;
      accumY += outPt.y + worldMat.ty * 0.001;

      if ((i & 7) === 0) {
        const rgb = ((f * 17 + i * 31) & 0xffffff);
        sharedColor.setValue(rgb);
        bgrSum = (bgrSum + sharedColor.toBgrNumber()) | 0;
      }
    }
  }

  return Math.round(accumX + accumY) + visibleHits + (bgrSum & 0xffff);
}

console.log('pixi_scene checksum=' +
  measure('pixi_scene', () => runPixiSceneBench(2000, 40)));
