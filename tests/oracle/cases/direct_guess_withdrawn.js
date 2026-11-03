// A method call whose direct target lowering guesses by the method's name on
// a class (`Geometry.setIndex`) while the receivers the whole program builds
// hold another function under that key in an own slot (three.js's
// WebGLIndexedBufferRenderer). The whole-program prediction withdraws the
// guess (opt/wpo/stamp.cpp); the answers must be the receivers' own either way,
// including for a receiver that really is the class.

class Geometry {
  constructor() { this.index = null; }
  setIndex(i) { this.index = i; return "geometry " + i; }
}

function IndexedRenderer() {
  let type = 0;
  function setIndex(value) { type = value * 2; return "renderer " + type; }
  function render(n) { return "render " + n + " as " + type; }
  this.setIndex = setIndex;
  this.render = render;
}

const renderer = new IndexedRenderer();
const geometry = new Geometry();

function draw(target, i) {
  const a = target.setIndex(i);
  return a + " / " + (target.render ? target.render(i) : "-");
}

// Only renderers reach this site, so the prediction names their closure.
function drawIndexed(target, i) { return target.setIndex(i); }
for (let i = 0; i < 3; i++) console.log(drawIndexed(renderer, i));

for (let i = 0; i < 3; i++) console.log(draw(renderer, i));
console.log(draw(geometry, 7), geometry.index);
console.log(draw(renderer, 9));

// A plain object holding another function under the key.
console.log(draw({ setIndex: (v) => "plain " + v }, 4));
