// A second super() in TypeScript, with parameter properties: node's transform
// assigns them after each call, and the second call throws before its own
// assignments run.

const log: string[] = [];
class Base {
  constructor(n: number) {
    log.push("base " + n);
  }
}

class Twice extends Base {
  constructor(public x: number) {
    super(1);
    this.x = x * 10;
    try {
      super(2);
    } catch (e) {
      log.push((e as Error).message);
    }
  }
}
console.log(JSON.stringify(new Twice(4)));
console.log(log.splice(0).join(" | "));

class Loop extends Base {
  constructor(public x: number, times: number) {
    for (let i = 0; i < times; i++) super(i);
  }
}
for (const times of [0, 1, 2]) {
  try {
    console.log(times, JSON.stringify(new Loop(7, times)));
  } catch (e) {
    console.log(times, (e as Error).constructor.name, (e as Error).message);
  }
  console.log(log.splice(0).join(" | "));
}
