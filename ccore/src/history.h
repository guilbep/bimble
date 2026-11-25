/* Undo/redo stacks (crates/bimble-core/src/command.rs, History). Each step is one user action:
 * its label in words and the commands that take it back (or forward again). A history belongs
 * to one model: its steps borrow the model's arena. */
#ifndef CCORE_HISTORY_H
#define CCORE_HISTORY_H
#include <stddef.h>
#include "buf.h"
#include "command.h"
#include "model.h"

typedef struct {
    char *label; /* malloc'd, NUL-terminated */
    cc_command *cmds; /* malloc'd */
    size_t n;
} cc_step;

typedef struct {
    cc_step *undo, *redo; /* oldest first */
    size_t nundo, nredo, capundo, capredo;
} cc_history;

void cc_history_init(cc_history *h);
void cc_history_free(cc_history *h);

/* History::apply: the commands as one transaction, all kept or none. Pushes a step labelled by
 * describe_group (also for zero commands: "nothing") and clears redo. 0 ok; -1 with the first
 * command's error in `err`, the model unchanged. */
int cc_history_apply(cc_history *h, cc_model *m, const cc_command *cmds, size_t n, Buf *err);

/* History::undo / redo: 1 when a step was taken back (or forward), 0 when there was none. */
int cc_history_undo(cc_history *h, cc_model *m);
int cc_history_redo(cc_history *h, cc_model *m);

/* The labels as JSON string arrays, newest first, at most `max` each (0: all). Appends. */
void cc_history_labels(const cc_history *h, size_t max, Buf *json_undo, Buf *json_redo);
/* The viewer's `query history` (calls.rs): {"redo":[…],"undo":[…]} (json! sorts keys), newest
 * first, at most 50 each. */
void cc_history_json(const cc_history *h, Buf *out);
#endif
