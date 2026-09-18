// Error.stack: V8-compatible call-frame stack trace capture, formatting,
// Error.captureStackTrace and Error.stackTraceLimit.
//
// Stacks are path-dependent, so this case pins the invariants:
// - typeof e.stack === 'string'
// - the first line equals e.name + ': ' + e.message (or e.name alone when empty)
// - property descriptor attributes: enumerable: false, configurable: true
// - frame count and function names extracted via regex (/at (\S+)/g) for:
//   - nested call chain
//   - class method
//   - object-literal method
//   - a new (constructor)
//   - an async function after an await
//   - callback under Array.prototype.forEach (expect Array.forEach frame)
//   - subclass of Error
//   - Error.captureStackTrace with and without the fn argument
//   - Error.stackTraceLimit 0 / 2 / Infinity
//   - runtime TypeError from calling undefined
//   - stack property being writable / reassignable

function getFrames(stack) {
  const m = stack.match(/at (\S+)/g);
  return m ? m.map(s => s.replace('at ', '')) : [];
}

async function main() {
  // 1. Basic error and stack type
  const e1 = new Error('basic error');
  console.log(typeof e1.stack === 'string');

  // 2. First line equals name: message (and empty message)
  console.log(e1.stack.split('\n')[0] === e1.name + ': ' + e1.message);
  const eEmpty = new Error();
  console.log(eEmpty.stack.split('\n')[0] === 'Error');

  // 3. Descriptor attributes
  const desc = Object.getOwnPropertyDescriptor(e1, 'stack');
  console.log(desc.enumerable, desc.configurable);

  // 4. Nested call chain
  function f1() { return f2(); }
  function f2() { return f3(); }
  function f3() { return new Error('nested'); }
  const nestedErr = f1();
  const nestedFrames = getFrames(nestedErr.stack);
  console.log(nestedFrames.length >= 3);
  console.log(nestedFrames.slice(0, 3).join(','));

  // 5. Class method
  class Greeter {
    greet() {
      return new Error('inside greet');
    }
  }
  const greeterErr = new Greeter().greet();
  console.log(getFrames(greeterErr.stack)[0]);

  // 6. Object-literal method
  const obj = {
    method() {
      return new Error('inside obj method');
    }
  };
  const objErr = obj.method();
  console.log(getFrames(objErr.stack)[0]);

  // 7. A new (constructor)
  class Maker {
    constructor() {
      this.err = new Error('inside maker');
    }
  }
  const makerErr = new Maker().err;
  console.log(getFrames(makerErr.stack)[0]);
  console.log(makerErr.stack.split('\n')[1].includes('new Maker'));

  // 8. Async function after an await
  async function asyncTask() {
    await Promise.resolve();
    return new Error('inside async');
  }
  const asyncErr = await asyncTask();
  console.log(getFrames(asyncErr.stack)[0]);

  // 9. Callback under forEach (expect Array.forEach frame)
  [1].forEach(() => {
    const forEachErr = new Error('inside forEach');
    const frames = getFrames(forEachErr.stack);
    console.log(frames.includes('Array.forEach'));
  });

  // 10. Subclass
  class SubError extends Error {}
  const subErr = new SubError('subclass');
  console.log(subErr instanceof SubError, subErr instanceof Error);
  console.log(subErr.stack.split('\n')[0] === 'Error: subclass');

  // 11. Error.captureStackTrace with and without fn argument
  function testCapture() {
    function innerCapture() {
      const o1 = {};
      Error.captureStackTrace(o1);
      console.log(getFrames(o1.stack)[0]);
      const o2 = {};
      Error.captureStackTrace(o2, innerCapture);
      console.log(getFrames(o2.stack)[0]);
    }
    innerCapture();
  }
  testCapture();

  // 12. Error.stackTraceLimit
  Error.stackTraceLimit = 0;
  const eLim0 = new Error('lim0');
  console.log(eLim0.stack === 'Error: lim0');

  Error.stackTraceLimit = 2;
  const eLim2 = f1();
  console.log(getFrames(eLim2.stack).length === 2);

  Error.stackTraceLimit = Infinity;
  const eLimInf = f1();
  console.log(getFrames(eLimInf.stack).length >= 3);
  Error.stackTraceLimit = 10;

  // 13. Runtime TypeError from calling undefined
  try {
    const badFn = undefined;
    badFn();
  } catch (rtErr) {
    console.log(rtErr instanceof TypeError);
    console.log(typeof rtErr.stack === 'string');
    console.log(rtErr.stack.split('\n')[0].startsWith('TypeError:'));
  }

  // 14. Stack being writable/reassignable
  const writeErr = new Error('write');
  writeErr.stack = 'custom stack';
  console.log(writeErr.stack);
}

main();
