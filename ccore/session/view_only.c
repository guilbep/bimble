/* cc_view_only_refuses (view_only.h). */
#include "view_only.h"
#include <string.h>

#include "measure.h"
#include "visibility.h"

#ifdef CC_VIEW_ONLY
static const char *const REFUSED_OPS[] = {"apply", "undo", "redo", "verb", "set_placing"};
static const char *const REFUSED_ACTIONS[] = {
    "new", "import_ifc", "convert_open", "save", "save_as", "export_ifc4", "export_ifc2x3",
    "share_for_viewing", "undo", "redo", "link_pdf", /* L1 */
};

static int refused(const char *op, const char *name) {
    for (size_t i = 0; i < sizeof REFUSED_OPS / sizeof *REFUSED_OPS; i++)
        if (!strcmp(op, REFUSED_OPS[i])) return 1;
    if (!strcmp(op, "tool")) return !cc_vis_tool_name(name) && !cc_measure_name(name); /* the section box and measuring stay */
    if (strcmp(op, "action")) return 0;
    if (!strncmp(name, "place_", 6)) return 1;
    for (size_t i = 0; i < sizeof REFUSED_ACTIONS / sizeof *REFUSED_ACTIONS; i++)
        if (!strcmp(name, REFUSED_ACTIONS[i])) return 1;
    return 0;
}

int cc_view_only_refuses(const char *op, const char *name, cc_buf *out) {
    if (!refused(op, name)) return 0;
    buf_puts(out, CC_VIEW_ONLY_TEXT);
    return 1;
}
#else
int cc_view_only_refuses(const char *op, const char *name, cc_buf *out) {
    (void)op, (void)name, (void)out;
    return 0;
}
#endif
