/* tests/unit/test_w32_a1_fixtures2.c — data, fwd, manifest groups.
 *
 * mantest carries two imports, not three: the body has no failure path,
 * so ExitProcess would be an unreferenced extern -- and lld-link drops
 * those (measured).  The spec records the linked shape, not the wish.
 */

#include <stddef.h>

#include "test_w32_a1.h"

#define K32 "KERNEL32.dll"
#define IMP(d, n, l, v) { d, n, 0, 0, l, v }
#define KFN(n) IMP(K32, n, "REAL", "static")
#define NOIMP ((const a1_exp_t *)0)

void a1_group_data_fwd(void) {
    static const a1_exp_t datamain[] = {
        IMP("datadll.dll", "answer", "REAL", "file:DATA"),
        IMP("datadll.dll", "data_fn", "REAL", "file"),
        KFN("GetStdHandle"), KFN("WriteFile"), KFN("ExitProcess"),
    };
    static const a1_expout_t datadll_exp[] = {
        { "answer", NULL }, { "data_fn", NULL },
    };
    static const a1_exp_t fwdmain[] = {
        KFN("LoadLibraryA"), KFN("GetProcAddress"), KFN("GetLastError"),
        KFN("FreeLibrary"), KFN("GetStdHandle"), KFN("WriteFile"),
        KFN("ExitProcess"),
    };
    static const a1_exp_t fwdstatic[] = {
        IMP("fwdtest.dll", "FwdFunc", "REFUSED", "forwarder"),
        KFN("GetStdHandle"), KFN("WriteFile"), KFN("ExitProcess"),
    };
    static const a1_expout_t fwdtest_exp[] = {
        { "FwdFunc", "kernel32.GetTickCount64" },
    };
    A1_SECTION("data/fwd");
    a1_check_imports("datamain.exe", datamain,
                     sizeof datamain / sizeof datamain[0]);
    a1_check_delays("datamain.exe", NOIMP, 0);
    a1_check_imports("datadll.dll", NOIMP, 0);
    a1_check_delays("datadll.dll", NOIMP, 0);
    a1_check_exports("datadll.dll", datadll_exp,
                     sizeof datadll_exp / sizeof datadll_exp[0]);
    a1_check_imports("fwdmain.exe", fwdmain,
                     sizeof fwdmain / sizeof fwdmain[0]);
    a1_check_delays("fwdmain.exe", NOIMP, 0);
    a1_check_imports("fwdstatic.exe", fwdstatic,
                     sizeof fwdstatic / sizeof fwdstatic[0]);
    a1_check_delays("fwdstatic.exe", NOIMP, 0);
    a1_check_imports("fwdtest.dll", NOIMP, 0);
    a1_check_delays("fwdtest.dll", NOIMP, 0);
    a1_check_exports("fwdtest.dll", fwdtest_exp,
                     sizeof fwdtest_exp / sizeof fwdtest_exp[0]);
}

void a1_group_manifest(void) {
    static const a1_exp_t man[] = {
        KFN("GetStdHandle"), KFN("WriteFile"),
    };
    static const char *const exes[] = {
        "mantest_v6.exe", "mantest_v5.exe", "mantest_none.exe",
        "mantest_admin.exe", "mantest_bad.exe",
    };
    size_t i;
    A1_SECTION("manifest");
    for (i = 0; i < sizeof exes / sizeof exes[0]; i++) {
        a1_check_imports(exes[i], man, sizeof man / sizeof man[0]);
        a1_check_delays(exes[i], NOIMP, 0);
    }
    a1_check_manifest("mantest_v6.exe", 6, "asInvoker", 1, 2);
    a1_check_manifest("mantest_v5.exe", 5, "asInvoker", 0, 0);
    a1_check_manifest("mantest_none.exe", 5, NULL, 0, 0);
    a1_check_manifest("mantest_admin.exe", 5, "requireAdministrator", 0, 0);
    a1_check_manifest_bad("mantest_bad.exe");
}
