/* tests/unit/test_w32_a1_report.c — the manifest matrix + --emit-report.
 *
 * The matrix feeds embedded strings to the REAL w32_manifest_parse: every
 * branch the gate depends on (v6/v5 select, the three exec levels, both
 * dpi spellings, supportedOS counting, BOM/prolog/apostrophes, the 512
 * and 256 scan windows, the three malformed shapes) is pinned without a
 * guest.  The report prints one row per fixture import; it must stay
 * byte-stable for a fixed tree (no addresses, no timestamps, file order).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_w32_a1.h"
#include "w32/w32_pe.h"
#include "w32/w32_manifest.h"

static void expect_parse(const char *label, const char *xml, int want_rc,
                         int want_major, int want_exec, int want_dpi,
                         int want_supported) {
    w32_manifest_t mf;
    int rc = w32_manifest_parse((const unsigned char *)xml, strlen(xml), &mf);
    A1_CHECK(rc == want_rc, "manifest %s: rc %d", label, want_rc);
    if (want_rc != 0 || rc != 0) return;
    A1_CHECK(mf.has_manifest == 1, "manifest %s: has_manifest", label);
    A1_CHECK(mf.comctl_major == want_major, "manifest %s: comctl v%d",
             label, want_major);
    A1_CHECK(mf.exec_level == want_exec, "manifest %s: exec level", label);
    A1_CHECK(mf.dpi_aware == want_dpi, "manifest %s: dpi %d", label, want_dpi);
    A1_CHECK(mf.supported_os == want_supported, "manifest %s: supportedOS %d",
             label, want_supported);
}

void a1_manifest_matrix(void) {
    static const char v6[] =
        "<assembly>"
        "<assemblyIdentity name=\"Microsoft.Windows.Common-Controls\" "
        "version=\"6.0.0.0\"/>"
        "<requestedExecutionLevel level=\"asInvoker\"/>"
        "<dpiAware>true/pm</dpiAware>"
        "<supportedOS Id=\"a\"/><supportedOS Id=\"b\"/>"
        "</assembly>";
    static const char v5[] =
        "<assembly>"
        "<assemblyIdentity name=\"Microsoft.Windows.Common-Controls\" "
        "version=\"5.82.0.0\"/>"
        "<requestedExecutionLevel level=\"asInvoker\"/>"
        "</assembly>";
    static const char admin[] =
        "<assembly><requestedExecutionLevel level=\"requireAdministrator\"/>"
        "</assembly>";
    static const char highest[] =
        "<assembly><requestedExecutionLevel level=\"highestAvailable\"/>"
        "</assembly>";
    static const char dpi2[] =
        "<assembly><dpiAwareness>PerMonitorV2</dpiAwareness></assembly>";
    static const char nodpi[] = "<assembly></assembly>";
    static const char bom[] =
        "\xEF\xBB\xBF<?xml version=\"1.0\"?>\n<assembly></assembly>";
    static const char apostrophe[] =
        "<assembly><requestedExecutionLevel level='requireAdministrator'/>"
        "</assembly>";
    static const char unversioned[] =
        "<assembly><assemblyIdentity "
        "name=\"Microsoft.Windows.Common-Controls\"/></assembly>";
    char far_ver[1024], far_lvl[768];
    A1_SECTION("manifest matrix");
    expect_parse("v6", v6, 0, 6, W32_MANIFEST_AS_INVOKER, 1, 2);
    expect_parse("v5", v5, 0, 5, W32_MANIFEST_AS_INVOKER, 0, 0);
    expect_parse("admin", admin, 0, 5, W32_MANIFEST_ADMIN, 0, 0);
    expect_parse("highest", highest, 0, 5, W32_MANIFEST_HIGHEST, 0, 0);
    expect_parse("dpiAwareness", dpi2, 0, 5, W32_MANIFEST_AS_INVOKER, 1, 0);
    expect_parse("nodpi", nodpi, 0, 5, W32_MANIFEST_AS_INVOKER, 0, 0);
    expect_parse("bom+prolog", bom, 0, 5, W32_MANIFEST_AS_INVOKER, 0, 0);
    /* Apostrophe quotes parse as "stanza absent" (documented): the level
     * stays the asInvoker default. */
    expect_parse("apostrophe", apostrophe, 0, 5, W32_MANIFEST_AS_INVOKER,
                 0, 0);
    expect_parse("unversioned-comctl", unversioned, 0, 5,
                 W32_MANIFEST_AS_INVOKER, 0, 0);
    /* The scan windows: a version past 512 bytes, a level past 256. */
    strcpy(far_ver, "<assembly><assemblyIdentity "
           "name=\"Microsoft.Windows.Common-Controls\"");
    memset(far_ver + strlen(far_ver), ' ', 600);
    far_ver[strlen("<assembly><assemblyIdentity "
                   "name=\"Microsoft.Windows.Common-Controls\"") + 600] = 0;
    strcat(far_ver, "version=\"6.0.0.0\"/></assembly>");
    expect_parse("far-version", far_ver, 0, 5, W32_MANIFEST_AS_INVOKER, 0, 0);
    strcpy(far_lvl, "<assembly><requestedExecutionLevel");
    memset(far_lvl + strlen(far_lvl), ' ', 300);
    far_lvl[strlen("<assembly><requestedExecutionLevel") + 300] = 0;
    strcat(far_lvl, "level=\"requireAdministrator\"/></assembly>");
    expect_parse("far-level", far_lvl, 0, 5, W32_MANIFEST_AS_INVOKER, 0, 0);
    /* Malformed shapes. */
    expect_parse("no-assembly", "plain text, no envelope", -1, 0, 0, 0, 0);
    expect_parse("no-close", "<assembly>never closed", -1, 0, 0, 0, 0);
    expect_parse("truncated-prolog", "<?xml version=\"1.0\"", -1, 0, 0, 0, 0);
    {
        w32_manifest_t mf;
        A1_CHECK(w32_manifest_parse(NULL, 0, &mf) != 0, "manifest NULL xml");
        A1_CHECK(w32_manifest_parse((const unsigned char *)nodpi,
                                    strlen(nodpi), NULL) != 0,
                 "manifest NULL out");
    }
}

static const char *exec_name(int level) {
    if (level == W32_MANIFEST_ADMIN) return "requireAdministrator";
    if (level == W32_MANIFEST_HIGHEST) return "highestAvailable";
    return "asInvoker";
}

static void report_one(const char *file) {
    static pe_import_t imports[256];
    static pe_delay_import_t delays[64];
    unsigned char *buf;
    size_t size, count = 0, dcount = 0, i;
    pe_image_t img;
    buf = a1_load_file(file, &size);
    if (!buf) {
        printf("%s: UNREADABLE\n", file);
        return;
    }
    if (pe_parse(buf, size, &img) != PE_OK ||
        pe_imports(&img, imports, 256, &count) != PE_OK ||
        pe_delay_imports(&img, delays, 64, &dcount) != PE_OK) {
        printf("%s: UNPARSEABLE\n", file);
        free(buf);
        return;
    }
    if (count > 256) count = 256;
    if (dcount > 64) dcount = 64;
    for (i = 0; i < count; i++) {
        a1_verdict_t v;
        a1_classify(file, imports[i].dll,
                    imports[i].by_ordinal ? NULL : imports[i].name,
                    imports[i].by_ordinal, imports[i].ordinal, 0, &v);
        if (imports[i].by_ordinal)
            printf("%s: %s!#%u -> %s via %s", file, imports[i].dll,
                   imports[i].ordinal, v.ledger, v.via);
        else
            printf("%s: %s!%s -> %s via %s", file, imports[i].dll,
                   imports[i].name, v.ledger, v.via);
        if (v.note[0]) printf(" [%s]", v.note);
        printf("\n");
    }
    for (i = 0; i < dcount; i++) {
        printf("%s: %s!%s -> DELAY via delay-dir\n", file, delays[i].dll,
               delays[i].by_ordinal ? "?" : delays[i].name);
    }
    /* Manifest facts ride with the fixture (parsed directly: the gate
     * entry prints notices, which would pollute the report). */
    if (!strncmp(file, "mantest_", 8)) {
        uint32_t rva = 0, mlen = 0, off = 0;
        w32_manifest_t mf;
        if (pe_find_resource(&img, PE_RSRC_MANIFEST, &rva, &mlen) != PE_OK)
            rva = mlen = 0;
        if (rva == 0 || mlen == 0) {
            printf("%s: no manifest\n", file);
        } else if (pe_rva_to_offset(&img, rva, mlen, &off) != PE_OK ||
                   w32_manifest_parse(buf + off, mlen, &mf) != 0) {
            printf("%s: manifest MALFORMED\n", file);
        } else {
            printf("%s: manifest comctl=v%d exec=%s dpi=%d supportedOS=%d\n",
                   file, mf.comctl_major, exec_name(mf.exec_level),
                   mf.dpi_aware, mf.supported_os);
        }
    }
    free(buf);
}

void a1_emit_report(void) {
    static const char *const fixtures[] = {
        "ordtest.exe", "ordbadnum.exe", "ordbadname.exe",
        "delaytest_present.exe", "delaytest_absent.exe",
        "chainmain.exe", "cycmain.exe", "datamain.exe",
        "fwdmain.exe", "fwdstatic.exe",
        "mantest_v6.exe", "mantest_v5.exe", "mantest_none.exe",
        "mantest_admin.exe", "mantest_bad.exe",
        "delaytarget.dll", "chain_a.dll", "chain_b.dll",
        "cyc_c.dll", "cyc_d.dll", "datadll.dll", "fwdtest.dll",
    };
    size_t i;
    printf("# W32A-1 bind report.  Regenerate with:\n");
    printf("#   build/test_w32_a1 --emit-report build/user > "
           "w32/tests/W32A1.bindreport\n");
    for (i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++)
        report_one(fixtures[i]);
}
