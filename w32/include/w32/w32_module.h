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
 *     that subtly wrong is a call into a string.  The refusal names the
 *     forwarder target (W32A-1).
 *   - Dependency cycles: a DLL that (transitively) imports itself refuses
 *     with the cycle named (W32A-1).
 *   - Dependency chains deeper than W32_LOAD_DEPTH_MAX, with the depth
 *     named (W32A-1).
 *
 * W32A-1 lifted the W32-7 delay-load and ordinal refusals: delay imports
 * bind lazily through LoadLibrary/GetProcAddress (the image's own helper
 * calls them), and ordinals translate through the ordinal map.
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

#define W32_MODULE_MAX      64   /* built-ins plus loaded files.
                                 * 16 died in W32A-1: the stub-module
                                 * registration alone fills 20, and a full
                                 * table fails every file load silently. */
#define W32_MODULE_NAME_MAX 64

/* W32A-1: a dependency chain (exe -> A -> B -> ...) stops here.  Deeper
 * than any ladder chain (two: an exe plus one DLL), shallow enough that a
 * malicious cycle-that-is-not-quite-a-cycle cannot recurse usefully. */
#define W32_LOAD_DEPTH_MAX   8

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

/* ---- W32A-1: recursive loading --------------------------------------------
 *
 * w32_load_dependency() is the static binder's back door into the module
 * loader: when an import names a DLL no built-in answers, the binder asks
 * here, and the DLL is found (already loaded) or loaded (from disk) and the
 * symbol resolved inside it.  by_ord/ord carry an ordinal import through so
 * ordinal-only exports in user DLLs match by number.  Returns the bound
 * address, or NULL with the last error set.  Cycles and over-deep chains
 * refuse here, loudly.
 *
 * Search order for a bare name (documented subset of the Windows order):
 * the main executable's directory first (set by w32run), then the path as
 * given (relative names resolve against the working directory).  A name
 * with a separator or drive letter is tried literally only.
 */
void *w32_load_dependency(const char *dll, const char *name,
                          int by_ord, unsigned ord);

/* Record the main executable's path so dependency loads search its
 * directory.  Call once at startup, before any bind; later calls replace. */
void w32_module_set_exe_dir(const char *exe_path);

/* Run DllMain(DLL_PROCESS_DETACH) for every loaded file module, newest
 * first, and unmap them all.  ExitProcess calls this: dependencies tear
 * down in reverse of the attach order, and nothing outlives the process. */
void w32_module_detach_all(void);

/* ---- testability -------------------------------------------------------
 * Reference count for @mod, or -1 if it is not a live handle.  Exposed so
 * the gate can prove that a second LoadLibrary of the same path shares one
 * mapping instead of loading it twice, and that FreeLibrary really releases
 * it -- neither is observable through the Win32 surface alone. */
int w32_module_refcount(W32_HMODULE mod);

/* Number of live modules, for leak checking across load/free cycles. */
int w32_module_count(void);

#endif /* AURALITE_W32_MODULE_H */
