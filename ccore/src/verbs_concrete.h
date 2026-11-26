/* CONC1 (ADR 0077): the Concrete tab's foundations and voids as verbs (C additions: Rust v0.19.0
 * has no verb for any of them; its model has pad footings and openings as data only).
 *   footing_add   an IfcFooting: a pad (pad_footing generator) centred under a column or free, or
 *                 a strip (wall generator, property Bimble/PredefinedType STRIP_FOOTING) along a
 *                 wall or between two points; top at the column's or wall's base, else at the level.
 *   raft_add      an IfcSlab BASESLAB (property Bimble/PredefinedType): verbs::new_slab's outline,
 *                 top at the lowest storey's elevation.
 *   opening_add   an opening of a wall or a slab (model.rs Opening, its own GlobalId): a box through
 *                 the host's thickness, or a sleeve (round: a CC_SLEEVE_SIDES-gon prism); refused
 *                 when it leaves the host or overlaps another opening. One set_openings Command.
 *   opening_set   the same opening moved or resized by its parameters (its GlobalId kept).
 *   opening_remove
 * Handlers follow verb_table.h's cc_verb_fn: they read the model and return Commands. */
#ifndef CCORE_VERBS_CONCRETE_H
#define CCORE_VERBS_CONCRETE_H
#include "buf.h"
#include "command.h"
#include "json_r.h"
#include "model.h"

#define CC_OPENING_OVERSHOOT 10.0 /* mm: an opening's prism passes this far beyond each face of its host */

int cc_vh_footing_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err);
int cc_vh_raft_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err);
int cc_vh_opening_add(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err);
int cc_vh_opening_set(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err);
int cc_vh_opening_remove(const cc_model *m, const JrVal *args, cc_cmds *l, Buf *err);

/* WAVE1B (B2): opening k of the element `e` as opening_set reads it. kind 0: a box (opening), 1: a
 * sleeve, 2: another shape (imported: not editable by its parameters). The sizes as spec: cx, cy the
 * centre in the host's plan frame (a wall: along it, cy 0), w, h (wall), len (slab), sill (wall),
 * dia (sleeve); box: the host-local box of its solid (min x, y, z, max x, y, z). */
typedef struct {
    int kind, wall;
    double cx, cy, w, h, len, sill, dia;
    double box[6];
} cc_opening_info;
void cc_opening_info_of(const cc_element *e, size_t k, cc_opening_info *out);
#endif
