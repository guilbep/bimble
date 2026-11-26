/* The compact mesh bytes "BMG1" (bimble_geom::codec, ADR 0036), byte for byte with Rust's
 * `encode`: positions quantized to 1/16 mm from the mesh's lower corner as zigzag varint deltas
 * (raw f32 when a coordinate is not finite, the span passes u32 steps, or a vertex would come
 * back more than 0.05 mm away); index deltas; edge endpoints as vertex indices when they are a
 * vertex (bits), else a quantized point; edge normals as indices into a table of the distinct
 * ones (w dropped, decoded 0). Format in golden/README.md (view/).
 *
 * `decode` takes untrusted bytes: counts are checked against what is left before allocating,
 * indices against what was read, nothing after the mesh; it never aborts. */
#ifndef CCORE_MESHCODEC_H
#define CCORE_MESHCODEC_H
#include <stddef.h>
#include <stdint.h>
#include "arena.h"
#include "buf.h"
#include "scene.h"

#define CC_MESHCODEC_TOLERANCE_MM 0.05

/* LEB128 varint, as codec::put. */
void cc_put_varint(Buf *b, uint64_t v);

/* Appends codec::encode(m) to `out`. 0 ok, -1 out of memory. */
int cc_meshcodec_encode(const cc_mesh *m, Buf *out);

/* Errors of decode, as codec::Error. */
enum { CC_MC_OK = 0, CC_MC_TRUNCATED = 1, CC_MC_BAD_MAGIC = 2, CC_MC_INVALID = 3, CC_MC_OOM = 4 };

/* codec::decode: the mesh in `out`, its arrays in `a`. Returns a CC_MC_* code; on
 * CC_MC_INVALID, *why is Rust's static reason ("counts", "index out of range"…). */
int cc_meshcodec_decode(const uint8_t *p, size_t n, Arena *a, cc_mesh *out, const char **why);

/* f64 to f32 as Rust's `as f32` (round to nearest, out of range to ±inf), without UB in C. */
float cc_f64_to_f32(double d);
#endif
