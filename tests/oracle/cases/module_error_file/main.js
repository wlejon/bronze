// Error.stack names the file each frame's position is in. A program's top
// level is one compiled function holding every module's top-level statements;
// its frames once all named the first imported module's file, with the
// importer's line numbers ("helper.js:24" for a throw at main.js:24). And an
// imported file's functions and classes are named as the source spelled them,
// not by the linker's rename of their bindings ("mod1.helperThrows").
import dflt, {
  topLevelStack, helperThrows, Shape, arrow, expr, assigned, gen, later, localName,
} from "./helper.js";

function frames(s) {
  const out = [];
  for (const l of s.split("\n").slice(1)) {
    const m = l.match(/([A-Za-z_]+\.js):(\d+):\d+\)?$/);
    if (!m) continue;
    const named = l.match(/at (\S+) \(/);
    out.push((named ? named[1] : "<top>") + "@" + m[1] + ":" + m[2]);
  }
  return out.join(",");
}

console.log(frames(topLevelStack));
try {
  notDefinedAnywhere(1);
} catch (e) {
  console.log(e.name, frames(e.stack));
}
try {
  helperThrows();
} catch (e) {
  console.log(e.name, frames(e.stack));
}
console.log(frames(new Shape().area()));
console.log(frames(new Error("main top level").stack));
console.log([helperThrows, Shape, arrow, expr, assigned, gen, later, dflt].map((f) => f.name).join(","));
console.log(localName, String(helperThrows).split("\n")[0]);
