// pixi.js v8.19.0 Text churn, the per-frame CPU work of bro's realworld
// pixi_text scene (bro/bench/realworld/scenes/pixi_text.js) with the canvas
// rasterisation and the GPU left out. N Text objects in four TextStyles have
// their string replaced every frame; the frame then runs what WebGLRenderer
// runs for them: updateRenderGroupTransforms and an instruction rebuild
// through the real CanvasTextPipe / CanvasTextSystem / BatcherPipe: the
// Text's styleKey (TextStyle.styleKey, generateTextStyleKey), the texture
// cache's reference counting, CanvasTextGenerator.getCanvasAndContext
// (CanvasTextMetrics.measureText and its measurement cache, font strings,
// CanvasPool), _renderTextToCanvas's fill/stroke style setup and letter-spacing
// draw, getPo2TextureFromSource / TexturePool, updateTextBounds, and the
// batcher's packQuadAttributes. The 2D context is pixi_env.mjs's stub: glyphs
// are measured by a fixed advance and fillText/strokeText draw nothing, so
// what is not covered is the glyph rasterisation and the texture upload.

import { installStubAdapter } from './pixi_env.mjs';
import * as PIXI from '../tests/oracle/pixi/pixi.mjs';
import { measure } from './harness.js';

installStubAdapter(PIXI);

const WORDS = ['alpha', 'bravo', 'charlie', 'delta', 'echo', 'foxtrot', 'golf', 'hotel', 'india', 'juliet'];

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
  renderer.canvasText = new PIXI.CanvasTextSystem(renderer);
  renderer.renderPipes.batch = new PIXI.BatcherPipe(renderer, {});
  return renderer;
}

function runTextBench(n, frames) {
  const rng = makeLCG(777);
  const W = 1280, H = 720;
  const stage = new PIXI.Container({ isRenderGroup: true });
  const styles = [
    new PIXI.TextStyle({ fontFamily: 'sans-serif', fontSize: 14, fill: 0xffffff }),
    new PIXI.TextStyle({ fontFamily: 'sans-serif', fontSize: 18, fill: 0xffd060, fontWeight: 'bold' }),
    new PIXI.TextStyle({ fontFamily: 'serif', fontSize: 16, fill: 0x80d0ff, stroke: { color: 0x000000, width: 2 } }),
    new PIXI.TextStyle({ fontFamily: 'monospace', fontSize: 12, fill: 0xa0ffa0 }),
  ];
  const cols = Math.max(1, Math.ceil(Math.sqrt(n * W / H / 4)));
  const cw = W / cols, chh = Math.max(14, cw / 4);
  const texts = [];
  for (let i = 0; i < n; i++) {
    const t = new PIXI.Text({ text: 'init', style: styles[i % styles.length] });
    t.x = (i % cols) * cw + 4;
    t.y = Math.floor(i / cols) * chh + 2;
    t.rotation = (rng() - 0.5) * 0.1;
    stage.addChild(t);
    texts.push({ t, k: Math.floor(rng() * 1000), w: Math.floor(rng() * WORDS.length) });
  }
  const renderer = stubRenderer();
  const textPipe = new PIXI.CanvasTextPipe(renderer);
  const batchPipe = renderer.renderPipes.batch;
  const instructionSet = new PIXI.InstructionSet();

  let check = 0;
  for (let f = 0; f < frames; f++) {
    for (let i = 0; i < texts.length; i++) {
      const it = texts[i];
      it.t.text = WORDS[(it.w + f) % WORDS.length] + ' ' + ((it.k + f * 7) % 1000);
    }
    PIXI.updateRenderGroupTransforms(stage.renderGroup, true);
    instructionSet.reset();
    batchPipe.buildStart(instructionSet);
    let rebuild = 0;
    for (let i = 0; i < texts.length; i++) {
      if (textPipe.validateRenderable(texts[i].t)) rebuild++;
    }
    for (let i = 0; i < texts.length; i++) {
      const t = texts[i].t;
      textPipe.addRenderable(t, instructionSet);
      t.didViewUpdate = false;
    }
    batchPipe.buildEnd(instructionSet);
    const batcher = batchPipe._activeBatch;
    const f32 = batcher.attributeBuffer.float32View;
    let s = 0;
    for (let k = 0; k < batcher.attributeSize; k += 6 * 7) s += f32[k] + f32[k + 1] + f32[k + 2] * 64;
    check = (check + Math.round(s) + rebuild + instructionSet.instructionSize) | 0;
  }
  return check;
}

console.log('pixi_text checksum=' +
  measure('pixi_text', () => runTextBench(400, 60)));
