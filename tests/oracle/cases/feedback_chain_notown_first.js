// Inline way chains whose usual answer is past the receiver's own slots, so
// a profile lays that answer out first: reads of a key most receivers lack
// (`undefined` from a negative entry, three.js's `object.isInstancedMesh`)
// and reads most receivers answer from their prototype (a constant on a
// class). Every answer must follow the objects: a receiver that has the key
// after all, a prototype gaining, changing and losing it, an own property
// shadowing it, a getter on the prototype, and a receiver of a new shape.
class Mesh { constructor(i) { this.id = i; this.v = i * 2; } }
class Line { constructor(i) { this.id = i; this.w = i + 1; this.v = i; } }
class Points { constructor(i) { this.id = i; } }
Points.prototype.isPoints = true;
Mesh.prototype.kind = "mesh";
Line.prototype.kind = "line";

const objs = [];
for (let i = 0; i < 30; i++) objs.push(i % 3 === 0 ? new Mesh(i) : i % 3 === 1 ? new Line(i) : new Points(i));
const owner = new Mesh(99);
owner.isInstanced = true;  // one receiver of a fourth shape holds the key
objs.push(owner);

function isInstanced(o) { return o.isInstanced; }
function kindOf(o) { return o.kind; }
function points(o) { return o.isPoints; }

function round(label, reps) {
    let a = 0, k = "", p = 0;
    for (let r = 0; r < reps; r++) {
        for (const o of objs) {
            if (isInstanced(o)) a++;
            k = kindOf(o) + k.length;
            if (points(o)) p++;
            if (k.length > 40) k = "";
        }
    }
    console.log(label, a, k, p, String(isInstanced(objs[0])), String(kindOf(objs[2])), String(points(objs[1])));
}

round("warm", 400);
Mesh.prototype.isInstanced = 1;
round("proto-gains", 3);
Line.prototype.kind = "LINE";
round("proto-changes", 3);
delete Mesh.prototype.isInstanced;
round("proto-loses", 3);
objs[3].kind = "own";
objs[4].isPoints = false;
round("own-shadow", 3);
Object.defineProperty(Line.prototype, "isInstanced", { get() { return this.w > 10; }, configurable: true });
round("getter", 3);
delete Mesh.prototype.kind;
Object.setPrototypeOf(Points.prototype, { isInstanced: "deep", kind: "deep" });
round("deeper", 3);
objs.push({ id: 5, isInstanced: 0, kind: "plain", isPoints: 1 });
round("new-shape", 3);
