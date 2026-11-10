// pixi.js v8.19.0 Graphics redraw, the per-frame work of bro's realworld
// pixi_graphics scene (bro/bench/realworld/scenes/pixi_graphics.js) with the
// GPU left out. N Graphics objects are cleared and redrawn every frame with an
// animated shape, filled and stroked; the frame then runs what WebGLRenderer
// runs for them on the CPU: updateRenderGroupTransforms, and an instruction
// rebuild through the real GraphicsPipe / GraphicsContextSystem / BatcherPipe
// (buildContextBatches: shape building, earcut triangulation, buildLine stroke
// extrusion; the per-renderable BatchableGraphics clones; DefaultBatcher add,
// break, packAttributes / packIndex; the geometry buffers' setDataWithSize).
// The renderer those systems are handed is a stub of the fields they read; no
// GL call is made, so what is not covered is the upload and draw.

import { installStubAdapter } from './pixi_env.mjs';
import * as PIXI from '../tests/oracle/pixi/pixi.mjs';
import { measure } from './harness.js';

installStubAdapter(PIXI);

function makeLCG(seed) {
  let s = seed % 2147483647;
  if (s <= 0) s += 2147483646;
  return function () {
    s = (s * 16807) % 2147483647;
    return (s - 1) / 2147483646;
  };
}

function stubRenderer() {
  const noop = { add() {} };
  const renderer = {
    uid: 1,
    _roundPixels: 0,
    resolution: 1,
    limits: { maxBatchableTextures: 16 },
    runners: { contextChange: noop, resolutionChange: noop },
    gc: { now: 0, addResourceHash() {} },
    renderPipes: {},
    texture: { initSource() {} },
  };
  renderer.graphicsContext = new PIXI.GraphicsContextSystem(renderer);
  renderer.renderPipes.batch = new PIXI.BatcherPipe(renderer, {});
  return renderer;
}

function runGraphicsBench(n, frames) {
  const rng = makeLCG(12345);
  const W = 1280, H = 720;
  const stage = new PIXI.Container({ isRenderGroup: true });
  const cols = Math.max(1, Math.ceil(Math.sqrt(n * W / H)));
  const cell = W / cols;
  const items = [];
  for (let i = 0; i < n; i++) {
    const g = new PIXI.Graphics();
    g.x = (i % cols + 0.5) * cell;
    g.y = (Math.floor(i / cols) + 0.5) * cell;
    stage.addChild(g);
    items.push({ g, kind: i % 6, phase: rng() * Math.PI * 2, fill: Math.floor(rng() * 0xffffff), line: Math.floor(rng() * 0xffffff) });
  }
  const renderer = stubRenderer();
  const graphicsPipe = new PIXI.GraphicsPipe(renderer, { contextChange() {} });
  const batchPipe = renderer.renderPipes.batch;
  const instructionSet = new PIXI.InstructionSet();

  const draw = (it, t) => {
    const g = it.g, r = cell * (0.3 + 0.1 * Math.sin(t * 2 + it.phase));
    g.clear();
    switch (it.kind) {
      case 0: g.circle(0, 0, r); break;
      case 1: g.rect(-r, -r * 0.7, r * 2, r * 1.4); break;
      case 2: g.roundRect(-r, -r, r * 2, r * 2, r * 0.3); break;
      case 3: g.star(0, 0, 5, r, r * 0.45, t + it.phase); break;
      case 4: g.ellipse(0, 0, r, r * 0.6); break;
      default: {
        const pts = [];
        for (let k = 0; k < 7; k++) { const a = k / 7 * Math.PI * 2 + t; pts.push(Math.cos(a) * r * (k % 2 ? 0.6 : 1), Math.sin(a) * r * (k % 2 ? 0.6 : 1)); }
        g.poly(pts);
      }
    }
    g.fill({ color: it.fill, alpha: 0.85 });
    g.stroke({ width: 2, color: it.line });
    g.rotation = t * 0.5 + it.phase;
  };

  let check = 0;
  for (let f = 0; f < frames; f++) {
    const t = f / 60;
    for (let i = 0; i < items.length; i++) draw(items[i], t);
    PIXI.updateRenderGroupTransforms(stage.renderGroup, true);
    instructionSet.reset();
    batchPipe.buildStart(instructionSet);
    for (let i = 0; i < items.length; i++) {
      const g = items[i].g;
      graphicsPipe.validateRenderable(g);
      graphicsPipe.addRenderable(g, instructionSet);
      g.didViewUpdate = false;
    }
    batchPipe.buildEnd(instructionSet);
    const batcher = batchPipe._activeBatch;
    const f32 = batcher.attributeBuffer.float32View;
    let s = 0;
    for (let k = 0; k < batcher.attributeSize; k += 6 * 17) s += f32[k] + f32[k + 1];
    check = (check + Math.round(s) + batcher.indexSize + instructionSet.instructionSize) | 0;
  }
  return check;
}

console.log('pixi_graphics checksum=' +
  measure('pixi_graphics', () => runGraphicsBench(2000, 30)));
