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

// --- Phase 5 Subsystems ---
// 4. Menu (visibility toggles, item removal)
let menuVis0 = bro.menu.visible;
bro.menu.show();
let menuVis1 = bro.menu.visible;
bro.menu.hide();
let menuVis2 = bro.menu.visible;
let menuRemoved = bro.menu.removeItem("file.new");

// 5. Mic (sample rate query, active state, start/stop)
let micRate = bro.mic.engineRate();
let micActive0 = bro.mic.isActive();
bro.mic.start();
let micActive1 = bro.mic.isActive();
bro.mic.stop();
let micActive2 = bro.mic.isActive();

// 6. Gamepad (connection status, axis/button querying, rumble haptics)
let gpConnected = bro.gamepad.isConnected(0);
let gpAxis = bro.gamepad.getAxis(0, 0);
let gpBtn = bro.gamepad.getButton(0, 0);
let gpRumble = bro.gamepad.rumble(0, 0.8, 0.4, 200);
let gpTriggers = bro.gamepad.rumbleTriggers(0, 0.5, 0.5, 100);

// --- Phase 6 Subsystems ---
// 7. Media (availability query)
let mediaAvail = bro.media.available;

// 8. Listen (support check, retain, frame query)
let listenSupported = bro.listen.supported();
bro.listen.retain(10);
let listenFrame = bro.listen.frame();

// 9. Audio (AudioContext, VoiceAllocator, Sequence)
let ctx = new AudioContext();
let ctxRate = ctx.sampleRate;
let ctxState = ctx.state;
let va = new VoiceAllocator();
let vaCount = va.voiceCount();
let seq = new Sequence();
let seqTempo = seq.tempo;
// --- Phase 7 Subsystems ---
// 10. Steam (availability, persona, appId)
let steamAvail = bro.steam.available;
let steamPersona = bro.steam.personaName;
let steamAppId = bro.steam.appId;

// 11. Server (tickrate, uptime, stop)
let srvTick = bro.server.tickrate;
let srvUptime = bro.server.uptime;
bro.server.stop();

// 12. Net (peers)
let netPeers = bro.net.peers();

// 13. Worker (new Worker)
let worker = new Worker("worker.js");

// 14. Abort (AbortController, signal.aborted)
let controller = new AbortController();
let aborted = controller.signal.aborted;

// --- Phase 8 Subsystems ---
// 15. File (Blob, File)
let b = new Blob();
let bSize = b.size;
let f = new File(["hello"], "hello.txt");
let fName = f.name;

// 16. DOMParser
let parser = new DOMParser();

// 17. Events
let evt = new Event("click");
let evtType = evt.type;

// 18. Text
let bidiAvail = bro.text.bidiAvailable;

// --- Phase 9 Subsystems: Graphics, Canvas, WebGL2, Imaging & GPU ---
// 19. GPU
let gpuAvail = bro.gpu.available;
let gpuBackend = bro.gpu.backend;
let gpuDevCount = bro.gpu.deviceCount("cpu");
let gpuDevName = bro.gpu.deviceName("cpu");
let gpuTrimmed = bro.gpu.trim("cpu", 0);

// 20. Image
let img = new Image();
img.src = "assets/logo.png";
let imgSrc = img.src;
let imgComplete = img.complete;
let imgW = img.width;
let imgH = img.height;

// 21. ImageBitmap & ImageData
let imgData = new ImageData(32, 32);
let imgDataW = imgData.width;
let imgDataH = imgData.height;
let imgBmp = new ImageBitmap();
let imgBmpW = imgBmp.width;
imgBmp.close();
let createdBmp = createImageBitmap(imgData);

// 22. Canvas (CanvasRenderingContext2D, CanvasGradient, TextMetrics)
let canvasCtx = new CanvasRenderingContext2D();
canvasCtx.fillStyle = "#ff0000";
canvasCtx.lineWidth = 2.0;
let ctxFill = canvasCtx.fillStyle;
let ctxLineW = canvasCtx.lineWidth;
canvasCtx.save();
canvasCtx.beginPath();
canvasCtx.closePath();
canvasCtx.stroke();
canvasCtx.fill();
canvasCtx.restore();

let grad = new CanvasGradient();
grad.addColorStop(0.0, "#000000");
grad.addColorStop(1.0, "#ffffff");

let metrics = new TextMetrics();
let metricsW = metrics.width;

// 23. WebGL2 (WebGL2RenderingContext, WebGLBuffer, etc.)
let gl = new WebGL2RenderingContext();
gl.viewport(0, 0, 800, 600);
gl.clearColor(0.1, 0.2, 0.3, 1.0);
gl.clear(16384);
let glBuf = gl.createBuffer();
let glTex = gl.createTexture();
let glProg = gl.createProgram();
gl.deleteBuffer(glBuf);
gl.deleteTexture(glTex);
gl.deleteProgram(glProg);

// --- Phase 10 Subsystems: 3D Scene Graph, Mesh Geometry, Lighting, Gizmo & Animation Subsystems ---
// 24. Scene
let sceneNode = new SceneNode();
sceneNode.name = "rootNode";
let sceneNodeName = sceneNode.name;
let sceneNodeVis = sceneNode.visible;
sceneNode.setPosition(1.0, 2.0, 3.0);

// 25. Mesh
let mesh = new Mesh();
let bvh = new MeshBVH();

// 26. Lighting
let light = new LightNode();
let shape = new ShapeNode();

// 27. Gizmo
let gizmoVis0 = bro.gizmo.visible;
bro.gizmo.show();
let gizmoVis1 = bro.gizmo.visible;
bro.gizmo.setMode("translate");

// 28. Animation
let tween = new Tween();
let animPlayer = new AnimationPlayer();

// --- Phase 11 Subsystems: Physics, World Simulation & Terrains Subsystems ---
// 29. Physics
Physics.setGravity(0.0, -9.81, 0.0);
Physics.step(0.016);
let physWorld = new PhysicsWorldHandle();
let physChar = new PhysicsCharacter();
let physVeh = new PhysicsVehicle();
let physRag = new PhysicsRagdoll();
let physSoft = new PhysicsSoftBody();

// 30. Terrain
let terr = new Terrain();
let tChunks = terr.chunkCount;
let tH = terr.heightAt(0.0, 0.0);
let tElev = terr.elevation(0.0, 0.0);
let tLayers = terr.layers;
terr.update(0.0, 0.0, 0.0);
terr.destroy();

// 31. Clipmap
let clip = new ClipmapTerrain();
let cLevels = clip.levels;
clip.setSnowLine(1500.0);
clip.update(0.0, 0.0, 0.0);
clip.destroy();

// 32. TileWorld
let tw = new TileWorld();
tw.setTile(0, 1, 2, 42);
let tile = tw.getTile(0, 1, 2);
let twChunks = tw.chunks;
tw.paging = true;
let twPaging = tw.paging;
tw.update(0.0, 0.0, 0.0);
tw.destroy();

// 33. Flora
bro.flora.setWind(10.0, 1.0, 0.0);
bro.flora.setDensity(0.75);
bro.flora.update(0.016);
bro.flora.clear();

// --- Phase 12 Subsystems: Skeletal Rigging, Procedural Motion, Splats & Web Animations Subsystems ---
// 34. Rigging
let skel = new Skeleton();
let skelBones = skel.boneCount;
let pose = new Pose(skel);
let poseBones = pose.boneCount;
let skin = new SkinData();
let skinVerts = skin.vertexCount;
let vox = new VoxelChunk(4, 4, 4);
let voxVal0 = vox.get(1, 2, 3);
vox.set(1, 2, 3, 7);
let voxVal1 = vox.get(1, 2, 3);

// 35. Motion
bro.motion.init();

// 36. TripoSplat
let splat = new TripoSplatPipeline();

// 37. WebAnimations
let webAnims = new WebAnimations();
let anim = new Animation();
let animRate = anim.playbackRate;

console.log(v, c, count, nearest, r1, cur1, cur2, t0, t1, p0, p1, now, appD, userD, pRes, wRes, noiseSample, confirmed, promptResult, saveDialog, openDialog, folderDialog, winState, winBorderless, winTop, posX, posY, minW, minH, maxW, maxH, dispCount, moveOk, audioVol, jumpPressed, jumpStrength, menuVis0, menuVis1, menuVis2, menuRemoved, micRate, micActive0, micActive1, micActive2, gpConnected, gpAxis, gpBtn, gpRumble, gpTriggers, mediaAvail, listenSupported, listenFrame, ctxRate, ctxState, vaCount, seqTempo, steamAvail, steamPersona, steamAppId, srvTick, srvUptime, aborted, bSize, fName, evtType, bidiAvail, gpuAvail, gpuBackend, gpuDevCount, gpuDevName, gpuTrimmed, imgSrc, imgComplete, imgW, imgH, imgDataW, imgDataH, imgBmpW, ctxFill, ctxLineW, metricsW, sceneNodeName, sceneNodeVis, gizmoVis0, gizmoVis1, tChunks, tH, tElev, tLayers, cLevels, tile, twChunks, twPaging, skelBones, poseBones, skinVerts, voxVal0, voxVal1, animRate);


