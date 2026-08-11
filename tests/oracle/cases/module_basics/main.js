// The import and export forms, and the order they make things happen in
// (docs/0023). A module's dependencies are evaluated before its own body, in
// the order its import declarations are written, so 'side effect' precedes
// everything below.
import './side.js';
import theDefault, { label, twice, describe, secret as hidden } from './util.js';

// The same name as util.js's own `helper`, which is not exported. Both files
// keep theirs.
function helper() {
  return 'main helper';
}

console.log(label);
console.log(twice(21));
console.log(hidden);
console.log(theDefault());
console.log(describe());
console.log(helper());
