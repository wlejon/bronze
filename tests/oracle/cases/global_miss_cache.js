// A feature test of a global that does not exist runs per call; its miss is
// cached, and must stop being one the moment the name appears.
function hasFrame() { return typeof VideoFrame !== 'undefined'; }
function readFrame() { try { return VideoFrame; } catch (e) { return e.constructor.name; } }
let n = 0;
for (let i = 0; i < 1000; i++) if (hasFrame()) n++;
console.log(n, readFrame());

// Created by a sloppy write.
(function () { VideoFrame = 'by write'; })();
console.log(hasFrame(), readFrame());
delete globalThis.VideoFrame;
console.log(hasFrame(), readFrame());

// Created by a define, by a property set on globalThis, and after many other
// globals were added in between.
for (let i = 0; i < 1000; i++) hasFrame();
Object.defineProperty(globalThis, 'VideoFrame', { value: 'by define', configurable: true });
console.log(hasFrame(), readFrame());
delete globalThis.VideoFrame;
for (let i = 0; i < 1000; i++) hasFrame();
for (let i = 0; i < 50; i++) globalThis['filler' + i] = i;
globalThis.VideoFrame = 'by set';
console.log(hasFrame(), readFrame(), filler49);
delete globalThis.VideoFrame;

// Strict code reading a missing name still throws.
(function () {
  'use strict';
  for (let i = 0; i < 100; i++) hasFrame();
  try { VideoFrame; } catch (e) { console.log('strict', e.constructor.name); }
})();
