/* The C core's wasm32 API (build.sh wasm; compiled only for __wasm__). C ABI, all integers are
 * wasm i32 (pointers and sizes are byte offsets in the module's linear memory) unless noted.
 *
 * Memory protocol
 *   - Inputs: the host allocates with cc_alloc(n), writes the bytes (UTF-8 text, no NUL needed),
 *     passes (ptr, len) and frees with cc_free(ptr) when the call returned.
 *   - Results: every call that answers takes `res`, a pointer to 12 bytes the host owns:
 *       res[0] u32 ptr   the answer bytes (malloc'd by the module; 0 when empty)
 *       res[1] u32 len   their length
 *       res[2] u32 value a number: the model handle (open_*), 1/0 (undo, redo), the model
 *                        revision (mesh), else 0
 *     The host reads ptr[0..len) then calls cc_free(ptr). The answer is the result (status 200)
 *     or the error text (any other status), UTF-8, except mesh's 200 answer (BMSH bytes).
 *   - Status codes, as the native host: 200 ok, 400 the call failed (bad input; the text says
 *     why, Rust's text), 404 no model with that handle, 500 out of memory or a scene that cannot
 *     be built.
 *   - Models are handles 1..64 (open_text / open_ifc), each with its own undo history; close
 *     frees one. All calls are synchronous; the module is single-threaded.
 *
 * Exports (export name: signature)
 *   cc_alloc(n) -> ptr                      malloc; 0 when out of memory
 *   cc_free(ptr)                            free (0 is fine)
 *   cc_heap_used() -> bytes                 heap carved so far (blocks in use or free)
 *   open_text(ptr, len, res) -> status      .bimble JSON text (file::from_str); value = handle
 *   open_ifc(ptr, len, res) -> status       IFC STEP text (import_str); answer = the report
 *                                           lines; value = handle
 *   close(h) -> status                      frees the model and its history
 *   query(h, name, name_len, args, args_len, res) -> status
 *                                           dispatch::query: compact JSON (args: JSON, empty = {})
 *   apply(h, cmds, len, res) -> status      a JSON array of Commands as one History step
 *                                           (answer empty)
 *   verb(h, name, name_len, args, args_len, res) -> status
 *                                           dispatch::run then History::apply; answer = the
 *                                           commands as compact JSON
 *   undo(h, res), redo(h, res) -> status    answer "true"/"false", value 1/0
 *   history(h, res) -> status               {"redo":[…],"undo":[…]}, newest first, at most 50
 *   save(h, res) -> status                  file::to_string (pretty JSON, no final newline)
 *   mesh(h, flags, res) -> status           the scene as BMSH (layout below); flags bit 0 = local
 *   guid_seed(k)                            k: i64 (a BigInt in JS). k > 0: new ids are
 *                                           from_u128(k), from_u128(k+1)… ; 0: random
 *   guid_entropy(ptr, len)                  mixes host random bytes (crypto.getRandomValues)
 *                                           into the id generator; call once at start
 *   fmt_f64(x: f64, debug, res) -> status   serde_json (debug 0) or Rust {:?} (1) text of x;
 *                                           400 for NaN and inf
 *   parse_f64(ptr, len) -> f64              the exact decimal parser of JSON and STEP numbers
 *
 * The host call layer (host/calls.c, session.c, mesh.c, mesh_model.c compiled into the module,
 * ticket T13b): the same call code as the native host (bimble-ccore), on one session (the
 * module's "open file"), independent of the handles above.
 *   host_open_text(name, name_len, text, text_len, res) -> status
 *                                           opens the session's model from .bimble JSON text
 *                                           (session.c cc_session_open; `name` is the file name
 *                                           the viewer shows and Save "writes"); value = rev.
 *                                           Open once per instance: a second open replaces the
 *                                           session as cc_session_open does natively
 *   call(op, op_len, name, name_len, body, body_len, res) -> status
 *                                           host/calls.c cc_call: exactly the native host's
 *                                           POST /<op>/<name> (status 200 or 400, the answer or
 *                                           the error text); 404 for an unknown op (server.c)
 *   change_json(res) -> status              cc_change_json: the change feed's JSON now
 *   host_mesh(flags, res) -> status         the session's scene as BMSH (mesh.c cc_mesh_get: the
 *                                           native GET /mesh bytes; flags bit 0 = the local form
 *                                           above); value = rev
 *   host_saved(res) -> status               the text the last `action save` wrote (the module has
 *                                           no files: Save keeps it here); 404 before any save
 *   host_log(res) -> status                 what the host layer wrote on stderr since the last
 *                                           host_log ("bimble-ccore: Saved …"), then cleared
 *
 * BMSH (flags 0): exactly host/mesh.c's version 1 (rev = the model revision, bumped by every
 * change: apply, verb, undo, redo that did something): world positions relative to origin_mm.
 * BMSH local (flags 1): the same header with version 2, the same rows and blocks, but each
 * element's mesh is its local mesh (SceneElement::local_mesh: a shared shape in its own
 * coordinates; indices still absolute), then one more block of nE x 52 bytes:
 *   u32 instanced (1: the element is a placed shape, 0: own mesh), f32[12] Placement32 columns
 *   x, y, z, t (zeros when not instanced).
 * The local form is what golden/mesh/<k>.txt holds; the world form is what a viewer draws. */
#ifndef CCORE_WASM_API_H
#define CCORE_WASM_API_H
#include <stddef.h>
#include <stdint.h>

#define CC_WASM_MAX_MODELS 64
#define CC_BMSH_LOCAL_VERSION 2u

typedef struct {
    uint32_t ptr, len, value;
} cc_wasm_res;

#ifdef __wasm__
/* The libc the host call layer (calls.c, session.c, mesh.c, mesh_model.c) uses in the
 * freestanding wasm32 build, implemented in wasm_api.c. The host files include this header
 * instead of <stdio.h> under __wasm__. snprintf knows %s %.*s %d %u %zu %llu %.<n>f and %%;
 * fprintf to stderr appends to a log that the export host_log hands out (and clears). */
#include <stdarg.h>
#include "buf.h"
#include "model.h"
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strrchr(const char *s, int c);
char *strcpy(char *d, const char *s);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
int snprintf(char *out, size_t cap, const char *fmt, ...);
typedef struct cc_wasm_file cc_wasm_file;
#define stderr ((cc_wasm_file *)0)
int fprintf(cc_wasm_file *f, const char *fmt, ...);
/* What `action save` wrote (calls.c save() under __wasm__), and whether it ever did. */
extern Buf cc_wasm_saved;
extern int cc_wasm_has_saved;
/* session.c under __wasm__: cc_session_open(name) reads this text instead of a file. */
int cc_session_open_text(const char *name, const char *text, size_t n, cc_error *err);
#endif

#endif
