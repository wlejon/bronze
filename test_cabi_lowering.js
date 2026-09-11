let v = bro.math.lerp(10.0, 30.0, 0.5);
let c = bro.math.clamp(150.0, 0.0, 100.0);

let sh = new SpatialHash3D(2.0, 1000.0);
sh.insert(42.0, 1.0, 2.0, 3.0);
let count = sh.size;
let nearest = sh.nearest(1.1, 2.1, 3.1, 5.0);

let rng = new Rng(12345.0);
let r1 = rng.int(10, 20);

let sm = new Smoother(100.0, 60.0);
sm.reset(50.0);
sm.setTarget(100.0);
let cur1 = sm.current;
let cur2 = sm.tick();

let t0 = bro.time.scale;
bro.time.scale = 2.5;
let t1 = bro.time.scale;
let p0 = bro.time.paused;
bro.time.paused = true;
let p1 = bro.time.paused;
let now = bro.time.now;

console.log(v, c, count, nearest, r1, cur1, cur2, t0, t1, p0, p1, now);
