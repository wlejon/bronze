// `for await` over a SYNC iterable goes through CreateAsyncFromSyncIterator
// (ECMA-262 27.1.6): each value the sync iterator produces is awaited, so a
// promise in an array arrives as what it resolves to. A value that REJECTS
// throws inside the loop at that step, and the sync iterator is closed first.

async function plainArray() {
  const out = [];
  for await (const v of [Promise.resolve('a'), 'b', Promise.resolve('c')]) out.push(v);
  return out.join(',');
}

async function assignmentHead() {
  const o = {};
  let last;
  for await (o.v of [Promise.resolve(1)]) {}
  for await (last of [Promise.resolve(2)]) {}
  return o.v + last;
}

function* producer(log) {
  try {
    yield Promise.resolve(1);
    yield Promise.reject(new Error('boom'));
    log.push('never reached');
    yield 3;
  } finally {
    log.push('closed');
  }
}

async function rejection() {
  const log = [];
  const seen = [];
  try {
    for await (const v of producer(log)) seen.push(v);
  } catch (e) {
    log.push('caught ' + e.message);
  }
  return seen.join(',') + ' | ' + log.join(',');
}

async function rejectionInArray() {
  try {
    for await (const v of ['ok', Promise.reject(new TypeError('bad'))]) {
      if (v !== 'ok') return 'unexpected ' + v;
    }
  } catch (e) {
    return e.name + ': ' + e.message;
  }
  return 'no throw';
}

// A thenable is adopted like a promise.
async function thenable() {
  const t = { then(resolve) { resolve('adopted'); } };
  for await (const v of [t]) return v;
}

// `break` still closes the sync iterator.
async function early() {
  const log = [];
  for await (const v of producer(log)) {
    log.push('got ' + v);
    break;
  }
  return log.join(',');
}

(async () => {
  console.log(await plainArray());
  console.log(await assignmentHead());
  console.log(await rejection());
  console.log(await rejectionInArray());
  console.log(await thenable());
  console.log(await early());
})();
