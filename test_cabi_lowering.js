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

let appD = bro.appDir;
let userD = bro.userDataDir;
let pRes = bro.resolvePath("test.txt");
let wRes = bro.paths.resolveWritePath("save.dat");

let simplex = FastNoise.Simplex();
simplex.set("Frequency", 0.05);
let noiseSample = simplex.genSingle2D(1.5, 2.5, 1337);

// --- Phase 4 Subsystems ---
// 1. Dialogs (both unqualified global and bro.dialogs paths)
alert("Native alert test");
let confirmed = confirm("Are you ready?");
let promptResult = prompt("Enter text:", "default_input");
let saveDialog = showSaveFileDialog("Text|txt", "output.txt");
let openDialog = showOpenFileDialog("All|*");
let folderDialog = showOpenFolderDialog("C:/");

// 2. Window (state, properties, coordinates, display queries)
let winState = bro.window.state;
bro.window.borderless = true;
let winBorderless = bro.window.borderless;
bro.window.alwaysOnTop = true;
let winTop = bro.window.alwaysOnTop;
bro.window.setPosition(150, 250);
let posX = bro.window.getPositionX();
let posY = bro.window.getPositionY();
bro.window.setMinSize(800, 600);
let minW = bro.window.getMinWidth();
let minH = bro.window.getMinHeight();
bro.window.setMaxSize(1920, 1080);
let maxW = bro.window.getMaxWidth();
let maxH = bro.window.getMaxHeight();
let dispCount = bro.window.getDisplayCount();
let moveOk = bro.window.moveToDisplay(0);
bro.window.minimize();
bro.window.maximize();
bro.window.restore();

// 3. Settings (load, set, get, action checks, save, reset)
bro.settings.load();
bro.settings.set("audio.volume", "0.85");
let audioVol = bro.settings.get("audio.volume");
let jumpPressed = bro.settings.isActionPressed("jump");
let jumpStrength = bro.settings.getActionStrength("jump");
bro.settings.save();
bro.settings.reset("audio");

console.log(v, c, count, nearest, r1, cur1, cur2, t0, t1, p0, p1, now, appD, userD, pRes, wRes, noiseSample, confirmed, promptResult, saveDialog, openDialog, folderDialog, winState, winBorderless, winTop, posX, posY, minW, minH, maxW, maxH, dispCount, moveOk, audioVol, jumpPressed, jumpStrength);
