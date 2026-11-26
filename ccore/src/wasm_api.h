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
 *     or the error text (any other status), UTF-8, except mesh's 200 answer (BMSH bytes) and the
 *     200 answer of `call query sheet_pdf` / `view_pdf` (PDF bytes, D7).
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
 *   export_ifc(h, schema, name, name_len, stamp, stamp_len, res) -> status
 *                                           bimble_ifc::export_as (ifc_out.h): schema 0 IFC4,
 *                                           1 IFC2x3; name and stamp go in FILE_NAME; answer =
 *                                           the STEP text
 *   mesh(h, flags, res) -> status           the scene as BMSH (layout below); flags bit 0 = local
 *   guid_seed(k)                            k: i64 (a BigInt in JS). k > 0: new ids are
 *                                           from_u128(k), from_u128(k+1)… ; 0: random
 *   guid_entropy(ptr, len)                  mixes host random bytes (crypto.getRandomValues)
 *                                           into the id generator; call once at start
 *   fmt_f64(x: f64, debug, res) -> status   serde_json (debug 0) or Rust {:?} (1) text of x;
 *                                           400 for NaN and inf
 *   parse_f64(ptr, len) -> f64              the exact decimal parser of JSON and STEP numbers
 *
 * The session layer (session/calls.c, session.c, mesh.c, mesh_model.c compiled into the
 * module, ticket T13b; a carrier with its own cc_platform since ticket A1): the same call code
 * as the native host (bimble-ccore), on the module's default session (its "open file"),
 * independent of the handles above.
 *   host_open_text(name, name_len, text, text_len, res) -> status
 *                                           opens the session's model from .bimble JSON text
 *                                           (session.c cc_session_open; `name` is the file name
 *                                           the viewer shows and Save "writes"); value = rev.
 *                                           Once per instance (400 after a model is open): later
 *                                           opens go through host_open
 *   call(op, op_len, name, name_len, body, body_len, res) -> status
 *                                           session/calls.c cc_call: exactly the native host's
 *                                           POST /<op>/<name> (status 200 or 400, the answer or
 *                                           the error text); 404 for an unknown op (server.c)
 *   change_json(res) -> status              cc_change_json: the change feed's JSON now
 *   host_mesh(flags, res) -> status         the session's scene as BMSH (mesh.c cc_mesh_get: the
 *                                           native GET /mesh bytes; flags bit 0 = the local form
 *                                           above; bit 1 = BMSH version 3, src/scene_bmsh.h, the
 *                                           native GET /mesh?shapes=1, ticket 3D1); value = rev
 *   host_saved(res) -> status               the bytes of the last write (the module has no files:
 *                                           `action save`, `save_as`, `export_ifc4`/`export_ifc2x3`,
 *                                           `share_for_viewing` keep them here: a .bimble is v2
 *                                           binary, a .json the v1 text, an export the STEP text,
 *                                           a view file gzip); 404 before any
 *   host_saved_path(res) -> status          the file name of that write (the session's file name,
 *                                           the {"path"} given, or "<stem>.ifc" / ".bimbleview");
 *                                           404 before any
 *   host_open(flags, name, name_len, bytes, len, res) -> status
 *                                           `action open {"path": name, "discard": …}` on the
 *                                           page's bytes (ticket A5; calls.c file_action): .bimble
 *                                           v1 or v2, .json editable; .ifc and .bimbleview
 *                                           read-only (ticket A5c, as the Rust viewer). flags bit
 *                                           1 (2): `action import_ifc` instead (.ifc imported as
 *                                           an editable copy). Replaces what is open (400 with
 *                                           unsaved changes unless flags bit 0 (1, discard); 400
 *                                           when it does not load, the open file kept); value = rev
 *                                           flags bit 2 (4): `action link_model {"path": name}`
 *                                           (ticket B5, session/links.h): the bytes linked
 *                                           read-only beside what is open (the same name again
 *                                           replaces that link); discard is not used
 *                                           flags bit 3 (8, ticket L1): `action link_pdf {"path":
 *                                           name, "scale": flags >> 8}` (session/links_ref.h):
 *                                           the PDF added to the active 2D view at 1:(flags >> 8)
 * *                                           (0: the view's scale)
 *                                           flags bit 4 (16, ticket L2): `action link_dxf {"path":
 *                                           name}`: the DXF added to the active 2D view
 *   host_action_file(name, nl, body, bl, bytes, len, res) -> status   (ticket LNK1)
 *                                           `action <name> <body>` with `bytes` staged: every
 *                                           file read during the call answers them (manage_links
 *                                           reload {id, path} of a reference, locate {name, path})
 *   host_clock(unix_s: f64)                 the wall clock for the session (IFC export stamps);
 *                                           0 until set (1970-01-01T00:00:00)
 *   host_log(res) -> status                 what the host layer wrote on stderr since the last
 *                                           host_log ("bimble-ccore: Saved …"), then cleared
 *   host_fonts(reg, reg_len, bold, bold_len, res) -> status
 *                                           D7: Roboto-Regular.ttf and Roboto-Bold.ttf for the PDF
 *                                           export (session/sheet_svg.h; copied; empty: none).
 *                                           SIZE1: the page hands none over; the export is
 *                                           Helvetica unless the body asks "font":"roboto" and
 *                                           the files are here (`query pdf_fonts`). `call query sheet_pdf` /
 *                                           `view_pdf` answer the PDF's bytes (status 200)
 *
 * BMSH (flags 0): exactly session/mesh.c's version 1 (rev = the model revision, bumped by every
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

#endif
