/* w32_bind.h — resolving a PE's imports to the w32 implementation.
 *
 * WIN32_PLAN.md phase W32-4, and decision D2: "Import resolution in user
 * space, loading in the kernel."
 *
 * The kernel maps the image and applies relocations (W32-3).  Binding the
 * import table is done here, in user space, because it is the part that walks
 * attacker-controlled structures and calls into a name table -- a bug in it
 * should kill one process, not the machine.
 *
 * There is no dynamic linker and no kernel32.dll on disk.  The exports are
 * ordinary functions already linked into the loader, and binding is a lookup
 * in a static table.  That is the whole mechanism; LoadLibrary of a *real*
 * user-supplied DLL is W32-7 and may yet end in a documented refusal.
 */

#ifndef AURALITE_W32_BIND_H
#define AURALITE_W32_BIND_H

#include <stddef.h>
#include <stdint.h>

/* One exported name and the address behind it. */
typedef struct {
    const char *dll;        /* case-insensitive match, e.g. "KERNEL32.dll" */
    const char *name;
    void       *addr;
} w32_export_t;

/* The built-in export table, NULL-terminated. */
const w32_export_t *w32_exports(void);

/* Look up one export.  Returns NULL when the DLL or name is unknown, which the
 * caller must treat as a hard failure: continuing with an unbound IAT slot
 * means the first call jumps to whatever was there. */
void *w32_resolve(const char *dll, const char *name);

/* Bind every import of an image already mapped at `base`.
 *
 * `image` is the raw file (for parsing) and `base` is where the kernel mapped
 * it.  Writes resolved addresses into the IAT.  Returns 0 on success, or a
 * negative value; on failure `*missing_dll`/`*missing_name` describe the first
 * unresolved import so the caller can say which one, rather than "failed".
 */
int w32_bind_imports(const uint8_t *image, size_t image_size, uint64_t base,
                     const char **missing_dll, const char **missing_name);

/* ---- the gap ledger (RESIDUE3 C8) ---------------------------------------
 *
 * w32_bind_imports() stops at the FIRST unresolved import, which is the right
 * behaviour for a loader (the image cannot run either way) and the wrong one
 * for measurement: Audacity 3.7.5's bundle is 19 modules and 419 symbols away
 * from running, and "the first one" is a sample of one.  This variant walks
 * every import, binds what resolves, and records what does not, grouped per
 * module — so a refusal can say "19 module(s), 419 symbol(s) missing" and name
 * the modules, which is a number a later phase can be measured against.
 *
 * `first_dll`/`first_name` keep the old single-miss shape for the callers that
 * report one line.
 */
#define W32_GAP_MODULES_MAX 32
#define W32_GAP_NAME_MAX    64
/* RESIDUE3 C8 / DOOM D7: counts answered "how bad", not "which".  The ledger
 * keeps the first W32_GAP_LIST_MAX missing names as well, so a refusal can be
 * read as a to-do list -- DOOM's Win32 build misses 52 symbols and every one
 * of them is a decision. */
#define W32_GAP_LIST_MAX    128

typedef struct {
    long symbols;                          /* total unresolved symbols        */
    int  modules;                          /* distinct DLLs with >= 1 miss    */
    int  overloaded;                       /* more distinct DLLs than the table */
    char dll[W32_GAP_MODULES_MAX][W32_GAP_NAME_MAX];
    long per_module[W32_GAP_MODULES_MAX];
    char first_dll[W32_GAP_NAME_MAX];
    char first_name[128];
    int  listed;                           /* names kept in list[0..listed)   */
    char list_dll[W32_GAP_LIST_MAX][W32_GAP_NAME_MAX];
    char list_name[W32_GAP_LIST_MAX][W32_GAP_NAME_MAX];
} w32_gap_t;

/* 0 = every import bound.  1 = the image parsed and every resolvable import
 * was bound, but `gap` records the ones that were not (the caller must refuse
 * to run it).  <0 = the same parse/resource failures w32_bind_imports()
 * returns. */
int w32_bind_imports_gap(const uint8_t *image, size_t image_size, uint64_t base,
                         w32_gap_t *gap);

/* One human line: "[w32] gap: 19 module(s), 419 symbol(s) missing". */
int w32_gap_describe(const w32_gap_t *gap, char *out, size_t n);

/* One missing name: 1 = filled out, 0 = no more (index past `gap->listed`). */
int w32_gap_at(const w32_gap_t *gap, int index, char *dll, size_t dn,
               char *name, size_t nn);

#endif /* AURALITE_W32_BIND_H */
