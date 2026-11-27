/* The shell machine of the web lab (web-ui/src/shell/machine.ts: its TRANSITIONS), ported to C by hand
 * once for the native app (ticket NUI1, ADR 0084). machine.ts's rows are functions (payloads, the state
 * to go back to), so the port is checked, not generated: web-ui/scripts/export-shell-data.mjs runs each
 * row of the TypeScript on one shell of each kind and three sessions and writes where it goes
 * (data/machine.json "dest"); ccore/test/native_machine_test.mjs builds this file and compares, row by
 * row: a change to machine.ts that this port does not follow fails ccore/ci/run.sh unit.
 * No SDL, no ImGui: the test compiles it alone (machine.h). */
#include <string.h>
#include "machine.h"

const char *const SHELL_KIND_NAME[SK_COUNT] = {"home", "opening", "model.view", "model.page", "model.sheet", "error", "confirmClose",
                                               "disconnected", "sessionLost"};
const char *const SHELL_EVENT_NAME[SE_COUNT] = {"new", "open", "import", "close", "opened", "closed", "failed", "cancel", "discard",
                                                "showView", "showPage", "showSheet", "back", "home", "core", "lost", "retry", "reconnected"};

static int is_model(int k) { return k == SK_VIEW || k == SK_PAGE || k == SK_SHEET; }
/* machine.ts derive: what the session allows a state to be now */
static int derive(int k, ShellCore c) {
    if (is_model(k) && !c.file) return SK_HOME;
    if (k == SK_VIEW && !c.tab) return SK_HOME;
    return k;
}
static int file_row(ShellCore c) { return c.unsaved && c.file ? SK_CONFIRM : SK_OPENING; } /* file(what) */
static int need_file(ShellCore c, int to) { return c.file ? to : SK_REFUSED; }           /* opened, view, page, sheet */

int shell_next(int at, int back, int ev, ShellCore c, int reconnected_same) {
    switch (at) {
    case SK_HOME:
        switch (ev) {
        case SE_NEW: case SE_OPEN: case SE_IMPORT: return file_row(c);
        case SE_CLOSE: return c.file ? file_row(c) : SK_REFUSED;
        case SE_OPENED: case SE_SHOW_VIEW: return need_file(c, SK_VIEW);
        case SE_CLOSED: case SE_HOME: return SK_HOME;
        case SE_FAILED: return SK_ERROR;
        case SE_SHOW_PAGE: return need_file(c, SK_PAGE);
        case SE_SHOW_SHEET: return need_file(c, SK_SHEET);
        case SE_CORE: return derive(SK_HOME, c);
        case SE_LOST: return SK_DISC;
        default: return SK_REFUSED;
        }
    case SK_OPENING:
        switch (ev) {
        case SE_OPENED: return SK_VIEW;
        case SE_CLOSED: return SK_HOME;
        case SE_FAILED: return SK_ERROR;
        case SE_CANCEL: return derive(back, c);
        case SE_CORE: return SK_OPENING;
        case SE_LOST: return SK_DISC;
        default: return SK_REFUSED;
        }
    case SK_VIEW: case SK_PAGE: case SK_SHEET:
        switch (ev) {
        case SE_NEW: case SE_OPEN: case SE_IMPORT: case SE_CLOSE: return file_row(c);
        case SE_OPENED: case SE_SHOW_VIEW: return need_file(c, SK_VIEW);
        case SE_CLOSED: case SE_HOME: return SK_HOME;
        case SE_FAILED: return SK_ERROR;
        case SE_SHOW_PAGE: return need_file(c, SK_PAGE);
        case SE_SHOW_SHEET: return need_file(c, SK_SHEET);
        case SE_CORE: return derive(at, c);
        case SE_LOST: return SK_DISC;
        case SE_BACK: return at == SK_VIEW ? SK_REFUSED : need_file(c, SK_VIEW);
        default: return SK_REFUSED;
        }
    case SK_ERROR:
        switch (ev) {
        case SE_BACK: return derive(back, c);
        case SE_HOME: case SE_CLOSED: return SK_HOME;
        case SE_OPENED: return need_file(c, SK_VIEW);
        case SE_FAILED: case SE_CORE: return SK_ERROR;
        case SE_LOST: return SK_DISC;
        default: return SK_REFUSED;
        }
    case SK_CONFIRM:
        switch (ev) {
        case SE_DISCARD: return SK_OPENING;
        case SE_CANCEL: return derive(back, c);
        case SE_OPENED: return need_file(c, SK_VIEW);
        case SE_CLOSED: return SK_HOME;
        case SE_FAILED: return SK_ERROR;
        case SE_CORE: return SK_CONFIRM;
        case SE_LOST: return SK_DISC;
        default: return SK_REFUSED;
        }
    case SK_DISC:
        switch (ev) {
        case SE_RETRY: case SE_LOST: case SE_FAILED: case SE_OPENED: case SE_CLOSED: case SE_CORE: return SK_DISC;
        case SE_RECONNECTED: return reconnected_same ? derive(back, c) : SK_LOST;
        default: return SK_REFUSED;
        }
    case SK_LOST:
        switch (ev) {
        case SE_HOME: return SK_HOME;
        case SE_NEW: case SE_OPEN: case SE_IMPORT: return file_row(c);
        case SE_SHOW_VIEW: return need_file(c, SK_VIEW);
        case SE_OPENED: case SE_CLOSED: case SE_CORE: return SK_LOST;
        case SE_FAILED: return SK_ERROR;
        case SE_LOST: return SK_DISC;
        default: return SK_REFUSED;
        }
    default: return SK_REFUSED;
    }
}

int shell_event(const char *name) {
    for (int e = 0; e < SE_COUNT; e++) if (!strcmp(SHELL_EVENT_NAME[e], name)) return e;
    return -1;
}
