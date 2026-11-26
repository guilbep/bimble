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
#endif
