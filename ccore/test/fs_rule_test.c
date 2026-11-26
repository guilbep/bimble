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
    /* FIXSES: DOS device names, a segment anywhere, any case, extension, trailing dots or spaces */
    norm("device name CON", "C:\\a\\CON", 1, FSR_DEVICE, NULL);
    norm("device name con.txt", "C:\\a\\con.txt", 1, FSR_DEVICE, NULL);
    norm("device name NUL, a folder", "C:/NUL/x.ifc", 1, FSR_DEVICE, NULL);
    norm("device name Aux.tar.gz", "C:\\Aux.tar.gz", 1, FSR_DEVICE, NULL);
    norm("device name prn, trailing dots", "C:\\a\\prn...", 1, FSR_DEVICE, NULL);
    norm("device name COM1 with a space before the extension", "C:\\a\\COM1 .ifc", 1, FSR_DEVICE, NULL);
    norm("device name lpt9", "C:\\a\\lpt9", 1, FSR_DEVICE, NULL);
    norm("device name COM, superscript 1", "C:\\a\\COM\xc2\xb9.txt", 1, FSR_DEVICE, NULL);
    norm("device name CONIN$", "C:\\a\\conin$", 1, FSR_DEVICE, NULL);
    norm("device name CONOUT$.log", "C:\\a\\CONOUT$.log", 1, FSR_DEVICE, NULL);
    norm("device name in a UNC path", "\\\\server\\share\\nul", 1, FSR_DEVICE, NULL);
    norm("not a device: CONSOLE", "C:\\a\\CONSOLE", 1, FSR_OK, "C:/a/CONSOLE");
    norm("not a device: COM0", "C:\\a\\COM0", 1, FSR_OK, "C:/a/COM0");
    norm("not a device: COM10", "C:\\a\\COM10", 1, FSR_OK, "C:/a/COM10");
    norm("not a device: a.con", "C:\\a\\a.con", 1, FSR_OK, "C:/a/a.con");
    norm("not a device: xNUL", "C:\\a\\xNUL", 1, FSR_OK, "C:/a/xNUL");
    norm("a device name is a name on POSIX", "/a/CON", 0, FSR_OK, "/a/CON");
    check_true("fsr_dos_device: Nul", fsr_dos_device("Nul", 3));
    check_true("fsr_dos_device: \"nul  .x\"", fsr_dos_device("nul  .x", 7));
    check_true("fsr_dos_device: LPT, superscript 3", fsr_dos_device("LPT\xc2\xb3", 5));
    check_true("fsr_dos_device: not LPT, acute accent", !fsr_dos_device("LPT\xc2\xb4", 5));
    check_true("fsr_dos_device: not an empty name", !fsr_dos_device("", 0));
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
