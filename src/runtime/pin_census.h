#pragma once

// THE PIN CENSUS (`bronze build --census <out.pins>`, stage C1).
//
// A `--pins` manifest is a promise about a program (src/types/pins.h). Until
// this stage somebody had to WRITE that promise by hand, against the source,
// class by class. The census is the tool that writes it: an instrumented build
// records what the program's unproven slots, parameters, returns and fields
// actually hold on a representative run, and at exit prints a manifest in the
// grammar `--pins` reads.
//
// It is an OFFLINE step and never a JIT. Two compiles, one artefact:
//
//     bronze build app.js -o census.exe --census app.pins
//     ./census.exe                       # a representative run
//     bronze build app.js -o app.exe --pins app.pins
//
// WHAT IT MAY SAY, and it is exactly the enforcement story stage B1 shipped.
// A violated pin is a catchable TypeError naming the manifest line, so a wrong
// inference on a cold path is a DIAGNOSTIC and not a pointer read as a double.
// That is what makes an inferred entry shippable at all: the census only has
// to be right about the hot path, not right in the "no false positives, ever"
// sense a proof would need.
//
// The one place that is NOT true is B1's own negative: a store through a
// receiver inference types `dynamic` carries no barrier, while a class-known
// read elsewhere still spends the claim. The census sees those stores — they
// are sites like any other — and marks every entry they could reach with
// `@observed`, which `PinManifest::parse` REFUSES unless the build opts in
// with `--pins-allow-observed`. An entry with no marker is one whose every
// store the compiler can hold to the promise.
//
// The recording is a fixed-size TAG SUMMARY per site, never a trace: how many
// numbers, how many nullish, how many strings, how many dense all-number
// arrays. Sites that name the same manifest entry — every call site of one
// escaped closure, every store to one field — JOIN, and the join is the whole
// analysis: all number is a pin, one polymorphic site is no pin.

#include <cstdint>

namespace bronze::runtime {

// Registers the sites of a compiled module, whether or not they ever execute,
// and arms the atexit writer. `outPath` is the manifest to write.
//
// The site table exists so that "never observed" and "not a site" are
// different answers, and so that a STATIC refusal — a return whose body can
// fall off its end, an owner spelling that would govern two different
// functions — disqualifies its entry even on a run that never reaches it.
// `table` is `count` pairs of (module key index, site info); `keyMap` turns a
// module key index into the process-wide id `rtKeyString` speaks, exactly as
// the class-family registration does.
void censusRegister(const char* outPath, const uint32_t* table, uint32_t count,
                    const uint32_t* keyMap);

// One observation: the value that reached the site named by the interned key
// `keyId`. `siteInfo` is the same packing the table carries.
void censusRecord(uint32_t keyId, uint32_t siteInfo, uint64_t valueBits);

// Writes the manifest now. Called from `atexit`; exposed for the tests, which
// cannot wait for a process to end.
void censusWriteManifest();

}  // namespace bronze::runtime
