/* Verbs of views (ticket SEC1), C additions of the verb table (verb_table.c): view_add,
 * view_remove, view_edit. Rust has no such verbs: its viewer makes the View and stores it with
 * Command::AddView / SetView / RemoveView (viewing.rs store_view, remove_view); these verbs make
 * the same View and the same one command, one history step:
 *   view_add {kind: "plan"|"ceiling", storey}     views.rs new_plan: "Plan <level> (copy)",
 *                                                 "Ceiling <level> (copy)", ViewRange::default_for
 *   view_add {kind: "section", facing, origin}    views.rs new_section: "Section <n> (looking
 *                                                 <facing>)", n = sections + 1, depth 0 (the
 *                                                 session gives `origin` when it is missing: the
 *                                                 middle of the scene's bounds, rounded, as Rust)
 *   view_add {kind: "three_d"}                    viewing.rs:836: "3D <views + 1>"
 *   view_add {from: id}                           viewing.rs:781 Duplicate: "<name> (copy)"
 *   (`name` replaces the made-up name in every form.)
 *   view_remove {id}                              viewing.rs remove_view: a stored view; a
 *                                                 changed default view goes back to its default
 *   view_edit {id, name?, origin?, facing?, depth_mm?, levels?: {top?, cut?, bottom?, depth?},
 *              visibility?: {hidden_classes?, hidden_families?, hidden_storeys?, hidden_links?,
 *              hidden_materials?, hidden_elements?, isolated_elements?}}
 *                                                 the view-properties fields Rust's form stores
 *                                                 (viewing.rs:976-1440): a list given replaces
 *                                                 the view's (sorted, unique, as a BTreeSet); a
 *                                                 default view is stored on its first change
 * Arguments are untrusted: unknown fields are refused, texts and lists bounded. Layer L3. */
#ifndef CCORE_VERBS_VIEWS_H
#define CCORE_VERBS_VIEWS_H
#include "buf.h"
#include "command.h"
#include "json_r.h"
#include "model.h"

int cc_vh_view_add(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_view_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_view_edit(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);

/* The visibility `v` with the 7 lists sorted and unique (byte order), copies in `a`. 0, -1 out of
 * memory. */
int cc_visibility_set(Arena *a, cc_visibility *v);
#endif
