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
 * What IS refused is narrower and deliberate -- forwarders, delay-load
 * imports, and imports by ordinal.  Each is detected by name and reported,
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
    int      refs;
    char     name[W32_MODULE_NAME_MAX];   /* as matched, case-insensitively */
    uint8_t *base;                        /* mapping, for file modules      */
    size_t   span;                        /* mapped bytes                   */
    uint8_t *file;                        /* the raw file, kept for parsing */
    size_t   file_size;
} w32_module_t;

static w32_module_t modules[W32_MODULE_MAX];
static int          modules_ready;

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
}

void w32_module_init(void) {
    if (modules_ready) return;
    modules_ready = 1;
    /* The three modules whose exports are linked into the loader.  There is
     * no file behind any of them, which is why they are registered rather
     * than loaded. */
    add_builtin("kernel32");
    add_builtin("user32");
    add_builtin("gdi32");
}

static w32_module_t *find_by_name(const char *name) {
    for (int i = 0; i < W32_MODULE_MAX; i++) {
        if (!modules[i].used) continue;
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
    /* Delay-load imports need a stub-patching mechanism this does not have;
     * detected and refused rather than ignored, since ignoring them means
     * the first call through a delay stub jumps somewhere arbitrary. */
    if (img.dir[PE_DIR_DELAY_IMPORT].rva && img.dir[PE_DIR_DELAY_IMPORT].size) {
        *why = "delay-load imports are not supported";
        return -1;
    }

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
        munmap(base, span);
        *why = "unresolved import in the DLL";
        return -1;
    }

    *out_base = base;
    *out_span = span;
    return 0;
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
    w32_module_t *m = find_by_name(name);
    if (m) {
        if (!m->builtin) m->refs++;
        return slot_to_handle(slot_index(m));
    }

    int slot = -1;
    for (int i = 0; i < W32_MODULE_MAX; i++) {
        if (!modules[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        w32_set_last_error(W32_ERROR_NOT_SUPPORTED);
        return NULL;
    }

    size_t file_size = 0;
    uint8_t *file = read_whole_file(name, &file_size);
    if (!file) {
        w32_set_last_error(W32_ERROR_MOD_NOT_FOUND);
        return NULL;
    }

    uint8_t *base = NULL;
    size_t span = 0;
    const char *why = "unknown";
    if (map_dll(file, file_size, &base, &span, &why) != 0) {
        printf("w32: LoadLibrary(%s) refused: %s\n", name, why);
        free(file);
        w32_set_last_error(W32_ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    modules[slot].used      = 1;
    modules[slot].builtin   = 0;
    modules[slot].refs      = 1;
    modules[slot].base      = base;
    modules[slot].span      = span;
    modules[slot].file      = file;
    modules[slot].file_size = file_size;
    size_t k = 0;
    while (name[k] && k + 1 < sizeof modules[slot].name) {
        modules[slot].name[k] = name[k]; k++;
    }
    modules[slot].name[k] = 0;

    /* DllMain(DLL_PROCESS_ATTACH).  A DLL that returns FALSE has refused to
     * initialise, and the documented behaviour is that the load fails --
     * so the mapping is torn down rather than left for a program that
     * believes it succeeded. */
    pe_image_t img;
    if (pe_parse(file, file_size, &img) == PE_OK && img.entry_point_rva) {
        typedef int (W32ABI *dllmain_fn)(void *, uint32_t, void *);
        dllmain_fn dm = (dllmain_fn)(void *)(base + img.entry_point_rva);
        if (!dm((void *)base, W32_DLL_PROCESS_ATTACH, NULL)) {
            printf("w32: LoadLibrary(%s) refused: DllMain returned FALSE\n",
                   name);
            munmap(base, span);
            free(file);
            memset(&modules[slot], 0, sizeof modules[slot]);
            w32_set_last_error(W32_ERROR_BAD_EXE_FORMAT);
            return NULL;
        }
    }

    return slot_to_handle(slot);
}

void *W32ABI w32_GetProcAddress(W32_HMODULE mod, const char *name) {
    w32_module_init();

    w32_module_t *m = handle_to_slot(mod);
    if (!m || !name) {
        w32_set_last_error(W32_ERROR_MOD_NOT_FOUND);
        return NULL;
    }

    if (m->builtin) {
        /* The export table is keyed by the full "KERNEL32.dll" spelling,
         * while modules are registered under the bare name a program is
         * most likely to ask for.  Try the bare name first and then with
         * the suffix, rather than storing the same module under two names:
         * one module, two spellings of it. */
        void *fn = w32_resolve(m->name, name);
        if (!fn) {
            char full[W32_MODULE_NAME_MAX + 8];
            size_t k = 0;
            while (m->name[k] && k + 5 < sizeof full) { full[k] = m->name[k]; k++; }
            full[k++] = '.'; full[k++] = 'd'; full[k++] = 'l'; full[k++] = 'l';
            full[k] = 0;
            fn = w32_resolve(full, name);
        }
        if (!fn) {
            w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
            return NULL;
        }
        return fn;
    }

    pe_image_t img;
    if (pe_parse(m->file, m->file_size, &img) != PE_OK) {
        w32_set_last_error(W32_ERROR_BAD_EXE_FORMAT);
        return NULL;
    }

    static pe_export_t exports[256];
    size_t count = 0;
    if (pe_exports(&img, exports, sizeof exports / sizeof exports[0],
                   &count) != PE_OK) {
        w32_set_last_error(W32_ERROR_BAD_EXE_FORMAT);
        return NULL;
    }
    if (count > sizeof exports / sizeof exports[0])
        count = sizeof exports / sizeof exports[0];

    for (size_t i = 0; i < count; i++) {
        if (!exports[i].name[0]) continue;
        if (strcmp(exports[i].name, name) != 0) continue;

        /* A forwarder's "address" is a string like "KERNEL32.Sleep".
         * Returning it would hand the caller a pointer to text that it would
         * then call.  Refused by name instead. */
        if (exports[i].is_forwarder) {
            printf("w32: GetProcAddress(%s): forwarder exports "
                   "are not supported\n", name);
            w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
            return NULL;
        }
        if ((size_t)exports[i].rva >= m->span) {
            w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
            return NULL;
        }
        return (void *)(m->base + exports[i].rva);
    }

    w32_set_last_error(W32_ERROR_PROC_NOT_FOUND);
    return NULL;
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

    pe_image_t img;
    if (pe_parse(m->file, m->file_size, &img) == PE_OK && img.entry_point_rva) {
        typedef int (W32ABI *dllmain_fn)(void *, uint32_t, void *);
        dllmain_fn dm = (dllmain_fn)(void *)(m->base + img.entry_point_rva);
        dm((void *)m->base, W32_DLL_PROCESS_DETACH, NULL);
    }

    munmap(m->base, m->span);
    free(m->file);
    memset(m, 0, sizeof *m);
    return 1;
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
