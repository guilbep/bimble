/* Saved quantity lists (ticket QTY1, ADR 0080): three verbs, C additions of the verb table
 * (verb_table.c). Rust's viewer edits schedules with the command SetSchedules straight from its
 * panel (crates/bimble-viewer/src/schedules.rs: new_schedule, change_schedule, Duplicate, Delete,
 * Back to built-in); its dispatch has one verb, set_schedule (grouping, family, columns), kept as
 * Rust wrote it. The C core's writers go through verbs (ADR 0068 rule 1), so:
 *   schedule_add {name?, preset?, classes?, family?, columns?, properties?, group_by_level?,
 *                 group_by_material?, summary?, totals?, filter?, sort?}
 *       a new list, stored after the model's own. Without a name: "Schedule N", the first N free
 *       (schedules.rs new_schedule); without columns: level, name, class, material, volume, grouped
 *       by level, with totals (new_schedule's). preset "by_material": the By material list
 *       (named "By material" unless `name` is given): material, level, count, area, volume, mass,
 *       grouped by material then level, subtotals only. Refused when the name is taken (any ASCII
 *       case, built-in ones too).
 *   schedule_edit {name, new_name?, ...the same fields}: the fields given replace the list's (a
 *       built-in one is stored changed, as change_schedule); `new_name` renames a list of the
 *       model's own (a built-in keeps its name, as Rust's panel). `filter` or `sort` null: removed.
 *   schedule_remove {name}: a list of the model's own is deleted; a built-in one changed goes back
 *       to the built-in (Rust's Delete and Back to built-in).
 * Each is one set_schedules Command (one history step), checked by cc_schedules_validate (names,
 * 1 to 32 columns with the property columns, texts of 1 to 200 bytes), none when nothing changes.
 * Columns: the field names of view.c (Rust's 13 and count, type, gross_volume); properties: keys
 * as the model stores them ("Pset_WallCommon/IsExternal"). Layer L3 (api). */
#ifndef CCORE_VERBS_SCHEDULES_H
#define CCORE_VERBS_SCHEDULES_H
#include "buf.h"
#include "command.h"
#include "json_r.h"
#include "model.h"

int cc_vh_schedule_add(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_schedule_edit(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_schedule_remove(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
#endif
