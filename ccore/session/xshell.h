/* The cross-link exterior shell (ticket SHELL1, ADR 0078 amendment 7; src/shell.h).
 *
 * A link's own shell is computed alone: its columns, pipes and furniture are "exterior" in their file although another
 * link's facade hides them (a project split by discipline: on the owner's folder of 17 links the links' own shells marked
 * 99.2 % of the triangles exterior). The cross-link pass classifies the visible links together (their caches read in
 * place, each placed: the opaque classes of every link hide the others' elements) in a background job, and keeps the
 * result per link: a byte per entry of its pyramid's element list (1: exterior), what the outside walk then uses in place
 * of the element flag 4 (toc->xmask). Each link's own shell stays the fallback while the pass runs, when it fails, and for a
 * link baked without a shell.
 *
 * The result is keyed by the visible links' caches (path, size, time) and placements, and the shell settings: any change
 * (a link moved, hidden, added, re-indexed) starts the pass again (the previous one cancelled). It is kept in the cache
 * folder (<cache dir>/links/xshell-<16 hex of the key>.bin: never beside the user's files; a cache, computed again when
 * missing or damaged): read untrusted. Carriers without threads (the wasm module) do not run it. */
#ifndef CCORE_SESSION_XSHELL_H
#define CCORE_SESSION_XSHELL_H
#include <stddef.h>
#include <stdint.h>

typedef struct cc_session cc_session;
struct cc_link;
typedef struct cc_xshell cc_xshell;

/* The state for the visible links now: starts the pass (or reads its file) when the key changed, takes a finished job's
 * result. 2 ready, 1 computing, 0 none (fewer than two links with a shell baked, the setting off, no thread), -1 failed. */
int cc_xshell_poll(cc_session *s);
/* Link `l`'s marks, one byte per entry of its pyramid's element list (1 exterior), NULL when not ready. */
const uint8_t *cc_xshell_marks(cc_session *s, const struct cc_link *l, uint32_t *n);
/* Numbers for the tools: elements marked of those listed, the pass's time (ms), its buffers, the overlapping groups of links
 * classified apart (0: read from its file). */
void cc_xshell_stats(cc_session *s, uint64_t *exterior, uint64_t *elements, double *ms, uint64_t *peak, uint32_t *groups);
void cc_xshell_free(cc_session *s);
#endif
