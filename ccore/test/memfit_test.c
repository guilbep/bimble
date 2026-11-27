/* Suite memfit (ticket SIZE2, ADR 0079 amendment): "too large for this machine" (session.c
 * cc_session_fits) against what reading each IFC of a local folder of 17 files actually took (the
 * LINKF table of ADR 0079: size, peak resident memory of link-convert). The estimate must stay above
 * every measured peak (a file it lets through must not run the machine out), and must not ask much
 * more than that: the 0.87 GB file (peak 2.91 GB) is accepted with 3.4 GB free. Fails on the code
 * before SIZE2: 4 x + min(512 MiB, 6 x) asked 4.0 GB for it (refused the owner's 17th file with
 * 2.1 GB free while its read took 2.9 GB, and refused it with 3.4 GB free here). */
#include <stdio.h>
#include <string.h>
#include "../session/platform.h"
#include "../session/session.h"
#include "check.h"

static uint64_t avail_now;
static uint64_t fake_avail(void *ctx) { return (void)ctx, avail_now; }

int main(void) {
    cc_platform pf;
    memset(&pf, 0, sizeof pf);
    pf.mem_available = fake_avail;
    /* ADR 0079's table: size (MB, 10^6) and peak (MB) */
    static const double rows[17][2] = {{0.20, 5},   {1.56, 8},     {18.7, 39},   {25.3, 131},  {43.7, 151},  {53.5, 369},
                                       {59.4, 186}, {60.0, 598},   {71.3, 305},  {159, 704},   {201, 450},   {336, 1130},
                                       {394, 1280}, {415, 1520},   {709, 2570},  {758, 2230},  {872, 2910}};
    cc_error err;
    int above = 1;
    for (int i = 0; i < 17; i++) {
        uint64_t size = (uint64_t)(rows[i][0] * 1e6), peak = (uint64_t)(rows[i][1] * 1e6);
        avail_now = peak; /* exactly the measured peak free: the estimate must say it is not enough */
        int r = cc_session_fits(&pf, "x.ifc", size, &err);
        if (r != -2) {
            above = 0;
            printf("  row %d: %.2f MB, peak %.0f MB: accepted with only the peak free\n", i + 1, rows[i][0], rows[i][1]);
        }
    }
    check_true("the estimate is above every measured peak of the folder's 17 files", above);
    avail_now = (uint64_t)3400 * 1000000;
    int r = cc_session_fits(&pf, "x.ifc", (uint64_t)872 * 1000000, &err);
    if (r) printf("  %s\n", err.msg);
    check_true("the 0.87 GB file (peak 2.91 GB) is accepted with 3.4 GB free", r == 0);
    avail_now = (uint64_t)2100 * 1000000;
    r = cc_session_fits(&pf, "x.ifc", (uint64_t)872 * 1000000, &err);
    printf("  2.1 GB free: %s\n", err.msg);
    check_true("and refused with 2.1 GB free, with the numbers", r == -2 && strstr(err.msg, "too large for this machine") && strstr(err.msg, "3.3 GB"));
    return report("memfit");
}
