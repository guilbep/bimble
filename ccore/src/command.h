/* Model mutations (crates/bimble-core/src/command.rs). Every change to a model is a command:
 * applying one validates it first, makes it canonical (-0.0 to 0.0), mutates the model and
 * returns its inverse. Every command of Rust (docs/research/c-core-spec.md §3); set_material since
 * MAT1. */
#ifndef CCORE_COMMAND_H
#define CCORE_COMMAND_H
#include <stddef.h>
#include "arena.h"
#include "buf.h"
#include "json_r.h"
#include "json_w.h"
#include "model.h"

typedef enum { /* serde tag "op" */
    CC_CMD_ADD_STOREY,
    CC_CMD_REMOVE_STOREY,
    CC_CMD_ADD_MATERIAL,
    CC_CMD_REMOVE_MATERIAL,
    CC_CMD_ADD_ELEMENT,
    CC_CMD_REMOVE_ELEMENT,
    CC_CMD_SET_GENERATOR,
    CC_CMD_SET_PLACEMENT,
    CC_CMD_RENAME,
    CC_CMD_SET_ELEMENT_MATERIAL,
    CC_CMD_SET_PROPERTY,
    CC_CMD_SET_PROJECT,
    CC_CMD_ADD_VIEW,
    CC_CMD_REMOVE_VIEW,
    CC_CMD_SET_VIEW,
    CC_CMD_ADD_SHEET,
    CC_CMD_REMOVE_SHEET,
    CC_CMD_SET_SHEET,
    CC_CMD_SET_SCHEDULES,
    CC_CMD_ADD_RELATION, /* the C core's own (ADR 0070): Rust has no relations */
    CC_CMD_REMOVE_RELATION,
    CC_CMD_SET_RELATION,
    CC_CMD_SET_STOREY, /* the C core's own (ADR 0056): Rust has no levels nor grids */
    CC_CMD_ADD_GRID,
    CC_CMD_REMOVE_GRID,
    CC_CMD_SET_GRID,
    CC_CMD_SET_GEOREF, /* the C core's own (ADR 0045): Rust sets the georeference on import only */
    CC_CMD_SET_MATERIAL, /* MAT1: Rust's SetMaterial (command.rs:317), last so older ops keep their numbers */
    CC_CMD_SET_OPENINGS, /* CONC1 (ADR 0077): the C core's own: an element's whole opening list (Rust has none) */
    CC_CMD_COUNT
} cc_cmd_op;

/* One command. Which fields count depends on `op`:
 *   add_storey: storey          remove_storey/material/element: id
 *   add_material, set_material: material (set: the whole material, by id)   add_element: element
 *   set_generator: id, generator          set_placement: id, placement
 *   rename: id, name            set_element_material: id, material_id
 *   set_property: id, key, value if has_value     set_project: project[nproject] (sorted keys)
 *   add_view, set_view: view    remove_view, remove_sheet: id
 *   add_sheet, set_sheet: sheet set_schedules: schedules[nschedules]
 *   add_relation, set_relation: relation          remove_relation: id
 *   set_storey: storey (the whole level, by id)   add_grid, set_grid: grid   remove_grid: id
 *   set_georef: georef (the whole georeference; the default clears it)
 *   set_openings: id, element.openings[element.nopenings] (the element's whole list, CONC1)
 * Strings and arrays are borrowed: from the cc_cmds arena that made the command, or from the
 * model's arena for the inverses that apply returns. */
typedef struct {
    cc_cmd_op op;
    cc_gid id;
    cc_storey storey;
    cc_material material;
    cc_element element;
    cc_generator generator;
    cc_placement placement;
    cc_str name;
    cc_gid material_id;
    cc_str key, value;
    int has_value;
    cc_kv *project;
    size_t nproject;
    cc_view view;
    cc_sheet sheet;
    cc_schedule *schedules;
    size_t nschedules;
    cc_relation relation;
    cc_grid grid;
    cc_georef georef;
} cc_command;

/* A list of commands with an arena for their strings and arrays. */
typedef struct {
    Arena arena;
    cc_command *v; /* malloc'd */
    size_t n, cap;
} cc_cmds;

void cc_cmds_init(cc_cmds *l);
void cc_cmds_free(cc_cmds *l);
/* A zeroed command at the end, NULL when out of memory. */
cc_command *cc_cmds_push(cc_cmds *l, cc_cmd_op op);

const char *cc_cmd_op_name(cc_cmd_op op); /* "add_storey"… */

/* Command::apply: user-input checks (rename: a name not blank; set_property: key and value one
 * line, not padded, bounded), then validation, then the command is copied into the model's arena,
 * made canonical and applied. On success fills *inverse (borrowing the model's arena) and
 * returns 0. On error the model is unchanged, -1, and `err` holds Rust's CommandError text
 * ("no such object: <id>", "object already exists: <id>", "<id> is still used by elements",
 * "invalid: <why>"). The C core's own: remove_element of an element in a relation is refused,
 * "<id> is still used by the relation <relation id> (<kind>)" (ADR 0070). ADR 0056: set_storey
 * makes a level a datum only while no element is on it ("invalid: the level <name> holds
 * elements: it stays a storey"), add_element on a datum level is refused, and grid commands
 * validate the whole grid table (grid.h). set_material (command.rs:317): a name not blank and at
 * most 200 characters, valid props, no other material of that name (trimmed, ASCII case ignored):
 * "invalid: there is already a material named \"<name>\"". */
int cc_command_apply(cc_model *m, const cc_command *c, cc_command *inverse, Buf *err);

/* Command::apply_inverse: no user-input checks, no copy (the command's data must live as long as
 * the model: an inverse from cc_command_apply). */
int cc_command_apply_inverse(cc_model *m, const cc_command *c, cc_command *inverse, Buf *err);

/* Command::describe(m): what applying it does, for the Undo and Redo buttons. Appends to `out`. */
void cc_command_describe(const cc_command *c, const cc_model *m, Buf *out);
/* describe_group: "nothing", the one command, or "<first> and N more change(s)". */
void cc_command_describe_group(const cc_command *c, size_t n, const cc_model *m, Buf *out);

/* serde_json shape: {"op": ..., fields in declaration order}. */
void cc_command_write(JsonW *w, const cc_command *c);
/* serde_json::to_string(&[Command]): compact JSON array. Appends to `out`. */
void cc_commands_json(const cc_command *c, size_t n, Buf *out);

/* serde_json::from_str::<Command>: one command object from the DOM. Strings are copied into
 * l's arena and the command pushed on l. 0 ok, -1 with `err` (serde-like text). */
int cc_command_parse(const JrVal *v, cc_cmds *l, Buf *err);
/* A JSON array of commands (the body of the viewer's `apply`), appended to l. */
int cc_commands_parse(const char *text, size_t n, cc_cmds *l, Buf *err);

/* Text helpers shared with the verbs. */
/* str::trim: Unicode White_Space at both ends. */
cc_str cc_str_trim(cc_str s);
#endif
