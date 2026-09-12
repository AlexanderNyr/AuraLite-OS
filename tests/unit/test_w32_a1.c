/* tests/unit/test_w32_a1.c — mode dispatch, the classifier, file helpers.
 *
 * The classifier (a1_classify) mirrors the loader's bind order exactly:
 * ordinals name through the REAL ordinal map; names resolve through the
 * static table, then the generated table (cross-checked against the TSV
 * for skew), then the built DLLs' real export directories; cycles refuse
 * with the path the loader prints.  Whatever it says, --emit-report
 * prints and the fixture groups assert -- one implementation, two uses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_w32_a1.h"
#include "w32/w32_pe.h"
#include "w32/w32_gen.h"
#include "w32/w32_bind.h"
#include "w32/w32_manifest.h"
#include "w32/w32_module.h"
#include "w32/kernel32.h"
#include "w32_a1_tables.h"

int a1_checks = 0;
int a1_failures = 0;
const char *a1_dir = "build/user";

/* The generated stubs call the real ExitProcess (the msvcrt exit family).
 * The harness never runs stub bodies, but the link needs the symbol; a
 * call would be a harness bug, and 99 says so. */
__attribute__((noreturn)) W32ABI void ExitProcess(unsigned int code) {
    fprintf(stderr, "a1: ExitProcess(%u) called -- harness bug\n", code);
    _exit(99);
}

int a1_ieq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += (char)32;
        if (cb >= 'A' && cb <= 'Z') cb += (char)32;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

unsigned char *a1_load_file(const char *name, size_t *size_out) {
    char path[512];
    FILE *f;
    long len;
    unsigned char *buf;
    if (snprintf(path, sizeof path, "%s/%s", a1_dir, name) >=
        (int)sizeof path)
        return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    len = ftell(f);
    if (len < 0) { fclose(f); return 0; }
    rewind(f);
    buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return 0; }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(buf);
        return 0;
    }
    fclose(f);
    *size_out = (size_t)len;
    return buf;
}

static int in_statics(const char *dll, const char *name) {
    const w32_a1_builtin_t *b;
    for (b = w32_a1_builtins; b->dll; b++)
        if (a1_ieq(b->dll, dll) && strcmp(b->name, name) == 0) return 1;
    return 0;
}

static const w32_a1_stubclass_t *in_stubmap(const char *dll,
                                            const char *name) {
    const w32_a1_stubclass_t *r;
    for (r = w32_a1_stubclass; r->dll; r++)
        if (a1_ieq(r->dll, dll) && strcmp(r->name, name) == 0) return r;
    return 0;
}

static int in_gen(const char *dll, const char *name) {
    const w32_export_t *e;
    for (e = w32_gen_exports(); e->dll; e++)
        if (a1_ieq(e->dll, dll) && strcmp(e->name, name) == 0) return 1;
    return 0;
}

/* A module the loader answers without a file: the static table's DLLs and
 * every stub-covered module (w32_gen_modules).  Both sides trim ".dll":
 * queries arrive as-imported ("COMCTL32.dll"), tables store either. */
static void trim_dll(const char *src, char *dst, size_t n) {
    size_t i = 0;
    while (src[i] && i + 1 < n) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    if (i > 4 && a1_ieq(dst + i - 4, ".dll")) dst[i - 4] = 0;
}

static int is_known_module(const char *dll) {
    char base[64], ob[64];
    const char *const *mp;
    const w32_a1_builtin_t *b;
    for (b = w32_a1_builtins; b->dll; b++)
        if (a1_ieq(b->dll, dll)) return 1;
    trim_dll(dll, base, sizeof base);
    for (mp = w32_gen_modules(); *mp; mp++) {
        trim_dll(*mp, ob, sizeof ob);
        if (a1_ieq(ob, base)) return 1;
    }
    return 0;
}

/* True when the export RVA lives in writable, non-executable section data
 * -- the harness's DATA test (the loader binds the address either way). */
static int export_is_data(const pe_image_t *img, unsigned rva) {
    uint16_t i;
    for (i = 0; i < img->section_count; i++) {
        pe_section_t s;
        uint32_t span;
        if (pe_get_section(img, i, &s) != PE_OK) continue;
        span = s.virtual_size ? s.virtual_size : s.raw_size;
        if (rva >= s.virtual_address && rva < s.virtual_address + span)
            return (s.characteristics & PE_SCN_MEM_WRITE) != 0 &&
                   (s.characteristics & PE_SCN_MEM_EXECUTE) == 0;
    }
    return 0;
}

/* Look one export up in a built DLL.  0 found, 1 DLL absent, 2 no export. */
static int file_lookup(const char *dll, const char *name, int by_ord,
                       unsigned ord, int *is_fwd, char *fwd, size_t fwdsz,
                       int *is_data) {
    pe_image_t img;
    static pe_export_t exports[256];
    unsigned char *buf;
    size_t size, count = 0, i;
    *is_fwd = 0;
    *is_data = 0;
    if (fwdsz) fwd[0] = 0;
    buf = a1_load_file(dll, &size);
    if (!buf) return 1;
    if (pe_parse(buf, size, &img) != PE_OK ||
        pe_exports(&img, exports, 256, &count) != PE_OK) {
        free(buf);
        return 2;
    }
    if (count > 256) count = 256;
    for (i = 0; i < count; i++) {
        if (by_ord) {
            if (exports[i].ordinal != (uint16_t)ord) continue;
        } else {
            if (!exports[i].name[0]) continue;
            if (strcmp(exports[i].name, name) != 0) continue;
        }
        if (exports[i].is_forwarder) {
            size_t k = 0;
            *is_fwd = 1;
            while (exports[i].forwarder[k] && k + 1 < fwdsz) {
                fwd[k] = exports[i].forwarder[k];
                k++;
            }
            if (fwdsz) fwd[k] = 0;
        } else {
            *is_data = export_is_data(&img, exports[i].rva);
        }
        free(buf);
        return 0;
    }
    free(buf);
    return 2;
}

/* Does loading `dll` re-enter a load already in progress?  DFS over classic
 * file-import edges; a back edge is the loader's in-progress refusal, and
 * `path` accumulates the "a -> b -> a" the loader prints.  Built-ins are
 * pre-registered, never loaded, so only on-disk DLLs are edges -- exactly
 * the loader's find_by_name-then-load_one split. */
static int dfs_cycle(const char *dll, const char *stack[], int depth,
                     char *path, size_t pathsz) {
    pe_image_t img;
    static pe_import_t imports[256];
    unsigned char *buf;
    size_t size, count = 0, i;
    int d;
    for (d = 0; d < depth; d++) {
        if (a1_ieq(stack[d], dll)) {
            size_t off = 0;
            int k;
            for (k = 0; k < depth; k++)
                off += (size_t)snprintf(path + off, pathsz > off ?
                                        pathsz - off : 0, "%s%s", k ? " -> " : "",
                                        stack[k]);
            snprintf(path + off, pathsz > off ? pathsz - off : 0,
                     " -> %s", dll);
            return 1;
        }
    }
    if (depth > 32) return 0;
    buf = a1_load_file(dll, &size);
    if (!buf) return 0;
    if (pe_parse(buf, size, &img) != PE_OK ||
        pe_imports(&img, imports, 256, &count) != PE_OK) {
        free(buf);
        return 0;
    }
    if (count > 256) count = 256;
    stack[depth] = dll;
    for (i = 0; i < count; i++) {
        unsigned char *probe;
        size_t psize, k = 0;
        /* An edge iff the import names an on-disk DLL (a load_one).  The
         * name is copied per level: `imports` is one static buffer shared
         * by every recursion depth, so a stack of pointers into it would
         * all read the last parse. */
        char child[64];
        probe = a1_load_file(imports[i].dll, &psize);
        if (!probe) continue;
        free(probe);
        while (imports[i].dll[k] && k + 1 < sizeof child) {
            child[k] = imports[i].dll[k];
            k++;
        }
        child[k] = 0;
        if (dfs_cycle(child, stack, depth + 1, path, pathsz)) {
            free(buf);
            return 1;
        }
    }
    free(buf);
    return 0;
}

void a1_classify(const char *importer, const char *dll, const char *name,
                 int by_ord, unsigned ordinal, int is_delay,
                 a1_verdict_t *out) {
    const char *n = name;
    const w32_a1_stubclass_t *tsv;
    const char *stack[40];
    int gen_hit;
    (void)importer;
    out->ledger = "?";
    out->via = "?";
    out->note[0] = 0;
    if (is_delay) {
        out->ledger = "DELAY";
        out->via = "delay-dir";
        snprintf(out->note, sizeof out->note,
                 "loader never binds; __delayLoadHelper2 resolves at runtime");
        return;
    }
    if (by_ord) {
        n = w32_ordmap_lookup(dll, ordinal);
        if (!n) {
            out->ledger = "REFUSED";
            out->via = "ordmap:unmapped";
            snprintf(out->note, sizeof out->note, "loader spells #%u",
                     ordinal);
            return;
        }
        snprintf(out->note, sizeof out->note, "via ordmap: %s", n);
    }
    if (in_statics(dll, n)) {
        out->ledger = "REAL";
        out->via = "static";
        return;
    }
    /* The loader binds through the generated table; the TSV carries the
     * class.  Both must agree -- skew either way is a loud verdict no
     * expectation row matches, so the harness fails rather than guesses. */
    gen_hit = in_gen(dll, n);
    tsv = in_stubmap(dll, n);
    if (gen_hit && !tsv) {
        out->ledger = "SKEW";
        out->via = "gen-without-tsv";
        return;
    }
    if (!gen_hit && tsv && strcmp(tsv->kind, "REAL") != 0) {
        out->ledger = "SKEW";
        out->via = "tsv-without-gen";
        return;
    }
    if (!gen_hit && tsv) {
        out->ledger = "SKEW";
        out->via = "real-without-static";
        return;
    }
    if (is_known_module(dll) && !gen_hit && !tsv) {
        /* A name no table carries in a module the loader owns: the
         * w32_load_dependency builtin branch ("unknown NAME in a known
         * module"), which never touches the disk. */
        out->ledger = "REFUSED";
        out->via = "unresolved";
        return;
    }
    if (gen_hit && tsv) {
        out->ledger = tsv->cls;
        /* Via strings are compared, so they need stable storage. */
        if (!strcmp(tsv->kind, "TODO")) out->via = "gen:TODO";
        else if (!strcmp(tsv->kind, "FAILCLEAN")) out->via = "gen:FAILCLEAN";
        else if (!strcmp(tsv->kind, "DATA")) out->via = "gen:DATA";
        else out->via = "gen:REAL";
        return;
    }
    {
        char path[256];
        int is_fwd, is_data, rc;
        char fwd[128];
        path[0] = 0;
        if (dfs_cycle(dll, stack, 0, path, sizeof path)) {
            out->ledger = "REFUSED";
            out->via = "cycle";
            snprintf(out->note, sizeof out->note, "%.159s", path);
            return;
        }
        rc = file_lookup(dll, n, by_ord, ordinal, &is_fwd, fwd, sizeof fwd,
                         &is_data);
        if (rc == 1) {
            out->ledger = "REFUSED";
            out->via = "file:absent";
        } else if (rc == 2) {
            out->ledger = "REFUSED";
            out->via = "file:missing-export";
        } else if (is_fwd) {
            out->ledger = "REFUSED";
            out->via = "forwarder";
            snprintf(out->note, sizeof out->note, "target %s", fwd);
        } else if (is_data) {
            out->ledger = "REAL";
            out->via = "file:DATA";
        } else {
            out->ledger = "REAL";
            out->via = "file";
        }
        return;
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [--ledger-harness [DIR] | --emit-report DIR]\n",
            prog);
}

int main(int argc, char **argv) {
    const char *mode = "--ledger-harness";
    if (argc > 1) mode = argv[1];
    if (strcmp(mode, "--emit-report") == 0) {
        if (argc > 2) a1_dir = argv[2];
        a1_emit_report();
        return 0;
    }
    if (strcmp(mode, "--ledger-harness") != 0) {
        usage(argv[0]);
        return 2;
    }
    if (argc > 2) a1_dir = argv[2];

    A1_SECTION("loader constants");
    A1_CHECK(W32_LOAD_DEPTH_MAX >= 2 && W32_LOAD_DEPTH_MAX <= 64,
             "depth cap %d is a sane bound (plan: recursion is capped)",
             W32_LOAD_DEPTH_MAX);

    a1_group_ord();
    a1_group_delay();
    a1_group_chain_cyc();
    a1_group_data_fwd();
    a1_group_manifest();
    a1_manifest_matrix();

    if (a1_failures)
        printf("A1: %d checks, %d FAILURES\n", a1_checks, a1_failures);
    else
        printf("A1-HARNESS-OK %d checks\n", a1_checks);
    return a1_failures ? 1 : 0;
}
