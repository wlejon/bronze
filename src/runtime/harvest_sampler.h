#pragma once

// THE HARVEST'S SAMPLES (docs/type-feedback.md, "Harvest", samples).
//
// A harvest says which sites ran, not how often, and what ran is most of a
// program (a scene's setup runs megabytes of code once; its frame runs tens
// of kilobytes of it, over and over). Function placement needs the second:
// so while a harvest is armed, a sampler thread arms the JS thread's
// interrupt every kSamplePeriodUs (tls_block.h: a store into its stack-limit
// word), and the next interrupt poll the thread reaches — a function's
// prologue check or a loop back edge, both already in every compiled
// function — notes the compiled function it is in. Nothing is added to the
// code: the polls are there anyway, and a poll that finds no interrupt costs
// what it always cost. A sample costs one code-range lookup on the JS
// thread, four thousand times a second: a harvest run of a few seconds
// (a bench scene's) gives some ten thousand samples (at 2 ms it was ~1200,
// and which functions made the hot set's tail changed from run to run). The
// tail still varies between runs; the head, where the time is, does not.
//
// One thread is sampled: the one that armed the harvest. The counts are per
// code range (one compiled function), read by the harvest's writer
// (ic_harvest.cpp), which files each under the module whose range table
// holds it, by the function's descriptor `name_hash`.
//
// What it costs an app that is not running JS: nothing. The sampler thread
// arms an interrupt and sleeps kSamplePeriodUs; if no poll took the sample
// meanwhile, the JS thread is running no JS (waiting for events, blocked in a
// native call, in the background), and the sampler sleeps with no timeout
// until the poll that takes it wakes it. A busy JS thread takes a sample
// with an uncontended lock and no system call.
// It exits for good after kSampleLimit samples (enough for the hot set; a
// harvesting launch samples its first seconds of JS), when the harvest is
// disarmed or written (rtHarvestSamplerStop), and when the sampled thread
// exits. An arm that a stack-limit store would overwrite (a fiber switch, a
// compartment's disarm) is put back by that store (tls_block.cpp), so a
// pending sample is never lost and the sampler never needs to look again.
//
// Portable: the interrupt is the stack-limit word every compiled prologue
// and back edge already compares with, on every target; the thread is a
// std::thread. Only the caller credit at a prologue needs the frame-pointer
// chain, which MSVC builds do not read (BRONZE_CALLER_FP below): there the
// polling function alone is credited.

#include <atomic>
#include <cstdint>

#include "abi/bronze_abi.h"

// The return address of the function this is written in (where a poll's
// call into the runtime came from), and the frame pointer its caller had:
// compiled code keeps the frame-pointer chain (brass x64_frame.cpp: push rbp;
// mov rbp, rsp; aarch64 frame records), and the caller's is the one this
// function's own frame saved. MSVC has neither in a portable form: there
// only the polling function is credited.
#if defined(_MSC_VER)
#include <intrin.h>
#define BRONZE_RETURN_ADDRESS() _ReturnAddress()
#define BRONZE_CALLER_FP() nullptr
#else
#define BRONZE_RETURN_ADDRESS() __builtin_return_address(0)
#define BRONZE_CALLER_FP() (*static_cast<void* const*>(__builtin_frame_address(0)))
#endif

namespace bronze::runtime {

inline constexpr int kSamplePeriodUs = 250;

// Samples taken before the sampler stops for good in this process.
inline constexpr uint64_t kSampleLimit = 10000;

// How many polls in a row a sample may be passed on (rtHarvestSampleAt).
inline constexpr uint32_t kSamplePassLimit = 256;

// The block of the thread a sample is wanted from, while one is (null
// otherwise). A store over the block's stack-limit word re-arms the
// interrupt when it is this block's (tls_block.cpp), after a seq_cst fence
// that pairs with the one the sampler issues between setting this and
// arming: one of the two stores the armed value last.
extern std::atomic<bronze_tls_block*> g_harvestSampleWanted;

// On the thread to be sampled, when a harvest is armed: starts the sampler
// thread unless it runs, or the process already took kSampleLimit samples.
void rtHarvestSamplerStart();

// Stops the sampler thread (it exits; no interrupt is armed for it after
// this returns). A later rtHarvestSamplerStart starts it again.
void rtHarvestSamplerStop();

// From the interrupt polls, when the sampler asked for a sample: `pc` is the
// return address into the compiled code that polled, `fp` the frame pointer
// it polled with (null where frames are not chained by it), `entry` whether
// the poll is a function's prologue check (bronze_stack_overflow) rather
// than a loop's back edge (bronze_interrupt_poll).
//
// What a sample credits: the time since the poll before it went to the
// code between them. A back edge's poll ends a stretch of its own loop, so
// the function polling is credited. A prologue's poll ends a stretch of the
// caller (it ran up to the call), so the caller is credited, and the callee
// too: it was entered, which a function that never polls (no loop, called
// right after another poll) is otherwise never seen to be.
//
// A poll in code with no descriptor (a stub's loop: a sort's merge, which
// calls the comparator) passes the sample on: the sample stays wanted, so
// the poll's disarm arms the interrupt again and the next poll (the
// comparator's prologue) takes it, crediting the function the stub's loop runs rather than none.
// At most kSamplePassLimit polls in a row pass it on (a stub loop calling
// nothing); the next drops it.
void rtHarvestSampleAt(const void* pc, const void* fp, bool entry) noexcept;

// Takes the samples of each range in [ranges, ranges + count): adds them to
// `out` (indexed like the ranges) and forgets them, so taking them again
// counts only what came since. Any thread.
void rtHarvestTakeSamples(const bronze_code_range* ranges, uint32_t count, uint64_t* out);

}  // namespace bronze::runtime
