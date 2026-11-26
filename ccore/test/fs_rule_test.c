/* Suite fs_rule: the lexical part of the file dialog's path rule (host/fs_rule.h, ticket FIXDOC),
 * with the POSIX and the Windows forms on every system: network and device paths, "..", streams,
 * the case of a Windows root. test/fs_test.mjs checks the whole rule through the host. */
#include "../host/fs_rule.h"
#include "check.h"

static void norm(const char *what, const char *p, int win, int want_rc, const char *want) {
    char out[FSR_MAX + 2];
    int rc = fsr_normalise(p, win, out, sizeof out);
    char name[256];
    snprintf(name, sizeof name, "%s: %s (%s) rc", what, p, win ? "windows" : "posix");
    if (rc != want_rc) printf("  rc %d, want %d\n", rc, want_rc);
    check_true(name, rc == want_rc);
    if (want && rc == want_rc) {
        snprintf(name, sizeof name, "%s: %s (%s)", what, p, win ? "windows" : "posix");
        check_eq(name, out, strlen(out), want, strlen(want));
    }
}

int main(void) {
    /* POSIX */
    norm("root", "/", 0, FSR_OK, "/");
    norm("plain", "/home/u/x", 0, FSR_OK, "/home/u/x");
    norm("empty and dot segments", "//home///u/./x/", 0, FSR_OK, "/home/u/x");
    norm("dot dot", "/home/u/../v", 0, FSR_OK, "/home/v");
    norm("dot dot to the root", "/home/..", 0, FSR_OK, "/");
    norm("dot dot above the root", "/..", 0, FSR_BAD, NULL);
    norm("dot dot above the root, later", "/a/../../b", 0, FSR_BAD, NULL);
    norm("relative", "home/u", 0, FSR_BAD, NULL);
    norm("empty", "", 0, FSR_BAD, NULL);
    norm("a drive is relative on POSIX", "C:/Users", 0, FSR_BAD, NULL);
    norm("a backslash is a name on POSIX", "/a\\b", 0, FSR_OK, "/a\\b");
    norm("a name with three dots", "/a/.../b", 0, FSR_OK, "/a/.../b");
    {
        static char big[FSR_MAX + 3];
        memset(big, 'a', sizeof big - 1);
        big[0] = '/';
        norm("over 4096 bytes", big, 0, FSR_BAD, NULL);
    }
    /* Windows: drives */
    norm("drive, backslashes", "C:\\Users\\a", 1, FSR_OK, "C:/Users/a");
    norm("drive root", "c:\\", 1, FSR_OK, "c:/");
    norm("drive, mixed separators and dot dot", "C:/Users\\a\\..\\b\\.", 1, FSR_OK, "C:/Users/b");
    norm("drive, above the root", "C:\\..\\Windows", 1, FSR_BAD, NULL);
    norm("drive without a separator", "C:Users", 1, FSR_BAD, NULL);
    norm("drive alone", "C:", 1, FSR_BAD, NULL);
    norm("no drive", "\\Users\\a", 1, FSR_BAD, NULL);
    norm("no drive, slash", "/Users/a", 1, FSR_BAD, NULL);
    norm("a stream", "C:\\a\\file.ifc:secret", 1, FSR_BAD, NULL);
    norm("a trailing dot", "C:\\a\\b.\\c", 1, FSR_BAD, NULL);
    norm("a trailing space", "C:\\a\\b \\c", 1, FSR_BAD, NULL);
    norm("dot dot space", "C:\\a\\.. \\c", 1, FSR_BAD, NULL);
    /* Windows: network and device paths */
    norm("UNC", "\\\\server\\share\\x", 1, FSR_UNC, "//server/share/x");
    norm("UNC with slashes", "//server/share", 1, FSR_UNC, "//server/share");
    norm("UNC mixed", "/\\server\\share\\x\\..\\y", 1, FSR_UNC, "//server/share/y");
    norm("UNC above the share", "\\\\server\\share\\..\\other", 1, FSR_BAD, NULL);
    norm("UNC server alone", "\\\\server", 1, FSR_BAD, NULL);
    norm("UNC, an IP", "\\\\10.0.0.1\\c$", 1, FSR_UNC, "//10.0.0.1/c$");
    norm("device", "\\\\.\\PhysicalDrive0", 1, FSR_DEVICE, NULL);
    norm("device, slashes", "//./pipe/x", 1, FSR_DEVICE, NULL);
    norm("long path prefix", "\\\\?\\C:\\Users", 1, FSR_DEVICE, NULL);
    norm("long UNC prefix", "\\\\?\\UNC\\server\\share", 1, FSR_DEVICE, NULL);
    norm("NT namespace", "\\??\\C:\\Users", 1, FSR_DEVICE, NULL);
    norm("device alone", "\\\\.", 1, FSR_DEVICE, NULL);
    /* fsr_under */
    check_true("under: the root itself", fsr_under("/home/u", "/home/u", 0));
    check_true("under: below", fsr_under("/home/u/x", "/home/u", 0));
    check_true("under: a sibling with the root's prefix is not", !fsr_under("/home/uv", "/home/u", 0));
    check_true("under: the POSIX root", fsr_under("/x", "/", 0));
    check_true("under: POSIX compares case", !fsr_under("/Home/u/x", "/home/u", 0));
    check_true("under: Windows compares without case", fsr_under("c:/users/A/x", "C:/Users/a", 1));
    check_true("under: a drive root", fsr_under("C:/x", "C:/", 1));
    check_true("under: another drive", !fsr_under("D:/x", "C:/", 1));
    check_true("under: a UNC path under a drive root", !fsr_under("//server/share", "C:/", 1));
    return report("fs_rule");
}
