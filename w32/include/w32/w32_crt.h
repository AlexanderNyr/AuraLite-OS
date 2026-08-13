/* w32/include/w32/w32_crt.h — WIN32_PLAN.md phase W32-6.
 *
 * The things a real compiler emits that a hand-written .exe avoids: TLS
 * callbacks, static initialisers, and structured exception handling.
 *
 * Scope, stated up front because the gap matters:
 *
 *   This is NOT table-driven unwinding.  Real Win64 SEH walks .pdata/.xdata
 *   to find handlers and to run cleanup for each frame it passes.  What is
 *   here is a setjmp/longjmp shim: __try/__except transfers control, but
 *   frames between the throw and the handler are abandoned without running
 *   anything.  For C++ that means destructors of live objects DO NOT RUN.
 *   The same limitation is recorded in docs/win32.md and in WIN32_PLAN.md.
 *
 * The shim is honest about which faults it can catch.  It catches what the
 * CPU raises synchronously and the kernel maps to a signal -- divide by
 * zero, an invalid access, an illegal instruction.  It does not implement
 * RaiseException, so a software-raised C++ throw is out of scope (D8 already
 * excludes table-driven SEH).
 */
#ifndef AURALITE_W32_CRT_H
#define AURALITE_W32_CRT_H

#include <stdint.h>
#include <stddef.h>

#include "w32/w32_abi.h"

/* ---- exception codes ---------------------------------------------------
 *
 * The documented NTSTATUS values a faulting instruction produces.  Programs
 * compare against these numerically, so the values are the interface. */
#define W32_EXCEPTION_ACCESS_VIOLATION      0xC0000005u
#define W32_EXCEPTION_ILLEGAL_INSTRUCTION   0xC000001Du
#define W32_EXCEPTION_INT_DIVIDE_BY_ZERO    0xC0000094u
#define W32_EXCEPTION_INT_OVERFLOW          0xC0000095u
#define W32_EXCEPTION_FLT_DIVIDE_BY_ZERO    0xC000008Eu
#define W32_EXCEPTION_PRIV_INSTRUCTION      0xC0000096u
#define W32_EXCEPTION_BREAKPOINT            0x80000003u
#define W32_EXCEPTION_DATATYPE_MISALIGNMENT 0x80000002u

/* Filter return values (documented EXCEPTION_* dispositions). */
#define W32_EXCEPTION_EXECUTE_HANDLER     1
#define W32_EXCEPTION_CONTINUE_SEARCH     0
#define W32_EXCEPTION_CONTINUE_EXECUTION (-1)

/* ---- TLS ---------------------------------------------------------------
 *
 * IMAGE_TLS_DIRECTORY64, the on-disk structure the linker emits when a
 * program uses __declspec(thread) or registers a TLS callback.  Field names
 * and order are the documented PE layout. */
typedef struct {
    uint64_t start_address_of_raw_data;
    uint64_t end_address_of_raw_data;
    uint64_t address_of_index;      /* VA of a DWORD receiving the TLS slot */
    uint64_t address_of_callbacks;  /* VA of a NULL-terminated fn ptr array */
    uint32_t size_of_zero_fill;
    uint32_t characteristics;
} w32_tls_directory_t;

/* Reasons passed to a TLS callback and to DllMain. */
#define W32_DLL_PROCESS_ATTACH 1
#define W32_DLL_THREAD_ATTACH  2
#define W32_DLL_THREAD_DETACH  3
#define W32_DLL_PROCESS_DETACH 0

typedef void (W32ABI *w32_tls_callback_fn)(void *dll_handle,
                                           uint32_t reason,
                                           void *reserved);

/* Run every TLS callback in the image, in order, with DLL_PROCESS_ATTACH.
 *
 * @base is the mapped image base; @dir_rva/@dir_size come from the PE TLS
 * data directory (0 means the image has none, which is not an error).
 *
 * Returns the number of callbacks run, or negative on a malformed
 * directory.  A callback array pointing outside the image is refused rather
 * than followed -- that is a hostile-input path, and the phase gate tests
 * it.
 */
int w32_crt_run_tls_callbacks(unsigned char *base, size_t image_size,
                              uint32_t dir_rva, uint32_t dir_size);

/* Run the .CRT$XC* static-initialiser table between @start and @end.
 *
 * These are the C++ global constructors.  The section is an array of
 * function pointers, padded with NULLs by the linker's section merging, so
 * NULL entries are skipped rather than treated as the end.
 *
 * Returns the number of initialisers run.
 */
int w32_crt_run_initializers(unsigned char *base, size_t image_size,
                             uint32_t start_rva, uint32_t end_rva);

/* ---- structured exception handling ------------------------------------ */

/* Install the fault handlers that make w32_try_begin() work.
 *
 * Idempotent.  Returns 0 on success, -1 if the handlers could not be
 * installed (in which case __try will not catch anything, and saying so is
 * better than pretending). */
int w32_seh_init(void);

/* The __try/__except shim.
 *
 * Usage mirrors what a compiler would emit:
 *
 *     if (w32_try_begin() == 0) {
 *         ... guarded code ...
 *         w32_try_end();
 *     } else {
 *         ... handler; w32_exception_code() says what happened ...
 *     }
 *
 * w32_try_begin() returns 0 on the initial call and the exception code when
 * control arrives via a fault.  It must be a macro, not a function: the
 * setjmp buffer has to belong to the caller's frame, and a helper function
 * that called setjmp on its own frame would return into a dead frame.
 */
#ifndef AURALITE_W32_HOST_TEST
#include <setjmp.h>
#endif

/* Depth of the __try nesting stack.  Deliberately small and fixed: this is
 * a shim, and an unbounded one would need allocation on the fault path. */
#define W32_SEH_MAX_DEPTH 16

/* Push a jump buffer and return it, or NULL if nesting is too deep. */
sigjmp_buf *w32_seh_push(void);
/* Pop the innermost buffer (the normal, non-faulting exit from a __try). */
void        w32_seh_pop(void);
/* The code of the exception that transferred control here. */
uint32_t    w32_exception_code(void);
/* The faulting address, for an access violation. */
void       *w32_exception_address(void);

#define w32_try_begin()                                                    \
    ({ sigjmp_buf *_jb = w32_seh_push();                                   \
       _jb ? sigsetjmp(*_jb, 1) : (int)W32_EXCEPTION_CONTINUE_SEARCH; })

#define w32_try_end() w32_seh_pop()

/* SetUnhandledExceptionFilter: the last-chance filter, called when a fault
 * happens with no __try active.  Returning EXCEPTION_EXECUTE_HANDLER makes
 * the process exit quietly; anything else lets it die on the signal. */
typedef int32_t (W32ABI *w32_unhandled_filter_fn)(void *exception_info);

w32_unhandled_filter_fn
w32_SetUnhandledExceptionFilter(w32_unhandled_filter_fn filter);

#endif /* AURALITE_W32_CRT_H */
