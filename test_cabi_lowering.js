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

// --- Phase 13 Subsystems: Game AI, GpuTensor, Vision ML & Diffusion Inference Subsystems ---
// 38. Game AI (AIAgent, AIWorld, AINavGrid, CombatAction, Formation, VecSim, bro.ai.game)
let aiWorld = new AIWorld();
aiWorld.step(0.016);
let agent = new AIAgent();
let agentId = agent.id;
agent.setPosition(10.0, 0.0, 20.0);
agent.setVelocity(1.0, 0.0, 0.0);
agent.setGoal(50.0, 0.0, 50.0);
agent.stop();
let worldAgent = aiWorld.createAgent();
aiWorld.destroyAgent(worldAgent);

let navGrid = new AINavGrid();
let walkable0 = navGrid.isWalkable(0.0, 0.0);
navGrid.setWalkable(0.0, 0.0, false);
let walkable1 = navGrid.isWalkable(0.0, 0.0);

let combat = new CombatAction();
combat.targetX = 12.5;
let cX = combat.targetX;
combat.targetZ = 25.0;
let cZ = combat.targetZ;
combat.actionType = 3;
let cType = combat.actionType;

let formation = new Formation();
formation.setLeader(agent);
formation.addFollower(agent, 2.0, 2.0);
formation.update(0.016);

let vecSim = new VecSim();
vecSim.step(0.016);

let fWorld = bro.ai.game.createWorld();
let fCombat = bro.ai.game.createCombatAction();

// 39. GpuTensor & bro.tensor
bro.tensor.init();
let tensorAvail = bro.tensor.available;
let tensorBackend = bro.tensor.backend;
let gpuTensor = new GpuTensor();
let gtRows = gpuTensor.rows;
let gtCols = gpuTensor.cols;
let gtSize = gpuTensor.size;
let gtBytes = gpuTensor.bytes;
gpuTensor.zero();
let gtDtype = gpuTensor.dtype();
let gtClone = gpuTensor.clone();
let createdTensor = bro.tensor.createTensor(4, 4);
let ctRows = createdTensor.rows;
let ctCols = createdTensor.cols;

// 40. Vision (DepthEstimator, Sam, NormalEstimator, Hed, Lineart, Mlsd, Openpose, Segformer, Birefnet, StyleGAN3, Dinov2, Dinov3, bro.vision)
bro.vision.init();
let depth = new DepthEstimator();
let depthDev = depth.device;
let sam = new Sam();
let samDev = sam.device;
let samHasImg = sam.hasImage;
let sg3 = new StyleGAN3();
let sg3Z = sg3.zDim;
let sg3Res = sg3.imgResolution;
let dinov2 = new Dinov2();
let d2Dev = dinov2.device;
let dinov3 = new Dinov3();
let d3Dev = dinov3.device;

// 41. Diffusion (Pipeline, PipelineState, bro.diffusion)
bro.diffusion.init();
let diffVer = bro.diffusion.version;
let pipe = new Pipeline();
let pipeState = new PipelineState();

// --- Phase 14 Subsystems: Large Language Models (LLM) & Tokenizers Subsystems ---
// 42. Language Models & Tokenizers (AsyncHandle, QwenTokenizer, MistralTokenizer, GemmaTokenizer, LMModel, Qwen35Model, Qwen3VLModel, NllbModel, ClipModel, T5Model, bro.lm)
bro.lm.init();
let asyncH = new AsyncHandle();
asyncH.cancel();

let qwenTok = new QwenTokenizer();
let qwenImEnd = qwenTok.imEndId;
let qwenImStart = qwenTok.imStartId;

let mistralTok = new MistralTokenizer();
let misEos = mistralTok.eosId;
let misBos = mistralTok.bosId;
let misVocab = mistralTok.vocabCount;

let gemmaTok = new GemmaTokenizer();
let gemEos = gemmaTok.eosId;
let gemBos = gemmaTok.bosId;
let gemPad = gemmaTok.padId;
let gemUnk = gemmaTok.unkId;
let gemVocab = gemmaTok.vocabCount;

let lmModel = new LMModel();
let lmFamily = lmModel.family;
let lmVocab = lmModel.vocabSize;
let lmHidden = lmModel.hiddenSize;
let lmLayers = lmModel.numLayers;
let lmMaxSeq = lmModel.maxSeqLen;
let lmCache = lmModel.cacheLen;
lmModel.allocateCache(2048);
lmModel.resetCache();

let q35Model = new Qwen35Model();
let q35Family = q35Model.family;
let q35Vocab = q35Model.vocabSize;
let q35Hidden = q35Model.hiddenSize;
let q35Layers = q35Model.numLayers;
let q35MaxSeq = q35Model.maxSeqLen;
let q35Eos = q35Model.eosId;
let q35ImEnd = q35Model.imEndId;
let q35EndText = q35Model.endoftextId;

let q3vlModel = new Qwen3VLModel();
let q3vlFamily = q3vlModel.family;
let q3vlVocab = q3vlModel.vocabSize;
let q3vlHidden = q3vlModel.hiddenSize;
let q3vlLayers = q3vlModel.numLayers;
let q3vlMaxSeq = q3vlModel.maxSeqLen;
let q3vlEos = q3vlModel.eosId;
let q3vlImEnd = q3vlModel.imEndId;
let q3vlEndText = q3vlModel.endoftextId;

let nllbModel = new NllbModel();
let nllbFamily = nllbModel.family;
let nllbVocab = nllbModel.vocabSize;
let nllbDModel = nllbModel.dModel;
let nllbEncLayers = nllbModel.encoderLayers;
let nllbDecLayers = nllbModel.decoderLayers;
let nllbLangCount = nllbModel.languageCount;
let nllbHasEng = nllbModel.hasLanguage("eng_Latn");

let clipModel = new ClipModel();
let clipDim = clipModel.projectionDim;

let t5Model = new T5Model();
let t5DModel = t5Model.dModel;
let t5MaxLen = t5Model.maxLength;
let t5PadId = t5Model.padId;
let t5EosId = t5Model.eosId;
let t5Vocab = t5Model.vocabCount;

// --- Phase 15: Audio ML, STT, TTS, KWS, DIAR, RAVE ---
bro.stt.init();
let wtok = new WhisperTokenizer();
let wtokLoaded = wtok.loaded;
let wmod = new WhisperModel();
let wmodDev = wmod.device;
let ptok = new ParakeetTokenizer();
let ptokLoaded = ptok.loaded;
let qmod = new QwenAsrModel();
let qmodDev = qmod.device;

bro.tts.init();
let kmod = new KokoroModel();
let kmodDev = kmod.device;
let smod = new SupertonicModel();
let smodDev = smod.device;
let senc = new SpeakerEncoder();
let sencDev = senc.device;
let qtmod = new QwenTtsModel();
let qtmodDev = qtmod.device;

bro.kws.init();
let kwsView = new KwsStreamView();
let kwsActive = kwsView.active;

bro.diar.init();
let sft = new Sortformer();
let sftDev = sft.device;
let cdiar = new ClusterDiarizer();
let cdiarDev = cdiar.device;

bro.rave.init();
let rave = new Rave();
let raveSr = rave.sampleRate;

// --- Phase 16: Multimodal Sensors and Gestures (Gesture, Sense, Wake) ---
bro.gesture.init();
let gestureView = new GestureStreamView();
let gestureActive = gestureView.active;

bro.sense.init();
let senseView = new SenseStreamView();
let senseActive = senseView.active;

bro.wake.init();
let wakeView = new WakeStreamView();
let wakeActive = wakeView.active;

// --- Phase 17: DOM Elements, Compatibility Layer & Vendor Globals ---
let customRegistry = new CustomElementRegistry();
customRegistry.define("my-elem", null, null);
let myElem = customRegistry.get("my-elem");
customRegistry.upgrade(null);
let htmlEl = new HTMLElement();

let iframeEl = new HTMLIFrameElement();
let iframeSrc0 = iframeEl.src;
iframeEl.src = "app.html";
let iframeSrc1 = iframeEl.src;
let iframeW = iframeEl.width;
let iframeH = iframeEl.height;
iframeEl.reload();

let mql = new MediaQueryList();
let mqlMatches = mql.matches;
let mqlMedia = mql.media;
mql.addListener(null);
mql.removeListener(null);

let mqlFromFn = matchMedia("(min-width: 600px)");
let mqlFnMatches = mqlFromFn.matches;

let mqlFromWin = window.matchMedia("(min-width: 600px)");
let mqlWinMatches = mqlFromWin.matches;

let mqlFromBroWin = bro.window.matchMedia("(min-width: 600px)");
let mqlBroWinMatches = mqlFromBroWin.matches;

let vgSignals = window.signals;
let vgCodeMirror = window.CodeMirror;
let vgAcorn = window.acorn;
let vgTern = window.tern;
let vgEsprima = window.esprima;
let vgJsonlint = window.jsonlint;
let vgDraco = window.draco_encoder;
let vgBroSignals = bro.vendor_globals.signals;
let vgBroCodeMirror = bro.vendor_globals.CodeMirror;

console.log(v, c, count, nearest, r1, cur1, cur2, t0, t1, p0, p1, now, appD, userD, pRes, wRes, noiseSample, confirmed, promptResult, saveDialog, openDialog, folderDialog, winState, winBorderless, winTop, posX, posY, minW, minH, maxW, maxH, dispCount, moveOk, audioVol, jumpPressed, jumpStrength, menuVis0, menuVis1, menuVis2, menuRemoved, micRate, micActive0, micActive1, micActive2, gpConnected, gpAxis, gpBtn, gpRumble, gpTriggers, mediaAvail, listenSupported, listenFrame, ctxRate, ctxState, vaCount, seqTempo, steamAvail, steamPersona, steamAppId, srvTick, srvUptime, aborted, bSize, fName, evtType, bidiAvail, gpuAvail, gpuBackend, gpuDevCount, gpuDevName, gpuTrimmed, imgSrc, imgComplete, imgW, imgH, imgDataW, imgDataH, imgBmpW, ctxFill, ctxLineW, metricsW, sceneNodeName, sceneNodeVis, gizmoVis0, gizmoVis1, tChunks, tH, tElev, tLayers, cLevels, tile, twChunks, twPaging, skelBones, poseBones, skinVerts, voxVal0, voxVal1, animRate, agentId, walkable0, walkable1, cX, cZ, cType, tensorAvail, tensorBackend, gtRows, gtCols, gtSize, gtBytes, gtDtype, ctRows, ctCols, depthDev, samDev, samHasImg, sg3Z, sg3Res, d2Dev, d3Dev, diffVer, qwenImEnd, qwenImStart, misEos, misBos, misVocab, gemEos, gemBos, gemPad, gemUnk, gemVocab, lmFamily, lmVocab, lmHidden, lmLayers, lmMaxSeq, lmCache, q35Family, q35Vocab, q35Eos, q3vlFamily, q3vlVocab, q3vlEos, nllbFamily, nllbVocab, nllbDModel, nllbEncLayers, nllbDecLayers, nllbLangCount, nllbHasEng, clipDim, t5DModel, t5MaxLen, t5PadId, t5EosId, t5Vocab, wtokLoaded, wmodDev, ptokLoaded, qmodDev, kmodDev, smodDev, sencDev, qtmodDev, kwsActive, sftDev, cdiarDev, raveSr, gestureActive, senseActive, wakeActive, iframeSrc0, iframeSrc1, iframeW, iframeH, mqlMatches, mqlMedia, mqlFnMatches, mqlWinMatches, mqlBroWinMatches);




