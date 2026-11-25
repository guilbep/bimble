/* The call router (L5): one function per call, as `Viewer::call` in
 * crates/bimble-viewer/src/calls.rs, on a session (session.h). The carriers (host/server.c over
 * HTTP, src/wasm_api.c in the module) own the transport and the change feed. */
#ifndef CCORE_SESSION_CALLS_H
#define CCORE_SESSION_CALLS_H
#include <stddef.h>
#include "buf.h"
#include "session.h"

typedef Buf cc_buf;

/* Status of a call, as HTTP: OK = 200 with the answer text (empty for a void call),
 * ERR = 400 with the error text the lab throws (calls.rs `Err(String)`, serve.rs:282-283). */
enum { CC_OK = 200, CC_ERR = 400 };

/* One call. `op` is one of cc_known_op (the router checked it), `name` is the decoded name
 * segment ("" for "-"), `body` holds `n` bytes of UTF-8 (NUL-terminated after n). Writes the
 * answer or the error text into `out` (empty on entry) and returns CC_OK or CC_ERR. */
int cc_call(cc_session *s, const char *op, const char *name, const char *body, size_t n, cc_buf *out);

/* The change JSON (`Viewer::change_json`, calls.rs:134-167) for the state now. The host
 * publishes it after each call (as the twin's loop does) and once at start. */
void cc_change_json(cc_session *s, cc_buf *out);

/* 1 if `op` is one of the ops of `dispatch_op` (calls.rs:27-41), else 0. */
int cc_known_op(const char *op);
#endif
