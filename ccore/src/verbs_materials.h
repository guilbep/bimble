/* The materials of a model (ADR 0031, ticket MAT1): three verbs, C additions of the verb table
 * (verb_table.c), and the two queries the materials page reads.
 *
 * Rust's viewer edits materials with the commands AddMaterial, SetMaterial and RemoveMaterial
 * straight from its page (crates/bimble-viewer/src/materials_page.rs); its dispatch has no verb for
 * them (dispatch.rs: set_material gives elements a material). The C core's rule is that writers
 * go through verbs (ADR 0068 rule 1), so:
 *   material_add {library?: "C30/37", name?: "...", category?: "steel"}
 *       a library grade (library.rs to_material, named as the grade unless `name` is given) or
 *       a blank material (`name` required; category default "other", no values): add_material.
 *       Refused when a material of that name is there (trimmed, ASCII case ignored), as Rust's
 *       page offers no Add for a grade already in the model (materials_page.rs:180).
 *   material_set {id, name?, category?, props?: {density_kg_m3?, e_mpa?, strength_mpa?, source?}}
 *       the fields given replace the material's (`props` replaces all four values; {} clears
 *       them): one set_material, none when nothing changes. The checks are the command's
 *       (command.rs:317: name, values, unique name), run by the history.
 *   material_remove {id}: remove_material. Refused while elements are made of it, with their
 *       count (the command's own text names the id only).
 * Each is one history step. No Rust oracle for the verbs (golden/DEVIATIONS.md); the commands
 * they return are checked against golden/materials/. Layer L3 (api). */
#ifndef CCORE_VERBS_MATERIALS_H
#define CCORE_VERBS_MATERIALS_H
#include "buf.h"
#include "command.h"
#include "json_r.h"
#include "json_w.h"
#include "model.h"

int cc_vh_material_add(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_material_set(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_material_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);

/* query material_library: library.rs library() in its order, every field:
 * [{category, density_kg_m3, e_mpa, ifc_category, name, source, strength_mpa, strength_symbol}]. */
void cc_material_library_json(JsonW *w);

/* query materials_full: every material of the model with every field (model.rs Material) and
 * its element count, in id order: [{category, element_count, id, name, props?: {...}}] (props
 * left out when empty, as the file). -1 out of memory. */
int cc_materials_full_json(const cc_model *m, JsonW *w);

/* MaterialCategory::ifc_category: "concrete", "steel", "wood", "brick"…; "" for other. */
const char *cc_category_ifc(cc_category c);
#endif
