// A method held in an own slot that differs between objects of one shape (a
// renderer's uniforms, each with the setter for its type): the call site
// sees one shape and several functions in the slot. Each call must run the
// function the receiver holds.
function setF(v) { this.v = v * 1.5; return 1; }
function setI(v) { this.v = v | 0; return 2; }
function setS(v) { this.v = "s" + v; return 3; }
function setA(v) { this.v = [v]; return 4; }
function Uniform(id, kind) {
  this.id = id;
  this.v = null;
  this.setValue = [setF, setI, setS, setA][kind];
}
function upload(list, x) {
  let n = 0;
  for (let i = 0; i < list.length; i++) n += list[i].setValue(x + i);
  return n;
}
function main() {
  const list = [];
  for (let i = 0; i < 12; i++) list.push(new Uniform(i, i % 3 === 0 ? 0 : i % 4));
  let s = 0;
  for (let r = 0; r < 60000; r++) s += upload(list, r);
  console.log(s, list.map((u) => JSON.stringify(u.v)).join(" "));
}
main();
