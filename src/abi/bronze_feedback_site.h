#ifndef BRONZE_FEEDBACK_SITE_H
#define BRONZE_FEEDBACK_SITE_H

/* TYPE FEEDBACK site words (`bronze build --feedback-out`, docs/type-feedback.md).
 *
 * A feedback build rides the pin census's two entry points rather than adding
 * its own: `bronze_census_register` takes the output path and a table of
 * (module key index, site info) rows, `bronze_census_record` takes (key id,
 * site info, value). What tells the runtime a row or a record is FEEDBACK and
 * not a census site is the top bit of the info word, which no census kind ever
 * sets (census info is a kind byte and BRONZE_ABI_CENSUS_REFUSES). Sharing the
 * channel keeps the generated-code ABI — and with it the fingerprint every
 * host is built against — unchanged; the two instruments are never in one
 * build (the CLI refuses `--census` with `--feedback-out`).
 *
 * Header-only and beside the ABI header for the reason
 * bronze_native_type.h is: the runtime links nothing above `abi`, the lowerer
 * nothing below `il`, and both have to agree on these words. It is NOT part of
 * the fingerprinted ABI text (cmake/bronze_abi_text.cmake).
 *
 * A record's key is the FUNCTION KEY (`<stable name>#<IL hash>`, il/feedback_site.h)
 * and its info word packs the site:
 *
 *   bit 31        BRONZE_FEEDBACK_FLAG
 *   bits 24..30   the site kind (BRONZE_FEEDBACK_KIND_*)
 *   bits 0..23    the site's ordinal within its function
 */

#define BRONZE_FEEDBACK_FLAG          0x80000000u
#define BRONZE_FEEDBACK_KIND_SHIFT    24u
#define BRONZE_FEEDBACK_KIND_MASK     0x7Fu
#define BRONZE_FEEDBACK_ORDINAL_MASK  0x00FFFFFFu

/* Site kinds. Every kind counts value tags; SHAPE and METHOD also name the
 * receiver's shape, TARGET and METHOD the callee.
 *
 *   MARKER       registration only: the row that hands over the output path
 *   ENTRY        function entry; the value is ignored (no operand)
 *   VALUE        a Dynamic operand of an arithmetic, relational, equality,
 *                truthiness or checked-unbox instruction
 *   PARAM        a Dynamic source parameter, at entry
 *   THIS         the `this` parameter, at entry
 *   RESULT       the result of a call, prop.get or elem.get
 *   SHAPE        the receiver of a prop.get / prop.set (an IC site)
 *   TARGET       the callee of a call.dynamic
 *   METHOD       the receiver of a method.call: shape, and the method the key
 *                resolves to on it
 *   METHOD_NAME  registration only: follows a METHOD row, its key is the
 *                method name that site calls */
#define BRONZE_FEEDBACK_KIND_MARKER       0u
#define BRONZE_FEEDBACK_KIND_ENTRY        1u
#define BRONZE_FEEDBACK_KIND_VALUE        2u
#define BRONZE_FEEDBACK_KIND_PARAM        3u
#define BRONZE_FEEDBACK_KIND_THIS         4u
#define BRONZE_FEEDBACK_KIND_RESULT       5u
#define BRONZE_FEEDBACK_KIND_SHAPE        6u
#define BRONZE_FEEDBACK_KIND_TARGET       7u
#define BRONZE_FEEDBACK_KIND_METHOD       8u
#define BRONZE_FEEDBACK_KIND_METHOD_NAME  9u
#define BRONZE_FEEDBACK_KIND_COUNT        10u

#define BRONZE_FEEDBACK_INFO(kind, ordinal)                                     \
    (BRONZE_FEEDBACK_FLAG | (((kind) & BRONZE_FEEDBACK_KIND_MASK) << BRONZE_FEEDBACK_KIND_SHIFT) | \
     ((ordinal) & BRONZE_FEEDBACK_ORDINAL_MASK))
#define BRONZE_FEEDBACK_INFO_KIND(info) \
    (((info) >> BRONZE_FEEDBACK_KIND_SHIFT) & BRONZE_FEEDBACK_KIND_MASK)
#define BRONZE_FEEDBACK_INFO_ORDINAL(info) ((info) & BRONZE_FEEDBACK_ORDINAL_MASK)

/* The spelling of each kind in the file, in kind order. */
#define BRONZE_FEEDBACK_KIND_NAMES \
    { "marker", "entry", "value", "param", "this", "result", "shape", "target", "method", "method-name" }

/* Value tags, one count each per site. `i32` is a Number whose value is an
 * int32 (an Int32 box, or a double that is integral, in range and not -0);
 * `f64` is every other Number. `object` is a plain object; `other` is a
 * symbol, a BigInt, a hole or a heap kind with no tag of its own. */
#define BRONZE_FEEDBACK_TAG_F64        0u
#define BRONZE_FEEDBACK_TAG_I32        1u
#define BRONZE_FEEDBACK_TAG_UNDEFINED  2u
#define BRONZE_FEEDBACK_TAG_NULL       3u
#define BRONZE_FEEDBACK_TAG_BOOL       4u
#define BRONZE_FEEDBACK_TAG_STRING     5u
#define BRONZE_FEEDBACK_TAG_OBJECT     6u
#define BRONZE_FEEDBACK_TAG_ARRAY      7u
#define BRONZE_FEEDBACK_TAG_TYPEDARRAY 8u
#define BRONZE_FEEDBACK_TAG_FUNCTION   9u
#define BRONZE_FEEDBACK_TAG_OTHER      10u
#define BRONZE_FEEDBACK_TAG_COUNT      11u

#define BRONZE_FEEDBACK_TAG_NAMES                                                   \
    { "f64", "i32", "undefined", "null", "bool", "string", "object", "array",       \
      "typedarray", "function", "other" }

/* The file format version `bronze-type-feedback <N>` the writers stamp. The
 * reader takes 1 to 3: 2 names every key of a shape (1 cut the list at 32
 * with `,+N`) and may carry an inline-cache harvest (`harvest`, `ic` and
 * `way` rows, runtime/ic_harvest.h); 3 adds the harvest's `other` row (a
 * site's receivers its cache could not hold), which a version 2 harvest
 * lacks, so a build on one claims as it did before. */
#define BRONZE_FEEDBACK_FORMAT_VERSION 3u

/* Shapes and targets kept per site, largest first at write time; the rest are
 * summed under `*other*`. */
#define BRONZE_FEEDBACK_MAX_NAMES 8u

#endif /* BRONZE_FEEDBACK_SITE_H */
