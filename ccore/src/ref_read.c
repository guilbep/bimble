/* Reference plans: the shared read result and the dispatch by file name. See ref_read.h. */
#include "ref_read.h"
#include <stdlib.h>
#include <string.h>
#include "pdf_in.h"

void cc_refread_init(cc_refread *r) {
    memset(r, 0, sizeof *r);
    buf_init(&r->notes);
}

void cc_refread_free(cc_refread *r) {
    free(r->xy), free(r->start);
    buf_free(&r->notes);
    memset(r, 0, sizeof *r);
}

static int finite2(const double p[2]) { return p[0] - p[0] == 0.0 && p[1] - p[1] == 0.0; }

int cc_refread_push(cc_refread *r, const double (*p)[2], size_t n) {
    if (n < 2) return 0;
    for (size_t i = 0; i < n; i++)
        if (!finite2(p[i])) return 0;
    if (r->npts + n > CC_REF_MAX_POINTS) {
        r->truncated = 1;
        return 0;
    }
    if (r->npts + n > r->cappts) {
        size_t c = r->cappts ? r->cappts : 256;
        while (c < r->npts + n) c *= 2;
        void *w = realloc(r->xy, c * sizeof *r->xy);
        if (!w) return r->oom = 1, -1;
        r->xy = w, r->cappts = c;
    }
    if (r->nlines + 2 > r->caplines) {
        size_t c = r->caplines ? r->caplines * 2 : 64;
        void *w = realloc(r->start, c * sizeof *r->start);
        if (!w) return r->oom = 1, -1;
        r->start = w, r->caplines = c;
    }
    memcpy(r->xy + r->npts, p, n * sizeof *p);
    r->start[r->nlines] = r->npts;
    r->npts += n;
    r->nlines++;
    r->start[r->nlines] = r->npts;
    return 0;
}

void cc_refread_note(cc_refread *r, const char *text) {
    buf_puts(&r->notes, text);
    buf_putc(&r->notes, '\n');
    if (r->notes.oom) r->oom = 1;
}

static int ends_with_ci(const char *s, size_t n, const char *ext) {
    size_t k = strlen(ext);
    if (n < k) return 0;
    for (size_t i = 0; i < k; i++) {
        char c = s[n - k + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != ext[i]) return 0;
    }
    return 1;
}

int cc_ref_read(const char *name, size_t name_len, const unsigned char *bytes, size_t n, double pdf_scale,
                cc_refread *out, Buf *err) {
    cc_refread_init(out);
    if (n > CC_REF_MAX_FILE_BYTES) {
        buf_append(err, name, name_len);
        buf_puts(err, ": larger than 256 MB");
        return -1;
    }
    if (ends_with_ci(name, name_len, ".dxf")) {
        /* ---- L2: the DXF reader goes here (cc_dxf_read) ---- */
        buf_append(err, name, name_len);
        buf_puts(err, ": DXF reference plans are not read by the C core yet");
        return -1;
    }
    if (ends_with_ci(name, name_len, ".pdf")) return cc_pdf_read(bytes, n, pdf_scale, out, err);
    if (ends_with_ci(name, name_len, ".dwg")) {
        buf_puts(err, "DWG can't be read directly (its libraries are closed or GPL). Convert it to DXF first, for "
                      "example with the free ODA File Converter or LibreDWG's dwg2dxf, then add the DXF.");
        return -1;
    }
    buf_append(err, name, name_len);
    buf_puts(err, ": a reference plan must be a .dxf or a .pdf");
    return -1;
}
