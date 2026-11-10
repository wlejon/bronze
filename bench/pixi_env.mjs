// The environment pixi.mjs reads, for the pixi drivers in this directory.
//
// At import: tests/oracle/pixi/setup.mjs's globals, defined only where the
// engine has none (node defines `navigator` as a getter-only global, which
// setup.mjs's plain assignment throws on). Build these drivers with
// `--host-globals tests/oracle/pixi/host.globals`.
//
// After import, `installStubAdapter(PIXI)`: a DOMAdapter whose canvases have
// no WebGL context (pixi's shader setup then takes its no-GL defaults) and a
// 2D context that records nothing and measures every glyph 0.55 em wide,
// ascent 0.8 em, descent 0.2 em — bro's realworld runner's measuring stub —
// so node and bronze measure and lay out text alike.
if (globalThis.navigator === undefined) {
  globalThis.navigator = { userAgent: 'bronze', platform: 'bronze', maxTouchPoints: 0 };
}
if (globalThis.Intl === undefined) {
  globalThis.Intl = {};
}

function make2d(canvas) {
  return {
    canvas,
    font: '10px sans-serif', letterSpacing: '0px', textLetterSpacing: '0px',
    textBaseline: 'alphabetic', lineWidth: 1, miterLimit: 10, lineJoin: 'miter', lineCap: 'butt',
    fillStyle: '#000', strokeStyle: '#000', shadowColor: 'rgba(0,0,0,0)', shadowBlur: 0,
    shadowOffsetX: 0, shadowOffsetY: 0, globalAlpha: 1,
    ops: 0,
    measureText(s) {
      const m = /(\d+(?:\.\d+)?)px/.exec(this.font);
      const px = m ? Number(m[1]) : 10;
      const w = String(s).length * px * 0.55;
      return { width: w, actualBoundingBoxLeft: 0, actualBoundingBoxRight: w, actualBoundingBoxAscent: px * 0.8, actualBoundingBoxDescent: px * 0.2, fontBoundingBoxAscent: px * 0.8, fontBoundingBoxDescent: px * 0.2 };
    },
    resetTransform() { this.ops++; },
    setTransform() { this.ops++; },
    scale() { this.ops++; },
    clearRect() { this.ops++; },
    fillRect() { this.ops++; },
    fillText() { this.ops++; },
    strokeText() { this.ops++; },
    save() { this.ops++; },
    restore() { this.ops++; },
    createLinearGradient() { return { addColorStop() {} }; },
    createPattern() { return {}; },
  };
}

function makeCanvas(width, height) {
  const canvas = { width: width || 0, height: height || 0, style: {}, _ctx: null };
  canvas.getContext = function (kind) {
    if (kind !== '2d') return null;
    if (!this._ctx) this._ctx = make2d(this);
    return this._ctx;
  };
  return canvas;
}

export function installStubAdapter(PIXI) {
  PIXI.DOMAdapter.set({
    createCanvas: (w, h) => makeCanvas(w, h),
    createImage: () => ({}),
    getCanvasRenderingContext2D: () => Object,
    getWebGLRenderingContext: () => Object,
    getNavigator: () => globalThis.navigator,
    getBaseUrl: () => 'file:///',
    getFontFaceSet: () => null,
    fetch: () => { throw new Error('no fetch in the pixi drivers'); },
    parseXML: () => { throw new Error('no XML in the pixi drivers'); },
  });
  PIXI.CanvasTextMetrics.__canvas = makeCanvas(10, 10);
  PIXI.CanvasTextMetrics.__context = null;
  PIXI.CanvasTextMetrics._experimentalLetterSpacingSupported = false;
  // pixi's own fallback when Intl.Segmenter is absent (bronze, bro): node
  // has a Segmenter, and the drivers time the same JS under both engines.
  PIXI.CanvasTextMetrics.graphemeSegmenter = (s) => [...s];
}
