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

#endif /* AURALITE_W32_BIND_H */
