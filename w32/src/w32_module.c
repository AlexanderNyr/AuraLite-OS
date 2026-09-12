/* w32/src/w32_module.c — WIN32_PLAN.md phase W32-7.
 *
 * LoadLibrary, GetProcAddress, FreeLibrary.
 *
 * The phase allowed this to end in a documented refusal if per-process
 * module lists needed address-space work the kernel does not have.  It did
 * not: AuraLite's mmap grants PROT_EXEC and mprotect exists, so a real
 * user-supplied DLL can be mapped, relocated and entered.  The refusal was
 * not needed and would have been the weaker answer.
 *
 * W32A-1 (W32APP_PLAN.md) lifted two of the three W32-7 refusals: delay-load
 * imports bind lazily through LoadLibrary/GetProcAddress, and ordinals
 * translate through the ordinal map.  What stays refused is narrower still --
 * forwarder exports (now naming their target), dependency cycles (named),
 * and over-deep chains (depth named).  Each is detected and reported,
 * because the failure mode of a half-supported forwarder is a call into a
 * string, which crashes far away from the cause.
 */

#ifndef AURALITE_W32_HOST_TEST
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#endif

#include "w32/w32_module.h"
#include "w32/w32_errno.h"
#include "w32/w32_bind.h"
#include "w32/w32_pe.h"
#include "w32/w32_crt.h"
#include "w32/w32_gen.h"

#ifndef PROT_READ
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#endif

/* A loaded module.  Built-ins have base == NULL and are never unmapped. */
typedef struct {
    int      used;
    int      builtin;
    int      loading;                      /* claimed, load in flight        */
    int      refs;
    char     name[W32_MODULE_NAME_MAX];   /* as matched, case-insensitively */
    uint8_t *base;                        /* mapping, for file modules      */
    size_t   span;                        /* mapped bytes                   */
    uint8_t *file;                        /* the raw file, kept for parsing */
    size_t   file_size;
    unsigned seq;                         /* load order, for teardown       */
    int      holds[W32_MODULE_MAX];       /* held dependency slots          */
    int      nholds;
} w32_module_t;

static w32_module_t modules[W32_MODULE_MAX];
static int          modules_ready;

/* W32A-1 recursion state.  Single-threaded, so globals are exact: the stack
 * of DLL names currently being loaded (the cycle detector), the main
 * executable's directory (first in the dependency search order), and the
 * load sequence counter (teardown runs in reverse). */
static char     load_stack[W32_LOAD_DEPTH_MAX][W32_MODULE_NAME_MAX];
static int      load_depth;
static unsigned load_seq;
static char     exe_dir[256];

static w32_module_t *find_by_name(const char *name);

/* Handles are minted from the table, biased so that neither NULL nor a small
 * integer is ever a valid handle.  Same reasoning as the W32-4 HANDLE table:
 * a token the program can fabricate must not be dereferenceable. */
#define HMODULE_BIAS 0x4000

static W32_HMODULE slot_to_handle(int i) {
    return (W32_HMODULE)(uintptr_t)(HMODULE_BIAS + i);
}

static w32_module_t *handle_to_slot(W32_HMODULE h) {
    uintptr_t v = (uintptr_t)h;
    if (v < HMODULE_BIAS) return NULL;
    uintptr_t i = v - HMODULE_BIAS;
    if (i >= W32_MODULE_MAX) return NULL;
    if (!modules[i].used) return NULL;
    return &modules[i];
}

static int ieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
}

/* Windows programs say "kernel32", "KERNEL32.dll" and "KERNEL32.DLL"
 * interchangeably.  Compare ignoring case and an optional .dll suffix, or
 * half of those spellings fail to find a module that is loaded. */
static int module_name_matches(const char *have, const char *want) {
    if (ieq(have, want)) return 1;

    char trimmed[W32_MODULE_NAME_MAX];
    size_t n = 0;
    while (want[n] && n + 1 < sizeof trimmed) { trimmed[n] = want[n]; n++; }
    trimmed[n] = 0;
    if (n > 4 && ieq(trimmed + n - 4, ".dll")) {
        trimmed[n - 4] = 0;
        return ieq(have, trimmed);
    }
    return 0;
}

static void add_builtin(const char *name) {
    for (int i = 0; i < W32_MODULE_MAX; i++) {
        if (modules[i].used) continue;
        modules[i].used    = 1;
        modules[i].builtin = 1;
        modules[i].refs    = 1;
        size_t k = 0;
        while (name[k] && k + 1 < sizeof modules[i].name) {
            modules[i].name[k] = name[k]; k++;
        }
        modules[i].name[k] = 0;
        return;
    }
    /* A silent drop here fails every later file load with no diagnostic
     * (W32A-1: 20 built-ins into 16 slots).  Never silent again. */
    printf("w32: module table full, dropping built-in %s\n", name);
}

void w32_module_init(void) {
    const char *const *mp;
    if (modules_ready) return;
    modules_ready = 1;
    /* The three modules whose exports are linked into the loader.  There is
     * no file behind any of them, which is why they are registered rather
     * than loaded. */
    add_builtin("kernel32");
    add_builtin("user32");
    add_builtin("gdi32");
    /* W32A-1: every stub-covered module answers GetModuleHandle too, so a
     * program probing for mpr, comctl32 or shell32 finds a module whose
     * functions fail cleanly instead of a missing DLL.  Names already
     * registered are skipped, never duplicated. */
    for (mp = w32_gen_modules(); *mp; mp++) {
        if (find_by_name(*mp)) continue;
        add_builtin(*mp);
    }
}

static w32_module_t *find_by_name(const char *name) {
    for (int i = 0; i < W32_MODULE_MAX; i++) {
        if (!modules[i].used || modules[i].loading) continue;
        if (module_name_matches(modules[i].name, name)) return &modules[i];
    }
    return NULL;
}

static int slot_index(const w32_module_t *m) {
    return (int)(m - modules);
}

W32_HMODULE W32ABI w32_GetModuleHandleA(const char *name) {
    w32_module_init();

    /* NULL asks for the main executable.  It is not one of our modules --
     * w32run is an ELF -- so report the process token rather than inventing
     * a base a program might try to read headers from. */
    if (!name) return slot_to_handle(0);

    w32_module_t *m = find_by_name(name);
    if (!m) {
        w32_set_last_error(W32_ERROR_MOD_NOT_FOUND);
        return NULL;
    }
    return slot_to_handle(slot_index(m));
}

/* ---- loading a real DLL ------------------------------------------------ */

static uint8_t *read_whole_file(const char *path, size_t *out_size) {
    int fd = open(path, 0 /* O_RDONLY */);
    if (fd < 0) return NULL;

    /* Grow-and-read rather than fstat: the size is only used to bound the
     * parse, and the parser bounds-checks everything against it anyway. */
    size_t cap = 64 * 1024, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }

    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            if (ncap > 8u * 1024u * 1024u) { free(buf); close(fd); return NULL; }
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb; cap = ncap;
        }
        long n = read(fd, buf + len, cap - len);
        if (n < 0) { free(buf); close(fd); return NULL; }
        if (n == 0) break;
        len += (size_t)n;
    }
    close(fd);
    *out_size = len;
    return buf;
}

void w32_module_set_exe_dir(const char *exe_path) {
    size_t n = 0;
    long last = -1;
    exe_dir[0] = 0;
    if (!exe_path) return;
    while (exe_path[n] && n + 1 < sizeof exe_dir) {
        if (exe_path[n] == '/' || exe_path[n] == '\\') last = (long)n;
        exe_dir[n] = exe_path[n]; n++;
    }
    exe_dir[n] = 0;
    if (last >= 0) exe_dir[last] = 0;
    else exe_dir[0] = 0;
}

/* Open a dependency: the exe's directory first for a bare name, then the
 * name as given (relative names resolve against the working directory). */
static uint8_t *open_module_file(const char *name, size_t *out_size) {
    int bare = 1;
    const char *pp;
    for (pp = name; *pp; pp++)
        if (*pp == '/' || *pp == '\\' || *pp == ':') { bare = 0; break; }
    if (bare && exe_dir[0]) {
        char path[256 + 64 + 2];
        size_t d = 0, k = 0;
        while (exe_dir[d] && d + 1 < sizeof path) {
            path[d] = exe_dir[d]; d++;
        }
        if (d + 1 < sizeof path) path[d++] = '/';
        while (name[k] && d + 1 < sizeof path) { path[d++] = name[k++]; }
        path[d] = 0;
        {
            uint8_t *f = read_whole_file(path, out_size);
            if (f) return f;
        }
    }
    return read_whole_file(name, out_size);
}

/* Map, relocate and bind one DLL.  Returns 0 on success. */
static int map_dll(const uint8_t *file, size_t file_size,
                   uint8_t **out_base, size_t *out_span,
                   const char **why) {
    pe_image_t img;
    if (pe_parse(file, file_size, &img) != PE_OK) {
        *why = "not a PE image";
        return -1;
    }
    if (img.machine != PE_MACHINE_AMD64 ||
        img.opt_magic != PE_OPT_MAGIC_PE32PLUS) {
        *why = "not an AMD64 PE32+ image";
        return -1;
    }
    /* A DLL must actually be one.  Loading an .exe as a library would run
     * its entry point under DllMain's contract, which it does not follow. */
    if (!(img.characteristics & 0x2000u)) {
        *why = "not a DLL (IMAGE_FILE_DLL is clear)";
        return -1;
    }
    /* Delay-load imports are NOT bound here (W32A-1): the delay directory is
     * legal, and the image's own __delayLoadHelper2 resolves each target on
     * first call through LoadLibrary/GetProcAddress below.  Nothing to do. */

    size_t span = (img.size_of_image + 0xFFFu) & ~(size_t)0xFFFu;
    if (span == 0 || span > 64u * 1024u * 1024u) {
        *why = "implausible SizeOfImage";
        return -1;
    }

    uint8_t *base = mmap(0, span, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (!base || (long)(intptr_t)base < 0) {
        *why = "mmap failed";
        return -1;
    }
    memset(base, 0, span);

    size_t hdr = img.size_of_headers;
    if (hdr > file_size) hdr = file_size;
    memcpy(base, file, hdr);

    for (uint16_t i = 0; i < img.section_count; i++) {
        pe_section_t s;
        if (pe_get_section(&img, i, &s) != PE_OK) {
            munmap(base, span); *why = "bad section table"; return -1;
        }
        if (!s.raw_size) continue;
        if ((size_t)s.virtual_address + s.raw_size > span) {
            munmap(base, span); *why = "section outside image"; return -1;
        }
        memcpy(base + s.virtual_address, file + s.raw_offset, s.raw_size);
    }

    /* Relocate.  A DLL is almost never loaded at its preferred base, so an
     * image with no relocation table cannot be moved and is refused rather
     * than run at the wrong address. */
    static pe_reloc_t relocs[4096];
    size_t nrel = 0;
    if (pe_relocations(&img, relocs, sizeof relocs / sizeof relocs[0],
                       &nrel) != PE_OK) {
        munmap(base, span); *why = "bad relocation table"; return -1;
    }
    if (nrel > sizeof relocs / sizeof relocs[0]) {
        munmap(base, span); *why = "too many relocations"; return -1;
    }

    uint64_t delta = (uint64_t)(uintptr_t)base - img.image_base;
    /* An empty relocation table is NOT the same as an unrelocatable image.
     * A fully position-independent DLL -- everything RIP-relative, no
     * absolute addresses in its data -- legitimately needs no fixups, and
     * the fixture in w32/tests/testdll.asm is exactly that. What actually
     * says "this image cannot be moved" is IMAGE_FILE_RELOCS_STRIPPED, so
     * that is what is tested. Refusing on nrel == 0 would have rejected a
     * perfectly good DLL. */
    if (delta != 0 && (img.characteristics & 0x0001u)) {
        munmap(base, span);
        *why = "relocations stripped and not at its preferred base";
        return -1;
    }
    for (size_t i = 0; i < nrel; i++) {
        if (relocs[i].type != PE_REL_DIR64) {
            munmap(base, span); *why = "unsupported relocation type"; return -1;
        }
        if ((size_t)relocs[i].rva + 8 > span) {
            munmap(base, span); *why = "relocation outside image"; return -1;
        }
        uint8_t *p = base + relocs[i].rva;
        uint64_t v = 0;
        for (int k = 7; k >= 0; k--) v = (v << 8) | p[k];
        v += delta;
        for (int k = 0; k < 8; k++) p[k] = (uint8_t)((v >> (k * 8)) & 0xFF);
    }

    /* The DLL's own imports resolve against the built-in table, exactly as
     * the main image's do. */
    const char *mdll = NULL, *mname = NULL;
    if (w32_bind_imports(file, file_size, (uint64_t)(uintptr_t)base,
                         &mdll, &mname) != 0) {
        printf("w32: LoadLibrary: unresolved import %s!%s in the DLL\n",
               mdll ? mdll : "?", mname ? mname : "?");
        munmap(base, span);
        *why = "unresolved import in the DLL";
        return -1;
    }

    *out_base = base;
    *out_span = span;
    return 0;
}

/* Tear down one file slot: DETACH itself first, then release what it holds.
 * Children detach after their importer -- teardown in reverse. */
static void free_slot(int slot) {
    w32_module_t *m = &modules[slot];
    pe_image_t img;
    int k;
    if (pe_parse(m->file, m->file_size, &img) == PE_OK && img.entry_point_rva) {
        typedef int (W32ABI *dllmain_fn)(void *, uint32_t, void *);
        dllmain_fn dm = (dllmain_fn)(void *)(m->base + img.entry_point_rva);
        dm((void *)m->base, W32_DLL_PROCESS_DETACH, NULL);
    }
    for (k = 0; k < m->nholds; k++) {
        w32_module_t *dep;
        if (m->holds[k] < 0 || m->holds[k] >= W32_MODULE_MAX) continue;
        dep = &modules[m->holds[k]];
        if (!dep->used || dep->builtin) continue;
        if (--dep->refs > 0) continue;
        free_slot(m->holds[k]);
    }
    m->nholds = 0;
    munmap(m->base, m->span);
    free(m->file);
    memset(m, 0, sizeof *m);
}

/* Undo the holds without tearing the slot down (the DllMain-FALSE path). */
static void release_holds_only(int slot) {
    w32_module_t *m = &modules[slot];
    int k;
    for (k = 0; k < m->nholds; k++) {
        w32_module_t *dep;
        if (m->holds[k] < 0 || m->holds[k] >= W32_MODULE_MAX) continue;
        dep = &modules[m->holds[k]];
        if (!dep->used || dep->builtin) continue;
        if (--dep->refs > 0) continue;
        free_slot(m->holds[k]);
    }
    m->nholds = 0;
}

/* Take a hold on every file module this slot's imports name.  The bind
 * already loaded them (or found them loaded); the hold keeps them alive
 * exactly as long as the importer. */
static void take_holds(int slot) {
    w32_module_t *m = &modules[slot];
    pe_image_t img;
    static pe_import_t imports[256];
    size_t count = 0, i;
    if (pe_parse(m->file, m->file_size, &img) != PE_OK) return;
    if (pe_imports(&img, imports, sizeof imports / sizeof imports[0],
                   &count) != PE_OK) return;
    if (count > sizeof imports / sizeof imports[0]) return;
    for (i = 0; i < count; i++) {
        w32_module_t *dep = find_by_name(imports[i].dll);
        int di, k;
        if (!dep || dep->builtin || dep == m) continue;
        di = slot_index(dep);
        for (k = 0; k < m->nholds; k++)
            if (m->holds[k] == di) break;
        if (k < m->nholds) continue;
        if (m->nholds >= W32_MODULE_MAX) continue;
        dep->refs++;
        m->holds[m->nholds++] = di;
    }
}

/* Load one DLL from disk, recursively.  The load_stack is the cycle
 * detector: a name already on it means the chain leads back to itself, and
 * the refusal prints the whole cycle.  Dependencies attach before their
 * importer (their DllMain runs inside their load, which completes first). */
static W32_HMODULE load_one(const char *name) {
    int slot = -1, i;
    size_t file_size = 0;
    uint8_t *file;
    uint8_t *base = NULL;
    size_t span = 0;
    const char *why = "unknown";
    pe_image_t img;
    size_t k;

    for (i = 0; i < load_depth; i++) {
        if (module_name_matches(load_stack[i], name)) {
            int j;
            printf("w32: LoadLibrary(%s) refused: dependency cycle: ", name);
            for (j = 0; j < load_depth; j++)
                printf("%s -> ", load_stack[j]);
            printf("%s\n", name);
            w32_set_last_error(W32_ERROR_BAD_EXE_FORMAT);
            return NULL;
        }
    }
    if (load_depth >= W32_LOAD_DEPTH_MAX) {
        printf("w32: LoadLibrary(%s) refused: dependency chain deeper "
               "than %d\n", name, W32_LOAD_DEPTH_MAX);
        w32_set_last_error(W32_ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    for (i = 0; i < W32_MODULE_MAX; i++) {
        if (!modules[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        w32_set_last_error(W32_ERROR_NOT_SUPPORTED);
        return NULL;
    }

    /* Claim the slot BEFORE mapping: nested loads (this very function,
     * re-entered through map_dll -> bind -> load) scan the same table,
     * and an unclaimed slot is stolen -- the nested module's entry is
     * then overwritten at commit (W32A-1: chain_b lost its slot to
     * chain_a, so it never detached and never answered GetModuleHandle).
     * find_by_name skips claimed slots, so a nested reference to a
     * module already loading falls through to the cycle check, which is
     * exactly what it is. */
    modules[slot].used    = 1;
    modules[slot].loading = 1;

    file = open_module_file(name, &file_size);
    if (!file) {
        memset(&modules[slot], 0, sizeof modules[slot]);
        w32_set_last_error(W32_ERROR_MOD_NOT_FOUND);
        return NULL;
    }

    /* Push before mapping: everything map_dll binds -- including nested
     * loads -- sees this name in progress. */
    k = 0;
    while (name[k] && k + 1 < sizeof load_stack[0]) {
        load_stack[load_depth][k] = name[k]; k++;
    }
    load_stack[load_depth][k] = 0;
    load_depth++;

    if (map_dll(file, file_size, &base, &span, &why) != 0) {
        load_depth--;
        printf("w32: LoadLibrary(%s) refused: %s\n", name, why);
        free(file);
        memset(&modules[slot], 0, sizeof modules[slot]);
        w32_set_last_error(W32_ERROR_BAD_EXE_FORMAT);
        return NULL;
    }
    load_depth--;

    /* used was claimed before mapping; the load is now real. */
    modules[slot].loading   = 0;
    modules[slot].builtin   = 0;
    modules[slot].refs      = 1;
    modules[slot].base      = base;
    modules[slot].span      = span;
    modules[slot].file      = file;
    modules[slot].file_size = file_size;
    modules[slot].seq       = load_seq++;
    modules[slot].nholds    = 0;
    k = 0;
    while (name[k] && k + 1 < sizeof modules[slot].name) {
        modules[slot].name[k] = name[k]; k++;
    }
    modules[slot].name[k] = 0;

    /* Holds before DllMain: a DllMain that (against the rules) frees a
     * dependency must not unmap what this module is bound against. */
    take_holds(slot);

    /* DllMain(DLL_PROCESS_ATTACH).  A DLL that returns FALSE has refused to
     * initialise, and the documented behaviour is that the load fails --
     * so the mapping is torn down rather than left for a program that
     * believes it succeeded. */
    if (pe_parse(file, file_size, &img) == PE_OK && img.entry_point_rva) {
        typedef int (W32ABI *dllmain_fn)(void *, uint32_t, void *);
        dllmain_fn dm = (dllmain_fn)(void *)(base + img.entry_point_rva);
        if (!dm((void *)base, W32_DLL_PROCESS_ATTACH, NULL)) {
            printf("w32: LoadLibrary(%s) refused: DllMain returned FALSE\n",
                   name);
            release_holds_only(slot);
            munmap(base, span);
            free(file);
            memset(&modules[slot], 0, sizeof modules[slot]);
            w32_set_last_error(W32_ERROR_BAD_EXE_FORMAT);
            return NULL;
        }
    }

    return slot_to_handle(slot);
}

W32_HMODULE W32ABI w32_LoadLibraryA(const char *name) {
    w32_module_init();
    if (!name || !*name) {
        w32_set_last_error(W32_ERROR_MOD_NOT_FOUND);
        return NULL;
    }

    /* Already loaded, or built in?  Share it and take a reference.  Loading
     * the same DLL twice would give a program two copies of its state,
     * which is a real bug and not merely wasteful. */
    {
        w32_module_t *m = find_by_name(name);
        if (m) {
            if (!m->builtin) m->refs++;
            return slot_to_handle(slot_index(m));
        }
    }
    return load_one(name);
}

/* Resolve one symbol inside one module.  by_ord/ord carry an ordinal
 * import through: built-ins translate via the ordinal map, file modules
 * match the export directory's ordinal column (which needs no name, so
 * ordinal-only exports work).
 *
 * Data exports need no special case: an RVA is an RVA, and the caller binds
 * the address without calling it.  GetProcAddress returns them; the loader
 * never calls them (W32A-1).
 */
static void *resolve_in_module(w32_module_t *m, const char *name,
                               int by_ord, unsigned ord) {
    if (m->builtin) {
        const char *n = name;
        char full[W32_MODULE_NAME_MAX + 8];
        void *fn;
        size_t k;
        if (by_ord) {
            n = w32_ordmap_lookup(m->name, ord);
            if (!n) {
                w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
                return NULL;
            }
        }
        /* The export table is keyed by the full "KERNEL32.dll" spelling,
         * while modules are registered under the bare name a program is
         * most likely to ask for.  Try the bare name first and then with
         * the suffix, rather than storing the same module under two names:
         * one module, two spellings of it. */
        fn = w32_resolve(m->name, n);
        if (!fn) {
            k = 0;
            while (m->name[k] && k + 5 < sizeof full) { full[k] = m->name[k]; k++; }
            full[k++] = '.'; full[k++] = 'd'; full[k++] = 'l'; full[k++] = 'l';
            full[k] = 0;
            fn = w32_resolve(full, n);
        }
        if (!fn) {
            w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
            return NULL;
        }
        return fn;
    }

    {
        pe_image_t img;
        static pe_export_t exports[256];
        size_t count = 0, i;
        if (pe_parse(m->file, m->file_size, &img) != PE_OK) {
            w32_set_last_error(W32_ERROR_BAD_EXE_FORMAT);
            return NULL;
        }
        if (pe_exports(&img, exports, sizeof exports / sizeof exports[0],
                       &count) != PE_OK) {
            w32_set_last_error(W32_ERROR_BAD_EXE_FORMAT);
            return NULL;
        }
        if (count > sizeof exports / sizeof exports[0])
            count = sizeof exports / sizeof exports[0];

        for (i = 0; i < count; i++) {
            if (by_ord) {
                if (exports[i].ordinal != (uint16_t)ord) continue;
            } else {
                if (!exports[i].name[0]) continue;
                if (strcmp(exports[i].name, name) != 0) continue;
            }

            /* A forwarder's "address" is a string like "KERNEL32.Sleep".
             * Returning it would hand the caller a pointer to text that it
             * would then call.  Refused by name instead -- and the refusal
             * names the forwarder target (W32A-1). */
            if (exports[i].is_forwarder) {
                printf("w32: GetProcAddress(%s): forwarder exports "
                       "are not supported (target %s)\n",
                       by_ord ? "<by ordinal>" : name, exports[i].forwarder);
                w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
                return NULL;
            }
            if ((size_t)exports[i].rva >= m->span) {
                w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
                return NULL;
            }
            return (void *)(m->base + exports[i].rva);
        }
    }

    w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
    return NULL;
}

void *W32ABI w32_GetProcAddress(W32_HMODULE mod, const char *name) {
    w32_module_init();

    w32_module_t *m = handle_to_slot(mod);
    if (!m || !name) {
        w32_set_last_error(W32_ERROR_MOD_NOT_FOUND);
        return NULL;
    }

    /* Ordinal form: MAKEINTRESOURCE packs the number into the pointer, so a
     * "pointer" below 64K is an ordinal, never an address. */
    if ((uintptr_t)name < 65536u)
        return resolve_in_module(m, 0, 1, (unsigned)(uintptr_t)name);
    return resolve_in_module(m, name, 0, 0);
}

void *w32_load_dependency(const char *dll, const char *name,
                          int by_ord, unsigned ord) {
    w32_module_t *m;
    W32_HMODULE h;
    w32_module_init();
    if (!dll || (!name && !by_ord)) return NULL;
    m = find_by_name(dll);
    if (m) {
        /* A built-in the binder could not resolve stays unresolvable: the
         * binder tried every table already, so this is an unknown NAME in
         * a known module, not a module to load. */
        if (m->builtin) {
            w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
            return NULL;
        }
        return resolve_in_module(m, name, by_ord, ord);
    }
    h = load_one(dll);
    if (!h) return NULL;
    m = handle_to_slot(h);
    if (!m) return NULL;
    return resolve_in_module(m, name, by_ord, ord);
}

int W32ABI w32_FreeLibrary(W32_HMODULE mod) {
    w32_module_t *m = handle_to_slot(mod);
    if (!m) {
        w32_set_last_error(W32_ERROR_MOD_NOT_FOUND);
        return 0;
    }
    /* Built-ins are not mappings and cannot be freed.  Reporting success is
     * what Windows does for a module that stays loaded, and it keeps a
     * well-behaved program's cleanup path from looking like a failure. */
    if (m->builtin) return 1;

    if (--m->refs > 0) return 1;
    free_slot(slot_index(m));
    return 1;
}

void w32_module_detach_all(void) {
    /* Newest first: dependencies attached before their importers, so
     * reverse load order is teardown order.  References are ignored -- the
     * process is going away; everything is freed. */
    for (;;) {
        int pick = -1, i;
        unsigned best = 0;
        for (i = 0; i < W32_MODULE_MAX; i++) {
            if (!modules[i].used || modules[i].builtin || modules[i].loading) continue;
            if (pick < 0 || modules[i].seq > best) {
                pick = i;
                best = modules[i].seq;
            }
        }
        if (pick < 0) return;
        free_slot(pick);
    }
}

int w32_module_refcount(W32_HMODULE mod) {
    w32_module_t *m = handle_to_slot(mod);
    return m ? m->refs : -1;
}

int w32_module_count(void) {
    int n = 0;
    for (int i = 0; i < W32_MODULE_MAX; i++) if (modules[i].used) n++;
    return n;
}
