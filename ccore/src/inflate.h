/* gzip (RFC 1952) over deflate (RFC 1951), for the binary .bimble (version 2, ADR 0042).
 * Read: a full inflate (stored, fixed and dynamic Huffman blocks) of one gzip member, CRC and
 * size checked. Untrusted input: the output is capped (a small file can unpack huge), never
 * reads out of bounds, never aborts. Write: LZ77 with fixed Huffman codes in one block: smaller
 * than Rust's dynamic codes would be only by some percent, and any inflater reads it. */
#ifndef CCORE_INFLATE_H
#define CCORE_INFLATE_H
#include <stddef.h>
#include <stdint.h>
#include "buf.h"

/* CRC-32 (ISO 3309, gzip's), continued from `crc` (0 to start). */
uint32_t cc_crc32(uint32_t crc, const void *p, size_t n);

/* Inflates the gzip member in `in` and appends it to `out`. Bytes after the member are an
 * error. 0 ok; -1 with *why set to a static message ("gzip: …"), when the data is damaged or
 * its output would pass `max_out` bytes. */
int cc_gunzip(const unsigned char *in, size_t n, size_t max_out, Buf *out, const char **why);

/* Appends a gzip member of `in` to `out`. 0 ok, -1 out of memory. */
int cc_gzip(const unsigned char *in, size_t n, Buf *out);
/* The same in stored blocks (no compression): fast to write and to read, n + ~5/64 KiB + 18 bytes. */
int cc_gzip_stored(const unsigned char *in, size_t n, Buf *out);
#endif
