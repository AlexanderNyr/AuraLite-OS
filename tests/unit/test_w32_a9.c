/* test_w32_a9.c — W32APP_PLAN.md phase W32A-9 host gate.
 *
 * The registry/security engine (w32/src/advapi32.c) amalgamated with the
 * last-error slot (w32_errno.c), the UTF conversions (w32_utf.c) and the
 * real libatls hash sources, against a scratch hive file.
 *
 * What this gate proves that the guest fixture cannot:
 *   * the hive round-trip: every value type survives save + reload
 *     (the guest proves the same across a REBOOT, which is a different
 *     claim -- here the serialize/parse path is isolated)
 *   * the torn-write contract, on all three failure shapes: bad magic,
 *     short payload, corrupted payload (CRC) -- each latches
 *     ERROR_FILE_CORRUPT and never half-reads
 *   * the HKLM write policy and the HKCR merge view, asserted against
 *     the real backing trees rather than through one path
 *   * CryptoAPI digests against the public "abc" vectors AND against
 *     atls one-shots on a 64 KiB pseudorandom buffer, fed incrementally
 *     in ragged chunks (the buffering contract)
 *   * the NTE_BAD_ALGID refusals (SHA-1/MD5/SHA-384: the libatls set is
 *     the contract, the decision is recorded in the plan)
 *   * the failclean finals' exact return + last-error pairs
 */
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define AURALITE_W32_HOST_TEST 1

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "w32/w32_abi.h"
#include "w32/kernel32.h"   /* IsTextUnicode: forwarder prototype + winnls.h flags */
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/advapi32.h"

static int checks, fails;
static void ok(int cond, const char *what, ...) {
    checks++;
    if (!cond) {
        fails++;
        va_list ap;
        va_start(ap, what);
        vprintf(what, ap);
        va_end(ap);
        printf("\n");
    }
}

static uint16_t *W(const char *s) {
    static uint16_t buf[8][1024];
    static int slot;
    uint16_t *out = buf[slot++ & 7];
    size_t n = strlen(s);
    for (size_t i = 0; i <= n; i++) out[i] = (uint16_t)(unsigned char)s[i];
    return out;
}

#define LONG_OK(call) (ok((call) == 0, "%s != ERROR_SUCCESS (line %d)", #call, __LINE__))

/* ---- host shim: the TEB (pre-init path; the fallback slot is used) ------ */
struct w32_teb *w32_teb_self(void) { return 0; }

/* ---- the engine under test ---------------------------------------------- */
#include "../../w32/src/w32_errno.c"
#include "../../w32/src/w32_utf.c"
#include "../../w32/src/advapi32.c"
/* libatls sources are compiled as separate TUs by the Makefile rule (the
 * test_atls_hash pattern): their internal constant tables are file-scope
 * and must not share this translation unit. */

static char scratch_hive[256];

static void use_hive(const char *tag) {
    snprintf(scratch_hive, sizeof scratch_hive, "/tmp/w32a9_hive_%s_%d.bin", tag, (int)getpid());
    unlink(scratch_hive);
    w32_advapi_hive_override = scratch_hive;
    w32_advapi_reset_for_host_test();
}

/* hex helpers for digest comparison */
static int hexeq(const uint8_t *d, int n, const char *hex) {
    for (int i = 0; i < n; i++) {
        int v;
        sscanf(hex + 2 * i, "%2x", &v);
        if (d[i] != v) return 0;
    }
    return 1;
}

int main(void) {
    /* ================= the hive: CRUD over every value type ============= */
    use_hive("crud");

    W32_HKEY k;
    W32_DWORD disp = 0;
    LONG_OK(RegCreateKeyExW(W32_HKEY_CURRENT_USER, W("Software\\PuTTY\\Sessions"),
                            0, NULL, 0, 0, NULL, &k, &disp));
    ok(disp == W32_REG_CREATED_NEW_KEY, "create: disposition NEW (got %u)", disp);
    LONG_OK(RegCloseKey(k));

    LONG_OK(RegCreateKeyExW(W32_HKEY_CURRENT_USER, W("Software\\PuTTY\\Sessions"),
                            0, NULL, 0, 0, NULL, &k, &disp));
    ok(disp == W32_REG_OPENED_EXISTING_KEY, "reopen: disposition EXISTING (got %u)", disp);

    /* case-insensitive open of the same path (ASCII folding) */
    W32_HKEY k2;
    LONG_OK(RegOpenKeyExW(W32_HKEY_CURRENT_USER, W("software\\putty\\sessions"), 0, 0, &k2));
    LONG_OK(RegCloseKey(k2));

    /* REG_SZ */
    static const uint16_t hostname[] = { 'm','y','h','o','s','t',0 };
    LONG_OK(RegSetValueExW(k, W("HostName"), 0, W32_REG_SZ,
                           (const uint8_t *)hostname, sizeof hostname));
    /* REG_DWORD */
    W32_DWORD port = 22;
    LONG_OK(RegSetValueExW(k, W("PortNumber"), 0, W32_REG_DWORD,
                           (const uint8_t *)&port, 4));
    /* REG_QWORD */
    uint64_t qw = 0x1122334455667788ull;
    LONG_OK(RegSetValueExW(k, W("Big"), 0, W32_REG_QWORD, (const uint8_t *)&qw, 8));
    /* REG_BINARY */
    static const uint8_t bin[] = { 1, 2, 3, 4, 255, 0, 9 };
    LONG_OK(RegSetValueExW(k, W("Raw"), 0, W32_REG_BINARY, bin, sizeof bin));
    /* REG_MULTI_SZ (three strings, double NUL) */
    static const uint16_t multi[] = { 'a',0,'b','c',0,0 };
    LONG_OK(RegSetValueExW(k, W("List"), 0, W32_REG_MULTI_SZ,
                           (const uint8_t *)multi, sizeof multi));
    /* REG_EXPAND_SZ */
    static const uint16_t expnd[] = { 'P','A','T','H','%','%' ,':','%','T','E','M','P','%',0 };
    LONG_OK(RegSetValueExW(k, W("Expanded"), 0, W32_REG_EXPAND_SZ,
                           (const uint8_t *)expnd, sizeof expnd));
    /* the default (unnamed) value */
    LONG_OK(RegSetValueExW(k, NULL, 0, W32_REG_SZ,
                           (const uint8_t *)hostname, sizeof hostname));

    /* queries: type + data + size-only + MORE_DATA */
    W32_DWORD type = 0, len = 0;
    uint8_t out[256];
    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, W("HostName"), NULL, &type, out, &len));
    ok(type == W32_REG_SZ, "query HostName: type REG_SZ (got %u)", type);
    ok(len == sizeof hostname, "query HostName: size incl NUL (got %u)", len);
    ok(memcmp(out, hostname, len) == 0, "query HostName: bytes");

    len = 0; out[0] = 0xAA;
    LONG_OK(RegQueryValueExW(k, W("PortNumber"), NULL, &type, NULL, &len));
    ok(type == W32_REG_DWORD && len == 4, "size-only query PortNumber");
    len = 2;
    ok(RegQueryValueExW(k, W("PortNumber"), NULL, NULL, out, &len) ==
       (W32_LONG)W32_ERROR_MORE_DATA, "short buffer -> ERROR_MORE_DATA");
    ok(len == 4, "short buffer: *len = needed (got %u)", len);

    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, W("Big"), NULL, &type, out, &len));
    ok(len == 8 && memcmp(out, &qw, 8) == 0, "query REG_QWORD round-trip");
    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, W("Raw"), NULL, &type, out, &len));
    ok(len == sizeof bin && memcmp(out, bin, sizeof bin) == 0, "query REG_BINARY round-trip");
    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, W("List"), NULL, &type, out, &len));
    ok(len == sizeof multi && memcmp(out, multi, len) == 0, "query REG_MULTI_SZ round-trip");
    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, NULL, NULL, &type, out, &len));
    ok(type == W32_REG_SZ && len == sizeof hostname, "query the default value");

    ok(RegQueryValueExW(k, W("Nope"), NULL, NULL, NULL, &len) ==
       (W32_LONG)W32_ERROR_FILE_NOT_FOUND, "missing value -> FILE_NOT_FOUND");
    LONG_OK(RegCloseKey(k));
    ok(RegOpenKeyExW(W32_HKEY_CURRENT_USER, W("Software\\PuTTY\\Nothere"), 0, 0, &k2) ==
       (W32_LONG)W32_ERROR_FILE_NOT_FOUND, "missing key -> FILE_NOT_FOUND");
    ok(RegCloseKey((W32_HKEY)(uintptr_t)0x80000001u) == 0, "closing a predefined key is a no-op");
#if UINTPTR_MAX > UINT32_MAX
    /* mingw-w64's HKEY_* macros cast signed LONG to a 64-bit pointer.  The
     * fixture's NASM mov ecx, 0x80000002 and our own W32_HKEY_* macros both
     * zero-extend, so they never caught the PE application's real value. */
    W32_HKEY mingw_hklm = (W32_HKEY)(uintptr_t)(int64_t)(int32_t)0x80000002u;
    W32_HKEY mingw_hkcu = (W32_HKEY)(uintptr_t)(int64_t)(int32_t)0x80000001u;
    W32_HKEY mingw_hkcr = (W32_HKEY)(uintptr_t)(int64_t)(int32_t)0x80000000u;
    LONG_OK(RegOpenKeyExA(mingw_hklm, "Software\\AuraLite\\CurrentVersion", 0, 0, &k2));
    LONG_OK(RegCloseKey(k2));
    LONG_OK(RegOpenKeyExA(mingw_hkcu, "Software\\PuTTY", 0, 0, &k2));
    LONG_OK(RegCloseKey(k2));
    LONG_OK(RegCreateKeyExA(mingw_hkcr, "SignedHandle", 0, NULL, 0, 0,
                            NULL, &k2, &disp));
    LONG_OK(RegCloseKey(k2));
    LONG_OK(RegCloseKey(mingw_hklm));
    ok(RegOpenKeyExA((W32_HKEY)(uintptr_t)0x1234567880000002ull,
                     "Software", 0, 0, &k2) == (W32_LONG)W32_ERROR_INVALID_HANDLE,
       "an arbitrary pointer with HKEY low bits is not a predefined key");
#endif
    ok(RegCloseKey((W32_HKEY)(uintptr_t)0x70000000u) ==
       (W32_LONG)W32_ERROR_INVALID_HANDLE, "closing garbage -> INVALID_HANDLE");

    /* ---- enumeration + info (insertion order, documented) -------------- */
    LONG_OK(RegCreateKeyExA(W32_HKEY_CURRENT_USER, "Software\\EnumTest", 0, NULL, 0, 0,
                            NULL, &k, &disp));
    LONG_OK(RegCreateKeyExA(W32_HKEY_CURRENT_USER, "Software\\EnumTest\\gamma", 0, NULL, 0, 0,
                            NULL, &k2, &disp));
    LONG_OK(RegCloseKey(k2));
    LONG_OK(RegCreateKeyExA(W32_HKEY_CURRENT_USER, "Software\\EnumTest\\Alpha", 0, NULL, 0, 0,
                            NULL, &k2, &disp));
    LONG_OK(RegCloseKey(k2));
    LONG_OK(RegCreateKeyExA(W32_HKEY_CURRENT_USER, "Software\\EnumTest\\beta2", 0, NULL, 0, 0,
                            NULL, &k2, &disp));
    LONG_OK(RegCloseKey(k2));

    char name[64];
    LONG_OK(RegEnumKeyA(k, 0, name, sizeof name));
    ok(strcmp(name, "gamma") == 0, "enum[0] is insertion order (got %s)", name);
    LONG_OK(RegEnumKeyA(k, 1, name, sizeof name));
    ok(strcmp(name, "Alpha") == 0, "enum[1] (got %s)", name);
    LONG_OK(RegEnumKeyA(k, 2, name, sizeof name));
    ok(strcmp(name, "beta2") == 0, "enum[2] (got %s)", name);
    ok(RegEnumKeyA(k, 3, name, sizeof name) == (W32_LONG)W32_ERROR_NO_MORE_ITEMS,
       "enum past the end -> NO_MORE_ITEMS");
    ok(RegEnumKeyA(k, 0, name, 3) == (W32_LONG)W32_ERROR_MORE_DATA,
       "enum short buffer -> MORE_DATA");

    uint16_t wname[64];
    W32_DWORD wcap = 64;
    uint64_t ft = 0;
    LONG_OK(RegEnumKeyExW(k, 0, wname, &wcap, NULL, NULL, NULL, &ft));
    ok(wcap == 5 && wname[5] == 0, "RegEnumKeyExW name+cap (got %u)", wcap);
    ok(ft != 0, "RegEnumKeyExW fills a last-write FILETIME");

    W32_DWORD nsub = 0, maxsub = 0, nval = 0, maxvn = 0, maxvl = 0;
    LONG_OK(RegQueryInfoKeyW(k, NULL, NULL, NULL, &nsub, &maxsub, NULL,
                             &nval, &maxvn, &maxvl, NULL, NULL));
    ok(nsub == 3, "info: 3 subkeys (got %u)", nsub);
    ok(maxsub == 5, "info: max subkey name 5 (got %u)", maxsub);
    ok(nval == 0, "info: 0 values (got %u)", nval);
    LONG_OK(RegCloseKey(k));

    /* ---- delete value / delete key (the no-subkeys contract) ----------- */
    LONG_OK(RegOpenKeyExA(W32_HKEY_CURRENT_USER, "Software\\PuTTY\\Sessions", 0, 0, &k));
    LONG_OK(RegDeleteValueW(k, W("Raw")));
    ok(RegQueryValueExW(k, W("Raw"), NULL, NULL, NULL, &len) ==
       (W32_LONG)W32_ERROR_FILE_NOT_FOUND, "deleted value is gone");
    ok(RegDeleteValueW(k, W("Raw")) == (W32_LONG)W32_ERROR_FILE_NOT_FOUND,
       "re-delete value -> FILE_NOT_FOUND");
    LONG_OK(RegCloseKey(k));

    LONG_OK(RegOpenKeyExA(W32_HKEY_CURRENT_USER, "Software\\EnumTest", 0, 0, &k));
    ok(RegDeleteKeyW(k, W("Alpha")) == 0, "delete leaf key Alpha");
    ok(RegDeleteKeyW(k, W("gamma")) == 0, "delete leaf key gamma");
    /* beta2 gets a subkey, then a value: key-with-subkeys must be refused */
    LONG_OK(RegCreateKeyExA(k, "beta2\\child", 0, NULL, 0, 0, NULL, &k2, &disp));
    LONG_OK(RegCloseKey(k2));
    ok(RegDeleteKeyA(k, "beta2") == (W32_LONG)W32_ERROR_ACCESS_DENIED,
       "delete key with subkeys -> ACCESS_DENIED (the documented contract)");
    ok(RegDeleteKeyA(k, "missing") == (W32_LONG)W32_ERROR_FILE_NOT_FOUND,
       "delete missing key -> FILE_NOT_FOUND");
    LONG_OK(RegCloseKey(k));

    /* ================= persistence: reload from the file ================= */
    w32_advapi_reset_for_host_test();          /* drop the tree, re-read */
    LONG_OK(RegOpenKeyExA(W32_HKEY_CURRENT_USER, "Software\\PuTTY\\Sessions", 0, 0, &k));
    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, W("HostName"), NULL, &type, out, &len));
    ok(type == W32_REG_SZ && len == sizeof hostname &&
       memcmp(out, hostname, len) == 0, "reload: REG_SZ survived the round-trip");
    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, W("Big"), NULL, &type, out, &len));
    ok(len == 8 && memcmp(out, &qw, 8) == 0, "reload: REG_QWORD survived");
    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, W("List"), NULL, &type, out, &len));
    ok(len == sizeof multi, "reload: REG_MULTI_SZ survived");
    LONG_OK(RegCloseKey(k));

    /* ================= HKLM: the read-mostly policy ====================== */
    LONG_OK(RegOpenKeyExA(W32_HKEY_LOCAL_MACHINE,
                          "Software\\AuraLite\\CurrentVersion", 0, 0, &k));
    len = sizeof out;
    LONG_OK(RegQueryValueExA(k, "ProductName", NULL, &type, out, &len));
    ok(type == W32_REG_SZ, "HKLM seed: ProductName is REG_SZ");
    static const char product[] = "AuraLite OS (w32 personality)";
    ok(len == sizeof product && memcmp(out, product, sizeof product) == 0,
       "HKLM seed: RegQueryValueExA returns the complete NUL-terminated string");
    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, W("ProductName"), NULL, &type, out, &len));
    ok(len == sizeof product * 2 && out[len - 1] == 0 && out[len - 2] == 0,
       "HKLM seed: UTF-16 string ends in a real NUL");
    LONG_OK(RegCloseKey(k));

    /* A takes/returns UTF-8 bytes; W takes/returns UTF-16LE bytes.  Embedded
     * NULs in MULTI_SZ must survive and non-string types stay byte-exact. */
    LONG_OK(RegCreateKeyExA(W32_HKEY_CURRENT_USER, "Software\\AnsiProbe", 0,
                            NULL, 0, 0, NULL, &k, &disp));
    static const uint8_t cafe[] = { 'c', 'a', 'f', 0xC3, 0xA9, 0 };
    static const uint16_t cafe_w[] = { 'c', 'a', 'f', 0xE9, 0 };
    LONG_OK(RegSetValueExA(k, "Cafe", 0, W32_REG_SZ, cafe, sizeof cafe));
    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, W("Cafe"), NULL, &type, out, &len));
    ok(type == W32_REG_SZ && len == sizeof cafe_w &&
       memcmp(out, cafe_w, sizeof cafe_w) == 0,
       "A string is stored as UTF-16LE for W callers");
    len = 0;
    LONG_OK(RegQueryValueExA(k, "Cafe", NULL, &type, NULL, &len));
    ok(len == sizeof cafe, "A size-only query reports UTF-8 bytes incl NUL");
    memset(out, 0xAA, sizeof out);
    len = 2;
    ok(RegQueryValueExA(k, "Cafe", NULL, &type, out, &len) ==
       (W32_LONG)W32_ERROR_MORE_DATA && len == sizeof cafe && out[0] == 0xAA,
       "A undersized buffer reports UTF-8 length without writing data");
    len = sizeof out;
    LONG_OK(RegQueryValueExA(k, "Cafe", NULL, &type, out, &len));
    ok(len == sizeof cafe && memcmp(out, cafe, sizeof cafe) == 0,
       "A string round-trips every UTF-8 byte");

    static const uint8_t multi_a[] = { 'a', 0, 0xC3, 0xA9, 0, 0 };
    static const uint16_t multi_w[] = { 'a', 0, 0xE9, 0, 0 };
    LONG_OK(RegSetValueExA(k, "Multi", 0, W32_REG_MULTI_SZ, multi_a, sizeof multi_a));
    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, W("Multi"), NULL, &type, out, &len));
    ok(type == W32_REG_MULTI_SZ && len == sizeof multi_w &&
       memcmp(out, multi_w, sizeof multi_w) == 0,
       "A REG_MULTI_SZ preserves embedded NULs in W form");
    len = sizeof out;
    LONG_OK(RegQueryValueExA(k, "Multi", NULL, &type, out, &len));
    ok(len == sizeof multi_a && memcmp(out, multi_a, sizeof multi_a) == 0,
       "A REG_MULTI_SZ round-trips embedded NULs");
    LONG_OK(RegSetValueExA(k, "Expand", 0, W32_REG_EXPAND_SZ, cafe, sizeof cafe));
    len = sizeof out;
    LONG_OK(RegQueryValueExA(k, "Expand", NULL, &type, out, &len));
    ok(type == W32_REG_EXPAND_SZ && len == sizeof cafe &&
       memcmp(out, cafe, sizeof cafe) == 0,
       "A REG_EXPAND_SZ retains unexpanded UTF-8 data");
    static const uint8_t raw_a[] = { 0x00, 0xFF, 0x80, 0x7F };
    LONG_OK(RegSetValueExA(k, "RawA", 0, W32_REG_BINARY, raw_a, sizeof raw_a));
    len = sizeof out;
    LONG_OK(RegQueryValueExA(k, "RawA", NULL, &type, out, &len));
    ok(type == W32_REG_BINARY && len == sizeof raw_a &&
       memcmp(out, raw_a, sizeof raw_a) == 0,
       "non-string A value remains byte-identical");
    static const uint8_t bad_utf8[] = { 0xC0, 0x80, 0 };
    ok(RegSetValueExA(k, "Bad", 0, W32_REG_SZ, bad_utf8, sizeof bad_utf8) ==
       (W32_LONG)W32_ERROR_NO_UNICODE_TRANSLATION,
       "malformed UTF-8 string is refused rather than stored");
    static const uint8_t odd_utf16[] = { 'A', 0, 0xFF };
    LONG_OK(RegSetValueExW(k, W("Odd"), 0, W32_REG_SZ, odd_utf16, sizeof odd_utf16));
    len = sizeof out;
    ok(RegQueryValueExA(k, "Odd", NULL, NULL, out, &len) ==
       (W32_LONG)W32_ERROR_NO_UNICODE_TRANSLATION,
       "odd UTF-16LE byte count is refused on ANSI read");
    LONG_OK(RegCloseKey(k));

    LONG_OK(RegCreateKeyExA(W32_HKEY_LOCAL_MACHINE, "Software\\Installer\\Probe",
                            0, NULL, 0, 0, NULL, &k, &disp));
    LONG_OK(RegCloseKey(k));                    /* \\Software is writable */

    ok(RegCreateKeyExA(W32_HKEY_LOCAL_MACHINE, "System\\Probe", 0, NULL, 0, 0,
                       NULL, &k, &disp) == (W32_LONG)W32_ERROR_ACCESS_DENIED,
       "HKLM outside \\Software: create refused");
    LONG_OK(RegOpenKeyExA(W32_HKEY_LOCAL_MACHINE, "Software", 0, 0, &k));
    ok(RegSetValueExW(k, W("X"), 0, W32_REG_DWORD, (const uint8_t *)&port, 4) == 0,
       "HKLM\\Software: set works");
    LONG_OK(RegDeleteValueW(k, W("X")));
    LONG_OK(RegCloseKey(k));

    /* ================= HKCR: the merge view ============================== */
    LONG_OK(RegCreateKeyExA(W32_HKEY_CURRENT_USER,
                            "Software\\Classes\\.zt\\shell\\open", 0, NULL, 0, 0,
                            NULL, &k, &disp));
    static const uint16_t cmdv[] = { 'o','p','e','n',0 };
    LONG_OK(RegSetValueExW(k, W("cmd"), 0, W32_REG_SZ, (const uint8_t *)cmdv, sizeof cmdv));
    LONG_OK(RegCloseKey(k));
    LONG_OK(RegCreateKeyExA(W32_HKEY_LOCAL_MACHINE,
                            "Software\\Classes\\.zt\\icon", 0, NULL, 0, 0, NULL, &k, &disp));
    LONG_OK(RegCloseKey(k));

    /* read through HKCR: both sides answer; HKCU wins on the same name */
    LONG_OK(RegOpenKeyExA(W32_HKEY_CLASSES_ROOT, ".zt", 0, 0, &k));
    W32_DWORD n2 = 0;
    LONG_OK(RegQueryInfoKeyW(k, NULL, NULL, NULL, &n2, NULL, NULL, NULL, NULL,
                             NULL, NULL, NULL));
    ok(n2 == 2, "HKCR merge: both sides' subkeys enumerate (got %u)", n2);
    LONG_OK(RegCloseKey(k));
    LONG_OK(RegOpenKeyExA(W32_HKEY_CLASSES_ROOT, ".zt\\shell\\open", 0, 0, &k));
    len = sizeof out;
    LONG_OK(RegQueryValueExW(k, W("cmd"), NULL, &type, out, &len));
    ok(len == sizeof cmdv, "HKCR: HKCU half answers");
    LONG_OK(RegCloseKey(k));
    LONG_OK(RegOpenKeyExA(W32_HKEY_CLASSES_ROOT, ".zt\\icon", 0, 0, &k));
    LONG_OK(RegCloseKey(k));

    /* write through HKCR: lands in the HKCU half, verifiable directly */
    LONG_OK(RegCreateKeyExA(W32_HKEY_CLASSES_ROOT, ".zz\\handler", 0, NULL, 0, 0,
                            NULL, &k, &disp));
    LONG_OK(RegCloseKey(k));
    ok(RegOpenKeyExA(W32_HKEY_LOCAL_MACHINE, "Software\\Classes\\.zz\\handler",
                     0, 0, &k2) == (W32_LONG)W32_ERROR_FILE_NOT_FOUND,
       "HKCR write did NOT land in HKLM");
    LONG_OK(RegOpenKeyExA(W32_HKEY_CURRENT_USER, "Software\\Classes\\.zz\\handler",
                     0, 0, &k2));
    LONG_OK(RegCloseKey(k2));

    /* ================= RegGetValueW: coercion + expansion ================= */
    setenv("W32A9VAR", "expanded-value", 1);
    LONG_OK(RegOpenKeyExA(W32_HKEY_CURRENT_USER, "Software\\PuTTY\\Sessions", 0, 0, &k));
    static const uint16_t expsrc[] = { 'p','r','e','%','W','3','2','A','9','V','A','R','%',
                                       'p','o','s','t',0 };
    LONG_OK(RegSetValueExW(k, W("ToExpand"), 0, W32_REG_EXPAND_SZ,
                           (const uint8_t *)expsrc, sizeof expsrc));
    LONG_OK(RegCloseKey(k));

    len = sizeof out;
    LONG_OK(RegGetValueW(W32_HKEY_CURRENT_USER, W("Software\\PuTTY\\Sessions"),
                         W("ToExpand"), W32_RRF_RT_REG_SZ, &type, out, &len));
    ok(type == W32_REG_SZ, "RegGetValueW expanded: type reported REG_SZ");
    static const uint16_t want[] = { 'p','r','e','e','x','p','a','n','d','e','d','-',
                                     'v','a','l','u','e','p','o','s','t',0 };
    ok(len == sizeof want && memcmp(out, want, len) == 0,
       "RegGetValueW expanded: %%W32A9VAR%% expanded");
    len = sizeof out;
    LONG_OK(RegGetValueW(W32_HKEY_CURRENT_USER, W("Software\\PuTTY\\Sessions"),
                         W("PortNumber"), W32_RRF_RT_REG_DWORD, &type, out, &len));
    ok(type == W32_REG_DWORD && len == 4 && port == 22, "RegGetValueW DWORD coercion");
    ok(RegGetValueW(W32_HKEY_CURRENT_USER, W("Software\\PuTTY\\Sessions"),
                    W("PortNumber"), W32_RRF_RT_REG_SZ, &type, out, &len) ==
       (W32_LONG)W32_ERROR_UNSUPPORTED_TYPE, "RegGetValueW wrong mask -> UNSUPPORTED_TYPE");
    ok(RegGetValueW(W32_HKEY_CURRENT_USER, W("Software\\PuTTY\\Sessions"),
                    W("ToExpand"),
                    W32_RRF_RT_REG_SZ | W32_RRF_NOEXPAND, &type, out, &len) ==
       (W32_LONG)W32_ERROR_INVALID_PARAMETER,
       "RegGetValueW NOEXPAND on REG_EXPAND_SZ -> INVALID_PARAMETER");
    LONG_OK(RegFlushKey(W32_HKEY_CURRENT_USER));

    /* ================= the torn-write contract =========================== */
    /* shape 1: bad magic */
    {
        int fd = open(scratch_hive, O_RDWR);
        ok(fd >= 0, "torn: hive file exists");
        uint8_t b;
        lseek(fd, 2, SEEK_SET);
        b = 'X';
        write(fd, &b, 1);
        close(fd);
    }
    w32_advapi_reset_for_host_test();
    ok(RegOpenKeyExA(W32_HKEY_CURRENT_USER, "Software", 0, 0, &k) ==
       (W32_LONG)W32_ERROR_FILE_CORRUPT, "torn magic -> every call FILE_CORRUPT");
    ok(RegCreateKeyExA(W32_HKEY_CURRENT_USER, "X", 0, NULL, 0, 0, NULL, &k, &disp) ==
       (W32_LONG)W32_ERROR_FILE_CORRUPT, "torn magic: create also refused");

    /* shape 2: truncated payload (fresh valid file first) */
    use_hive("torn2");
    LONG_OK(RegCreateKeyExA(W32_HKEY_CURRENT_USER, "Keep\\Me", 0, NULL, 0, 0,
                            NULL, &k, &disp));
    LONG_OK(RegCloseKey(k));
    {
        struct stat st;
        stat(scratch_hive, &st);
        truncate(scratch_hive, st.st_size - 3);   /* bite the payload's tail */
    }
    w32_advapi_reset_for_host_test();
    ok(RegOpenKeyExA(W32_HKEY_CURRENT_USER, "Keep", 0, 0, &k) ==
       (W32_LONG)W32_ERROR_FILE_CORRUPT, "short payload -> FILE_CORRUPT");

    /* shape 3: corrupted payload (CRC mismatch) */
    use_hive("torn3");
    LONG_OK(RegCreateKeyExA(W32_HKEY_CURRENT_USER, "Keep\\Me", 0, NULL, 0, 0,
                            NULL, &k, &disp));
    LONG_OK(RegCloseKey(k));
    {
        int fd = open(scratch_hive, O_RDWR);
        struct stat st;
        fstat(fd, &st);
        lseek(fd, st.st_size - 1, SEEK_SET);
        uint8_t b = 0;
        read(fd, &b, 1);
        b ^= 0xFF;
        lseek(fd, st.st_size - 1, SEEK_SET);
        write(fd, &b, 1);
        close(fd);
    }
    w32_advapi_reset_for_host_test();
    ok(RegOpenKeyExA(W32_HKEY_CURRENT_USER, "Keep", 0, 0, &k) ==
       (W32_LONG)W32_ERROR_FILE_CORRUPT, "CRC mismatch -> FILE_CORRUPT");

    /* shape 4: the empty file is a fresh start (documented) */
    use_hive("empty");
    LONG_OK(RegCreateKeyExA(W32_HKEY_CURRENT_USER, "A", 0, NULL, 0, 0, NULL, &k, &disp));
    LONG_OK(RegCloseKey(k));
    {
        int fd = open(scratch_hive, O_WRONLY | O_TRUNC);
        close(fd);                               /* 0 bytes */
    }
    w32_advapi_reset_for_host_test();
    ok(RegOpenKeyExA(W32_HKEY_CURRENT_USER, "A", 0, 0, &k) ==
       (W32_LONG)W32_ERROR_FILE_NOT_FOUND, "empty file -> fresh start");

    /* ================= SIDs ================================================= */
    {
        W32_SID_IDENTIFIER_AUTHORITY nt = { {0, 0}, {0,0,0,0,0,5} };
        void *admin = NULL, *other = NULL;
        ok(AllocateAndInitializeSid(&nt, 2, 32, 544, 0,0,0,0,0,0, &admin) == 1,
           "AllocateAndInitializeSid");
        ok(GetLengthSid(admin) == 16, "GetLengthSid S-1-5-32-544 = 16 (got %u)",
           GetLengthSid(admin));
        W32_SID copy;
        ok(CopySid(sizeof copy, &copy, admin) == 1, "CopySid");
        ok(EqualSid(&copy, admin) == 1, "EqualSid: copy equals original");
        ok(AllocateAndInitializeSid(&nt, 1, 999, 0,0,0,0,0,0,0, &other) == 1 &&
           EqualSid(admin, other) == 0, "EqualSid: different SIDs differ");
        W32_BOOL member = 0;
        ok(CheckTokenMembership(NULL, admin, &member) == 1 && member == 1,
           "CheckTokenMembership: Administrators TRUE (the documented model)");
        member = 0;
        ok(CheckTokenMembership(NULL, other, &member) == 1 && member == 0,
           "CheckTokenMembership: unknown SID FALSE");
        FreeSid(admin); FreeSid(other);
    }

    /* ================= security descriptors =============================== */
    {
        W32_SECURITY_DESCRIPTOR sd;
        ok(InitializeSecurityDescriptor(&sd, W32_SECURITY_DESCRIPTOR_REVISION) == 1,
           "InitializeSecurityDescriptor");
        ok(sd.revision == 1 && (sd.control & W32_SE_DACL_PRESENT) == 0,
           "fresh descriptor: revision, no DACL");
        ok(SetSecurityDescriptorDacl(&sd, 1, (void *)0x1234, 0) == 1,
           "SetSecurityDescriptorDacl present");
        ok((sd.control & W32_SE_DACL_PRESENT) && sd.dacl == 0x1234,
           "descriptor: DACL recorded");
        ok(SetSecurityDescriptorOwner(&sd, (void *)0x5678, 1) == 1,
           "SetSecurityDescriptorOwner defaulted");
        ok(sd.owner == 0x5678 && (sd.control & W32_SE_OWNER_DEFAULTED),
           "descriptor: owner + defaulted bit");
    }

    /* ================= CryptoAPI ============================================ */
    {
        W32_HCRYPTPROV prov;
        ok(CryptAcquireContextW(&prov, NULL, NULL, W32_PROV_RSA_FULL,
                                W32_CRYPT_VERIFYCONTEXT) == 1, "CryptAcquireContextW");

        /* the NTE_BAD_ALGID refusals first (the recorded decision) */
        W32_HCRYPTHASH h;
        for (uint32_t alg = 0; alg < 3; alg++) {
            uint32_t a = alg == 0 ? W32_CALG_SHA1 : alg == 1 ? W32_CALG_MD5
                                                             : W32_CALG_SHA_384;
            ok(CryptCreateHash(prov, a, NULL, 0, &h) == 0 &&
               w32_get_last_error_raw() == W32_NTE_BAD_ALGID,
               "CryptCreateHash alg %#x -> NTE_BAD_ALGID", a);
        }

        struct { uint32_t alg; const char *vec; int sz; } algs[] = {
            { W32_CALG_SHA_256,
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32 },
            { W32_CALG_SHA_512,
              "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
              "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", 64 },
            { W32_CALG_SHA3_256,
              "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532", 32 },
            { W32_CALG_SHA3_512,
              "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
              "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0", 64 },
        };
        for (size_t i = 0; i < sizeof algs / sizeof algs[0]; i++) {
            ok(CryptCreateHash(prov, algs[i].alg, NULL, 0, &h) == 1,
               "CryptCreateHash alg %#x", algs[i].alg);
            static const char abc[] = "abc";
            ok(CryptHashData(h, (const uint8_t *)abc, 3, 0) == 1, "CryptHashData abc");
            W32_DWORD sz = 0, glen = 64;
            uint8_t digest[64];
            ok(CryptGetHashParam(h, W32_HP_HASHSIZE, (uint8_t *)&sz, &glen, 0) == 1 &&
               (int)sz == algs[i].sz, "HP_HASHSIZE alg %#x = %d (got %u)",
               algs[i].alg, algs[i].sz, sz);
            glen = sizeof digest;
            ok(CryptGetHashParam(h, W32_HP_HASHVAL, digest, &glen, 0) == 1,
               "HP_HASHVAL alg %#x", algs[i].alg);
            ok(hexeq(digest, algs[i].sz, algs[i].vec),
               "digest alg %#x matches the public 'abc' vector", algs[i].alg);
            W32_DWORD algid = 0; glen = 4;
            ok(CryptGetHashParam(h, W32_HP_ALGID, (uint8_t *)&algid, &glen, 0) == 1 &&
               algid == algs[i].alg, "HP_ALGID echoes");
            ok(CryptDestroyHash(h) == 1, "CryptDestroyHash");
        }

        /* ragged incremental feed == atls one-shot on a big buffer */
        static uint8_t big[64 * 1024];
        for (size_t i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 131 + 7);
        uint8_t want[64];
        atls_sha256(big, sizeof big, want);
        ok(CryptCreateHash(prov, W32_CALG_SHA_256, NULL, 0, &h) == 1, "big: create");
        size_t off = 0;
        while (off < sizeof big) {
            size_t chunk = 1 + (off % 977);
            if (off + chunk > sizeof big) chunk = sizeof big - off;
            ok(CryptHashData(h, big + off, (W32_DWORD)chunk, 0) == 1, "big: feed");
            off += chunk;
        }
        uint8_t got[64];
        W32_DWORD glen = 32;
        ok(CryptGetHashParam(h, W32_HP_HASHVAL, got, &glen, 0) == 1, "big: final");
        ok(memcmp(got, want, 32) == 0, "big: ragged feed == atls one-shot");
        ok(CryptDestroyHash(h) == 1, "big: destroy");

        /* provider lifetime */
        ok(CryptReleaseContext(prov, 0) == 1, "CryptReleaseContext");
        ok(CryptReleaseContext(prov, 0) == 0 &&
           w32_get_last_error_raw() == W32_ERROR_INVALID_HANDLE,
           "double release -> INVALID_HANDLE");
    }

    /* ================= RtlGenRandom ========================================= */
    {
        uint8_t a[64], b[64];
        memset(a, 0, sizeof a); memset(b, 0, sizeof b);
        ok(SystemFunction036(a, sizeof a) == 1, "SystemFunction036 fills");
        ok(SystemFunction036(b, sizeof b) == 1, "SystemFunction036 fills again");
        ok(memcmp(a, b, sizeof a) != 0, "two draws differ");
        int nz = 0;
        for (size_t i = 0; i < sizeof a; i++) if (a[i]) nz++;
        ok(nz > 32, "the draw is not zeros (nz=%d)", nz);
        ok(SystemFunction036(a, 0) == 1, "zero length is a valid no-op fill");
    }

    /* ================= identity ============================================= */
    {
        char user[16];
        W32_ULONG n = sizeof user;
        ok(GetUserNameA(user, &n) == 1 && strcmp(user, "user") == 0 && n == 5,
           "GetUserNameA 'user' (len incl NUL)");
        n = 3;
        ok(GetUserNameA(user, &n) == 0 && n == 5 &&
           w32_get_last_error_raw() == W32_ERROR_INSUFFICIENT_BUFFER,
           "GetUserNameA short buffer -> INSUFFICIENT_BUFFER + needed");
        uint16_t wuser[16];
        n = 16;
        ok(GetUserNameW(wuser, &n) == 1 && n == 5 && wuser[4] == 0,
           "GetUserNameW");

        /* The engine is kernel32_loc.c's (bound under ADVAPI32, the
         * forwarder shape); *result is the in-mask of tests to run. */
        int r;
        static const uint8_t le_ascii[] = { 'h',0,'e',0,'l',0,'l',0,'o',0 };
        r = 0xFFFF;                  /* all tests */
        ok(IsTextUnicode(le_ascii, 10, &r) == 1 &&
           (r & W32_IS_TEXT_UNICODE_ASCII16) &&
           (r & W32_IS_TEXT_UNICODE_STATISTICS),
           "IsTextUnicode: UTF-16LE text -> TRUE (ASCII16+STATISTICS)");
        ok(IsTextUnicode(le_ascii, 10, NULL) == 1,
           "IsTextUnicode: NULL flags runs the full set -> TRUE");
        r = W32_IS_TEXT_UNICODE_ASCII16;   /* the caller's mask is honored */
        ok(IsTextUnicode(le_ascii, 10, &r) == 1 &&
           r == W32_IS_TEXT_UNICODE_ASCII16,
           "IsTextUnicode: the in-mask limits the tests run");
        r = 0;                       /* zero mask: no test can pass */
        ok(IsTextUnicode(le_ascii, 10, &r) == 0,
           "IsTextUnicode: a zero in-mask runs no tests -> FALSE");
        static const uint8_t raw_ascii[] = { 'a','b','c','d' };
        r = 0xFFFF;
        ok(IsTextUnicode(raw_ascii, 4, &r) == 0,
           "IsTextUnicode: packed ASCII bytes -> FALSE");
        static const uint8_t odd[] = { 'a','b','c' };
        r = 0xFFFF;
        ok(IsTextUnicode(odd, 3, &r) == 0 &&
           (r & W32_IS_TEXT_UNICODE_ODD_LENGTH),
           "IsTextUnicode: odd length -> FALSE, ODD_LENGTH set");
        static const uint8_t bom[] = { 0xFF, 0xFE, 'h', 0, 'i', 0 };
        r = 0xFFFF;
        ok(IsTextUnicode(bom, 6, &r) == 1 &&
           (r & W32_IS_TEXT_UNICODE_SIGNATURE),
           "IsTextUnicode: BOM detected, SIGNATURE reported");
        static const uint8_t bad[] = { 0xFE, 0xFF, 0x00, 0xD8 }; /* BE BOM + lone high surrogate */
        r = 0xFFFF;
        ok(IsTextUnicode(bad, 4, &r) == 0 &&
           (r & W32_IS_TEXT_UNICODE_ILLEGAL_CHARS) &&
           (r & W32_IS_TEXT_UNICODE_REVERSE_SIGNATURE),
           "IsTextUnicode: lone surrogate -> FALSE, ILLEGAL_CHARS set");
    }

    /* ================= the failclean finals ================================= */
    {
        void *lsah = (void *)0x1;
        ok(LsaOpenPolicy(NULL, NULL, 0, &lsah) == W32_STATUS_ACCESS_DENIED,
           "LsaOpenPolicy -> STATUS_ACCESS_DENIED");
        ok(lsah == NULL, "LsaOpenPolicy: handle stays NULL");
        ok(LsaAddAccountRights(NULL, NULL, NULL, 0) == W32_STATUS_ACCESS_DENIED,
           "LsaAddAccountRights -> STATUS_ACCESS_DENIED");
        ok(LsaClose(NULL) == W32_STATUS_INVALID_HANDLE,
           "LsaClose -> STATUS_INVALID_HANDLE");
        ok(LookupAccountNameW(NULL, W("user"), NULL, NULL, NULL, NULL, NULL) == 0 &&
           w32_get_last_error_raw() == W32_ERROR_NONE_MAPPED,
           "LookupAccountNameW -> FALSE + ERROR_NONE_MAPPED");
        ok(LookupPrivilegeValueW(NULL, W("SeBackupPrivilege"), NULL) == 0 &&
           w32_get_last_error_raw() == W32_ERROR_NO_SUCH_PRIVILEGE,
           "LookupPrivilegeValueW -> FALSE + ERROR_NO_SUCH_PRIVILEGE");
        W32_HANDLE tok = (W32_HANDLE)0x1;
        ok(OpenProcessToken((W32_HANDLE)-1, 0, &tok) == 0 &&
           tok == NULL && w32_get_last_error_raw() == W32_ERROR_NO_TOKEN,
           "OpenProcessToken -> FALSE + ERROR_NO_TOKEN, token NULL");
        ok(AdjustTokenPrivileges(NULL, 0, NULL, 0, NULL, NULL) == 1 &&
           w32_get_last_error_raw() == W32_ERROR_NOT_ALL_ASSIGNED,
           "AdjustTokenPrivileges -> TRUE + ERROR_NOT_ALL_ASSIGNED (the degradation)");
        ok(GetFileSecurityW(W("C:\\x"), 0, NULL, 0, NULL) == 0 &&
           w32_get_last_error_raw() == W32_ERROR_NOT_SUPPORTED,
           "GetFileSecurityW -> FALSE + ERROR_NOT_SUPPORTED");
        ok(SetFileSecurityW(W("C:\\x"), 0, NULL) == 0 &&
           w32_get_last_error_raw() == W32_ERROR_NOT_SUPPORTED,
           "SetFileSecurityW -> FALSE + ERROR_NOT_SUPPORTED");
    }

    unlink(scratch_hive);
    w32_advapi_hive_override = NULL;

    printf("w32_a9: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
