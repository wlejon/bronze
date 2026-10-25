// TypeScript declaration merging: two declarations of one name in one scope
// make one value. A namespace runs its body on the object a class, a
// function, an enum or an earlier namespace of the same name already made
// (TypeScript's `M || (M = {})`), and a second enum adds its members to the
// first one's object.

namespace Config {
    export const a = 1;
}
namespace Config {
    export const b = 2;
    export function both() { return Config.a + b; }
}
console.log("namespace + namespace", Config.a, Config.b, Config.both(), Object.keys(Config).join(","));

// A class's static helpers, the way TypeScript code commonly spells them.
class Shape {
    constructor(public kind: string) {}
}
namespace Shape {
    export function square() { return new Shape("square"); }
    export const sides = 4;
}
console.log("class + namespace", Shape.square().kind, Shape.sides, typeof Shape);

function greet(name: string) { return "hello " + name; }
namespace greet {
    export const punctuation = "!";
    export function loud(name: string) { return greet(name).toUpperCase() + punctuation; }
}
console.log("function + namespace", greet("a"), greet.loud("b"), greet.length);

enum Color { Red, Green }
enum Color { Blue = 2, Violet = Blue * 2 }
console.log("enum + enum", Color.Red, Color.Green, Color.Blue, Color.Violet, Color[4], Object.keys(Color).join(","));

// A merged block may name an earlier block's members bare.
enum Size { Small = 1 }
enum Size { Large = Small + 9 }
console.log("bare member of an earlier block", Size.Large, Size[10]);

enum Direction { Up, Down }
namespace Direction {
    export function flip(d: Direction) { return d === Direction.Up ? Direction.Down : Direction.Up; }
}
console.log("enum + namespace", Direction.flip(Direction.Up), Direction[Direction.flip(Direction.Down)]);

// Merging inside a namespace: a nested namespace exported twice, and an
// exported function with a namespace of its own.
namespace Lib {
    export namespace Util { export const x = 1; }
    export namespace Util { export const y = 2; }
    export function tool() { return "tool"; }
    export namespace tool { export const version = 3; }
}
console.log("nested merging", Lib.Util.x, Lib.Util.y, Lib.tool(), Lib.tool.version);
