/* tests/unit/test_w32_a1.h — the W32A-1 binder-only ledger harness.
 *
 * A host tool (build/test_w32_a1) that parses the BUILT fixtures with the
 * real w32_pe.c / w32_manifest.c, resolves every import the way the loader
 * would bind it (real ordinal map + generated tables from w32_stubs_gen.c,
 * build-derived static names from w32_a1_tables.h, real export directories
 * of the built DLLs), and asserts the ledger class per import.
 *
 * Modes: no args (what `make test-unit` runs) and --ledger-harness assert;
 * --emit-report DIR prints the bind report the checker diffs textually.
 */

#ifndef AURALITE_TEST_W32_A1_H
#define AURALITE_TEST_W32_A1_H

#include <stddef.h>
#include <stdio.h>

extern int a1_checks;
extern int a1_failures;
/* Directory holding the built fixtures (argv, default "build/user"). */
extern const char *a1_dir;

#define A1_CHECK(cond, ...) do {                                          \
        a1_checks++;                                                      \
        if (!(cond)) {                                                    \
            a1_failures++;                                                \
            printf("not ok - " __VA_ARGS__);                              \
            printf("\n");                                                 \
        }                                                                 \
    } while (0)

#define A1_SECTION(name) printf("-- " name "\n")

/* Case-insensitive ASCII compare (module names; the loader's ieq). */
int a1_ieq(const char *a, const char *b);

/* Read a whole file from a1_dir into malloc'd memory; 0 on failure. */
unsigned char *a1_load_file(const char *name, size_t *size_out);

/* One expectation row: an import the fixture must carry, and the verdict
 * the loader must reach for it.  `name` is NULL for ordinal imports. */
typedef struct {
    const char *dll;
    const char *name;
    int by_ordinal;
    unsigned ordinal;
    const char *want_ledger;
    const char *want_via;
} a1_exp_t;

/* The verdict: ledger class + bind mechanism + a human note. */
typedef struct {
    const char *ledger;
    const char *via;
    char note[160];
} a1_verdict_t;

/* Classify one import exactly as w32_bind_imports + w32_load_dependency +
 * resolve_in_module would bind it.  `importer` is the fixture file name
 * (for the cycle walk); NULL classifies without cycle context. */
void a1_classify(const char *importer, const char *dll, const char *name,
                 int by_ordinal, unsigned ordinal, int is_delay,
                 a1_verdict_t *out);

/* Generic comparators over the built fixtures. */
void a1_check_imports(const char *file, const a1_exp_t *exp, size_t nexp);
void a1_check_delays(const char *file, const a1_exp_t *exp, size_t nexp);

/* One expected export: `forwarder` non-NULL means the export must forward
 * to that target text.  (DATA-ness is asserted through classification: the
 * importer's row for a data export must read REAL via file:DATA.) */
typedef struct {
    const char *name;
    const char *forwarder;
} a1_expout_t;

void a1_check_exports(const char *file, const a1_expout_t *exp, size_t nexp);

/* Manifest expectations: exec is "asInvoker" / "highestAvailable", or NULL
 * when the file must carry no manifest at all (the rest then ignored). */
void a1_check_manifest(const char *file, int want_major, const char *want_exec,
                       int want_dpi, int want_supported);
void a1_check_manifest_bad(const char *file);

/* Fixture groups (tests/unit/test_w32_a1_fixtures1.c / fixtures2.c). */
void a1_group_ord(void);
void a1_group_delay(void);
void a1_group_chain_cyc(void);
void a1_group_data_fwd(void);
void a1_group_manifest(void);

/* The manifest parse matrix over embedded strings. */
void a1_manifest_matrix(void);

/* The report mode. */
void a1_emit_report(void);

#endif
