/* The viewer-only wasm (ticket W1, ADR 0069 rule 3: build.sh wasm-view, -DCC_VIEW_ONLY): the
 * calls that edit, save or export answer CC_VIEW_ONLY_TEXT (status 400), because the build leaves
 * out their code (verbs, the verb table, the IFC writer and import; build.sh VIEW_DENY). Every
 * other build compiles this file too and refuses nothing. */
#ifndef CCORE_VIEW_ONLY_H
#define CCORE_VIEW_ONLY_H
#include "calls.h"

#define CC_VIEW_ONLY_TEXT "this build is view-only"

/* 1 (and CC_VIEW_ONLY_TEXT in `out`) when the view build refuses cc_call(op, name): apply, undo,
 * redo, verb, set_placing; tool calls but the section box's; the actions new, import_ifc,
 * convert_open, save, save_as, export_ifc4, export_ifc2x3, share_for_viewing, undo, redo and
 * place_*. 0 otherwise, and always 0 outside the view build. */
int cc_view_only_refuses(const char *op, const char *name, cc_buf *out);
#endif
