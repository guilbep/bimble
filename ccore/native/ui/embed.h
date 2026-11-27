/* The files built into the native app (ticket NUI1): the shell's data (data/ JSON files, exported from the
 * web UI's TypeScript) and the lab's fonts (the WOFF2 files of web-ui/src/brand), as C arrays written by
 * native/embed.sh at build time (out/app/gen/embed.c). The binary needs no file beside it. */
#ifndef BIMBLE_NATIVE_EMBED_H
#define BIMBLE_NATIVE_EMBED_H
#include <stddef.h>

typedef struct {
    const char *name;
    const unsigned char *p;
    size_t n;
} nui_blob;

extern const nui_blob NUI_BLOBS[];
extern const size_t NUI_NBLOBS;
const nui_blob *nui_blob_get(const char *name); /* by file name, NULL none */
#endif
