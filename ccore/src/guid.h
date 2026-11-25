/* IFC GlobalId (crates/bimble-core/src/guid.rs): a 128-bit UUID compressed to 22 characters of
 * the IFC base64 alphabet "0-9A-Za-z_$". The first character holds the top 2 bits (0..3).
 * Ids are stable: they are never regenerated on edit. */
#ifndef CCORE_GUID_H
#define CCORE_GUID_H
#include <stddef.h>
#include <stdint.h>

/* The 22 characters, not NUL-terminated. */
typedef struct {
    char c[22];
} cc_guid;

/* The 16 raw bytes, big-endian (byte 0 holds the top bits of the u128). */
typedef struct {
    uint8_t b[16];
} cc_guid_raw;

/* GlobalId::from_str: s[0..n) must be 22 alphabet characters, the first one in "0123".
 * Returns 0 and fills *out, or -1 and sets *err (if err is not NULL) to Rust's message:
 * `invalid IFC GlobalId: "<s as Rust {:?}>"`. The message stays valid until the next failed
 * parse. Not thread-safe. */
int cc_guid_parse(const char *s, size_t n, cc_guid *out, const char **err);

/* GlobalId::to_u128 / from_u128, as 16 big-endian bytes. to_bytes needs a valid id. */
void cc_guid_to_bytes(const cc_guid *g, cc_guid_raw *out);
cc_guid cc_guid_from_bytes(const cc_guid_raw *raw);
/* GlobalId::from_u128(hi << 64 | lo). */
cc_guid cc_guid_from_u128(uint64_t hi, uint64_t lo);

/* GlobalId::derived: the first 16 bytes of blake3(data), big-endian. */
cc_guid cc_guid_derived(const void *data, size_t n);

/* GlobalId::new: a random UUID v4 (version 4, variant 10).
 * Seeded mode (tests, golden masking): when cc_guid_seed(k) was called with k > 0, or natively
 * when the environment variable CCORE_SEED is set (to anything; it starts at 1), new() returns
 * from_u128(k), from_u128(k + 1), ... in creation order.
 * Random source: getrandom(2) natively. On wasm32 (no imports) the bytes come from a
 * splitmix64 generator that the host must seed with cc_guid_entropy() (e.g. 32 bytes from
 * crypto.getRandomValues); unseeded, it is deterministic and the ids may repeat across runs. */
cc_guid cc_guid_new(void);
/* k > 0: seeded mode, next new() is from_u128(k). k == 0: back to random. */
void cc_guid_seed(uint64_t k);
/* Mixes host entropy into the wasm32 generator (also accepted natively, where it is unused). */
void cc_guid_entropy(const void *data, size_t n);

/* Order of Rust's derived Ord on GlobalId([u8; 22]): byte order of the 22 characters (memcmp),
 * which is not the numeric order ('$' < '0' < 'A' < '_' < 'a'). Returns <0, 0 or >0. */
int cc_guid_cmp(const cc_guid *a, const cc_guid *b);
#endif
