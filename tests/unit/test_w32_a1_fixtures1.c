/* tests/unit/test_w32_a1_fixtures1.c — ord, delay, chain/cyc groups.
 *
 * Each table is the SPEC for one built fixture: the imports it must carry
 * (nothing dropped, nothing extra) and the verdict each must classify to.
 * DLL spellings follow the LIBRARY lines; comparison is case-insensitive
 * (the loader's ieq), names case-sensitive (its strcmp).
 *
 * The delay exes carry LoadLibraryA + GetProcAddress as CLASSIC imports:
 * __delayLoadHelper2 cannot delay-load its own loader, so the helper's
 * needs stay on the classic table while the payload goes to the delay
 * directory.  That split is the laziness proof, not an accident.
 */

#include <stddef.h>

#include "test_w32_a1.h"

#define K32 "KERNEL32.dll"
#define IMP(d, n, l, v) { d, n, 0, 0, l, v }
#define ORD(d, o, l, v) { d, NULL, 1, o, l, v }
#define KFN(n) IMP(K32, n, "REAL", "static")
#define NOIMP ((const a1_exp_t *)0)

void a1_group_ord(void) {
    static const a1_exp_t ordtest[] = {
        KFN("GetLastError"), KFN("GetModuleHandleA"), KFN("GetProcAddress"),
        KFN("GetStdHandle"), KFN("WriteFile"), KFN("ExitProcess"),
        ORD("COMCTL32.dll", 17, "REAL", "gen:TODO"),
        ORD("COMCTL32.dll", 381, "REAL", "gen:TODO"),
        ORD("OLEAUT32.dll", 2, "REAL", "static"),
        ORD("OLEAUT32.dll", 6, "REAL", "static"),
        ORD("OLEAUT32.dll", 7, "REAL", "static"),
    };
    static const a1_exp_t ordbadnum[] = {
        KFN("ExitProcess"),
        ORD("COMCTL32.dll", 9999, "REFUSED", "ordmap:unmapped"),
    };
    static const a1_exp_t ordbadname[] = {
        KFN("ExitProcess"),
        IMP("COMCTL32.dll", "NoSuchFunction", "REFUSED", "unresolved"),
    };
    A1_SECTION("ord");
    a1_check_imports("ordtest.exe", ordtest,
                     sizeof ordtest / sizeof ordtest[0]);
    a1_check_delays("ordtest.exe", NOIMP, 0);
    a1_check_imports("ordbadnum.exe", ordbadnum,
                     sizeof ordbadnum / sizeof ordbadnum[0]);
    a1_check_delays("ordbadnum.exe", NOIMP, 0);
    a1_check_imports("ordbadname.exe", ordbadname,
                     sizeof ordbadname / sizeof ordbadname[0]);
    a1_check_delays("ordbadname.exe", NOIMP, 0);
}

void a1_group_delay(void) {
    static const a1_exp_t present[] = {
        KFN("GetModuleHandleA"), KFN("LoadLibraryA"), KFN("GetProcAddress"),
        KFN("GetStdHandle"), KFN("WriteFile"), KFN("ExitProcess"),
    };
    static const a1_exp_t present_delays[] = {
        IMP("delaytarget.dll", "delayed_add", "DELAY", "delay-dir"),
    };
    static const a1_exp_t absent[] = {
        KFN("LoadLibraryA"), KFN("GetProcAddress"),
        KFN("GetStdHandle"), KFN("WriteFile"), KFN("ExitProcess"),
    };
    static const a1_exp_t absent_delays[] = {
        IMP("nosuchdll.dll", "NoSuchFunc", "DELAY", "delay-dir"),
    };
    static const a1_exp_t target[] = {
        KFN("GetStdHandle"), KFN("WriteFile"),
    };
    static const a1_expout_t target_exp[] = {
        { "delayed_add", NULL },
    };
    A1_SECTION("delay");
    a1_check_imports("delaytest_present.exe", present,
                     sizeof present / sizeof present[0]);
    a1_check_delays("delaytest_present.exe", present_delays,
                    sizeof present_delays / sizeof present_delays[0]);
    a1_check_imports("delaytest_absent.exe", absent,
                     sizeof absent / sizeof absent[0]);
    a1_check_delays("delaytest_absent.exe", absent_delays,
                    sizeof absent_delays / sizeof absent_delays[0]);
    a1_check_imports("delaytarget.dll", target,
                     sizeof target / sizeof target[0]);
    a1_check_delays("delaytarget.dll", NOIMP, 0);
    a1_check_exports("delaytarget.dll", target_exp,
                     sizeof target_exp / sizeof target_exp[0]);
}

void a1_group_chain_cyc(void) {
    static const a1_exp_t chainmain[] = {
        IMP("chain_a.dll", "chain_a_fn", "REAL", "file"),
        KFN("GetModuleHandleA"), KFN("GetStdHandle"), KFN("WriteFile"),
        KFN("ExitProcess"),
    };
    static const a1_exp_t chain_a[] = {
        IMP("chain_b.dll", "chain_b_fn", "REAL", "file"),
        KFN("GetStdHandle"), KFN("WriteFile"),
    };
    static const a1_exp_t chain_b[] = {
        KFN("GetStdHandle"), KFN("WriteFile"),
    };
    static const a1_expout_t chain_a_exp[] = { { "chain_a_fn", NULL } };
    static const a1_expout_t chain_b_exp[] = { { "chain_b_fn", NULL } };
    static const a1_exp_t cycmain[] = {
        IMP("cyc_c.dll", "cyc_c_fn", "REFUSED", "cycle"),
        KFN("GetStdHandle"), KFN("WriteFile"), KFN("ExitProcess"),
    };
    static const a1_exp_t cyc_c[] = {
        IMP("cyc_d.dll", "cyc_d_fn", "REFUSED", "cycle"),
    };
    static const a1_exp_t cyc_d[] = {
        IMP("cyc_c.dll", "cyc_c_fn", "REFUSED", "cycle"),
    };
    static const a1_expout_t cyc_c_exp[] = { { "cyc_c_fn", NULL } };
    static const a1_expout_t cyc_d_exp[] = { { "cyc_d_fn", NULL } };
    A1_SECTION("chain/cyc");
    a1_check_imports("chainmain.exe", chainmain,
                     sizeof chainmain / sizeof chainmain[0]);
    a1_check_delays("chainmain.exe", NOIMP, 0);
    a1_check_imports("chain_a.dll", chain_a,
                     sizeof chain_a / sizeof chain_a[0]);
    a1_check_delays("chain_a.dll", NOIMP, 0);
    a1_check_exports("chain_a.dll", chain_a_exp,
                     sizeof chain_a_exp / sizeof chain_a_exp[0]);
    a1_check_imports("chain_b.dll", chain_b,
                     sizeof chain_b / sizeof chain_b[0]);
    a1_check_delays("chain_b.dll", NOIMP, 0);
    a1_check_exports("chain_b.dll", chain_b_exp,
                     sizeof chain_b_exp / sizeof chain_b_exp[0]);
    a1_check_imports("cycmain.exe", cycmain,
                     sizeof cycmain / sizeof cycmain[0]);
    a1_check_delays("cycmain.exe", NOIMP, 0);
    a1_check_imports("cyc_c.dll", cyc_c, sizeof cyc_c / sizeof cyc_c[0]);
    a1_check_delays("cyc_c.dll", NOIMP, 0);
    a1_check_exports("cyc_c.dll", cyc_c_exp,
                     sizeof cyc_c_exp / sizeof cyc_c_exp[0]);
    a1_check_imports("cyc_d.dll", cyc_d, sizeof cyc_d / sizeof cyc_d[0]);
    a1_check_delays("cyc_d.dll", NOIMP, 0);
    a1_check_exports("cyc_d.dll", cyc_d_exp,
                     sizeof cyc_d_exp / sizeof cyc_d_exp[0]);
}
