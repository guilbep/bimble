/* The verb table (ADR 0068 §3): one static entry per verb, in the order of Rust's
 * dispatch::VERBS. An entry gives the verb's name, its one-line summary (dispatch::list), the JSON
 * schema of its arguments (dispatch::describe: schemars draft 2020-12, compact, keys sorted as
 * serde_json prints a Value; golden/verbs_schema/), its handler (verbs.c) and whether it only
 * reads. Every writer goes through this table: cc_verb_run, the `verbs` and `describe` queries. */
#ifndef CCORE_VERB_TABLE_H
#define CCORE_VERB_TABLE_H
#include <stddef.h>
#include "buf.h"
#include "command.h"
#include "json_r.h"
#include "model.h"

/* A verb handler: `args` is the parsed argument object ({} for null). Appends the commands to
 * `out` (strings in out's arena) and returns 0, or returns -1 with Rust's error text in `err`. */
typedef int (*cc_verb_fn)(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);

typedef struct {
    const char *name;
    const char *summary;
    const char *schema; /* compact JSON text */
    cc_verb_fn run;
    int read_only; /* 1: never returns commands (none of the 14 so far) */
} cc_verb_def;

extern const cc_verb_def cc_verb_table[];
extern const size_t cc_verb_count;

/* The entry named `name` (NUL-terminated), NULL when none. */
const cc_verb_def *cc_verb_find(const char *name);
/* dispatch::list as serde_json::to_string prints it: [{"name":…,"summary":…},…]. Appends. */
void cc_verbs_list_json(Buf *out);

/* Handlers (verbs.c). */
int cc_vh_place(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_move(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_copy(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_delete(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_set_generator(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_set_view(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_set_schedule(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_set_material(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_set_note(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_set_project_field(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_rotate(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_mirror(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_add_sheet(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
int cc_vh_set_sheet(const cc_model *m, const JrVal *args, cc_cmds *out, Buf *err);
#endif
