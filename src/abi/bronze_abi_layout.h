/*
 * ---- the inline property cache contract ---------------------------------
 *
 * The IC table is a zero-initialized global array in the GENERATED object
 * file, one BRONZE_ABI_IC_ENTRY_SIZE-byte entry per property site, and the
 * entry pointer is what `bronze_prop_get` / `bronze_prop_set` take (the
 * BRONZE_ABI_MU64 above) instead of an index into a runtime vector. A vector
 * reallocates, so generated code could not hold a pointer into it, so the
 * check had to happen inside the helper and the CALL was most of the cost.
 *
 * Generated code loads these fields itself, so the runtime's C++ layouts
 * below are part of this ABI, not private to the runtime. object.h and
 * rt_helpers.cpp static_assert every constant here against the real struct,
 * so adding a field to `InlineCache`, `HeapObjectHeader` or `ObjectHeader`
 * is a compile error rather than a silent miscompile.
 *
 * The entry is four plain words the collector never touches: shapes are
 * immortal and non-moving, and the holder is derived from `cached_depth`
 * rather than cached.
 *
 * The fourth word is the prototype-mutation epoch the entry was filled at,
 * and it is what makes a depth > 0 entry sound: the receiver's shape cannot
 * notice a property added to an object BETWEEN the receiver and the holder,
 * because that add changes only the intermediate's shape.
 *
 * ---- a SITE is BRONZE_ABI_IC_WAYS entries ---------------------------------
 *
 * The table's stride is a SITE, not an entry: BRONZE_ABI_IC_WAYS entries laid
 * out one after another, way 0 first. A site's way 0 is byte-identical to
 * what a whole site used to be, which is why every runtime path that takes an
 * `InlineCache*` still works when handed the site pointer generated code and
 * the helpers pass around.
 *
 * READ sites use every way; a WRITE site uses way 0 only (a write's bill is
 * transitions, not shape variety — measured on the pure-compute fixtures).
 * The unused ways of a write site cost BSS and nothing else.
 *
 * Generated code compares the receiver's shape against way 0, then — only on
 * a way-0 miss, and only while `poly_ic_enabled` — against ways 1..N-1, and
 * the matched way's entry pointer is what the rest of the fast path reads.
 * The helper installs with move-to-front: a fresh entry lands at way 0 and
 * pushes the others down, so the last shape to miss is the first one checked
 * and the least recently installed falls off the end. No cursor word is
 * needed, which is why the site is exactly N entries wide.
 *
 * ---- the ABSENT (negative) entry ------------------------------------------
 *
 * `cached_depth == BRONZE_ABI_IC_DEPTH_ABSENT_FLAG` means: this key is on
 * NEITHER the receiver nor any link of its prototype chain, so the read
 * answers `undefined` with no walk at all. `cached_slot` is unused and 0.
 *
 * Its validity is the depth > 0 entry's, exactly: the receiver's shape covers
 * every own-property add (an add transitions the shape), and the epoch covers
 * every way a key can appear on the chain — an add to any marked-prototype
 * shape, a dictionary define, a prototype swap. The fill additionally proves
 * the chain runs to its END through plain, non-dictionary objects whose
 * shapes are all MARKED as prototypes, because an unmarked link is one whose
 * adds would not bump the epoch (runtime/object.cpp, absentThroughChain).
 *
 * The flag is 0x40000000 rather than a shape sentinel because a negative
 * entry still names a real shape to compare against, and because it must not
 * collide with the accessor flag in the same field. Real depths are bounded
 * by ObjectHeader::kMaxPrototypeDepth, far below either bit.
 *
 * ---- the DOUBLE-SLOT entry (set sites only) -------------------------------
 *
 * `cached_depth & BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG` means: the slot this entry
 * names is one the shape calls an f64 (runtime/slot_repr.h). The store arm may
 * still take it — the bits of a boxed Number ARE the double's bits — but ONLY
 * after testing that the value being stored is a Number, because a raw store
 * of anything else would put a pointer in a slot whose representation says
 * there is a double there. A non-Number at such a site misses to
 * `bronze_prop_set`, where `ObjectHeader::setSlot` generalizes the slot back
 * to boxed and the entry is refilled without the flag.
 *
 * The flag rides in the depth field because the set arm's guard already loads
 * that word and compares it to zero: the test costs the arm one `and` and one
 * compare, and an arm that has not learned about representations refuses the
 * entry outright (a nonzero depth is not an own-property hit) rather than
 * taking it wrongly. GET sites never carry it — a read of a double slot is a
 * read of the number's box and needs no test at all.
 *
 * Non-shape sentinel discipline:
 * When an entry caches an Array built-in method (or the Array constructor),
 * `cached_shape` holds BRONZE_ABI_IC_SHAPE_ARRAY_METHOD ((uintptr_t)1).
 * Real Shape pointers are 8-byte aligned arena allocations and can never be 1.
 * The slot word then holds the method ID — an index into
 * the TLS block's `array_method_tbl`, NOT a Value: the collector moves function
 * objects, so the Value lives in that rooted table and the entry stores
 * only the immortal index. The epoch word is 0 and unread for these
 * entries. Only GET sites ever hold the sentinel (lowering never shares
 * an IC index between a read and a write, `lower_update.cpp`), which is
 * why the set-side transition arm may still dereference `cached_shape`
 * after its null check.
 */
#define BRONZE_ABI_IC_ENTRY_SIZE     24 /* sizeof(InlineCache) */
#define BRONZE_ABI_IC_SHAPE_OFFSET    0 /* InlineCache::cached_shape (pointer) */
#define BRONZE_ABI_IC_SLOT_OFFSET     8 /* InlineCache::cached_slot  (uint32) */
#define BRONZE_ABI_IC_DEPTH_OFFSET   12 /* InlineCache::cached_depth (uint32) */
#define BRONZE_ABI_IC_EPOCH_OFFSET   16 /* InlineCache::cached_epoch (uint64) */
#define BRONZE_ABI_IC_SHAPE_ARRAY_METHOD 1ull
#define BRONZE_ABI_IC_DEPTH_ACCESSOR_FLAG 0x80000000u
#define BRONZE_ABI_IC_DEPTH_ABSENT_FLAG   0x40000000u
#define BRONZE_ABI_IC_DEPTH_DOUBLE_FLAG   0x20000000u
/* How many (shape -> answer) entries one property site holds, and the stride
 * the IC table is therefore indexed by. Four, from the marker-probe evidence
 * in brobench/analysis/chunk1_bill.md: three.js's polymorphic sites mix an
 * Object3D/Mesh pair with a Scene and a light or two, and four ways is the
 * width that holds that mix while keeping the way scan a short compare chain
 * a way-0 hit never enters. */
#define BRONZE_ABI_IC_WAYS 4
#define BRONZE_ABI_IC_SITE_SIZE (BRONZE_ABI_IC_ENTRY_SIZE * BRONZE_ABI_IC_WAYS)
/* slot and depth are adjacent and little-endian, so the single u64 at
 * IC_SLOT_OFFSET is (depth << 32) | slot. `that word < kInlineSlots` is
 * therefore ONE compare meaning "own property, in an inline slot" — the
 * exact envelope the inline fast path covers. Reading only the low half
 * would forget the depth and return an ancestor's slot off the receiver,
 * which is the bug the cached depth exists to prevent. */
#define BRONZE_ABI_IC_SLOTWORD_OFFSET BRONZE_ABI_IC_SLOT_OFFSET

/* ---- the METHOD-CALL site, as generated code reads it ---------------------
 *
 * A method-call site owns the same BRONZE_ABI_IC_SITE_SIZE bytes a property
 * site does — the lowerer numbers them out of one table — but its words mean
 * something different, and only the first four are used (the rest stay the
 * zero .bss gave them):
 *
 *   word 0: the receiver shape the entry was latched against, or 0 for an
 *           entry that has never latched. The guard, exactly as before.
 *   word 1: the callee's code pointer (DIRECT form; unused in slot form).
 *   word 2: low 32 bits the callee's arity; HIGH 32 bits select the form:
 *           zero is DIRECT, and any nonzero value is SLOT form carrying the
 *           receiver's own slot index PLUS ONE (so an all-zero word cannot
 *           read as slot 0).
 *   word 3: DIRECT form's env argument, passed to the code pointer verbatim.
 *           BRONZE_ABI_UNDEFINED_BITS for an env-free callee — and for every
 *           slot-form entry, so the word always parses as a Value — or the
 *           callee's environment record, a HEAP Value the module registered
 *           as a value cell via bronze_register_method_ic_cells at init.
 *
 * The DIRECT form is for a callee found on the PROTOTYPE CHAIN (depth >= 1)
 * as a data property: shape match AND epoch match calls word 1 with word 3 as
 * env. The receiver's shape determines the holder, but not what the holder's
 * slot holds — `C.prototype.m = f` overwrites it without moving any shape, and
 * an add to an intermediate prototype shadows it without moving the
 * receiver's. So word BRONZE_ABI_METHOD_IC_EPOCH_WORD records the thread's
 * prototype-mutation epoch (bronze_tls_block.proto_epoch) at latch time, and a
 * hit requires it to still be current. Every such change moves the epoch: an
 * add, delete, accessor define or prototype swap on a marked prototype, and an
 * overwrite of a FUNCTION in a marked prototype's slot — which always reaches
 * the runtime, because no set-site entry is ever filled for a marked shape
 * (Shape::markObjectAsPrototype). With the epoch current the function object
 * is the one latched, so caching its env is as sound as caching its code.
 * A callee found anywhere else — an own property, through an accessor, on a
 * dictionary — is never latched DIRECT.
 *
 * The SLOT form is for a callee that is the receiver's OWN data property
 * (depth 0), where same-shape receivers can hold DIFFERENT functions in the
 * same slot — per-instance closures, and every host function an embedder
 * hangs on an object — so nothing about the callee may be cached. The entry
 * caches only WHERE the method lives: generated code loads the receiver's
 * slot (inline or overflow, split at BRONZE_ABI_OBJ_INLINE_SLOTS), verifies
 * the value is a Function, and calls its CURRENT code with its CURRENT env
 * and arity — the same universal dispatch bronze_dynamic_call performs, so a
 * swapped-in non-function still reaches the helper's TypeError. No heap word
 * lands in the entry at all, which is what makes the form GC-free.
 *
 * A FUNCTION receiver takes the SLOT form too, and word 0 is then its STATICS
 * BOX's shape rather than the receiver's — a function object has no shape word
 * at all. The words mean exactly what they mean above: a shape, and a slot in
 * the object that shape describes. Which object the slot is read from is
 * decided by the arm the LIVE receiver's flags select — the receiver itself
 * when Plain, the box at BRONZE_ABI_FN_PROPERTIES_OFFSET when Function — and
 * never by anything stored in the entry, so the two arms share word 0 and
 * word 2 without either misreading the other's. They cannot disagree about the
 * answer either: a shape is a key-to-slot map and nothing else, so every
 * object matching word 0 holds the site's key at that slot in itself. `this`
 * stays the receiver in both cases.
 *
 * That argument is the SLOT form's alone. A function receiver must never take
 * a DIRECT entry, because a DIRECT entry latched for a plain receiver was
 * resolved off that receiver's PROTOTYPE CHAIN, whose key need not be in the
 * shape at all — so generated code's function arm refuses a zero high half and
 * takes the helper.
 *
 * The EXOTIC form serves the receivers whose flags are NOT Plain — an Array
 * or a global-constructor FUNCTION (`Array.isArray(x)`), whose methods are
 * native builtins answered from an immutable C table beside the value rather
 * than from any shape-indexed slot, and a typed-array view, whose shape word
 * generated code's Plain arm never reads. Word 0 then holds, instead of a
 * shape:
 *
 *      (auxOffset << BRONZE_ABI_METHOD_IC_BOX_SHIFT)
 *    | (receiver kind << BRONZE_ABI_METHOD_IC_KIND_SHIFT)
 *    | [BRONZE_ABI_METHOD_IC_CODE_GUARD_BIT]
 *    | BRONZE_ABI_METHOD_IC_EXOTIC_BIT
 *
 * Bit 0 set is what distinguishes it: a real Shape* is an 8-byte-aligned
 * arena allocation and can never be odd (the same argument the property IC's
 * BRONZE_ABI_IC_SHAPE_ARRAY_METHOD sentinel makes). The kind is the
 * receiver's HeapObjectHeader::flags as latched — a runtime value the guard
 * compares against the live receiver's flags, so no kind number is baked
 * into generated code. The guard's first clause is always
 *
 *   receiver flags == latched kind,
 *
 * and its second clause loads the u64 at `auxOffset` bytes from the
 * receiver's header and asks one of two questions, selected by bit 1:
 *
 *   bit 1 clear (BOX guard): the loaded Value must not be Object-tagged.
 *   `auxOffset` names the receiver's ordinary named-property box
 *   (ArrayHeader::properties / MapHeader::properties — runtime layouts,
 *   carried in the entry rather than baked into code). The box is the ONLY
 *   way such a receiver can answer a member with anything but the C table —
 *   an own named property (`a.push = f`) lives in it, and a subclass
 *   instance's [[Prototype]] chain hangs off it (runtime/native_base.h) —
 *   so a receiver carrying one takes the helper, which walks the box first
 *   exactly as the read path does. The table itself cannot change:
 *   decorating `Array.prototype` is a hard error by construction
 *   (rt_prop_write.cpp).
 *
 *   bit 1 set (CODE guard): the loaded word must EQUAL word
 *   BRONZE_ABI_METHOD_IC_AUX_WORD of the site. Two latches use it:
 *
 *   - a Function receiver whose callee is a global constructor's static
 *     (builtin_constructors.cpp's kCtors): `auxOffset` is the FunctionHeader
 *     code offset and the aux word is the constructor's own code pointer,
 *     the one identity a moving collector never rewrites. No box clause is
 *     needed at all, because the statics table is consulted FIRST on the
 *     function-receiver ladder, ahead even of the own-property box
 *     (rt_prop.cpp) — nothing can shadow it, so the answer is a pure
 *     function of (receiver code, key).
 *
 *   - a typed-array VIEW, an ordinary shape-carrying object whose methods
 *     live on %TypedArray%.prototype: `auxOffset` is its shape word
 *     (BRONZE_ABI_OBJ_SHAPE_OFFSET) and the aux word the Shape* latched
 *     against, so the guard is the plain DIRECT form's own shape compare —
 *     with the plain form's depth >= 1 envelope — spelled for a receiver
 *     generated code's Plain arm does not take. A Shape is an immortal,
 *     non-moving arena allocation.
 *
 *   The aux word is a raw C pointer either way, never a Value: the collector
 *   must not touch it, and it never does — a module's method-site
 *   registration covers word 3 only.
 *
 * Words 1–3 are the DIRECT form's: the native's code pointer (a C function
 * in the runtime image, immortal), its arity with a zero high half, and
 * BRONZE_ABI_UNDEFINED_BITS for env — a native builtin is created env-free
 * (rt_builtins.h's rtNativeFunction) and the latch refuses any callee that
 * is not.  No heap word in the entry: GC-free, like the slot form.
 *
 * Mixed binaries stay sound in both directions: an old hit path compares the
 * odd word against a real shape and misses; a new exotic arm compares an old
 * runtime's shape-or-zero word 0 against an odd expectation and misses. Both
 * fall to the helper, which is always correct.
 *
 * ---- WAY 1: the site's second method entry --------------------------------
 *
 * Words 6-9 are a SECOND way, holding only a PLAIN-receiver DIRECT entry —
 * shape, code, arity, env at the same relative layout way 0 uses (6 <-> 0,
 * 7 <-> 1, 8 <-> 2, 9 <-> 3). It exists for the polymorphic method site a
 * recursive scene-graph walk makes — `node.updateMatrixWorld()` over a tree
 * mixing Object3D, Mesh and Scene shapes — which under one way misses
 * forever, relatching per receiver (three.js's `hierarchy` bench measures
 * 392 K such misses a run, ~1.9 % of its method calls).
 *
 * The fill policy is displacement, not scan-install: the latch always writes
 * way 0, and when doing so would overwrite a healthy plain-direct entry for
 * a DIFFERENT shape, that entry is copied into way 1 first. So way 0 is
 * always the most recent latch and way 1 the previous resident, and the
 * generated way-1 compare happens only after a way-0 shape miss. SLOT and
 * EXOTIC entries never occupy way 1 (their word-2/word-0 machinery stays
 * way-0-only), but either may displace a plain-direct entry into it.
 *
 * A way-1 hit obeys the same envelope as the way-0 direct entry it once was:
 * shape match and epoch match, the epoch in word
 * BRONZE_ABI_METHOD_IC_WAY1_EPOCH_WORD (moved there with the rest of the
 * entry). Word 9 is an env argument exactly as word 3 is, and
 * bronze_register_method_ic_cells registers BOTH as value cells — which is
 * why this contract change moves the fingerprint: an old runtime would leave
 * way 1's env word dangling at the first flip.
 *
 * ---- the PRIMITIVE form: a STRING receiver ---------------------------------
 *
 * `s.charCodeAt(i)` in a string-walking loop has no shape word to guard on
 * at all — the receiver is a NaN-boxed string, not an object — so the site
 * missed to the helper on every call, and the helper walked `String.prototype`
 * by name each time. The form is the EXOTIC encoding with the receiver's
 * VALUE TAG in the kind field:
 *
 *      (tag << BRONZE_ABI_METHOD_IC_KIND_SHIFT) | BRONZE_ABI_METHOD_IC_EXOTIC_BIT
 *
 * A tag is 0xFFF0 or above and a HeapObjectHeader kind is a small integer,
 * so the two can never be confused in the same field: the object arm
 * compares against the live receiver's flags and the primitive arm against
 * the live receiver's tag, and neither can match the other's word.
 *
 * It is a SLOT-form entry whose slot lives in `String.prototype` rather than
 * in the receiver — the intrinsic is an ordinary object a program may
 * decorate, and `String.prototype.charCodeAt = f` overwrites a slot in
 * place without moving its shape, which is exactly the write a cached code
 * pointer cannot see. So word BRONZE_ABI_METHOD_IC_ENV_WORD holds the
 * intrinsic itself (a heap Value, in the one word of the entry the collector
 * forwards), word BRONZE_ABI_METHOD_IC_AUX_WORD holds the intrinsic's Shape*
 * at latch time, and word 2 carries the slot index plus one in its high half
 * as every SLOT entry does. The hit re-reads the intrinsic's shape against
 * the aux word — a delete, an accessor install or a dictionary flip changes
 * it — and then reads the slot's CURRENT value and dispatches on that
 * function, Function test included, through the same path the receiver's own
 * slot takes. `this` is the string itself, exactly as the helper passes it.
 * The env word is never an env here: a SLOT hit derives the callee's env
 * from the function it finds, so the word is free to name the holder. */
#define BRONZE_ABI_METHOD_IC_CODE_WORD   1
#define BRONZE_ABI_METHOD_IC_ARITY_WORD  2
#define BRONZE_ABI_METHOD_IC_ENV_WORD    3
#define BRONZE_ABI_METHOD_IC_SLOT_SHIFT 32
#define BRONZE_ABI_METHOD_IC_EXOTIC_BIT       1ull
#define BRONZE_ABI_METHOD_IC_CODE_GUARD_BIT   2ull
#define BRONZE_ABI_METHOD_IC_KIND_SHIFT  2
#define BRONZE_ABI_METHOD_IC_BOX_SHIFT  32
#define BRONZE_ABI_METHOD_IC_AUX_WORD    4
#define BRONZE_ABI_METHOD_IC_WAY1_SHAPE_WORD 6
#define BRONZE_ABI_METHOD_IC_WAY1_CODE_WORD  7
#define BRONZE_ABI_METHOD_IC_WAY1_ARITY_WORD 8
#define BRONZE_ABI_METHOD_IC_WAY1_ENV_WORD   9
/* The prototype-mutation epoch a DIRECT entry was latched at, per way. Also
 * written for the typed-array view's EXOTIC form, which is the DIRECT form's
 * depth >= 1 case under a different guard. Plain integers, never Values. */
#define BRONZE_ABI_METHOD_IC_EPOCH_WORD      5
#define BRONZE_ABI_METHOD_IC_WAY1_EPOCH_WORD 10

/* ---- the COMPUTED-read cache, as generated code reads it -----------------
 *
 * runtime/elem_ic.h's `ElemCacheEntry`: an `InlineCache` (the same struct a
 * property site's way is, so the validity questions are literally the same
 * code) followed by the witness that pins the KEY, the arena copy of that key,
 * and the key's kind. Direct-mapped, one thread's table published into
 * `elem_cache_tbl` above.
 *
 * Generated code inlines the hit for a NUMBER, BOOLEAN or STRING key. The
 * first two confirm against the WITNESS word (raw double bits, 0/1); a string
 * cannot — the entry's `key` is an ARENA COPY, so a live key string is never
 * the same object, and confirming content means a length compare and a memcmp,
 * a loop the inline path must not carry. What it confirms against instead is
 * the IDENT word: the raw Value bits of the last LIVE string
 * `bronze_elem_get` proved content-equal to the entry's arena key (by
 * `StringHeader::equals`, which stays the helper's job). The guard is one
 * 64-bit compare — key bits against the ident word — and it is sound because
 * the runtime maintains two invariants around that word:
 *
 *   - a non-zero ident always names a string object content-equal to the
 *     entry's `key` AT THE SAME TIME as the entry's other words: every fill
 *     rewrites ident beside kind/witness/key (zero for a non-string kind),
 *     and the helper re-latches it only after `equals` has confirmed the
 *     live key against the entry it is latched into;
 *   - a moving collection clears, in the same pause, every ident that points
 *     into the MOVABLE reservation (elem_ic.h's sweep, a per-heap
 *     post-collection hook). The Cheney collector reuses an address only
 *     across a collection, so an ident that survives to compare equal still
 *     names the object it was latched from. An ident pointing OUTSIDE the
 *     reservation is an immortal arena string (a shape key handed out by
 *     for-in / Object.keys) and survives the sweep, which is what makes the
 *     enumeration-driven three.js sites hit across GC.
 *
 * The string arm needs the key's memoized hash to find the bucket, so it
 * reads the string's flags word (offsets below) and takes the helper when the
 * hash has not been memoized yet — the helper's first probe memoizes it.
 *
 * Seam: BRONZE_NO_ELEM_KEY_IC=1 gates the LATCH side (fills write 0, hits do
 * not re-latch), so an old-style run and a new one are one binary: with the
 * seam off, no ident is ever non-zero and the inline string arm can only
 * miss into the helper it always took.
 *
 * The bucket function is splitmix64's finalizer applied twice, and generated
 * code must reproduce it EXACTLY: a probe that hashes differently from the
 * fill does not answer wrongly, it simply never hits, which is a silent
 * regression rather than a bug. elem_ic.cpp static_asserts the constants. */
#define BRONZE_ABI_ELEM_ENTRY_SIZE      56 /* sizeof(ElemCacheEntry) */
#define BRONZE_ABI_ELEM_IC_OFFSET        0 /* ElemCacheEntry::ic (InlineCache) */
#define BRONZE_ABI_ELEM_WITNESS_OFFSET  24 /* ElemCacheEntry::witness (uint64) */
#define BRONZE_ABI_ELEM_KEY_OFFSET      32 /* ElemCacheEntry::key (StringHeader*) */
#define BRONZE_ABI_ELEM_KIND_OFFSET     40 /* ElemCacheEntry::kind (uint8) */
#define BRONZE_ABI_ELEM_IDENT_OFFSET    48 /* ElemCacheEntry::key_ident (uint64) */
#define BRONZE_ABI_ELEM_ENTRIES       4096 /* kElemCacheEntries, a power of two */
#define BRONZE_ABI_ELEM_SET_ENTRIES   1024 /* kElemSetCacheEntries, a power of two */
#define BRONZE_ABI_ELEM_KIND_NUMBER      1
#define BRONZE_ABI_ELEM_KIND_STRING      2
#define BRONZE_ABI_ELEM_KIND_BOOL        3

/* StringHeader, as the inline string-key arm reads it: the mutable flags
 * word carries the memoized hash. Bit 0 is the UTF-16 flag, bit 1 says the
 * hash IS memoized, and the top 30 bits are the hash itself (hash() returns
 * `h & ~3u`, so masking the flags word with the same mask recovers exactly
 * what witnessFor stored). runtime/elem_ic.cpp static_asserts all four
 * against the real struct. */
#define BRONZE_ABI_STRING_LENGTH_OFFSET  8 /* StringHeader::length (uint32, code units) */
#define BRONZE_ABI_STRING_FLAGS_OFFSET  12 /* StringHeader::flags (uint32) */
#define BRONZE_ABI_STRING_DATA_OFFSET   16 /* StringHeader payload (latin1 or utf16) */
#define BRONZE_ABI_STRING_UTF16_BIT      1 /* StringHeader::kUTF16Flag */
#define BRONZE_ABI_STRING_HASHED_BIT     2 /* StringHeader::kHasHashFlag */
#define BRONZE_ABI_STRING_HASH_MASK     0xFFFFFFFCu
#define BRONZE_ABI_STRING_BUILDER_BIT   1 /* StringHeader::kBuilderFlag (in HeapObjectHeader::flags) */
#define BRONZE_ABI_MIX64_ADD  0x9E3779B97F4A7C15ull
#define BRONZE_ABI_MIX64_MUL1 0xBF58476D1CE4E5B9ull
#define BRONZE_ABI_MIX64_MUL2 0x94D049BB133111EBull

/* Value: NaN-boxed, tag in the top 16 bits (runtime/value.h). */
#define BRONZE_ABI_VALUE_TAG_SHIFT      48
#define BRONZE_ABI_VALUE_PAYLOAD_MASK   0x0000FFFFFFFFFFFFull
#define BRONZE_ABI_TAG_OBJECT           0xFFF1
#define BRONZE_ABI_TAG_STRING           0xFFF2
#define BRONZE_ABI_TAG_INT32            0xFFF3
#define BRONZE_ABI_TAG_BOOL             0xFFF4
#define BRONZE_ABI_TAG_NULL             0xFFF5
#define BRONZE_ABI_TAG_UNDEFINED        0xFFF6
#define BRONZE_ABI_TAG_HOLE             0xFFF7
/* Above the number range like every other tag, and named here because the
 * inline `===` needs to tell the two values whose equality is NOT bit
 * equality — a string and a BigInt — from every value whose equality is.
 * runtime/value.h holds the definition; runtime/rt_convert.cpp asserts the
 * two agree. */
#define BRONZE_ABI_TAG_BIGINT           0xFFFB
#define BRONZE_ABI_CANONICAL_NAN_BITS   0x7FF8000000000000ull
#define BRONZE_ABI_NUMBER_MAX_BITS      0xFFF0000000000000ull
#define BRONZE_ABI_HOLE_BITS            0xFFF7000000000000ull

/* PIN CENSUS site info (`bronze_census_register` / `bronze_census_record`,
 * src/runtime/pin_census.h). The low byte is WHICH MANIFEST FORM the site's
 * key names, because the four forms admit different kinds — only the field
 * forms have `number-or-nullish` and `numeric-elements` to fall back to.
 *
 * `OPAQUE` is not a form: it is a store to a field NAME through a receiver
 * inference could not type, which is B1's one remaining silent hole
 * (src/types/pins.h). It names no class, so it can never be an entry; what it
 * does is mark every entry for a field of that name `@observed`.
 *
 * `REFUSES` is a site that disqualifies its entry on REGISTRATION, with no
 * observation needed — a return the body can fall off, or an owner spelling
 * that would govern two different functions. */
#define BRONZE_ABI_CENSUS_KIND_MASK  0xFFu
#define BRONZE_ABI_CENSUS_REFUSES    0x100u
#define BRONZE_ABI_CENSUS_ENV_SLOT   0u
#define BRONZE_ABI_CENSUS_FIELD      1u
#define BRONZE_ABI_CENSUS_PARAM      2u
#define BRONZE_ABI_CENSUS_RETURN     3u
#define BRONZE_ABI_CENSUS_OPAQUE     4u

/* HeapObjectHeader::flags, and the values that mean "a plain object" as
 * opposed to an array (1), a function (2), a typed-array view (3) or an
 * ArrayBuffer (4). All of them reach bronze_prop_get, so the fast path has
 * to discriminate on this before it believes anything else. */
#define BRONZE_ABI_OBJ_FLAGS_OFFSET      2
#define BRONZE_ABI_OBJ_FLAGS_PLAIN       0
#define BRONZE_ABI_OBJ_FLAGS_ARRAY       1
#define BRONZE_ABI_OBJ_FLAGS_TYPED_ARRAY 3

/* ArrayHeader field offsets */
#define BRONZE_ABI_ARRAY_LENGTH_OFFSET   8
#define BRONZE_ABI_ARRAY_CAPACITY_OFFSET 12
#define BRONZE_ABI_ARRAY_HEAD_OFFSET     16
#define BRONZE_ABI_ARRAY_ELEMS_OFFSET    24
#define BRONZE_ABI_ARRAY_PROPS_OFFSET    32
/* The whole ArrayHeader, header word included, and the elements block it
 * points at: a heap object of BRONZE_ABI_OBJ_FLAGS_VALUE_BLOCK kind whose
 * payload is `capacity` Values, HOLE-filled past `length`. An array literal of
 * a few elements is both objects bump-allocated from the inline window by
 * generated code (llvm_construct.cpp), laid out exactly as bronze_create_array
 * lays them out — including its capacity floor. */
#define BRONZE_ABI_ARRAY_HEADER_BYTES    40
#define BRONZE_ABI_ARRAY_MIN_CAPACITY    4
#define BRONZE_ABI_OBJ_FLAGS_SLOT_BLOCK  18
#define BRONZE_ABI_OBJ_FLAGS_VALUE_BLOCK 19

/* Environment records (runtime/env.h EnvHeader): the parent link, then the
 * slot array. Generated code inlines captured-variable reads and writes —
 * a load per depth step and a load or store of the slot — with every guard
 * failure (wrong tag, wrong kind, short chain, slot out of range) routed to
 * the helper, which still owns the fatal that names the lowering bug.
 * Pinned by static_asserts in runtime/env.h. */
#define BRONZE_ABI_OBJ_FLAGS_ENV        12
#define BRONZE_ABI_ENV_PARENT_OFFSET     8
#define BRONZE_ABI_ENV_SLOTS_OFFSET     16

/* HeapObjectHeader::size — total object bytes, header included. The inline
 * environment path reads it to keep the helper's slot-range tripwire: a slot
 * index past the record is a lowering bug and must still reach the fatal
 * rather than a load past the object. Pinned in runtime/object.h. */
#define BRONZE_ABI_HDR_SIZE_OFFSET       4

/* Total bytes of HeapObjectHeader itself — where a heap block's payload
 * starts. The method-IC slot form reads an overflow block's Value array
 * through it. Pinned in runtime/object.h. */
#define BRONZE_ABI_HDR_BYTES             8

/* TypedArrayHeader (runtime/typed_array.h): the ordinary-object prefix (the
 * shape word, the overflow word and BRONZE_ABI_OBJ_INLINE_SLOTS property
 * slots, laid out exactly as a plain object's so a view carries a real
 * [[Prototype]] and own properties), then the buffer Value, the window, and
 * the element kind. BUF_EXTPTR_OFFSET is the buffer's external-storage word,
 * after the same prefix: zero for an ordinary buffer (bytes inline, starting
 * at BUF_DATA_OFFSET == sizeof(ArrayBufferHeader)), or the address of a
 * NON-MOVING host byte store (embed.h externalizeArrayBuffer) — the element
 * paths select between the two, which is the whole cost of shareable
 * buffers. Pinned in runtime/typed_array.h. */
#define BRONZE_ABI_TA_BUFFER_OFFSET     56
#define BRONZE_ABI_TA_BYTEOFFSET_OFFSET 64
#define BRONZE_ABI_TA_LENGTH_OFFSET     68
#define BRONZE_ABI_TA_KIND_OFFSET       72
#define BRONZE_ABI_TA_KIND_INT8          0
#define BRONZE_ABI_TA_KIND_UINT8         1
#define BRONZE_ABI_TA_KIND_UINT8CLAMPED  2
#define BRONZE_ABI_TA_KIND_INT16         3
#define BRONZE_ABI_TA_KIND_UINT16        4
#define BRONZE_ABI_TA_KIND_INT32         5
#define BRONZE_ABI_TA_KIND_UINT32        6
#define BRONZE_ABI_TA_KIND_FLOAT32       7
#define BRONZE_ABI_TA_KIND_FLOAT64       8
/* The first kind whose elements are BIGINTS rather than Numbers. Generated
 * code never stores to these inline, but the dynamic-store fast path must
 * KNOW where they start: an out-of-bounds store on a Number kind is a
 * discard (10.4.5.16 — ToNumber of a number is the number), while on a
 * BigInt kind the same store still owes the ToBigInt that throws for a
 * Number value, so it must reach the helper. kind < BIGINT64 is that test. */
#define BRONZE_ABI_TA_KIND_BIGINT64     10
#define BRONZE_ABI_BUF_EXTPTR_OFFSET    72
#define BRONZE_ABI_BUF_DATA_OFFSET      80

/* ObjectHeader: the shape word, then the out-of-line overflow Value, then
 * kInlineSlots inline Values. */
#define BRONZE_ABI_OBJ_SHAPE_OFFSET      8
#define BRONZE_ABI_OBJ_OVERFLOW_OFFSET  16
#define BRONZE_ABI_OBJ_SLOTS_OFFSET     24
#define BRONZE_ABI_OBJ_INLINE_SLOTS      4

/* Shape (runtime/shape.h): the fields the two inline IC fast paths read, all
 * deliberately laid out BEFORE the transitions vector so no standard-library
 * type's size can shift them between build configurations. Shapes are immortal
 * and non-moving, so generated code may chase these pointers freely.
 *
 * The depth > 0 READ walk needs root -> prototype (the chain step) and dict
 * (a dictionary on the path is the miss `cachedProtoHolder` answers). The
 * shape-transition WRITE hit needs parent (is the cached shape one add above
 * the receiver's?), slot_index (does the cached shape's own node carry the
 * site's key? — slot uniqueness along a chain makes `slot_index ==
 * cached_slot` that exact question), the four attribute bytes as one word
 * (an assignment creates enumerable/writable/configurable data properties
 * and nothing else may be cached into one), and used_as_prototype (a marked
 * receiver must bump the epoch, which is the helper's job). Pinned by
 * static_asserts in runtime/shape.h. */
#define BRONZE_ABI_SHAPE_PARENT_OFFSET      0
#define BRONZE_ABI_SHAPE_SLOTINDEX_OFFSET  16
#define BRONZE_ABI_SHAPE_ATTRS_OFFSET      20
/* enumerable=1, accessor=0, writable=1, configurable=1 as the one
 * little-endian u32 the four adjacent bool bytes spell. */
#define BRONZE_ABI_SHAPE_ATTRS_PLAIN_DATA  0x01010001u
#define BRONZE_ABI_SHAPE_ROOT_OFFSET       24
#define BRONZE_ABI_SHAPE_PROTO_OFFSET      32
#define BRONZE_ABI_SHAPE_DICT_OFFSET       40
#define BRONZE_ABI_SHAPE_USEDPROTO_OFFSET  48
/* Shape::family_stamp — the layout-family id `bronze_family_stamp` writes and
 * a family guard reads. It lives on the SHAPE and not on the object because
 * that is where the fact belongs: every object at a shape has that shape's
 * property names at that shape's slots, so one verification answers for all of
 * them, and an object header stays the width it was. The guard already loads
 * the shape pointer for the identity form, so this is one more load off memory
 * that is immortal, shared by every instance, and hot. */
#define BRONZE_ABI_SHAPE_FAMILY_OFFSET     56
/* Not a class id: zero is "never looked at" and one is "looked at and matched
 * no registered class". Both fail every range compare, because the runtime
 * hands out ids from BRONZE_ABI_FAMILY_FIRST_ID up. */
#define BRONZE_ABI_FAMILY_UNSTAMPED        0
#define BRONZE_ABI_FAMILY_NONE             1
#define BRONZE_ABI_FAMILY_FIRST_ID         2

/* Shape::double_slots — the per-slot REPRESENTATION word (stage R1,
 * runtime/slot_repr.h). Bit N is set when slot N of an object at this shape
 * holds a double rather than a boxed Value: reading those eight bytes as an
 * f64 is correct without a tag test, and writing one requires a number.
 *
 * A BITMAP on the shape rather than a byte on each node, because both
 * consumers want the whole answer at once and neither wants a chain walk. The
 * collector asks "which of this object's slots must I NOT trace" once per
 * object; a future codegen (stage R2) asks "is the slot this site loads a
 * double" once per guard, against a word it has already loaded the shape for.
 * Slots at or above BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT are never given the
 * representation, so a clear bit is always a truthful "boxed".
 *
 * Zero on every shape when BRONZE_NO_SLOT_REPR=1, which is the seam: with it
 * set no shape node is ever created double and this word is dead. */
#define BRONZE_ABI_SHAPE_DOUBLESLOTS_OFFSET 64
#define BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT  64
/* Shape::repr — the representation of the ONE slot this node owns, as the
 * SlotRepr enum spells it (0 boxed, 1 double). Redundant with the bitmap
 * above and kept anyway: the bitmap is a summary of the whole chain and
 * cannot say which node introduced a bit, which is what the generalization
 * rebuild and the census both need. */
#define BRONZE_ABI_SHAPE_REPR_OFFSET        72
#define BRONZE_ABI_SLOT_REPR_BOXED           0
#define BRONZE_ABI_SLOT_REPR_DOUBLE          1

/* FunctionHeader::code — the identity the Math direct-dispatch guard compares
 * (see the bronze_math_* registry entries). Pinned in runtime/fn.cpp. */
#define BRONZE_ABI_OBJ_FLAGS_FUNCTION    2
#define BRONZE_ABI_FN_CODE_OFFSET        8

/* The FunctionHeader fields the inline `new` fast path reads, and the vet
 * byte that gates it. `construct_vetted` is set by exactly one line in the
 * runtime — bronze_construct's ordinary path, after the bound-function and
 * primitive-wrapper probes have both missed and the prototype/instance_shape
 * pair exists — so a set byte means "the helper has already taken the plain
 * path for this function object", which is monotone: a function's code
 * pointer never changes, so it can never later become bound or a wrapper
 * constructor. Reassigning `.prototype` swaps `instance_shape` in the same
 * write (rt_prop_write.cpp) and never nulls it, so the vetted fast path
 * reads whatever is current. Pinned in runtime/fn.h. */
#define BRONZE_ABI_FN_ENV_OFFSET            16
#define BRONZE_ABI_FN_PROTOTYPE_OFFSET      24
#define BRONZE_ABI_FN_PROPERTIES_OFFSET     32
#define BRONZE_ABI_FN_INSTANCE_SHAPE_OFFSET 40
#define BRONZE_ABI_FN_ARITY_OFFSET          56
#define BRONZE_ABI_FN_CTOR_VETTED_OFFSET    65

/* Total bytes of a plain object with no internal slots — header, shape word,
 * overflow word, and the kInlineSlots inline Values — which is the one size
 * the inline `new` fast path allocates. Already 8-aligned, so it is also the
 * exact amount the bump cursor advances. Pinned in runtime/object.cpp. */
#define BRONZE_ABI_PLAIN_OBJECT_BYTES    56

/* One entry of a module's fn-singleton table: the code pointer, then the Value.
 * An entry answers a mention only when its code word matches the mention's own
 * function pointer, so a slot that was never filled (a zeroed table) misses,
 * and the by-code-pointer map in rt_state.cpp stays the authority on identity
 * across every module in the process. */
#define BRONZE_ABI_FNSLOT_SIZE          16
#define BRONZE_ABI_FNSLOT_CODE_OFFSET    0
#define BRONZE_ABI_FNSLOT_VALUE_OFFSET   8

/* Every Object-tagged heap allocation has at least this many payload bytes,
 * which is what makes the fast path's unconditional load of the shape word
 * (offset 8..15) safe BEFORE the flags discrimination has passed. Pinned by
 * static_asserts over every Object-tagged header in rt_helpers.cpp. */
#define BRONZE_ABI_OBJ_MIN_PAYLOAD       8

