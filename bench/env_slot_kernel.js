// Kernel isolation: a FACTORY CLOSURE whose hot state lives in captured
// variables, not object fields — three.js's `WebGLState` / `WebGLUniforms`
// shape. Every `setBlending`-style call reads several captured numeric flags,
// compares them and writes the ones that changed, so the whole cost is
// `env.get` / `env.set` and the comparisons over what they return.
//
// Two kinds of slot on purpose:
//
//   stateChanges, frames — every write in the program text is
//     `<slot> + <literal>`, so the greatest fixpoint in lower_scope.cpp proves
//     them Number with no manifest and no flag. These are the SOUND case.
//   currentBlending, currentSrc, currentDst, currentProgram, currentDepth —
//     written from PARAMETERS, which nothing proves. These fail the proof and
//     are the `--pins` env-slot case (`function WebGLState.<slot>: number`).
//
// The loop lives inside the factory so the calls are sibling-closure calls and
// what is measured is slot access rather than the boxed call boundary.
//
// It times its own loop through bench/harness.js and reports ns_per_iter, so
// the figure is directly comparable with `env_slot_kernel_registers.js` — the
// same arithmetic with nothing captured — from the same session.

import { measure } from './harness.js';

const ITERS = 6000000;

function WebGLState() {
  let currentBlending = 0;
  let currentSrc = 0;
  let currentDst = 0;
  let currentProgram = 0;
  let currentDepth = 0;
  let stateChanges = 0;
  let frames = 0;

  function setBlending(blending, src, dst) {
    if (blending !== currentBlending) {
      currentBlending = blending;
      stateChanges = stateChanges + 1;
    }
    if (src !== currentSrc || dst !== currentDst) {
      currentSrc = src;
      currentDst = dst;
      stateChanges = stateChanges + 2;
    }
  }

  function useProgram(program) {
    if (program !== currentProgram) {
      currentProgram = program;
      stateChanges = stateChanges + 1;
      return 1;
    }
    return 0;
  }

  function setDepthFunc(depthFunc) {
    if (depthFunc !== currentDepth) {
      currentDepth = depthFunc;
      stateChanges = stateChanges + 1;
    }
  }

  function endFrame() {
    frames = frames + 1;
  }

  function total() {
    return stateChanges * 3 + frames * 7 + currentBlending + currentSrc + currentDst +
           currentProgram + currentDepth;
  }

  function render(iters) {
    let hits = 0;
    for (let i = 0; i < iters; i++) {
      const k = i & 7;
      setBlending(k, k + 1, k + 2);
      hits = hits + useProgram((i >> 1) & 3);
      setDepthFunc(i & 3);
      endFrame();
    }
    return total() + hits;
  }

  return render;
}

const render = WebGLState();
const checksum = measure('env_slot_kernel', () => render(ITERS), ITERS);
console.log(`env_slot iters=${ITERS} checksum=${checksum}`);
