// Async stack traces: an error made in an async function that was resumed
// from the microtask queue lists, after its own frame, the async functions
// awaiting it, innermost first, as "at async <name>" (node's zero-cost async
// stack traces). Only frames of this file's functions are printed, without
// their positions, so the case pins the chain and not the paths.

function frames(stack) {
  return stack.split('\n').slice(1)
    .map(l => l.trim())
    .filter(l => /inner|middle|outer|helper/.test(l))
    .map(l => l.replace(/ \(.*$/, ''));
}

async function inner() {
  await null;
  return new Error('deep');
}
async function middle() {
  const e = await inner();
  return e;
}
async function outer() {
  return await middle();
}

// An error made by a plain function the resumed body calls.
function helper() {
  return new Error('from helper');
}
async function innerCalls() {
  await null;
  return helper();
}
async function outerCalls() {
  return await innerCalls();
}

async function main() {
  console.log(frames((await outer()).stack).join(' | '));
  console.log(frames((await outerCalls()).stack).join(' | '));
  // A promise that is not an async call's links nothing.
  const plain = await Promise.resolve().then(() => new Error('reaction'));
  console.log(frames(plain.stack).length);
}

main();
