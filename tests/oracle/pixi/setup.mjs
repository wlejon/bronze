// The environment half of the pixi milestone, evaluated before pixi.mjs is.
//
// pixi's import-time code assumes two globals every browser (and modern node)
// defines: `navigator` — read unconditionally by isSafari() at module level —
// and `Intl`, whose `== null` feature check still evaluates the binding.
// Assigning them onto `globalThis` creates the global bindings the free names
// then resolve (a property of the global object IS a global binding;
// rt_state.cpp's fallback is the runtime half). `host.globals` beside this
// file is what admits the two names at compile time.
//
// The values are the minimal truthful shape of a headless environment: a
// user agent that names bronze, and an Intl namespace with no segmenter —
// which is a real environment shape (pixi's own comment names Firefox), so
// pixi's feature detection takes its fallback path rather than being lied to.
globalThis.navigator = { userAgent: 'bronze', platform: 'bronze', maxTouchPoints: 0 };
globalThis.Intl = {};
