// THE REGISTER PROBE for `env_slot_kernel`: the same arithmetic, the same
// iteration count and the same checksum (`126000020` / `12600020`), with the
// hot state in bindings no closure captures — so lowering gives them SSA
// registers instead of an environment record, and every load and store the
// kernel's loop is made of goes away.
//
// It is not a benchmark and it is not a shippable shape: the whole point of
// `env_slot_kernel` is the factory closure that three.js's `WebGLState` is,
// and flattening it away is exactly what a real program cannot do. It is the
// one measurement that separates "an environment slot access costs something"
// from "the state being in MEMORY at all costs something", and it is run
// under node too, because node is compiling the same two shapes and the
// question is what node does with the first that bronze does not.
//
// Run it the way every kernel here is run: it times its own loop through
// bench/harness.js and prints ns_per_iter, so it is directly comparable with
// `env_slot_kernel`'s figure from the same session.
import { measure } from './harness.js';

const ITERS = 6000000;

function render(iters) {
  let currentBlending = 0;
  let currentSrc = 0;
  let currentDst = 0;
  let currentProgram = 0;
  let currentDepth = 0;
  let stateChanges = 0;
  let frames = 0;
  let hits = 0;

  for (let i = 0; i < iters; i++) {
    const k = i & 7;
    const blending = k;
    const src = k + 1;
    const dst = k + 2;
    if (blending !== currentBlending) {
      currentBlending = blending;
      stateChanges = stateChanges + 1;
    }
    if (src !== currentSrc || dst !== currentDst) {
      currentSrc = src;
      currentDst = dst;
      stateChanges = stateChanges + 2;
    }
    const program = (i >> 1) & 3;
    if (program !== currentProgram) {
      currentProgram = program;
      stateChanges = stateChanges + 1;
      hits = hits + 1;
    }
    const depthFunc = i & 3;
    if (depthFunc !== currentDepth) {
      currentDepth = depthFunc;
      stateChanges = stateChanges + 1;
    }
    frames = frames + 1;
  }

  return stateChanges * 3 + frames * 7 + currentBlending + currentSrc + currentDst +
         currentProgram + currentDepth + hits;
}

const checksum = measure('env_slot_kernel_registers', () => render(ITERS), ITERS);
console.log(`env_slot iters=${ITERS} checksum=${checksum}`);
