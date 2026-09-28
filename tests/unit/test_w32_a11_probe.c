/* W32A-11 activation gate (host, ASan/UBSan): CLSIDFromProgID resolves the
 * committed ProgID table -- the W32A-9 hive's HKCR view, seeded empty and
 * managed through Reg* -- and CoCreateInstance serves the committed (empty)
 * in-process activation table with typed refusals.  The GUIDs/ProgIDs here
 * are SYNTHETIC test vectors, NOT observations from any pinned application:
 * the pinned-app probe recorded zero CLSID/IID pairs, so the activation
 * table MUST stay empty (pinned by W32_COM_CLASS_COUNT below and by the
 * receipts).  SPDX-License-Identifier: Apache-2.0. */
#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#define AURALITE_W32_HOST_TEST 1

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "w32/w32_abi.h"
#include "w32/kernel32.h"
#include "w32/w32_errno.h"
#include "w32/w32_utf.h"
#include "w32/advapi32.h"
#include "w32/ole32.h"

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

/* ---- host shims ----------------------------------------------------------
 * w32_teb_self: pre-init path (fallback last-error slot), as in the A9 gate.
 * GetCurrentThreadId: ole32.c binds COM depth to a stable TID. */
struct w32_teb *w32_teb_self(void) { return 0; }
static _Thread_local W32_DWORD tid;
W32ABI W32_DWORD GetCurrentThreadId(void) { return tid; }

/* ---- the engines under test (libatls objects linked as separate TUs) ----- */
#include "../../w32/src/w32_errno.c"
#include "../../w32/src/w32_utf.c"
#include "../../w32/src/advapi32.c"
#include "../../w32/src/ole32.c"

static char scratch_hive[256];
static void use_hive(const char *tag) {
    snprintf(scratch_hive, sizeof scratch_hive, "/tmp/w32a11_hive_%s_%d.bin",
             tag, (int)getpid());
    unlink(scratch_hive);
    w32_advapi_hive_override = scratch_hive;
    w32_advapi_reset_for_host_test();
}

static const W32_GUID g1 = { 0x12345678, 0x9abc, 0xdef0,
                             { 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88 } };
static const W32_GUID iid_unk = { 0, 0, 0, { 0xc0,0,0,0,0,0,0,0x46 } };

int main(void) {
    tid = 1;

    /* ================= helpers: GUID text <-> bytes ======================= */
    char text[37];
    w32a11_guid_text(&g1, text);
    ok(!strcmp(text, "12345678-9abc-def0-1122-334455667788"),
       "guid_text canonical form: %s", text);
    w32a11_guid_text(&iid_unk, text);
    ok(!strcmp(text, "00000000-0000-0000-c000-000000000046"),
       "guid_text IID zero data4 prefix: %s", text);
    w32a11_guid_text(NULL, text);
    ok(!strcmp(text, "(null)"), "guid_text NULL spelled (null): %s", text);
    W32_GUID back;
    ok(w32a11_guid_parse(W("{12345678-9abc-def0-1122-334455667788}"), &back) &&
       !memcmp(&back, &g1, sizeof back), "parse round-trips the bytes");
    ok(w32a11_guid_parse(W("{12345678-9ABC-DEF0-1122-334455667788}"), &back) &&
       !memcmp(&back, &g1, sizeof back), "parse accepts uppercase hex");
    ok(!w32a11_guid_parse(W("12345678-9abc-def0-1122-334455667788"), &back),
       "parse refuses missing braces");
    ok(!w32a11_guid_parse(W("{123456789abc-def0-1122-334455667788}"), &back),
       "parse refuses a missing dash");
    ok(!w32a11_guid_parse(W("{1234567g-9abc-def0-1122-334455667788}"), &back),
       "parse refuses a non-hex digit");
    ok(!w32a11_guid_parse(W("{12345678-9abc-def0-1122-33445566778}"), &back),
       "parse refuses 31 nibbles");
    ok(!w32a11_guid_parse(W("{12345678-9abc-def0-1122-33445566778800}"), &back),
       "parse refuses trailing garbage");
    ok(!w32a11_guid_parse(NULL, &back), "parse refuses NULL");

    /* ============ helpers: lossless one-line ProgID escaping ============= */
    uint16_t progid[] = { 'N','P','P','.',0x00e9,0xd840,0xdc00,'\\','\n','=',' ',0 };
    char name[W32A11_PROGID_LOG_UNITS * 6u + 1u];
    ok(w32a11_progid_escape(progid, name) == 0 &&
       !strcmp(name, "NPP.\\u00e9\\ud840\\udc00\\u005c\\u000a\\u003d\\u0020"),
       "progid_escape reversible single line: %s", name);
    ok(w32a11_progid_escape(NULL, name) == 0 && !strcmp(name, "(null)"),
       "progid_escape NULL spelled (null)");
    uint16_t long_id[W32A11_PROGID_LOG_UNITS + 2u];
    for (unsigned i = 0; i < W32A11_PROGID_LOG_UNITS + 1u; i++) long_id[i] = 'x';
    long_id[W32A11_PROGID_LOG_UNITS + 1u] = 0;
    ok(w32a11_progid_escape(long_id, name) == 1 &&
       strlen(name) == W32A11_PROGID_LOG_UNITS, "escape marks truncation");
    long_id[W32A11_PROGID_LOG_UNITS] = 0;
    ok(w32a11_progid_escape(long_id, name) == 0, "exact bound is complete");

    /* ================= CLSIDFromProgID over the real hive ================= */
    use_hive("clsid");
    W32_GUID out;
    memset(&out, 0xa5, sizeof out);
    ok(CLSIDFromProgID(NULL, &out) == W32_COM_E_INVALIDARG &&
       w32_get_last_error_raw() == W32_ERROR_INVALID_PARAMETER,
       "NULL ProgID: E_INVALIDARG + ERROR_INVALID_PARAMETER");
    uint8_t zeros[16] = { 0 };
    ok(!memcmp(&out, zeros, 16), "NULL ProgID leaves a zeroed CLSID");
    memset(&out, 0xa5, sizeof out);
    ok(CLSIDFromProgID(W(""), &out) == W32_COM_CO_E_CLASSSTRING &&
       w32_get_last_error_raw() == W32_ERROR_INVALID_NAME &&
       !memcmp(&out, zeros, 16), "empty ProgID: CLASSSTRING + INVALID_NAME");
    uint16_t longest[W32A11_PROGID_MAX_UNITS + 1u];
    for (unsigned i = 0; i < W32A11_PROGID_MAX_UNITS; i++) longest[i] = 'x';
    longest[W32A11_PROGID_MAX_UNITS] = 0;
    ok(CLSIDFromProgID(longest, &out) == W32_COM_CO_E_CLASSSTRING &&
       w32_get_last_error_raw() == W32_ERROR_INVALID_NAME,
       "unbounded ProgID: CLASSSTRING + INVALID_NAME (no unbounded scan)");
    longest[W32A11_PROGID_MAX_UNITS - 1u] = 0;   /* 255 units: legal spelling */
    ok(CLSIDFromProgID(longest, &out) == W32_COM_CO_E_CLASSSTRING &&
       w32_get_last_error_raw() == W32_ERROR_FILE_NOT_FOUND &&
       !memcmp(&out, zeros, 16), "255-unit unregistered ProgID: FILE_NOT_FOUND");

    /* The committed ProgID table is the hive: seed HKCR\<progid>\CLSID
     * default value through Reg*, resolve it back through the API. */
    W32_HKEY k = 0;
    W32_DWORD disp = 0;
    ok(RegCreateKeyExW(W32_HKEY_CLASSES_ROOT, W("AuraW32A11.Probe\\CLSID"),
                       0, NULL, 0, 0, NULL, &k, &disp) == 0,
       "create HKCR\\AuraW32A11.Probe\\CLSID");
    static const uint16_t g1_text[] = {
        '{','1','2','3','4','5','6','7','8','-','9','a','b','c','-',
        'd','e','f','0','-','1','1','2','2','-','3','3','4','4','5',
        '5','6','6','7','7','8','8','}',0 };
    ok(RegSetValueExW(k, NULL, 0, W32_REG_SZ,
                      (const uint8_t *)g1_text, sizeof g1_text) == 0,
       "write the default CLSID string");
    ok(RegCloseKey(k) == 0, "close key");
    w32_set_last_error(0xdeadbeefu);
    ok(CLSIDFromProgID(W("AuraW32A11.Probe"), &out) == W32_COM_S_OK &&
       !memcmp(&out, &g1, sizeof g1), "registered ProgID resolves");
    ok(w32_get_last_error_raw() == 0xdeadbeefu,
       "success leaves last-error untouched");
    ok(CLSIDFromProgID(W("auraw32a11.PROBE"), &out) == W32_COM_S_OK &&
       !memcmp(&out, &g1, sizeof g1), "ASCII case-insensitive resolve");

    /* The HKCR merge view through the same API: HKCU half wins over HKLM. */
    ok(RegCreateKeyExW(W32_HKEY_LOCAL_MACHINE, W("Software\\Classes\\AuraW32A11.Merge\\CLSID"),
                       0, NULL, 0, 0, NULL, &k, &disp) == 0, "seed HKLM half");
    static const uint16_t loser_text[] = {
        '{','a','a','a','a','a','a','a','a','-','1','1','1','1','-',
        '1','1','1','1','-','1','1','1','1','-','1','1','1','1','1',
        '1','1','1','1','1','1','1','}',0 };
    ok(RegSetValueExW(k, NULL, 0, W32_REG_SZ,
                      (const uint8_t *)loser_text, sizeof loser_text) == 0 &&
       RegCloseKey(k) == 0, "write HKLM CLSID");
    W32_GUID loser = { 0xaaaaaaaa, 0x1111, 0x1111, { 0x11,0x11,0x11,0x11,0x11,0x11,0x11,0x11 } };
    ok(CLSIDFromProgID(W("AuraW32A11.Merge"), &out) == W32_COM_S_OK &&
       !memcmp(&out, &loser, sizeof loser), "HKLM half visible first");
    ok(RegCreateKeyExW(W32_HKEY_CURRENT_USER, W("Software\\Classes\\AuraW32A11.Merge\\CLSID"),
                       0, NULL, 0, 0, NULL, &k, &disp) == 0, "seed HKCU half");
    ok(RegSetValueExW(k, NULL, 0, W32_REG_SZ,
                      (const uint8_t *)g1_text, sizeof g1_text) == 0 &&
       RegCloseKey(k) == 0, "write HKCU CLSID");
    ok(CLSIDFromProgID(W("AuraW32A11.Merge"), &out) == W32_COM_S_OK &&
       !memcmp(&out, &g1, sizeof g1), "HKCU half wins the merge");

    /* Registered-but-malformed: the value IS there and is not a CLSID. */
    ok(RegCreateKeyExW(W32_HKEY_CLASSES_ROOT, W("AuraW32A11.Bad\\CLSID"),
                       0, NULL, 0, 0, NULL, &k, &disp) == 0, "create Bad key");
    ok(RegSetValueExW(k, NULL, 0, W32_REG_SZ,
                      (const uint8_t *)W("not-a-guid"), 22) == 0 &&
       RegCloseKey(k) == 0, "write malformed text");
    memset(&out, 0xa5, sizeof out);
    ok(CLSIDFromProgID(W("AuraW32A11.Bad"), &out) == W32_COM_CO_E_CLASSSTRING &&
       w32_get_last_error_raw() == W32_ERROR_INVALID_DATA &&
       !memcmp(&out, zeros, 16), "malformed stored GUID: INVALID_DATA, zeroed");
    ok(RegCreateKeyExW(W32_HKEY_CLASSES_ROOT, W("AuraW32A11.NoTerm\\CLSID"),
                       0, NULL, 0, 0, NULL, &k, &disp) == 0, "create NoTerm key");
    ok(RegSetValueExW(k, NULL, 0, W32_REG_SZ,
                      (const uint8_t *)g1_text, sizeof g1_text - 2) == 0 &&
       RegCloseKey(k) == 0, "write unterminated text");
    memset(&out, 0xa5, sizeof out);
    ok(CLSIDFromProgID(W("AuraW32A11.NoTerm"), &out) == W32_COM_CO_E_CLASSSTRING &&
       w32_get_last_error_raw() == W32_ERROR_INVALID_DATA &&
       !memcmp(&out, zeros, 16), "unterminated stored text refused");
    W32_DWORD dword_val = 42;
    ok(RegCreateKeyExW(W32_HKEY_CLASSES_ROOT, W("AuraW32A11.WrongType\\CLSID"),
                       0, NULL, 0, 0, NULL, &k, &disp) == 0, "create WrongType key");
    ok(RegSetValueExW(k, NULL, 0, W32_REG_DWORD,
                      (const uint8_t *)&dword_val, sizeof dword_val) == 0 &&
       RegCloseKey(k) == 0, "write REG_DWORD instead of REG_SZ");
    ok(CLSIDFromProgID(W("AuraW32A11.WrongType"), &out) == W32_COM_CO_E_CLASSSTRING &&
       w32_get_last_error_raw() == W32_ERROR_INVALID_DATA,
       "wrong value type refused, not reinterpreted");

    /* ================== CoCreateInstance typed refusals =================== */
    void *obj = (void *)(uintptr_t)0x1234;
    ok(CoCreateInstance(&g1, NULL, 1, &iid_unk, NULL) == W32_COM_E_POINTER &&
       w32_get_last_error_raw() == W32_ERROR_INVALID_PARAMETER,
       "NULL out: E_POINTER + ERROR_INVALID_PARAMETER");
    ok(CoCreateInstance(NULL, NULL, 1, &iid_unk, &obj) == W32_COM_E_INVALIDARG &&
       w32_get_last_error_raw() == W32_ERROR_INVALID_PARAMETER && obj == NULL,
       "NULL CLSID: E_INVALIDARG, out cleared");
    obj = (void *)(uintptr_t)0x1234;
    ok(CoCreateInstance(&g1, NULL, 1, NULL, &obj) == W32_COM_E_INVALIDARG &&
       w32_get_last_error_raw() == W32_ERROR_INVALID_PARAMETER && obj == NULL,
       "NULL IID: E_INVALIDARG, out cleared");
    obj = (void *)(uintptr_t)0x1234;
    ok(CoCreateInstance(&g1, (void *)1, 1, &iid_unk, &obj) ==
       W32_COM_CLASS_E_NOAGGREGATION &&
       w32_get_last_error_raw() == W32_ERROR_NOT_SUPPORTED && obj == NULL,
       "aggregation: CLASS_E_NOAGGREGATION by name");
    obj = (void *)(uintptr_t)0x1234;
    ok(CoCreateInstance(&g1, NULL, 0, &iid_unk, &obj) == W32_COM_E_INVALIDARG &&
       w32_get_last_error_raw() == W32_ERROR_INVALID_PARAMETER && obj == NULL,
       "ctx==0: E_INVALIDARG, no silent default context");
    obj = (void *)(uintptr_t)0x1234;
    ok(CoCreateInstance(&g1, NULL, W32_CLSCTX_LOCAL_SERVER, &iid_unk, &obj) ==
       W32_COM_E_NOTIMPL &&
       w32_get_last_error_raw() == W32_ERROR_NOT_SUPPORTED && obj == NULL,
       "out-of-process context: named E_NOTIMPL, never coerced");
    obj = (void *)(uintptr_t)0x1234;
    ok(CoCreateInstance(&g1, NULL, W32_CLSCTX_INPROC_SERVER | 0x100u, &iid_unk, &obj) ==
       W32_COM_E_NOTIMPL, "unknown context bits are not masked away");
    obj = (void *)(uintptr_t)0x1234;
    ok(CoCreateInstance(&g1, NULL, W32_CLSCTX_INPROC_SERVER, &iid_unk, &obj) ==
       W32_COM_REGDB_E_CLASSNOTREG &&
       w32_get_last_error_raw() == W32_ERROR_FILE_NOT_FOUND && obj == NULL,
       "in-process activation: REGDB_E_CLASSNOTREG, out cleared");
    ok(W32_COM_CLASS_COUNT == 0,
       "activation table pinned EMPTY: no unobserved CLSID/IID pair exists");

    /* ============== stdout probe lines (the runtime evidence) ============= */
    int saved = dup(STDOUT_FILENO);
    FILE *cap = tmpfile();
    ok(saved >= 0 && cap != NULL, "stdout capture armed");
    if (saved >= 0 && cap) {
        fflush(stdout);
        ok(dup2(fileno(cap), STDOUT_FILENO) >= 0, "stdout redirected");
        memset(&out, 0xa5, sizeof out);
        W32_DWORD rc = CLSIDFromProgID(progid, &out);
        void *o2 = NULL;
        W32_DWORD rc2 = CoCreateInstance(&g1, NULL, 1, &iid_unk, &o2);
        fflush(stdout);
        ok(dup2(saved, STDOUT_FILENO) >= 0, "stdout restored");
        close(saved);
        rewind(cap);
        char log[4096];
        size_t got = fread(log, 1, sizeof log - 1, cap);
        log[got] = 0;
        fclose(cap);
        ok(rc == W32_COM_CO_E_CLASSSTRING && rc2 == W32_COM_REGDB_E_CLASSNOTREG,
           "captured calls returned the typed refusals");
        ok(strstr(log, "w32a11-progid-probe: UTF16=NPP.\\u00e9\\ud840\\udc00\\u005c\\u000a\\u003d\\u0020 truncated=0 result=0x800401f3\n") != NULL,
           "progid probe line: lossless, bounded, outcome-stamped:\n%s", log);
        ok(strstr(log, "w32a11-clsid-probe: CLSID=12345678-9abc-def0-1122-334455667788 IID=00000000-0000-0000-c000-000000000046 CLSCTX=1\n") != NULL,
           "clsid probe line keeps its committed format:\n%s", log);
    }
    unlink(scratch_hive);
    fprintf(stderr, "w32a11-probe: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
