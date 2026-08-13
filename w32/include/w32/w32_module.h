/* w32/include/w32/w32_module.h — WIN32_PLAN.md phase W32-7.
 *
 * LoadLibrary / GetProcAddress / FreeLibrary.
 *
 * The phase asked whether dynamic loading is reachable at all, and allowed
 * the answer to be a documented refusal.  It is reachable: AuraLite's mmap
 * grants PROT_EXEC and the export directory parses, so real user-supplied
 * DLLs load here rather than only the built-in modules.
 *
 * Two kinds of module share one handle space:
 *
 *   BUILT-IN modules (KERNEL32, USER32, GDI32) are not files.  They are the
 *   functions already linked into the loader, and GetModuleHandleA returns a
 *   token for them.  There is no kernel32.dll on disk to load.
 *
 *   FILE modules are ordinary PE DLLs: mapped, relocated, their own imports
 *   bound, DllMain called.
 *
 * What is deliberately refused, rather than half-supported:
 *
 *   - Forwarder exports ("KERNEL32.Sleep" as a string in place of code).
 *     Detected by pe_exports() and refused by name, because following one
 *     means resolving into another module and the failure mode of getting
 *     that subtly wrong is a call into a string.
 *   - Delay-load imports: the directory is detected and refused.
 *   - Imports by ordinal, matching the policy w32_bind.c already applies.
 *
 * Refusals are explicit and named.  A loader that silently returns a handle
 * to a half-initialised module is worse than one that says no.
 */
#ifndef AURALITE_W32_MODULE_H
#define AURALITE_W32_MODULE_H

#include <stddef.h>
#include <stdint.h>

#include "w32/w32_abi.h"

/* An HMODULE is an opaque token, minted from a table -- never a raw mapped
 * address cast to a handle.  Same reasoning as the W32-4 HANDLE table: a
 * value the program can fabricate must not be dereferenceable. */
typedef void *W32_HMODULE;

#define W32_MODULE_MAX      16   /* built-ins plus loaded files          */
#define W32_MODULE_NAME_MAX 64

/* Documented error codes this layer reports through w32_set_last_error(). */
#define W32_ERROR_MOD_NOT_FOUND   126u
#define W32_ERROR_PROC_NOT_FOUND  127u
#define W32_ERROR_BAD_EXE_FORMAT  193u
#define W32_ERROR_NOT_SUPPORTED   50u

/* Register the built-in modules.  Idempotent; safe to call more than once. */
void w32_module_init(void);

/* GetModuleHandleA: a handle for an ALREADY-loaded module, without loading.
 * NULL name means the main executable, which is not a loadable module here
 * and reports itself as the built-in "process" token. */
W32_HMODULE W32ABI w32_GetModuleHandleA(const char *name);

/* LoadLibraryA: a built-in name returns its token; anything else is treated
 * as a path to a PE DLL and actually loaded.  Returns NULL on failure with
 * the last error set. */
W32_HMODULE W32ABI w32_LoadLibraryA(const char *name);

/* GetProcAddress: NULL + ERROR_PROC_NOT_FOUND for an unknown name, never a
 * crash, and never a pointer to a forwarder string. */
void *W32ABI w32_GetProcAddress(W32_HMODULE mod, const char *name);

/* FreeLibrary: drops one reference.  Built-ins cannot be freed (they are not
 * mappings); a file module is unmapped when its last reference goes, after
 * DllMain(DLL_PROCESS_DETACH). */
int W32ABI w32_FreeLibrary(W32_HMODULE mod);

/* ---- testability -------------------------------------------------------
 * Reference count for @mod, or -1 if it is not a live handle.  Exposed so
 * the gate can prove that a second LoadLibrary of the same path shares one
 * mapping instead of loading it twice, and that FreeLibrary really releases
 * it -- neither is observable through the Win32 surface alone. */
int w32_module_refcount(W32_HMODULE mod);

/* Number of live modules, for leak checking across load/free cycles. */
int w32_module_count(void);

#endif /* AURALITE_W32_MODULE_H */
