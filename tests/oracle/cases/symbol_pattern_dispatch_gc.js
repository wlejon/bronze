// The dispatch path of 22.1.3 under a MOVING collector.
//
// `String.prototype.replace` and its siblings hold three things across a call
// into user code: the receiver string, the pattern argument the method was
// found on, and the method itself. Each hook below allocates hard — arrays,
// objects and strings per call — so a collection lands inside the dispatched
// call rather than around it, and every one of the three is read again
// afterwards. The oracle harness runs each case a second time with collection
// at every allocation, which is what makes this a test rather than a program.
//
// Expectations are the same 22.1.3 dispatch the other two cases pin, computed
// over a loop: nothing here depends on the collector, which is the point.

const pattern = {
  count: 0,
  [Symbol.replace](s, value) {
    this.count = this.count + 1;
    const junk = [];
    for (let i = 0; i < 4; i = i + 1) junk.push({ i: i, text: s + i });
    return s.split("-").join(value) + "#" + junk.length;
  },
  [Symbol.split](s, limit) {
    const out = [];
    let acc = "";
    for (let i = 0; i < s.length; i = i + 1) {
      acc = acc + s[i];
      out.push(acc);
    }
    return out.slice(0, 3);
  },
  [Symbol.match](s) { return { text: s, n: s.length }; },
  [Symbol.search](s) { return { at: s.indexOf("b") }.at; },
};

const lines = [];
for (let n = 0; n < 200; n = n + 1) {
  const subject = "a-b-" + n;
  const replaced = subject.replace(pattern, String(n % 3));
  const pieces = subject.split(pattern).join("|");
  const matched = subject.match(pattern);
  const found = subject.search(pattern);
  if (n % 50 === 0) {
    lines.push(replaced + " " + pieces + " " + matched.n + " " + found);
  }
}
for (const line of lines) console.log(line);
console.log(pattern.count);

// The same loop against a REAL RegExp, which takes the fast path instead of the
// dispatch: the guard must not have made a plain pattern reach the property
// path, and its answers must survive the same collector.
let joined = "";
for (let n = 0; n < 200; n = n + 1) {
  const subject = "x" + n + "y" + n;
  joined = subject.replace(/\d+/g, "<$&>");
  if (n % 75 === 0) console.log(joined, subject.split(/\d+/).join("|"), subject.search(/y/));
}
console.log(joined);
