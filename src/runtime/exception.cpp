// The pending-exception cell, the raise helpers, and the `Error` family. The
// cell is an ABI global because generated code tests it inline; everything else
// here is C++ the runtime calls on its own behalf.

#include "runtime/exception.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"
#include "runtime/fn.h"
#include "runtime/gc.h"
#include "runtime/object.h"
#include "runtime/rt_internal.h"
#include "runtime/shape.h"
#include "runtime/string.h"
#include "runtime/value.h"

extern "C" {

// Defined here rather than in rt_state.cpp because everything that reads or
// writes it is in this file; its ROOT registration is below, and the
// registration is what the collector needs, not the storage.
uint64_t bronze_exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS;

}  // extern "C"

namespace bronze::runtime {

namespace {

static_assert(Value::fromHole().rawBits() == BRONZE_ABI_NO_EXCEPTION_BITS,
              "BRONZE_ABI_NO_EXCEPTION_BITS in bronze_abi.h has drifted from the Hole singleton");
static_assert(sizeof(Value) == sizeof(uint64_t) && alignof(Value) == alignof(uint64_t),
              "the exception cell is rooted by reinterpreting its address as a Value*");

struct ErrorClass {
    const char* name;
    ErrorKind kind;
    // Each class needs its OWN code pointer, because function objects for
    // native code are interned by code pointer (`bronze_function_singleton`).
    // Sharing one made `Error`, `TypeError` and `RangeError` the same object,
    // so the last class built won every `.prototype` and `new Error("x").name`
    // answered "RangeError".
    bronze_fn_code code;
    Value constructor = Value::fromUndefined();
    Value prototype = Value::fromUndefined();
};

// The two keys `rtErrorText` reads, as the ARENA-INTERNED headers the shapes
// hold. Taken off Error.prototype's shape once, which is the only way to get
// them without calling rtMakeString — and rtErrorText must not allocate:
// console.log's walk holds raw `ArrayHeader*`/`ObjectHeader*` across every
// element it formats, so one allocation inside it moves the container out
// from under the loop (it crashed under BRONZE_GC_STRESS=1 printing an array
// of two errors).
StringHeader* g_nameKey = nullptr;
StringHeader* g_messageKey = nullptr;

uint64_t errorCtorError(uint64_t, uint64_t, uint32_t, const uint64_t*);
uint64_t errorCtorTypeError(uint64_t, uint64_t, uint32_t, const uint64_t*);
uint64_t errorCtorRangeError(uint64_t, uint64_t, uint32_t, const uint64_t*);
uint64_t errorCtorSyntaxError(uint64_t, uint64_t, uint32_t, const uint64_t*);
uint64_t errorCtorReferenceError(uint64_t, uint64_t, uint32_t, const uint64_t*);
uint64_t errorCtorURIError(uint64_t, uint64_t, uint32_t, const uint64_t*);
uint64_t errorCtorAggregateError(uint64_t, uint64_t, uint32_t, const uint64_t*);

// Order matters only in that `Error` is first: the other two chain their
// prototypes to its, so it has to exist before they are built.
ErrorClass g_errorClasses[] = {
    {"Error", ErrorKind::Error, errorCtorError},
    {"TypeError", ErrorKind::TypeError, errorCtorTypeError},
    {"RangeError", ErrorKind::RangeError, errorCtorRangeError},
    {"SyntaxError", ErrorKind::SyntaxError, errorCtorSyntaxError},
    {"ReferenceError", ErrorKind::ReferenceError, errorCtorReferenceError},
    {"URIError", ErrorKind::URIError, errorCtorURIError},
    {"AggregateError", ErrorKind::AggregateError, errorCtorAggregateError},
};

ErrorClass& classFor(ErrorKind kind) {
    for (ErrorClass& cls : g_errorClasses) {
        if (cls.kind == kind) return cls;
    }
    return g_errorClasses[0];
}

// A fresh instance of one class, with no `message` of its own — which is what
// `new Error()` builds, and why the prototype carries the empty string.
Value newErrorInstance(ErrorClass& cls) {
    ObjectHeader* instance = ObjectHeader::create(
        rtHeap(), rtArena(), cls.constructor.asObject<FunctionHeader>()->instance_shape);
    instance->header.flags = HeapKind::Plain;
    return Value::fromObject(instance);
}

// 20.5.1.1 step 4 is CreateNonEnumerableDataPropertyOrThrow: `message` must
// not be enumerable, or `Object.keys(err)` would report it and a `for-in`
// over an error would visit it.
void setMessage(Rooted<Value>& self, Rooted<Value>& message) {
    Rooted<Value> key{rtMakeString("message")};
    self.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), key, message,
                                                 /*ic=*/nullptr, /*enumerable=*/false);
}

// Registered on first use, NOT at static initialization: `rtHeap()` returns a
// namespace-scope object in another translation unit, and registering into it
// from this one's static initializer is the initialization-order fiasco —
// which showed up as the roots being silently dropped when the heap's own
// constructor ran afterwards, and then as a crash under BRONZE_GC_STRESS=1
// with the error prototypes collected out from under the classes.
void ensureExceptionRoots() {
    static const bool registered = [] {
        // A thrown object is live for exactly as long as it is pending, which
        // spans an arbitrary number of frames and every collection inside
        // them. Nothing else roots it — the value has left the throwing
        // frame's root slots by the time any handler sees it.
        rtHeap().add_permanent_root(reinterpret_cast<Value*>(&bronze_exception_cell));
        rtHeap().add_root_source([](const Heap::RootVisitor& visit) {
            for (ErrorClass& c : g_errorClasses) {
                visit(c.constructor);
                visit(c.prototype);
            }
        });
        return true;
    }();
    (void)registered;
}

// The constructor body: `this.message = argv[0]` when an argument was passed,
// and nothing otherwise — 20.5.1.1 leaves `message` off the instance entirely
// for `new Error()`, so the prototype's empty string shows through and
// `Object.keys(new Error())` is `[]`.
//
// 20.5.1.1 opens with "if NewTarget is undefined, let newTarget be the active
// function object", which is the spec saying `Error("x")` and `new Error("x")`
// build the same thing. Reached as a plain call, `this` is not an ordinary
// object, so the instance is made here rather than borrowed.
uint64_t errorCtorImpl(ErrorKind kind, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    Rooted<Value> self{Value(thisBits)};
    if (!self.get().isObject() ||
        self.get().asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
        self.set(newErrorInstance(classFor(kind)));
    }
    if (argc == 0 || Value(argv[0]).isUndefined()) return self.get().rawBits();
    RootedArgs args{argc, argv};
    Rooted<Value> message{rtValueToString(args[0])};
    setMessage(self, message);
    return self.get().rawBits();
}

uint64_t errorCtorError(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return errorCtorImpl(ErrorKind::Error, thisBits, argc, argv);
}
uint64_t errorCtorTypeError(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return errorCtorImpl(ErrorKind::TypeError, thisBits, argc, argv);
}
uint64_t errorCtorRangeError(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return errorCtorImpl(ErrorKind::RangeError, thisBits, argc, argv);
}
uint64_t errorCtorSyntaxError(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return errorCtorImpl(ErrorKind::SyntaxError, thisBits, argc, argv);
}
uint64_t errorCtorReferenceError(uint64_t, uint64_t thisBits, uint32_t argc,
                                 const uint64_t* argv) {
    return errorCtorImpl(ErrorKind::ReferenceError, thisBits, argc, argv);
}
uint64_t errorCtorURIError(uint64_t, uint64_t thisBits, uint32_t argc, const uint64_t* argv) {
    return errorCtorImpl(ErrorKind::URIError, thisBits, argc, argv);
}

// 20.5.7.1.1 AggregateError(errors, message): the family constructor with one
// leading argument more. `message` is argv[1] where the others read argv[0],
// and `errors` — the iterable, materialized to an array — becomes a
// non-enumerable own property (step 5 is CreateNonEnumerableDataProperty), so
// `Object.keys` of one is as empty as any other error's.
uint64_t errorCtorAggregateError(uint64_t, uint64_t thisBits, uint32_t argc,
                                 const uint64_t* argv) {
    Rooted<Value> self{Value(thisBits)};
    if (!self.get().isObject() ||
        self.get().asObject<HeapObjectHeader>()->flags != HeapKind::Plain) {
        self.set(newErrorInstance(classFor(ErrorKind::AggregateError)));
    }
    RootedArgs args{argc, argv};
    if (!args[1].isUndefined()) {
        Rooted<Value> message{rtValueToString(args[1])};
        setMessage(self, message);
    }
    // Step 4: CreateListFromIterable over `errors`. A non-iterable argument
    // is the TypeError rtOpenIterator raises, left pending for the caller.
    Rooted<Value> source{args[0]};
    Rooted<Value> list{Value(bronze_create_array(0))};
    Rooted<Value> rec{Value(bronze_iter_open(source.get().rawBits()))};
    if (rtExceptionPending()) return Value::fromUndefined().rawBits();
    while (bronze_iter_step(rec.get().rawBits())) {
        Rooted<Value> item{Value(bronze_iter_value(rec.get().rawBits()))};
        bronze_array_append(list.get().rawBits(), item.get().rawBits());
        if (rtExceptionPending()) break;
    }
    if (rtExceptionPending()) {
        bronze_iter_close(rec.get().rawBits(), /*suppress=*/true);
        return Value::fromUndefined().rawBits();
    }
    Rooted<Value> errorsKey{rtMakeString("errors")};
    self.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), errorsKey, list,
                                                 /*ic=*/nullptr, /*enumerable=*/false);
    return self.get().rawBits();
}

void ensureErrorClasses() {
    if (g_errorClasses[0].constructor.isObject()) return;
    ensureExceptionRoots();

    for (ErrorClass& cls : g_errorClasses) {
        Rooted<Value> ctor{rtNativeFunction(cls.code, 1)};
        // `Error.prototype` and `TypeError.prototype` must be distinct objects
        // with the second's prototype pointing at the first, which is exactly
        // what `class TypeError extends Error` would build — so it is built the
        // same way, with a root shape naming the parent.
        Value parentProto =
            (&cls == &g_errorClasses[0]) ? Value::fromUndefined() : g_errorClasses[0].prototype;
        Rooted<Value> parent{parentProto};
        ObjectHeader* protoObj =
            ObjectHeader::create(rtHeap(), rtArena(), rtNewRootShape(parent.get()));
        protoObj->header.flags = HeapKind::Plain;
        Rooted<Value> proto{Value::fromObject(protoObj)};

        // 20.5.3.2 and 20.5.3.3: both are data properties ON THE PROTOTYPE,
        // so an instance that was given neither still answers them, and
        // neither appears in `Object.keys` of an instance.
        Rooted<Value> nameKey{rtMakeString("name")};
        Rooted<Value> nameVal{rtMakeString(cls.name)};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), nameKey, nameVal,
                                                     /*ic=*/nullptr, /*enumerable=*/false);
        Rooted<Value> msgVal{rtMakeString("")};
        setMessage(proto, msgVal);

        // 10.2.5 step 6 again, for a class the runtime provides rather than
        // one the program wrote: `e.constructor` is how idiomatic code asks
        // what kind of error it caught, and without the back-pointer
        // `e.constructor.name` threw on undefined. A DEFINITION, because
        // `TypeError.prototype` inherits from `Error.prototype` and must get
        // its own rather than write through to the parent's.
        Rooted<Value> ctorKey{rtMakeString("constructor")};
        proto.get().asObject<ObjectHeader>()->setProp(rtHeap(), rtArena(), ctorKey, ctor,
                                                     /*ic=*/nullptr, /*enumerable=*/false,
                                                     /*defineOwn=*/true);

        FunctionHeader* fn = ctor.get().asObject<FunctionHeader>();
        fn->prototype = proto.get();
        fn->instance_shape = rtNewRootShape(proto.get());
        cls.constructor = ctor.get();
        cls.prototype = proto.get();
    }

    // The two keys the error prototype was just given, recovered as the
    // arena-interned headers the walk above already holds. Both are string
    // keys by construction; the filter says so rather than assuming it, since
    // a shape's own keys are no longer strings by definition.
    for (PropertyKey key :
         g_errorClasses[0].prototype.asObject<ObjectHeader>()->shape->ownKeysInInsertionOrder()) {
        StringHeader* name = key.string();
        if (!name) continue;
        const std::string text = rtUtf8Chars(name);
        if (text == "name") g_nameKey = name;
        if (text == "message") g_messageKey = name;
    }
}

// One data property of an object or of anything on its prototype chain,
// without the accessor path and without allocating. Answers false for an
// accessor, for a dictionary-mode object and for a missing key: every one of
// those is a reason for the caller to print the value some other way rather
// than to invent a rendering.
bool lookupDataProperty(ObjectHeader* obj, StringHeader* key, Value& out) {
    for (uint32_t depth = 0; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        ObjectHeader* cur = depth == 0 ? obj : obj->protoAncestor(depth);
        if (cur == nullptr) return false;
        PropertyInfo info;
        if (cur->shape != nullptr && cur->shape->lookupProperty(key, info)) {
            if (info.accessor) return false;
            out = cur->getSlot(info.slot);
            return true;
        }
    }
    return false;
}

}  // namespace

bool rtExceptionPending() noexcept {
    return bronze_exception_cell != BRONZE_ABI_NO_EXCEPTION_BITS;
}

void rtClearException() noexcept { bronze_exception_cell = BRONZE_ABI_NO_EXCEPTION_BITS; }

Value rtThrow(Value thrown) noexcept {
    // `throw "x"` never touches the Error classes, so this is the one place
    // every raise passes through and therefore where the cell's root has to
    // be established.
    ensureExceptionRoots();
    // A second throw while one is pending would be a runtime that lost track
    // of its own unwind, not a program error: every path that sets the cell
    // returns immediately, and every consumer clears it before running code
    // that could set it again.
    if (rtExceptionPending()) {
        fatal("internal: a second exception raised while one is already pending");
    }
    bronze_exception_cell = thrown.rawBits();
    return Value::fromUndefined();
}

Value rtThrowError(ErrorKind kind, const std::string& message) {
    ensureErrorClasses();
    ErrorClass& cls = classFor(kind);
    Rooted<Value> msg{rtMakeString(message)};
    Rooted<Value> self{newErrorInstance(cls)};
    setMessage(self, msg);
    return rtThrow(self.get());
}

Value rtThrowTypeError(const std::string& message) {
    return rtThrowError(ErrorKind::TypeError, message);
}

Value rtThrowRangeError(const std::string& message) {
    return rtThrowError(ErrorKind::RangeError, message);
}

Value rtThrowSyntaxError(const std::string& message) {
    return rtThrowError(ErrorKind::SyntaxError, message);
}

Value rtThrowReferenceError(const std::string& message) {
    return rtThrowError(ErrorKind::ReferenceError, message);
}

Value rtErrorConstructor(const std::string& name) {
    ensureErrorClasses();
    for (const ErrorClass& cls : g_errorClasses) {
        if (name == cls.name) return cls.constructor;
    }
    return Value::fromUndefined();
}

Value rtNewErrorValue(ErrorKind kind, const std::string& message) {
    ensureErrorClasses();
    Rooted<Value> self{newErrorInstance(classFor(kind))};
    if (!message.empty()) {
        Rooted<Value> msg{rtMakeString(message)};
        setMessage(self, msg);
    }
    return self.get();
}

bool rtIsErrorInstance(Value v) {
    if (!g_errorClasses[0].prototype.isObject()) return false;
    if (!v.isObject() || v.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) return false;
    ObjectHeader* obj = v.asObject<ObjectHeader>();
    const uint64_t errorProtoBits = g_errorClasses[0].prototype.rawBits();
    for (uint32_t depth = 1; depth <= ObjectHeader::kMaxPrototypeDepth; ++depth) {
        ObjectHeader* ancestor = obj->protoAncestor(depth);
        if (!ancestor) return false;
        if (Value::fromObject(ancestor).rawBits() == errorProtoBits) return true;
    }
    return false;
}

bool rtErrorText(Value v, std::string& out) {
    if (g_nameKey == nullptr || g_messageKey == nullptr) return false;
    if (!v.isObject() || v.asObject<HeapObjectHeader>()->flags != HeapKind::Plain) return false;
    ObjectHeader* obj = v.asObject<ObjectHeader>();
    Value nameValue;
    Value messageValue;
    if (!lookupDataProperty(obj, g_nameKey, nameValue)) return false;
    if (!lookupDataProperty(obj, g_messageKey, messageValue)) return false;
    // ToString of either would allocate, and 20.5.3.4's ToString is a
    // question for `Error.prototype.toString`, which bronze does not provide.
    // An error whose `name` or `message` was overwritten with a non-string
    // therefore prints as the object it is.
    if (!nameValue.isString() || !messageValue.isString()) return false;
    const std::string name = rtUtf8Chars(nameValue.asObject<StringHeader>());
    const std::string message = rtUtf8Chars(messageValue.asObject<StringHeader>());
    out = message.empty() ? name : name + ": " + message;
    return true;
}

std::string rtUncaughtText(Value thrown) {
    if (std::string text; rtIsErrorInstance(thrown) && rtErrorText(thrown, text)) {
        return "Uncaught " + text;
    }
    // A non-Error is legal and common (`throw "negative"` is what the oracle
    // case does), so it is reported the way console.log would show it rather
    // than being coerced to a string — `Uncaught 7` and `Uncaught '7'` are
    // different programs.
    return "Uncaught " + rtInspect(thrown);
}

extern "C" {

// An unresolvable reference, EVALUATED. ECMA-262 6.2.5.5 GetValue step 2: a
// Reference Record whose base is unresolvable throws a ReferenceError — at the
// moment of use, which is why lowering emits an instruction here instead of
// refusing the program. The key index is the module's interned name, so the
// message can name the identifier without the backend carrying a string.
//
// Returns `undefined` for the reason every other raise helper does: the value
// lands in a caller's GC root slot before the pending cell is tested, so
// anything the collector cannot parse would put a bad word in a live root.
uint64_t bronze_reference_error(uint32_t keyIndex) {
    return rtThrowReferenceError(rtKeyString(keyIndex) + " is not defined").rawBits();
}

// The end of a program with an exception still pending. Reported on STDERR,
// which is what node does and what keeps an uncaught-throw oracle case
// pinnable: stdout holds exactly what the program printed before it died.
void bronze_uncaught_exception() {
    const std::string text = rtUncaughtText(Value(bronze_exception_cell));
    std::fflush(stdout);
    std::fprintf(stderr, "%s\n", text.c_str());
    std::fflush(stderr);
    disableCrashDialogs();
    std::exit(1);
}

}  // extern "C"

}  // namespace bronze::runtime
