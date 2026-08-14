#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

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

enum class Op : uint8_t {
    ConstF64,   // a = const.f64 <imm>
    ConstI32,   // a = const.i32 <imm>
    ConstBool,  // a = const.bool <imm>
    ConstUndefined, // a: dynamic = const.undefined
    ConstNull,      // a: dynamic = const.null
    Add,        // a = add b, c        (numeric, operands same type)
    Sub,
    // Unary minus, which is NOT `0 - x`: IEEE-754 says 0 - 0 is +0,
    // while negation flips the sign bit, so `-0` was printing as 0.
    Neg,        // a: f64 = neg b
    Mul,
    Div,
    Mod,
    // JS `**`, which is NOT C's pow: ECMA-262 Number::exponentiate makes an
    // exponent of NaN yield NaN even for base 1, and a base of magnitude 1
    // with an infinite exponent yield NaN. Its own op so the one algorithm
    // serves `**` and `Math.pow` alike.
    Pow,        // a: f64 = pow b, c
    // The bitwise family. Operands are i32 — lowering puts a `to.int32` in
    // front of every one of them — and the RESULT is the JS number that int32
    // denotes, so these read `%n: f64 = and %i32, %i32`. Keeping the result an
    // i32 would leak a type inference has no element for into block joins and
    // calling conventions; the int32 is an intermediate of the operator and
    // never escapes it.
    ToInt32,    // a: i32 = to.int32 b        (b: f64, bool or dynamic)
    BitAnd,     // a: f64 = and b, c
    BitOr,
    BitXor,
    Shl,        // a: f64 = shl b, c          (count masked to 5 bits)
    Shr,        // arithmetic: the sign bit is replicated
    UShr,       // logical, and the ONE bitwise op whose result is ToUint32
    CmpLt,      // a: bool = cmp.lt b, c
    CmpGt,
    // The ORDERED `<=` and `>=` on numbers, which are not `!(a > b)` and
    // `!(a < b)`: that identity needs a total order and NaN does not give one.
    // ECMA-262 13.10 answers false when IsLessThan produces *undefined* —
    // 13.10.1 step 4.c, either operand NaN — and a negation maps that same
    // undefined to true, so `NaN <= 1` came out yes. These two answer false for
    // NaN the way cmp.lt and cmp.gt already did.
    CmpLe,      // a: bool = cmp.le b, c
    CmpGe,
    CmpEq,
    // The exact negation of CmpEq, NaN included: `NaN != NaN` is true, so on
    // doubles this is the UNORDERED compare. Numeric truthiness is a
    // different question with the same shape and has its own op below —
    // conflating them printed `NaN !== NaN` as false.
    CmpNe,
    // ToBoolean of a number: neither zero nor NaN, which is the ORDERED
    // "not equal to 0". Named rather than spelled `cmp.ne x, 0` because the
    // two differ exactly at NaN and every use of one is a wrong answer for
    // the other.
    NumTruthy,  // a: bool = num.truthy b     (b: f64)
    StrictEq,   // a: bool = strict.eq b, c   (JS ===, both operands dynamic)
    LooseEq,    // a: bool = loose.eq b, c    (JS ==, both operands dynamic)
    // The four relational operators over BOXED operands: ECMA-262 13.10 and
    // 13.10.1 IsLessThan entire, which no compare instruction implements. Step
    // 3 asks whether both operands are Strings after ToPrimitive and, if they
    // are, compares them by CODE UNIT and converts nothing — so `"2" < "10"` is
    // true where `2 < 10` is false. ToNumeric is step 4, the else-branch, and
    // reaching for it first is how two strings came to be compared as NaNs.
    //
    // Four named ops rather than one carrying an operator field, for the reason
    // `print.err` is not a flag on `print`: the canonical dump is what a reader
    // bisects with, and which comparison this is, is exactly the kind of fact a
    // field can silently omit.
    RelLt,      // a: bool = rel.lt b, c      (both operands dynamic)
    RelGt,
    RelLe,
    RelGe,
    TypeOf,     // a: dynamic = typeof b      (one of six strings)
    // ECMA-262 7.1.17 ToString, step 1 included: an OBJECT operand is
    // ToPrimitive'd with hint STRING first, so `toString` is tried before
    // `valueOf`. It is deliberately not spelled `"" + b`: 13.15.3 asks
    // ToPrimitive for NO hint, which reverses that pair — so `+` and this
    // give different answers for an object defining both, and a template
    // substitution is ToString (13.2.8.6) rather than a concatenation.
    ToStr,      // a: dynamic = to.string b
    InstanceOf, // a: bool = instanceof b, c
    In,         // a: bool = in b, c          (b: key, c: object)
    IsNullish,  // a: bool = is.nullish b
    Ret,        // ret [a]
    // `throw v`: stores v into the pending-exception cell and goes to this
    // block's handler. A terminator, because it is a way OUT of the block like
    // a jump — the edge it takes is just written on the block rather than on
    // the instruction.
    Throw,      // throw a
    // The pending value, taken and cleared. The first instruction of every
    // handler block, and the only way to read the cell: clearing it here is
    // what lets a `finally` run its body with nothing pending and then decide
    // whether to re-raise.
    ExcTake,    // a: dynamic = exc.take
    Jump,       // jump bN(args...)
    Branch,     // br %cond, bThen(args...), bElse(args...)
    Call,       // a = call <funcRef>(args...)
    Box,        // a = box.<type> b
    Unbox,      // a = unbox.<type> b
    PropGet,    // a = prop.get b, <key_const_index>, <ic_site_index>
    // `super.k`: a read of the PARENT prototype's property with `this` as the
    // receiver. Identical to prop.get for a method — the value is the same
    // function either way — and not identical at all for an accessor, whose
    // getter would otherwise run with the prototype as its receiver. No inline
    // cache: the receiver and the holder are different objects, and an entry
    // describes one shape.
    SuperGet,   // a = super.get proto, <key_const_index>, thisArg
    SuperSet,   // super.set proto, <key_const_index>, thisArg, val
    // `immI32` is 1 when the reference this write goes through is STRICT
    // (ECMA-262 11.2.2 decides which code is; 13.15.2 PutValue step 6.d is what
    // reads it). It is the whole difference between a refused Set — a
    // getter-only property, a non-writable one, a non-extensible receiver —
    // raising a TypeError and being discarded, so it is carried per
    // INSTRUCTION: a module holds strict and sloppy bodies at once, and both
    // reach the same helper.
    PropSet,    // prop.set b, <key_const_index>, c, <ic_site_index>, <strict>
    ElemGet,    // a = elem.get obj, idx        (both dynamic; computed index)
    ElemSet,    // elem.set obj, idx, val, <strict>   (all dynamic)
    DynamicCall,// a = call.dynamic callee, thisArg, argc, argv
    Construct,  // a = new callee, args...
    CreateObject, // a = create.object
    // A GENERATOR OBJECT (ECMA-262 27.5.1): %GeneratorPrototype% for a
    // prototype, and the RESUME FUNCTION its operand names for a body. Its own
    // op and not `create.object` plus two writes, because neither of the two
    // things that make it one is a property: the prototype lives on the shape,
    // and the resume closure is an internal slot.
    CreateGeneratorObject, // a = create.generator_object b
    CreateAsyncGeneratorObject, // a = create.async_generator_object b
    // The three edges of an ASYNC function (ECMA-262 27.7): make the machine
    // the runtime driver holds (operand = the resume closure), start it (run
    // the body synchronously to the first await, 27.7.5.1, and answer the
    // promise), and subscribe one await (machine, awaited value). Their own
    // ops rather than dynamic calls to named globals because — like
    // `create.generator_object` — what they touch are internal slots the
    // program can neither read nor forge.
    CreateAsyncMachine, // a = create.async_machine b     (b = resume closure)
    AsyncStart,         // a = async.start b              (b = machine; a = promise)
    AsyncAwait,         // async.await machine, value     (no result; subscribes)
    DynamicImport,      // a = dynamic_import specifier
    // A MODULE NAMESPACE EXOTIC OBJECT (ECMA-262 10.4.6), built from the object
    // of getters the operand holds. Its own op for the reason
    // `create.generator_object` is one: what it produces is not an object
    // literal with a different attribute set, it is a receiver KIND whose
    // own-key order, [[Set]] and [[GetOwnProperty]] are each its own.
    ModuleNamespace, // a = module.namespace b
    ObjectKeys, // a = object.keys b
    // The keys a `for-in` will visit, as one array built before the first
    // iteration: own AND inherited enumerable string keys, each once.
    // Snapshotting is what lets the loop itself be for-of's index walk over the
    // result, and it is a legal answer to the spec's open question about
    // mutation during enumeration.
    ForInKeys,  // a = forin.keys b
    // A class method: a property write with `enumerable: false`, which an
    // ordinary `prop.set` cannot express and which is what keeps a method out
    // of `Object.keys` and `for-in`. No IC index — a class body runs once.
    MethodDef,  // method.def obj, <key_const_index>, v
    // The same definition with a key that is a VALUE rather than a compile-time
    // constant — `class C { [Symbol.iterator]() {} }`. Its own op and not
    // `elem.set`, because a method is `enumerable: false` (15.7.14) and an
    // assignment cannot say that; and not `method.def`, because there is no
    // key constant to name. The key is whatever the expression evaluated to,
    // which for the one spelling bronze admits is the well-known symbol.
    MethodDefComputed,  // method.def.computed obj, key, v
    AccessorDef,  // accessor.def obj, <key_const_index>, getter, setter, <enumerable>
    AccessorDefComputed, // accessor.def.computed obj, key, getter, setter, <enumerable>
    GetNewTarget, // a = get.new_target
    // `delete o.k` and `delete o[i]`. A reference operation, not a read:
    // the operand's property is never loaded, and the result is the boolean
    // ECMA-262 13.5.1 defines rather than the property's value.
    // `immI32` is the strict flag, on the same rule prop.set carries one:
    // 13.5.1.2 step 5.b turns a delete that answers false into a TypeError,
    // but only for strict code.
    PropDelete,  // a: bool = prop.delete obj, <key_const_index>, <strict>
    ElemDelete,  // a: bool = elem.delete obj, idx, <strict>
    GlobalGet,  // a = global.get <key_const_index>
    // A name lowering could not resolve to anything, EVALUATED. Raises
    // ReferenceError at run time rather than refusing the program at compile
    // time, because what a free name denotes is a fact only the running
    // environment holds. Not a terminator: it is a helper call like any other,
    // and the block's handler is what the backend's exception test after it
    // branches to.
    RefError,   // a: dynamic = ref.error <key_const_index>
    // `class D extends B`: links D.prototype's proto to B.prototype and
    // D's static properties to B's. One op because both links have to
 // be made together, before any method is stored.
    ClassExtend, // class.extend derived, base
    // The iterator protocol. One iteration is a CURSOR — opened once, stepped,
    // read, and closed if it is abandoned — rather than an index and a length,
    // because a Map, a Set and a user-defined iterable have no length to
    // compare against and no index to read at. The array/string/typed-array
    // walk survives as a kind INSIDE the record, so the common case still costs
    // no allocation and no call into user code.
    IterOpen,    // a: dynamic = iter.open b        (GetIterator, 7.4.2)
    AsyncIterOpen, // a: dynamic = async_iter.open b
    AsyncIterNext, // a: dynamic = async_iter.next %record
    AsyncIterClose, // async_iter.close %record, <suppress>
    // Advances the cursor, stashes what it produced in the record, and
    // answers whether there was anything. Two ops rather than one because
    // an SSA instruction has one result and a step has two answers.
    IterStep,    // a: bool = iter.step %record     (IteratorStep, 7.4.6)
    IterValue,   // a: dynamic = iter.value %record
    // IteratorClose (7.4.9): the iterator's `return` method, for a for-of
    // left by `break`, `return` or `throw`. `immI32` is 1 when a throw is
    // already in flight, which is when 7.4.9 step 6 discards an error the
    // `return` method raises rather than letting it replace the original.
    IterClose,   // iter.close %record, <suppress>
    // Everything the cursor has left, as a fresh array — a rest element's
    // value. Drains the same record the elements before it were stepped from.
    IterRest,    // a = iter.rest %record
    // One RESUMPTION forwarded to a delegated iterator: ECMA-262 27.5.3.7
    // steps 5.a, 5.b and 5.c, without the loop around them. The loop is
    // compiled code — it has a suspension in it, and a suspension is a return
    // from the resume function — so what is left for one instruction is the
    // part that is not control flow: WHICH method of the inner iterator this
    // resumption calls, and what happens when it has none.
    //
    // `b` is the record, `c` the resumption kind as a number (the runtime's
    // GeneratorResumeMode), and `d` the value it carried. The result is the
    // inner iterator's RESULT OBJECT — forwarded by identity, which is what
    // 27.5.3.8 GeneratorYield does with it — or `undefined` for the one case
    // that produces no object at all: a `return` resumption to an iterator
    // with no `return` method, which 5.c.iii passes straight through.
    IterDelegate,  // a: dynamic = iter.delegate %record, %mode, %sent
    // The source of a destructuring, checked once before any element is read.
    // `immI32` names which pattern asked, so the diagnostic can say `array
    // destructuring` rather than `for-of`, and the check is what lets every
    // element read below it assume a walkable value.
    PatternCheck,  // a = pattern.check b, <kind>
    // The three container-building ops a spread needs: one element, then all of
    // an iterable's, then all of an object's own enumerable properties.
    // Appending rather than indexing is the point — a literal with a spread in
    // it has no length until it is built.
    ArrayAppend,  // array.append arr, v
    ArrayAppendHole, // array.append.hole arr
    ArraySpread,  // array.spread arr, iterable
    ObjectSpread, // object.spread obj, source
    // `{ a, ...others }`: a fresh object of `source`'s own enumerable
    // properties minus the ones the pattern already named, which arrive as
    // an array of keys because a computed key is not known until it runs.
    ObjectRest,   // a = object.rest source, excludedKeys
    // A call whose argument list contains a spread, so its length is a
    // runtime fact: the arguments are built into an array and the callee is
    // entered through the uniform convention over it.
    DynamicCallSpread,  // a = call.dynamic.spread callee, thisArg, args
    SuperCall,          // a = call.super base, thisArg, args...
    SuperCallSpread,    // a = call.super.spread base, thisArg, args
    TemplateObject,     // a = template.object cookedArray, rawArray
    ConstructSpread,    // a = new.spread callee, args
    CreateArray,  // a = create.array <length>
    CreateFunction,// a = create.func <funcIndex>, env
    FunctionRef,   // a = func.ref <funcIndex>
    EnvCreate,  // a = env.create parent, <slots>
    EnvGet,     // a = env.get env, <depth>, <index>
    EnvSet,     // env.set env, <depth>, <index>, v
    // The temporal dead zone, in two instructions. A scope entry writes the
    // uninitialized-binding marker into the slot of every `let`, `const` and
    // `class` it declares, and every READ of such a slot goes through the
    // checked form, which is 9.1.1.1.6 GetBindingValue's ReferenceError.
    //
    // Two ops rather than a flag on env.get/env.set, for the reason `print.err`
    // is not a flag on `print`: the canonical dump is what a reader bisects
    // with, and whether a read can throw is exactly the kind of fact a field
    // can silently omit. `env.get.tdz` also carries the NAME, because which
    // binding was read too early is the whole content of the diagnostic.
    //
    // A `var`, a parameter, a hoisted `function` and the synthetic `this` and
    // `arguments` slots never take these: none of them is ever uninitialized,
    // and giving them a check would be a dead zone the language does not have.
    EnvInitTdz, // env.init.tdz env, <depth>, <index>
    EnvGetTdz,  // a = env.get.tdz env, <depth>, <index>, <key_const_index>
    // The MODULE scope's environment record, which is a singleton: the top
    // level runs exactly once, so there is exactly one activation of that scope
    // and no reason to thread it through every calling convention. `main`
    // publishes it; a module function that needs a module-level binding loads
    // it at entry and chains its own record to it.
    ModuleEnvSet,  // module.env.set %env
    ModuleEnvGet,  // a: dynamic = module.env.get
    Print,      // print a, ...            (console.log / info / debug)
    // Same formatter, other stream. Its own op rather than a flag on `Print`
    // because the canonical dump is what a reader bisects with, and a stream
    // carried in a field is a stream the dump can silently omit.
    PrintErr,   // print.err a, ...        (console.warn / error)
    PrintSpread, // print.spread arr
    PrintSpreadErr, // print.spread.err arr
};
const char* opName(Op op);
bool isTerminator(Op op);

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
    Type boxType = Type::Void;       // Box: input type being boxed (F64, I32, Bool, Str)
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
    uint32_t envDepth = 0;           // EnvGet/EnvSet: parent hops
    uint32_t envIndex = 0;           // EnvGet/EnvSet: slot within that environment

    BlockTarget target;              // Jump target / Branch then-target
    BlockTarget elseTarget;          // Branch else-target
};

// Whether this instruction can leave an exception pending, and so needs the
// backend's cell test after it. Defined in print.cpp, beside `isTerminator`,
// because both are one-line facts about the op table.
bool canThrow(const Instruction& inst);

struct Block {
    BlockId id = 0;
    std::vector<BlockParam> params;
    std::vector<Instruction> instructions;
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
    bool isExported = false;
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
    // A deque, not a vector: lowering a function body can append nested
    // closures, and the body being lowered is itself an element. Only a
    // reference-stable container lets a recursive call read its own
    // (still-being-inferred) signature without dangling on a reallocation.
    std::deque<Function> functions;
};

}  // namespace bronze::il
