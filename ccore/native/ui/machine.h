/* The shell machine (machine.c): the lab's machine.ts transitions in C, for the native app. */
#ifndef BIMBLE_NATIVE_MACHINE_H
#define BIMBLE_NATIVE_MACHINE_H
enum { SK_REFUSED = -1, SK_HOME, SK_OPENING, SK_VIEW, SK_PAGE, SK_SHEET, SK_ERROR, SK_CONFIRM, SK_DISC, SK_LOST, SK_COUNT };
enum { SE_NEW, SE_OPEN, SE_IMPORT, SE_CLOSE, SE_OPENED, SE_CLOSED, SE_FAILED, SE_CANCEL, SE_DISCARD, SE_SHOW_VIEW, SE_SHOW_PAGE,
       SE_SHOW_SHEET, SE_BACK, SE_HOME, SE_CORE, SE_LOST, SE_RETRY, SE_RECONNECTED, SE_COUNT };
extern const char *const SHELL_KIND_NAME[SK_COUNT];
extern const char *const SHELL_EVENT_NAME[SE_COUNT];
/* What the session holds (machine.ts CoreState). */
typedef struct { int file, unsaved, tab; } ShellCore;
/* The kind after event `ev` in kind `at` (`back`: the kind the state goes back to), SK_REFUSED when refused. */
int shell_next(int at, int back, int ev, ShellCore c, int reconnected_same);
int shell_event(const char *name); /* -1 none */
#endif
