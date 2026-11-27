/* The property and quantity sets of an IFC's products (ticket M1LINKS, ADR 0081, audit finding C1):
 * what the display reader (ifc_read.h) skips, read in a pass of its own over the same text
 * (cc_step_parse_only: IfcRelDefinesByProperties, IfcPropertySet, IfcElementQuantity and their
 * properties and quantities), for the properties panel of a linked element. Rust's reader keeps
 * none (read.rs NOT_DISPLAYED); DEVIATIONS 39.
 *
 * The result is a table, one entry per scene element (same index), each the element's sets as
 * compact JSON (empty: none), keys sorted:
 *   [{"name": "Pset_ColumnCommon", "properties": [{"name": "IsExternal", "value": "false"}, …],
 *     "type": "IfcPropertySet" | "IfcElementQuantity"}, …]
 * sets in the order of the file's relations, properties in the set's order; a value is text: a
 * label or text as is, a number as its shortest form, a boolean "true" / "false" / "unknown", an
 * enumeration's values joined by ", ", a quantity's value (its unit left out). Types' property
 * sets (IfcRelDefinesByType) are not read.
 *
 * The table's bytes (a link cache's section PROP, or kept in memory for a small IFC):
 *   u32 n, u32 end[n] (the end of entry i in the text after the table; entry i starts at end[i-1],
 *   0 for i = 0), then the texts. Little-endian.
 * Bounded: at most CC_PSETS_SETS sets and CC_PSETS_PROPS properties per set are written, an entry at
 * most CC_PSETS_ENTRY bytes (the sets beyond are left out), the table at most CC_PSETS_MAX bytes
 * (the elements beyond get none). A table read back is untrusted: cc_psets_entry checks it. */
#ifndef CCORE_IFC_PSETS_H
#define CCORE_IFC_PSETS_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"
#include "ifc_read.h"

#define CC_PSETS_SETS 64
#define CC_PSETS_PROPS 512
#define CC_PSETS_ENTRY ((size_t)64 << 10)
#define CC_PSETS_MAX ((size_t)256 << 20)

/* The table of scene `s` read from `text` (the IFC `s` was read from) appended to `out`. 0 ok
 * (also when the text has no sets: every entry empty), -1 with `err` (out of memory, not a STEP
 * text). */
int cc_ifc_psets(const char *text, size_t n, const cc_ifc_scene *s, Buf *out, cc_error *err);
/* The size of the table's head for `n` entries. */
static inline uint64_t cc_psets_head(uint64_t n) { return 4 + 4 * n; }
/* Entry `i` of a whole table `t` (n bytes): its text in [*p, *p + *len). 0 ok, -1 a table that
 * does not hold together (or no such entry). */
int cc_psets_entry(const uint8_t *t, size_t n, size_t i, const uint8_t **p, size_t *len);

/* SIZE2: the table packed (a link cache's section PRPZ, cache format 5): each distinct set once (IFC
 * shares an IfcPropertySet between elements through IfcRelDefinesByProperties: the same text once), the
 * elements pointing to set ids, the sets' texts in blocks of about CC_PSETS_BLOCK bytes, each block one
 * gzip member (inflate.h). Little-endian:
 *   0  u32 nE elements   4 u32 nS distinct sets   8 u32 nR set references   12 u32 nB blocks
 *   16 u32 first[nE + 1]  element i's sets are refs[first[i] .. first[i + 1]) (first[0] 0, rising, first[nE] nR)
 *      u32 refs[nR]       set ids (< nS), in the element's order
 *      u32 send[nS]       the end of set j in the sets' text (set j starts at send[j - 1], 0 for j 0)
 *      u32 bset[nB + 1]   block b holds sets [bset[b], bset[b + 1]) (bset[0] 0, rising, bset[nB] nS)
 *      u32 boff[nB + 1]   block b's gzip member is blob[boff[b] .. boff[b + 1]) (boff[0] 0, rising)
 *      blob
 * Element i's entry is "[" + its sets joined by "," + "]" ("" when it has none): cc_psets_entry's text.
 * Read per element with a few small reads and one block inflated (bounded: a block inflates to exactly
 * its sets' bytes, at most CC_PSETS_BLOCK_MAX). */
#define CC_PSETS_BLOCK ((size_t)64 << 10)
#define CC_PSETS_BLOCK_MAX ((size_t)1 << 20)
/* The packed form of table `t` (n bytes, cc_ifc_psets's) appended to `out`. 0 ok, -1 out of memory or a
 * table that does not hold together. */
int cc_psets_pack(const uint8_t *t, size_t n, Buf *out);
/* Checks a packed head (the first `n` bytes of the section, at least the arrays before the blob; `len`
 * the section's bytes): counts against `nelements` and `len`, first[] rising to nR. 0 ok, -1 not. */
int cc_psets_packed_check(const uint8_t *p, size_t n, uint64_t len, uint64_t nelements);
/* The bytes of a packed head before the blob (0: not a head). */
uint64_t cc_psets_packed_head(const uint8_t *p16);
/* Reads `n` bytes at `off` of the section into `dst`: 0 ok (all of them), else -1. */
typedef int (*cc_psets_read_fn)(void *ctx, uint64_t off, size_t n, void *dst);
/* Element i's entry (cc_psets_entry's text) from a packed section `len` bytes long, through `rd`, appended
 * to `out`. 0 ok, -1 damaged (or unreadable), -2 out of memory. */
int cc_psets_packed_entry(cc_psets_read_fn rd, void *ctx, uint64_t len, size_t i, Buf *out);
#endif
