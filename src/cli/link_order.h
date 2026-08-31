#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Binary LAYOUT as a controlled variable.
//
// A large module is emitted as several partition objects and linked as a list
// (link.h). The order of that list decides where every function lands in the
// image, and that placement — code alignment, which functions share a cache
// set, how far a call is from its callee — moves a benchmark by percent-scale
// amounts that have nothing to do with the code under test. An A/B that pins
// the order pins one arbitrary draw of a distribution and reports it as the
// difference between two compilers.
//
// So the order becomes an input. `--link-seed` permutes it deterministically,
// and measuring the same arm under several seeds turns layout from a hidden
// bias into a spread that can be printed next to the delta. Nothing else about
// the build changes: same objects, same symbols, same program, and with no
// seed set the linker is handed exactly the order the backend chose — which is
// the affinity order the partition plan decides, not the bin numbering
// (codegen-llvm/llvm_partition.h).
//
// `--keep-objs` is the other half. A seed costs a LINK, not a compile, only if
// the objects outlive the build that made them; without it a per-seed sweep of
// a library-sized graph is minutes per point instead of seconds.
//
// Both are process-wide settings rather than parameters threaded through
// `runBuild`, because they belong to the measurement harness and not to the
// compilation: a build that sets neither is the build bronze has always run.

namespace bronze::cli {

// The link-order seed, absent by default. Absent is not "seed 0": it is the
// untouched path, where `orderForLink` is the identity.
void setLinkSeed(std::optional<uint64_t> seed);
std::optional<uint64_t> linkSeed();

// The order the objects are handed to the linker in. The identity when no
// seed is set; otherwise a seeded Fisher-Yates permutation, which is a
// function of the seed and the count alone — the same seed gives the same
// order on every run, on every machine.
std::vector<std::string> orderForLink(const std::vector<std::string>& objPaths);

// Where a build leaves the partition objects it emitted instead of deleting
// them. Empty, the default, deletes.
void setKeptObjectDir(const std::string& dir);
const std::string& keptObjectDir();

// Copies `objPaths` into `keptObjectDir()`, renamed so that lexicographic
// order is the order they arrived in — that is what lets `linkFromObjectDir`
// recover the unpermuted order from the directory alone, and it is why a
// relink of a kept directory reproduces the build's own layout rather than an
// arbitrary one. Returns a diagnostic, empty on success.
std::string retainObjects(const std::vector<std::string>& objPaths);

// Links an executable from a directory `retainObjects` filled, under whatever
// seed is currently set. An empty or unreadable directory is an error naming
// it, never a link of nothing.
int linkFromObjectDir(const std::string& objDir, const std::string& outputPath,
                      std::string* errOut);

// `bronze link <objdir> -o <exe> [--link-seed <n>]`.
int runLink(int argc, char** argv);

// `--link-seed <n>` / `--keep-objs <dir>`, in both the spaced and `=` spellings,
// consumed where `build` parses its own flags. Answers whether `arg` was one of
// them; `error` non-empty means it was, and was malformed.
bool consumeLinkMeasurementFlag(const std::string& arg, int& i, int argc, char** argv,
                                std::string& error);

}  // namespace bronze::cli
