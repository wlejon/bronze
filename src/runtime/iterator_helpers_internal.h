#pragma once

#include <cstdint>
#include <string>


#include "runtime/gc.h"
#include "runtime/value.h"

// The pieces the two iterator-helper translation units share.
//
// ECMA-262 27.1.4 splits along a seam bronze splits on too: a helper that
// returns an ITERATOR (`map`, `filter`, `take`, `drop`, `flatMap`) is lazy and
// carries state, and a helper that returns a VALUE (`reduce`, `toArray`,
// `forEach`, `some`, `every`, `find`) drives the iteration to its end right
// here. The first family is iterator_helpers.cpp, the second
// iterator_terminal.cpp, and what they have in common is exactly this file:
// the three operations of the iterator protocol as the helpers must perform it.
//
// Every one of them performs the protocol GENERICALLY — `next` read off the
// receiver, called, its result's `done` and `value` read as properties. That is
// not a slow path taken for want of a fast one: 27.1.4.1 defines each helper
// over an arbitrary object with a `next`, so a hand-written iterator, a
// generator and a Map's iterator all have to work, and the fast cursor walk
// `for-of` takes cannot be reached from here at all.

namespace bronze::runtime::iterator_helpers {

bool isCallable(Value v);

// 7.4.1 GetIteratorDirect(obj): the `next` method, read ONCE, off the receiver
// itself — there is no @@iterator call here, because the receiver already IS
// the iterator. False with a TypeError pending when the receiver is not an
// object, which is step 2 of every helper.
bool getIteratorDirect(Rooted<Value>& obj, const char* member, Rooted<Value>& nextOut);

// 7.4.2 GetIteratorFlattenable, which is what `Iterator.from` and `flatMap`
// accept as "something iterable enough". A value with an @@iterator is opened
// through it; a value WITHOUT one is taken to be an iterator already, which is
// the whole point of the operation — `Iterator.from({next(){...}})` works on a
// bare protocol object that no `for-of` would accept.
//
// `allowStringPrimitive` is the spec's primitiveHandling parameter:
// `Iterator.from` iterates a string primitive, `flatMap` refuses every
// primitive by TypeError (a string yielded by the mapper is a mistake far more
// often than an intention to iterate its characters).
bool getIteratorFlattenable(Rooted<Value>& value, bool allowStringPrimitive, const char* member,
                            Rooted<Value>& iterOut, Rooted<Value>& nextOut);

// One step of the protocol, as 7.4.8 IteratorStepValue folds it: call `next`,
// check the result is an object, read `done`, and read `value` only when it is
// not. The three outcomes are distinct because a helper does something
// different in each.
enum class Step { Produced, Done, Threw };
Step stepIterator(Rooted<Value>& iter, Rooted<Value>& next, Rooted<Value>& out);

// 7.4.11 IteratorClose. `suppress` discards an error the `return` method
// raises, which is step 6's rule for a close that is already carrying a throw.
void closeIterator(Rooted<Value>& iter, bool suppress);

// IfAbruptCloseIterator (7.4.12) as one operation: the pending exception is
// taken, the iterator closed, and the ORIGINAL exception re-thrown. Every
// callback a helper runs is wrapped in this — a mapper that throws must still
// leave the underlying iterator closed, and its own error must be the one the
// program catches rather than whatever `return` did afterwards.
void closeAfterThrow(Rooted<Value>& iter);

// Close the iterator and throw `message` as a TypeError or a RangeError. The
// argument-validation failures of 27.1.4.1 are spelled this way — `IteratorClose(O,
// error)` — so a bad limit or a non-callable predicate still leaves the
// underlying iterator closed rather than suspended for ever.
void closeAndThrowTypeError(Rooted<Value>& iter, const std::string& message);
void closeAndThrowRangeError(Rooted<Value>& iter, const std::string& message);

// 7.4.13 CreateIterResultObject, in the field order the spec writes it.
Value iterResult(Rooted<Value>& value, bool done);

// The terminal operations (iterator_terminal.cpp), named here so that the ONE
// table of what %Iterator.prototype% carries can live beside the lazy helpers
// in iterator_helpers.cpp. The same arrangement `Array.prototype`'s table uses
// for the members whose bodies live in another file.
uint64_t iteratorReduce(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t iteratorToArray(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t iteratorForEach(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t iteratorSome(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t iteratorEvery(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);
uint64_t iteratorFind(uint64_t env, uint64_t thisBits, uint32_t argc, const uint64_t* argv);

}  // namespace bronze::runtime::iterator_helpers
