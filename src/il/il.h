#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"
// What a `census.record` site observes. Its own header — the runtime's census
// table is the other half of that fact — included here so an instruction's
// readers still find `CensusSite` where they always did.
#include "il/census_site.h"
// The pin claims the IL carries (`PinBarrier`, `kElemKindPlainArrayF64`).
// Included here rather than left to each user, because every one of them
// reaches these names through this header and a seam is not an occasion to
// make twenty files say where they now live.
#include "il/il_pin.h"
#include "il/il_op.h"

namespace bronze::il {

// bronze IL: a typed, SSA, mid-level IR. Static layouts are used wherever
// analysis can PROVE them (inference-first; TS annotations are untrusted
// hints); Dynamic is the explicit fallback type for code the analysis
// cannot type — wild JS must always compile. Dynamic is the fallback and
// never the substrate: a value is boxed because nothing PROVED it, not
// because boxing is the default the proofs opt out of.
//
// Deliberately tiny today: enough structure to carry lower→codegen work and
// to pin the canonical text form. Every addition must keep print(parse(x))
// byte-stable once the text parser lands.

enum class Type : uint8_t {
    Void,
    Bool,
    I32,
    F64,
    Str,      // native string (representation decided in)
    Dynamic,  // boundary-only boxed value; using it is an explicit opt-in
};
const char* typeName(Type t);

using ValueId = uint32_t;
inline constexpr ValueId kNoValue = UINT32_MAX;

using BlockId = uint32_t;
inline constexpr BlockId kNoBlock = UINT32_MAX;

struct BlockParam {
    ValueId id = kNoValue;
    Type type = Type::Void;
};

struct BlockTarget {
    BlockId block = kNoBlock;
    std::vector<ValueId> args;
};

struct Instruction {
    Op op;
    Type type = Type::Void;          // result type (Void: no result)
    ValueId result = kNoValue;
    std::vector<ValueId> operands;
    double immF64 = 0;               // ConstF64
    int32_t immI32 = 0;              // ConstI32 / CreateArray length / CreateFunction arity
    uint32_t calleeIndex = 0;        // Call/CreateFunction: index into Module::functions
    // The OPERAND's type, for the two ops whose behaviour turns on it and whose
    // own `type` is the result: Box (what is being boxed) and ToInt32 (whether
    // step 1's ToNumber is a machine conversion or a call that can throw).
    // Not printed for ToInt32 — `to.int32 %n` names one operation either way,
    // and the operand's own definition already says what type it is.
    Type boxType = Type::Void;
    uint32_t keyIndex = 0;           // PropGet/PropSet: key constant index
    uint32_t icIndex = 0;            // PropGet/PropSet: IC site index
    // PropGet: inference proved this site's receiver has ONE shape class, so
    // the backend may inline the cache check instead of calling the helper.
    // False is always sound — it is the plain call — and an unproven site keeps
    // it, so the inline form can never degenerate into a polymorphic guard
    // chain in generated code. It licenses the inline sequence; it does not
    // remove the guard. The shape word is the runtime's authority and a shape
    // class can name a layout the runtime never builds (`this.x =...` inside a
    // branch), so the guard is what makes the proof sound, not a redundancy on
    // top.
    bool icMonomorphic = false;
    // PropGet/MethodCall: this site's RECEIVER EXPRESSION is a name bound to a
    // class or function DECLARATION — `Object3D.DEFAULT_UP`, `Object.keys`,
    // `MathUtils.generateUUID`, a class name used inside its own body —
    // rather than `this`, a parameter, or a local holding an instance. So the
    // receiver can be a FUNCTION object at run time, and a function's members
    // do not live where every other receiver's do: a `static` sits in a side
    // object hanging off the FunctionHeader, with its own shape and slots.
    //
    // It is a HINT and never a proof. The binding can be reassigned, shadowed
    // by a `with`, or hold something else entirely, so generated code still
    // tests the receiver's flags — what the bit decides is only whether the
    // arm that handles a function receiver is EMITTED at this site.
    //
    // That is worth a bit of its own because the arm is not free to emit
    // everywhere. Emitting it at every read measurably slowed pure-math code,
    // in block placement and live ranges around the cache, for an arm those
    // reads can never take. False is always sound; it is the helper, which is
    // the answer the arm exists to avoid asking for and never a different one.
    bool icFnRecv = false;
    // Unbox: the operand is PROVEN to be a Number, so the conversion is a
    // bitcast and nothing else — no tag test, no branch, no ToNumber helper, no
    // phi. bronze's Value is NaN-boxed with the doubles at the bottom of the
    // encoding, so a Number's bits ARE its double's bits: an unboxed f64 field
    // needs no second representation, only permission to stop checking.
    //
    // Granted by exactly one thing, `InferenceResult::provenFieldReads`: a read
    // of a field whose class installs it on every construction path, whose
    // receiver this compilation watched being made, and whose name the
    // whole-program write audit certified — or by a DOMINATING `is.number` on
    // this same SSA value, which is what the guarded-region pass emits: the
    // test is the condition of a block's terminator and the raw unbox sits in
    // the block on its true edge. On an SSA VALUE and never on a location — a
    // property re-read after a call is a different value and gets its own
    // guard. `false` is the checked form, which is ToNumber and correct for
    // anything, and is what every other Number claim in the lattice keeps.
    bool rawUnbox = false;
    // Unbox: the operand is PROVEN to be a Number, `null` or `undefined` and
    // nothing else, so ToNumber is decidable from the bits with no call. The
    // number range is the BOTTOM of the NaN box, so one unsigned compare
    // separates the arms, and the nullish arm is two constants — +0 for null
    // and NaN for undefined (7.1.4 table 14). Emitted branchless: the compares
    // and selects stay in the block, which is the point, because what this
    // exists for is the middle of a chain of fmuls.
    //
    // Mutually exclusive with `rawUnbox` and strictly weaker: a raw unbox of
    // `undefined` reads 0xFFF6000000000000 as a double. Granted by exactly one
    // thing, `InferenceResult::nullishNumberFieldReads` — a `--pins
    // number-or-nullish` declaration (types/pins.h) — and by no lattice type,
    // because there is no lattice element for "number or nullish" and there is
    // deliberately never going to be one.
    bool nullishUnbox = false;
    // PropGet/PropSet: the instance slot a PROVEN CLASS LAYOUT puts this key
    // at, or `kNoStaticSlot`. Strictly stronger than `icMonomorphic`, which is
    // only an identity claim: this says the receiver's class was modellable end
    // to end (every field it installs, in the runtime's own transition order,
    // through the whole `extends` chain) and that this key is one of the own
    // data properties in it — never a prototype method, never an accessor.
    //
    // What it licenses is a load at a COMPILE-TIME CONSTANT offset behind a
    // single shape compare, instead of decoding a cache entry's slot word. It
    // still does not remove the guard, and it cannot: the layout is derived
    // from source and the shape is built at run time. The two are reconciled at
    // `staticCellIndex` below.
    static constexpr uint32_t kNoStaticSlot = 0xFFFFFFFFu;
    uint32_t staticSlot = kNoStaticSlot;
    // The module-global cell this site compares the receiver's shape against.
    // One i64 per static site, zero-initialized, published by the runtime the
    // first time the site's slow path sees an object that really does carry
    // this key as an own data property at `staticSlot`. A layout that was
    // wrong therefore never publishes, and the site simply keeps taking its
    // slow path — the failure mode is a cost, never an answer.
    uint32_t staticCellIndex = 0;
    // PropGet/PropSet: the LAYOUT FAMILY this site guards on instead of one
    // shape's identity, as the preorder id of the receiver's class and the size
    // of its `extends` subtree. `kNoFamily` for a site that keeps the identity
    // form.
    //
    // This is what a `this` receiver inside a method of an EXTENDED class gets.
    // Its static class is not its runtime shape — three.js never constructs a
    // bare `Object3D`, so `this.matrixWorld` in `updateMatrixWorld` runs on a
    // Group, a Mesh and a Scene — and a cell that pins one of those misses on
    // the rest forever. The layout was never wrong: a proven subclass's fields
    // begin with its base's, at the same slots. So the guard changes shape
    // rather than the claim, from `shape == pinned` to `stamp - (base + lo) <=u
    // span`, where the stamp is a word the runtime wrote onto the SHAPE after
    // verifying that class's whole field list against it.
    //
    // `staticSlot` still says which slot; these say who may use it.
    static constexpr uint32_t kNoFamily = 0xFFFFFFFFu;
    uint32_t familyLo = kNoFamily;
    uint32_t familySpan = 0;
    // MethodCall: the module function this site's key resolves to, on the class
    // lowering believes the receiver has — a GUESS, and named as one, because
    // the receiver's static class is not its runtime shape and a subclass may
    // override the name.
    //
    // What makes the guess sound is that the backend does not spend it on the
    // receiver at all. The site's own inline cache has already established, for
    // a matching shape, WHICH function object the key resolves to and what its
    // code pointer is; the direct edge adds one compare of that code pointer
    // against this function's call wrapper. A wrong guess therefore never
    // matches and the site takes the cache's ordinary indirect dispatch — the
    // same failure mode a mispredicted class layout has, a cost and never an
    // answer.
    //
    // What it buys is everything the indirect call blocks: the argument vector
    // is never built, the wrapper's unpack never runs, and the callee is an
    // ordinary internal LLVM function at a call site LLVM may inline — which is
    // how a `setValue -> arraysEqual -> copyArray` chain becomes one region.
    static constexpr uint32_t kNoDirectTarget = 0xFFFFFFFFu;
    uint32_t directTarget = kNoDirectTarget;
    uint32_t envDepth = 0;           // EnvGet/EnvSet: parent hops
    uint32_t envIndex = 0;           // EnvGet/EnvSet: slot within that environment
    bool envImmutable = false;       // EnvGet: slot is an initialized immutable binding
    // A direct `call` to a CLOSURE, and the number of parent links from the
    // record in operand 0 to the one the closure captured.
    //
    // A direct call normally cannot reach a closure at all: `__env` arrives
    // through the dynamic calling convention, and a call site that named the
    // function without supplying one would enter it with garbage. This says the
    // site supplies it — not by loading the closure value, which would put the
    // whole guarded environment read back on the fast path, but by handing over
    // the CALLER's own record and a hop count the scope plan established
    // (lower_scope.cpp, `planStableFunctionSlots`). Operand 0 is that record,
    // and it occupies the callee's `__env` parameter.
    //
    // `kNoEnvHops` — the default — means the site supplies no environment, and
    // the verifier then holds the original rule: a direct call names a function
    // that needs none.
    static constexpr uint32_t kNoEnvHops = 0xFFFFFFFFu;
    uint32_t callEnvHops = kNoEnvHops;

    BlockTarget target;              // Jump target / Branch then-target
    BlockTarget elseTarget;          // Branch else-target
};

// Whether this instruction can leave an exception pending, and so needs the
// backend's cell test after it. Defined in print.cpp, beside `isTerminator`,
// because both are one-line facts about the op table.
bool canThrow(const Instruction& inst);

// Whether running this instruction can move a heap object: it allocates, or it
// reaches user code that might. STRICTLY WIDER than `canThrow` — an allocation
// that fails is fatal rather than catchable, so `create.object` throws nothing
// and collects anyway — and the two are not interchangeable at a use site.
//
// It exists for the backend's receiver proof (llvm_recv_proof.h), which holds a
// pointer DERIVED from a heap object across several instructions. A derived
// pointer is not a GC root: nothing forwards it, so it is valid exactly as far
// as the next collection, and this predicate is where that distance is
// measured.
//
// The default is TRUE. Only ops enumerated as neither allocating nor reaching
// user code answer false, so an op added later is a missed optimisation rather
// than a dangling pointer.
bool canCollect(const Instruction& inst);

// Which copy of a guarded numeric region a block belongs to
// (src/lower/guard_region.h). The pass duplicates a region into a FAST copy
// whose numbers are carried as `f64` and leaves the original blocks as the SLOW
// copy, entered only through one-way trampolines. The two are MUTUALLY
// EXCLUSIVE: control enters the fast copy at most once per region entry and,
// once it has left through a trampoline, never returns.
//
// It is a fact about the block and not about the function, so it lives here: it
// travels with the block through every copy, move and renumbering the pass and
// the pruner perform, which a parallel vector on `Function` would have to be
// kept in step with by hand at each of them.
//
// What reads it is `planFrame` (codegen-llvm/llvm_frame.h): two values that can
// never both be live may share one GC root slot, and the two copies of one
// region are exactly that. What makes that a proof rather than a hope is the
// verifier, which rejects a value defined in one copy and used in the other.
enum class CopyClass : uint8_t {
    Shared,  // outside every region — including a region's preheader and exits
    Fast,    // the fast copy proper, its guard chains and its trampolines
    Slow,    // an original region block, or the tail of one split for a guard
};

// `Block::copyRegion` when the block belongs to no region's copy.
inline constexpr uint32_t kNoCopyRegion = UINT32_MAX;

struct Block {
    BlockId id = 0;
    std::vector<BlockParam> params;
    std::vector<Instruction> instructions;
    // Which copy this block is, and of WHICH region: one function can hold
    // several disjoint fast/slow pairs, and only the two copies of the SAME
    // region are known to be mutually exclusive. `Shared` always carries
    // `kNoCopyRegion`; the two together are the block's copy identity.
    CopyClass copyClass = CopyClass::Shared;
    uint32_t copyRegion = kNoCopyRegion;
    // Where control goes if an exception becomes pending inside this block: the
    // innermost enclosing handler, or `kNoBlock` for "leave the function". It
    // sits on the block rather than on each call so that lowering emits no test
    // at all — the backend derives them from `canThrow`, and the block is never
    // split in the IL, so no join here grows a parameter.
    //
    // A handler block therefore takes NO parameters: it is entered from an
    // arbitrary point in the protected region, and nothing here knows what a
    // binding held there, which is what an environment record makes possible.
    BlockId handler = kNoBlock;
};

struct Param {
    std::string name;
    Type type;
    // This slot is `F64` because a `--pins` SIGNATURE entry said so, and no
    // proof agrees (src/types/pins.h, stage B1). It is the licence for the
    // barrier the boxed wrapper and the enumerated call sites emit, and it is
    // deliberately NOT "the type is F64": a parameter typed by
    // `applyProvenSignature` or by the closure parameter proof has a proof
    // behind it, and re-checking a proof would tax the programs that need no
    // barrier at all. The pin index names the manifest line for the message.
    bool pinned = false;
    // The key-constant index of the manifest line this pin came from, valid
    // only when `pinned`.
    uint32_t pinKeyIndex = 0;
};

struct Function {
    std::string name;
    // Synthetic leading parameters come first, in this order: [__env if
    // needsEnv] [__this if needsThis] [__arguments if needsArguments] source
    // parameters... __env can only come from the closure, so a needsEnv
    // function is never a direct-call target; __this is supplied by every
    // caller, including direct ones, so needsThis carries no such restriction.
    std::vector<Param> params;
    Type returnType = Type::Void;
    // `returnType` is `F64` because a `--pins` `return <owner>: number` entry
    // said so, with no proof agreeing — the return half of `Param::pinned`,
    // and the licence for the barrier `lowerReturnStmt` puts on a returned
    // value it cannot type. `pinKeyIndex` names the manifest line.
    bool returnPinned = false;
    uint32_t returnPinKeyIndex = 0;
    bool isExported = false;
    // A DECLARATION rather than a definition: no blocks, and the name is a
    // symbol the backend resolves — a runtime helper the lowering calls by
    // name (the native argument helpers), or a native import thunk
    // (`__bronze_native_<i>`, see Module::nativeImports). Skipped by the
    // verifier's body checks and printed as an `extern` line.
    bool isExternal = false;
    // The module's entry point. Everything else about a function is the same,
    // and one thing is not: it has no caller to propagate an exception to, so
    // its unwind path reports the value and exits instead of returning. A flag
    // rather than a name comparison in the backend, because "which function is
    // `main`" is a fact lowering knows and codegen should not have to spell.
    bool isEntryPoint = false;
    bool needsEnv = false;
    bool needsThis = false;
    // `arguments`: every argument the caller really passed, as one array. Like
    // the rest array it can only be built where the true argument count is
    // visible, which is the call WRAPPER — so a function that needs one is
    // never a direct-call target, the same restriction `needsEnv` carries and
    // for a related reason.
    bool needsArguments = false;
    bool isStrict = false;
    bool isGenerator = false;
    // The BODY of a generator or async function, lowered as a resume machine:
    // `<name>.resume(__env, __mode, __sent)`, whose entry block dispatches on a
    // resume index held in the frame (src/lower/lower_generator.cpp).
    //
    // It is a fact lowering knows and nothing downstream can read off the
    // shape: the dispatch is an ordinary compare-and-branch chain, its targets
    // are ordinary blocks, and a function that happens to start with a switch
    // over a number is indistinguishable from it. The guarded-region pass is
    // what asks — a whole-function duplication of a resume machine copies a
    // state machine whose live values cross suspensions in the FRAME rather
    // than in SSA, so the promotion has nothing to carry and the copy is pure
    // growth.
    bool isResumeBody = false;
    // The BRONZE_ABI_FN_FLAG_* byte the created function object carries: what
    // its syntax decided about [[Construct]] and about the `prototype`
    // property (src/abi/bronze_abi.h says why neither is derivable here).
    // `isGenerator` above is a different question — it selects the frame plus
    // resume machine this body is lowered INTO — and the two are set from the
    // same AST node without either being read off the other.
    uint32_t fnFlags = BRONZE_ABI_FN_FLAGS_ORDINARY;
    // `...rest`: the LAST source parameter, and the one no caller supplies a
    // value for. It arrives as an array built from whatever arguments were left
    // over — by the call wrapper on the uniform path, by the call site on a
    // direct one.
    bool hasRestParam = false;
    // The parameters before the first one with a default. NOT a minimum a call
    // has to meet — the language has no arity error, and a call short of this
    // is filled with `undefined` at the call site exactly as one short of the
    // fixed parameter count is, with the callee's prologue deciding what that
    // means.
    //
    // It is also, exactly, ECMA-262 15.1.5 ExpectedArgumentCount — so it is
    // what 10.2.10 SetFunctionLength makes the function's `length` property,
    // and the backend passes it through as such. Two facts, one count, and
    // deliberately not two fields: a program that could see them disagree would
    // be seeing a bug.
    uint32_t requiredArgs = 0;
    // 10.2.9 SetFunctionName's answer for this function, as an index into
    // `Module::keyConstants`. It is NOT `name` above: that is the IL's own
    // identifier, synthesized as `__anon_fn_N` for a function the source did
    // not name, while this is the string the language says `f.name` is — "" for
    // a genuinely anonymous function, the binding's name under NamedEvaluation
    // (8.6.2), and "get x" / "set x" for an accessor.
    //
    // `BRONZE_ABI_FN_NAME_NONE` means the name was never recorded, which a
    // program can only reach for a function whose name is computed at runtime
    // (`class C { [k]() {} }`); reading `.name` off one is then a diagnosed
    // refusal rather than a wrong answer.
    uint32_t nameKeyIndex = 0xFFFFFFFFu;
    // The bytes of source this function was written from, as a half-open range
    // into `Module::sourceTexts[sourceFile]`. It is what 20.2.3.5
    // Function.prototype.toString returns, which the spec makes the SOURCE
    // TEXT verbatim and not a reconstruction — so nothing downstream may
    // re-render it, and the range has to be exact: `sourceEnd` is the end of
    // the function's last token, never the start of the next one.
    //
    // For a class it is the whole `class C { ... }`, because the constructor
    // IS the class value and `C.toString()` is the class's text (15.7.14 makes
    // the class's source text the constructor's [[SourceText]]).
    //
    // `sourceEnd == 0` means no text was recorded — a function bronze
    // synthesized, or a build that passed `--no-fn-source`.
    uint16_t sourceFile = 0;
    uint32_t sourceBegin = 0;
    uint32_t sourceEnd = 0;
    std::string displayName;
    uint32_t descFlags = 0;

    // Index of the first source-level parameter.
    size_t firstSourceParam() const {
        return static_cast<size_t>(needsEnv) + static_cast<size_t>(needsThis) +
               static_cast<size_t>(needsArguments);
    }
    // How many arguments a CALLER passes: every source parameter but the
    // rest one, which is not a value the convention carries.
    size_t callerParamCount() const {
        return params.size() - firstSourceParam() - static_cast<size_t>(hasRestParam);
    }
    // The arity a short call is PADDED to with `undefined`. Zero means "do not
    // pad", which is what a function owning an `arguments` object needs: the
    // object is built from the argument count the wrapper sees, and padding
    // would make `f(1)` indistinguishable from `f(1, undefined)` where the
    // language says `arguments.length` is 1 and 2. Its wrapper reads argv
    // through `bronze_arg_at` instead.
    uint32_t adaptArity() const {
        return needsArguments ? 0u : static_cast<uint32_t>(callerParamCount());
    }
    std::vector<Block> blocks;
    uint32_t valueCount = 0;  // number of ValueIds in use (params first)
};

struct Module {
    std::string name;
    std::vector<std::string> keyConstants;
    // How many inline-cache sites lowering handed out across the whole module.
    // The backend emits exactly this many entries as a global array in the
    // object file, so it is the size of a real allocation, not a hint: the
    // verifier checks every icIndex against it.
    uint32_t icSiteCount = 0;
    // How many TAGGED TEMPLATE sites lowering handed out. Sized the same way
    // `icSiteCount` is and for the same reason: the backend emits exactly this
    // many Value cells as a global array, and each site's cell holds the one
    // template object 13.2.8.4 GetTemplateObject requires that site to hand its
    // tag function on every call.
    uint32_t templateSiteCount = 0;
    // How many STATIC-SLOT sites lowering handed out. Sized exactly like
    // `icSiteCount`: the backend emits this many i64 cells as a global array,
    // and the verifier checks every `staticCellIndex` against it. Zero when the
    // seam is off or nothing proved, and then no array is emitted at all.
    uint32_t staticSiteCount = 0;
    // The proven class layouts, in the PREORDER the family ids number: what the
    // module hands `bronze_register_class_family` at init so that the runtime
    // can recognise a shape as an instance of one. Empty when nothing proved,
    // or when the family seam is off.
    struct ClassFamilyField {
        uint32_t keyIndex = 0;   // into `keyConstants`
        bool writable = true;    // as the construction sequence installs it
    };
    struct ClassFamilyEntry {
        std::string name;  // the class's binding, for the IL dump only
        std::vector<ClassFamilyField> fields;
    };
    std::vector<ClassFamilyEntry> classFamilies;
    // THE SLOT-REPRESENTATION ELIGIBILITY LIST (stage R1,
    // src/runtime/slot_repr.h): indices into `keyConstants` naming the property
    // names a `--pins` manifest declared `number` on some class this
    // compilation proved a layout for. The runtime is told them at module init
    // and uses them for one decision — whether a shape transition that FIRST
    // installs such a name, with a Number in hand, may store the slot as a raw
    // double instead of a boxed Value.
    //
    // NAMES and not (class, slot) pairs: the runtime meets a transition long
    // before it can recognise a class, and the name is the only fact available
    // at the moment the storage decision has to be made. Over-application is
    // sound — the runtime generalizes a slot whose promise a store breaks — so
    // a name shared with an unpinned class costs a shape split at worst.
    // Empty without a manifest, and empty under BRONZE_NO_SLOT_REPR=1.
    std::vector<uint32_t> slotReprFields;
    // THE PIN CENSUS SITE TABLE (`--census`, src/runtime/pin_census.h). One
    // entry per site lowering created, handed to the runtime at module init so
    // that a site the run never reaches is still known — "never observed" and
    // "not a site" are different answers, and a STATIC refusal has to
    // disqualify its entry on a run that never touches it.
    //
    // Built here rather than scanned back out of the instructions, because
    // some entries have no instruction at all: an owner spelling that would
    // govern two different IL functions is refused by a table row and nothing
    // else.
    struct CensusSiteEntry {
        uint32_t keyIndex = 0;  // into `keyConstants`
        uint32_t info = 0;      // BRONZE_ABI_CENSUS_* kind | flags
    };
    std::vector<CensusSiteEntry> censusSites;
    // Where the census run writes its manifest. Empty unless `--census`.
    std::string censusOutPath;
    // THE NATIVE IMPORT TABLE (`--native-manifest`; bronze_abi.h's loadable-
    // module section): one entry per native the program calls directly, in
    // the order lowering first met them. Entry `i` is reached through the
    // external IL function `__bronze_native_<i>`, whose parameter and return
    // types are the native's C signature in IL terms (a typed array is two
    // parameters, pointer then length; a handle or pointer is `Dynamic`, the
    // 64-bit word). The backend emits the table as `<entry>_native_imports`
    // and one thunk per entry that loads slot `i` and calls through it; a
    // `class <path>` entry's thunk takes nothing and returns the slot itself,
    // which is the class tag the argument helpers compare against.
    struct NativeImport {
        std::string name;       // "<kind> <path>", or "class <path>"
        std::string signature;  // canonical text; empty for a class slot
        uint32_t functionIndex = 0;  // the `__bronze_native_<i>` declaration
        // A native answering `T[]`: the ElementKind of T (the ABI's
        // numbering, abi/bronze_native_type.h), and the thunk passes a
        // descriptor slot as the C function's trailing argument and wraps
        // it after the call. UINT32_MAX for every other return type.
        uint32_t bufferReturnKind = UINT32_MAX;
    };
    std::vector<NativeImport> nativeImports;
    // A deque, not a vector: lowering a function body can append nested
    // closures, and the body being lowered is itself an element. Only a
    // reference-stable container lets a recursive call read its own
    // (still-being-inferred) signature without dangling on a reallocation.
    std::deque<Function> functions;
    // Every source file of the program, indexed by `Function::sourceFile`, so
    // that a function's `sourceBegin`/`sourceEnd` name real bytes. ONE COPY of
    // each file rather than a string per function: three.js's 28 files are
    // 1.6 MB together, and the ~3000 functions in them overlap almost
    // completely, so per-function strings would be that figure many times over.
    //
    // Empty when the build asked for no function source, which is the whole of
    // what `--no-fn-source` does — the ranges above stay, and address nothing.
    std::vector<std::string> sourceTexts;
    std::vector<std::string> sourceFiles;
};

}  // namespace bronze::il
