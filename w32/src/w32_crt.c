/* w32/src/w32_crt.c — WIN32_PLAN.md phase W32-6.
 *
 * TLS callbacks, static initialisers, and a setjmp-based __try/__except.
 *
 * The SEH shim rests on a fact worth stating, because it decided the whole
 * design: AuraLite's kernel ALREADY delivers CPU faults to user handlers.
 * kernel/arch/x86_64/isr.c maps #DE to SIGFPE and #PF to SIGSEGV and calls
 * signal_raise_fault(), which installs a handler frame and returns to it.
 * So __try/__except needs no kernel change at all -- it is a sigaction plus
 * a siglongjmp.  Discovering that turned this phase from "modify the fault
 * path" into "use the fault path that exists", which is both less risky and
 * less code.
 *
 * The one subtlety is the signal mask.  A signal is blocked while its own
 * handler runs, and jumping out with plain longjmp would leave it blocked
 * forever -- so the SECOND divide-by-zero in a program would kill it, while
 * the first was caught.  That is a horrible bug to debug and it is entirely
 * avoided by using sigsetjmp/siglongjmp with savemask = 1, which restore the
 * mask on the way out.  AuraLite's libc implements those correctly
 * (lib/libc/src/compat.c saves and restores via sigprocmask), so the shim
 * uses them rather than the plain pair.  The phase gate faults twice in a
 * row precisely to prove this.
 */

#ifndef AURALITE_W32_HOST_TEST
#include <signal.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#endif

#include "w32/w32_crt.h"

/* ------------------------------------------------------------------------
 * Image-bounds checking
 *
 * Everything below follows a pointer that came out of the file being run, so
 * every one of them is checked against the mapped image first.  A PE can
 * name a TLS callback array anywhere it likes; following that blindly is how
 * a loader turns a bad file into arbitrary execution.
 * ------------------------------------------------------------------------ */

static int in_image(size_t image_size, uint64_t rva, size_t len) {
    if (rva > image_size) return 0;
    if (len > image_size - rva) return 0;
    return 1;
}

/* Convert an absolute VA from the image into an RVA, refusing anything that
 * does not actually land inside this image. */
static int va_to_rva(unsigned char *base, size_t image_size,
                     uint64_t va, uint64_t *out_rva) {
    uint64_t b = (uint64_t)(uintptr_t)base;
    if (va < b) return 0;
    uint64_t rva = va - b;
    if (rva >= image_size) return 0;
    *out_rva = rva;
    return 1;
}

/* ------------------------------------------------------------------------
 * TLS callbacks
 * ------------------------------------------------------------------------ */

int w32_crt_run_tls_callbacks(unsigned char *base, size_t image_size,
                              uint32_t dir_rva, uint32_t dir_size) {
    /* No TLS directory is the common case and not an error. */
    if (dir_rva == 0 || dir_size == 0) return 0;

    if (!in_image(image_size, dir_rva, sizeof(w32_tls_directory_t)))
        return -1;

    const w32_tls_directory_t *tls =
        (const w32_tls_directory_t *)(void *)(base + dir_rva);

    /* The TLS index is a DWORD the loader owns: the image reads it to find
     * its slot.  With one TLS block per process (see the note in
     * WIN32_PLAN.md about GS), slot 0 is the only correct answer, and
     * writing it is what makes __declspec(thread) reads resolve. */
    if (tls->address_of_index) {
        uint64_t idx_rva;
        if (va_to_rva(base, image_size, tls->address_of_index, &idx_rva) &&
            in_image(image_size, idx_rva, sizeof(uint32_t))) {
            *(uint32_t *)(void *)(base + idx_rva) = 0;
        }
        /* An out-of-image index pointer is ignored rather than fatal: the
         * image is malformed, but nothing has been executed on its say-so
         * yet, and refusing to start it would be a harsher policy than the
         * rest of the loader applies. */
    }

    if (!tls->address_of_callbacks) return 0;

    uint64_t cb_rva;
    if (!va_to_rva(base, image_size, tls->address_of_callbacks, &cb_rva))
        return -2;

    int ran = 0;
    for (;;) {
        if (!in_image(image_size, cb_rva + (uint64_t)ran * 8, 8)) return -3;

        uint64_t fn_va = *(const uint64_t *)(const void *)
                             (base + cb_rva + (size_t)ran * 8);
        if (fn_va == 0) break;   /* NULL terminates the array */

        /* A callback pointing outside the image is refused.  This is the
         * hostile case the gate exercises: the array is attacker-controlled
         * data, and the whole point of the check is that we never call
         * through it. */
        uint64_t fn_rva;
        if (!va_to_rva(base, image_size, fn_va, &fn_rva)) return -4;

        w32_tls_callback_fn cb = (w32_tls_callback_fn)(void *)(base + fn_rva);
        cb((void *)base, W32_DLL_PROCESS_ATTACH, NULL);
        ran++;

        if (ran > 64) return -5;   /* refuse an absurd or looping array */
    }
    return ran;
}

/* ------------------------------------------------------------------------
 * Static initialisers (.CRT$XCA .. .CRT$XCZ)
 *
 * The linker sorts contributions by section name, so the table is bracketed
 * by the empty XCA and XCZ markers and the constructors land in between.
 * Padding means NULL entries appear inside the range; they are skipped, not
 * treated as a terminator -- treating them as the end would silently drop
 * every constructor after the first padded gap.
 * ------------------------------------------------------------------------ */

int w32_crt_run_initializers(unsigned char *base, size_t image_size,
                             uint32_t start_rva, uint32_t end_rva) {
    if (start_rva == 0 || end_rva <= start_rva) return 0;
    if (!in_image(image_size, start_rva, end_rva - start_rva)) return -1;

    int ran = 0;
    for (uint32_t rva = start_rva; rva + 8 <= end_rva; rva += 8) {
        uint64_t fn_va = *(const uint64_t *)(const void *)(base + rva);
        if (fn_va == 0) continue;            /* padding */

        uint64_t fn_rva;
        if (!va_to_rva(base, image_size, fn_va, &fn_rva)) return -2;

        void (W32ABI *ctor)(void) = (void (W32ABI *)(void))(void *)
                                        (base + fn_rva);
        ctor();
        ran++;
    }
    return ran;
}

/* ------------------------------------------------------------------------
 * __try / __except
 * ------------------------------------------------------------------------ */

static sigjmp_buf seh_stack[W32_SEH_MAX_DEPTH];
static int        seh_depth;
static uint32_t   seh_code;
static void      *seh_addr;
static int        seh_installed;
static w32_unhandled_filter_fn seh_unhandled;

sigjmp_buf *w32_seh_push(void) {
    if (seh_depth >= W32_SEH_MAX_DEPTH) return NULL;
    return &seh_stack[seh_depth++];
}

void w32_seh_pop(void) {
    if (seh_depth > 0) seh_depth--;
}

uint32_t w32_exception_code(void)    { return seh_code; }
void    *w32_exception_address(void) { return seh_addr; }

/* Map a POSIX signal plus its si_code to the documented NTSTATUS a Windows
 * program expects.  Programs switch on these values, so a wrong mapping is
 * not cosmetic: a divide-by-zero reported as an access violation sends a
 * handler down the wrong path. */
static uint32_t signal_to_code(int signo, int si_code) {
    switch (signo) {
    case SIGFPE:
        /* AuraLite's <signal.h> defines only the si_codes its kernel
         * actually raises (FPE_INTDIV for #DE); FPE_INTOVF and FPE_FLTDIV
         * are not produced, so switching on them would be dead code
         * pretending to be coverage.  #DE is the one the gate exercises. */
        if (si_code == FPE_INTDIV) return W32_EXCEPTION_INT_DIVIDE_BY_ZERO;
        return W32_EXCEPTION_INT_DIVIDE_BY_ZERO;
    case SIGILL:  return W32_EXCEPTION_ILLEGAL_INSTRUCTION;
    case SIGBUS:  return W32_EXCEPTION_DATATYPE_MISALIGNMENT;
    case SIGTRAP: return W32_EXCEPTION_BREAKPOINT;
    case SIGSEGV: /* fall through */
    default:      return W32_EXCEPTION_ACCESS_VIOLATION;
    }
}

static void seh_handler(int signo, siginfo_t *info, void *uctx) {
    (void)uctx;

    seh_code = signal_to_code(signo, info ? info->si_code : 0);
    seh_addr = info ? info->si_addr : NULL;

    if (seh_depth > 0) {
        /* siglongjmp, not longjmp: it restores the signal mask, so the
         * signal we are inside of is unblocked again on the way out.  With
         * plain longjmp the first fault would be caught and every later one
         * would kill the process. */
        seh_depth--;
        siglongjmp(seh_stack[seh_depth], (int)seh_code);
    }

    /* No __try active: give the unhandled filter its last chance. */
    if (seh_unhandled) {
        int32_t disp = seh_unhandled(NULL);
        if (disp == W32_EXCEPTION_EXECUTE_HANDLER) {
            /* The documented meaning is "handled; terminate the process".
             * _exit rather than exit: running atexit handlers from inside a
             * fault handler would re-enter code that may be exactly what
             * faulted. */
            _exit(1);
        }
    }

    /* Otherwise let the fault be fatal.  Restoring the default disposition
     * and returning re-executes the faulting instruction, which now kills
     * the process with the right signal and the right exit status -- rather
     * than us inventing one, which would hide the cause from the shell and
     * from the test harness. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigaction(signo, &sa, NULL);
}

int w32_seh_init(void) {
    if (seh_installed) return 0;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = seh_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    static const int sigs[] = { SIGFPE, SIGSEGV, SIGILL, SIGBUS, SIGTRAP };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        if (sigaction(sigs[i], &sa, NULL) != 0) return -1;
    }
    seh_installed = 1;
    return 0;
}

w32_unhandled_filter_fn
w32_SetUnhandledExceptionFilter(w32_unhandled_filter_fn filter) {
    w32_unhandled_filter_fn old = seh_unhandled;
    seh_unhandled = filter;
    return old;
}
