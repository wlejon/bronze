console.log('base');

// One counter for the whole graph. If `base.js` were evaluated once per
// importer, `tick()` would answer 1 twice below.
export let inits = 0;

export function tick() {
  inits += 1;
  return inits;
}
