/* The materials page's data (ticket MAT1, materials_page.c): query `materials_page`. */
#ifndef CCORE_MATERIALS_PAGE_H
#define CCORE_MATERIALS_PAGE_H
#include "buf.h"

struct cc_session;
/* The JSON answer into `out`; CC_OK, or CC_ERR with the error text. Any file (read only: no ids,
 * no edits). */
int cc_materials_page_query(struct cc_session *s, Buf *out);
#endif
