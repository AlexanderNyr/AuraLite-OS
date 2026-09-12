/* tests/unit/test_w32_a1_check.c — generic comparators over fixtures.
 *
 * Every check is order-insensitive (linkers order import tables their own
 * way) but count-exact: a silently dropped import fails the count, and an
 * unexpected import fails the match.  Verdicts come from a1_classify, the
 * same function --emit-report prints, so asserts and report cannot skew.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_w32_a1.h"
#include "w32/w32_pe.h"
#include "w32/w32_manifest.h"

static void fmt_imp(char *buf, size_t n, const char *dll, const char *name,
                    int by_ord, unsigned ord) {
    if (by_ord)
        snprintf(buf, n, "%s!#%u", dll, ord);
    else
        snprintf(buf, n, "%s!%s", dll, name ? name : "?");
}

static int exp_matches(const a1_exp_t *e, const char *dll, const char *name,
                       int by_ord, unsigned ord) {
    if (!a1_ieq(e->dll, dll)) return 0;
    if (e->by_ordinal != by_ord) return 0;
    if (by_ord) return e->ordinal == ord;
    return e->name && name && strcmp(e->name, name) == 0;
}

void a1_check_imports(const char *file, const a1_exp_t *exp, size_t nexp) {
    static pe_import_t imports[256];
    unsigned char *buf;
    size_t size, count = 0, i, n = 256;
    pe_image_t img;
    int *used;
    char spelling[256];
    buf = a1_load_file(file, &size);
    A1_CHECK(buf != 0, "%s: fixture readable", file);
    if (!buf) return;
    if (pe_parse(buf, size, &img) != PE_OK) {
        A1_CHECK(0, "%s: parses", file);
        free(buf);
        return;
    }
    A1_CHECK(pe_imports(&img, imports, n, &count) == PE_OK,
             "%s: import directory walks", file);
    if (count > n) count = n;
    A1_CHECK(count == nexp, "%s: %lu classic import(s), want %lu", file,
             (unsigned long)count, (unsigned long)nexp);
    used = calloc(nexp ? nexp : 1, sizeof *used);
    if (!used) {
        A1_CHECK(0, "%s: out of memory", file);
        free(buf);
        return;
    }
    for (i = 0; i < count; i++) {
        size_t j;
        int found = 0;
        for (j = 0; j < nexp; j++) {
            if (used[j]) continue;
            if (exp_matches(&exp[j], imports[i].dll,
                            imports[i].by_ordinal ? NULL : imports[i].name,
                            imports[i].by_ordinal, imports[i].ordinal)) {
                a1_verdict_t v;
                used[j] = 1;
                found = 1;
                a1_classify(file, imports[i].dll,
                            imports[i].by_ordinal ? NULL : imports[i].name,
                            imports[i].by_ordinal, imports[i].ordinal, 0, &v);
                fmt_imp(spelling, sizeof spelling, imports[i].dll,
                        imports[i].by_ordinal ? NULL : imports[i].name,
                        imports[i].by_ordinal, imports[i].ordinal);
                A1_CHECK(strcmp(v.ledger, exp[j].want_ledger) == 0 &&
                         strcmp(v.via, exp[j].want_via) == 0,
                         "%s: %s -> %s via %s (want %s via %s)%s%s", file,
                         spelling, v.ledger, v.via, exp[j].want_ledger,
                         exp[j].want_via, v.note[0] ? " [" : "",
                         v.note[0] ? v.note : "");
                break;
            }
        }
        if (!found) {
            fmt_imp(spelling, sizeof spelling, imports[i].dll,
                    imports[i].by_ordinal ? NULL : imports[i].name,
                    imports[i].by_ordinal, imports[i].ordinal);
            A1_CHECK(0, "%s: unexpected import %s", file, spelling);
        }
    }
    for (i = 0; i < nexp; i++) {
        if (!used[i]) {
            fmt_imp(spelling, sizeof spelling, exp[i].dll, exp[i].name,
                    exp[i].by_ordinal, exp[i].ordinal);
            A1_CHECK(0, "%s: missing expected import %s", file, spelling);
        }
    }
    free(used);
    free(buf);
}

void a1_check_delays(const char *file, const a1_exp_t *exp, size_t nexp) {
    static pe_delay_import_t delays[64];
    static pe_import_t classic[256];
    unsigned char *buf;
    size_t size, count = 0, ccount = 0, i;
    pe_image_t img;
    char spelling[256];
    buf = a1_load_file(file, &size);
    A1_CHECK(buf != 0, "%s: fixture readable", file);
    if (!buf) return;
    if (pe_parse(buf, size, &img) != PE_OK) {
        A1_CHECK(0, "%s: parses", file);
        free(buf);
        return;
    }
    A1_CHECK(pe_delay_imports(&img, delays, 64, &count) == PE_OK,
             "%s: delay directory walks", file);
    if (count > 64) count = 64;
    A1_CHECK(count == nexp, "%s: %lu delay import(s), want %lu", file,
             (unsigned long)count, (unsigned long)nexp);
    pe_imports(&img, classic, 256, &ccount);
    if (ccount > 256) ccount = 256;
    for (i = 0; i < count && i < nexp; i++) {
        size_t c;
        int leaked = 0;
        a1_verdict_t v;
        for (c = 0; c < ccount; c++) {
            if (a1_ieq(classic[c].dll, delays[i].dll) &&
                !classic[c].by_ordinal && !delays[i].by_ordinal &&
                strcmp(classic[c].name, delays[i].name) == 0)
                leaked = 1;
        }
        fmt_imp(spelling, sizeof spelling, delays[i].dll,
                delays[i].by_ordinal ? NULL : delays[i].name,
                delays[i].by_ordinal, delays[i].ordinal);
        A1_CHECK(!leaked, "%s: %s stays off the classic table", file,
                 spelling);
        A1_CHECK(exp_matches(&exp[i], delays[i].dll,
                             delays[i].by_ordinal ? NULL : delays[i].name,
                             delays[i].by_ordinal, delays[i].ordinal),
                 "%s: delay import %lu is %s", file, (unsigned long)i,
                 spelling);
        a1_classify(file, delays[i].dll,
                    delays[i].by_ordinal ? NULL : delays[i].name,
                    delays[i].by_ordinal, delays[i].ordinal, 1, &v);
        A1_CHECK(strcmp(v.ledger, "DELAY") == 0,
                 "%s: %s classifies DELAY", file, spelling);
    }
    free(buf);
}

void a1_check_exports(const char *file, const a1_expout_t *exp, size_t nexp) {
    static pe_export_t exports[256];
    unsigned char *buf;
    size_t size, count = 0, i;
    pe_image_t img;
    buf = a1_load_file(file, &size);
    A1_CHECK(buf != 0, "%s: fixture readable", file);
    if (!buf) return;
    if (pe_parse(buf, size, &img) != PE_OK) {
        A1_CHECK(0, "%s: parses", file);
        free(buf);
        return;
    }
    A1_CHECK(pe_exports(&img, exports, 256, &count) == PE_OK,
             "%s: export directory walks", file);
    if (count > 256) count = 256;
    A1_CHECK(count == nexp, "%s: %lu export(s), want %lu", file,
             (unsigned long)count, (unsigned long)nexp);
    for (i = 0; i < count && i < nexp; i++) {
        A1_CHECK(strcmp(exports[i].name, exp[i].name) == 0,
                 "%s: export %lu is %s", file, (unsigned long)i,
                 exp[i].name);
        if (exp[i].forwarder) {
            A1_CHECK(exports[i].is_forwarder &&
                     strcmp(exports[i].forwarder, exp[i].forwarder) == 0,
                     "%s: %s forwards to %s", file, exp[i].name,
                     exp[i].forwarder);
        } else {
            A1_CHECK(!exports[i].is_forwarder, "%s: %s is not a forwarder",
                     file, exp[i].name);
        }
    }
    free(buf);
}

static int want_exec_level(const char *want) {
    if (!strcmp(want, "asInvoker")) return W32_MANIFEST_AS_INVOKER;
    if (!strcmp(want, "highestAvailable")) return W32_MANIFEST_HIGHEST;
    if (!strcmp(want, "requireAdministrator")) return W32_MANIFEST_ADMIN;
    return -99;
}

void a1_check_manifest(const char *file, int want_major, const char *want_exec,
                       int want_dpi, int want_supported) {
    unsigned char *buf;
    size_t size;
    pe_image_t img;
    uint32_t rva = 0, mlen = 0, off = 0;
    w32_manifest_t mf, mf2;
    buf = a1_load_file(file, &size);
    A1_CHECK(buf != 0, "%s: fixture readable", file);
    if (!buf) return;
    if (pe_parse(buf, size, &img) != PE_OK) {
        A1_CHECK(0, "%s: parses", file);
        free(buf);
        return;
    }
    if (pe_find_resource(&img, PE_RSRC_MANIFEST, &rva, &mlen) != PE_OK)
        rva = mlen = 0;
    if (!want_exec) {
        A1_CHECK(rva == 0 || mlen == 0, "%s: carries no manifest", file);
        free(buf);
        return;
    }
    A1_CHECK(rva != 0 && mlen != 0, "%s: carries a manifest", file);
    if (pe_rva_to_offset(&img, rva, mlen, &off) != PE_OK) {
        A1_CHECK(0, "%s: manifest bytes addressable", file);
        free(buf);
        return;
    }
    A1_CHECK(w32_manifest_parse(buf + off, mlen, &mf) == 0,
             "%s: manifest parses", file);
    A1_CHECK(mf.comctl_major == want_major, "%s: comctl v%d", file, want_major);
    A1_CHECK(mf.exec_level == want_exec_level(want_exec), "%s: exec=%s",
             file, want_exec);
    A1_CHECK(mf.dpi_aware == want_dpi, "%s: dpi %s", file,
             want_dpi ? "recorded" : "absent");
    A1_CHECK(mf.supported_os == want_supported, "%s: %d supportedOS", file,
             want_supported);
    /* The gate entry agrees with the direct parse (it also prints the
     * supportedOS notices -- expected noise in assert mode). */
    A1_CHECK(w32_manifest_check(buf, size, &mf2) == 0, "%s: gate accepts",
             file);
    A1_CHECK(mf2.comctl_major == want_major &&
             mf2.exec_level == want_exec_level(want_exec),
             "%s: gate fields agree", file);
    free(buf);
}

void a1_check_manifest_bad(const char *file) {
    unsigned char *buf;
    size_t size;
    pe_image_t img;
    uint32_t rva = 0, mlen = 0, off = 0;
    w32_manifest_t mf;
    buf = a1_load_file(file, &size);
    A1_CHECK(buf != 0, "%s: fixture readable", file);
    if (!buf) return;
    if (pe_parse(buf, size, &img) != PE_OK) {
        A1_CHECK(0, "%s: parses", file);
        free(buf);
        return;
    }
    if (pe_find_resource(&img, PE_RSRC_MANIFEST, &rva, &mlen) != PE_OK)
        rva = mlen = 0;
    A1_CHECK(rva != 0 && mlen != 0, "%s: carries the malformed blob", file);
    if (pe_rva_to_offset(&img, rva, mlen, &off) != PE_OK) {
        A1_CHECK(0, "%s: manifest bytes addressable", file);
        free(buf);
        return;
    }
    A1_CHECK(w32_manifest_parse(buf + off, mlen, &mf) != 0,
             "%s: manifest refused by the parser", file);
    A1_CHECK(w32_manifest_check(buf, size, &mf) != 0,
             "%s: manifest refused by the gate", file);
    free(buf);
}
