#pragma once

// THE INLINE-CACHE HARVEST (docs/type-feedback.md, "Harvest").
//
// An ordinary build's run fills its inline caches with what its sites met:
// a property site's ways hold the receiver shapes it read or wrote (and
// whether the key was own, inherited, absent or an accessor), a method site's
// two ways the receiver shapes and the callee each latched. A harvest reads
// those words back, names them the way the type-feedback recorder names what
// it observes (type_feedback.h), and writes them as a type-feedback file a
// later build reads with `--feedback-in`. Nothing is recorded while the
// program runs: the caches are the record, kept anyway.
//
// A module is harvested once something names a file for it: its image's
// identity record (`bronze build --harvest-out`), or a host through
// `rtIcHarvestConfigure` (eval::runModule with EvalOptions::harvestOutPath).
// A module built with a profile is never harvested: its caches describe a
// specialized build (a claimed site's hits never touch its cache).
//
// The caches are per (thread, realm) instance and are read on the thread that
// owns them: an instance is folded into its module's harvest when it is
// dropped (a reload puts its tables back as they were, a realm ends) while
// harvesting is armed, and every live one of the calling thread by
// `rtIcHarvestWriteAll`. Folding is a union of what each instance held, so
// folding one twice changes nothing. The fold reads shapes, prototypes and
// constructor names off the live heap: it runs only where a program could
// (never from a thread-exit or atexit hook), and allocates nothing on the JS
// heap.

#include <cstdint>
#include <string>
#include <vector>

namespace bronze {
class Shape;
}

namespace bronze::runtime {

// Whether any module is being harvested. One relaxed load; the only cost the
// harvest adds anywhere (on dropping an instance, a cold path).
bool rtIcHarvestArmed() noexcept;

// From `bronze_register_ic_hot`: a module whose identity names a file is
// harvested into it.
void rtIcHarvestNoteIdentity(const uint64_t* identity);

// Harvest the module whose identity record this is into `path` (an empty
// path stops harvesting it). False when the module spent a profile, or the
// record is not one this runtime reads.
bool rtIcHarvestConfigure(const void* identity, const std::string& path);

// One instance's table: `siteCount` sites at `table`, `methodSites` the
// ascending method-call site numbers (null for none). Cold; a no-op unless
// the module is being harvested.
void rtIcHarvestFold(const uint64_t* table, uint64_t siteCount, const uint64_t* identity,
                     const std::vector<uint64_t>* methodSites);

// A property was added through the runtime and `node` is the shape it took:
// `number` when the add was a plain data property holding a Number (or an
// Int32), the add a double slot would be born from under an eligible key.
// Remembered per node, a non-Number add sticking. Cold (an add the inline
// transition arm did not answer); called only while armed.
//
// With what the live heap holds when the files are written, this is how the
// harvest says which slots stayed Numbers (the `doubles` and `mixed` rows,
// docs/type-feedback.md) without changing a shape of the run it measures.
void rtIcHarvestNoteBirth(const Shape* node, bool number);

// Folds this thread's live instances, then writes every harvested module's
// file. The number of files written.
int rtIcHarvestWriteAll();

// The harvest of the module `identity` names, in the file format, after
// folding this thread's live instances. Empty when it is not harvested.
std::string rtIcHarvestText(const void* identity);

}  // namespace bronze::runtime
